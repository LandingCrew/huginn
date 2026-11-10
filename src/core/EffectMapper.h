#pragma once

// =============================================================================
// EFFECT MAPPER -- plain item records -> cap(i), the 244 effect columns
// =============================================================================
// Doc 9's extractor rules (effects.csv `how`), in three steps:
//
//   1. ClassifyAll: every magic effect -> its column (core/EffectRules.h),
//      once per MGEF.
//   2. MapItem: one item -> its kept effect rows and item features, still
//      ungraded, plus the coverage tally of its effect rows.
//   3. Populations + Grade: graded columns become percentiles within the load
//      order (doc 9 rule 8), so lists whose magnitudes differ ~5x compare.
//
// BuildCaps runs all three over a whole load order. The game's EffectCatalog
// (src/effect/) and the host tools call the same functions.
//
// Rules worth knowing (effects.csv has the full list):
//   - Scope (rule 1): plain spells taught by a tome (fire-and-forget or
//     concentration, a visible effect, cost < 5000); scrolls with a value;
//     playable potions, poisons and food; playable weapons, ammo and armour
//     with a value (no hand-to-hand); soul gems; carried lights. Powers,
//     shouts, abilities and ingredients are out. A dump without the tome
//     column falls back to "magicka cost > 0.5".
//   - Rows (rule 2): visible effects, plus hidden effects of a whitelisted
//     family whose name agrees with their column (frost's hidden Slow, LoreRim's
//     hidden Stagger). Per item and column a visible row beats a hidden one,
//     and rows of one column combine by max.
//   - Values: `Amount` columns (restore, damage, absorb) grade the total
//     magnitude x max(duration, 1); `Level` columns (fortify, resist, regen,
//     defense ...) grade magnitude x min(duration, 3600)/3600 for timed effects
//     and the magnitude for constant ones; presence columns are 1, times
//     D(duration) = log1p(min(d,3600))/log1p(3600) where effects.csv says
//     "P x D", times a graded magnitude where it says "P x G". A timed Level
//     row grades within its column's timed population, apart from the
//     constant ones (enchanted gear, constant abilities; 0.24.27): weighed by
//     duration/3600 against a ring's constant magnitude, a 60 s Resist Fire
//     potion read about 0.08 and never outranked anything, so a timed effect
//     is ranked among timed effects (potions, spells, scrolls, food). A row
//     of an hour or more already reads its magnitude, gear's scale, so it
//     stays with gear; a column with fewer than kMinTimedGroup timed items
//     grades its timed rows against gear, as before (a crafted potion or a
//     perk row is graded against a population it is not in). A graded
//     column with no magnitude has an unknown strength: through a script route
//     (keyword, name, description, override) it is kNeutralStrength = 0.5
//     (times D where the rule has one), never the top; through the engine's
//     own data it is resolved per ResolveZeroMagnitudes below (a stated
//     percentage is its own value, an unknown one 0.5). Presence-only (P,
//     PD) columns stay 1 (x D). Durations of a day or
//     more are sentinels: clipped to 3600, and `long_lasting` set. A restore
//     of 9999+ is a full restore: value 1, `full_restore` set.
//   - Conditions (0.23.30, core/EffectConditions.h): a row whose conditions
//     need a perk of the player is kept beside cap(i), not in it
//     (ItemMapping::perkRows, graded by GradePerkRows; WithPerks adds the ones
//     a player meets); a GetRandomPercent proc grades its expected strength
//     (the chance times its magnitude, or times its presence value); a row
//     that fires only on a target family and lands on others is a bane row,
//     dropped from cap(i) and the populations unless its column names its
//     target (turn undead, banish, sun). Rows left out still give an item that
//     keeps nothing else its delivery, timing, school, area and duration.
//   - A Cloak's or hazard's payload is graded over the wrapper's duration
//     (max of the two; 0.23.30).
//   - Weapon stats grade within the OCF sub-type (OCF_WeapTypeQuarterstaff2H
//     ...), the vanilla type the fallback below kMinSubtypeGroup (0.23.30).
//   - Families are the max over their specifics.
//   - Side effects (0.23.16): a harm row (damage, drain, weakness, control
//     ...) on a food or potion is detrimental or hostile to the item's own
//     user, so it sets self_harm_<vital> (a vital's damage or drain, graded
//     as an Amount within its own population) or the family self_harm (any
//     other harm), never the target-harm column (core/EffectRules.h,
//     SelfHarmColumn). Such a row, whatever column it keeps (survival_
//     intoxication's "Drugged" too), does not set `hostile`; a self_harm*
//     row is the primary row (delivery, timing) only when the item keeps
//     nothing else. A Cloak/hazard's payload hits others: no such rule there.
//
// Coverage (doc 9's "Mapped" figure): over in-scope items, the share of
// visible effect rows that map to a column, not counting helper rows (visual,
// dummy ...) and Cloak/hazard wrappers with nothing to read. A wrapper with a
// payload (dump gap 3) counts, and maps when its payload does; one without a
// payload (none, or a dump that predates the column) counts and maps when its
// own description maps, and is left out otherwise.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/EffectColumns.h"
#include "core/EffectRecords.h"
#include "core/EffectRules.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Huginn::Core::Effect
{
    /// One nonzero column of cap(i).
    struct Value
    {
        Col col = Col::_Count;
        float v = 0.0f;
    };

    /// cap(i): the nonzero columns, sorted by column.
    using Cap = std::vector<Value>;

    [[nodiscard]] float Get(const Cap& cap, Col c) noexcept;
    [[nodiscard]] bool HasEffectColumn(const Cap& cap) noexcept;

    /// "col=value;col=value" (4 significant digits), the dumps' and the
    /// console's form. Game and host print through this, so caps compare as text.
    [[nodiscard]] std::string FormatCap(const Cap& cap);

    /// A tempered weapon's damage for grading against base forms: tempering
    /// ADDS, it does not multiply (src/weapon/WeaponData.h: LoreRim's (1.2) is
    /// +2 points). The +(h - 1) x 10 rule is EXTRAPOLATED from that one
    /// measurement at h = 1.2 (the vanilla smithing tiers fit it too, unmeasured
    /// here); WeaponData::BestDamage still falls back to base x temper. The
    /// game's displayed damage (PlayerCharacter::GetDamage, which the registry
    /// reads) is not used: it adds the player's skill and perks, so it does not
    /// compare with the base-form population. h <= 1 (or an unfilled 0) is
    /// untempered.
    [[nodiscard]] constexpr float TemperedWeaponDamage(float baseDamage, float temper) noexcept
    {
        return temper > 1.0f ? baseDamage + (temper - 1.0f) * 10.0f : baseDamage;
    }

    /// The coverage diff's test: does cap(i) say what the item does? An effect
    /// column for anything magic; for a plain weapon or ammo its damage stat;
    /// for a light, being a light.
    [[nodiscard]] bool DescribesItem(const Cap& cap, Kind kind) noexcept;

    /// The `narcotic` item feature (effects.csv; 0.24.9 as `alcohol`, 0.24.16
    /// renamed): a potion or food that is an alcoholic drink or a drug, one
    /// group (the user, 2026-10-30). Never a poison. Marks, any one of them: an
    /// alcohol or drug item keyword (OCF, LoreRim - Food Tweaks' Alcohol*,
    /// Simonrim's MAG_FoodTypeDrugs, CACO's), an effect whose MGEF carries an
    /// alcohol or drug keyword (Requiem's Feat_KW_Alcohol, BOOB's
    /// BOOB_AlcoholBadEffect, Simonrim's MAG_ ones), or an effect that is a
    /// drink or drug MGEF by plugin and local id: the drink drawback
    /// Skyrim.esm 10C645, vanilla's Drugged (0E3CBB) and Redwater Skooma
    /// effect, Requiem's two Skooma effects, WindhelmSSE's moonshine.
    /// MagicAlchHarmful is not one: poisoned food and Rendil's Screaming
    /// Cheese carry it too.
    [[nodiscard]] bool IsNarcotic(const ItemRecord& item, const EffectTable& effects);

    /// The column's value rule from effects.csv's `how`: "Amount", "Level",
    /// "P", "PD", "PG", "PGD", "PArea", "Hunger" or "Thirst" (graded ones grade
    /// a magnitude). For the report tool and tests.
    [[nodiscard]] std::string_view ValueRuleName(Col c);

    enum class School : std::uint8_t { None, Alteration, Conjuration, Destruction, Illusion, Restoration };
    [[nodiscard]] School SchoolFromName(std::string_view associatedSkill) noexcept;
    [[nodiscard]] std::string_view SchoolName(School s) noexcept;

    /// What happened to one effect row of an item (dump view, coverage).
    struct RowOutcome
    {
        EffectClass cls;
        bool hidden = false;
        bool kept = false;     // contributes to cap(i)
        bool counted = false;  // in the coverage denominator
        bool mapped = false;   // counted and set a column (directly or through its payload)
        bool carrier = false;  // a companion row with nothing in it: not kept, not counted
        // 0.23.30 (core/EffectConditions.h): the row's conditions keep it out
        // of cap(i) -- `perk`: it fires only with a perk (carried in
        // ItemMapping::perkRows); `bane`: only on a target family (dropped
        // until the bane columns exist). `chance`: its GetRandomPercent share,
        // already applied to its value.
        bool perk = false;
        bool bane = false;
        float chance = 1.0f;
    };

    struct RowTally
    {
        int rows = 0;
        int hidden = 0;
        int hiddenKept = 0;
        int visible = 0;
        int helper = 0;           // visible helper rows (not counted)
        int wrapperUnknown = 0;   // visible wrappers with no payload and no description to read (not counted)
        int carrier = 0;          // visible companion rows dropped as carriers (not counted): an engine
                                  // effect of unknown strength, no description, beside a kept row
        int counted = 0;
        int mapped = 0;

        RowTally& operator+=(const RowTally& o) noexcept;
    };

    /// A row that contributes to cap(i), before grading.
    struct KeptRow
    {
        Col col = Col::_Count;
        Col col2 = Col::_Count;
        float raw = 0.0f;   // the quantity graded into a percentile (graded rules)
        float post = 1.0f;  // a factor applied after grading (D(duration)), or the value itself
        bool graded = false;
        bool fullRestore = false;
        bool visible = true;
        bool cureByArchetype = false;
        bool unknown = false;   // strength unknown (kNeutralStrength): yields to a known row on the column
        bool timed = false;     // a Level row graded on magnitude x duration share: its own population (0.24.27)
        bool hostile = false;
        int delivery = 0;
        std::uint32_t duration = 0;  // as recorded (sentinels included)
        std::uint32_t area = 0;
        School school = School::None;
    };

    /// An item feature graded within a group (weapon damage within its type ...).
    struct GroupedFeature
    {
        Col col = Col::_Count;
        std::uint32_t group = 0;
        float raw = 0.0f;
        bool ratioToMedian = false;  // value = raw / median(group), clipped [lo, hi]
        float lo = 0.0f;
        float hi = 0.0f;
        // 0.23.30: a weapon sub-type's group (a quarterstaff among
        // quarterstaffs) falls back to its vanilla type's when the sub-type
        // has fewer than kMinSubtypeGroup members in the load order; the
        // fallback population holds every weapon of the type.
        std::uint32_t fallback = 0;
        bool hasFallback = false;
    };

    /// The smallest weapon sub-type population graded on its own: under it a
    /// percentile moves in steps of more than 5%, so the sub-type is graded
    /// within its vanilla type instead (whose population holds it too).
    inline constexpr std::size_t kMinSubtypeGroup = 20;

    /// The smallest timed population a timed Level row grades in. Under it
    /// the row grades against the column's constant population (gear), as
    /// it did before 0.24.27: an empty or one-item population would read 1.0
    /// for any row graded against it but not in it (a crafted potion, a perk
    /// row). LoreRim's resist columns hold about 20 timed items and the
    /// skill fortifies 4, so the bar is low on purpose.
    inline constexpr std::size_t kMinTimedGroup = 3;

    /// A kept row that fires only with a perk (core/EffectConditions.h):
    /// carried beside cap(i), not in it, so the catalog -- built at the main
    /// menu, before there is a player -- stays the same for every character.
    struct PerkRow
    {
        KeptRow row;
        std::vector<std::vector<std::uint32_t>> perks;  // every clause needs one of its perks
    };

    /// A perk row graded against the column's population (which it is not in).
    struct PerkValue
    {
        std::vector<std::vector<std::uint32_t>> perks;
        Col col = Col::_Count;
        float v = 0.0f;
    };

    /// One item, mapped but not yet graded.
    struct ItemMapping
    {
        Kind kind = Kind::Spell;
        std::uint32_t formId = 0;
        bool inScope = false;
        std::vector<KeptRow> rows;
        std::vector<RowOutcome> outcomes;  // per direct effect row, in item order
        RowTally tally;
        Cap fixed;                          // columns known without a population
        std::vector<GroupedFeature> grouped;
        bool constantItem = false;
        float magickaCost = 0.0f;           // spells: graded within spells
        int value = 0;                      // consumables: graded within kind
        bool consumable = false;
        // Absolute restore amounts (overshoot_*), not graded: total restored
        // per vital, and whether a row is a full-restore sentinel.
        std::array<float, 3> restoreAmount{};
        std::array<bool, 3> fullRestore{};
        // 0.23.30: rows that fire only with a perk (beside cap(i), graded by
        // GradePerkRows), and the perk and bane rows themselves, from which an
        // item that keeps no row at all still takes its delivery, timing,
        // school, area, duration and hostility.
        std::vector<PerkRow> perkRows;
        std::vector<KeptRow> factRows;
        // 0.24.30 (core/LockReach.h): the highest lock level the item's kept
        // unlock rows open, read from their text; Lock::kUnknownReach when it
        // keeps none or one of them names no level (never dropped for a lock).
        int unlockReach = -1;
    };

    /// The class of every effect in the table (index-aligned).
    [[nodiscard]] std::vector<EffectClass> ClassifyAll(const EffectTable& effects, const OverrideTable* overrides);

    /// A graded column with an unknown strength, neither the top nor the
    /// bottom of its population: the median's percentile, by definition 0.5.
    /// Such a row counts for its column only when the item has no row of
    /// known strength there (a "Restore Health" of unknown amount does not lift
    /// a known weaker heal); for the item's primary row (delivery, timing,
    /// school) it ranks at its presence value, so the neutral factor does not
    /// hand those to a lesser row.
    inline constexpr float kNeutralStrength = 0.5f;

    /// For every data-route effect some item uses with its strength unknown
    /// (magnitude 0 on a graded column; a Light with no light radius): read
    /// what its description and name say (EffectClass::zero*). MapItem then,
    /// in order:
    ///   1. the description hits a no-column rule ("Restoration spells are 25%
    ///      more powerful"): counted, unmapped;
    ///   2. the description names a column: kept under it. A plain stated
    ///      number is the magnitude; a percentage ("ignores <80>% of physical
    ///      damage") is the value p/100 itself, outside the population -- an
    ///      APPROXIMATION: on a percent-native column (resist_*) p/100 is
    ///      close to its scale, on a points column such as defense_armor it
    ///      only ranks the effect (Dragonhide's 80% at 0.8), it is not a
    ///      percentile of the points; no number is kNeutralStrength;
    ///   3. the name says the row's own family ("Restore Health" whose amount a
    ///      script carries): kept, kNeutralStrength;
    ///   4. a visible row with no description next to a row the item keeps is
    ///      a carrier (Requiem's "Dispel Armor"): dropped and not counted;
    ///   5. anything else is counted as unmapped (Mehrunes' Razor's "Instant
    ///      Kill", Soul Gem Evocation, Wabbajack).
    void ResolveZeroMagnitudes(const std::vector<ItemRecord>& items, const EffectTable& effects,
                               std::vector<EffectClass>& classes);

    /// Rule 1: a player-facing item Huginn describes.
    [[nodiscard]] bool InScope(const ItemRecord& item, const EffectTable& effects);

    /// Rule 1's first tests for a spell, from its type and casting type alone:
    /// a plain spell, cast fire-and-forget or concentration. False means
    /// InScope is false whatever the rest of the record says -- an ability, a
    /// power, a disease -- so a caller can rule it out without reading it.
    [[nodiscard]] constexpr bool SpellShapeInScope(int spellType, int castingType) noexcept
    {
        return spellType == kSpellTypeSpell &&
               (castingType == kCastFireAndForget || castingType == kCastConcentration);
    }

    [[nodiscard]] ItemMapping MapItem(const ItemRecord& item, const EffectTable& effects,
                                      const std::vector<EffectClass>& classes);

    /// The load order's distributions, for percentiles and medians.
    class Populations
    {
    public:
        void Add(const ItemMapping& m);
        void Finalize();

        /// Share of the population at or below `x`, in (0, 1]; 1 if the
        /// population is empty.
        [[nodiscard]] float Percentile(Col col, std::uint32_t group, float x) const;
        [[nodiscard]] float Median(Col col, std::uint32_t group) const;
        [[nodiscard]] std::size_t Size(Col col, std::uint32_t group) const;

        /// One value an item contributes to a column's population: its max
        /// graded raw over the rows it keeps for that column, per population
        /// (group 0, or the column's timed population for timed Level rows).
        struct ColumnRaw
        {
            Col col = Col::_Count;
            std::uint32_t group = 0;
            float raw = 0.0f;
        };
        [[nodiscard]] static std::vector<ColumnRaw> ColumnRaws(const ItemMapping& m);

        /// The population a kept row belongs to: the timed one for a timed
        /// Level row (KeptRow::timed, 0.24.27), group 0 otherwise.
        [[nodiscard]] static std::uint32_t RowGroup(const KeptRow& r) noexcept;
        /// The population a kept row grades in on column `c`: its RowGroup,
        /// except a timed row on a column whose timed population is under
        /// kMinTimedGroup, which grades in group 0.
        [[nodiscard]] std::uint32_t GradeGroup(Col c, const KeptRow& r) const;
        /// Must not collide with the grouped populations' ids in
        /// EffectMapper.cpp (static_assert there).
        static constexpr std::uint32_t kGroupTimed = 0x20000000u;

    private:
        static std::uint64_t Key(Col c, std::uint32_t g) noexcept
        {
            return (static_cast<std::uint64_t>(c) << 32) | g;
        }
        std::map<std::uint64_t, std::vector<float>> pops_;
        bool finalized_ = false;
    };

    /// The graded cap of one mapped item.
    [[nodiscard]] Cap Grade(const ItemMapping& m, const Populations& pops);

    /// The item's school (spells, scrolls, staves): its primary kept row's.
    [[nodiscard]] School PrimarySchool(const ItemMapping& m, const Populations& pops);

    /// The item's perk rows, each column graded against the load order's
    /// population for it (0.23.30).
    [[nodiscard]] std::vector<PerkValue> GradePerkRows(const ItemMapping& m, const Populations& pops);

    /// Does a player with these perks meet every clause (each needs one of
    /// its perks; a perk printed without a FormID, 0, never matches)?
    [[nodiscard]] bool PerksMet(const std::vector<std::vector<std::uint32_t>>& perks,
                                const std::function<bool(std::uint32_t)>& hasPerk);

    /// cap(i) with the perk rows the player meets: each raises its column (and
    /// the column's family) to its value where that is higher. The runtime
    /// half of the perk rows (effect/CrossFeatures.h PerkAdjustedCap).
    [[nodiscard]] Cap WithPerks(const Cap& cap, const std::vector<PerkValue>& rows,
                                const std::function<bool(std::uint32_t)>& hasPerk);

    struct BuildResult
    {
        std::vector<EffectClass> classes;      // per effect
        std::vector<ItemMapping> mappings;     // per item (all items, in input order)
        std::vector<Cap> caps;                 // per item; empty for an item out of scope
        Populations pops;
        RowTally tally;                        // over in-scope items
        [[nodiscard]] double Coverage() const noexcept
        {
            return tally.counted > 0 ? static_cast<double>(tally.mapped) / tally.counted : 1.0;
        }
    };

    /// Steps 1-3 over a load order.
    [[nodiscard]] BuildResult BuildCaps(const std::vector<ItemRecord>& items, const EffectTable& effects,
                                        const OverrideTable* overrides);
}
