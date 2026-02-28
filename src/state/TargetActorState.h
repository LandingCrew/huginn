#pragma once

#include "PlayerActorState.h"  // For shared components
#include "StateConstants.h"     // For distance thresholds, epsilons
#include "GameState.h"  // For TargetType enum
#include "StateManagerConstants.h"  // For TargetTracking constants
#include <unordered_map>
#include <vector>
#include <optional>
#include <algorithm>  // For std::max

// Undefine Windows min/max macros to avoid conflicts with std::max/min
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Huginn::State
{
   // =============================================================================
   // TARGET SOURCE (v0.6.1)
   // =============================================================================
   // How was this target detected?
   // =============================================================================

   enum class TargetSource : uint8_t
   {
      None = 0,          // No target
      Crosshair,         // Player looking directly at target (highest priority)
      CombatPrimary,     // Player's current combat target
      NearbyEnemy,       // Nearby hostile actor (ProcessLists)
      NearbyAlly         // Nearby friendly actor (future: healing spells)
   };

   // =============================================================================
   // TARGET ACTOR STATE (v0.6.1)
   // =============================================================================
   // Represents a single tracked actor (enemy or ally).
   // Reuses shared components from PlayerActorState for consistency.
   //
   // NOTE (v0.6.1): effects and buffs are NOT polled yet - deferred to Phase 7.
   // These fields exist for API compatibility but remain default-initialized.
   // =============================================================================

   struct TargetActorState
   {
      // Actor identity
      RE::FormID actorFormID = 0;
      TargetType targetType = TargetType::None;
      TargetSource source = TargetSource::None;

      // Shared actor components
      ActorVitals vitals;    // Polled in v0.6.1
      ActorEffects effects;  // NOT polled yet (Phase 7) - default-initialized
      ActorBuffs buffs;      // NOT polled yet (Phase 7) - default-initialized

      // Position and state
      float distanceToPlayerSq = DefaultState::NO_TARGET;  // Squared distance (performance)
      bool isHostile = false;
      bool isDead = false;
      bool isCasting = false;  // Is currently casting a spell

      // v0.6.6: Enhanced target state
      uint16_t level = 0;      // Actor level for illusion spell level cap checks
      bool isStaggered = false;  // Target is staggered (damage window)

      // v0.6.10: Follower tracking
      bool isFollower = false;  // Player teammate (follower/companion via IsPlayerTeammate())

      // v0.6.11: Mage detection
      bool isMage = false;  // Has spell equipped in either hand

      // Tracking metadata
      float lastSeenTime = 0.0f;       // Game time when last detected (for pruning)
      float lastVitalsPollTime = 0.0f; // Game time when vitals were last polled (v0.6.11 optimization)
      float priority = 0.0f;           // Priority score for eviction decisions

      // Distance accessor - computes sqrt only when needed (display/logging)
      [[nodiscard]] float GetDistanceToPlayer() const noexcept {
      return std::sqrt(distanceToPlayerSq);
      }

      // Distance bucket helpers (performance-optimized using squared distance)
      [[nodiscard]] bool IsTargetMelee() const noexcept {
      constexpr float MELEE_MAX_SQ = DistanceThresholds::MELEE_MAX * DistanceThresholds::MELEE_MAX;
      return distanceToPlayerSq > DefaultState::NO_TARGET && distanceToPlayerSq < MELEE_MAX_SQ;
      }

      [[nodiscard]] bool IsTargetClose() const noexcept {
      constexpr float CLOSE_MIN_SQ = DistanceThresholds::CLOSE_MIN * DistanceThresholds::CLOSE_MIN;
      constexpr float CLOSE_MAX_SQ = DistanceThresholds::CLOSE_MAX * DistanceThresholds::CLOSE_MAX;
      return distanceToPlayerSq >= CLOSE_MIN_SQ && distanceToPlayerSq < CLOSE_MAX_SQ;
      }

      [[nodiscard]] bool IsTargetMid() const noexcept {
      constexpr float MID_MIN_SQ = DistanceThresholds::MID_MIN * DistanceThresholds::MID_MIN;
      constexpr float MID_MAX_SQ = DistanceThresholds::MID_MAX * DistanceThresholds::MID_MAX;
      return distanceToPlayerSq >= MID_MIN_SQ && distanceToPlayerSq < MID_MAX_SQ;
      }

      [[nodiscard]] bool IsTargetRanged() const noexcept {
      constexpr float RANGED_MIN_SQ = DistanceThresholds::RANGED_MIN * DistanceThresholds::RANGED_MIN;
      constexpr float MAX_RANGE_SQ = TargetTracking::DETECTION_RANGE_SQ;
      return distanceToPlayerSq >= RANGED_MIN_SQ && distanceToPlayerSq <= MAX_RANGE_SQ;
      }

      // Vitals helpers
      [[nodiscard]] bool IsTargetHealthCritical() const noexcept {
      return vitals.health < VitalThreshold::TARGET_CRITICAL;
      }

      [[nodiscard]] bool IsTargetHealthVeryLow() const noexcept {
      return vitals.health < VitalThreshold::TARGET_VERY_LOW;
      }

      [[nodiscard]] bool IsTargetHealthLow() const noexcept {
      return vitals.health < VitalThreshold::TARGET_LOW;
      }

      [[nodiscard]] bool IsTargetMagickaLow() const noexcept {
      return vitals.magicka < VitalThreshold::TARGET_RESOURCE_LOW;
      }

      [[nodiscard]] bool IsTargetStaminaLow() const noexcept {
      return vitals.stamina < VitalThreshold::TARGET_RESOURCE_LOW;
      }

      // Level helpers (v0.6.6)
      [[nodiscard]] bool IsAboveLevel(uint16_t spellLevelCap) const noexcept {
      return level > spellLevelCap;
      }

      // Stagger helpers (v0.6.6)
      [[nodiscard]] bool IsDamageWindowOpen() const noexcept {
      return isStaggered && !isDead;
      }

      // Vitals polling helpers (v0.6.11)
      // Returns true if vitals need to be re-polled for secondary targets
      [[nodiscard]] bool NeedsVitalsPoll(float currentGameTime, float intervalMs) const noexcept {
      // Convert game time difference to milliseconds
      // Game time is in days, need to convert to seconds then ms
      constexpr float GAME_DAYS_TO_REAL_SECONDS = 86400.0f / 24.0f;  // 1 game hour = 1 real hour (3600 sec)
      const float timeSinceLastPoll = currentGameTime - lastVitalsPollTime;
      const float msSinceLastPoll = timeSinceLastPoll * GAME_DAYS_TO_REAL_SECONDS * 1000.0f;
      return msSinceLastPoll >= intervalMs || lastVitalsPollTime <= 0.0f;
      }

      // Priority calculation for target tracking
      // Higher priority = more important to track (used for eviction decisions)
      [[nodiscard]] float CalculatePriority() const noexcept {
      float distance = std::sqrt(distanceToPlayerSq);
      distance = std::max(distance, 1.0f);  // Avoid division by zero

      float calculatedPriority = TargetTracking::PRIORITY_DISTANCE_WEIGHT / distance;

      // Source-based bonuses (mutually exclusive)
      if (source == TargetSource::Crosshair) {
        calculatedPriority += TargetTracking::PRIORITY_CROSSHAIR_BONUS;
      } else if (source == TargetSource::CombatPrimary) {
        calculatedPriority += TargetTracking::PRIORITY_COMBAT_TARGET_BONUS;
      }

      // State-based bonuses (cumulative)
      if (isHostile) {
        calculatedPriority += TargetTracking::PRIORITY_HOSTILE_BONUS;
      }

      if (vitals.health < VitalThreshold::LOW) {
        calculatedPriority += TargetTracking::PRIORITY_LOW_HEALTH_BONUS;
      }

      return calculatedPriority;
      }

      // Equality comparison (epsilon-tolerant)
      bool operator==(const TargetActorState& other) const {
      // PERFORMANCE: Compare squared distances with proper epsilon tolerance
      // Fast path: if squared difference is tiny, distances are definitely equal
      constexpr float EPSILON_SQ = Epsilon::DISTANCE * Epsilon::DISTANCE;
      const float distSqDiff = std::abs(distanceToPlayerSq - other.distanceToPlayerSq);

      bool distanceEqual;
      if (distSqDiff < EPSILON_SQ) {
        // Difference is less than epsilon², so actual distance difference < epsilon
        distanceEqual = true;
      } else {
        // Slow path: compute actual distances for proper comparison
        const float dist1 = std::sqrt(distanceToPlayerSq);
        const float dist2 = std::sqrt(other.distanceToPlayerSq);
        distanceEqual = std::abs(dist1 - dist2) < Epsilon::DISTANCE;
      }

      return actorFormID == other.actorFormID &&
             targetType == other.targetType &&
             source == other.source &&
             vitals == other.vitals &&
             effects == other.effects &&
             buffs == other.buffs &&
             distanceEqual &&
             isHostile == other.isHostile &&
             isDead == other.isDead &&
             isCasting == other.isCasting &&
             level == other.level &&
             isStaggered == other.isStaggered &&
             isFollower == other.isFollower &&
             isMage == other.isMage;
      // Note: lastSeenTime and priority excluded (metadata, not state)
      }
   };

   // =============================================================================
   // TARGET COLLECTION (v0.6.1)
   // =============================================================================
   // Manages up to MAX_TRACKED_TARGETS actors with priority-based eviction.
   // Provides aggregate query methods for multi-target recommendations.
   //
   // THREAD SAFETY:
   // - Entire struct copied out via StateManager::GetTargets()
   // - No heap allocations after initialization (std::unordered_map pre-reserved)
   // =============================================================================

   struct TargetCollection
   {
      // Constructor - reserves capacity to avoid rehashing
      TargetCollection() {
      targets.reserve(TargetTracking::MAX_TRACKED_TARGETS);
      }

      // Primary target (crosshair or combat target - highest priority)
      std::optional<TargetActorState> primary;

      // All tracked targets (FormID -> TargetActorState)
      // Capacity pre-reserved in constructor to avoid rehashing
      std::unordered_map<RE::FormID, TargetActorState> targets;

      // =============================================================================
      // QUERY METHODS
      // =============================================================================

      // Get target by FormID
      [[nodiscard]] std::optional<TargetActorState> GetTarget(RE::FormID formID) const noexcept {
      auto it = targets.find(formID);
      if (it != targets.end()) {
        return it->second;
      }
      return std::nullopt;
      }

      // Get all nearby enemies (hostile actors)
      [[nodiscard]] std::vector<TargetActorState> GetNearbyEnemies() const {
      std::vector<TargetActorState> enemies;
      enemies.reserve(targets.size());
      for (const auto& [formID, target] : targets) {
        if (target.isHostile && !target.isDead) {
           enemies.push_back(target);
        }
      }
      return enemies;
      }

      // Get all nearby allies (non-hostile actors)
      [[nodiscard]] std::vector<TargetActorState> GetNearbyAllies() const {
      std::vector<TargetActorState> allies;
      allies.reserve(targets.size());
      for (const auto& [formID, target] : targets) {
        if (!target.isHostile && !target.isDead) {
           allies.push_back(target);
        }
      }
      return allies;
      }

      // Get injured allies (for healing spell recommendations - Phase 7)
      [[nodiscard]] std::vector<TargetActorState> GetInjuredAllies() const {
      std::vector<TargetActorState> injured;
      injured.reserve(targets.size());
      for (const auto& [formID, target] : targets) {
        if (!target.isHostile && !target.isDead && target.vitals.IsHealthLow()) {
           injured.push_back(target);
        }
      }
      return injured;
      }

      // Get most injured ally (for targeted healing recommendations)
      [[nodiscard]] std::optional<TargetActorState> GetMostInjuredAlly() const noexcept {
      std::optional<TargetActorState> mostInjured;
      float lowestHealth = 1.0f;

      for (const auto& [formID, target] : targets) {
        if (!target.isHostile && !target.isDead && target.vitals.health < lowestHealth) {
           lowestHealth = target.vitals.health;
           mostInjured = target;
        }
      }
      return mostInjured;
      }

      // =============================================================================
      // FOLLOWER QUERIES (v0.6.10)
      // =============================================================================
      // Followers are player teammates (IsPlayerTeammate()) - not random friendly NPCs.
      // These methods enable Heal Other/Grand Healing recommendations for followers.
      // =============================================================================

      // Get all nearby followers (player teammates)
      [[nodiscard]] std::vector<TargetActorState> GetNearbyFollowers() const {
      std::vector<TargetActorState> followers;
      followers.reserve(targets.size());
      for (const auto& [formID, target] : targets) {
        if (target.isFollower && !target.isDead) {
           followers.push_back(target);
        }
      }
      return followers;
      }

      // Get injured followers (for healing spell recommendations)
      [[nodiscard]] std::vector<TargetActorState> GetInjuredFollowers() const {
      std::vector<TargetActorState> injured;
      injured.reserve(targets.size());
      for (const auto& [formID, target] : targets) {
        if (target.isFollower && !target.isDead && target.vitals.IsHealthLow()) {
           injured.push_back(target);
        }
      }
      return injured;
      }

      // Get most injured follower (best healing target)
      [[nodiscard]] std::optional<TargetActorState> GetMostInjuredFollower() const noexcept {
      std::optional<TargetActorState> mostInjured;
      float lowestHealth = 1.0f;

      for (const auto& [formID, target] : targets) {
        if (target.isFollower && !target.isDead && target.vitals.health < lowestHealth) {
           lowestHealth = target.vitals.health;
           mostInjured = target;
        }
      }
      return mostInjured;
      }

      // Count followers
      [[nodiscard]] int GetFollowerCount() const noexcept {
      int count = 0;
      for (const auto& [formID, target] : targets) {
        if (target.isFollower && !target.isDead) {
           ++count;
        }
      }
      return count;
      }

      // Check if any follower is injured (for healing spell recommendations)
      [[nodiscard]] bool HasInjuredFollower() const noexcept {
      for (const auto& [formID, target] : targets) {
        if (target.isFollower && !target.isDead && target.vitals.IsHealthLow()) {
           return true;
        }
      }
      return false;
      }

      // Get closest enemy (for melee/close-range recommendations)
      // Skips entries with distanceToPlayerSq <= 0 (the NO_TARGET sentinel from default init)
      [[nodiscard]] std::optional<TargetActorState> GetClosestEnemy() const noexcept {
      std::optional<TargetActorState> closest;
      float closestDistSq = std::numeric_limits<float>::max();

      for (const auto& [formID, target] : targets) {
        if (target.isHostile && !target.isDead &&
            target.distanceToPlayerSq > DefaultState::NO_TARGET &&
            target.distanceToPlayerSq < closestDistSq) {
           closestDistSq = target.distanceToPlayerSq;
           closest = target;
        }
      }
      return closest;
      }

      // =============================================================================
      // AGGREGATE COUNTING
      // =============================================================================

      // Count hostile actors in range
      [[nodiscard]] int CountHostilesInRange(float maxDistanceUnits) const noexcept {
      const float maxDistSq = maxDistanceUnits * maxDistanceUnits;
      int count = 0;
      for (const auto& [formID, target] : targets) {
        if (target.isHostile && !target.isDead && target.distanceToPlayerSq <= maxDistSq) {
           ++count;
        }
      }
      return count;
      }

      // Count enemies in melee range
      [[nodiscard]] int CountEnemiesInMeleeRange() const noexcept {
      // PERFORMANCE: Avoid sqrt by passing MELEE_MAX directly
      // (CountHostilesInRange squares it internally anyway)
      return CountHostilesInRange(DistanceThresholds::MELEE_MAX);
      }

      // Count casting enemies (for ward spell recommendations)
      [[nodiscard]] int CountCastingEnemies() const noexcept {
      int count = 0;
      for (const auto& [formID, target] : targets) {
        if (target.isHostile && !target.isDead && target.isCasting) {
           ++count;
        }
      }
      return count;
      }

      // Check if outnumbered (3+ enemies)
      [[nodiscard]] bool IsOutnumbered() const noexcept {
      return CountHostilesInRange(2048.0f) >= 3;
      }

      // Get enemy count (all hostile actors)
      [[nodiscard]] int GetEnemyCount() const noexcept {
      int count = 0;
      for (const auto& [formID, target] : targets) {
        if (target.isHostile && !target.isDead) {
           ++count;
        }
      }
      return count;
      }

      // =============================================================================
      // TARGET SYNCHRONIZATION
      // =============================================================================

      // Synchronize primary target with targets map
      // EDGE CASE: If primary target was removed from targets map (died, out of range),
      // this method clears the stale primary reference.
      // USAGE: Call after PruneStaleTargets() to ensure primary is always valid.
      void SyncPrimaryTarget() {
      if (primary.has_value()) {
        auto it = targets.find(primary->actorFormID);
        if (it == targets.end()) {
           // Primary target no longer tracked - clear it
           primary.reset();
        } else {
           // Update primary with latest data from targets map
           primary = it->second;
        }
      }
      }

      // =============================================================================
      // PRIORITY CALCULATION
      // =============================================================================

      // Calculate priority score for a target (higher = more important to track)
      // Used for eviction decisions when target collection is full.
      //
      // Formula: Priority = (10.0 / distance) + bonuses
      // - Distance penalty: Closer = higher priority
      // - Hostility bonus: +5.0 if hostile
      // - Low health bonus: +3.0 if health < 30%
      // - Combat target bonus: +10.0 if player's combat target
      // - Crosshair bonus: +15.0 if crosshair target (highest priority)
      //
      // Example scores:
      // - Crosshair enemy at 256 units, 50% HP: (10/256) + 15 + 5 = ~20.04
      // - Combat enemy at 512 units, 20% HP: (10/512) + 10 + 5 + 3 = ~18.02
      // - Nearby enemy at 1024 units, 80% HP: (10/1024) + 5 = ~5.01
      // - Distant ally at 2048 units, 30% HP: (10/2048) + 3 = ~3.005
      // Delegate to TargetActorState member function
      [[nodiscard]] static float CalculatePriority(const TargetActorState& target) noexcept {
      return target.CalculatePriority();
      }

      // Find FormID of lowest-priority target (for eviction)
      [[nodiscard]] std::optional<RE::FormID> FindLowestPriorityTarget() const noexcept {
      if (targets.empty()) {
        return std::nullopt;
      }

      RE::FormID lowestFormID = 0;
      float lowestPriority = std::numeric_limits<float>::max();

      for (const auto& [formID, target] : targets) {
        if (target.priority < lowestPriority) {
           lowestPriority = target.priority;
           lowestFormID = formID;
        }
      }

      return lowestFormID;
      }

      // Equality comparison
      bool operator==(const TargetCollection& other) const {
      if (primary != other.primary) {
        return false;
      }

      if (targets.size() != other.targets.size()) {
        return false;
      }

      for (const auto& [formID, target] : targets) {
        auto it = other.targets.find(formID);
        if (it == other.targets.end() || target != it->second) {
           return false;
        }
      }

      return true;
      }
   };
}
