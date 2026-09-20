#pragma once

#include "../Config.h"
#include <cstdint>

namespace Huginn::State
{
   // =============================================================================
   // TARGET TRACKING CONFIGURATION (v0.6.1)
   // =============================================================================

   namespace TargetTracking
   {
      // Maximum number of actors to track simultaneously (configured in Config.h)
      inline constexpr size_t MAX_TRACKED_TARGETS = Config::MAX_TRACKED_TARGETS;

      // Target detection range (configured in Config.h)
      inline constexpr float DETECTION_RANGE = Config::TARGET_DETECTION_RANGE;

      // Target detection range squared (for performance - avoids sqrt)
      inline constexpr float DETECTION_RANGE_SQ = DETECTION_RANGE * DETECTION_RANGE;

      // v0.6.12: Reduced detection range for non-follower allies (guards, merchants, etc.)
      // Why 512: Close enough to be contextually relevant, avoids tracking entire city
      inline constexpr float ALLY_DETECTION_RANGE = 512.0f;
      inline constexpr float ALLY_DETECTION_RANGE_SQ = ALLY_DETECTION_RANGE * ALLY_DETECTION_RANGE;

      // Release margin — the gap that makes the range a BAND rather than an edge.
      //
      // Acquisition and the prune used the same 512 threshold, so a townsperson
      // standing near it was added on one poll and pruned on the next, flipping
      // AllyStatus None<->Present at the poll rate. Observed 2026-09-19: four
      // transitions in 1.1 s (20:23:57.322 through 20:23:58.470), each one
      // re-running the whole pipeline to produce an identical result and
      // logging an info-level "State transition — scoring" line for it.
      //
      // Same shape as OverrideConditions' activationThreshold + hysteresisGap,
      // which is the only other place in the codebase that had this right:
      // engage at the threshold, disengage past it.
      //
      // Why 128 (a quarter of the ally range): wide enough that an NPC standing
      // still cannot jitter across it, narrow enough that walking out of range
      // still drops within a stride. It is applied to the hostile/follower 2048
      // range too — the same zero-gap edge exists there and simply has not been
      // caught flapping yet.
      inline constexpr float RANGE_RELEASE_MARGIN = 128.0f;
      inline constexpr float ALLY_RELEASE_RANGE_SQ =
         (ALLY_DETECTION_RANGE + RANGE_RELEASE_MARGIN) * (ALLY_DETECTION_RANGE + RANGE_RELEASE_MARGIN);
      inline constexpr float DETECTION_RELEASE_RANGE_SQ =
         (DETECTION_RANGE + RANGE_RELEASE_MARGIN) * (DETECTION_RANGE + RANGE_RELEASE_MARGIN);

      // Last seen timeout (seconds)
      // Why 3.0f: Remove targets not seen for 3 seconds (likely out of range/dead)
      inline constexpr float LAST_SEEN_TIMEOUT = 3.0f;

      // Dead actor timeout (seconds)
      // Why 5.0f: Keep dead actors tracked for 5 seconds (for necromancy/looting context)
      inline constexpr float DEAD_ACTOR_TIMEOUT = 5.0f;

      // Priority threshold for eviction
      // When collection is full, evict targets with priority < NEW_TARGET_MIN_PRIORITY
      // Only if new target has higher priority
      inline constexpr float NEW_TARGET_MIN_PRIORITY = 0.5f;

      // =============================================================================
      // PRIORITY CALCULATION BONUSES
      // =============================================================================
      // Base priority formula: 10.0 / distance
      // Total priority = base + source bonus + state bonuses

      // Source-based priority bonuses (mutually exclusive)
      inline constexpr float PRIORITY_CROSSHAIR_BONUS = 15.0f;      // Player is looking at target
      inline constexpr float PRIORITY_COMBAT_TARGET_BONUS = 10.0f;  // Player's combat target

      // State-based priority bonuses (cumulative)
      inline constexpr float PRIORITY_HOSTILE_BONUS = 5.0f;         // Target is hostile
      inline constexpr float PRIORITY_LOW_HEALTH_BONUS = 3.0f;      // Target has low HP

      // Distance weight for base priority calculation
      inline constexpr float PRIORITY_DISTANCE_WEIGHT = 10.0f;      // Higher = closer targets prioritized more
   }

   // =============================================================================
   // POLL INTERVAL DEFAULTS (v0.6.1)
   // =============================================================================
   // Default polling intervals for each state component (milliseconds).
   // These can be overridden via configuration.
   //
   // PERFORMANCE NOTES:
   // - Faster polling (50-100ms): Responsive to rapid changes (vitals, crosshair)
   // - Slower polling (500-1000ms): Infrequent changes (equipment, survival)
   // - Total overhead target: < 0.5ms per update cycle
   // =============================================================================

   namespace PollInterval
   {
      // World state polling (locks, ore veins, time, light)
      // Why 100ms: Crosshair detection needs fast response
      inline constexpr float WORLD_MS = 100.0f;

      // Player vitals polling (health/magicka/stamina)
      // Why 100ms: Vital changes during combat need fast detection
      inline constexpr float PLAYER_VITALS_MS = 100.0f;

      // Player magic effects polling (damage effects + buffs)
      // Why 100ms: Effect changes (fire damage, buff expiration) need fast detection
      // NOTE: This polls BOTH effects and buffs in a single iteration (optimization)
      inline constexpr float PLAYER_MAGIC_EFFECTS_MS = 100.0f;

      // Player equipment polling (weapons, ammo, charge)
      // Phase 6: 100ms for better charge tracking responsiveness (was 500ms)
      // Inventory traversal has early-exit optimization to minimize overhead
      inline constexpr float PLAYER_EQUIPMENT_MS = 100.0f;

      // Player survival polling (hunger, cold, fatigue)
      // Why 1000ms: Survival stats change slowly (minutes scale)
      inline constexpr float PLAYER_SURVIVAL_MS = 1000.0f;

      // Player position polling (underwater, falling, combat, sneak)
      // Why 100ms: Position state changes need fast detection for recommendations
      inline constexpr float PLAYER_POSITION_MS = 100.0f;

      // Target tracking polling (multi-target detection and update)
      // Why 100ms: Combat target tracking needs fast updates
      inline constexpr float TARGETS_MS = 100.0f;

      // Player health tracking polling (damage/healing detection)
      // Why 100ms: Fast polling for accurate damage rate detection
      inline constexpr float PLAYER_HEALTH_TRACKING_MS = 100.0f;

      // Player resistances polling (v0.6.6)
      // Why 500ms: Resistances change infrequently (equipment change, potion, spell)
      inline constexpr float PLAYER_RESISTANCES_MS = 500.0f;

      // Player stamina tracking polling (v0.6.9)
      // Why 100ms: Fast polling for accurate stamina rate detection
      inline constexpr float PLAYER_STAMINA_TRACKING_MS = 100.0f;

      // Player magicka tracking polling (v0.6.9)
      // Why 100ms: Fast polling for accurate magicka rate detection
      inline constexpr float PLAYER_MAGICKA_TRACKING_MS = 100.0f;
   }

   // =============================================================================
   // TARGET VITALS POLLING OPTIMIZATION (v0.6.11)
   // =============================================================================
   // Configurable settings for optimizing target vitals (HP/MP/SP) polling.
   // Primary target always gets full-rate polling. Secondary targets use these.
   // =============================================================================

   namespace TargetVitalsPolling
   {
      // Polling interval for secondary targets (enemies/allies not under crosshair)
      // Why 500ms: Balance between accuracy and performance
      // NOTE: Followers always polled at this rate for injured follower detection
      inline constexpr float SECONDARY_VITALS_INTERVAL_MS = 500.0f;

      // Distance threshold for polling secondary target vitals
      // Targets beyond this distance don't get vitals updated (saves API calls)
      // Why 1024: ~16m, reasonable combat awareness range
      inline constexpr float VITALS_POLL_DISTANCE = 1024.0f;
      inline constexpr float VITALS_POLL_DISTANCE_SQ = VITALS_POLL_DISTANCE * VITALS_POLL_DISTANCE;

      // Always poll follower vitals regardless of distance (for injured detection)
      // Set to false to apply distance culling to followers too
      inline constexpr bool ALWAYS_POLL_FOLLOWER_VITALS = true;
   }

}
