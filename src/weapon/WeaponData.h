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
      // (Skyrim has no distinct warhammer WEAPON_TYPE — warhammers report as
      //  kTwoHandAxe, so there is no TwoHandMace value to map them to.)
      TwoHandSword,
      TwoHandAxe,

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
   // Weapon metadata for contextual bandit and filtering.
   // Represents a favorited weapon in the player's inventory.
   // =============================================================================

   struct WeaponData
   {
      RE::FormID formID = 0;       // Weapon BASE form ID - shared by every instance
      uint16_t uniqueID = 0;       // ExtraUniqueID of THIS inventory stack (0 if it has none)
      std::string name;            // Display name - the stack's own if it was tempered or renamed
      WeaponType type = WeaponType::Unknown;   // Primary type classification
      WeaponTag tags = WeaponTag::None;        // Contextual tags (bitflags)

      // Combat stats
      //
      // Three fields rather than one because tempering is per-INSTANCE while
      // classification is per-base-form. The classifier only ever sees the base
      // form, so it fills baseDamage; the registry multiplies in the temper
      // factor it read off this stack's ExtraHealth.
      //
      // Rank and display on `damage`. baseDamage exists so a re-temper can
      // recompute without a reclassify, and so the log can print both - which is
      // how the temper model gets checked against what the game shows.
      float baseDamage = 0.0f;     // Base form's damage, before tempering
      float temperFactor = 1.0f;   // ExtraHealth on this stack; 1.0 = untempered
      float damage = 0.0f;         // baseDamage x temperFactor - the effective number
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
        "WeaponData[id={:08X}/uid={}, name='{}', type={}, tags={:08X}, dmg={:.1f} (base {:.1f} x{:.2f}), spd={:.2f}, ench={}, charge={:.0f}%]",
        formID,
        uniqueID,
        name,
        WeaponTypeToString(type),
        std::to_underlying(tags),
        damage,
        baseDamage,
        temperFactor,
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

   /// The registry key for one inventory stack, packed from the pair that
   /// identifies it. InventoryWeapon::Key() is the same expression; this exists
   /// for the callers that hold a formID and a uniqueID but no entry yet.
   [[nodiscard]] inline constexpr uint64_t MakeWeaponKey(RE::FormID formID, uint16_t uniqueID) noexcept
   {
      return (static_cast<uint64_t>(uniqueID) << 32) | static_cast<uint64_t>(formID);
   }

   struct InventoryWeapon
   {
      WeaponData data;             // Classification data
      bool isFavorited = false;    // Is in favorites menu
      bool isEquipped = false;     // Currently equipped
      float previousCharge = 0.0f; // Charge at last poll (for delta detection)

      /// Registry key. ONE ENTRY PER INVENTORY STACK, not per base form.
      ///
      /// A tempered Iron Mace and a plain one are both formID 0x1399C, and
      /// keying on the form alone collapsed them into a single record whose
      /// instance fields - name, damage, charge, uniqueID, isEquipped - were
      /// whichever instance the scan resolved last. The Wheeler push keys on
      /// uniqueID, so that record could hand Wheeler the uid of the copy the
      /// player did not want.
      ///
      /// Same layout as InventoryApparel::Key() and
      /// CandidateBase::GetDeduplicationKey(), deliberately: the three have to
      /// agree on what counts as one thing.
      ///
      /// The LEARNER does not, and that is a decision rather than an oversight.
      /// FeatureBanditLearner is keyed on FormID alone, so both Iron Daggers
      /// share one weight vector. Tempering does not change what a weapon is
      /// FOR, and splitting the weights would halve the evidence behind each
      /// one and reset a weapon's learned preference every time the player
      /// visits a grindstone. Identity is per stack; preference is per form.
      [[nodiscard]] uint64_t Key() const noexcept {
      return MakeWeaponKey(data.formID, data.uniqueID);
      }

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

   // =============================================================================
   // EQUIPPED WEAPONS CACHE (v0.7.19)
   // =============================================================================
   // Caches currently equipped weapons to avoid redundant GetEquippedObject() calls.
   // Query once per update cycle and pass to methods that need equipped weapon info.
   // Lives here (not in WeaponRegistry.h) so consumers like UpdateLoop can use it
   // without pulling in the whole registry.
   // =============================================================================

   struct EquippedWeapons
   {
      RE::TESObjectWEAP* rightHand = nullptr;
      RE::TESObjectWEAP* leftHand = nullptr;

      /**
       * @brief Query current equipped weapons from player
       * @param player Player character (must not be null)
       * @return EquippedWeapons with right/left hand weapons (may be nullptr if empty)
       */
      [[nodiscard]] static EquippedWeapons Query(RE::PlayerCharacter* player)
      {
      EquippedWeapons eq;
      if (!player) return eq;

      if (auto* rightObj = player->GetEquippedObject(false)) {
        eq.rightHand = rightObj->As<RE::TESObjectWEAP>();
      }
      if (auto* leftObj = player->GetEquippedObject(true)) {
        eq.leftHand = leftObj->As<RE::TESObjectWEAP>();
      }
      return eq;
      }

      /**
       * @brief Check if a weapon is currently equipped in either hand
       */
      [[nodiscard]] bool IsEquipped(const RE::TESObjectWEAP* weapon) const noexcept
      {
      return weapon && (weapon == rightHand || weapon == leftHand);
      }
   };
}
