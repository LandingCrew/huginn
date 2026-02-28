#pragma once

#include "ScrollData.h"
#include "ScrollClassifier.h"
#include <shared_mutex>  // v0.7.12 - thread safety
#include <atomic>        // M3 v0.7.21 - atomic m_isLoading
#include <type_traits>   // For ForEach visitor pattern

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
   // - Follows ItemRegistry two-tier refresh pattern
   // - Delta scan (500ms): Fast count comparison, emits change events
   // - Full reconcile (30s): Adds new scrolls, removes gone scrolls
   //
   // DESIGN NOTES:
   // - Separate from ItemRegistry since scrolls are RE::ScrollItem, not RE::AlchemyItem
   // - Uses ScrollClassifier which delegates to SpellClassifier for effect analysis
   //
   // THREAD SAFETY:
   // - Not thread-safe (single-threaded access from update loop)
   // - If needed, can add shared_mutex following SpellRegistry pattern
   // =============================================================================

   class ScrollRegistry
   {
   public:
      // Constructor takes SpellClassifier dependency (for ScrollClassifier)
      explicit ScrollRegistry(const Spell::SpellClassifier& spellClassifier);
      ~ScrollRegistry() = default;

      // Disable copy/move (singleton-style usage)
      ScrollRegistry(const ScrollRegistry&) = delete;
      ScrollRegistry& operator=(const ScrollRegistry&) = delete;
      ScrollRegistry(ScrollRegistry&&) = delete;
      ScrollRegistry& operator=(ScrollRegistry&&) = delete;

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
       * @note O(n) where n = scroll count, no reclassification
       * @note Only detects count changes for tracked scrolls
       */
      std::vector<ScrollChangeEvent> RefreshCounts();

      /**
       * @brief Fast delta scan with pre-fetched player pointer (v0.7.19 S2 optimization)
       * @param player Pre-fetched player pointer (avoids redundant GetSingleton)
       */
      std::vector<ScrollChangeEvent> RefreshCounts(RE::PlayerCharacter* player);

      /**
       * @brief Full scroll reconciliation (call at 30s intervals)
       * @return Number of scrolls added or removed
       * @note Adds new scrolls, removes gone scrolls, updates previousCount snapshot
       * @note O(n) + O(m) classifications where m = new scrolls
       */
      size_t ReconcileScrolls();

      /**
       * @brief Full reconciliation with pre-fetched player pointer (v0.7.19 S2 optimization)
       * @param player Pre-fetched player pointer (avoids redundant GetSingleton)
       */
      size_t ReconcileScrolls(RE::PlayerCharacter* player);

      // =============================================================================
      // ACCESSORS
      // =============================================================================

      /**
       * @brief Get scroll by FormID
       * @param formID The scroll's form ID
       * @return Pointer to InventoryScroll, or nullptr if not found
       */
      [[nodiscard]] const InventoryScroll* GetScroll(RE::FormID formID) const;

      /**
       * @brief Get all scrolls of a specific type
       * @param type ScrollType to filter by
       * @return Vector of pointers to matching scrolls
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetScrollsByType(ScrollType type) const;

      /**
       * @brief Get all scrolls with a specific tag
       * @param tag ScrollTag to filter by (bitflag check)
       * @return Vector of pointers to matching scrolls
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetScrollsWithTag(ScrollTag tag) const;

      // =============================================================================
      // TYPE-BASED ACCESSORS (ScrollScanner v0.7.7)
      // =============================================================================

      /**
       * @brief Get damage scrolls sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to damage scrolls, sorted by magnitude descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetDamageScrolls(size_t topK = 3) const;

      /**
       * @brief Get healing scrolls sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to healing scrolls, sorted by magnitude descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetHealingScrolls(size_t topK = 3) const;

      /**
       * @brief Get defensive scrolls (wards, armor) sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to defensive scrolls
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetDefensiveScrolls(size_t topK = 3) const;

      /**
       * @brief Get utility scrolls (detect life, light, telekinesis)
       * @return Vector of pointers to utility scrolls
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetUtilityScrolls() const;

      /**
       * @brief Get summon scrolls (conjuration)
       * @return Vector of pointers to summon scrolls
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetSummonScrolls() const;

      // =============================================================================
      // ELEMENT-BASED ACCESSORS (ScrollScanner v0.7.7)
      // =============================================================================

      /**
       * @brief Get fire scrolls sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to fire scrolls
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetFireScrolls(size_t topK = 3) const;

      /**
       * @brief Get frost scrolls sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to frost scrolls
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetFrostScrolls(size_t topK = 3) const;

      /**
       * @brief Get shock scrolls sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to shock scrolls
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetShockScrolls(size_t topK = 3) const;

      // =============================================================================
      // SCHOOL-BASED ACCESSORS (ScrollScanner v0.7.7)
      // =============================================================================

      /**
       * @brief Get scrolls by magic school
       * @param school MagicSchool to filter by
       * @return Vector of pointers to matching scrolls
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetScrollsBySchool(MagicSchool school) const;

      /**
       * @brief Get destruction scrolls
       * @return Vector of pointers to destruction scrolls
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetDestructionScrolls() const;

      /**
       * @brief Get restoration scrolls
       * @return Vector of pointers to restoration scrolls
       */
      [[nodiscard]] std::vector<const InventoryScroll*> GetRestorationScrolls() const;

      // =============================================================================
      // CONVENIENCE "BEST" ACCESSORS (ScrollScanner v0.7.7)
      // =============================================================================
      // These return the single best scroll for quick slot allocation.
      // Returns nullptr if no scrolls of that type are available.
      //
      // PERFORMANCE: O(n) single-pass max-find instead of O(n log n) sort.

      [[nodiscard]] const InventoryScroll* GetBestDamageScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestHealingScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestFireScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestFrostScroll() const noexcept;
      [[nodiscard]] const InventoryScroll* GetBestShockScroll() const noexcept;

      /**
       * @brief Get total number of tracked scroll types
       * @return Number of unique scroll FormIDs in registry
       */
      [[nodiscard]] size_t GetScrollCount() const noexcept;

      /**
       * @brief Get all tracked scrolls (returns copy for thread safety - v0.7.12)
       * @return Copy of internal scrolls vector
       */
      [[nodiscard]] std::vector<InventoryScroll> GetAllScrolls() const;

      /**
       * @brief Iterate over all scrolls without copying (zero-allocation visitor pattern)
       * @tparam Func Callable with signature void(const InventoryScroll&) or bool(const InventoryScroll&)
       * @param func Function to call for each scroll. If returns bool, iteration stops on false.
       * @note Thread-safe: Holds shared_lock during iteration
       * @note PERFORMANCE: Use this instead of GetAllScrolls() in hot paths
       */
      template<typename Func>
      void ForEachScroll(Func&& func) const
      {
      std::shared_lock lock(m_mutex);
      for (const auto& scroll : m_scrolls) {
        if constexpr (std::is_same_v<std::invoke_result_t<Func, const InventoryScroll&>, bool>) {
           if (!func(scroll)) return;
        } else {
           func(scroll);
        }
      }
      }

      /**
       * @brief Check if registry is currently loading/rebuilding
       * M3 (v0.7.21): Use atomic load with acquire semantics
       */
      [[nodiscard]] bool IsLoading() const noexcept { return m_isLoading.load(std::memory_order_acquire); }

      // =============================================================================
      // CHANGE TRACKING
      // =============================================================================

      /**
       * @brief Get and clear pending change events
       * @return Vector of change events since last call (clears internal buffer)
       * @note NOT thread-safe: single-threaded access only (from update loop)
       */
      [[nodiscard]] std::vector<ScrollChangeEvent> GetAndClearChanges();

      // =============================================================================
      // DEBUG
      // =============================================================================

      /**
       * @brief Log all tracked scrolls to debug log
       */
      void LogAllScrolls() const;

   private:
      // =============================================================================
      // INTERNAL HELPERS
      // =============================================================================

      /**
       * @brief Scan player inventory for scrolls
       * @return Vector of (ScrollItem*, count) pairs
       * @note Single traversal via GetInventoryChanges()->entryList
       */
      [[nodiscard]] std::vector<std::pair<RE::ScrollItem*, int32_t>> ScanPlayerInventory() const;
      [[nodiscard]] std::vector<std::pair<RE::ScrollItem*, int32_t>> ScanPlayerInventory(RE::PlayerCharacter* player) const;

      /**
       * @brief Add scroll to registry
       * @param scroll The scroll to add
       * @param count Current inventory count
       */
      void AddScroll(RE::ScrollItem* scroll, int32_t count);

      /**
       * @brief Remove scroll from registry by FormID
       * @param formID The form ID to remove
       * @return true if removed, false if not found
       * @note Uses swap-remove pattern for O(1) deletion
       */
      bool RemoveScroll(RE::FormID formID);

      // =============================================================================
      // STORAGE
      // =============================================================================

      // Dual-index storage (mirrors ItemRegistry/SpellRegistry pattern)
      // - Vector for iteration and cache-friendly access
      // - Map for O(1) FormID lookup
      std::vector<InventoryScroll> m_scrolls;
      std::unordered_map<RE::FormID, size_t> m_formIDIndex;

      // Change tracking buffer (cleared by GetAndClearChanges)
      std::vector<ScrollChangeEvent> m_pendingChanges;

      // Scroll classifier instance
      ScrollClassifier m_classifier;

      // Thread safety (v0.7.12)
      // Protects m_scrolls and m_formIDIndex from concurrent access
      // Readers (render thread) use shared_lock, writers (update thread) use unique_lock
      mutable std::shared_mutex m_mutex;

      // Loading state flag (M3 v0.7.21: atomic for thread-safe access)
      std::atomic<bool> m_isLoading{false};
   };
}
