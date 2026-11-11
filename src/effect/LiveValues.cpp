#include "LiveValues.h"

#include "EffectCatalog.h"
#include "SpellPower.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Huginn::Effect::LiveValues
{
    namespace
    {
        /// Items read per frame: a spell or a potion is a few perk entry
        /// point calls per effect (they walk the perk list); a weapon or an
        /// armour piece one inventory-display call per stack.
        constexpr std::size_t kItemsPerFrame = 12;
        /// The self-check's pace, and how new an effect it compares is (game
        /// seconds since it started; over the pace, so each is seen once).
        constexpr auto kCheckEvery = std::chrono::seconds(1);
        constexpr float kCheckNewWithin = 1.5f;
        /// A sweep with no fingerprint change: a perk entry point's own
        /// conditions (in combat, magicka, a state LoreRim toggles) move the
        /// numbers without moving anything the fingerprint reads -- seen
        /// 2026-11-10 as every spell flipping by 10 % and back.
        constexpr auto kSweepEvery = std::chrono::seconds(3);
        /// A change under this is no change (rounding in the perk math).
        constexpr float kEpsilon = 0.01f;
        /// After a load, the first read waits this long: read in the load's
        /// first frames, a dozen spells came out half their value (abilities
        /// not yet applied) and the next sweep put them back (2026-11-10).
        constexpr auto kLoadSettle = std::chrono::seconds(2);
        /// Lines a flip's cause list logs at most.
        constexpr std::size_t kMaxCauseLines = 16;

        constexpr std::array<RE::ActorValue, 5> kSkills{
            RE::ActorValue::kAlteration, RE::ActorValue::kConjuration, RE::ActorValue::kDestruction,
            RE::ActorValue::kIllusion, RE::ActorValue::kRestoration };
        constexpr std::array<RE::ActorValue, 5> kPowers{
            RE::ActorValue::kAlterationPowerModifier, RE::ActorValue::kConjurationPowerModifier,
            RE::ActorValue::kDestructionPowerModifier, RE::ActorValue::kIllusionPowerModifier,
            RE::ActorValue::kRestorationPowerModifier };
        constexpr std::array<const char*, 5> kSchoolNames{ "Alteration", "Conjuration", "Destruction", "Illusion",
                                                           "Restoration" };

        /// What drives a spell's power.
        struct Fingerprint
        {
            std::uint16_t level = 0;
            std::int8_t perkPoints = 0;
            std::array<std::int32_t, 5> skills{};   // whole points
            std::array<std::int32_t, 5> powers{};   // tenths of a percent
            std::size_t spellCount = 0;

            bool operator==(const Fingerprint&) const = default;
        };

        using Numbers = LiveNumbers;

        /// What an item is, for how it is read.
        enum class What : std::uint8_t
        {
            Spell,        // a spell the player knows
            Consumable,   // a potion or food the player carries (not a poison)
            Weapon,
            Armour,
        };
        constexpr std::array<const char*, 4> kWhatNames{ "spells", "potions and food", "weapons", "armour" };

        struct Work
        {
            RE::TESBoundObject* form = nullptr;
            What what = What::Spell;
        };

        /// What can move a spell's numbers without the fingerprint moving,
        /// taken at every sweep's end; a flip logs what changed since the
        /// last one (2026-11-10: every school x0.91 and back, cause unknown).
        ///   entries: each perk entry the player has for Mod Spell Magnitude /
        ///     Duration, and whether its Perk Owner conditions hold now (the
        ///     Spell and Effect tabs are the spell's keywords and school,
        ///     which do not change while it is known);
        ///   effects: each effect running on the player, and whether the game
        ///     holds it inactive (a conditional ability: a standing stone, a
        ///     blessing, a racial power);
        ///   hands: what each hand holds.
        struct Causes
        {
            std::unordered_map<const RE::BGSEntryPointPerkEntry*, bool> entries;
            std::unordered_map<std::uint64_t, bool> effects;   // (spell, effect setting) -> active
            std::unordered_map<std::uint64_t, std::string> effectNames;
            RE::FormID right = 0;
            RE::FormID left = 0;
            std::string rightName;
            std::string leftName;
            bool valid = false;
        };

        class EntryCollector final : public RE::PerkEntryVisitor
        {
        public:
            EntryCollector(RE::PlayerCharacter* player, Causes& out) : player_(player), out_(out) {}

            RE::BSContainer::ForEachResult Visit(RE::BGSPerkEntry* entry) override
            {
                if (entry && entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint) {
                    auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
                    // Tab 0 is the Perk Owner's: conditions on the player.
                    // An empty tab holds (TESCondition::IsTrue with no head).
                    const bool holds = ep->conditions.size() == 0 || ep->conditions[0].IsTrue(player_, player_);
                    out_.entries[ep] = holds;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }

        private:
            RE::PlayerCharacter* player_;
            Causes& out_;
        };

        std::string NameOf(const RE::TESForm* form)
        {
            if (!form) return "nothing";
            const char* n = form->GetName();
            return std::format("'{}' ({:08X})", n && *n ? n : "?", form->GetFormID());
        }

        Causes TakeCauses(RE::PlayerCharacter* player)
        {
            Causes c;
            EntryCollector collect(player, c);
            player->ForEachPerkEntry(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude, collect);
            player->ForEachPerkEntry(RE::BGSEntryPoint::ENTRY_POINT::kModSpellDuration, collect);
            if (auto* target = player->AsMagicTarget()) {
                if (auto* list = target->GetActiveEffectList()) {
                    for (auto* ae : *list) {
                        if (!ae || !ae->spell) continue;
                        const auto* mgef = ae->GetBaseObject();
                        const std::uint64_t key = (static_cast<std::uint64_t>(ae->spell->GetFormID()) << 32) |
                                                  (mgef ? mgef->GetFormID() : 0u);
                        const bool active = !ae->flags.any(RE::ActiveEffect::Flag::kInactive);
                        auto [it, inserted] = c.effects.try_emplace(key, active);
                        if (!inserted) it->second = it->second || active;   // any instance active
                        if (inserted) {
                            c.effectNames.emplace(key, std::format("{} effect {}", NameOf(ae->spell), NameOf(mgef)));
                        }
                    }
                }
            }
            const auto* r = player->GetEquippedObject(false);
            const auto* l = player->GetEquippedObject(true);
            c.right = r ? r->GetFormID() : 0;
            c.left = l ? l->GetFormID() : 0;
            c.rightName = NameOf(r);
            c.leftName = NameOf(l);
            c.valid = true;
            return c;
        }

        std::string DescribeEntry(const RE::BGSEntryPointPerkEntry* ep)
        {
            const char* point =
                ep->entryData.entryPoint == RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude ? "Mod Spell Magnitude"
                                                                                               : "Mod Spell Duration";
            std::string fn;
            const auto* data = static_cast<const RE::BGSEntryPointFunctionData*>(ep->functionData);
            const float v = data && data->GetType() == RE::BGSEntryPointFunctionData::FunctionType::kOneValue
                                ? static_cast<const RE::BGSEntryPointFunctionDataOneValue*>(data)->data
                                : 0.0f;
            using F = RE::BGSEntryPointPerkEntry::EntryData::Function;
            switch (ep->entryData.function.get()) {
            case F::kSetValue:
                fn = std::format("= {:g}", v);
                break;
            case F::kAddValue:
                fn = std::format("+ {:g}", v);
                break;
            case F::kMultiplyValue:
                fn = std::format("x {:g}", v);
                break;
            default:
                fn = std::format("function {}", static_cast<int>(ep->entryData.function.get()));
                break;
            }
            return std::format("perk {} {} {}", NameOf(ep->perk), point, fn);
        }

        /// The flip's cause list: what changed between two snapshots.
        void LogCauses(const Causes& before, const Causes& now)
        {
            std::vector<std::string> lines;
            for (const auto& [ep, holds] : now.entries) {
                const auto it = before.entries.find(ep);
                if (it == before.entries.end()) {
                    lines.push_back(std::format("{}: new, {}", DescribeEntry(ep), holds ? "holds" : "does not hold"));
                }
                else if (it->second != holds) {
                    lines.push_back(std::format("{}: {}", DescribeEntry(ep), holds ? "now holds" : "no longer holds"));
                }
            }
            for (const auto& [ep, holds] : before.entries) {
                if (!now.entries.contains(ep)) lines.push_back(std::format("{}: gone", DescribeEntry(ep)));
            }
            for (const auto& [key, active] : now.effects) {
                const auto it = before.effects.find(key);
                const auto& name = now.effectNames.at(key);
                if (it == before.effects.end()) {
                    lines.push_back(std::format("{}: started{}", name, active ? "" : " (inactive)"));
                }
                else if (it->second != active) {
                    lines.push_back(std::format("{}: {}", name, active ? "now active" : "now inactive"));
                }
            }
            for (const auto& [key, active] : before.effects) {
                if (!now.effects.contains(key)) lines.push_back(std::format("{}: ended", before.effectNames.at(key)));
            }
            if (before.right != now.right) lines.push_back(std::format("right hand {} -> {}", before.rightName, now.rightName));
            if (before.left != now.left) lines.push_back(std::format("left hand {} -> {}", before.leftName, now.leftName));

            // At debug: Faction Ranks' Mages Guild Boon (x1.1 from a kill to
            // the next cast) flips every spell on each kill and cast; the
            // flip line itself stays at info.
            if (lines.empty()) {
                logger::debug(
                    "[LiveValues] flip cause: none seen -- no perk entry's owner conditions, effect on the player or hand changed since the last sweep (hands {} / {})"sv,
                    now.rightName, now.leftName);
                return;
            }
            // Perk entries first: the likeliest cause.
            std::ranges::stable_sort(lines, [](const std::string& a, const std::string& b) {
                return a.starts_with("perk") && !b.starts_with("perk");
            });
            const std::size_t shown = std::min(lines.size(), kMaxCauseLines);
            for (std::size_t i = 0; i < shown; ++i) logger::debug("[LiveValues] flip cause? {}"sv, lines[i]);
            if (lines.size() > shown) logger::debug("[LiveValues] flip cause? ... and {} more"sv, lines.size() - shown);
        }

        // Main thread only.
        struct MainState
        {
            Fingerprint last;
            bool haveLast = false;
            std::uint32_t seenGeneration = 0;
            std::vector<Work> queue;
            std::array<std::size_t, 4> kinds{};   // this sweep's items by What, for its summary line
            std::size_t cursor = 0;
            std::size_t moved = 0;   // this sweep, for its summary line
            std::string why;
            bool quiet = false;      // the periodic sweep: its line at debug
            float ratio = 0.0f;      // the first moved magnitude's new / old, for the flip line
            std::chrono::steady_clock::time_point nextSweep{};
            std::unordered_map<RE::FormID, Numbers> computed;
            // An item's numbers before its last move: a cast can move them
            // (the Mages Guild Boon is spent by it) before the self-check sees
            // the effect it made.
            std::unordered_map<RE::FormID, Numbers> prior;
            std::unordered_set<std::uint64_t> checked;   // (spell, effect index) logged by the self-check
            std::chrono::steady_clock::time_point nextCheck{};
            std::chrono::steady_clock::time_point loadedAt{};
            Causes causes;   // at the last sweep's end
        };
        MainState g_main;

        std::atomic<bool> g_loaded{ false };
        std::atomic<std::uint32_t> g_generation{ 0 };

        // Handed to the update loop.
        std::mutex g_pendingMutex;
        std::unordered_map<RE::FormID, std::pair<RE::TESBoundObject*, Numbers>> g_pending;

        /// The player's spells a key can show (a cast spell; abilities,
        /// diseases and powers are never in scope).
        std::vector<RE::SpellItem*> KnownSpells(RE::PlayerCharacter* player)
        {
            std::vector<RE::SpellItem*> out;
            const auto add = [&](RE::SpellItem* s) {
                if (s && s->GetSpellType() == RE::MagicSystem::SpellType::kSpell &&
                    std::ranges::find(out, s) == out.end()) {
                    out.push_back(s);
                }
            };
            if (auto* base = player->GetActorBase()) {
                if (auto* list = base->GetSpellList(); list && list->spells) {
                    for (std::uint32_t i = 0; i < list->numSpells; ++i) add(list->spells[i]);
                }
            }
            for (auto* s : player->GetActorRuntimeData().addedSpells) add(s);
            return out;
        }

        /// Everything a sweep reads: the spells known, then the potions,
        /// food, weapons and armour the player carries (the inventory's
        /// entries; a poison's strength is the Poisoner perks' entry points,
        /// not these, and a staff has no damage).
        std::vector<Work> Items(RE::PlayerCharacter* player)
        {
            std::vector<Work> out;
            for (auto* s : KnownSpells(player)) out.push_back({ s, What::Spell });
            auto* inv = player->GetInventoryChanges();
            if (!inv || !inv->entryList) return out;
            for (auto* e : *inv->entryList) {
                auto* obj = e ? e->object : nullptr;
                if (!obj) continue;
                if (obj->Is(RE::FormType::AlchemyItem)) {
                    if (!static_cast<RE::AlchemyItem*>(obj)->IsPoison()) out.push_back({ obj, What::Consumable });
                }
                else if (obj->Is(RE::FormType::Weapon)) {
                    if (!static_cast<RE::TESObjectWEAP*>(obj)->IsStaff()) out.push_back({ obj, What::Weapon });
                }
                else if (obj->Is(RE::FormType::Armor)) {
                    out.push_back({ obj, What::Armour });
                }
            }
            return out;
        }

        Fingerprint Read(RE::PlayerCharacter* player, std::size_t spellCount)
        {
            Fingerprint f;
            f.level = player->GetLevel();
            f.perkPoints = player->GetGameStatsData().perkCount;
            if (auto* avo = player->AsActorValueOwner()) {
                for (std::size_t i = 0; i < kSkills.size(); ++i) {
                    f.skills[i] = static_cast<std::int32_t>(std::floor(avo->GetActorValue(kSkills[i])));
                    f.powers[i] = static_cast<std::int32_t>(std::lround(avo->GetActorValue(kPowers[i]) * 10.0f));
                }
            }
            f.spellCount = spellCount;
            return f;
        }

        /// What moved, for the sweep's log line.
        std::string WhatMoved(const Fingerprint& a, const Fingerprint& b)
        {
            std::string out;
            const auto add = [&](const std::string& s) {
                if (!out.empty()) out += ", ";
                out += s;
            };
            if (a.level != b.level) add(std::format("level {} -> {}", a.level, b.level));
            if (a.perkPoints != b.perkPoints) add(std::format("perk points {} -> {}", a.perkPoints, b.perkPoints));
            for (std::size_t i = 0; i < kSkills.size(); ++i) {
                if (a.skills[i] != b.skills[i]) add(std::format("{} {} -> {}", kSchoolNames[i], a.skills[i], b.skills[i]));
                if (a.powers[i] != b.powers[i]) {
                    add(std::format("Fortify {} {:+.1f}% -> {:+.1f}%", kSchoolNames[i], a.powers[i] / 10.0, b.powers[i] / 10.0));
                }
            }
            if (a.spellCount != b.spellCount) add(std::format("spells known {} -> {}", a.spellCount, b.spellCount));
            return out.empty() ? std::string("nothing") : out;
        }

        Numbers RowsOf(const std::vector<SpellEffectPower>& power)
        {
            Numbers n;
            n.rows.reserve(power.size());
            for (std::uint32_t i = 0; i < power.size(); ++i) {
                n.rows.push_back(LiveRow{ i, power[i].magnitude, power[i].duration });
            }
            return n;
        }

        bool Same(const Numbers& a, const Numbers& b)
        {
            if (a.rows.size() != b.rows.size() || std::fabs(a.damage - b.damage) > kEpsilon ||
                std::fabs(a.armorRating - b.armorRating) > kEpsilon) {
                return false;
            }
            for (std::size_t i = 0; i < a.rows.size(); ++i) {
                if (std::fabs(a.rows[i].magnitude - b.rows[i].magnitude) > kEpsilon ||
                    std::fabs(a.rows[i].duration - b.rows[i].duration) > kEpsilon) {
                    return false;
                }
            }
            return true;
        }

        float RecordDamage(const RE::TESBoundObject* obj)
        {
            return static_cast<float>(static_cast<const RE::TESObjectWEAP*>(obj)->GetAttackDamage());
        }

        float RecordRating(const RE::TESBoundObject* obj)
        {
            return const_cast<RE::TESObjectARMO*>(static_cast<const RE::TESObjectARMO*>(obj))->GetArmorRating();
        }

        /// The record's own numbers: an item no perk, skill or tempering
        /// moves keeps the catalog's entry.
        bool SameAsRecord(const Work& w, const Numbers& n)
        {
            if (w.what == What::Weapon) return std::fabs(n.damage - RecordDamage(w.form)) <= kEpsilon;
            if (w.what == What::Armour) return std::fabs(n.armorRating - RecordRating(w.form)) <= kEpsilon;
            const auto* item = static_cast<const RE::MagicItem*>(w.form);
            std::uint32_t i = 0;
            for (const auto* e : item->effects) {
                if (!e) {
                    ++i;
                    continue;
                }
                for (const auto& r : n.rows) {
                    if (r.index != i) continue;
                    if (std::fabs(r.magnitude - e->effectItem.magnitude) > kEpsilon ||
                        std::fabs(r.duration - static_cast<float>(e->effectItem.duration)) > kEpsilon) {
                        return false;
                    }
                }
                ++i;
            }
            return true;
        }

        /// A weapon's damage or an armour piece's rating as the inventory
        /// shows it (PlayerCharacter::GetDamage / GetArmorValue: skill, perks,
        /// tempering), for the best stack the player carries, over the same
        /// item with no stack data -- times the record's value. So tempering
        /// (and a perk that keys on the stack, as worn) stays, and the
        /// player's skill and a perk every item of the type shares divide
        /// out: the catalog grades a weapon among its own type and an armour
        /// piece among its slot and weight (EffectMapper's groups), where a
        /// multiplier they all share changes no order and would only push
        /// every grade toward the top of a population of records. < 0: not
        /// read (not carried, no value).
        float ReadGear(RE::PlayerCharacter* player, RE::TESBoundObject* obj, bool weapon)
        {
            const float record = weapon ? RecordDamage(obj) : RecordRating(obj);
            if (record <= 0.0f) return -1.0f;
            auto* inv = player->GetInventoryChanges();
            if (!inv || !inv->entryList) return -1.0f;
            RE::InventoryEntryData* entry = nullptr;
            for (auto* e : *inv->entryList) {
                if (e && e->object == obj) {
                    entry = e;
                    break;
                }
            }
            if (!entry) return -1.0f;
            const auto shown = [&](RE::ExtraDataList* xl) {
                // One stack as the inventory menu builds it: its own list of
                // one extra data list. The destructor frees the list's nodes,
                // never the extra data.
                RE::InventoryEntryData one(obj, 1);
                if (xl) {
                    one.extraLists = new RE::BSSimpleList<RE::ExtraDataList*>();
                    one.extraLists->push_front(xl);
                }
                return weapon ? player->GetDamage(&one) : player->GetArmorValue(&one);
            };
            const float plain = shown(nullptr);
            if (!(plain > 0.0f)) return -1.0f;
            float best = plain;
            if (entry->extraLists) {
                for (auto* xl : *entry->extraLists) {
                    if (xl) best = std::max(best, shown(xl));
                }
            }
            if (!g_main.quiet) {
                logger::debug("[LiveValues] {} {:08X} '{}': record {:.1f}, shown {:.1f} untempered, {:.1f} best stack -> {:.1f}"sv,
                    weapon ? "weapon" : "armour", obj->GetFormID(), obj->GetName() ? obj->GetName() : "?", record, plain,
                    best, record * best / plain);
            }
            return record * best / plain;
        }

        /// One item's numbers now; false when it has none to read.
        bool ReadOne(RE::PlayerCharacter* player, const Work& w, Numbers& out)
        {
            switch (w.what) {
            case What::Spell:
            case What::Consumable:
                out = RowsOf(SpellPowerOf(player, static_cast<RE::MagicItem*>(w.form)));
                return true;
            case What::Weapon:
                out = Numbers{};
                out.damage = ReadGear(player, w.form, true);
                return out.damage >= 0.0f;
            case What::Armour:
                out = Numbers{};
                out.armorRating = ReadGear(player, w.form, false);
                return out.armorRating >= 0.0f;
            }
            return false;
        }

        /// New over old, for the flip line: the first number that moved.
        float RatioOf(const Numbers& from, const Numbers& to)
        {
            if (from.damage > kEpsilon && std::fabs(to.damage - from.damage) > kEpsilon) return to.damage / from.damage;
            if (from.armorRating > kEpsilon && std::fabs(to.armorRating - from.armorRating) > kEpsilon) {
                return to.armorRating / from.armorRating;
            }
            for (std::size_t i = 0; i < to.rows.size() && i < from.rows.size(); ++i) {
                if (std::fabs(from.rows[i].magnitude) > kEpsilon &&
                    std::fabs(to.rows[i].magnitude - from.rows[i].magnitude) > kEpsilon) {
                    return to.rows[i].magnitude / from.rows[i].magnitude;
                }
                if (from.rows[i].duration > kEpsilon && std::fabs(to.rows[i].duration - from.rows[i].duration) > kEpsilon) {
                    return to.rows[i].duration / from.rows[i].duration;
                }
            }
            return 0.0f;
        }

        void StartSweep(RE::PlayerCharacter* player, std::string why, bool quiet = false, bool onlyNew = false)
        {
            g_main.queue = Items(player);
            if (onlyNew) {
                std::erase_if(g_main.queue, [](const Work& w) { return g_main.computed.contains(w.form->GetFormID()); });
            }
            g_main.kinds = {};
            for (const auto& w : g_main.queue) ++g_main.kinds[static_cast<std::size_t>(w.what)];
            g_main.cursor = 0;
            g_main.moved = 0;
            g_main.why = std::move(why);
            g_main.quiet = quiet;
            g_main.ratio = 0.0f;
            g_main.nextSweep = std::chrono::steady_clock::now() + kSweepEvery;
        }

        std::string KindsText()
        {
            std::string out;
            for (std::size_t i = 0; i < kWhatNames.size(); ++i) {
                if (g_main.kinds[i] == 0) continue;
                if (!out.empty()) out += ", ";
                out += std::format("{} {}", g_main.kinds[i], kWhatNames[i]);
            }
            return out.empty() ? std::string("nothing") : out;
        }

        void ReadSome(RE::PlayerCharacter* player)
        {
            const std::size_t end = std::min(g_main.queue.size(), g_main.cursor + kItemsPerFrame);
            for (; g_main.cursor < end; ++g_main.cursor) {
                const Work& w = g_main.queue[g_main.cursor];
                if (!w.form) continue;
                Numbers now;
                if (!ReadOne(player, w, now)) continue;
                const auto it = g_main.computed.find(w.form->GetFormID());
                const bool known = it != g_main.computed.end();
                if (known ? Same(it->second, now) : SameAsRecord(w, now)) {
                    if (!known) g_main.computed.emplace(w.form->GetFormID(), now);
                    continue;
                }
                if (known && g_main.ratio == 0.0f) g_main.ratio = RatioOf(it->second, now);
                if (known) g_main.prior[w.form->GetFormID()] = it->second;
                g_main.computed[w.form->GetFormID()] = now;
                ++g_main.moved;
                std::lock_guard lock(g_pendingMutex);
                g_pending[w.form->GetFormID()] = { w.form, std::move(now) };
            }
            if (g_main.cursor >= g_main.queue.size() && !g_main.queue.empty()) {
                if (!g_main.quiet) {
                    logger::info("[LiveValues] read {} ({}): {} moved"sv, KindsText(), g_main.why, g_main.moved);
                }
                else if (g_main.moved > 0) {
                    // Numbers moved with nothing the fingerprint reads: a perk
                    // entry point's own condition (Faction Ranks' Mages Guild
                    // Boon, 2026-11-11), a stack tempered, a potion picked up.
                    auto* avo = player->AsActorValueOwner();
                    const auto pct = [avo](RE::ActorValue av) {
                        if (!avo) return -1.0f;
                        const float max = avo->GetPermanentActorValue(av);
                        return max > 0.0f ? 100.0f * avo->GetActorValue(av) / max : -1.0f;
                    };
                    const auto* state = player->AsActorState();
                    logger::info(
                        "[LiveValues] {} items moved x{:.2f} with no level, perk, skill or Fortify change ({}, {}): sneaking {}, in combat {}, drawn {}, health {:.0f}% magicka {:.0f}% stamina {:.0f}%"sv,
                        g_main.moved, g_main.ratio, g_main.why, KindsText(), player->IsSneaking(), player->IsInCombat(),
                        state && state->IsWeaponDrawn(), pct(RE::ActorValue::kHealth), pct(RE::ActorValue::kMagicka),
                        pct(RE::ActorValue::kStamina));
                }
                // What could move the numbers, now; a flip names what changed
                // since the last sweep.
                Causes now = TakeCauses(player);
                if (g_main.quiet && g_main.moved > 0 && g_main.causes.valid) LogCauses(g_main.causes, now);
                g_main.causes = std::move(now);
                g_main.queue.clear();
                g_main.cursor = 0;
            }
        }

        /// A spell the player cast, or a potion or food they took, that just
        /// started on them: the game's own numbers against ours, once per
        /// (item, effect) a session. Only a new effect: one running from
        /// before (a save's, a drink taken before Hydrated) carries the
        /// numbers of when it started, not now's (2026-11-11: a waterskin
        /// 20 for 900 s against 23.1 for 945 s, read after the drink gave the
        /// perks that scale it).
        void SelfCheck(RE::PlayerCharacter* player)
        {
            auto* target = player->AsMagicTarget();
            auto* list = target ? target->GetActiveEffectList() : nullptr;
            if (!list) return;
            for (auto* ae : *list) {
                if (!ae || !ae->spell || !ae->effect) continue;
                // Only what the player cast: an enemy's Firebolt on them is
                // the enemy's numbers.
                if (ae->GetCasterActor().get() != player) continue;
                if (ae->elapsedSeconds > kCheckNewWithin) continue;
                const auto it = g_main.computed.find(ae->spell->GetFormID());
                if (it == g_main.computed.end()) continue;
                std::uint32_t index = 0;
                bool found = false;
                for (const auto* e : ae->spell->effects) {
                    if (e == ae->effect) {
                        found = true;
                        break;
                    }
                    ++index;
                }
                if (!found) continue;
                const std::uint64_t key = (static_cast<std::uint64_t>(ae->spell->GetFormID()) << 32) | index;
                if (g_main.checked.contains(key)) continue;
                const auto rowOf = [index](const Numbers& n) -> const LiveRow* {
                    for (const auto& r : n.rows) {
                        if (r.index == index) return &r;
                    }
                    return nullptr;
                };
                const LiveRow* ours = rowOf(it->second);
                if (!ours) continue;
                g_main.checked.insert(key);
                const auto* mgef = ae->GetBaseObject();
                const float liveMag = std::fabs(ae->magnitude);
                // No magnitude to compare: the record has none (Waterbreathing:
                // the game fills in 2.58, which grades nothing) or the effect
                // takes none (Featherfall: ours 2.58, the game's 0).
                const bool noMagnitude =
                    std::fabs(ours->magnitude) <= kEpsilon ||
                    (mgef && mgef->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kNoMagnitude));
                const auto agrees = [&](const LiveRow& r) {
                    const bool magOk =
                        noMagnitude || std::fabs(liveMag - std::fabs(r.magnitude)) <= std::max(0.05f, 0.02f * liveMag);
                    const bool durOk = std::fabs(ae->duration - r.duration) <= std::max(0.5f, 0.02f * ae->duration);
                    return magOk && durOk;
                };
                bool ok = agrees(*ours);
                bool byPrior = false;
                if (!ok) {
                    // The numbers before the last move: the cast itself moved them.
                    if (const auto pr = g_main.prior.find(ae->spell->GetFormID()); pr != g_main.prior.end()) {
                        if (const auto* before = rowOf(pr->second); before && agrees(*before)) {
                            ok = true;
                            byPrior = true;
                            ours = before;
                        }
                    }
                }
                logger::info(
                    "[LiveValues] check {:08X} '{}' effect {} '{}': ours {:.2f} for {:.0f}s, the game's {:.2f} for {:.0f}s -- {}"sv,
                    ae->spell->GetFormID(), ae->spell->GetName() ? ae->spell->GetName() : "?", index,
                    mgef && mgef->GetName() ? mgef->GetName() : "?", ours->magnitude, ours->duration, liveMag,
                    ae->duration,
                    !ok        ? "MISMATCH (a dual cast scales the game's; otherwise the reading misses a modifier)"
                    : byPrior  ? "match (the numbers before the cast moved them)"
                    : noMagnitude ? "match (no magnitude to compare: the duration checked)"
                                  : "match");
            }
        }
    }

    void OnPlayerUpdate(RE::PlayerCharacter* player)
    {
        if (!player || !g_loaded.load(std::memory_order_acquire)) return;
        if (!player->Get3D() || !Effect::EffectCatalog::GetSingleton().Ready()) return;

        const std::uint32_t gen = g_generation.load(std::memory_order_acquire);
        if (gen != g_main.seenGeneration) {
            // A load: everything read again, the self-check asks again.
            g_main.seenGeneration = gen;
            g_main.haveLast = false;
            g_main.computed.clear();
            g_main.prior.clear();
            g_main.checked.clear();
            g_main.queue.clear();
            g_main.cursor = 0;
            g_main.causes = Causes{};
            g_main.loadedAt = std::chrono::steady_clock::now();
        }
        // The load's first frames: abilities are still being applied.
        if (!g_main.haveLast && std::chrono::steady_clock::now() - g_main.loadedAt < kLoadSettle) return;

        auto& added = player->GetActorRuntimeData().addedSpells;
        std::size_t baseCount = 0;
        if (auto* base = player->GetActorBase()) {
            if (auto* list = base->GetSpellList()) baseCount = list->numSpells;
        }
        const Fingerprint now = Read(player, baseCount + added.size());
        if (!g_main.haveLast) {
            g_main.last = now;
            g_main.haveLast = true;
            StartSweep(player, "a load");
        }
        else if (!(now == g_main.last)) {
            // Only the spells known moved (LoreRim's Arcane Anchor comes and
            // goes while a tome is read): read the new ones, nothing else.
            Fingerprint countOnly = g_main.last;
            countOnly.spellCount = now.spellCount;
            const bool onlyCount = countOnly == now;
            const std::string why = WhatMoved(g_main.last, now);
            g_main.last = now;
            if (onlyCount) {
                if (g_main.queue.empty()) StartSweep(player, why, /*quiet=*/true, /*onlyNew=*/true);
            }
            else {
                StartSweep(player, why);
            }
        }
        else if (g_main.queue.empty() && std::chrono::steady_clock::now() >= g_main.nextSweep) {
            StartSweep(player, "the periodic sweep", /*quiet=*/true);
        }
        if (!g_main.queue.empty()) ReadSome(player);

        const auto t = std::chrono::steady_clock::now();
        if (t >= g_main.nextCheck) {
            g_main.nextCheck = t + kCheckEvery;
            SelfCheck(player);
        }
    }

    void SetGameLoaded(bool loaded) noexcept
    {
        g_generation.fetch_add(1, std::memory_order_acq_rel);
        g_loaded.store(loaded, std::memory_order_release);
        {
            std::lock_guard lock(g_pendingMutex);
            g_pending.clear();
        }
        Effect::EffectCatalog::GetSingleton().ClearLive();
    }

    bool ApplyPending(std::size_t budget)
    {
        std::vector<std::pair<RE::TESBoundObject*, Numbers>> take;
        {
            std::lock_guard lock(g_pendingMutex);
            if (g_pending.empty()) return false;
            for (auto it = g_pending.begin(); it != g_pending.end() && take.size() < budget;) {
                take.push_back(std::move(it->second));
                it = g_pending.erase(it);
            }
        }
        auto& catalog = Effect::EffectCatalog::GetSingleton();
        bool changed = false;
        for (auto& [form, n] : take) {
            if (!catalog.SetLive(form, n)) continue;
            changed = true;
            if (const auto* entry = catalog.Find(form->GetFormID())) {
                std::string numbers;
                for (const auto& r : n.rows) {
                    if (!numbers.empty()) numbers += ", ";
                    numbers += std::format("{:.1f} for {:.0f}s", r.magnitude, r.duration);
                }
                if (n.damage >= 0.0f) numbers = std::format("damage {:.1f} (record {:.0f})", n.damage, RecordDamage(form));
                if (n.armorRating >= 0.0f) {
                    numbers = std::format("armour {:.1f} (record {:.0f})", n.armorRating, RecordRating(form));
                }
                logger::debug("[LiveValues] {:08X} '{}' now {}: {}"sv, form->GetFormID(),
                    form->GetName() ? form->GetName() : "?", numbers, Core::Effect::FormatCap(entry->cap));
            }
        }
        return changed;
    }
}
