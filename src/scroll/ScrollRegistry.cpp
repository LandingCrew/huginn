#include "ScrollRegistry.h"
#include "Config.h"
#include "util/ScopedTimer.h"
#include "util/AtomicGuard.h"
#include "util/AlgorithmUtils.h"

namespace Huginn::Scroll
{
   using namespace Spell;  // For HasTag helper

   ScrollRegistry::ScrollRegistry(const SpellClassifier& spellClassifier)
      : m_classifier(spellClassifier)
   {
      logger::info("ScrollRegistry initialized");
   }

   void ScrollRegistry::RebuildRegistry()
   {
      logger::info("Rebuilding scroll registry..."sv);
      m_isLoading = true;

      // E3 (v0.7.21): RAII guard to ensure m_isLoading gets cleared even on exception
      Util::AtomicBoolGuard guard{ m_isLoading, false };

      // Scan player's inventory BEFORE acquiring lock (SKSE API call)
      auto inventoryScrolls = ScanPlayerInventory();
      logger::info("Found {} scrolls in player inventory"sv, inventoryScrolls.size());

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Clear existing data
      m_scrolls.clear();
      m_formIDIndex.clear();
      m_pendingChanges.clear();

      // Reserve space for both containers to avoid reallocation/rehashing
      const size_t capacity = std::min(inventoryScrolls.size(), Config::MAX_TRACKED_ITEMS);
      m_scrolls.reserve(capacity);
      m_formIDIndex.reserve(capacity);

      // Classify and add each scroll (AddScroll assumes lock is held by caller)
      for (const auto& [scroll, count] : inventoryScrolls) {
      if (m_scrolls.size() >= Config::MAX_TRACKED_ITEMS) {
        logger::warn("Scroll registry reached max capacity ({}), some scrolls skipped"sv,
           Config::MAX_TRACKED_ITEMS);
        break;
      }
      AddScroll(scroll, count);
      }

      logger::info("Scroll registry built: {} scrolls registered"sv, m_scrolls.size());
      // m_isLoading cleared by LoadingGuard destructor
   }

   std::vector<ScrollChangeEvent> ScrollRegistry::RefreshCounts()
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return {};
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return RefreshCounts(player);
   }

   std::vector<ScrollChangeEvent> ScrollRegistry::RefreshCounts(RE::PlayerCharacter* player)
   {
      SCOPED_TIMER("ScrollRegistry::RefreshCounts");

      if (!player) return {};

      // Scan current inventory counts BEFORE acquiring lock (SKSE API call)
      // OPTIMIZATION (S2 v0.7.19): Use pre-fetched player pointer
      auto currentInventory = ScanPlayerInventory(player);

      // Build map for fast lookup: FormID -> count (outside critical section)
      std::unordered_map<RE::FormID, int32_t> currentCounts;
      currentCounts.reserve(currentInventory.size());
      for (const auto& [scroll, count] : currentInventory) {
      if (scroll) {
        currentCounts[scroll->GetFormID()] = count;
      }
      }

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Track where new changes start (for return value)
      const size_t changesStart = m_pendingChanges.size();

      // Compare tracked scrolls with current counts - build directly in m_pendingChanges
      for (auto& invScroll : m_scrolls) {
      auto it = currentCounts.find(invScroll.data.formID);
      int32_t currentCount = (it != currentCounts.end()) ? it->second : 0;

      if (currentCount != invScroll.count) {
        int32_t delta = currentCount - invScroll.count;

        // Emit change event directly to pending changes
        m_pendingChanges.push_back(ScrollChangeEvent{
           .formID = invScroll.data.formID,
           .name = invScroll.data.name,
           .type = invScroll.data.type,
           .delta = delta
        });

        // Update stored count
        invScroll.count = currentCount;

        logger::trace("[ScrollRegistry] Count change: {} {} ({})"sv,
           invScroll.data.name,
           delta > 0 ? "+" : "",
           delta);
      }
      }

      // Return only the newly added changes (avoids full copy)
      return std::vector<ScrollChangeEvent>(
      m_pendingChanges.begin() + static_cast<ptrdiff_t>(changesStart),
      m_pendingChanges.end());
   }

   size_t ScrollRegistry::ReconcileScrolls()
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return 0;
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return ReconcileScrolls(player);
   }

   size_t ScrollRegistry::ReconcileScrolls(RE::PlayerCharacter* player)
   {
      SCOPED_TIMER("ScrollRegistry::ReconcileScrolls");
      m_isLoading = true;

      // E3 (v0.7.21): RAII guard to ensure m_isLoading gets cleared even on exception/early return
      Util::AtomicBoolGuard guard{ m_isLoading, false };

      if (!player) return 0;

      // Scan current inventory BEFORE acquiring lock (SKSE API call)
      // OPTIMIZATION (S2 v0.7.19): Use pre-fetched player pointer
      auto currentInventory = ScanPlayerInventory(player);

      // Build set of current FormIDs for fast lookup (outside critical section)
      std::unordered_set<RE::FormID> currentFormIDs;
      currentFormIDs.reserve(currentInventory.size());
      for (const auto& [scroll, count] : currentInventory) {
      if (scroll) {
        currentFormIDs.insert(scroll->GetFormID());
      }
      }

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      size_t scrollsAdded = 0;
      size_t scrollsRemoved = 0;

      // STEP 1: Add new scrolls (not in registry but in inventory)
      // (AddScroll assumes lock is held by caller)
      for (const auto& [scroll, count] : currentInventory) {
      if (!scroll) continue;

      RE::FormID formID = scroll->GetFormID();

      // Check if already registered
      if (!m_formIDIndex.contains(formID)) {
        // Check capacity limit
        if (m_scrolls.size() >= Config::MAX_TRACKED_ITEMS) {
           logger::warn("Scroll registry at max capacity, cannot add new scrolls"sv);
           break;
        }

        // New scroll found - add it
        AddScroll(scroll, count);
        scrollsAdded++;

        logger::debug("[ScrollRegistry] Added new scroll: {} x{}"sv, scroll->GetName(), count);
      }
      }

      // STEP 2: Remove scrolls no longer in inventory
      std::vector<RE::FormID> toRemove;
      toRemove.reserve(m_scrolls.size() / 10);  // Estimate ~10% removal rate

      for (const auto& invScroll : m_scrolls) {
      if (!currentFormIDs.contains(invScroll.data.formID)) {
        toRemove.push_back(invScroll.data.formID);
      }
      }

      for (auto formID : toRemove) {
      if (RemoveScroll(formID)) {
        scrollsRemoved++;
      }
      }

      // STEP 3: Update previousCount snapshot for all remaining scrolls
      for (auto& invScroll : m_scrolls) {
      invScroll.previousCount = invScroll.count;
      }

      if (scrollsAdded > 0 || scrollsRemoved > 0) {
      logger::info("[ScrollRegistry] Reconciliation: +{} scroll(s), -{} scroll(s), total: {}"sv,
        scrollsAdded, scrollsRemoved, m_scrolls.size());
      }

      // m_isLoading cleared by LoadingGuard destructor
      return scrollsAdded + scrollsRemoved;
   }

   const InventoryScroll* ScrollRegistry::GetScroll(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return nullptr;
      }
      return &m_scrolls[it->second];
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetScrollsByType(ScrollType type) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);  // Rough estimate

      for (const auto& scroll : m_scrolls) {
      if (scroll.data.type == type && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      return result;
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetScrollsWithTag(ScrollTag tag) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.17 - thread safety fix
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);  // Rough estimate

      for (const auto& scroll : m_scrolls) {
      if (HasTag(scroll.data.tags, tag) && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      return result;
   }

   // =============================================================================
   // TYPE-BASED ACCESSORS
   // =============================================================================

   std::vector<const InventoryScroll*> ScrollRegistry::GetDamageScrolls(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);

      for (const auto& scroll : m_scrolls) {
      if (scroll.data.type == ScrollType::Damage && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      // OPTIMIZATION (v0.7.20 H4): partial_sort for top-K is O(n log k) vs O(n log n)
      Util::SortTopK(result, [](const InventoryScroll* a, const InventoryScroll* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetHealingScrolls(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);

      for (const auto& scroll : m_scrolls) {
      if (scroll.data.type == ScrollType::Healing && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      Util::SortTopK(result, [](const InventoryScroll* a, const InventoryScroll* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetDefensiveScrolls(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);

      for (const auto& scroll : m_scrolls) {
      if (scroll.data.type == ScrollType::Defensive && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      Util::SortTopK(result, [](const InventoryScroll* a, const InventoryScroll* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetUtilityScrolls() const
   {
      // No lock here — GetScrollsByType() already acquires shared_lock(m_mutex).
      // Double shared_lock is UB (std::shared_mutex is not reentrant) and deadlocks on MSVC.
      return GetScrollsByType(ScrollType::Utility);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetSummonScrolls() const
   {
      // No lock here — GetScrollsByType() already acquires shared_lock(m_mutex).
      return GetScrollsByType(ScrollType::Summon);
   }

   // =============================================================================
   // ELEMENT-BASED ACCESSORS
   // =============================================================================

   std::vector<const InventoryScroll*> ScrollRegistry::GetFireScrolls(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);

      for (const auto& scroll : m_scrolls) {
      if (HasTag(scroll.data.tags, ScrollTag::Fire) && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      Util::SortTopK(result, [](const InventoryScroll* a, const InventoryScroll* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetFrostScrolls(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);

      for (const auto& scroll : m_scrolls) {
      if (HasTag(scroll.data.tags, ScrollTag::Frost) && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      Util::SortTopK(result, [](const InventoryScroll* a, const InventoryScroll* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetShockScrolls(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);

      for (const auto& scroll : m_scrolls) {
      if (HasTag(scroll.data.tags, ScrollTag::Shock) && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      Util::SortTopK(result, [](const InventoryScroll* a, const InventoryScroll* b) {
      return a->data.magnitude > b->data.magnitude;
      }, topK);

      return result;
   }

   // =============================================================================
   // SCHOOL-BASED ACCESSORS
   // =============================================================================

   std::vector<const InventoryScroll*> ScrollRegistry::GetScrollsBySchool(MagicSchool school) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.17 - thread safety fix
      std::vector<const InventoryScroll*> result;
      result.reserve(m_scrolls.size() / 4);

      for (const auto& scroll : m_scrolls) {
      if (scroll.data.school == school && scroll.count > 0) {
        result.push_back(&scroll);
      }
      }

      return result;
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetDestructionScrolls() const
   {
      return GetScrollsBySchool(MagicSchool::Destruction);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetRestorationScrolls() const
   {
      return GetScrollsBySchool(MagicSchool::Restoration);
   }

   // =============================================================================
   // CONVENIENCE "BEST" ACCESSORS - O(n) single-pass implementation
   // =============================================================================

   const InventoryScroll* ScrollRegistry::GetBestDamageScroll() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryScroll* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& scroll : m_scrolls) {
      if (scroll.data.type == ScrollType::Damage &&
          scroll.count > 0 &&
          scroll.data.magnitude > maxMagnitude) {
        best = &scroll;
        maxMagnitude = scroll.data.magnitude;
      }
      }
      return best;
   }

   const InventoryScroll* ScrollRegistry::GetBestHealingScroll() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryScroll* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& scroll : m_scrolls) {
      if (scroll.data.type == ScrollType::Healing &&
          scroll.count > 0 &&
          scroll.data.magnitude > maxMagnitude) {
        best = &scroll;
        maxMagnitude = scroll.data.magnitude;
      }
      }
      return best;
   }

   const InventoryScroll* ScrollRegistry::GetBestFireScroll() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryScroll* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& scroll : m_scrolls) {
      if (HasTag(scroll.data.tags, ScrollTag::Fire) &&
          scroll.count > 0 &&
          scroll.data.magnitude > maxMagnitude) {
        best = &scroll;
        maxMagnitude = scroll.data.magnitude;
      }
      }
      return best;
   }

   const InventoryScroll* ScrollRegistry::GetBestFrostScroll() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryScroll* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& scroll : m_scrolls) {
      if (HasTag(scroll.data.tags, ScrollTag::Frost) &&
          scroll.count > 0 &&
          scroll.data.magnitude > maxMagnitude) {
        best = &scroll;
        maxMagnitude = scroll.data.magnitude;
      }
      }
      return best;
   }

   const InventoryScroll* ScrollRegistry::GetBestShockScroll() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryScroll* best = nullptr;
      float maxMagnitude = 0.0f;

      for (const auto& scroll : m_scrolls) {
      if (HasTag(scroll.data.tags, ScrollTag::Shock) &&
          scroll.count > 0 &&
          scroll.data.magnitude > maxMagnitude) {
        best = &scroll;
        maxMagnitude = scroll.data.magnitude;
      }
      }
      return best;
   }

   std::vector<ScrollChangeEvent> ScrollRegistry::GetAndClearChanges()
   {
      std::unique_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<ScrollChangeEvent> result = std::move(m_pendingChanges);
      m_pendingChanges.clear();
      return result;
   }

   void ScrollRegistry::LogAllScrolls() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      logger::info("=== Scroll Registry ({} scrolls) ==="sv, m_scrolls.size());

      // Group scrolls by type for organized logging
      std::map<ScrollType, std::vector<const InventoryScroll*>> scrollsByType;
      for (const auto& scroll : m_scrolls) {
      scrollsByType[scroll.data.type].push_back(&scroll);
      }

      for (const auto& [type, scrolls] : scrollsByType) {
      logger::info("--- {} ({} scrolls) ---"sv, SpellTypeToString(type), scrolls.size());
      for (const auto* scroll : scrolls) {
        logger::debug("  {} x{} (mag={:.1f}, cost={}, school={})"sv,
           scroll->data.name,
           scroll->count,
           scroll->data.magnitude,
           scroll->data.baseCost,
           MagicSchoolToString(scroll->data.school));
      }
      }

      logger::info("=== End Scroll Registry ==="sv);
   }

   // =============================================================================
   // INTERNAL HELPERS
   // =============================================================================

   std::vector<std::pair<RE::ScrollItem*, int32_t>> ScrollRegistry::ScanPlayerInventory() const
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[ScrollRegistry] Player not available for inventory scan"sv);
      return {};
      }
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return ScanPlayerInventory(player);
   }

   std::vector<std::pair<RE::ScrollItem*, int32_t>> ScrollRegistry::ScanPlayerInventory(RE::PlayerCharacter* player) const
   {
      std::vector<std::pair<RE::ScrollItem*, int32_t>> scrolls;

      if (!player) {
      logger::debug("[ScrollRegistry] ScanPlayerInventory called with null player"sv);
      return scrolls;
      }

      // GetInventory() merges base container + inventory changes for true counts.
      // The old entryList + countDelta approach missed base container scrolls
      // (countDelta only tracks CHANGES, not total count).
      auto inventory = player->GetInventory([](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::Scroll);
      });

      scrolls.reserve(inventory.size());

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      auto* scroll = obj->As<RE::ScrollItem>();
      if (!scroll) continue;

      scrolls.emplace_back(scroll, static_cast<int32_t>(count));
      }

      return scrolls;
   }

   void ScrollRegistry::AddScroll(RE::ScrollItem* scroll, int32_t count)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!scroll) return;

      RE::FormID formID = scroll->GetFormID();

      // M2 (v0.7.21): Single lookup instead of contains() + find()
      auto it = m_formIDIndex.find(formID);
      if (it != m_formIDIndex.end()) {
      logger::debug("[ScrollRegistry] Scroll {:08X} already registered, updating count"sv, formID);
      m_scrolls[it->second].count = count;
      return;
      }

      // Classify directly (cheap, ~0.01ms per form)
      ScrollData scrollData = m_classifier.ClassifyScroll(scroll);

      // Skip if classification failed
      if (scrollData.formID == 0) {
      logger::warn("[ScrollRegistry] Failed to classify scroll, skipping"sv);
      return;
      }

      // Create inventory scroll and add to registry
      InventoryScroll invScroll{
      .data = scrollData,
      .count = count,
      .previousCount = count  // Initialize previousCount to current count
      };

      // Add to vector and index
      size_t index = m_scrolls.size();
      m_scrolls.push_back(std::move(invScroll));
      m_formIDIndex[formID] = index;

      logger::trace("[ScrollRegistry] Added scroll: {}"sv, scrollData.ToString());
   }

   bool ScrollRegistry::RemoveScroll(RE::FormID formID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return false;  // Not found
      }

      size_t index = it->second;
      const std::string removedName = m_scrolls[index].data.name;

      // Swap-remove pattern: Move last element to removed slot
      if (index != m_scrolls.size() - 1) {
      // Swap with last element
      m_scrolls[index] = std::move(m_scrolls.back());
      // Update index of swapped element
      m_formIDIndex[m_scrolls[index].data.formID] = index;
      }

      // Remove last element
      m_scrolls.pop_back();
      m_formIDIndex.erase(formID);

      logger::trace("[ScrollRegistry] Removed scroll: {}"sv, removedName);
      return true;
   }

   // Thread-safe accessor implementations (v0.7.12)
   size_t ScrollRegistry::GetScrollCount() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return m_scrolls.size();
   }

   std::vector<InventoryScroll> ScrollRegistry::GetAllScrolls() const
   {
      std::shared_lock lock(m_mutex);
      return m_scrolls;  // Returns copy for thread safety
   }
}


