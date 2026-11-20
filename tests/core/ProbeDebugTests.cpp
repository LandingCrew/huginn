// Host tests for src/core/ProbeDebug.h (0.24.40): the drop / edge probe's
// picture -- each ray ProbeAll casts, coloured by what it found.

#include "core/ProbeDebug.h"
#include "core/EdgeApproach.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <span>
#include <vector>

using namespace Huginn::Core::Needs;
namespace PD = Huginn::Core::Needs::ProbeDebug;

namespace
{
    // A world along +Y: ground height per Y (NaN = a void), walls up to a top
    // Z, and a Y where the down ray runs out of recasts. Records every cast.
    struct World
    {
        std::function<float(float)> ground;
        struct Wall { float y; float top; };
        std::vector<Wall> walls;
        float crowdY = -1.0f;
        std::array<PD::Cast, PD::kMaxCasts> casts{};
        std::size_t count = 0;

        RayResult operator()(Vec3 from, Vec3 dir, float length, RayKind kind)
        {
            RayResult r;
            if (kind == RayKind::Down) {
                if (crowdY >= 0.0f && std::abs(from.y - crowdY) < 1.0f) {
                    r = { RayResult::Outcome::Exhausted, 0.0f };
                } else {
                    const float g = ground(from.y);
                    r = (std::isnan(g) || from.z - g > length) ? RayResult{ RayResult::Outcome::Clear, 0.0f }
                                                               : RayResult{ RayResult::Outcome::Hit, from.z - g };
                }
            } else {
                float best = -1.0f;
                for (const auto& w : walls) {
                    const float d = w.y - from.y;
                    if (d > 0.0f && d <= length && from.z <= w.top && (best < 0.0f || d < best)) best = d;
                }
                r = best < 0.0f ? RayResult{ RayResult::Outcome::Clear, 0.0f } : RayResult{ RayResult::Outcome::Hit, best };
            }
            if (count < casts.size()) casts[count++] = { from, dir, length, kind, r };
            return r;
        }
    };

    constexpr Vec3 kFeet{ 0.0f, 0.0f, 1000.0f };
    constexpr Dir2 kNorth{ 0.0f, 1.0f };

    PD::Frame Picture(World& w, std::array<ProbeHit, kMaxPoints>* hitsOut = nullptr,
                      std::function<void(std::array<ProbeHit, kMaxPoints>&)> addWater = {}, float speed = 0.0f)
    {
        const DropProbeConfig cfg;
        const auto layout = LayoutFor(speed, cfg);
        auto hits = ProbeChain(kFeet, kNorth, layout, cfg, w);
        if (addWater) addWater(hits);
        if (hitsOut) *hitsOut = hits;
        return PD::Build(kFeet, kNorth, layout, w.casts, w.count, std::span<const ProbeHit>(hits.data(), layout.count), cfg);
    }

    std::size_t CountLook(const PD::Frame& f, PD::Look look)
    {
        std::size_t n = 0;
        for (std::size_t i = 0; i < f.segmentCount; ++i) n += f.segments[i].look == look;
        return n;
    }
}

TEST_CASE("probe picture: flat ground is green everywhere")
{
    World w;
    w.ground = [](float) { return 1000.0f; };
    const auto f = Picture(w);
    CHECK(f.measured);
    CHECK(f.segmentCount == 9);   // two picks and a down ray per point
    CHECK(CountLook(f, PD::Look::Okay) == 9);
    REQUIRE(f.markCount == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(f.marks[i].look == PD::Look::Okay);
        CHECK(f.marks[i].drop == doctest::Approx(0.0));
    }
    CHECK(f.unreachedCount == 0);
}

TEST_CASE("probe picture: a cliff is red; a void's ray is drawn short and says so")
{
    World w;
    w.ground = [](float y) { return y < 150.0f ? 1000.0f : std::nanf(""); };
    const auto f = Picture(w);
    REQUIRE(f.markCount == 3);
    CHECK(f.marks[0].look == PD::Look::Okay);
    CHECK(f.marks[1].look == PD::Look::Fall);
    CHECK(f.marks[1].voidBelow);
    CHECK(f.marks[2].look == PD::Look::Fall);
    // the void's segment ends kVoidDrawLength under its start, not 16000
    bool found = false;
    for (std::size_t i = 0; i < f.segmentCount; ++i) {
        const auto& s = f.segments[i];
        if (s.look == PD::Look::Fall) {
            CHECK(s.a.z - s.b.z == doctest::Approx(PD::kVoidDrawLength));
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("probe picture: a small step down stays green; red starts at the edge threshold")
{
    World w;
    w.ground = [](float y) { return y < 150.0f ? 1000.0f : 1000.0f - (kEdgeDropThreshold - 10.0f); };
    auto f = Picture(w);
    CHECK(f.marks[1].look == PD::Look::Okay);
    World w2;
    w2.ground = [](float y) { return y < 150.0f ? 1000.0f : 1000.0f - (kEdgeDropThreshold + 10.0f); };
    f = Picture(w2);
    CHECK(f.marks[1].look == PD::Look::Fall);
}

TEST_CASE("probe picture: water under a probe is blue, with a tick at its surface")
{
    World w;
    w.ground = [](float y) { return y < 150.0f ? 1000.0f : 400.0f; };   // a 600 drop onto a lake bed
    const auto f = Picture(w, nullptr, [](std::array<ProbeHit, kMaxPoints>& hits) {
        for (std::size_t i = 1; i < 3; ++i) {
            hits[i].waterKnown = true;
            hits[i].waterZ = 700.0f;   // 300 deep: a safe landing, no fall
        }
    });
    CHECK(f.marks[1].look == PD::Look::Water);
    CHECK(f.marks[1].waterDepth == doctest::Approx(300.0));
    CHECK(f.marks[1].drop == doctest::Approx(0.0));
    // two down rays and two surface ticks are blue
    CHECK(CountLook(f, PD::Look::Water) == 4);
    CHECK(CountLook(f, PD::Look::Fall) == 0);
}

TEST_CASE("probe picture: a wall is yellow, and the points behind it are unreached")
{
    World w;
    w.ground = [](float) { return 1000.0f; };
    w.walls.push_back({ 100.0f, 5000.0f });
    const auto f = Picture(w);
    CHECK(CountLook(f, PD::Look::Blocked) == 1);   // the waist pick to the second point
    CHECK(f.markCount == 1);                       // only the first point's down ray
    REQUIRE(f.unreachedCount == 2);
    CHECK(f.unreached[0].y == doctest::Approx(175.0));
    CHECK(f.unreached[1].y == doctest::Approx(280.0));
    CHECK(CountLook(f, PD::Look::Fall) == 0);      // red only ever means a fall
}

TEST_CASE("probe picture: running, the far points are drawn and marked far")
{
    World w;
    w.ground = [](float y) { return y < 600.0f ? 1000.0f : std::nanf(""); };   // a cliff 600 ahead
    const auto f = Picture(w, nullptr, {}, 400.0f);   // 400 units/game s: reach 800
    CHECK(f.reach == doctest::Approx(800.0));
    REQUIRE(f.markCount == 6);
    for (std::size_t i = 0; i < 3; ++i) CHECK_FALSE(f.marks[i].farPoint);
    for (std::size_t i = 3; i < 6; ++i) CHECK(f.marks[i].farPoint);
    CHECK(f.marks[2].look == PD::Look::Okay);   // the near three see no edge
    CHECK(f.marks[5].look == PD::Look::Fall);   // the last far point (800) is past it
}

TEST_CASE("probe picture: a down ray out of recasts is grey, never red")
{
    World w;
    w.ground = [](float) { return 1000.0f; };
    w.crowdY = 70.0f;
    const auto f = Picture(w);
    CHECK(CountLook(f, PD::Look::Skipped) == 1);
    CHECK(CountLook(f, PD::Look::Fall) == 0);
}

// 0.24.40: the far points (core/DropAhead.h LayoutFor, ProbeChain;
// core/EdgeApproach.h NearestEdgeDistance over a layout).
TEST_CASE("probe layout: the near three at a walk; far points out to 2 s ahead at a run, capped")
{
    const DropProbeConfig cfg;
    auto l = LayoutFor(0.0f, cfg);
    CHECK(l.count == kNearPoints);
    CHECK(l.reach == doctest::Approx(280.0));
    l = LayoutFor(150.0f, cfg);   // a walk: 300, within farMinGap of 280
    CHECK(l.count == kNearPoints);
    l = LayoutFor(400.0f, cfg);   // a run: 800
    REQUIRE(l.count == kMaxPoints);
    CHECK(l.reach == doctest::Approx(800.0));
    CHECK(l.distances[3] == doctest::Approx(280.0 + 520.0 / 3.0));
    CHECK(l.distances[5] == doctest::Approx(800.0));
    l = LayoutFor(5000.0f, cfg);  // capped
    CHECK(l.reach == doctest::Approx(cfg.farMaxReach));
    CHECK(LayoutFor(-300.0f, cfg).count == kNearPoints);                       // backing away
    CHECK(LayoutFor(std::nanf(""), cfg).count == kNearPoints);
    CHECK(LayoutFor(std::numeric_limits<float>::infinity(), cfg).count == kNearPoints);   // not finite: a bad reading, near only
}

TEST_CASE("probe layout: running at a cliff 600 ahead, the edge is seen by a far point; drop_ahead still reads the near three")
{
    const DropProbeConfig cfg;
    World w;
    w.ground = [](float y) { return y < 600.0f ? 1000.0f : std::nanf(""); };
    const auto layout = LayoutFor(400.0f, cfg);
    const auto hits = ProbeChain(kFeet, kNorth, layout, cfg, w);
    const auto edge = NearestEdgeDistance(kFeet.z, std::span<const ProbeHit>(hits.data(), layout.count), layout.Distances(), cfg);
    REQUIRE(edge.has_value());
    CHECK(*edge == doctest::Approx(layout.distances[4]));   // ~627, the first far point past 600
    const std::array<ProbeHit, 3> nearHits{ hits[0], hits[1], hits[2] };
    CHECK(MeasureAhead(kFeet.z, nearHits, cfg).drop == doctest::Approx(0.0));
    // walking, the same cliff is not seen yet
    World w2;
    w2.ground = w.ground;
    const auto walk = LayoutFor(100.0f, cfg);
    const auto walkHits = ProbeChain(kFeet, kNorth, walk, cfg, w2);
    CHECK_FALSE(NearestEdgeDistance(kFeet.z, std::span<const ProbeHit>(walkHits.data(), walk.count), walk.Distances(), cfg));
}

TEST_CASE("probe layout: ProbeAll is ProbeChain's near three")
{
    const DropProbeConfig cfg;
    World a, b;
    a.ground = b.ground = [](float y) { return y < 150.0f ? 1000.0f : 700.0f; };
    const auto all = ProbeAll(kFeet, kNorth, cfg, a);
    const auto chain = ProbeChain(kFeet, kNorth, LayoutFor(0.0f, cfg), cfg, b);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(all[i].known == chain[i].known);
        CHECK(all[i].hit == chain[i].hit);
        CHECK(all[i].hitZ == doctest::Approx(chain[i].hitZ));
    }
    CHECK(a.count == b.count);
}

// /code-review #262: a far point's edge is a step, not a long slope; a fall
// into shallow water is a fall; a point the pass could not read has no label.
TEST_CASE("far points: a sprint down an ordinary hill is no edge; a sudden step out there is")
{
    const DropProbeConfig cfg;
    const auto layout = LayoutFor(700.0f, cfg);   // a sprint: far points 653, 1027, 1400
    REQUIRE(layout.reach == doctest::Approx(1400.0));
    World hill;
    const float slope = std::tan(19.0f * 3.14159265f / 180.0f);
    hill.ground = [slope](float y) { return 1000.0f - slope * std::max(y, 0.0f); };   // 1400 ahead: 482 below
    auto hits = ProbeChain(kFeet, kNorth, layout, cfg, hill);
    CHECK_FALSE(NearestEdgeDistance(kFeet.z, std::span<const ProbeHit>(hits.data(), layout.count), layout.Distances(), cfg));
    auto f = PD::Build(kFeet, kNorth, layout, hill.casts, hill.count, std::span<const ProbeHit>(hits.data(), layout.count), cfg);
    CHECK(CountLook(f, PD::Look::Fall) == 0);

    World step;
    step.ground = [](float y) { return y < 900.0f ? 1000.0f : 400.0f; };   // a 600 step at 900
    hits = ProbeChain(kFeet, kNorth, layout, cfg, step);
    const auto edge = NearestEdgeDistance(kFeet.z, std::span<const ProbeHit>(hits.data(), layout.count), layout.Distances(), cfg);
    REQUIRE(edge.has_value());
    CHECK(*edge == doctest::Approx(layout.distances[4]));   // 1027, the first point past the step
    f = PD::Build(kFeet, kNorth, layout, step.casts, step.count, std::span<const ProbeHit>(hits.data(), layout.count), cfg);
    REQUIRE(f.markCount == 6);
    CHECK(f.marks[4].look == PD::Look::Fall);
    CHECK(f.marks[4].drop == doctest::Approx(600.0));
    CHECK(f.marks[5].look == PD::Look::Okay);   // the next point: level with the foot of the step
}

TEST_CASE("probe picture: a dangerous drop into shallow water is red, with its water")
{
    World w;
    w.ground = [](float y) { return y < 150.0f ? 1000.0f : 100.0f; };   // a 900 drop
    const auto f = Picture(w, nullptr, [](std::array<ProbeHit, kMaxPoints>& hits) {
        for (std::size_t i = 1; i < 3; ++i) {
            hits[i].waterKnown = true;
            hits[i].waterZ = 130.0f;   // a 30-deep stream: no safe landing
        }
    });
    CHECK(f.marks[1].look == PD::Look::Fall);
    CHECK(f.marks[1].drop == doctest::Approx(870.0));
    CHECK(f.marks[1].waterDepth == doctest::Approx(30.0));
}

TEST_CASE("probe picture: a point a knee-high obstacle hides gets no label")
{
    World w;
    w.ground = [](float) { return 1000.0f; };
    w.walls.push_back({ 150.0f, 1040.0f });   // over the knee pick (1024), under the waist pick (1064)
    const auto f = Picture(w);
    CHECK(f.markCount == 1);                      // only the first point
    CHECK(CountLook(f, PD::Look::Skipped) == 1);  // the hidden point's down ray, grey
    CHECK(CountLook(f, PD::Look::Blocked) == 1);  // the knee pick
    CHECK(f.unreachedCount == 1);                 // the third point
}
