#pragma once

#include "EquipSourceTracker.h"
#include "ExternalEquipLearner.h"

namespace Huginn::Learning
{
    // =========================================================================
    // EXTERNAL EQUIP LISTENER
    // =========================================================================
    // General-purpose TESEquipEvent sink that detects equips NOT triggered by
    // Huginn (EquipManager, WheelerClient). Covers all form types: spells,
    // weapons, potions, ammo, armor, etc.
    //
    // Phase 3a: Logging only. Phase 3b will wire in ExternalEquipLearner.
    // =========================================================================

    class ExternalEquipListener : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        static ExternalEquipListener& GetSingleton()
        {
            static ExternalEquipListener instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESEquipEvent* event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            if (!event || !event->equipped) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Only care about player equips
            if (event->actor.get() != RE::PlayerCharacter::GetSingleton()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Skip if this was an Huginn-mediated equip
            if (EquipSourceTracker::GetSingleton().IsRecentHuginnEquip()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Look up the form to identify what was equipped
            auto* form = RE::TESForm::LookupByID(event->baseObject);
            if (!form) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Classify the form type for logging
            const char* formType = "Unknown";
            if (form->As<RE::SpellItem>()) {
                // SpellRegistry already handles spell equips (calls ExternalEquipLearner directly)
                // Skip here to avoid duplicate learning and logs
                return RE::BSEventNotifyControl::kContinue;
            } else if (form->As<RE::TESObjectWEAP>()) {
                formType = "Weapon";
            } else if (form->As<RE::AlchemyItem>()) {
                formType = "Potion";
            } else if (form->As<RE::TESAmmo>()) {
                formType = "Ammo";
            } else if (form->As<RE::TESObjectLIGH>()) {
                formType = "Torch";
            } else if (form->As<RE::TESObjectARMO>()) {
                // Armor equips aren't relevant for Huginn learning — skip
                return RE::BSEventNotifyControl::kContinue;
            } else {
                // Other form types (scrolls, misc items) — skip for now
                return RE::BSEventNotifyControl::kContinue;
            }

            logger::debug("{} equip event detected: {} ({:08X})"sv,
                formType, form->GetName(), form->GetFormID());
            ExternalEquipLearner::GetSingleton().OnExternalEquip(form->GetFormID(), formType);

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ExternalEquipListener() = default;
        ~ExternalEquipListener() override = default;
        ExternalEquipListener(const ExternalEquipListener&) = delete;
        ExternalEquipListener& operator=(const ExternalEquipListener&) = delete;
    };

}  // namespace Huginn::Learning
