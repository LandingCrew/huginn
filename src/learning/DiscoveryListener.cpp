#include "DiscoveryListener.h"

#include "Globals.h"
#include "learning/ChoiceLearner.h"
#include "spell/SpellRegistry.h"

#include <algorithm>
#include <atomic>

namespace Huginn::Learning::Discovery
{
    namespace
    {
        bool IsDiscoverableKind(const RE::TESForm* f)
        {
            return f && (f->Is(RE::FormType::Spell) || f->Is(RE::FormType::Scroll) || f->Is(RE::FormType::AlchemyItem) ||
                            f->Is(RE::FormType::Weapon) || f->Is(RE::FormType::Ammo) || f->Is(RE::FormType::Armor) ||
                            f->Is(RE::FormType::SoulGem) || f->Is(RE::FormType::Light));
        }

        class Sink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
        {
        public:
            static Sink& GetSingleton()
            {
                static Sink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
                RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
            {
                // Cheapest first: the event's own fields, then the player, then
                // the form. A form already seen costs one hash lookup under
                // the learner's lock (an arrow shot and picked up again).
                if (!event || event->baseObj == 0 || event->itemCount <= 0 || !g_choiceLearner) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player || event->newContainer != player->GetFormID() || event->oldContainer == event->newContainer) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!IsDiscoverableKind(RE::TESForm::LookupByID(event->baseObj))) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                g_choiceLearner->OnGained(event->baseObj, "inventory");
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Register()
    {
        static std::atomic<bool> s_registered{ false };
        if (s_registered.exchange(true)) return;
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            s_registered.store(false);
            logger::warn("[Discovery] No event source holder: newly gained items get no discovery boost"sv);
            return;
        }
        holder->GetEventSource<RE::TESContainerChangedEvent>()->AddEventSink(&Sink::GetSingleton());
        logger::info("[Discovery] Registered for TESContainerChangedEvent (items gained in play)"sv);
    }

    std::vector<std::uint32_t> PresentForms()
    {
        std::vector<std::uint32_t> out;
        if (g_spellRegistry) {
            g_spellRegistry->ForEachSpell([&](const auto& s) { out.push_back(s.formID); });
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return out;
        // The entry's object only: no extraLists (the header).
        if (auto* changes = player->GetInventoryChanges(); changes && changes->entryList) {
            for (auto* entry : *changes->entryList) {
                if (entry && entry->object && IsDiscoverableKind(entry->object)) {
                    out.push_back(entry->object->GetFormID());
                }
            }
        }
        if (auto* container = player->GetContainer()) {
            container->ForEachContainerObject([&](RE::ContainerObject& e) {
                if (e.obj && IsDiscoverableKind(e.obj)) out.push_back(e.obj->GetFormID());
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    void ForwardLearnedSpells()
    {
        if (!g_spellRegistry || !g_choiceLearner) return;
        for (const RE::FormID id : g_spellRegistry->TakeNewlyLearned()) {
            g_choiceLearner->OnGained(id, "spell");
        }
    }
}
