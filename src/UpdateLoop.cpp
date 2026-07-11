#include "UpdateLoop.h"
#include "Globals.h"
#include "pipeline/PipelineCoordinator.h"
#include "Profiling.h"

#include "state/StateManager.h"
#include "candidate/CandidateGenerator.h"
#include "override/OverrideManager.h"
#include "slot/SlotAllocator.h"
#include "wheeler/WheelerClient.h"
#include "learning/PipelineStateCache.h"
#include "learning/EquipEventBus.h"
#include "learning/InventoryExitTracker.h"
#include "util/ScopedTimer.h"
#include "util/InventoryUtil.h"
#include "weapon/WeaponRegistry.h"

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

static void ApplyConsumptionReward(RE::FormID formID, std::string_view name)
{
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
// STAGE 1: SUBSYSTEM UPDATES
// =============================================================================
// Tick time-dependent subsystems: state sensors, candidate cooldowns,
// potion discriminator combat timer, override hysteresis.

static void UpdateSubsystems(float deltaSeconds, float deltaMs)
{
    Huginn_ZONE_NAMED("Update::Subsystems");

    State::StateManager::GetSingleton().Update(deltaMs);

    {
        Huginn_ZONE_NAMED("CandidateGenerator::Update");
        Candidate::CandidateGenerator::GetSingleton().Update(deltaSeconds);
    }

    if (g_utilityScorer) {
        Huginn_ZONE_NAMED("UtilityScorer::Update");
        g_utilityScorer->Update(deltaSeconds);

        auto transition = State::StateManager::GetSingleton().ConsumeCombatTransition();
        if (transition == State::StateManager::CombatTransition::Entered) {
            g_utilityScorer->OnCombatStart();
        } else if (transition == State::StateManager::CombatTransition::Exited) {
            g_utilityScorer->OnCombatEnd();
        }
    }

    {
        Huginn_ZONE_NAMED("OverrideManager::Update");
        Override::OverrideManager::GetSingleton().Update(deltaMs);
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

            if (itemDeltaDue) {
                auto changes = g_itemRegistry->RefreshCounts(player, inventory);
                for (const auto& change : changes) {
                    if (change.delta < 0) {
                        if (!IsConsumption(change.formID, change.delta)) {
                            logger::debug("[ItemRegistry] Left inventory (drop/sell/store), no reward: {} x{}"sv,
                                change.name, -change.delta);
                            continue;
                        }
                        logger::debug("[ItemRegistry] Consumed: {} x{}"sv, change.name, -change.delta);
                        ApplyConsumptionReward(change.formID, change.name);
                    }
                }
                g_registryTimers.itemDelta.Reset(now);
            }
            if (scrollDeltaDue) {
                auto changes = g_scrollRegistry->RefreshCounts(player, inventory);
                for (const auto& change : changes) {
                    if (change.delta < 0) {
                        if (!IsConsumption(change.formID, change.delta)) {
                            logger::debug("[ScrollRegistry] Left inventory (drop/sell/store), no reward: {} x{}"sv,
                                change.name, -change.delta);
                            continue;
                        }
                        logger::debug("[ScrollRegistry] Consumed: {} x{}"sv, change.name, -change.delta);
                        ApplyConsumptionReward(change.formID, change.name);
                    }
                }
                g_registryTimers.scrollDelta.Reset(now);
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

    if (!stateChanged && !pageChanged) {
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
}
