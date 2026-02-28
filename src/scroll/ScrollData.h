#pragma once

#include "../spell/SpellData.h"  // Reuse SpellType, MagicSchool, ElementType, SpellTag

namespace Huginn::Scroll
{
   // =============================================================================
   // SCROLL DATA (v0.7.7)
   // =============================================================================
   // Scroll metadata for Q-learning and filtering.
   // Scrolls are consumable spell casts - they reuse spell classification.
   // A scroll is essentially a one-time spell with no magicka cost.
   // =============================================================================

   using ScrollType = Spell::SpellType;      // Damage, Healing, Defensive, Utility, Summon, Buff, Debuff
   using ScrollTag = Spell::SpellTag;        // Fire, Frost, Shock, AOE, AntiUndead, etc. (reuse spell tags)
   using MagicSchool = Spell::MagicSchool;   // Destruction, Restoration, Alteration, Illusion, Conjuration
   using ElementType = Spell::ElementType;   // Fire, Frost, Shock, Poison, Sun, Magic

   struct ScrollData
   {
      RE::FormID formID = 0;                     // Unique scroll form ID
      std::string name;                          // Scroll name for display
      ScrollType type = ScrollType::Unknown;     // Primary type classification
      ScrollTag tags = ScrollTag::None;          // Contextual tags (bitflags)
      MagicSchool school = MagicSchool::Unknown; // Magic school
      ElementType element = ElementType::None;   // Element type for damage/resist

      // Scroll-specific properties
      float magnitude = 0.0f;                    // Effect magnitude (for potency comparison)
      float duration = 0.0f;                     // Effect duration in seconds
      uint32_t baseCost = 0;                     // Base magicka cost (of linked spell, for comparison)

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "ScrollData[id={:08X}, name='{}', type={}, school={}, element={}, tags={:08X}, magnitude={:.1f}, duration={:.1f}, cost={}]",
        formID,
        name,
        Spell::SpellTypeToString(type),
        Spell::MagicSchoolToString(school),
        Spell::ElementTypeToString(element),
        std::to_underlying(tags),
        magnitude,
        duration,
        baseCost);
      }

      // Equality operator
      bool operator==(const ScrollData&) const = default;
   };

   // =============================================================================
   // INVENTORY WRAPPER (v0.7.7)
   // =============================================================================
   // Track scrolls in the player's inventory.
   // Unlike potions (alchemy items) but like weapons, scrolls can be of the same
   // type but with different counts. We track count changes for consumption detection.
   // =============================================================================

   struct InventoryScroll
   {
      ScrollData data;                           // Classification data
      int32_t count = 0;                         // Current inventory count
      int32_t previousCount = 0;                 // Count at last full reconciliation (for delta detection)

      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "InventoryScroll[{}, count={}, prev={}]",
        data.name,
        count,
        previousCount);
      }
   };
}
