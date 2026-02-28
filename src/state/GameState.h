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

   // Complete game state representation
   // Hash states: 6 × 6 × 3 × 7 × 4 × 3 × 2 × 2 = 36,288 (stamina excluded from hash)
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
      AllyStatus allyStatus;      // 3 states (collapsed from AllyCount × HasInjuredAlly)

      // Player state
      CombatStatus inCombat;      // 2 states
      SneakStatus isSneaking;     // 2 states

      // Generate unique hash for Q-table lookup
      // Returns value in range [0, 36287]
      // Stamina excluded: PotionDiscriminator reads it directly, ContextRuleEngine uses raw float
      // Multi-radix bases: [6, 6, 3, 7, 4, 3, 2, 2]
      // Multipliers: [6048, 1008, 336, 48, 12, 4, 2, 1]
      [[nodiscard]] uint32_t GetHash() const noexcept
      {
      return static_cast<uint32_t>(health) * 6048 +
             static_cast<uint32_t>(magicka) * 1008 +
             static_cast<uint32_t>(distance) * 336 +
             static_cast<uint32_t>(targetType) * 48 +
             static_cast<uint32_t>(enemyCount) * 12 +
             static_cast<uint32_t>(allyStatus) * 4 +
             static_cast<uint32_t>(inCombat) * 2 +
             static_cast<uint32_t>(isSneaking);
      }

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      // Static constexpr arrays - compiled once, not recreated per call
      static constexpr std::array<const char*, 6> healthNames = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      static constexpr std::array<const char*, 6> magickaNames = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      static constexpr std::array<const char*, 6> staminaNames = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      static constexpr std::array<const char*, 3> distanceNames = { "Melee", "Mid", "Ranged" };
      static constexpr std::array<const char*, 7> targetNames = { "None", "Humanoid", "Undead", "Beast", "Dragon", "Construct", "Daedra" };
      static constexpr std::array<const char*, 4> enemyCountNames = { "None", "One", "Few", "Many" };
      static constexpr std::array<const char*, 3> allyStatusNames = { "None", "Present", "InjuredPresent" };
      static constexpr std::array<const char*, 2> combatNames = { "OutOfCombat", "InCombat" };
      static constexpr std::array<const char*, 2> sneakNames = { "Standing", "Sneaking" };

      return std::format(
        "GameState[HP:{}, MP:{}, SP:{}, Dist:{}, Target:{}, Enemies:{}, Ally:{}, Combat:{}, Sneak:{}] hash={}",
        healthNames[std::to_underlying(health)],
        magickaNames[std::to_underlying(magicka)],
        staminaNames[std::to_underlying(stamina)],
        distanceNames[std::to_underlying(distance)],
        targetNames[std::to_underlying(targetType)],
        enemyCountNames[std::to_underlying(enemyCount)],
        allyStatusNames[std::to_underlying(allyStatus)],
        combatNames[std::to_underlying(inCombat)],
        sneakNames[std::to_underlying(isSneaking)],
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
      static constexpr std::array<const char*, 6> healthNames = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      static constexpr std::array<const char*, 6> magickaNames = { "Critical", "VeryLow", "Low", "Med", "High", "VeryHigh" };
      static constexpr std::array<const char*, 3> distanceNames = { "Melee", "Mid", "Ranged" };
      static constexpr std::array<const char*, 7> targetNames = { "None", "Humanoid", "Undead", "Beast", "Dragon", "Construct", "Daedra" };
      static constexpr std::array<const char*, 4> enemyCountNames = { "None", "One", "Few", "Many" };
      static constexpr std::array<const char*, 3> allyStatusNames = { "None", "Present", "InjuredPresent" };
      static constexpr std::array<const char*, 2> combatNames = { "OutOfCombat", "InCombat" };
      static constexpr std::array<const char*, 2> sneakNames = { "Standing", "Sneaking" };

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
         append("HP", healthNames[std::to_underlying(prev.health)], healthNames[std::to_underlying(curr.health)]);
      if (prev.magicka != curr.magicka)
         append("MP", magickaNames[std::to_underlying(prev.magicka)], magickaNames[std::to_underlying(curr.magicka)]);
      if (prev.distance != curr.distance)
         append("Dist", distanceNames[std::to_underlying(prev.distance)], distanceNames[std::to_underlying(curr.distance)]);
      if (prev.targetType != curr.targetType)
         append("Target", targetNames[std::to_underlying(prev.targetType)], targetNames[std::to_underlying(curr.targetType)]);
      if (prev.enemyCount != curr.enemyCount)
         append("Enemies", enemyCountNames[std::to_underlying(prev.enemyCount)], enemyCountNames[std::to_underlying(curr.enemyCount)]);
      if (prev.allyStatus != curr.allyStatus)
         append("Ally", allyStatusNames[std::to_underlying(prev.allyStatus)], allyStatusNames[std::to_underlying(curr.allyStatus)]);
      if (prev.inCombat != curr.inCombat)
         append("Combat", combatNames[std::to_underlying(prev.inCombat)], combatNames[std::to_underlying(curr.inCombat)]);
      if (prev.isSneaking != curr.isSneaking)
         append("Sneak", sneakNames[std::to_underlying(prev.isSneaking)], sneakNames[std::to_underlying(curr.isSneaking)]);

      if (result.empty()) result = "(no changes)";
      return result;
   }
}
