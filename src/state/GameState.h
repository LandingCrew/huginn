#pragma once

namespace Huginn::State
{
   // Health percentage buckets (6 levels - exponential for better low-HP granularity)
   enum class HealthBucket : uint8_t
   {
      Critical = 0,   // 0-10%   (immediate danger)
      VeryLow = 1,    // 11-25%  (urgent)
      Low = 2,        // 26-40%  (concerning)
      Medium = 3,     // 41-60%  (moderate)
      High = 4,       // 61-80%  (comfortable)
      VeryHigh = 5    // 81-100% (safe)
   };

   // Magicka percentage buckets (6 levels - exponential for better low-magicka granularity)
   enum class MagickaBucket : uint8_t
   {
      Critical = 0,   // 0-10%   (almost OOM)
      VeryLow = 1,    // 11-25%  (running low)
      Low = 2,        // 26-40%  (limited casting)
      Medium = 3,     // 41-60%  (moderate pool)
      High = 4,       // 61-80%  (plenty available)
      VeryHigh = 5    // 81-100% (full pool)
   };

   // Stamina percentage buckets (6 levels - important for melee/hybrid playstyles)
   enum class StaminaBucket : uint8_t
   {
      Critical = 0,   // 0-10%   (completely exhausted)
      VeryLow = 1,    // 11-25%  (one power attack left)
      Low = 2,        // 26-40%  (limited actions)
      Medium = 3,     // 41-60%  (moderate pool)
      High = 4,       // 61-80%  (plenty available)
      VeryHigh = 5    // 81-100% (full pool)
   };

   // Distance to target buckets
   enum class DistanceBucket : uint8_t
   {
      Melee = 0,   // 0-256 units (~4m)
      Mid = 1,     // 257-768 units (~4-12m)
      Ranged = 2   // 769+ units (>12m)
   };

   // Target type buckets (7 types)
   enum class TargetType : uint8_t
   {
      None = 0,       // No target
      Humanoid = 1,   // NPCs, bandits, etc.
      Undead = 2,     // Draugr, skeletons, vampires
      Beast = 3,      // Wolves, bears, sabre cats
      Dragon = 4,     // Dragons
      Construct = 5,  // Dwemer automatons (mechanical)
      Daedra = 6      // Atronachs, Dremora (from Oblivion) - affected by anti-daedra magic
   };

   // Helper to get target type name for UI display (v0.6.11)
   [[nodiscard]] inline constexpr const char* GetTargetTypeName(TargetType type) noexcept {
      switch (type) {
      case TargetType::Humanoid: return "Humanoid";
      case TargetType::Undead: return "Undead";
      case TargetType::Beast: return "Beast";
      case TargetType::Dragon: return "Dragon";
      case TargetType::Construct: return "Construct";
      case TargetType::Daedra: return "Daedra";
      default: return "None";
      }
   }

   // Combat status
   enum class CombatStatus : uint8_t
   {
      NotInCombat = 0,
      InCombat = 1
   };

   // Sneak status
   enum class SneakStatus : uint8_t
   {
      NotSneaking = 0,
      Sneaking = 1
   };

   // Hostile-caster status (any living hostile currently casting)
   // Sourced from TargetCollection::cachedAnyCasting; drives ward/counter weights
   // in ContextRuleEngine, so it must be a hash dimension or the pipeline-skip
   // gate ignores casting transitions.
   enum class CastingStatus : uint8_t
   {
      NoneCasting = 0,
      EnemyCasting = 1
   };

   // Enemy count buckets (4 levels - tactical AoE/single-target decisions)
   // Phase 6 stress testing: Staggered percentage-based thresholds scale with MAX_TRACKED_TARGETS
   // Staggered distribution: 20%, 40%, 40% (not flat splits - reflects tactical decision points)
   enum class EnemyCountBucket : uint8_t
   {
      None = 0,    // 0 enemies (safe, buff context)
      One = 1,     // 1-20% of max targets (1-10 @ 50 max: small engagements, single-target optimal)
      Few = 2,     // 21-60% of max targets (11-30 @ 50 max: medium battles, AoE viable)
      Many = 3     // 61-100% of max targets (31+ @ 50 max: large battles, strong AoE preference)
   };

   // Ally status (3 levels - collapsed from AllyCount × HasInjuredAlly)
   // Reduction: 6 combinations → 3 (2x state space reduction)
   enum class AllyStatus : uint8_t
   {
      None = 0,           // No allies nearby
      Present = 1,        // Allies present, all healthy
      InjuredPresent = 2  // At least one injured ally (health < 30%)
   };

   // Shared name arrays for ToString() and DiffGameState()
   namespace BucketNames
   {
      inline constexpr std::array<const char*, 6> kHealth = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      inline constexpr std::array<const char*, 6> kMagicka = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      inline constexpr std::array<const char*, 6> kStamina = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      inline constexpr std::array<const char*, 3> kDistance = { "Melee", "Mid", "Ranged" };
      inline constexpr std::array<const char*, 7> kTarget = { "None", "Humanoid", "Undead", "Beast", "Dragon", "Construct", "Daedra" };
      inline constexpr std::array<const char*, 4> kEnemyCount = { "None", "One", "Few", "Many" };
      inline constexpr std::array<const char*, 3> kAllyStatus = { "None", "Present", "InjuredPresent" };
      inline constexpr std::array<const char*, 2> kCombat = { "OutOfCombat", "InCombat" };
      inline constexpr std::array<const char*, 2> kSneak = { "Standing", "Sneaking" };
      inline constexpr std::array<const char*, 2> kCasting = { "NoCasting", "EnemyCasting" };
   }

   // Complete game state representation
   // Hash states: 6 × 6 × 3 × 7 × 4 × 2 × 2 × 2 × 2 = 48,384
   // (stamina excluded entirely; allyStatus hashed as 2 states, not 3)
   struct GameState
   {
      // Player vitals
      HealthBucket health;        // 6 states
      MagickaBucket magicka;      // 6 states
      StaminaBucket stamina;      // 6 states (KEPT in struct for PotionDiscriminator, excluded from hash)

      // Target context
      DistanceBucket distance;    // 3 states
      TargetType targetType;      // 7 states (None, Humanoid, Undead, Beast, Dragon, Construct, Daedra)

      // Multi-target context
      EnemyCountBucket enemyCount; // 4 states
      // 3 states in the struct, TWO in the hash: GetHash asks only whether the
      // value is InjuredPresent. The full value stays here and in
      // ToString/Diff, where it costs nothing.
      //
      // Only the injured bit is read. Nothing reads allyStatus ITSELF --
      // StateEvaluator writes it and only the logging below consumes it -- but
      // the FACT it encodes is read through a different accessor, which is why
      // grepping the field name is not enough to retire it: ScoreCandidates
      // builds ContextReasonSignals{.allyInjured = targets.HasInjuredFollower()},
      // ContextRuleEngine_Reason marks R::AllyInjured from it, and
      // DominantReason can surface that as the "Ally Hurt" label. All of it
      // runs BELOW CheckHashSkip, so the gate has to be able to see an ally
      // becoming injured or the label never appears -- a follower taking fall
      // damage beside an idle player at full vitals moves no other bucket.
      //
      // The None/Present half is what nothing reads, and it is also all of the
      // observed flapping: `Ally:None<->Present`, 11 times in a 565 s
      // quiet-town log, each costing a full pipeline pass (~1.8 ms, 68% of it
      // the Wheeler push) to recompute an identical answer. Two were 0.31 s
      // pairs no distance hysteresis could catch, because crossing the release
      // margin that fast needs about a sprint. Collapsing to the injured bit
      // makes every one of them free without blinding the gate.
      //
      // Deliberately a SUPERSET of what is read: EvaluateAllyStatus returns
      // InjuredPresent for any injured non-hostile, while HasInjuredFollower
      // requires isFollower. The gate can therefore wake for an injured
      // non-follower ally that produces no label. Over-triggering is the safe
      // direction; the alternative is a second ally concept in GameState.
      AllyStatus allyStatus;
      CastingStatus anyCasting;   // 2 states (any living hostile casting)

      // Player state
      CombatStatus inCombat;      // 2 states
      SneakStatus isSneaking;     // 2 states

      // Generate unique hash for weight table lookup
      // Returns value in range [0, kTotalStates - 1]
      // Stamina excluded: PotionDiscriminator reads it directly, ContextRuleEngine uses raw float
      // AllyStatus narrowed to its injured bit: None and Present are the half
      // nothing reads, and all of the observed flapping (see the field)
      // Multi-radix bases: [6, 6, 3, 7, 4, 2, 2, 2, 2]
      // Multipliers computed at compile time from bases (right-to-left product)
   private:
      static constexpr uint32_t kBases[] = { 6, 6, 3, 7, 4, 2, 2, 2, 2 };
      static constexpr size_t kDims = std::size(kBases);

      // Compute multiplier for dimension i: product of bases[i+1..N-1]
      static constexpr uint32_t Multiplier(size_t i) noexcept {
      uint32_t m = 1;
      for (size_t j = i + 1; j < kDims; ++j) m *= kBases[j];
      return m;
      }

   public:
      static constexpr uint32_t kTotalStates = [] {
      uint32_t t = 1;
      for (auto b : kBases) t *= b;
      return t;
      }();  // 48,384

      [[nodiscard]] uint32_t GetHash() const noexcept
      {
      return static_cast<uint32_t>(health)      * Multiplier(0) +
             static_cast<uint32_t>(magicka)     * Multiplier(1) +
             static_cast<uint32_t>(distance)    * Multiplier(2) +
             static_cast<uint32_t>(targetType)  * Multiplier(3) +
             static_cast<uint32_t>(enemyCount)  * Multiplier(4) +
             // The injured BIT, not the 3-state value -- see the field comment.
             static_cast<uint32_t>(allyStatus == AllyStatus::InjuredPresent)
                                                * Multiplier(5) +
             static_cast<uint32_t>(anyCasting)  * Multiplier(6) +
             static_cast<uint32_t>(inCombat)    * Multiplier(7) +
             static_cast<uint32_t>(isSneaking);
      }

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "GameState[HP:{}, MP:{}, SP:{}, Dist:{}, Target:{}, Enemies:{}, Ally:{}, Casting:{}, Combat:{}, Sneak:{}] hash={}",
        BucketNames::kHealth[std::to_underlying(health)],
        BucketNames::kMagicka[std::to_underlying(magicka)],
        BucketNames::kStamina[std::to_underlying(stamina)],
        BucketNames::kDistance[std::to_underlying(distance)],
        BucketNames::kTarget[std::to_underlying(targetType)],
        BucketNames::kEnemyCount[std::to_underlying(enemyCount)],
        BucketNames::kAllyStatus[std::to_underlying(allyStatus)],
        BucketNames::kCasting[std::to_underlying(anyCasting)],
        BucketNames::kCombat[std::to_underlying(inCombat)],
        BucketNames::kSneak[std::to_underlying(isSneaking)],
        GetHash());
      }

      // Equality operator for testing
      bool operator==(const GameState&) const = default;
   };

   // Returns compact string of changed fields: "Combat:OutOfCombat→InCombat, HP:VeryHigh→Med"
   // Compares each field and builds a comma-separated list of "FieldName:Old→New" for any that differ.
   // On first call (prev is default-constructed), logs all fields.
   [[nodiscard]] inline std::string DiffGameState(const GameState& prev, const GameState& curr)
   {
      std::string result;
      auto append = [&](std::string_view field, std::string_view oldVal, std::string_view newVal) {
         if (!result.empty()) result += ", ";
         result += field;
         result += ':';
         result += oldVal;
         result += "\xe2\x86\x92";  // UTF-8 arrow →
         result += newVal;
      };

      if (prev.health != curr.health)
         append("HP", BucketNames::kHealth[std::to_underlying(prev.health)], BucketNames::kHealth[std::to_underlying(curr.health)]);
      if (prev.magicka != curr.magicka)
         append("MP", BucketNames::kMagicka[std::to_underlying(prev.magicka)], BucketNames::kMagicka[std::to_underlying(curr.magicka)]);
      if (prev.distance != curr.distance)
         append("Dist", BucketNames::kDistance[std::to_underlying(prev.distance)], BucketNames::kDistance[std::to_underlying(curr.distance)]);
      if (prev.targetType != curr.targetType)
         append("Target", BucketNames::kTarget[std::to_underlying(prev.targetType)], BucketNames::kTarget[std::to_underlying(curr.targetType)]);
      if (prev.enemyCount != curr.enemyCount)
         append("Enemies", BucketNames::kEnemyCount[std::to_underlying(prev.enemyCount)], BucketNames::kEnemyCount[std::to_underlying(curr.enemyCount)]);
      if (prev.allyStatus != curr.allyStatus)
         append("Ally", BucketNames::kAllyStatus[std::to_underlying(prev.allyStatus)], BucketNames::kAllyStatus[std::to_underlying(curr.allyStatus)]);
      if (prev.anyCasting != curr.anyCasting)
         append("Casting", BucketNames::kCasting[std::to_underlying(prev.anyCasting)], BucketNames::kCasting[std::to_underlying(curr.anyCasting)]);
      if (prev.inCombat != curr.inCombat)
         append("Combat", BucketNames::kCombat[std::to_underlying(prev.inCombat)], BucketNames::kCombat[std::to_underlying(curr.inCombat)]);
      if (prev.isSneaking != curr.isSneaking)
         append("Sneak", BucketNames::kSneak[std::to_underlying(prev.isSneaking)], BucketNames::kSneak[std::to_underlying(curr.isSneaking)]);

      if (result.empty()) result = "(no changes)";
      return result;
   }
}
