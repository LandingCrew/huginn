// =============================================================================
// StateManager_MagickaTracking.cpp - Magicka tracking polling (v0.6.9)
// =============================================================================
// Part of StateManager implementation split
// Polls: magicka usage/regen detection and rate calculation
// Updates: MagickaTrackingState
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   // =============================================================================
   // MAGICKA SOURCE CLASSIFICATION (v0.6.9)
   // =============================================================================
   // Classifies magicka usage source based on player casting state.
   // Returns the source type and sets outSpellID to the spell being cast.
   // =============================================================================

   MagickaUsageSource StateManager::ClassifyMagickaUsage(RE::PlayerCharacter* player, RE::FormID& outSpellID) noexcept
   {
      if (!player) {
      outSpellID = 0;
      return MagickaUsageSource::Unknown;
      }

      // Helper lambda to check a single caster
      auto checkCaster = [&outSpellID](RE::MagicCaster* caster) -> MagickaUsageSource {
      if (!caster || !caster->currentSpell) {
        return MagickaUsageSource::Unknown;
      }

      outSpellID = caster->currentSpell->GetFormID();
      auto castType = caster->currentSpell->GetCastingType();

      if (castType == RE::MagicSystem::CastingType::kConcentration) {
        // Check for ward by name (no kWard archetype exists in CommonLibSSE-NG)
        // Wards are Restoration concentration spells that absorb damage
        const char* spellName = caster->currentSpell->GetName();
        if (spellName) {
           std::string nameLower(spellName);
           // Case-insensitive search via std::transform (clearer intent than raw loop)
           std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
           if (nameLower.find("ward") != std::string::npos) {
            return MagickaUsageSource::Ward;
           }
        }
        return MagickaUsageSource::Concentration;
      }

      return MagickaUsageSource::SpellCast;
      };

      // Check left hand caster
      auto* casterL = player->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand);
      if (auto src = checkCaster(casterL); src != MagickaUsageSource::Unknown) {
      return src;
      }

      // Check right hand caster
      auto* casterR = player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
      if (auto src = checkCaster(casterR); src != MagickaUsageSource::Unknown) {
      return src;
      }

      outSpellID = 0;
      return MagickaUsageSource::Unknown;
   }

   // =============================================================================
   // MAGICKA TRACKING POLLING (v0.6.9)
   // =============================================================================
   // Tracks player magicka usage and regeneration events for magicka potion
   // and restore magicka spell recommendations.
   // Uses magicka delta detection and casting state classification.
   // =============================================================================

   bool StateManager::PollMagickaTracking()
   {
      Huginn_ZONE_NAMED("PollMagickaTracking");
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

      // Get current magicka value
      float currentMagicka = actorValueOwner->GetActorValue(RE::ActorValue::kMagicka);

      // Initialize previous magicka on first poll
      if (m_previousMagicka < 0.0f) {
      m_previousMagicka = currentMagicka;
      return false;
      }

      // Calculate magicka delta
      float magickaDelta = currentMagicka - m_previousMagicka;

      // Build new tracking state
      MagickaTrackingState newState;

      // Copy existing history (will prune old events below)
      {
      std::shared_lock lock(m_playerMutex);
      newState = m_magickaTracking;
      }

      // Check current casting state for channeling/ward detection
      RE::FormID currentSpellID = 0;
      MagickaUsageSource currentSource = ClassifyMagickaUsage(player, currentSpellID);
      newState.isChanneling = (currentSource == MagickaUsageSource::Concentration);
      newState.isHoldingWard = (currentSource == MagickaUsageSource::Ward);

      // v0.12.x: Accumulate sub-threshold magicka losses across ticks.
      VitalTracking::UpdateAccumulator(m_accumulatedMagickaUsage, magickaDelta);

      // Emit usage event when accumulated total crosses threshold
      if (m_accumulatedMagickaUsage >= VitalTracking::MAGICKA_USAGE_THRESHOLD) {
      float usageAmount = m_accumulatedMagickaUsage;
      RE::FormID spellID = 0;
      MagickaUsageSource source = ClassifyMagickaUsage(player, spellID);

      MagickaUsageEvent event(gameTime, usageAmount, source, spellID);
      newState.usage.history.push_back(event);
      m_accumulatedMagickaUsage = 0.0f;
      }

      // Detect regen (magicka increased)
      if (magickaDelta > VitalTracking::REGEN_THRESHOLD) {
      MagickaRegenEvent event(gameTime, magickaDelta);
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
      float instantCastUsage = 0.0f;
      float concentrationUsage = 0.0f;
      float latestUsageTime = 0.0f;

      for (const auto& event : newState.usage.history) {
      float eventAge = gameTime - event.timestamp;
      if (eventAge <= VitalTracking::ACTIVE_WINDOW_DAYS) {
        // Exponential decay weighting
        float weight = std::exp(-VitalTracking::EXPONENTIAL_DECAY_CONSTANT * eventAge / VitalTracking::ACTIVE_WINDOW_DAYS);
        totalUsage += event.amount * weight;

        switch (event.source) {
           case MagickaUsageSource::SpellCast:
            instantCastUsage += event.amount * weight;
            break;
           case MagickaUsageSource::Concentration:
           case MagickaUsageSource::Ward:
            concentrationUsage += event.amount * weight;
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
      newState.instantCastPercent = (totalUsage > 0.0f) ? (instantCastUsage / totalUsage) : 0.0f;
      newState.concentrationPercent = (totalUsage > 0.0f) ? (concentrationUsage / totalUsage) : 0.0f;

      // Calculate time since last usage
      newState.usage.timeSinceLast = VitalTracking::TimeSince(gameTime, latestUsageTime);

      // Calculate usage rate (magicka/sec)
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
      float usageRateChange = newState.usage.rate - m_previousMagickaUsageRate;
      newState.usage.isIncreasing = usageRateChange > VitalTracking::MAGICKA_TREND_THRESHOLD;
      newState.usage.isDecreasing = usageRateChange < -VitalTracking::MAGICKA_TREND_THRESHOLD;

      // Update state with change detection
      {
      std::unique_lock lock(m_playerMutex);
      bool changed = (m_magickaTracking != newState);
      if (changed) {
        m_magickaTracking = newState;
#ifdef _DEBUG
        if (newState.IsActivelyCasting()) {
           logger::trace("[StateManager] MagickaTracking: usageRate={:.1f}/s, regenRate={:.1f}/s, channeling={}, ward={}",
            newState.usage.rate, newState.regen.rate, newState.isChanneling, newState.isHoldingWard);
        }
#endif
      }

      // Update previous values for next poll
      m_previousMagicka = currentMagicka;
      m_previousMagickaUsageRate = newState.usage.rate;
      return changed;
      }
   }

} // namespace Huginn::State
