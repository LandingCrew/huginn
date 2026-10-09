#include "SelectionLogV3.h"

#include "Globals.h"
#include "PipelineStateCache.h"
#include "PlayerInputGate.h"
#include "SelectionTracker.h"
#include "TestHarness.h"
#include "UtilityScorer.h"
#include "core/CrossFeatures.h"
#include "core/NeedEpisodes.h"
#include "effect/CrossFeatures.h"
#include "effect/EffectCatalog.h"
#include "needs/NeedMonitor.h"
#include "state/StateManager.h"
#include "util/InventoryUtil.h"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Huginn::Learning::SelectionLogV3
{
    namespace
    {
        using namespace Core::DecisionLog;
        using Clock = std::chrono::steady_clock;
        namespace DL = Core::DecisionLog;

        constexpr auto kFileName = "Huginn_Selections_v3.jsonl";
        constexpr auto kTestFileName = "Huginn_Selections_v3_test.jsonl";   // test mode: never the player's data
        constexpr std::uintmax_t kRotateBytes = 64ull * 1024 * 1024;
        constexpr size_t kMaxQueued = 1024;
        constexpr double kHeldCacheSec = 1.0;
        constexpr size_t kInstanceCacheMax = 512;

        double Sec(Clock::time_point t)
        {
            return std::chrono::duration<double>(t.time_since_epoch()).count();
        }

        std::string NowUtc()
        {
            return std::format("{:%F %T}", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
        }

        std::string FormName(RE::FormID formID)
        {
            const auto* form = RE::TESForm::LookupByID(formID);
            const char* name = form ? form->GetName() : nullptr;
            return (name && *name) ? name : "?";
        }

        void SetCross(Row& r, const Effect::RuntimeFeatures& f)
        {
            r.cross[static_cast<size_t>(Cross::overshoot_health)] = f.overshootHealth;
            r.cross[static_cast<size_t>(Cross::overshoot_magicka)] = f.overshootMagicka;
            r.cross[static_cast<size_t>(Cross::overshoot_stamina)] = f.overshootStamina;
            r.cross[static_cast<size_t>(Cross::weapon_charge)] = f.weaponCharge;
            r.cross[static_cast<size_t>(Cross::stack_count)] = f.stackCount;
            r.cross[static_cast<size_t>(Cross::ammo_matches_launcher)] = f.ammoMatchesLauncher;
            r.cross[static_cast<size_t>(Cross::school_fortified)] = f.schoolFortified;
        }

        // =====================================================================
        // HELD ITEMS -- the inventory and known spells the catalog describes
        // =====================================================================
        struct HeldItem
        {
            RE::FormID form = 0;
            uint16_t uid = 0;
            int32_t count = 0;
            bool equipped = false;
            float chargeFraction = 0.0f;   // enchanted weapons: current / max
            bool hasCharge = false;
            const Effect::CatalogEntry* entry = nullptr;
            std::shared_ptr<const Effect::CatalogEntry> instance;   // tempered / player-enchanted
        };

        std::mutex g_heldMutex;
        std::vector<HeldItem> g_held;
        double g_heldAt = -1e300;
        std::unordered_map<std::string, std::shared_ptr<const Effect::CatalogEntry>> g_instances;

        std::shared_ptr<const Effect::CatalogEntry> InstanceFor(const Effect::EffectCatalog& catalog,
                                                                RE::TESBoundObject* obj, RE::ExtraDataList* xl)
        {
            // Only a stack the base form does not describe: tempered (weapons)
            // or carrying a player enchantment. InstanceEntry runs the mapper, so
            // the result is cached by what makes the stack differ.
            float temper = 1.0f;
            if (obj->Is(RE::FormType::Weapon)) {
                if (const auto* h = xl->GetByType<RE::ExtraHealth>(); h && h->health > 0.0f) temper = h->health;
            }
            const auto* xe = xl->GetByType<RE::ExtraEnchantment>();
            const RE::FormID ench = xe && xe->enchantment ? xe->enchantment->GetFormID() : 0;
            if (std::fabs(temper - 1.0f) < 1e-4f && ench == 0) return nullptr;
            const std::string key = std::format("{:08X}|{:.4f}|{:08X}", obj->GetFormID(), temper, ench);
            if (const auto it = g_instances.find(key); it != g_instances.end()) return it->second;
            std::shared_ptr<const Effect::CatalogEntry> made;
            if (auto e = catalog.InstanceEntry(obj, xl)) made = std::make_shared<const Effect::CatalogEntry>(std::move(*e));
            if (g_instances.size() >= kInstanceCacheMax) g_instances.clear();
            g_instances.emplace(key, made);
            return made;
        }

        void ReadCharge(RE::TESBoundObject* obj, const RE::ExtraDataList* xl, HeldItem& h)
        {
            float maxCharge = 0.0f;
            if (const auto* ench = obj->As<RE::TESEnchantableForm>(); ench && ench->amountofEnchantment > 0) {
                maxCharge = static_cast<float>(ench->amountofEnchantment);
            }
            const auto* xe = xl ? xl->GetByType<RE::ExtraEnchantment>() : nullptr;
            if (xe && xe->charge > 0) maxCharge = static_cast<float>(xe->charge);
            if (!(maxCharge > 0.0f)) return;
            h.hasCharge = true;
            h.chargeFraction = 1.0f;   // no ExtraCharge: full
            if (const auto* xc = xl ? xl->GetByType<RE::ExtraCharge>() : nullptr) {
                h.chargeFraction = std::clamp(xc->charge / maxCharge, 0.0f, 1.0f);
            }
        }

        std::vector<HeldItem> ReadHeld()
        {
            std::vector<HeldItem> out;
            auto& catalog = Effect::EffectCatalog::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !catalog.Ready()) return out;

            auto inventory = Util::GetInventorySafe(player, [&catalog](RE::TESBoundObject& o) {
                return catalog.Find(o.GetFormID()) != nullptr;
            });
            const auto* ammo = player->GetCurrentAmmo();
            for (auto& [obj, data] : inventory) {
                auto& [count, entryData] = data;
                if (!obj || count <= 0) continue;
                const auto* entry = catalog.Find(obj->GetFormID());
                if (!entry) continue;
                const bool perStack = obj->Is(RE::FormType::Weapon) || obj->Is(RE::FormType::Armor) ||
                                      obj->Is(RE::FormType::Light);
                int32_t remaining = count;
                HeldItem plain{ .form = obj->GetFormID(), .entry = entry };
                if (entryData && entryData->extraLists) {
                    for (auto* xl : *entryData->extraLists) {
                        if (!xl) continue;
                        const bool worn = xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>();
                        if (!perStack) {
                            plain.equipped = plain.equipped || worn;
                            continue;
                        }
                        const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                        auto instance = InstanceFor(catalog, obj, xl);
                        HeldItem h{ .form = obj->GetFormID(), .uid = xu ? xu->uniqueID : uint16_t{ 0 },
                                    .count = std::max(1, static_cast<int32_t>(xl->GetCount())), .equipped = worn,
                                    .entry = entry, .instance = std::move(instance) };
                        ReadCharge(obj, xl, h);
                        if (h.uid == 0 && !h.equipped && !h.instance && !(h.hasCharge && h.chargeFraction < 1.0f)) {
                            continue;   // a list that does not set the stack apart: part of the plain row
                        }
                        remaining -= h.count;
                        out.push_back(std::move(h));
                    }
                }
                if (remaining > 0) {
                    plain.count = remaining;
                    plain.equipped = plain.equipped || obj == ammo;
                    if (obj->Is(RE::FormType::Weapon)) ReadCharge(obj, nullptr, plain);
                    out.push_back(plain);
                }
            }

            // Known spells (base and added), in hand = equipped.
            std::unordered_set<RE::FormID> seen;
            const auto* left = player->GetEquippedObject(true);
            const auto* right = player->GetEquippedObject(false);
            auto addSpell = [&](RE::SpellItem* spell) {
                if (!spell || !seen.insert(spell->GetFormID()).second) return;
                const auto* entry = catalog.Find(spell->GetFormID());
                if (!entry) return;
                out.push_back(HeldItem{ .form = spell->GetFormID(), .equipped = spell == left || spell == right,
                                        .entry = entry });
            };
            if (auto* base = player->GetActorBase()) {
                if (auto* list = base->GetSpellList(); list && list->spells) {
                    for (uint32_t i = 0; i < list->numSpells; ++i) addSpell(list->spells[i]);
                }
            }
            for (auto* spell : player->GetActorRuntimeData().addedSpells) addSpell(spell);
            return out;
        }

        std::vector<HeldItem> Held(double nowSec)
        {
            std::lock_guard lock(g_heldMutex);
            if (nowSec - g_heldAt >= kHeldCacheSec || nowSec < g_heldAt) {
                g_held = ReadHeld();
                g_heldAt = nowSec;
            }
            return g_held;
        }

        // =====================================================================
        // PER TICK -- the eligible rows with their cross-features, the needs
        // =====================================================================
        struct TickState
        {
            uint64_t cacheGeneration = UINT64_MAX;
            std::vector<PipelineStateCache::EligibleRow> eligible;
            std::vector<Row> rows;
            Core::Needs::NeedArray need{};
            Core::Needs::NeedArray input{};
        };

        std::mutex g_tickMutex;
        TickState g_tick;

        Row EligibleToRow(const PipelineStateCache::EligibleRow& e, const State::PlayerActorState& player)
        {
            Row r;
            r.form = e.formID;
            r.uid = e.uniqueID;
            r.src = static_cast<uint8_t>(e.sourceType);
            r.flags = Flag::Eligible | (std::isfinite(e.utility) ? Flag::Scored : 0);
            r.util = e.utility;
            r.wildcardP = e.wildcardPropensity;
            if (const auto* entry = Effect::EffectCatalog::GetSingleton().Find(e.formID)) {
                r.kind = static_cast<uint8_t>(entry->kind);
                r.cap = &entry->cap;
                Effect::StackInfo stack;
                stack.count = e.count;
                if (e.enchanted) {
                    stack.charge = e.chargeFraction;
                    stack.maxCharge = 1.0f;
                }
                SetCross(r, Effect::ComputeRuntimeFeatures(*entry, player, stack));
            }
            return r;
        }

        // =====================================================================
        // EPISODES
        // =====================================================================
        std::mutex g_episodeMutex;
        Core::Needs::EpisodeTracker<std::shared_ptr<const DL::Context>> g_episodes;

        std::atomic<uint64_t> g_nextContext{ 0 };
        std::atomic<uint64_t> g_nextSeq{ 0 };

        // =====================================================================
        // THE MENU -- the context taken when a selection menu opened
        // =====================================================================
        struct MenuVisit
        {
            std::shared_ptr<const DL::Context> ctx;
            std::string name;
            bool open = false;
            Clock::time_point openedAt{};
            Clock::time_point closedAt{};
            uint64_t ticks = 0;            // update ticks while it was open
            uint64_t pausedTicks = 0;      // ... of which with the game paused
            uint64_t startGeneration = 0;  // pipeline runs while open = generation delta
        };
        std::mutex g_menuMutex;
        MenuVisit g_menu;

        bool IsMenuVia(std::string_view via)
        {
            return via.starts_with("inventory menu") || via.starts_with("favorites menu") ||
                   via.starts_with("magic menu") || via.starts_with("menu (just closed)");
        }

        // =====================================================================
        // STATS
        // =====================================================================
        std::mutex g_statsMutex;
        Stats g_stats;
        double g_tickSumUs = 0.0;

        // =====================================================================
        // CONTEXTS
        // =====================================================================
        std::shared_ptr<const DL::Context> BuildContext(const char* why, std::string menu)
        {
            const auto now = Clock::now();
            auto ctx = std::make_shared<DL::Context>();
            ctx->id = ++g_nextContext;
            ctx->utc = NowUtc();
            ctx->tSec = Sec(now);
            ctx->why = why;
            ctx->menu = std::move(menu);
            {
                std::lock_guard lock(g_tickMutex);
                ctx->need = g_tick.need;
                ctx->input = g_tick.input;
                ctx->rows = g_tick.rows;
            }

            const auto page = PipelineStateCache::GetSingleton().TakeShown();
            ctx->pipeValid = page.valid;
            ctx->page = page.valid ? static_cast<int>(page.page) : -1;
            ctx->pageSlots = static_cast<int>(page.pageSlots);
            ctx->pipeAgeMs = page.ageMs;

            const auto player = State::StateManager::GetSingleton().GetPlayerState();
            auto& catalog = Effect::EffectCatalog::GetSingleton();

            // Held items: flag the eligible rows, add the rest.
            std::unordered_map<uint64_t, size_t> index;
            index.reserve(ctx->rows.size() * 2);
            auto key = [](RE::FormID f, uint16_t uid) { return (static_cast<uint64_t>(uid) << 32) | f; };
            for (size_t i = 0; i < ctx->rows.size(); ++i) index.try_emplace(key(ctx->rows[i].form, ctx->rows[i].uid), i);
            for (const auto& h : Held(ctx->tSec)) {
                const Effect::CatalogEntry& use = h.instance ? *h.instance : *h.entry;
                Effect::StackInfo stack;
                stack.count = h.count;
                if (h.hasCharge) {
                    stack.charge = h.chargeFraction;
                    stack.maxCharge = 1.0f;
                }
                const auto f = Effect::ComputeRuntimeFeatures(use, player, stack);
                if (const auto it = index.find(key(h.form, h.uid)); it != index.end()) {
                    auto& r = ctx->rows[it->second];
                    r.flags |= Flag::Held | (h.equipped ? Flag::Equipped : 0);
                    if (h.instance) {
                        r.cap = &h.instance->cap;
                        r.capOwner = std::shared_ptr<const Core::Effect::Cap>(h.instance, &h.instance->cap);
                    }
                    SetCross(r, f);   // the inventory's count, the stack's own charge and cap
                    continue;
                }
                Row r;
                r.form = h.form;
                r.uid = h.uid;
                r.kind = static_cast<uint8_t>(use.kind);
                r.flags = Flag::Held | (h.equipped ? Flag::Equipped : 0);
                r.cap = &use.cap;
                if (h.instance) r.capOwner = std::shared_ptr<const Core::Effect::Cap>(h.instance, &h.instance->cap);
                SetCross(r, f);
                index.try_emplace(key(r.form, r.uid), ctx->rows.size());
                ctx->rows.push_back(std::move(r));
            }

            // The page: mark what was shown. A propensity stays only on a row
            // shown as a wildcard.
            for (auto& r : ctx->rows) {
                if (!(r.flags & Flag::Shown)) r.wildcardP = kNone;
            }
            for (const auto& s : page.shown) {
                Row* hit = nullptr;
                for (auto& r : ctx->rows) {
                    if (r.form == s.formID && !(r.flags & Flag::Shown)) {
                        hit = &r;
                        if (r.flags & Flag::Eligible) break;
                    }
                }
                if (!hit) {
                    Row r;
                    r.form = s.formID;
                    if (const auto* entry = catalog.Find(s.formID)) {
                        r.kind = static_cast<uint8_t>(entry->kind);
                        r.cap = &entry->cap;
                    }
                    ctx->rows.push_back(std::move(r));
                    hit = &ctx->rows.back();
                }
                hit->flags |= Flag::Shown;
                hit->slot = static_cast<int8_t>(std::min<size_t>(s.slotIndex, 127));
                switch (s.type) {
                case Slot::AssignmentType::Wildcard: hit->flags |= Flag::Wildcard; break;
                case Slot::AssignmentType::Override: hit->flags |= Flag::Override; break;
                case Slot::AssignmentType::Remembered: hit->flags |= Flag::Remembered; break;
                default: break;
                }
            }
            // The eligible snapshot carried every wildcard's propensity; keep it
            // only where the page shows that wildcard.
            {
                std::lock_guard lock(g_tickMutex);
                for (auto& r : ctx->rows) {
                    if (!(r.flags & Flag::Wildcard)) continue;
                    for (const auto& e : g_tick.eligible) {
                        if (e.formID == r.form && e.uniqueID == r.uid) {
                            r.wildcardP = e.wildcardPropensity;
                            break;
                        }
                    }
                }
            }

            // The hostile target's race (perceivable: its name and look are on
            // screen; the race map keys on it).
            const auto targets = State::StateManager::GetSingleton().GetTargets();
            if (targets.primary && targets.primary->isHostile && !targets.primary->isDead) {
                if (const auto* actor = RE::TESForm::LookupByID<RE::Actor>(targets.primary->actorFormID)) {
                    if (const auto* race = actor->GetRace()) {
                        if (const char* edid = race->GetFormEditorID(); edid && *edid) ctx->race = edid;
                    }
                }
            }

            if (g_utilityScorer) {
                const auto& wc = g_utilityScorer->GetWildcardManager();
                ctx->wildcardBase = wc.GetBaseProbability();
                ctx->wildcardMax = wc.GetMaxProbability();
            }
            return ctx;
        }

        void ResolveChosen(Decision& d, bool preferShown)
        {
            const auto& rows = d.ctx->rows;
            int found = -1;
            for (size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].form != d.form) continue;
                if (found < 0) found = static_cast<int>(i);
                if (preferShown && (rows[i].flags & Flag::Shown)) {
                    found = static_cast<int>(i);
                    break;
                }
                if (!preferShown && (rows[i].flags & Flag::Eligible)) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            if (found >= 0) {
                d.row = found;
                return;
            }
            Row r;
            r.form = d.form;
            r.flags = Flag::AddedAtPick | Flag::Held;
            if (const auto* entry = Effect::EffectCatalog::GetSingleton().Find(d.form)) {
                r.kind = static_cast<uint8_t>(entry->kind);
                r.cap = &entry->cap;
            }
            d.added.push_back(std::move(r));
            d.row = static_cast<int>(rows.size());
        }

        Decision MakeDecision(Outcome outcome, uint32_t gen)
        {
            Decision d;
            d.seq = ++g_nextSeq;
            d.utc = NowUtc();
            d.launch = g_launchStamp;
            d.list = g_listName;
            d.character = g_activeCharacterID.load(std::memory_order_relaxed);
            d.gen = gen;
            d.outcome = outcome;
            return d;
        }

        // =====================================================================
        // THE WRITER -- a background thread, as the v2 log's
        // =====================================================================
        class Writer
        {
        public:
            static Writer& Get()
            {
                // Leaked on purpose: a thread joined from a static destructor
                // runs inside DLL_PROCESS_DETACH, where joining can deadlock.
                static auto* writer = new Writer();
                return *writer;
            }

            void Enqueue(Decision d)
            {
                {
                    std::lock_guard lock(m_mutex);
                    if (m_queue.size() >= kMaxQueued) {
                        if (!m_warnedFull) {
                            logger::warn("[SelectionV3] writer is {} records behind -- dropping records"sv, kMaxQueued);
                            m_warnedFull = true;
                        }
                        return;
                    }
                    m_queue.push_back(std::move(d));
                    ++m_pending;
                }
                m_cv.notify_one();
            }

            bool Flush(std::chrono::milliseconds timeout)
            {
                std::unique_lock lock(m_mutex);
                return m_idle.wait_for(lock, timeout, [this] { return m_pending == 0; });
            }

            std::filesystem::path Path()
            {
                std::lock_guard lock(m_mutex);
                return m_path;
            }

        private:
            Writer() { std::thread([this] { Run(); }).detach(); }

            void Run()
            {
                for (;;) {
                    Decision d;
                    {
                        std::unique_lock lock(m_mutex);
                        m_cv.wait(lock, [this] { return !m_queue.empty(); });
                        d = std::move(m_queue.front());
                        m_queue.pop_front();
                    }
                    Write(d);
                    {
                        std::lock_guard lock(m_mutex);
                        --m_pending;
                        if (m_pending == 0) m_idle.notify_all();
                    }
                }
            }

            bool Open()
            {
                const auto dir = SKSE::log::log_directory();
                if (!dir) return false;
                const bool test = TestHarness::Active();
                const auto path = *dir / (test ? kTestFileName : kFileName);
                // Test mode starts the test file afresh once per launch; the
                // player's file is appended across launches (each launch is a
                // segment of its own: it starts with a head line).
                const auto mode = std::ios::binary | ((test && !m_truncatedTest) ? std::ios::trunc : std::ios::app);
                m_out.open(path, mode);
                if (!m_out.is_open()) return false;
                m_truncatedTest = m_truncatedTest || test;
                std::error_code ec;
                m_fileBytes = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
                {
                    std::lock_guard lock(m_mutex);
                    m_path = path;
                }
                Head head;
                head.launch = g_launchStamp;
                head.list = g_listName;
                head.build = std::format("{} ({})", Huginn::VERSION_STRING, Huginn::GIT_COMMIT);
                for (size_t i = 0; i < Candidate::SOURCE_TYPE_COUNT; ++i) {
                    head.sourceNames.emplace_back(Candidate::SourceTypeToString(static_cast<Candidate::SourceType>(i)));
                }
                const auto& p = g_episodes.Params();
                head.onset = p.onset;
                head.expiry = p.expiry;
                head.minSec = p.minSec;
                head.graceSec = p.graceSec;
                const std::string line = m_encoder.BeginSegment(head);
                m_out << line;
                m_fileBytes += line.size();
                logger::info("[SelectionV3] writing {} ({} bytes before this launch's segment)"sv, path.string(),
                    m_fileBytes - line.size());
                return true;
            }

            void Rotate()
            {
                m_out.close();
                const auto path = Path();
                std::error_code ec;
                for (int n = 1; n < 1000; ++n) {
                    auto rotated = path;
                    rotated.replace_filename(std::format("Huginn_Selections_v3-{}-{}.jsonl", g_launchStamp, n));
                    if (std::filesystem::exists(rotated, ec)) continue;
                    std::filesystem::rename(path, rotated, ec);
                    logger::info("[SelectionV3] {} reached {} MB: kept as {}{}"sv, path.filename().string(),
                        kRotateBytes >> 20, rotated.filename().string(), ec ? " (rename failed: " + ec.message() + ")" : "");
                    break;
                }
                m_truncatedTest = false;   // a rotated test file starts afresh too
            }

            void Write(const Decision& d)
            {
                if (!m_out.is_open() && !Open()) {
                    if (!m_warnedOpen) {
                        logger::error("[SelectionV3] cannot open the v3 log for append"sv);
                        m_warnedOpen = true;
                    }
                    return;
                }
                const bool ctxNew = d.ctx && !m_ctxSeen.contains(d.ctx->id);
                const size_t capsBefore = m_encoder.CapsDefined();
                const std::string lines = m_encoder.Encode(d);
                if (d.ctx) m_ctxSeen.insert(d.ctx->id);
                m_out << lines;
                m_out.flush();
                if (!m_out) {
                    if (!m_warnedWrite) {
                        logger::error("[SelectionV3] write failed -- reopening on the next record (one record lost)"sv);
                        m_warnedWrite = true;
                    }
                    m_out.close();
                    m_out.clear();
                    m_ctxSeen.clear();
                    return;
                }
                m_fileBytes += lines.size();
                const size_t capsNew = m_encoder.CapsDefined() - capsBefore;
                {
                    std::lock_guard lock(g_statsMutex);
                    ++g_stats.written;
                    g_stats.bytes += lines.size();
                    g_stats.ctxWritten += ctxNew ? 1 : 0;
                    g_stats.capsWritten += capsNew;
                }
                logger::debug("[SelectionV3] seq={} out={} rows={} bytes={} (context {}, {} new cap(s))"sv, d.seq,
                    OutcomeName(d.outcome), d.ctx ? d.ctx->rows.size() + d.added.size() : d.added.size(), lines.size(),
                    ctxNew ? "new" : "shared", capsNew);
                if (m_fileBytes >= kRotateBytes) {
                    Rotate();
                    m_ctxSeen.clear();
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::condition_variable m_idle;
            std::deque<Decision> m_queue;
            size_t m_pending = 0;
            bool m_warnedFull = false;
            std::filesystem::path m_path;

            // Writer thread only.
            Encoder m_encoder;
            std::ofstream m_out;
            std::uintmax_t m_fileBytes = 0;
            std::unordered_set<uint64_t> m_ctxSeen;   // for the debug line (the encoder decides)
            bool m_truncatedTest = false;
            bool m_warnedOpen = false;
            bool m_warnedWrite = false;
        };

        void Queue(Decision d)
        {
            {
                std::lock_guard lock(g_statsMutex);
                switch (d.outcome) {
                case Outcome::Key: ++g_stats.key; break;
                case Outcome::Wheel: ++g_stats.wheel; break;
                case Outcome::Menu: ++g_stats.menu; break;
                case Outcome::Nothing: ++g_stats.nothing; break;
                }
                g_stats.unlearned += d.learned ? 0 : 1;
                g_stats.lastRows = (d.ctx ? d.ctx->rows.size() : 0) + d.added.size();
            }
            Writer::Get().Enqueue(std::move(d));
        }

        void AnswerEpisodes(double pressSec)
        {
            std::lock_guard lock(g_episodeMutex);
            g_episodes.OnSelection(pressSec);
        }

        // =====================================================================
        // PICKS THE FROZEN LEARNER DOES NOT TAKE (v3 only)
        // =====================================================================
        struct PendingPick
        {
            RE::FormID form = 0;
            std::string via;
            std::string skip;
            std::string caseLabel;
            bool consumable = false;
            Clock::time_point at{};
            Clock::time_point deadline{};
            DecisionCapture capture;
        };
        std::mutex g_pickMutex;
        std::vector<PendingPick> g_picks;

        void ConfirmPick(PendingPick& p, const char* how)
        {
            const auto now = Clock::now();
            Decision d = MakeDecision(Outcome::Menu, g_loadGeneration.load(std::memory_order_relaxed));
            d.form = p.form;
            d.name = FormName(p.form);
            d.src = EquipSourceToString(EquipSource::External);
            d.via = p.via;
            d.caseLabel = p.caseLabel;
            d.how = how;
            d.kind = p.consumable ? "consume" : "equip";
            d.confirmMs = std::chrono::duration<float, std::milli>(now - p.at).count();
            d.learned = false;
            d.skip = p.skip;
            d.ctx = p.capture.ctx;
            d.ctxAgeMs = p.capture.ageMs;
            d.open = p.capture.open;
            if (!d.ctx) d.ctx = BuildContext("press", {});
            ResolveChosen(d, false);
            logger::info("[SelectionV3] {:08X} '{}' via {}: kept in the v3 log only (the learner skips it: {})"sv,
                d.form, d.name, d.via, d.skip);
            AnswerEpisodes(p.capture.pressSec);
            Queue(std::move(d));
        }

        void TickPicks(Clock::time_point now)
        {
            std::vector<PendingPick> due;
            {
                std::lock_guard lock(g_pickMutex);
                for (auto it = g_picks.begin(); it != g_picks.end();) {
                    if (now >= it->deadline) {
                        due.push_back(std::move(*it));
                        it = g_picks.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
            for (auto& p : due) {
                if (!p.consumable && SelectionTracker::IsStillEquipped(p.form)) {
                    ConfirmPick(p, "still equipped");
                }
                else {
                    logger::debug("[SelectionV3] {:08X} not confirmed ({}): not recorded"sv, p.form,
                        p.consumable ? "count never dropped" : "no longer equipped");
                }
            }
        }

        // =====================================================================
        // THE MENU SINK
        // =====================================================================
        class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!e || !PlayerInputGate::IsSelectionMenu(e->menuName)) return RE::BSEventNotifyControl::kContinue;
                const auto now = Clock::now();
                const std::string name(e->menuName.c_str());
                if (e->opening) {
                    auto ctx = BuildContext("menu", name);
                    std::lock_guard lock(g_menuMutex);
                    g_menu = MenuVisit{ .ctx = std::move(ctx), .name = name, .open = true, .openedAt = now,
                                        .startGeneration = PipelineStateCache::GetSingleton().Generation() };
                    return RE::BSEventNotifyControl::kContinue;
                }
                std::lock_guard lock(g_menuMutex);
                if (!g_menu.open || g_menu.name != name) return RE::BSEventNotifyControl::kContinue;
                g_menu.open = false;
                g_menu.closedAt = now;
                // The measurement the roadmap asked for (R4): does the update
                // loop tick inside a selection menu, and does the pipeline run?
                const double ms = std::chrono::duration<double, std::milli>(now - g_menu.openedAt).count();
                const uint64_t runs = PipelineStateCache::GetSingleton().Generation() - g_menu.startGeneration;
                const auto page = PipelineStateCache::GetSingleton().TakeShown();
                logger::info("[SelectionV3] {} was open {:.0f} ms: {} update tick(s) inside ({:.1f}/s, {} with the "
                             "game paused), {} pipeline run(s); page cache {:.0f} ms old at close"sv,
                    name, ms, g_menu.ticks, ms > 0 ? 1000.0 * static_cast<double>(g_menu.ticks) / ms : 0.0,
                    g_menu.pausedTicks, runs, page.ageMs);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuSink g_menuSink;
    }

    void Register()
    {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);
            logger::info("[SelectionV3] menu sink registered"sv);
        }
    }

    void Tick(Clock::time_point now)
    {
        const auto t0 = Clock::now();
        const double nowSec = Sec(now);

        // 1. The needs and the eligible rows, with this tick's cross-features.
        const auto live = Needs::NeedMonitor::GetSingleton().Latest();
        const auto player = State::StateManager::GetSingleton().GetPlayerState();
        size_t rowCount = 0;
        {
            std::lock_guard lock(g_tickMutex);
            uint64_t generation = g_tick.cacheGeneration;
            PipelineStateCache::GetSingleton().TakeEligibleIfNewer(g_tick.cacheGeneration, g_tick.eligible, generation);
            g_tick.cacheGeneration = generation;
            if (live) {
                g_tick.need = live->vector.value;
                g_tick.input = live->vector.input;
            }
            g_tick.rows.clear();
            g_tick.rows.reserve(g_tick.eligible.size());
            for (const auto& e : g_tick.eligible) g_tick.rows.push_back(EligibleToRow(e, player));
            rowCount = g_tick.rows.size();
        }

        // 2. The menu measurement.
        auto* ui = RE::UI::GetSingleton();
        const bool paused = ui && ui->GameIsPaused();
        {
            std::lock_guard lock(g_menuMutex);
            if (g_menu.open) {
                ++g_menu.ticks;
                g_menu.pausedTicks += paused ? 1 : 0;
            }
        }

        // 3. Episodes -- not while the game is paused: the world is frozen,
        // so an onset or an expiry then is a wall-clock decay or the player's
        // own menu action, which a pick inside the still-open episode answers.
        if (live && !paused) {
            std::vector<size_t> onsets;
            std::vector<Core::Needs::EpisodeTracker<std::shared_ptr<const DL::Context>>::Episode> unanswered;
            {
                std::lock_guard lock(g_episodeMutex);
                onsets = g_episodes.Tick(live->vector.value, nowSec);
                unanswered = g_episodes.TakeUnanswered(nowSec);
            }
            if (!onsets.empty()) {
                auto ctx = BuildContext("onset", {});
                std::lock_guard lock(g_episodeMutex);
                for (const size_t k : onsets) g_episodes.SetPayload(k, ctx);
            }
            for (auto& e : unanswered) {
                if (!e.payload) continue;   // an onset whose context was never attached (not expected)
                Decision d = MakeDecision(Outcome::Nothing, g_loadGeneration.load(std::memory_order_relaxed));
                d.ctx = e.payload;
                d.ctxAgeMs = static_cast<float>((nowSec - d.ctx->tSec) * 1000.0);
                d.need = static_cast<int>(e.need);
                d.durSec = e.endSec - e.onsetSec;
                d.peak = e.peak;
                d.onsetUtc = d.ctx->utc;
                logger::debug("[SelectionV3] nothing pressed: {} episode of {:.1f}s (peak {:.2f}) ended unanswered"sv,
                    Core::Needs::kNeeds[e.need].id, d.durSec, d.peak);
                Queue(std::move(d));
            }
        }

        // 4. Picks kept in the v3 log only.
        TickPicks(now);

        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        std::lock_guard lock(g_statsMutex);
        ++g_stats.ticks;
        g_tickSumUs += us;
        g_stats.tickMeanUs = g_tickSumUs / static_cast<double>(g_stats.ticks);
        g_stats.tickMaxUs = std::max(g_stats.tickMaxUs, us);
        g_stats.tickRows = rowCount;
        {
            std::lock_guard dl(g_episodeMutex);
            g_stats.episodesDroppedShort = g_episodes.DroppedShort();
        }
    }

    DecisionCapture CaptureForPress(EquipSource source, std::string_view via)
    {
        const auto now = Clock::now();
        DecisionCapture cap;
        cap.pressSec = Sec(now);
        {
            std::lock_guard lock(g_episodeMutex);
            for (const size_t k : g_episodes.OpenNeeds()) cap.open.push_back(static_cast<uint8_t>(k));
        }
        if (source == EquipSource::External && IsMenuVia(via)) {
            std::lock_guard lock(g_menuMutex);
            const bool recent = g_menu.open ||
                std::chrono::duration<float, std::milli>(now - g_menu.closedAt).count() <= Config::MENU_CLOSE_INPUT_WINDOW_MS;
            if (g_menu.ctx && recent) {
                cap.ctx = g_menu.ctx;
                cap.ageMs = static_cast<float>((cap.pressSec - g_menu.ctx->tSec) * 1000.0);
                return cap;
            }
        }
        cap.ctx = BuildContext("press", {});
        return cap;
    }

    void OnConfirmed(const EquipEvent& event, Clock::time_point selectedAt, std::string_view how)
    {
        const Outcome outcome = event.source == EquipSource::Hotkey  ? Outcome::Key
                              : event.source == EquipSource::Wheeler ? Outcome::Wheel
                                                                     : Outcome::Menu;
        Decision d = MakeDecision(outcome, event.loadGeneration);
        d.form = event.formID;
        d.name = FormName(event.formID);
        d.src = EquipSourceToString(event.source);
        d.via = event.via;
        d.caseLabel = event.attribution;
        d.how = how;
        d.kind = SelectionKindToString(event.kind);
        d.confirmMs = event.confirmMs;
        d.repeat = event.repeatPick;
        d.learned = true;
        d.ctx = event.v3.ctx;
        d.ctxAgeMs = event.v3.ageMs;
        d.open = event.v3.open;
        if (!d.ctx) d.ctx = BuildContext("press", {});
        ResolveChosen(d, outcome != Outcome::Menu);
        AnswerEpisodes(event.v3.ctx ? event.v3.pressSec : Sec(selectedAt));
        Queue(std::move(d));
    }

    void OnUnlearnedPick(RE::FormID formID, std::string via, std::string_view skip, std::string caseLabel)
    {
        if (formID == 0 || SelectionTracker::GetSingleton().IsPending(formID)) return;
        {
            std::lock_guard lock(g_pickMutex);
            for (const auto& p : g_picks) {
                if (p.form == formID) return;   // the same pick (both hands, a doubled event)
            }
        }
        PendingPick p;
        p.form = formID;
        p.skip = std::string(skip);
        p.caseLabel = std::move(caseLabel);
        p.at = Clock::now();
        const auto* form = RE::TESForm::LookupByID(formID);
        p.consumable = form && (form->Is(RE::FormType::AlchemyItem) || form->Is(RE::FormType::SoulGem));
        const float windowMs = p.consumable ? Config::CONSUMPTION_HUGINN_WINDOW_MS : Config::SELECTION_CONFIRM_MS;
        p.deadline = p.at + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float, std::milli>(windowMs));
        p.capture = CaptureForPress(EquipSource::External, via);
        p.via = std::move(via);
        logger::debug("[SelectionV3] pending (v3 only, {}) {:08X} '{}' via {}"sv, p.skip, formID, FormName(formID), p.via);
        std::lock_guard lock(g_pickMutex);
        for (const auto& q : g_picks) {
            if (q.form == formID) return;
        }
        g_picks.push_back(std::move(p));
    }

    bool OnConsumed(RE::FormID formID)
    {
        std::optional<PendingPick> hit;
        {
            std::lock_guard lock(g_pickMutex);
            for (auto it = g_picks.begin(); it != g_picks.end(); ++it) {
                if (it->form == formID && it->consumable) {
                    hit = std::move(*it);
                    g_picks.erase(it);
                    break;
                }
            }
        }
        if (!hit) return false;
        ConfirmPick(*hit, "consumed");
        return true;
    }

    void OnTrackerSelect(RE::FormID formID)
    {
        std::lock_guard lock(g_pickMutex);
        std::erase_if(g_picks, [formID](const PendingPick& p) { return p.form == formID; });
    }

    void Reset()
    {
        {
            std::lock_guard lock(g_episodeMutex);
            g_episodes.Reset();
        }
        {
            std::lock_guard lock(g_pickMutex);
            g_picks.clear();
        }
        {
            std::lock_guard lock(g_menuMutex);
            g_menu = MenuVisit{};
        }
        {
            std::lock_guard lock(g_heldMutex);
            g_held.clear();
            g_heldAt = -1e300;
        }
        {
            std::lock_guard lock(g_tickMutex);
            g_tick = TickState{};
        }
    }

    Stats GetStats()
    {
        std::lock_guard lock(g_statsMutex);
        return g_stats;
    }

    std::filesystem::path FilePath()
    {
        return Writer::Get().Path();
    }

    bool Flush(std::chrono::milliseconds timeout)
    {
        return Writer::Get().Flush(timeout);
    }
}
