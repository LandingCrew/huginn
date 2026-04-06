// =============================================================================
// StateManager_HealthTracking.cpp - Health tracking polling (v0.6.9)
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: damage/healing detection and rate calculation
// Updates: HealthTrackingState
// Renamed from StateManager_Damage.cpp for consistency with other vital trackers.
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "DamageEventSink.h"
#include "../Profiling.h"

namespace Huginn::State
{
   // =============================================================================
   // HEALTH TRACKING POLLING (v0.6.2, v0.6.9 - Renamed)
   // =============================================================================
   // Tracks player damage and healing events for ward/shield recommendations.
   // Uses health delta detection and active effect classification.
   // =============================================================================

   bool StateManager::PollHealthTracking()
   {
      Huginn_ZONE_NAMED("PollHealthTracking");
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return false;
      }

      auto* actorValueOwner = player->AsActorValueOwner();
      if (!actorValueOwner) {
      return false;
      }

      // Get current game time (total elapsed days, monotonically increasing)
      // NOTE: Use GetCurrentGameTime() not GetHour() to avoid midnight wraparound bug
      // GetHour() returns 0-24 and wraps, GetCurrentGameTime() returns total days
      float gameTime = 0.0f;
      auto* calendar = RE::Calendar::GetSingleton();
      if (calendar) {
      gameTime = calendar->GetCurrentGameTime();
      }

      // Get current health value
      float currentHealth = actorValueOwner->GetActorValue(RE::ActorValue::kHealth);

      // Initialize previous health on first poll
      if (m_healthTracker.previousValue < 0.0f) {
      m_healthTracker.previousValue = currentHealth;
      return false;
      }

      // Calculate health delta
      float healthDelta = currentHealth - m_healthTracker.previousValue;

      // v0.6.8: Drain queued TESHitEvent damage types early to avoid stale accumulation
      // These events capture instant damage types that ActiveEffect polling misses
      auto queuedHitEvents = DamageEventSink::GetSingleton().DrainQueue();

      // Build new health tracking state
      HealthTrackingState newState;

      // Copy existing history (will prune old events below)
      {
      std::shared_lock lock(m_trackingMutex);
      newState = m_healthTracking;
      }

      // v0.12.x: Accumulate sub-threshold health losses across ticks.
      // A 3 HP/sec poison deals ~0.3 HP per 100ms tick — below the 5.0 HP threshold.
      VitalTracking::UpdateAccumulator(m_healthTracker.accumulated, healthDelta);

      // Emit damage event when accumulated total crosses threshold
      if (m_healthTracker.accumulated >= VitalTracking::HEALTH_DAMAGE_THRESHOLD) {
      const float damageAmount = m_healthTracker.accumulated;

      // Classify damage type using dual-path detection:
      // 1. TESHitEvent (authoritative for instant damage — knows exact spell/enchant)
      // 2. ActiveEffect scan (fallback for DoTs with no hit event)
      // Priority: hit event first, then highest-magnitude active effect.
      DamageType damageType = DamageType::Physical;

      // Path 1: Check queued TESHitEvent (most reliable for instant damage)
      if (!queuedHitEvents.empty()) {
        const auto& latestEvent = queuedHitEvents.back();
        if (latestEvent.type != DamageType::Physical) {
           damageType = latestEvent.type;
           logger::trace("[StateManager] Used TESHitEvent for damage type: {}"sv,
            GetDamageTypeName(damageType));
        }
      }

      // Path 2: ActiveEffect scan (for DoTs — pick highest magnitude, not first match)
      if (damageType == DamageType::Physical) {
        float highestMagnitude = 0.0f;
        auto* magicTargetForDamage = player->AsMagicTarget();
        if (magicTargetForDamage) {
           auto* activeEffectsForDamage = magicTargetForDamage->GetActiveEffectList();
           if (activeEffectsForDamage) {
            for (const auto* effect : *activeEffectsForDamage) {
              if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kInactive)) {
                continue;
              }

              const auto* baseEffect = effect->GetBaseObject();
              if (!baseEffect || !baseEffect->IsDetrimental()) {
                continue;
              }

              const auto resistAV = baseEffect->data.resistVariable;
              DamageType effectType = DamageType::Physical;
              if (resistAV == RE::ActorValue::kResistFire) {
                effectType = DamageType::Fire;
              } else if (resistAV == RE::ActorValue::kResistFrost) {
                effectType = DamageType::Frost;
              } else if (resistAV == RE::ActorValue::kResistShock) {
                effectType = DamageType::Shock;
              } else if (resistAV == RE::ActorValue::kPoisonResist) {
                effectType = DamageType::Poison;
              } else if (resistAV == RE::ActorValue::kResistDisease) {
                effectType = DamageType::Disease;
              }

              if (effectType != DamageType::Physical) {
                float mag = std::abs(effect->magnitude);
                if (mag > highestMagnitude) {
                   highestMagnitude = mag;
                   damageType = effectType;
                }
              }
            }
           }
        }
      }

      // Create and record damage event with accumulated amount
      newState.damageHistory.push_back(DamageEvent(gameTime, damageAmount, damageType));
      m_healthTracker.accumulated = 0.0f;
      }
      else if (!queuedHitEvents.empty()) {
      // v0.12.x: Accumulated damage hasn't crossed threshold yet, but DamageEventSink
      // captured a typed hit event. Record it with zero magnitude so that per-type
      // timestamps (timeSinceLastFire, etc.) still update for enrichment.
      const auto& latestEvent = queuedHitEvents.back();
      if (latestEvent.type != DamageType::Physical && latestEvent.type != DamageType::Unknown) {
        newState.damageHistory.push_back(DamageEvent(gameTime, 0.0f, latestEvent.type));
        logger::debug("[StateManager] Sub-threshold elemental hit recorded: {} (accumulated={:.1f}, threshold={:.1f})"sv,
           GetDamageTypeName(latestEvent.type), m_healthTracker.accumulated, VitalTracking::HEALTH_DAMAGE_THRESHOLD);
      }
      }

      // Detect healing (health increased)
      if (healthDelta > VitalTracking::HEALTH_HEALING_THRESHOLD) {
      float healAmount = healthDelta;

      // Classify healing source by checking active effects
      HealingEvent::Source healSource = HealingEvent::Source::Unknown;

      // Check for active healing effects to classify source
      auto* magicTarget = player->AsMagicTarget();
      if (magicTarget) {
        auto* activeEffects = magicTarget->GetActiveEffectList();
        if (activeEffects) {
           for (auto* effect : *activeEffects) {
            if (!effect) continue;
            if (effect->flags.any(RE::ActiveEffect::Flag::kInactive)) continue;

            auto* baseEffect = effect->GetBaseObject();
            if (!baseEffect) continue;

            // Check if this is a healing effect (restores health)
            auto archetype = baseEffect->GetArchetype();
            auto primaryAV = baseEffect->data.primaryAV;

            if (primaryAV == RE::ActorValue::kHealth &&
                (archetype == RE::EffectSetting::Archetype::kValueModifier ||
                 archetype == RE::EffectSetting::Archetype::kPeakValueModifier) &&
                !baseEffect->IsDetrimental()) {

              // Classify by spell type
              auto* spell = effect->spell;
              if (spell) {
                auto spellType = spell->GetSpellType();
                if (spellType == RE::MagicSystem::SpellType::kPotion) {
                   healSource = HealingEvent::Source::Potion;
                   break;
                } else if (spellType == RE::MagicSystem::SpellType::kSpell) {
                   healSource = HealingEvent::Source::Spell;
                   break;
                }
              }
            }
           }
        }
      }

      // If no active healing effect found, it's natural regen
      if (healSource == HealingEvent::Source::Unknown) {
        healSource = HealingEvent::Source::NaturalRegen;
      }

      // Create and record healing event
      HealingEvent healEvent(gameTime, healAmount, healSource);
      newState.healingHistory.push_back(healEvent);
      }

      // Prune old events (older than HISTORY_RETENTION_DAYS)
      while (!newState.damageHistory.empty() &&
             (gameTime - newState.damageHistory.front().timestamp) > VitalTracking::HISTORY_RETENTION_DAYS) {
      newState.damageHistory.pop_front();
      }
      while (!newState.healingHistory.empty() &&
             (gameTime - newState.healingHistory.front().timestamp) > VitalTracking::HISTORY_RETENTION_DAYS) {
      newState.healingHistory.pop_front();
      }

      // Calculate aggregate damage values
      float totalDamage = 0.0f;
      float magicDamage = 0.0f;
      float latestDamageTime = 0.0f;

      // v0.6.7: Track per-type timestamps for reactive resist recommendations
      float latestFireTime = 0.0f;
      float latestFrostTime = 0.0f;
      float latestShockTime = 0.0f;
      float latestPoisonTime = 0.0f;
      DamageType latestType = DamageType::Unknown;

      for (const auto& event : newState.damageHistory) {
      float eventAge = gameTime - event.timestamp;
      if (eventAge <= VitalTracking::ACTIVE_WINDOW_DAYS) {
        // Exponential decay weighting (recent events weighted more heavily)
        float weight = std::exp(-VitalTracking::EXPONENTIAL_DECAY_CONSTANT * eventAge / VitalTracking::ACTIVE_WINDOW_DAYS);
        totalDamage += event.amount * weight;
        if (event.WasMagic()) {
           magicDamage += event.amount * weight;
        }
      }
      if (event.timestamp > latestDamageTime) {
        latestDamageTime = event.timestamp;
        latestType = event.type;  // Track most recent damage type
      }

      // Track per-type timestamps
      switch (event.type) {
        case DamageType::Fire:
           latestFireTime = std::max(latestFireTime, event.timestamp);
           break;
        case DamageType::Frost:
           latestFrostTime = std::max(latestFrostTime, event.timestamp);
           break;
        case DamageType::Shock:
           latestShockTime = std::max(latestShockTime, event.timestamp);
           break;
        case DamageType::Poison:
           latestPoisonTime = std::max(latestPoisonTime, event.timestamp);
           break;
        default:
           break;
      }
      }

      newState.recentDamageTaken = totalDamage;
      newState.takingMagicDamage = magicDamage > 0.0f;
      newState.magicDamagePercent = (totalDamage > 0.0f) ? (magicDamage / totalDamage) : 0.0f;

      // Calculate time since last hit (convert game-time days to real seconds)
      newState.timeSinceLastHit = VitalTracking::TimeSince(gameTime, latestDamageTime);

      // v0.6.7: Track last damage type and per-type timestamps
      newState.lastDamageType = latestType;
      newState.timeSinceLastFire = VitalTracking::TimeSince(gameTime, latestFireTime);
      newState.timeSinceLastFrost = VitalTracking::TimeSince(gameTime, latestFrostTime);
      newState.timeSinceLastShock = VitalTracking::TimeSince(gameTime, latestShockTime);
      newState.timeSinceLastPoison = VitalTracking::TimeSince(gameTime, latestPoisonTime);

      // Calculate damage rate (HP/sec, based on recent window)
      // Active damage window is 2 real seconds = 2/180 game hours
      float damageWindowSeconds = 2.0f;
      newState.damageRate = totalDamage / damageWindowSeconds;

      // Calculate aggregate healing values
      float totalHealing = 0.0f;
      float potionHealing = 0.0f;
      float spellHealing = 0.0f;
      float regenHealing = 0.0f;
      float latestHealTime = 0.0f;

      for (const auto& event : newState.healingHistory) {
      float eventAge = gameTime - event.timestamp;
      if (eventAge <= VitalTracking::ACTIVE_WINDOW_DAYS) {
        float weight = std::exp(-VitalTracking::EXPONENTIAL_DECAY_CONSTANT * eventAge / VitalTracking::ACTIVE_WINDOW_DAYS);
        totalHealing += event.amount * weight;

        switch (event.source) {
           case HealingEvent::Source::Potion:
            potionHealing += event.amount * weight;
            break;
           case HealingEvent::Source::Spell:
            spellHealing += event.amount * weight;
            break;
           case HealingEvent::Source::NaturalRegen:
            regenHealing += event.amount * weight;
            break;
           default:
            break;
        }
      }
      if (event.timestamp > latestHealTime) {
        latestHealTime = event.timestamp;
      }
      }

      newState.recentHealingReceived = totalHealing;
      newState.potionHealingPercent = (totalHealing > 0.0f) ? (potionHealing / totalHealing) : 0.0f;
      newState.spellHealingPercent = (totalHealing > 0.0f) ? (spellHealing / totalHealing) : 0.0f;
      newState.naturalRegenPercent = (totalHealing > 0.0f) ? (regenHealing / totalHealing) : 0.0f;

      // Calculate time since last heal (convert game-time days to real seconds)
      newState.timeSinceLastHeal = VitalTracking::TimeSince(gameTime, latestHealTime);

      // Calculate healing rate
      newState.healingRate = totalHealing / damageWindowSeconds;

      // Detect damage trend (increasing/decreasing)
      float damageRateChange = newState.damageRate - m_healthTracker.previousRate;
      newState.damageIncreasing = damageRateChange > VitalTracking::TREND_CHANGE_THRESHOLD;
      newState.damageDecreasing = damageRateChange < -VitalTracking::TREND_CHANGE_THRESHOLD;

      // Detect healing trend
      float healingRateChange = newState.healingRate - m_healthTracker.previousSecondaryRate;
      newState.healingIncreasing = healingRateChange > VitalTracking::TREND_CHANGE_THRESHOLD;
      newState.healingDecreasing = healingRateChange < -VitalTracking::TREND_CHANGE_THRESHOLD;

      // Update state with change detection
      {
      std::unique_lock lock(m_trackingMutex);
      bool changed = (m_healthTracking != newState);
      if (changed) {
        m_healthTracking = newState;
#ifdef _DEBUG
        if (newState.IsTakingDamage()) {
           logger::trace("[StateManager] HealthTracking: rate={:.1f} HP/s, magic={:.0f}%, timeSinceHit={:.1f}s",
            newState.damageRate, newState.magicDamagePercent * 100.0f, newState.timeSinceLastHit);
        }
        if (newState.IsActivelyHealing()) {
           logger::trace("[StateManager] HealthTracking: healingRate={:.1f} HP/s, timeSinceHeal={:.1f}s",
            newState.healingRate, newState.timeSinceLastHeal);
        }
#endif
      }

      // Update previous values for next poll (inside lock to prevent data races)
      m_healthTracker.previousValue = currentHealth;
      m_healthTracker.previousRate = newState.damageRate;
      m_healthTracker.previousSecondaryRate = newState.healingRate;
      return changed;
      }
   }

} // namespace Huginn::State
