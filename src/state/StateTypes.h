#pragma once

#include "StateConstants.h"
#include "core/RingBuffer.h"
#include <array>
#include <cmath>
#include <cstdint>

// =============================================================================
// STATE TYPES
// =============================================================================
// This file contains shared types used by state models and debug widgets.
// Migrated from context/SensorState.h in v0.6.x refactor.
// =============================================================================

namespace Huginn::State
{
   // =============================================================================
   // EFFECT TYPE ENUMS
   // =============================================================================

   // Effect type classification for cloaks and similar effects
   enum class EffectType : uint8_t
   {
      None = 0,
      CloakFire = 1,
      CloakFrost = 2,
      CloakShock = 3
   };

   // NOTE: TargetSource enum is defined in TargetActorState.h with more values

   // =============================================================================
   // EVENT RING BUFFER - Compatibility alias (deprecated, do not use in new code)
   // =============================================================================
   // Kept so existing state model code compiles unchanged after extraction of
   // RingBuffer to core/RingBuffer.h. New code should use Huginn::RingBuffer<T, N>
   // directly from that header. This alias will be removed once all call sites
   // are migrated.
   // =============================================================================
   template<typename T, size_t Capacity>
   using EventRingBuffer = Huginn::RingBuffer<T, Capacity>;

   // =============================================================================
   // DAMAGE TYPE CLASSIFICATION (v0.6.7)
   // =============================================================================
   enum class DamageType : uint8_t
   {
      Physical = 0,  // Melee, arrows, fall damage (no magic resist)
      Fire,          // Fire spells, dragon breath, fire traps
      Frost,         // Frost spells, ice traps
      Shock,         // Shock spells, lightning traps
      Poison,        // Poison damage over time
      Disease,       // Disease damage (rare, future use)
      Magic,         // Generic magic damage (no specific element)
      Unknown        // Unable to classify
   };

   constexpr const char* GetDamageTypeName(DamageType type) noexcept
   {
      switch (type) {
      case DamageType::Physical: return "Physical";
      case DamageType::Fire: return "Fire";
      case DamageType::Frost: return "Frost";
      case DamageType::Shock: return "Shock";
      case DamageType::Poison: return "Poison";
      case DamageType::Disease: return "Disease";
      case DamageType::Magic: return "Magic";
      default: return "Unknown";
      }
   }

   // =============================================================================
   // DAMAGE EVENT (v0.5.0 - Phase 4, v0.6.7 - Specific type tracking)
   // =============================================================================
   struct DamageEvent
   {
      float timestamp = 0.0f;       // Game time when damage occurred (in-game hours from Calendar)
      float amount = 0.0f;          // Absolute HP damage value
      DamageType type = DamageType::Physical;  // Specific damage type (v0.6.7)

      constexpr DamageEvent() noexcept = default;
      constexpr DamageEvent(float ts, float dmg, DamageType t) noexcept
      : timestamp(ts), amount(dmg), type(t) {}

      // Backward compatibility helper for code that only needs magic vs. physical distinction.
      // Returns true for Fire/Frost/Shock/Poison/Disease/Magic, false for Physical/Unknown.
      // New code should check 'type' directly for specific damage classification.
      [[nodiscard]] constexpr bool WasMagic() const noexcept
      {
      return type != DamageType::Physical && type != DamageType::Unknown;
      }
   };

   // =============================================================================
   // HEALING EVENT (v0.5.0 - Phase 4.5)
   // =============================================================================
   struct HealingEvent
   {
      // Healing source classification
      enum class Source : uint8_t
      {
      Unknown = 0,      // Unable to determine source
      Potion,           // Healing potion consumed
      Spell,            // Healing spell cast (Restoration)
      NaturalRegen      // Natural health regeneration (not from active effect)
      };

      float timestamp = 0.0f;     // Game time when healing occurred (in-game hours from Calendar)
      float amount = 0.0f;        // Absolute HP healing value
      Source source = Source::Unknown;

      constexpr HealingEvent() noexcept = default;
      constexpr HealingEvent(float ts, float amt, Source src) noexcept
      : timestamp(ts), amount(amt), source(src) {}
   };

   // =============================================================================
   // RESOURCE FLOW TRACKER (v0.6.9 - Abstracted from HealthTrackingState)
   // =============================================================================
   // Generic tracker for resource consumption/recovery with exponential decay.
   // Used by: HealthTrackingState (future refactor), StaminaTrackingState, MagickaTrackingState
   // =============================================================================
   template<typename EventType, size_t MaxEvents = 10>
   struct ResourceFlowTracker
   {
      // Event history ring buffer
      EventRingBuffer<EventType, MaxEvents> history;

      // Aggregate values (with exponential decay)
      float recentAmount = 0.0f;      // Total in active window
      float rate = 0.0f;              // Units per second
      float timeSinceLast = 99.0f;    // Seconds since last event (99 = sentinel)

      // Trend detection
      bool isIncreasing = false;      // Rate accelerating
      bool isDecreasing = false;      // Rate decelerating

      // Helper methods
      [[nodiscard]] bool IsActive(float threshold = 2.0f) const noexcept {
      return timeSinceLast < threshold;
      }

      [[nodiscard]] bool IsHeavy(float rateThreshold) const noexcept {
      return rate > rateThreshold;
      }

      bool operator==(const ResourceFlowTracker& other) const {
      return std::abs(recentAmount - other.recentAmount) < Epsilon::VITAL_PERCENTAGE &&
             std::abs(rate - other.rate) < 3.0f &&
             std::abs(timeSinceLast - other.timeSinceLast) < 0.2f &&
             isIncreasing == other.isIncreasing &&
             isDecreasing == other.isDecreasing;
      }
   };

   // =============================================================================
   // STAMINA USAGE SOURCE (v0.6.9)
   // =============================================================================
   enum class StaminaUsageSource : uint8_t
   {
      Unknown = 0,
      PowerAttack,    // Heavy melee swing
      Sprint,         // Running
      Block,          // Shield block
      Jump,           // Jumping
      ShieldBash,     // Offensive bash
      Swimming        // Swimming movement
   };

   // =============================================================================
   // STAMINA USAGE EVENT (v0.6.9)
   // =============================================================================
   struct StaminaUsageEvent
   {
      float timestamp = 0.0f;
      float amount = 0.0f;
      StaminaUsageSource source = StaminaUsageSource::Unknown;

      constexpr StaminaUsageEvent() noexcept = default;
      constexpr StaminaUsageEvent(float ts, float amt, StaminaUsageSource src) noexcept
      : timestamp(ts), amount(amt), source(src) {}
   };

   // =============================================================================
   // STAMINA REGEN EVENT (v0.6.9) - Simple amount-only event
   // =============================================================================
   struct StaminaRegenEvent
   {
      float timestamp = 0.0f;
      float amount = 0.0f;

      constexpr StaminaRegenEvent() noexcept = default;
      constexpr StaminaRegenEvent(float ts, float amt) noexcept
      : timestamp(ts), amount(amt) {}
   };

   // =============================================================================
   // STAMINA TRACKING STATE (v0.6.9)
   // =============================================================================
   struct StaminaTrackingState
   {
      // Core tracking using abstracted component
      ResourceFlowTracker<StaminaUsageEvent> usage;
      ResourceFlowTracker<StaminaRegenEvent> regen;

      // Stamina-specific: source percentages
      float powerAttackPercent = 0.0f;
      float sprintPercent = 0.0f;

      // Helper methods
      [[nodiscard]] bool IsUsingStaminaHeavily() const noexcept {
      return usage.IsHeavy(10.0f);  // >10/sec
      }

      [[nodiscard]] bool IsStaminaDraining() const noexcept {
      return usage.rate > regen.rate + 5.0f;
      }

      [[nodiscard]] float GetNetStaminaChange() const noexcept {
      return regen.rate - usage.rate;
      }

      bool operator==(const StaminaTrackingState& other) const {
      return usage == other.usage &&
             regen == other.regen &&
             std::abs(powerAttackPercent - other.powerAttackPercent) < 0.05f &&
             std::abs(sprintPercent - other.sprintPercent) < 0.05f;
      }
   };

   // =============================================================================
   // MAGICKA USAGE SOURCE (v0.6.9)
   // =============================================================================
   enum class MagickaUsageSource : uint8_t
   {
      Unknown = 0,
      SpellCast,      // Instant cast
      Concentration,  // Sustained spell (flames, healing)
      Ward,           // Ward maintenance
      Staff           // Staff discharge
   };

   // =============================================================================
   // MAGICKA USAGE EVENT (v0.6.9)
   // =============================================================================
   struct MagickaUsageEvent
   {
      float timestamp = 0.0f;
      float amount = 0.0f;
      MagickaUsageSource source = MagickaUsageSource::Unknown;
      RE::FormID spellFormID = 0;  // For learning correlation

      constexpr MagickaUsageEvent() noexcept = default;
      constexpr MagickaUsageEvent(float ts, float amt, MagickaUsageSource src, RE::FormID spell = 0) noexcept
      : timestamp(ts), amount(amt), source(src), spellFormID(spell) {}
   };

   // =============================================================================
   // MAGICKA REGEN EVENT (v0.6.9)
   // =============================================================================
   struct MagickaRegenEvent
   {
      float timestamp = 0.0f;
      float amount = 0.0f;

      constexpr MagickaRegenEvent() noexcept = default;
      constexpr MagickaRegenEvent(float ts, float amt) noexcept
      : timestamp(ts), amount(amt) {}
   };

   // =============================================================================
   // MAGICKA TRACKING STATE (v0.6.9)
   // =============================================================================
   struct MagickaTrackingState
   {
      // Core tracking using abstracted component
      ResourceFlowTracker<MagickaUsageEvent> usage;
      ResourceFlowTracker<MagickaRegenEvent> regen;

      // Magicka-specific: casting state
      bool isChanneling = false;      // Concentration spell active
      bool isHoldingWard = false;     // Ward being maintained

      // Source percentages
      float instantCastPercent = 0.0f;
      float concentrationPercent = 0.0f;

      // Helper methods
      [[nodiscard]] bool IsUsingMagickaHeavily() const noexcept {
      return usage.IsHeavy(15.0f);  // >15/sec
      }

      [[nodiscard]] bool IsMagickaDraining() const noexcept {
      return usage.rate > regen.rate + 5.0f;
      }

      [[nodiscard]] bool IsActivelyCasting() const noexcept {
      return usage.IsActive(3.0f) || isChanneling || isHoldingWard;
      }

      [[nodiscard]] float GetNetMagickaChange() const noexcept {
      return regen.rate - usage.rate;
      }

      bool operator==(const MagickaTrackingState& other) const {
      return usage == other.usage &&
             regen == other.regen &&
             isChanneling == other.isChanneling &&
             isHoldingWard == other.isHoldingWard &&
             std::abs(instantCastPercent - other.instantCastPercent) < 0.05f &&
             std::abs(concentrationPercent - other.concentrationPercent) < 0.05f;
      }
   };

   // =============================================================================
   // HEALTH TRACKING STATE (v0.5.0 - Phase 4, v0.6.0 - Thread Safety, v0.6.9 - Renamed)
   // =============================================================================
   // Tracks recent damage taken and healing received by player with exponential decay.
   // Used for ward/shield spell weight calculations and healing recommendations.
   // Renamed from HealthTrackingState for consistency with StaminaTrackingState/MagickaTrackingState.
   //
   // THREAD SAFETY (v0.6.0):
   // - Uses EventRingBuffer instead of std::deque for trivially copyable state
   // - Safe to copy entire struct without synchronization (no heap pointers)
   // =============================================================================
   struct HealthTrackingState
   {
      // Ring buffer of recent damage events (max 10 events or 5 seconds)
      // Uses fixed-size circular buffer for thread-safe copying
      EventRingBuffer<DamageEvent, 10> damageHistory;

      // Aggregate values (calculated from damageHistory)
      float recentDamageTaken = 0.0f;      // Total damage in last 2 seconds (with exponential decay)
      float magicDamagePercent = 0.0f;   // Percentage of recent damage that was magic (0.0-1.0)
      bool takingMagicDamage = false;      // True if any recent damage was magic
      float damageRate = 0.0f;      // HP/sec damage rate (weighted average)
      float timeSinceLastHit = 99.0f;      // Seconds since last damage event (99 = not hit recently)

      // === Phase 6.7: Last damage type tracking ===
      DamageType lastDamageType = DamageType::Unknown;  // Most recent damage type received
      float timeSinceLastFire = 99.0f;    // Real seconds since last fire damage
      float timeSinceLastFrost = 99.0f;   // Real seconds since last frost damage
      float timeSinceLastShock = 99.0f;   // Real seconds since last shock damage
      float timeSinceLastPoison = 99.0f;  // Real seconds since last poison damage

      // Damage trend tracking (for future panic/retreat mechanics)
      bool damageIncreasing = false;      // True if damage rate is accelerating
      bool damageDecreasing = false;      // True if damage rate is decelerating

      // === Phase 4.5: Healing Tracking ===

      // Ring buffer of recent healing events (max 10 events or 5 seconds)
      // Uses fixed-size circular buffer for thread-safe copying
      EventRingBuffer<HealingEvent, 10> healingHistory;

      // Aggregate healing values (calculated from healingHistory)
      float recentHealingReceived = 0.0f;      // Total healing in last 2 seconds (with exponential decay)
      float potionHealingPercent = 0.0f;      // Percentage from potions (0.0-1.0)
      float spellHealingPercent = 0.0f;      // Percentage from spells (0.0-1.0)
      float naturalRegenPercent = 0.0f;      // Percentage from natural regen (0.0-1.0)
      float healingRate = 0.0f;        // HP/sec healing rate (weighted average)
      float timeSinceLastHeal = 99.0f;      // Seconds since last healing event (99 = not healing recently)

      // Healing trend tracking
      bool healingIncreasing = false;      // True if healing rate is accelerating
      bool healingDecreasing = false;      // True if healing rate is decelerating

      // Helper methods
      [[nodiscard]] bool IsTakingDamage() const noexcept
      {
      return timeSinceLastHit < 2.0f;
      }

      [[nodiscard]] bool IsUnderSustainedAttack() const noexcept
      {
      return damageRate > 5.0f;  // Losing >5 HP/sec
      }

      [[nodiscard]] bool IsCriticalDamage() const noexcept
      {
      return damageRate > 20.0f;  // Losing >20 HP/sec (1/5 of base health)
      }

      // Healing state helpers
      [[nodiscard]] bool IsActivelyHealing() const noexcept
      {
      return timeSinceLastHeal < 2.0f && healingRate > 1.0f;
      }

      [[nodiscard]] float GetNetHealthChange() const noexcept
      {
      return healingRate - damageRate;  // Positive = net healing, Negative = net damage
      }

      [[nodiscard]] bool IsLosingHealthFast() const noexcept
      {
      // True if taking more damage than healing can sustain
      return damageRate > healingRate + 10.0f;  // Losing >10 HP/s net
      }

      [[nodiscard]] bool IsHealingRedundant() const noexcept
      {
      // True if already healing faster than current damage
      return IsActivelyHealing() && healingRate > damageRate * 1.5f;
      }

      // Check if specific damage type was received recently (v0.6.7)
      // Only Fire/Frost/Shock/Poison have dedicated per-type tracking timestamps.
      // Other types (Physical, Disease, Magic, Unknown) can use lastDamageType + timeSinceLastHit.
      [[nodiscard]] bool TookDamageTypeRecently(DamageType type, float withinSeconds = 5.0f) const noexcept
      {
      switch (type) {
        case DamageType::Fire:
           return timeSinceLastFire < withinSeconds;
        case DamageType::Frost:
           return timeSinceLastFrost < withinSeconds;
        case DamageType::Shock:
           return timeSinceLastShock < withinSeconds;
        case DamageType::Poison:
           return timeSinceLastPoison < withinSeconds;
        case DamageType::Physical:
        case DamageType::Disease:
        case DamageType::Magic:
        case DamageType::Unknown:
           // These types don't have per-type tracking; check lastDamageType instead
           return lastDamageType == type && timeSinceLastHit < withinSeconds;
      }
      return false;  // Unreachable, but satisfies compiler
      }

      // Diagnostic logging (Debug builds only)
#ifdef _DEBUG
      void LogDifferences(const HealthTrackingState& other) const
      {
      if (std::abs(recentDamageTaken - other.recentDamageTaken) >= Epsilon::VITAL_PERCENTAGE) {
        logger::trace("  HealthTrackingState.recentDamageTaken changed: {:.1f} -> {:.1f}"sv,
   other.recentDamageTaken, recentDamageTaken);
      }
      if (takingMagicDamage != other.takingMagicDamage) {
        logger::trace("  HealthTrackingState.takingMagicDamage changed: {} -> {}"sv,
   other.takingMagicDamage, takingMagicDamage);
      }
      // Phase 4.5: Healing logging
      if (std::abs(recentHealingReceived - other.recentHealingReceived) >= Epsilon::VITAL_PERCENTAGE) {
        logger::trace("  HealthTrackingState.recentHealingReceived changed: {:.1f} -> {:.1f}"sv,
   other.recentHealingReceived, recentHealingReceived);
      }
      if (std::abs(healingRate - other.healingRate) >= 3.0f) {
        logger::trace("  HealthTrackingState.healingRate changed: {:.1f} -> {:.1f} HP/s"sv,
   other.healingRate, healingRate);
      }
      if (IsActivelyHealing() != other.IsActivelyHealing()) {
        logger::trace("  HealthTrackingState.IsActivelyHealing changed: {} -> {}"sv,
   other.IsActivelyHealing(), IsActivelyHealing());
      }
      }
#endif

      bool operator==(const HealthTrackingState& other) const
      {
      bool equal = std::abs(recentDamageTaken - other.recentDamageTaken) < Epsilon::VITAL_PERCENTAGE &&
           std::abs(magicDamagePercent - other.magicDamagePercent) < Epsilon::VITAL_PERCENTAGE &&
        takingMagicDamage == other.takingMagicDamage &&
        damageIncreasing == other.damageIncreasing &&
        damageDecreasing == other.damageDecreasing &&
        std::abs(timeSinceLastHit - other.timeSinceLastHit) < 0.2f &&
        // v0.6.7: Last damage type tracking
        lastDamageType == other.lastDamageType &&
        std::abs(timeSinceLastFire - other.timeSinceLastFire) < 0.5f &&
        std::abs(timeSinceLastFrost - other.timeSinceLastFrost) < 0.5f &&
        std::abs(timeSinceLastShock - other.timeSinceLastShock) < 0.5f &&
        std::abs(timeSinceLastPoison - other.timeSinceLastPoison) < 0.5f &&
        // Phase 4.5: Healing fields
        std::abs(recentHealingReceived - other.recentHealingReceived) < Epsilon::VITAL_PERCENTAGE &&
        std::abs(potionHealingPercent - other.potionHealingPercent) < Epsilon::VITAL_PERCENTAGE &&
        std::abs(spellHealingPercent - other.spellHealingPercent) < Epsilon::VITAL_PERCENTAGE &&
        std::abs(naturalRegenPercent - other.naturalRegenPercent) < Epsilon::VITAL_PERCENTAGE &&
        std::abs(healingRate - other.healingRate) < 3.0f &&
        healingIncreasing == other.healingIncreasing &&
        healingDecreasing == other.healingDecreasing &&
        std::abs(timeSinceLastHeal - other.timeSinceLastHeal) < 0.2f;
#ifdef _DEBUG
      if (!equal) {
        logger::trace("HealthTrackingState changed:"sv);
        LogDifferences(other);
      }
#endif
      return equal;
      }
   };
}
