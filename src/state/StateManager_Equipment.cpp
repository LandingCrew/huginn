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
      std::string newAmmoName;
      float newAmmoDamage = 0.0f;
      if (newHasBowEquipped || newHasCrossbowEquipped) {
      equippedAmmo = player->GetCurrentAmmo();
      needAmmoCount = (equippedAmmo != nullptr);
      if (equippedAmmo) {
        newAmmoName = equippedAmmo->GetName();
        if (auto* ammo = equippedAmmo->As<RE::TESAmmo>()) {
           newAmmoDamage = ammo->GetRuntimeData().data.damage;
        }
      }
      }

      // Inventory traversal for ammo count only
      auto* invChanges = player->GetInventoryChanges();
      if (invChanges && invChanges->entryList && needAmmoCount) {
      for (auto* entry : *invChanges->entryList) {
        if (!entry) continue;

        // Check for ammo count (only if bow/crossbow equipped and ammo found)
        if (needAmmoCount && entry->object == equippedAmmo) {
           if (newHasBowEquipped) {
            newArrowCount = entry->countDelta;
           } else if (newHasCrossbowEquipped) {
            newBoltCount = entry->countDelta;
           }
           needAmmoCount = false;
        }

        // Early exit when ammo found
        if (!needAmmoCount) {
           break;
        }
      }
      }

      // Update equipment state with change detection
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
        m_playerState.arrowCount = newArrowCount;
        changed = true;
      }
      if (m_playerState.boltCount != newBoltCount) {
        m_playerState.boltCount = newBoltCount;
        changed = true;
      }
      if (m_playerState.equippedAmmoName != newAmmoName) {
        m_playerState.equippedAmmoName = std::move(newAmmoName);
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
      return changed;  // Stage 3b: Return change detection flag
      }
   }

} // namespace Huginn::State
