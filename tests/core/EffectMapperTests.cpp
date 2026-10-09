// Hand-written cases for core/EffectRules, core/EffectMapper and
// core/CrossFeatures: each rule in effects.csv's `how` column that the dump
// fixtures (EffectFixtureTests.cpp) cannot pin by themselves -- values,
// percentiles, sentinels, the hidden-row whitelist, payloads, scope, item
// features -- and the deliberate deviations from the reference extractor.

#include "core/CrossFeatures.h"
#include "core/EffectMapper.h"

#include <doctest/doctest.h>

#include <cmath>
#include <ostream>
#include <string>

using namespace Huginn::Core::Effect;

namespace
{
    MagicEffectRecord Mgef(int archetype, std::string av, std::string name, std::uint32_t flags = 0)
    {
        MagicEffectRecord m;
        static std::uint32_t next = 0x100;
        m.formId = ++next;
        m.plugin = "Test.esp";
        m.archetype = archetype;
        m.primaryAV = std::move(av);
        m.name = std::move(name);
        m.flags = flags;
        m.detrimental = (flags & kFlagDetrimental) != 0;
        m.hostile = (flags & kFlagHostile) != 0;
        m.payloadKnown = true;
        return m;
    }

    Col ColOf(const MagicEffectRecord& m) { return ClassifyEffect(m, nullptr).col; }

    /// A small load order: items over one effect table.
    struct World
    {
        EffectTable effects;
        std::vector<ItemRecord> items;

        std::uint32_t Add(MagicEffectRecord m)
        {
            effects.push_back(std::move(m));
            return static_cast<std::uint32_t>(effects.size() - 1);
        }
        ItemRecord& Item(Kind k, std::string name)
        {
            ItemRecord it;
            it.kind = k;
            it.formId = 0x1000 + static_cast<std::uint32_t>(items.size());
            it.name = std::move(name);
            it.value = 10;
            if (k == Kind::Spell) {
                it.spellType = 0;
                it.castingType = kCastFireAndForget;
                it.magickaCost = 50;
                it.taughtByTome = true;
            }
            items.push_back(std::move(it));
            return items.back();
        }
        static void Fx(ItemRecord& it, std::uint32_t e, float mag, std::uint32_t dur = 0, std::uint32_t area = 0)
        {
            it.effects.push_back(EffectRow{ e, mag, dur, area, 0.0f });
        }
        BuildResult Build() const { return BuildCaps(items, effects, nullptr); }
    };

    float CapOf(const BuildResult& r, std::size_t i, Col c) { return Get(r.caps[i], c); }
}

// =============================================================================
// One effect -> its column
// =============================================================================
TEST_CASE("effect rules: vitals by detrimental and Recover")
{
    CHECK(ColOf(Mgef(kArchValueModifier, "Health", "Restore Health")) == Col::restore_health);
    CHECK(ColOf(Mgef(kArchValueModifier, "Health", "Fortify Health", kFlagRecover)) == Col::fortify_vital_health);
    CHECK(ColOf(Mgef(kArchValueModifier, "Magicka", "Drain", kFlagRecover | kFlagDetrimental)) == Col::drain_vital_magicka);
    CHECK(ColOf(Mgef(kArchValueModifier, "Stamina", "Frostbite stamina", kFlagDetrimental)) == Col::damage_stamina);
    CHECK(ColOf(Mgef(kArchPeakValueModifier, "HealRate", "Regenerate Health")) == Col::regen_health);
    CHECK(ColOf(Mgef(kArchValueModifier, "StaminaRateMult", "Ale", kFlagDetrimental)) == Col::weaken_regen_stamina);
    CHECK(ColOf(Mgef(kArchAbsorb, "Magicka", "Absorb Magicka")) == Col::absorb_magicka);
}

TEST_CASE("effect rules: damage element by keyword first, then the resisted actor value")
{
    auto fire = Mgef(kArchValueModifier, "Health", "Firebolt", kFlagDetrimental);
    fire.resistAV = "FireResist";
    CHECK(ColOf(fire) == Col::damage_health_fire);
    auto sun = fire;
    sun.keywords = { "MagicDamageSun" };  // keyword beats the resist AV
    CHECK(ColOf(sun) == Col::damage_health_sun);
    auto plain = Mgef(kArchValueModifier, "Health", "Drain", kFlagDetrimental);
    CHECK(ColOf(plain) == Col::damage_health_magic);  // unresisted folds into magic
    plain.resistAV = "DiseaseResist";
    CHECK(ColOf(plain) == Col::damage_health_disease);
}

TEST_CASE("effect rules: one canonical skill per X, XMod, XPowerMod; never XSkillAdvance")
{
    CHECK(ColOf(Mgef(kArchValueModifier, "Destruction", "a")) == Col::fortify_skill_destruction);
    CHECK(ColOf(Mgef(kArchValueModifier, "DestructionMod", "a")) == Col::fortify_skill_destruction);
    CHECK(ColOf(Mgef(kArchValueModifier, "DestructionPowerMod", "a")) == Col::fortify_skill_destruction);
    CHECK(ColOf(Mgef(kArchValueModifier, "PickPocket", "a")) == Col::fortify_skill_pickpocket);
    CHECK(ColOf(Mgef(kArchValueModifier, "Speechcraft", "a", kFlagDetrimental)) == Col::drain_skill);
    // A reused actor value alone maps to nothing; its keyword decides.
    CHECK(ColOf(Mgef(kArchValueModifier, "OneHandedSkillAdvance", "Mystery")) == Col::_Count);
    auto burden = Mgef(kArchValueModifier, "OneHandedSkillAdvance", "Burden", kFlagDetrimental);
    burden.keywords = { "MAG_MagicEnchBurden" };
    const auto c = ClassifyEffect(burden, nullptr);
    CHECK(c.col == Col::control_slow);
    CHECK(c.route == Route::Keyword);
}

TEST_CASE("effect rules: Simonrim deviations -- Fortify Security, Fortify Potion Duration")
{
    auto security = Mgef(kArchValueModifier, "PickPocketSkillAdvance", "Fortify Security");
    security.keywords = { "MagicEnchFortifyPickPocket" };
    const auto s = ClassifyEffect(security, nullptr);
    CHECK(s.col == Col::fortify_skill_lockpicking);
    CHECK(s.col2 == Col::fortify_skill_pickpocket);

    auto duration = Mgef(kArchValueModifier, "AlchemySkillAdvance", "Fortify Potion Duration");
    duration.keywords = { "MagicEnchFortifyAlchemy" };
    CHECK(ColOf(duration) == Col::meta_potion_duration);
    auto alchemy = Mgef(kArchValueModifier, "Alchemy", "Fortify Alchemy");
    alchemy.keywords = { "MagicEnchFortifyAlchemy" };
    CHECK(ColOf(alchemy) == Col::fortify_skill_alchemy);
}

TEST_CASE("effect rules: a name veto on a carrier actor value (LoreRim Turn Undead on Health)")
{
    // Fortify Health's shape (Recover, not detrimental) carrying another mechanic.
    const auto c = ClassifyEffect(Mgef(kArchValueModifier, "Health", "Turn Undead", kFlagRecover), nullptr);
    CHECK(c.col == Col::influence_turn_undead);
    CHECK(c.route == Route::Name);
    CHECK(ColOf(Mgef(kArchValueModifier, "Magicka", "Silence", kFlagDetrimental | kFlagRecover)) ==
          Col::control_silence);
}

TEST_CASE("effect rules: archetypes, summons, cures")
{
    auto atronach = Mgef(kArchSummonCreature, "", "Conjure Flame Atronach");
    atronach.keywords = { "MagicSummonFire" };
    const auto a = ClassifyEffect(atronach, nullptr);
    CHECK(a.col == Col::summon_creature_fire);
    CHECK(a.col2 == Col::summon_creature);
    // A scripted summon gets no element (keywords are loose on scripts).
    auto scripted = Mgef(kArchScript, "", "Conjure Ice Wraith");
    scripted.keywords = { "MagicSummonShock" };
    CHECK(ColOf(scripted) == Col::summon_creature);
    CHECK(ColOf(Mgef(kArchReanimate, "", "Raise Zombie")) == Col::summon_reanimate);

    const auto cure = ClassifyEffect(Mgef(kArchCureDisease, "", "Cure Disease"), nullptr);
    CHECK(cure.col == Col::cure_disease);
    CHECK(cure.cureByArchetype);
    // LoreRim: CureParalysis archetype named Cure Disease.
    CHECK(ColOf(Mgef(kArchCureParalysis, "", "Cure Disease")) == Col::cure_disease);
    CHECK(ColOf(Mgef(kArchCureParalysis, "", "Free Movement")) == Col::cure_paralysis);
    const auto scriptCure = ClassifyEffect(Mgef(kArchScript, "", "Cure Disease"), nullptr);
    CHECK(scriptCure.col == Col::cure_disease);
    CHECK_FALSE(scriptCure.cureByArchetype);
}

TEST_CASE("effect rules: helpers, wrappers, description route, overrides")
{
    CHECK(ClassifyEffect(Mgef(kArchScript, "", "Dummy Effect"), nullptr).route == Route::Helper);
    CHECK(IsHelperName("Perk Impact Stagger"));
    CHECK_FALSE(IsHelperName("Fireball"));

    auto cloak = Mgef(kArchCloak, "", "Flame Cloak");
    CHECK(ClassifyEffect(cloak, nullptr).route == Route::Wrapper);
    cloak.resistAV = "FireResist";  // a resisted actor value alone: still a wrapper
    CHECK(ClassifyEffect(cloak, nullptr).route == Route::Wrapper);
    cloak.keywords = { "MagicDamageFire" };  // a damage keyword: damage itself
    CHECK(ColOf(cloak) == Col::damage_health_fire);

    auto script = Mgef(kArchScript, "", "Mystic Thing");
    script.description = "Targets <mag> feet away are knocked back.";
    const auto d = ClassifyEffect(script, nullptr);
    CHECK(d.col == Col::control_stagger);
    CHECK(d.route == Route::Description);
    // The detrimental flag flips a beneficial reading.
    CHECK(DescriptionColumn("Resist fire by <mag>%.", true) == Col::weakness_fire);
    CHECK(DescriptionColumn("Resist fire by <mag>%.", false) == Col::resist_fire);
    // Spell power has no column: the reference stops there...
    CHECK(DescriptionColumn("Spells do 25% more damage.", false) == std::nullopt);
    // ...unless a school is named first (the reference's order: SKILL before NONE).
    CHECK(DescriptionColumn("Destruction spells are 25% stronger.", false) == Col::fortify_skill_destruction);

    OverrideTable ov;
    ov.Add("Test.ESP", 0x000ABC, Col::utility_teleport);
    auto overridden = Mgef(kArchValueModifier, "Health", "Restore Health");
    overridden.formId = 0x05000ABC;
    const auto o = ClassifyEffect(overridden, &ov);
    CHECK(o.col == Col::utility_teleport);
    CHECK(o.route == Route::Override);
    CHECK(OverrideTable::LocalId(0xFE012ABC) == 0xABCu);  // light plugin: 12 bits
}

TEST_CASE("effect rules: the deliberate name-table changes")
{
    CHECK(NameColumn("Become Ethereal") == Col::defense_ethereal);
    CHECK(NameColumn("Nord Polymorph") == Col::stealth_disguise);
    CHECK(NameColumn("Shapeshift: Wolf") == Col::stealth_disguise);
    CHECK(NameColumn("Vampire Form") == Col::transform_vampire_lord);
    CHECK(NameColumn("Beast Form") == Col::transform_werewolf);
    CHECK(NameColumn("Fortify Stamina") == Col::fortify_vital_stamina);           // the \b fix
    CHECK(NameColumn("Fortify Health Regneration") == Col::regen_health);        // the typo
    CHECK(NameColumn("Damage Magicka") == Col::damage_magicka);
    CHECK(NameColumn("Fortify One-Handed") == Col::fortify_skill_one_handed);
    CHECK(NameColumn("Fortify Barter") == Col::fortify_skill_speech);
}

// =============================================================================
// Items: rows kept, values, percentiles
// =============================================================================
TEST_CASE("effect mapper: hidden rows -- whitelist, name agreement, visible preferred")
{
    World w;
    const auto frost = [&] {
        auto m = Mgef(kArchValueModifier, "Health", "Frost Damage", kFlagDetrimental);
        m.resistAV = "FrostResist";
        return w.Add(m);
    }();
    const auto slowHidden = w.Add(Mgef(kArchValueModifier, "SpeedMult", "Frost Slow", kFlagDetrimental | kFlagHideInUI));
    const auto invisHidden = w.Add(Mgef(kArchInvisibility, "", "Invisibility", kFlagHideInUI));
    // A hidden DamageResist carrier whose name names another family: dropped.
    const auto carrier = w.Add(Mgef(kArchValueModifier, "DamageResist", "Regeneration", kFlagHideInUI));
    auto& sword = w.Item(Kind::Weapon, "Frost Sword");
    sword.weaponType = 1;
    sword.damage = 7;
    sword.enchanted = true;
    World::Fx(sword, frost, 10);
    World::Fx(sword, slowHidden, 50, 3);
    World::Fx(sword, invisHidden, 1, 10);
    World::Fx(sword, carrier, 5);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::damage_health_frost) > 0.0f);
    CHECK(CapOf(r, 0, Col::control_slow) > 0.0f);
    CHECK(CapOf(r, 0, Col::stealth_invisibility) == 0.0f);
    CHECK(CapOf(r, 0, Col::defense_armor) == 0.0f);
    CHECK(r.mappings[0].tally.counted == 1);  // hidden rows are not in the coverage denominator
    CHECK(r.mappings[0].tally.hiddenKept == 1);
}

TEST_CASE("effect mapper: amount and level values, percentiles within the load order, families as max")
{
    World w;
    const auto heal = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health"));
    const auto fortify = w.Add(Mgef(kArchValueModifier, "Health", "Fortify Health", kFlagRecover));
    // Three potions: 25 instant, 5/s x 20 s (= 100), 50 instant.
    for (auto [mag, dur] : { std::pair{ 25.0f, 0u }, std::pair{ 5.0f, 20u }, std::pair{ 50.0f, 0u } }) {
        auto& p = w.Item(Kind::Potion, "Potion");
        World::Fx(p, heal, mag, dur);
    }
    auto& f = w.Item(Kind::Potion, "Fortify");
    World::Fx(f, fortify, 100, 1800);  // a level: 100 x 1800/3600 = 50
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::restore_health) == doctest::Approx(1.0 / 3.0));
    CHECK(CapOf(r, 1, Col::restore_health) == doctest::Approx(1.0));  // 100 total beats 50 instant
    CHECK(CapOf(r, 2, Col::restore_health) == doctest::Approx(2.0 / 3.0));
    CHECK(CapOf(r, 1, Col::restore) == CapOf(r, 1, Col::restore_health));
    CHECK(CapOf(r, 3, Col::fortify_vital_health) == doctest::Approx(1.0));
    CHECK(r.mappings[1].restoreAmount[0] == doctest::Approx(100.0));
    CHECK(CapOf(r, 1, Col::timing_over_time) == 1.0f);
    CHECK(CapOf(r, 0, Col::timing_instant) == 1.0f);
    CHECK(CapOf(r, 0, Col::delivery_self) == 1.0f);
    CHECK(CapOf(r, 0, Col::consumable) == 1.0f);
    CHECK(CapOf(r, 0, Col::kind_potion) == 1.0f);
}

TEST_CASE("effect mapper: sentinels -- full restore and permanent durations")
{
    World w;
    const auto heal = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health"));
    const auto fire = w.Add(Mgef(kArchValueModifier, "FireResist", "Resist Fire"));
    auto& phial = w.Item(Kind::Potion, "White Phial");
    World::Fx(phial, heal, 9999);
    auto& small = w.Item(Kind::Potion, "Small");
    World::Fx(small, heal, 20);
    auto& ring = w.Item(Kind::Potion, "Forever");
    World::Fx(ring, fire, 30, 99999999);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::restore_health) == 1.0f);
    CHECK(CapOf(r, 0, Col::full_restore) == 1.0f);
    CHECK(r.mappings[0].fullRestore[0]);
    CHECK(CapOf(r, 1, Col::restore_health) == 1.0f);  // the sentinel is not in the population
    CHECK(CapOf(r, 2, Col::long_lasting) == 1.0f);
    CHECK(CapOf(r, 2, Col::duration) == doctest::Approx(1.0));  // clipped to 3600
    CHECK(CapOf(r, 1, Col::long_lasting) == 0.0f);
}

TEST_CASE("effect mapper: presence columns -- P x D, a script effect with no magnitude, hunger, thirst")
{
    World w;
    const auto para = w.Add(Mgef(kArchParalysis, "", "Paralysis", kFlagDetrimental | kFlagHostile));
    auto armorPen = Mgef(kArchScript, "", "Fortify Armor Penetration");
    const auto pen = w.Add(armorPen);
    auto hungerM = Mgef(kArchValueModifier, "Mood", "Restore Hunger Large");
    hungerM.keywords = { "CCSM_RestoreHungerLarge" };
    const auto hunger = w.Add(hungerM);
    auto tinyM = Mgef(kArchScript, "", "Restore Hunger Very Small");
    tinyM.keywords = { "CCSM_RestoreHungerTiny" };
    const auto tiny = w.Add(tinyM);
    const auto thirst = w.Add(Mgef(kArchScript, "", "Restore Thirst"));
    const auto hydrated = w.Add(Mgef(kArchScript, "", "Hydrated"));
    auto& poison = w.Item(Kind::Poison, "Paralyze");
    World::Fx(poison, para, 1, 10);
    auto& sword = w.Item(Kind::Potion, "Pen");
    World::Fx(sword, pen, 0);
    auto& stew = w.Item(Kind::Food, "Stew");
    World::Fx(stew, hunger, 0);
    auto& berry = w.Item(Kind::Food, "Berry");
    World::Fx(berry, tiny, 0);
    auto& water = w.Item(Kind::Food, "Water");
    World::Fx(water, thirst, 1);
    auto& tea = w.Item(Kind::Food, "Tea");
    World::Fx(tea, thirst, 1);
    World::Fx(tea, hydrated, 1);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::control_paralysis) == doctest::Approx(std::log1p(10.0) / std::log1p(3600.0)));
    CHECK(CapOf(r, 0, Col::hostile) == 1.0f);
    CHECK(CapOf(r, 1, Col::fortify_combat_armor_penetration) == 1.0f);
    CHECK(CapOf(r, 2, Col::survival_hunger) == 1.0f);
    CHECK(CapOf(r, 3, Col::survival_hunger) == 0.25f);
    CHECK(CapOf(r, 4, Col::survival_thirst) == 0.5f);
    CHECK(CapOf(r, 5, Col::survival_thirst) == 1.0f);
}

TEST_CASE("effect mapper: a zero magnitude through the engine's data grades at the bottom, not as presence")
{
    World w;
    const auto heal = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health"));
    const auto scripted = w.Add(Mgef(kArchScript, "", "Restore Health"));
    auto& dud = w.Item(Kind::Potion, "Unknown Potion");
    World::Fx(dud, heal, 0);
    auto& small = w.Item(Kind::Potion, "Small");
    World::Fx(small, heal, 25);
    auto& big = w.Item(Kind::Potion, "Ultimate");
    World::Fx(big, heal, 200);
    auto& script = w.Item(Kind::Potion, "Scripted");
    World::Fx(script, scripted, 0);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::restore_health) == doctest::Approx(1.0 / 3.0));  // 1/(N+1), N = 2 real heals
    CHECK(CapOf(r, 0, Col::restore_health) < CapOf(r, 1, Col::restore_health));
    CHECK(CapOf(r, 1, Col::restore_health) == doctest::Approx(0.5));
    CHECK(CapOf(r, 2, Col::restore_health) == doctest::Approx(1.0));
    CHECK(CapOf(r, 3, Col::restore_health) == 1.0f);  // the script carries the amount: presence
}

TEST_CASE("effect mapper: tempering adds damage (the codebase's model), it does not multiply")
{
    CHECK(TemperedWeaponDamage(20.0f, 1.6f) == doctest::Approx(26.0));
    CHECK(TemperedWeaponDamage(20.0f, 1.2f) == doctest::Approx(22.0));  // LoreRim measured +2 for 1.2
    CHECK(TemperedWeaponDamage(20.0f, 1.0f) == 20.0f);
    CHECK(TemperedWeaponDamage(20.0f, 0.0f) == 20.0f);  // an unfilled ExtraHealth is untempered
    World w;
    for (float d : { 20.0f, 25.0f, 30.0f }) {
        auto& s = w.Item(Kind::Weapon, "Sword");
        s.weaponType = 1;
        s.damage = d;
        s.speed = 1.0f;
    }
    const auto r = w.Build();
    ItemRecord tempered = w.items[0];
    tempered.damage = TemperedWeaponDamage(tempered.damage, 1.6f);
    const auto classes = ClassifyAll(w.effects, nullptr);
    const Cap cap = Grade(MapItem(tempered, w.effects, classes), r.pops);
    CHECK(Get(cap, Col::weapon_damage) == doctest::Approx(2.0 / 3.0));  // 26: above 25, below 30
}

TEST_CASE("effect mapper: a school only for spells, scrolls and staves")
{
    World w;
    auto fireM = Mgef(kArchValueModifier, "Health", "Fire Damage", kFlagDetrimental);
    fireM.resistAV = "FireResist";
    fireM.school = "Destruction";
    const auto fire = w.Add(fireM);
    auto& spell = w.Item(Kind::Spell, "Firebolt");
    World::Fx(spell, fire, 25);
    auto& sword = w.Item(Kind::Weapon, "Sword of Burning");
    sword.weaponType = 1;
    sword.damage = 8;
    sword.enchanted = true;
    World::Fx(sword, fire, 10);
    auto& staff = w.Item(Kind::Weapon, "Staff of Firebolts");
    staff.weaponType = kWeaponStaff;
    staff.enchanted = true;
    World::Fx(staff, fire, 25);
    const auto r = w.Build();
    CHECK(PrimarySchool(r.mappings[0], r.pops) == School::Destruction);
    CHECK(PrimarySchool(r.mappings[1], r.pops) == School::None);
    CHECK(PrimarySchool(r.mappings[2], r.pops) == School::Destruction);
    CHECK(CapOf(r, 0, Col::school_destruction) == 1.0f);
    CHECK(CapOf(r, 1, Col::school_destruction) == 0.0f);
    CHECK(CapOf(r, 2, Col::school_destruction) == 1.0f);
}

TEST_CASE("effect rules: verifier round 1 -- resist-damage names, wrappers by keyword only, helper names")
{
    auto resist = Mgef(kArchValueModifier, "Variable05", "Resist Magicka Damage");
    CHECK(ColOf(resist) == Col::_Count);  // not damage_magicka: beneficial, and "resist"
    auto scriptDamage = Mgef(kArchScript, "", "Damage Magicka");
    CHECK(ColOf(scriptDamage) == Col::damage_magicka);  // a script effect keeps its name
    auto whirl = Mgef(kArchCloak, "", "Whirlwind Cloak");
    whirl.resistAV = "FrostResist";
    CHECK(ClassifyEffect(whirl, nullptr).route == Route::Wrapper);
    auto flame = Mgef(kArchCloak, "", "Flame Cloak");
    flame.keywords = { "MagicDamageFire" };
    CHECK(ColOf(flame) == Col::damage_health_fire);
    CHECK_FALSE(IsHelperName("Blank Slate"));
    CHECK(IsHelperName("Blank"));
    CHECK_FALSE(IsHelperName("Dispel Soul Gems"));
    CHECK(NameColumn("Dispel Soul Gems") == std::nullopt);
    CHECK(NameColumn("Dispel") == Col::cure_dispel);
}

TEST_CASE("effect mapper: overshoot uses the total restored")
{
    World w;
    const auto heal = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health"));
    const auto regenRow = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health over time"));
    auto& p = w.Item(Kind::Potion, "Two-part");
    World::Fx(p, heal, 20);
    World::Fx(p, regenRow, 5, 10);  // 50 more over 10 s
    const auto r = w.Build();
    CHECK(r.mappings[0].restoreAmount[0] == doctest::Approx(70.0));
}

TEST_CASE("effect mapper: wrappers -- payload, own description, nothing to read")
{
    World w;
    auto dmg = Mgef(kArchValueModifier, "Health", "Flame Cloak damage", kFlagDetrimental | kFlagHideInUI);
    dmg.resistAV = "FireResist";
    const auto payload = w.Add(dmg);
    auto cloakM = Mgef(kArchCloak, "", "Flame Cloak");
    cloakM.payload = { EffectRow{ payload, 8, 1, 0, 0 } };
    const auto cloak = w.Add(cloakM);
    auto circleM = Mgef(kArchSpawnHazard, "", "Guardian Circle");
    circleM.description = "Undead entering the circle will flee. Caster heals <20> Health per second.";
    const auto circle = w.Add(circleM);
    const auto empty = w.Add(Mgef(kArchCloak, "", "Odd Cloak"));
    auto& s1 = w.Item(Kind::Spell, "Flame Cloak");
    World::Fx(s1, cloak, 0, 60);
    auto& s2 = w.Item(Kind::Spell, "Guardian Circle");
    World::Fx(s2, circle, 0, 60);
    auto& s3 = w.Item(Kind::Spell, "Odd");
    World::Fx(s3, empty, 0, 60);
    World::Fx(s3, w.Add(Mgef(kArchValueModifier, "Health", "Restore Health")), 5);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::damage_health_fire) > 0.0f);
    CHECK(CapOf(r, 0, Col::aura) == 1.0f);
    CHECK(r.mappings[0].tally.counted == 1);
    CHECK(r.mappings[0].tally.mapped == 1);
    CHECK(CapOf(r, 1, Col::influence_fear) > 0.0f);
    CHECK(CapOf(r, 1, Col::ground_area) == 1.0f);
    CHECK(r.mappings[1].outcomes[0].cls.route == Route::Description);
    CHECK(r.mappings[2].tally.wrapperUnknown == 1);
    CHECK(r.mappings[2].tally.counted == 1);  // only the heal
}

TEST_CASE("effect mapper: restore_health_other from any kept heal that reaches others")
{
    World w;
    const auto heal = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health"));
    const auto hiddenArea = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health", kFlagHideInUI));
    auto& self = w.Item(Kind::Spell, "Healing");
    World::Fx(self, heal, 10, 1);
    auto& breath = w.Item(Kind::Spell, "Breath of Life");
    World::Fx(breath, heal, 50);
    World::Fx(breath, hiddenArea, 50, 0, 50);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::restore_health_other) == 0.0f);
    CHECK(CapOf(r, 1, Col::restore_health_other) == CapOf(r, 1, Col::restore_health));
    CHECK(CapOf(r, 1, Col::area) > 0.0f);
}

TEST_CASE("effect mapper: scope")
{
    World w;
    const auto heal = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health"));
    const auto hidden = w.Add(Mgef(kArchValueModifier, "Health", "Restore Health", kFlagHideInUI));
    auto tome = [&](const char* n) -> ItemRecord& {
        auto& s = w.Item(Kind::Spell, n);
        World::Fx(s, heal, 10);
        return s;
    };
    tome("Healing");
    tome("NPC only").taughtByTome = false;
    tome("Power").spellType = 2;
    tome("Ability").castingType = kCastConstant;
    auto& old = tome("Old dump spell");
    old.taughtByTome.reset();  // a dump without the tome column: cost > 0.5
    auto& hiddenOnly = w.Item(Kind::Spell, "Hidden only");
    World::Fx(hiddenOnly, hidden, 10);
    tome("Dummy test spell");
    w.Item(Kind::Scroll, "Worthless").value = 0;
    w.Item(Kind::Potion, "Unplayable").playable = false;
    auto& fists = w.Item(Kind::Weapon, "Fists");
    fists.weaponType = kWeaponHandToHand;
    w.Item(Kind::SoulGem, "Petty");
    const auto r = w.Build();
    const bool expect[] = { true, false, false, false, true, false, false, false, false, false, true };
    for (std::size_t i = 0; i < std::size(expect); ++i) {
        INFO(w.items[i].name);
        CHECK(r.mappings[i].inScope == expect[i]);
    }
}

TEST_CASE("effect mapper: weapon, ammo, armour, soul gem and light features")
{
    World w;
    auto& hammer = w.Item(Kind::Weapon, "Warhammer");
    hammer.weaponType = 6;
    hammer.keywords = { "WeapTypeWarhammer", "WeapMaterialSilver" };
    hammer.damage = 20;
    hammer.speed = 0.5f;
    hammer.reach = 1.0f;
    hammer.twoHanded = true;
    auto& axe = w.Item(Kind::Weapon, "Battleaxe");
    axe.weaponType = 6;
    axe.damage = 22;
    axe.speed = 0.7f;
    auto& bow = w.Item(Kind::Weapon, "Bow");
    bow.weaponType = 7;
    bow.damage = 9;
    auto& pick = w.Item(Kind::Weapon, "Pickaxe");
    pick.weaponType = 3;
    pick.damage = 5;
    pick.keywords = { "VendorItemTool" };
    auto& staff = w.Item(Kind::Weapon, "Staff");
    staff.weaponType = kWeaponStaff;
    auto& bolt = w.Item(Kind::Ammo, "Steel Bolt");
    bolt.damage = 12;
    auto& arrow = w.Item(Kind::Ammo, "Iron Arrow");
    arrow.damage = 8;
    arrow.ammoNonBolt = true;
    auto& cuirass = w.Item(Kind::Armour, "Cuirass");
    cuirass.slotMask = 0x4 | 0x8;  // body beats hands
    cuirass.armourWeight = ArmourWeight::Heavy;
    cuirass.armorRating = 40;
    cuirass.keywords = { "Survival_ArmorWarm" };
    auto& ring = w.Item(Kind::Armour, "Ring");
    ring.slotMask = 0x40;
    auto& gem = w.Item(Kind::SoulGem, "Grand");
    gem.soulCapacity = 5;
    gem.soulContained = 2;
    auto& torch = w.Item(Kind::Light, "Torch");
    torch.lightRadius = 512;
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::weapon_type_warhammer) == 1.0f);
    CHECK(CapOf(r, 1, Col::weapon_type_battleaxe) == 1.0f);
    CHECK(CapOf(r, 0, Col::weapon_silver) == 1.0f);
    CHECK(CapOf(r, 0, Col::weapon_melee) == 1.0f);
    CHECK(CapOf(r, 0, Col::weapon_two_handed) == 1.0f);
    CHECK(CapOf(r, 0, Col::weapon_damage) == 1.0f);  // alone in its type
    CHECK(CapOf(r, 0, Col::weapon_speed) == 1.0f);   // its own median
    CHECK(CapOf(r, 2, Col::weapon_ranged) == 1.0f);
    CHECK(CapOf(r, 3, Col::weapon_pickaxe) == 1.0f);
    CHECK(CapOf(r, 4, Col::kind_staff) == 1.0f);
    CHECK(CapOf(r, 4, Col::kind_weapon) == 0.0f);
    CHECK(CapOf(r, 5, Col::ammo_bolt) == 1.0f);   // by name, no flag
    CHECK(CapOf(r, 6, Col::ammo_arrow) == 1.0f);  // by the NonBolt flag
    CHECK(CapOf(r, 7, Col::armour_slot_body) == 1.0f);
    CHECK(CapOf(r, 7, Col::armour_slot_hands) == 0.0f);
    CHECK(CapOf(r, 7, Col::armour_weight_heavy) == 1.0f);
    CHECK(CapOf(r, 7, Col::armour_warm) == 1.0f);
    CHECK(CapOf(r, 7, Col::armour_rating) == 1.0f);
    CHECK(CapOf(r, 8, Col::armour_slot_ring) == 1.0f);
    CHECK(CapOf(r, 8, Col::armour_weight_clothing) == 1.0f);
    CHECK(CapOf(r, 9, Col::soulgem_capacity) == 1.0f);
    CHECK(CapOf(r, 9, Col::soulgem_charge) == doctest::Approx(0.4));
    CHECK(CapOf(r, 10, Col::light_radius) == doctest::Approx(std::log1p(512.0) / std::log1p(1024.0)));
    CHECK(DescribesItem(r.caps[0], Kind::Weapon));
    CHECK_FALSE(DescribesItem(r.caps[4], Kind::Weapon));  // an unenchanted staff says nothing
}

TEST_CASE("effect mapper: populations and the cap's text form")
{
    Populations p;
    ItemMapping m;
    m.inScope = true;
    m.kind = Kind::Weapon;
    for (float d : { 4.0f, 8.0f, 8.0f, 10.0f }) {
        m.grouped = { GroupedFeature{ Col::weapon_damage, 1, d, false, 0, 0 } };
        p.Add(m);
    }
    p.Finalize();
    CHECK(p.Percentile(Col::weapon_damage, 1, 8.0f) == doctest::Approx(0.75));
    CHECK(p.Percentile(Col::weapon_damage, 1, 1.0f) == doctest::Approx(0.25));  // below all: still present
    CHECK(p.Percentile(Col::weapon_damage, 2, 1.0f) == 1.0f);                   // no population
    CHECK(p.Median(Col::weapon_damage, 1) == doctest::Approx(8.0));
    CHECK(FormatCap({ Value{ Col::restore, 0.5f }, Value{ Col::kind_potion, 1.0f } }) == "restore=0.5;kind_potion=1");
}

// =============================================================================
// Runtime cross-features
// =============================================================================
TEST_CASE("cross features")
{
    CHECK(Overshoot(100, false, 40, 200) == doctest::Approx(0.3));
    CHECK(Overshoot(20, false, 40, 200) == doctest::Approx(-0.1));
    CHECK(Overshoot(0, true, 40, 200) == doctest::Approx(0.8));  // full restore = max
    CHECK(Overshoot(9e9f, false, 0, 100) == 1.0f);
    CHECK(Overshoot(10, false, 5, 0) == 0.0f);
    CHECK(WeaponCharge(50, 200) == doctest::Approx(0.25));
    CHECK(WeaponCharge(50, 0) == 0.0f);
    CHECK(StackCount(0) == 0.0f);
    CHECK(StackCount(1) == doctest::Approx(std::log1p(1.0) / std::log1p(20.0)));
    CHECK(StackCount(500) == 1.0f);
    CHECK(AmmoMatchesLauncher(false, Launcher::Bow) == 1.0f);
    CHECK(AmmoMatchesLauncher(true, Launcher::Bow) == 0.0f);
    CHECK(AmmoMatchesLauncher(true, Launcher::Crossbow) == 1.0f);
    CHECK(AmmoMatchesLauncher(false, Launcher::None) == 0.0f);
    const SchoolMask mask = SchoolBit(static_cast<std::uint8_t>(School::Destruction));
    CHECK(SchoolFortified(static_cast<std::uint8_t>(School::Destruction), mask) == 1.0f);
    CHECK(SchoolFortified(static_cast<std::uint8_t>(School::Illusion), mask) == 0.0f);
    CHECK(SchoolFortified(0, 0xFF) == 0.0f);
}
