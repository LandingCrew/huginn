#include "ItemRegistry.h"
#include "Config.h"
#include "util/ScopedTimer.h"
#include "util/AtomicGuard.h"
#include "util/AlgorithmUtils.h"
#include "util/InventoryUtil.h"

namespace Huginn::Item
{
   ItemRegistry::ItemRegistry()
   {
      // Load overrides from default location
      auto overridesPath = std::filesystem::path("Data/SKSE/Plugins/Huginn_Overrides.ini");
      LoadOverrides(overridesPath);
   }

   void ItemRegistry::LoadOverrides(const std::filesystem::path& iniPath)
   {
      m_classifier.LoadOverrides(iniPath);
   }

   void ItemRegistry::RebuildRegistry()
   {
      logger::info("Rebuilding item registry..."sv);
      m_isLoading = true;

      // E3 (v0.7.21): RAII guard to ensure m_isLoading gets cleared even on exception
      Util::AtomicBoolGuard guard{ m_isLoading, false };

      // OPTIMIZATION (v0.7.19): Single traversal for both item types
      auto scanResult = ScanPlayerInventoryAll();
      auto& inventoryItems = scanResult.alchemyItems;
      auto& soulGems = scanResult.soulGems;
      logger::info("Found {} alchemy items, {} soul gems in player inventory"sv,
      inventoryItems.size(), soulGems.size());

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Clear existing data
      m_items.clear();
      m_formIDIndex.clear();
      m_pendingChanges.clear();

      // Reserve space for both containers to avoid reallocation/rehashing
      const size_t capacity = std::min(inventoryItems.size() + soulGems.size(), Config::MAX_TRACKED_ITEMS);
      m_items.reserve(capacity);
      m_formIDIndex.reserve(capacity);

      // Classify and add each item (AddItem assumes lock is held by caller)
      for (const auto& scanned : inventoryItems) {
      if (m_items.size() >= Config::MAX_TRACKED_ITEMS) {
        logger::warn("Item registry reached max capacity ({}), some items skipped"sv,
           Config::MAX_TRACKED_ITEMS);
        break;
      }
      AddItem(scanned.item, scanned.count);
      }

      // Add soul gems (v0.7.8) - separate form type (AddSoulGem assumes lock is held)
      for (const auto& scanned : soulGems) {
      if (m_items.size() >= Config::MAX_TRACKED_ITEMS) {
        logger::warn("Item registry reached max capacity ({}), soul gems skipped"sv,
           Config::MAX_TRACKED_ITEMS);
        break;
      }
      AddSoulGem(scanned.soulGem, scanned.count, scanned.filledCount);
      }

      logger::info("Item registry built: {} items registered"sv, m_items.size());
      // m_isLoading cleared by LoadingGuard destructor
   }

   std::vector<ItemChangeEvent> ItemRegistry::RefreshCounts()
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return {};
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return RefreshCounts(player);
   }

   std::vector<ItemChangeEvent> ItemRegistry::RefreshCounts(RE::PlayerCharacter* player)
   {
      SCOPED_TIMER("ItemRegistry::RefreshCounts");

      if (!player) return {};

      // OPTIMIZATION (v0.7.19): Single traversal for both item types
      // OPTIMIZATION (S2 v0.7.19): Use pre-fetched player pointer
      auto scanResult = ScanPlayerInventoryAll(player);
      auto& currentInventory = scanResult.alchemyItems;
      auto& currentSoulGems = scanResult.soulGems;

      // Build map for fast lookup: FormID -> count (outside critical section)
      // OPTIMIZATION (S5 v0.7.19): Use cached formID instead of calling GetFormID() again
      std::unordered_map<RE::FormID, int32_t> currentCounts;
      std::unordered_map<RE::FormID, int32_t> currentFilledCounts;  // v0.10.0: Track fill state
      currentCounts.reserve(currentInventory.size() + currentSoulGems.size());
      for (const auto& scanned : currentInventory) {
      currentCounts[scanned.formID] = scanned.count;  // Use cached formID
      }
      // Add soul gems to the count map (v0.10.0: also track filled count)
      for (const auto& scanned : currentSoulGems) {
      currentCounts[scanned.formID] = scanned.count;  // Use cached formID
      currentFilledCounts[scanned.formID] = scanned.filledCount;  // v0.10.0
      }

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Track where new changes start (for return value)
      const size_t changesStart = m_pendingChanges.size();

      // Compare tracked items with current counts - build directly in m_pendingChanges
      for (auto& invItem : m_items) {
      auto it = currentCounts.find(invItem.data.formID);
      int32_t currentCount = (it != currentCounts.end()) ? it->second : 0;

      if (currentCount != invItem.count) {
        int32_t delta = currentCount - invItem.count;

        // Emit change event directly to pending changes
        m_pendingChanges.push_back(ItemChangeEvent{
           .formID = invItem.data.formID,
           .name = invItem.data.name,
           .type = invItem.data.type,
           .delta = delta
        });

        // Update stored count
        invItem.count = currentCount;

        logger::trace("[ItemRegistry] Count change: {} {} ({})"sv,
           invItem.data.name,
           delta > 0 ? "+" : "",
           delta);
      }

      // v0.10.0: Update soul gem fill state (e.g., after soul trap)
      if (invItem.data.type == ItemType::SoulGem) {
        auto fillIt = currentFilledCounts.find(invItem.data.formID);
        bool currentlyFilled = (fillIt != currentFilledCounts.end() && fillIt->second > 0);
        if (currentlyFilled != invItem.data.isFilled) {
           invItem.data.isFilled = currentlyFilled;
           logger::debug("[ItemRegistry] Soul gem fill state changed: {} -> {}"sv,
            invItem.data.name, currentlyFilled ? "filled" : "empty");
        }
      }
      }

      // Return only the newly added changes (avoids full copy)
      return std::vector<ItemChangeEvent>(
      m_pendingChanges.begin() + static_cast<ptrdiff_t>(changesStart),
      m_pendingChanges.end());
   }

   size_t ItemRegistry::ReconcileItems()
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return 0;
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return ReconcileItems(player);
   }

   size_t ItemRegistry::ReconcileItems(RE::PlayerCharacter* player)
   {
      SCOPED_TIMER("ItemRegistry::ReconcileItems");
      m_isLoading = true;

      // E3 (v0.7.21): RAII guard to ensure m_isLoading gets cleared even on exception/early return
      Util::AtomicBoolGuard guard{ m_isLoading, false };

      if (!player) return 0;

      // OPTIMIZATION (v0.7.19): Single traversal for both item types
      // OPTIMIZATION (S2 v0.7.19): Use pre-fetched player pointer
      auto scanResult = ScanPlayerInventoryAll(player);
      auto& currentInventory = scanResult.alchemyItems;
      auto& currentSoulGems = scanResult.soulGems;

      // Build set of current FormIDs for fast lookup (outside critical section)
      // OPTIMIZATION (S5 v0.7.19): Use cached formID instead of calling GetFormID() again
      std::unordered_set<RE::FormID> currentFormIDs;
      currentFormIDs.reserve(currentInventory.size() + currentSoulGems.size());
      for (const auto& scanned : currentInventory) {
      currentFormIDs.insert(scanned.formID);  // Use cached formID
      }
      for (const auto& scanned : currentSoulGems) {
      currentFormIDs.insert(scanned.formID);  // Use cached formID
      }

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      size_t itemsAdded = 0;
      size_t itemsRemoved = 0;

      // STEP 1A: Add new alchemy items (not in registry but in inventory)
      // (AddItem assumes lock is held by caller)
      for (const auto& scanned : currentInventory) {
      if (!scanned.item) continue;

      // Check if already registered (use cached formID)
      if (!m_formIDIndex.contains(scanned.formID)) {
        // Check capacity limit
        if (m_items.size() >= Config::MAX_TRACKED_ITEMS) {
           logger::warn("Item registry at max capacity, cannot add new items"sv);
           break;
        }

        // New item found - add it
        AddItem(scanned.item, scanned.count);
        itemsAdded++;

        logger::trace("[ItemRegistry] Added new item: {} x{}"sv, scanned.item->GetName(), scanned.count);
      }
      }

      // STEP 1B: Add new soul gems (v0.7.8)
      for (const auto& scanned : currentSoulGems) {
      if (!scanned.soulGem) continue;

      // Check if already registered (use cached formID)
      if (!m_formIDIndex.contains(scanned.formID)) {
        // Check capacity limit
        if (m_items.size() >= Config::MAX_TRACKED_ITEMS) {
           logger::warn("Item registry at max capacity, cannot add new soul gems"sv);
           break;
        }

        // New soul gem found - add it
        AddSoulGem(scanned.soulGem, scanned.count, scanned.filledCount);
        itemsAdded++;

        logger::trace("[ItemRegistry] Added new soul gem: {} x{} (filled={})"sv,
           scanned.soulGem->GetName(), scanned.count, scanned.filledCount);
      }
      }

      // STEP 2: Remove items no longer in inventory
      std::vector<RE::FormID> toRemove;
      toRemove.reserve(m_items.size() / 10);  // Estimate ~10% removal rate

      for (const auto& invItem : m_items) {
      if (!currentFormIDs.contains(invItem.data.formID)) {
        toRemove.push_back(invItem.data.formID);
      }
      }

      for (auto formID : toRemove) {
      if (RemoveItem(formID)) {
        itemsRemoved++;
      }
      }

      // STEP 3: Update previousCount snapshot for all remaining items
      for (auto& invItem : m_items) {
      invItem.previousCount = invItem.count;
      }

      if (itemsAdded > 0 || itemsRemoved > 0) {
      logger::info("[ItemRegistry] Reconciliation: +{} item(s), -{} item(s), total: {}"sv,
        itemsAdded, itemsRemoved, m_items.size());
      }

      // m_isLoading cleared by LoadingGuard destructor
      return itemsAdded + itemsRemoved;
   }

   const InventoryItem* ItemRegistry::GetItem(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return nullptr;
      }
      return &m_items[it->second];
   }

   std::vector<const InventoryItem*> ItemRegistry::GetItemsByType(ItemType type) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 4);  // Rough estimate

      for (const auto& item : m_items) {
      if (item.data.type == type && item.count > 0) {
        result.push_back(&item);
      }
      }

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetItemsWithTag(ItemTag tag) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 4);  // Rough estimate

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, tag) && item.count > 0) {
        result.push_back(&item);
      }
      }

      return result;
   }

   // OPTIMIZATION (v0.7.20 H4): Helper lambda for partial_sort pattern
   // Uses std::partial_sort when k < n (O(n log k)) for top-K queries,
   // falls back to std::sort for full results (O(n log n)) when k >= n or k == 0

   std::vector<const InventoryItem*> ItemRegistry::GetHealthPotionsByMagnitude(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 4);

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::HealthPotion && item.count > 0) {
        result.push_back(&item);
      }
      }

      // OPTIMIZATION (v0.7.20 H4): partial_sort for top-K is O(n log k) vs O(n log n)
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetMagickaPotionsByMagnitude(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 4);

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::MagickaPotion && item.count > 0) {
        result.push_back(&item);
      }
      }

      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetStaminaPotionsByMagnitude(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 4);

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::StaminaPotion && item.count > 0) {
        result.push_back(&item);
      }
      }

      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   // =============================================================================
   // RESIST POTION ACCESSORS
   // =============================================================================

   std::vector<const InventoryItem*> ItemRegistry::GetResistFirePotions(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistFire) && item.count > 0) {
        result.push_back(&item);
      }
      }

      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistFrostPotions(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistFrost) && item.count > 0) {
        result.push_back(&item);
      }
      }

      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistShockPotions(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistShock) && item.count > 0) {
        result.push_back(&item);
      }
      }

      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistPoisonPotions(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistPoison) && item.count > 0) {
        result.push_back(&item);
      }
      }

      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistMagicPotions(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistMagic) && item.count > 0) {
        result.push_back(&item);
      }
      }

      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   // =============================================================================
   // CURE POTION ACCESSORS
   // =============================================================================

   std::vector<const InventoryItem*> ItemRegistry::GetCureDiseasePotions() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::CureDisease) && item.count > 0) {
        result.push_back(&item);
      }
      }

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetCurePoisonPotions() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::CurePoison) && item.count > 0) {
        result.push_back(&item);
      }
      }

      return result;
   }

   // =============================================================================
   // BUFF/FORTIFY POTION ACCESSORS (v0.8: Refactored to grouped tags)
   // =============================================================================

   std::vector<const InventoryItem*> ItemRegistry::GetFortifySchoolPotions(MagicSchool school, size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::FortifyMagicSchool) && item.count > 0) {
        // If specific school requested, filter to that school
        if (school != MagicSchool::None && item.data.school != school) {
           continue;
        }
        result.push_back(&item);
      }
      }

      // Sort by magnitude (stronger fortify effects first)
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetFortifyCombatPotions(CombatSkill skill, size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::FortifyCombatSkill) && item.count > 0) {
        // If specific skill requested, filter to that skill
        if (skill != CombatSkill::None && item.data.combatSkill != skill) {
           continue;
        }
        result.push_back(&item);
      }
      }

      // Sort by magnitude (stronger fortify effects first)
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetFortifyUtilityPotions(UtilitySkill skill, size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::FortifyUtilitySkill) && item.count > 0) {
        // If specific skill requested, filter to that skill
        if (skill != UtilitySkill::None && item.data.utilitySkill != skill) {
           continue;
        }
        result.push_back(&item);
      }
      }

      // Sort by magnitude (stronger fortify effects first)
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetInvisibilityPotions(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(4);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::Invisibility) && item.count > 0) {
        result.push_back(&item);
      }
      }

      // Sort by duration (longer invisibility first)
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.duration > b->data.duration;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetWaterbreathingPotions(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(4);

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::Waterbreathing) && item.count > 0) {
        result.push_back(&item);
      }
      }

      // Sort by duration (longer waterbreathing first)
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.duration > b->data.duration;
      }, topK);

      return result;
   }

   // =============================================================================
   // CONVENIENCE "BEST" ACCESSORS - O(n) single-pass implementation
   // =============================================================================
   // These use single-pass max-find instead of O(n log n) sort for performance.
   // Called frequently in recommendation pipeline hot path (~10x per 500ms update).

   const InventoryItem* ItemRegistry::GetBestHealthPotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::HealthPotion &&
          item.count > 0 &&
          item.data.magnitude > maxMagnitude) {
        best = &item;
        maxMagnitude = item.data.magnitude;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestMagickaPotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::MagickaPotion &&
          item.count > 0 &&
          item.data.magnitude > maxMagnitude) {
        best = &item;
        maxMagnitude = item.data.magnitude;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestStaminaPotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::StaminaPotion &&
          item.count > 0 &&
          item.data.magnitude > maxMagnitude) {
        best = &item;
        maxMagnitude = item.data.magnitude;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestResistFirePotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistFire) &&
          item.count > 0 &&
          item.data.magnitude > maxMagnitude) {
        best = &item;
        maxMagnitude = item.data.magnitude;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestResistFrostPotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistFrost) &&
          item.count > 0 &&
          item.data.magnitude > maxMagnitude) {
        best = &item;
        maxMagnitude = item.data.magnitude;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestResistShockPotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistShock) &&
          item.count > 0 &&
          item.data.magnitude > maxMagnitude) {
        best = &item;
        maxMagnitude = item.data.magnitude;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestResistPoisonPotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::ResistPoison) &&
          item.count > 0 &&
          item.data.magnitude > maxMagnitude) {
        best = &item;
        maxMagnitude = item.data.magnitude;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestCureDiseasePotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      // Cure potions are binary effects (cures or doesn't), so we prefer
      // the one with lowest gold value (most economical choice)
      const InventoryItem* best = nullptr;
      uint32_t minValue = UINT32_MAX;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::CureDisease) &&
          item.count > 0 &&
          item.data.value < minValue) {
        best = &item;
        minValue = item.data.value;
      }
      }
      return best;
   }

   const InventoryItem* ItemRegistry::GetBestCurePoisonPotion() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      // Cure potions are binary effects (cures or doesn't), so we prefer
      // the one with lowest gold value (most economical choice)
      const InventoryItem* best = nullptr;
      uint32_t minValue = UINT32_MAX;

      for (const auto& item : m_items) {
      if (HasTag(item.data.tags, ItemTag::CurePoison) &&
          item.count > 0 &&
          item.data.value < minValue) {
        best = &item;
        minValue = item.data.value;
      }
      }
      return best;
   }

   // =============================================================================
   // SOUL GEM ACCESSORS (SoulGemScanner v0.7.8)
   // =============================================================================

   std::vector<const InventoryItem*> ItemRegistry::GetSoulGems(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      // Debug: Count soul gems by type
      size_t soulGemCount = 0;
      size_t totalItems = m_items.size();

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::SoulGem) {
        soulGemCount++;
        logger::trace("[GetSoulGems] Found: {} (type={}, count={})"sv,
           item.data.name,
           static_cast<int>(item.data.type),
           item.count);

        if (item.count > 0) {
           result.push_back(&item);
        }
      }
      }

      logger::trace("[GetSoulGems] Total items: {}, Soul gems: {}, With count>0: {}"sv,
      totalItems, soulGemCount, result.size());

      // Sort by capacity (magnitude) descending - highest capacity first
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetSoulGemsByCapacity(float minCapacity, size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;
      result.reserve(m_items.size() / 8);

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::SoulGem &&
          item.count > 0 &&
          item.data.magnitude >= minCapacity) {
        result.push_back(&item);
      }
      }

      // Sort by capacity (magnitude) descending - highest capacity first
      Util::SortTopK(result, [](const InventoryItem* a, const InventoryItem* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryItem*> ItemRegistry::GetBlackSoulGems() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryItem*> result;

      for (const auto& item : m_items) {
      if (item.data.type == ItemType::SoulGem &&
          item.count > 0 &&
          item.data.magnitude >= 6.0f) {  // Black soul gem capacity
        result.push_back(&item);
      }
      }

      return result;
   }

   const InventoryItem* ItemRegistry::GetBestSoulGem() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryItem* best = nullptr;
      float maxCapacity = 0.0f;

      // v0.10.0: Only return FILLED soul gems (for weapon recharge override)
      // Empty soul gems are useless for recharging weapons
      for (const auto& item : m_items) {
      if (item.data.type == ItemType::SoulGem &&
          item.count > 0 &&
          item.data.isFilled &&  // v0.10.0: Must be filled
          item.data.magnitude > maxCapacity) {
        best = &item;
        maxCapacity = item.data.magnitude;
      }
      }

      return best;
   }

   std::vector<ItemChangeEvent> ItemRegistry::GetAndClearChanges()
   {
      std::unique_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<ItemChangeEvent> result = std::move(m_pendingChanges);
      m_pendingChanges.clear();
      return result;
   }

   void ItemRegistry::LogAllItems() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      logger::info("=== Item Registry ({} items) ==="sv, m_items.size());

      // Group items by type for organized logging
      std::map<ItemType, std::vector<const InventoryItem*>> itemsByType;
      for (const auto& item : m_items) {
      itemsByType[item.data.type].push_back(&item);
      }

      for (const auto& [type, items] : itemsByType) {
      logger::info("--- {} ({} items) ---"sv, ItemTypeToString(type), items.size());
      for (const auto* item : items) {
        if (item->data.type == ItemType::SoulGem) {
           logger::debug("  {} x{} (capacity={:.0f}, filled={})"sv,
            item->data.name,
            item->count,
            item->data.magnitude,
            item->data.isFilled);
        } else {
           logger::debug("  {} x{} (mag={:.1f}, dur={:.1f})"sv,
            item->data.name,
            item->count,
            item->data.magnitude,
            item->data.duration);
        }
      }
      }

      logger::info("=== End Item Registry ==="sv);
   }

   // =============================================================================
   // COMBINED INVENTORY SCAN (v0.7.19)
   // =============================================================================
   // Single traversal for both alchemy items and soul gems, reducing SKSE API
   // calls by 50% on the 500ms hot path. Replaces separate scan methods.
   // =============================================================================

   InventoryScanResult ItemRegistry::ScanPlayerInventoryAll() const
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[ItemRegistry] Player not available for combined inventory scan"sv);
      return {};
      }
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return ScanPlayerInventoryAll(player);
   }

   InventoryScanResult ItemRegistry::ScanPlayerInventoryAll(RE::PlayerCharacter* player) const
   {
      InventoryScanResult result;

      if (!player) {
      logger::debug("[ItemRegistry] ScanPlayerInventoryAll called with null player"sv);
      return result;
      }

      // FIX (v0.12.x): Use GetInventory() to include base container items (starting potions, etc.)
      // The old entryList + countDelta approach missed items from the player's base container
      // because countDelta only tracks changes, not the total count.
      auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::AlchemyItem) || obj.Is(RE::FormType::SoulGem);
      });

      // Pre-allocate reasonable capacities
      result.alchemyItems.reserve(64);
      result.soulGems.reserve(16);

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      // Try alchemy item first (more common)
      if (auto* alchemyItem = obj->As<RE::AlchemyItem>()) {
        result.alchemyItems.push_back({
           .item = alchemyItem,
           .formID = alchemyItem->GetFormID(),
           .count = count
        });
      }
      // Then try soul gem
      else if (auto* soulGem = obj->As<RE::TESSoulGem>()) {
        int32_t filledCount = 0;

        // Check 1: Pre-filled soul gems (vendor/loot) store the soul on the
        // base form itself. These are separate FormIDs from their empty variants
        // (e.g. SoulGemPettyFilled vs SoulGemPetty) and have no ExtraSoul data.
        if (soulGem->GetContainedSoul() != RE::SOUL_LEVEL::kNone) {
           filledCount = count;  // All instances of this base form are filled
        }
        // Check 2: Player-filled gems (via Soul Trap) use ExtraSoul extra data
        // attached at runtime to an empty gem base form.
        else if (entry && entry->extraLists) {
           for (auto* extraList : *entry->extraLists) {
            if (!extraList) continue;
            if (auto* extraSoul = extraList->GetByType<RE::ExtraSoul>()) {
              auto soulLevel = extraSoul->GetContainedSoul();
              if (soulLevel != RE::SOUL_LEVEL::kNone) {
                ++filledCount;
              }
            }
           }
        }

        result.soulGems.push_back({
           .soulGem = soulGem,
           .formID = soulGem->GetFormID(),
           .count = count,
           .filledCount = filledCount
        });

        logger::trace("[ItemRegistry] Soul gem scan: {} total={}, filled={}"sv,
           soulGem->GetName(), count, filledCount);
      }
      }

      return result;
   }

   // =============================================================================
   // LEGACY SCAN METHODS (deprecated, kept for compatibility)
   // =============================================================================

   std::vector<std::pair<RE::AlchemyItem*, int32_t>> ItemRegistry::ScanPlayerInventory() const
   {
      std::vector<std::pair<RE::AlchemyItem*, int32_t>> items;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[ItemRegistry] Player not available for inventory scan"sv);
      return items;
      }

      // Get player's inventory changes
      auto* invChanges = player->GetInventoryChanges();
      if (!invChanges || !invChanges->entryList) {
      logger::debug("[ItemRegistry] No inventory changes available"sv);
      return items;
      }

      // Pre-allocate reasonable capacity
      items.reserve(64);

      // Single traversal of inventory entry list
      for (auto* entry : *invChanges->entryList) {
      if (!entry || !entry->object) continue;

      // Only process AlchemyItems (potions, poisons, food, ingredients)
      auto* alchemyItem = entry->object->As<RE::AlchemyItem>();
      if (!alchemyItem) continue;

      // Get item count (countDelta is the change from base container)
      // For player inventory, this is effectively the current count
      int32_t count = entry->countDelta;

      // Skip items with zero or negative count
      if (count <= 0) continue;

      items.emplace_back(alchemyItem, count);
      }

      return items;
   }

   std::vector<std::pair<RE::TESSoulGem*, int32_t>> ItemRegistry::ScanPlayerSoulGems() const
   {
      std::vector<std::pair<RE::TESSoulGem*, int32_t>> soulGems;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[ItemRegistry] Player not available for soul gem scan"sv);
      return soulGems;
      }

      // Get player's inventory changes
      auto* invChanges = player->GetInventoryChanges();
      if (!invChanges || !invChanges->entryList) {
      logger::debug("[ItemRegistry] No inventory changes available"sv);
      return soulGems;
      }

      // Single traversal of inventory entry list
      for (auto* entry : *invChanges->entryList) {
      if (!entry || !entry->object) continue;

      // Only process Soul Gems (v0.7.8)
      auto* soulGem = entry->object->As<RE::TESSoulGem>();
      if (!soulGem) continue;

      // Get item count
      int32_t count = entry->countDelta;

      // Skip items with zero or negative count
      if (count <= 0) continue;

      soulGems.emplace_back(soulGem, count);
      logger::trace("[ItemRegistry] Found soul gem: {} (count={})"sv,
        soulGem->GetName(), count);
      }

      return soulGems;
   }

   void ItemRegistry::AddItem(RE::AlchemyItem* item, int32_t count)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!item) return;

      RE::FormID formID = item->GetFormID();

      // M2 (v0.7.21): Single lookup instead of contains() + find()
      auto it = m_formIDIndex.find(formID);
      if (it != m_formIDIndex.end()) {
      logger::debug("[ItemRegistry] Item {:08X} already registered, updating count"sv, formID);
      m_items[it->second].count = count;
      return;
      }

      // Classify directly (cheap, ~0.01ms per form)
      ItemData itemData = m_classifier.ClassifyItem(item);

      // Skip if classification failed
      if (itemData.formID == 0) {
      logger::warn("[ItemRegistry] Failed to classify item, skipping"sv);
      return;
      }

      // Create inventory item and add to registry
      InventoryItem invItem{
      .data = itemData,
      .count = count,
      .previousCount = count  // Initialize to current count on first add
      };

      // Add to dual-index storage
      size_t index = m_items.size();
      m_items.push_back(std::move(invItem));
      m_formIDIndex[formID] = index;

      logger::trace("[ItemRegistry] Registered item: {} x{}"sv,
      itemData.name,
      count);
   }

   // Add soul gem to registry (v0.7.8, v0.10.0: filledCount tracking)
   // Note: Soul gems use ClassifySoulGem() but are NOT cached (v0.7.10)
   // Rationale: Classification is trivial (just capacity lookup), caching overhead not justified
   void ItemRegistry::AddSoulGem(RE::TESSoulGem* soulGem, int32_t count, int32_t filledCount)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!soulGem) return;

      RE::FormID formID = soulGem->GetFormID();

      // M2 (v0.7.21): Single lookup instead of contains() + find()
      auto it = m_formIDIndex.find(formID);
      if (it != m_formIDIndex.end()) {
      logger::debug("[ItemRegistry] Soul gem {:08X} already registered, updating count/filled"sv, formID);
      m_items[it->second].count = count;
      // v0.10.0: Update fill state - filled if ANY gems of this type are filled
      m_items[it->second].data.isFilled = (filledCount > 0);
      return;
      }

      // Classify soul gem using ItemClassifier (v0.7.10)
      // Note: We skip caching because classification is trivial (just capacity lookup)
      // and cache overhead would exceed the cost of direct classification
      ItemData itemData = ItemClassifier::ClassifySoulGem(soulGem);

      // Sanity check
      if (itemData.formID == 0) {
      logger::warn("[ItemRegistry] Failed to classify soul gem {:08X}, skipping"sv, formID);
      return;
      }

      // v0.10.0: Set fill state based on extraData scan
      itemData.isFilled = (filledCount > 0);

      // Create inventory item and add to registry
      InventoryItem invItem{
      .data = itemData,
      .count = count,
      .previousCount = count  // Initialize to current count on first add
      };

      // Add to dual-index storage
      size_t index = m_items.size();
      m_items.push_back(std::move(invItem));
      m_formIDIndex[formID] = index;

      logger::info("[ItemRegistry] Registered soul gem: {} (capacity={:.0f}) x{} (filled={})"sv,
      itemData.name,
      itemData.magnitude,
      count,
      itemData.isFilled);
   }

   bool ItemRegistry::RemoveItem(RE::FormID formID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      // Find the item in the index
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return false;  // Item not found
      }

      const size_t indexToRemove = it->second;

      // Capture name before potential move
      std::string itemName = m_items[indexToRemove].data.name;

      // Remove from FormID index first using iterator (avoids second lookup)
      m_formIDIndex.erase(it);

      // Swap-remove pattern: O(1) deletion from vector
      const size_t lastIndex = m_items.size() - 1;
      if (indexToRemove != lastIndex) {
      // Move last element into the removed slot
      m_items[indexToRemove] = std::move(m_items[lastIndex]);
      // Update index for the moved element
      m_formIDIndex[m_items[indexToRemove].data.formID] = indexToRemove;
      }

      // Remove last element
      m_items.pop_back();

      logger::info("[ItemRegistry] Removed item: {} ({:08X})"sv, itemName, formID);

      return true;
   }

   // Thread-safe accessor implementations (v0.7.12)
   size_t ItemRegistry::GetItemCount() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return m_items.size();
   }

   std::vector<InventoryItem> ItemRegistry::GetAllItems() const
   {
      std::shared_lock lock(m_mutex);
      return m_items;  // Returns copy for thread safety
   }
}
