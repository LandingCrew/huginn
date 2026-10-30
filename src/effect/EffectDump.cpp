#include "EffectDump.h"

#include "EffectCatalog.h"
#include "EffectReader.h"
#include "ScriptNames.h"

#include "core/CapFacets.h"
#include "slot/SlotConfig.h"
#include "util/FormRead.h"
#include "util/InventoryUtil.h"

#include <fstream>

namespace Huginn::Effect
{
    namespace
    {
        using namespace Core::Effect;
        using Util::CsvQuote;

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

        // ---- the slot class (R11: re-derived from cap(i)) -----------------------
        // The class core/CapFacets.h gives the item as a candidate: the
        // catalog's facets, the school the catalog read and the weapon's type.
        // Before R11 this ran the old spell / item / weapon / scroll / apparel
        // classifiers over every form; they feed only the D2 legacy feature now.
        std::string_view ClassOf(const ItemRecord& it, const CatalogEntry* entry)
        {
            using Core::Facets::Source;
            Core::Facets::Subject s;
            switch (it.kind) {
                case Kind::Spell: s.source = Source::Spell; break;
                case Kind::Scroll: s.source = Source::Scroll; break;
                case Kind::Weapon: s.source = Source::Weapon; break;
                case Kind::Ammo: s.source = Source::Ammo; break;
                case Kind::Armour: s.source = Source::Apparel; break;
                case Kind::Light: s.source = Source::Torch; break;
                default: s.source = Source::Item; break;
            }
            s.kind = it.kind;
            if (entry) {
                s.known = true;
                s.facets = Core::Facets::FromCap(entry->cap, entry->kind);
                using CS = Core::Effect::School;
                using FS = Core::Facets::School;
                switch (entry->school) {
                    case CS::Alteration: s.school = FS::Alteration; break;
                    case CS::Conjuration: s.school = FS::Conjuration; break;
                    case CS::Destruction: s.school = FS::Destruction; break;
                    case CS::Illusion: s.school = FS::Illusion; break;
                    case CS::Restoration: s.school = FS::Restoration; break;
                    default: break;
                }
            }
            // The record's weaponType (EffectRecords.h): 1-6 melee, 7 bow, 8 staff, 9 crossbow.
            s.melee = it.weaponType >= 1 && it.weaponType <= 6;
            s.ranged = it.weaponType >= 7 && it.weaponType <= 9;
            return Slot::SlotClassificationToString(Core::Facets::Classify(s));
        }

        // The old columns keep the old dump's {:g}.
        std::string Num(float v) { return std::format("{:g}", v); }
    }

    bool WriteDumpAll(const std::filesystem::path& path, std::string& summary, std::chrono::milliseconds wait)
    {
        auto& catalog = EffectCatalog::GetSingleton();
        if (!(catalog.Ready() || (wait.count() > 0 && catalog.WaitUntilReady(wait)))) {
            summary = catalog.Failed() ? "The effect catalog failed to build (see the log)"
                                       : "The effect catalog is not ready yet; try again in a few seconds";
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
               "ammoNonBolt,enchantCasting,effectPlugin,effectCost,effectSchool,effectLightRadius,effectText,effectScripts,"
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
                slotClass = std::string(ClassOf(it, entry));
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
                std::string newCols;  // effectPlugin..effectKept (12), each with its comma
            };
            auto effectParts = [&](std::size_t index, const EffectRow& row, const RE::Effect* /*item*/, std::string_view payloadOf,
                                   std::string_view payloadSpell, const RowOutcome* outcome) {
                const auto& mg = read.effects[row.effect];
                const auto& cls = classes[row.effect];
                const auto* mgef = read.mgefs[row.effect];
                // The reader's condition text (EffectReader FormatConditions),
                // the same strings the mapper reads (core/EffectConditions.h).
                std::string conds;
                if (!mg.conditions.empty()) conds = "mgef[" + mg.conditions + "]";
                if (!row.conditions.empty()) {
                    if (!conds.empty()) conds += ' ';
                    conds += "item[" + row.conditions + "]";
                }
                const auto sc = scripts.find(mg.formId);
                Parts p;
                p.oldCols = std::format("{},{:08X},{},{},{},{},{},{},{},{},{},{},{},{},{},{:08X},{},{},", index, mg.formId,
                    CsvQuote(mg.name), mg.archetype, mg.primaryAV, mg.secondaryAV, mg.resistAV, mg.delivery,
                    mg.castingType, Num(row.magnitude), row.duration, row.area, Num(mg.baseCost), mg.detrimental ? 1 : 0,
                    mg.hostile ? 1 : 0, mg.flags, CsvQuote(Util::KeywordList(mgef)), CsvQuote(mg.description));
                p.newCols = std::format("{},{},{},{},{},{},{},{},{},{},{},{},", CsvQuote(mg.plugin), Num(row.cost), mg.school,
                    mg.lightRadius > 0 ? std::to_string(mg.lightRadius) : std::string{},
                    CsvQuote(FillText(mg.description, row.magnitude, row.duration, row.area)),
                    CsvQuote(sc == scripts.end() ? std::string_view{} : std::string_view(sc->second)), CsvQuote(conds),
                    payloadOf, payloadSpell, Name(outcome ? outcome->cls.col : cls.col),
                    outcome && outcome->carrier ? std::string_view("carrier")
                                                : RouteName(outcome ? outcome->cls.route : cls.route),
                    outcome ? (outcome->kept ? "1" : outcome->perk ? "perk" : outcome->bane ? "bane" : "0") : "");
                return p;
            };

            if (it.effects.empty()) {
                out << prefix << std::string(18, ',') << itemTail << std::string(12, ',') << itemEnd() << '\n';
                ++rows;
                continue;
            }
            for (std::size_t j = 0; j < it.effects.size(); ++j) {
                const auto& row = it.effects[j];
                const Parts p = effectParts(row.index, row, read.effectItems[i][j], "", "", &m.outcomes[j]);
                out << prefix << p.oldCols << itemTail << p.newCols << itemEnd() << '\n';
                ++rows;
                // Gap 3: the payload of a Cloak/hazard, as rows under its wrapper.
                const auto& payload = read.effects[row.effect].payload;
                const auto* spell = read.payloadSpells[row.effect];
                const std::string spellId = spell ? std::format("{:08X}", spell->GetFormID()) : std::string{};
                for (std::size_t k = 0; k < payload.size(); ++k) {
                    const Parts pp = effectParts(payload[k].index, payload[k], read.payloadItems[row.effect][k],
                                                 std::to_string(row.index),
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
            "{} have a slot class (from cap, R11), {} of them with no description in cap; scripts read for {} of {} script "
            "effects ({} plugin files, {} unreadable)",
            read.items.size(), rows, path.filename().string(), inScope, cov, tally.mapped, tally.counted, withClass,
            undescribed, scriptStats.withScripts, scriptStats.wanted, scriptStats.filesRead, scriptStats.filesFailed);
        return true;
    }
}
