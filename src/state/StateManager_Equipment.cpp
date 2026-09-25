// =============================================================================
// StateManager_Equipment.cpp - Player equipment polling
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: weapons, ammo, charge tracking, shield
// Updates: PlayerActorState equipment fields
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"
#include "../util/InventoryUtil.h"

namespace Huginn::State
{
   bool StateManager::PollPlayerEquipment()
   {
      Huginn_ZONE_NAMED("PollPlayerEquipment");
      // Pattern from EquipmentSensor.cpp
      // Due to complexity, this is a simplified implementation
      // Full implementation would include inventory traversal for charge/ammo

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      std::unique_lock lock(m_playerMutex);
      // Clear equipment state
      m_playerState.hasEnchantedWeapon = false;
      m_playerState.hasBowEquipped = false;
      m_playerState.hasCrossbowEquipped = false;
      m_playerState.hasMeleeEquipped = false;
      m_playerState.hasStaffEquipped = false;
      m_playerState.hasShieldEquipped = false;
      m_playerState.hasOneHandedEquipped = false;
      m_playerState.hasTwoHandedEquipped = false;
      m_playerState.hasSpellEquipped = false;
      m_playerState.hasTorchEquipped = false;
      return false;
      }

      // Get equipped weapons
      auto* rightHand = player->GetEquippedObject(false);
      auto* leftHand = player->GetEquippedObject(true);

      // Build new equipment state
      bool newHasEnchantedWeapon = false;
      bool newHasBowEquipped = false;
      bool newHasCrossbowEquipped = false;
      bool newHasMeleeEquipped = false;
      bool newHasStaffEquipped = false;
      bool newHasShieldEquipped = false;
      bool newHasOneHandedEquipped = false;
      bool newHasTwoHandedEquipped = false;
      bool newHasSpellEquipped = false;
      bool newHasTorchEquipped = false;
      RE::FormID newRightHandWeapon = 0;
      RE::FormID newLeftHandWeapon = 0;
      RE::FormID newRightHandSpell = 0;
      RE::FormID newLeftHandSpell = 0;
      RE::FormID newEquippedShield = 0;

      // Check right hand
      if (rightHand) {
      if (auto* weapon = rightHand->As<RE::TESObjectWEAP>()) {
        newRightHandWeapon = rightHand->GetFormID();

        auto weaponType = weapon->GetWeaponType();

        // Debug: Log weapon details for troubleshooting mod compatibility
        auto* enchForm = weapon->As<RE::TESEnchantableForm>();
        logger::trace("[Equipment] Right hand: {} (type={}, formEnch={}, amountEnch={})"sv,
           weapon->GetName(),
           static_cast<int>(weaponType),
           enchForm && enchForm->formEnchanting ? "yes" : "no",
           enchForm ? enchForm->amountofEnchantment : 0);

        switch (weaponType) {
           case RE::WEAPON_TYPE::kOneHandSword:
           case RE::WEAPON_TYPE::kOneHandDagger:
           case RE::WEAPON_TYPE::kOneHandAxe:
           case RE::WEAPON_TYPE::kOneHandMace:
            newHasOneHandedEquipped = true;
            newHasMeleeEquipped = true;
            break;
           case RE::WEAPON_TYPE::kTwoHandSword:
           case RE::WEAPON_TYPE::kTwoHandAxe:
            newHasTwoHandedEquipped = true;
            newHasMeleeEquipped = true;
            break;
           case RE::WEAPON_TYPE::kBow:
            newHasBowEquipped = true;
            break;
           case RE::WEAPON_TYPE::kCrossbow:
            newHasCrossbowEquipped = true;
            break;
           case RE::WEAPON_TYPE::kStaff:
            newHasStaffEquipped = true;
            break;
           default:
            break;
        }

        // Check for enchantment
        auto* enchantable = weapon->As<RE::TESEnchantableForm>();
        if (enchantable && enchantable->formEnchanting) {
           newHasEnchantedWeapon = true;
        }
        // v0.10.0: Staves are inherently enchanted (use charges when casting)
        // They don't use formEnchanting but are still "enchanted weapons"
        if (weaponType == RE::WEAPON_TYPE::kStaff) {
           newHasEnchantedWeapon = true;
        }
      } else if (rightHand->As<RE::SpellItem>()) {  // Type check only, spell details not needed
        newRightHandSpell = rightHand->GetFormID();
        newHasSpellEquipped = true;
      }
      }

      // Check left hand
      if (leftHand) {
      if (auto* weapon = leftHand->As<RE::TESObjectWEAP>()) {
        newLeftHandWeapon = leftHand->GetFormID();

        auto weaponType = weapon->GetWeaponType();
        switch (weaponType) {
           case RE::WEAPON_TYPE::kOneHandSword:
           case RE::WEAPON_TYPE::kOneHandDagger:
           case RE::WEAPON_TYPE::kOneHandAxe:
           case RE::WEAPON_TYPE::kOneHandMace:
            newHasOneHandedEquipped = true;
            newHasMeleeEquipped = true;
            break;
           default:
            break;
        }
      } else if (auto* armor = leftHand->As<RE::TESObjectARMO>()) {
        if (armor->IsShield()) {
           newHasShieldEquipped = true;
           newEquippedShield = leftHand->GetFormID();
        }
      } else if (leftHand->As<RE::TESObjectLIGH>()) {
        newHasTorchEquipped = true;
      } else if (leftHand->As<RE::SpellItem>()) {  // Type check only, spell details not needed
        newLeftHandSpell = leftHand->GetFormID();
        newHasSpellEquipped = true;
      }
      }

      // Weapon charge via Actor Values (v0.12.x: replaces unreliable ExtraCharge traversal)
      // Skyrim tracks weapon charge as kRightItemCharge / kLeftItemCharge actor values,
      // which is what the game's own HUD reads. ExtraCharge inventory data is lazily
      // created and doesn't reliably reflect current charge for base-enchanted weapons.
      float newWeaponChargePercent = DefaultState::FULL_CHARGE;
      float newWeaponChargeMax = 0.0f;
      if (newHasEnchantedWeapon && rightHand) {
      auto* weapon = rightHand->As<RE::TESObjectWEAP>();
      auto* enchantable = weapon ? weapon->As<RE::TESEnchantableForm>() : nullptr;
      if (enchantable) {
        float maxCharge = static_cast<float>(enchantable->amountofEnchantment);
        if (maxCharge > 0.0f) {
           float currentCharge = player->AsActorValueOwner()->GetActorValue(
            RE::ActorValue::kRightItemCharge);
           newWeaponChargePercent = std::clamp(currentCharge / maxCharge, 0.0f, 1.0f);
           newWeaponChargeMax = maxCharge;
        }
      }
      }

      // Ammo count via inventory traversal
      const RE::TESBoundObject* equippedAmmo = nullptr;
      bool needAmmoCount = false;
      std::int32_t newArrowCount = DefaultState::NO_ARROWS;
      std::int32_t newBoltCount = DefaultState::NO_ARROWS;

      // Get equipped ammo for bows/crossbows
      RE::FormID newAmmoFormID = 0;
      float newAmmoDamage = 0.0f;
      if (newHasBowEquipped || newHasCrossbowEquipped) {
      equippedAmmo = player->GetCurrentAmmo();
      needAmmoCount = (equippedAmmo != nullptr);
      if (equippedAmmo) {
        newAmmoFormID = equippedAmmo->GetFormID();
        if (auto* ammo = equippedAmmo->As<RE::TESAmmo>()) {
           newAmmoDamage = ammo->GetRuntimeData().data.damage;
        }
      }
      }

      // How many of the equipped ammo the player actually has. Base container
      // plus changes delta, not the delta on its own -- the loop that used to sit
      // here read `entry->countDelta` into these two fields directly, and for the
      // arrows a character STARTS with that is how many they have spent. Three
      // consumers read them and all three were wrong for starting ammo:
      // IntuitionMenu gates on `count > 0` and so silently dropped the count
      // beside the bow, OverrideManager clamped the negative to 0 and could
      // announce an empty quiver to a player holding thirteen arrows, and
      // PlayerActorState::IsOutOfArrows -- which is `== 0` -- made
      // ContextRuleEngine weight ammo up for a full one that had no changes entry
      // at all. Same bug the WeaponRegistry fast path had, fixed in #131; that
      // fix could not cover these, because it lives on InventoryAmmo::baseCount
      // and StateManager has no registry.
      if (needAmmoCount) {
      const std::int32_t count = Util::GetItemCountSafe(player, equippedAmmo);
      if (newHasBowEquipped) {
        newArrowCount = count;
      } else if (newHasCrossbowEquipped) {
        newBoltCount = count;
      }
      }

      // Update equipment state with change detection
      bool ammoCountChanged = false;
      std::int32_t prevAmmoCount = 0;
      bool returnChanged = false;
      {
      std::unique_lock lock(m_playerMutex);
      bool changed = false;

      if (m_playerState.hasEnchantedWeapon != newHasEnchantedWeapon) {
        m_playerState.hasEnchantedWeapon = newHasEnchantedWeapon;
        changed = true;
      }
      if (m_playerState.hasBowEquipped != newHasBowEquipped) {
        m_playerState.hasBowEquipped = newHasBowEquipped;
        changed = true;
      }
      if (m_playerState.hasCrossbowEquipped != newHasCrossbowEquipped) {
        m_playerState.hasCrossbowEquipped = newHasCrossbowEquipped;
        changed = true;
      }
      if (m_playerState.hasMeleeEquipped != newHasMeleeEquipped) {
        m_playerState.hasMeleeEquipped = newHasMeleeEquipped;
        changed = true;
      }
      if (m_playerState.hasStaffEquipped != newHasStaffEquipped) {
        m_playerState.hasStaffEquipped = newHasStaffEquipped;
        changed = true;
      }
      if (m_playerState.hasShieldEquipped != newHasShieldEquipped) {
        m_playerState.hasShieldEquipped = newHasShieldEquipped;
        changed = true;
      }
      if (m_playerState.hasOneHandedEquipped != newHasOneHandedEquipped) {
        m_playerState.hasOneHandedEquipped = newHasOneHandedEquipped;
        changed = true;
      }
      if (m_playerState.hasTwoHandedEquipped != newHasTwoHandedEquipped) {
        m_playerState.hasTwoHandedEquipped = newHasTwoHandedEquipped;
        changed = true;
      }
      if (m_playerState.hasSpellEquipped != newHasSpellEquipped) {
        m_playerState.hasSpellEquipped = newHasSpellEquipped;
        changed = true;
      }
      if (m_playerState.hasTorchEquipped != newHasTorchEquipped) {
        m_playerState.hasTorchEquipped = newHasTorchEquipped;
        changed = true;
      }
      if (m_playerState.rightHandWeapon != newRightHandWeapon) {
        m_playerState.rightHandWeapon = newRightHandWeapon;
        changed = true;
      }
      if (m_playerState.leftHandWeapon != newLeftHandWeapon) {
        m_playerState.leftHandWeapon = newLeftHandWeapon;
        changed = true;
      }
      if (m_playerState.rightHandSpell != newRightHandSpell) {
        m_playerState.rightHandSpell = newRightHandSpell;
        changed = true;
      }
      if (m_playerState.leftHandSpell != newLeftHandSpell) {
        m_playerState.leftHandSpell = newLeftHandSpell;
        changed = true;
      }
      if (m_playerState.equippedShield != newEquippedShield) {
        m_playerState.equippedShield = newEquippedShield;
        changed = true;
      }
      // Weapon charge and ammo tracking (Phase 5 fix)
      if (std::abs(m_playerState.weaponChargePercent - newWeaponChargePercent) >= Epsilon::WEAPON_CHARGE) {
        m_playerState.weaponChargePercent = newWeaponChargePercent;
        changed = true;
      }
      if (std::abs(m_playerState.weaponChargeMax - newWeaponChargeMax) >= Epsilon::WEAPON_CHARGE) {
        m_playerState.weaponChargeMax = newWeaponChargeMax;
        changed = true;
      }
      if (m_playerState.arrowCount != newArrowCount) {
        prevAmmoCount = m_playerState.arrowCount;
        ammoCountChanged = true;
        m_playerState.arrowCount = newArrowCount;
        changed = true;
      }
      if (m_playerState.boltCount != newBoltCount) {
        prevAmmoCount = m_playerState.boltCount;
        ammoCountChanged = true;
        m_playerState.boltCount = newBoltCount;
        changed = true;
      }
      if (m_playerState.equippedAmmoFormID != newAmmoFormID) {
        m_playerState.equippedAmmoFormID = newAmmoFormID;
        changed = true;
      }
      if (std::abs(m_playerState.equippedAmmoDamage - newAmmoDamage) >= 0.1f) {
        m_playerState.equippedAmmoDamage = newAmmoDamage;
        changed = true;
      }

      if (changed) {
#ifdef _DEBUG
        logger::trace("[StateManager] PlayerEquipment changed"sv);
#endif
      }
      returnChanged = changed;
      }

      // Transition only, and outside the lock. This is the number the widget
      // prints beside the bow and the one LowAmmo fires on, and until v0.21.11
      // it was a delta -- there was no way to see that from the log, because the
      // only line here said "PlayerEquipment changed". One line per shot while
      // shooting, silent otherwise.
      if (ammoCountChanged) {
      const bool bow = newHasBowEquipped;
      logger::debug("[StateManager] {} {} -> {}"sv,
        bow ? "arrows"sv : "bolts"sv,
        prevAmmoCount,
        bow ? newArrowCount : newBoltCount);
      }

      return returnChanged;  // Stage 3b: Return change detection flag
   }

} // namespace Huginn::State
