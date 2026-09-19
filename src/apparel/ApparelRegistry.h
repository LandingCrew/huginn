#pragma once

#include "ApparelClassifier.h"
#include "ApparelData.h"

#include <atomic>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Huginn::Apparel
{
   // =============================================================================
   // APPAREL REGISTRY (#65)
   // =============================================================================
   // Tracks craft-relevant wearables in the player's inventory: gear that
   // fortifies Alchemy, Smithing or Enchanting. Everything else is rejected at
   // classification and never stored.
   //
   // WHY THIS IS SMALLER THAN WeaponRegistry:
   //   - No charge tracking (apparel has no charge)
   //   - No favorites (nothing reads one — see InventoryApparel)
   //   - No "best of type" accessors (the scorer ranks on magnitude)
   //
   // EXTRALIST DEPENDENCY — the one real difference from every other registry.
   // A weapon's type and damage come from its base form, so WeaponRegistry can
   // do a useful scan immediately after load and fill in extraLists detail later.
   // Apparel cannot: fortify-crafting gear is usually PLAYER-enchanted, and a
   // player enchantment lives in ExtraEnchantment on the inventory stack, not on
   // the base form. Scanning before the stabilization window would classify every
   // player-made piece as CraftSkill::None and reject it.
   //
   // So the scan is DEFERRED rather than degraded: ReconcileApparel() is a no-op
   // until Util::IsExtraListStable(). Nothing is lost by waiting — apparel is a
   // workstation-context source, and the player is not at a forge during the
   // first 500 ms after a load. This is also why there is no m_rejected cache:
   // caching a rejection made from unreadable extraLists would make it permanent.
   //
   // THREAD SAFETY: shared_mutex, same pattern as WeaponRegistry/SpellRegistry.
   // =============================================================================

   class ApparelRegistry
   {
   public:
      ApparelRegistry() = default;
      ~ApparelRegistry() = default;

      ApparelRegistry(const ApparelRegistry&) = delete;
      ApparelRegistry& operator=(const ApparelRegistry&) = delete;
      ApparelRegistry(ApparelRegistry&&) = delete;
      ApparelRegistry& operator=(ApparelRegistry&&) = delete;

      // =============================================================================
      // LIFECYCLE
      // =============================================================================

      /**
       * @brief Drop all tracked apparel on game load
       * @note Does NOT scan — the first ReconcileApparel() after the extraList
       *       stabilization window does that. See the class comment.
       */
      void RebuildRegistry();

      /**
       * @brief Add newly acquired apparel, drop what left the inventory, refresh worn status
       * @return Number of entries added or removed (0 while extraLists are unstable)
       * @note Safe to call on the periodic reconcile tick alongside the other registries.
       */
      size_t ReconcileApparel();

      // =============================================================================
      // ACCESSORS
      // =============================================================================
      //
      // LIFETIME CONTRACT: as with WeaponRegistry, GetApparel() hands out a raw
      // pointer into m_apparel but releases the shared_lock on return. A later
      // ReconcileApparel() can reallocate the vector and dangle it. Safe only
      // synchronously on the update thread; use ForEachApparel() for anything
      // that outlives the call.

      /**
       * @brief Look up one entry by its composite key (see InventoryApparel::Key)
       * @return Pointer into storage, or nullptr if not tracked
       */
      [[nodiscard]] const InventoryApparel* GetApparel(RE::FormID formID, uint16_t uniqueID = 0) const;

      /**
       * @brief Mark one entry as worn (or no longer worn) without a full scan
       * @return true if any tracked entry changed - the equipped piece, or one
       *         this equip displaced from the same body slot
       *
       * WHY THIS EXISTS: apparel has no cooldown, because isEquipped is supposed
       * to be what removes a piece from the pool once it is on the player. But
       * isEquipped is only refreshed by ReconcileApparel() on a 30 s tick, so
       * after Huginn equipped a ring the registry went on reporting it available:
       * the slot lock expired a few seconds later and the same ring was scored
       * and re-assigned for the rest of the interval. The equip path calls this
       * so the flag is true the moment the piece goes on. Equipping also clears
       * the flag on whatever this piece displaces; see the note on the sweep.
       */
      bool MarkEquipped(RE::FormID formID, uint16_t uniqueID, bool equipped = true);

      /**
       * @brief Iterate all tracked apparel without copying
       * @tparam Func void(const InventoryApparel&) or bool(const InventoryApparel&)
       * @note Holds the shared_lock for the whole visit; returning false stops early.
       */
      template<typename Func>
      void ForEachApparel(Func&& func) const
      {
         std::shared_lock lock(m_mutex);
         for (const auto& apparel : m_apparel) {
            if constexpr (std::is_same_v<std::invoke_result_t<Func, const InventoryApparel&>, bool>) {
               if (!func(apparel)) return;
            } else {
               func(apparel);
            }
         }
      }

      [[nodiscard]] size_t GetApparelCount() const noexcept;

      /// True while a rebuild is in flight — candidate gathering skips the registry.
      [[nodiscard]] bool IsLoading() const noexcept {
         return m_isLoading.load(std::memory_order_acquire);
      }

      /// Log every tracked piece at debug level.
      void LogAllApparel() const;

   private:
      /// One inventory STACK, resolved far enough to classify.
      ///
      /// One per ExtraDataList, not one per base form. An earlier version emitted
      /// one per TESBoundObject and filled these fields last-wins across every
      /// extraList in the stack, which defeated the whole point of the uniqueID
      /// key: two player-enchanted Gold Rings collapsed into a single entry, and
      /// the surviving one could pair the first ring's enchantment with the
      /// second ring's uniqueID.
      struct ScannedApparel
      {
         RE::TESObjectARMO*    armor = nullptr;
         RE::EnchantmentItem*  enchantment = nullptr;  // player-applied if present, else base form
         bool                  isEquipped = false;
         uint16_t              uniqueID = 0;
         std::string           displayName;            // ExtraTextDisplayData, empty if not renamed
      };

      /// Walk the player's armor inventory. Requires stable extraLists; the
      /// caller checks. Runs entirely outside the write lock.
      [[nodiscard]] std::vector<ScannedApparel> ScanPlayerApparel() const;

      std::vector<InventoryApparel>          m_apparel;
      std::unordered_map<uint64_t, size_t>   m_index;   // InventoryApparel::Key() -> index

      ApparelClassifier          m_classifier;
      mutable std::shared_mutex  m_mutex;
      std::atomic<bool>          m_isLoading{false};
   };
}
