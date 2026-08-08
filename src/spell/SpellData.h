#pragma once

namespace Huginn::Spell
{
   // Primary spell type classification
   enum class SpellType : uint8_t
   {
      Unknown = 0,
      Healing,       // Restoration healing spells
      Damage,        // Destruction damage spells
      Defensive,     // Armor/ward spells
      Utility,       // Utility spells (detect, light, etc.)
      Summon,        // Conjuration summons
      Buff,          // Enhancement spells (Courage, Muffle, etc.)
      Debuff         // Weakening spells (Paralyze, Calm, etc.)
   };

   // Spell tags for contextual matching (bitflags)
   enum class SpellTag : uint32_t
   {
      None = 0,

      // Damage types
      Fire = 1 << 0,
      Frost = 1 << 1,
      Shock = 1 << 2,
      Poison = 1 << 3,
      Sun = 1 << 4,  // Anti-undead

      // Range/area
      Ranged = 1 << 5,
      Melee = 1 << 6,
      AOE = 1 << 7,
      Concentration = 1 << 8,

      // Special properties
      AntiUndead = 1 << 9,
      AntiDaedra = 1 << 10,
      Stealth = 1 << 11,  // Muffle, Invisibility
      Conjuration = 1 << 12,

      // Restoration specific
      RestoreHealth = 1 << 13,
      RestoreMagicka = 1 << 14,
      RestoreStamina = 1 << 15,
      Ward = 1 << 16,
      TurnUndead = 1 << 17,

      // Alteration specific
      Armor = 1 << 18,
      DetectLife = 1 << 19,
      Light = 1 << 20,
      Telekinesis = 1 << 21,
      Paralysis = 1 << 22,

      // Illusion specific
      Calm = 1 << 23,
      Fear = 1 << 24,
      Frenzy = 1 << 25,
      Invisibility = 1 << 26,
      Muffle = 1 << 27,

      // Conjuration specific
      SummonDaedra = 1 << 28,
      SummonUndead = 1 << 29,
      SummonCreature = 1 << 30,
      BoundWeapon = 1u << 31

      // FULL. Anything new goes in SpellTagExt below.
   };

   // =========================================================================
   // EXTENDED SPELL TAGS (#79)
   // =========================================================================
   // SpellTag has all 32 bits spoken for, and that is the whole reason four
   // context weights sat computed-but-unread for as long as they existed.
   // ContextRuleEngine raises unlockWeight, slowFallWeight, antiDragonWeight
   // and waterbreathingWeight; DominantReason names them on the widget; and no
   // candidate could draw on them because there was no bit left to match
   // against. The contexts fired, labelled themselves, and moved no ranking.
   //
   // A second field rather than widening SpellTag to uint64_t: ItemTagExt
   // already solved the identical squeeze on the item side, so this is the
   // shape a reader of this codebase expects, and it groups "the ones that did
   // not fit" instead of hiding them mid-enum.
   //
   // Detection is API-based for three of the four (see DetermineSpellTagsExt).
   // Only AntiDragon is name-only, because no effect archetype describes it.
   enum class SpellTagExt : uint16_t
   {
      None = 0,

      Unlock         = 1 << 0,  // Open Lock / Knock          → unlockWeight
      SlowFall       = 1 << 1,  // Slow Fall / Become Ethereal → slowFallWeight
      AntiDragon     = 1 << 2,  // Dragonrend and friends      → antiDragonWeight
      Waterbreathing = 1 << 3   // → waterbreathingWeight (the potion half already worked)
   };

   // Enable bitwise operations on SpellTag
   inline SpellTag operator|(SpellTag a, SpellTag b)
   {
      return static_cast<SpellTag>(std::to_underlying(a) | std::to_underlying(b));
   }

   inline SpellTag operator&(SpellTag a, SpellTag b)
   {
      return static_cast<SpellTag>(std::to_underlying(a) & std::to_underlying(b));
   }

   inline SpellTag& operator|=(SpellTag& a, SpellTag b)
   {
      a = a | b;
      return a;
   }

   inline bool HasTag(SpellTag tags, SpellTag check)
   {
      return std::to_underlying(tags & check) != 0;
   }

   // Enable bitwise operations on SpellTagExt
   inline SpellTagExt operator|(SpellTagExt a, SpellTagExt b)
   {
      return static_cast<SpellTagExt>(std::to_underlying(a) | std::to_underlying(b));
   }

   inline SpellTagExt operator&(SpellTagExt a, SpellTagExt b)
   {
      return static_cast<SpellTagExt>(std::to_underlying(a) & std::to_underlying(b));
   }

   inline SpellTagExt& operator|=(SpellTagExt& a, SpellTagExt b)
   {
      a = a | b;
      return a;
   }

   // Named HasTagExt, not an overload of HasTag: the two enums are distinct
   // types, so an overload would compile, but a reader skimming
   // `HasTag(c.tags, ...)` at a call site could not tell which set was meant.
   inline bool HasTagExt(SpellTagExt tags, SpellTagExt check)
   {
      return std::to_underlying(tags & check) != 0;
   }

   // Magic school classification (from GetMagickSkill() API)
   // Used for school-specific recommendations and perk interactions
   enum class MagicSchool : uint8_t
   {
      Unknown = 0,
      Destruction,
      Restoration,
      Alteration,
      Illusion,
      Conjuration
   };

   // Element type for damage/resist spells (from resistVariable API)
   // Single enum (not bitflags) - uses dominant element for multi-element spells
   enum class ElementType : uint8_t
   {
      None = 0,
      Fire,
      Frost,
      Shock,
      Poison,
      Sun,    // Anti-undead (Dawnguard)
      Magic   // Generic magic damage (no resist)
   };

   // MagicSchool to string (for logging)
   inline std::string_view MagicSchoolToString(MagicSchool school)
   {
      switch (school) {
      case MagicSchool::Destruction:  return "Destruction";
      case MagicSchool::Restoration:  return "Restoration";
      case MagicSchool::Alteration:   return "Alteration";
      case MagicSchool::Illusion:     return "Illusion";
      case MagicSchool::Conjuration:  return "Conjuration";
      default:                        return "Unknown";
      }
   }

   // ElementType to string (for logging)
   inline std::string_view ElementTypeToString(ElementType element)
   {
      switch (element) {
      case ElementType::Fire:    return "Fire";
      case ElementType::Frost:   return "Frost";
      case ElementType::Shock:   return "Shock";
      case ElementType::Poison:  return "Poison";
      case ElementType::Sun:     return "Sun";
      case ElementType::Magic:   return "Magic";
      default:                   return "None";
      }
   }

   // Spell type to string (for logging) - defined before SpellData for use in ToString()
   inline std::string_view SpellTypeToString(SpellType type)
   {
      switch (type) {
      case SpellType::Healing:    return "Healing";
      case SpellType::Damage:     return "Damage";
      case SpellType::Defensive:  return "Defensive";
      case SpellType::Utility:    return "Utility";
      case SpellType::Summon:     return "Summon";
      case SpellType::Buff:       return "Buff";
      case SpellType::Debuff:     return "Debuff";
      default:                    return "Unknown";
      }
   }

   // Spell metadata for Q-learning and filtering
   struct SpellData
   {
      RE::FormID formID;           // Unique spell form ID
      std::string name;            // Spell name for display
      SpellType type;              // Primary type classification
      SpellTag tags;               // Contextual tags (bitflags)
      SpellTagExt tagsExt = SpellTagExt::None;  // Extended tags — the four that didn't fit (#79)
      MagicSchool school;          // Magic school (v0.7.1)
      ElementType element;         // Element type for damage/resist (v0.7.1)
      uint32_t baseCost;           // Magicka cost (unmodified by perks)
      bool isConcentration;        // Continuous vs one-shot
      float range;                 // Max effective range (0 = self/touch)
      bool isFavorited = false;    // Is in favorites menu (v0.7.8)

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "SpellData[id={:08X}, name='{}', type={}, school={}, element={}, tags={:08X}, tagsExt={:04X}, cost={}, concentration={}, range={}, fav={}]",
        formID,
        name,
        SpellTypeToString(type),
        MagicSchoolToString(school),
        ElementTypeToString(element),
        std::to_underlying(tags),
        std::to_underlying(tagsExt),
        baseCost,
        isConcentration,
        range,
        isFavorited);
      }

      // Equality operator
      bool operator==(const SpellData&) const = default;
   };
}
