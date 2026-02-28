// =============================================================================
// StateManager_StaminaTracking.cpp - Stamina tracking polling (v0.6.9)
// =============================================================================
// Part of StateManager implementation split
// Polls: stamina usage/regen detection and rate calculation
// Updates: StaminaTrackingState
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   // =============================================================================
   // STAMINA SOURCE CLASSIFICATION (v0.6.9)
   // =============================================================================
   // Classifies stamina usage source based on player state.
   // Returns the most likely source of stamina drain.
   // =============================================================================

   StaminaUsageSource StateManager::ClassifyStaminaUsage(RE::PlayerCharacter* player) noexcept
   {
      if (!player) {
      return StaminaUsageSource::Unknown;
      }

      auto* actorState = player->AsActorState();
      if (!actorState) {
      return StaminaUsageSource::Unknown;
      }

      // Check sprinting first (most common stamina drain)
      // Use ActorState to check if sprinting via movement flags
      if (actorState->IsSprinting()) {
      return StaminaUsageSource::Sprint;
      }

      // Check blocking (shield stamina drain)
      if (actorState->GetAttackState() == RE::ATTACK_STATE_ENUM::kBash) {
      return StaminaUsageSource::ShieldBash;
      }

      // Check swimming
      if (actorState->IsSwimming()) {
      return StaminaUsageSource::Swimming;
      }

      // Check mid-air (jump stamina)
      if (player->IsInMidair()) {
      return StaminaUsageSource::Jump;
      }

      // Check attack state for power attacks
      auto attackState = actorState->GetAttackState();
      // Power attacks have longer wind-up animations
      if (attackState == RE::ATTACK_STATE_ENUM::kSwing ||
          attackState == RE::ATTACK_STATE_ENUM::kBowDraw ||
          attackState == RE::ATTACK_STATE_ENUM::kBowAttached) {
      return StaminaUsageSource::PowerAttack;
      }

      return StaminaUsageSource::Unknown;
   }

   // =============================================================================
   // STAMINA TRACKING POLLING (v0.6.9)
   // =============================================================================
   // Tracks player stamina usage and regeneration events for stamina potion
   // and restore stamina spell recommendations.
   // Uses stamina delta detection and player state classification.
   // =============================================================================

   bool StateManager::PollStaminaTracking()
   {
      Huginn_ZONE_NAMED("PollStaminaTracking");
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return false;
      }

      auto* actorValueOwner = player->AsActorValueOwner();
      if (!actorValueOwner) {
      return false;
      }

      // Get current game time (total elapsed days, monotonically increasing)
      float gameTime = 0.0f;
      auto* calendar = RE::Calendar::GetSingleton();
      if (calendar) {
      gameTime = calendar->GetCurrentGameTime();
      }

      // Get current stamina value
      float currentStamina = actorValueOwner->GetActorValue(RE::ActorValue::kStamina);

      // Initialize previous stamina on first poll
      if (m_previousStamina < 0.0f) {
      m_previousStamina = currentStamina;
      return false;
      }

      // Calculate stamina delta
      float staminaDelta = currentStamina - m_previousStamina;

      // Build new tracking state
      StaminaTrackingState newState;

      // Copy existing history (will prune old events below)
      {
      std::shared_lock lock(m_playerMutex);
      newState = m_staminaTracking;
      }

      // v0.12.x: Accumulate sub-threshold stamina losses across ticks.
      VitalTracking::UpdateAccumulator(m_accumulatedStaminaUsage, staminaDelta);

      // Emit usage event when accumulated total crosses threshold
      if (m_accumulatedStaminaUsage >= VitalTracking::STAMINA_USAGE_THRESHOLD) {
      float usageAmount = m_accumulatedStaminaUsage;
      StaminaUsageSource source = ClassifyStaminaUsage(player);

      StaminaUsageEvent event(gameTime, usageAmount, source);
      newState.usage.history.push_back(event);
      m_accumulatedStaminaUsage = 0.0f;
      }

      // Detect regen (stamina increased)
      if (staminaDelta > VitalTracking::REGEN_THRESHOLD) {
      StaminaRegenEvent event(gameTime, staminaDelta);
      newState.regen.history.push_back(event);
      }

      // Prune old events (older than HISTORY_RETENTION_DAYS)
      while (!newState.usage.history.empty() &&
             (gameTime - newState.usage.history.front().timestamp) > VitalTracking::HISTORY_RETENTION_DAYS) {
      newState.usage.history.pop_front();
      }
      while (!newState.regen.history.empty() &&
             (gameTime - newState.regen.history.front().timestamp) > VitalTracking::HISTORY_RETENTION_DAYS) {
      newState.regen.history.pop_front();
      }

      // Calculate aggregate usage values
      float totalUsage = 0.0f;
      float powerAttackUsage = 0.0f;
      float sprintUsage = 0.0f;
      float latestUsageTime = 0.0f;

      for (const auto& event : newState.usage.history) {
      float eventAge = gameTime - event.timestamp;
      if (eventAge <= VitalTracking::ACTIVE_WINDOW_DAYS) {
        // Exponential decay weighting
        float weight = std::exp(-VitalTracking::EXPONENTIAL_DECAY_CONSTANT * eventAge / VitalTracking::ACTIVE_WINDOW_DAYS);
        totalUsage += event.amount * weight;

        switch (event.source) {
           case StaminaUsageSource::PowerAttack:
            powerAttackUsage += event.amount * weight;
            break;
           case StaminaUsageSource::Sprint:
            sprintUsage += event.amount * weight;
            break;
           default:
            break;
        }
      }
      if (event.timestamp > latestUsageTime) {
        latestUsageTime = event.timestamp;
      }
      }

      newState.usage.recentAmount = totalUsage;
      newState.powerAttackPercent = (totalUsage > 0.0f) ? (powerAttackUsage / totalUsage) : 0.0f;
      newState.sprintPercent = (totalUsage > 0.0f) ? (sprintUsage / totalUsage) : 0.0f;

      // Calculate time since last usage
      newState.usage.timeSinceLast = VitalTracking::TimeSince(gameTime, latestUsageTime);

      // Calculate usage rate (stamina/sec)
      float windowSeconds = 2.0f;
      newState.usage.rate = totalUsage / windowSeconds;

      // Calculate aggregate regen values
      float totalRegen = 0.0f;
      float latestRegenTime = 0.0f;

      for (const auto& event : newState.regen.history) {
      float eventAge = gameTime - event.timestamp;
      if (eventAge <= VitalTracking::ACTIVE_WINDOW_DAYS) {
        float weight = std::exp(-VitalTracking::EXPONENTIAL_DECAY_CONSTANT * eventAge / VitalTracking::ACTIVE_WINDOW_DAYS);
        totalRegen += event.amount * weight;
      }
      if (event.timestamp > latestRegenTime) {
        latestRegenTime = event.timestamp;
      }
      }

      newState.regen.recentAmount = totalRegen;

      // Calculate time since last regen
      newState.regen.timeSinceLast = VitalTracking::TimeSince(gameTime, latestRegenTime);

      // Calculate regen rate
      newState.regen.rate = totalRegen / windowSeconds;

      // Detect usage trend (increasing/decreasing)
      float usageRateChange = newState.usage.rate - m_previousStaminaUsageRate;
      newState.usage.isIncreasing = usageRateChange > VitalTracking::STAMINA_TREND_THRESHOLD;
      newState.usage.isDecreasing = usageRateChange < -VitalTracking::STAMINA_TREND_THRESHOLD;

      // Stage 3b: Update state with change detection
      {
      std::unique_lock lock(m_playerMutex);
      bool changed = (m_staminaTracking != newState);
      if (changed) {
        m_staminaTracking = newState;
#ifdef _DEBUG
        if (newState.usage.IsActive(2.0f)) {
           logger::trace("[StateManager] StaminaTracking: usageRate={:.1f}/s, regenRate={:.1f}/s, net={:.1f}/s",
            newState.usage.rate, newState.regen.rate, newState.GetNetStaminaChange());
        }
#endif
      }

      // Update previous values for next poll
      m_previousStamina = currentStamina;
      m_previousStaminaUsageRate = newState.usage.rate;
      return changed;
      }
   }

} // namespace Huginn::State
