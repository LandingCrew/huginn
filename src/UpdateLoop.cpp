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
#include "util/ScopedTimer.h"
#include "weapon/WeaponRegistry.h"

using namespace Huginn;

// =============================================================================
// CONSUMPTION REWARD HELPER
// =============================================================================
// Publishes a Consumption event to the EquipEventBus when the player consumes
// an item or scroll. Subscribers (FQL, UsageMemory, Cooldown) handle the rest.
// =============================================================================

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
// UPDATE LOOP
// =============================================================================

void OnUpdate(float deltaSeconds)
{
  // Issue #4: Early-exit if update system failed to register properly
  // Log periodic warnings so users know something is wrong
  if (g_updateSystemFailed.load(std::memory_order_acquire)) {
    static auto lastWarning = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::minutes>(now - lastWarning).count() >= 1) {
      logger::warn("[Huginn] Update system is in degraded mode - handler registration failed"sv);
      lastWarning = now;
    }
    return;  // Don't try to update with broken system
  }

  SCOPED_TIMER("MainUpdate");
  Huginn_ZONE_NAMED("OnUpdate");

  auto now = std::chrono::steady_clock::now();

  // OPTIMIZATION (S2 v0.7.19): Query player once per update cycle
  auto* player = RE::PlayerCharacter::GetSingleton();
  if (!player) return;  // No player, nothing to update

  // Update state systems (converts deltaSeconds to milliseconds)
  float deltaMs = deltaSeconds * 1000.0f;

  // StateManager is the primary state system (v0.6.x Phase 5)
  State::StateManager::GetSingleton().Update(deltaMs);

  // Update CandidateGenerator cooldowns (v0.8.x)
  Candidate::CandidateGenerator::GetSingleton().Update(deltaSeconds);

  // Update UtilityScorer (combat tracking for potion discrimination - v0.9.0)
  if (g_utilityScorer) {
    g_utilityScorer->Update(deltaSeconds);

    // Combat transitions are now detected inside StateManager::PollPlayerPosition,
    // avoiding the full PlayerActorState copy (~300 bytes + heap alloc) that the
    // old approach required just to read a single bool.
    auto transition = State::StateManager::GetSingleton().ConsumeCombatTransition();
    if (transition == State::StateManager::CombatTransition::Entered) {
      g_utilityScorer->OnCombatStart();
    } else if (transition == State::StateManager::CombatTransition::Exited) {
      g_utilityScorer->OnCombatEnd();
    }
  }

  // Update OverrideManager (hysteresis timers - v0.10.0)
  Override::OverrideManager::GetSingleton().Update(deltaMs);

  // State transition logging moved to pipeline entry (DiffGameState)

  // Reconcile spell registry periodically
  if (g_spellRegistry) {
    if (g_registryTimers.spellReconcile.CheckAndReset(now, Config::SPELL_RECONCILE_INTERVAL_SECONDS * 1000.0f)) {
      g_spellRegistry->ReconcileSpells(player);
    }
    if (!g_spellRegistry->IsLoading() &&
        g_registryTimers.spellFavorites.CheckAndReset(now, Config::SPELL_FAVORITES_REFRESH_INTERVAL_MS)) {
      g_spellRegistry->RefreshFavorites(player);
    }
  }

  // Item registry refresh (v0.7.4) - two-tier strategy
  if (g_itemRegistry && !g_itemRegistry->IsLoading()) {
    if (g_registryTimers.itemDelta.CheckAndReset(now, Config::ITEM_COUNT_REFRESH_INTERVAL_MS)) {
      auto changes = g_itemRegistry->RefreshCounts(player);
      for (const auto& change : changes) {
        if (change.delta < 0) {
          logger::debug("[ItemRegistry] Consumed: {} x{}"sv, change.name, -change.delta);
          ApplyConsumptionReward(change.formID, change.name);
        }
      }
    }
    if (g_registryTimers.itemReconcile.CheckAndReset(now, Config::ITEM_RECONCILE_INTERVAL_MS)) {
      g_itemRegistry->ReconcileItems(player);
    }
  }

  // Weapon registry refresh (v0.7.6) - two-tier with shared EquippedWeapons query
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
        auto equipped = Huginn::Weapon::EquippedWeapons::Query(player);
        if (needsCharge) { g_weaponRegistry->RefreshCharges(equipped); g_registryTimers.weaponCharge.Reset(now); }
        if (needsReconcile) { g_weaponRegistry->ReconcileWeapons(equipped); g_registryTimers.weaponReconcile.Reset(now); }
      }
    }
  }

  // Scroll registry refresh (v0.7.7) - two-tier strategy like ItemRegistry
  if (g_scrollRegistry) {
    if (g_registryTimers.scrollDelta.CheckAndReset(now, Config::ITEM_COUNT_REFRESH_INTERVAL_MS)) {
      auto changes = g_scrollRegistry->RefreshCounts(player);
      for (const auto& change : changes) {
        if (change.delta < 0) {
          logger::debug("[ScrollRegistry] Consumed: {} x{}"sv, change.name, -change.delta);
          ApplyConsumptionReward(change.formID, change.name);
        }
      }
    }
    if (g_registryTimers.scrollReconcile.CheckAndReset(now, Config::ITEM_RECONCILE_INTERVAL_MS)) {
      g_scrollRegistry->ReconcileScrolls(player);
    }
  }

  // Stage 3c-pre: Process deferred Wheeler close (must run BEFORE ConsumePageChanged)
  // Wheeler fires its close callback before updating IsWheelOpen(), so we defer
  // the actual close processing to here where the API state is accurate.
  // If the wheel truly closed, this calls MarkPageDirty() which ConsumePageChanged()
  // will pick up below, ensuring the pipeline runs and refreshes IntuitionMenu.
  Wheeler::WheelerClient::GetSingleton().CheckPendingWheelClose();

  // Stage 3c: Skip expensive pipeline when state unchanged (performance optimization)
  // StateManager Update() already ran and tracked whether any sensor detected changes.
  // If no state changed, recommendations don't need to be recomputed.
  // EXCEPTION: Page cycling (key 4/5) changes what to display without changing game state,
  // so we must run the pipeline to update IntuitionMenu with the new page's slots.
  auto& stateManager = State::StateManager::GetSingleton();
  bool stateChanged = stateManager.DidLastUpdateChangeState();
  bool pageChanged = Slot::SlotAllocator::GetSingleton().PeekPageChanged();

  if (!stateChanged && !pageChanged) {
    Learning::PipelineStateCache::GetSingleton().RefreshTimestamp();
    return;  // Skip entire pipeline — huge performance win when idle
  }

  // Recommendation pipeline: state → candidates → scoring → slot allocation → display
  if (g_utilityScorer && g_stateEvaluator && g_spellRegistry && !g_spellRegistry->IsLoading()) {
      // Consume the page-changed flag only when we're actually going to run the pipeline.
      // Previously, ConsumePageChanged() ran before this guard — if the guard failed
      // (e.g., registry loading), the flag was lost and the display never updated.
      if (pageChanged) {
          Slot::SlotAllocator::GetSingleton().ConsumePageChanged();
      }
      Pipeline::PipelineCoordinator::GetSingleton().RunPipeline(
          deltaMs, player, now, pageChanged);
  }
}
