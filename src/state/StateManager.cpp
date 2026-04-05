// =============================================================================
// StateManager.cpp - Core state management (v0.6.x)
// =============================================================================
// Central coordinator for all game state polling and management.
// This file contains the core methods: constructor, Update(), ForceUpdate(),
// and state accessors.
//
// Poll method implementations are split into separate files:
// - StateManager_World.cpp           - PollWorldObjects() + crosshair helpers
// - StateManager_Vitals.cpp          - PollPlayerVitals()
// - StateManager_MagicEffects.cpp    - PollPlayerMagicEffects() + CacheRaceFormIDs()
// - StateManager_Equipment.cpp       - PollPlayerEquipment()
// - StateManager_Survival.cpp        - PollPlayerSurvival() + CacheSurvivalGlobals()
// - StateManager_Position.cpp        - PollPlayerPosition()
// - StateManager_Targets.cpp         - PollTargets() + target management helpers
// - StateManager_HealthTracking.cpp  - PollHealthTracking()
// - StateManager_StaminaTracking.cpp - PollStaminaTracking()
// - StateManager_MagickaTracking.cpp - PollMagickaTracking()
// - StateManager_Resistances.cpp     - PollPlayerResistances()
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   // =============================================================================
   // CONSTRUCTOR
   // =============================================================================

   StateManager::StateManager()
   {
      // Pre-reserve ore vein caches to avoid rehashing
      // (pattern from CrosshairSensor)
      m_oreVeinCache.reserve(50);
      m_notOreVeinCache.reserve(50);

      // Pre-reserve target collection capacity
      m_targets.targets.reserve(TargetTracking::MAX_TRACKED_TARGETS);

      // Pre-reserve optimization caches
      m_actorTypeCache.reserve(TargetTracking::MAX_TRACKED_TARGETS);
      m_processedAllies.reserve(TargetTracking::MAX_TRACKED_TARGETS);

      logger::info("[StateManager] Initialized - v0.6.1 State Architecture"sv);
   }

   // =============================================================================
   // POLL TABLE
   // =============================================================================

   auto StateManager::GetPollTable() noexcept -> std::array<PollEntry, kPollEntryCount>
   {
      return {{
         {&m_worldObjectsTimer,       &m_worldPollInterval,              &StateManager::PollWorldObjects},
         {&m_playerVitalsTimer,       &m_playerVitalsPollInterval,       &StateManager::PollPlayerVitals},
         {&m_playerMagicEffectsTimer, &m_playerMagicEffectsPollInterval, &StateManager::PollPlayerMagicEffects},
         {&m_playerEquipmentTimer,    &m_playerEquipmentPollInterval,    &StateManager::PollPlayerEquipment},
         {&m_playerSurvivalTimer,     &m_playerSurvivalPollInterval,     &StateManager::PollPlayerSurvival},
         {&m_playerPositionTimer,     &m_playerPositionPollInterval,     &StateManager::PollPlayerPosition},
         {&m_targetsTimer,            &m_targetsPollInterval,            &StateManager::PollTargets},
         {&m_healthTrackingTimer,     &m_healthTrackingPollInterval,     &StateManager::PollHealthTracking},
         {&m_playerResistancesTimer,  &m_playerResistancesPollInterval,  &StateManager::PollPlayerResistances},
         {&m_staminaTrackingTimer,    &m_staminaTrackingPollInterval,    &StateManager::PollStaminaTracking},
         {&m_magickaTrackingTimer,    &m_magickaTrackingPollInterval,    &StateManager::PollMagickaTracking},
      }};
   }

   // =============================================================================
   // UPDATE METHODS
   // =============================================================================

   void StateManager::Update(float deltaMs)
   {
      Huginn_ZONE_NAMED("StateManager::Update");
      bool changed = false;
      for (auto [timer, interval, poll] : GetPollTable()) {
         *timer += deltaMs;
         if (*timer >= *interval) {
            changed |= (this->*poll)();
            *timer = 0.0f;
         }
      }
      m_lastUpdateChanged = changed;
   }

   bool StateManager::PollAll()
   {
      bool changed = false;
      for (auto [timer, interval, poll] : GetPollTable()) {
         changed |= (this->*poll)();
      }
      return changed;
   }

   void StateManager::ResetTrackingState()
   {
      logger::info("[StateManager] ResetTrackingState() - clearing accumulated state for save load"sv);

      // --- Health/Stamina/Magicka tracking state ---
      {
         std::unique_lock lock(m_playerMutex);
         m_healthTracking = HealthTrackingState{};
         m_staminaTracking = StaminaTrackingState{};
         m_magickaTracking = MagickaTrackingState{};
      }

      // --- Delta baselines (sentinel = not initialized, triggers re-init on next poll) ---
      m_previousHealth = -1.0f;
      m_previousStamina = -1.0f;
      m_previousMagicka = -1.0f;
      m_previousDamageRate = 0.0f;
      m_previousHealingRate = 0.0f;
      m_previousStaminaUsageRate = 0.0f;
      m_previousMagickaUsageRate = 0.0f;

      // --- Sub-threshold accumulators ---
      m_accumulatedHealthDamage = 0.0f;
      m_accumulatedStaminaUsage = 0.0f;
      m_accumulatedMagickaUsage = 0.0f;

      // --- Target tracking ---
      {
         std::unique_lock lock(m_targetsMutex);
         m_targets = TargetCollection{};
         m_targets.targets.reserve(TargetTracking::MAX_TRACKED_TARGETS);
      }
      m_stickyTargetFormID = 0;
      m_stickyTargetLastSeenTime = 0.0f;

      // --- Actor type cache (race could differ between save files) ---
      m_actorTypeCache.clear();
      m_processedAllies.clear();

      // --- Drain any pending DamageEventSink events (prevent stale queued hits) ---
      DamageEventSink::GetSingleton().DrainQueue();

      // --- Reset all poll timers (force immediate re-poll) ---
      for (auto [timer, interval, poll] : GetPollTable()) {
         *timer = 0.0f;
      }

      // --- Combat transition tracking ---
      m_combatTransition.store(CombatTransition::None, std::memory_order_relaxed);
      m_isInCombat.store(false, std::memory_order_relaxed);
      m_wasInCombat = false;

      m_lastUpdateChanged = true;  // Force pipeline to run on next update
   }

   void StateManager::ForceUpdate()
   {
      logger::info("[StateManager] ForceUpdate() called - polling all sensors"sv);

      PollAll();

      for (auto [timer, interval, poll] : GetPollTable()) {
         *timer = 0.0f;
      }
   }

   // =============================================================================
   // STATE ACCESSORS (Thread-safe via copy-out)
   // =============================================================================

   WorldState StateManager::GetWorldState() const noexcept
   {
      std::shared_lock lock(m_worldMutex);
      return m_worldState;  // Copy-out pattern
   }

   PlayerActorState StateManager::GetPlayerState() const noexcept
   {
      std::shared_lock lock(m_playerMutex);
      return m_playerState;  // Copy-out pattern
   }

   TargetCollection StateManager::GetTargets() const noexcept
   {
      std::shared_lock lock(m_targetsMutex);
      return m_targets;  // Copy-out pattern
   }

   std::optional<TargetActorState> StateManager::GetPrimaryTarget() const
   {
      std::shared_lock lock(m_targetsMutex);
      return m_targets.primary;  // Copy-out pattern
   }

   HealthTrackingState StateManager::GetHealthTracking() const noexcept
   {
      std::shared_lock lock(m_playerMutex);
      return m_healthTracking;  // Copy-out pattern
   }

   StaminaTrackingState StateManager::GetStaminaTracking() const noexcept
   {
      std::shared_lock lock(m_playerMutex);
      return m_staminaTracking;  // Copy-out pattern
   }

   MagickaTrackingState StateManager::GetMagickaTracking() const noexcept
   {
      std::shared_lock lock(m_playerMutex);
      return m_magickaTracking;  // Copy-out pattern
   }

} // namespace Huginn::State
