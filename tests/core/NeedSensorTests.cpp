// core/DropAhead.h and core/NeedSensorMath.h: the arithmetic the R3 sensors
// run on the game side (state/DropAheadProbe.cpp, StateManager polls).

#include "core/DropAhead.h"
#include "core/NeedSensorMath.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <ostream>

using namespace Huginn::Core::Needs;

TEST_CASE("drop ahead: probe points along the movement, else the facing, at waist height")
{
    const DropProbeConfig cfg;
    // Moving east (+X) faster than the threshold: the movement wins over the facing.
    auto dir = ProbeDirection({ 300.0f, 0.0f, -50.0f }, 0.0f, cfg.minMoveSpeed);
    CHECK(dir.x == doctest::Approx(1.0));
    CHECK(dir.y == doctest::Approx(0.0));
    // Standing still facing north (yaw 0): +Y.
    dir = ProbeDirection({ 5.0f, 5.0f, 0.0f }, 0.0f, cfg.minMoveSpeed);
    CHECK(dir.x == doctest::Approx(0.0));
    CHECK(dir.y == doctest::Approx(1.0));
    // Facing east is yaw pi/2 (clockwise from north).
    dir = ProbeDirection({}, std::numbers::pi_v<float> / 2.0f, cfg.minMoveSpeed);
    CHECK(dir.x == doctest::Approx(1.0));
    CHECK(dir.y == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(std::hypot(dir.x, dir.y) == doctest::Approx(1.0));

    const auto starts = ProbeStarts({ 100.0f, 200.0f, 50.0f }, Dir2{ 0.0f, 1.0f }, cfg);
    CHECK(starts[0].y == doctest::Approx(270.0));   // ~1 m
    CHECK(starts[1].y == doctest::Approx(375.0));   // ~2.5 m
    CHECK(starts[2].y == doctest::Approx(480.0));   // ~4 m
    for (const auto& p : starts) {
        CHECK(p.x == doctest::Approx(100.0));
        CHECK(p.z == doctest::Approx(114.0));       // feet + 64
    }
    CHECK(ProbeEnd(starts[0], cfg).z == doctest::Approx(114.0 - 4000.0));

    const auto v = HorizontalVelocity({ 0.0f, 0.0f, 0.0f }, { 10.0f, -20.0f, 99.0f }, 0.1f);
    CHECK(v.x == doctest::Approx(100.0));
    CHECK(v.y == doctest::Approx(-200.0));
    CHECK(v.z == 0.0f);
    CHECK(HorizontalVelocity({}, { 1.0f, 1.0f, 1.0f }, 0.0f).x == 0.0f);
}

TEST_CASE("drop ahead: feet Z minus the surface, the largest over the probes")
{
    const DropProbeConfig cfg;
    const float feet = 1000.0f;
    // Flat ground: every probe hits at the feet.
    std::array<ProbeHit, 3> flat{ { { true, 1000.0f }, { true, 1000.0f }, { true, 1000.0f } } };
    CHECK(DropAhead(feet, flat, cfg) == 0.0f);
    // A cliff edge between the first and second point.
    std::array<ProbeHit, 3> cliff{ { { true, 998.0f }, { true, 200.0f }, { true, 150.0f } } };
    CHECK(DropAhead(feet, cliff, cfg) == doctest::Approx(850.0));
    // Rising ground is no drop.
    std::array<ProbeHit, 3> slope{ { { true, 1020.0f }, { true, 1040.0f }, { true, 1060.0f } } };
    CHECK(DropAhead(feet, slope, cfg) == 0.0f);
    // No hit at all: the bottom of the ray, a very big drop.
    std::array<ProbeHit, 3> void3{ { { true, 1000.0f }, {}, {} } };
    CHECK(DropAhead(feet, void3, cfg) == doctest::Approx(4000.0 - 64.0));
    // Deep water below a bridge: measured to the surface, not the river bed.
    std::array<ProbeHit, 3> bridge{ { { true, 1000.0f }, { true, -500.0f, true, 600.0f }, { true, -500.0f, true, 600.0f } } };
    CHECK(DropAhead(feet, bridge, cfg) == doctest::Approx(400.0));
    // Water below the hit (a pool under a ledge already counted) changes nothing.
    std::array<ProbeHit, 3> dry{ { { true, 700.0f, true, 100.0f }, {}, {} } };
    dry[1] = { true, 1000.0f };
    dry[2] = { true, 1000.0f };
    CHECK(DropAhead(feet, dry, cfg) == doctest::Approx(300.0));
    // Standing in water up to the waist: the surface is above the feet, no drop.
    std::array<ProbeHit, 3> wading{ { { true, 950.0f, true, 1050.0f }, { true, 950.0f, true, 1050.0f }, { true, 950.0f, true, 1050.0f } } };
    CHECK(DropAhead(feet, wading, cfg) == 0.0f);
    // Water over the ray's bottom with no hit (open sea from a cliff).
    std::array<ProbeHit, 3> sea{ { { true, 1000.0f }, { false, 0.0f, true, 0.0f }, { false, 0.0f, true, 0.0f } } };
    CHECK(DropAhead(feet, sea, cfg) == doctest::Approx(1000.0));
    // A NaN reading never makes a drop.
    std::array<ProbeHit, 3> bad{ { { true, std::nanf("") }, { true, 1000.0f }, { true, 1000.0f } } };
    CHECK(DropAhead(feet, bad, cfg) == 0.0f);
    // Water above the probe's start (the start is under water): no drop.
    std::array<ProbeHit, 3> drowned{ { { false, 0.0f, true, 2000.0f }, { true, 1000.0f }, { true, 1000.0f } } };
    CHECK(DropAhead(feet, drowned, cfg) == 0.0f);
}

TEST_CASE("drop ahead: an unknown probe is nothing, never a cliff")
{
    const DropProbeConfig cfg;
    const float feet = 1000.0f;
    ProbeHit unknown;
    unknown.known = false;
    // Uphill: the ground rises past waist height before the 2nd and 3rd
    // points, so the reachability pick blocks them; the 1st reads flat.
    std::array<ProbeHit, 3> uphill{ { { true, 1010.0f }, unknown, unknown } };
    CHECK(DropAhead(feet, uphill, cfg) == 0.0f);
    // A wall or a door right in front: nothing known at all -> not measured.
    std::array<ProbeHit, 3> wall{ { unknown, unknown, unknown } };
    CHECK(DropAhead(feet, wall, cfg) == -1.0f);
    // Stairs going down: a few steps, a small drop.
    std::array<ProbeHit, 3> stairs{ { { true, 984.0f }, { true, 960.0f }, { true, 936.0f } } };
    CHECK(DropAhead(feet, stairs, cfg) == doctest::Approx(64.0));
    // Stairs going up past the waist: the far points blocked, the near one a step up.
    std::array<ProbeHit, 3> upstairs{ { { true, 1016.0f }, unknown, unknown } };
    CHECK(DropAhead(feet, upstairs, cfg) == 0.0f);
    // A probe that ran out of recasts (a crowd) is unknown; the others count.
    std::array<ProbeHit, 3> crowd{ { { true, 1000.0f }, unknown, { true, 400.0f } } };
    CHECK(DropAhead(feet, crowd, cfg) == doctest::Approx(600.0));
    // An unknown probe's fields are ignored even when they would read a void.
    ProbeHit unknownVoid;
    unknownVoid.known = false;
    unknownVoid.hit = false;
    std::array<ProbeHit, 3> ignored{ { { true, 1000.0f }, unknownVoid, unknownVoid } };
    CHECK(DropAhead(feet, ignored, cfg) == 0.0f);
    CHECK(ProbeOrigin({ 1.0f, 2.0f, 3.0f }, cfg).z == doctest::Approx(67.0));
}

TEST_CASE("drop ahead: the movement threshold is 20 units/s")
{
    const DropProbeConfig cfg;
    CHECK(cfg.minMoveSpeed == 20.0f);
    // 25 units/s east, facing north: just over the threshold, the movement wins.
    auto dir = ProbeDirection({ 25.0f, 0.0f, 0.0f }, 0.0f, cfg.minMoveSpeed);
    CHECK(dir.x == doctest::Approx(1.0));
    // 19 units/s: the facing.
    dir = ProbeDirection({ 19.0f, 0.0f, 0.0f }, 0.0f, cfg.minMoveSpeed);
    CHECK(dir.y == doctest::Approx(1.0));
}

TEST_CASE("decaying sum: tau 3 s, adds on top of what is left")
{
    DecayingSum d;
    CHECK(d.At(10.0) == 0.0f);
    d.Add(10.0, 30.0f);
    CHECK(d.At(10.0) == doctest::Approx(30.0));
    CHECK(d.At(13.0) == doctest::Approx(30.0 * std::exp(-1.0)).epsilon(1e-5));
    d.Add(13.0, 10.0f);
    CHECK(d.At(13.0) == doctest::Approx(30.0 * std::exp(-1.0) + 10.0).epsilon(1e-5));
    CHECK(d.At(12.0) == doctest::Approx(d.At(13.0)));  // no growth backwards
    d.Add(14.0, -5.0f);                                  // negative amounts ignored
    CHECK(d.At(14.0) == doctest::Approx((30.0 * std::exp(-1.0) + 10.0) * std::exp(-1.0 / 3.0)).epsilon(1e-5));
    CHECK(d.At(1.0e6) == 0.0f);
    d.Reset();
    CHECK(d.At(14.0) == 0.0f);
}

TEST_CASE("sole hostile time-to-kill: needs 5 s of the same actor, then health over the fall rate")
{
    SoleHostileTtk t;
    CHECK(t.Update(0x1234, 1.0f, 100.0) == -1.0f);  // starts the window
    CHECK(t.Update(0x1234, 0.9f, 103.0) == -1.0f);  // too short
    // 10 s, bar 1.0 -> 0.8: 0.02/s, 0.8 left -> 40 s
    CHECK(t.Update(0x1234, 0.8f, 110.0) == doctest::Approx(40.0));
    // Another actor: start again.
    CHECK(t.Update(0x9999, 0.5f, 111.0) == -1.0f);
    // No damage in 6 s: a fight going nowhere.
    CHECK(t.Update(0x9999, 0.5f, 117.0) == kNever);
    // A sliver of regeneration is still no progress; a real heal restarts.
    CHECK(t.Update(0x9999, 0.52f, 118.0) == kNever);
    CHECK(t.Update(0x9999, 0.7f, 119.0) == -1.0f);
    // Not exactly one hostile.
    CHECK(t.Update(0, 0.5f, 120.0) == -1.0f);
    CHECK(t.Update(0x9999, 0.5f, 121.0) == -1.0f);  // the window restarted
}

TEST_CASE("seconds since and encumbrance")
{
    CHECK(SecondsSince(-1.0, 50.0) == kNever);
    CHECK(SecondsSince(40.0, 50.0) == doctest::Approx(10.0));
    CHECK(SecondsSince(60.0, 50.0) == 0.0f);
    CHECK(EncumbranceRatio(285.0f, 300.0f) == doctest::Approx(0.95));
    CHECK(EncumbranceRatio(10.0f, 0.0f) == 0.0f);
    CHECK(EncumbranceRatio(-1.0f, 300.0f) == 0.0f);
}
