#pragma once

#include "ItemData.h"
#include "ItemClassifier.h"
#include <shared_mutex>  // v0.7.12 - thread safety
#include <atomic>        // M3 v0.7.21 - atomic m_isLoading
#include <type_traits>   // For ForEach visitor pattern

namespace Huginn::Item
{
   // =============================================================================
   // SCANNED ITEM STRUCTS (v0.7.19)
   // =============================================================================
   // Store FormID alongside form pointer to avoid redundant GetFormID() calls
   // when building lookup maps. FormID is captured once during initial scan.
   // =============================================================================

   struct ScannedAlchemyItem
   {
      RE::AlchemyItem* item;
      RE::FormID formID;  // OPTIMIZATION (S5): Captured once during scan
      int32_t count;
   };

   struct ScannedSoulGem
   {
      RE::TESSoulGem* soulGem;
      RE::FormID formID;  // OPTIMIZATION (S5): Captured once during scan
      int32_t count;
      int32_t filledCount = 0;  // v0.10.0: Count of gems with souls (for weapon recharge)
   };

   // =============================================================================
   // INVENTORY SCAN RESULT (v0.7.19)
   // =============================================================================
   // Combined result from single inventory traversal, avoiding duplicate iterations
   // for alchemy items and soul gems. Used by RefreshCounts, ReconcileItems, and
   // RebuildRegistry to halve SKSE API calls on hot paths.
   // =============================================================================

   struct InventoryScanResult
   {
      std::vector<ScannedAlchemyItem> alchemyItems;
      std::vector<ScannedSoulGem> soulGems;
   };

   // =============================================================================
   // INVENTORY ITEM (v0.7.4)
   // =============================================================================
   // Wraps ItemData with inventory count tracking for delta detection.
   // Unlike spells which are either known or not, items have quantities that
   // change frequently through consumption and looting.
   // =============================================================================

   struct InventoryItem
   {
      ItemData data;              // Classification data
      int32_t count = 0;          // Current inventory count
      int32_t previousCount = 0;  // Count at last full reconciliation (for delta detection)

      // String representation for logging
      [[nodiscard]] std::string ToString() const
      {
      return std::format(
        "InventoryItem[{}, count={}, prev={}]",
        data.name,
        count,
        previousCount);
      }
   };

   // =============================================================================
   // ITEM CHANGE EVENT (v0.7.4)
   // =============================================================================
   // Emitted when item counts change. Used for Q-learning feedback:
   // - Consumed item (delta < 0) → potential reward signal
   // - Acquired item (delta > 0) → inventory update notification
   // =============================================================================

   struct ItemChangeEvent
   {
      RE::FormID formID;
      std::string name;
      ItemType type;
      int32_t delta;  // Positive = acquired, Negative = consumed

      [[nodiscard]] bool IsConsumption() const noexcept { return delta < 0; }
      [[nodiscard]] bool IsAcquisition() const noexcept { return delta > 0; }
   };

   // =============================================================================
   // ITEM REGISTRY (v0.7.4)
   // =============================================================================
   // Tracks alchemy items (potions, poisons, food, ingredients) in player inventory.
   //
   // KEY DIFFERENCE FROM SPELLREGISTRY:
   // Items have quantities that change frequently (consumption/looting), requiring
   // a two-tier refresh strategy:
   //   - Delta scan (500ms): Fast count comparison, emits change events
   //   - Full reconcile (30s): Adds new items, removes gone items
   //
   // THREAD SAFETY:
   // - Read-write locking via shared_mutex (same pattern as SpellRegistry)
   //
   // =============================================================================

   class ItemRegistry
   {
   public:
      ItemRegistry();
      ~ItemRegistry() = default;

      // Disable copy/move (singleton-style usage)
      ItemRegistry(const ItemRegistry&) = delete;
      ItemRegistry& operator=(const ItemRegistry&) = delete;
      ItemRegistry(ItemRegistry&&) = delete;
      ItemRegistry& operator=(ItemRegistry&&) = delete;

      // =============================================================================
      // LIFECYCLE
      // =============================================================================

      /**
       * @brief Load item classification overrides from INI file
       * @param iniPath Path to overrides INI file
       * @note Should be called before RebuildRegistry()
       */
      void LoadOverrides(const std::filesystem::path& iniPath);

      /**
       * @brief Full inventory scan and rebuild on game load
       * @note Clears existing registry and reclassifies all items
       */
      void RebuildRegistry();

      // =============================================================================
      // TWO-TIER REFRESH STRATEGY
      // =============================================================================

      /**
       * @brief Fast delta scan (call at 500ms intervals)
       * @return Vector of change events (consumed/acquired items)
       * @note O(n) where n = inventory size, no reclassification
       * @note Only detects count changes for tracked items
       */
      std::vector<ItemChangeEvent> RefreshCounts();

      /**
       * @brief Fast delta scan with pre-fetched player pointer (v0.7.19 S2 optimization)
       * @param player Pre-fetched player pointer (avoids redundant GetSingleton)
       */
      std::vector<ItemChangeEvent> RefreshCounts(RE::PlayerCharacter* player);

      /**
       * @brief Full item reconciliation (call at 30s intervals)
       * @return Number of items added or removed
       * @note Adds new items, removes gone items, updates previousCount snapshot
       * @note O(n) + O(m) classifications where m = new items
       */
      size_t ReconcileItems();

      /**
       * @brief Full reconciliation with pre-fetched player pointer (v0.7.19 S2 optimization)
       * @param player Pre-fetched player pointer (avoids redundant GetSingleton)
       */
      size_t ReconcileItems(RE::PlayerCharacter* player);

      // =============================================================================
      // ACCESSORS
      // =============================================================================

      /**
       * @brief Get item by FormID
       * @param formID The item's form ID
       * @return Pointer to InventoryItem, or nullptr if not found
       */
      [[nodiscard]] const InventoryItem* GetItem(RE::FormID formID) const;

      /**
       * @brief Get all items of a specific type
       * @param type ItemType to filter by
       * @return Vector of pointers to matching items
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetItemsByType(ItemType type) const;

      /**
       * @brief Get all items with a specific tag
       * @param tag ItemTag to filter by (bitflag check)
       * @return Vector of pointers to matching items
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetItemsWithTag(ItemTag tag) const;

      /**
       * @brief Get health potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to health potions, sorted by magnitude descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetHealthPotionsByMagnitude(size_t topK = 3) const;

      /**
       * @brief Get magicka potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to magicka potions, sorted by magnitude descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetMagickaPotionsByMagnitude(size_t topK = 3) const;

      /**
       * @brief Get stamina potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to stamina potions, sorted by magnitude descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetStaminaPotionsByMagnitude(size_t topK = 3) const;

      // =============================================================================
      // RESIST POTION ACCESSORS (PotionScanner v0.7.5)
      // =============================================================================

      /**
       * @brief Get resist fire potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to resist fire potions
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetResistFirePotions(size_t topK = 3) const;

      /**
       * @brief Get resist frost potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to resist frost potions
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetResistFrostPotions(size_t topK = 3) const;

      /**
       * @brief Get resist shock potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to resist shock potions
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetResistShockPotions(size_t topK = 3) const;

      /**
       * @brief Get resist poison potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to resist poison potions
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetResistPoisonPotions(size_t topK = 3) const;

      /**
       * @brief Get resist magic potions sorted by magnitude (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to resist magic potions
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetResistMagicPotions(size_t topK = 3) const;

      // =============================================================================
      // CURE POTION ACCESSORS (PotionScanner v0.7.5)
      // =============================================================================

      /**
       * @brief Get cure disease potions
       * @return Vector of pointers to cure disease potions
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetCureDiseasePotions() const;

      /**
       * @brief Get cure poison potions
       * @return Vector of pointers to cure poison potions
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetCurePoisonPotions() const;

      // =============================================================================
      // BUFF/FORTIFY POTION ACCESSORS (PotionScanner v0.7.5, refactored v0.8)
      // =============================================================================

      /**
       * @brief Get fortify magic school potions (Alteration, Conjuration, Destruction, etc.)
       * @param school Optional: Filter to specific school, or None for all schools
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to fortify school potions, sorted by magnitude descending
       * @note v0.8: Split from GetFortifySkillPotions for grouped tag support
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetFortifySchoolPotions(MagicSchool school = MagicSchool::None, size_t topK = 3) const;

      /**
       * @brief Get fortify combat skill potions (OneHanded, TwoHanded, Archery, Smithing, etc.)
       * @param skill Optional: Filter to specific skill, or None for all combat skills
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to fortify combat potions, sorted by magnitude descending
       * @note v0.8: Split from GetFortifySkillPotions for grouped tag support
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetFortifyCombatPotions(CombatSkill skill = CombatSkill::None, size_t topK = 3) const;

      /**
       * @brief Get fortify utility skill potions (Sneak, Lockpicking, Pickpocket, Speech, Alchemy)
       * @param skill Optional: Filter to specific skill, or None for all utility skills
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to fortify utility potions, sorted by magnitude descending
       * @note v0.8: Split from GetFortifySkillPotions for grouped tag support
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetFortifyUtilityPotions(UtilitySkill skill = UtilitySkill::None, size_t topK = 3) const;

      /**
       * @brief Get invisibility potions
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to invisibility potions, sorted by duration descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetInvisibilityPotions(size_t topK = 3) const;

      /**
       * @brief Get waterbreathing potions
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to waterbreathing potions, sorted by duration descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetWaterbreathingPotions(size_t topK = 3) const;

      // =============================================================================
      // CONVENIENCE "BEST" ACCESSORS (PotionScanner v0.7.5)
      // =============================================================================
      // These return the single best potion for quick slot allocation.
      // Returns nullptr if no potions of that type are available.
      //
      // PERFORMANCE: O(n) single-pass max-find instead of O(n log n) sort.
      // These are called frequently in the recommendation pipeline hot path.

      [[nodiscard]] const InventoryItem* GetBestHealthPotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestMagickaPotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestStaminaPotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestResistFirePotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestResistFrostPotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestResistShockPotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestResistPoisonPotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestCureDiseasePotion() const noexcept;
      [[nodiscard]] const InventoryItem* GetBestCurePoisonPotion() const noexcept;

      // =============================================================================
      // SOUL GEM ACCESSORS (SoulGemScanner v0.7.8)
      // =============================================================================

      /**
       * @brief Get all soul gems sorted by capacity (highest first)
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to soul gems, sorted by magnitude descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetSoulGems(size_t topK = 3) const;

      /**
       * @brief Get soul gems by minimum capacity (1=Petty, 6=Black)
       * @param minCapacity Minimum capacity threshold
       * @param topK Maximum number of results (default 3, 0 = all)
       * @return Vector of pointers to soul gems with capacity >= minCapacity, sorted descending
       * @note OPTIMIZATION (v0.7.20 H4): Uses partial_sort for O(n log k) vs O(n log n)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetSoulGemsByCapacity(float minCapacity, size_t topK = 3) const;

      /**
       * @brief Get black soul gems only (for future archetype tracking)
       * @return Vector of pointers to black soul gems (magnitude >= 6.0)
       */
      [[nodiscard]] std::vector<const InventoryItem*> GetBlackSoulGems() const;

      /**
       * @brief Get best soul gem (highest capacity available)
       * @return Pointer to highest capacity soul gem, or nullptr if none available
       */
      [[nodiscard]] const InventoryItem* GetBestSoulGem() const noexcept;

      /**
       * @brief Get total number of tracked item types
       * @return Number of unique item FormIDs in registry
       */
      [[nodiscard]] size_t GetItemCount() const noexcept;

      /**
       * @brief Get all tracked items (returns copy for thread safety - v0.7.12)
       * @return Copy of internal items vector
       */
      [[nodiscard]] std::vector<InventoryItem> GetAllItems() const;

      /**
       * @brief Iterate over all items without copying (zero-allocation visitor pattern)
       * @tparam Func Callable with signature void(const InventoryItem&) or bool(const InventoryItem&)
       * @param func Function to call for each item. If returns bool, iteration stops on false.
       * @note Thread-safe: Holds shared_lock during iteration
       * @note PERFORMANCE: Use this instead of GetAllItems() in hot paths
       */
      template<typename Func>
      void ForEachItem(Func&& func) const
      {
      std::shared_lock lock(m_mutex);
      for (const auto& item : m_items) {
        if constexpr (std::is_same_v<std::invoke_result_t<Func, const InventoryItem&>, bool>) {
           if (!func(item)) return;  // Early exit if callback returns false
        } else {
           func(item);
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
      [[nodiscard]] std::vector<ItemChangeEvent> GetAndClearChanges();

      // =============================================================================
      // DEBUG
      // =============================================================================

      /**
       * @brief Log all tracked items to debug log
       */
      void LogAllItems() const;

   private:
      // =============================================================================
      // INTERNAL HELPERS
      // =============================================================================

      /**
       * @brief Combined inventory scan for all item types (v0.7.19)
       * @return InventoryScanResult with both alchemy items and soul gems
       * @note Single traversal via GetInventoryChanges()->entryList
       * @note Replaces separate ScanPlayerInventory() + ScanPlayerSoulGems() calls
       */
      [[nodiscard]] InventoryScanResult ScanPlayerInventoryAll() const;

      /**
       * @brief Combined scan with pre-fetched player pointer (v0.7.19 S2 optimization)
       * @param player Pre-fetched player pointer (avoids redundant GetSingleton)
       */
      [[nodiscard]] InventoryScanResult ScanPlayerInventoryAll(RE::PlayerCharacter* player) const;

      /**
       * @brief Scan player inventory for alchemy items
       * @return Vector of (AlchemyItem*, count) pairs
       * @note Single traversal via GetInventoryChanges()->entryList
       * @deprecated Use ScanPlayerInventoryAll() for combined scan
       */
      [[nodiscard]] std::vector<std::pair<RE::AlchemyItem*, int32_t>> ScanPlayerInventory() const;

      /**
       * @brief Scan player inventory for soul gems (v0.7.8)
       * @return Vector of (TESSoulGem*, count) pairs
       * @note Soul gems are separate form type from AlchemyItem
       * @deprecated Use ScanPlayerInventoryAll() for combined scan
       */
      [[nodiscard]] std::vector<std::pair<RE::TESSoulGem*, int32_t>> ScanPlayerSoulGems() const;

      /**
       * @brief Add item to registry
       * @param item The alchemy item to add
       * @param count Current inventory count
       */
      void AddItem(RE::AlchemyItem* item, int32_t count);

      /**
       * @brief Add soul gem to registry (v0.7.8)
       * @param soulGem The soul gem to add
       * @param count Current inventory count
       * @note Soul gems are TESSoulGem, not AlchemyItem - handled separately
       */
      void AddSoulGem(RE::TESSoulGem* soulGem, int32_t count, int32_t filledCount);

      /**
       * @brief Remove item from registry by FormID
       * @param formID The form ID to remove
       * @return true if removed, false if not found
       * @note Uses swap-remove pattern for O(1) deletion
       */
      bool RemoveItem(RE::FormID formID);

      // =============================================================================
      // STORAGE
      // =============================================================================

      // Dual-index storage (mirrors SpellRegistry pattern)
      // - Vector for iteration and cache-friendly access
      // - Map for O(1) FormID lookup
      std::vector<InventoryItem> m_items;
      std::unordered_map<RE::FormID, size_t> m_formIDIndex;

      // Change tracking buffer (cleared by GetAndClearChanges)
      std::vector<ItemChangeEvent> m_pendingChanges;

      // Item classifier instance
      ItemClassifier m_classifier;

      // Thread safety (v0.7.12)
      // Protects m_items and m_formIDIndex from concurrent access
      // Readers (render thread) use shared_lock, writers (update thread) use unique_lock
      mutable std::shared_mutex m_mutex;

      // Loading state flag (M3 v0.7.21: atomic for thread-safe access)
      std::atomic<bool> m_isLoading{false};
   };
}
