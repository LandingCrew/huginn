#pragma once

namespace Huginn::Weapon
{
   // =============================================================================
   // WEAPON TYPE (v0.7.6)
   // =============================================================================
   // Primary weapon classification based on RE::WEAPON_TYPE mapping.
   // Used for contextual recommendations (melee vs ranged, one-hand vs two-hand).
   // =============================================================================

   enum class WeaponType : uint8_t
   {
      Unknown = 0,

      // One-handed melee
      OneHandSword,
      OneHandAxe,
      OneHandMace,
      OneHandDagger,

      // Two-handed melee
      TwoHandSword,
      TwoHandAxe,
      TwoHandMace,

      // Ranged
      Bow,
      Crossbow,

      // Special
      Staff
   };

   // =============================================================================
   // AMMO TYPE (v0.7.6)
   // =============================================================================
   // Simple classification for arrows and bolts.
   // =============================================================================

   enum class AmmoType : uint8_t
   {
      Unknown = 0,
      Arrow,
      Bolt
   };

   // =============================================================================
   // WEAPON TAG (v0.7.6)
   // =============================================================================
   // Bitflags for weapon properties and contextual matching.
   // Enables fast filtering: GetSilveredWeapons(), GetEnchantedWeapons(), etc.
   // =============================================================================

   enum class WeaponTag : uint32_t
   {
      None = 0,

      // Combat style
      Melee       = 1 << 0,
      Ranged      = 1 << 1,
      OneHanded   = 1 << 2,
      TwoHanded   = 1 << 3,

      // Material properties (for contextual bonuses)
      Silver      = 1 << 4,    // Bonus vs undead/werewolves
      Daedric     = 1 << 5,    // High-tier material
      Bound       = 1 << 6,    // Conjured weapons

      // Enchantment presence
      Enchanted   = 1 << 7,

      // Enchantment elements (mutually exclusive in most cases)
      EnchantFire         = 1 << 8,
      EnchantFrost        = 1 << 9,
      EnchantShock        = 1 << 10,

      // Special enchantment effects
      EnchantAbsorbHealth     = 1 << 11,
      EnchantAbsorbMagicka    = 1 << 12,
      EnchantAbsorbStamina    = 1 << 13,
      EnchantSoulTrap         = 1 << 14,
      EnchantParalyze         = 1 << 15,
      EnchantFear             = 1 << 16,
      EnchantTurnUndead       = 1 << 17,
      EnchantBanish           = 1 << 18,
      EnchantSilence          = 1 << 19,

      // Weapon state
      NeedsCharge = 1 << 20,   // Enchanted weapon with low charge

      // Ammo-specific tags
      MagicAmmo   = 1 << 21    // Enchanted arrows/bolts
   };

   // Enable bitwise operations on WeaponTag
   // Using constexpr noexcept for compile-time evaluation and optimization
   inline constexpr WeaponTag operator|(WeaponTag a, WeaponTag b) noexcept
   {
      return static_cast<WeaponTag>(std::to_underlying(a) | std::to_underlying(b));
   }

   inline constexpr WeaponTag operator&(WeaponTag a, WeaponTag b) noexcept
   {
      return static_cast<WeaponTag>(std::to_underlying(a) & std::to_underlying(b));
   }

   inline constexpr WeaponTag& operator|=(WeaponTag& a, WeaponTag b) noexcept
   {
      a = a | b;
      return a;
   }

   inline constexpr WeaponTag& operator&=(WeaponTag& a, WeaponTag b) noexcept
   {
      a = a & b;
      return a;
   }

   inline constexpr WeaponTag operator~(WeaponTag a) noexcept
   {
      return static_cast<WeaponTag>(~std::to_underlying(a));
   }

   inline constexpr bool HasTag(WeaponTag tags, WeaponTag check) noexcept
   {
      return std::to_underlying(tags & check) != 0;
   }

   // =============================================================================
   // STRING CONVERSION HELPERS
   // =============================================================================

   inline std::string_view WeaponTypeToString(WeaponType type)
   {
      switch (type) {
      case WeaponType::OneHandSword:  return "OneHandSword";
      case WeaponType::OneHandAxe:    return "OneHandAxe";
      case WeaponType::OneHandMace:   return "OneHandMace";
      case WeaponType::OneHandDagger: return "OneHandDagger";
      case WeaponType::TwoHandSword:  return "TwoHandSword";
      case WeaponType::TwoHandAxe:    return "TwoHandAxe";
      case WeaponType::TwoHandMace:   return "TwoHandMace";
      case WeaponType::Bow:           return "Bow";
      case WeaponType::Crossbow:      return "Crossbow";
      case WeaponType::Staff:         return "Staff";
      default:                        return "Unknown";
      }
   }

   inline std::string_view AmmoTypeToString(AmmoType type)
   {
      switch (type) {
      case AmmoType::Arrow: return "Arrow";
      case AmmoType::Bolt:  return "Bolt";
      default:              return "Unknown";
      }
   }

   // =============================================================================
   // WEAPON DATA (v0.7.6)
   // =============================================================================
   // Weapon metadata for Q-learning and filtering.
   // Represents a favorited weapon in the player's inventory.
   // =============================================================================

   struct WeaponData
   {
      RE::FormID formID = 0;       // Unique weapon form ID
      uint16_t uniqueID = 0;       // ExtraUniqueID for Wheeler (identifies specific inventory instance)
      std::string name;            // Weapon name for display
      WeaponType type = WeaponType::Unknown;   // Primary type classification
      WeaponTag tags = WeaponTag::None;        // Contextual tags (bitflags)

      // Combat stats
      float baseDamage = 0.0f;     // Base damage (unmodified by perks/skills)
      float speed = 1.0f;          // Attack speed multiplier
      float reach = 1.0f;          // Reach multiplier

      // Enchantment info
      bool hasEnchantment = false;
      float currentCharge = 0.0f;  // Current enchantment charge (0-100%)
      float maxCharge = 0.0f;      // Maximum charge capacity

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "WeaponData[id={:08X}, name='{}', type={}, tags={:08X}, dmg={:.1f}, spd={:.2f}, ench={}, charge={:.0f}%]",
        formID,
        name,
        WeaponTypeToString(type),
        std::to_underlying(tags),
        baseDamage,
        speed,
        hasEnchantment,
        currentCharge * 100.0f);
      }

      // Equality operator
      bool operator==(const WeaponData&) const = default;
   };

   // =============================================================================
   // AMMO DATA (v0.7.6)
   // =============================================================================
   // Ammunition metadata for bow/crossbow recommendations.
   // Tracks arrow/bolt types and their enchantments.
   // =============================================================================

   struct AmmoData
   {
      RE::FormID formID = 0;       // Unique ammo form ID
      std::string name;            // Ammo name for display
      AmmoType type = AmmoType::Unknown;       // Arrow or Bolt
      WeaponTag tags = WeaponTag::None;        // Enchantment tags (reuse WeaponTag)

      // Combat stats
      float baseDamage = 0.0f;     // Base damage bonus

      // Enchantment info
      bool hasEnchantment = false;

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "AmmoData[id={:08X}, name='{}', type={}, tags={:08X}, dmg={:.1f}, ench={}]",
        formID,
        name,
        AmmoTypeToString(type),
        std::to_underlying(tags),
        baseDamage,
        hasEnchantment);
      }

      // Equality operator
      bool operator==(const AmmoData&) const = default;
   };

   // =============================================================================
   // INVENTORY WRAPPERS (v0.7.6)
   // =============================================================================
   // Track weapons and ammo in the player's favorites/inventory.
   // Unlike items, weapons don't have "counts" but track enchantment charge.
   // =============================================================================

   struct InventoryWeapon
   {
      WeaponData data;             // Classification data
      bool isFavorited = false;    // Is in favorites menu
      bool isEquipped = false;     // Currently equipped
      float previousCharge = 0.0f; // Charge at last poll (for delta detection)

      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "InventoryWeapon[{}, fav={}, eq={}, charge={:.0f}%]",
        data.name,
        isFavorited,
        isEquipped,
        data.currentCharge * 100.0f);
      }
   };

   struct InventoryAmmo
   {
      AmmoData data;               // Classification data
      int32_t count = 0;           // Current inventory count
      bool isEquipped = false;     // Currently equipped

      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "InventoryAmmo[{}, count={}, eq={}]",
        data.name,
        count,
        isEquipped);
      }
   };
}
