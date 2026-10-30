#include "ChoiceLearner.h"

#include "core/ChoiceLive.h"
#include "core/Coverage.h"
#include "core/Sha256.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace Huginn::Learning
{
    namespace
    {
        namespace C = Core::Choice;
        namespace DL = Core::DecisionLog;
        using ClockEvent = Core::LearningClock::Event;

        constexpr std::size_t kTakenContexts = 64;     // Plackett-Luce memory: the latest contexts
        constexpr double kForgetSweepSec = 60.0;

        bool KindOk(const RE::TESForm* f)
        {
            // The kinds the effect catalog describes (core/EffectRecords.h Kind):
            // spells, scrolls, potions/poisons/food, weapons, ammo, armour, soul
            // gems, lights.
            return f->Is(RE::FormType::Spell) || f->Is(RE::FormType::Scroll) || f->Is(RE::FormType::AlchemyItem) ||
                   f->Is(RE::FormType::Weapon) || f->Is(RE::FormType::Ammo) || f->Is(RE::FormType::Armor) ||
                   f->Is(RE::FormType::SoulGem) || f->Is(RE::FormType::Light);
        }

        /// The upgrade gate's inputs for a logged context (doc 11 decision 2).
        /// The logged covered(i) is max(other columns, summon term); the summon
        /// term at the limit is left(r_out). A row whose covered exceeds it was
        /// covered by another column (that value is kept as the floor); else the
        /// other columns' part is not known and taken as 0.
        struct ContextGate
        {
            bool atLimit = false;
            std::uint32_t source = 0;
            double left = 1.0;
        };

        ContextGate GateOf(const DL::Context& ctx)
        {
            ContextGate g;
            if (!ctx.summon.valid) return g;
            g.atLimit = Core::Coverage::SummonLimitReached(ctx.summon.count, ctx.summon.limit);
            if (!g.atLimit) return g;
            g.source = Core::Coverage::OldestSource(ctx.summon.list);
            g.left = Core::Coverage::LeftPrior(Core::Coverage::OldestRemaining(ctx.summon.list), ctx.refreshPriorSec);
            return g;
        }

        bool HasSummonColumn(const Core::Effect::Cap* cap)
        {
            if (!cap) return false;
            for (const auto& v : *cap) {
                if (v.v > 0.0f && Core::Coverage::IsSummonCountColumn(v.col)) return true;
            }
            return false;
        }

        /// The extra features a logged row carries (format v1's list); the old
        /// engine's per-row multipliers (ln fav / corr / potion, ln learner) are
        /// not in the row and read 0. R8.4: `ln util` reads 0 too (the build
        /// refuses a weight on it, core/Bootstrap.cpp), as in the live ranking.
        /// The D2 feature is the one the live score read: the row's um and lm
        /// (0.24.17). Until 0.24.10 it was read off whether the old floors
        /// scored the row (a finite `util`), which was the same thing while the
        /// legacy pass ran only for the live score; from 0.24.17 it also runs
        /// for the D2 shadow with the live score reading 0 / 1, and `util` is
        /// then set on rows whose score read um 1. A row with no um (held,
        /// never a candidate) reads 1, as it did (its util was never set).
        C::Features RowFeatures(const DL::Row& r)
        {
            C::Features f{};
            const bool scored = r.um == 0;
            f[static_cast<std::size_t>(C::Feature::util_missing)] = scored ? 0.0 : 1.0;
            if (scored) {
                if (std::isfinite(r.lm)) f[static_cast<std::size_t>(C::Feature::ln_learn_mult)] = r.lm;
            }
            if ((r.flags & DL::Flag::Shown) && r.slot >= 0 && r.slot < 8) {
                f[static_cast<std::size_t>(C::Feature::slot0) + static_cast<std::size_t>(r.slot)] = 1.0;
            }
            if (r.flags & DL::Flag::Eligible) f[static_cast<std::size_t>(C::Feature::eligible)] = 1.0;
            return f;
        }
    }

    // =========================================================================
    // SETTINGS
    // =========================================================================
    ChoiceSettings ChoiceSettings::FromIni(const CSimpleIniA& ini)
    {
        ChoiceSettings s;
        const std::string_view bias = ini.GetValue("Learning", "sBootstrapBias", "match");
        s.biasMode = Core::Bootstrap::ParseBiasMode(bias);
        const bool isMatch = bias.size() == 5 && std::equal(bias.begin(), bias.end(), "match", [](char a, char b) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(a))) == b;
        });
        if (s.biasMode == Core::Bootstrap::BiasMode::Match && !isMatch) {
            // ParseBiasMode reads anything it does not know as match
            logger::warn("[Choice] [Learning] sBootstrapBias = '{}' is not match, always or never: read as match"sv, bias);
        }
        s.thetaProcessNoise = std::clamp(ini.GetDoubleValue("Learning", "fThetaProcessNoise", s.thetaProcessNoise), 0.0, 1.0);
        // A NaN passes std::clamp untouched (fCoveredPrior = nan): read as the
        // default instead, as SlotSettings does; +-inf reads as the default too.
        auto finiteOr = [](double v, double fallback) { return std::isfinite(v) ? v : fallback; };
        s.coveredPrior = std::clamp(
            finiteOr(ini.GetDoubleValue("Scoring", "fCoveredPrior", s.coveredPrior), s.coveredPrior), -10.0, 10.0);
        s.legacyLearnFeature = ini.GetBoolValue("Scoring", "bLegacyLearnFeature", s.legacyLearnFeature);
        s.shadowLegacy = ini.GetBoolValue("Scoring", "bShadowLegacy", s.shadowLegacy);
        s.favoriteLifeMult = std::clamp(ini.GetDoubleValue("Favorites", "fFavoriteLifeMult", s.favoriteLifeMult), 1.0, 100.0);
        s.discovery.enabled = ini.GetBoolValue("Learning", "bDiscoveryBoost", s.discovery.enabled);
        s.discovery.varMult = ini.GetDoubleValue("Learning", "fDiscoveryBoostVarMult", s.discovery.varMult);
        s.discovery.halfLifeSec = ini.GetDoubleValue("Learning", "fDiscoveryBoostHalfLifeSec", s.discovery.halfLifeSec);
        s.discovery = s.discovery.Clamped();
        return s;
    }

    // =========================================================================
    // SNAPSHOT
    // =========================================================================
    std::array<double, C::K> ChoiceLearner::Snapshot::Needs(const Core::Needs::NeedArray& value,
                                                            const Core::Needs::NeedArray& input) const
    {
        return Core::Bootstrap::CurveNeeds(curves, value, input);
    }

    C::BRead ChoiceLearner::Snapshot::B(std::uint32_t form, bool favorite, double playNow) const
    {
        const auto it = b.find(form);
        if (it == b.end()) return C::BRead{};
        const double mult = (favorite && favoritesBoost) ? favoriteLifeMult : 1.0;
        return C::ReadB(it->second, life, playNow, mult);
    }

    double ChoiceLearner::Snapshot::DiscoveryMult(std::uint32_t form, double playNow) const
    {
        if (!discovery.enabled) return 1.0;
        const auto it = discovered.find(form);
        return it == discovered.end() ? 1.0 : Core::Discovery::Mult(discovery, playNow - it->second);
    }

    // =========================================================================
    // THE FILE
    // =========================================================================
    bool ChoiceLearner::LoadBootstrap(const std::filesystem::path& path)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                logger::error("[Choice] {} not found: the choice model is off and the pipeline ranks on the old "
                              "utility (FALLBACK; `hg status` says so)"sv, path.string());
                m_ready.store(false, std::memory_order_release);
                return false;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        auto read = Core::Bootstrap::Read(text);
        const double parseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!read.Ok()) {
            std::string why;
            for (std::size_t i = 0; i < read.errors.size() && i < 3; ++i) why += (i ? "; " : "") + read.errors[i];
            if (read.errors.size() > 3) why += std::format("; ... ({} problems)", read.errors.size());
            logger::error("[Choice] {} is not a valid bootstrap (format version 1): {} -- the choice model is off and "
                          "the pipeline ranks on the old utility (FALLBACK; `hg status` says so)"sv,
                path.string(), why);
            m_ready.store(false, std::memory_order_release);
            return false;
        }
        const ChoiceSettings settings = GetSettings();
        Core::Bootstrap::MapOptions opt;
        opt.coveredPrior = settings.coveredPrior;
        auto mapped = Core::Bootstrap::MapToBuild(*read.doc, opt);

        // Items: TESDataHandler::LookupForm(local, plugin), kind and name cross-checked.
        Core::Bootstrap::ItemReport irep;
        std::vector<Core::Bootstrap::ResolvedItem> items;
        if (auto* dh = RE::TESDataHandler::GetSingleton()) {
            items = Core::Bootstrap::ResolveItems(*read.doc,
                [dh](const std::string& plugin, std::uint32_t local) -> std::optional<Core::Bootstrap::Found> {
                    auto* f = dh->LookupForm(local, plugin);
                    if (!f) return std::nullopt;
                    const char* name = f->GetName();
                    return Core::Bootstrap::Found{ f->GetFormID(), KindOk(f), name ? name : "" };
                }, irep);
        }
        const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        // Features the file weighs that a logged row cannot supply (R8.2: lm,
        // um, slot N and eligible are supplied; R8.4: ln util is refused, below).
        std::string unsupplied;
        for (const auto& [name, w] : read.doc->features) {
            if (name == "ln fav" || name == "ln corr" || name == "ln potion" || name == "ln learner") {
                unsupplied += (unsupplied.empty() ? "" : ", ") + name;
            }
        }
        {
            std::unique_lock lock(m_mutex);
            m_createdUtc = read.doc->createdUtc;
            m_sha256 = Core::Sha256Hex(text);
            m_bootModel = std::move(mapped.model);
            // covered starts at fCoveredPrior (MapToBuild read it outside the
            // lock; a reload may have moved it since): the boot model holds
            // the value the live one starts from, which MovedLocked compares.
            m_bootModel.covered = C::Weight{ m_settings.coveredPrior, 1.0 };
            m_curves = std::move(mapped.curves);
            m_bootItems = std::move(items);
            m_doc = std::move(*read.doc);
            // The live model is filled here, before Ready() turns true: a tick on
            // the loading-screen thread can run the shadow between this call and
            // ResetPipelineSubsystems (step 8), and it must never see an empty
            // theta. ResetToBootstrap later redoes this and seeds b.
            m_model = m_bootModel;
            m_b.clear();
            BuildKeys();
            Touch();
            Note(ClockEvent::Rebootstrap);
        }
        const auto& rep = mapped.report;
        const auto& d = *m_doc;
        logger::info("[Choice] Bootstrap '{}' read in {:.1f} ms ({:.1f} ms with items): {} {}, created {}, sha256 {}; "
                     "needs {} mapped, {} unknown (skipped), {} at the prior; columns {} mapped, {} unknown, {} at the prior; "
                     "recent {} from the file, {} at the prior (tau {}); kappa {:.4g}; {} feature weight(s); curves {}; "
                     "items {} resolved of {} ({} gone, {} not a catalog kind, {} name differs, {} duplicate); "
                     "b from the file: {} (sBootstrapBias = {}, file character {}){}"sv,
            d.spec, parseMs, totalMs, d.outcomes,
            d.grading == C::Grading::Graded ? "graded" : d.grading == C::Grading::Presence ? "presence" : "presence+strength",
            d.createdUtc.empty() ? "?" : d.createdUtc, m_sha256.substr(0, 16),
            rep.needsMapped, rep.needsUnknown.size(), rep.needsPrior, rep.colsMapped, rep.colsUnknown.size(), rep.colsPrior,
            rep.recentMapped, rep.recentPrior, d.taus.size(), d.kappa, rep.featuresWeighted, rep.curvesMapped,
            irep.resolved, d.items.size(), irep.unresolved, irep.kindMismatch, irep.nameMismatch, irep.duplicateForm,
            settings.biasMode == Core::Bootstrap::BiasMode::Always  ? "always"
            : settings.biasMode == Core::Bootstrap::BiasMode::Never ? "never"
                                                                    : "when the save is that character",
            Core::Bootstrap::BiasModeName(settings.biasMode), d.sourceChar.empty() ? "several" : d.sourceChar,
            unsupplied.empty() ? "" : std::format("; WARNING: weighs {} which a row does not supply (read 0)", unsupplied));
        if (rep.featuresRefused > 0) {
            logger::warn("[Choice] The bootstrap weighs 'ln util': refused, read as 0 (R8.4: the old utility does not "
                         "rank; D2 reads only 'ln learn mult' and 'util missing')"sv);
        }
        if (C::WeighsSlots(m_bootModel)) {
            // core/ChoiceLive.h: a position is the allocation's output, so the
            // ranking cannot read it; only the logged mu and logit do.
            logger::warn("[Choice] The bootstrap weighs the slot N features: the ranking ignores them (a row's key "
                         "is the allocation's result); only the logged mu and logit read them"sv);
        }
        m_ready.store(true, std::memory_order_release);
        return true;
    }

    void ChoiceLearner::SetSettings(const ChoiceSettings& s)
    {
        std::unique_lock lock(m_mutex);
        m_settings = s;
        Touch();
        Note(ClockEvent::Settings);
    }

    ChoiceSettings ChoiceLearner::GetSettings() const
    {
        std::shared_lock lock(m_mutex);
        return m_settings;
    }

    void ChoiceLearner::SetMemoryLife(const Core::Battery::MemoryLife& life)
    {
        std::unique_lock lock(m_mutex);
        m_life = life.Clamped();
        Touch();
        Note(ClockEvent::Settings);
    }

    void ChoiceLearner::SetFavoritesBoost(bool boost)
    {
        std::unique_lock lock(m_mutex);
        m_favoritesBoost = boost;
        Touch();
        Note(ClockEvent::Settings);
    }

    // =========================================================================
    // KEYS -- one stable key per weight slot
    // =========================================================================
    void ChoiceLearner::BuildKeys()
    {
        using Core::Bootstrap::StableKey;
        m_keys.clear();
        m_slotOf.clear();
        const bool st = m_model.HasStrength() && m_model.strength.size() == C::K * C::J;
        auto add = [&](std::uint64_t key) {
            m_slotOf.emplace(key, m_keys.size());
            m_keys.push_back(key);
        };
        for (std::size_t k = 0; k < C::K; ++k) {
            for (std::size_t j = 0; j < C::J; ++j) add(StableKey("theta", Core::Needs::kNeeds[k].id, Core::Effect::kColumns[j].id));
        }
        if (st) {
            for (std::size_t k = 0; k < C::K; ++k) {
                for (std::size_t j = 0; j < C::J; ++j) add(StableKey("strength", Core::Needs::kNeeds[k].id, Core::Effect::kColumns[j].id));
            }
        }
        for (const auto& rr : m_model.recent) {
            const std::string fam = std::string(Core::Effect::kColumns[rr.col].id) + "@" + Core::Bootstrap::TauText(rr.tau);
            for (std::size_t j = 0; j < C::J; ++j) add(StableKey("recent", fam, Core::Effect::kColumns[j].id));
            if (st) {
                for (std::size_t j = 0; j < C::J; ++j) add(StableKey("recent~strength", fam, Core::Effect::kColumns[j].id));
            }
        }
        add(StableKey("covered"));
    }

    namespace
    {
        template <class M>
        auto SlotWeight(M& m, std::size_t slot) -> decltype(&m.theta[0])
        {
            const std::size_t KJ = C::K * C::J;
            const bool st = m.HasStrength() && m.strength.size() == KJ;
            if (slot < KJ) return m.theta.size() == KJ ? &m.theta[slot] : nullptr;
            slot -= KJ;
            if (st) {
                if (slot < KJ) return &m.strength[slot];
                slot -= KJ;
            }
            for (auto& rr : m.recent) {
                if (slot < C::J) return &rr.theta[slot];
                slot -= C::J;
                if (st) {
                    if (slot < C::J) return rr.strength.size() == C::J ? &rr.strength[slot] : nullptr;
                    slot -= C::J;
                }
            }
            return slot == 0 ? &m.covered : nullptr;
        }
    }

    // =========================================================================
    // RESET
    // =========================================================================
    void ChoiceLearner::ResetToBootstrap(std::uint64_t characterID, bool seedB)
    {
        if (!Ready()) return;
        std::size_t seeded = 0;
        bool fromFile = false;
        std::string fileChar;
        Core::Bootstrap::BiasMode mode{};
        {
            std::unique_lock lock(m_mutex);
            // fCoveredPrior as it is now (a hot reload changes the setting,
            // not the live weight): the boot model's covered is the start
            // MovedLocked compares against, so it moves with the reset.
            m_bootModel.covered = C::Weight{ m_settings.coveredPrior, 1.0 };
            m_model = m_bootModel;
            m_b.clear();
            mode = m_settings.biasMode;
            fileChar = m_doc ? m_doc->sourceChar : std::string{};
            fromFile = seedB && m_doc && Core::Bootstrap::UseFileBias(mode, characterID, m_doc->SourceCharId());
            const double now = m_playSeconds.load(std::memory_order_relaxed);
            if (fromFile) {
                for (const auto& it : m_bootItems) {
                    C::BBelief b;
                    b.mu = it.b;
                    b.var = it.var;
                    b.n = static_cast<float>(it.picks);
                    b.chosenAt = now;
                    m_b[it.form] = b;
                    ++seeded;
                }
            }
            m_recent.Reset();
            m_onsetCounter.Reset();
            m_taken.clear();
            BuildKeys();
            m_stats.picksLearned = m_stats.onsets = m_stats.onsetsCounted = 0;
            m_stats.bEntries = m_b.size();
            Moved();
            // a load's, or a reset's: the reset site ticks (SettingsReloader,
            // `hg reset all`), whether or not the model is ready
            Note(ClockEvent::Rebootstrap);
        }
        logger::info("[Choice] Reset to the bootstrap for character {:016X}: theta and kappa from the file, b {} "
                     "(sBootstrapBias = {}, file character {})"sv,
            characterID,
            fromFile ? std::format("from the file ({} items)", seeded)
                     : std::string(seedB ? "at its prior N(0, 0.25)" : "cleared (a reset)"),
            Core::Bootstrap::BiasModeName(mode), fileChar.empty() ? "several" : fileChar);
    }

    void ChoiceLearner::ClearTransient()
    {
        std::unique_lock lock(m_mutex);
        m_recent.Reset();
        m_onsetCounter.Reset();
        m_taken.clear();
        Note(ClockEvent::Transient);
    }

    std::shared_ptr<const ChoiceLearner::Snapshot> ChoiceLearner::GetSnapshot()
    {
        {
            std::shared_lock lock(m_mutex);
            if (m_snapshot && m_snapshot->version == m_version) return m_snapshot;
        }
        std::unique_lock lock(m_mutex);
        if (m_snapshot && m_snapshot->version == m_version) return m_snapshot;
        auto s = std::make_shared<Snapshot>();
        s->model = m_model;
        s->b = m_b;
        s->curves = m_curves;
        s->life = m_life;
        s->favoriteLifeMult = m_settings.favoriteLifeMult;
        s->favoritesBoost = m_favoritesBoost;
        s->discovered = m_discovery.BoostedMap();
        s->discovery = m_settings.discovery;
        s->version = m_version;
        m_snapshot = std::move(s);
        return m_snapshot;
    }

    std::vector<double> ChoiceLearner::RecentValues(const Snapshot& s, double t, std::uint32_t gen) const
    {
        std::shared_lock lock(m_mutex);
        return m_recent.Values(s.model, t, gen);
    }

    // =========================================================================
    // THE UPDATE
    // =========================================================================
    void ChoiceLearner::OnPick(const DL::Decision& d, double pickTime)
    {
        const bool nothing = d.outcome == DL::Outcome::Nothing;
        // The item's first pick ends its discovery boost (core/DiscoveryBoost.h),
        // whether or not the update can use the decision, and with the model
        // off too (the fallback ranking still has picks): the player found it.
        bool boostEnded = false;
        {
            std::unique_lock lock(m_mutex);
            boostEnded = m_discovery.OnDecision(nothing, d.form);
            if (boostEnded) {
                Touch();
                Note(ClockEvent::BoostEnded);
            }
        }
        if (boostEnded) {
            logger::debug("[Discovery] {:08X} '{}' picked ({}): its discovery boost ends"sv, d.form, d.name,
                DL::OutcomeName(d.outcome));
        }
        if (!Ready()) return;
        const bool usable = C::UsablePick(nothing, d.ctx != nullptr, d.row >= 0, d.name, d.list);
        std::unique_lock lock(m_mutex);
        if (!usable) {
            Note(ClockEvent::UnusablePick);
            return;
        }
        const DL::Context& ctx = *d.ctx;
        std::vector<const DL::Row*> rows;
        rows.reserve(ctx.rows.size() + d.added.size());
        for (const auto& r : ctx.rows) rows.push_back(&r);
        for (const auto& r : d.added) rows.push_back(&r);
        const auto chosen = static_cast<std::size_t>(d.row);
        if (chosen >= rows.size()) {
            Note(ClockEvent::UnusablePick);
            return;
        }
        auto takenIt = std::find_if(m_taken.begin(), m_taken.end(), [&](const auto& p) { return p.first == ctx.id; });
        if (takenIt == m_taken.end()) {
            m_taken.emplace_back(ctx.id, std::set<std::uint64_t>{});
            if (m_taken.size() > kTakenContexts) m_taken.pop_front();
            takenIt = m_taken.end() - 1;
        }
        const DL::Row& ch = *rows[chosen];
        const std::uint64_t pickId = m_nextPickId++;
        auto record = [&] {
            // the timeline (recent_<family>) and the Plackett-Luce memory count
            // every usable pick, repeats included, as fitlib's do
            takenIt->second.insert(C::StackKey(ch.form, ch.uid));
            C::RecentPick p;
            p.t = pickTime;
            p.gen = d.gen;
            p.id = pickId;
            p.hasCap = ch.cap != nullptr;
            if (ch.cap) {
                std::vector<C::SparseCol> cap;
                for (const auto& v : *ch.cap) cap.emplace_back(static_cast<std::uint16_t>(v.col), static_cast<double>(v.v));
                p.cols = C::PositiveCols(cap);
            }
            m_recent.Add(std::move(p));
        };
        if (d.repeat) {
            // REPEAT_PICK_WINDOW_SEC: the same decision again, no update
            record();
            // no Touch(): m_recent and m_taken are not in the Snapshot (nor
            // in the save: no tick)
            Note(ClockEvent::RepeatPick);
            return;
        }
        const bool keyOrWheel = d.outcome == DL::Outcome::Key || d.outcome == DL::Outcome::Wheel;
        const auto set = C::ChoiceSet(rows, chosen, keyOrWheel, takenIt->second);

        // The situation: the context's needs (through the file's curves) and
        // recent_<family> at the pick, before this pick enters the timeline.
        const std::array<double, C::K> n = Core::Bootstrap::CurveNeeds(m_curves, ctx.need, ctx.input);
        const auto r = m_recent.Values(m_model, pickTime, d.gen);
        const C::Tick tick = C::MakeTick(m_model, n, r);

        // A pick renews the chosen item's b from what is left (the battery).
        const double now = m_playSeconds.load(std::memory_order_relaxed);
        auto lifeMult = [&](bool fav) { return (fav && m_favoritesBoost) ? m_settings.favoriteLifeMult : 1.0; };
        {
            auto [it, fresh] = m_b.try_emplace(ch.form);
            if (fresh) it->second.chosenAt = now;
            it->second.favorite = (ch.flags & DL::Flag::Favorite) != 0;
            C::RenewB(it->second, m_life, now, lifeMult(it->second.favorite));
        }

        std::vector<C::Member> members;
        members.reserve(set.size());
        std::vector<C::Attr> attrs;
        std::vector<C::RowExtra> extras;
        std::vector<C::GateRow> gates;
        std::size_t chosenAt = 0, offPage = 0, outIdx = SIZE_MAX;
        const ContextGate cg = GateOf(ctx);
        for (const auto& e : set) {
            const DL::Row& row = *rows[e.row];
            C::Member m;
            m.attr = C::MakeAttr(m_model.grading, row.cap, row.cross);
            m.covered = row.cross[static_cast<std::size_t>(DL::Cross::covered)];
            m.form = row.form;
            m.offPage = e.offPage;
            m.feat = RowFeatures(row);
            const bool fav = (row.flags & DL::Flag::Favorite) != 0;
            if (const auto bi = m_b.find(row.form); bi != m_b.end()) {
                const auto br = C::ReadB(bi->second, m_life, now, lifeMult(fav));
                m.bMu = br.mu;
                m.bVar = br.var;
            }
            if (e.chosen) chosenAt = members.size();
            offPage += e.offPage ? 1 : 0;
            C::GateRow g;
            if (cg.atLimit && HasSummonColumn(row.cap)) {
                if (row.form == cg.source) {
                    if (outIdx == SIZE_MAX) outIdx = members.size();
                }
                else {
                    g.summon = true;
                    g.other = m.covered > cg.left + 1e-6 ? m.covered : 0.0;
                }
            }
            gates.push_back(g);
            members.push_back(std::move(m));
        }
        // a_out's source outside the choice set (filtered at the limit, but
        // known: a held row of the context): its s-hat from its own row.
        std::optional<C::Member> outside;
        if (cg.atLimit && outIdx == SIZE_MAX && cg.source != 0) {
            for (const DL::Row* row : rows) {
                if (row->form != cg.source) continue;
                C::Member m;
                m.attr = C::MakeAttr(m_model.grading, row->cap, row->cross);
                m.form = row->form;
                m.feat = RowFeatures(*row);
                if (const auto bi = m_b.find(row->form); bi != m_b.end()) {
                    const auto br = C::ReadB(bi->second, m_life, now, lifeMult((row->flags & DL::Flag::Favorite) != 0));
                    m.bMu = br.mu;
                    m.bVar = br.var;
                }
                outside = std::move(m);
                break;
            }
        }
        if (cg.atLimit && (outIdx != SIZE_MAX || outside)) {
            attrs.reserve(members.size() + 1);
            extras.reserve(members.size() + 1);
            for (const auto& m : members) {
                attrs.push_back(m.attr);
                C::RowExtra x;
                x.covered = m.covered;
                x.bMu = m.bMu;
                x.bVar = m.bVar;
                x.feat = m.feat;
                x.offPage = m.offPage;
                extras.push_back(x);
            }
            if (outIdx == SIZE_MAX) {
                attrs.push_back(outside->attr);
                C::RowExtra x;
                x.bMu = outside->bMu;
                x.bVar = outside->bVar;
                x.feat = outside->feat;
                extras.push_back(x);
                gates.push_back(C::GateRow{});
                outIdx = attrs.size() - 1;
            }
            C::ApplyUpgradeGate(m_model, tick, attrs, extras, gates, outIdx, cg.left);
            for (std::size_t i = 0; i < members.size(); ++i) members[i].covered = extras[i].covered;
        }

        const auto res = C::Update(m_model, tick, members, chosenAt);
        for (const auto& mv : res.b) {
            auto [it, fresh] = m_b.try_emplace(mv.form);
            if (fresh) it->second.chosenAt = now;
            C::ApplyB(it->second.mu, it->second.var, mv);
        }
        record();
        ++m_stats.picksLearned;
        m_stats.bEntries = m_b.size();
        Moved();
        Note(ClockEvent::PickLearned);
        logger::debug("[Choice] learned {:08X} '{}' ({}): {} rows ({} off the page), P(chosen) {:.3f}, {} weights and {} b moved"sv,
            d.form, d.name, DL::OutcomeName(d.outcome), members.size(), offPage, std::exp(res.logpChosen),
            res.weightsMoved, res.b.size());
    }

    void ChoiceLearner::OnPipelineRun(const std::array<double, C::K>& n, const std::vector<C::Attr>& page)
    {
        if (!Ready()) return;
        std::vector<std::uint16_t> grown;
        std::size_t onsets = 0;
        {
            std::unique_lock lock(m_mutex);
            const auto on = m_onsetCounter.Step(n);
            if (on.empty()) return;
            onsets = on.size();
            m_stats.onsets += on.size();
            for (const std::uint16_t k : on) {
                const bool answered = std::any_of(page.begin(), page.end(),
                                                  [&](const C::Attr& a) { return C::Answers(m_model, k, a); });
                if (!answered) continue;
                C::GrowNeed(m_model, k, m_settings.thetaProcessNoise);
                ++m_stats.onsetsCounted;
                grown.push_back(k);
            }
            if (!grown.empty()) {
                Touch();
                Note(ClockEvent::NeedGrown);
            }
        }
        if (!grown.empty()) {
            std::string names;
            for (const auto k : grown) names += (names.empty() ? "" : ", ") + std::string(Core::Needs::kNeeds[k].id);
            logger::debug("[Choice] {} onset(s); with an answer on the page (theta variances grown): {}"sv, onsets, names);
        }
    }

    // =========================================================================
    // THE BATTERY
    // =========================================================================
    void ChoiceLearner::AdvancePlayTime(float seconds) noexcept
    {
        if (seconds > 0.0f) {
            m_playSeconds.store(m_playSeconds.load(std::memory_order_relaxed) + seconds, std::memory_order_relaxed);
        }
        // the battery decays at read time; only the sweep's erase is saved
        Note(ClockEvent::PlayTime);
    }

    std::size_t ChoiceLearner::MaybeForgetFaded()
    {
        const double now = m_playSeconds.load(std::memory_order_relaxed);
        double last = m_lastForgetSweep.load(std::memory_order_relaxed);
        if (now - last < kForgetSweepSec || !m_lastForgetSweep.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            return 0;
        }
        std::size_t forgotten = 0;
        std::size_t boostsEnded = 0;
        {
            std::unique_lock lock(m_mutex);
            // The discovery boosts that ran out (their excess under 1%): a
            // variance-only change, so Touch, no latch.
            boostsEnded = m_discovery.Expire(m_settings.discovery, now);
            if (boostsEnded) {
                Touch();
                Note(ClockEvent::BoostsExpired);
            }
            for (auto it = m_b.begin(); it != m_b.end();) {
                const double mult = (it->second.favorite && m_favoritesBoost) ? m_settings.favoriteLifeMult : 1.0;
                if (Core::Battery::Faded(m_life, now, it->second.chosenAt, it->second.n, mult)) {
                    it = m_b.erase(it);
                    ++forgotten;
                }
                else {
                    ++it;
                }
            }
            if (forgotten) {
                m_stats.bEntries = m_b.size();
                Moved();
                Note(ClockEvent::BForgotten);
            }
        }
        if (forgotten) logger::info("[Choice] {} b entr{} past their useful life of play forgotten"sv, forgotten, forgotten == 1 ? "y" : "ies");
        if (boostsEnded) logger::debug("[Discovery] {} discovery boost{} ran out"sv, boostsEnded, boostsEnded == 1 ? "" : "s");
        return forgotten;
    }

    // =========================================================================
    // THE DISCOVERY BOOST (0.24.14, core/DiscoveryBoost.h)
    // =========================================================================
    Core::Discovery::Tracker::Gain ChoiceLearner::OnGained(std::uint32_t form, const char* via)
    {
        using Gain = Core::Discovery::Tracker::Gain;
        {
            // Most gains are forms already seen (arrows picked back up, a
            // potion restocked): answer those under the shared lock.
            std::shared_lock lock(m_mutex);
            if (!m_discovery.Live() || form == 0) return Gain::Dropped;
            if (m_discovery.Seen(form)) return Gain::Known;
        }
        Gain g = Gain::Dropped;
        Core::Discovery::Params p;
        {
            std::unique_lock lock(m_mutex);
            g = m_discovery.OnGained(form, m_playSeconds.load(std::memory_order_relaxed));
            if (g == Gain::New) {
                Touch();   // the snapshot carries the boosts; mu unchanged: no latch
                Note(ClockEvent::NewlyGained);
            }
            p = m_settings.discovery;
        }
        if (g == Gain::New) {
            const auto* f = RE::TESForm::LookupByID(form);
            const char* name = f ? f->GetName() : "";
            logger::debug("[Discovery] {:08X} '{}' newly gained ({}): b's variance x{:.2f} for the wildcard draw, "
                          "halving every {:.0f} min of play{}"sv,
                form, name ? name : "", via, p.varMult, p.halfLifeSec / 60.0, p.enabled ? "" : " (bDiscoveryBoost off)");
        }
        return g;
    }

    void ChoiceLearner::SuspendDiscovery()
    {
        std::unique_lock lock(m_mutex);
        m_discovery.Suspend();
        Note(ClockEvent::LoadSuspend);
    }

    std::size_t ChoiceLearner::PrimeDiscovery(const std::vector<std::uint32_t>& present)
    {
        std::size_t added = 0, fromB = 0, gone = 0, seen = 0, boosted = 0;
        Core::Discovery::Params p;
        {
            std::unique_lock lock(m_mutex);
            // Dynamic forms (0xFF) the save named that no longer exist: the
            // engine reuses those IDs, so a later item under one must not read
            // as seen. Game thread, after the load (InitializeGameSystems).
            gone = m_discovery.DropDynamicIf([](std::uint32_t f) { return RE::TESForm::LookupByID(f) == nullptr; });
            added = m_discovery.Prime(present);
            // A b entry proves the character held the item (it was picked, or
            // a candidate passed over): seen, though it may be used up, sold
            // or in a chest now -- on the first load with DISC those would
            // otherwise read as newly gained when they come back.
            std::vector<std::uint32_t> bForms;
            bForms.reserve(m_b.size());
            for (const auto& [form, belief] : m_b) {
                (void)belief;
                bForms.push_back(form);
            }
            fromB = m_discovery.Prime(bForms);
            seen = m_discovery.SeenCount();
            boosted = m_discovery.BoostedCount();
            p = m_settings.discovery;
            Touch();
            Note(ClockEvent::LoadPrime);
        }
        logger::info("[Discovery] Load: {} forms present ({} new to this character's seen set, not boosted; {} more from "
                     "b's entries; {} vanished dynamic forms dropped); {} known in all, {} boosted (bDiscoveryBoost = {}, "
                     "x{:.2f} halving every {:.0f} min of play)"sv,
            present.size(), added, fromB, gone, seen, boosted, p.enabled, p.varMult, p.halfLifeSec / 60.0);
        return added + fromB;
    }

    Core::Discovery::Saved ChoiceLearner::ExportDiscovery() const
    {
        std::shared_lock lock(m_mutex);
        return m_discovery.Export(m_playSeconds.load(std::memory_order_relaxed));
    }

    void ChoiceLearner::ImportDiscovery(const Core::Discovery::Saved& saved)
    {
        std::unique_lock lock(m_mutex);
        m_discovery.Import(saved, m_playSeconds.load(std::memory_order_relaxed));
        Touch();
        Note(ClockEvent::LoadImport);
    }

    std::pair<std::size_t, std::size_t> ChoiceLearner::ReplaceDynamicDiscovery(const Core::Discovery::Saved& saved)
    {
        std::unique_lock lock(m_mutex);
        const auto r = m_discovery.ReplaceDynamic(saved, m_playSeconds.load(std::memory_order_relaxed));
        Touch();
        Note(ClockEvent::LoadReplaceDynamic);
        return r;
    }

    void ChoiceLearner::ClearDiscovery()
    {
        std::unique_lock lock(m_mutex);
        m_discovery.Clear();
        Touch();
        Note(ClockEvent::LoadClearDiscovery);
    }

    ChoiceLearner::DiscoveryStats ChoiceLearner::GetDiscoveryStats() const
    {
        std::shared_lock lock(m_mutex);
        return DiscoveryStats{ m_discovery.SeenCount(), m_discovery.BoostedCount(), m_discovery.Live() };
    }

    std::pair<double, double> ChoiceLearner::DiscoveryOf(std::uint32_t form) const
    {
        std::shared_lock lock(m_mutex);
        const auto& map = m_discovery.BoostedMap();
        const auto it = map.find(form);
        if (it == map.end()) return { 1.0, -1.0 };
        const double age = m_playSeconds.load(std::memory_order_relaxed) - it->second;
        return { Core::Discovery::Mult(m_settings.discovery, age), age };
    }

    // =========================================================================
    // EXPORT / IMPORT (the cosave: persist/ChoiceSerializer.cpp)
    // =========================================================================
    bool ChoiceLearner::MovedLocked(std::size_t slot) const
    {
        const auto* a = SlotWeight(m_model, slot);
        if (!a) return false;
        // covered (the last slot, BuildKeys) is compared against the prior the
        // model started from (m_bootModel.covered, set at the load or reset),
        // not against fCoveredPrior now: after a hot reload changed the key,
        // an untouched weight is not "moved", so THTA does not save the old
        // prior over the new one (0.24.18).
        const auto* b = SlotWeight(m_bootModel, slot);
        if (!b) return true;
        return a->mu != b->mu || a->var != b->var;
    }

    std::vector<ChoiceLearner::Entry> ChoiceLearner::ExportEntries(bool movedOnly) const
    {
        std::shared_lock lock(m_mutex);
        std::vector<Entry> out;
        out.reserve(movedOnly ? 256 : m_keys.size());
        for (std::size_t s = 0; s < m_keys.size(); ++s) {
            if (movedOnly && !MovedLocked(s)) continue;
            if (const auto* w = SlotWeight(m_model, s)) out.push_back(Entry{ m_keys[s], w->mu, w->var });
        }
        return out;
    }

    std::vector<std::uint64_t> ChoiceLearner::Keys() const
    {
        std::shared_lock lock(m_mutex);
        return m_keys;
    }

    std::size_t ChoiceLearner::ImportEntries(const std::vector<Entry>& entries)
    {
        std::unique_lock lock(m_mutex);
        std::size_t applied = 0;
        for (const auto& e : entries) {
            const auto it = m_slotOf.find(e.key);
            if (it == m_slotOf.end() || !(e.var > 0.0) || !std::isfinite(e.mu)) continue;
            if (auto* w = SlotWeight(m_model, it->second)) {
                *w = C::Weight{ e.mu, e.var };
                ++applied;
            }
        }
        Moved();
        Note(ClockEvent::LoadImport);
        return applied;
    }

    std::vector<ChoiceLearner::BEntry> ChoiceLearner::ExportB() const
    {
        std::shared_lock lock(m_mutex);
        const double now = m_playSeconds.load(std::memory_order_relaxed);
        std::vector<BEntry> out;
        out.reserve(m_b.size());
        for (const auto& [form, b] : m_b) {
            const double minutes = std::floor(std::max(0.0, now - b.chosenAt) / 60.0);
            out.push_back(BEntry{ form, b.mu, b.var, b.n, static_cast<std::uint32_t>(std::min(minutes, 4.0e9)), b.favorite });
        }
        return out;
    }

    void ChoiceLearner::ImportB(const std::vector<BEntry>& entries)
    {
        std::unique_lock lock(m_mutex);
        const double now = m_playSeconds.load(std::memory_order_relaxed);
        m_b.clear();
        for (const auto& e : entries) {
            if (!(e.var > 0.0) || !std::isfinite(e.mu)) continue;
            C::BBelief b;
            b.mu = e.mu;
            b.var = e.var;
            b.n = e.n;
            b.chosenAt = now - 60.0 * e.minutesSinceChosen;
            b.favorite = e.favorite;
            m_b[e.form] = b;
        }
        m_stats.bEntries = m_b.size();
        Moved();
        Note(ClockEvent::LoadImport);
    }

    std::pair<std::size_t, std::size_t> ChoiceLearner::ReplaceDynamicB(const std::vector<BEntry>& saved)
    {
        auto dynamic = [](std::uint32_t form) { return (form >> 24) == 0xFF; };
        std::unique_lock lock(m_mutex);
        const double now = m_playSeconds.load(std::memory_order_relaxed);
        std::size_t removed = 0, added = 0;
        for (auto it = m_b.begin(); it != m_b.end();) {
            if (dynamic(it->first)) {
                it = m_b.erase(it);
                ++removed;
            }
            else {
                ++it;
            }
        }
        for (const auto& e : saved) {
            if (!dynamic(e.form) || !(e.var > 0.0) || !std::isfinite(e.mu)) continue;
            C::BBelief b;
            b.mu = e.mu;
            b.var = e.var;
            b.n = e.n;
            b.chosenAt = now - 60.0 * e.minutesSinceChosen;
            b.favorite = e.favorite;
            m_b[e.form] = b;
            ++added;
        }
        m_stats.bEntries = m_b.size();
        if (removed || added) Moved();
        Note(ClockEvent::LoadReplaceDynamic);
        return { removed, added };
    }

    ChoiceLearner::Stats ChoiceLearner::GetStats() const
    {
        std::shared_lock lock(m_mutex);
        Stats s = m_stats;
        s.bEntries = m_b.size();
        s.thetaEntries = m_keys.size();
        // Moved from the bootstrap: the same slot in both models, compared
        // exactly (an imported entry equal to the file's counts as not moved).
        // What THTA writes (ExportEntries(true)).
        s.thetaMoved = 0;
        for (std::size_t slot = 0; slot < m_keys.size(); ++slot) {
            if (MovedLocked(slot)) ++s.thetaMoved;
        }
        return s;
    }

    ChoiceLearner::DriftReport ChoiceLearner::GetDrift() const
    {
        std::shared_lock lock(m_mutex);
        DriftReport r;
        r.ready = Ready();
        if (!r.ready) return r;
        r.theta = Core::Choice::ComputeDrift(m_model.theta, m_bootModel.theta);
        if (m_model.HasStrength()) r.strength = Core::Choice::ComputeDrift(m_model.strength, m_bootModel.strength);
        std::vector<std::pair<std::uint32_t, double>> bs;
        bs.reserve(m_b.size());
        for (const auto& [form, belief] : m_b) bs.emplace_back(form, belief.mu);
        r.b = Core::Choice::ComputeBDrift(bs);
        r.covered = m_model.covered;
        // theta_covered's start: fCoveredPrior with precision 1 as it was at
        // the load or reset (MovedLocked).
        r.coveredBoot = m_bootModel.covered;
        return r;
    }

    std::vector<ChoiceLearner::ThetaRow> ChoiceLearner::ThetaRows(bool movedOnly) const
    {
        std::shared_lock lock(m_mutex);
        std::vector<ThetaRow> out;
        if (!Ready() || m_model.theta.size() != C::K * C::J || m_bootModel.theta.size() != C::K * C::J) return out;
        for (std::size_t k = 0; k < C::K; ++k) {
            for (std::size_t j = 0; j < C::J; ++j) {
                const auto& a = m_model.Th(k, j);
                const auto& b = m_bootModel.Th(k, j);
                if (movedOnly && a.mu == b.mu && a.var == b.var) continue;
                out.push_back(ThetaRow{ static_cast<std::uint16_t>(k), static_cast<std::uint16_t>(j), a, b });
            }
        }
        return out;
    }

    std::pair<std::string, std::string> ChoiceLearner::FileIdentity() const
    {
        std::shared_lock lock(m_mutex);
        return { m_createdUtc, m_sha256 };
    }

    std::vector<Core::Bootstrap::BuildCurve> ChoiceLearner::Curves() const
    {
        std::shared_lock lock(m_mutex);
        return m_curves;
    }
}
