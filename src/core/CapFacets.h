#pragma once

// =============================================================================
// CAP FACETS (R11) -- what the live engine asks of an item, read off cap(i)
// =============================================================================
// Map Phase 10: the slot classes, the overrides, the filters and the labels
// stop reading the old spell / item / scroll classifiers' tags and types and
// read the effect catalog instead. A facet is one yes/no question those
// consumers ask ("does it restore health?", "is it for someone else?"),
// answered from the nonzero columns of cap(i) (core/EffectColumns.h, the
// columns of docs/architecture/9-data/effects.csv). This file is the ONE
// place that answer is written, so the slot classes, the filters and the
// override finders cannot disagree about what an item is.
//
// The old classifiers stay, but only as inputs to the D2 legacy feature
// (learning/legacy/, [Scoring] bLegacyLearnFeature): the old context rules,
// correlations, potion multiplier and floors that compute `ln learn mult`
// read their tags, and R6 chose D2. tools/prune/r8_check.py confines them.
//
// Without a catalog entry (an out-of-scope form, or the catalog not built
// yet) an item has no facets: it matches the classes its source alone decides
// (Regular, SpellsAny, ScrollsAny, the school classes, WeaponsAny, AmmoAny,
// ApparelAny; PotionsAny / FoodAny from its source type) and no effect class,
// and no effect filter drops it. The scorer reads only b for it too.
// One classifier output still reaches it: its kind (potion / food / soul gem)
// comes from the candidate's source type (effect/Facets.cpp), which
// candidate/CandidateTypes.cpp derives from ItemClassifier's ItemType. r8_check
// does not see that path; it goes when the item source type stops reading it.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/EffectColumns.h"
#include "core/EffectMapper.h"
#include "core/EffectRecords.h"
#include "core/SlotClassification.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Huginn::Core::Facets
{
    /// One question. Each is answered by FromCap below; the comment names the
    /// columns.
    enum class F : std::uint8_t
    {
        RestoreHealth,     // restore_health or restore_health_other
        RestoreMagicka,    // restore_magicka
        RestoreStamina,    // restore_stamina
        RegenHealth,       // regen_health
        HealsOverTime,     // a health restore that works over time: regen_health, or restore_health on
                           // an aura or over time (timing_over_time) and not concentration
        TargetsOthers,     // a spell or scroll for someone else: not hostile, delivered by touch, aimed or
                           // at a location, and restoring, regenerating, fortifying a vital or armouring
        Damage,            // the damage or absorb family
        Hostile,           // the hostile modifier
        Fortify,           // the fortify_vital, fortify_skill or fortify_combat family
        Regen,             // the regen family
        Stealth,           // the stealth family (invisibility, muffle, disguise)
        Invisibility,      // stealth_invisibility
        Muffle,            // stealth_muffle
        ArmorBuff,         // defense_armor
        Ward,              // defense_ward
        Defense,           // the defense family (armour, ward, spell absorb, reflect, ...)
        Resist,            // the resist family
        ResistFire,        // resist_fire
        ResistFrost,       // resist_frost
        ResistShock,       // resist_shock
        ResistPoison,      // resist_poison
        ResistMagic,       // resist_magic
        Summon,            // the summon family (creatures, reanimation, bound weapons)
        Cure,              // the cure family
        CurePoison,        // cure_poison
        CureDisease,       // cure_disease
        Vision,            // the vision family (light, night eye, detect life, clairvoyance)
        Utility,           // the utility family (carry weight, water breathing, telekinesis, unlock, ...)
        Movement,          // the movement family (speed, jump / fall, water walking)
        Waterbreathing,    // utility_water_breathing
        Warming,           // survival_warmth
        Intoxicating,      // survival_intoxication
        SelfHarm,          // the self_harm family (a consumable's cost to its user)
        Narcotic,          // narcotic: an alcoholic drink or a drug (0.24.16; core/EffectMapper.h IsNarcotic)
        Unlock,            // utility_unlock: opens a lock (0.24.30; how far it reaches: core/LockReach.h)
        _Count
    };
    inline constexpr std::size_t kFacetCount = static_cast<std::size_t>(F::_Count);
    static_assert(kFacetCount <= 64, "facets fit one 64-bit set");

    inline constexpr std::array<std::string_view, kFacetCount> kFacetNames{
        "RestoreHealth", "RestoreMagicka", "RestoreStamina", "RegenHealth", "HealsOverTime", "TargetsOthers",
        "Damage", "Hostile", "Fortify", "Regen", "Stealth", "Invisibility", "Muffle", "ArmorBuff", "Ward", "Defense",
        "Resist", "ResistFire", "ResistFrost", "ResistShock", "ResistPoison", "ResistMagic", "Summon", "Cure",
        "CurePoison", "CureDisease", "Vision", "Utility", "Movement", "Waterbreathing", "Warming",
        "Intoxicating", "SelfHarm", "Narcotic", "Unlock",
    };

    /// A set of facets.
    struct Set
    {
        std::uint64_t bits = 0;

        [[nodiscard]] constexpr bool Has(F f) const noexcept
        {
            return (bits >> static_cast<unsigned>(f)) & 1u;
        }
        constexpr void Add(F f) noexcept { bits |= std::uint64_t{ 1 } << static_cast<unsigned>(f); }
        [[nodiscard]] constexpr bool Any() const noexcept { return bits != 0; }
        constexpr bool operator==(const Set&) const noexcept = default;
    };

    /// The facets of a cap. `kind` is the catalog entry's form kind (a spell
    /// and a scroll are the only kinds TargetsOthers applies to).
    [[nodiscard]] inline Set FromCap(std::span<const Effect::Value> cap, Effect::Kind kind) noexcept
    {
        using Effect::Col;
        std::array<bool, Effect::kColumnCount> on{};
        for (const auto& v : cap) {
            const auto j = static_cast<std::size_t>(v.col);
            if (j < Effect::kColumnCount && v.v != 0.0f) on[j] = true;
        }
        const auto has = [&on](Col c) { return on[static_cast<std::size_t>(c)]; };

        Set s;
        const auto add = [&s](F f, bool yes) {
            if (yes) s.Add(f);
        };
        add(F::RestoreHealth, has(Col::restore_health) || has(Col::restore_health_other));
        add(F::RestoreMagicka, has(Col::restore_magicka));
        add(F::RestoreStamina, has(Col::restore_stamina));
        add(F::RegenHealth, has(Col::regen_health));
        add(F::HealsOverTime, has(Col::regen_health) ||
                                  ((has(Col::restore_health) || has(Col::restore_health_other)) &&
                                   (has(Col::aura) || (has(Col::timing_over_time) && !has(Col::concentration)))));
        const bool spellLike = kind == Effect::Kind::Spell || kind == Effect::Kind::Scroll;
        const bool atOthers = has(Col::delivery_touch) || has(Col::delivery_aimed) || has(Col::delivery_target_location);
        const bool helps = has(Col::restore) || has(Col::regen) || has(Col::fortify_vital) || has(Col::defense_armor);
        add(F::TargetsOthers, spellLike && !has(Col::hostile) && atOthers && helps);
        add(F::Damage, has(Col::damage) || has(Col::absorb));
        add(F::Hostile, has(Col::hostile));
        add(F::Fortify, has(Col::fortify_vital) || has(Col::fortify_skill) || has(Col::fortify_combat));
        add(F::Regen, has(Col::regen));
        add(F::Stealth, has(Col::stealth));
        add(F::Invisibility, has(Col::stealth_invisibility));
        add(F::Muffle, has(Col::stealth_muffle));
        add(F::ArmorBuff, has(Col::defense_armor));
        add(F::Ward, has(Col::defense_ward));
        add(F::Defense, has(Col::defense));
        add(F::Resist, has(Col::resist));
        add(F::ResistFire, has(Col::resist_fire));
        add(F::ResistFrost, has(Col::resist_frost));
        add(F::ResistShock, has(Col::resist_shock));
        add(F::ResistPoison, has(Col::resist_poison));
        add(F::ResistMagic, has(Col::resist_magic));
        add(F::Summon, has(Col::summon));
        add(F::Cure, has(Col::cure));
        add(F::CurePoison, has(Col::cure_poison));
        add(F::CureDisease, has(Col::cure_disease));
        add(F::Vision, has(Col::vision));
        add(F::Utility, has(Col::utility));
        add(F::Movement, has(Col::movement));
        add(F::Waterbreathing, has(Col::utility_water_breathing));
        add(F::Warming, has(Col::survival_warmth));
        add(F::Intoxicating, has(Col::survival_intoxication));
        add(F::SelfHarm, has(Col::self_harm));
        add(F::Narcotic, has(Col::narcotic));
        add(F::Unlock, has(Col::utility_unlock));
        return s;
    }

    /// "RestoreHealth|Hostile" (the console's and the log's form); "-" for none.
    [[nodiscard]] inline std::string Names(Set s)
    {
        std::string out;
        for (std::size_t i = 0; i < kFacetCount; ++i) {
            if (!s.Has(static_cast<F>(i))) continue;
            if (!out.empty()) out += '|';
            out += kFacetNames[i];
        }
        return out.empty() ? std::string("-") : out;
    }

    // -------------------------------------------------------------------------
    // The subject of a class question: what the slot code knows of a candidate
    // -------------------------------------------------------------------------

    /// The candidate's own kind of record (the candidate variant), known
    /// whether or not the catalog has an entry for it.
    enum class Source : std::uint8_t { Spell, Item, Scroll, Weapon, Ammo, Apparel, Torch };

    /// The spell schools a slot class can name (the candidate's school, a
    /// form fact: the costliest effect's magic skill).
    enum class School : std::uint8_t { None, Destruction, Restoration, Alteration, Illusion, Conjuration };

    struct Subject
    {
        Source source = Source::Item;
        /// The catalog had an entry: `kind` and `facets` are its answer. False:
        /// `kind` is the candidate's source type's (Potion, Food, SoulGem for an
        /// item) and `facets` is empty.
        bool known = false;
        Effect::Kind kind = Effect::Kind::Potion;
        Set facets;
        School school = School::None;
        bool melee = false;    // a weapon: a one- or two-handed blade, axe, mace or dagger (the weapon's type)
        bool ranged = false;   // a weapon: bow, crossbow or staff (the weapon's type, a form fact)
        int unlockReach = -1;  // 0.24.30: the catalog's reach for an Unlock item (core/LockReach.h), -1 unknown
    };

    /// Narcotic: the `narcotic` column (effects.csv, EffectMapper.h
    /// IsNarcotic), an alcoholic drink or a drug, potion or food: the food
    /// drinks, LoreRim's Balmora Blue, and the Skoomas and Sleeping Tree Sap
    /// the load order makes potions. The user (2026-10-30): alcohol is a
    /// hybrid of food, potion and poison, and is not food for the food keys
    /// (FoodAny); drugs go with it ("just group into narcotics", 0.24.16;
    /// 0.24.9 called the group alcohol). Before 0.24.9 it was guessed from
    /// the side effects, which also took Rendil's Screaming Cheese and raw
    /// fish with a paralysis.
    [[nodiscard]] constexpr bool Narcotic(const Subject& s) noexcept
    {
        return s.facets.Has(F::Narcotic);
    }

    /// Whether the resist-redundancy filter may judge an item: it gives its
    /// user a resistance (a resist_* facet) and is neither hostile nor
    /// damaging. LoreRim's elemental cloaks carry a resistance to the opposite
    /// element beside their damage aura (Flame Cloak: damage, resist_frost,
    /// hostile); judged by that rider, a Nord mage over the frost threshold
    /// lost Flame Cloak to the resist filter. The rider is not why anyone
    /// casts a cloak, so it does not make the cloak redundant (doc 11: cloaks
    /// are not resist-filtered). A poison never reaches the question (hostile).
    [[nodiscard]] constexpr bool ResistJudgeable(const Set& f) noexcept
    {
        return !f.Has(F::Hostile) && !f.Has(F::Damage) &&
               (f.Has(F::ResistFire) || f.Has(F::ResistFrost) || f.Has(F::ResistShock) ||
                f.Has(F::ResistPoison) || f.Has(F::ResistMagic));
    }

    /// A consumable's side effect on its drinker, as the override finders ask
    /// it (pure before impure): the self_harm family (a damage, weakness,
    /// ravage or regen drain on the drinker: effects.csv's self_harm rows),
    /// intoxication (survival_intoxication: Skooma's Drugged; Simonrim's
    /// Skooma keeps its Damage Health under it, not under self_harm), or a
    /// hostile row (a poison; a wrapper's payload aimed at others). Replaces
    /// the old ItemData tag test (DamageHealth/Magicka/Stamina, isHostile, and
    /// the Ravage, regen-drain and weakness ext bits, deleted in 0.24.5).
    [[nodiscard]] constexpr bool HasSideEffect(const Set& f) noexcept
    {
        return f.Has(F::SelfHarm) || f.Has(F::Intoxicating) || f.Has(F::Hostile);
    }

    // -------------------------------------------------------------------------
    // The slot classes, re-derived from cap(i) (R11; docs/architecture/5-slots.md
    // has the review of each against what the classifiers did)
    // -------------------------------------------------------------------------
    [[nodiscard]] constexpr bool Matches(const Subject& s, Slot::SlotClassification c) noexcept
    {
        using C = Slot::SlotClassification;
        using K = Effect::Kind;
        const Set f = s.facets;
        if (c == C::Regular) return true;

        switch (s.source) {
            case Source::Spell:
            case Source::Scroll: {
                switch (c) {
                    case C::DamageAny:
                    case C::DamageMagic:
                        return f.Has(F::Damage);
                    case C::HealingAny:
                        return f.Has(F::RestoreHealth) || f.Has(F::RegenHealth);
                    case C::BuffsAny:
                        return !f.Has(F::Hostile) && (f.Has(F::Fortify) || f.Has(F::Regen) || f.Has(F::Stealth) ||
                                                      f.Has(F::ArmorBuff));
                    case C::DefensiveAny:
                        return !f.Has(F::Hostile) && (f.Has(F::Defense) || f.Has(F::Resist));
                    case C::SummonsAny:
                        return f.Has(F::Summon);
                    case C::Utility:
                        return !f.Has(F::Hostile) &&
                               (f.Has(F::Vision) || f.Has(F::Utility) || f.Has(F::Movement) || f.Has(F::Cure));
                    case C::SpellsAny:
                        return s.source == Source::Spell;
                    case C::ScrollsAny:
                        return s.source == Source::Scroll;
                    case C::SpellsDestruction: return s.school == School::Destruction;
                    case C::SpellsRestoration: return s.school == School::Restoration;
                    case C::SpellsConjuration: return s.school == School::Conjuration;
                    case C::SpellsIllusion: return s.school == School::Illusion;
                    case C::SpellsAlteration: return s.school == School::Alteration;
                    default:
                        return false;
                }
            }
            case Source::Item: {
                const bool poison = s.kind == K::Poison;
                switch (c) {
                    case C::DamageAny:
                    case C::PoisonsAny:
                        return poison;
                    case C::HealingAny:
                        return !poison && (f.Has(F::RestoreHealth) || f.Has(F::RegenHealth));
                    case C::BuffsAny:
                        return !poison && (f.Has(F::Fortify) || f.Has(F::Invisibility));
                    case C::DefensiveAny:
                        return !poison && f.Has(F::Resist);
                    case C::Utility:
                        return !poison && (f.Has(F::Cure) || f.Has(F::Utility) || f.Has(F::Vision));
                    case C::PotionsAny:
                        return s.kind == K::Potion || poison;
                    case C::FoodAny:
                        return s.kind == K::Food && !Narcotic(s);   // 0.24.9: a drink or drug is not food
                    case C::NarcoticsAny:
                        return !poison && Narcotic(s);              // a poison is applied, never taken
                    default:
                        return false;
                }
            }
            case Source::Weapon:
                switch (c) {
                    case C::DamageAny:
                    case C::WeaponsAny:
                        return true;
                    case C::WeaponsMelee: return s.melee;
                    case C::WeaponsRanged: return s.ranged;
                    default: return false;
                }
            case Source::Ammo:
                return c == C::AmmoAny;
            case Source::Apparel:
                return c == C::ApparelAny;
            case Source::Torch:
                // A torch is the Light job done with an item: Utility.
                return c == C::Utility;
        }
        return false;
    }

    /// The most specific class a subject matches (the v2 log's and the dump's
    /// "class", the class cap's grouping): the first in this order.
    inline constexpr std::array<Slot::SlotClassification, 23> kClassPriority{
        Slot::SlotClassification::HealingAny,
        Slot::SlotClassification::DamageMagic,    // before DamageAny: the narrower of the two
        Slot::SlotClassification::PoisonsAny,     // before DamageAny and PotionsAny, which both take poisons
        Slot::SlotClassification::DamageAny,
        Slot::SlotClassification::DefensiveAny,
        Slot::SlotClassification::BuffsAny,
        Slot::SlotClassification::SummonsAny,
        Slot::SlotClassification::Utility,
        Slot::SlotClassification::NarcoticsAny,   // before PotionsAny, which takes a Skooma potion too
        Slot::SlotClassification::FoodAny,
        Slot::SlotClassification::PotionsAny,
        Slot::SlotClassification::ScrollsAny,
        Slot::SlotClassification::WeaponsMelee,
        Slot::SlotClassification::WeaponsRanged,
        Slot::SlotClassification::WeaponsAny,
        Slot::SlotClassification::AmmoAny,
        Slot::SlotClassification::ApparelAny,
        Slot::SlotClassification::SpellsDestruction,
        Slot::SlotClassification::SpellsRestoration,
        Slot::SlotClassification::SpellsConjuration,
        Slot::SlotClassification::SpellsIllusion,
        Slot::SlotClassification::SpellsAlteration,
        Slot::SlotClassification::SpellsAny,
    };
    static_assert(kClassPriority.size() + 1 == Slot::SLOT_CLASSIFICATION_COUNT,
        "every class but Regular has a place in the priority order");

    [[nodiscard]] constexpr Slot::SlotClassification Classify(const Subject& s) noexcept
    {
        for (const auto c : kClassPriority) {
            if (Matches(s, c)) return c;
        }
        return Slot::SlotClassification::Regular;
    }
}
