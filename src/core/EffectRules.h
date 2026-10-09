#pragma once

// =============================================================================
// EFFECT RULES -- one magic effect (MGEF) -> the effect column it sets
// =============================================================================
// The per-effect half of the mapper. An effect's column depends only on the
// MGEF (archetype, actor values, flags, keywords, name, description), never on
// the item, so the catalog classifies each MGEF once and caches it.
//
// Layers, first match wins (doc 9, "Reused actor values are resolved in
// layers", and effects.csv's `how` column):
//   0. Override table: per load order, keyed by plugin + local FormID. Checked
//      first, so an explicit entry can also correct a keyword misroute (doc 9
//      lists it second; see the implementation map, Phase 1).
//   For effects whose archetype is Script, or whose actor value mods reuse
//   (`*SkillAdvance`, Fame, Infamy, Mood, Morality, Variable##, VoicePoints,
//   ...), never trusted alone:
//     1. helper names (dummy, visual, cooldown ...) -> Helper, not an effect;
//     2. the keyword table (editor IDs; language-independent);
//     3. the English name table;
//     4. effect-description patterns (only for what nothing else maps);
//     5. unmapped.
//   For engine archetypes: the archetype and actor value decide (route Data),
//   with a name veto when a vital/armour actor value carries another mechanic
//   (LoreRim's Turn Undead on Health, Deep Freeze, Simonrim's Silence).
//   Cloak and SpawnHazard effects are wrappers: their payload spell is mapped
//   instead (core/EffectMapper.cpp), unless the wrapper names an element.
//
// Ported from the Python reference extractor that measured doc 9's coverage
// (scratch `effects-v2/scripts/cap.py` + `desc.py`, 2026-10-07), with the
// column names of effects.csv and these deliberate changes (from a note in
// effects.csv where one exists; tools/effects/make_fixtures.py applies the
// same list to the reference when it writes the test expectations):
//   - "ethereal" names -> defense_ethereal (was utility_slow_time);
//   - "<race> Polymorph" / "Shapeshift: <race>" -> stealth_disguise (was
//     transform_werewolf);
//   - "^fortify (health|magicka|stamina)\b" matches again: the reference had a
//     stray backspace byte where the \b should be, so it never matched;
//   - a bare "Polymorph" (LoreRim's hostile one) no longer matches a name rule:
//     the description route decides (no effects.csv note);
//   - "vampire form" -> transform_vampire_lord (was transform_werewolf; no
//     effects.csv note);
//   - Simonrim Fortify Potion Duration (AlchemySkillAdvance with
//     MagicEnchFortifyAlchemy) -> meta_potion_duration;
//   - Simonrim Fortify Security (PickPocketSkillAdvance) sets lockpicking AND
//     pickpocket;
//   - keyword MagicRestoreMagicka/MagicRestoreStamina restore like the Alch ones;
//   - "damage magicka"/"damage stamina" names are matched before the generic
//     damage-health name rule (which used to swallow them);
//   - an elemental summon (archetype SummonCreature + MagicSummonFire/Frost/
//     Shock) sets summon_creature_<element> and summon_creature;
//   - the regen name rule also takes the misspelt "Regneration" (Simonrim);
//   - a "damage" name maps to damage only on a detrimental effect (script
//     effects exempt) and never with "resist" in it (effects.csv lines 45-46:
//     Simonrim's beneficial "Resist Magicka/Stamina Damage");
//   - a Cloak/SpawnHazard is damage itself only by a MagicDamage* keyword, not
//     by its resisted actor value (Simonrim's Whirlwind Cloak carries
//     FrostResist): otherwise its payload, or failing that its description,
//     decides (core/EffectMapper.cpp);
//   - helper names no longer swallow "Blank Slate" (only a bare "Blank"), and
//     Requiem's "Dispel Soul Gems" is neither a helper nor a dispel (unmapped
//     by name).
//   Spec names with no column of their own fall back to their family column
//   (weaken_combat_crit -> weaken_combat; Dragonrend -> shout; a detrimental
//   warmth effect -> survival; Cure Addiction -> cure; weakness to disease ->
//   weakness).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/EffectColumns.h"
#include "core/EffectRecords.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Huginn::Core::Effect
{
    enum class Route : std::uint8_t
    {
        Data,         // archetype + actor value
        Keyword,      // keyword table
        Override,     // per-load-order override table
        Name,         // English name table (or a name veto on a data row)
        Description,  // effect-description patterns
        Helper,       // a helper effect (visual, dummy, cooldown...): not an effect
        Wrapper,      // Cloak/SpawnHazard with no element of its own: see its payload
        Unmapped
    };

    [[nodiscard]] constexpr std::string_view RouteName(Route r) noexcept
    {
        switch (r) {
            case Route::Data: return "data";
            case Route::Keyword: return "keyword";
            case Route::Override: return "override";
            case Route::Name: return "name";
            case Route::Description: return "desc";
            case Route::Helper: return "helper";
            case Route::Wrapper: return "wrapper";
            case Route::Unmapped: return "unmapped";
        }
        return "";
    }

    struct EffectClass
    {
        Col col = Col::_Count;   // the column the effect sets (a specific, or a family)
        Col col2 = Col::_Count;  // a second column it also sets, if any
        Route route = Route::Unmapped;
        bool cureByArchetype = false;  // a cure through an engine Cure* archetype (cure_instant)
        // Per-MGEF facts the item mapper needs for every row, computed once
        // (ClassifyEffect): the helper-name check and the name table's family
        // (hidden, mapped effects only: the hidden-row checks), and a "Hydrated" name.
        bool helperName = false;
        Col nameFamily = Col::_Count;
        bool hydrated = false;
        // A wrapper's column from its own description, used only when it
        // carries no payload the mapper can read (doc 9's layer 4 applied to
        // a Cloak/hazard whose spell is missing or unknown).
        Col wrapperDescription = Col::_Count;

        [[nodiscard]] bool Mapped() const noexcept { return col != Col::_Count; }
    };

    /// The family a column belongs to for the hidden-row checks: its family
    /// column, or itself for a family or a family-less specific.
    [[nodiscard]] constexpr Col FamilyKey(Col c) noexcept
    {
        if (c == Col::_Count) return c;
        const Col f = FamilyOf(c);
        return f != Col::_Count ? f : c;
    }

    /// Per-load-order overrides (doc 9 layer 2): plugin + local FormID -> column.
    /// The plugin is matched case-insensitively. The local FormID is the low 24
    /// bits, or the low 12 for a light plugin's 0xFE.. form.
    class OverrideTable
    {
    public:
        void Add(std::string_view plugin, std::uint32_t localId, Col col);
        [[nodiscard]] std::optional<Col> Find(std::string_view plugin, std::uint32_t formId) const;
        [[nodiscard]] std::size_t Size() const noexcept { return map_.size(); }
        [[nodiscard]] static std::uint32_t LocalId(std::uint32_t formId) noexcept
        {
            return (formId >> 24) == 0xFE ? (formId & 0xFFFu) : (formId & 0xFFFFFFu);
        }

    private:
        std::map<std::pair<std::string, std::uint32_t>, Col> map_;
    };

    /// The column of one magic effect. `overrides` may be null.
    [[nodiscard]] EffectClass ClassifyEffect(const MagicEffectRecord& effect, const OverrideTable* overrides = nullptr);

    /// The name table alone (lower-cased inside): the column an effect's name
    /// alone suggests, if any. Used by the hidden-row name-agreement check.
    [[nodiscard]] std::optional<Col> NameColumn(std::string_view name);

    /// The helper-name check (dummy, visual, cooldown, "perk impact" ...).
    [[nodiscard]] bool IsHelperName(std::string_view name);

    /// Scope rule 1's name filter: test, dummy, unused, "zz..." items.
    [[nodiscard]] bool IsBadItemName(std::string_view name);

    /// The description route alone (normalised inside), for tests and dumps:
    /// the column a description suggests, or nullopt. `detrimental` flips a
    /// beneficial reading (resist -> weakness).
    [[nodiscard]] std::optional<Col> DescriptionColumn(std::string_view description, bool detrimental);

    /// Every pattern in the rule tables compiles (for a host test); the first
    /// failure's message otherwise.
    [[nodiscard]] std::string CheckRuleTables();

    /// Every rule-table pattern, as written (for the regex oracle fixture,
    /// which checks MiniRegex against Python's `re` on each of them).
    [[nodiscard]] std::vector<std::string> RulePatterns();

    /// Lower-case ASCII copy.
    [[nodiscard]] std::string Lower(std::string_view s);
}
