#pragma once

#include "WeaponData.h"

namespace Huginn::Weapon
{
   // =============================================================================
   // WEAPON CLASSIFIER (v0.7.6)
   // =============================================================================
   // Analyzes weapon and ammo FormIDs from the game and classifies them
   // by type and tags for use in contextual recommendations.
   //
   // CLASSIFICATION STRATEGY:
   //   1. API-first: Use RE::WEAPON_TYPE, keywords, enchantments
   //   2. Name fallback: Match "Silver", element names for edge cases
   //   3. Override support: User customization via INI file (future)
   // =============================================================================

   class WeaponClassifier
   {
   public:
      WeaponClassifier() = default;
      ~WeaponClassifier() = default;

      // =============================================================================
      // PRIMARY CLASSIFICATION METHODS
      // =============================================================================

      /**
       * @brief Classify a weapon from its SKSE object
       * @param weapon The weapon to classify
       * @return WeaponData with type, tags, and combat stats
       */
      [[nodiscard]] WeaponData ClassifyWeapon(RE::TESObjectWEAP* weapon) const;

      /**
       * @brief Classify ammunition from its SKSE object
       * @param ammo The ammo to classify
       * @return AmmoData with type, tags, and damage
       */
      [[nodiscard]] AmmoData ClassifyAmmo(RE::TESAmmo* ammo) const;

      /**
       * @brief Update charge level for an already-classified weapon
       * @param data The weapon data to update
       * @param weapon The weapon instance with current charge
       * @note Call during refresh to track charge changes without full reclassification
       */
      void UpdateWeaponCharge(WeaponData& data, RE::TESObjectWEAP* weapon) const;

   private:
      // =============================================================================
      // TYPE DETERMINATION
      // =============================================================================

      /**
       * @brief Map RE::WEAPON_TYPE to our WeaponType enum
       * @param weapon The weapon to classify
       * @return WeaponType matching the weapon's game type
       */
      [[nodiscard]] WeaponType DetermineWeaponType(RE::TESObjectWEAP* weapon) const;

      /**
       * @brief Determine ammo type (Arrow vs Bolt)
       * @param ammo The ammo to classify
       * @return AmmoType::Arrow or AmmoType::Bolt
       */
      [[nodiscard]] AmmoType DetermineAmmoType(RE::TESAmmo* ammo) const;

      // =============================================================================
      // TAG DETERMINATION
      // =============================================================================

      /**
       * @brief Generate all contextual tags for a weapon
       * @param weapon The weapon to analyze
       * @param type Already-determined weapon type (for Melee/Ranged inference)
       * @return WeaponTag bitflags combining all detected properties
       */
      [[nodiscard]] WeaponTag DetermineWeaponTags(RE::TESObjectWEAP* weapon, WeaponType type) const;

      /**
       * @brief Generate tags for ammunition
       * @param ammo The ammo to analyze
       * @return WeaponTag bitflags for enchantment effects
       */
      [[nodiscard]] WeaponTag DetermineAmmoTags(RE::TESAmmo* ammo) const;

      // =============================================================================
      // SPECIAL PROPERTY DETECTION
      // =============================================================================

      /**
       * @brief Check if weapon is made of silver (bonus vs undead/werewolves)
       * @param weapon The weapon to check
       * @return true if weapon has silver keyword or name contains "Silver"
       */
      [[nodiscard]] bool IsSilvered(RE::TESObjectWEAP* weapon) const;

      /**
       * @brief Check if weapon is a bound conjuration weapon
       * @param weapon The weapon to check
       * @return true if weapon has bound weapon keyword
       */
      [[nodiscard]] bool IsBound(RE::TESObjectWEAP* weapon) const;

      /**
       * @brief Check if weapon is daedric material
       * @param weapon The weapon to check
       * @return true if weapon has daedric keyword
       */
      [[nodiscard]] bool IsDaedric(RE::TESObjectWEAP* weapon) const;

      // =============================================================================
      // ENCHANTMENT ANALYSIS
      // =============================================================================

      /**
       * @brief Get enchantment from a weapon
       * @param weapon The weapon to check
       * @return Enchantment if present, nullptr otherwise
       */
      [[nodiscard]] RE::EnchantmentItem* GetEnchantment(RE::TESObjectWEAP* weapon) const;

      /**
       * @brief Classify enchantment effects into WeaponTag bitflags
       * @param enchantment The enchantment to analyze
       * @return WeaponTag with element and effect flags set
       */
      [[nodiscard]] WeaponTag ClassifyEnchantmentEffects(RE::EnchantmentItem* enchantment) const;

      /**
       * @brief Get current charge percentage for an enchanted weapon
       * @param weapon The weapon to check
       * @return Charge as 0.0-1.0 percentage, or 1.0 if not applicable
       */
      [[nodiscard]] float GetChargePercentage(RE::TESObjectWEAP* weapon) const;

      // =============================================================================
      // COMBAT STATS
      // =============================================================================

      /**
       * @brief Get base damage value (unmodified by skills/perks)
       * @param weapon The weapon to check
       * @return Base damage from weapon form
       */
      [[nodiscard]] float GetBaseDamage(RE::TESObjectWEAP* weapon) const;

      /**
       * @brief Get attack speed multiplier
       * @param weapon The weapon to check
       * @return Speed multiplier (1.0 = normal)
       */
      [[nodiscard]] float GetSpeed(RE::TESObjectWEAP* weapon) const;

      /**
       * @brief Get weapon reach
       * @param weapon The weapon to check
       * @return Reach multiplier (1.0 = normal)
       */
      [[nodiscard]] float GetReach(RE::TESObjectWEAP* weapon) const;

      // =============================================================================
      // HELPERS
      // =============================================================================

      /**
       * @brief Check if weapon/ammo has a specific keyword
       * @param form The form to check (weapon or ammo)
       * @param keywordEditorID The keyword's editor ID
       * @return true if keyword is present
       */
      [[nodiscard]] bool HasKeyword(RE::TESForm* form, std::string_view keywordEditorID) const;

      /**
       * @brief Check if name contains keyword (case-insensitive)
       * @param name The name to search
       * @param keyword The keyword to find
       * @return true if name contains keyword
       */
      [[nodiscard]] bool NameContains(std::string_view name, std::string_view keyword) const;
   };
}
