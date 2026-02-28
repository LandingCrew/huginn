#pragma once

#include <cstdint>

// =============================================================================
// STATE CONSTANTS
// =============================================================================
// This file contains all magic numbers used by StateManager and state models,
// organized by category with full documentation.
//
// Purpose: Replace hardcoded magic numbers with named constants to improve
// code readability, maintainability, and make tuning easier.
//
// Organization:
// - Each namespace groups related constants by their usage context
// - Each constant has a comment explaining what it represents, why the value
//   was chosen, and the units (if applicable)
//
// Note: Some constants like poll intervals are already defined in Config.h
// and should remain there (referenced but not duplicated here).
//
// History: Migrated from context/ContextSensorConstants.h in v0.6.x refactor
// =============================================================================

namespace Huginn::State
{
   // =============================================================================
   // EPSILON TOLERANCES - Float comparison thresholds to prevent log spam
   // =============================================================================
   namespace Epsilon
   {
      // Vital percentage comparison tolerance (health/magicka/stamina %)
      // Why 0.01f: 1% tolerance handles regen micro-changes at 50ms poll rate
      // without triggering state change spam. Balances responsiveness with stability.
      // Units: percentage (0.01 = 1%)
      inline constexpr float VITAL_PERCENTAGE = 0.01f;

      // Vital max value comparison tolerance (absolute HP/MP/SP values)
      // (v0.5.5: increased from 1.0 to 5.0)
      // Why 5.0f: Log analysis showed max values flickering by 1-2 points from expiring micro-buffs
      // (Fortify effects, food bonuses). 5.0 threshold ignores noise while detecting meaningful changes
      // (major buffs/debuffs, level-ups). Example: maxMagicka 199→198→197 oscillation now ignored.
      // Units: game units (health/magicka/stamina points)
      inline constexpr float VITAL_MAX_VALUE = 5.0f;

      // Target vital percentage tolerance (for crosshair target vitals)
      // Why 0.01f: Same as player vitals - 1% tolerance for target HP/MP/SP
      // Units: percentage (0.01 = 1%)
      inline constexpr float TARGET_VITAL_PERCENTAGE = 0.01f;

      // Target vital max value tolerance (absolute target HP/MP/SP values)
      // Why 1.0f: Same as player - whole-number changes only
      // Units: game units (health/magicka/stamina points)
      inline constexpr float TARGET_VITAL_MAX_VALUE = 1.0f;

      // Crosshair distance tolerance (v0.5.5: increased from 50.0 to 100.0)
      // Why 100.0f: NPCs move at ~200-300 units/sec. At 100ms poll rate, movement = 20-30 units/poll.
      // 100-unit epsilon = ~333ms of NPC movement before triggering state change.
      // Log analysis showed 50.0f caused 4.3 changes/sec when looking at moving actors.
      // 100.0f reduces this to ~1-2 changes/sec while still detecting meaningful distance changes.
      // Units: Skyrim distance units (roughly 1 unit ≈ 1 inch)
      inline constexpr float DISTANCE = 100.0f;

      // Equipment weapon charge tolerance
      // Why 0.02f: 2% tolerance handles charge drain rounding at 500ms poll rate
      // Prevents log spam from gradual weapon charge depletion
      // Units: percentage (0.02 = 2%)
      inline constexpr float WEAPON_CHARGE = 0.02f;

      // Environment height above ground tolerance
      // Why 25.0f: 25-unit tolerance handles terrain variations/character bobbing
      // when walking on uneven ground without triggering false "falling" states
      // Units: Skyrim distance units (vertical)
      inline constexpr float HEIGHT_ABOVE_GROUND = 25.0f;

      // Environment time of day tolerance
      // Why 0.2f: 0.2 hours = 12 in-game minutes. Prevents time-based state changes
      // from triggering every single game tick (useful for day/night transitions)
      // Units: hours (0.0-24.0 in-game time)
      inline constexpr float TIME_OF_DAY = 0.2f;

      // Environment light level tolerance
      // Why 0.049f: Half of the 0.1 quantization step used in light level calculation
      // Prevents boundary flickering when light level hovers at quantization edges
      // (e.g., 0.29 → 0.3 → 0.29 loop). Added in v0.5.2.
      // Units: percentage (0.0 = dark, 1.0 = bright)
      inline constexpr float LIGHT_LEVEL = 0.049f;
   }

   // =============================================================================
   // PHYSICS CONSTANTS - Height, distance, fall time conversions
   // =============================================================================
   namespace PhysicsConstants
   {
      // Height offset from actor position to head/eyes for underwater detection
      // Why 120.0f: Typical humanoid head height in Skyrim units above actor origin
      // Actor origin is at feet, so we add this to check if head is submerged
      // Units: Skyrim distance units (vertical offset)
      inline constexpr float HEAD_HEIGHT = 120.0f;

      // Approximate fall velocity (units per second)
      // Why 500.0f: Empirical estimate from testing Skyrim's fall speed
      // Used to convert fall timer (seconds) to estimated height above ground
      // Formula: height = fallTime * FALL_VELOCITY
      // Units: Skyrim distance units per second
      inline constexpr float FALL_VELOCITY = 500.0f;

      // Invalid water height sentinel value
      // Why -1000000.0f: Skyrim engine uses this sentinel value to indicate
      // "no water present" in a cell. Any value below this means no water check.
      // Must match RE::TESObjectCELL::GetExteriorWaterHeight() behavior.
      // Units: Skyrim distance units (Z-axis world position)
      inline constexpr float INVALID_WATER_HEIGHT_VALUE = -1000000.0f;
   }

   // =============================================================================
   // DISTANCE THRESHOLDS - Melee/Close/Mid/Ranged distance thresholds
   // =============================================================================
   // NOTE: Named DistanceThresholds (not DistanceBucket) to avoid collision with
   // the DistanceBucket enum in GameState.h
   namespace DistanceThresholds
   {
      // Melee range upper bound
      // Why 256.0f: Typical melee weapon range in Skyrim (sword/axe/mace)
      // Matches StateEvaluator::DistanceBucket::Melee upper bound
      // Units: Skyrim distance units
      inline constexpr float MELEE_MAX = 256.0f;

      // Close range lower bound
      // Why 256.0f: Starts where melee ends (no gap)
      // Units: Skyrim distance units
      inline constexpr float CLOSE_MIN = 256.0f;

      // Close range upper bound
      // Why 512.0f: Close combat range for spells/bows (still clearly visible)
      // Matches StateEvaluator::DistanceBucket::Close upper bound
      // Units: Skyrim distance units
      inline constexpr float CLOSE_MAX = 512.0f;

      // Mid range lower bound
      // Why 512.0f: Starts where close ends (no gap)
      // Units: Skyrim distance units
      inline constexpr float MID_MIN = 512.0f;

      // Mid range upper bound
      // Why 2048.0f: Mid-range combat for ranged spells/bows
      // Matches StateEvaluator::DistanceBucket::Mid upper bound
      // Units: Skyrim distance units
      inline constexpr float MID_MAX = 2048.0f;

      // Ranged distance lower bound
      // Why 2048.0f: Starts where mid ends (extreme range)
      // Matches StateEvaluator::DistanceBucket::Ranged lower bound
      // Units: Skyrim distance units
      inline constexpr float RANGED_MIN = 2048.0f;
   }

   // =============================================================================
   // VITAL THRESHOLDS - Health/Magicka/Stamina percentage thresholds
   // =============================================================================
   namespace VitalThreshold
   {
      // Player health/magicka/stamina "Critical" threshold
      // Why 0.15f: Below 15% is critically low - emergency healing needed
      // Used by IsHealthCritical(), IsMagickaCritical(), IsStaminaCritical()
      // Units: percentage (0.15 = 15%)
      inline constexpr float CRITICAL = 0.15f;

      // Player health/magicka/stamina "Low" threshold
      // Why 0.30f: Below 30% is low - should consider healing/restore
      // Used by IsHealthLow(), IsMagickaLow(), IsStaminaLow()
      // Units: percentage (0.30 = 30%)
      inline constexpr float LOW = 0.30f;

      // Player health "Medium" threshold (for state bucketing)
      // Why 0.50f: Below 50% triggers moderate healing recommendations
      // Used in HealthBucket classification in StateEvaluator
      // Units: percentage (0.50 = 50%)
      inline constexpr float MEDIUM = 0.50f;

      // Target health "Critical" threshold
      // Why 0.15f: Same as player - target is near death
      // Used for execute/finishing move recommendations
      // Units: percentage (0.15 = 15%)
      inline constexpr float TARGET_CRITICAL = 0.15f;

      // Target health "Very Low" threshold
      // Why 0.30f: Target is wounded - good for finishing spells
      // Units: percentage (0.30 = 30%)
      inline constexpr float TARGET_VERY_LOW = 0.30f;

      // Target health "Low" threshold
      // Why 0.50f: Target is weakened - opportunity for aggressive spells
      // Units: percentage (0.50 = 50%)
      inline constexpr float TARGET_LOW = 0.50f;

      // Target magicka/stamina "Low" threshold
      // Why 0.30f: Target is low on resources - opportunity for drain spells
      // Units: percentage (0.30 = 30%)
      inline constexpr float TARGET_RESOURCE_LOW = 0.30f;
   }

   // =============================================================================
   // LIGHT LEVEL THRESHOLDS - Light-related thresholds
   // =============================================================================
   namespace LightLevel
   {
      // Dark threshold (for stealth/dark vision)
      // Why 0.3f: Below 30% light level is considered "dark" for stealth purposes
      // Used by EnvironmentState::IsDark()
      // Units: percentage (0.0 = pitch black, 1.0 = full daylight)
      inline constexpr float DARK_THRESHOLD = 0.3f;

      // Well-lit threshold
      // Why 0.7f: Above 70% light level is considered "well lit" (bright)
      // Used by EnvironmentState::IsWellLit()
      // Units: percentage (0.0 = pitch black, 1.0 = full daylight)
      inline constexpr float WELL_LIT_THRESHOLD = 0.7f;

      // Light level quantization step
      // Why 10.0f: Quantize to 10% increments (0.0, 0.1, 0.2, ..., 1.0)
      // Reduces jitter from continuous time-of-day changes
      // Formula: std::round(rawLightLevel * 10.0f) / 10.0f
      // Units: multiplier for rounding
      inline constexpr float QUANTIZATION_MULTIPLIER = 10.0f;

      // Base light level for interiors
      // Why 0.5f: Default moderate light level for interior cells
      // Without cell-specific data, we assume interiors are moderately lit
      // Units: percentage (0.0 = pitch black, 1.0 = full daylight)
      inline constexpr float INTERIOR_DEFAULT = 0.5f;

      // Daytime start hour
      // Why 6.0f: 6am - start of daytime lighting calculations
      // Units: hours (0.0-24.0 in-game time)
      inline constexpr float DAYTIME_START = 6.0f;

      // Daytime end hour
      // Why 18.0f: 6pm - end of daytime lighting calculations
      // Units: hours (0.0-24.0 in-game time)
      inline constexpr float DAYTIME_END = 18.0f;

      // Noon hour (peak daylight)
      // Why 12.0f: 12pm - peak light level calculation reference
      // Units: hours (0.0-24.0 in-game time)
      inline constexpr float NOON = 12.0f;

      // Nighttime base light level
      // Why 0.2f: Dark at night but not pitch black (moon/stars)
      // Units: percentage (0.0 = pitch black, 1.0 = full daylight)
      inline constexpr float NIGHTTIME_BASE = 0.2f;

      // Night time threshold (for helper methods)
      // Why 6.0f: Before 6am is considered night
      // Used by EnvironmentState::IsNightTime()
      // Units: hours (0.0-24.0 in-game time)
      inline constexpr float NIGHT_END = 6.0f;

      // Night time threshold (for helper methods)
      // Why 20.0f: After 8pm is considered night
      // Used by EnvironmentState::IsNightTime()
      // Units: hours (0.0-24.0 in-game time)
      inline constexpr float NIGHT_START = 20.0f;
   }

   // =============================================================================
   // SURVIVAL MODE THRESHOLDS - Survival mode detection thresholds
   // =============================================================================
   namespace SurvivalThreshold
   {
      // Minimum penalty to detect survival mode is active
      // Why 0.1f: Penalties below 0.1 are likely noise/rounding errors
      // Any penalty ≥ 0.1 means survival mode has applied a debuff
      // Units: actor value penalty (health/magicka/stamina points)
      inline constexpr float MIN_PENALTY_DETECTION = 0.1f;

      // Hunger level: Well Fed
      // Why 0: Best state - no hunger penalty
      // Survival Mode Improved SKSE: "Well Fed" effect
      inline constexpr int HUNGER_WELL_FED = 0;

      // Hunger level: Fed
      // Why 1: Slightly hungry
      // Survival Mode Improved SKSE: "Fed" effect
      inline constexpr int HUNGER_FED = 1;

      // Hunger level: Peckish
      // Why 2: Moderately hungry
      // Survival Mode Improved SKSE: "Peckish" effect
      inline constexpr int HUNGER_PECKISH = 2;

      // Hunger level: Hungry
      // Why 3: Hungry - penalties starting
      // Survival Mode Improved SKSE: "Hungry" effect
      inline constexpr int HUNGER_HUNGRY = 3;

      // Hunger level: Famished
      // Why 4: Very hungry - significant penalties
      // Survival Mode Improved SKSE: "Famished" effect
      inline constexpr int HUNGER_FAMISHED = 4;

      // Hunger level: Starving (critical)
      // Why 5: Critical hunger - severe penalties
      // Survival Mode Improved SKSE: "Starving" effect
      // Helper method: IsStarving() checks >= 4 (works for vanilla CC too)
      inline constexpr int HUNGER_STARVING = 5;

      // Cold level: Warm
      // Why 0: Best state - no cold penalty
      // Survival Mode Improved SKSE: "Warm" effect
      inline constexpr int COLD_WARM = 0;

      // Cold level: Comfortable
      // Why 1: Slightly cold
      // Survival Mode Improved SKSE: "Comfortable" effect
      inline constexpr int COLD_COMFORTABLE = 1;

      // Cold level: Chilly
      // Why 2: Moderately cold
      // Survival Mode Improved SKSE: "Chilly" effect
      inline constexpr int COLD_CHILLY = 2;

      // Cold level: Very Cold
      // Why 3: Very cold - penalties starting
      // Survival Mode Improved SKSE: "Very Cold" effect
      inline constexpr int COLD_VERY_COLD = 3;

      // Cold level: Freezing
      // Why 4: Freezing - significant penalties
      // Survival Mode Improved SKSE: "Freezing" effect
      // Helper method: IsFreezing() checks >= 4 (works for vanilla CC too)
      inline constexpr int COLD_FREEZING = 4;

      // Cold level: Numb (critical)
      // Why 5: Critical cold - severe penalties
      // Survival Mode Improved SKSE: "Numb" effect
      inline constexpr int COLD_NUMB = 5;

      // Fatigue level: Well Rested (bonus)
      // Why -3: Best rested state - provides bonus
      // Survival Mode Improved SKSE: "Well Rested" effect
      inline constexpr int FATIGUE_WELL_RESTED = -3;

      // Fatigue level: Rested (bonus)
      // Why -2: Rested state - provides bonus
      // Survival Mode Improved SKSE: "Rested" effect
      inline constexpr int FATIGUE_RESTED = -2;

      // Fatigue level: Lover's Comfort (bonus)
      // Why -3: Same as Well Rested - special bonus
      // Survival Mode Improved SKSE: "Lover's Comfort" effect
      inline constexpr int FATIGUE_LOVERS_COMFORT = -3;

      // Fatigue level: Refreshed (neutral)
      // Why 0: Neutral state - no penalty or bonus
      // Survival Mode Improved SKSE: "Refreshed" effect
      inline constexpr int FATIGUE_REFRESHED = 0;

      // Fatigue level: Slightly Tired
      // Why 1: Slightly tired - minor penalty
      // Survival Mode Improved SKSE: "Slightly Tired" effect
      inline constexpr int FATIGUE_SLIGHTLY_TIRED = 1;

      // Fatigue level: Tired
      // Why 2: Tired - moderate penalty
      // Survival Mode Improved SKSE: "Tired" effect
      inline constexpr int FATIGUE_TIRED = 2;

      // Fatigue level: Weary
      // Why 3: Weary - significant penalty
      // Survival Mode Improved SKSE: "Weary" effect
      // Helper method: IsExhausted() checks >= 3 (works for vanilla CC too)
      inline constexpr int FATIGUE_WEARY = 3;

      // Fatigue level: Debilitated (critical)
      // Why 4: Critical fatigue - severe penalties
      // Survival Mode Improved SKSE: "Debilitated" effect
      inline constexpr int FATIGUE_DEBILITATED = 4;
   }

   // =============================================================================
   // LOCK LEVEL MAPPING - LOCK_LEVEL enum to integer mapping
   // =============================================================================
   namespace LockLevel
   {
      // Novice lock
      // Maps RE::LOCK_LEVEL::kVeryEasy → 0
      inline constexpr int NOVICE = 0;

      // Apprentice lock
      // Maps RE::LOCK_LEVEL::kEasy → 1
      inline constexpr int APPRENTICE = 1;

      // Adept lock
      // Maps RE::LOCK_LEVEL::kAverage → 2
      inline constexpr int ADEPT = 2;

      // Expert lock
      // Maps RE::LOCK_LEVEL::kHard → 3
      inline constexpr int EXPERT = 3;

      // Master lock
      // Maps RE::LOCK_LEVEL::kVeryHard → 4
      inline constexpr int MASTER = 4;

      // Requires Key
      // Maps RE::LOCK_LEVEL::kRequiresKey → 5
      inline constexpr int REQUIRES_KEY = 5;
   }

   // =============================================================================
   // PERCENTAGE MULTIPLIERS - For converting floats to percentages in logs
   // =============================================================================
   namespace Percentage
   {
      // Multiplier to convert 0.0-1.0 float to 0-100 percentage for logging
      // Why 100.0f: Standard percentage conversion
      // Example: 0.75f * 100.0f = 75.0f (for "75%" display)
      // Units: multiplier
      inline constexpr float TO_PERCENT = 100.0f;
   }

   // =============================================================================
   // CLAMP BOUNDS - Min/Max values for clamping percentages
   // =============================================================================
   namespace ClampBounds
   {
      // Minimum percentage value
      // Why 0.0f: Vital percentages cannot be negative
      // Units: percentage (0.0 = 0%)
      inline constexpr float MIN_PERCENTAGE = 0.0f;

      // Maximum percentage value
      // Why 1.0f: Vital percentages cannot exceed 100%
      // Units: percentage (1.0 = 100%)
      inline constexpr float MAX_PERCENTAGE = 1.0f;
   }

   // =============================================================================
   // DEFAULT STATE VALUES - Default values for state structs
   // =============================================================================
   namespace DefaultState
   {
      // Default vital percentage (full health/magicka/stamina)
      // Why 1.0f: Assume full vitals if no player/data available
      // Units: percentage (1.0 = 100%)
      inline constexpr float FULL_VITAL = 1.0f;

      // Default max vital values (fallback if no actor data)
      // Why 100.0f: Standard starting values in Skyrim
      // Units: game units (health/magicka/stamina points)
      inline constexpr float DEFAULT_MAX_VITAL = 100.0f;

      // Default time of day (noon)
      // Why 12.0f: Noon is a safe neutral time
      // Units: hours (0.0-24.0 in-game time)
      inline constexpr float NOON_TIME = 12.0f;

      // Default light level (well-lit)
      // Why 1.0f: Assume well-lit if no data available
      // Units: percentage (0.0 = dark, 1.0 = bright)
      inline constexpr float DEFAULT_LIGHT = 1.0f;

      // Default height above ground (on ground)
      // Why 0.0f: Assume on ground if no data available
      // Units: Skyrim distance units (vertical)
      inline constexpr float ON_GROUND = 0.0f;

      // Default distance to target (no target)
      // Why 0.0f: Zero means no target or unknown distance
      // Units: Skyrim distance units
      inline constexpr float NO_TARGET = 0.0f;

      // Default weapon charge (full charge)
      // Why 1.0f: Assume full charge if no weapon/data
      // Units: percentage (1.0 = 100%)
      inline constexpr float FULL_CHARGE = 1.0f;

      // Default arrow count (none)
      // Why 0: Assume no arrows if no bow equipped
      // Units: count
      // TYPE NOTE: int32_t matches RE::InventoryEntryData::countDelta to avoid narrowing conversion
      inline constexpr std::int32_t NO_ARROWS = 0;

      // Default survival level (neutral/none)
      // Why 0: Neutral state for all survival stats
      inline constexpr int NEUTRAL_SURVIVAL = 0;
   }

   namespace EnemyDetection
   {
      // Enemy casting detection range (squared for performance)
      // Why 2048.0f: Only check IsCasting() for enemies within 2048 units
      // IsCasting() is expensive, so limit to reasonable combat range
      // Units: Skyrim distance units
      inline constexpr float CASTING_CHECK_RANGE = 2048.0f;

      // Enemy casting detection range squared (avoids sqrt in hot path)
      // Why 2048^2: Precomputed square for distance comparison
      // Units: Skyrim distance units squared
      inline constexpr float CASTING_CHECK_RANGE_SQUARED = CASTING_CHECK_RANGE * CASTING_CHECK_RANGE;
   }

   // =============================================================================
   // VITAL TRACKING CONSTANTS (v0.6.9)
   // =============================================================================
   // Shared constants for health, stamina, and magicka tracking sensors.
   // Previously split across DamageTracking and ResourceTracking namespaces.
   // =============================================================================
   namespace VitalTracking
   {
      // === Timing Constants (shared by all vitals) ===

      // Active window (in-game days, for use with Calendar::GetCurrentGameTime())
      // 2 seconds real-time at default timescale 20:
      // Formula: realSeconds / (60 * 3 * 24) = realSeconds / 4320 game-days
      // Why 2 seconds: Roadmap requirement for ward/shield relevance
      // NOTE: Assumes default timescale=20 (1 real minute = 20 game minutes)
      inline constexpr float ACTIVE_WINDOW_DAYS = 2.0f / 4320.0f;

      // History retention (in-game days)
      // 5 seconds real-time = 5/4320 game-days ≈ 0.00116 days
      inline constexpr float HISTORY_RETENTION_DAYS = 5.0f / 4320.0f;

      // Sentinel value for "no recent hits/heals/usage"
      // Why 99.0f: Exceeds any reasonable combat duration (>1 minute)
      // Used by timeSinceLastHit/timeSinceLastHeal when no events in history
      inline constexpr float NO_RECENT_EVENT_SENTINEL = 99.0f;

      // Conversion factor from game-days to real seconds (at default timescale 20)
      // Derivation: 1 game day = 24 game hours = 24 × 180 real seconds = 4320 real seconds
      inline constexpr float GAME_DAYS_TO_REAL_SECONDS = 4320.0f;

      // Max events in ring buffer
      inline constexpr size_t MAX_EVENTS = 10;

      // Exponential decay constant
      // λ = 2.0 means half-life of ln(2)/2 ≈ 0.35 game-hours
      // Faster decay emphasizes recent events
      inline constexpr float EXPONENTIAL_DECAY_CONSTANT = 2.0f;

      // Trend detection threshold (units/sec change)
      // If rate changes by >2 units/sec, flag as increasing/decreasing
      inline constexpr float TREND_CHANGE_THRESHOLD = 2.0f;

      // === Health-specific thresholds ===

      // Damage detection threshold
      // Why 5.0f: Epsilon::VITAL_MAX_VALUE from VitalsState
      // Prevents triggering on health regen micro-fluctuations
      inline constexpr float HEALTH_DAMAGE_THRESHOLD = 5.0f;

      // Healing detection threshold (v0.5.3 - Phase 4.5)
      // Why 0.5f: Lower than damage threshold to catch gradual healing
      // Healing spells heal ~1 HP/100ms, natural regen ~0.1-0.2 HP/100ms
      // Must be sensitive enough to detect incremental healing events
      inline constexpr float HEALTH_HEALING_THRESHOLD = 0.5f;

      // Sustained attack threshold (HP/sec)
      // >5 HP/sec indicates active combat damage
      inline constexpr float HEALTH_SUSTAINED_DAMAGE = 5.0f;

      // Critical damage threshold (HP/sec)
      // >20 HP/sec indicates life-threatening damage rate
      inline constexpr float HEALTH_CRITICAL_DAMAGE = 20.0f;

      // === Stamina-specific thresholds ===

      inline constexpr float STAMINA_USAGE_THRESHOLD = 3.0f;    // Min delta to record
      inline constexpr float STAMINA_HEAVY_RATE = 10.0f;        // "Heavy" usage/sec
      inline constexpr float STAMINA_TREND_THRESHOLD = 3.0f;    // Rate change for trend

      // === Magicka-specific thresholds ===

      inline constexpr float MAGICKA_USAGE_THRESHOLD = 5.0f;    // Min delta to record
      inline constexpr float MAGICKA_HEAVY_RATE = 15.0f;        // "Heavy" usage/sec
      inline constexpr float MAGICKA_TREND_THRESHOLD = 5.0f;    // Rate change for trend

      // === Shared regen threshold ===

      inline constexpr float REGEN_THRESHOLD = 0.5f;            // Min regen delta

      // === Shared helpers (reduce duplication across Health/Stamina/MagickaTracking) ===

      // Convert a game-time event timestamp to "seconds since event" (real time).
      // Returns NO_RECENT_EVENT_SENTINEL if no event has occurred (timestamp == 0).
      inline float TimeSince(float gameTime, float eventTime) noexcept
      {
         return eventTime > 0.0f
            ? (gameTime - eventTime) * GAME_DAYS_TO_REAL_SECONDS
            : NO_RECENT_EVENT_SENTINEL;
      }

      // Accumulate sub-threshold vital losses across ticks, with decay on recovery.
      // On loss: accumulate delta. On gain: decay 0.8x (~310ms half-life at 100ms polls).
      // Snaps to zero below 0.1 to avoid float drift.
      inline void UpdateAccumulator(float& accumulator, float delta) noexcept
      {
         if (delta < 0.0f) {
            accumulator += -delta;
         } else {
            accumulator *= 0.8f;
            if (accumulator < 0.1f) {
               accumulator = 0.0f;
            }
         }
      }

      // === Elemental damage enrichment (v0.12.x) ===

      // Time window for treating instant-hit elemental damage as "active"
      // Why 5.0f: Long enough for player to react to recommendation, short enough to fade naturally
      // Used in Main.cpp to bridge the gap between DoT detection (PollPlayerMagicEffects)
      // and instant-hit detection (DamageEventSink → HealthTrackingState)
      // Units: real seconds
      inline constexpr float ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW = 5.0f;
   }

   // =============================================================================
   // RESISTANCE THRESHOLDS (v0.6.6)
   // =============================================================================
   // Player resistance values for deprioritizing redundant resist buffs
   namespace ResistanceThreshold
   {
      // High resistance threshold (deprioritize resist items)
      // Why 50.0f: At 50% resistance, resist spells provide diminishing returns
      // Units: percentage (0-100)
      inline constexpr float HIGH_RESIST = 50.0f;

      // Capped resistance threshold (resist items nearly useless)
      // Why 75.0f: Near cap, additional resist spells are minimal benefit
      // Units: percentage (0-100)
      inline constexpr float CAPPED_RESIST = 75.0f;

      // Maximum resistance (game cap)
      // Why 85.0f: Skyrim caps most elemental resistances at 85%
      // Units: percentage (0-100)
      inline constexpr float MAX_RESIST = 85.0f;

      // Epsilon for resistance comparison
      // Why 1.0f: 1% tolerance for change detection
      inline constexpr float EPSILON = 1.0f;
   }

   // =============================================================================
   // CROSSHAIR HYSTERESIS (v0.6.7)
   // =============================================================================
   // Prevents crosshair target flickering at short range by keeping the target
   // "sticky" for a brief period even when raycast detection fails momentarily.
   namespace CrosshairHysteresis
   {
      // Sticky target persistence timeout (seconds)
      // Why 0.3f: 300ms = 3 polls at 100ms rate. Covers typical raycast jitter
      // without being so long that stale targets feel "sticky"
      // Units: seconds (real time)
      inline constexpr float PERSISTENCE_TIMEOUT_SEC = 0.3f;

      // Maximum range to keep sticky target (squared for performance)
      // Why 2048.0f: Match detection range so allies at distance don't lose
      // sticky target when crosshair briefly moves away. Enemies have combat
      // fallback, but allies rely on sticky targeting for persistence.
      // Units: Skyrim distance units squared
      inline constexpr float MAX_STICKY_RANGE_SQ = 2048.0f * 2048.0f;

      // Pre-computed persistence timeout in game-days for direct comparison
      // Formula: realSeconds / GAME_DAYS_TO_REAL_SECONDS = game-days
      // Uses VitalTracking::GAME_DAYS_TO_REAL_SECONDS (4320.0f) for consistency
      // Why: Avoid runtime division in hot path
      inline constexpr float PERSISTENCE_TIMEOUT_DAYS =
          PERSISTENCE_TIMEOUT_SEC / VitalTracking::GAME_DAYS_TO_REAL_SECONDS;
   }

   // =============================================================================
   // WARMTH THRESHOLDS (v0.6.6)
   // =============================================================================
   // Warmth rating thresholds for CC Survival Mode cold resistance
   namespace WarmthThreshold
   {
      // Low warmth (vulnerable to cold)
      // Why 50.0f: Below 50 warmth provides minimal cold protection
      inline constexpr float LOW = 50.0f;

      // Moderate warmth (some protection)
      // Why 100.0f: 100+ warmth provides decent cold resistance
      inline constexpr float MODERATE = 100.0f;

      // High warmth (well protected)
      // Why 200.0f: 200+ warmth provides strong cold resistance
      inline constexpr float HIGH = 200.0f;

      // Very high warmth (excellent protection)
      // Why 300.0f: 300+ warmth provides near-immunity to cold
      inline constexpr float VERY_HIGH = 300.0f;
   }

   // =============================================================================
   // VAMPIRE THRESHOLDS (v0.6.6)
   // =============================================================================
   // Vampire stage detection for sun damage context
   namespace VampireThreshold
   {
      // Not a vampire
      inline constexpr int NOT_VAMPIRE = 0;

      // Vampire stage 1 (newly turned, minimal sun weakness)
      inline constexpr int STAGE_1 = 1;

      // Vampire stage 2 (moderate sun weakness)
      inline constexpr int STAGE_2 = 2;

      // Vampire stage 3 (severe sun weakness)
      inline constexpr int STAGE_3 = 3;

      // Vampire stage 4 (maximum sun weakness, NPC hostility)
      inline constexpr int STAGE_4 = 4;

      // Stage at which sun damage becomes significant
      // Why 3: Stage 3+ vampires take substantial sun damage
      inline constexpr int SUN_VULNERABLE_STAGE = 3;
   }

   // =============================================================================
   // EFFECT DETECTION THRESHOLDS (v0.12.x)
   // =============================================================================
   // Guards against misclassifying quasi-permanent effects as active combat drains.
   // CC Survival Mode (and SMI) applies "Attribute Penalty" effects with
   // duration=99999s and spell type kSpell. These are status effects managed by
   // eating/sleeping, not combat drains.
   namespace EffectDetection
   {
      // Maximum duration (seconds) for an effect to be considered an active drain.
      // Real combat drains: poisons 10-60s, absorb spells 10-30s, long DoTs up to 300s.
      // Survival penalties: 99999s. Standing stone debuffs: similar.
      // Why 3600: 1 hour real-time is far beyond any combat effect; safely excludes
      // quasi-permanent status effects while catching all real drains.
      inline constexpr float MAX_DRAIN_DURATION = 3600.0f;
   }
}
