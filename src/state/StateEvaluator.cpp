#include "StateEvaluator.h"
#include "util/ScopedTimer.h"

#include <algorithm>

namespace
{
   // Case-insensitive substring search - zero heap allocations
   [[nodiscard]] inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept
   {
      auto caseInsensitiveEqual = [](char a, char b) noexcept {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
      };
      return std::search(haystack.begin(), haystack.end(),
                         needle.begin(), needle.end(),
                         caseInsensitiveEqual) != haystack.end();
   }

   // Vital percentage bucketing - all three enums have identical ordinal layout
   template<typename BucketEnum>
   [[nodiscard]] constexpr BucketEnum ClassifyVitalPercentage(float percentage) noexcept
   {
      const float percent = percentage * 100.0f;

      // 6 buckets: Exponential distribution for better low-vital granularity
      // Critical(0-10), VeryLow(11-25), Low(26-40), Medium(41-60), High(61-80), VeryHigh(81-100)
      if (percent <= 10.0f)       return static_cast<BucketEnum>(0);  // Critical
      else if (percent <= 25.0f)  return static_cast<BucketEnum>(1);  // VeryLow
      else if (percent <= 40.0f)  return static_cast<BucketEnum>(2);  // Low
      else if (percent <= 60.0f)  return static_cast<BucketEnum>(3);  // Medium
      else if (percent <= 80.0f)  return static_cast<BucketEnum>(4);  // High
      else                        return static_cast<BucketEnum>(5);  // VeryHigh
   }

   // Distance thresholds for 3-bucket DistanceBucket classification
   // Note: Different from StateManagerConstants (4-bucket system for target tracking)
   namespace EvaluatorThresholds
   {
      inline constexpr float MELEE_MAX = 256.0f;   // 0-256 units (~4m)
      inline constexpr float MID_MAX = 768.0f;     // 257-768 units (~4-12m)
      // Ranged: 769+ units
   }
}

namespace Huginn::State
{
   GameState StateEvaluator::EvaluateCurrentState(
      const WorldState& world,
      const PlayerActorState& player,
      const TargetCollection& targets) const
   {
      SCOPED_TIMER("EvaluateCurrentState");

      return GameState{
      .health = EvaluateHealth(player),
      .magicka = EvaluateMagicka(player),
      .stamina = EvaluateStamina(player),
      .distance = EvaluateDistance(targets),
      .targetType = EvaluateTargetType(targets),
      .enemyCount = EvaluateEnemyCount(targets),
      .allyStatus = EvaluateAllyStatus(targets),
      .inCombat = player.isInCombat ? CombatStatus::InCombat : CombatStatus::NotInCombat,
      .isSneaking = player.isSneaking ? SneakStatus::Sneaking : SneakStatus::NotSneaking
      };
   }

   HealthBucket StateEvaluator::EvaluateHealth(const PlayerActorState& player) const
   {
      return ClassifyVitalPercentage<HealthBucket>(player.vitals.health);
   }

   MagickaBucket StateEvaluator::EvaluateMagicka(const PlayerActorState& player) const
   {
      return ClassifyVitalPercentage<MagickaBucket>(player.vitals.magicka);
   }

   StaminaBucket StateEvaluator::EvaluateStamina(const PlayerActorState& player) const
   {
      return ClassifyVitalPercentage<StaminaBucket>(player.vitals.stamina);
   }

   DistanceBucket StateEvaluator::EvaluateDistance(const TargetCollection& targets) const
   {
      if (!targets.primary.has_value()) {
      return DistanceBucket::Ranged;  // No target
      }

      const float distance = targets.primary->GetDistanceToPlayer();

      if (distance <= EvaluatorThresholds::MELEE_MAX) {
      return DistanceBucket::Melee;
      } else if (distance <= EvaluatorThresholds::MID_MAX) {
      return DistanceBucket::Mid;
      } else {
      return DistanceBucket::Ranged;
      }
   }

   TargetType StateEvaluator::EvaluateTargetType(const TargetCollection& targets) const
   {
      if (!targets.primary.has_value()) {
      return TargetType::None;
      }

      return targets.primary->targetType;
   }

   CombatStatus StateEvaluator::EvaluateCombatStatus() const noexcept
   {
      auto* player = GetPlayer();
      return (player && player->IsInCombat())
      ? CombatStatus::InCombat
      : CombatStatus::NotInCombat;
   }

   SneakStatus StateEvaluator::EvaluateSneakStatus() const noexcept
   {
      auto* player = GetPlayer();
      return (player && player->IsSneaking())
      ? SneakStatus::Sneaking
      : SneakStatus::NotSneaking;
   }

   RE::Actor* StateEvaluator::GetPlayer() const noexcept
   {
      return RE::PlayerCharacter::GetSingleton();
   }

   EnemyCountBucket StateEvaluator::EvaluateEnemyCount(const TargetCollection& targets) const
   {
      const int enemyCount = targets.GetEnemyCount();

      // Staggered percentage-based bucketing (Phase 6 stress testing)
      // Thresholds scale with MAX_TRACKED_TARGETS (currently 50)
      // Staggered ranges: 20%, 40%, 40% (reflects tactical decision points)
      constexpr int threshold_one = static_cast<int>(TargetTracking::MAX_TRACKED_TARGETS * 0.20f);  // 20% → 10
      constexpr int threshold_few = static_cast<int>(TargetTracking::MAX_TRACKED_TARGETS * 0.60f);  // 60% → 30

      if (enemyCount == 0) return EnemyCountBucket::None;
      if (enemyCount <= threshold_one) return EnemyCountBucket::One;  // 1-10 enemies (20%)
      if (enemyCount <= threshold_few) return EnemyCountBucket::Few;  // 11-30 enemies (40%)
      return EnemyCountBucket::Many;                                  // 31+ enemies (40%)
   }

   AllyStatus StateEvaluator::EvaluateAllyStatus(const TargetCollection& targets) const
   {
      const auto allies = targets.GetNearbyAllies();

      if (allies.empty()) return AllyStatus::None;

      // Check if any ally is injured
      const auto injured = targets.GetInjuredAllies();
      if (!injured.empty()) return AllyStatus::InjuredPresent;

      return AllyStatus::Present;
   }

   TargetType StateEvaluator::ClassifyActor(RE::Actor* actor) const
   {
      if (!actor) {
      return TargetType::None;
      }

      auto* race = actor->GetRace();
      if (!race) {
      return TargetType::Humanoid;  // Default fallback
      }

      // Get race editor ID for classification
      const char* raceEditorID = race->GetFormEditorID();
      if (!raceEditorID) {
      return TargetType::Humanoid;
      }

      // Use string_view to avoid heap allocation
      std::string_view raceID{raceEditorID};

      // Classify based on race keywords (case-insensitive, zero allocations)
      // Dragon detection (check first before other checks)
      if (ContainsIgnoreCase(raceID, "dragon")) {
      return TargetType::Dragon;
      }

      // Also check race flags for flying creatures (dragons have kFlies flag)
      // This catches dragons even if their race ID doesn't contain "dragon"
      if (race->data.flags.all(RE::RACE_DATA::Flag::kFlies)) {
      return TargetType::Dragon;
      }

      // Undead detection
      if (ContainsIgnoreCase(raceID, "draugr") ||
          ContainsIgnoreCase(raceID, "skeleton") ||
          ContainsIgnoreCase(raceID, "vampire") ||
          ContainsIgnoreCase(raceID, "ghost") ||
          ContainsIgnoreCase(raceID, "zombie")) {
      return TargetType::Undead;
      }

      // Daedra detection (creatures from Oblivion - affected by anti-daedra magic)
      // Check BEFORE Construct so atronachs are correctly classified
      if (ContainsIgnoreCase(raceID, "atronach") ||
          ContainsIgnoreCase(raceID, "dremora") ||
          ContainsIgnoreCase(raceID, "daedra") ||
          ContainsIgnoreCase(raceID, "scamp") ||
          ContainsIgnoreCase(raceID, "daedroth") ||
          ContainsIgnoreCase(raceID, "seeker") ||      // Hermaeus Mora's servants
          ContainsIgnoreCase(raceID, "lurker")) {      // Hermaeus Mora's servants
      return TargetType::Daedra;
      }

      // Construct detection (Dwemer automatons - mechanical, NOT affected by anti-daedra magic)
      // Check BEFORE Beast so Dwemer spiders are correctly classified as Construct, not Beast
      if (ContainsIgnoreCase(raceID, "dwarven") ||
          ContainsIgnoreCase(raceID, "dwemer") ||
          ContainsIgnoreCase(raceID, "sphere") ||
          ContainsIgnoreCase(raceID, "centurion") ||
          ContainsIgnoreCase(raceID, "ballista")) {
      return TargetType::Construct;
      }

      // Beast detection (natural creatures)
      if (ContainsIgnoreCase(raceID, "wolf") ||
          ContainsIgnoreCase(raceID, "bear") ||
          ContainsIgnoreCase(raceID, "saber") ||
          ContainsIgnoreCase(raceID, "sabre") ||
          ContainsIgnoreCase(raceID, "spider") ||
          ContainsIgnoreCase(raceID, "troll") ||
          ContainsIgnoreCase(raceID, "mammoth") ||
          ContainsIgnoreCase(raceID, "skeever") ||
          ContainsIgnoreCase(raceID, "horker") ||
          ContainsIgnoreCase(raceID, "mudcrab") ||
          ContainsIgnoreCase(raceID, "slaughterfish")) {
      return TargetType::Beast;
      }

      // Default to humanoid (humans, elves, orcs, khajiit, argonians, etc.)
      return TargetType::Humanoid;
   }
}
