#include "UpdateLoop.h"
#include "Globals.h"
#include "pipeline/PipelineCoordinator.h"
#include "Profiling.h"

#include "state/StateManager.h"
#include "candidate/CandidateGenerator.h"
#include "override/OverrideManager.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "wheeler/WheelerClient.h"
#include "learning/PipelineStateCache.h"
#include "learning/EquipEventBus.h"
#include "learning/InventoryExitTracker.h"
#include "util/ScopedTimer.h"
#include "util/InventoryUtil.h"
#include "weapon/WeaponRegistry.h"
#include "telemetry/SoakMetrics.h"

// For the ProcessInventoryChanges constraint: std::same_as / std::convertible_to
// from <concepts>, std::remove_cvref_t from <type_traits>. Both explicit because
// nothing else under src/ pulls them in — they currently arrive through
// CommonLibSSE-NG's umbrella header (and MSVC's <concepts> happens to drag in
// <type_traits>), and this file would be the one that breaks, obscurely, if
// either stops being true.
#include <concepts>
#include <type_traits>

using namespace Huginn;

// =============================================================================
// CONSUMPTION REWARD HELPER
// =============================================================================

// Returns true if the count decrease was a real consumption (cast/drink/eat)
// rather than a drop/sell/store, which must not be rewarded — see
// InventoryExitTracker.
static bool IsConsumption(RE::FormID formID, int32_t delta)
{
    const int32_t removed = -delta;
    const int32_t transferred =
        Learning::InventoryExitTracker::GetSingleton().ClaimExits(formID, removed);
    return transferred < removed;
}

// True during the grace window after a game load / new game, when bulk item
// strips (alt-start mods, settling scripts) masquerade as consumption.
static bool InPostLoadGraceWindow()
{
    const float sinceLoadMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - g_lastGameLoad).count();
    return sinceLoadMs < Config::CONSUMPTION_POST_LOAD_GRACE_MS;
}

static void ApplyConsumptionReward(RE::FormID formID, std::string_view name)
{
    // Suppress rewards right after a load: alt-start/quest scripts strip items
    // in bulk and would otherwise train the learner on drinks that never
    // happened (see CONSUMPTION_POST_LOAD_GRACE_MS).
    if (InPostLoadGraceWindow()) {
        logger::debug("[Learning] Skipped consumption reward (post-load grace): {} ({:08X})",
            name, formID);
        return;
    }

    auto& cache = Learning::PipelineStateCache::GetSingleton();
    if (!cache.IsStale(500.0f)) {
        Learning::EquipEventBus::GetSingleton().Publish(
            formID, Learning::EquipSource::Consumption, 1.0f, false);

        logger::info("[Learning] Consumption event published: {} ({:08X})",
            name, formID);
    } else {
        logger::debug("[Learning] Skipped consumption reward (stale cache): {} ({:08X})",
            name, formID);
    }
}

// =============================================================================
// INVENTORY DELTA PROCESSING
// =============================================================================
// One consumption policy for every count-tracking registry: break the slot lock
// pinning the item, separate real consumption from drop/sell/store, and reward
// only the former.
//
// Templated rather than given a common base class: ItemChangeEvent and
// ScrollChangeEvent are independent structs that happen to agree on
// formID/name/delta, and both RefreshCounts overloads already take
// (player, InventoryItemMap). A shared base would exist solely to satisfy this
// one call.
//
// POLL THREAD ONLY. RefreshCounts populates unsynchronized scratch maps outside
// the registry mutex, which is safe only because it has a single caller on the
// poll thread (see ItemRegistry.h "Scratch maps for RefreshCounts delta scans").
// That invariant used to be self-evident from an inline call site; behind a
// helper built to be called from more places, it needs saying.
//
// `tag` is the log prefix ("ItemRegistry" / "ScrollRegistry"), kept verbatim so
// existing log greps still match. Returns true if any count changed — i.e. the
// widget needs a recompute, which is why the result must not be discarded:
// dropping it compiles cleanly and silently stops the widget refreshing on
// consumption.

template <typename Registry>
[[nodiscard]] static bool ProcessInventoryChanges(
    Registry& registry,
    std::string_view tag,
    RE::PlayerCharacter* player,
    const Util::InventoryItemMap& inventory)
{
    auto changes = registry.RefreshCounts(player, inventory);

    // Fail here, not somewhere inside the loop body, when a third registry turns
    // up whose change event doesn't carry these three fields.
    //
    // formID and delta demand the exact type: convertible_to would accept any
    // arithmetic type, so a registry declaring `float delta` for a charge delta
    // would pass and then narrow silently through IsConsumption() and
    // -change.delta. name stays convertible — std::string, const char* and
    // string_view are all fine to log.
    static_assert(
        requires(const typename decltype(changes)::value_type& c) {
            { c.name } -> std::convertible_to<std::string_view>;
            requires std::same_as<std::remove_cvref_t<decltype(c.formID)>, RE::FormID>;
            requires std::same_as<std::remove_cvref_t<decltype(c.delta)>, int32_t>;
        },
        "Registry change event must expose formID (RE::FormID), name, delta (int32_t)");

    for (const auto& change : changes) {
        if (change.delta >= 0) {
            continue;
        }

        // Item left inventory (consumed/dropped/sold) — break any slot lock
        // pinning it. Otherwise SlotLocker holds the FormID until its timer
        // expires and re-pins it through the recompute the caller triggers,
        // leaving a recommendation for an item the player no longer owns.
        Slot::SlotLocker::GetSingleton().OnItemUsed(change.formID, /*respectActivationLock=*/true);

        if (!IsConsumption(change.formID, change.delta)) {
            logger::debug("[{}] Left inventory (drop/sell/store), no reward: {} x{}"sv,
                tag, change.name, -change.delta);
            continue;
        }
        logger::debug("[{}] Consumed: {} x{}"sv, tag, change.name, -change.delta);
        ApplyConsumptionReward(change.formID, change.name);
    }

    return !changes.empty();
}

// =============================================================================
// STAGE 1: SUBSYSTEM UPDATES
// =============================================================================
// Tick time-dependent subsystems: state sensors, candidate cooldowns,
// potion discriminator combat timer, override hysteresis.

static void UpdateSubsystems(float deltaSeconds, float deltaMs)
{
    Huginn_ZONE_NAMED("Update::Subsystems");

    State::StateManager::GetSingleton().Update(deltaMs);

    // Timer-driven subsystems whose expirations change what should be displayed
    // but produce no state delta: each reports lapses and we force one pipeline
    // run. Without this, expired cooldowns/wildcards/latches/locks linger
    // on-screen while the skip gate holds the pipeline idle.
    bool forcePipelineRun = false;

    {
        Huginn_ZONE_NAMED("CandidateGenerator::Update");
        // Cooldown expiry: the item becomes recommendable again
        forcePipelineRun |= Candidate::CandidateGenerator::GetSingleton().Update(deltaSeconds);
    }

    if (g_utilityScorer) {
        Huginn_ZONE_NAMED("UtilityScorer::Update");
        // Wildcard expiry: the exploration slot reverts to the ranked pick
        forcePipelineRun |= g_utilityScorer->Update(deltaSeconds);

        auto transition = State::StateManager::GetSingleton().ConsumeCombatTransition();
        if (transition == State::StateManager::CombatTransition::Entered) {
            g_utilityScorer->OnCombatStart();
        } else if (transition == State::StateManager::CombatTransition::Exited) {
            g_utilityScorer->OnCombatEnd();
        }
    }

    {
        Huginn_ZONE_NAMED("OverrideManager::Update");
        // Hysteresis latch crossing min-duration: deactivation becomes possible
        forcePipelineRun |= Override::OverrideManager::GetSingleton().Update(deltaMs);
    }

    {
        Huginn_ZONE_NAMED("SlotLocker::Update");
        // Lock expiry: decay wall-clock (not just on pipeline-active ticks) and
        // let the freed slot's content swap immediately
        forcePipelineRun |= Slot::SlotLocker::GetSingleton().Update(deltaMs);
    }

    if (forcePipelineRun) {
        Slot::SlotAllocator::GetSingleton().MarkPageDirty();
    }
}

// =============================================================================
// STAGE 2: REGISTRY MAINTENANCE
// =============================================================================
// Timer-driven reconciliation and delta scans for all four registries.
// Consumption events are published here when item/scroll counts drop.

static void MaintainRegistries(RE::PlayerCharacter* player,
                               std::chrono::steady_clock::time_point now)
{
    Huginn_ZONE_NAMED("Update::Registries");

    // Spell registry
    if (g_spellRegistry) {
        if (g_registryTimers.spellReconcile.CheckAndReset(now, Config::SPELL_RECONCILE_INTERVAL_MS)) {
            Huginn_ZONE_NAMED("SpellRegistry::Reconcile");
            g_spellRegistry->ReconcileSpells(player);
        }
        if (!g_spellRegistry->IsLoading() &&
            g_registryTimers.spellFavorites.CheckAndReset(now, Config::SPELL_FAVORITES_REFRESH_INTERVAL_MS)) {
            Huginn_ZONE_NAMED("SpellRegistry::RefreshFavorites");
            g_spellRegistry->RefreshFavorites(player);
        }
    }

    // Item + Scroll delta scans (500ms, same interval) share ONE inventory pass.
    // A single GetInventorySafe with a combined filter feeds both registries —
    // each ignores the other's form types — mirroring the weapon block's
    // shared-query pattern below. Reconciles (30s) stay in their own blocks.
    //
    // In steady state both delta timers share a baseline and fire on the same
    // tick, so this reliably saves one traversal per window. If the item registry
    // is IsLoading() for some ticks while scroll keeps firing, the two timers can
    // desync and temporarily degrade back to two scans per window until they
    // realign — a transient post-load perf edge case, not a correctness issue.
    {
        const bool itemDeltaDue = g_itemRegistry && !g_itemRegistry->IsLoading() &&
            g_registryTimers.itemDelta.IsDue(now, Config::ITEM_COUNT_REFRESH_INTERVAL_MS);
        const bool scrollDeltaDue = g_scrollRegistry &&
            g_registryTimers.scrollDelta.IsDue(now, Config::ITEM_COUNT_REFRESH_INTERVAL_MS);

        if (itemDeltaDue || scrollDeltaDue) {
            Huginn_ZONE_NAMED("Inventory::DeltaScan");
            auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
                return obj.Is(RE::FormType::AlchemyItem) || obj.Is(RE::FormType::SoulGem) ||
                       obj.Is(RE::FormType::Scroll);
            });

            // Any inventory count change can invalidate the current widget — a
            // consumed/dropped item must leave it, a new item may belong on it.
            // The GameState hash excludes inventory, so without an explicit signal
            // the pipeline never recomputes while the player is otherwise idle.
            bool inventoryChanged = false;

            if (itemDeltaDue) {
                inventoryChanged |= ProcessInventoryChanges(
                    *g_itemRegistry, "ItemRegistry"sv, player, inventory);
                g_registryTimers.itemDelta.Reset(now);
            }
            if (scrollDeltaDue) {
                inventoryChanged |= ProcessInventoryChanges(
                    *g_scrollRegistry, "ScrollRegistry"sv, player, inventory);
                g_registryTimers.scrollDelta.Reset(now);
            }

            // Force one recompute so count==0 items drop from candidates and the
            // widget refreshes even when no GameState field changed. Reuses the
            // existing "page dirty" force-recompute path (as Wheeler-close does).
            if (inventoryChanged) {
                Slot::SlotAllocator::GetSingleton().MarkPageDirty();
            }
        }
    }

    // Item registry — full reconcile (30s); delta scan handled in the combined block above
    if (g_itemRegistry && !g_itemRegistry->IsLoading()) {
        if (g_registryTimers.itemReconcile.CheckAndReset(now, Config::ITEM_RECONCILE_INTERVAL_MS)) {
            Huginn_ZONE_NAMED("ItemRegistry::Reconcile");
            g_itemRegistry->ReconcileItems(player);
        }
    }

    // Weapon registry — shared EquippedWeapons query between charge + reconcile
    if (g_weaponRegistry) {
        if (g_weaponRegistry->IsLoading()) {
            static bool warnedAboutLoading = false;
            if (!warnedAboutLoading) {
                logger::warn("[WeaponRegistry] Stuck in loading state - reconciliation blocked"sv);
                warnedAboutLoading = true;
            }
        } else {
            bool needsCharge = g_registryTimers.weaponCharge.IsDue(now, Config::WEAPON_REFRESH_INTERVAL_MS);
            bool needsReconcile = g_registryTimers.weaponReconcile.IsDue(now, Config::WEAPON_RECONCILE_INTERVAL_MS);
            if (needsCharge || needsReconcile) {
                Huginn_ZONE_NAMED("WeaponRegistry::Refresh");
                auto equipped = [&] {
                    Huginn_ZONE_NAMED("EquippedWeapons::Query");
                    return Huginn::Weapon::EquippedWeapons::Query(player);
                }();
                if (needsCharge) { g_weaponRegistry->RefreshCharges(equipped); g_registryTimers.weaponCharge.Reset(now); }
                if (needsReconcile) { g_weaponRegistry->ReconcileWeapons(equipped); g_registryTimers.weaponReconcile.Reset(now); }
            }
        }
    }

    // Scroll registry — full reconcile (30s); delta scan handled in the combined block above
    if (g_scrollRegistry) {
        if (g_registryTimers.scrollReconcile.CheckAndReset(now, Config::ITEM_RECONCILE_INTERVAL_MS)) {
            Huginn_ZONE_NAMED("ScrollRegistry::Reconcile");
            g_scrollRegistry->ReconcileScrolls(player);
        }
    }
}

// =============================================================================
// STAGE 3: PIPELINE EXECUTION
// =============================================================================
// Skip-check + pipeline run. Returns early if state is unchanged and no
// page was cycled.

static void RunPipelineIfNeeded(float deltaMs, RE::PlayerCharacter* player,
                                std::chrono::steady_clock::time_point now)
{
    // Covers the skip-check (Wheeler close, dirty flags, cache timestamp
    // refresh); RunPipeline has its own zone for the actual pipeline.
    Huginn_ZONE_NAMED("Update::PipelineCheck");

    // Process deferred Wheeler close BEFORE checking page-changed flag.
    // If the wheel truly closed, this sets MarkPageDirty() for us.
    Wheeler::WheelerClient::GetSingleton().CheckPendingWheelClose();

    auto& stateManager = State::StateManager::GetSingleton();
    bool stateChanged = stateManager.DidLastUpdateChangeState();
    bool pageChanged = Slot::SlotAllocator::GetSingleton().PeekPageChanged();

    // Elemental enrichment flags decay with wall-clock time without producing a
    // state delta, so the outer gate must stay open for the whole window —
    // mirrors the inner-gate bypass in CheckHashSkip (which remains the
    // authority once fresh state is gathered).
    if (!stateChanged && !pageChanged && !stateManager.IsElementalWindowActive()) {
        Learning::PipelineStateCache::GetSingleton().RefreshTimestamp();
        return;
    }

    if (g_utilityScorer && g_stateEvaluator && g_spellRegistry && !g_spellRegistry->IsLoading()) {
        if (pageChanged) {
            Slot::SlotAllocator::GetSingleton().ConsumePageChanged();
        }
        Pipeline::PipelineCoordinator::GetSingleton().RunPipeline(
            deltaMs, player, now, pageChanged);
    }
}

// =============================================================================
// UPDATE LOOP ENTRY POINT
// =============================================================================

void OnUpdate(float deltaSeconds)
{
    if (g_updateSystemFailed.load(std::memory_order_acquire)) {
        static auto lastWarning = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::minutes>(now - lastWarning).count() >= 1) {
            logger::warn("[Huginn] Update system is in degraded mode - handler registration failed"sv);
            lastWarning = now;
        }
        return;
    }

    SCOPED_TIMER("MainUpdate");
    Huginn_ZONE_NAMED("OnUpdate");

    auto now = std::chrono::steady_clock::now();

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    float deltaMs = deltaSeconds * 1000.0f;

    UpdateSubsystems(deltaSeconds, deltaMs);
    MaintainRegistries(player, now);
    RunPipelineIfNeeded(deltaMs, player, now);

    // Soak telemetry: record whole-tick cost and emit the periodic heartbeat.
    const float tickMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - now).count();
    Telemetry::SoakMetrics::GetSingleton().RecordTick(tickMs, now);
}
