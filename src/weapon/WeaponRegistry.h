#pragma once

#include "WeaponData.h"
#include "WeaponClassifier.h"
#include <shared_mutex>  // v0.7.12 - thread safety
#include <atomic>        // M3 v0.7.21 - atomic m_isLoading
#include <type_traits>   // For ForEach visitor pattern

namespace Huginn::Weapon
{
   // =============================================================================
   // EQUIPPED WEAPONS CACHE (v0.7.19)
   // =============================================================================
   // Caches currently equipped weapons to avoid redundant GetEquippedObject() calls.
   // Query once per update cycle and pass to methods that need equipped weapon info.
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

   // =============================================================================
   // WEAPON REGISTRY (v0.7.6)
   // =============================================================================
   // Tracks all weapons and ammo in the player's inventory.
   //
   // KEY DIFFERENCES FROM ITEMREGISTRY:
   //   - Tracks enchantment charge instead of item count
   //   - Dual storage for weapons and ammo
   //
   // TRACKING:
   //   - All weapons in player inventory are tracked
   //   - isFavorited: true if starred in Favorites menu (ExtraFavorite)
   //   - isEquipped: true if currently in left or right hand
   //
   // THREAD SAFETY:
   //   - Read-write locking via shared_mutex (same pattern as SpellRegistry)
   // =============================================================================

   class WeaponRegistry
   {
   public:
      WeaponRegistry();
      ~WeaponRegistry() = default;

      // Disable copy/move
      WeaponRegistry(const WeaponRegistry&) = delete;
      WeaponRegistry& operator=(const WeaponRegistry&) = delete;
      WeaponRegistry(WeaponRegistry&&) = delete;
      WeaponRegistry& operator=(WeaponRegistry&&) = delete;

      // =============================================================================
      // LIFECYCLE
      // =============================================================================

      /**
       * @brief Full inventory scan on game load
       * @note Clears existing registry and rescans all favorited weapons
       */
      void RebuildRegistry();

      /**
       * @brief Refresh weapon charge levels and equipped status
       * @note Call at 500ms intervals for charge tracking
       */
      void RefreshCharges();

      /**
       * @brief Refresh weapon charge levels and equipped status (with cached equipped weapons)
       * @param equipped Pre-queried equipped weapons (avoids redundant GetEquippedObject calls)
       * @note OPTIMIZATION (v0.7.19): Use when caller already has equipped weapons cached
       */
      void RefreshCharges(const EquippedWeapons& equipped);

      /**
       * @brief Full reconciliation - add new favorites, remove unfavorited
       * @return Number of weapons added or removed
       * @note Call at 30s intervals or after favorites menu closes
       */
      size_t ReconcileWeapons();

      /**
       * @brief Full reconciliation with cached equipped weapons
       * @param equipped Pre-queried equipped weapons (avoids redundant GetEquippedObject calls)
       * @return Number of weapons added or removed
       * @note OPTIMIZATION (v0.7.19): Use when caller already has equipped weapons cached
       */
      size_t ReconcileWeapons(const EquippedWeapons& equipped);

      // =============================================================================
      // WEAPON ACCESSORS
      // =============================================================================

      /**
       * @brief Get weapon by FormID
       * @param formID The weapon's form ID
       * @return Pointer to InventoryWeapon, or nullptr if not found
       */
      [[nodiscard]] const InventoryWeapon* GetWeapon(RE::FormID formID) const;

      /**
       * @brief Get all melee weapons
       * @return Vector of pointers to melee weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetMeleeWeapons() const;

      /**
       * @brief Get all ranged weapons (bows, crossbows)
       * @return Vector of pointers to ranged weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetRangedWeapons() const;

      /**
       * @brief Get all one-handed weapons
       * @return Vector of pointers to one-handed weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetOneHandedWeapons() const;

      /**
       * @brief Get all two-handed weapons
       * @return Vector of pointers to two-handed weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetTwoHandedWeapons() const;

      /**
       * @brief Get all silvered weapons (bonus vs undead)
       * @return Vector of pointers to silver weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetSilveredWeapons() const;

      /**
       * @brief Get all enchanted weapons
       * @return Vector of pointers to enchanted weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetEnchantedWeapons() const;

      /**
       * @brief Get weapons that need recharging (< 20% charge)
       * @return Vector of pointers to low-charge weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetWeaponsNeedingCharge() const;

      /**
       * @brief Get weapons with a specific tag
       * @param tag The WeaponTag to filter by
       * @return Vector of pointers to matching weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetWeaponsWithTag(WeaponTag tag) const;

      // =============================================================================
      // STAFF ACCESSORS (StaffScanner v0.7.7)
      // =============================================================================

      /**
       * @brief Get all staves
       * @return Vector of pointers to staff weapons
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetStaffs() const;

      /**
       * @brief Get all enchanted staves
       * @return Vector of pointers to enchanted staves
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetEnchantedStaffs() const;

      /**
       * @brief Get fire staves (EnchantFire tag)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to fire staves, sorted by damage descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetFireStaffs(size_t topK = 3) const;

      /**
       * @brief Get frost staves (EnchantFrost tag)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to frost staves, sorted by damage descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetFrostStaffs(size_t topK = 3) const;

      /**
       * @brief Get shock staves (EnchantShock tag)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to shock staves, sorted by damage descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryWeapon*> GetShockStaffs(size_t topK = 3) const;

      /**
       * @brief Get best staff by damage
       * @return Pointer to highest-damage staff, or nullptr if none
       */
      [[nodiscard]] const InventoryWeapon* GetBestStaff() const noexcept;

      /**
       * @brief Get staff with highest charge percentage
       * @return Pointer to staff with most charge, or nullptr if none
       * @note Useful for preferring fully-charged staves
       */
      [[nodiscard]] const InventoryWeapon* GetHighestChargeStaff() const noexcept;

      // =============================================================================
      // CONVENIENCE "BEST" ACCESSORS - O(n) single-pass max-find
      // =============================================================================

      /**
       * @brief Get the best melee weapon by damage
       * @return Pointer to highest-damage melee weapon, or nullptr if none
       */
      [[nodiscard]] const InventoryWeapon* GetBestMeleeWeapon() const noexcept;

      /**
       * @brief Get the best ranged weapon by damage
       * @return Pointer to highest-damage ranged weapon, or nullptr if none
       */
      [[nodiscard]] const InventoryWeapon* GetBestRangedWeapon() const noexcept;

      /**
       * @brief Get the best silvered weapon by damage
       * @return Pointer to highest-damage silver weapon, or nullptr if none
       */
      [[nodiscard]] const InventoryWeapon* GetBestSilveredWeapon() const noexcept;

      // =============================================================================
      // AMMO ACCESSORS
      // =============================================================================

      /**
       * @brief Get ammo by FormID
       * @param formID The ammo's form ID
       * @return Pointer to InventoryAmmo, or nullptr if not found
       */
      [[nodiscard]] const InventoryAmmo* GetAmmo(RE::FormID formID) const;

      /**
       * @brief Get all arrows
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to arrows, sorted by damage descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryAmmo*> GetArrows(size_t topK = 3) const;

      /**
       * @brief Get all bolts (crossbow ammo)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to bolts, sorted by damage descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryAmmo*> GetBolts(size_t topK = 3) const;

      /**
       * @brief Get all magic arrows/bolts (enchanted ammo)
       * @return Vector of pointers to enchanted ammo
       */
      [[nodiscard]] std::vector<const InventoryAmmo*> GetMagicAmmo() const;

      /**
       * @brief Get all silver arrows/bolts (v0.7.8)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to silver ammo, sorted by damage descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryAmmo*> GetSilverAmmo(size_t topK = 3) const;

      /**
       * @brief Get the best arrow by damage
       * @return Pointer to highest-damage arrow, or nullptr if none
       */
      [[nodiscard]] const InventoryAmmo* GetBestArrow() const noexcept;

      /**
       * @brief Get the best silver arrow by damage (v0.7.8)
       * @return Pointer to highest-damage silver arrow, or nullptr if none
       */
      [[nodiscard]] const InventoryAmmo* GetBestSilverArrow() const noexcept;

      /**
       * @brief Get the best bolt by damage
       * @return Pointer to highest-damage bolt, or nullptr if none
       */
      [[nodiscard]] const InventoryAmmo* GetBestBolt() const noexcept;

      // =============================================================================
      // COUNTS AND STATE
      // =============================================================================

      /**
       * @brief Get total number of tracked weapons
       */
      [[nodiscard]] size_t GetWeaponCount() const noexcept;

      /**
       * @brief Get total number of tracked ammo types
       */
      [[nodiscard]] size_t GetAmmoCount() const noexcept;

      /**
       * @brief Get all tracked weapons (returns copy for thread safety - v0.7.12)
       */
      [[nodiscard]] std::vector<InventoryWeapon> GetAllWeapons() const;

      /**
       * @brief Iterate over all weapons without copying (zero-allocation visitor pattern)
       * @tparam Func Callable with signature void(const InventoryWeapon&) or bool(const InventoryWeapon&)
       * @param func Function to call for each weapon. If returns bool, iteration stops on false.
       * @note Thread-safe: Holds shared_lock during iteration
       * @note PERFORMANCE: Use this instead of GetAllWeapons() in hot paths
       */
      template<typename Func>
      void ForEachWeapon(Func&& func) const
      {
      std::shared_lock lock(m_mutex);
      for (const auto& weapon : m_weapons) {
        if constexpr (std::is_same_v<std::invoke_result_t<Func, const InventoryWeapon&>, bool>) {
           if (!func(weapon)) return;
        } else {
           func(weapon);
        }
      }
      }

      /**
       * @brief Get all tracked ammo (returns copy for thread safety - v0.7.12)
       */
      [[nodiscard]] std::vector<InventoryAmmo> GetAllAmmo() const;

      /**
       * @brief Iterate over all ammo without copying (zero-allocation visitor pattern)
       * @tparam Func Callable with signature void(const InventoryAmmo&) or bool(const InventoryAmmo&)
       * @param func Function to call for each ammo. If returns bool, iteration stops on false.
       * @note Thread-safe: Holds shared_lock during iteration
       * @note PERFORMANCE: Use this instead of GetAllAmmo() in hot paths
       */
      template<typename Func>
      void ForEachAmmo(Func&& func) const
      {
      std::shared_lock lock(m_mutex);
      for (const auto& ammo : m_ammo) {
        if constexpr (std::is_same_v<std::invoke_result_t<Func, const InventoryAmmo&>, bool>) {
           if (!func(ammo)) return;
        } else {
           func(ammo);
        }
      }
      }

      /**
       * @brief Check if registry is currently loading/rebuilding
       * M3 (v0.7.21): Use atomic load with acquire semantics
       */
      [[nodiscard]] bool IsLoading() const noexcept { return m_isLoading.load(std::memory_order_acquire); }

      // =============================================================================
      // DEBUG
      // =============================================================================

      /**
       * @brief Log all tracked weapons and ammo to debug log
       */
      void LogAllWeapons() const;

   private:
      // =============================================================================
      // INTERNAL HELPERS
      // =============================================================================

      /**
       * @brief Scan player inventory for favorited weapons
       * @return Vector of (weapon, isFavorited, isEquipped) tuples
       */
      struct ScannedWeapon {
      RE::TESObjectWEAP* weapon;
      bool isFavorited;
      bool isEquipped;
      float currentCharge;
      float maxCharge;
      uint16_t uniqueID;
      };
      [[nodiscard]] std::vector<ScannedWeapon> ScanPlayerWeapons() const;

      /**
       * @brief Scan for favorited weapon FormIDs (items in Favorites menu)
       * @return Set of FormIDs for weapons that are favorited (starred or hotkeyed)
       * @note Uses ExtraHotkey detection - works for both starred and numbered favorites
       * @note This is the weapon equivalent of SpellRegistry::ScanSpellFavorites()
       */
      [[nodiscard]] std::unordered_set<RE::FormID> ScanWeaponFavorites() const;

      /**
       * @brief Scan player inventory with pre-cached equipped weapons (v0.7.19)
       * @param equipped Pre-queried equipped weapons
       * @return Vector of scanned weapons
       */
      [[nodiscard]] std::vector<ScannedWeapon> ScanPlayerWeapons(const EquippedWeapons& equipped) const;

      /**
       * @brief Scan player inventory for ammo
       * @return Vector of (ammo, count, isEquipped) tuples
       */
      struct ScannedAmmo {
      RE::TESAmmo* ammo;
      int32_t count;
      bool isEquipped;
      };
      [[nodiscard]] std::vector<ScannedAmmo> ScanPlayerAmmo() const;

      /**
       * @brief Add a weapon to the registry
       * @param weapon The weapon to add
       * @param isFavorited Is the weapon in favorites
       * @param isEquipped Is the weapon currently equipped
       * @param currentCharge Current enchantment charge (0-maxCharge)
       * @param maxCharge Maximum enchantment charge
       */
      void AddWeapon(RE::TESObjectWEAP* weapon, bool isFavorited, bool isEquipped,
                     float currentCharge, float maxCharge, uint16_t uniqueID = 0);

      /**
       * @brief Add ammo to the registry
       * @param ammo The ammo to add
       * @param count Current inventory count
       * @param isEquipped Is the ammo currently equipped
       */
      void AddAmmo(RE::TESAmmo* ammo, int32_t count, bool isEquipped);

      /**
       * @brief Remove a weapon from registry by FormID
       * @return true if removed, false if not found
       */
      bool RemoveWeapon(RE::FormID formID);

      /**
       * @brief Remove ammo from registry by FormID
       * @return true if removed, false if not found
       */
      bool RemoveAmmo(RE::FormID formID);

      /**
       * @brief Extract weapon metadata from inventory entry (favorites, charge, equipped status)
       * @param weapon The weapon to extract metadata for
       * @param entry The inventory entry containing extraLists data
       * @param includeExtraLists If true, access extraLists for favorites/charge (requires 500ms+ after load)
       * @param equipped Currently equipped weapons (for isEquipped check)
       * @return ScannedWeapon with all metadata filled
       * @note Helper to eliminate code duplication between RefreshCharges() and ReconcileWeapons()
       * @note UPDATED (v0.7.19): Now accepts EquippedWeapons struct instead of separate pointers
       */
      [[nodiscard]] ScannedWeapon ExtractWeaponMetadata(
      RE::TESObjectWEAP* weapon,
      RE::InventoryEntryData* entry,
      bool includeExtraLists,
      const EquippedWeapons& equipped) const;

      // =============================================================================
      // STORAGE
      // =============================================================================

      // Dual-index storage for weapons (same pattern as SpellRegistry/ItemRegistry)
      std::vector<InventoryWeapon> m_weapons;
      std::unordered_map<RE::FormID, size_t> m_weaponIndex;

      // Dual-index storage for ammo
      std::vector<InventoryAmmo> m_ammo;
      std::unordered_map<RE::FormID, size_t> m_ammoIndex;

      // Weapon classifier instance
      WeaponClassifier m_classifier;

      // Thread safety (v0.7.12)
      // Protects m_weapons, m_weaponIndex, m_ammo, m_ammoIndex from concurrent access
      // Readers (render thread) use shared_lock, writers (update thread) use unique_lock
      mutable std::shared_mutex m_mutex;

      // Loading state flag (M3 v0.7.21: atomic for thread-safe access)
      std::atomic<bool> m_isLoading{false};
   };
}
