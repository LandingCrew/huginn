#include "SpellRegistry.h"
#include "Config.h"
#include "learning/EquipSourceTracker.h"
#include "learning/ExternalEquipLearner.h"
#include "util/AtomicGuard.h"
#include "util/ScopedTimer.h"

namespace Huginn::Spell
{
   SpellRegistry::SpellRegistry()
   {
      // Constructor - load overrides from default location
      auto overridesPath = std::filesystem::path("Data/SKSE/Plugins/Huginn_Overrides.ini");
      LoadOverrides(overridesPath);
   }

   RE::BSEventNotifyControl SpellRegistry::ProcessEvent(const RE::TESEquipEvent* event, RE::BSTEventSource<RE::TESEquipEvent>*)
   {
      if (!event || event->actor.get() != RE::PlayerCharacter::GetSingleton()) {
      return RE::BSEventNotifyControl::kContinue;  // Not player, ignore
      }

      // Check if the equipped/unequipped item is a spell
      auto* form = RE::TESForm::LookupByID(event->baseObject);
      if (!form) {
      return RE::BSEventNotifyControl::kContinue;
      }

      auto* spell = form->As<RE::SpellItem>();
      if (!spell) {
      return RE::BSEventNotifyControl::kContinue;  // Not a spell
      }

      // Immediately refresh favorites status when spell equipment changes
      logger::debug("Spell equip event detected: {} ({:08X}) (equipped={})"sv,
      spell->GetName(), spell->GetFormID(), event->equipped);

      // NEW (v0.7.8): If spell is being equipped and not yet in registry, add it immediately
      // FIX (v0.7.17): Removed unlocked check - AddNewSpell handles duplicate check under lock
      // FIX (v0.7.18): Removed immediate RefreshFavorites() - scheduled 500ms refresh is sufficient
      if (event->equipped) {
      AddNewSpell(spell);

      // Phase 3b: External equip learning
      if (!Learning::EquipSourceTracker::GetSingleton().IsRecentHuginnEquip()) {
        Learning::ExternalEquipLearner::GetSingleton().OnExternalEquip(
            spell->GetFormID(), "Spell");
      }
      }

      // Note: RefreshFavorites() is called at 500ms intervals by Main.cpp update loop
      // FIX (v0.7.18): Removed immediate call to eliminate micro-stutter on equip events

      return RE::BSEventNotifyControl::kContinue;
   }

   void SpellRegistry::LoadOverrides(const std::filesystem::path& iniPath)
   {
      m_classifier.LoadOverrides(iniPath);
   }

   void SpellRegistry::RebuildRegistry()
   {
      logger::info("Rebuilding spell registry..."sv);
      m_isLoading = true;
      Util::AtomicBoolGuard loadingGuard{ m_isLoading, false };

      // Scan favorites and player spells BEFORE acquiring lock (SKSE API calls)
      auto favoritedSpells = ScanSpellFavorites();
      logger::info("Found {} favorited spells in inventory"sv, favoritedSpells.size());

      auto playerSpells = ScanPlayerSpells();
      logger::info("Found {} player spells to classify"sv, playerSpells.size());

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Clear existing data
      m_spells.clear();
      m_formIDIndex.clear();

      // Classify and add each spell (AddSpell assumes lock is held by caller)
      for (auto* spell : playerSpells) {
      AddSpell(spell);
      }

      // Set favorited status for newly added spells
      size_t favoritedCount = 0;
      for (auto& spell : m_spells) {
      if (favoritedSpells.contains(spell.formID)) {
        spell.isFavorited = true;
        favoritedCount++;
      }
      }

      logger::info("Spell registry built: {} spells registered ({} favorited)"sv,
      m_spells.size(), favoritedCount);
   }

   bool SpellRegistry::AddNewSpell(RE::SpellItem* spell)
   {
      if (!spell) return false;

      // Filter to only player-castable spells (check before lock)
      auto spellType = spell->GetSpellType();
      if (spellType != RE::MagicSystem::SpellType::kSpell &&
          spellType != RE::MagicSystem::SpellType::kLeveledSpell) {
      return false;
      }

      RE::FormID formID = spell->GetFormID();

      // =========================================================================
      // PHASE 1: Quick check under shared_lock (fast, ~0.1ms)
      // FIX (v0.7.18): Move heavy classification/cache work OUTSIDE the lock
      // =========================================================================
      {
      std::shared_lock readLock(m_mutex);
      if (m_formIDIndex.contains(formID)) {
        logger::debug("Spell {:08X} already registered, skipping"sv, formID);
        return false;
      }
      }

      // =========================================================================
      // PHASE 2: Classify OUTSIDE the lock (cheap, ~0.01ms per form)
      // =========================================================================
      SpellData spellData = m_classifier.ClassifySpell(spell);
      if (spellData.formID == 0) {
      logger::warn("Failed to classify spell, skipping"sv);
      return false;
      }

      // =========================================================================
      // PHASE 3: Fast insertion under unique_lock (fast, ~0.5ms)
      // Only simple container operations
      // =========================================================================
      {
      std::unique_lock lock(m_mutex);

      // Double-check: another thread may have added it while we were classifying
      if (m_formIDIndex.contains(formID)) {
        logger::debug("Spell {:08X} added by another thread, skipping"sv, formID);
        return false;
      }

      // Add to registry (fast in-memory operations only)
      size_t index = m_spells.size();
      m_spells.push_back(spellData);
      m_formIDIndex[formID] = index;
      }

      logger::info("Dynamically added new spell: {} ({:08X})"sv, spell->GetName(), spell->GetFormID());

      // Debug notification (outside lock)
      std::string msg = std::format("Learned: {}", spell->GetName());
      RE::DebugNotification(msg.c_str());

      return true;
   }

   size_t SpellRegistry::ReconcileSpells()
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return 0;
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return ReconcileSpells(player);
   }

   size_t SpellRegistry::ReconcileSpells(RE::PlayerCharacter* player)
   {
      SCOPED_TIMER("ReconcileSpells");
      m_isLoading = true;
      Util::AtomicBoolGuard loadingGuard{ m_isLoading, false };

      if (!player) return 0;

      // Scan current player spells BEFORE acquiring lock (SKSE API call)
      // OPTIMIZATION (S2 v0.7.19): Use pre-fetched player pointer
      auto currentSpells = ScanPlayerSpells(player);

      // Build set of current spell FormIDs for fast lookup
      std::unordered_set<RE::FormID> currentFormIDs;
      currentFormIDs.reserve(currentSpells.size());
      for (auto* spell : currentSpells) {
      if (spell) {
        currentFormIDs.insert(spell->GetFormID());
      }
      }

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      size_t newSpellsAdded = 0;
      size_t spellsRemoved = 0;

      // STEP 1: Add new spells (registry <- current)
      for (auto* spell : currentSpells) {
      if (!spell) continue;

      // Check if already in registry
      if (!m_formIDIndex.contains(spell->GetFormID())) {
        // New spell found - add it (AddSpell assumes lock is held by caller)
        AddSpell(spell);
        newSpellsAdded++;
      }
      }

      // STEP 2: Remove spells no longer known (registry -> current)
      std::vector<RE::FormID> toRemove;
      toRemove.reserve(m_spells.size() / 10);  // Optimize: estimate ~10% removal rate
      for (const auto& spell : m_spells) {
      if (!currentFormIDs.contains(spell.formID)) {
        toRemove.push_back(spell.formID);
      }
      }

      // Remove spells that are no longer known (RemoveSpell assumes lock is held by caller)
      for (auto formID : toRemove) {
      if (RemoveSpell(formID)) {
        spellsRemoved++;
      }
      }

      if (newSpellsAdded > 0 || spellsRemoved > 0) {
      logger::info("Reconciliation: +{} spell(s), -{} spell(s), total: {}"sv,
        newSpellsAdded, spellsRemoved, m_spells.size());
      }

      // E2 (v0.7.21): Clear loading flag BEFORE calling RefreshFavorites
      // DESIGN NOTE: IsLoading() covers the main spell registry mutations (add/remove spells),
      // not the subsequent favorites refresh. This is intentional because:
      // 1. The spell data is fully consistent at this point (all adds/removes complete)
      // 2. RefreshFavorites() only updates the isFavorited bool, not structural data
      // 3. Keeping m_isLoading=true during RefreshFavorites would block reads unnecessarily
      // The RAII guard also resets to false at scope exit (harmless double-set).
      m_isLoading.store(false, std::memory_order_release);

      // Unlock before calling RefreshFavorites (it will acquire its own lock)
      lock.unlock();

      // Refresh favorites status as part of reconciliation
      RefreshFavorites();

      return newSpellsAdded + spellsRemoved;
   }

   size_t SpellRegistry::RefreshFavorites()
   {
      // Note: RefreshFavorites doesn't actually need player (uses MagicFavorites singleton)
      // The overload exists for API consistency with other registries
      return RefreshFavorites(nullptr);
   }

   size_t SpellRegistry::RefreshFavorites([[maybe_unused]] RE::PlayerCharacter* player)
   {
      // Note: player parameter unused - RefreshFavorites uses MagicFavorites singleton
      // Parameter exists for API consistency (S2 v0.7.19)

      // Scan current favorites from inventory BEFORE acquiring lock (SKSE API call)
      auto currentFavorites = ScanSpellFavorites();

      logger::trace("RefreshFavorites: Found {} currently equipped spells"sv, currentFavorites.size());

      std::unique_lock lock(m_mutex);  // v0.7.12 - thread safety

      size_t changesCount = 0;

      // Update favorited status for all tracked spells
      for (auto& spell : m_spells) {
      bool wasFavorited = spell.isFavorited;
      bool isFavorited = currentFavorites.contains(spell.formID);

      if (wasFavorited != isFavorited) {
        spell.isFavorited = isFavorited;
        changesCount++;

        logger::trace("Spell {} ({:08X}): favorite status changed {} -> {}"sv,
           spell.name, spell.formID, wasFavorited, isFavorited);
      }
      }

      if (changesCount > 0) {
      logger::trace("Favorites refresh: {} spell(s) changed status"sv, changesCount);
      } else {
      logger::trace("Favorites refresh: No changes detected"sv);
      }

      return changesCount;
   }

   const SpellData* SpellRegistry::GetSpellData(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return nullptr;
      }

      return &m_spells[it->second];
   }

   std::vector<const SpellData*> SpellRegistry::GetSpellsByType(SpellType type) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const SpellData*> result;
      result.reserve(m_spells.size() / 4);  // Rough estimate

      for (const auto& spell : m_spells) {
      if (spell.type == type) {
        result.push_back(&spell);
      }
      }

      return result;
   }

   size_t SpellRegistry::GetSpellCountByType(SpellType type) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      return std::ranges::count_if(m_spells, [type](const auto& spell) {
      return spell.type == type;
      });
   }

   std::vector<const SpellData*> SpellRegistry::GetSpellsWithTag(SpellTag tag) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const SpellData*> result;
      result.reserve(m_spells.size() / 4);  // Rough estimate

      for (const auto& spell : m_spells) {
      if (HasTag(spell.tags, tag)) {
        result.push_back(&spell);
      }
      }

      return result;
   }

   std::vector<const SpellData*> SpellRegistry::GetCastableSpells(float currentMagicka) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const SpellData*> result;
      result.reserve(m_spells.size());

      for (const auto& spell : m_spells) {
      // Filter out concentration spells - they need continuous magicka
      // For concentration spells, check if player has enough to start casting
      if (static_cast<float>(spell.baseCost) <= currentMagicka) {
        result.push_back(&spell);
      }
      }

      return result;
   }

   std::vector<const SpellData*> SpellRegistry::GetFavoritedSpells() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const SpellData*> result;
      result.reserve(m_spells.size() / 8);  // Rough estimate

      for (const auto& spell : m_spells) {
      if (spell.isFavorited) {
        result.push_back(&spell);
      }
      }

      return result;
   }

   bool SpellRegistry::IsFavorited(RE::FormID formID) const
   {
      auto* spell = GetSpellData(formID);  // GetSpellData acquires its own shared_lock
      return spell ? spell->isFavorited : false;
   }

   size_t SpellRegistry::GetSpellCount() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      return m_spells.size();
   }

   std::vector<SpellData> SpellRegistry::GetAllSpells() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      return m_spells;  // Returns copy for thread safety
   }

   void SpellRegistry::LogAllSpells() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      logger::info("=== Spell Registry ({} spells) ==="sv, m_spells.size());

      // Group spells by type for organized logging
      std::map<SpellType, std::vector<const SpellData*>> spellsByType;
      for (const auto& spell : m_spells) {
      spellsByType[spell.type].push_back(&spell);
      }

      for (const auto& [type, spells] : spellsByType) {
      logger::info("--- {} ({} spells) ---"sv, SpellTypeToString(type), spells.size());
      for (const auto* spell : spells) {
        logger::debug("  {}"sv, spell->ToString());
      }
      }

      logger::info("=== End Spell Registry ==="sv);
   }

   std::vector<RE::SpellItem*> SpellRegistry::ScanPlayerSpells() const
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::error("Failed to get player character"sv);
      return {};
      }
      // OPTIMIZATION (S2 v0.7.19): Delegate to player-accepting version
      return ScanPlayerSpells(player);
   }

   std::vector<RE::SpellItem*> SpellRegistry::ScanPlayerSpells(RE::PlayerCharacter* player) const
   {
      std::vector<RE::SpellItem*> spells;

      if (!player) {
      logger::error("ScanPlayerSpells called with null player"sv);
      return spells;
      }

      // Helper: filter to player-castable spells only
      auto isPlayerCastable = [](RE::SpellItem* spell) -> bool {
      if (!spell) return false;
      auto spellType = spell->GetSpellType();
      return spellType == RE::MagicSystem::SpellType::kSpell ||
             spellType == RE::MagicSystem::SpellType::kLeveledSpell;
      };

      // Use a set to deduplicate (a spell could appear in both base and added lists)
      std::unordered_set<RE::FormID> seen;

      // Source 1: Base spells from NPC/Race record (Flames, Healing, racial powers, etc.)
      // These are defined on the actor's base template, NOT added at runtime.
      // In heavily modded setups, race mods may inject spells here that don't exist in vanilla.
      auto* actorBase = player->GetActorBase();
      if (actorBase) {
      auto* spellData = actorBase->GetSpellList();
      if (spellData && spellData->spells) {
        for (uint32_t i = 0; i < spellData->numSpells; ++i) {
           auto* spell = spellData->spells[i];
           if (isPlayerCastable(spell) && seen.insert(spell->GetFormID()).second) {
            spells.push_back(spell);
           }
        }
        static size_t s_lastBaseCount = SIZE_MAX;
        if (spells.size() != s_lastBaseCount) {
           logger::info("ScanPlayerSpells: {} base spells from actor template"sv, spells.size());
           s_lastBaseCount = spells.size();
        }
      }
      }

      // Source 2: Runtime-added spells (spell tomes, quest rewards, console commands)
      // In modded setups, perks/scripts may add spells at runtime that overlap with base spells.
      auto& addedSpells = player->GetActorRuntimeData().addedSpells;
      size_t addedCount = 0;
      size_t dupCount = 0;
      for (auto* spell : addedSpells) {
      if (!isPlayerCastable(spell)) continue;
      if (seen.insert(spell->GetFormID()).second) {
        spells.push_back(spell);
        addedCount++;
      } else {
        dupCount++;
        logger::trace("ScanPlayerSpells: [added] {:08X} '{}' skipped (duplicate of base)"sv,
           spell->GetFormID(), spell->GetName());
      }
      }
      static size_t s_lastAddedCount = SIZE_MAX;
      if (addedCount != s_lastAddedCount) {
        logger::info("ScanPlayerSpells: {} added spells from runtime ({} duplicates skipped)"sv,
           addedCount, dupCount);
        s_lastAddedCount = addedCount;
      }

      return spells;
   }

   std::unordered_set<RE::FormID> SpellRegistry::ScanSpellFavorites() const
   {
      std::unordered_set<RE::FormID> favoritedSpells;

      // PRIMARY: Use MagicFavorites singleton to detect ALL favorited spells
      auto* magicFavorites = RE::MagicFavorites::GetSingleton();
      if (magicFavorites) {
      // Scan favorited spells array (ALL favorited spells from Favorites menu)
      for (auto* form : magicFavorites->spells) {
        if (!form) continue;

        auto* spell = form->As<RE::SpellItem>();
        if (!spell) continue;

        favoritedSpells.insert(spell->GetFormID());
        logger::trace("Found favorited spell via MagicFavorites: {} ({:08X})"sv,
           spell->GetName(), spell->GetFormID());
      }

      // Also check hotkey-assigned spells (hotkeys 1-8)
      for (auto* form : magicFavorites->hotkeys) {
        if (!form) continue;

        auto* spell = form->As<RE::SpellItem>();
        if (!spell) continue;

        // Avoid duplicates (spell might be in both arrays)
        if (!favoritedSpells.contains(spell->GetFormID())) {
           favoritedSpells.insert(spell->GetFormID());
           logger::trace("Found hotkey-assigned spell: {} ({:08X})"sv,
            spell->GetName(), spell->GetFormID());
        }
      }

      logger::trace("MagicFavorites scan: {} favorited spells found"sv, favoritedSpells.size());
      } else {
      // FALLBACK: Detect currently equipped spells (unlikely to fail)
      logger::warn("MagicFavorites singleton unavailable - falling back to equipped spell detection"sv);

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
        logger::error("Failed to get player character for favorites scan"sv);
        return favoritedSpells;
      }

      // Check equipped spells (left/right/power)
      auto* leftSpell = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kLeftHand];
      if (leftSpell) {
        favoritedSpells.insert(leftSpell->GetFormID());
        logger::trace("Left hand spell (fallback): {} ({:08X})"sv, leftSpell->GetName(), leftSpell->GetFormID());
      }

      auto* rightSpell = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
      if (rightSpell) {
        favoritedSpells.insert(rightSpell->GetFormID());
        logger::trace("Right hand spell (fallback): {} ({:08X})"sv, rightSpell->GetName(), rightSpell->GetFormID());
      }

      auto* powerSpell = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kPowerOrShout];
      if (powerSpell) {
        favoritedSpells.insert(powerSpell->GetFormID());
        logger::trace("Power/shout (fallback): {} ({:08X})"sv, powerSpell->GetName(), powerSpell->GetFormID());
      }

      logger::info("Fallback scan: {} equipped spells found"sv, favoritedSpells.size());
      }

      return favoritedSpells;
   }

   void SpellRegistry::AddSpell(RE::SpellItem* spell)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!spell) return;

      RE::FormID formID = spell->GetFormID();

      // Check for duplicate FormID first (fast path)
      if (m_formIDIndex.contains(formID)) {
      logger::debug("Spell {:08X} already registered, skipping"sv, formID);
      return;
      }

      // Classify directly (cheap, ~0.01ms per form)
      SpellData spellData = m_classifier.ClassifySpell(spell);

      // Skip if classification failed
      if (spellData.formID == 0) {
      logger::warn("Failed to classify spell, skipping"sv);
      return;
      }

      // Add to registry
      size_t index = m_spells.size();
      m_spells.push_back(spellData);
      m_formIDIndex[spellData.formID] = index;

      logger::trace("Registered spell: {}"sv, spellData.ToString());
   }

   bool SpellRegistry::RemoveSpell(RE::FormID formID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      // Find the spell in the index
      auto it = m_formIDIndex.find(formID);
      if (it == m_formIDIndex.end()) {
      return false;  // Spell not found
      }

      size_t indexToRemove = it->second;
      std::string spellName = m_spells[indexToRemove].name;

      // Remove from vector by swapping with last element
      if (indexToRemove < m_spells.size() - 1) {
      // Swap with last element
      std::swap(m_spells[indexToRemove], m_spells.back());

      // Update the index of the swapped spell
      m_formIDIndex[m_spells[indexToRemove].formID] = indexToRemove;
      }

      // Remove last element
      m_spells.pop_back();

      // Remove from FormID index
      m_formIDIndex.erase(formID);

      logger::info("Removed spell: {} ({:08X})"sv, spellName, formID);

      // Debug notification
      std::string msg = std::format("Forgot: {}", spellName);
      RE::DebugNotification(msg.c_str());

      return true;
   }
}
