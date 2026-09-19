#pragma once

#include "EquipSourceTracker.h"
#include "ExternalEquipLearner.h"

namespace Huginn::Learning
{
    // Scroll learning depends on this inheritance: scrolls are routed to
    // SpellRegistry::ProcessEvent via As<SpellItem>() (see below and
    // SpellRegistry.cpp). If this ever breaks, scrolls need their own
    // explicit branch BEFORE the SpellItem check.
    static_assert(std::is_base_of_v<RE::SpellItem, RE::ScrollItem>,
        "ScrollItem must inherit from SpellItem — scroll equip learning relies on it");

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

            // Look up the form to identify what was equipped
            auto* form = RE::TESForm::LookupByID(event->baseObject);
            if (!form) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Skip if this was a Huginn-mediated equip of this specific form
            if (EquipSourceTracker::GetSingleton().IsRecentHuginnEquip(form->GetFormID())) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // Classify by form type — single enum switch instead of a chain of
            // up to six As<>() RTTI dispatches (common weapon case ran two).
            const char* formType = "Unknown";
            switch (form->GetFormType()) {
            case RE::FormType::Spell:
            case RE::FormType::Scroll:
                // Spells AND scrolls are handled by SpellRegistry::ProcessEvent
                // (ScrollItem is-a SpellItem, so its As<SpellItem>() check matches
                // both). Skip here to avoid duplicate learning and logs.
                return RE::BSEventNotifyControl::kContinue;
            case RE::FormType::Weapon:
                formType = "Weapon";
                break;
            case RE::FormType::AlchemyItem:
                formType = "Potion";
                break;
            case RE::FormType::Ammo:
                formType = "Ammo";
                break;
            case RE::FormType::Light:
                formType = "Torch";
                break;
            case RE::FormType::Armor:
                // Still skipped after #65 made apparel recommendable, and the
                // reason changed — so has this comment, which used to claim armor
                // was simply irrelevant.
                //
                // Huginn DOES learn from apparel it recommended: EquipSlot fires
                // m_equipCallback on success, so taking a suggestion is rewarded
                // on the internal path. What is dropped here is the EXTERNAL
                // signal — the player opening their inventory and putting gear on
                // themselves. Almost all of that traffic is ordinary dressing
                // (armor, clothes, a helmet) with nothing to do with crafting,
                // and feeding it in would train on noise and move accept% for
                // reasons unrelated to recommendation quality.
                //
                // Turning it on is a one-line change plus a craft-relevance check
                // against ApparelRegistry, but it shifts a soak metric, so it
                // wants its own decision and its own run. See #65.
                return RE::BSEventNotifyControl::kContinue;
            default:
                // Misc forms (books, keys, ingredients, ...) — not Huginn candidates
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
