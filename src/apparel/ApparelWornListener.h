#pragma once

#include "Globals.h"
#include "apparel/ApparelRegistry.h"
#include "slot/SlotAllocator.h"
#include "util/InventoryUtil.h"

namespace Huginn::Apparel
{
    // =========================================================================
    // APPAREL WORN LISTENER - the registry's worn flag from the game's events
    // =========================================================================
    // A worn piece is not offered (there is nothing to put on), so the worn
    // flag decides whether craft gear can surface at all. It was set at once
    // only when Huginn's own key put a piece ON (EquipManager's apparel
    // callback); a piece taken off -- or put on -- any other way waited for the
    // 30 s reconcile. At an alchemy lab on 2026-10-04 the circlet and ring the
    // player had just taken off stayed "worn" until the next reconcile, and
    // reached the page 25 s after the lab context began.
    //
    // Every player armour equip and unequip now reaches MarkEquipped, and a
    // change forces a re-allocation. Nothing here touches learning: the
    // learning listener (ExternalEquipListener) still skips armour on purpose,
    // since ordinary dressing would be noise there.
    // =========================================================================
    class ApparelWornListener final : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        static ApparelWornListener& GetSingleton()
        {
            static ApparelWornListener instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESEquipEvent* event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (!event || !g_apparelRegistry) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (event->actor.get() != RE::PlayerCharacter::GetSingleton()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto* form = RE::TESForm::LookupByID(event->baseObject);
            if (!form || form->GetFormType() != RE::FormType::Armor) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // An unequip also fires when a worn piece LEAVES the inventory --
            // sold, dropped, stored. Marking it "not worn" then would offer a
            // piece the player no longer owns until the 30 s reconcile removes
            // it; leave it as it is and let the reconcile drop it.
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!event->equipped &&
                Util::GetItemCountSafe(player, form->As<RE::TESBoundObject>()) <= 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // No slot sweep: the engine sends its own unequip for whatever this
            // equip displaced, and the sweep's one-piece-per-slot guess can
            // clear a still-worn piece (two-ring mods). MarkEquipped returns
            // false for a piece the registry does not hold -- ordinary armour,
            // most of this traffic.
            if (g_apparelRegistry->MarkEquipped(event->baseObject, event->uniqueID, event->equipped,
                    /*sweepSlot=*/false)) {
                Slot::SlotAllocator::GetSingleton().MarkPageDirty();
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ApparelWornListener() = default;
        ~ApparelWornListener() override = default;
        ApparelWornListener(const ApparelWornListener&) = delete;
        ApparelWornListener& operator=(const ApparelWornListener&) = delete;
    };
}
