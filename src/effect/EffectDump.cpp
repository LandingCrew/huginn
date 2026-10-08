#include "EffectDump.h"

#include "EffectCatalog.h"
#include "EffectReader.h"
#include "ScriptNames.h"

#include "apparel/ApparelClassifier.h"
#include "candidate/CandidateTypes.h"
#include "learning/ScoredCandidate.h"
#include "learning/item/ItemClassifier.h"
#include "scroll/ScrollClassifier.h"
#include "slot/SlotClassifier.h"
#include "spell/SpellClassifier.h"
#include "util/FormRead.h"
#include "util/InventoryUtil.h"
#include "weapon/WeaponClassifier.h"

#include <fstream>

namespace Huginn::Effect
{
    namespace
    {
        using namespace Core::Effect;
        using Util::CsvQuote;

        // ---- gap 8: conditions ------------------------------------------------
        bool IsFormParam(RE::SCRIPT_PARAM_TYPE t)
        {
            using T = RE::SCRIPT_PARAM_TYPE;
            switch (t) {
                case T::kInventoryObject: case T::kObjectRef: case T::kActor: case T::kSpellItem: case T::kCell:
                case T::kMagicItem: case T::kSound: case T::kTopic: case T::kQuest: case T::kRace: case T::kClass:
                case T::kFaction: case T::kGlobal: case T::kFurnitureOrFormList: case T::kObject: case T::kMapMarker:
                case T::kActorBase: case T::kContainerRef: case T::kWorldOrList: case T::kPackage:
                case T::kCombatStyle: case T::kMagicEffect: case T::kWeather: case T::kNPC: case T::kOwner:
                case T::kShaderEffect: case T::kFormList: case T::kPerk: case T::kNote: case T::kImagespaceMod:
                case T::kImagespace: case T::kVoiceType: case T::kEncounterZone: case T::kIdleForm: case T::kMessage:
                case T::kInvObjectOrFormList: case T::kEquipType: case T::kObjectOrFormList: case T::kMusic:
                case T::kKeyword: case T::kRefType: case T::kLocation: case T::kForm: case T::kShout:
                case T::kWordOfPower: case T::kBGSScene: case T::kAssociationType: case T::kReferenceEffect:
                case T::kKnowableForm:
                    return true;
                default:
                    return false;
            }
        }

        std::string FormLabel(const RE::TESForm* f)
        {
            if (!f) return "none";
            const char* ed = f->GetFormEditorID();
            if (ed && *ed) return ed;
            return std::format("{:08X}", f->GetFormID());
        }

        std::string FormatConditions(const RE::TESCondition& cond)
        {
            static constexpr std::string_view kOps[] = { "==", "!=", ">", ">=", "<", "<=" };
            std::string out;
            const auto* commands = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
            for (const auto* item = cond.head; item; item = item->next) {
                const auto& d = item->data;
                const auto fid = static_cast<std::uint32_t>(d.functionData.function.underlying());
                const RE::SCRIPT_FUNCTION* fn = (commands && fid < RE::SCRIPT_FUNCTION::Commands::kScriptCommandsEnd)
                                                    ? commands + fid
                                                    : nullptr;
                std::string params;
                for (std::uint16_t k = 0; fn && k < fn->numParams && k < 2; ++k) {
                    if (!params.empty()) params += ',';
                    const auto type = fn->params ? fn->params[k].paramType.get() : RE::SCRIPT_PARAM_TYPE::kInt;
                    const void* p = d.functionData.params[k];
                    if (IsFormParam(type)) {
                        params += FormLabel(static_cast<const RE::TESForm*>(p));
                    }
                    else if (type == RE::SCRIPT_PARAM_TYPE::kActorValue) {
                        params += Util::AvName(static_cast<RE::ActorValue>(reinterpret_cast<std::uintptr_t>(p)));
                    }
                    else {
                        params += std::to_string(static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(p)));
                    }
                }
                const char* subject = d.object.get() == RE::CONDITIONITEMOBJECT::kTarget ? "T." : "";
                const auto op = static_cast<std::size_t>(d.flags.opCode);
                const std::string value = d.flags.global ? FormLabel(d.comparisonValue.g)
                                                         : std::format("{}", d.comparisonValue.f);
                out += std::format("{}{}({}){}{}", subject, fn && fn->functionName ? fn->functionName : std::to_string(fid).c_str(),
                    params, op < std::size(kOps) ? kOps[op] : "?", value);
                if (item->next) out += d.flags.isOR ? " OR " : " AND ";
            }
            return out;
        }

        // ---- gap 5: the description with its numbers ------------------------
        std::string FillText(std::string_view text, float mag, std::uint32_t dur, std::uint32_t area)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '<') {
                    const auto close = text.find('>', i + 1);
                    if (close != std::string_view::npos) {
                        std::string tag(text.substr(i + 1, close - i - 1));
                        for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (tag == "mag") { out += std::format("{}", mag); i = close; continue; }
                        if (tag == "dur") { out += std::to_string(dur); i = close; continue; }
                        if (tag == "area") { out += std::to_string(area); i = close; continue; }
                    }
                }
                out += text[i];
            }
            return out;
        }

        // ---- today's slot class -------------------------------------------------
        struct Legacy
        {
            Spell::SpellClassifier spells;
            Item::ItemClassifier items;
            Weapon::WeaponClassifier weapons;
            Apparel::ApparelClassifier apparel;
            std::unique_ptr<Scroll::ScrollClassifier> scrolls;

            Legacy()
            {
                // The registries load the same file at kPostLoadGame.
                const auto overrides = std::filesystem::path("Data/SKSE/Plugins/Huginn_Overrides.ini");
                spells.LoadOverrides(overrides);
                items.LoadOverrides(overrides);
                scrolls = std::make_unique<Scroll::ScrollClassifier>(spells);
            }

            std::string_view ClassOf(const RE::TESBoundObject* form)
            {
                using namespace Candidate;
                auto classify = [](CandidateVariant v) {
                    Scoring::ScoredCandidate sc;
                    sc.candidate = std::move(v);
                    return Slot::SlotClassificationToString(Slot::SlotClassifier::Classify(sc));
                };
                auto* f = const_cast<RE::TESBoundObject*>(form);
                switch (form->GetFormType()) {
                    case RE::FormType::Spell:
                        return classify(SpellCandidate::FromSpellData(spells.ClassifySpell(static_cast<RE::SpellItem*>(f))));
                    case RE::FormType::Scroll:
                        return classify(ScrollCandidate::FromScrollData(scrolls->ClassifyScroll(static_cast<RE::ScrollItem*>(f)), 1));
                    case RE::FormType::AlchemyItem:
                        return classify(ItemCandidate::FromItemData(items.ClassifyItem(static_cast<RE::AlchemyItem*>(f)), 1));
                    case RE::FormType::SoulGem:
                        return classify(ItemCandidate::FromItemData(Item::ItemClassifier::ClassifySoulGem(static_cast<RE::TESSoulGem*>(f)), 1));
                    case RE::FormType::Weapon:
                        return classify(WeaponCandidate::FromWeaponData(weapons.ClassifyWeapon(static_cast<RE::TESObjectWEAP*>(f)), false));
                    case RE::FormType::Ammo:
                        return classify(AmmoCandidate::FromAmmoData(weapons.ClassifyAmmo(static_cast<RE::TESAmmo*>(f)), 1));
                    case RE::FormType::Armor: {
                        // Only craft gear is a candidate today (ApparelClassifier's scope guard).
                        auto* armor = static_cast<RE::TESObjectARMO*>(f);
                        return apparel.ClassifyApparel(armor, armor->formEnchanting).IsCraftRelevant()
                                   ? classify(ApparelCandidate{})
                                   : std::string_view{};
                    }
                    case RE::FormType::Light: return classify(TorchCandidate{});
                    default: return {};
                }
            }
        };

        std::string Num(float v) { return std::format("{}", v); }
    }

    bool WriteDumpAll(const std::filesystem::path& path, std::string& summary)
    {
        auto& catalog = EffectCatalog::GetSingleton();
        if (!catalog.WaitUntilReady(std::chrono::seconds(60))) {
            summary = "The effect catalog is not built (yet); try again";
            return false;
        }
        std::ofstream out(path, std::ios::trunc | std::ios::binary);
        if (!out) {
            summary = std::format("Could not open {} for writing", path.filename().string());
            return false;
        }

        const ReadResult read = EffectReader{}.ReadLoadOrder();
        // The catalog's class of every effect (by MGEF FormID).
        std::vector<EffectClass> classes;
        classes.reserve(read.effects.size());
        for (const auto& e : read.effects) {
            const auto* c = catalog.ClassOf(e.formId);
            classes.push_back(c ? *c : ClassifyEffect(e, &catalog.Overrides()));
        }
        // Gap 6: scripts on every script-archetype effect.
        std::vector<const RE::EffectSetting*> scripted;
        for (std::size_t i = 0; i < read.effects.size(); ++i) {
            if (read.effects[i].archetype == kArchScript) scripted.push_back(read.mgefs[i]);
        }
        ScriptNameStats scriptStats;
        const auto scripts = ReadScriptNames(scripted, &scriptStats);

        Legacy legacy;
        auto* player = RE::PlayerCharacter::GetSingleton();

        out << "kind,formID,plugin,winningPlugin,name,playable,value,weight,playerCount,keywords,"
               "spellType,castingType,delivery,magickaCost,taughtByTome,"
               "weaponType,twoHanded,damage,speed,reach,critDamage,"
               "armorSlots,armorRating,armorType,"
               "soulCapacity,soulContained,lightRadius,"
               "enchantment,enchantmentCharge,"
               "effectIndex,effectFormID,effectName,archetype,primaryAV,secondaryAV,resistAV,"
               "effectDelivery,effectCasting,magnitude,duration,area,effectBaseCost,detrimental,hostile,effectFlags,"
               "effectKeywords,effectDescription,"
               "ammoNonBolt,enchantCasting,effectPlugin,effectCost,effectSchool,effectText,effectScripts,"
               "effectConditions,payloadOf,payloadSpell,effectColumn,effectRoute,effectKept,inScope,slotClass,cap\n";

        std::size_t rows = 0, inScope = 0, withClass = 0, undescribed = 0;
        RowTally tally;
        for (std::size_t i = 0; i < read.items.size(); ++i) {
            const auto& it = read.items[i];
            const auto* form = read.forms[i];
            const ItemMapping m = MapItem(it, read.effects, classes);
            const auto* entry = m.inScope ? catalog.Find(it.formId) : nullptr;
            const std::string cap = entry ? FormatCap(entry->cap) : std::string{};
            std::string slotClass;
            if (m.inScope) {
                ++inScope;
                tally += m.tally;
                slotClass = std::string(legacy.ClassOf(form));
                if (!slotClass.empty() && slotClass != "Regular") {
                    ++withClass;
                    if (!entry || !DescribesItem(entry->cap, it.kind)) ++undescribed;
                }
            }

            const auto* enchantable = form->As<RE::TESEnchantableForm>();
            const RE::EnchantmentItem* ench = (it.kind == Kind::Weapon || it.kind == Kind::Armour) && enchantable
                                                  ? enchantable->formEnchanting
                                                  : nullptr;
            const bool magic = it.kind == Kind::Spell || it.kind == Kind::Scroll;
            const bool isSpell = it.kind == Kind::Spell;
            const std::string head = std::format(
                "{},{:08X},{},{},{},{},{},{},{},{},",
                KindName(it.kind), it.formId, CsvQuote(it.plugin), CsvQuote(Util::WinningPluginOf(form)),
                CsvQuote(it.name), it.playable ? 1 : 0, isSpell ? std::string{} : std::to_string(it.value),
                it.weight < 0.0f ? std::string{} : Num(it.weight),
                player ? Util::GetItemCountSafe(player, form) : 0, CsvQuote(Util::KeywordList(form->As<RE::BGSKeywordForm>())));
            const std::string magicCols = magic ? std::format("{},{},{},{},{},", it.spellType, it.castingType, it.delivery,
                                                      isSpell ? Num(it.magickaCost) : std::string{},
                                                      isSpell ? (it.taughtByTome.value_or(false) ? "1" : "0") : "")
                                                : std::string(",,,,,");
            std::string weaponCols = ",,,,,,";
            if (it.kind == Kind::Weapon) {
                weaponCols = std::format("{},{},{},{},{},{},", it.weaponType, it.twoHanded ? 1 : 0, Num(it.damage),
                    Num(it.speed), Num(it.reach), Num(it.critDamage));
            }
            else if (it.kind == Kind::Ammo) {
                weaponCols = std::format(",,{},,,,", Num(it.damage));
            }
            const std::string armorCols =
                it.kind == Kind::Armour
                    ? std::format("{:08X},{},{},", it.slotMask, Num(it.armorRating),
                          it.armourWeight == ArmourWeight::Heavy ? "Heavy" : it.armourWeight == ArmourWeight::Light ? "Light" : "None")
                    : std::string(",,,");
            std::string miscCols = ",,,";
            if (it.kind == Kind::SoulGem) miscCols = std::format("{},{},,", it.soulCapacity, it.soulContained);
            if (it.kind == Kind::Light) miscCols = std::format(",,{},", it.lightRadius);
            const std::string enchCols = std::format("{},{},", CsvQuote(ench && ench->GetName() ? ench->GetName() : ""),
                ench && enchantable ? std::to_string(enchantable->amountofEnchantment) : std::string{});
            const std::string prefix = head + magicCols + weaponCols + armorCols + miscCols + enchCols;
            const std::string itemTail = std::format("{},{},",
                it.ammoNonBolt ? (*it.ammoNonBolt ? "1" : "0") : "", it.enchantCastingType >= 0 ? std::to_string(it.enchantCastingType) : std::string{});

            bool first = true;
            auto itemEnd = [&]() {
                std::string s = first ? std::format("{},{},{}", m.inScope ? 1 : 0, slotClass, CsvQuote(cap)) : std::string(",,");
                first = false;
                return s;
            };
            struct Parts
            {
                std::string oldCols;  // effectIndex..effectDescription (18), each with its comma
                std::string newCols;  // effectPlugin..effectKept (11), each with its comma
            };
            auto effectParts = [&](std::size_t index, const EffectRow& row, const RE::Effect* e, std::string_view payloadOf,
                                   std::string_view payloadSpell, const RowOutcome* outcome) {
                const auto& mg = read.effects[row.effect];
                const auto& cls = classes[row.effect];
                const auto* mgef = read.mgefs[row.effect];
                std::string conds;
                if (mgef && mgef->conditions.head) conds = "mgef[" + FormatConditions(mgef->conditions) + "]";
                if (e && e->conditions.head) {
                    if (!conds.empty()) conds += ' ';
                    conds += "item[" + FormatConditions(e->conditions) + "]";
                }
                const auto sc = scripts.find(mg.formId);
                Parts p;
                p.oldCols = std::format("{},{:08X},{},{},{},{},{},{},{},{},{},{},{},{},{},{:08X},{},{},", index, mg.formId,
                    CsvQuote(mg.name), mg.archetype, mg.primaryAV, mg.secondaryAV, mg.resistAV, mg.delivery,
                    mg.castingType, Num(row.magnitude), row.duration, row.area, Num(mg.baseCost), mg.detrimental ? 1 : 0,
                    mg.hostile ? 1 : 0, mg.flags, CsvQuote(Util::KeywordList(mgef)), CsvQuote(mg.description));
                p.newCols = std::format("{},{},{},{},{},{},{},{},{},{},{},", CsvQuote(mg.plugin), Num(row.cost), mg.school,
                    CsvQuote(FillText(mg.description, row.magnitude, row.duration, row.area)),
                    CsvQuote(sc == scripts.end() ? std::string_view{} : std::string_view(sc->second)), CsvQuote(conds),
                    payloadOf, payloadSpell, Name(cls.col), RouteName(cls.route),
                    outcome ? (outcome->kept ? "1" : "0") : "");
                return p;
            };

            if (it.effects.empty()) {
                out << prefix << std::string(18, ',') << itemTail << std::string(11, ',') << itemEnd() << '\n';
                ++rows;
                continue;
            }
            for (std::size_t j = 0; j < it.effects.size(); ++j) {
                const auto& row = it.effects[j];
                const Parts p = effectParts(j, row, read.effectItems[i][j], "", "", &m.outcomes[j]);
                out << prefix << p.oldCols << itemTail << p.newCols << itemEnd() << '\n';
                ++rows;
                // Gap 3: the payload of a Cloak/hazard, as rows under its wrapper.
                const auto& payload = read.effects[row.effect].payload;
                const auto* spell = read.payloadSpells[row.effect];
                const std::string spellId = spell ? std::format("{:08X}", spell->GetFormID()) : std::string{};
                for (std::size_t k = 0; k < payload.size(); ++k) {
                    const Parts pp = effectParts(k, payload[k], read.payloadItems[row.effect][k], std::to_string(j),
                                                 spellId, nullptr);
                    out << prefix << pp.oldCols << itemTail << pp.newCols << ",," << '\n';
                    ++rows;
                }
            }
        }
        out.close();

        const double cov = tally.counted ? 100.0 * tally.mapped / tally.counted : 100.0;
        summary = std::format(
            "Wrote {} forms as {} rows to {}: {} in scope, coverage {:.2f}% ({} of {} visible effect rows); "
            "{} have a slot class today, {} of them with no description in cap; scripts read for {} of {} script "
            "effects ({} plugin files, {} unreadable)",
            read.items.size(), rows, path.filename().string(), inScope, cov, tally.mapped, tally.counted, withClass,
            undescribed, scriptStats.withScripts, scriptStats.wanted, scriptStats.filesRead, scriptStats.filesFailed);
        return true;
    }
}
