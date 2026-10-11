#include "StateEvaluator.h"
#include "StateConstants.h"
#include "core/ActorTypeClassifier.h"
#include "core/BucketHysteresis.h"
#include "util/ScopedTimer.h"

#include <algorithm>
#include <array>

namespace
{
   using Huginn::Core::BucketEdge;

   // The six vital buckets, in percent: Critical(<=10), VeryLow(<=25),
   // Low(<=40), Medium(<=60), High(<=80), VeryHigh -- all three enums share
   // the ordinal layout. Exponential spacing for low-vital granularity.
   //
   // The band applies to a RISING vital only (RisingBandEdge): one falling from
   // above a boundary changes bucket at the boundary, where it did before. One
   // that falls back after recovering less than the band (8% -> 12% -> 9%)
   // never left the lower bucket, so its second fall is no hash change -- the
   // bucket is never LESS urgent than the raw value, but that fall does not
   // wake the pipeline by itself. Overrides do not rely on it: they read raw
   // vitals, and PipelineCoordinator wakes on their own thresholds
   // (Core::OverrideZone). On the way up nothing is urgent, and that side is
   // where a regenerating pool flaps.
   [[nodiscard]] std::array<BucketEdge, 5> VitalEdges(float band) noexcept
   {
      const double points = static_cast<double>(band) * 100.0;
      return { Huginn::Core::RisingBandEdge(10.0, points), Huginn::Core::RisingBandEdge(25.0, points),
               Huginn::Core::RisingBandEdge(40.0, points), Huginn::Core::RisingBandEdge(60.0, points),
               Huginn::Core::RisingBandEdge(80.0, points) };
   }

   // One vital: the percent is formed in float exactly as the old classifier
   // did (fraction * 100.0f, compared against 10/25/40/60/80, all exact in
   // either width), so at band 0 the bucket is bit-for-bit the old one.
   [[nodiscard]] int VitalBucket(float fraction, int previous, const std::array<BucketEdge, 5>& edges) noexcept
   {
      const float percent = fraction * 100.0f;
      return Huginn::Core::HystereticBucket(static_cast<double>(percent), previous, edges);
   }
}

namespace Huginn::State
{
   GameState StateEvaluator::EvaluateCurrentState(
      const PlayerActorState& player,
      const TargetCollection& targets) const
   {
      SCOPED_TIMER("EvaluateCurrentState");

      BucketMemory memory;
      {
         std::lock_guard lock(m_memoryMutex);
         memory = m_memory;
      }
      BucketMemory unused;
      return Evaluate(player, targets, memory, unused);
   }

   GameState StateEvaluator::EvaluateAndCommit(
      const PlayerActorState& player,
      const TargetCollection& targets,
      BucketBands bands)
   {
      SCOPED_TIMER("EvaluateCurrentState");

      std::lock_guard lock(m_memoryMutex);
      BucketMemory memory = m_memory;
      memory.bands = bands;
      BucketMemory next;
      const GameState state = Evaluate(player, targets, memory, next);
      m_memory = next;
      return state;
   }

   void StateEvaluator::ResetBucketMemory()
   {
      std::lock_guard lock(m_memoryMutex);
      const BucketBands bands = m_memory.bands;
      m_memory = BucketMemory{};
      m_memory.bands = bands;
   }

   GameState StateEvaluator::Evaluate(
      const PlayerActorState& player,
      const TargetCollection& targets,
      const BucketMemory& memory,
      BucketMemory& next) const
   {
      next = BucketMemory{};
      next.bands = memory.bands;

      const auto vitalEdges = VitalEdges(memory.bands.vitalBand);
      next.health = VitalBucket(player.vitals.health, memory.health, vitalEdges);
      next.magicka = VitalBucket(player.vitals.magicka, memory.magicka, vitalEdges);
      next.stamina = VitalBucket(player.vitals.stamina, memory.stamina, vitalEdges);

      // Closest living hostile, not the crosshair primary -- see GameState::distance.
      // No hostile is Ranged and clears the memory, so the next enemy to
      // appear is bucketed where it actually stands. The target digest
      // (StateManager::TargetDigest) still uses the raw band: it only opens
      // the outer skip gate, and waking for a raw crossing the hash then
      // ignores costs a skip check, not a re-score.
      DistanceBucket distance = DistanceBucket::Ranged;
      if (const auto enemy = targets.GetClosestEnemy(); enemy.has_value()) {
         const double f = static_cast<double>(memory.bands.distanceFraction);
         const std::array<BucketEdge, 2> edges{
            Core::SquaredDistanceEdge(DistanceThresholds::EVAL_MELEE_MAX, f),
            Core::SquaredDistanceEdge(DistanceThresholds::EVAL_MID_MAX, f) };
         next.distance = Core::HystereticBucket(
            static_cast<double>(enemy->distanceToPlayerSq), memory.distance, edges);
         distance = static_cast<DistanceBucket>(next.distance);
      }

      return GameState{
      .health = static_cast<HealthBucket>(next.health),
      .magicka = static_cast<MagickaBucket>(next.magicka),
      .stamina = static_cast<StaminaBucket>(next.stamina),
      .distance = distance,
      .targetType = EvaluateTargetType(targets),
      .enemyCount = EvaluateEnemyCount(targets),
      .allyStatus = EvaluateAllyStatus(targets),
      .anyCasting = targets.cachedAnyCasting ? CastingStatus::EnemyCasting : CastingStatus::NoneCasting,
      .inCombat = player.isInCombat ? CombatStatus::InCombat : CombatStatus::NotInCombat,
      .isSneaking = player.isSneaking ? SneakStatus::Sneaking : SneakStatus::NotSneaking
      };
   }

   TargetType StateEvaluator::EvaluateTargetType(const TargetCollection& targets) const
   {
      return targets.ScoringTargetType();
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
      bool hasAlly = false;

      for (const auto& target : targets.targets) {
      if (target.isHostile || target.isDead) continue;
      hasAlly = true;
      if (target.vitals.IsHealthLow()) return AllyStatus::InjuredPresent;
      }

      return hasAlly ? AllyStatus::Present : AllyStatus::None;
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

      // The rules live in ActorTypeClassifier (pure, so `hg dump races` and the
      // host check against race_map.csv run the very same code). Keywords come
      // from the race and from the actor's own NPC record; mods tag their
      // creatures with these, and their race IDs follow no pattern (a LoreRim
      // Gloom Wraith read Beast and then Humanoid and never Undead,
      // 2026-10-03 19:35).
      const auto* base = actor->GetActorBase();
      const char* edid = race->GetFormEditorID();
      return ActorTypeClassifier::Classify(
      edid ? std::string_view{ edid } : std::string_view{},
      race->data.flags.all(RE::RACE_DATA::Flag::kFlies),
      [&](std::string_view kw) { return race->HasKeywordString(kw); },
      [&](std::string_view kw) { return base && base->HasKeywordString(kw); });
   }
}
