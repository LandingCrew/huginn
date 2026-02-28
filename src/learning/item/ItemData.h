#pragma once

namespace Huginn::Item
{
   // =============================================================================
   // SKILL/SCHOOL ENUMS (for grouped fortification tracking)
   // =============================================================================
   // These enums specify WHICH skill/school when a grouped tag is set.
   // Example: FortifyMagicSchool tag + school=Destruction → Fortify Destruction
   // =============================================================================

   // Magic schools (for Fortify School potions and spell synergy)
   enum class MagicSchool : uint8_t
   {
      None = 0,
      Alteration,    // Armor spells, Waterbreathing, Paralysis, Transmute
      Conjuration,   // Summons, Bound Weapons, Soul Trap
      Destruction,   // Fire/Frost/Shock damage spells
      Illusion,      // Calm, Fear, Frenzy, Muffle, Invisibility
      Restoration,   // Healing, Turn Undead, Wards
      Enchanting     // Not a school, but uses same fortify pattern
   };

   // Combat skills (for Fortify combat skill potions)
   enum class CombatSkill : uint8_t
   {
      None = 0,
      OneHanded,
      TwoHanded,
      Marksman,      // Archery
      Block,
      HeavyArmor,
      LightArmor,
      Smithing       // Crafting but combat-adjacent (at forge)
   };

   // Utility skills (lower priority for recommendations)
   enum class UtilitySkill : uint8_t
   {
      None = 0,
      Sneak,
      Lockpicking,
      Pickpocket,
      Speech,        // Barter
      Alchemy        // For completeness (at alchemy table)
   };

   // Element type (for resist/weakness effects - also used in spells)
   enum class ElementType : uint8_t
   {
      None = 0,
      Fire,
      Frost,
      Shock,
      Magic,         // Generic magic resistance/weakness
      Poison,
      Disease
   };

   // =============================================================================
   // ITEM TYPE (Primary classification)
   // =============================================================================

   enum class ItemType : uint8_t
   {
      Unknown = 0,
      HealthPotion,     // Restore health
      MagickaPotion,    // Restore magicka
      StaminaPotion,    // Restore stamina
      ResistPotion,     // Resist fire/frost/shock/poison/magic
      BuffPotion,       // Fortify attribute/skill
      CurePotion,       // Cure disease/poison
      Poison,           // Hostile (apply to weapon)
      Food,             // CC Survival Mode food
      Ingredient,       // Raw alchemy ingredient
      SoulGem           // Soul gem for weapon recharge
   };

   // =============================================================================
   // ITEM TAGS (Bitflags for contextual matching)
   // =============================================================================
   // Organized by category. Grouped tags (e.g., FortifyMagicSchool) require
   // checking the corresponding enum field (e.g., school) for specifics.
   // =============================================================================

   enum class ItemTag : uint32_t
   {
      None = 0,

      // -------------------------------------------------------------------------
      // RESTORATION EFFECTS (bits 0-2)
      // -------------------------------------------------------------------------
      RestoreHealth   = 1 << 0,
      RestoreMagicka  = 1 << 1,
      RestoreStamina  = 1 << 2,

      // -------------------------------------------------------------------------
      // RESISTANCES (bits 3-8) - Individual for fast lookup
      // -------------------------------------------------------------------------
      ResistFire      = 1 << 3,
      ResistFrost     = 1 << 4,
      ResistShock     = 1 << 5,
      ResistMagic     = 1 << 6,
      ResistPoison    = 1 << 7,
      ResistDisease   = 1 << 8,   // Not craftable but exists in game

      // -------------------------------------------------------------------------
      // FORTIFY ATTRIBUTES (bits 9-11)
      // -------------------------------------------------------------------------
      FortifyHealth   = 1 << 9,
      FortifyMagicka  = 1 << 10,
      FortifyStamina  = 1 << 11,

      // -------------------------------------------------------------------------
      // FORTIFY SKILLS - GROUPED (bits 12-14)
      // Check corresponding enum field for which skill/school
      // -------------------------------------------------------------------------
      FortifyMagicSchool  = 1 << 12,  // Check 'school' field (Destruction, etc.)
      FortifyCombatSkill  = 1 << 13,  // Check 'combatSkill' field (OneHanded, etc.)
      FortifyUtilitySkill = 1 << 14,  // Check 'utilitySkill' field (Sneak, etc.)

      // -------------------------------------------------------------------------
      // REGENERATION (bits 15-17)
      // -------------------------------------------------------------------------
      RegenHealth     = 1 << 15,
      RegenMagicka    = 1 << 16,
      RegenStamina    = 1 << 17,  // NEW: Was missing!

      // -------------------------------------------------------------------------
      // CURES (bits 18-19)
      // -------------------------------------------------------------------------
      CureDisease     = 1 << 18,
      CurePoison      = 1 << 19,

      // -------------------------------------------------------------------------
      // SURVIVAL MODE (bits 20-21)
      // -------------------------------------------------------------------------
      SatisfiesHunger = 1 << 20,
      SatisfiesCold   = 1 << 21,  // Warming effect (soups, stews)

      // -------------------------------------------------------------------------
      // UTILITY BUFFS (bits 22-24)
      // -------------------------------------------------------------------------
      Invisibility    = 1 << 22,
      Waterbreathing  = 1 << 23,
      FortifyCarryWeight = 1 << 24,  // NEW: When overencumbered

      // -------------------------------------------------------------------------
      // POISON EFFECTS - Damage (bits 25-27)
      // -------------------------------------------------------------------------
      DamageHealth    = 1 << 25,
      DamageMagicka   = 1 << 26,
      DamageStamina   = 1 << 27,

      // -------------------------------------------------------------------------
      // POISON EFFECTS - Control (bits 28-30)
      // -------------------------------------------------------------------------
      Paralyze        = 1 << 28,
      Slow            = 1 << 29,
      Fear            = 1 << 30,  // NEW: Crowd control poison

      // -------------------------------------------------------------------------
      // POISON MODIFIERS (bit 31)
      // Combined with other tags for full effect description
      // -------------------------------------------------------------------------
      Frenzy          = 1u << 31  // Makes target attack allies

      // NOTE: Lingering moved to ItemTagExt to make room
   };

   // =============================================================================
   // EXTENDED TAGS (for effects that don't fit in 32 bits)
   // =============================================================================
   // These are less common effects that can use a secondary tag field
   // or be inferred from combinations of primary tags + fields.
   // =============================================================================

   enum class ItemTagExt : uint16_t
   {
      None = 0,

      // Poison modifiers (moved from primary to save bits)
      Lingering           = 1 << 0,   // Damage over time modifier

      // Weakness effects (check 'element' field for which)
      WeaknessElement     = 1 << 1,   // Weakness to Fire/Frost/Shock/Magic/Poison

      // Ravage effects (instant damage to MAX attribute)
      RavageHealth        = 1 << 2,
      RavageMagicka       = 1 << 3,
      RavageStamina       = 1 << 4,

      // Damage regen effects
      DamageHealthRegen   = 1 << 5,
      DamageMagickaRegen  = 1 << 6,
      DamageStaminaRegen  = 1 << 7,

      // Soul gem capacities (moved from primary to save bits)
      SoulGemPetty        = 1 << 8,
      SoulGemLesser       = 1 << 9,
      SoulGemCommon       = 1 << 10,
      SoulGemGreater      = 1 << 11,
      SoulGemGrand        = 1 << 12,
      SoulGemBlack        = 1 << 13
   };

   // Enable bitwise operations on ItemTag
   inline ItemTag operator|(ItemTag a, ItemTag b)
   {
      return static_cast<ItemTag>(std::to_underlying(a) | std::to_underlying(b));
   }

   inline ItemTag operator&(ItemTag a, ItemTag b)
   {
      return static_cast<ItemTag>(std::to_underlying(a) & std::to_underlying(b));
   }

   inline ItemTag& operator|=(ItemTag& a, ItemTag b)
   {
      a = a | b;
      return a;
   }

   inline bool HasTag(ItemTag tags, ItemTag check)
   {
      return std::to_underlying(tags & check) != 0;
   }

   // Enable bitwise operations on ItemTagExt
   inline ItemTagExt operator|(ItemTagExt a, ItemTagExt b)
   {
      return static_cast<ItemTagExt>(std::to_underlying(a) | std::to_underlying(b));
   }

   inline ItemTagExt operator&(ItemTagExt a, ItemTagExt b)
   {
      return static_cast<ItemTagExt>(std::to_underlying(a) & std::to_underlying(b));
   }

   inline ItemTagExt& operator|=(ItemTagExt& a, ItemTagExt b)
   {
      a = a | b;
      return a;
   }

   inline bool HasTagExt(ItemTagExt tags, ItemTagExt check)
   {
      return std::to_underlying(tags & check) != 0;
   }

   // =============================================================================
   // STRING CONVERSIONS (for logging)
   // =============================================================================

   inline std::string_view ItemTypeToString(ItemType type)
   {
      switch (type) {
      case ItemType::HealthPotion:   return "HealthPotion";
      case ItemType::MagickaPotion:  return "MagickaPotion";
      case ItemType::StaminaPotion:  return "StaminaPotion";
      case ItemType::ResistPotion:   return "ResistPotion";
      case ItemType::BuffPotion:     return "BuffPotion";
      case ItemType::CurePotion:     return "CurePotion";
      case ItemType::Poison:         return "Poison";
      case ItemType::Food:           return "Food";
      case ItemType::Ingredient:     return "Ingredient";
      case ItemType::SoulGem:        return "SoulGem";
      default:                       return "Unknown";
      }
   }

   inline std::string_view MagicSchoolToString(MagicSchool school)
   {
      switch (school) {
      case MagicSchool::Alteration:  return "Alteration";
      case MagicSchool::Conjuration: return "Conjuration";
      case MagicSchool::Destruction: return "Destruction";
      case MagicSchool::Illusion:    return "Illusion";
      case MagicSchool::Restoration: return "Restoration";
      case MagicSchool::Enchanting:  return "Enchanting";
      default:                       return "None";
      }
   }

   inline std::string_view CombatSkillToString(CombatSkill skill)
   {
      switch (skill) {
      case CombatSkill::OneHanded:   return "OneHanded";
      case CombatSkill::TwoHanded:   return "TwoHanded";
      case CombatSkill::Marksman:    return "Marksman";
      case CombatSkill::Block:       return "Block";
      case CombatSkill::HeavyArmor:  return "HeavyArmor";
      case CombatSkill::LightArmor:  return "LightArmor";
      case CombatSkill::Smithing:    return "Smithing";
      default:                       return "None";
      }
   }

   inline std::string_view UtilitySkillToString(UtilitySkill skill)
   {
      switch (skill) {
      case UtilitySkill::Sneak:       return "Sneak";
      case UtilitySkill::Lockpicking: return "Lockpicking";
      case UtilitySkill::Pickpocket:  return "Pickpocket";
      case UtilitySkill::Speech:      return "Speech";
      case UtilitySkill::Alchemy:     return "Alchemy";
      default:                        return "None";
      }
   }

   inline std::string_view ElementTypeToString(ElementType element)
   {
      switch (element) {
      case ElementType::Fire:    return "Fire";
      case ElementType::Frost:   return "Frost";
      case ElementType::Shock:   return "Shock";
      case ElementType::Magic:   return "Magic";
      case ElementType::Poison:  return "Poison";
      case ElementType::Disease: return "Disease";
      default:                   return "None";
      }
   }

   // =============================================================================
   // ITEM DATA STRUCT
   // =============================================================================
   // Complete item metadata for Q-learning and filtering.
   // The grouped tag fields (school, combatSkill, etc.) specify WHICH
   // skill/school when the corresponding grouped tag is set.
   // =============================================================================

   struct ItemData
   {
      RE::FormID formID = 0;              // Unique item form ID
      std::string name;                   // Item name for display
      ItemType type = ItemType::Unknown;  // Primary type classification
      ItemTag tags = ItemTag::None;       // Primary contextual tags (bitflags)
      ItemTagExt tagsExt = ItemTagExt::None;  // Extended tags (less common effects)

      // Effect specifics
      float magnitude = 0.0f;             // Effect magnitude (for potency comparison)
      float duration = 0.0f;              // Effect duration
      uint32_t value = 0;                 // Gold value
      bool isHostile = false;             // Poison vs beneficial

      // Grouped tag specifics - which skill/school/element
      MagicSchool school = MagicSchool::None;          // For FortifyMagicSchool
      CombatSkill combatSkill = CombatSkill::None;     // For FortifyCombatSkill
      UtilitySkill utilitySkill = UtilitySkill::None;  // For FortifyUtilitySkill
      ElementType element = ElementType::None;         // For resist/weakness effects

      // Soul gem fill state (v0.10.0)
      bool isFilled = false;  // True if soul gem contains a soul (for weapon recharge)

      // =======================================================================
      // HELPER METHODS
      // =======================================================================

      // Check if this is a Fortify Destruction potion
      [[nodiscard]] bool IsFortifyDestruction() const noexcept {
      return HasTag(tags, ItemTag::FortifyMagicSchool) &&
             school == MagicSchool::Destruction;
      }

      // Check if this is a Fortify [specific school] potion
      [[nodiscard]] bool IsFortifySchool(MagicSchool targetSchool) const noexcept {
      return HasTag(tags, ItemTag::FortifyMagicSchool) &&
             school == targetSchool;
      }

      // Check if this is a Fortify Smithing potion (at forge)
      [[nodiscard]] bool IsFortifySmithing() const noexcept {
      return HasTag(tags, ItemTag::FortifyCombatSkill) &&
             combatSkill == CombatSkill::Smithing;
      }

      // Check if this is a Fortify [specific combat skill] potion
      [[nodiscard]] bool IsFortifyCombat(CombatSkill targetSkill) const noexcept {
      return HasTag(tags, ItemTag::FortifyCombatSkill) &&
             combatSkill == targetSkill;
      }

      // Check if this is a Weakness to [element] poison
      [[nodiscard]] bool IsWeaknessTo(ElementType targetElement) const noexcept {
      return HasTagExt(tagsExt, ItemTagExt::WeaknessElement) &&
             element == targetElement;
      }

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      std::string extras;
      if (school != MagicSchool::None) {
        extras += std::format(" school={}", MagicSchoolToString(school));
      }
      if (combatSkill != CombatSkill::None) {
        extras += std::format(" combat={}", CombatSkillToString(combatSkill));
      }
      if (utilitySkill != UtilitySkill::None) {
        extras += std::format(" utility={}", UtilitySkillToString(utilitySkill));
      }
      if (element != ElementType::None) {
        extras += std::format(" element={}", ElementTypeToString(element));
      }
      if (type == ItemType::SoulGem) {
        extras += std::format(" filled={}", isFilled);
      }

      return std::format(
        "ItemData[id={:08X}, name='{}', type={}, tags={:08X}, tagsExt={:04X}, "
        "magnitude={:.1f}, duration={:.1f}, value={}, hostile={}{}]",
        formID,
        name,
        ItemTypeToString(type),
        std::to_underlying(tags),
        std::to_underlying(tagsExt),
        magnitude,
        duration,
        value,
        isHostile,
        extras);
      }

      // Equality operator
      bool operator==(const ItemData&) const = default;
   };
}
