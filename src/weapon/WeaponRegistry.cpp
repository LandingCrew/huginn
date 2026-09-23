#include "WeaponRegistry.h"
#include "Config.h"
#include "Globals.h"
#include "../Profiling.h"
#include "registry/FormRegistry.h"  // Registry::CollectLocked/QueryTopKLocked/FindBestLocked (finding #8)
#include "util/ScopedTimer.h"
#include "util/AtomicGuard.h"
#include "util/AlgorithmUtils.h"
#include "util/InventoryUtil.h"
#include "util/ExtraListStability.h"

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
      // Note: ScanPlayerWeapons() reads extraLists only when they are safe to
      // read. On the load path they are not, so favorites/charge/instances are
      // recovered by the primed short-retry reconcile (Main.cpp step 9) just
      // after the stabilization window passes. A rebuild at any other time
      // (`hg rebuild`) gets the full per-stack picture immediately.
      auto scannedWeapons = ScanPlayerWeapons();
      size_t favCount = std::count_if(scannedWeapons.begin(), scannedWeapons.end(),
      [](const auto& w) { return w.isFavorited; });
      logger::info("Found {} weapons in player inventory ({} favorited at scan time)"sv,
      scannedWeapons.size(), favCount);

      // Note: favCount will be 0 during initial load - favorites are recovered by
      // the primed short-retry reconcile after the stabilization window passes

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
      m_rejectedWeapons.clear();  // rebuild retries rejected weapons (mod update may have named them)
      m_ammo.clear();
      m_ammoIndex.clear();

      // Reserve space
      m_weapons.reserve(std::min(scannedWeapons.size(), Config::MAX_TRACKED_WEAPONS));
      m_weaponIndex.reserve(m_weapons.capacity());
      m_ammo.reserve(std::min(scannedAmmo.size(), Config::MAX_TRACKED_AMMO));
      m_ammoIndex.reserve(m_ammo.capacity());

      // Add all inventory weapons (AddWeapon assumes lock is held by caller).
      //
      // Same key-collision guard as ReconcileWeapons: two stacks of one form
      // with no ExtraUniqueID between them share a key, and without this the
      // two paths disagreed about which one survives -- reconcile kept the
      // first, rebuild kept whichever the scan reached last. Raised in review
      // of #122.
      std::unordered_set<uint64_t> seenKeys;
      seenKeys.reserve(scannedWeapons.size());
      size_t keyCollisions = 0;

      for (const auto& sw : scannedWeapons) {
      if (m_weapons.size() >= Config::MAX_TRACKED_WEAPONS) {
        logger::warn("Weapon registry at max capacity ({})"sv, Config::MAX_TRACKED_WEAPONS);
        break;
      }

      if (sw.weapon &&
          !seenKeys.insert(MakeWeaponKey(sw.weapon->GetFormID(), sw.uniqueID)).second) {
        ++keyCollisions;
        continue;
      }

      AddWeapon(sw);
      }

      if (keyCollisions > 0) {
      logger::debug("[WeaponRegistry] {} scanned stacks shared a registry key "
                     "(no ExtraUniqueID to separate them); kept the first of each"sv,
        keyCollisions);
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
      if (!Util::IsExtraListStable()) {
      logger::trace("[WeaponRegistry] RefreshCharges() skipped - extraLists not stable"sv);
      return;
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return;

      // OPTIMIZATION (v0.7.19): Query equipped weapons once and delegate
      RefreshCharges(EquippedWeapons::Query(player), nullptr);
   }

   void WeaponRegistry::RefreshCharges(const EquippedWeapons& equipped,
                                       std::vector<DepartedStack>* departed)
   {
      Huginn_ZONE_NAMED("WeaponRegistry::RefreshCharges");
      SCOPED_TIMER("WeaponRegistry::RefreshCharges");

      // Stabilization guard: this overload is called directly from the update loop,
      // so it must gate on extraList stability itself rather than relying on the
      // zero-arg wrapper or on the refresh interval coincidentally equalling
      // EXTRALIST_STABILIZATION_MS. Reading extraLists too early can crash.
      if (!Util::IsExtraListStable()) {
      return;
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return;
      }

      // O1: Enchantment charge only drains on EQUIPPED weapons (at most two), so
      // read charge straight off the equipped items' entry data instead of walking
      // the entire player inventory every 500ms. GetEquippedEntryData returns the
      // equipped item's InventoryEntryData (with its ExtraCharge) directly from the
      // actor — no traversal, no per-call inventory-map allocation. Favorite
      // *discovery* (finding newly-starred weapons) is owned by ReconcileWeapons
      // (30s); ammo *counts* are refreshed below from the inventory-changes list.
      // This method was the #1 Huginn CPU cost (~1.2ms/call of full-inventory walk
      // at 2Hz); it is now O(equipped) for charge plus one guarded entryList pass.
      // Keyed by InventoryWeapon::Key(), not by FormID. Two instances of one
      // base form are two records now, and only the key says which of them is
      // the one in hand -- a formID compare would mark both equipped and drain
      // the charge of whichever the loop reached first.
      std::unordered_map<uint64_t, ScannedWeapon> equippedCharge;
      uint64_t equippedKeys[2] = { 0, 0 };
      for (const bool leftHand : { false, true }) {
      RE::TESObjectWEAP* weapon = leftHand ? equipped.leftHand : equipped.rightHand;
      if (!weapon) {
        continue;
      }

      // GetEquippedEntryData returns the hand's own entry, so its extraLists
      // are the worn stack's. Prefer the list that says ExtraWorn outright and
      // fall back to the first one -- a hand entry normally carries exactly one.
      RE::InventoryEntryData* entry = player->GetEquippedEntryData(leftHand);
      RE::ExtraDataList* wornList = nullptr;
      if (entry && entry->extraLists) {
        for (auto* extraList : *entry->extraLists) {
           if (!extraList) continue;
           if (!wornList) {
            wornList = extraList;
           }
           if (extraList->HasType<RE::ExtraWorn>() || extraList->HasType<RE::ExtraWornLeft>()) {
            wornList = extraList;
            break;
           }
        }
      }

      // resolveName = false: this path wants a charge and a uniqueID, and it
      // wants them twice a second.
      ScannedWeapon sw = ExtractWeaponMetadata(weapon, wornList, equipped, false);
      sw.isEquipped = true;  // it is in hand; nothing read below can argue
      const uint64_t key = MakeWeaponKey(weapon->GetFormID(), sw.uniqueID);
      equippedKeys[leftHand ? 1 : 0] = key;
      equippedCharge[key] = std::move(sw);
      }

      // Ammo counts still need per-tick freshness: the low-ammo override's
      // FindBestAmmo gates on the cached count via GetBestArrow/GetBestBolt, so a
      // stale count would surface an ammo type the player has actually run out of —
      // violating "recommend only what the player has". Collect current counts from
      // the inventory-changes entry list (countDelta) — the same guarded entryList
      // walk StateManager uses for arrow/bolt counts, and the pattern used
      // everywhere else in the codebase.
      //
      // Do NOT call RE::InventoryChanges::GetItemCount here: it is the codebase's
      // only use of that raw game function and it crashes on save-load (confirmed
      // by bisecting PR #41 — sub-commit 23555b2 added exactly this call and it
      // reproduced the access violation; 81eecc6 without it was stable). Ammo is a
      // small tracked set, so this is nothing like the full walk O1 removed.
      auto* equippedAmmo = player->GetCurrentAmmo();
      const RE::FormID equippedAmmoID = equippedAmmo ? equippedAmmo->GetFormID() : 0;

      std::unordered_map<RE::FormID, int32_t> ammoCounts;
      // Whether the list was actually readable, which is NOT the same as "every
      // count came back 0". The counts below treat an absent type as depleted
      // and that is right for filtering, but reporting a depletion to the caller
      // breaks a slot lock, so that only happens on a scan that really ran.
      bool ammoCountsRead = false;
      if (auto* invChanges = player->GetInventoryChanges();
      invChanges && invChanges->entryList) {
      ammoCountsRead = true;
      for (auto* entry : *invChanges->entryList) {
        if (!entry || !entry->object || !entry->object->Is(RE::FormType::Ammo)) {
        continue;
        }
        // Accumulate (+=), not assign: one ammo object can appear as multiple
        // entryList entries (distinct extra-data stacks). GetInventorySafe sums
        // the same way; a bare assign would keep only the last stack's count.
        ammoCounts[entry->object->GetFormID()] += static_cast<int32_t>(entry->countDelta);
      }
      }

      // =========================================================================
      // Fast in-memory updates under unique_lock (no SKSE API calls).
      // =========================================================================
      {
      Huginn_ZONE_NAMED("RefreshCharges::Apply");
      std::unique_lock lock(m_mutex);

      for (auto& invWeapon : m_weapons) {
      // Equipped status: cheap key compare against the <=2 equipped stacks
      // (no inventory walk needed to know which tracked weapons are equipped).
      // This is also the authority on isEquipped at 2 Hz -- the 30 s reconcile
      // reads ExtraWorn per stack, but only this runs often enough to matter.
      const uint64_t key = invWeapon.Key();
      invWeapon.isEquipped =
        (invWeapon.data.formID != 0 && (key == equippedKeys[0] || key == equippedKeys[1]));

      // Charge only needs refreshing for the equipped enchanted weapons.
      auto it = equippedCharge.find(key);
      if (it == equippedCharge.end()) {
        continue;
      }
      const auto& sw = it->second;

      // No uniqueID write-back here: it is half the key, so changing it in
      // place would leave m_weaponIndex pointing at the wrong record. A stack
      // that gains a uniqueID is a new key, and the reconcile adds it.

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

      // Refresh tracked ammo counts from the pre-built map. A type absent from the
      // changes list reads back as 0 (depleted) and is filtered out by
      // GetBestArrow/GetBestBolt (count > 0), so FindBestAmmo can't surface ammo
      // the player no longer has.
      for (auto& invAmmo : m_ammo) {
      auto it = ammoCounts.find(invAmmo.data.formID);
      const int32_t newCount = (it != ammoCounts.end()) ? it->second : 0;

      // A type that just hit zero stops being a candidate on the next pipeline
      // run (the affordability filter drops count <= 0), but the slot lock
      // pinning it does not care, and nothing about running out of arrows moves
      // the GameState hash. Report the transition so the caller can break the
      // lock and force one recompute; the record itself lives on until the 30 s
      // reconcile, which is what restores it if the player picks more up.
      if (departed && ammoCountsRead && invAmmo.count > 0 && newCount <= 0) {
        departed->push_back({ invAmmo.data.formID, 0 });
        logger::debug("[WeaponRegistry] Ammo depleted: {} ({} -> 0)"sv,
           invAmmo.data.name, invAmmo.count);
      }

      invAmmo.count = newCount;
      invAmmo.isEquipped = (equippedAmmoID != 0 && invAmmo.data.formID == equippedAmmoID);
      }
      }  // end RefreshCharges::Apply zone
   }

   size_t WeaponRegistry::ReconcileWeapons()
   {
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) return 0;

      // OPTIMIZATION (v0.7.19): Query equipped weapons once and delegate
      return ReconcileWeapons(EquippedWeapons::Query(player), nullptr);
   }

   size_t WeaponRegistry::ReconcileWeapons(const EquippedWeapons& equipped,
                                          std::vector<DepartedStack>* departed)
   {
      Huginn_ZONE_NAMED("WeaponRegistry::ReconcileWeapons");
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

      // === WEAPONS + AMMO ===
      // Check if safe to access extraLists (v0.7.9)
      const bool safeToAccessExtraLists = Util::IsExtraListStable();

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return 0;
      }

      // Scan weapons AND ammo in a SINGLE inventory traversal, entirely OUTSIDE
      // the write lock. (Ammo previously rescanned via ScanPlayerAmmo() while the
      // unique_lock was held — a 20-40ms SKSE traversal blocking all readers.)
      auto* equippedAmmo = player->GetCurrentAmmo();
      auto inventory = [&] {
      Huginn_ZONE_NAMED("ReconcileWeapons::GetInventory");
      return Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
        return obj.Is(RE::FormType::Weapon) || obj.Is(RE::FormType::Ammo);
      });
      }();

      std::vector<ScannedWeapon> scannedWeapons;
      scannedWeapons.reserve(32);
      std::vector<ScannedAmmo> scannedAmmo;
      scannedAmmo.reserve(16);

      {
      Huginn_ZONE_NAMED("ReconcileWeapons::ExtractMetadata");
      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      if (auto* weapon = obj->As<RE::TESObjectWEAP>()) {
        // One record per stack, not per base form. ScanWeaponEntry degrades to
        // a single record itself when extraLists cannot be read.
        ScanWeaponEntry(weapon, entry.get(), count, safeToAccessExtraLists,
                        equipped, scannedWeapons);
      } else if (auto* ammo = obj->As<RE::TESAmmo>()) {
        ScannedAmmo sa{};
        sa.ammo = ammo;
        sa.count = count;
        sa.isEquipped = (ammo == equippedAmmo);
        scannedAmmo.push_back(sa);
      }
      }
      }  // end ReconcileWeapons::ExtractMetadata zone

      // Build set of current inventory stack keys (outside critical section)
      std::unordered_set<uint64_t> currentWeaponKeys;
      for (const auto& sw : scannedWeapons) {
      if (sw.weapon) {
        currentWeaponKeys.insert(MakeWeaponKey(sw.weapon->GetFormID(), sw.uniqueID));
      }
      }

      // Acquire unique lock for write access (v0.7.12 - thread safety)
      {
      Huginn_ZONE_NAMED("ReconcileWeapons::Apply");
      std::unique_lock lock(m_mutex);

      // Add new weapons or update existing (assumes lock is held).
      //
      // Two stacks of one form with no ExtraUniqueID between them land on the
      // same key, and there is nothing left to separate them with -- Wheeler
      // needs the real uid, so a synthetic one is not an option. Keep the first
      // and count the rest. Letting the second fall through to the update
      // branch would overwrite the record's name, temper and charge from
      // whichever stack the scan reached last, which is the thrash this whole
      // change exists to remove.
      std::unordered_set<uint64_t> seenKeys;
      seenKeys.reserve(scannedWeapons.size());
      size_t keyCollisions = 0;

      for (const auto& sw : scannedWeapons) {
      if (!sw.weapon) continue;

      const uint64_t key = MakeWeaponKey(sw.weapon->GetFormID(), sw.uniqueID);
      if (!seenKeys.insert(key).second) {
        ++keyCollisions;
        continue;
      }

      auto indexIt = m_weaponIndex.find(key);
      if (indexIt == m_weaponIndex.end()) {
        if (m_weapons.size() < Config::MAX_TRACKED_WEAPONS) {
           // Only count actual insertions — AddWeapon no-ops on rejected weapons,
           // and counting those would report phantom changes every reconcile
           if (AddWeapon(sw)) {
            weaponsAdded++;
            logger::trace("[WeaponRegistry] Added weapon: {}"sv, sw.weapon->GetName());
           }
        }
      } else {
        // Update favorited/equipped status for existing weapons
        auto& invWeapon = m_weapons[indexIt->second];

        // Only update favorites when extraLists are stable — reading them too early
        // yields false negatives. ReconcileWeapons is now the sole favorites detector
        // (RefreshCharges no longer walks the inventory), so during the unstable
        // window we keep the existing favorite state until the next reconcile.
        if (safeToAccessExtraLists) {
           if (invWeapon.isFavorited != sw.isFavorited) {
            logger::debug("[WeaponRegistry] Favorite status changed: {} (fav={} -> {})"sv,
              sw.weapon->GetName(), invWeapon.isFavorited, sw.isFavorited);
            favoriteChanges++;
           }
           invWeapon.isFavorited = sw.isFavorited;
        }

        invWeapon.isEquipped = sw.isEquipped;

        // A re-temper changes the stack's damage and the name the player reads
        // without changing its identity, so the record is never removed and
        // re-added. Refresh both here or they stay at whatever they were when
        // the weapon was first picked up.
        if (safeToAccessExtraLists) {
           if (sw.temperFactor != invWeapon.data.temperFactor) {
            logger::debug("[WeaponRegistry] '{}' temper {:.2f} -> {:.2f}, dmg {:.1f} -> {:.1f}"sv,
              invWeapon.data.name, invWeapon.data.temperFactor, sw.temperFactor,
              invWeapon.data.damage, invWeapon.data.baseDamage * sw.temperFactor);
            invWeapon.data.temperFactor = sw.temperFactor;
            invWeapon.data.damage = invWeapon.data.baseDamage * sw.temperFactor;
           }
           if (sw.displayDamage > 0.0f) {
            invWeapon.data.displayDamage = sw.displayDamage;
           }
           if (!sw.displayName.empty() && sw.displayName != invWeapon.data.name) {
            invWeapon.data.name = sw.displayName;
           }
        }

        // Update charge for existing enchanted weapons (mirrors RefreshCharges).
        // Weapons first seen while extraLists were unstable assumed full charge,
        // and RefreshCharges reads only the ≤2 equipped weapons — this is the
        // only path that corrects an unequipped weapon's charge.
        if (safeToAccessExtraLists && invWeapon.data.hasEnchantment && sw.maxCharge > 0.0f) {
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
        }
      }
      }

      if (keyCollisions > 0) {
      logger::debug("[WeaponRegistry] {} scanned stacks shared a registry key "
                     "(no ExtraUniqueID to separate them); kept the first of each"sv,
        keyCollisions);
      }

      // Remove stacks no longer in inventory.
      //
      // Only when extraLists were readable. An unstable scan emits one uid-0
      // record per base form, so every real per-instance record would look like
      // it had left the inventory and the registry would churn itself empty
      // once per load. The add pass above is safe either way: it only inserts.
      if (safeToAccessExtraLists) {
      std::vector<uint64_t> weaponsToRemove;
      for (const auto& invWeapon : m_weapons) {
        if (!currentWeaponKeys.contains(invWeapon.Key())) {
           weaponsToRemove.push_back(invWeapon.Key());
        }
      }
      for (auto key : weaponsToRemove) {
        if (RemoveWeapon(key)) {
           weaponsRemoved++;
           // The key IS the pair the caller needs: low 32 bits the form, high
           // 16 the stack (MakeWeaponKey). Reported per stack so a lock on the
           // player's OTHER copy of this form survives.
           if (departed) {
            departed->push_back({
              static_cast<RE::FormID>(key & 0xFFFFFFFFull),
              static_cast<uint16_t>(key >> 32) });
           }
        }
      }
      }

      // === AMMO === (scannedAmmo was gathered above, outside the lock)
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
      std::vector<RE::FormID> ammoToRemove;
      for (const auto& invAmmo : m_ammo) {
      if (!currentAmmoIDs.contains(invAmmo.data.formID)) {
        ammoToRemove.push_back(invAmmo.data.formID);
      }
      }
      for (auto formID : ammoToRemove) {
      if (RemoveAmmo(formID)) {
        ammoRemoved++;
        // Ammo is keyed by form -- there are no instances to tell apart -- so
        // uniqueID 0, which SlotLocker reads as every lock on the form.
        if (departed) departed->push_back({ formID, 0 });
      }
      }
      }  // end ReconcileWeapons::Apply zone

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

   const InventoryWeapon* WeaponRegistry::GetWeapon(RE::FormID formID, uint16_t uniqueID) const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      auto it = m_weaponIndex.find(MakeWeaponKey(formID, uniqueID));
      if (it == m_weaponIndex.end() || it->second >= m_weapons.size()) {
      return nullptr;
      }
      return &m_weapons[it->second];
   }

   // Accessors below delegate to the shared lock-free query helpers (finding #8);
   // WeaponRegistry keeps its own storage + mutex, so it locks then queries.
   // Weapons are all-tracked (no per-entry count), so predicates omit count>0.

   std::vector<const InventoryWeapon*> WeaponRegistry::GetMeleeWeapons() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::Melee); });
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetRangedWeapons() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::Ranged); });
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetOneHandedWeapons() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::OneHanded); });
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetTwoHandedWeapons() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::TwoHanded); });
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetSilveredWeapons() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::Silver); });
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetEnchantedWeapons() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [](const InventoryWeapon& w) { return w.data.hasEnchantment; });
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetWeaponsNeedingCharge() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::NeedsCharge); });
   }

   std::vector<const InventoryWeapon*> WeaponRegistry::GetWeaponsWithTag(WeaponTag tag) const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_weapons,
      [tag](const InventoryWeapon& w) { return HasTag(w.data.tags, tag); });
   }

   // =============================================================================
   // CONVENIENCE "BEST" ACCESSORS
   // =============================================================================

   const InventoryWeapon* WeaponRegistry::GetBestMeleeWeapon() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return Registry::FindBestLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::Melee); },
      [](const InventoryWeapon& w) { return w.data.damage; });
   }

   const InventoryWeapon* WeaponRegistry::GetBestRangedWeapon() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return Registry::FindBestLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::Ranged); },
      [](const InventoryWeapon& w) { return w.data.damage; });
   }

   const InventoryWeapon* WeaponRegistry::GetBestSilveredWeapon() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return Registry::FindBestLocked(m_weapons,
      [](const InventoryWeapon& w) { return HasTag(w.data.tags, WeaponTag::Silver); },
      [](const InventoryWeapon& w) { return w.data.damage; });
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
      std::shared_lock lock(m_mutex);
      return Registry::QueryTopKLocked(m_ammo,
      [](const InventoryAmmo& a) { return a.data.type == AmmoType::Arrow && a.count > 0; },
      [](const InventoryAmmo& a) { return a.data.baseDamage; }, topK);
   }

   std::vector<const InventoryAmmo*> WeaponRegistry::GetBolts(size_t topK) const
   {
      std::shared_lock lock(m_mutex);
      return Registry::QueryTopKLocked(m_ammo,
      [](const InventoryAmmo& a) { return a.data.type == AmmoType::Bolt && a.count > 0; },
      [](const InventoryAmmo& a) { return a.data.baseDamage; }, topK);
   }

   std::vector<const InventoryAmmo*> WeaponRegistry::GetMagicAmmo() const
   {
      std::shared_lock lock(m_mutex);
      return Registry::CollectLocked(m_ammo,
      [](const InventoryAmmo& a) { return a.data.hasEnchantment && a.count > 0; });
   }

   std::vector<const InventoryAmmo*> WeaponRegistry::GetSilverAmmo(size_t topK) const
   {
      std::shared_lock lock(m_mutex);
      return Registry::QueryTopKLocked(m_ammo,
      [](const InventoryAmmo& a) { return HasTag(a.data.tags, WeaponTag::Silver) && a.count > 0; },
      [](const InventoryAmmo& a) { return a.data.baseDamage; }, topK);
   }

   const InventoryAmmo* WeaponRegistry::GetBestArrow() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return Registry::FindBestLocked(m_ammo,
      [](const InventoryAmmo& a) { return a.data.type == AmmoType::Arrow && a.count > 0; },
      [](const InventoryAmmo& a) { return a.data.baseDamage; });
   }

   const InventoryAmmo* WeaponRegistry::GetBestSilverArrow() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return Registry::FindBestLocked(m_ammo,
      [](const InventoryAmmo& a) {
        return a.data.type == AmmoType::Arrow && HasTag(a.data.tags, WeaponTag::Silver) && a.count > 0;
      },
      [](const InventoryAmmo& a) { return a.data.baseDamage; });
   }

   const InventoryAmmo* WeaponRegistry::GetBestBolt() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return Registry::FindBestLocked(m_ammo,
      [](const InventoryAmmo& a) { return a.data.type == AmmoType::Bolt && a.count > 0; },
      [](const InventoryAmmo& a) { return a.data.baseDamage; });
   }

   // =============================================================================
   // DEBUG
   // =============================================================================

   void WeaponRegistry::LogAllWeapons() const
   {
      std::shared_lock lock(m_mutex);  // v0.7.12 - thread safety
      logger::info("=== Weapon Registry ({} weapons, {} ammo) ==="sv,
      m_weapons.size(), m_ammo.size());

      // Every line here is info, including the per-item rows, which used to be
      // debug. Release pins the logger to info (Main.cpp), so `hg status` --
      // whose whole purpose is reading uid and temper off a live session --
      // printed a header, a footer and nothing between them on exactly the
      // build where it is most wanted. Not a per-tick log: this only runs when
      // someone types the command. Raised in review of #122.
      logger::info("--- Weapons ---"sv);
      for (const auto& weapon : m_weapons) {
      // uid and the temper pair are the whole point of this log now: two lines
      // sharing a FormID with different uids is the registry tracking two
      // instances, which is what could not happen before.
      //
      // `shown` is what the widget prints and should equal the number in the
      // player's inventory: PlayerCharacter::GetDamage, skill and perks
      // included. `rank` is what the scorer compares, which is the form's
      // damage times temper and is missing those terms by design. Printing
      // both beside base and temper is how either model gets checked against
      // the game -- the gap between shown and rank IS the skill/perk term.
      logger::info("  {} ({:08X}/uid{}): shown={:.1f} rank={:.1f} (base {:.1f} x{:.2f}), tags={:08X}, fav={}, eq={}, charge={:.0f}%"sv,
        weapon.data.name,
        weapon.data.formID,
        weapon.data.uniqueID,
        weapon.data.DamageForDisplay(),
        weapon.data.damage,
        weapon.data.baseDamage,
        weapon.data.temperFactor,
        std::to_underlying(weapon.data.tags),
        weapon.isFavorited,
        weapon.isEquipped,
        weapon.data.currentCharge * 100.0f);
      }

      logger::info("--- Ammo ---"sv);
      for (const auto& ammo : m_ammo) {
      logger::info("  {}: x{}, dmg={:.1f}, ench={}"sv,
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
      RE::ExtraDataList* extraList,
      const EquippedWeapons& equipped,
      bool resolveName) const
   {
      ScannedWeapon sw{};
      sw.weapon = weapon;

      // With an extraList, every per-instance question is asked of THAT stack.
      // Without one there is no instance to speak of, so equipped falls back to
      // the base form -- which is all the load-path scan can say anyway.
      sw.isEquipped = extraList
      ? (extraList->HasType<RE::ExtraWorn>() || extraList->HasType<RE::ExtraWornLeft>())
      : equipped.IsEquipped(weapon);

      // OPTIMIZATION (S10 v0.7.19): Cache RTTI cast - was called twice before
      auto* enchantable = weapon->As<RE::TESEnchantableForm>();
      const bool hasEnchantment = enchantable && enchantable->formEnchanting;

      // Detect enchanted staves (no formEnchanting but still use charges)
      const bool isStaff = weapon->GetWeaponType() == RE::WEAPON_TYPE::kStaff;
      const bool isEnchanted = hasEnchantment || isStaff;

      // Get max charge from base form
      if (isEnchanted && enchantable) {
      sw.maxCharge = static_cast<float>(enchantable->amountofEnchantment);
      }

      bool foundExtraCharge = false;
      if (extraList) {
      // Favouriting is per STACK. ExtraHotkey lives on the extraList (with
      // hotkey -1 for a plain star), and InventoryEntryData::IsFavorited()
      // answers for the whole ENTRY -- so starring one dagger reported its
      // untempered twin as favorited too.
      sw.isFavorited = extraList->HasType<RE::ExtraHotkey>();

      // Enchantment charge (only created after the weapon has been used)
      if (auto* extraCharge = extraList->GetByType<RE::ExtraCharge>(); extraCharge) {
        sw.currentCharge = extraCharge->charge;
        foundExtraCharge = true;
      }

      // Unique ID for Wheeler, and half of this registry's key
      if (auto* extraUnique = extraList->GetByType<RE::ExtraUniqueID>(); extraUnique) {
        sw.uniqueID = extraUnique->uniqueID;
      }

      // Tempering. ExtraHealth is the game's own quality multiplier for this
      // stack: 1.0 untempered, higher once it has been to a grindstone.
      if (auto* extraHealth = extraList->GetByType<RE::ExtraHealth>(); extraHealth) {
        sw.temperFactor = extraHealth->health;
      }

      // The name the PLAYER reads. ExtraDataList::GetDisplayName is the game's
      // own accessor: it applies a player-set name and appends the temper
      // quality suffix, so a tempered Iron Mace answers "Iron Mace - Okay"
      // where the base form answers "Iron Mace" -- and the widget said the
      // latter for an item the player owns exactly one of.
      //
      // Gated on the stack ALREADY having ExtraTextDisplayData, which makes
      // this a pure read. Without the gate, CommonLib's GetDisplayName does
      //     if (!xText && !dfHealth) { xText = new ExtraTextDisplayData(); Add(xText); }
      // -- a game-data write, into the player's save, from whichever thread the
      // update loop is (an InputEvent sink; UpdateHandler::RunExclusive exists
      // because the console is a DIFFERENT thread, so "it is the main thread"
      // is not something this codebase can currently assert). Not worth the
      // hazard for a string.
      //
      // The cost of the gate is close to zero in practice: the engine attaches
      // that data the first time the item is drawn in any menu, and the
      // grindstone that tempered it is such a menu. If it is ever genuinely
      // absent we show the base-form name for that session, which is the old
      // behaviour, rather than writing to a save from an unproven thread.
      // Raised in review of #122.
      if (resolveName && extraList->HasType<RE::ExtraTextDisplayData>()) {
        const char* shown = extraList->GetDisplayName(weapon);
        const char* base = weapon->GetName();
        if (shown && *shown && (!base || std::string_view{ shown } != base)) {
           sw.displayName = shown;
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

   void WeaponRegistry::ScanWeaponEntry(
      RE::TESObjectWEAP* weapon,
      RE::InventoryEntryData* entry,
      int32_t count,
      bool includeExtraLists,
      const EquippedWeapons& equipped,
      std::vector<ScannedWeapon>& out) const
   {
      if (!weapon) {
      return;
      }

      if (!includeExtraLists || !entry || !entry->extraLists) {
      // Degraded: one record for the form, no instance detail at all. During
      // the stabilization window after a load this is every weapon; the primed
      // short-retry reconcile replaces these with real per-stack records a
      // second later.
      out.push_back(ExtractWeaponMetadata(weapon, nullptr, equipped));
      return;
      }

      // The number the player will actually read, asked of the ACTOR rather
      // than the form, because the skill and perk terms live on the actor and
      // no amount of reading the form will produce them.
      //
      // PlayerCharacter::GetDamage is the game's own accessor -- the inventory
      // card calls it, and it sits beside GetArmorValue which does the same job
      // for apparel. It is also a raw native in a codebase that has been bitten
      // by one before (InventoryChanges::GetItemCount, PR #41, crashed on
      // save-load), so it is called only here: inside the includeExtraLists
      // gate, which already means Util::IsExtraListStable() said the inventory
      // is safe to read, and never on the load path.
      //
      // Per ENTRY, so every stack of one base form gets the same answer; see
      // WeaponData::displayDamage. A non-positive result means "no answer" and
      // falls back to the computed number rather than showing a zero.
      float displayDamage = 0.0f;
      if (auto* player = RE::PlayerCharacter::GetSingleton()) {
      const float asked = player->GetDamage(entry);
      if (asked > 0.0f) {
        displayDamage = asked;
      }
      }

      // Util::GetInventorySafe returns ONE entry per TESBoundObject, so this is
      // where a base form fans back out into the instances the player owns.
      int32_t plainCopies = count;
      for (auto* extraList : *entry->extraLists) {
      if (!extraList) continue;
      plainCopies -= extraList->GetCount();
      auto sw = ExtractWeaponMetadata(weapon, extraList, equipped);
      sw.displayDamage = displayDamage;
      out.push_back(std::move(sw));
      }

      if (plainCopies > 0) {
      // The copies carrying no extra data of their own: untempered,
      // unenchanted, never equipped, never favorited. They share one record
      // because nothing distinguishes them -- including, for Wheeler, a
      // uniqueID, which is why the push filters them out (#118).
      ScannedWeapon sw = ExtractWeaponMetadata(weapon, nullptr, equipped);
      sw.displayDamage = displayDamage;
      // ...and they cannot be the equipped one: ExtraWorn lives on an
      // extraList, so an equipped copy always has one and was emitted above.
      sw.isEquipped = false;
      out.push_back(std::move(sw));
      }
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

      // Ask, rather than assume no. On the load path extraLists are unreadable
      // and this degrades to one record per base form -- no uniqueID, no
      // temper, full charge assumed -- which the primed short-retry reconcile
      // then replaces with per-stack records.
      //
      // But RebuildRegistry also runs from `hg rebuild` and from a later load,
      // when the window has long passed. Hardcoding false there threw away
      // instance data that was sitting right there and left the registry
      // degraded until the next 30 s reconcile, which then had to remove every
      // uid-0 record and re-add the real ones. Observed 2026-09-19: a rebuild
      // at 19:46:12 produced 5 uid-0 entries, and the 19:46:31 reconcile
      // reported +6/4 putting the same 7 stacks back.
      const bool stable = Util::IsExtraListStable();

      for (auto& [obj, data] : inventory) {
      auto& [count, entry] = data;
      if (count <= 0) continue;

      auto* weapon = obj->As<RE::TESObjectWEAP>();
      if (!weapon) continue;

      ScanWeaponEntry(weapon, entry.get(), count, stable, equipped, weapons);
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
      if (!Util::IsExtraListStable()) {
      logger::trace("[WeaponRegistry] ScanWeaponFavorites() skipped - extraLists not stable"sv);
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

   bool WeaponRegistry::AddWeapon(const ScannedWeapon& sw)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      if (!sw.weapon) return false;

      const RE::FormID formID = sw.weapon->GetFormID();
      const uint64_t key = MakeWeaponKey(formID, sw.uniqueID);

      // M2 (v0.7.21): Single lookup instead of contains() + subscript
      auto it = m_weaponIndex.find(key);
      if (it != m_weaponIndex.end()) {
      logger::debug("[WeaponRegistry] Stack {:08X}/uid{} already registered, updating status"sv,
        formID, sw.uniqueID);
      // OR, never assign. Reaching here means two scanned stacks collided on
      // one key, which only happens when neither has an ExtraUniqueID to tell
      // them apart -- and then a plain remainder arriving second would clear
      // the favorited/equipped flags the starred stack set, dropping the
      // weapon out of the favorites-gated candidate pool until the next
      // reconcile. Merging upward keeps the truer answer: if EITHER stack of
      // this form is starred or in hand, the record says so. Raised in review
      // of #122.
      m_weapons[it->second].isFavorited |= sw.isFavorited;
      m_weapons[it->second].isEquipped |= sw.isEquipped;
      return true;
      }

      // Known-rejected weapon — don't re-classify or re-log every scan cycle.
      // Tombstoned by BASE form, because classification only ever sees the base
      // form: if one instance is unnameable, so is every other.
      if (m_rejectedWeapons.contains(formID)) {
      return false;
      }

      // Classify weapon directly (no caching - classification is cheap ~0.01ms)
      // NOTE: Weapons not cached as of v0.7.11 - see CLAUDE.md design rationale
      WeaponData data = m_classifier.ClassifyWeapon(sw.weapon);

      // Classification rejected (formID stays 0 — nameless modded weapons). Storing it
      // would desync data.formID from the m_weaponIndex key and corrupt RemoveWeapon's
      // swap-pop re-keying. Tombstone it so this logs once, not every scan cycle.
      if (data.formID == 0) {
      m_rejectedWeapons.insert(formID);
      logger::warn("[WeaponRegistry] Failed to classify weapon {:08X}, skipping (won't retry)"sv, formID);
      return false;
      }

      data.uniqueID = sw.uniqueID;

      // Everything the classifier could not know, because it was handed a base
      // form: what this particular stack was tempered to, and what the player
      // sees it called.
      data.temperFactor = sw.temperFactor;
      data.displayDamage = sw.displayDamage;
      data.damage = data.baseDamage * sw.temperFactor;
      if (!sw.displayName.empty()) {
      data.name = sw.displayName;
      }

      // Update charge info
      if (data.hasEnchantment && sw.maxCharge > 0.0f) {
      data.currentCharge = sw.currentCharge / sw.maxCharge;
      data.maxCharge = sw.maxCharge;

      if (data.currentCharge < Config::WEAPON_CHARGE_LOW_THRESHOLD) {
        data.tags |= WeaponTag::NeedsCharge;
      }
      }

      // Create inventory wrapper
      InventoryWeapon invWeapon{
      .data = std::move(data),
      .isFavorited = sw.isFavorited,
      .isEquipped = sw.isEquipped,
      .previousCharge = (sw.maxCharge > 0.0f) ? sw.currentCharge / sw.maxCharge : 1.0f
      };

      // Add to dual-index storage
      size_t index = m_weapons.size();
      m_weapons.push_back(std::move(invWeapon));
      m_weaponIndex[key] = index;

      logger::trace("[WeaponRegistry] Added weapon: {} (uid={}, temper={:.2f}, fav={}, eq={})"sv,
      m_weapons[index].data.name, sw.uniqueID, sw.temperFactor, sw.isFavorited, sw.isEquipped);
      return true;
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

   bool WeaponRegistry::RemoveWeapon(uint64_t key)
   {
      // NOTE: Assumes m_mutex is already held by caller (v0.7.12 - thread safety)
      auto it = m_weaponIndex.find(key);
      if (it == m_weaponIndex.end()) {
      return false;
      }

      const size_t indexToRemove = it->second;
      std::string weaponName = m_weapons[indexToRemove].data.name;
      const RE::FormID formID = m_weapons[indexToRemove].data.formID;
      const uint16_t uniqueID = m_weapons[indexToRemove].data.uniqueID;

      m_weaponIndex.erase(it);

      // Swap-remove pattern
      const size_t lastIndex = m_weapons.size() - 1;
      if (indexToRemove != lastIndex) {
      m_weapons[indexToRemove] = std::move(m_weapons[lastIndex]);
      m_weaponIndex[m_weapons[indexToRemove].Key()] = indexToRemove;
      }
      m_weapons.pop_back();

      logger::info("[WeaponRegistry] Removed weapon: {} ({:08X}/uid{})"sv,
      weaponName, formID, uniqueID);
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
