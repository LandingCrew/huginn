#pragma once

// =============================================================================
// EFFECT MAPPER -- plain item records -> cap(i), the 239 effect columns
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
//     "P x D", times a graded magnitude where it says "P x G". A graded column
//     with no magnitude has an unknown strength: through a script route
//     (keyword, name, description, override) it is kNeutralStrength = 0.5
//     (times D where the rule has one), never the top; through the engine's
//     own data it is resolved per ResolveZeroMagnitudes below (a stated
//     percentage is its own value, an unknown one 0.5). Presence-only (P,
//     PD) columns stay 1 (x D). Durations of a day or
//     more are sentinels: clipped to 3600, and `long_lasting` set. A restore
//     of 9999+ is a full restore: value 1, `full_restore` set.
//   - Families are the max over their specifics.
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

        /// The values one item contributes to a column's population (its max
        /// graded raw over the rows it keeps for that column).
        [[nodiscard]] static std::vector<std::pair<Col, float>> ColumnRaws(const ItemMapping& m);

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
