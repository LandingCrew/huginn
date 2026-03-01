#include "WeaponRegistry.h"
#include "Config.h"
#include "Globals.h"
#include "util/ScopedTimer.h"
#include "util/AtomicGuard.h"
#include "util/AlgorithmUtils.h"
#include "util/InventoryUtil.h"

namespace Huginn::Weapon
{
   WeaponRegistry::WeaponRegistry()
   {
      // Future: Load overrides from INI file
   }

   void WeaponRegistry::RebuildRegistry()
   {
      logger::info("Rebuilding weapon registry..."sv);
      m_isLoading = true;

      // RAII guard to ensure m_isLoading gets cleared even on exception
      Util::AtomicBoolGuard guard{ m_isLoading, false };

      // Scan BEFORE acquiring lock (SKSE API calls)
      // Note: ScanPlayerWeapons() doesn't access extraLists for safety during early load
      // Favorites will be detected by RefreshCharges() after 500ms stabilization period
      auto scannedWeapons = ScanPlayerWeapons();
      size_t favCount = std::count_if(scannedWeapons.begin(), scannedWeapons.end(),
      [](const auto& w) { return w.isFavorited; });
      logger::info("Found {} weapons in player inventory ({} favorited at scan time)"sv,
      scannedWeapons.size(), favCount);

      // Note: favCount will be 0 during initial load - favorites are updated by RefreshCharges()
      // after the 500ms extraLists stabilization period passes

      // Debug: Log favorited weapons
      if (favCount > 0) {
      logger::info("Favorited weapons:"sv);
      for (const auto& sw : scannedWeapons) {
        if (sw.isFavorited) {
           logger::info("  - {}"sv, sw.weapon->GetName());
        }
      }
      }

      auto scannedAmmo = ScanPlayerAmmo();
      logger::info("Found {} ammo types in player inventory"sv, scannedAmmo.size());

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Clear existing data
      m_weapons.clear();
      m_weaponIndex.clear();
      m_ammo.clear();
      m_ammoIndex.clear();

      // Reserve space
      m_weapons.reserve(std::min(scannedWeapons.size(), Config::MAX_TRACKED_WEAPONS));
      m_weaponIndex.reserve(m_weapons.capacity());
      m_ammo.reserve(std::min(scannedAmmo.size(), Config::MAX_TRACKED_AMMO));
      m_ammoIndex.reserve(m_ammo.capacity());

      // Add all inventory weapons (AddWeapon assumes lock is held by caller)
      for (const auto& sw : scannedWeapons) {
      if (m_weapons.size() >= Config::MAX_TRACKED_WEAPONS) {
        logger::warn("Weapon registry at max capacity ({})"sv, Config::MAX_TRACKED_WEAPONS);
        break;
      }

      AddWeapon(sw.weapon, sw.isFavorited, sw.isEquipped, sw.currentCharge, sw.maxCharge, sw.uniqueID);
      }

      // Add all ammo (AddAmmo assumes lock is held by caller)
      for (const auto& sa : scannedAmmo) {
      if (m_ammo.size() >= Config::MAX_TRACKED_AMMO) {
        logger::warn("Ammo registry at max capacity ({})"sv, Config::MAX_TRACKED_AMMO);
        break;
      }

      AddAmmo(sa.ammo, sa.count, sa.isEquipped);
      }

      logger::info("Weapon registry built: {} weapons, {} ammo types"sv,
      m_weapons.size(), m_ammo.size());
      // m_isLoading cleared by LoadingGuard destructor
   }

   void WeaponRegistry::RefreshCharges()
   {
      // CRITICAL SAFETY: Check if enough time has passed since save load (v0.7.9)
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastGameLoad);

      if (elapsed.count() < static_cast<int64_t>(Config::EXTRALIST_STABILIZATION_MS)) {
      logger::trace("[WeaponRegistry] RefreshCharges() skipped - extraLists not stable "
                    "({}ms since load, need {}ms)"sv, elapsed.count(),
                    static_cast<int64_t>(Config::EXTRALIST_STABILIZATION_MS));
      return;
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return;

      // OPTIMIZATION (v0.7.19): Query equipped weapons once and delegate
      RefreshCharges(EquippedWeapons::Query(player));
   }

   void WeaponRegistry::RefreshCharges(const EquippedWeapons& equipped)
   {
      SCOPED_TIMER("WeaponRegistry::RefreshCharges");

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return;
      }

      // =========================================================================
      // PHASE 1: Snapshot tracked FormIDs under shared_lock (fast, ~0.1ms)
      // FIX (v0.7.18): Move heavy inventory iteration OUTSIDE the lock to eliminate micro-stutter
      // =========================================================================
      std::unordered_set<RE::FormID> trackedFormIDs;
      {
      std::shared_lock readLock(m_mutex);
      trackedFormIDs.reserve(m_weapons.size());
      for (const auto& [formID, index] : m_weaponIndex) {
        trackedFormIDs.insert(formID);
      }
      }

      // =========================================================================
      // PHASE 2: Heavy SKSE API work OUTSIDE the lock (slow, 20-40ms)
      // FIX (v0.12.x): Use GetInventory() to include base container weapons
      // FIX (v0.12.x): Use IsFavorited() instead of kHotkey check (catches starred favorites)
      // =========================================================================

      // Build map of current weapon state (including extraLists data)
      auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::Weapon);
      });

      std::unordered_map<RE::FormID, ScannedWeapon> currentState;
      currentState.reserve(trackedFormIDs.size() + 8);

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      auto* weapon = obj->As<RE::TESObjectWEAP>();
      if (!weapon) continue;

      RE::FormID formID = weapon->GetFormID();

      // Skip weapons that are neither tracked nor favorited (performance optimization)
      bool isTracked = trackedFormIDs.contains(formID);
      if (!isTracked) {
        bool isFavorited = entry && entry->IsFavorited();
        if (!isFavorited) {
           continue;
        }
      }

      // Extract metadata using helper if entry data available
      if (entry) {
        ScannedWeapon sw = ExtractWeaponMetadata(weapon, entry.get(), true, equipped);
        currentState[formID] = sw;
      } else {
        ScannedWeapon sw{};
        sw.weapon = weapon;
        sw.isFavorited = false;
        sw.isEquipped = equipped.IsEquipped(weapon);
        sw.currentCharge = 0.0f;
        sw.maxCharge = 0.0f;
        sw.uniqueID = 0;
        auto* enchantable = weapon->As<RE::TESEnchantableForm>();
        const bool isStaff = weapon->GetWeaponType() == RE::WEAPON_TYPE::kStaff;
        const bool isEnchanted = (enchantable && enchantable->formEnchanting) || isStaff;
        if (isEnchanted && enchantable) {
           sw.maxCharge = static_cast<float>(enchantable->amountofEnchantment);
           sw.currentCharge = sw.maxCharge;
        }
        currentState[formID] = sw;
      }
      }

      // Scan ammo OUTSIDE lock as well
      auto scannedAmmo = ScanPlayerAmmo();
      std::unordered_map<RE::FormID, ScannedAmmo> ammoState;
      ammoState.reserve(scannedAmmo.size());
      for (const auto& sa : scannedAmmo) {
      if (sa.ammo) {
        ammoState[sa.ammo->GetFormID()] = sa;
      }
      }

      // =========================================================================
      // PHASE 3: Fast in-memory updates under unique_lock (fast, ~2-5ms)
      // Only simple assignments, no SKSE API calls
      // =========================================================================
      std::unique_lock lock(m_mutex);

      // Add newly favorited weapons
      for (const auto& [formID, sw] : currentState) {
      if (sw.isFavorited && !m_weaponIndex.contains(formID)) {
        // Found favorited weapon not in registry - add it immediately
        if (m_weapons.size() < Config::MAX_TRACKED_WEAPONS) {
           logger::info("[WeaponRegistry] Favorited weapon {} ({:08X}) not in registry - adding immediately"sv,
            sw.weapon->GetName(), formID);

           AddWeapon(sw.weapon, sw.isFavorited, sw.isEquipped,
                     sw.currentCharge, sw.maxCharge, sw.uniqueID);
        } else {
           logger::warn("[WeaponRegistry] Cannot add favorited weapon {} - registry full (max {})"sv,
            sw.weapon->GetName(), Config::MAX_TRACKED_WEAPONS);
        }
      }
      }

      // Update tracked weapons
      size_t favoritesUpdated = 0;
      for (auto& invWeapon : m_weapons) {
      auto it = currentState.find(invWeapon.data.formID);
      if (it != currentState.end()) {
        const auto& sw = it->second;

        // Update equipped status
        invWeapon.isEquipped = sw.isEquipped;

        // Update uniqueID (populated after extraList stabilization)
        if (sw.uniqueID != 0) {
           invWeapon.data.uniqueID = sw.uniqueID;
        }

        // Update favorite status - log changes
        if (invWeapon.isFavorited != sw.isFavorited) {
           logger::debug("[WeaponRegistry] Favorite status changed: {} ({:08X}) {} -> {}"sv,
            invWeapon.data.name, invWeapon.data.formID,
            invWeapon.isFavorited ? "true" : "false",
            sw.isFavorited ? "true" : "false");
           if (sw.isFavorited) {
            favoritesUpdated++;
           }
        }
        invWeapon.isFavorited = sw.isFavorited;

        // Update charge for enchanted weapons
        if (invWeapon.data.hasEnchantment && sw.maxCharge > 0.0f) {
           float chargePercent = sw.currentCharge / sw.maxCharge;
           invWeapon.previousCharge = invWeapon.data.currentCharge;
           invWeapon.data.currentCharge = chargePercent;
           invWeapon.data.maxCharge = sw.maxCharge;

           // Update NeedsCharge tag only when crossing threshold (avoids repeated bitfield ops)
           bool wasLow = HasTag(invWeapon.data.tags, WeaponTag::NeedsCharge);
           bool isLow = chargePercent < Config::WEAPON_CHARGE_LOW_THRESHOLD;
           if (wasLow != isLow) {
            if (isLow) {
              invWeapon.data.tags |= WeaponTag::NeedsCharge;
            } else {
              invWeapon.data.tags &= ~WeaponTag::NeedsCharge;
            }
           }

           // Log significant charge changes
           if (std::abs(invWeapon.data.currentCharge - invWeapon.previousCharge) > 0.05f) {
            logger::trace("[WeaponRegistry] Charge changed: {} {:.0f}% -> {:.0f}%"sv,
              invWeapon.data.name,
              invWeapon.previousCharge * 100.0f,
              invWeapon.data.currentCharge * 100.0f);
           }
        }
      }
      }

      // Log summary of favorites detection (helps diagnose timing issues)
      if (favoritesUpdated > 0) {
      size_t totalFavorited = std::count_if(m_weapons.begin(), m_weapons.end(),
        [](const auto& w) { return w.isFavorited; });
      logger::info("[WeaponRegistry] RefreshCharges updated {} weapon favorites (total now: {})"sv,
        favoritesUpdated, totalFavorited);
      }

      // Update ammo counts
      for (auto& invAmmo : m_ammo) {
      auto it = ammoState.find(invAmmo.data.formID);
      if (it != ammoState.end()) {
        invAmmo.count = it->second.count;
        invAmmo.isEquipped = it->second.isEquipped;
      } else {
        invAmmo.count = 0;  // Ammo depleted
      }
      }
   }

   size_t WeaponRegistry::ReconcileWeapons()
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return 0;

      // OPTIMIZATION (v0.7.19): Query equipped weapons once and delegate
      return ReconcileWeapons(EquippedWeapons::Query(player));
   }

   size_t WeaponRegistry::ReconcileWeapons(const EquippedWeapons& equipped)
   {
      SCOPED_TIMER("WeaponRegistry::ReconcileWeapons");
      logger::trace("[WeaponRegistry] ReconcileWeapons() triggered"sv);
      m_isLoading = true;

      // RAII guard to ensure m_isLoading gets cleared even on exception
      Util::AtomicBoolGuard guard{ m_isLoading, false };

      size_t weaponsAdded = 0;
      size_t weaponsRemoved = 0;
      size_t favoriteChanges = 0;
      size_t ammoAdded = 0;
      size_t ammoRemoved = 0;

      // === WEAPONS ===
      // Check if safe to access extraLists (v0.7.9)
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastGameLoad);
      bool safeToAccessExtraLists =
          elapsed.count() >= static_cast<int64_t>(Config::EXTRALIST_STABILIZATION_MS);

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return 0;
      }

      // FIX (v0.12.x): Use GetInventory() to include base container weapons
      auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::Weapon);
      });

      std::vector<ScannedWeapon> scannedWeapons;
      scannedWeapons.reserve(32);

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      auto* weapon = obj->As<RE::TESObjectWEAP>();
      if (!weapon) continue;

      // Use entry data if available for metadata extraction
      if (entry && safeToAccessExtraLists) {
        ScannedWeapon sw = ExtractWeaponMetadata(weapon, entry.get(), true, equipped);
        scannedWeapons.push_back(sw);
      } else {
        // No entry data or extraLists not stable - basic scan only
        ScannedWeapon sw{};
        sw.weapon = weapon;
        sw.isFavorited = false;
        sw.isEquipped = equipped.IsEquipped(weapon);
        sw.currentCharge = 0.0f;
        sw.maxCharge = 0.0f;
        sw.uniqueID = 0;

        auto* enchantable = weapon->As<RE::TESEnchantableForm>();
        if (enchantable && enchantable->formEnchanting) {
           sw.maxCharge = static_cast<float>(enchantable->amountofEnchantment);
           sw.currentCharge = sw.maxCharge;
        }
        scannedWeapons.push_back(sw);
      }
      }

      // Build set of current inventory weapon FormIDs (outside critical section)
      std::unordered_set<RE::FormID> currentWeaponIDs;
      for (const auto& sw : scannedWeapons) {
      if (sw.weapon) {
        currentWeaponIDs.insert(sw.weapon->GetFormID());
      }
      }

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      std::unique_lock lock(m_mutex);

      // Add new weapons or update existing (assumes lock is held)
      for (const auto& sw : scannedWeapons) {
      if (!sw.weapon) continue;

      RE::FormID formID = sw.weapon->GetFormID();
      if (!m_weaponIndex.contains(formID)) {
        if (m_weapons.size() < Config::MAX_TRACKED_WEAPONS) {
           AddWeapon(sw.weapon, sw.isFavorited, sw.isEquipped,
                     sw.currentCharge, sw.maxCharge, sw.uniqueID);
           weaponsAdded++;
           logger::trace("[WeaponRegistry] Added weapon: {}"sv, sw.weapon->GetName());
        }
      } else {
        // Update favorited/equipped status for existing weapons
        auto& invWeapon = m_weapons[m_weaponIndex[formID]];

        // Only update favorites if we could reliably detect them (extraLists stable)
        // Otherwise we'd overwrite RefreshCharges' correct detection with false
        if (safeToAccessExtraLists) {
           if (invWeapon.isFavorited != sw.isFavorited) {
            logger::debug("[WeaponRegistry] Favorite status changed: {} (fav={} -> {})"sv,
              sw.weapon->GetName(), invWeapon.isFavorited, sw.isFavorited);
            favoriteChanges++;
           }
           invWeapon.isFavorited = sw.isFavorited;
        }

        invWeapon.isEquipped = sw.isEquipped;

        // Update uniqueID (populated after extraList stabilization)
        if (safeToAccessExtraLists && sw.uniqueID != 0) {
           invWeapon.data.uniqueID = sw.uniqueID;
        }
      }
      }

      // Remove weapons no longer in inventory
      std::vector<RE::FormID> toRemove;
      for (const auto& invWeapon : m_weapons) {
      if (!currentWeaponIDs.contains(invWeapon.data.formID)) {
        toRemove.push_back(invWeapon.data.formID);
      }
      }
      for (auto formID : toRemove) {
      if (RemoveWeapon(formID)) {
        weaponsRemoved++;
      }
      }

      // === AMMO ===
      auto scannedAmmo = ScanPlayerAmmo();

      // Build set of current ammo FormIDs
      std::unordered_set<RE::FormID> currentAmmoIDs;
      for (const auto& sa : scannedAmmo) {
      if (sa.ammo && sa.count > 0) {
        currentAmmoIDs.insert(sa.ammo->GetFormID());
      }
      }

      // Add new ammo
      for (const auto& sa : scannedAmmo) {
      if (!sa.ammo || sa.count <= 0) continue;

      RE::FormID formID = sa.ammo->GetFormID();
      if (!m_ammoIndex.contains(formID)) {
        if (m_ammo.size() < Config::MAX_TRACKED_AMMO) {
           AddAmmo(sa.ammo, sa.count, sa.isEquipped);
           ammoAdded++;
           logger::trace("[WeaponRegistry] Added ammo: {}"sv, sa.ammo->GetName());
        }
      }
      }

      // Remove depleted ammo
      toRemove.clear();
      for (const auto& invAmmo : m_ammo) {
      if (!currentAmmoIDs.contains(invAmmo.data.formID)) {
        toRemove.push_back(invAmmo.data.formID);
      }
      }
      for (auto formID : toRemove) {
      if (RemoveAmmo(formID)) {
        ammoRemoved++;
      }
      }

      size_t totalChanges = weaponsAdded + weaponsRemoved + favoriteChanges + ammoAdded + ammoRemoved;
      if (totalChanges > 0) {
      logger::info("[WeaponRegistry] Reconciliation: +{}/{} weapons, {} fav changes, +{}/{} ammo"sv,
        weaponsAdded, weaponsRemoved, favoriteChanges, ammoAdded, ammoRemoved);
      }

      // m_isLoading cleared by LoadingGuard destructor
      return totalChanges;
   }

   // =============================================================================
   // WEAPON ACCESSORS
   // =============================================================================

   const InventoryWeapon* WeaponRegistry::GetWeapon(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      auto it = m_weaponIndex.find(formID);
      if (it == m_weaponIndex.end()) {
      return nullptr;
      }
      return &m_weapons[it->second];
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetMeleeWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(m_weapons.size() / 2);

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::Melee)) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetRangedWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(m_weapons.size() / 4);

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::Ranged)) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetOneHandedWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(m_weapons.size() / 2);

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::OneHanded)) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetTwoHandedWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(m_weapons.size() / 2);

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::TwoHanded)) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetSilveredWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(4);  // Silver weapons are relatively rare

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::Silver)) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetEnchantedWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(m_weapons.size() / 3);

      for (const auto& weapon : m_weapons) {
      if (weapon.data.hasEnchantment) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetWeaponsNeedingCharge() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(4);

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::NeedsCharge)) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetWeaponsWithTag(WeaponTag tag) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(m_weapons.size() / 4);

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, tag)) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   // =============================================================================
   // STAFF ACCESSORS (StaffScanner v0.7.7)
   // =============================================================================

   std::vector<const InventoryWeapon*> WeaponRegistry::GetStaffs() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(4);  // Staves are relatively rare

      for (const auto& weapon : m_weapons) {
      if (weapon.data.type == WeaponType::Staff) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetEnchantedStaffs() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.17 - thread safety fix
      std::vector<const InventoryWeapon*> result;
      result.reserve(4);

      for (const auto& weapon : m_weapons) {
      if (weapon.data.type == WeaponType::Staff && weapon.data.hasEnchantment) {
        result.push_back(&weapon);
      }
      }

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetFireStaffs(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(4);

      for (const auto& weapon : m_weapons) {
      if (weapon.data.type == WeaponType::Staff &&
          HasTag(weapon.data.tags, WeaponTag::EnchantFire)) {
        result.push_back(&weapon);
      }
      }

      // OPTIMIZATION (v0.7.20 H4): partial_sort for top-K is O(n log k) vs O(n log n)
      Util::SortTopK(result, [](const InventoryWeapon* a, const InventoryWeapon* b) {
      return a->data.baseDamage > b->data.baseDamage;
      }, topK);

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetFrostStaffs(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(4);

      for (const auto& weapon : m_weapons) {
      if (weapon.data.type == WeaponType::Staff &&
          HasTag(weapon.data.tags, WeaponTag::EnchantFrost)) {
        result.push_back(&weapon);
      }
      }

      Util::SortTopK(result, [](const InventoryWeapon* a, const InventoryWeapon* b) {
      return a->data.baseDamage > b->data.baseDamage;
      }, topK);

      return result;
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetShockStaffs(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryWeapon*> result;
      result.reserve(4);

      for (const auto& weapon : m_weapons) {
      if (weapon.data.type == WeaponType::Staff &&
          HasTag(weapon.data.tags, WeaponTag::EnchantShock)) {
        result.push_back(&weapon);
      }
      }

      Util::SortTopK(result, [](const InventoryWeapon* a, const InventoryWeapon* b) {
      return a->data.baseDamage > b->data.baseDamage;
      }, topK);

      return result;
   }

   const InventoryWeapon* WeaponRegistry::GetBestStaff() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryWeapon* best = nullptr;
      float maxDamage = 0.0f;

      for (const auto& weapon : m_weapons) {
      if (weapon.data.type == WeaponType::Staff &&
          weapon.data.baseDamage > maxDamage) {
        best = &weapon;
        maxDamage = weapon.data.baseDamage;
      }
      }

      return best;
   }

   const InventoryWeapon* WeaponRegistry::GetHighestChargeStaff() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryWeapon* best = nullptr;
      float maxCharge = 0.0f;

      for (const auto& weapon : m_weapons) {
      if (weapon.data.type == WeaponType::Staff &&
          weapon.data.hasEnchantment &&
          weapon.data.currentCharge > maxCharge) {
        best = &weapon;
        maxCharge = weapon.data.currentCharge;
      }
      }

      return best;
   }

   // =============================================================================
   // CONVENIENCE "BEST" ACCESSORS
   // =============================================================================

   const InventoryWeapon* WeaponRegistry::GetBestMeleeWeapon() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryWeapon* best = nullptr;
      float maxDamage = 0.0f;

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::Melee) &&
          weapon.data.baseDamage > maxDamage) {
        best = &weapon;
        maxDamage = weapon.data.baseDamage;
      }
      }

      return best;
   }

   const InventoryWeapon* WeaponRegistry::GetBestRangedWeapon() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryWeapon* best = nullptr;
      float maxDamage = 0.0f;

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::Ranged) &&
          weapon.data.baseDamage > maxDamage) {
        best = &weapon;
        maxDamage = weapon.data.baseDamage;
      }
      }

      return best;
   }

   const InventoryWeapon* WeaponRegistry::GetBestSilveredWeapon() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryWeapon* best = nullptr;
      float maxDamage = 0.0f;

      for (const auto& weapon : m_weapons) {
      if (HasTag(weapon.data.tags, WeaponTag::Silver) &&
          weapon.data.baseDamage > maxDamage) {
        best = &weapon;
        maxDamage = weapon.data.baseDamage;
      }
      }

      return best;
   }

   // =============================================================================
   // AMMO ACCESSORS
   // =============================================================================

   const InventoryAmmo* WeaponRegistry::GetAmmo(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      auto it = m_ammoIndex.find(formID);
      if (it == m_ammoIndex.end()) {
      return nullptr;
      }
      return &m_ammo[it->second];
   }

   std::vector<const InventoryAmmo*> WeaponRegistry::GetArrows(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryAmmo*> result;
      result.reserve(m_ammo.size());

      for (const auto& ammo : m_ammo) {
      if (ammo.data.type == AmmoType::Arrow && ammo.count > 0) {
        result.push_back(&ammo);
      }
      }

      // OPTIMIZATION (v0.7.20 H4): partial_sort for top-K is O(n log k) vs O(n log n)
      Util::SortTopK(result, [](const InventoryAmmo* a, const InventoryAmmo* b) {
      return a->data.baseDamage > b->data.baseDamage;
      }, topK);

      return result;
   }

   std::vector<const InventoryAmmo*> WeaponRegistry::GetBolts(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryAmmo*> result;
      result.reserve(m_ammo.size() / 2);

      for (const auto& ammo : m_ammo) {
      if (ammo.data.type == AmmoType::Bolt && ammo.count > 0) {
        result.push_back(&ammo);
      }
      }

      Util::SortTopK(result, [](const InventoryAmmo* a, const InventoryAmmo* b) {
      return a->data.baseDamage > b->data.baseDamage;
      }, topK);

      return result;
   }

   std::vector<const InventoryAmmo*> WeaponRegistry::GetMagicAmmo() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryAmmo*> result;
      result.reserve(m_ammo.size() / 4);

      for (const auto& ammo : m_ammo) {
      if (ammo.data.hasEnchantment && ammo.count > 0) {
        result.push_back(&ammo);
      }
      }

      return result;
   }

   std::vector<const InventoryAmmo*> WeaponRegistry::GetSilverAmmo(size_t topK) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      std::vector<const InventoryAmmo*> result;
      result.reserve(m_ammo.size() / 4);

      for (const auto& ammo : m_ammo) {
      if (HasTag(ammo.data.tags, WeaponTag::Silver) && ammo.count > 0) {
        result.push_back(&ammo);
      }
      }

      // OPTIMIZATION (v0.7.20 H4): partial_sort for top-K is O(n log k) vs O(n log n)
      Util::SortTopK(result, [](const InventoryAmmo* a, const InventoryAmmo* b) {
      return a->data.baseDamage > b->data.baseDamage;
      }, topK);

      return result;
   }

   const InventoryAmmo* WeaponRegistry::GetBestArrow() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryAmmo* best = nullptr;
      float maxDamage = 0.0f;

      for (const auto& ammo : m_ammo) {
      if (ammo.data.type == AmmoType::Arrow &&
          ammo.count > 0 &&
          ammo.data.baseDamage > maxDamage) {
        best = &ammo;
        maxDamage = ammo.data.baseDamage;
      }
      }

      return best;
   }

   const InventoryAmmo* WeaponRegistry::GetBestSilverArrow() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryAmmo* best = nullptr;
      float maxDamage = 0.0f;

      for (const auto& ammo : m_ammo) {
      if (ammo.data.type == AmmoType::Arrow &&
          HasTag(ammo.data.tags, WeaponTag::Silver) &&
          ammo.count > 0 &&
          ammo.data.baseDamage > maxDamage) {
        best = &ammo;
        maxDamage = ammo.data.baseDamage;
      }
      }

      return best;
   }

   const InventoryAmmo* WeaponRegistry::GetBestBolt() const noexcept
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      const InventoryAmmo* best = nullptr;
      float maxDamage = 0.0f;

      for (const auto& ammo : m_ammo) {
      if (ammo.data.type == AmmoType::Bolt &&
          ammo.count > 0 &&
          ammo.data.baseDamage > maxDamage) {
        best = &ammo;
        maxDamage = ammo.data.baseDamage;
      }
      }

      return best;
   }

   // =============================================================================
   // DEBUG
   // =============================================================================

   void WeaponRegistry::LogAllWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      logger::info("=== Weapon Registry ({} weapons, {} ammo) ==="sv,
      m_weapons.size(), m_ammo.size());

      logger::info("--- Weapons ---"sv);
      for (const auto& weapon : m_weapons) {
      logger::debug("  {}: dmg={:.1f}, tags={:08X}, fav={}, eq={}, charge={:.0f}%"sv,
        weapon.data.name,
        weapon.data.baseDamage,
        std::to_underlying(weapon.data.tags),
        weapon.isFavorited,
        weapon.isEquipped,
        weapon.data.currentCharge * 100.0f);
      }

      logger::info("--- Ammo ---"sv);
      for (const auto& ammo : m_ammo) {
      logger::debug("  {}: x{}, dmg={:.1f}, ench={}"sv,
        ammo.data.name,
        ammo.count,
        ammo.data.baseDamage,
        ammo.data.hasEnchantment);
      }

      logger::info("=== End Weapon Registry ==="sv);
   }

   // =============================================================================
   // INTERNAL HELPERS
   // =============================================================================

   WeaponRegistry::ScannedWeapon WeaponRegistry::ExtractWeaponMetadata(
      RE::TESObjectWEAP* weapon,
      RE::InventoryEntryData* entry,
      bool includeExtraLists,
      const EquippedWeapons& equipped) const
   {
      ScannedWeapon sw{};
      sw.weapon = weapon;
      sw.isFavorited = false;
      sw.isEquipped = equipped.IsEquipped(weapon);  // OPTIMIZATION (v0.7.19): Use struct helper
      sw.currentCharge = 0.0f;
      sw.maxCharge = 0.0f;
      sw.uniqueID = 0;

      // OPTIMIZATION (S10 v0.7.19): Cache RTTI cast - was called twice before
      auto* enchantable = weapon->As<RE::TESEnchantableForm>();
      const bool hasEnchantment = enchantable && enchantable->formEnchanting;

      // FIX (v0.12.x): Use IsFavorited() which catches both starred favorites AND hotkeyed items
      // The old kHotkey check only detected hotkey-assigned items (1-8), missing starred favorites.
      if (includeExtraLists && entry) {
      sw.isFavorited = entry->IsFavorited();
      }

      // Detect enchanted staves (no formEnchanting but still use charges)
      const bool isStaff = weapon->GetWeaponType() == RE::WEAPON_TYPE::kStaff;
      const bool isEnchanted = hasEnchantment || isStaff;

      // Get max charge from base form
      if (isEnchanted && enchantable) {
      sw.maxCharge = static_cast<float>(enchantable->amountofEnchantment);
      }

      // Only access extraLists if safe to do so (500ms+ after load)
      bool foundExtraCharge = false;
      if (includeExtraLists && entry && entry->extraLists) {
      for (auto* extraList : *entry->extraLists) {
        if (!extraList) continue;

        // Get enchantment charge (only created after weapon has been used)
        auto* extraCharge = extraList->GetByType<RE::ExtraCharge>();
        if (extraCharge) {
           sw.currentCharge = extraCharge->charge;
           foundExtraCharge = true;
        }

        // Get unique ID for Wheeler (identifies specific inventory instance)
        auto* extraUnique = extraList->GetByType<RE::ExtraUniqueID>();
        if (extraUnique) {
           sw.uniqueID = extraUnique->uniqueID;
        }
      }
      }

      // If enchanted but no ExtraCharge found, weapon is at full charge
      // (Skyrim only creates ExtraCharge after the weapon has been used)
      if (isEnchanted && sw.maxCharge > 0.0f && !foundExtraCharge) {
      sw.currentCharge = sw.maxCharge;
      }

      return sw;
   }

   std::vector<WeaponRegistry::ScannedWeapon> WeaponRegistry::ScanPlayerWeapons() const
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[WeaponRegistry] Player not available for weapon scan"sv);
      return {};
      }

      // OPTIMIZATION (v0.7.19): Query equipped weapons once and delegate
      return ScanPlayerWeapons(EquippedWeapons::Query(player));
   }

   std::vector<WeaponRegistry::ScannedWeapon> WeaponRegistry::ScanPlayerWeapons(const EquippedWeapons& equipped) const
   {
      std::vector<ScannedWeapon> weapons;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[WeaponRegistry] Player not available for weapon scan"sv);
      return weapons;
      }

      // FIX (v0.12.x): Use GetInventory() to include base container items (starting weapons)
      // The old entryList + countDelta approach missed items from the player's base container.
      auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::Weapon);
      });

      weapons.reserve(32);

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      auto* weapon = obj->As<RE::TESObjectWEAP>();
      if (!weapon) continue;

      ScannedWeapon sw{};
      sw.weapon = weapon;
      sw.isFavorited = false;
      sw.isEquipped = equipped.IsEquipped(weapon);
      sw.currentCharge = 0.0f;
      sw.maxCharge = 0.0f;
      sw.uniqueID = 0;

      // Skip extraLists during initial scan (time-guarded access pattern)
      // RefreshCharges() will populate metadata after stabilization period.

      // If weapon is enchanted, assume full charge (no ExtraCharge during initial scan)
      // Includes staves which don't use formEnchanting but still have charges
      auto* enchantable = weapon->As<RE::TESEnchantableForm>();
      const bool isStaff = weapon->GetWeaponType() == RE::WEAPON_TYPE::kStaff;
      const bool isEnchanted = (enchantable && enchantable->formEnchanting) || isStaff;
      if (isEnchanted && enchantable) {
        sw.maxCharge = static_cast<float>(enchantable->amountofEnchantment);
        sw.currentCharge = sw.maxCharge;  // Assume full charge until RefreshCharges
      }

      weapons.push_back(sw);
      }

      return weapons;
   }

   std::vector<WeaponRegistry::ScannedAmmo> WeaponRegistry::ScanPlayerAmmo() const
   {
      std::vector<ScannedAmmo> ammoList;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[WeaponRegistry] Player not available for ammo scan"sv);
      return ammoList;
      }

      // FIX (v0.12.x): Use GetInventory() to include base container ammo
      auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::Ammo);
      });

      // Get equipped ammo
      auto* equippedAmmo = player->GetCurrentAmmo();

      ammoList.reserve(16);

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      auto* ammo = obj->As<RE::TESAmmo>();
      if (!ammo) continue;

      ScannedAmmo sa{};
      sa.ammo = ammo;
      sa.count = count;
      sa.isEquipped = (ammo == equippedAmmo);

      ammoList.push_back(sa);
      }

      return ammoList;
   }

   std::unordered_set<RE::FormID> WeaponRegistry::ScanWeaponFavorites() const
   {
      std::unordered_set<RE::FormID> favoritedWeapons;

      // CRITICAL: Check if enough time has passed since save load for extraLists to be stable
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastGameLoad);

      if (elapsed.count() < static_cast<int64_t>(Config::EXTRALIST_STABILIZATION_MS)) {
      logger::trace("[WeaponRegistry] ScanWeaponFavorites() skipped - extraLists not stable "
                    "({}ms since load, need {}ms)"sv, elapsed.count(),
                    static_cast<int64_t>(Config::EXTRALIST_STABILIZATION_MS));
      return favoritedWeapons;  // Return empty - will be populated by RefreshCharges later
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::debug("[WeaponRegistry] Player not available for favorites scan"sv);
      return favoritedWeapons;
      }

      // FIX (v0.12.x): Use GetInventory() + IsFavorited() to include base container items
      // and properly detect starred favorites (not just hotkeyed ones).
      auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
      return obj.Is(RE::FormType::Weapon);
      });

      favoritedWeapons.reserve(16);

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      auto* weapon = obj->As<RE::TESObjectWEAP>();
      if (!weapon) continue;

      if (entry && entry->IsFavorited()) {
        favoritedWeapons.insert(weapon->GetFormID());
        logger::trace("[WeaponRegistry] Found favorited weapon: {} ({:08X})"sv,
           weapon->GetName(), weapon->GetFormID());
      }
      }

      logger::debug("[WeaponRegistry] ScanWeaponFavorites: {} favorited weapons found"sv,
      favoritedWeapons.size());

      return favoritedWeapons;
   }

   void WeaponRegistry::AddWeapon(RE::TESObjectWEAP* weapon, bool isFavorited, bool isEquipped,
                                  float currentCharge, float maxCharge, uint16_t uniqueID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!weapon) return;

      RE::FormID formID = weapon->GetFormID();

      // M2 (v0.7.21): Single lookup instead of contains() + subscript
      auto it = m_weaponIndex.find(formID);
      if (it != m_weaponIndex.end()) {
      logger::debug("[WeaponRegistry] Weapon {:08X} already registered, updating status"sv, formID);
      m_weapons[it->second].isFavorited = isFavorited;
      m_weapons[it->second].isEquipped = isEquipped;
      return;
      }

      // Classify weapon directly (no caching - classification is cheap ~0.01ms)
      // NOTE: Weapons not cached as of v0.7.11 - see CLAUDE.md design rationale
      WeaponData data = m_classifier.ClassifyWeapon(weapon);
      data.uniqueID = uniqueID;

      // Update charge info
      if (data.hasEnchantment && maxCharge > 0.0f) {
      data.currentCharge = currentCharge / maxCharge;
      data.maxCharge = maxCharge;

      if (data.currentCharge < Config::WEAPON_CHARGE_LOW_THRESHOLD) {
        data.tags |= WeaponTag::NeedsCharge;
      }
      }

      // Create inventory wrapper
      InventoryWeapon invWeapon{
      .data = std::move(data),
      .isFavorited = isFavorited,
      .isEquipped = isEquipped,
      .previousCharge = (maxCharge > 0.0f) ? currentCharge / maxCharge : 1.0f
      };

      // Add to dual-index storage
      size_t index = m_weapons.size();
      m_weapons.push_back(std::move(invWeapon));
      m_weaponIndex[formID] = index;

      logger::trace("[WeaponRegistry] Added weapon: {} (fav={}, eq={})"sv,
      weapon->GetName(), isFavorited, isEquipped);
   }

   void WeaponRegistry::AddAmmo(RE::TESAmmo* ammo, int32_t count, bool isEquipped)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!ammo) return;

      RE::FormID formID = ammo->GetFormID();

      // M2 (v0.7.21): Single lookup instead of contains() + subscript
      auto it = m_ammoIndex.find(formID);
      if (it != m_ammoIndex.end()) {
      logger::debug("[WeaponRegistry] Ammo {:08X} already registered, updating count"sv, formID);
      m_ammo[it->second].count = count;
      m_ammo[it->second].isEquipped = isEquipped;
      return;
      }

      // Classify ammo directly (no caching - classification is cheap ~0.01ms)
      // NOTE: Ammo not cached as of v0.7.11 - same rationale as weapons
      AmmoData data = m_classifier.ClassifyAmmo(ammo);

      // Create inventory wrapper
      InventoryAmmo invAmmo{
      .data = std::move(data),
      .count = count,
      .isEquipped = isEquipped
      };

      // Add to dual-index storage
      size_t index = m_ammo.size();
      m_ammo.push_back(std::move(invAmmo));
      m_ammoIndex[formID] = index;

      logger::trace("[WeaponRegistry] Added ammo: {} x{}"sv, ammo->GetName(), count);
   }

   bool WeaponRegistry::RemoveWeapon(RE::FormID formID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      auto it = m_weaponIndex.find(formID);
      if (it == m_weaponIndex.end()) {
      return false;
      }

      const size_t indexToRemove = it->second;
      std::string weaponName = m_weapons[indexToRemove].data.name;

      m_weaponIndex.erase(it);

      // Swap-remove pattern
      const size_t lastIndex = m_weapons.size() - 1;
      if (indexToRemove != lastIndex) {
      m_weapons[indexToRemove] = std::move(m_weapons[lastIndex]);
      m_weaponIndex[m_weapons[indexToRemove].data.formID] = indexToRemove;
      }
      m_weapons.pop_back();

      logger::info("[WeaponRegistry] Removed weapon: {} ({:08X})"sv, weaponName, formID);
      return true;
   }

   bool WeaponRegistry::RemoveAmmo(RE::FormID formID)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      auto it = m_ammoIndex.find(formID);
      if (it == m_ammoIndex.end()) {
      return false;
      }

      const size_t indexToRemove = it->second;
      std::string ammoName = m_ammo[indexToRemove].data.name;

      m_ammoIndex.erase(it);

      // Swap-remove pattern
      const size_t lastIndex = m_ammo.size() - 1;
      if (indexToRemove != lastIndex) {
      m_ammo[indexToRemove] = std::move(m_ammo[lastIndex]);
      m_ammoIndex[m_ammo[indexToRemove].data.formID] = indexToRemove;
      }
      m_ammo.pop_back();

      logger::info("[WeaponRegistry] Removed ammo: {} ({:08X})"sv, ammoName, formID);
      return true;
   }

   // Thread-safe accessor implementations (v0.7.12)
   size_t WeaponRegistry::GetWeaponCount() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return m_weapons.size();
   }

   size_t WeaponRegistry::GetAmmoCount() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return m_ammo.size();
   }

   std::vector<InventoryWeapon> WeaponRegistry::GetAllWeapons() const
   {
      std::shared_lock lock(m_mutex);
      return m_weapons;  // Returns copy for thread safety
   }

   std::vector<InventoryAmmo> WeaponRegistry::GetAllAmmo() const
   {
      std::shared_lock lock(m_mutex);
      return m_ammo;  // Returns copy for thread safety
   }
}
