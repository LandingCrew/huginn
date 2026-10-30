// Host tests for R11: src/core/CapFacets.h -- the facets the live engine asks
// of an item (the slot classes, the filters, the override finders, the label),
// read off cap(i) instead of the old classifiers' tags.
//
// Two kinds of evidence. Synthetic caps pin each facet's columns and each
// class arm. Real items from the three dump fixtures (tests/core/fixtures,
// mapped by the same BuildCaps the catalog runs) pin what the rules give the
// spells, potions, poisons and food a player actually carries.

#include "DumpCsv.h"
#include "core/CapFacets.h"
#include "core/EffectMapper.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <string>

using namespace Huginn::Core;
using Huginn::Core::Effect::Col;
using Huginn::Core::Effect::Kind;
using Huginn::Slot::SlotClassification;
using F = Facets::F;

namespace
{
    Effect::Cap CapOf(std::initializer_list<Col> cols)
    {
        Effect::Cap c;
        for (const Col col : cols) c.push_back(Effect::Value{ col, 0.5f });
        return c;
    }

    Facets::Subject Subj(Facets::Source src, Kind kind, std::initializer_list<Col> cols,
                         Facets::School school = Facets::School::None)
    {
        Facets::Subject s;
        s.source = src;
        s.known = true;
        s.kind = kind;
        const auto cap = CapOf(cols);
        s.facets = Facets::FromCap(cap, kind);
        s.school = school;
        return s;
    }

    bool In(const Facets::Subject& s, SlotClassification c) { return Facets::Matches(s, c); }
}

// =============================================================================
// Facets from synthetic caps
// =============================================================================

TEST_CASE("cap facets: restores, and who a restore is for")
{
    // Healing (self, instant).
    auto heal = Facets::FromCap(CapOf({ Col::restore, Col::restore_health, Col::delivery_self, Col::timing_instant }), Kind::Spell);
    CHECK(heal.Has(F::RestoreHealth));
    CHECK_FALSE(heal.Has(F::TargetsOthers));
    CHECK_FALSE(heal.Has(F::HealsOverTime));

    // Heal Other: aimed, restore_health_other.
    auto other = Facets::FromCap(CapOf({ Col::restore, Col::restore_health, Col::restore_health_other, Col::delivery_aimed }), Kind::Spell);
    CHECK(other.Has(F::RestoreHealth));
    CHECK(other.Has(F::TargetsOthers));

    // A healing aura cast on self: restore_health_other through its area, but
    // delivered on self, so not "for someone else"; it heals over time.
    auto aura = Facets::FromCap(CapOf({ Col::restore, Col::restore_health_other, Col::delivery_self, Col::aura, Col::area }), Kind::Spell);
    CHECK(aura.Has(F::RestoreHealth));
    CHECK_FALSE(aura.Has(F::TargetsOthers));
    CHECK(aura.Has(F::HealsOverTime));

    // A heal over time that is concentration is not "over time" (it is per second while held).
    auto conc = Facets::FromCap(CapOf({ Col::restore, Col::restore_health, Col::timing_over_time, Col::concentration }), Kind::Spell);
    CHECK_FALSE(conc.Has(F::HealsOverTime));
    auto overTime = Facets::FromCap(CapOf({ Col::restore, Col::restore_health, Col::timing_over_time }), Kind::Spell);
    CHECK(overTime.Has(F::HealsOverTime));

    // Oakflesh on Target: armour, touch, not hostile -> for someone else.
    auto oakTarget = Facets::FromCap(CapOf({ Col::defense, Col::defense_armor, Col::delivery_touch }), Kind::Spell);
    CHECK(oakTarget.Has(F::TargetsOthers));
    CHECK(oakTarget.Has(F::ArmorBuff));

    // A Calm: aimed, not hostile, but neither restores nor armours -> not gated.
    auto calm = Facets::FromCap(CapOf({ Col::influence, Col::influence_calm, Col::delivery_aimed }), Kind::Spell);
    CHECK_FALSE(calm.Has(F::TargetsOthers));

    // A hostile aimed drain is not for an ally.
    auto drain = Facets::FromCap(CapOf({ Col::absorb, Col::absorb_health, Col::restore, Col::delivery_aimed, Col::hostile }), Kind::Spell);
    CHECK_FALSE(drain.Has(F::TargetsOthers));
    CHECK(drain.Has(F::Damage));

    // TargetsOthers is a spell's or scroll's: a potion is drunk.
    auto potion = Facets::FromCap(CapOf({ Col::restore, Col::restore_health_other, Col::delivery_touch }), Kind::Potion);
    CHECK_FALSE(potion.Has(F::TargetsOthers));
}

TEST_CASE("cap facets: one facet per column family the consumers read")
{
    struct Case
    {
        Col col;
        F facet;
    };
    for (const Case c : {
             Case{ Col::restore_magicka, F::RestoreMagicka }, Case{ Col::restore_stamina, F::RestoreStamina },
             Case{ Col::regen_health, F::RegenHealth }, Case{ Col::damage, F::Damage }, Case{ Col::absorb, F::Damage },
             Case{ Col::hostile, F::Hostile }, Case{ Col::fortify_vital, F::Fortify },
             Case{ Col::fortify_skill, F::Fortify }, Case{ Col::fortify_combat, F::Fortify }, Case{ Col::regen, F::Regen },
             Case{ Col::stealth, F::Stealth }, Case{ Col::stealth_invisibility, F::Invisibility },
             Case{ Col::stealth_muffle, F::Muffle }, Case{ Col::defense_armor, F::ArmorBuff },
             Case{ Col::defense, F::Defense }, Case{ Col::resist, F::Resist }, Case{ Col::resist_fire, F::ResistFire },
             Case{ Col::resist_frost, F::ResistFrost }, Case{ Col::resist_shock, F::ResistShock },
             Case{ Col::resist_poison, F::ResistPoison }, Case{ Col::resist_magic, F::ResistMagic },
             Case{ Col::summon, F::Summon }, Case{ Col::cure, F::Cure }, Case{ Col::cure_poison, F::CurePoison },
             Case{ Col::cure_disease, F::CureDisease }, Case{ Col::vision, F::Vision }, Case{ Col::utility, F::Utility },
             Case{ Col::movement, F::Movement }, Case{ Col::utility_water_breathing, F::Waterbreathing },
             Case{ Col::survival_warmth, F::Warming }, Case{ Col::survival_intoxication, F::Intoxicating },
             Case{ Col::self_harm, F::SelfHarm }, Case{ Col::narcotic, F::Narcotic } }) {
        INFO("column " << Effect::kColumns[static_cast<std::size_t>(c.col)].id);
        const auto s = Facets::FromCap(CapOf({ c.col }), Kind::Potion);
        CHECK(s.Has(c.facet));
    }
    // A zero value is no column.
    Effect::Cap zero{ Effect::Value{ Col::restore_health, 0.0f } };
    CHECK_FALSE(Facets::FromCap(zero, Kind::Potion).Any());
    CHECK(Facets::Names(Facets::Set{}) == "-");
    CHECK(Facets::Names(Facets::FromCap(CapOf({ Col::restore_health, Col::hostile }), Kind::Spell)) == "RestoreHealth|Hostile");
}

TEST_CASE("cap facets: Narcotic is the narcotic column, never a side effect")
{
    // Mead (LoreRim): its column, beside the Inebriation side effect.
    auto mead = Facets::FromCap(CapOf({ Col::self_harm, Col::survival, Col::survival_hunger, Col::narcotic }), Kind::Food);
    CHECK(mead.Has(F::Narcotic));
    // The side effects alone are no drink: R11's reading (self_harm with no
    // vital drain) took a paralysing cheese; Weak Stomach is raw food.
    CHECK_FALSE(Facets::FromCap(CapOf({ Col::self_harm, Col::survival, Col::survival_hunger }), Kind::Food).Has(F::Narcotic));
    auto raw = Facets::FromCap(CapOf({ Col::self_harm, Col::self_harm_stamina, Col::survival_hunger }), Kind::Food);
    CHECK(raw.Has(F::SelfHarm));
    CHECK_FALSE(raw.Has(F::Narcotic));
    // Nor is intoxication by itself (Simonrim's Skooma has its drug keywords too).
    CHECK_FALSE(Facets::FromCap(CapOf({ Col::survival_intoxication }), Kind::Food).Has(F::Narcotic));
    CHECK(Facets::Names(mead) == "SelfHarm|Narcotic");
}

// =============================================================================
// The slot classes, arm by arm
// =============================================================================

TEST_CASE("slot classes from cap: spells")
{
    using S = Facets::Source;
    const auto fire = Subj(S::Spell, Kind::Spell, { Col::damage, Col::damage_health_fire, Col::hostile }, Facets::School::Destruction);
    CHECK(In(fire, SlotClassification::DamageAny));
    CHECK(In(fire, SlotClassification::DamageMagic));
    CHECK(In(fire, SlotClassification::SpellsAny));
    CHECK(In(fire, SlotClassification::SpellsDestruction));
    CHECK_FALSE(In(fire, SlotClassification::SpellsRestoration));
    CHECK_FALSE(In(fire, SlotClassification::ScrollsAny));
    CHECK_FALSE(In(fire, SlotClassification::BuffsAny));
    CHECK(In(fire, SlotClassification::Regular));
    CHECK(Facets::Classify(fire) == SlotClassification::DamageMagic);

    const auto heal = Subj(S::Spell, Kind::Spell, { Col::restore, Col::restore_health, Col::delivery_self }, Facets::School::Restoration);
    CHECK(In(heal, SlotClassification::HealingAny));
    CHECK_FALSE(In(heal, SlotClassification::DamageAny));
    CHECK(Facets::Classify(heal) == SlotClassification::HealingAny);

    const auto oak = Subj(S::Spell, Kind::Spell, { Col::defense, Col::defense_armor, Col::delivery_self });
    CHECK(In(oak, SlotClassification::BuffsAny));
    CHECK(In(oak, SlotClassification::DefensiveAny));
    CHECK(Facets::Classify(oak) == SlotClassification::DefensiveAny);

    const auto ward = Subj(S::Spell, Kind::Spell, { Col::defense, Col::defense_ward, Col::defense_armor });
    CHECK(In(ward, SlotClassification::DefensiveAny));

    const auto resist = Subj(S::Spell, Kind::Spell, { Col::resist, Col::resist_fire });
    CHECK(In(resist, SlotClassification::DefensiveAny));
    CHECK_FALSE(In(resist, SlotClassification::BuffsAny));

    const auto invis = Subj(S::Spell, Kind::Spell, { Col::stealth, Col::stealth_invisibility });
    CHECK(In(invis, SlotClassification::BuffsAny));

    const auto familiar = Subj(S::Spell, Kind::Spell, { Col::summon, Col::summon_creature }, Facets::School::Conjuration);
    CHECK(In(familiar, SlotClassification::SummonsAny));
    CHECK(In(familiar, SlotClassification::SpellsConjuration));
    CHECK(Facets::Classify(familiar) == SlotClassification::SummonsAny);

    const auto bound = Subj(S::Spell, Kind::Spell, { Col::summon, Col::summon_bound_weapon });
    CHECK(In(bound, SlotClassification::SummonsAny));

    for (const Col u : { Col::vision_light, Col::vision_detect_life, Col::utility_telekinesis, Col::utility_unlock,
                         Col::utility_water_breathing, Col::movement_jump_fall, Col::cure_disease }) {
        const Col fam = Effect::kColumns[static_cast<std::size_t>(u)].family;
        const auto s = Subj(S::Spell, Kind::Spell, { fam, u });
        INFO("utility column " << Effect::kColumns[static_cast<std::size_t>(u)].id);
        CHECK(In(s, SlotClassification::Utility));
    }

    // A hostile utility (a hostile Telekinesis-like effect) is not a utility key's.
    const auto hostileUtil = Subj(S::Spell, Kind::Spell, { Col::utility, Col::utility_telekinesis, Col::hostile });
    CHECK_FALSE(In(hostileUtil, SlotClassification::Utility));

    // A Fear: no class but the spell ones.
    const auto fear = Subj(S::Spell, Kind::Spell, { Col::influence, Col::influence_fear, Col::hostile }, Facets::School::Illusion);
    CHECK(Facets::Classify(fear) == SlotClassification::SpellsIllusion);
}

TEST_CASE("slot classes from cap: scrolls")
{
    using S = Facets::Source;
    const auto fireball = Subj(S::Scroll, Kind::Scroll, { Col::damage, Col::damage_health_fire, Col::hostile }, Facets::School::Destruction);
    CHECK(In(fireball, SlotClassification::ScrollsAny));
    CHECK(In(fireball, SlotClassification::DamageMagic));
    CHECK(In(fireball, SlotClassification::SpellsDestruction));
    CHECK_FALSE(In(fireball, SlotClassification::SpellsAny));
    CHECK_FALSE(In(fireball, SlotClassification::PotionsAny));
}

TEST_CASE("slot classes from cap: potions, poisons and food")
{
    using S = Facets::Source;
    const auto healthPotion = Subj(S::Item, Kind::Potion, { Col::restore, Col::restore_health });
    CHECK(In(healthPotion, SlotClassification::HealingAny));
    CHECK(In(healthPotion, SlotClassification::PotionsAny));
    CHECK_FALSE(In(healthPotion, SlotClassification::FoodAny));
    CHECK_FALSE(In(healthPotion, SlotClassification::DamageAny));
    CHECK(Facets::Classify(healthPotion) == SlotClassification::HealingAny);

    const auto magickaPotion = Subj(S::Item, Kind::Potion, { Col::restore, Col::restore_magicka });
    CHECK(Facets::Classify(magickaPotion) == SlotClassification::PotionsAny);

    const auto poison = Subj(S::Item, Kind::Poison, { Col::damage, Col::damage_health_poison, Col::hostile });
    CHECK(In(poison, SlotClassification::DamageAny));
    CHECK(In(poison, SlotClassification::PoisonsAny));
    CHECK(In(poison, SlotClassification::PotionsAny));
    CHECK_FALSE(In(poison, SlotClassification::DamageMagic));
    CHECK(Facets::Classify(poison) == SlotClassification::PoisonsAny);

    // A poison that "restores" (a paralysis poison with a heal rider) is still a poison only.
    const auto oddPoison = Subj(S::Item, Kind::Poison, { Col::restore, Col::restore_health, Col::control_paralysis, Col::control });
    CHECK_FALSE(In(oddPoison, SlotClassification::HealingAny));

    const auto resistFire = Subj(S::Item, Kind::Potion, { Col::resist, Col::resist_fire });
    CHECK(In(resistFire, SlotClassification::DefensiveAny));
    CHECK(Facets::Classify(resistFire) == SlotClassification::DefensiveAny);

    const auto fortify = Subj(S::Item, Kind::Potion, { Col::fortify_skill, Col::fortify_skill_destruction });
    CHECK(In(fortify, SlotClassification::BuffsAny));

    const auto cure = Subj(S::Item, Kind::Potion, { Col::cure, Col::cure_poison });
    CHECK(In(cure, SlotClassification::Utility));
    const auto breath = Subj(S::Item, Kind::Potion, { Col::utility, Col::utility_water_breathing });
    CHECK(In(breath, SlotClassification::Utility));

    const auto bread = Subj(S::Item, Kind::Food, { Col::restore, Col::restore_health, Col::survival, Col::survival_hunger });
    CHECK(In(bread, SlotClassification::FoodAny));
    CHECK(In(bread, SlotClassification::HealingAny));   // as before: a food that restores health
    CHECK_FALSE(In(bread, SlotClassification::PotionsAny));
    CHECK_FALSE(In(bread, SlotClassification::NarcoticsAny));
    CHECK(Facets::Classify(bread) == SlotClassification::HealingAny);

    const auto mead = Subj(S::Item, Kind::Food, { Col::self_harm, Col::survival, Col::survival_thirst, Col::narcotic });
    CHECK(In(mead, SlotClassification::NarcoticsAny));
    CHECK_FALSE(In(mead, SlotClassification::FoodAny));   // 0.24.9: alcohol is not food (R11 took it)
    CHECK_FALSE(In(mead, SlotClassification::PotionsAny));
    CHECK(Facets::Classify(mead) == SlotClassification::NarcoticsAny);

    // A healing drink keeps its effect class, and still stays off the food keys.
    const auto healingAle = Subj(S::Item, Kind::Food, { Col::restore, Col::restore_health, Col::narcotic });
    CHECK(In(healingAle, SlotClassification::HealingAny));
    CHECK(In(healingAle, SlotClassification::NarcoticsAny));
    CHECK_FALSE(In(healingAle, SlotClassification::FoodAny));

    // A Skooma the load order makes a potion: a narcotic and a potion (LoreRim's Skoomas).
    const auto skoomaPotion = Subj(S::Item, Kind::Potion, { Col::restore, Col::restore_stamina, Col::narcotic });
    CHECK(In(skoomaPotion, SlotClassification::NarcoticsAny));
    CHECK(In(skoomaPotion, SlotClassification::PotionsAny));
    CHECK_FALSE(In(skoomaPotion, SlotClassification::FoodAny));

    // A poison is never a narcotic key's, even if its cap carries the column
    // (the mapper never sets it on one; this is the class's own gate).
    const auto drugPoison = Subj(S::Item, Kind::Poison, { Col::damage, Col::hostile, Col::narcotic });
    CHECK_FALSE(In(drugPoison, SlotClassification::NarcoticsAny));
    CHECK(In(drugPoison, SlotClassification::PoisonsAny));
    CHECK(Facets::Classify(drugPoison) == SlotClassification::PoisonsAny);

    // A food whose cost is a paralysis (R11's stray, Rendil's Screaming Cheese): food, no drink.
    const auto cheese = Subj(S::Item, Kind::Food, { Col::self_harm, Col::survival, Col::survival_hunger });
    CHECK_FALSE(In(cheese, SlotClassification::NarcoticsAny));
    CHECK(In(cheese, SlotClassification::FoodAny));

    const auto raw = Subj(S::Item, Kind::Food, { Col::self_harm, Col::self_harm_stamina, Col::survival_hunger });
    CHECK_FALSE(In(raw, SlotClassification::NarcoticsAny));
    CHECK(Facets::Classify(raw) == SlotClassification::FoodAny);

    // No catalog entry: the source type decides, and no effect class (so never NarcoticsAny).
    Facets::Subject unknownFood;
    unknownFood.source = S::Item;
    unknownFood.kind = Kind::Food;
    CHECK(In(unknownFood, SlotClassification::FoodAny));
    CHECK_FALSE(In(unknownFood, SlotClassification::NarcoticsAny));

    const auto gem = Subj(S::Item, Kind::SoulGem, {});
    CHECK(Facets::Classify(gem) == SlotClassification::Regular);
    CHECK_FALSE(In(gem, SlotClassification::PotionsAny));
}

TEST_CASE("slot classes from cap: weapons, ammo, apparel, torches")
{
    using S = Facets::Source;
    Facets::Subject sword;
    sword.source = S::Weapon;
    sword.kind = Kind::Weapon;
    sword.melee = true;
    CHECK(In(sword, SlotClassification::WeaponsAny));
    CHECK(In(sword, SlotClassification::WeaponsMelee));
    CHECK(In(sword, SlotClassification::DamageAny));
    CHECK_FALSE(In(sword, SlotClassification::WeaponsRanged));
    CHECK_FALSE(In(sword, SlotClassification::DamageMagic));
    // DamageAny comes before the weapon classes in the priority order, as it
    // did for the old classifier: a weapon's class (the class cap's group) is
    // DamageAny.
    CHECK(Facets::Classify(sword) == SlotClassification::DamageAny);
    Facets::Subject bow = sword;
    bow.melee = false;
    bow.ranged = true;
    CHECK(In(bow, SlotClassification::WeaponsRanged));
    CHECK_FALSE(In(bow, SlotClassification::WeaponsMelee));

    Facets::Subject arrows;
    arrows.source = S::Ammo;
    CHECK(Facets::Classify(arrows) == SlotClassification::AmmoAny);
    CHECK_FALSE(In(arrows, SlotClassification::WeaponsAny));

    Facets::Subject ring;
    ring.source = S::Apparel;
    CHECK(Facets::Classify(ring) == SlotClassification::ApparelAny);

    Facets::Subject torch;
    torch.source = S::Torch;
    CHECK(Facets::Classify(torch) == SlotClassification::Utility);
}

TEST_CASE("slot classes from cap: no catalog entry -> only what the source decides")
{
    using S = Facets::Source;
    Facets::Subject spell;
    spell.source = S::Spell;
    spell.known = false;
    spell.school = Facets::School::Restoration;
    CHECK(In(spell, SlotClassification::SpellsAny));
    CHECK(In(spell, SlotClassification::SpellsRestoration));
    CHECK_FALSE(In(spell, SlotClassification::HealingAny));
    CHECK(Facets::Classify(spell) == SlotClassification::SpellsRestoration);

    Facets::Subject potion;   // kind from its source type (SourceType::Potion)
    potion.source = S::Item;
    potion.known = false;
    potion.kind = Kind::Potion;
    CHECK(Facets::Classify(potion) == SlotClassification::PotionsAny);
    CHECK_FALSE(In(potion, SlotClassification::HealingAny));
}

TEST_CASE("slot classes from cap: every class has a matching subject and Regular takes all")
{
    using S = Facets::Source;
    // Each class but Regular is the Classify answer of some subject: no
    // dead class (the enum stays for the INI; each value must still match).
    std::array<bool, Huginn::Slot::SLOT_CLASSIFICATION_COUNT> reached{};
    const Facets::Subject subjects[] = {
        Subj(S::Spell, Kind::Spell, { Col::restore, Col::restore_health }),
        Subj(S::Spell, Kind::Spell, { Col::damage, Col::damage_health_fire }),
        Subj(S::Item, Kind::Poison, { Col::damage }),
        Subj(S::Spell, Kind::Spell, { Col::resist, Col::resist_fire }),
        Subj(S::Spell, Kind::Spell, { Col::fortify_vital }),
        Subj(S::Spell, Kind::Spell, { Col::summon }),
        Subj(S::Spell, Kind::Spell, { Col::vision, Col::vision_light }),
        Subj(S::Item, Kind::Food, { Col::survival_intoxication, Col::narcotic }),
        Subj(S::Item, Kind::Food, { Col::survival_hunger }),
        Subj(S::Item, Kind::Potion, { Col::restore_magicka }),
        Subj(S::Scroll, Kind::Scroll, { Col::influence_fear }),
        Subj(S::Spell, Kind::Spell, { Col::influence_fear }, Facets::School::Destruction),
        Subj(S::Spell, Kind::Spell, { Col::influence_fear }, Facets::School::Restoration),
        Subj(S::Spell, Kind::Spell, { Col::influence_fear }, Facets::School::Conjuration),
        Subj(S::Spell, Kind::Spell, { Col::influence_fear }, Facets::School::Illusion),
        Subj(S::Spell, Kind::Spell, { Col::influence_fear }, Facets::School::Alteration),
        Subj(S::Spell, Kind::Spell, { Col::influence_fear }),
    };
    for (const auto& s : subjects) reached[static_cast<std::size_t>(Facets::Classify(s))] = true;
    Facets::Subject w;
    w.source = S::Weapon;
    w.melee = true;
    reached[static_cast<std::size_t>(Facets::Classify(w))] = true;
    w.melee = false;
    w.ranged = true;
    reached[static_cast<std::size_t>(Facets::Classify(w))] = true;
    Facets::Subject a;
    a.source = S::Ammo;
    reached[static_cast<std::size_t>(Facets::Classify(a))] = true;
    a.source = S::Apparel;
    reached[static_cast<std::size_t>(Facets::Classify(a))] = true;
    // The weapon classes are never the MOST specific class (DamageAny comes
    // first and takes every weapon, as before R11), but they match:
    Facets::Subject melee;
    melee.source = S::Weapon;
    melee.melee = true;
    CHECK(In(melee, SlotClassification::WeaponsMelee));
    CHECK(In(w, SlotClassification::WeaponsRanged));
    CHECK(In(w, SlotClassification::WeaponsAny));
    reached[static_cast<std::size_t>(SlotClassification::WeaponsMelee)] = true;
    reached[static_cast<std::size_t>(SlotClassification::WeaponsRanged)] = true;
    reached[static_cast<std::size_t>(SlotClassification::WeaponsAny)] = true;
    reached[static_cast<std::size_t>(SlotClassification::Regular)] = true;
    for (std::size_t c = 0; c < reached.size(); ++c) {
        INFO("class " << c);
        CHECK(reached[c]);
    }
}

// =============================================================================
// Real items from the dump fixtures
// =============================================================================

namespace
{
    struct Fixture
    {
        Huginn::Test::Dump dump;
        Effect::BuildResult built;

        explicit Fixture(const std::string& list)
        {
            Huginn::Test::DumpReader reader;
            reader.extraColumns = { "expectColumn", "expectNote", "expectInScope", "expectEffects", "expectValues" };
            std::string err;
            REQUIRE_MESSAGE(reader.Load(std::string(HUGINN_REPO_ROOT) + "/tests/core/fixtures/effects_" + list + ".csv", dump, &err), err);
            built = Effect::BuildCaps(dump.items, dump.effects, nullptr);
        }

        Facets::Subject Of(std::uint32_t formId, Kind kind, Facets::School school = Facets::School::None) const
        {
            for (std::size_t i = 0; i < dump.items.size(); ++i) {
                if (dump.items[i].formId != formId || dump.items[i].kind != kind) continue;
                Facets::Subject s;
                switch (kind) {
                    case Kind::Spell: s.source = Facets::Source::Spell; break;
                    case Kind::Scroll: s.source = Facets::Source::Scroll; break;
                    case Kind::Weapon: s.source = Facets::Source::Weapon; break;
                    case Kind::Ammo: s.source = Facets::Source::Ammo; break;
                    case Kind::Armour: s.source = Facets::Source::Apparel; break;
                    case Kind::Light: s.source = Facets::Source::Torch; break;
                    default: s.source = Facets::Source::Item; break;
                }
                s.known = true;
                s.kind = kind;
                s.facets = Facets::FromCap(built.caps[i], kind);
                s.school = school;
                return s;
            }
            FAIL("item " << formId << " not in the fixture");
            return {};
        }
    };
}

TEST_CASE("cap facets on real items: vanilla+")
{
    const Fixture fx("vanilla");
    const auto healOther = fx.Of(0x00012FD2, Kind::Spell);     // Heal Other
    CHECK(healOther.facets.Has(F::RestoreHealth));
    CHECK(healOther.facets.Has(F::TargetsOthers));
    CHECK(Facets::Classify(healOther) == SlotClassification::HealingAny);

    const auto grand = fx.Of(0x000B62EE, Kind::Spell);         // Grand Healing: an area heal cast on self
    CHECK(grand.facets.Has(F::RestoreHealth));
    CHECK_FALSE(grand.facets.Has(F::TargetsOthers));

    const auto ward = fx.Of(0x000211F0, Kind::Spell);          // Greater Ward
    CHECK(Facets::Matches(ward, SlotClassification::DefensiveAny));
    CHECK_FALSE(ward.facets.Has(F::TargetsOthers));

    const auto thrall = fx.Of(0x0007E5D5, Kind::Spell);        // Flame Thrall
    CHECK(Facets::Classify(thrall) == SlotClassification::SummonsAny);

    const auto storm = fx.Of(0x0007A82B, Kind::Spell);         // Fire Storm
    CHECK(Facets::Classify(storm) == SlotClassification::DamageMagic);

    const auto cloak = fx.Of(0x0003AE9F, Kind::Spell);         // Flame Cloak: damage around the caster
    CHECK(Facets::Matches(cloak, SlotClassification::DamageMagic));

    const auto candle = fx.Of(0x00043324, Kind::Spell);        // Candlelight
    CHECK(Facets::Classify(candle) == SlotClassification::Utility);

    const auto invis = fx.Of(0x00027EB6, Kind::Spell);         // Invisibility
    CHECK(invis.facets.Has(F::Invisibility));
    CHECK(Facets::Matches(invis, SlotClassification::BuffsAny));

    const auto muffle = fx.Of(0x0008F3EB, Kind::Spell);        // Muffle
    CHECK(muffle.facets.Has(F::Muffle));

    const auto breath = fx.Of(0x0005D175, Kind::Spell);        // Waterbreathing
    CHECK(breath.facets.Has(F::Waterbreathing));
    CHECK(Facets::Matches(breath, SlotClassification::Utility));

    const auto curePoison = fx.Of(0x00065A64, Kind::Potion);   // Potion of Cure Poison
    CHECK(curePoison.facets.Has(F::CurePoison));
    CHECK(Facets::Classify(curePoison) == SlotClassification::Utility);
    CHECK(Facets::Matches(curePoison, SlotClassification::PotionsAny));

    const auto philter = fx.Of(0x00039B4B, Kind::Potion);      // Philter of Resist Fire
    CHECK(philter.facets.Has(F::ResistFire));
    CHECK(Facets::Classify(philter) == SlotClassification::DefensiveAny);

    const auto frenzy = fx.Of(0x00073F5D, Kind::Poison);       // Deadly Frenzy Poison
    CHECK(Facets::Classify(frenzy) == SlotClassification::PoisonsAny);

    const auto honey = fx.Of(0x0010394D, Kind::Food);          // Honey
    CHECK(Facets::Matches(honey, SlotClassification::FoodAny));
    CHECK_FALSE(Facets::Matches(honey, SlotClassification::NarcoticsAny));

    // Vanilla tags no drink: the drawback MGEF Skyrim.esm 10C645 is its mark.
    const auto bloodwine = fx.Of(0x03003535, Kind::Food);      // Argonian Bloodwine
    CHECK(bloodwine.facets.Has(F::Narcotic));
    CHECK(Facets::Matches(bloodwine, SlotClassification::NarcoticsAny));
    CHECK_FALSE(Facets::Matches(bloodwine, SlotClassification::FoodAny));
    CHECK(Facets::Classify(bloodwine) == SlotClassification::DefensiveAny);   // its Resist Poison first

    // Vanilla tags no drug either: Drugged (Skyrim.esm 0E3CBB) marks Sleeping
    // Tree Sap; Redwater Skooma has Drugged and its own script effect (0.24.16).
    const auto sap = fx.Of(0x000AED90, Kind::Potion);          // Sleeping Tree Sap
    CHECK(sap.facets.Has(F::Narcotic));
    CHECK(Facets::Matches(sap, SlotClassification::NarcoticsAny));
    CHECK(Facets::Matches(sap, SlotClassification::PotionsAny));
    const auto redwater = fx.Of(0x0201391D, Kind::Potion);     // Redwater Skooma
    CHECK(redwater.facets.Has(F::Narcotic));
    CHECK(Facets::Matches(redwater, SlotClassification::NarcoticsAny));
    // Vanilla's other potions are not narcotics (Cure Poison above, any of them).
    CHECK_FALSE(curePoison.facets.Has(F::Narcotic));
    CHECK_FALSE(Facets::Matches(frenzy, SlotClassification::NarcoticsAny));

    const auto stew = fx.Of(0x000F4314, Kind::Food);           // Beef Stew: fortify stamina
    CHECK(Facets::Matches(stew, SlotClassification::BuffsAny));
    CHECK(Facets::Matches(stew, SlotClassification::FoodAny));
}

TEST_CASE("cap facets on real items: Simonrim and LoreRim drinks, poisons, scrolls")
{
    const Fixture sr("simonrim");
    const auto skooma = sr.Of(0x0003F4BD, Kind::Food);         // Double-Distilled Skooma: MAG_FoodTypeDrugs
    CHECK(skooma.facets.Has(F::Intoxicating));
    CHECK(skooma.facets.Has(F::Narcotic));
    CHECK(Facets::Matches(skooma, SlotClassification::NarcoticsAny));
    CHECK_FALSE(Facets::Matches(skooma, SlotClassification::FoodAny));
    CHECK(Facets::Classify(skooma) == SlotClassification::BuffsAny);   // its Fortify Stamina comes first
    const auto redwater = sr.Of(0x0201391D, Kind::Food);       // Redwater Skooma
    CHECK(Facets::Matches(redwater, SlotClassification::NarcoticsAny));

    const Fixture lr("lorerim");
    const auto mead = lr.Of(0x00107A8A, Kind::Food);           // Mead with Juniper Berry: every mark
    CHECK(mead.facets.Has(F::Narcotic));
    CHECK(Facets::Matches(mead, SlotClassification::NarcoticsAny));
    CHECK_FALSE(Facets::Matches(mead, SlotClassification::FoodAny));
    const auto sap = lr.Of(0x000AED90, Kind::Potion);          // Sleeping Tree Sap: OCF_AlchDrug, a potion
    CHECK(Facets::Matches(sap, SlotClassification::NarcoticsAny));
    CHECK(Facets::Matches(sap, SlotClassification::PotionsAny));

    const auto snout = lr.Of(0x000669A4, Kind::Food);          // Raw Mammoth Snout: Weak Stomach
    CHECK(snout.facets.Has(F::SelfHarm));
    CHECK_FALSE(Facets::Matches(snout, SlotClassification::NarcoticsAny));
    CHECK(Facets::Matches(snout, SlotClassification::FoodAny));

    const auto pie = lr.Of(0x00064B43, Kind::Food);            // Apple Pie: "Fortify Magicka", not a restore
    CHECK_FALSE(pie.facets.Has(F::RestoreMagicka));
    CHECK(pie.facets.Has(F::Fortify));

    const auto venom = lr.Of(0x000E41B8, Kind::Poison);        // Venom of a Spider
    CHECK(Facets::Classify(venom) == SlotClassification::PoisonsAny);

    const auto cyclone = lr.Of(0x2804F8FC, Kind::Scroll);      // Scroll of Cyclone
    CHECK(Facets::Matches(cyclone, SlotClassification::DamageMagic));
    CHECK(Facets::Matches(cyclone, SlotClassification::ScrollsAny));

    const auto noise = lr.Of(0x8D005EC8, Kind::Spell);         // Noise Rune
    CHECK(noise.facets.Has(F::Muffle));
}

TEST_CASE("resist filter: a cloak's resist rider never makes it redundant")
{
    // Synthetic: a pure resist potion or ward is judged; a damage aura with a
    // resist rider, or a poison, is not.
    CHECK(Facets::ResistJudgeable(Facets::FromCap(CapOf({ Col::resist, Col::resist_fire }), Kind::Potion)));
    CHECK(Facets::ResistJudgeable(Facets::FromCap(CapOf({ Col::resist, Col::resist_magic }), Kind::Spell)));
    CHECK_FALSE(Facets::ResistJudgeable(Facets::FromCap(
        CapOf({ Col::damage, Col::resist, Col::resist_frost, Col::hostile, Col::aura }), Kind::Spell)));
    CHECK_FALSE(Facets::ResistJudgeable(Facets::FromCap(CapOf({ Col::resist, Col::resist_fire, Col::hostile }), Kind::Poison)));
    CHECK_FALSE(Facets::ResistJudgeable(Facets::FromCap(CapOf({ Col::restore, Col::restore_health }), Kind::Potion)));

    // Real items: LoreRim's elemental cloaks carry the opposite element's
    // resistance beside their damage (huginn_effect_report on the LoreRim
    // dump). Each must stay out of the resist filter's reach.
    const Fixture lr("lorerim");
    const auto flame = lr.Of(0x0003AE9F, Kind::Spell);          // Flame Cloak: damage + resist_frost
    CHECK(flame.facets.Has(F::ResistFrost));
    CHECK(flame.facets.Has(F::Damage));
    CHECK_FALSE(Facets::ResistJudgeable(flame.facets));
    const auto frost = lr.Of(0x0003AEA2, Kind::Spell);          // Frost Cloak: damage + resist_fire
    CHECK(frost.facets.Has(F::ResistFire));
    CHECK_FALSE(Facets::ResistJudgeable(frost.facets));
    const auto shock = lr.Of(0x0003AEA3, Kind::Spell);          // Lightning Cloak: damage + resist_shock
    CHECK(shock.facets.Has(F::ResistShock));
    CHECK_FALSE(Facets::ResistJudgeable(shock.facets));

    const Fixture va("vanilla");
    const auto philter = va.Of(0x00039B4B, Kind::Potion);       // Philter of Resist Fire: judged
    CHECK(Facets::ResistJudgeable(philter.facets));
}

TEST_CASE("override finders: side effects from cap")
{
    CHECK_FALSE(Facets::HasSideEffect(Facets::FromCap(CapOf({ Col::restore, Col::restore_health }), Kind::Potion)));
    CHECK(Facets::HasSideEffect(Facets::FromCap(
        CapOf({ Col::restore, Col::restore_stamina, Col::self_harm, Col::self_harm_health }), Kind::Food)));
    CHECK(Facets::HasSideEffect(Facets::FromCap(CapOf({ Col::damage, Col::hostile }), Kind::Poison)));

    const Fixture sr("simonrim");
    const auto skooma = sr.Of(0x0003F4BD, Kind::Food);          // Double-Distilled Skooma: Fortify|Intoxicating
    CHECK(Facets::HasSideEffect(skooma.facets));
    CHECK(Facets::HasSideEffect(Facets::FromCap(CapOf({ Col::fortify_vital, Col::survival_intoxication }), Kind::Food)));
    const Fixture va("vanilla");
    const auto curePoison = va.Of(0x00065A64, Kind::Potion);    // Potion of Cure Poison: pure
    CHECK_FALSE(Facets::HasSideEffect(curePoison.facets));
}
