// Host tests for src/core/ProbeDebug.h (0.24.40): the drop / edge probe's
// picture -- each ray ProbeAll casts, coloured by what it found.

#include "core/ProbeDebug.h"

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
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

    PD::Frame Picture(World& w, std::array<ProbeHit, 3>* hitsOut = nullptr,
                      std::function<void(std::array<ProbeHit, 3>&)> addWater = {})
    {
        const DropProbeConfig cfg;
        auto hits = ProbeAll(kFeet, kNorth, cfg, w);
        if (addWater) addWater(hits);
        if (hitsOut) *hitsOut = hits;
        return PD::Build(kFeet, kNorth, w.casts, w.count, hits, cfg);
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
    const auto f = Picture(w, nullptr, [](std::array<ProbeHit, 3>& hits) {
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

TEST_CASE("probe picture: a down ray out of recasts is grey, never red")
{
    World w;
    w.ground = [](float) { return 1000.0f; };
    w.crowdY = 70.0f;
    const auto f = Picture(w);
    CHECK(CountLook(f, PD::Look::Skipped) == 1);
    CHECK(CountLook(f, PD::Look::Fall) == 0);
}
