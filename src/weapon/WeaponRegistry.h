#pragma once

#include "WeaponData.h"
#include "WeaponClassifier.h"
#include <shared_mutex>  // v0.7.12 - thread safety
#include <atomic>        // M3 v0.7.21 - atomic m_isLoading
#include <type_traits>   // For ForEach visitor pattern

namespace Huginn::Weapon
{
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

      /// One inventory stack that left the player's possession, as the update
      /// loop needs to hear about it: the form to look up, and the stack to
      /// name. uniqueID is 0 for ammo and for weapons the game never gave an
      /// ExtraUniqueID, which SlotLocker reads as "every stack of this form".
      struct DepartedStack
      {
      RE::FormID formID = 0;
      uint16_t uniqueID = 0;
      };

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
       * @param departed Optional out-param, appended to: ammo whose count went
       *        from positive to zero on THIS pass (uniqueID 0 - ammo is named by
       *        form). The update loop uses these exactly as it uses
       *        ReconcileWeapons' departures - break the slot lock, force one
       *        recompute - so an empty quiver leaves the widget in 500 ms rather
       *        than at the 30 s reconcile.
       * @note OPTIMIZATION (v0.7.19): Use when caller already has equipped weapons cached
       */
      void RefreshCharges(const EquippedWeapons& equipped,
                          std::vector<DepartedStack>* departed = nullptr);

      /**
       * @brief Full reconciliation - add new favorites, remove unfavorited
       * @return Number of weapons added or removed
       * @note Call at 30s intervals or after favorites menu closes
       */
      size_t ReconcileWeapons();

      /**
       * @brief Full reconciliation with cached equipped weapons
       * @param equipped Pre-queried equipped weapons (avoids redundant GetEquippedObject calls)
       * @param departed Optional out-param, appended to: every inventory STACK
       *        removed this pass, plus the ammo forms removed. The update loop
       *        hands these to SlotLocker::OnItemUsed so a lock stops pinning
       *        something the player no longer owns.
       *
       *        Per stack, not per form, because the player can own two of one
       *        form: dropping the tempered Iron Dagger must free the slot that
       *        named THAT dagger while leaving the plain one's alone.
       * @return Number of weapons added or removed
       * @note OPTIMIZATION (v0.7.19): Use when caller already has equipped weapons cached
       */
      size_t ReconcileWeapons(const EquippedWeapons& equipped,
                              std::vector<DepartedStack>* departed = nullptr);

      // =============================================================================
      // WEAPON ACCESSORS
      // =============================================================================
      //
      // LIFETIME CONTRACT: the pointer- and vector-of-pointer-returning accessors
      // below hand out raw pointers into m_weapons/m_ammo, but the shared_lock is
      // released when the accessor returns. A subsequent write (RefreshCharges,
      // ReconcileWeapons) can reallocate the backing vector or swap-remove an
      // element, dangling those pointers. They are therefore only safe to use
      // synchronously on the update thread, before yielding to the next write.
      // Do NOT cache them across frames or hand them to another thread; for
      // cross-thread/persistent use, copy via GetAllWeapons()/GetAllAmmo() or
      // ForEachWeapon()/ForEachAmmo() (which hold the lock for the whole visit).

      /**
       * @brief Get one tracked inventory stack by its composite key
       * @param formID The weapon's BASE form ID
       * @param uniqueID ExtraUniqueID of the stack wanted; 0 for the entry that
       *        has no ExtraUniqueID of its own (plain stock of that form)
       * @return Pointer to InventoryWeapon, or nullptr if not tracked
       * @note The registry holds one entry PER STACK (see InventoryWeapon::Key),
       *       so a bare FormID is not enough to name a tempered or enchanted
       *       instance.
       */
      [[nodiscard]] const InventoryWeapon* GetWeapon(RE::FormID formID, uint16_t uniqueID = 0) const;

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

      /// One inventory STACK, resolved far enough to register.
      ///
      /// One per ExtraDataList, not one per base form. Util::GetInventorySafe
      /// returns a map keyed by TESBoundObject*, so the inventory hands us one
      /// entry per base form; folding that entry's extraLists into a single
      /// record made every per-instance field last-wins, and a tempered weapon
      /// and a plain one of the same form could not both be tracked.
      ///
      /// A record with uniqueID 0, temperFactor 1.0 and an empty displayName is
      /// the plain remainder of the stack: the copies carrying no ExtraDataList
      /// of their own. It is also what the load-path scan produces for
      /// everything, because extraLists cannot be read that early.
      struct ScannedWeapon {
      RE::TESObjectWEAP* weapon = nullptr;
      bool isFavorited = false;
      bool isEquipped = false;
      float currentCharge = 0.0f;
      float maxCharge = 0.0f;
      uint16_t uniqueID = 0;
      float temperFactor = 1.0f;   // ExtraHealth; 1.0 when untempered
      float displayDamage = 0.0f;  // PlayerCharacter::GetDamage; 0 when unread
      std::string displayName;     // Empty unless this stack names itself
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

      /// Scanned ammo entry (form, current count, equipped status).
      struct ScannedAmmo {
      RE::TESAmmo* ammo;
      int32_t count;
      bool isEquipped;
      };
      /// Standalone ammo-only scan (used by RebuildRegistry). RefreshCharges and
      /// ReconcileWeapons instead fold ammo into their combined weapon+ammo scan.
      [[nodiscard]] std::vector<ScannedAmmo> ScanPlayerAmmo() const;

      /**
       * @brief Add one scanned stack to the registry
       * @param sw The scanned stack (see ScannedWeapon)
       * @return true if the stack is registered (inserted or updated),
       *         false if null or rejected by classification
       * @note Takes the whole scan record rather than eight positional
       *       arguments; the instance fields travel together or they drift.
       */
      bool AddWeapon(const ScannedWeapon& sw);

      /**
       * @brief Add ammo to the registry
       * @param ammo The ammo to add
       * @param count Current inventory count
       * @param isEquipped Is the ammo currently equipped
       */
      void AddAmmo(RE::TESAmmo* ammo, int32_t count, bool isEquipped);

      /**
       * @brief Remove one tracked stack by its InventoryWeapon::Key()
       * @return true if removed, false if not found
       */
      bool RemoveWeapon(uint64_t key);

      /**
       * @brief Remove ammo from registry by FormID
       * @return true if removed, false if not found
       */
      bool RemoveAmmo(RE::FormID formID);

      /**
       * @brief Build the scan record for ONE inventory stack
       * @param weapon The base form
       * @param extraList The stack's extra data, or nullptr for the plain
       *        remainder (no per-instance data to read)
       * @param equipped Currently equipped weapons - the fallback for isEquipped
       *        when there is no extraList to ask
       * @param resolveName False to skip the display-name lookup. RefreshCharges
       *        runs at 2 Hz and never writes a name, and the lookup is a native
       *        call that can attach an ExtraTextDisplayData the first time.
       * @return ScannedWeapon for that single stack
       * @note Per-stack, not per-entry: ScanWeaponEntry() calls this once for
       *       each extraList. Reading the whole entry here is what collapsed
       *       instances into one last-wins record.
       */
      [[nodiscard]] ScannedWeapon ExtractWeaponMetadata(
      RE::TESObjectWEAP* weapon,
      RE::ExtraDataList* extraList,
      const EquippedWeapons& equipped,
      bool resolveName = true) const;

      /**
       * @brief Append one ScannedWeapon per stack of an inventory entry
       * @param weapon The base form
       * @param entry The inventory entry (may be null)
       * @param count Total count of this form, used to size the plain remainder
       * @param includeExtraLists False while extraLists are unsafe to read
       *        (the 500ms window after a load) - emits a single degraded record
       * @param equipped Currently equipped weapons
       * @param out Destination, appended to
       */
      void ScanWeaponEntry(
      RE::TESObjectWEAP* weapon,
      RE::InventoryEntryData* entry,
      int32_t count,
      bool includeExtraLists,
      const EquippedWeapons& equipped,
      std::vector<ScannedWeapon>& out) const;

      // =============================================================================
      // STORAGE
      // =============================================================================

      // Dual-index storage for weapons (same pattern as SpellRegistry/ItemRegistry).
      // Keyed by InventoryWeapon::Key() - formID alone cannot name an instance.
      std::vector<InventoryWeapon> m_weapons;
      std::unordered_map<uint64_t, size_t> m_weaponIndex;

      // FormIDs whose classification was rejected (see AddWeapon guard). Without this,
      // the periodic scans re-classify and re-log the same unnameable weapon every
      // cycle, since it never enters m_weaponIndex. Cleared on RebuildRegistry.
      std::unordered_set<RE::FormID> m_rejectedWeapons;

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
