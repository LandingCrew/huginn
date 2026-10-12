#include "ReachReporter.h"
#include "PipelineStateCache.h"
#include "core/ReachLog.h"
#include "state/StateManager.h"
#include "telemetry/SoakMetrics.h"
#include "util/ExtraListStability.h"

namespace Huginn::Learning::ReachReporter
{
    namespace
    {
        Core::Reach::GearGate g_gearGate;

        std::string FormName(RE::FormID formID)
        {
            const auto* form = RE::TESForm::LookupByID(formID);
            const char* name = form ? form->GetName() : nullptr;
            return (name && *name) ? name : "?";
        }

        Core::Reach::Kind KindOf(RE::FormID formID)
        {
            using Core::Reach::FormClass;
            const auto* form = RE::TESForm::LookupByID(formID);
            if (!form) return Core::Reach::Kind::Other;
            FormClass cls = FormClass::Other;
            bool food = false;
            bool poison = false;
            switch (form->GetFormType()) {
            case RE::FormType::Spell:   cls = FormClass::Spell; break;
            case RE::FormType::Scroll:  cls = FormClass::Scroll; break;
            case RE::FormType::Weapon:  cls = FormClass::Weapon; break;
            case RE::FormType::Ammo:    cls = FormClass::Ammo; break;
            case RE::FormType::Armor:   cls = FormClass::Armor; break;
            case RE::FormType::Light:   cls = FormClass::Light; break;
            case RE::FormType::SoulGem: cls = FormClass::SoulGem; break;
            case RE::FormType::AlchemyItem:
                cls = FormClass::Alchemy;
                if (const auto* alch = form->As<RE::AlchemyItem>()) {
                    food = alch->IsFood();
                    poison = alch->IsPoison();
                }
                break;
            default: break;
            }
            return Core::Reach::ClassifyKind(cls, food, poison);
        }

        // The base form's enchantment, or a player one on the stack. The worn
        // stack decides when one is marked worn (two Gold Rings, one enchanted);
        // otherwise any enchanted stack of the form does. Extra data is read
        // only once it is stable after a load (Util::IsExtraListStable).
        bool IsEnchantedArmour(RE::FormID formID)
        {
            const auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(formID);
            if (!armor) return false;
            if (armor->formEnchanting) return true;
            if (!Util::IsExtraListStable()) return false;

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* changes = player ? player->GetInventoryChanges() : nullptr;
            if (!changes || !changes->entryList) return false;
            bool anyEnchanted = false;
            bool wornSeen = false;
            bool wornEnchanted = false;
            for (auto* entry : *changes->entryList) {
                if (!entry || !entry->object || entry->object->GetFormID() != formID || !entry->extraLists) continue;
                for (auto* xl : *entry->extraLists) {
                    if (!xl) continue;
                    const auto* xe = xl->GetByType<RE::ExtraEnchantment>();
                    const bool enchanted = xe && xe->enchantment;
                    anyEnchanted |= enchanted;
                    if (xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>()) {
                        wornSeen = true;
                        wornEnchanted |= enchanted;
                    }
                }
            }
            return wornSeen ? wornEnchanted : anyEnchanted;
        }

        bool Loading()
        {
            if (!Util::IsExtraListStable()) return true;
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        }

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void FillKeys(Core::Reach::Line& line, bool valid, size_t page,
                      const std::vector<PipelineStateCache::ShownSlot>& shown)
        {
            line.pageKnown = valid;
            line.page = page;
            line.keys.reserve(shown.size());
            for (const auto& s : shown) {
                line.keys.emplace_back(s.slotIndex, s.name);
            }
        }
    }

    void OnSelectionReach(const EquipEvent& event)
    {
        const std::string name = FormName(event.formID);
        Core::Reach::Line line;
        line.name = name;
        line.formID = event.formID;
        line.kind = KindOf(event.formID);
        line.via = event.via;
        // Press-time state, the in-combat flag GameState (and so the pipeline) uses.
        line.inFight = event.gameState.inCombat == State::CombatStatus::InCombat;
        line.caseLabel = event.attribution;
        if (const auto* row = event.shown.Find(event.formID)) {
            line.scored = true;
            line.ranked = row->rank < PipelineStateCache::kUnrankedTail;
            line.rank = row->rank;
            line.utility = row->utility;
        }
        FillKeys(line, event.shown.valid, event.shown.page, event.shown.shown);

        logger::info("{}"sv, Core::Reach::FormatLine(line));
        Telemetry::SoakMetrics::GetSingleton().RecordReach(line.inFight, line.kind);
    }

    void OnArmourPick(RE::FormID formID, std::string_view via, std::string_view caseLabel)
    {
        const Core::Reach::GearEquip equip{ formID, IsEnchantedArmour(formID), Loading(), via };
        if (!g_gearGate.Counts(equip, NowMs())) {
            return;
        }

        const std::string name = FormName(formID);
        Core::Reach::Line line;
        line.name = name;
        line.formID = formID;
        line.kind = Core::Reach::Kind::Gear;
        line.via = via;
        line.inFight = State::StateManager::GetSingleton().IsInCombat();
        line.caseLabel = caseLabel;
        auto& cache = PipelineStateCache::GetSingleton();
        if (const auto info = cache.GetCandidateInfo(formID); info.wasCandidate) {
            line.scored = true;
            line.ranked = info.rank < PipelineStateCache::kUnrankedTail;
            line.rank = info.rank;
            line.utility = info.utility;
        }
        const auto shown = cache.TakeShown();
        FillKeys(line, shown.valid, shown.page, shown.shown);

        logger::info("{}"sv, Core::Reach::FormatLine(line));
        Telemetry::SoakMetrics::GetSingleton().RecordReach(line.inFight, line.kind);
    }

    void Reset()
    {
        g_gearGate.Reset();
    }
}
