#include "ItemRegistry.h"
#include "Config.h"
#include "util/ScopedTimer.h"
#include "util/AtomicGuard.h"
#include "util/InventoryUtil.h"
#include "util/ExtraListStability.h"

namespace Huginn::Item
{
   ItemRegistry::ItemRegistry()
   {
      // Load overrides from default location.
      // Path is retained so RebuildRegistry() can re-load on demand (hot-reload),
      // matching SpellRegistry — before this, item overrides loaded ONCE at
      // construction, so editing them needed a full game restart while spell
      // overrides picked up on `hg rebuild`.
      m_overridesPath = std::filesystem::path("Data/SKSE/Plugins/Huginn_Overrides.ini");
      LoadOverrides(m_overridesPath);
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

      // Re-load classification overrides so edits to the INI are picked up on
      // `hg rebuild` / `hg reset all` without a game restart. Cheap (small file).
      // Mirrors SpellRegistry::RebuildRegistry.
      if (!m_overridesPath.empty()) {
      m_classifier.LoadOverrides(m_overridesPath);
      }

      // OPTIMIZATION (v0.7.19): Single traversal for both item types
      auto scanResult = ScanPlayerInventoryAll();
      auto& inventoryItems = scanResult.alchemyItems;
      auto& soulGems = scanResult.soulGems;
      logger::info("Found {} alchemy items, {} soul gems in player inventory"sv,
      inventoryItems.size(), soulGems.size());

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Clear existing data
      ClearStoreLocked();

      // Reserve space for both containers to avoid reallocation/rehashing
      const size_t capacity = std::min(inventoryItems.size() + soulGems.size(), Config::MAX_TRACKED_ITEMS);
      m_entries.reserve(capacity);
      m_formIDIndex.reserve(capacity);

      // Classify and add each item (AddItem assumes lock is held by caller)
      for (const auto& scanned : inventoryItems) {
      if (m_entries.size() >= Config::MAX_TRACKED_ITEMS) {
        logger::warn("Item registry reached max capacity ({}), some items skipped"sv,
           Config::MAX_TRACKED_ITEMS);
        break;
      }
      AddItem(scanned.item, scanned.count);
      }

      // Add soul gems (v0.7.8) - separate form type (AddSoulGem assumes lock is held)
      for (const auto& scanned : soulGems) {
      if (m_entries.size() >= Config::MAX_TRACKED_ITEMS) {
        logger::warn("Item registry reached max capacity ({}), soul gems skipped"sv,
           Config::MAX_TRACKED_ITEMS);
        break;
      }
      AddSoulGem(scanned.soulGem, scanned.count, scanned.filledCount, scanned.bestSoulLevel);
      }

      logger::info("Item registry built: {} items registered"sv, m_entries.size());
      // m_isLoading cleared by guard destructor
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
      if (!player) return {};
      return RefreshCountsFromScan(ScanPlayerInventoryAll(player));
   }

   // Shared item+scroll inventory pass (UpdateLoop): reuse a pre-scanned inventory
   // map instead of running a second GetInventorySafe traversal.
   std::vector<ItemChangeEvent> ItemRegistry::RefreshCounts(
      RE::PlayerCharacter* player, const Util::InventoryItemMap& inventory)
   {
      if (!player) return {};
      return RefreshCountsFromScan(ScanPlayerInventoryAll(inventory));
   }

   std::vector<ItemChangeEvent> ItemRegistry::RefreshCountsFromScan(const InventoryScanResult& scanResult)
   {
      SCOPED_TIMER("ItemRegistry::RefreshCounts");

      // OPTIMIZATION (v0.7.19): Single traversal for both item types
      const auto& currentInventory = scanResult.alchemyItems;
      const auto& currentSoulGems = scanResult.soulGems;

      // Build map for fast lookup: FormID -> count (outside critical section)
      // OPTIMIZATION (S5 v0.7.19): Use cached formID instead of calling GetFormID() again
      // Persistent scratch members (single-caller poll thread, NOT under m_mutex):
      // clear() keeps the bucket arrays, so the 2 Hz scan is allocation-free.
      auto& currentCounts = m_scanCounts;
      auto& currentFilledCounts = m_scanFilledCounts;
      auto& currentSoulLevels = m_scanSoulLevels;
      currentCounts.clear();
      currentFilledCounts.clear();
      currentSoulLevels.clear();
      currentCounts.reserve(currentInventory.size() + currentSoulGems.size());
      for (const auto& scanned : currentInventory) {
      currentCounts[scanned.formID] = scanned.count;  // Use cached formID
      }
      // Add soul gems to the count map (v0.10.0: also track filled count)
      for (const auto& scanned : currentSoulGems) {
      currentCounts[scanned.formID] = scanned.count;  // Use cached formID
      currentFilledCounts[scanned.formID] = scanned.filledCount;  // v0.10.0
      currentSoulLevels[scanned.formID] = scanned.bestSoulLevel;
      }

      // Change events for this scan only; the caller (update loop) consumes
      // the return value immediately — nothing is buffered across scans.
      std::vector<ItemChangeEvent> changes;

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Compare tracked items with current counts
      for (auto& invItem : m_entries) {
      auto it = currentCounts.find(invItem.data.formID);
      int32_t currentCount = (it != currentCounts.end()) ? it->second : 0;
      const bool countChanged = (currentCount != invItem.count);

      if (countChanged) {
        int32_t delta = currentCount - invItem.count;

        // Emit change event
        changes.push_back(ItemChangeEvent{
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

        // Ranking value, refreshed every scan rather than only on a flip: the
        // soul can change without the BOOL changing — trap a grand soul into a
        // stack that already held a petty one and the gem is worth more while
        // isFilled stays true. Kept outside the flip branch for that reason.
        //
        // Only when the scan actually saw this form. Absent means it is not a
        // TESSoulGem at all — mod gems that are AlchemyItem forms wearing soul
        // gem keywords classify as ItemType::SoulGem but never reach the soul
        // scan, and an unguarded write zeroed the keyword-derived magnitude
        // ItemClassifier gave them, 500 ms after every registration. A form
        // that IS in the map with bestSoulLevel 0 is a real emptied gem, and
        // still writes through.
        bool soulChanged = false;
        auto soulIt = currentSoulLevels.find(invItem.data.formID);
        if (soulIt != currentSoulLevels.end()) {
           const float currentSoul = static_cast<float>(soulIt->second);
           if (invItem.data.magnitude != currentSoul) {
              invItem.data.magnitude = currentSoul;
              soulChanged = true;
           }
        }

        // v0.10.0: fill flip also updates the filled-instance count, which is
        // what the candidate path treats as the usable quantity.
        const int32_t currentFilledCount =
           (fillIt != currentFilledCounts.end()) ? fillIt->second : 0;
        invItem.data.filledCount = currentFilledCount;

        const bool fillFlipped = (currentlyFilled != invItem.data.isFilled);
        if (fillFlipped || soulChanged) {
           invItem.data.isFilled = currentlyFilled;
           // Both feed ranking, and neither moves the count, so both need the
           // zero-delta event to reach the caller's inventoryChanged →
           // MarkPageDirty signal (delta == 0 takes none of the
           // consumption/lock-break paths in the consumer). Without it the new
           // value sits in the registry unread: the pipeline's skip gate holds,
           // and a gem that just went from petty to grand keeps its old rank
           // until something unrelated dirties the page.
           // Skipped when a count event for this gem was already emitted above.
           if (!countChanged) {
              changes.push_back(ItemChangeEvent{
                 .formID = invItem.data.formID,
                 .name = invItem.data.name,
                 .type = invItem.data.type,
                 .delta = 0
              });
           }
           if (fillFlipped) {
              logger::debug("[ItemRegistry] Soul gem fill state changed: {} -> {}"sv,
               invItem.data.name, currentlyFilled ? "filled" : "empty");
           } else {
              logger::debug("[ItemRegistry] Soul gem soul changed: {} -> {:.0f}"sv,
               invItem.data.name, invItem.data.magnitude);
           }
        }
      }
      }

      return changes;
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
        if (m_entries.size() >= Config::MAX_TRACKED_ITEMS) {
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
        if (m_entries.size() >= Config::MAX_TRACKED_ITEMS) {
           logger::warn("Item registry at max capacity, cannot add new soul gems"sv);
           break;
        }

        // New soul gem found - add it
        AddSoulGem(scanned.soulGem, scanned.count, scanned.filledCount, scanned.bestSoulLevel);
        itemsAdded++;

        logger::trace("[ItemRegistry] Added new soul gem: {} x{} (filled={})"sv,
           scanned.soulGem->GetName(), scanned.count, scanned.filledCount);
      }
      }

      // STEP 2: Remove items no longer in inventory
      std::vector<RE::FormID> toRemove;
      toRemove.reserve(m_entries.size() / 10);  // Estimate ~10% removal rate

      for (const auto& invItem : m_entries) {
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
      for (auto& invItem : m_entries) {
      invItem.previousCount = invItem.count;
      }

      if (itemsAdded > 0 || itemsRemoved > 0) {
      logger::info("[ItemRegistry] Reconciliation: +{} item(s), -{} item(s), total: {}"sv,
        itemsAdded, itemsRemoved, m_entries.size());
      }

      // m_isLoading cleared by guard destructor
      return itemsAdded + itemsRemoved;
   }

   const InventoryItem* ItemRegistry::GetItem(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return nullptr;
      }
      return &m_entries[it->second];
   }

   // =============================================================================
   // ACCESSORS — thin wrappers over the FormRegistry query primitives (finding #8)
   // =============================================================================
   // Every accessor below was a hand-copied lock + loop + (optional) SortTopK. They
   // now delegate to Collect / QueryTopK / FindBest, which fold the count>0 filter
   // into the predicate and take the shared_lock internally.

   std::vector<const InventoryItem*> ItemRegistry::GetItemsByType(ItemType type) const
   {
      return Collect([type](const InventoryItem& i) {
      return i.data.type == type && i.count > 0;
      });
   }

   std::vector<const InventoryItem*> ItemRegistry::GetItemsWithTag(ItemTag tag) const
   {
      return Collect([tag](const InventoryItem& i) {
      return HasTag(i.data.tags, tag) && i.count > 0;
      });
   }

   std::vector<const InventoryItem*> ItemRegistry::GetHealthPotionsByMagnitude(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return i.data.type == ItemType::HealthPotion && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetMagickaPotionsByMagnitude(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return i.data.type == ItemType::MagickaPotion && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetStaminaPotionsByMagnitude(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return i.data.type == ItemType::StaminaPotion && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   // ---- Resist potions (tag-filtered, magnitude-sorted) ----

   std::vector<const InventoryItem*> ItemRegistry::GetResistFirePotions(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistFire) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistFrostPotions(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistFrost) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistShockPotions(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistShock) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistPoisonPotions(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistPoison) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetResistMagicPotions(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistMagic) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   // ---- Cure potions (tag-filtered, unsorted) ----

   std::vector<const InventoryItem*> ItemRegistry::GetCureDiseasePotions() const
   {
      return Collect([](const InventoryItem& i) {
      return HasTag(i.data.tags, ItemTag::CureDisease) && i.count > 0;
      });
   }

   std::vector<const InventoryItem*> ItemRegistry::GetCurePoisonPotions() const
   {
      return Collect([](const InventoryItem& i) {
      return HasTag(i.data.tags, ItemTag::CurePoison) && i.count > 0;
      });
   }

   // ---- Fortify potions (grouped tag + optional specific skill/school) ----

   std::vector<const InventoryItem*> ItemRegistry::GetFortifySchoolPotions(MagicSchool school, size_t topK) const
   {
      return QueryTopK(
      [school](const InventoryItem& i) {
        return HasTag(i.data.tags, ItemTag::FortifyMagicSchool) && i.count > 0 &&
           (school == MagicSchool::None || i.data.school == school);
      },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetFortifyCombatPotions(CombatSkill skill, size_t topK) const
   {
      return QueryTopK(
      [skill](const InventoryItem& i) {
        return HasTag(i.data.tags, ItemTag::FortifyCombatSkill) && i.count > 0 &&
           (skill == CombatSkill::None || i.data.combatSkill == skill);
      },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetFortifyUtilityPotions(UtilitySkill skill, size_t topK) const
   {
      return QueryTopK(
      [skill](const InventoryItem& i) {
        return HasTag(i.data.tags, ItemTag::FortifyUtilitySkill) && i.count > 0 &&
           (skill == UtilitySkill::None || i.data.utilitySkill == skill);
      },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   // ---- Duration-sorted potions ----

   std::vector<const InventoryItem*> ItemRegistry::GetInvisibilityPotions(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::Invisibility) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.duration; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetWaterbreathingPotions(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::Waterbreathing) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.duration; }, topK);
   }

   // =============================================================================
   // CONVENIENCE "BEST" ACCESSORS — single-pass FindBest (O(n), no allocation)
   // =============================================================================

   const InventoryItem* ItemRegistry::GetBestWaterbreathingPotion() const noexcept
   {
      // Longest duration wins (matches GetWaterbreathingPotions ordering)
      return FindBest(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::Waterbreathing) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.duration; });
   }

   const InventoryItem* ItemRegistry::GetBestHealthPotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return i.data.type == ItemType::HealthPotion && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   const InventoryItem* ItemRegistry::GetBestMagickaPotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return i.data.type == ItemType::MagickaPotion && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   const InventoryItem* ItemRegistry::GetBestStaminaPotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return i.data.type == ItemType::StaminaPotion && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   const InventoryItem* ItemRegistry::GetBestResistFirePotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistFire) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   const InventoryItem* ItemRegistry::GetBestResistFrostPotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistFrost) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   const InventoryItem* ItemRegistry::GetBestResistShockPotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistShock) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   const InventoryItem* ItemRegistry::GetBestResistPoisonPotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::ResistPoison) && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   // Cure potions are binary effects, so we prefer the most economical (lowest
   // gold value). FindBest maximizes its key, so negate value to pick the minimum.
   const InventoryItem* ItemRegistry::GetBestCureDiseasePotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::CureDisease) && i.count > 0; },
      [](const InventoryItem& i) { return -static_cast<int64_t>(i.data.value); });
   }

   const InventoryItem* ItemRegistry::GetBestCurePoisonPotion() const noexcept
   {
      return FindBest(
      [](const InventoryItem& i) { return HasTag(i.data.tags, ItemTag::CurePoison) && i.count > 0; },
      [](const InventoryItem& i) { return -static_cast<int64_t>(i.data.value); });
   }

   // =============================================================================
   // SOUL GEM ACCESSORS (SoulGemScanner v0.7.8)
   // =============================================================================

   std::vector<const InventoryItem*> ItemRegistry::GetSoulGems(size_t topK) const
   {
      return QueryTopK(
      [](const InventoryItem& i) { return i.data.type == ItemType::SoulGem && i.count > 0; },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetSoulGemsBySoulLevel(float minSoulLevel, size_t topK) const
   {
      // Renamed from GetSoulGemsByCapacity: magnitude stopped being capacity
      // and became the soul held, so the old name described the opposite of
      // what the filter did. Capacity now lives in tagsExt — see
      // GetSoulGemsByCapacity below, which asks the question the old name
      // promised.
      return QueryTopK(
      [minSoulLevel](const InventoryItem& i) {
        return i.data.type == ItemType::SoulGem && i.count > 0 && i.data.magnitude >= minSoulLevel;
      },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetSoulGemsByCapacity(ItemTagExt minCapacity, size_t topK) const
   {
      // Capacity is a tag bit now, and the bits are ordered petty → grand, so
      // "at least this big" is "at or above this bit". Black carries the grand
      // bit too, so it satisfies any threshold a grand gem does.
      //
      // The ordering is load-bearing — a reshuffle of ItemTagExt would silently
      // invert this comparison, so assert it here rather than trust the enum.
      static_assert(static_cast<uint32_t>(ItemTagExt::SoulGemPetty) <
                    static_cast<uint32_t>(ItemTagExt::SoulGemLesser) &&
                    static_cast<uint32_t>(ItemTagExt::SoulGemLesser) <
                    static_cast<uint32_t>(ItemTagExt::SoulGemCommon) &&
                    static_cast<uint32_t>(ItemTagExt::SoulGemCommon) <
                    static_cast<uint32_t>(ItemTagExt::SoulGemGreater) &&
                    static_cast<uint32_t>(ItemTagExt::SoulGemGreater) <
                    static_cast<uint32_t>(ItemTagExt::SoulGemGrand),
                    "GetSoulGemsByCapacity compares capacity bits numerically — "
                    "they must stay ordered petty < lesser < common < greater < grand");
      const auto minBit = static_cast<uint32_t>(minCapacity);
      return QueryTopK(
      [minBit](const InventoryItem& i) {
        if (i.data.type != ItemType::SoulGem || i.count <= 0) return false;
        constexpr uint32_t kCapacityMask =
           static_cast<uint32_t>(ItemTagExt::SoulGemPetty) |
           static_cast<uint32_t>(ItemTagExt::SoulGemLesser) |
           static_cast<uint32_t>(ItemTagExt::SoulGemCommon) |
           static_cast<uint32_t>(ItemTagExt::SoulGemGreater) |
           static_cast<uint32_t>(ItemTagExt::SoulGemGrand);
        const uint32_t capacityBits = static_cast<uint32_t>(i.data.tagsExt) & kCapacityMask;
        return capacityBits >= minBit;
      },
      [](const InventoryItem& i) { return i.data.magnitude; }, topK);
   }

   std::vector<const InventoryItem*> ItemRegistry::GetBlackSoulGems() const
   {
      // Was `magnitude >= 6.0`, which stopped being satisfiable the moment
      // magnitude became SOUL_LEVEL (max 5) — the query returned nothing at
      // all. The record flag is the real answer and always was.
      return Collect([](const InventoryItem& i) {
      return i.data.type == ItemType::SoulGem && i.count > 0 &&
             HasTagExt(i.data.tagsExt, ItemTagExt::SoulGemBlack);
      });
   }

   const InventoryItem* ItemRegistry::GetBestSoulGem() const noexcept
   {
      // v0.10.0: Only FILLED soul gems are useful for weapon recharge.
      return FindBest(
      [](const InventoryItem& i) {
        return i.data.type == ItemType::SoulGem && i.count > 0 && i.data.isFilled;
      },
      [](const InventoryItem& i) { return i.data.magnitude; });
   }

   ItemRegistry::BestPotionPick ItemRegistry::GetBestPotion(ItemType type) const noexcept
   {
      std::shared_lock lock(m_mutex);
      BestPotionPick pick;

      // Single pass, no allocation/sort — called per-tick by override evaluation.
      // Two tracked bests (pure + any) means this doesn't reduce to a single FindBest.
      for (const auto& item : m_entries) {
      if (item.data.type != type || item.count <= 0) {
        continue;
      }
      if (!pick.any || item.data.magnitude > pick.any->data.magnitude) {
        pick.any = &item;
      }
      if ((!pick.pure || item.data.magnitude > pick.pure->data.magnitude) &&
          !item.data.HasHarmfulSideEffects()) {
        pick.pure = &item;
      }
      }

      return pick;
   }

   void ItemRegistry::LogAllItems() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      logger::info("=== Item Registry ({} items) ==="sv, m_entries.size());

      // Group items by type for organized logging
      std::map<ItemType, std::vector<const InventoryItem*>> itemsByType;
      for (const auto& item : m_entries) {
      itemsByType[item.data.type].push_back(&item);
      }

      for (const auto& [type, items] : itemsByType) {
      logger::info("--- {} ({} items) ---"sv, ItemTypeToString(type), items.size());
      for (const auto* item : items) {
        if (item->data.type == ItemType::SoulGem) {
           logger::debug("  {} x{} (soul={:.0f}, filled={})"sv,
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
      if (!player) {
      logger::debug("[ItemRegistry] ScanPlayerInventoryAll called with null player"sv);
      return {};
      }

      // FIX (v0.12.x): Use GetInventory() to include base container items (starting potions, etc.)
      // The old entryList + countDelta approach missed items from the player's base container
      // because countDelta only tracks changes, not the total count.
      return ScanPlayerInventoryAll(Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::AlchemyItem) || obj.Is(RE::FormType::SoulGem);
      }));
   }

   InventoryScanResult ItemRegistry::ScanPlayerInventoryAll(const Util::InventoryItemMap& inventory) const
   {
      InventoryScanResult result;

      // Check if safe to access extraLists — reachable straight from kPostLoadGame
      // (ReconcileItems), inside the post-load window where reading extra data can
      // crash. When not yet stable, Check 2 below is skipped; player-filled gems
      // read as empty until the next RefreshCounts pass (500ms), which updates
      // fill state per scan.
      const bool safeToAccessExtraLists = Util::IsExtraListStable();

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
        // Best soul present, not the gem's capacity: capacity is the box, this
        // is what is in it, and only this decides how much charge comes back.
        // Max across the stack because the registry keeps one entry per form and
        // GetBestSoulGem asks for the best available.
        int32_t bestSoulLevel = 0;

        // Check 1: Pre-filled soul gems (vendor/loot) store the soul on the
        // base form itself. These are separate FormIDs from their empty variants
        // (e.g. SoulGemPettyFilled vs SoulGemPetty) and have no ExtraSoul data.
        if (soulGem->GetContainedSoul() != RE::SOUL_LEVEL::kNone) {
           filledCount = count;  // All instances of this base form are filled
           bestSoulLevel = static_cast<int32_t>(soulGem->GetContainedSoul());
        }
        // Check 2: Player-filled gems (via Soul Trap) use ExtraSoul extra data
        // attached at runtime to an empty gem base form.
        else if (safeToAccessExtraLists && entry && entry->extraLists) {
           for (auto* extraList : *entry->extraLists) {
            if (!extraList) continue;
            if (auto* extraSoul = extraList->GetByType<RE::ExtraSoul>()) {
              auto soulLevel = extraSoul->GetContainedSoul();
              if (soulLevel != RE::SOUL_LEVEL::kNone) {
                ++filledCount;
                bestSoulLevel = std::max(bestSoulLevel, static_cast<int32_t>(soulLevel));
              }
            }
           }
        }

        result.soulGems.push_back({
           .soulGem = soulGem,
           .formID = soulGem->GetFormID(),
           .count = count,
           .filledCount = filledCount,
           .bestSoulLevel = bestSoulLevel
        });

        logger::trace("[ItemRegistry] Soul gem scan: {} total={}, filled={}, bestSoul={}"sv,
           soulGem->GetName(), count, filledCount, bestSoulLevel);
      }
      }

      return result;
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
      m_entries[it->second].count = count;
      return;
      }

      // Classify directly (cheap, ~0.01ms per form)
      ItemData itemData = m_classifier.ClassifyItem(item);

      // Skip if classification failed
      if (itemData.formID == 0) {
      logger::warn("[ItemRegistry] Failed to classify item, skipping"sv);
      return;
      }

      // Create inventory item and add to registry via the shared core primitive.
      InventoryItem invItem{
      .data = itemData,
      .count = count,
      .previousCount = count  // Initialize to current count on first add
      };

      PushEntryLocked(std::move(invItem), formID);

      logger::trace("[ItemRegistry] Registered item: {} x{}"sv,
      itemData.name,
      count);
   }

   // Add soul gem to registry (v0.7.8, v0.10.0: filledCount tracking)
   // Note: Soul gems use ClassifySoulGem() but are NOT cached (v0.7.10)
   // Rationale: Classification is trivial (just capacity lookup), caching overhead not justified
   void ItemRegistry::AddSoulGem(RE::TESSoulGem* soulGem, int32_t count, int32_t filledCount,
                                 int32_t bestSoulLevel)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!soulGem) return;

      RE::FormID formID = soulGem->GetFormID();

      // M2 (v0.7.21): Single lookup instead of contains() + find()
      auto it = m_formIDIndex.find(formID);
      if (it != m_formIDIndex.end()) {
      logger::debug("[ItemRegistry] Soul gem {:08X} already registered, updating count/filled"sv, formID);
      m_entries[it->second].count = count;
      // v0.10.0: Update fill state - filled if ANY gems of this type are filled
      m_entries[it->second].data.isFilled = (filledCount > 0);
      m_entries[it->second].data.filledCount = filledCount;
      // Ranking value travels with fill state — a gem filled since the last
      // scan is worth what it now holds, not what it held before.
      m_entries[it->second].data.magnitude = static_cast<float>(bestSoulLevel);
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
      itemData.filledCount = filledCount;
      // Overrides the capacity ClassifySoulGem read off the base form. The scan
      // is authoritative because it is the only place that sees ExtraSoul, which
      // is where a player-filled gem keeps its soul.
      itemData.magnitude = static_cast<float>(bestSoulLevel);

      // Create inventory item and add to registry via the shared core primitive.
      InventoryItem invItem{
      .data = itemData,
      .count = count,
      .previousCount = count  // Initialize to current count on first add
      };

      PushEntryLocked(std::move(invItem), formID);

      logger::info("[ItemRegistry] Registered soul gem: {} (soul={:.0f}) x{} (filled={})"sv,
      itemData.name,
      itemData.magnitude,
      count,
      itemData.isFilled);
   }

   bool ItemRegistry::RemoveItem(RE::FormID formID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return false;  // Item not found
      }

      // Capture name before the swap-pop moves the entry.
      const std::string itemName = m_entries[it->second].data.name;

      const bool removed = RemoveEntryLocked(formID);  // swap-pop in the shared core
      if (removed) {
      logger::info("[ItemRegistry] Removed item: {} ({:08X})"sv, itemName, formID);
      }
      return removed;
   }
}
