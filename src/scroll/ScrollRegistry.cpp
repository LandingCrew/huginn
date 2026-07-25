#include "ScrollRegistry.h"
#include "Config.h"
#include "util/ScopedTimer.h"
#include "util/AtomicGuard.h"
#include "util/InventoryUtil.h"

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
      ClearStoreLocked();

      // Reserve space for both containers to avoid reallocation/rehashing
      const size_t capacity = std::min(inventoryScrolls.size(), Config::MAX_TRACKED_ITEMS);
      m_entries.reserve(capacity);
      m_formIDIndex.reserve(capacity);

      // Classify and add each scroll (AddScroll assumes lock is held by caller)
      for (const auto& [scroll, count] : inventoryScrolls) {
      if (m_entries.size() >= Config::MAX_TRACKED_ITEMS) {
        logger::warn("Scroll registry reached max capacity ({}), some scrolls skipped"sv,
           Config::MAX_TRACKED_ITEMS);
        break;
      }
      AddScroll(scroll, count);
      }

      logger::info("Scroll registry built: {} scrolls registered"sv, m_entries.size());
      // m_isLoading cleared by guard destructor
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
      if (!player) return {};
      return RefreshCountsFromScan(ScanPlayerInventory(player));
   }

   // Shared item+scroll inventory pass (UpdateLoop): reuse a pre-scanned inventory
   // map instead of running a second GetInventorySafe traversal.
   std::vector<ScrollChangeEvent> ScrollRegistry::RefreshCounts(
      RE::PlayerCharacter* player, const Util::InventoryItemMap& inventory)
   {
      if (!player) return {};
      return RefreshCountsFromScan(ScanPlayerInventory(inventory));
   }

   std::vector<ScrollChangeEvent> ScrollRegistry::RefreshCountsFromScan(
      const std::vector<std::pair<RE::ScrollItem*, int32_t>>& currentInventory)
   {
      SCOPED_TIMER("ScrollRegistry::RefreshCounts");

      // Build map for fast lookup: FormID -> count (outside critical section)
      std::unordered_map<RE::FormID, int32_t> currentCounts;
      currentCounts.reserve(currentInventory.size());
      for (const auto& [scroll, count] : currentInventory) {
      if (scroll) {
        currentCounts[scroll->GetFormID()] = count;
      }
      }

      // Change events for this scan only; the caller (update loop) consumes
      // the return value immediately — nothing is buffered across scans.
      std::vector<ScrollChangeEvent> changes;

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Compare tracked scrolls with current counts
      for (auto& invScroll : m_entries) {
      auto it = currentCounts.find(invScroll.data.formID);
      int32_t currentCount = (it != currentCounts.end()) ? it->second : 0;

      if (currentCount != invScroll.count) {
        int32_t delta = currentCount - invScroll.count;

        // Emit change event
        changes.push_back(ScrollChangeEvent{
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

      return changes;
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
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
        // Check capacity limit
        if (m_entries.size() >= Config::MAX_TRACKED_ITEMS) {
           logger::warn("Scroll registry at max capacity, cannot add new scrolls"sv);
           break;
        }

        // New scroll found - add it
        AddScroll(scroll, count);
        scrollsAdded++;

        logger::debug("[ScrollRegistry] Added new scroll: {} x{}"sv, scroll->GetName(), count);
      } else {
        // Already tracked: sync count from inventory so reconcile is self-consistent
        // even if it runs before the first RefreshCounts delta scan (e.g. on load,
        // where a stale serialized count would otherwise produce a spurious delta).
        // Reconcile emits no ScrollChangeEvents, so this never double-rewards.
        //
        // TRADEOFF: this also absorbs a consumption that lands on a reconcile tick
        // when the 500ms delta scan isn't simultaneously due — that delta is lost
        // and yields no consumption reward. RefreshCounts runs first in the update
        // tick (UpdateLoop.cpp), so the window is a rare sub-500ms case. We accept
        // it: correct counts outweigh an occasional missed reward. Do NOT "fix" the
        // missed reward by leaving count stale here — that reintroduces the spurious
        // load-time delta this sync exists to suppress.
        m_entries[it->second].count = count;
      }
      }

      // STEP 2: Remove scrolls no longer in inventory
      std::vector<RE::FormID> toRemove;
      toRemove.reserve(m_entries.size() / 10);  // Estimate ~10% removal rate

      for (const auto& invScroll : m_entries) {
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
      for (auto& invScroll : m_entries) {
      invScroll.previousCount = invScroll.count;
      }

      if (scrollsAdded > 0 || scrollsRemoved > 0) {
      logger::info("[ScrollRegistry] Reconciliation: +{} scroll(s), -{} scroll(s), total: {}"sv,
        scrollsAdded, scrollsRemoved, m_entries.size());
      }

      // m_isLoading cleared by guard destructor
      return scrollsAdded + scrollsRemoved;
   }

   // =============================================================================
   // ACCESSORS — thin wrappers over the FormRegistry query primitives (finding #8)
   // =============================================================================

   std::vector<const InventoryScroll*> ScrollRegistry::GetScrollsByType(ScrollType type) const
   {
      return Collect([type](const InventoryScroll& s) {
      return s.data.type == type && s.count > 0;
      });
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetDamageScrolls(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryScroll& s) { return s.data.type == ScrollType::Damage && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; },
      topK);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetHealingScrolls(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryScroll& s) { return s.data.type == ScrollType::Healing && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; },
      topK);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetDefensiveScrolls(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryScroll& s) { return s.data.type == ScrollType::Defensive && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; },
      topK);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetUtilityScrolls() const
   {
      return GetScrollsByType(ScrollType::Utility);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetSummonScrolls() const
   {
      return GetScrollsByType(ScrollType::Summon);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetFireScrolls(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryScroll& s) { return HasTag(s.data.tags, ScrollTag::Fire) && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; },
      topK);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetFrostScrolls(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryScroll& s) { return HasTag(s.data.tags, ScrollTag::Frost) && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; },
      topK);
   }

   std::vector<const InventoryScroll*> ScrollRegistry::GetShockScrolls(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryScroll& s) { return HasTag(s.data.tags, ScrollTag::Shock) && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; },
      topK);
   }

   const InventoryScroll* ScrollRegistry::GetBestDamageScroll() const noexcept
   {
      return FindBest(
      [](const InventoryScroll& s) { return s.data.type == ScrollType::Damage && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; });
   }

   const InventoryScroll* ScrollRegistry::GetBestHealingScroll() const noexcept
   {
      return FindBest(
      [](const InventoryScroll& s) { return s.data.type == ScrollType::Healing && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; });
   }

   const InventoryScroll* ScrollRegistry::GetBestFireScroll() const noexcept
   {
      return FindBest(
      [](const InventoryScroll& s) { return HasTag(s.data.tags, ScrollTag::Fire) && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; });
   }

   const InventoryScroll* ScrollRegistry::GetBestFrostScroll() const noexcept
   {
      return FindBest(
      [](const InventoryScroll& s) { return HasTag(s.data.tags, ScrollTag::Frost) && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; });
   }

   const InventoryScroll* ScrollRegistry::GetBestShockScroll() const noexcept
   {
      return FindBest(
      [](const InventoryScroll& s) { return HasTag(s.data.tags, ScrollTag::Shock) && s.count > 0; },
      [](const InventoryScroll& s) { return s.data.magnitude; });
   }

   void ScrollRegistry::LogAllScrolls() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      logger::info("=== Scroll Registry ({} scrolls) ==="sv, m_entries.size());

      // Group scrolls by type for organized logging
      std::map<ScrollType, std::vector<const InventoryScroll*>> scrollsByType;
      for (const auto& scroll : m_entries) {
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
      if (!player) {
      logger::debug("[ScrollRegistry] ScanPlayerInventory called with null player"sv);
      return {};
      }

      // GetInventory() merges base container + inventory changes for true counts.
      // The old entryList + countDelta approach missed base container scrolls
      // (countDelta only tracks CHANGES, not total count).
      return ScanPlayerInventory(Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::Scroll);
      }));
   }

   std::vector<std::pair<RE::ScrollItem*, int32_t>> ScrollRegistry::ScanPlayerInventory(const Util::InventoryItemMap& inventory) const
   {
      std::vector<std::pair<RE::ScrollItem*, int32_t>> scrolls;
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
      m_entries[it->second].count = count;
      return;
      }

      // Classify directly (cheap, ~0.01ms per form). No formID==0 check needed:
      // ClassifyScroll guarantees no rejection path for non-null input (see the
      // CONTRACT note on its declaration).
      ScrollData scrollData = m_classifier.ClassifyScroll(scroll);

      // Create inventory scroll and add to registry via the shared core primitive.
      InventoryScroll invScroll{
      .data = scrollData,
      .count = count,
      .previousCount = count  // Initialize previousCount to current count
      };

      PushEntryLocked(std::move(invScroll), formID);

      logger::trace("[ScrollRegistry] Added scroll: {}"sv, scrollData.ToString());
   }

   bool ScrollRegistry::RemoveScroll(RE::FormID formID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return false;  // Not found
      }

      const std::string removedName = m_entries[it->second].data.name;
      const bool removed = RemoveEntryLocked(formID);  // swap-pop in the shared core
      if (removed) {
      logger::trace("[ScrollRegistry] Removed scroll: {}"sv, removedName);
      }
      return removed;
   }
}
