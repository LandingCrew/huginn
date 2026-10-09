// core/NeedEvaluator.h: named cases for the inputs needs.csv's r3_input
// defines, the skip-gate signature, Advance and TimeDriven, and the text
// record. The replayed fixtures (with expectations from the independent
// oracle) are in NeedFixtureTests.cpp.

#include "core/NeedEvaluator.h"
#include "core/NeedSnapshotIO.h"

#include <doctest/doctest.h>

#include <cmath>
#include <ostream>
#include <set>
#include <string>

using namespace Huginn::Core::Needs;

namespace
{
    const CurveTable kCurves = DefaultCurves();

    float In(const NeedSnapshot& s, NeedId id) { return NeedInputs(s)[Index(id)]; }
    float Val(const NeedSnapshot& s, NeedId id) { return EvaluateNeeds(s, kCurves).value[Index(id)]; }
}

TEST_CASE("vitals: deficits use the held magicka and stamina, health raw")
{
    NeedSnapshot s;
    s.health = 0.3f;
    s.magicka = 0.2f;
    s.magickaHeld = 0.1f;
    s.stamina = 0.9f;
    s.staminaHeld = 0.9f;
    CHECK(In(s, NeedId::health_deficit) == doctest::Approx(0.7));
    CHECK(In(s, NeedId::magicka_deficit) == doctest::Approx(0.9));
    CHECK(In(s, NeedId::stamina_deficit) == doctest::Approx(0.1));
    CHECK(Val(s, NeedId::health_deficit) == doctest::Approx(1.0 / (1.0 + std::exp(-10.0 * 0.2))).epsilon(1e-5));
}

TEST_CASE("rates are per max and per second; a zero max reads 0")
{
    NeedSnapshot s;
    s.maxHealth = 200.0f;
    s.damageRate = 10.0f;
    s.healingRate = 4.0f;
    CHECK(In(s, NeedId::health_falling) == doctest::Approx(0.03));
    CHECK(Val(s, NeedId::health_falling) == doctest::Approx(0.5));
    s.maxHealth = 0.0f;
    CHECK(In(s, NeedId::health_falling) == 0.0f);
    s.maxMagicka = 300.0f;
    s.magickaUsageRate = 30.0f;
    s.magickaRegenRate = 15.0f;
    CHECK(In(s, NeedId::magicka_burn) == doctest::Approx(0.05));
    s.dmgFire = 50.0f;
    s.maxHealth = 250.0f;
    CHECK(In(s, NeedId::fire_damage_rate) == doctest::Approx(0.2));
}

TEST_CASE("restore pending: remaining over the deficit, 0 with nothing pending or nothing missing, capped at 10")
{
    NeedSnapshot s;
    s.maxHealth = 100.0f;
    s.health = 0.5f;
    CHECK(In(s, NeedId::restore_pending_health) == 0.0f);
    s.restoreHealthPending = 25.0f;
    CHECK(In(s, NeedId::restore_pending_health) == doctest::Approx(0.5));
    s.health = 0.98f;  // 2 points missing: covered 12.5 times over, capped
    CHECK(In(s, NeedId::restore_pending_health) == 10.0f);
    s.health = 0.75f;
    s.restoreHealthPending = 25.0f;  // 25 missing, 25 pending: exactly covered
    CHECK(In(s, NeedId::restore_pending_health) == doctest::Approx(1.0));
    // 0.23.16: under one point missing there is nothing to cover -- 0, not
    // "covered" (it read 1.00 at full health while a restore ticked).
    s.health = 1.0f;
    CHECK(In(s, NeedId::restore_pending_health) == 0.0f);
    s.health = 0.995f;
    CHECK(In(s, NeedId::restore_pending_health) == 0.0f);
    s.magicka = 1.0f;
    s.maxMagicka = 200.0f;
    s.restoreMagickaPending = 50.0f;
    CHECK(In(s, NeedId::restore_pending_magicka) == 0.0f);
    s.stamina = 1.0f;
    s.maxStamina = 200.0f;
    s.restoreStaminaPending = 50.0f;
    CHECK(In(s, NeedId::restore_pending_stamina) == 0.0f);
}

TEST_CASE("enemy casting an element: a step while seen, decayed below one level by 2 s, NEVER reads 0")
{
    NeedSnapshot s;
    CHECK(Val(s, NeedId::enemy_casting_fire) == 0.0f);
    s.castFireAgo = 0.0f;
    CHECK(Val(s, NeedId::enemy_casting_fire) == 1.0f);
    s.castFireAgo = 1.0f;  // decay tau 0.5 s: e^-2 = 0.135335
    CHECK(Val(s, NeedId::enemy_casting_fire) == doctest::Approx(0.135335).epsilon(1e-5));
    s.castFireAgo = 2.0f;
    CHECK(SignatureLevel(Val(s, NeedId::enemy_casting_fire)) == 0);
}

TEST_CASE("survival: off gates everything; the CC meter beats the stage; stages clamp")
{
    NeedSnapshot s;
    s.hungerStage = 5;
    s.coldRaw = 900.0f;
    s.warmth = 0.0f;
    CHECK(In(s, NeedId::hunger) == 0.0f);
    CHECK(In(s, NeedId::cold) == 0.0f);
    CHECK(In(s, NeedId::warmth_deficit) == 0.0f);
    s.survival = true;
    CHECK(In(s, NeedId::hunger) == doctest::Approx(1.0));
    CHECK(In(s, NeedId::cold) == doctest::Approx(0.9));
    CHECK(Val(s, NeedId::cold) == doctest::Approx(0.81));
    s.hungerRaw = 340.0f;
    CHECK(In(s, NeedId::hunger) == doctest::Approx(0.34));
    s.fatigueStage = -3;
    CHECK(In(s, NeedId::fatigue) == 0.0f);
    s.fatigueStage = 9;
    CHECK(In(s, NeedId::fatigue) == 1.0f);
    CHECK(In(s, NeedId::warmth_deficit) == 1.0f);
    s.warmth = 300.0f;
    CHECK(In(s, NeedId::warmth_deficit) == 0.0f);
}

TEST_CASE("combat timers: onset only in combat, ended-recent only out of it, downtime needs no hostile")
{
    NeedSnapshot s;  // never fought since the load
    CHECK(Val(s, NeedId::combat_onset) == 0.0f);
    CHECK(Val(s, NeedId::combat_ended_recent) == 0.0f);
    CHECK(Val(s, NeedId::downtime) == 1.0f);  // a long time out of combat
    s.inCombat = true;
    s.combatStartAgo = 0.0f;
    CHECK(Val(s, NeedId::combat_onset) == 1.0f);
    CHECK(Val(s, NeedId::downtime) < 0.01f);
    s.combatStartAgo = 30.0f;
    CHECK(Val(s, NeedId::combat_onset) < 0.025f);  // gone by ~30 s
    s.inCombat = false;
    s.combatEndAgo = 15.0f;
    CHECK(Val(s, NeedId::combat_ended_recent) == doctest::Approx(std::exp(-1.0)).epsilon(1e-5));
    // logistic c = 20 s, slope 0.3: 1 / (1 + e^1.5) = 0.182426
    CHECK(Val(s, NeedId::downtime) == doctest::Approx(0.182426).epsilon(1e-5));
    s.enemyCount = 1;  // a hostile still tracked (the crosshair one): no downtime
    CHECK(In(s, NeedId::downtime) == 0.0f);
}

TEST_CASE("distance bands: no hostile is neither close nor far")
{
    NeedSnapshot s;
    CHECK(In(s, NeedId::enemy_close) == 4096.0f);
    // enemy_far reads 0 with no hostile (it read 1 before 0.23.15, an always-on
    // episode through every stretch of exploring).
    CHECK(In(s, NeedId::enemy_far) == 0.0f);
    CHECK(Val(s, NeedId::enemy_far) < 1e-3f);
    s.closestEnemy = 3000.0f;
    CHECK(Val(s, NeedId::enemy_far) > 0.999f);
    s.closestEnemy = -1.0f;
    CHECK(Val(s, NeedId::enemy_close) < 1e-6f);
    s.closestEnemy = 600.0f;
    CHECK(Val(s, NeedId::enemy_mid) == 1.0f);
    s.closestEnemy = 150.0f;
    CHECK(Val(s, NeedId::enemy_close) > 0.9f);
    s.closestEnemy = 0.0f;
    CHECK(In(s, NeedId::enemy_close) == 0.0f);
}

TEST_CASE("boss fight: one hostile with an estimate only")
{
    NeedSnapshot s;
    s.enemyCount = 1;
    s.soleHostileTtk = 45.0f;
    CHECK(In(s, NeedId::boss_fight) == 45.0f);
    CHECK(Val(s, NeedId::boss_fight) > 0.9f);
    s.enemyCount = 2;
    CHECK(In(s, NeedId::boss_fight) == 0.0f);
    s.enemyCount = 1;
    s.soleHostileTtk = -1.0f;
    CHECK(In(s, NeedId::boss_fight) == 0.0f);
}

TEST_CASE("families and schools are bit reads")
{
    NeedSnapshot s;
    s.families = FamilyBit(Family::Undead) | FamilyBit(Family::Humanoid) | FamilyBit(Family::ElementShock);
    CHECK(Val(s, NeedId::target_undead) == 1.0f);
    CHECK(Val(s, NeedId::target_humanoid) == 1.0f);
    CHECK(Val(s, NeedId::target_element_shock) == 1.0f);
    CHECK(Val(s, NeedId::target_dragon) == 0.0f);
    s.handSchools = SchoolBit(School::Illusion);
    CHECK(Val(s, NeedId::loadout_illusion) == 1.0f);
    CHECK(Val(s, NeedId::loadout_destruction) == 0.0f);
}

TEST_CASE("environment: darkness off in open daylight, underwater by breath, drop ahead unmeasured reads 0")
{
    NeedSnapshot s;
    s.light = 0.1f;
    CHECK(In(s, NeedId::darkness) == doctest::Approx(0.9));
    s.openDaylight = true;
    CHECK(In(s, NeedId::darkness) == 0.0f);
    s.submergedFor = 10.0f;
    CHECK(In(s, NeedId::underwater) == 0.0f);  // not under: the timer is stale
    s.underwater = true;
    CHECK(In(s, NeedId::underwater) == doctest::Approx(0.5));
    CHECK(Val(s, NeedId::underwater) == doctest::Approx(0.5));
    CHECK(In(s, NeedId::drop_ahead) == 0.0f);
    s.dropAhead = 900.0f;
    CHECK(Val(s, NeedId::drop_ahead) > 0.9f);
    s.dropAhead = 40.0f;
    CHECK(Val(s, NeedId::drop_ahead) < 0.01f);
    s.vampireStage = 3;
    CHECK(Val(s, NeedId::vampire_sun_exposure) == 1.0f);
    s.openDaylight = false;
    CHECK(Val(s, NeedId::vampire_sun_exposure) == 0.0f);
}

TEST_CASE("equipment: ammo by the launcher in hand; hands empty counts every weapon")
{
    NeedSnapshot s;
    s.arrows = 0;
    CHECK(In(s, NeedId::ammo_low) == 0.0f);  // no launcher
    s.bow = true;
    CHECK(In(s, NeedId::ammo_low) == 1.0f);
    s.arrows = 10;
    CHECK(Val(s, NeedId::ammo_low) == doctest::Approx(0.5));
    s.arrows = 60;
    CHECK(In(s, NeedId::ammo_low) == 0.0f);
    s.bow = false;
    s.crossbow = true;
    s.bolts = 5;
    CHECK(In(s, NeedId::ammo_low) == doctest::Approx(0.8));
    CHECK(Val(s, NeedId::hands_empty) == 0.0f);
    CHECK(Val(s, NeedId::loadout_archery) == 1.0f);
    s.crossbow = false;
    CHECK(Val(s, NeedId::hands_empty) == 1.0f);
    s.staff = true;
    CHECK(Val(s, NeedId::hands_empty) == 0.0f);
    s.enchantedWeapon = true;
    s.weaponCharge = 0.25f;
    CHECK(Val(s, NeedId::weapon_charge_deficit) == doctest::Approx(0.5));
}

TEST_CASE("deferred needs read input 0 whatever the snapshot")
{
    NeedSnapshot s;
    s.survival = true;
    s.hungerStage = 5;
    s.sneaking = true;
    s.targetHealth = 0.1f;
    for (std::size_t i = 0; i < kNeedCount; ++i) {
        if (!kNeeds[i].deferred) continue;
        INFO(kNeeds[i].id);
        CHECK(NeedInputs(s)[i] == 0.0f);
    }
}

TEST_CASE("signature: 0.05 steps, 21 levels")
{
    CHECK(SignatureLevel(0.0f) == 0);
    CHECK(SignatureLevel(0.024f) == 0);
    CHECK(SignatureLevel(0.026f) == 1);
    CHECK(SignatureLevel(0.5f) == 10);
    CHECK(SignatureLevel(1.0f) == 20);
    CHECK(SignatureLevel(2.0f) == 20);
    CHECK(SignatureLevel(-1.0f) == 0);
    CHECK(SignatureLevel(std::nanf("")) == 0);
    // A wobble inside a step does not move the signature; crossing one does.
    NeedSnapshot a;
    a.health = 0.48f;
    NeedSnapshot b = a;
    b.health = 0.4801f;
    CHECK(Signature(EvaluateNeeds(a, kCurves).value) == Signature(EvaluateNeeds(b, kCurves).value));
    b.health = 0.40f;
    CHECK(Signature(EvaluateNeeds(a, kCurves).value) != Signature(EvaluateNeeds(b, kCurves).value));
}

TEST_CASE("signature deadband for the [Needs] log line: on/off always, else 5 steps from the last line (0.23.16)")
{
    NeedSignature logged{};
    const auto dark = static_cast<std::size_t>(NeedId::darkness);
    const auto hunger = static_cast<std::size_t>(NeedId::hunger);
    logged[dark] = 18;  // darkness 0.90
    NeedSignature now = logged;
    CHECK_FALSE(SignatureMoved(logged, now, 5));
    now[dark] = 14;  // 0.68 / 0.70: torchlight flicker, 4 steps
    CHECK_FALSE(SignatureMoved(logged, now, 5));
    now[dark] = 13;  // 5 steps
    CHECK(SignatureMoved(logged, now, 5));
    now = logged;
    now[hunger] = 1;  // a need turning on is always a line
    CHECK(SignatureMoved(logged, now, 5));
    now = logged;
    now[dark] = 0;  // and turning off
    CHECK(SignatureMoved(logged, now, 5));
    // Deadband 1 is the plain "the signature changed".
    now = logged;
    now[dark] = 17;
    CHECK(SignatureMoved(logged, now, 1));
}

TEST_CASE("Advance moves the timers and decays the damage sums, nothing else")
{
    NeedSnapshot s;
    s.inCombat = true;
    s.combatStartAgo = 1.0f;
    s.castFrostAgo = 0.5f;
    s.castFireAgo = 0.25f;
    s.castShockAgo = 0.75f;
    s.combatEndAgo = 9.0f;
    s.dmgFire = 3.0f;
    s.dmgFrost = 6.0f;
    s.dmgMagic = 9.0f;
    s.dmgPhysical = 12.0f;
    s.underwater = true;
    s.submergedFor = 2.0f;
    s.dmgShock = 30.0f;
    s.health = 0.4f;
    const auto a = Advance(s, 3.0f);
    CHECK(a.combatStartAgo == doctest::Approx(4.0));
    CHECK(a.castFrostAgo == doctest::Approx(3.5));
    CHECK(a.submergedFor == doctest::Approx(5.0));
    CHECK(a.dmgShock == doctest::Approx(30.0 * std::exp(-1.0)).epsilon(1e-5));
    CHECK(a.health == 0.4f);
    // Every timer and every sum moves (one forgotten would be a need that
    // never settles, or settles too early).
    CHECK(a.castFireAgo == doctest::Approx(3.25));
    CHECK(a.castShockAgo == doctest::Approx(3.75));
    CHECK(a.combatEndAgo == doctest::Approx(12.0));
    const double k = std::exp(-1.0);
    CHECK(a.dmgFire == doctest::Approx(3.0 * k).epsilon(1e-5));
    CHECK(a.dmgFrost == doctest::Approx(6.0 * k).epsilon(1e-5));
    CHECK(a.dmgMagic == doctest::Approx(9.0 * k).epsilon(1e-5));
    CHECK(a.dmgPhysical == doctest::Approx(12.0 * k).epsilon(1e-5));
    NeedSnapshot never;
    CHECK(Advance(never, 5.0f).castFireAgo >= kNever);
    NeedSnapshot dry = s;
    dry.underwater = false;
    CHECK(Advance(dry, 3.0f).submergedFor == 2.0f);
    CHECK(DiffSnapshots(Advance(s, 0.0f), s).empty());
}

TEST_CASE("TimeDriven: true while a timer or a decay still moves the signature, false once settled")
{
    NeedSnapshot s;  // out of combat, never fought: downtime already saturated
    CHECK_FALSE(TimeDriven(s, kCurves));
    s.health = 0.3f;  // a sensor value, not time-driven
    CHECK_FALSE(TimeDriven(s, kCurves));
    s.inCombat = true;
    s.combatStartAgo = 2.0f;
    CHECK(TimeDriven(s, kCurves));  // combat onset will fade
    s.combatStartAgo = 120.0f;
    s.enemyCount = 2;
    CHECK_FALSE(TimeDriven(s, kCurves));  // onset gone, mid-fight is steady
    s.dmgFire = 40.0f;
    CHECK(TimeDriven(s, kCurves));  // the fire sum decays
    s.dmgFire = 0.0f;
    s.inCombat = false;
    s.enemyCount = 0;
    s.combatEndAgo = 3.0f;
    CHECK(TimeDriven(s, kCurves));  // ended-recent fades, downtime rises
    s.combatEndAgo = 600.0f;
    CHECK_FALSE(TimeDriven(s, kCurves));
    s.underwater = true;
    s.submergedFor = 1.0f;
    CHECK(TimeDriven(s, kCurves));  // breath running out
}

TEST_CASE("snapshot text: every field round-trips; errors are reported")
{
    NeedSnapshot s;
    s.health = 0.123456789f;
    s.maxHealth = 312.5f;
    s.families = 0x7fff;
    s.handSchools = 5;
    s.fatigueStage = -2;
    s.inCombat = true;
    s.closestEnemy = -1.0f;
    s.combatEndAgo = kNever;
    s.dropAhead = 1234.5678f;
    const std::string text = "# header\n" + WriteSnapshot("one", s) + "\n" + WriteSnapshot("two", NeedSnapshot{});
    std::string err;
    const auto back = ReadSnapshots(text, err);
    CHECK(err.empty());
    REQUIRE(back.size() == 2);
    CHECK(back[0].label == "one");
    CHECK(DiffSnapshots(back[0].snapshot, s).empty());
    CHECK(DiffSnapshots(back[1].snapshot, NeedSnapshot{}).empty());

    // Fields left out keep their defaults.
    const auto partial = ReadSnapshots("snapshot p\nhealth 0.5\nend\n", err);
    REQUIRE(partial.size() == 1);
    CHECK(err.empty());
    CHECK(partial[0].snapshot.health == 0.5f);
    CHECK(partial[0].snapshot.maxHealth == NeedSnapshot{}.maxHealth);

    (void)ReadSnapshots("snapshot x\nnope 1\nend\n", err);
    CHECK(err.find("unknown field") != std::string::npos);
    (void)ReadSnapshots("snapshot x\ninCombat 2\nend\n", err);
    CHECK(err.find("bad value") != std::string::npos);
    (void)ReadSnapshots("snapshot x\nhealth 0.5\n", err);
    CHECK(err.find("no 'end'") != std::string::npos);
    (void)ReadSnapshots("health 0.5\n", err);
    CHECK(!err.empty());
}

TEST_CASE("field table: unique names, every name readable")
{
    std::set<std::string_view> names;
    for (const auto& f : kFields) names.insert(f.name);
    CHECK(names.size() == kFields.size());
    CHECK(kFields.size() == 82);
}
