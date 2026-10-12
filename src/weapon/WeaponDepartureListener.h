#pragma once

#include "Globals.h"
#include "display/WheelUidFilter.h"
#include "learning/SelectionLogV3.h"
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
    //
    // The FIRST EQUIP of a plain stack (0.23.40) asks for one too. A weapon
    // that was never equipped, tempered or enchanted has no ExtraUniqueID, so
    // the registry holds it as uniqueID 0, and while Wheeler drives a page it
    // stays off that page (display/WheelUidFilter.h). Equipping it gives it
    // one, but the registry would only read it at the 30 s reconcile -- and
    // meanwhile the Swap Back hold for it, and any lock on it, has nothing to
    // show. Asked only where it matters -- a page hides uid-less gear, so
    // without Wheeler nothing changes -- when the registry holds a uniqueID-0
    // record of the form that the event's stack is not already known by, only
    // while the inventory's extra data may be read (no load under way, past
    // the post-load window: SelectionLogV3::IsInventoryExtraReadable), and
    // once per (form, stack) per load (core::ShouldRequestEquipReconcile).
    // The reconcile that follows treats the plain record giving way to the
    // new uniqueID as an upgrade, not a departure, so the key the player just
    // pressed keeps its lock (core/StackIdentity.h).
    // =========================================================================
    class WeaponDepartureListener final : public RE::BSTEventSink<RE::TESContainerChangedEvent>,
                                          public RE::BSTEventSink<RE::TESEquipEvent>
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

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESEquipEvent* event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (!event || !event->equipped || !g_weaponRegistry) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (event->actor.get() != RE::PlayerCharacter::GetSingleton()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto* form = RE::TESForm::LookupByID(event->baseObject);
            if (!form || form->GetFormType() != RE::FormType::Weapon) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (g_weaponRegistry->RequestReconcileForEquip(event->baseObject, event->uniqueID,
                    Display::AnyPageHidesUidlessGear(), Learning::SelectionLogV3::IsInventoryExtraReadable())) {
                logger::debug("[WeaponRegistry] {:08X} equipped (uid{}) while held as uid 0: reconcile requested"sv,
                    event->baseObject, event->uniqueID);
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
