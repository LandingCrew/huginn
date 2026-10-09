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
#include "util/ExtraListStability.h"
#include "util/InventoryUtil.h"

#include <array>
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
        // preEquipped: the equipped state from the newest tick at least this
        // long before the press (a Huginn key's hand swap can wait a frame or
        // two before its callback; an outside equip's event follows at once).
        constexpr double kPreEquippedLeadSec = 0.25;

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

        struct HeldSet
        {
            std::vector<HeldItem> items;
            bool full = false;   // extra data read (Util::IsExtraListStable at the read)
        };

        // What makes an instance differ from its base form.
        struct InstanceKey
        {
            RE::FormID form = 0;
            int32_t temperMilli = 1000;
            RE::FormID enchantment = 0;
            bool operator==(const InstanceKey&) const = default;
        };
        struct InstanceKeyHash
        {
            size_t operator()(const InstanceKey& k) const noexcept
            {
                return std::hash<uint64_t>{}((static_cast<uint64_t>(k.form) << 32) ^ k.enchantment ^
                                             (static_cast<uint64_t>(static_cast<uint32_t>(k.temperMilli)) << 13));
            }
        };

        std::mutex g_heldMutex;
        std::shared_ptr<const HeldSet> g_held;
        double g_heldAt = -1e300;
        // Per-instance caps by what sets the stack apart. Cleared on every load
        // (Reset): a player enchantment is a dynamic FF form, and its ID means
        // another enchantment in another save.
        std::unordered_map<InstanceKey, std::shared_ptr<const Effect::CatalogEntry>, InstanceKeyHash> g_instances;

        // Call only when Util::IsExtraListStable(): reads the list's extra data.
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
            const InstanceKey key{ obj->GetFormID(), static_cast<int32_t>(std::lround(temper * 1000.0f)), ench };
            if (const auto it = g_instances.find(key); it != g_instances.end()) return it->second;
            std::shared_ptr<const Effect::CatalogEntry> made;
            if (auto e = catalog.InstanceEntry(obj, xl)) made = std::make_shared<const Effect::CatalogEntry>(std::move(*e));
            if (g_instances.size() >= kInstanceCacheMax) g_instances.clear();
            g_instances.emplace(key, made);
            return made;
        }

        // `xl` may be null (the base form's charge only); a non-null list only
        // when Util::IsExtraListStable().
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

        // Inside the post-load window (Util::IsExtraListStable false) the
        // engine is still finalizing inventory extra data and reading it can
        // crash (the registries gate the same way). Then no extra list is
        // touched: one plain row per base form, "equipped" from the hands and
        // the nocked ammo only (armour reads unworn), no unique IDs, no
        // per-instance caps or stack charges -- and the context says so
        // (heldFull = 0). The next read after the window is a full one.
        HeldSet ReadHeld(bool stable)
        {
            HeldSet out;
            out.full = stable;
            auto& catalog = Effect::EffectCatalog::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !catalog.Ready()) return out;

            auto inventory = Util::GetInventorySafe(player, [&catalog](RE::TESBoundObject& o) {
                return catalog.Find(o.GetFormID()) != nullptr;
            });
            const auto* ammo = player->GetCurrentAmmo();
            const auto* left = player->GetEquippedObject(true);
            const auto* right = player->GetEquippedObject(false);
            for (auto& [obj, data] : inventory) {
                auto& [count, entryData] = data;
                if (!obj || count <= 0) continue;
                const auto* entry = catalog.Find(obj->GetFormID());
                if (!entry) continue;
                const bool perStack = obj->Is(RE::FormType::Weapon) || obj->Is(RE::FormType::Armor) ||
                                      obj->Is(RE::FormType::Light);
                int32_t remaining = count;
                HeldItem plain{ .form = obj->GetFormID(), .entry = entry };
                if (stable && entryData && entryData->extraLists) {
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
                        out.items.push_back(std::move(h));
                    }
                }
                if (remaining > 0) {
                    plain.count = remaining;
                    plain.equipped = plain.equipped || obj == ammo || (!stable && (obj == left || obj == right));
                    if (obj->Is(RE::FormType::Weapon)) ReadCharge(obj, nullptr, plain);
                    out.items.push_back(plain);
                }
            }

            // Known spells (base and added), in hand = equipped.
            std::unordered_set<RE::FormID> seen;
            auto addSpell = [&](RE::SpellItem* spell) {
                if (!spell || !seen.insert(spell->GetFormID()).second) return;
                const auto* entry = catalog.Find(spell->GetFormID());
                if (!entry) return;
                out.items.push_back(HeldItem{ .form = spell->GetFormID(), .equipped = spell == left || spell == right,
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

        /// The held set, read at most once a second; a basic read (inside the
        /// post-load window) is redone as soon as a full one is possible.
        /// `readMs` is the time spent reading (0 for a cache hit).
        std::shared_ptr<const HeldSet> Held(double nowSec, double& readMs)
        {
            readMs = 0.0;
            const bool stable = Util::IsExtraListStable();
            std::lock_guard lock(g_heldMutex);
            const bool upgrade = g_held && !g_held->full && stable;
            if (!g_held || upgrade || nowSec - g_heldAt >= kHeldCacheSec || nowSec < g_heldAt) {
                const auto t0 = Clock::now();
                g_held = std::make_shared<const HeldSet>(ReadHeld(stable));
                g_heldAt = nowSec;
                readMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
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
            // The catalog entry of each eligible row, looked up once per run
            // (nullptr: not in the catalog); redone once the catalog is ready.
            std::vector<const Effect::CatalogEntry*> entries;
            bool entriesFromReadyCatalog = false;
            std::vector<Row> rows;
            Core::Needs::NeedArray need{};
            Core::Needs::NeedArray input{};
        };

        std::mutex g_tickMutex;
        TickState g_tick;
        // A load arms the gate at the cache's generation: until the new
        // session's first pipeline run, the cache still holds the previous
        // save's candidates and page. Guarded by g_tickMutex.
        RunGate g_runGate;

        Row EligibleToRow(const PipelineStateCache::EligibleRow& e, const Effect::CatalogEntry* entry,
                          const State::PlayerActorState& player)
        {
            Row r;
            r.form = e.formID;
            r.uid = e.uniqueID;
            r.src = static_cast<uint8_t>(e.sourceType);
            r.flags = Flag::Eligible | (std::isfinite(e.utility) ? Flag::Scored : 0);
            r.util = e.utility;
            r.wildcardP = e.wildcardPropensity;
            if (entry) {
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

        // The hands and the nocked ammo of the last few ticks, for preEquipped.
        struct HandSnapshot
        {
            double tSec = -1e300;
            RE::FormID left = 0, right = 0, ammo = 0;
        };
        std::mutex g_handMutex;
        std::array<HandSnapshot, 8> g_hands{};
        size_t g_handNext = 0;

        void RecordHands(double nowSec)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            HandSnapshot h;
            h.tSec = nowSec;
            if (const auto* o = player->GetEquippedObject(true)) h.left = o->GetFormID();
            if (const auto* o = player->GetEquippedObject(false)) h.right = o->GetFormID();
            if (const auto* a = player->GetCurrentAmmo()) h.ammo = a->GetFormID();
            std::lock_guard lock(g_handMutex);
            g_hands[g_handNext] = h;
            g_handNext = (g_handNext + 1) % g_hands.size();
        }

        bool WasEquippedBefore(RE::FormID form, double pressSec)
        {
            std::lock_guard lock(g_handMutex);
            const HandSnapshot* best = nullptr;
            for (const auto& h : g_hands) {
                if (h.tSec <= pressSec - kPreEquippedLeadSec && (!best || h.tSec > best->tSec)) best = &h;
            }
            return best && form != 0 && (best->left == form || best->right == form || best->ammo == form);
        }

        // =====================================================================
        // EPISODES
        // =====================================================================
        std::mutex g_episodeMutex;
        Core::Needs::EpisodeTracker<std::shared_ptr<const DL::Context>> g_episodes;

        std::atomic<uint64_t> g_nextContext{ 0 };

        // =====================================================================
        // THE MENU -- the context taken when a pausing selection menu opened
        // =====================================================================
        struct MenuVisit
        {
            std::shared_ptr<const DL::Context> ctx;   // null for a menu that joins at the press (Favorites)
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

        /// A pick from one of the menus that join the menu-open context: the
        /// inventory and magic menus, which pause the game and hide the page.
        /// The favourites menu does not pause and the widget stays visible, so
        /// its picks join the context at the press (the defaults the
        /// coordinator applied, 2026-10-09).
        bool IsPausingMenuVia(std::string_view via)
        {
            return via.starts_with("inventory menu") || via.starts_with("magic menu") ||
                   via.starts_with("menu (just closed)");
        }

        bool JoinsAtOpen(const RE::BSFixedString& menuName)
        {
            return menuName == RE::InventoryMenu::MENU_NAME || menuName == RE::MagicMenu::MENU_NAME;
        }

        // =====================================================================
        // STATS
        // =====================================================================
        std::mutex g_statsMutex;
        Stats g_stats;
        double g_tickSumUs = 0.0;
        double g_buildSumMs = 0.0;

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
            std::vector<PipelineStateCache::EligibleRow> wildcardRows;   // eligible rows with a propensity
            RunGate gate;
            {
                std::lock_guard lock(g_tickMutex);
                ctx->need = g_tick.need;
                ctx->input = g_tick.input;
                ctx->rows = g_tick.rows;
                for (const auto& e : g_tick.eligible) {
                    if (std::isfinite(e.wildcardPropensity)) wildcardRows.push_back(e);
                }
                gate = g_runGate;
            }

            auto page = PipelineStateCache::GetSingleton().TakeShown();
            // A page from before this session's first run is the previous
            // save's: not logged (pipe.ok = 0, no shown rows).
            if (!gate.IsCurrent(page.generation)) {
                page.valid = false;
                page.shown.clear();
            }
            ctx->pipeValid = page.valid;
            ctx->page = page.valid ? static_cast<int>(page.page) : -1;
            ctx->pageSlots = page.valid ? static_cast<int>(page.pageSlots) : 0;
            ctx->pipeAgeMs = page.valid ? page.ageMs : 0.0f;

            const auto player = State::StateManager::GetSingleton().GetPlayerState();
            auto& catalog = Effect::EffectCatalog::GetSingleton();

            // Held items: flag the eligible rows, add the rest.
            double heldMs = 0.0;
            const auto held = Held(ctx->tSec, heldMs);
            ctx->heldFull = held->full;
            std::unordered_map<uint64_t, size_t> index;
            index.reserve((ctx->rows.size() + held->items.size()) * 2);
            auto key = [](RE::FormID f, uint16_t uid) { return (static_cast<uint64_t>(uid) << 32) | f; };
            for (size_t i = 0; i < ctx->rows.size(); ++i) index.try_emplace(key(ctx->rows[i].form, ctx->rows[i].uid), i);
            ctx->rows.reserve(ctx->rows.size() + held->items.size());
            for (const auto& h : held->items) {
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
            // shown as a wildcard. A shown stack is matched by FormID and
            // unique ID first, then by FormID alone (an assignment without one).
            for (auto& r : ctx->rows) r.wildcardP = kNone;
            for (const auto& s : page.shown) {
                Row* hit = nullptr;
                if (const auto it = index.find(key(s.formID, s.uniqueID)); it != index.end() &&
                                                                         !(ctx->rows[it->second].flags & Flag::Shown)) {
                    hit = &ctx->rows[it->second];
                }
                for (auto& r : ctx->rows) {
                    if (hit) break;
                    if (r.form == s.formID && !(r.flags & Flag::Shown)) {
                        hit = &r;
                        if (r.flags & Flag::Eligible) break;
                    }
                }
                if (!hit) {
                    Row r;
                    r.form = s.formID;
                    r.uid = s.uniqueID;
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
            for (auto& r : ctx->rows) {
                if (!(r.flags & Flag::Wildcard)) continue;
                for (const auto& e : wildcardRows) {
                    if (e.formID == r.form && e.uniqueID == r.uid) {
                        r.wildcardP = e.wildcardPropensity;
                        break;
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

            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
            {
                std::lock_guard lock(g_statsMutex);
                ++g_stats.contextsBuilt;
                g_buildSumMs += ms;
                g_stats.buildMeanMs = g_buildSumMs / static_cast<double>(g_stats.contextsBuilt);
                g_stats.buildMaxMs = std::max(g_stats.buildMaxMs, ms);
                g_stats.heldReadMaxMs = std::max(g_stats.heldReadMaxMs, heldMs);
                g_stats.heldReads += heldMs > 0.0 ? 1 : 0;
            }
            logger::debug("[SelectionV3] context {} ({}): {} rows in {:.2f} ms (held {}{:.2f} ms{})"sv, ctx->id, why,
                ctx->rows.size(), ms, heldMs > 0.0 ? "read " : "cached ", heldMs, held->full ? "" : ", basic");
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
            Decision d;   // seq: the writer's, in write order
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
                        ++m_droppedSinceWarn;
                        {
                            std::lock_guard s(g_statsMutex);
                            ++g_stats.dropped;
                        }
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
                        if (m_queue.empty()) {
                            // Drained: say what was lost, and warn again next time.
                            if (m_droppedSinceWarn > 0) {
                                logger::warn("[SelectionV3] writer caught up; {} record(s) were dropped"sv,
                                    m_droppedSinceWarn);
                                m_droppedSinceWarn = 0;
                            }
                            m_warnedFull = false;
                        }
                        if (m_pending == 0) m_idle.notify_all();
                    }
                }
            }

            /// True when the file exists, is not empty and does not end in a
            /// newline (a record torn by a crash or a full disk).
            static bool EndsTorn(const std::filesystem::path& path)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in) return false;
                in.seekg(0, std::ios::end);
                if (in.tellg() <= 0) return false;
                in.seekg(-1, std::ios::end);
                char last = 0;
                in.get(last);
                return last != '\n';
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
                const bool truncate = test && !m_truncatedTest;
                const bool torn = !truncate && EndsTorn(path);
                const auto mode = std::ios::binary | (truncate ? std::ios::trunc : std::ios::app);
                m_out.open(path, mode);
                if (!m_out.is_open()) return false;
                m_truncatedTest = m_truncatedTest || test;
                std::error_code ec;
                m_fileBytes = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
                {
                    std::lock_guard lock(m_mutex);
                    m_path = path;
                }
                if (torn) {
                    // Start on a line of our own; the reader skips the torn one.
                    m_out << '\n';
                    ++m_fileBytes;
                    logger::warn("[SelectionV3] {} ended in a torn record; starting on a new line"sv,
                        path.filename().string());
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
                head.answerSlackSec = p.answerSlackSec;
                const std::string line = m_encoder.BeginSegment(head);
                m_ctxSeen.clear();
                m_out << line;
                m_fileBytes += line.size();
                logger::info("[SelectionV3] writing {} ({} bytes before this launch's segment)"sv, path.string(),
                    m_fileBytes - line.size());
                return true;
            }

            /// Rename the full file aside; the next record opens a new one. A
            /// failed rename (the file held open elsewhere) keeps appending to
            /// the current file and does not try again this launch -- rather
            /// than reopening, re-heading and re-defining every cap and context
            /// on every following record.
            void Rotate()
            {
                const auto path = Path();
                std::error_code ec;
                for (int n = 1; n < 1000; ++n) {
                    auto rotated = path;
                    rotated.replace_filename(std::format("Huginn_Selections_v3-{}-{}.jsonl", g_launchStamp, n));
                    if (std::filesystem::exists(rotated, ec)) continue;
                    m_out.close();
                    std::filesystem::rename(path, rotated, ec);
                    if (ec) {
                        logger::warn("[SelectionV3] {} reached {} MB but could not be renamed ({}): appending to it "
                                     "for the rest of this launch"sv,
                            path.filename().string(), kRotateBytes >> 20, ec.message());
                        m_rotationFailed = true;
                        m_out.open(path, std::ios::binary | std::ios::app);   // same segment: tables stay valid
                        return;
                    }
                    logger::info("[SelectionV3] {} reached {} MB: kept as {}"sv, path.filename().string(),
                        kRotateBytes >> 20, rotated.filename().string());
                    m_truncatedTest = false;   // a rotated test file starts afresh too
                    return;
                }
                m_rotationFailed = true;   // a thousand rotated files this launch: stop rotating
            }

            void Write(Decision d)
            {
                if (!m_out.is_open() && !Open()) {
                    if (!m_warnedOpen) {
                        logger::error("[SelectionV3] cannot open the v3 log for append"sv);
                        m_warnedOpen = true;
                    }
                    return;
                }
                d.seq = ++m_seq;
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
                    return;
                }
                m_fileBytes += lines.size();
                const size_t capsNew = m_encoder.CapsDefined() - capsBefore;
                {
                    std::lock_guard lock(g_statsMutex);
                    ++g_stats.written;
                    switch (d.outcome) {
                    case Outcome::Key: ++g_stats.writtenKey; break;
                    case Outcome::Wheel: ++g_stats.writtenWheel; break;
                    case Outcome::Menu: ++g_stats.writtenMenu; break;
                    case Outcome::Nothing: ++g_stats.writtenNothing; break;
                    }
                    g_stats.bytes += lines.size();
                    g_stats.ctxWritten += ctxNew ? 1 : 0;
                    g_stats.capsWritten += capsNew;
                }
                logger::debug("[SelectionV3] seq={} out={} rows={} bytes={} (context {}, {} new cap(s))"sv, d.seq,
                    OutcomeName(d.outcome), d.ctx ? d.ctx->rows.size() + d.added.size() : d.added.size(), lines.size(),
                    ctxNew ? "new" : "shared", capsNew);
                if (m_fileBytes >= kRotateBytes && !m_rotationFailed) Rotate();
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::condition_variable m_idle;
            std::deque<Decision> m_queue;
            size_t m_pending = 0;
            bool m_warnedFull = false;
            uint64_t m_droppedSinceWarn = 0;
            std::filesystem::path m_path;

            // Writer thread only.
            Encoder m_encoder;
            std::ofstream m_out;
            std::uintmax_t m_fileBytes = 0;
            std::unordered_set<uint64_t> m_ctxSeen;   // for the debug line (the encoder decides)
            uint64_t m_seq = 0;
            bool m_truncatedTest = false;
            bool m_rotationFailed = false;
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
            d.preEquipped = p.capture.preEquipped;
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
                    // Only the menus whose picks join the menu-open context
                    // take one; the favourites menu's picks join at the press.
                    auto ctx = JoinsAtOpen(e->menuName) ? BuildContext("menu", name) : nullptr;
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

        double g_tickFirstHalfUs = 0.0;   // update thread only: Tick's share of this tick

        void RecordTickCost(Clock::time_point t0)
        {
            const double us = g_tickFirstHalfUs + std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            g_tickFirstHalfUs = 0.0;
            std::lock_guard lock(g_statsMutex);
            g_tickSumUs += us;
            g_stats.tickMeanUs = g_stats.ticks ? g_tickSumUs / static_cast<double>(g_stats.ticks) : 0.0;
            g_stats.tickMaxUs = std::max(g_stats.tickMaxUs, us);
        }
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
            bool newRun = PipelineStateCache::GetSingleton().TakeEligibleIfNewer(g_tick.cacheGeneration,
                g_tick.eligible, generation);
            g_tick.cacheGeneration = generation;
            if (newRun && !g_runGate.IsCurrent(generation)) {
                // The previous save's candidates, still cached after a load.
                g_tick.eligible.clear();
            }
            auto& catalog = Effect::EffectCatalog::GetSingleton();
            if (newRun || (!g_tick.entriesFromReadyCatalog && catalog.Ready())) {
                g_tick.entries.clear();
                g_tick.entries.reserve(g_tick.eligible.size());
                for (const auto& e : g_tick.eligible) g_tick.entries.push_back(catalog.Find(e.formID));
                g_tick.entriesFromReadyCatalog = catalog.Ready();
            }
            if (live) {
                g_tick.need = live->vector.value;
                g_tick.input = live->vector.input;
            }
            // Every tick: the cross-features follow the live vitals, buffs and
            // launcher whether or not the pipeline ran.
            g_tick.rows.clear();
            g_tick.rows.reserve(g_tick.eligible.size());
            for (size_t i = 0; i < g_tick.eligible.size(); ++i) {
                g_tick.rows.push_back(EligibleToRow(g_tick.eligible[i], g_tick.entries[i], player));
            }
            rowCount = g_tick.rows.size();
        }
        RecordHands(nowSec);

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

        // 3. Episode onsets and ends -- not while the game is paused: the
        // world is frozen, so an onset or an expiry then is a wall-clock decay
        // or the player's own menu action, which a pick inside the still-open
        // episode answers. Ended episodes are judged in TickAfterSelections.
        if (live && !paused) {
            std::vector<size_t> onsets;
            {
                std::lock_guard lock(g_episodeMutex);
                onsets = g_episodes.Tick(live->vector.value, nowSec);
            }
            if (!onsets.empty()) {
                auto ctx = BuildContext("onset", {});
                std::lock_guard lock(g_episodeMutex);
                for (const size_t k : onsets) g_episodes.SetPayload(k, ctx);
            }
        }

        {
            std::lock_guard lock(g_statsMutex);
            ++g_stats.ticks;
            g_stats.tickRows = rowCount;
        }
        g_tickFirstHalfUs = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    }

    void TickAfterSelections(Clock::time_point now)
    {
        const auto t0 = Clock::now();
        const double nowSec = Sec(now);
        auto* ui = RE::UI::GetSingleton();
        const bool paused = ui && ui->GameIsPaused();

        // Judged AFTER SelectionTracker::Update, so a selection that confirms on
        // this same tick -- after a stall of the loop, a confirmation and the
        // end of a grace can land together -- answers its episode first.
        if (!paused) {
            std::vector<Core::Needs::EpisodeTracker<std::shared_ptr<const DL::Context>>::Episode> unanswered;
            {
                std::lock_guard lock(g_episodeMutex);
                unanswered = g_episodes.TakeUnanswered(nowSec);
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

        // Picks kept in the v3 log only (confirmed like SelectionTracker's).
        TickPicks(now);

        {
            std::lock_guard dl(g_episodeMutex);
            std::lock_guard sl(g_statsMutex);
            g_stats.episodesDroppedShort = g_episodes.DroppedShort();
        }
        RecordTickCost(t0);
    }

    DecisionCapture CaptureForPress(RE::FormID formID, EquipSource source, std::string_view via)
    {
        const auto now = Clock::now();
        DecisionCapture cap;
        cap.pressSec = Sec(now);
        cap.preEquipped = WasEquippedBefore(formID, cap.pressSec);
        {
            std::lock_guard lock(g_episodeMutex);
            for (const size_t k : g_episodes.OpenNeeds()) cap.open.push_back(static_cast<uint8_t>(k));
        }
        if (source == EquipSource::External && IsPausingMenuVia(via)) {
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
        d.preEquipped = event.v3.preEquipped;
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
        p.capture = CaptureForPress(formID, EquipSource::External, via);
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
            g_held.reset();
            g_heldAt = -1e300;
            g_instances.clear();   // dynamic enchantment IDs mean other things in another save
        }
        {
            std::lock_guard lock(g_handMutex);
            g_hands.fill(HandSnapshot{});
        }
        {
            std::lock_guard lock(g_tickMutex);
            g_tick = TickState{};
            // Until this session's first pipeline run, the cache holds the
            // previous save's candidates and page.
            g_runGate.Reset(PipelineStateCache::GetSingleton().Generation());
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
