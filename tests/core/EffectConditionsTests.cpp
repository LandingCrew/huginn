// core/EffectConditions and what the mapper does with a row's conditions
// (0.23.30, the "tables" PR): perk rows beside cap(i), GetRandomPercent
// procs scaled by their chance, bane rows out of the generic columns; and the
// same PR's other table fixes -- payloads graded over their wrapper's
// duration, weapon stats within their OCF sub-type, the narrowed rules.
// The condition texts are the LoreRim dump's (Huginn_All-20261010-2116.csv).

#include "core/EffectConditions.h"
#include "core/EffectMapper.h"

#include <doctest/doctest.h>

#include <ostream>
#include <set>
#include <string>

using namespace Huginn::Core::Effect;

namespace
{
    MagicEffectRecord Mgef(int archetype, std::string av, std::string name, std::uint32_t flags = 0, int delivery = 0)
    {
        MagicEffectRecord m;
        static std::uint32_t next = 0x9000;
        m.formId = ++next;
        m.plugin = "Test.esp";
        m.archetype = archetype;
        m.primaryAV = std::move(av);
        m.name = std::move(name);
        m.flags = flags;
        m.detrimental = (flags & kFlagDetrimental) != 0;
        m.hostile = (flags & kFlagHostile) != 0;
        m.delivery = delivery;
        m.payloadKnown = true;
        return m;
    }

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
            it.formId = 0x2000 + static_cast<std::uint32_t>(items.size());
            it.name = std::move(name);
            it.value = 10;
            it.playable = true;
            if (k == Kind::Spell) {
                it.spellType = 0;
                it.castingType = kCastFireAndForget;
                it.magickaCost = 50;
                it.taughtByTome = true;
            }
            items.push_back(std::move(it));
            return items.back();
        }
        static EffectRow& Fx(ItemRecord& it, std::uint32_t e, float mag, std::uint32_t dur = 0, std::string conds = {})
        {
            EffectRow r{ e, mag, dur, 0, 0.0f };
            r.conditions = std::move(conds);
            it.effects.push_back(r);
            return it.effects.back();
        }
        BuildResult Build() const { return BuildCaps(items, effects, nullptr); }
    };

    float CapOf(const BuildResult& r, std::size_t i, Col c) { return Get(r.caps[i], c); }
}

TEST_CASE("conditions: parse the dump's text, OR before AND")
{
    const auto c = ParseConditions("GetRandomPercent()<5 AND GetActorValue(MagicResist)>=40 OR HasKeyword(ActorTypeDragon)!=0 OR "
                                   "HasKeyword(ImmuneParalysis)!=0");
    REQUIRE(c.size() == 4);
    CHECK(c[0].function == "GetRandomPercent");
    CHECK(c[0].op == "<");
    CHECK(c[0].value == 5.0f);
    CHECK_FALSE(c[0].orNext);
    CHECK(c[1].param == "MagicResist");
    CHECK(c[1].orNext);
    CHECK(c[3].param == "ImmuneParalysis");
    const auto clauses = ReadClauses(c);
    REQUIRE(clauses.size() == 2);  // (random) AND (resist OR dragon OR immune)
    CHECK(clauses[0].size() == 1);
    CHECK(clauses[1].size() == 3);

    const auto t = ParseConditions("T.HasPerk(000153D2)==1 AND IsHostileToActor(none)==0 AND GetGlobalValue(X)==Glob");
    REQUIRE(t.size() == 3);
    CHECK(t[0].onTarget);
    CHECK(t[0].param == "000153D2");
    CHECK(t[2].global);
    CHECK(ParseConditions("").empty());
    CHECK(ParseConditions("garbage AND HasPerk(00000001)==1").size() == 1);  // a malformed one is skipped
}

TEST_CASE("conditions: GetRandomPercent chances (0..99)")
{
    CHECK(RandomChance("<", 5) == doctest::Approx(0.05));
    CHECK(RandomChance("<=", 4) == doctest::Approx(0.05));
    CHECK(RandomChance(">=", 75) == doctest::Approx(0.25));
    CHECK(RandomChance(">", 50) == doctest::Approx(0.49));
    CHECK(RandomChance("==", 3) == doctest::Approx(0.01));
    CHECK(RandomChance("<", 150) == 1.0f);
}

TEST_CASE("conditions: the gate -- perk, chance, target family")
{
    // Impact (000153D3): perk AND random AND dual cast.
    const auto impact = ReadGate("GetRandomPercent()>=75 AND T.EffectWasDualCast()==1 AND T.HasPerk(000153D2)==1", false);
    REQUIRE(impact.perks.size() == 1);
    CHECK(impact.perks[0] == std::vector<std::uint32_t>{ 0x000153D2 });
    CHECK(impact.chance == doctest::Approx(0.25));
    CHECK_FALSE(impact.targetFamily);
    // A clause of three perks OR-ed: any of them.
    const auto mage = ReadGate("HasPerk(000D7999)==1 OR HasPerk(000D799A)==1 OR HasPerk(000D799B)==1 AND WornHasKeyword(ArmorCuirass)==0", true);
    REQUIRE(mage.perks.size() == 1);
    CHECK(mage.perks[0].size() == 3);
    // ... an unprefixed HasPerk is the victim's on a touch enchantment.
    CHECK_FALSE(ReadGate("HasPerk(000BABE4)==1", false).PerkGated());
    // "Without the perk" is the default row, not a gate.
    CHECK_FALSE(ReadGate("T.HasPerk(000D799C)==0", false).PerkGated());
    // Requiem's triple shock damage to constructs; undead-only damage.
    CHECK(ReadGate("HasKeyword(ActorTypeDwarven)==1", false).targetFamily);
    CHECK(ReadGate("HasKeyword(ActorTypeUndead)==1 OR IsUndead()==1", false).targetFamily);
    // Not a family gate: humanoids, a negative test, a mixed clause, the caster.
    CHECK_FALSE(ReadGate("HasKeyword(ActorTypeNPC)==1", false).targetFamily);
    CHECK_FALSE(ReadGate("IsUndead()!=1", false).targetFamily);
    CHECK_FALSE(ReadGate("GetActorValue(MagicResist)>=40 OR HasKeyword(ActorTypeDragon)!=0", false).targetFamily);
    CHECK_FALSE(ReadGate("T.HasKeyword(ActorTypeUndead)==1", false).targetFamily);
    // Two random clauses multiply; one OR-ed clause is any of its rolls.
    CHECK(ReadGate("GetRandomPercent()<50 AND GetRandomPercent()<50", false).chance == doctest::Approx(0.25));
    CHECK(ReadGate("GetRandomPercent()<50 OR GetRandomPercent()<50", false).chance == doctest::Approx(0.75));
    CHECK(TestsSubjectDead("T.HasPerk(2A011847)==0 AND T.HasPerk(000581DE)==0 AND GetDead()==1"));
    CHECK_FALSE(TestsSubjectDead("GetDead()==0"));
}

TEST_CASE("mapper: a perk row stays out of cap(i), is carried with its perk, and counts with it")
{
    World w;
    const auto frost = w.Add(Mgef(kArchValueModifier, "Health", "Frost Damage", kFlagDetrimental | kFlagHostile, kDeliveryAimed));
    w.effects[frost].resistAV = "FrostResist";
    auto stagger = Mgef(kArchStagger, "", "Stagger", kFlagHostile | kFlagHideInUI, kDeliveryAimed);
    stagger.conditions = "GetRandomPercent()>=75 AND T.EffectWasDualCast()==1 AND T.HasPerk(000153D2)==1";
    const auto st = w.Add(stagger);
    auto& bolt = w.Item(Kind::Spell, "Ice Spike");
    World::Fx(bolt, frost, 30);
    World::Fx(bolt, st, 0.5f);
    auto& plain = w.Item(Kind::Spell, "Unrelenting Force");
    World::Fx(plain, w.Add(Mgef(kArchStagger, "", "Stagger", kFlagHostile, kDeliveryAimed)), 1.0f);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::control_stagger) == 0.0f);
    CHECK(CapOf(r, 0, Col::damage_health_frost) > 0.0f);
    CHECK(r.mappings[0].outcomes[1].perk);
    CHECK(r.mappings[0].outcomes[1].chance == doctest::Approx(0.25));
    REQUIRE(r.mappings[0].perkRows.size() == 1);
    // The perk row is not in the stagger population: only Unrelenting Force is.
    CHECK(r.pops.Size(Col::control_stagger, 0) == 1);
    const auto perks = GradePerkRows(r.mappings[0], r.pops);
    REQUIRE(perks.size() == 1);
    CHECK(perks[0].col == Col::control_stagger);
    CHECK(perks[0].v > 0.0f);
    const auto without = WithPerks(r.caps[0], perks, [](std::uint32_t) { return false; });
    CHECK(Get(without, Col::control_stagger) == 0.0f);
    const auto with = WithPerks(r.caps[0], perks, [](std::uint32_t id) { return id == 0x000153D2; });
    CHECK(Get(with, Col::control_stagger) == doctest::Approx(perks[0].v));
    CHECK(Get(with, Col::control) == doctest::Approx(perks[0].v));  // and its family
    CHECK(Get(with, Col::damage_health_frost) == CapOf(r, 0, Col::damage_health_frost));
}

TEST_CASE("mapper: an item whose every row needs a perk keeps how it is cast")
{
    World w;
    auto potent = Mgef(kArchSummonCreature, "", "Conjure Potent Flame Atronach", 0, kDeliveryTargetLocation);
    potent.school = "Conjuration";
    const auto e = w.Add(potent);
    auto& s = w.Item(Kind::Spell, "Conjure Potent Flame Atronach");
    World::Fx(s, e, 0, 60, "HasPerk(000CB41A)==1");
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::summon_creature) == 0.0f);
    CHECK(CapOf(r, 0, Col::delivery_target_location) == 1.0f);
    CHECK(CapOf(r, 0, Col::school_conjuration) == 1.0f);
    CHECK(PrimarySchool(r.mappings[0], r.pops) == School::Conjuration);
}

TEST_CASE("mapper: a random proc grades its expected strength")
{
    World w;
    auto para = Mgef(kArchParalysis, "Paralysis", "Paralyze", kFlagHostile | kFlagDetrimental, kDeliveryTouch);
    const auto p = w.Add(para);
    auto& proc = w.Item(Kind::Weapon, "Sword of Paralysis");
    proc.weaponType = 1;
    proc.enchanted = true;
    World::Fx(proc, p, 1, 5, "GetRandomPercent()<5");
    auto& spell = w.Item(Kind::Spell, "Paralyze");
    World::Fx(spell, p, 1, 5);
    const auto r = w.Build();
    CHECK(CapOf(r, 0, Col::control_paralysis) == doctest::Approx(0.05f * CapOf(r, 1, Col::control_paralysis)));
    CHECK(CapOf(r, 0, Col::control_paralysis) < CapOf(r, 1, Col::control_paralysis));
}

TEST_CASE("mapper: a bane row leaves the generic column and its population")
{
    World w;
    auto shock = Mgef(kArchValueModifier, "Health", "Shock Damage", kFlagDetrimental | kFlagHostile, kDeliveryTouch);
    shock.resistAV = "ElectricResist";
    const auto base = w.Add(shock);
    auto bane = shock;
    bane.conditions = "HasKeyword(ActorTypeDwarven)==1";
    const auto b = w.Add(bane);
    auto& sword = w.Item(Kind::Weapon, "Imperial Sword of Shocks");
    sword.weaponType = 1;
    sword.enchanted = true;
    World::Fx(sword, base, 25);
    World::Fx(sword, b, 50);
    auto& spell = w.Item(Kind::Spell, "Sparks");
    World::Fx(spell, w.Add(shock), 30);
    // A target gate on a column that names its target is what the column means.
    auto turn = Mgef(kArchTurnUndead, "", "Turn Undead", kFlagHostile, kDeliveryAimed);
    turn.conditions = "HasKeyword(ActorTypeUndead)==1 OR IsUndead()==1";
    auto& ts = w.Item(Kind::Spell, "Turn Lesser Undead");
    World::Fx(ts, w.Add(turn), 10, 30);
    const auto r = w.Build();
    CHECK(r.mappings[0].outcomes[1].bane);
    CHECK_FALSE(r.mappings[0].outcomes[1].kept);
    CHECK(r.pops.Size(Col::damage_health_shock, 0) == 2);       // 25 and 30, not 50
    CHECK(CapOf(r, 1, Col::damage_health_shock) == 1.0f);        // Sparks' 30 tops the population
    CHECK(CapOf(r, 0, Col::damage_health_shock) == doctest::Approx(0.5));
    CHECK(CapOf(r, 2, Col::influence_turn_undead) > 0.0f);
    CHECK_FALSE(r.mappings[2].outcomes[0].bane);
    // Command Daedra names its target too (verifier of #205).
    CHECK(NamesItsTarget(Col::influence_command));
}

TEST_CASE("mapper: a sub-type on two vanilla types grades within each (type, sub-type)")
{
    World w;
    auto add = [&](int type, float dmg) {
        auto& it = w.Item(Kind::Weapon, "Glaive");
        it.weaponType = type;
        it.damage = dmg;
        it.keywords = { "OCF_WeapTypeGlaive2H" };
    };
    for (int i = 0; i < 20; ++i) add(5, 50.0f + static_cast<float>(i));   // greatswords 50..69
    for (int i = 0; i < 20; ++i) add(6, 100.0f + static_cast<float>(i));  // battleaxes 100..119
    const auto r = w.Build();
    CHECK(CapOf(r, 19, Col::weapon_damage) == 1.0f);                      // best greatsword glaive
    CHECK(CapOf(r, 20, Col::weapon_damage) == doctest::Approx(1.0 / 20));  // worst battleaxe glaive, not above all greatswords
}

TEST_CASE("rules: Turn Undead on a health carrier that deals damage is damage (and so a bane)")
{
    auto m = Mgef(kArchValueModifier, "Health", "Turn Undead", 0, kDeliveryTouch);
    m.description = "Additionally, deals <mag> magic damage to undead.";
    CHECK(ClassifyEffect(m).col == Col::damage_health_magic);
    auto t = Mgef(kArchValueModifier, "Health", "Turn Undead", 0, kDeliveryAimed);
    t.description = "Undead up to level <mag> flee for <dur> seconds.";
    CHECK(ClassifyEffect(t).col == Col::influence_turn_undead);

    World w;
    m.conditions = "HasKeyword(ActorTypeUndead)==1 OR IsUndead()==1";
    const auto e = w.Add(m);
    auto& sw = w.Item(Kind::Weapon, "Imperial Sword of Rebuke");
    sw.weaponType = 1;
    World::Fx(sw, e, 20);
    const auto r = w.Build();
    CHECK(r.mappings[0].outcomes[0].bane);
    CHECK(CapOf(r, 0, Col::damage_health_magic) == 0.0f);
    CHECK(CapOf(r, 0, Col::influence_turn_undead) == 0.0f);
}

TEST_CASE("rules: misroutes fixed -- ash pile, family tags, cure dispel, recharge, narrowed names")
{
    // Requiem's ash-pile helper of a reanimate spell.
    auto ash = Mgef(kArchScript, "", "Disintegrate");
    ash.keywords = { "MagicSummonUndead", "REQ_Reanimate" };
    ash.conditions = "T.HasPerk(2A011847)==0 AND T.HasPerk(000581DE)==0 AND GetDead()==1";
    CHECK(ClassifyEffect(ash).route == Route::Helper);
    // A Nox family keyword yields to a description of another family ...
    auto dispel = Mgef(kArchScript, "", "Dispel Magic");
    dispel.keywords = { "WB_Illusion_Keyword", "Nox_KW_Illusion_Silence" };
    dispel.description = "Dispels all magical effects (except diseases) on friends and foes.";
    CHECK(ClassifyEffect(dispel).col == Col::cure_dispel);
    auto might = Mgef(kArchDualValueModifier, "Fame", "Aura: Might");
    might.keywords = { "TVR_Auras_Keyword_AuraOfVigor", "Nox_KW_Restoration_FortifyAttributes" };
    might.description = "Caster and living or undead allies deal <mag>% more attack damage.";
    CHECK(ClassifyEffect(might).col == Col::fortify_combat_attack_damage);
    // ... but not when the name says the tag's family (Nox's own Silence).
    auto silence = Mgef(kArchScript, "", "Silence");
    silence.keywords = { "Nox_KW_Illusion_Silence" };
    silence.description = "Targets cannot cast spells and are staggered.";
    CHECK(ClassifyEffect(silence).col == Col::control_silence);
    // A vanilla family keyword on a script effect: a reflect aura.
    auto thorns = Mgef(kArchValueModifier, "Fame", "Aura: Thorns");
    thorns.keywords = { "MagicRestoreHealth", "TVR_Auras_Keyword_Aura_3" };
    thorns.description = "Caster and living or undead allies reflect <mag> fire and sun damage in melee.";
    CHECK(ClassifyEffect(thorns).col == Col::defense_reflect);
    // A damage keyword keeps damage the text mentions (verifier of #205):
    // Cyclonic Rift's portal deals shock damage; Consecrate Dead's text has none.
    auto rift = Mgef(kArchScript, "", "Cyclonic Rift");
    rift.keywords = { "MagicDamageShock", "WB_Destruction_Shock" };
    rift.description = "Creates a storm portal for <dur> seconds. If two portals are active, those approaching either get warped across and take <mag> shock damage to Health and Magicka.";
    CHECK(ClassifyEffect(rift).col == Col::damage_health_shock);
    auto consecrate = Mgef(kArchScript, "", "Consecrate Dead");
    consecrate.keywords = { "REQ_SunDamage", "LoreBox_LorerimSon" };
    consecrate.description = "Energy from the sun cleanses and destroys dead bodies. Restores <mag> points of Magicka and Stamina.";
    CHECK(FamilyKey(ClassifyEffect(consecrate).col) == Col::restore);
    // LoreRim's Potion of Dispel: archetype CureDisease.
    auto pod = Mgef(kArchCureDisease, "", "Dispel");
    pod.description = "All temporary effects are dispelled.";
    CHECK(ClassifyEffect(pod).col == Col::cure_dispel);
    auto cure = Mgef(kArchCureDisease, "", "Cure Disease");
    cure.description = "Cures all diseases.";
    CHECK(ClassifyEffect(cure).col == Col::cure_disease);
    // The Varla Stone: MagicAlchRestoreMagicka at 1e9 recharges weapons.
    auto varla = Mgef(kArchScript, "", "Recharge Weapons");
    varla.keywords = { "MagicAlchRestoreMagicka", "MagicAlchBeneficial" };
    varla.description = "Use the Varla Stone to fully recharge any enchantments on your two equipped weapons.";
    CHECK(ClassifyEffect(varla).col == Col::_Count);
    // Narrowed name rules.
    CHECK(NameColumn("Mark") == Col::utility_teleport);
    CHECK(NameColumn("Mark and Recall") == Col::utility_teleport);
    CHECK(NameColumn("Atronach Mark") != Col::utility_teleport);
    CHECK(NameColumn("Transmute Mineral Ore") == Col::utility_transmute);
    CHECK(NameColumn("Master Transmute") == Col::utility_transmute);
    CHECK(NameColumn("Transmutate Fins") != Col::utility_transmute);
    CHECK(NameColumn("Knock") == Col::utility_unlock);
    CHECK(NameColumn("Wind Knockup") == Col::control_stagger);
    CHECK(DescriptionColumn("Player can unlock any Average or lower lock.", false) == Col::utility_unlock);
    CHECK(DescriptionColumn("Losing 50% of your current Health unlocks the seal.", false) != Col::utility_unlock);
    CHECK(DescriptionColumn("You swim <mag>% faster for <dur> seconds.", false) == Col::movement_speed);
    CHECK(DescriptionColumn("Temporarily degrade the target's weapons and armor, reducing its physical damage by <mag>%", true) ==
          Col::weaken_combat_attack_damage);
    CHECK(DescriptionColumn("Channel shadow magic to gain immunity to staggering effects.", false) == Col::defense_resist_stagger);
}

TEST_CASE("mapper: a cloak's payload is graded over the cloak's duration")
{
    World w;
    auto dmg = Mgef(kArchValueModifier, "Health", "Poison Shroud damage", kFlagDetrimental | kFlagHideInUI, kDeliveryTouch);
    dmg.resistAV = "PoisonResist";
    const auto payload = w.Add(dmg);
    auto cloakM = Mgef(kArchCloak, "", "Poison Shroud");
    cloakM.payload = { EffectRow{ payload, 10, 1, 0, 0 } };
    const auto cloak = w.Add(cloakM);
    auto& shroud = w.Item(Kind::Spell, "Poison Shroud");
    World::Fx(shroud, cloak, 0, 60);
    auto& dart = w.Item(Kind::Spell, "Poison Dart");
    auto seen = dmg;
    seen.flags = kFlagDetrimental;  // visible: a spell needs one to be in scope
    World::Fx(dart, w.Add(seen), 300);  // 300 at once
    auto& sting = w.Item(Kind::Spell, "Poison Sting");
    World::Fx(sting, w.Add(seen), 900);
    const auto r = w.Build();
    // 10/s x 60 s = 600: between the 300 and the 900.
    CHECK(CapOf(r, 0, Col::damage_health_poison) == doctest::Approx(2.0 / 3.0));
    REQUIRE(!r.mappings[0].rows.empty());
    CHECK(r.mappings[0].rows[0].duration == 60u);
}

TEST_CASE("mapper: weapon stats within their OCF sub-type, the vanilla type as fallback")
{
    World w;
    auto add = [&](const char* name, float dmg, std::vector<std::string> kws) {
        auto& it = w.Item(Kind::Weapon, name);
        it.weaponType = 6;
        it.damage = dmg;
        it.keywords = std::move(kws);
    };
    // 20 quarterstaffs at 100..119 and two warhammers at 140 and 150.
    for (int i = 0; i < 20; ++i) add("Quarterstaff", 100.0f + static_cast<float>(i), { "WeapTypeWarhammer", "OCF_WeapTypeQuarterstaff2H", "OCF_WeapTypePole2H" });
    add("Warhammer A", 140.0f, { "WeapTypeWarhammer", "OCF_WeapTypeWarhammer2H" });
    add("Warhammer B", 150.0f, { "WeapTypeWarhammer", "OCF_WeapTypeWarhammer2H" });
    // A lone sub-type falls back to its type's whole population (22 + 1).
    add("Club", 120.0f, { "WeapTypeWarhammer", "OCF_WeapTypeClub2H" });
    const auto r = w.Build();
    CHECK(CapOf(r, 19, Col::weapon_damage) == 1.0f);                       // the best quarterstaff
    CHECK(CapOf(r, 0, Col::weapon_damage) == doctest::Approx(1.0 / 20));   // the worst
    CHECK(CapOf(r, 0, Col::weapon_type_warhammer) == 1.0f);                // still typed a warhammer
    CHECK(CapOf(r, 20, Col::weapon_damage) == doctest::Approx(0.5));       // warhammers among warhammers
    CHECK(CapOf(r, 22, Col::weapon_damage) == doctest::Approx(21.0 / 23)); // 120 in all 23
}
