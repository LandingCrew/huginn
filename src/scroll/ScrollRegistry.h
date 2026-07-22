#pragma once

#include "ScrollData.h"
#include "ScrollClassifier.h"
#include "registry/FormRegistry.h"       // Huginn::Registry::FormRegistry core (finding #8)
#include "util/InventoryUtil.h"          // Util::InventoryItemMap (shared inventory scan)

namespace Huginn::Scroll
{
   // =============================================================================
   // SCROLL CHANGE EVENT (v0.7.7)
   // =============================================================================
   // Emitted when scroll counts change. Used for Q-learning feedback:
   // - Consumed scroll (delta < 0) → potential reward signal
   // - Acquired scroll (delta > 0) → inventory update notification
   // =============================================================================

   struct ScrollChangeEvent
   {
      RE::FormID formID;
      std::string name;
      ScrollType type;
      int32_t delta;  // Positive = acquired, Negative = consumed

      [[nodiscard]] bool IsConsumption() const noexcept { return delta < 0; }
      [[nodiscard]] bool IsAcquisition() const noexcept { return delta > 0; }
   };

   // =============================================================================
   // SCROLL REGISTRY (v0.7.7)
   // =============================================================================
   // Tracks scrolls in player inventory.
   //
   // ARCHITECTURE:
   // - Storage/indexing/locking and the generic query helpers come from the
   //   shared Registry::FormRegistry core (finding #8). ScrollRegistry adds only
   //   the scroll-specific scan, classify, two-tier refresh, and typed accessors.
   // - Delta scan (500ms): Fast count comparison, emits change events
   // - Full reconcile (30s): Adds new scrolls, removes gone scrolls
   //
   // DESIGN NOTES:
   // - Separate from ItemRegistry since scrolls are RE::ScrollItem, not RE::AlchemyItem
   // - Uses ScrollClassifier which delegates to SpellClassifier for effect analysis
   // =============================================================================

   class ScrollRegistry : public Registry::FormRegistry<ScrollRegistry, InventoryScroll>
   {
   public:
      // Constructor takes SpellClassifier dependency (for ScrollClassifier)
      explicit ScrollRegistry(const Spell::SpellClassifier& spellClassifier);
      ~ScrollRegistry() = default;

      // CRTP contract for FormRegistry: where the FormID lives on an entry.
      [[nodiscard]] static RE::FormID FormIDOf(const InventoryScroll& scroll) noexcept
      {
         return scroll.data.formID;
      }

      // =============================================================================
      // LIFECYCLE
      // =============================================================================

      /**
       * @brief Full inventory scan and rebuild on game load
       * @note Clears existing registry and reclassifies all scrolls
       */
      void RebuildRegistry();

      // =============================================================================
      // TWO-TIER REFRESH STRATEGY
      // =============================================================================

      /**
       * @brief Fast delta scan (call at 500ms intervals)
       * @return Vector of change events (consumed/acquired scrolls)
       */
      std::vector<ScrollChangeEvent> RefreshCounts();

      /** @brief Fast delta scan with pre-fetched player pointer (v0.7.19 S2 optimization) */
      std::vector<ScrollChangeEvent> RefreshCounts(RE::PlayerCharacter* player);

      /**
       * @brief Delta scan reusing an already-scanned inventory (v0.18 G optimization)
       * @note Lets the update loop run ONE GetInventorySafe pass for items + scrolls
       */
      std::vector<ScrollChangeEvent> RefreshCounts(
         RE::PlayerCharacter* player, const Util::InventoryItemMap& inventory);

      /**
       * @brief Full scroll reconciliation (call at 30s intervals)
       * @return Number of scrolls added or removed
       */
      size_t ReconcileScrolls();

      /** @brief Full reconciliation with pre-fetched player pointer (v0.7.19 S2 optimization) */
      size_t ReconcileScrolls(RE::PlayerCharacter* player);

      // =============================================================================
      // ACCESSORS
      // =============================================================================

      /** @brief Get all scrolls of a specific type (count > 0) */
      [[nodiscard]] std::vector<const InventoryScroll*> GetScrollsByType(ScrollType type) const;

      // Type-based accessors (sorted by magnitude descending, top-K). topK 0 = all.
      [[nodiscard]] std::vector<const InventoryScroll*> GetDamageScrolls(size_t topK = 3) const;
      [[nodiscard]] std::vector<const InventoryScroll*> GetHealingScrolls(size_t topK = 3) const;
      [[nodiscard]] std::vector<const InventoryScroll*> GetDefensiveScrolls(size_t topK = 3) const;
      [[nodiscard]] std::vector<const InventoryScroll*> GetUtilityScrolls() const;
      [[nodiscard]] std::vector<const InventoryScroll*> GetSummonScrolls() const;

      // Element-based accessors (sorted by magnitude descending, top-K).
      [[nodiscard]] std::vector<const InventoryScroll*> GetFireScrolls(size_t topK = 3) const;
      [[nodiscard]] std::vector<const InventoryScroll*> GetFrostScrolls(size_t topK = 3) const;
      [[nodiscard]] std::vector<const InventoryScroll*> GetShockScrolls(size_t topK = 3) const;

      // Convenience "best" accessors — single best scroll, nullptr if none.
      [[nodiscard]] const InventoryScroll* GetBestDamageScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestHealingScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestFireScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestFrostScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestShockScroll() const noexcept;

      // Thin forwarders keeping the historical public names (zero call-site churn).
      [[nodiscard]] size_t GetScrollCount() const noexcept { return EntryCount(); }
      [[nodiscard]] std::vector<InventoryScroll> GetAllScrolls() const { return AllEntriesCopy(); }

      /** @brief Zero-allocation visitor over all scrolls (holds shared_lock). */
      template<typename Func>
      void ForEachScroll(Func&& func) const { ForEachEntry(std::forward<Func>(func)); }

      // =============================================================================
      // DEBUG
      // =============================================================================

      /** @brief Log all tracked scrolls to debug log */
      void LogAllScrolls() const;

   private:
      // =============================================================================
      // INTERNAL HELPERS
      // =============================================================================

      // Scan player inventory for scrolls (single GetInventory traversal).
      [[nodiscard]] std::vector<std::pair<RE::ScrollItem*, int32_t>> ScanPlayerInventory() const;
      [[nodiscard]] std::vector<std::pair<RE::ScrollItem*, int32_t>> ScanPlayerInventory(RE::PlayerCharacter* player) const;
      // Build the scroll list from an already-scanned inventory map (v0.18 G).
      // Non-scroll entries in the map are ignored.
      [[nodiscard]] std::vector<std::pair<RE::ScrollItem*, int32_t>> ScanPlayerInventory(const Util::InventoryItemMap& inventory) const;

      // Shared delta-diff tail for the RefreshCounts overloads.
      std::vector<ScrollChangeEvent> RefreshCountsFromScan(
         const std::vector<std::pair<RE::ScrollItem*, int32_t>>& currentInventory);

      // Add scroll to registry. Assumes m_mutex held by caller.
      void AddScroll(RE::ScrollItem* scroll, int32_t count);

      // Remove scroll by FormID (swap-pop). Assumes m_mutex held by caller.
      bool RemoveScroll(RE::FormID formID);

      // Scroll classifier instance (storage/index/mutex/isLoading live in the base).
      ScrollClassifier m_classifier;
   };
}
