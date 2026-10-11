#pragma once

#include "Globals.h"
#include "weapon/WeaponRegistry.h"

namespace Huginn::Weapon
{
    // =========================================================================
    // WEAPON DEPARTURE LISTENER - a weapon leaving the player reconciles now
    // =========================================================================
    // The weapon registry removes a weapon that left the inventory only on its
    // 30 s reconcile, so a dropped, disarmed, sold or stored weapon stayed on
    // its key until then, and a press on it was refused: the Staff of
    // Wandering Stars, gone at some point before 20:58:15 and refused by
    // EquipManager then; the same staff seated again at 21:00:05 and removed
    // by the reconcile at 21:00:15 (2026-10-10, trace-05). The item and scroll
    // registries see a departure within 500 ms through their delta scan;
    // weapons have no such scan (it would mean per-stack metadata twice a
    // second), so this asks for the reconcile instead, as ApparelWornListener
    // does for armour. TESContainerChangedEvent fires after the removal, so
    // the reconcile on the next tick no longer finds the weapon.
    //
    // Weapons only. Ammo is counted at 2 Hz by RefreshCharges, which already
    // reports a type that runs out, and firing arrows would otherwise request
    // a full reconcile per shot.
    // =========================================================================
    class WeaponDepartureListener final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static WeaponDepartureListener& GetSingleton()
        {
            static WeaponDepartureListener instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent* event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!event || !g_weaponRegistry || event->baseObj == 0 || event->itemCount <= 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || event->oldContainer != player->GetFormID()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // One request per tick however many go (a merchant sale). The
            // reconcile decides whether a TRACKED stack left; a weapon it never
            // held costs one reconcile and changes nothing.
            auto* form = RE::TESForm::LookupByID(event->baseObj);
            if (form && form->GetFormType() == RE::FormType::Weapon) {
                g_weaponRegistry->RequestReconcile();
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        WeaponDepartureListener() = default;
        ~WeaponDepartureListener() override = default;
        WeaponDepartureListener(const WeaponDepartureListener&) = delete;
        WeaponDepartureListener& operator=(const WeaponDepartureListener&) = delete;
    };
}
