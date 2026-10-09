#include "EffectMapper.h"

#include "core/MiniRegex.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace Huginn::Core::Effect
{
    namespace
    {
        constexpr float kDurationCap = 3600.0f;          // one hour: the D() and Level horizon
        constexpr std::uint32_t kDurationSentinel = 86400;  // a day or more: "permanent"
        constexpr float kFullRestore = 9999.0f;

        bool StartsWith(std::string_view s, std::string_view p) noexcept { return s.substr(0, p.size()) == p; }
        bool HasKeyword(const std::vector<std::string>& kws, std::string_view k) noexcept
        {
            return std::find(kws.begin(), kws.end(), k) != kws.end();
        }
        bool HasKeywordPrefix(const std::vector<std::string>& kws, std::string_view p) noexcept
        {
            return std::any_of(kws.begin(), kws.end(), [&](const std::string& k) { return StartsWith(k, p); });
        }

        enum class Rule : std::uint8_t { Amount, Level, P, PD, PG, PGD, PArea, Hunger, Thirst };

        Rule RuleOf(Col c)
        {
            if (LevelOf(c) == Level::Family) return Rule::P;
            const std::string_view n = Name(c);
            switch (c) {
                case Col::defense_ethereal: return Rule::PD;
                case Col::control_paralysis: return Rule::PD;
                case Col::control_stagger: return Rule::PG;
                case Col::control_slow: return Rule::PGD;
                case Col::control_disarm:
                case Col::control_grab: return Rule::P;
                case Col::control_silence:
                case Col::control_sleep:
                case Col::control_blind: return Rule::PD;
                case Col::summon_reanimate: return Rule::PG;
                case Col::vision_light:
                case Col::vision_detect_life: return Rule::PArea;
                case Col::vision_night_eye: return Rule::PD;
                case Col::vision_clairvoyance: return Rule::P;
                case Col::movement_speed: return Rule::Level;
                case Col::movement_jump_fall: return Rule::PD;
                case Col::movement_water_walking: return Rule::P;
                case Col::utility_carry_weight: return Rule::Level;
                case Col::utility_water_breathing: return Rule::PD;
                case Col::utility_telekinesis: return Rule::P;
                case Col::utility_unlock: return Rule::PG;
                case Col::utility_slow_time: return Rule::PD;
                case Col::utility_teleport:
                case Col::utility_transmute:
                case Col::utility_size: return Rule::P;
                case Col::shout_recovery: return Rule::Level;
                case Col::soul_trap: return Rule::PD;
                case Col::survival_hunger: return Rule::Hunger;
                case Col::survival_thirst: return Rule::Thirst;
                case Col::survival_warmth: return Rule::Level;
                case Col::survival_fatigue:
                case Col::survival_intoxication: return Rule::P;
                case Col::meta_potion_duration: return Rule::Level;
                case Col::drain_skill: return Rule::Level;
                default: break;
            }
            if (StartsWith(n, "restore_") || StartsWith(n, "damage_") || StartsWith(n, "absorb_")) return Rule::Amount;
            if (StartsWith(n, "fortify_vital_") || StartsWith(n, "regen_") || StartsWith(n, "drain_vital_") ||
                StartsWith(n, "weaken_regen_") || StartsWith(n, "resist_") || StartsWith(n, "weakness_") ||
                StartsWith(n, "fortify_skill_") || StartsWith(n, "fortify_combat_") || StartsWith(n, "weaken_combat_") ||
                StartsWith(n, "defense_")) {
                return Rule::Level;
            }
            if (StartsWith(n, "influence_")) return Rule::PGD;
            if (StartsWith(n, "summon_") || StartsWith(n, "stealth_")) return Rule::PD;
            return Rule::P;  // cure_*, transform_*, meta_xp_gain ...
        }

        float ClipDuration(std::uint32_t d) noexcept
        {
            return d >= kDurationSentinel ? kDurationCap : std::min(static_cast<float>(d), kDurationCap);
        }

        float DurationFactor(std::uint32_t d) noexcept
        {
            if (d == 0) return 1.0f;
            return static_cast<float>(std::log1p(ClipDuration(d)) / std::log1p(kDurationCap));
        }

        int VitalIndex(Col c) noexcept
        {
            switch (c) {
                case Col::restore_health: return 0;
                case Col::restore_magicka: return 1;
                case Col::restore_stamina: return 2;
                default: return -1;
            }
        }

        float HungerSize(const MagicEffectRecord& m)
        {
            for (const auto& k : m.keywords) {
                if (StartsWith(k, "CCSM_RestoreHunger")) {
                    const std::string_view s = std::string_view(k).substr(18);
                    if (s == "Tiny") return 0.25f;
                    if (s == "Small") return 0.5f;
                    if (s == "Medium") return 0.75f;
                    if (s == "Large") return 1.0f;
                }
            }
            const std::string n = Lower(m.name);
            if (n.find("very small") != std::string::npos || n.find("tiny") != std::string::npos) return 0.25f;
            if (n.find("small") != std::string::npos) return 0.5f;
            if (n.find("medium") != std::string::npos) return 0.75f;
            if (n.find("large") != std::string::npos) return 1.0f;
            return 0.5f;
        }

        Col SlotColumn(std::uint32_t mask) noexcept
        {
            if (mask & 0x4) return Col::armour_slot_body;
            if (mask & 0x200) return Col::armour_slot_shield;
            if (mask & (0x1 | 0x2 | 0x800 | 0x1000)) return Col::armour_slot_head;
            if (mask & (0x8 | 0x10)) return Col::armour_slot_hands;
            if (mask & (0x80 | 0x100)) return Col::armour_slot_feet;
            if (mask & 0x20) return Col::armour_slot_amulet;
            if (mask & 0x40) return Col::armour_slot_ring;
            return Col::_Count;
        }

        Col WeaponTypeColumn(const ItemRecord& it) noexcept
        {
            switch (it.weaponType) {
                case 1: return Col::weapon_type_sword;
                case 2: return Col::weapon_type_dagger;
                case 3: return Col::weapon_type_war_axe;
                case 4: return Col::weapon_type_mace;
                case 5: return Col::weapon_type_greatsword;
                case 6: return HasKeyword(it.keywords, "WeapTypeWarhammer") ? Col::weapon_type_warhammer
                                                                            : Col::weapon_type_battleaxe;
                case 7: return Col::weapon_type_bow;
                case 9: return Col::weapon_type_crossbow;
                default: return Col::_Count;
            }
        }

        bool IsBolt(const ItemRecord& it)
        {
            if (it.ammoNonBolt) return !*it.ammoNonBolt;
            if (HasKeyword(it.keywords, "OCF_AmmoTypeBolt")) return true;
            if (HasKeyword(it.keywords, "OCF_AmmoTypeArrow")) return false;
            return Lower(it.name).find("bolt") != std::string::npos;
        }

        void Set(Cap& cap, Col c, float v)
        {
            if (c == Col::_Count || !(v > 0.0f)) return;
            for (auto& x : cap) {
                if (x.col == c) {
                    x.v = std::max(x.v, v);
                    return;
                }
            }
            cap.push_back(Value{ c, v });
        }

        /// Is this kept row the one that counts for column `c` of its item?
        /// A visible row always is; a hidden row only when no visible row of
        /// the item sets `c` (Simonrim repeats rows hidden at perk magnitudes).
        struct Eligibility
        {
            std::array<bool, kColumnCount> visibleHas{};
            explicit Eligibility(const ItemMapping& m)
            {
                for (const auto& r : m.rows) {
                    if (!r.visible) continue;
                    if (r.col != Col::_Count) visibleHas[Index(r.col)] = true;
                    if (r.col2 != Col::_Count) visibleHas[Index(r.col2)] = true;
                }
            }
            [[nodiscard]] bool operator()(const KeptRow& r, Col c) const noexcept
            {
                return c != Col::_Count && (r.visible || !visibleHas[Index(c)]);
            }
        };

        float RowValue(const KeptRow& r, Col c, const Populations& pops)
        {
            if (r.fullRestore) return 1.0f;
            if (r.graded) return pops.Percentile(c, 0, r.raw) * r.post;
            return r.post;
        }

        bool BadName(std::string_view name)
        {
            static const MiniRegex bad(
                R"re(\b(dummy|test|testing|donotuse|do not use|unused|placeholder|deleted)\b|^zz|donotuse|takeme)re");
            return bad.Contains(Lower(name));
        }

        // Group ids for grouped populations.
        constexpr std::uint32_t kGroupArrow = 1;
        constexpr std::uint32_t kGroupBolt = 2;
    }

    // =========================================================================
    float Get(const Cap& cap, Col c) noexcept
    {
        for (const auto& v : cap) {
            if (v.col == c) return v.v;
        }
        return 0.0f;
    }

    bool HasEffectColumn(const Cap& cap) noexcept
    {
        return std::any_of(cap.begin(), cap.end(), [](const Value& v) { return IsEffect(v.col); });
    }

    std::string FormatCap(const Cap& cap)
    {
        std::string out;
        char buf[32];
        for (const auto& v : cap) {
            if (!out.empty()) out += ';';
            out += Name(v.col);
            out += '=';
            const auto r = std::to_chars(buf, buf + sizeof(buf), v.v, std::chars_format::general, 4);
            out.append(buf, r.ptr);
        }
        return out;
    }

    bool DescribesItem(const Cap& cap, Kind kind) noexcept
    {
        if (HasEffectColumn(cap)) return true;
        switch (kind) {
            case Kind::Weapon: return Get(cap, Col::weapon_damage) > 0.0f;
            case Kind::Ammo: return Get(cap, Col::ammo_damage) > 0.0f;
            case Kind::Light: return Get(cap, Col::kind_light) > 0.0f;
            case Kind::SoulGem: return Get(cap, Col::soulgem_capacity) > 0.0f;
            default: return false;
        }
    }

    School SchoolFromName(std::string_view s) noexcept
    {
        if (s == "Alteration") return School::Alteration;
        if (s == "Conjuration") return School::Conjuration;
        if (s == "Destruction") return School::Destruction;
        if (s == "Illusion") return School::Illusion;
        if (s == "Restoration") return School::Restoration;
        return School::None;
    }

    std::string_view SchoolName(School s) noexcept
    {
        switch (s) {
            case School::Alteration: return "Alteration";
            case School::Conjuration: return "Conjuration";
            case School::Destruction: return "Destruction";
            case School::Illusion: return "Illusion";
            case School::Restoration: return "Restoration";
            default: return "";
        }
    }

    RowTally& RowTally::operator+=(const RowTally& o) noexcept
    {
        rows += o.rows;
        hidden += o.hidden;
        hiddenKept += o.hiddenKept;
        visible += o.visible;
        helper += o.helper;
        wrapperUnknown += o.wrapperUnknown;
        counted += o.counted;
        mapped += o.mapped;
        return *this;
    }

    std::vector<EffectClass> ClassifyAll(const EffectTable& effects, const OverrideTable* overrides)
    {
        std::vector<EffectClass> out;
        out.reserve(effects.size());
        for (const auto& e : effects) out.push_back(ClassifyEffect(e, overrides));
        return out;
    }

    bool InScope(const ItemRecord& it, const EffectTable& effects)
    {
        if (BadName(it.name)) return false;
        switch (it.kind) {
            case Kind::Spell: {
                if (it.spellType != kSpellTypeSpell) return false;
                if (it.castingType != kCastFireAndForget && it.castingType != kCastConcentration) return false;
                if (!(it.magickaCost < 5000.0f)) return false;
                const bool visible = std::any_of(it.effects.begin(), it.effects.end(), [&](const EffectRow& r) {
                    return r.effect < effects.size() && !effects[r.effect].HiddenInUI();
                });
                if (!visible) return false;
                if (it.taughtByTome) return *it.taughtByTome;
                return it.magickaCost > 0.5f;
            }
            case Kind::Scroll: return it.value > 0;
            case Kind::Potion:
            case Kind::Poison:
            case Kind::Food: return it.playable;
            case Kind::Weapon: return it.playable && it.value > 0 && it.weaponType != kWeaponHandToHand;
            case Kind::Ammo:
            case Kind::Armour: return it.playable && it.value > 0;
            case Kind::SoulGem:
            case Kind::Light: return true;
            default: return false;
        }
    }

    // =========================================================================
    // MapItem
    // =========================================================================
    namespace
    {
        bool HiddenWhitelisted(Col c)
        {
            if (c == Col::stealth_invisibility) return false;  // perk / cloak variants
            switch (FamilyKey(c)) {
                case Col::damage: case Col::absorb: case Col::restore: case Col::fortify_vital: case Col::regen:
                case Col::drain_vital: case Col::weaken_regen: case Col::resist: case Col::weakness: case Col::defense:
                case Col::control: case Col::fortify_skill: case Col::fortify_combat: case Col::weaken_combat:
                case Col::movement: case Col::survival: case Col::cure: case Col::stealth:
                    return true;
                default:
                    return false;
            }
        }

        /// The hidden row's name must not name another family (damage and
        /// absorb count as one).
        bool NameAgrees(const EffectClass& cls, Col c)
        {
            if (cls.nameFamily == Col::_Count) return true;
            const Col a = cls.nameFamily;
            const Col b = FamilyKey(c);
            if (a == b) return true;
            const auto dmgAbs = [](Col x) { return x == Col::damage || x == Col::absorb; };
            return dmgAbs(a) && dmgAbs(b);
        }

        KeptRow MakeRow(const EffectRow& row, const MagicEffectRecord& m, const EffectClass& cls, bool visible,
                        bool constantItem)
        {
            KeptRow k;
            k.col = cls.col;
            k.col2 = cls.col2;
            k.visible = visible;
            k.cureByArchetype = cls.cureByArchetype;
            k.hostile = m.hostile || (m.flags & kFlagHostile) != 0;
            k.delivery = m.delivery;
            k.duration = row.duration;
            k.area = row.area;
            k.school = SchoolFromName(m.school);

            const float mag = std::fabs(row.magnitude);
            const float d = ClipDuration(row.duration);
            switch (RuleOf(cls.col)) {
                case Rule::Amount:
                    k.raw = mag * std::max(d, 1.0f);
                    k.graded = true;
                    if (VitalIndex(cls.col) >= 0 && row.magnitude >= kFullRestore) k.fullRestore = true;
                    break;
                case Rule::Level:
                    k.raw = (constantItem || row.duration == 0) ? mag : mag * d / kDurationCap;
                    k.graded = true;
                    break;
                case Rule::P: k.post = 1.0f; break;
                case Rule::PD: k.post = DurationFactor(row.duration); break;
                case Rule::PG:
                    k.raw = mag;
                    k.graded = true;
                    break;
                case Rule::PGD:
                    k.raw = mag;
                    k.graded = true;
                    k.post = DurationFactor(row.duration);
                    break;
                case Rule::PArea:
                    k.raw = std::max(static_cast<float>(row.area), mag);
                    k.graded = true;
                    break;
                case Rule::Hunger: k.post = HungerSize(m); break;
                case Rule::Thirst: k.post = 0.5f; break;
            }
            // A graded column with no magnitude (a script effect) is presence.
            if (k.graded && !(k.raw > 0.0f) && !k.fullRestore) {
                k.graded = false;
                k.raw = 0.0f;
            }
            return k;
        }
    }

    ItemMapping MapItem(const ItemRecord& it, const EffectTable& effects, const std::vector<EffectClass>& classes)
    {
        ItemMapping m;
        m.kind = it.kind;
        m.formId = it.formId;
        m.inScope = InScope(it, effects);
        m.constantItem = it.kind == Kind::Armour || it.castingType == kCastConstant || it.enchantCastingType == kCastConstant;

        bool hydrated = false;
        bool aura = false;
        bool hazard = false;
        for (const auto& row : it.effects) {
            RowOutcome o;
            ++m.tally.rows;
            if (row.effect >= effects.size()) {
                m.outcomes.push_back(o);
                continue;
            }
            const auto& mg = effects[row.effect];
            const auto& cls = classes[row.effect];
            o.cls = cls;
            o.hidden = mg.HiddenInUI();
            if (cls.hydrated) hydrated = true;

            if (!o.hidden) {
                ++m.tally.visible;
                if (mg.archetype == kArchCloak) aura = true;
                if (mg.archetype == kArchSpawnHazard) hazard = true;
                if (cls.route == Route::Helper) {
                    ++m.tally.helper;
                }
                else if (cls.route == Route::Wrapper) {
                    if (!mg.payload.empty()) {
                        o.counted = true;
                        // The payload spell's effects describe the wrapper. Their
                        // own hidden flag does not matter: the payload is never
                        // shown in the UI by itself.
                        for (const auto& p : mg.payload) {
                            if (p.effect >= effects.size()) continue;
                            const auto& pc = classes[p.effect];
                            if (!pc.Mapped() || pc.route == Route::Helper) continue;
                            m.rows.push_back(MakeRow(p, effects[p.effect], pc, true, m.constantItem));
                            o.mapped = true;
                            o.kept = true;
                        }
                    }
                    else if (cls.wrapperDescription != Col::_Count) {
                        // No payload to read (none, or a dump without the
                        // column): the wrapper's own description, as for any
                        // effect nothing else maps.
                        EffectClass d = cls;
                        d.col = cls.wrapperDescription;
                        d.col2 = Col::_Count;
                        d.route = Route::Description;
                        o.cls = d;
                        o.counted = true;
                        o.mapped = true;
                        o.kept = true;
                        m.rows.push_back(MakeRow(row, mg, d, true, m.constantItem));
                    }
                    else {
                        // A wrapper with nothing to read is not counted, like a
                        // helper: the payload is where its mechanics live.
                        ++m.tally.wrapperUnknown;
                    }
                }
                else {
                    o.counted = true;
                    if (cls.Mapped()) {
                        o.mapped = true;
                        o.kept = true;
                        m.rows.push_back(MakeRow(row, mg, cls, true, m.constantItem));
                    }
                }
            }
            else {
                ++m.tally.hidden;
                if (cls.Mapped() && cls.route != Route::Helper && HiddenWhitelisted(cls.col) &&
                    !cls.helperName && NameAgrees(cls, cls.col)) {
                    o.kept = true;
                    ++m.tally.hiddenKept;
                    m.rows.push_back(MakeRow(row, mg, cls, false, m.constantItem));
                }
            }
            if (o.counted) ++m.tally.counted;
            if (o.mapped) ++m.tally.mapped;
            m.outcomes.push_back(o);
        }

        if (hydrated) {
            for (auto& r : m.rows) {
                if (r.col == Col::survival_thirst) r.post = 1.0f;
            }
        }

        // Restore amounts (absolute) for the overshoot cross-feature.
        {
            const Eligibility eligible(m);
            for (const auto& r : m.rows) {
                const int v = VitalIndex(r.col);
                if (v < 0 || !eligible(r, r.col)) continue;
                if (r.fullRestore) m.fullRestore[static_cast<std::size_t>(v)] = true;
                else m.restoreAmount[static_cast<std::size_t>(v)] = std::max(m.restoreAmount[static_cast<std::size_t>(v)], r.raw);
            }
        }

        // ---- item features, weapon and armour stats -------------------------
        auto fix = [&](Col c, float v) { Set(m.fixed, c, v); };
        const bool isStaff = it.kind == Kind::Weapon && it.weaponType == kWeaponStaff;
        switch (it.kind) {
            case Kind::Spell: fix(Col::kind_spell, 1.0f); break;
            case Kind::Scroll: fix(Col::kind_scroll, 1.0f); break;
            case Kind::Potion: fix(Col::kind_potion, 1.0f); break;
            case Kind::Poison: fix(Col::kind_poison, 1.0f); break;
            case Kind::Food: fix(Col::kind_food, 1.0f); break;
            case Kind::Weapon: fix(isStaff ? Col::kind_staff : Col::kind_weapon, 1.0f); break;
            case Kind::Ammo: fix(Col::kind_ammo, 1.0f); break;
            case Kind::Armour: fix(Col::kind_armour, 1.0f); break;
            case Kind::SoulGem: fix(Col::kind_soulgem, 1.0f); break;
            case Kind::Light: fix(Col::kind_light, 1.0f); break;
            default: break;
        }
        m.consumable = it.kind == Kind::Potion || it.kind == Kind::Poison || it.kind == Kind::Food ||
                       it.kind == Kind::Scroll || it.kind == Kind::Ammo || it.kind == Kind::SoulGem;
        if (m.consumable) fix(Col::consumable, 1.0f);
        m.value = it.value;
        if (it.kind == Kind::Spell) m.magickaCost = it.magickaCost;
        if ((it.kind == Kind::Spell || it.kind == Kind::Scroll) && it.castingType == kCastConcentration) {
            fix(Col::concentration, 1.0f);
        }
        if (aura) fix(Col::aura, 1.0f);
        if (hazard || HasKeyword(it.keywords, "MagicRune") || HasKeyword(it.keywords, "CircleSpell") ||
            HasKeyword(it.keywords, "WallSpell")) {
            fix(Col::ground_area, 1.0f);
        }
        for (const auto& row : it.effects) {
            if (row.effect >= effects.size()) continue;
            const auto& kws = effects[row.effect].keywords;
            if (HasKeyword(kws, "MagicRune") || HasKeyword(kws, "CircleSpell") || HasKeyword(kws, "WallSpell")) {
                fix(Col::ground_area, 1.0f);
            }
        }

        if (it.kind == Kind::Weapon && !isStaff) {
            const Col tc = WeaponTypeColumn(it);
            if (tc != Col::_Count) {
                fix(tc, 1.0f);
                const auto g = static_cast<std::uint32_t>(tc);
                m.grouped.push_back({ Col::weapon_damage, g, it.damage, false, 0, 0 });
                m.grouped.push_back({ Col::weapon_crit, g, it.critDamage, false, 0, 0 });
                m.grouped.push_back({ Col::weapon_speed, g, it.speed, true, 0.5f, 2.0f });
                m.grouped.push_back({ Col::weapon_reach, g, it.reach, true, 0.0f, 0.0f });
            }
            const bool ranged = it.weaponType == kWeaponBow || it.weaponType == kWeaponCrossbow;
            fix(ranged ? Col::weapon_ranged : Col::weapon_melee, 1.0f);
            if (it.twoHanded) fix(Col::weapon_two_handed, 1.0f);
            if (HasKeyword(it.keywords, "WeapMaterialSilver") || HasKeywordPrefix(it.keywords, "DBWR_Silver")) {
                fix(Col::weapon_silver, 1.0f);
            }
            if (HasKeyword(it.keywords, "VendorItemTool") &&
                (Lower(it.name).find("pickaxe") != std::string::npos || HasKeyword(it.keywords, "OCF_CanMineOre") ||
                 HasKeyword(it.keywords, "WeapTypePickaxe"))) {
                fix(Col::weapon_pickaxe, 1.0f);
            }
        }
        if (it.kind == Kind::Weapon && it.enchanted) fix(Col::weapon_enchanted, 1.0f);
        if (it.kind == Kind::Ammo) {
            const bool bolt = IsBolt(it);
            fix(bolt ? Col::ammo_bolt : Col::ammo_arrow, 1.0f);
            m.grouped.push_back({ Col::ammo_damage, bolt ? kGroupBolt : kGroupArrow, it.damage, false, 0, 0 });
        }
        if (it.kind == Kind::Armour) {
            const Col sc = SlotColumn(it.slotMask);
            if (sc != Col::_Count) fix(sc, 1.0f);
            const Col wc = it.armourWeight == ArmourWeight::Heavy ? Col::armour_weight_heavy
                           : it.armourWeight == ArmourWeight::Light ? Col::armour_weight_light
                                                                    : Col::armour_weight_clothing;
            fix(wc, 1.0f);
            const auto g = static_cast<std::uint32_t>(Index(sc)) * 4u + static_cast<std::uint32_t>(it.armourWeight);
            if (it.armorRating > 0.0f) m.grouped.push_back({ Col::armour_rating, g, it.armorRating, false, 0, 0 });
            if (it.enchanted) fix(Col::armour_enchanted, 1.0f);
            if (HasKeyword(it.keywords, "Survival_ArmorWarm") || HasKeyword(it.keywords, "MAG_SurvivalArmorWarm")) {
                fix(Col::armour_warm, 1.0f);
            }
            if (HasKeyword(it.keywords, "Survival_ArmorCold") || HasKeyword(it.keywords, "MAG_SurvivalArmorCold")) {
                fix(Col::armour_cold, 1.0f);
            }
        }
        if (it.kind == Kind::SoulGem) {
            fix(Col::soulgem_capacity, std::clamp(static_cast<float>(it.soulCapacity) / 5.0f, 0.0f, 1.0f));
            fix(Col::soulgem_charge, std::clamp(static_cast<float>(it.soulContained) / 5.0f, 0.0f, 1.0f));
        }
        if (it.kind == Kind::Light && it.lightRadius > 0) {
            fix(Col::light_radius,
                std::min(1.0f, static_cast<float>(std::log1p(it.lightRadius) / std::log1p(1024.0))));
        }
        return m;
    }

    // =========================================================================
    // Populations
    // =========================================================================
    std::vector<std::pair<Col, float>> Populations::ColumnRaws(const ItemMapping& m)
    {
        std::array<float, kColumnCount> best{};
        const Eligibility eligible(m);
        for (const auto& r : m.rows) {
            if (!r.graded || r.fullRestore || !(r.raw > 0.0f)) continue;
            for (const Col c : { r.col, r.col2 }) {
                if (!eligible(r, c)) continue;
                best[Index(c)] = std::max(best[Index(c)], r.raw);
            }
        }
        std::vector<std::pair<Col, float>> out;
        for (std::size_t i = 0; i < kColumnCount; ++i) {
            if (best[i] > 0.0f) out.emplace_back(static_cast<Col>(i), best[i]);
        }
        return out;
    }

    void Populations::Add(const ItemMapping& m)
    {
        if (!m.inScope) return;
        for (const auto& [c, v] : ColumnRaws(m)) pops_[Key(c, 0)].push_back(v);
        for (const auto& g : m.grouped) pops_[Key(g.col, g.group)].push_back(g.raw);
        if (m.kind == Kind::Spell && m.magickaCost > 0.0f) pops_[Key(Col::magicka_cost, 0)].push_back(m.magickaCost);
        if (m.consumable) pops_[Key(Col::gold_value, static_cast<std::uint32_t>(m.kind))].push_back(static_cast<float>(m.value));
        finalized_ = false;
    }

    void Populations::Finalize()
    {
        for (auto& [k, v] : pops_) std::sort(v.begin(), v.end());
        finalized_ = true;
    }

    float Populations::Percentile(Col col, std::uint32_t group, float x) const
    {
        const auto it = pops_.find(Key(col, group));
        if (it == pops_.end() || it->second.empty()) return 1.0f;
        const auto& v = it->second;
        const auto n = static_cast<float>(std::upper_bound(v.begin(), v.end(), x) - v.begin());
        // An instance value below the whole population (a value no catalog
        // item has) still reads as present.
        return std::max(n, 1.0f) / static_cast<float>(v.size());
    }

    float Populations::Median(Col col, std::uint32_t group) const
    {
        const auto it = pops_.find(Key(col, group));
        if (it == pops_.end() || it->second.empty()) return 0.0f;
        const auto& v = it->second;
        const std::size_t n = v.size();
        return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    }

    std::size_t Populations::Size(Col col, std::uint32_t group) const
    {
        const auto it = pops_.find(Key(col, group));
        return it == pops_.end() ? 0 : it->second.size();
    }

    // =========================================================================
    // Grade
    // =========================================================================
    namespace
    {
        /// Index of the primary kept row (largest graded value; first on a tie),
        /// or -1.
        int PrimaryRow(const ItemMapping& m, const Populations& pops)
        {
            const Eligibility eligible(m);
            int best = -1;
            float bestV = -1.0f;
            for (std::size_t i = 0; i < m.rows.size(); ++i) {
                const auto& r = m.rows[i];
                if (!eligible(r, r.col)) continue;
                const float v = RowValue(r, r.col, pops);
                if (v > bestV) {
                    bestV = v;
                    best = static_cast<int>(i);
                }
            }
            return best;
        }
    }

    School PrimarySchool(const ItemMapping& m, const Populations& pops)
    {
        const int p = PrimaryRow(m, pops);
        return p >= 0 ? m.rows[static_cast<std::size_t>(p)].school : School::None;
    }

    Cap Grade(const ItemMapping& m, const Populations& pops)
    {
        std::array<float, kColumnCount> val{};
        const Eligibility eligible(m);
        auto put = [&](Col c, float v) {
            if (c == Col::_Count) return;
            val[Index(c)] = std::max(val[Index(c)], v);
        };

        bool anyHostile = false;
        bool anyFullRestore = false;
        bool anyCureArchetype = false;
        std::uint32_t maxArea = 0;
        std::uint32_t maxDur = 0;
        bool longLasting = false;
        bool healsOthers = false;
        for (const auto& r : m.rows) {
            for (const Col c : { r.col, r.col2 }) {
                // Any kept heal that reaches others, visible or not: Breath of
                // Life's visible row heals the caster and a hidden twin with
                // an area heals the allies around.
                if (c == Col::restore_health && (r.delivery != kDeliverySelf || r.area > 0)) healsOthers = true;
                if (!eligible(r, c)) continue;
                put(c, RowValue(r, c, pops));
            }
            anyHostile = anyHostile || r.hostile;
            anyFullRestore = anyFullRestore || r.fullRestore;
            if (FamilyKey(r.col) == Col::cure && r.cureByArchetype) anyCureArchetype = true;
            maxArea = std::max(maxArea, r.area);
            maxDur = std::max(maxDur, r.duration);
            if (r.duration >= kDurationSentinel) longLasting = true;
        }
        // restore_health_other: a conjunctive column, valued as restore_health.
        if (healsOthers) put(Col::restore_health_other, val[Index(Col::restore_health)]);
        // Families: max over their specifics.
        for (std::size_t i = 0; i < kColumnCount; ++i) {
            if (val[i] <= 0.0f) continue;
            const Col fam = FamilyOf(static_cast<Col>(i));
            if (fam != Col::_Count) val[Index(fam)] = std::max(val[Index(fam)], val[i]);
        }

        const int primary = PrimaryRow(m, pops);
        if (primary >= 0) {
            const auto& p = m.rows[static_cast<std::size_t>(primary)];
            switch (p.delivery) {
                case kDeliverySelf: put(Col::delivery_self, 1.0f); break;
                case kDeliveryTouch: put(Col::delivery_touch, 1.0f); break;
                case kDeliveryAimed:
                case kDeliveryTargetActor: put(Col::delivery_aimed, 1.0f); break;
                case kDeliveryTargetLocation: put(Col::delivery_target_location, 1.0f); break;
                default: break;
            }
            if (m.constantItem) put(Col::timing_constant, 1.0f);
            else if (p.duration > 0) put(Col::timing_over_time, 1.0f);
            else put(Col::timing_instant, 1.0f);
            if (m.kind == Kind::Spell || m.kind == Kind::Scroll ||
                (m.kind == Kind::Weapon && Get(m.fixed, Col::kind_staff) > 0.0f)) {
                switch (p.school) {
                    case School::Alteration: put(Col::school_alteration, 1.0f); break;
                    case School::Conjuration: put(Col::school_conjuration, 1.0f); break;
                    case School::Destruction: put(Col::school_destruction, 1.0f); break;
                    case School::Illusion: put(Col::school_illusion, 1.0f); break;
                    case School::Restoration: put(Col::school_restoration, 1.0f); break;
                    default: break;
                }
            }
        }
        if (!m.rows.empty()) {
            if (maxArea > 0) put(Col::area, std::min(1.0f, static_cast<float>(std::log1p(maxArea) / std::log1p(1000.0))));
            if (anyHostile) put(Col::hostile, 1.0f);
            if (maxDur > 0) put(Col::duration, DurationFactor(maxDur));
            if (longLasting) put(Col::long_lasting, 1.0f);
            if (anyFullRestore) put(Col::full_restore, 1.0f);
            if (val[Index(Col::cure)] > 0.0f && anyCureArchetype) put(Col::cure_instant, 1.0f);
        }

        for (const auto& f : m.fixed) put(f.col, f.v);
        for (const auto& g : m.grouped) {
            if (g.ratioToMedian) {
                const float med = pops.Median(g.col, g.group);
                if (med > 0.0f && g.raw > 0.0f) {
                    float r = g.raw / med;
                    if (g.hi > 0.0f) r = std::clamp(r, g.lo, g.hi);
                    put(g.col, r);
                }
            }
            else if (g.raw > 0.0f) {
                put(g.col, pops.Percentile(g.col, g.group, g.raw));
            }
        }
        if (m.kind == Kind::Spell && m.magickaCost > 0.0f) {
            put(Col::magicka_cost, pops.Percentile(Col::magicka_cost, 0, m.magickaCost));
        }
        if (m.consumable) {
            put(Col::gold_value, pops.Percentile(Col::gold_value, static_cast<std::uint32_t>(m.kind),
                                                 static_cast<float>(m.value)));
        }

        Cap cap;
        for (std::size_t i = 0; i < kColumnCount; ++i) {
            if (val[i] > 0.0f) cap.push_back(Value{ static_cast<Col>(i), val[i] });
        }
        return cap;
    }

    BuildResult BuildCaps(const std::vector<ItemRecord>& items, const EffectTable& effects,
                          const OverrideTable* overrides)
    {
        BuildResult out;
        out.classes = ClassifyAll(effects, overrides);
        out.mappings.reserve(items.size());
        for (const auto& it : items) {
            out.mappings.push_back(MapItem(it, effects, out.classes));
            const auto& m = out.mappings.back();
            if (m.inScope) {
                out.pops.Add(m);
                out.tally += m.tally;
            }
        }
        out.pops.Finalize();
        out.caps.resize(out.mappings.size());
        for (std::size_t i = 0; i < out.mappings.size(); ++i) {
            if (out.mappings[i].inScope) out.caps[i] = Grade(out.mappings[i], out.pops);
        }
        return out;
    }
}
