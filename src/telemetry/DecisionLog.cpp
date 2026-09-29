#include "DecisionLog.h"
#include "ItemKey.h"
#include "JsonLine.h"

#include "Config.h"
#include "Globals.h"                    // g_utilityScorer (cfg record)
#include "learning/ScorerSettings.h"
#include "learning/UtilityScorer.h"
#include "learning/WildcardManager.h"
#include "slot/SlotConfig.h"            // SlotClassificationToString
#include "slot/SlotLocker.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <utility>

namespace Huginn::Telemetry
{
    namespace
    {
        // StateFeatures::ToArray() order. Written into every session header so a
        // reader never has to hard-code it.
        constexpr std::array<std::string_view, Learning::StateFeatures::NUM_FEATURES> kFeatureNames{
            "healthPct"sv, "magickaPct"sv, "staminaPct"sv,
            "inCombat"sv, "isSneaking"sv, "distanceNorm"sv,
            "targetNone"sv, "targetHumanoid"sv, "targetUndead"sv,
            "targetBeast"sv, "targetConstruct"sv, "targetDragon"sv, "targetDaedra"sv,
            "hasMeleeEquipped"sv, "hasBowEquipped"sv, "hasSpellEquipped"sv, "hasShieldEquipped"sv,
            "bias"sv,
        };
        static_assert(Learning::StateFeatures::NUM_FEATURES == 18,
            "StateFeatures changed: update kFeatureNames (ToArray order) and bump SCHEMA_VERSION");

        // Stable machine names for ContextReason. The enum carries no text of its
        // own (the display layer owns the wording, which may change), so the log
        // keeps its own identifiers. Changing one is a schema change.
        [[nodiscard]] constexpr std::string_view ContextReasonName(Context::ContextReason r) noexcept
        {
            static_assert(Context::CONTEXT_REASON_COUNT == 27,
                "ContextReason changed: add the new case here and bump SCHEMA_VERSION");
            using R = Context::ContextReason;
            switch (r) {
            case R::None:            return "None";
            case R::CriticalHealth:  return "CriticalHealth";
            case R::Underwater:      return "Underwater";
            case R::LookingAtLock:   return "LookingAtLock";
            case R::OnFire:          return "OnFire";
            case R::Poisoned:        return "Poisoned";
            case R::Diseased:        return "Diseased";
            case R::TakingFrost:     return "TakingFrost";
            case R::TakingShock:     return "TakingShock";
            case R::Falling:         return "Falling";
            case R::LowHealth:       return "LowHealth";
            case R::LowMagicka:      return "LowMagicka";
            case R::LowStamina:      return "LowStamina";
            case R::WeaponLowCharge: return "WeaponLowCharge";
            case R::NeedsAmmo:       return "NeedsAmmo";
            case R::AllyInjured:     return "AllyInjured";
            case R::AtForge:         return "AtForge";
            case R::AtEnchanter:     return "AtEnchanter";
            case R::AtAlchemy:       return "AtAlchemy";
            case R::LookingAtOre:    return "LookingAtOre";
            case R::InDarkness:      return "InDarkness";
            case R::Sneaking:        return "Sneaking";
            case R::TargetUndead:    return "TargetUndead";
            case R::TargetDaedra:    return "TargetDaedra";
            case R::TargetDragon:    return "TargetDragon";
            case R::MultipleEnemies: return "MultipleEnemies";
            case R::EnemyCasting:    return "EnemyCasting";
            default:                 return "?";
            }
        }

        [[nodiscard]] std::string_view FavoritesModeName(Scoring::FavoritesMode m) noexcept
        {
            switch (m) {
            case Scoring::FavoritesMode::Boost:    return "Boost";
            case Scoring::FavoritesMode::Off:      return "Off";
            case Scoring::FavoritesMode::Suppress: return "Suppress";
            default:                               return "?";
            }
        }

        [[nodiscard]] std::string_view PotionTierName(Scoring::PotionTierPreference p) noexcept
        {
            switch (p) {
            case Scoring::PotionTierPreference::Higher: return "Higher";
            case Scoring::PotionTierPreference::None:   return "None";
            case Scoring::PotionTierPreference::Lower:  return "Lower";
            default:                                    return "?";
            }
        }

        template <size_t N>
        void AppendFloatArray(std::string& out, const std::array<float, N>& values)
        {
            out += '[';
            for (size_t i = 0; i < N; ++i) {
                if (i) out += ',';
                Json::AppendFloat(out, values[i]);
            }
            out += ']';
        }

        /// 128 random bits as hex. std::random_device only: nothing about the
        /// player, machine or time goes into it.
        [[nodiscard]] std::string MakeSessionId()
        {
            std::random_device rd;
            std::string id;
            id.reserve(32);
            for (int i = 0; i < 4; ++i) {
                std::format_to(std::back_inserter(id), "{:08x}", static_cast<std::uint32_t>(rd()));
            }
            return id;
        }
    }

    // =========================================================================
    // SINGLETON
    // =========================================================================

    DecisionLog& DecisionLog::GetSingleton()
    {
        // Intentionally leaked (see LIFETIME in the header).
        static DecisionLog* s_instance = new DecisionLog();
        return *s_instance;
    }

    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    void DecisionLog::ApplyConfig(const TelemetryConfig& config)
    {
        std::scoped_lock lk(m_lifecycleMutex);

        const TelemetryConfig previous = m_config;
        m_config = config;

        m_maxBytes.store(static_cast<std::uint64_t>(std::max<std::uint32_t>(config.maxFileSizeMB, 1u)) << 20,
            std::memory_order_relaxed);
        m_maxRotated.store(config.maxRotatedFiles, std::memory_order_relaxed);
        m_topCandidates.store(config.topCandidates, std::memory_order_relaxed);

        const bool running = m_writer.joinable();

        if (!config.enabled) {
            if (running) {
                m_enabled.store(false, std::memory_order_release);
                StopWriterLocked();
                logger::info("[Telemetry] Decision log disabled ({} records written, {} dropped)"sv,
                    m_written.load(), m_dropped.load());
            }
            return;
        }

        // Enabled. A file-name change means a different file: restart the
        // writer so the path it reads is never changed under it.
        if (running && previous.fileName != config.fileName) {
            m_enabled.store(false, std::memory_order_release);
            StopWriterLocked();
        }

        if (!m_writer.joinable()) {
            const auto logDir = SKSE::log::log_directory();
            if (!logDir) {
                logger::error("[Telemetry] No SKSE log directory - decision log stays off"sv);
                return;
            }
            m_filePath = *logDir / config.fileName;
            StartWriterLocked();
            logger::info("[Telemetry] Decision log enabled -> {} (SKSE log folder), cap {} MiB x {} rotated"sv,
                config.fileName, config.maxFileSizeMB, config.maxRotatedFiles);
        }

        {
            // Re-describe the policy on the next impression: this is startup or
            // a reload, and the scorer/wildcard values may just have changed.
            // The first impression after (re)enabling is written unconditionally.
            std::scoped_lock plk(m_producerMutex);
            m_cfgPending = true;
            m_forceNextImpression = true;
        }
        m_enabled.store(true, std::memory_order_release);
    }

    void DecisionLog::StartWriterLocked()
    {
        if (!m_sessionStarted.load(std::memory_order_acquire)) {
            m_sessionId = MakeSessionId();
            m_sessionStart = std::chrono::steady_clock::now();
            m_sessionStarted.store(true, std::memory_order_release);
        }
        m_writer = std::jthread([this](std::stop_token st) { WriterLoop(st); });
    }

    void DecisionLog::StopWriterLocked()
    {
        if (!m_writer.joinable()) {
            return;
        }
        m_writer.request_stop();
        m_queueCv.notify_all();
        m_writer.join();
        m_writer = std::jthread{};

        // Anything a producer slipped in after the writer's final drain belongs
        // to the session that just ended; do not replay it into the next file.
        // They already hold sequence numbers, so they count as dropped.
        std::scoped_lock qlk(m_queueMutex);
        if (!m_queue.empty()) {
            m_dropped.fetch_add(m_queue.size(), std::memory_order_relaxed);
            m_queue.clear();
        }
    }

    DecisionLog::Status DecisionLog::GetStatus() const
    {
        Status s;
        s.enabled = IsEnabled();
        s.written = m_written.load(std::memory_order_relaxed);
        s.dropped = m_dropped.load(std::memory_order_relaxed);
        s.fileBytes = m_fileBytes.load(std::memory_order_relaxed);
        {
            std::scoped_lock qlk(m_queueMutex);
            s.queued = m_queue.size();
        }
        {
            std::scoped_lock lk(m_lifecycleMutex);
            s.fileName = m_config.fileName;
        }
        return s;
    }

    // =========================================================================
    // QUEUE + WRITER
    // =========================================================================

    void DecisionLog::Enqueue(std::string&& line)
    {
        {
            std::scoped_lock lk(m_queueMutex);
            // Every record takes a sequence number, INCLUDING the ones dropped
            // below, so a reader sees the gap as well as the drop record.
            const std::uint64_t n = m_seq++;
            if (m_queue.size() >= QUEUE_CAPACITY) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                m_droppedUnreported.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // Producers build `{"t":...}`; the sequence number goes first.
            line.insert(1, std::format("\"n\":{},", n));
            line += '\n';
            m_queue.push_back(std::move(line));
        }
        m_queueCv.notify_one();
    }

    std::string DecisionLog::BuildSessionHeader()
    {
        std::uint64_t n = 0;
        {
            std::scoped_lock lk(m_queueMutex);
            n = m_seq++;
        }

        std::string out;
        out.reserve(512);
        Json::Object o(out);
        o.UInt("n", n);
        o.Str("t", "session");
        o.Int("schema", SCHEMA_VERSION);
        o.Str("plugin", Plugin::VERSION.string());
        o.Str("sid", m_sessionId);
        o.Int("rt", RealMsSinceSession());
        o.Key("features");
        out += '[';
        for (size_t i = 0; i < kFeatureNames.size(); ++i) {
            if (i) out += ',';
            Json::AppendEscaped(out, kFeatureNames[i]);
        }
        out += ']';
        o.Key("rewards");
        {
            Json::Object r(out);
            r.Num("equip", Config::EQUIP_REWARD);
            r.Num("consume", Config::CONSUME_REWARD);
            r.Num("misclick", Config::MISCLICK_PENALTY);
            r.Close();
        }
        o.UInt("qcap", QUEUE_CAPACITY);
        o.Close();
        out += '\n';
        return out;
    }

    void DecisionLog::WriterLoop(std::stop_token stop)
    {
        // FILE IO ONLY. No RE:: calls on this thread.
        std::ofstream out;
        std::uint64_t bytes = 0;
        bool openFailureLogged = false;

        const auto writeRaw = [&](const std::string& line) {
            out.write(line.data(), static_cast<std::streamsize>(line.size()));
            bytes += line.size();
            m_fileBytes.store(bytes, std::memory_order_relaxed);
        };

        const auto openFile = [&]() -> bool {
            std::error_code ec;
            bytes = 0;
            if (std::filesystem::exists(m_filePath, ec)) {
                const auto sz = std::filesystem::file_size(m_filePath, ec);
                if (!ec) bytes = static_cast<std::uint64_t>(sz);
            }
            out.clear();
            // Append: several game sessions accumulate in one file (each opens
            // with its own session header) until the size cap rotates it.
            out.open(m_filePath, std::ios::binary | std::ios::app);
            if (!out.is_open() || !out) {
                if (!openFailureLogged) {
                    // Filename only: the full path contains the Windows user folder.
                    logger::error("[Telemetry] Could not open {} for writing; records are dropped until it opens"sv,
                        m_filePath.filename().string());
                    openFailureLogged = true;
                }
                out.close();
                return false;
            }
            openFailureLogged = false;
            m_fileBytes.store(bytes, std::memory_order_relaxed);
            writeRaw(BuildSessionHeader());
            return true;
        };

        const auto rotatedPath = [this](std::uint32_t k) {
            const auto stem = m_filePath.stem().string();
            const auto ext = m_filePath.extension().string();
            return m_filePath.parent_path() / std::format("{}.{}{}", stem, k, ext);
        };

        const auto rotate = [&]() {
            out.flush();
            out.close();
            std::error_code ec;
            const std::uint32_t keep = m_maxRotated.load(std::memory_order_relaxed);
            if (keep == 0) {
                std::filesystem::remove(m_filePath, ec);
            } else {
                std::filesystem::remove(rotatedPath(keep), ec);
                for (std::uint32_t k = keep - 1; k >= 1; --k) {
                    const auto from = rotatedPath(k);
                    if (std::filesystem::exists(from, ec)) {
                        std::filesystem::rename(from, rotatedPath(k + 1), ec);
                    }
                }
                std::filesystem::rename(m_filePath, rotatedPath(1), ec);
            }
            openFile();
        };

        openFile();

        std::deque<std::string> batch;
        bool running = true;
        while (running) {
            {
                std::unique_lock lk(m_queueMutex);
                m_queueCv.wait(lk, stop, [this] { return !m_queue.empty(); });
                batch.swap(m_queue);
                // Stop requested: write what was drained, then exit.
                if (stop.stop_requested()) {
                    running = false;
                }
            }

            if (!out.is_open()) {
                openFile();  // retry after an earlier failure
            }
            if (!out.is_open()) {
                // Also unreported: the drop record goes out as soon as a later
                // batch manages to open the file.
                m_dropped.fetch_add(batch.size(), std::memory_order_relaxed);
                m_droppedUnreported.fetch_add(batch.size(), std::memory_order_relaxed);
                batch.clear();
                continue;
            }

            if (const auto lost = m_droppedUnreported.exchange(0, std::memory_order_relaxed); lost > 0) {
                std::uint64_t n = 0;
                {
                    std::scoped_lock lk(m_queueMutex);
                    n = m_seq++;
                }
                writeRaw(std::format("{{\"n\":{},\"t\":\"drop\",\"rt\":{},\"count\":{}}}\n",
                    n, RealMsSinceSession(), lost));
            }

            const std::uint64_t cap = m_maxBytes.load(std::memory_order_relaxed);
            for (const auto& line : batch) {
                if (bytes > 0 && bytes + line.size() > cap) {
                    rotate();
                    if (!out.is_open()) {
                        break;
                    }
                }
                writeRaw(line);
                m_written.fetch_add(1, std::memory_order_relaxed);
            }
            out.flush();
            batch.clear();
            if (out.is_open() && !out) {
                // Write error (disk full, file yanked): close so the next batch
                // retries the open instead of writing into a failed stream.
                out.close();
            }
        }

        out.flush();
        out.close();
    }

    // =========================================================================
    // PRODUCER HELPERS (may touch forms; never do IO)
    // =========================================================================

    std::int64_t DecisionLog::RealMsSinceSession() const
    {
        if (!m_sessionStarted.load(std::memory_order_acquire)) {
            return 0;
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_sessionStart).count();
    }

    std::optional<float> DecisionLog::GameSecondsSinceLoad() const
    {
        const double base = m_gameTimeBaseDays.load(std::memory_order_relaxed);
        if (base < 0.0) {
            return std::nullopt;
        }
        auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            return std::nullopt;
        }
        const double nowDays = static_cast<double>(calendar->GetCurrentGameTime());
        return static_cast<float>((nowDays - base) * 86400.0);
    }

    std::string DecisionLog::ItemKeyFor(RE::FormID formID)
    {
        {
            std::scoped_lock lk(m_keyMutex);
            if (const auto it = m_keyCache.find(formID); it != m_keyCache.end()) {
                return it->second;
            }
        }

        const LocalId local = SplitFormID(formID);
        std::string plugin;
        if (!local.dynamic) {
            if (const auto* form = RE::TESForm::LookupByID(formID)) {
                // File 0 is the ORIGINATING plugin (the one that owns the id),
                // not the last override -- that is what makes the key stable.
                if (const auto* file = form->GetFile(0)) {
                    plugin = std::string(file->GetFilename());
                }
            }
        }
        std::string key = FormatItemKey(plugin, local);

        std::scoped_lock lk(m_keyMutex);
        // Dynamic ids are recycled by the engine; not worth caching.
        if (!local.dynamic) {
            m_keyCache.emplace(formID, key);
        }
        return key;
    }

    std::string DecisionLog::BuildCfgRecord()
    {
        // Main thread (called from RecordImpression): the live scorer config
        // is the one that ranked this tick.
        const Scoring::ScorerConfig cfg = g_utilityScorer
            ? g_utilityScorer->GetConfig()
            : Scoring::ScorerSettings::GetSingleton().BuildConfig();

        std::string out;
        out.reserve(512);
        Json::Object o(out);
        o.Str("t", "cfg");
        o.Int("rt", RealMsSinceSession());
        o.Num("lambdaMin", cfg.lambdaMin);
        o.Num("lambdaMax", cfg.lambdaMax);
        o.Num("explore", cfg.explorationWeight);
        o.Num("coldBoost", cfg.coldStartUCBBoost);
        o.Num("minUtil", cfg.minimumUtility);
        o.Num("minCtx", cfg.minimumContextWeight);
        o.UInt("topN", cfg.topNCandidates);
        o.Str("favMode", FavoritesModeName(cfg.favoritesMode));
        o.Num("favMin", cfg.favoritesBoostMin);
        o.Num("favMax", cfg.favoritesBoostMax);
        o.Str("potionTier", PotionTierName(cfg.potionTierPreference));
        if (g_utilityScorer) {
            const auto& wc = g_utilityScorer->GetWildcardManager();
            o.Bool("wcOn", wc.IsEnabled());
            o.Num("wcBase", wc.GetBaseProbability());
            o.Num("wcMax", wc.GetMaxProbability());
            o.Num("wcCooldown", wc.GetCooldown());
            o.Num("wcRefractory", wc.GetRefractoryPeriod());
            o.Bool("wcFirstExcluded", wc.IsFirstSlotExcluded());
        }
        o.UInt("topCands", m_topCandidates.load(std::memory_order_relaxed));
        // TODO(integration: B-fit): o.Str("fitMode", ...)
        o.Close();
        return out;
    }

    void DecisionLog::AppendCandidateProps(std::string& out, const Candidate::CandidateVariant& variant)
    {
        Json::Object p(out);
        std::visit([&p](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                p.Str("type", Spell::SpellTypeToString(c.type));
                p.Str("school", Spell::MagicSchoolToString(c.school));
                p.Str("element", Spell::ElementTypeToString(c.element));
                p.UInt("baseCost", c.baseCost);
                p.Num("effCost", c.effectiveCost);
                p.Num("range", c.range);
                p.Bool("conc", c.isConcentration);
                p.Bool("afford", c.canAfford);
                p.Hex("tags", static_cast<std::uint64_t>(c.tags));
                p.Hex("tagsExt", static_cast<std::uint64_t>(c.tagsExt));
            } else if constexpr (std::is_same_v<T, Candidate::ScrollCandidate>) {
                // No baseCost: ScrollCandidate does not carry it on this schema.
                p.Str("type", Spell::SpellTypeToString(c.type));
                p.Str("school", Spell::MagicSchoolToString(c.school));
                p.Str("element", Spell::ElementTypeToString(c.element));
                p.Num("mag", c.magnitude);
                p.Num("dur", c.duration);
                p.Int("count", c.count);
                p.Hex("tags", static_cast<std::uint64_t>(c.tags));
                p.Hex("tagsExt", static_cast<std::uint64_t>(c.tagsExt));
            } else if constexpr (std::is_same_v<T, Candidate::ItemCandidate>) {
                p.Str("type", Item::ItemTypeToString(c.type));
                p.Str("school", Item::MagicSchoolToString(c.school));
                p.Num("mag", c.magnitude);
                p.Num("dur", c.duration);
                p.Int("count", c.count);
                p.Hex("tags", static_cast<std::uint64_t>(c.tags));
                p.Hex("tagsExt", static_cast<std::uint64_t>(c.tagsExt));
            } else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                p.Str("type", Weapon::WeaponTypeToString(c.type));
                p.Num("dmg", c.damage);
                p.Num("speed", c.speed);
                p.Num("charge", c.GetChargePercent());
                p.Bool("ench", c.hasEnchantment);
                p.Hex("tags", static_cast<std::uint64_t>(c.tags));
            } else if constexpr (std::is_same_v<T, Candidate::AmmoCandidate>) {
                p.Str("type", Weapon::AmmoTypeToString(c.type));
                p.Num("dmg", c.baseDamage);
                p.Bool("ench", c.hasEnchantment);
                p.Int("count", c.count);
                p.Hex("tags", static_cast<std::uint64_t>(c.tags));
            } else if constexpr (std::is_same_v<T, Candidate::ApparelCandidate>) {
                p.Str("craft", Apparel::CraftSkillToString(c.craftSkill));
                p.Num("mag", c.magnitude);
            } else {
                static_assert(Candidate::always_false_v<T>, "Unhandled CandidateVariant alternative in telemetry props");
            }
        }, variant);
        p.Close();
    }

    // =========================================================================
    // PRODUCERS
    // =========================================================================

    void DecisionLog::OnGameLoaded(bool isNewGame)
    {
        // Always rebase, enabled or not, so a mid-session enable measures game
        // time from this load rather than from process start.
        if (auto* calendar = RE::Calendar::GetSingleton()) {
            m_gameTimeBaseDays.store(static_cast<double>(calendar->GetCurrentGameTime()),
                std::memory_order_relaxed);
        }

        {
            std::scoped_lock lk(m_producerMutex);
            // A different character: never diff its first display against the
            // previous one's, and never join its rewards to the old impressions.
            m_lastSignature.clear();
            m_lastPage = SIZE_MAX;
            m_lastImpId = 0;
            m_forceNextImpression = true;
            m_cfgPending = true;
        }

        if (!IsEnabled()) {
            return;
        }

        std::string out;
        Json::Object o(out);
        o.Str("t", "load");
        o.Int("rt", RealMsSinceSession());
        o.Bool("new", isNewGame);
        o.Close();
        Enqueue(std::move(out));
    }

    void DecisionLog::RecordImpression(const ImpressionInput& in)
    {
        if (!IsEnabled()) {
            return;
        }

        // ---- transition gate ------------------------------------------------
        std::vector<SlotSig> sig;
        sig.reserve(in.assignments.size());
        for (const auto& a : in.assignments) {
            const bool empty = a.IsEmpty() || a.formID == 0;
            sig.push_back({ a.slotIndex, empty ? 0u : a.formID,
                            empty ? std::uint16_t{ 0 } : a.uniqueID, a.type });
        }

        std::uint64_t impId = 0;
        bool emitCfg = false;
        {
            std::scoped_lock lk(m_producerMutex);
            if (!m_forceNextImpression && in.pageIndex == m_lastPage && sig == m_lastSignature) {
                return;  // nothing the player can see changed
            }
            m_forceNextImpression = false;
            m_lastPage = in.pageIndex;
            m_lastSignature = std::move(sig);
            impId = ++m_impCounter;
            m_lastImpId = impId;
            emitCfg = std::exchange(m_cfgPending, false);
        }

        if (emitCfg) {
            Enqueue(BuildCfgRecord());
        }

        // ---- candidate set: top-K by utility + anything displayed ------------
        // ctx.scoredCandidates is only partially sorted (top-N prefix, then the
        // unsorted tail, then remembered-only extras), so rank pointers here.
        std::vector<const Scoring::ScoredCandidate*> cands;
        cands.reserve(in.scored.size());
        for (const auto& sc : in.scored) {
            if (!sc.isRememberedOnly) {
                cands.push_back(&sc);
            }
        }
        const size_t k = std::min<size_t>(m_topCandidates.load(std::memory_order_relaxed), cands.size());
        std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(k), cands.end(),
            [](const Scoring::ScoredCandidate* a, const Scoring::ScoredCandidate* b) { return *a < *b; });
        cands.resize(k);

        const auto indexOf = [&cands](RE::FormID id, std::uint16_t uid) -> int {
            for (size_t i = 0; i < cands.size(); ++i) {
                if (cands[i]->GetFormID() == id && cands[i]->GetUniqueID() == uid) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };

        for (const auto& a : in.assignments) {
            if (a.IsEmpty() || !a.candidate.has_value()) continue;
            if (indexOf(a.candidate->GetFormID(), a.candidate->GetUniqueID()) < 0) {
                cands.push_back(&*a.candidate);  // override / remembered / wildcard below the cut
            }
        }

        // ---- record ----------------------------------------------------------
        std::string out;
        out.reserve(512 + cands.size() * 384);
        Json::Object o(out);
        o.Str("t", "imp");
        o.UInt("id", impId);
        o.Int("rt", RealMsSinceSession());
        if (const auto gt = GameSecondsSinceLoad()) o.Num("gt", *gt); else o.Null("gt");
        o.UInt("page", in.pageIndex);
        o.UInt("npages", in.pageCount);
        o.Str("reason", ContextReasonName(in.reason));
        o.Bool("ovr", in.overrideTookSlot);
        o.Key("phi");
        AppendFloatArray(out, in.phi);

        o.Key("cands");
        out += '[';
        for (size_t i = 0; i < cands.size(); ++i) {
            const auto& sc = *cands[i];
            const auto& bd = sc.breakdown;
            if (i) out += ',';
            Json::Object c(out);
            c.Str("k", ItemKeyFor(sc.GetFormID()));
            if (const auto uid = sc.GetUniqueID(); uid != 0) c.UInt("uid", uid);
            c.Str("src", Candidate::SourceTypeToString(sc.GetSourceType()));
            c.Num("u", sc.utility);
            c.Num("ctx", bd.contextWeight);
            c.Num("prior", bd.prior);
            c.Num("est", bd.rewardEstimate);
            c.Num("ucb", bd.ucb);
            c.Num("conf", bd.confidence);
            c.Num("learn", bd.learningScore);
            c.Num("lam", bd.lambda);
            c.Num("rec", bd.recencyBoost);
            c.Num("corr", bd.correlationBonus);
            c.Num("potion", bd.potionMultiplier);
            c.Num("fav", bd.favoritesMultiplier);
            c.Bool("wc", sc.isWildcard);
            c.Bool("cold", sc.isColdStartBoosted);
            c.Key("p");
            AppendCandidateProps(out, sc.candidate);
            AppendFitFields(out, bd);            // TODO(integration: B-fit)
            AppendCapFields(out, sc.candidate);  // TODO(integration: C-cap)
            c.Close();
        }
        out += ']';

        const auto locks = Slot::SlotLocker::GetSingleton().GetLockSnapshot();
        o.Key("slots");
        out += '[';
        for (size_t i = 0; i < in.assignments.size(); ++i) {
            const auto& a = in.assignments[i];
            if (i) out += ',';
            Json::Object s(out);
            s.UInt("i", a.slotIndex);
            s.Str("cls", Slot::SlotClassificationToString(a.classification));
            s.Str("as", Slot::AssignmentTypeToString(a.type));
            const int idx = (a.IsEmpty() || !a.candidate.has_value())
                ? -1
                : indexOf(a.candidate->GetFormID(), a.candidate->GetUniqueID());
            s.Int("c", idx);
            s.Bool("lk", a.slotIndex < locks.size() && locks[a.slotIndex].isLocked);
            s.Close();
        }
        out += ']';
        o.Close();

        Enqueue(std::move(out));
    }

    void DecisionLog::RecordReward(const Learning::EquipEvent& event, float appliedReward, bool trained)
    {
        if (!IsEnabled()) {
            return;
        }
        EnqueueReward(event.formID, Learning::EquipSourceToString(event.source),
            event.rewardMultiplier, event.wasRecommended,
            trained ? appliedReward : 0.0f, trained, event.features);
    }

    void DecisionLog::RecordMisclick(RE::FormID penalized, const Learning::StateFeatures& features, float penalty)
    {
        if (!IsEnabled()) {
            return;
        }
        EnqueueReward(penalized, "Misclick"sv, 1.0f, false, penalty, true, features);
    }

    void DecisionLog::EnqueueReward(RE::FormID formID, std::string_view source, float multiplier,
                                    bool wasRecommended, float reward, bool trained,
                                    const Learning::StateFeatures& features)
    {
        std::uint64_t impId = 0;
        std::optional<std::pair<size_t, size_t>> shown;  // (page, slot)
        {
            std::scoped_lock lk(m_producerMutex);
            impId = m_lastImpId;
            for (const auto& s : m_lastSignature) {
                if (s.formID == formID && s.formID != 0) {
                    shown.emplace(m_lastPage, s.slot);
                    break;
                }
            }
            // Log what the display looks like AFTER the player acted, even if
            // it did not change -- the pair (imp before, imp after) is what a
            // replay needs around every reward.
            m_forceNextImpression = true;
        }

        std::string out;
        out.reserve(384);
        Json::Object o(out);
        o.Str("t", "rew");
        o.Int("rt", RealMsSinceSession());
        if (const auto gt = GameSecondsSinceLoad()) o.Num("gt", *gt); else o.Null("gt");
        if (impId != 0) o.UInt("imp", impId); else o.Null("imp");
        o.Str("k", ItemKeyFor(formID));
        o.Str("src", source);
        o.Num("mult", multiplier);
        o.Bool("recd", wasRecommended);
        o.Num("r", reward);
        o.Bool("trained", trained);
        o.Key("phi");
        AppendFloatArray(out, features.ToArray());
        o.Key("shown");
        if (shown) {
            Json::Object s(out);
            s.UInt("page", shown->first);
            s.UInt("slot", shown->second);
            s.Close();
        } else {
            out += "null";
        }
        o.Close();

        Enqueue(std::move(out));
    }

}  // namespace Huginn::Telemetry
