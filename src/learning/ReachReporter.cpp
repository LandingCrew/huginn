#include "ReachReporter.h"
#include "PipelineStateCache.h"
#include "SelectionLogV3.h"
#include "core/ReachLog.h"
#include "state/StateManager.h"
#include "telemetry/SoakMetrics.h"
#include "util/InventoryUtil.h"

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

        // Is the armour just put on enchanted? The facts for Core::Reach::
        // ClassifyEnchant: the base form's enchantment, else the player ones on
        // its stacks. Call only when SelectionLogV3::IsInventoryExtraReadable()
        // (no load under way, past the post-load window): this walks the
        // inventory's extra data.
        Core::Reach::Enchant EnchantmentOf(RE::FormID formID)
        {
            auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(formID);
            if (!armor) return Core::Reach::Enchant::No;
            Core::Reach::StackFacts f;
            f.baseEnchanted = armor->formEnchanting != nullptr;
            if (f.baseEnchanted) return Core::Reach::ClassifyEnchant(f);

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* changes = player ? player->GetInventoryChanges() : nullptr;
            if (!changes || !changes->entryList) return Core::Reach::Enchant::No;
            int32_t inStacks = 0;
            for (auto* entry : *changes->entryList) {
                if (!entry || !entry->object || entry->object->GetFormID() != formID || !entry->extraLists) continue;
                for (auto* xl : *entry->extraLists) {
                    if (!xl) continue;
                    const auto* xe = xl->GetByType<RE::ExtraEnchantment>();
                    const bool enchanted = xe && xe->enchantment;
                    const auto* xc = xl->GetByType<RE::ExtraCount>();
                    inStacks += xc ? xc->count : 1;
                    if (enchanted) {
                        ++f.enchantedStacks;
                    } else {
                        ++f.plainStacks;
                    }
                    if (xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>()) {
                        f.wornKnown = true;
                        f.wornEnchanted |= enchanted;
                    }
                }
            }
            // Copies with no extra data at all are plain ones.
            f.looseCopies = Util::GetItemCountSafe(player, armor) - inStacks;
            return Core::Reach::ClassifyEnchant(f);
        }

        // A load under way, the post-load window, or the loading screen up.
        bool Loading()
        {
            if (!SelectionLogV3::IsInventoryExtraReadable()) return true;
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
        // The cheap checks first: the inventory walk runs only for a menu equip
        // outside a load (a quickload just after the inventory closed reads
        // "menu (just closed)" while the engine re-dresses the player).
        if (formID == 0 || !Core::Reach::IsMenuVia(via) || Loading()) {
            return;
        }
        const auto enchant = EnchantmentOf(formID);
        if (enchant == Core::Reach::Enchant::Ambiguous) {
            logger::debug("[Reach] gear {:08X} '{}' not counted: plain and enchanted copies, none marked worn"sv,
                formID, FormName(formID));
            return;
        }
        const Core::Reach::GearEquip equip{ formID, enchant == Core::Reach::Enchant::Yes, false, via };
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
