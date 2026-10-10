// core/DropAhead.h and core/NeedSensorMath.h: the arithmetic the R3 sensors
// run on the game side (state/DropAheadProbe.cpp, StateManager polls).

#include "core/BenchKind.h"
#include "core/DropAhead.h"
#include "core/NeedSensorMath.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <cmath>
#include <numbers>
#include <ostream>
#include <string_view>
#include <vector>

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
    // Shallow water below a bridge (100 deep): measured to the surface, not
    // the river bed.
    std::array<ProbeHit, 3> bridge{ { { true, 1000.0f }, { true, 500.0f, true, 600.0f }, { true, 500.0f, true, 600.0f } } };
    CHECK(DropAhead(feet, bridge, cfg) == doctest::Approx(400.0));
    // Deep water below it (1100 deep): a safe landing, no drop (0.23.19).
    std::array<ProbeHit, 3> deepBridge{ { { true, 1000.0f }, { true, -500.0f, true, 600.0f }, { true, -500.0f, true, 600.0f } } };
    CHECK(DropAhead(feet, deepBridge, cfg) == 0.0f);
    // Water below the hit (a pool under a ledge already counted) changes nothing.
    std::array<ProbeHit, 3> dry{ { { true, 700.0f, true, 100.0f }, {}, {} } };
    dry[1] = { true, 1000.0f };
    dry[2] = { true, 1000.0f };
    CHECK(DropAhead(feet, dry, cfg) == doctest::Approx(300.0));
    // Standing in water up to the waist: the surface is above the feet, no drop.
    std::array<ProbeHit, 3> wading{ { { true, 950.0f, true, 1050.0f }, { true, 950.0f, true, 1050.0f }, { true, 950.0f, true, 1050.0f } } };
    CHECK(DropAhead(feet, wading, cfg) == 0.0f);
    // Water over the ray's bottom with no hit (open sea from a cliff): at
    // least 2936 deep, a safe landing (0.23.19; it read a 1000-unit drop to
    // the surface before).
    std::array<ProbeHit, 3> sea{ { { true, 1000.0f }, { false, 0.0f, true, 0.0f }, { false, 0.0f, true, 0.0f } } };
    CHECK(DropAhead(feet, sea, cfg) == 0.0f);
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

TEST_CASE("drop ahead and deep water ahead: deep water is a safe landing, shallow water a drop to its surface")
{
    // 0.23.19: the down ray passes through water to the bed; the depth is
    // the surface down to the hit. Landing in water at least
    // kSafeLandingDepth deep is no drop (Skyrim takes no fall damage there),
    // and the same pass reads the deepest water ahead (deep_water_ahead).
    const DropProbeConfig cfg;
    const float feet = 1000.0f;
    const ProbeHit ground{ true, 1000.0f };
    ProbeHit unknown;
    unknown.known = false;

    // A cliff into deep water: the bed 1000 below the feet, the sea 300 deep
    // over it. No drop; the deep-water output reads the depth.
    std::array<ProbeHit, 3> deepSea{ { ground, { true, 0.0f, true, 300.0f }, { true, 0.0f, true, 300.0f } } };
    auto r = MeasureAhead(feet, deepSea, cfg);
    CHECK(r.drop == 0.0f);
    CHECK(r.waterDepth == doctest::Approx(300.0));

    // A cliff into shallow water (a stream 60 deep): a drop, as before,
    // measured to the surface (1000 - 260), not to the bed.
    std::array<ProbeHit, 3> stream{ { ground, { true, 200.0f, true, 260.0f }, { true, 200.0f, true, 260.0f } } };
    r = MeasureAhead(feet, stream, cfg);
    CHECK(r.drop == doctest::Approx(740.0));
    CHECK(r.waterDepth == doctest::Approx(60.0));

    // A cliff onto dry ground: unchanged, and no water.
    std::array<ProbeHit, 3> rock{ { { true, 998.0f }, { true, 200.0f }, { true, 150.0f } } };
    r = MeasureAhead(feet, rock, cfg);
    CHECK(r.drop == doctest::Approx(850.0));
    CHECK(r.waterDepth == 0.0f);
    // Water known but below the hit (a pool under a ledge): no depth either.
    std::array<ProbeHit, 3> dryLedge{ { ground, { true, 200.0f, true, 100.0f }, ground } };
    r = MeasureAhead(feet, dryLedge, cfg);
    CHECK(r.drop == doctest::Approx(800.0));
    CHECK(r.waterDepth == 0.0f);

    // Walking to a lake shore on flat ground: the surface level with the
    // feet, the bed falling away ahead (10, 150, 300 deep). No drop; the
    // deep-water output reads the deepest point, before the player is in
    // the water.
    std::array<ProbeHit, 3> shore{ { { true, 990.0f, true, 1000.0f }, { true, 850.0f, true, 1000.0f },
                                     { true, 700.0f, true, 1000.0f } } };
    r = MeasureAhead(feet, shore, cfg);
    CHECK(r.drop == 0.0f);
    CHECK(r.waterDepth == doctest::Approx(300.0));

    // Unknown probes count as nothing in either output, whatever their
    // fields say.
    ProbeHit unknownDeep{ true, 0.0f, true, 900.0f };
    unknownDeep.known = false;
    std::array<ProbeHit, 3> blocked{ { ground, unknownDeep, unknown } };
    r = MeasureAhead(feet, blocked, cfg);
    CHECK(r.drop == 0.0f);
    CHECK(r.waterDepth == 0.0f);
    std::array<ProbeHit, 3> none{ { unknown, unknownDeep, unknown } };
    r = MeasureAhead(feet, none, cfg);
    CHECK(r.drop == -1.0f);
    CHECK(r.waterDepth == -1.0f);

    // No hit, water known: the depth is at least the surface down to the
    // ray's bottom (start 1064 - 4000 = -2936). Over the bottom by 2936:
    // deep, no drop. Over it by only 36: shallow, the drop to its surface.
    std::array<ProbeHit, 3> openSea{ { ground, { false, 0.0f, true, 0.0f }, ground } };
    r = MeasureAhead(feet, openSea, cfg);
    CHECK(r.drop == 0.0f);
    CHECK(r.waterDepth == doctest::Approx(2936.0));
    std::array<ProbeHit, 3> puddleInVoid{ { ground, { false, 0.0f, true, -2900.0f }, ground } };
    r = MeasureAhead(feet, puddleInVoid, cfg);
    CHECK(r.drop == doctest::Approx(3900.0));
    CHECK(r.waterDepth == doctest::Approx(36.0));
    // No hit and no water: a void, the bottom of the ray, as before.
    std::array<ProbeHit, 3> voidAhead{ { ground, { false, 0.0f }, ground } };
    r = MeasureAhead(feet, voidAhead, cfg);
    CHECK(r.drop == doctest::Approx(4000.0 - 64.0));
    CHECK(r.waterDepth == 0.0f);

    // The edge: exactly kSafeLandingDepth deep is safe; a unit shallower is
    // a drop to the surface.
    std::array<ProbeHit, 3> atEdge{ { ground, { true, 0.0f, true, kSafeLandingDepth }, ground } };
    CHECK(DropAhead(feet, atEdge, cfg) == 0.0f);
    std::array<ProbeHit, 3> underEdge{ { ground, { true, 0.0f, true, kSafeLandingDepth - 1.0f }, ground } };
    CHECK(DropAhead(feet, underEdge, cfg) == doctest::Approx(1000.0 - (kSafeLandingDepth - 1.0)));

    // A NaN water height or hit never makes a depth (nor a NaN).
    std::array<ProbeHit, 3> nanWater{ { ground, { true, 0.0f, true, std::nanf("") }, ground } };
    r = MeasureAhead(feet, nanWater, cfg);
    CHECK(r.drop == doctest::Approx(1000.0));
    CHECK(r.waterDepth == 0.0f);
    std::array<ProbeHit, 3> nanHit{ { ground, { true, std::nanf(""), true, 500.0f }, ground } };
    r = MeasureAhead(feet, nanHit, cfg);
    CHECK(r.drop == 0.0f);
    CHECK(r.waterDepth == 0.0f);

    // DropAhead is MeasureAhead's drop.
    CHECK(DropAhead(feet, stream, cfg) == MeasureAhead(feet, stream, cfg).drop);
}

TEST_CASE("drop ahead and deep water ahead: mixed probes, water at the hit, infinities, the no-water sentinel")
{
    // 0.23.19 fix round.
    const DropProbeConfig cfg;
    const float feet = 1000.0f;
    const ProbeHit ground{ true, 1000.0f };
    constexpr float kInf = std::numeric_limits<float>::infinity();

    // Flat ground, then deep water, then a rock cliff: the rock's drop wins
    // (the water point is no drop) and the deep water is still reported.
    std::array<ProbeHit, 3> mixed{ { ground, { true, 0.0f, true, 300.0f }, { true, 150.0f } } };
    auto r = MeasureAhead(feet, mixed, cfg);
    CHECK(r.drop == doctest::Approx(850.0));
    CHECK(r.waterDepth == doctest::Approx(300.0));

    // Water exactly at the hit: depth 0, the drop as before (to the hit).
    std::array<ProbeHit, 3> atHit{ { ground, { true, 200.0f, true, 200.0f }, ground } };
    r = MeasureAhead(feet, atHit, cfg);
    CHECK(r.waterDepth == 0.0f);
    CHECK(r.drop == doctest::Approx(800.0));

    // An infinite water height is no water: the drop is to the hit.
    for (const float wz : { kInf, -kInf }) {
        INFO("water " << wz);
        std::array<ProbeHit, 3> infWater{ { ground, { true, 200.0f, true, wz }, ground } };
        r = MeasureAhead(feet, infWater, cfg);
        CHECK(r.waterDepth == 0.0f);
        CHECK(r.drop == doctest::Approx(800.0));
    }
    // An infinite hit never makes a drop or a depth; water over a -inf hit
    // is measured to its surface (the depth is not finite, so not deep).
    std::array<ProbeHit, 3> infHigh{ { ground, { true, kInf, true, 500.0f }, ground } };
    r = MeasureAhead(feet, infHigh, cfg);
    CHECK(r.drop == 0.0f);
    CHECK(r.waterDepth == 0.0f);
    std::array<ProbeHit, 3> infLow{ { ground, { true, -kInf, true, 500.0f }, ground } };
    r = MeasureAhead(feet, infLow, cfg);
    CHECK(r.drop == doctest::Approx(500.0));
    CHECK(r.waterDepth == 0.0f);
    std::array<ProbeHit, 3> infLowDry{ { ground, { true, -kInf }, ground } };
    CHECK(DropAhead(feet, infLowDry, cfg) == 0.0f);

    // The heights the game uses for "no water" are not water: the XCLW
    // "use the worldspace default" sentinel (>= 2147483600), -FLT_MAX, NaN,
    // anything at or past 1e6 from the origin.
    CHECK_FALSE(IsUsableWaterHeight(2147483648.0f));
    CHECK_FALSE(IsUsableWaterHeight(2147483600.0f));
    CHECK_FALSE(IsUsableWaterHeight(-std::numeric_limits<float>::max()));
    CHECK_FALSE(IsUsableWaterHeight(std::nanf("")));
    CHECK_FALSE(IsUsableWaterHeight(kMaxWaterHeightAbs));
    CHECK_FALSE(IsUsableWaterHeight(-kMaxWaterHeightAbs));
    CHECK(IsUsableWaterHeight(0.0f));
    CHECK(IsUsableWaterHeight(-14000.0f));
    CHECK(IsUsableWaterHeight(99999.0f));
    // A sentinel over a cliff: no water, so a real drop and no depth (read
    // as water, it would have zeroed the cliff).
    std::array<ProbeHit, 3> sentinel{ { ground, { true, 0.0f, true, 2147483648.0f }, ground } };
    r = MeasureAhead(feet, sentinel, cfg);
    CHECK(r.drop == doctest::Approx(1000.0));
    CHECK(r.waterDepth == 0.0f);
    std::array<ProbeHit, 3> sentinelVoid{ { ground, { false, 0.0f, true, 2147483648.0f }, ground } };
    r = MeasureAhead(feet, sentinelVoid, cfg);
    CHECK(r.drop == doctest::Approx(4000.0 - 64.0));
    CHECK(r.waterDepth == 0.0f);
}

TEST_CASE("water read: the XCLW default sentinel resolves to the worldspace default water")
{
    // CommonLib's threshold (TESObjectCELL.cpp:95): below 2147483600 is the
    // cell's own height; at or above it, "use the worldspace default".
    CHECK(IsDefaultWaterSentinel(2147483600.0f));
    CHECK(IsDefaultWaterSentinel(2147483648.0f));
    CHECK(IsDefaultWaterSentinel(std::numeric_limits<float>::max()));
    CHECK_FALSE(IsDefaultWaterSentinel(2147483500.0f));  // the next float down: 2147483520
    CHECK_FALSE(IsDefaultWaterSentinel(0.0f));
    CHECK_FALSE(IsDefaultWaterSentinel(-std::numeric_limits<float>::max()));
    CHECK_FALSE(IsDefaultWaterSentinel(std::nanf("")));

    // Sentinel with a usable default: the default is the water.
    auto w = ResolveWaterHeight(2147483648.0f, -14000.0f);
    CHECK(w.known);
    CHECK(w.fromDefault);
    CHECK(w.z == -14000.0f);
    w = ResolveWaterHeight(2147483600.0f, 0.0f);  // the sea at z 0
    CHECK(w.known);
    CHECK(w.fromDefault);
    CHECK(w.z == 0.0f);

    // Sentinel with no default (an interior: nothing asked), or a default
    // that is no height (-FLT_MAX for a cell without water or worldspace,
    // the sentinel again, NaN, past 1e6): unknown, never a bogus height.
    for (std::optional<float> none : { std::optional<float>{}, std::optional<float>{ -std::numeric_limits<float>::max() },
                                       std::optional<float>{ -std::numeric_limits<float>::infinity() },
                                       std::optional<float>{ 2147483648.0f }, std::optional<float>{ std::nanf("") },
                                       std::optional<float>{ kMaxWaterHeightAbs } }) {
        w = ResolveWaterHeight(2147483648.0f, none);
        CHECK_FALSE(w.known);
        CHECK_FALSE(w.fromDefault);
    }

    // A normal height: used as is; a default offered alongside is ignored.
    w = ResolveWaterHeight(-200.0f, std::nullopt);
    CHECK(w.known);
    CHECK_FALSE(w.fromDefault);
    CHECK(w.z == -200.0f);
    w = ResolveWaterHeight(-200.0f, 5000.0f);
    CHECK(w.known);
    CHECK_FALSE(w.fromDefault);
    CHECK(w.z == -200.0f);

    // Non-finite or huge raw heights (not the sentinel): unknown, with or
    // without a default to ask.
    for (float bad : { std::nanf(""), std::numeric_limits<float>::infinity() * -1.0f,
                       -std::numeric_limits<float>::max(), kMaxWaterHeightAbs, -kMaxWaterHeightAbs, 2.0e9f }) {
        CHECK_FALSE(ResolveWaterHeight(bad, std::nullopt).known);
        CHECK_FALSE(ResolveWaterHeight(bad, 0.0f).known);
    }

    // End to end: a cliff over the sea. The cell reports the sentinel, the
    // worldspace default is the surface; deep water under the drop is a safe
    // landing, so drop 0 and the depth reads.
    const DropProbeConfig cfg;
    const float feet = 1000.0f;
    const ProbeHit ground{ true, 1000.0f };
    w = ResolveWaterHeight(2147483648.0f, 0.0f);
    ProbeHit sea{ true, -500.0f };
    sea.waterKnown = w.known;
    sea.waterZ = w.z;
    std::array<ProbeHit, 3> cliff{ { ground, sea, ground } };
    auto r = MeasureAhead(feet, cliff, cfg);
    CHECK(r.drop == 0.0f);
    CHECK(r.waterDepth == doctest::Approx(500.0));
    // The same cliff with no default to resolve: a real drop, no depth.
    w = ResolveWaterHeight(2147483648.0f, std::nullopt);
    sea.waterKnown = w.known;
    sea.waterZ = w.z;
    cliff = { { ground, sea, ground } };
    r = MeasureAhead(feet, cliff, cfg);
    CHECK(r.drop == doctest::Approx(1500.0));
    CHECK(r.waterDepth == 0.0f);
}

TEST_CASE("drop ahead and deep water ahead: a no-hit probe's bottom follows the ray's real start")
{
    // ProbeAll casts each point from waist height above the PREVIOUS
    // point's ground. A point cast from 500 (ground 436 before it) has its
    // bottom at 500 - 4000 = -3500, not at feet + 64 - 4000 = -2936.
    const DropProbeConfig cfg;
    const float feet = 1000.0f;
    const ProbeHit ground{ true, 1000.0f };
    ProbeHit low{ false, 0.0f };
    low.startZ = 500.0f;
    CHECK(RayStartZ(feet, low, cfg) == doctest::Approx(500.0));
    CHECK(RayBottomZ(feet, low, cfg) == doctest::Approx(-3500.0));
    std::array<ProbeHit, 3> voidLow{ { ground, low, ground } };
    CHECK(DropAhead(feet, voidLow, cfg) == doctest::Approx(4500.0));
    // Water 500 over that bottom: deep, no drop. Measured from feet + 64 it
    // would have been 0 deep and a 4000-unit drop.
    ProbeHit lowWet = low;
    lowWet.waterKnown = true;
    lowWet.waterZ = -3000.0f;
    std::array<ProbeHit, 3> wetLow{ { ground, lowWet, ground } };
    auto r = MeasureAhead(feet, wetLow, cfg);
    CHECK(r.waterDepth == doctest::Approx(500.0));
    CHECK(r.drop == 0.0f);
    // No startZ: feet + waist, the first point's start.
    CHECK(RayStartZ(feet, ProbeHit{}, cfg) == doctest::Approx(1064.0));
}

namespace
{
    // A scripted world along +Y for ProbeAll: ground height per Y (NaN = a
    // void), walls standing on the ground up to a top Z, and a Y where the
    // down ray runs out of recasts (a crowd). Rays go along +Y or straight down.
    struct World
    {
        std::function<float(float)> ground;
        struct Wall { float y; float top; };
        std::vector<Wall> walls;
        float crowdY = -1.0f;
        struct Call { RayKind kind; float z; float length; };
        std::vector<Call> calls;

        RayResult operator()(Vec3 from, Vec3 dir, float length, RayKind kind)
        {
            calls.push_back({ kind, from.z, length });
            if (kind == RayKind::Down) {
                REQUIRE(dir.z == -1.0f);
                if (crowdY >= 0.0f && std::abs(from.y - crowdY) < 1.0f) return { RayResult::Outcome::Exhausted, 0.0f };
                const float g = ground(from.y);
                if (std::isnan(g) || from.z - g > length) return { RayResult::Outcome::Clear, 0.0f };
                return { RayResult::Outcome::Hit, from.z - g };
            }
            REQUIRE(dir.y == doctest::Approx(1.0));
            float best = -1.0f;
            for (const auto& w : walls) {
                const float d = w.y - from.y;
                if (d > 0.0f && d <= length && from.z <= w.top && (best < 0.0f || d < best)) best = d;
            }
            for (float d = 1.0f; d <= length; d += 1.0f) {  // the ground rising into the ray
                const float g = ground(from.y + d);
                if (!std::isnan(g) && g >= from.z) {
                    if (best < 0.0f || d < best) best = d;
                    break;
                }
            }
            return best < 0.0f ? RayResult{ RayResult::Outcome::Clear, 0.0f } : RayResult{ RayResult::Outcome::Hit, best };
        }
    };

    constexpr Vec3 kFeet{ 0.0f, 0.0f, 1000.0f };
    constexpr Dir2 kNorth{ 0.0f, 1.0f };
    float Flat(float) { return 1000.0f; }
    float EdgeAt150(float y) { return y < 150.0f ? 1000.0f : std::nanf(""); }
    float EdgeAt120(float y) { return y < 120.0f ? 1000.0f : std::nanf(""); }
    float VoidAhead(float y) { return y < 40.0f ? 1000.0f : std::nanf(""); }
    float Uphill10(float y) { return 1000.0f + 0.18f * std::max(y, 0.0f); }  // ~10 degrees
    float Downstairs(float y) { return 1000.0f - 8.0f * std::floor(std::max(y, 0.0f) / 35.0f); }
}

TEST_CASE("probe sequence: the picks it casts, flat ground")
{
    const DropProbeConfig cfg;
    World w;
    w.ground = Flat;
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    CHECK(DropAhead(kFeet.z, hits, cfg) == 0.0f);
    REQUIRE(w.calls.size() == 9);  // per probe: waist pick, knee pick, down ray
    CHECK(w.calls[0].kind == RayKind::Horizontal);
    CHECK(w.calls[0].z == doctest::Approx(1064.0));
    CHECK(w.calls[0].length == doctest::Approx(70.0));
    CHECK(w.calls[1].z == doctest::Approx(1024.0));
    CHECK(w.calls[2].kind == RayKind::Down);
    CHECK(w.calls[2].z == doctest::Approx(1064.0));
    CHECK(w.calls[2].length == doctest::Approx(4000.0));
    CHECK(w.calls[3].length == doctest::Approx(105.0));  // from the previous point
    CHECK(w.calls[6].length == doctest::Approx(105.0));
}

TEST_CASE("probe sequence: a cliff reads as a drop; a void with no hit is a big one")
{
    const DropProbeConfig cfg;
    World w;
    w.ground = EdgeAt150;
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    CHECK(hits[0].known);
    CHECK(hits[1].known);
    CHECK_FALSE(hits[1].hit);
    CHECK(DropAhead(kFeet.z, hits, cfg) == doctest::Approx(4000.0 - 64.0));
}

TEST_CASE("probe sequence: a parapet under the waist blocks at the knee")
{
    const DropProbeConfig cfg;
    // A 40-unit wall at the edge, void behind: the waist pick passes over it.
    World w;
    w.ground = EdgeAt120;
    w.walls = { { 120.0f, 1040.0f } };
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    CHECK(hits[0].known);          // before the parapet
    CHECK_FALSE(hits[1].known);    // the first point past it
    CHECK_FALSE(hits[2].known);    // and all beyond
    CHECK(DropAhead(kFeet.z, hits, cfg) == 0.0f);
    // Right in front of the player: nothing is known at all.
    World close;
    close.ground = VoidAhead;
    close.walls = { { 40.0f, 1040.0f } };
    CHECK(DropAhead(kFeet.z, ProbeAll(kFeet, kNorth, cfg, close), cfg) == -1.0f);
}

TEST_CASE("probe sequence: an invisible wall at the edge blocks (any hit counts)")
{
    const DropProbeConfig cfg;
    World w;
    w.ground = EdgeAt150;
    w.walls = { { 150.0f, 5000.0f } };
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    CHECK(hits[0].known);
    CHECK_FALSE(hits[1].known);
    CHECK(DropAhead(kFeet.z, hits, cfg) == 0.0f);
}

TEST_CASE("probe sequence: a walkable slope uphill stays known at every angle up to 30 degrees")
{
    const DropProbeConfig cfg;
    for (const float degrees : { 5.0f, 10.0f, 15.0f, 20.0f, 30.0f }) {
        INFO("slope " << degrees << " degrees");
        const float t = std::tan(degrees * std::numbers::pi_v<float> / 180.0f);
        World up;
        up.ground = [t](float y) { return 1000.0f + t * std::max(y, 0.0f); };
        const auto u = ProbeAll(kFeet, kNorth, cfg, up);
        CHECK(u[0].known);
        CHECK(u[1].known);
        CHECK(u[2].known);
        CHECK(u[2].hitZ == doctest::Approx(1000.0 + t * 280.0).epsilon(1e-4));
        CHECK(DropAhead(kFeet.z, u, cfg) == 0.0f);
    }
    // Steeper than the waist over the spacing (64 over 70 units, ~42 degrees):
    // not walkable, the first point already unknown.
    World cliffFace;
    cliffFace.ground = [](float y) { return 1000.0f + std::max(y, 0.0f); };  // 45 degrees
    CHECK(DropAhead(kFeet.z, ProbeAll(kFeet, kNorth, cfg, cliffFace), cfg) == -1.0f);
    CHECK(Uphill10(70.0f) > 1000.0f);
}

TEST_CASE("probe sequence: a 6-degree rise to a crest, then a cliff at 250, reads the cliff")
{
    const DropProbeConfig cfg;
    const float t = std::tan(6.0f * std::numbers::pi_v<float> / 180.0f);
    World w;
    w.ground = [t](float y) { return y < 250.0f ? 1000.0f + t * std::max(y, 0.0f) : std::nanf(""); };
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    CHECK(hits[0].known);
    CHECK(hits[1].known);
    CHECK(hits[2].known);
    CHECK_FALSE(hits[2].hit);  // the void past the crest
    CHECK(DropAhead(kFeet.z, hits, cfg) > 3000.0f);
}

TEST_CASE("probe sequence: stairs down read small")
{
    const DropProbeConfig cfg;
    World down;
    down.ground = Downstairs;
    const float drop = DropAhead(kFeet.z, ProbeAll(kFeet, kNorth, cfg, down), cfg);
    CHECK(drop == doctest::Approx(64.0));  // 8 steps of 8 units by 280
}

TEST_CASE("probe sequence: a down ray out of recasts makes only that point unknown")
{
    const DropProbeConfig cfg;
    World w;
    w.ground = Flat;
    w.crowdY = 175.0f;
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    CHECK(hits[0].known);
    CHECK_FALSE(hits[1].known);
    CHECK(hits[2].known);   // reachability went on past the crowd
    CHECK(hits[2].hit);
}

TEST_CASE("probe sequence: a step up past the knee is ground, not a parapet")
{
    const DropProbeConfig cfg;
    // A 40-unit ledge up at y = 100, flat beyond: the knee pick hits its face,
    // the down ray at the point finds ground above the knee -> rising ground.
    World w;
    w.ground = [](float y) { return y < 100.0f ? 1000.0f : 1040.0f; };
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    CHECK(hits[0].known);
    CHECK(hits[1].known);
    CHECK(hits[1].hitZ == doctest::Approx(1040.0));
    CHECK(hits[2].known);
    CHECK(DropAhead(kFeet.z, hits, cfg) == 0.0f);
}

TEST_CASE("teleport: a jump past 500 units is not movement")
{
    CHECK_FALSE(IsTeleport({ 0.0f, 0.0f, 0.0f }, { 80.0f, 0.0f, 0.0f }));     // a sprint, 100 ms
    CHECK_FALSE(IsTeleport({ 0.0f, 0.0f, 0.0f }, { 300.0f, 300.0f, 0.0f }));  // a fast horse, a long frame
    CHECK(IsTeleport({ 0.0f, 0.0f, 0.0f }, { 3000.0f, 0.0f, 0.0f }));         // a coc
    CHECK(IsTeleport({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -9000.0f }));        // a load door down
    CHECK(IsTeleport({ 0.0f, 0.0f, 0.0f }, { std::nanf(""), 0.0f, 0.0f }));
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

TEST_CASE("bench kind: the workbench keyword tells a forge from a cooking spit (0.23.16)")
{
    using V = std::vector<std::string_view>;
    // Create-object benches: the engine's type is the same for all of them.
    CHECK(ClassifyBench(kBenchCreateObject, V{ "CraftingSmithingForge" }) == BenchKind::Smithing);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "CraftingSmithingSkyforge" }) == BenchKind::Smithing);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "CraftingCookpot" }) == BenchKind::Cooking);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "isCookingSpit", "CraftingCookpot" }) == BenchKind::Cooking);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "BYOHCraftingOven" }) == BenchKind::Cooking);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "CraftingSmelter" }) == BenchKind::Smelting);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "CraftingTanningRack" }) == BenchKind::Tanning);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "FurnitureSpecial", "SomeModStation" }) == BenchKind::Other);
    CHECK(ClassifyBench(kBenchCreateObject, V{}) == BenchKind::Other);
    CHECK(ClassifyBench(kBenchCreateObject, V{ "craftingsmithingforge" }) == BenchKind::Smithing);  // case
    // A forge that also carries a cooking keyword is a forge.
    CHECK(ClassifyBench(kBenchCreateObject, V{ "CraftingCookpot", "CraftingSmithingForge" }) == BenchKind::Smithing);
    // The other bench types say it on their own, whatever the keywords.
    CHECK(ClassifyBench(kBenchSmithingWeapon, V{ "CraftingSmithingSharpeningWheel" }) == BenchKind::Smithing);
    CHECK(ClassifyBench(kBenchSmithingArmor, V{}) == BenchKind::Smithing);
    CHECK(ClassifyBench(kBenchEnchanting, V{ "CraftingCookpot" }) == BenchKind::Enchanting);
    CHECK(ClassifyBench(kBenchAlchemyExperiment, V{}) == BenchKind::Alchemy);
    CHECK(ClassifyBench(kBenchNone, V{ "CraftingSmithingForge" }) == BenchKind::None);

    // Only the three craft needs exist: cooking, smelting, tanning set none.
    CHECK(NeedWorkstation(BenchKind::Smithing) == static_cast<int>(Workstation::Smithing));
    CHECK(NeedWorkstation(BenchKind::Enchanting) == static_cast<int>(Workstation::Enchanting));
    CHECK(NeedWorkstation(BenchKind::Alchemy) == static_cast<int>(Workstation::Alchemy));
    for (const auto k : { BenchKind::None, BenchKind::Cooking, BenchKind::Smelting, BenchKind::Tanning, BenchKind::Other }) {
        CHECK(NeedWorkstation(k) == static_cast<int>(Workstation::None));
    }
}

TEST_CASE("drop reading age: paused time does not age it, unpaused time does (0.23.16)")
{
    ReadingAge age;
    CHECK(age.Update(10.0, -1.0, false) == doctest::Approx(-1.0));  // no reading yet

    // A reading at 10.0, seen at 10.05; the game runs.
    CHECK(age.Update(10.05, 10.0, false) == doctest::Approx(0.05));
    CHECK(age.Update(10.15, 10.0, false) == doctest::Approx(0.15));

    // The console opens: the hook stops, the loop polls on for 60 s. The age stays.
    double t = 10.15;
    for (int i = 0; i < 600; ++i) {
        t += 0.1;
        CHECK(age.Update(t, 10.0, true) == doctest::Approx(0.15));
    }
    CHECK(age.Age() <= 1.0);  // still a usable reading (kMaxAgeSec 1)

    // The game resumes; the hook replaces the reading within 100 ms.
    CHECK(age.Update(t + 0.1, 10.0, false) == doctest::Approx(0.25));
    CHECK(age.Update(t + 0.15, t + 0.12, false) == doctest::Approx(0.03));
}

TEST_CASE("drop reading age: an unpaused stop of the hook still goes stale; a long poll gap counts at most 0.5 s")
{
    ReadingAge age;
    age.Update(0.0, 0.0, false);
    double t = 0.0;
    for (int i = 0; i < 12; ++i) age.Update(t += 0.1, 0.0, false);  // 1.2 s unpaused, no new reading
    CHECK(age.Age() > 1.0);

    // The loop did not poll through a 30 s pause: the first poll after it adds at most 0.5 s.
    age.Reset();
    age.Update(100.0, 100.0, false);
    CHECK(age.Update(130.0, 100.0, false) == doctest::Approx(ReadingAge::kMaxPollStepSec));

    // The reading is withdrawn (kPreLoadGame clears it): not measured, and a later one starts fresh.
    CHECK(age.Update(131.0, -1.0, false) == doctest::Approx(-1.0));
    CHECK(age.Update(132.0, 131.9, false) == doctest::Approx(0.1));
}

TEST_CASE("probe sequence: each point records the Z its down ray was cast from")
{
    const DropProbeConfig cfg;
    // Through ProbeAll: a 40-unit step down to 960 at the first point, then
    // a void. The second ray starts at 960 + 64 = 1024, its bottom -2976.
    World w;
    w.ground = [](float y) { return y < 40.0f ? 1000.0f : (y < 100.0f ? 960.0f : std::nanf("")); };
    const auto hits = ProbeAll(kFeet, kNorth, cfg, w);
    REQUIRE(hits[0].known);
    REQUIRE(hits[1].known);
    CHECK(hits[0].hitZ == doctest::Approx(960.0));
    REQUIRE(hits[0].startZ.has_value());
    CHECK(*hits[0].startZ == doctest::Approx(1064.0));
    CHECK_FALSE(hits[1].hit);
    REQUIRE(hits[1].startZ.has_value());
    CHECK(*hits[1].startZ == doctest::Approx(1024.0));
    CHECK(DropAhead(kFeet.z, hits, cfg) == doctest::Approx(1000.0 - (1024.0 - 4000.0)));
}
