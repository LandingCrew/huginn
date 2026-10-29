// core/EdgeApproach.h: time_to_edge and edge_approach (0.23.48), two needs
// from the drop_ahead probe pass. The game side (state/DropAheadProbe.cpp)
// only feeds the probe hits, the game clock and the probe direction.

#include "core/EdgeApproach.h"
#include "core/NeedEvaluator.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <optional>

using namespace Huginn::Core::Needs;

namespace
{
    const DropProbeConfig kCfg{};

    ProbeHit Ground(float z)
    {
        ProbeHit h;
        h.hit = true;
        h.hitZ = z;
        return h;
    }

    ProbeHit Unknown()
    {
        ProbeHit h;
        h.known = false;
        return h;
    }

    /// Flat ground at z 0 up to an edge `edgeAt` units ahead of the feet, and
    /// a `drop`-unit cliff past it (a probe point at or past the edge is over
    /// the cliff). edgeAt < 0: no edge.
    std::array<ProbeHit, 3> Cliff(float edgeAt, float drop = 900.0f)
    {
        std::array<ProbeHit, 3> hits{};
        for (std::size_t i = 0; i < hits.size(); ++i) {
            const bool over = edgeAt >= 0.0f && kCfg.distances[i] >= edgeAt;
            hits[i] = Ground(over ? -drop : 0.0f);
        }
        return hits;
    }

    float Value(NeedId id, const NeedSnapshot& s)
    {
        return EvaluateNeeds(s, DefaultCurves()).value[Index(id)];
    }

    float Input(NeedId id, const NeedSnapshot& s)
    {
        return NeedInputs(s)[Index(id)];
    }

    /// Walks the player toward an edge `startGap` units ahead at `speed`
    /// units per GAME second, one probe pass every `wallStep` wall seconds at
    /// time scale `timeScale` (game seconds = wall x scale), for `wallSec`.
    /// Returns the tracker's last slope; `maxSlope` gets the largest.
    float Approach(EdgeApproachTracker& t, float startGap, float speed, double wallStep, double timeScale,
                   double wallSec, float* maxSlope = nullptr, Dir2 dir = {})
    {
        float slope = 0.0f;
        float best = -1.0e9f;
        const int n = static_cast<int>(std::lround(wallSec / wallStep));
        for (int k = 0; k <= n; ++k) {
            const double game = k * wallStep * timeScale;
            const float gap = startGap - speed * static_cast<float>(game);
            slope = t.Add({ 1000.0 + game, dir, EdgeProximity(0.0f, Cliff(gap), kCfg) });
            best = std::max(best, slope);
        }
        if (maxSlope) *maxSlope = best;
        return slope;
    }
}

TEST_CASE("edge: an edge is a drop at drop_ahead's own centre, 466 units, read from the csv curve")
{
    CHECK(kEdgeDropThreshold == doctest::Approx(466.0f));
    CHECK(kEdgeDropThreshold == kNeeds[Index(NeedId::drop_ahead)].curve.p1);
    // drop_ahead reads 0.5 there: the threshold is where that need is "more on than off".
    CHECK(Evaluate(kNeeds[Index(NeedId::drop_ahead)].curve, kEdgeDropThreshold) == doctest::Approx(0.5f));
}

TEST_CASE("edge: the nearest known probe point whose drop is dangerous")
{
    // Flat: no edge.
    CHECK_FALSE(NearestEdgeDistance(0.0f, Cliff(-1.0f), kCfg).has_value());
    // A cliff between the middle and the far point: the far point, 280.
    CHECK(NearestEdgeDistance(0.0f, Cliff(250.0f), kCfg) == std::optional<float>(280.0f));
    // Between the near and the middle: 175. Inside the near point: 70.
    CHECK(NearestEdgeDistance(0.0f, Cliff(100.0f), kCfg) == std::optional<float>(175.0f));
    CHECK(NearestEdgeDistance(0.0f, Cliff(30.0f), kCfg) == std::optional<float>(70.0f));
    // Just under the threshold is no edge; at it is.
    CHECK_FALSE(NearestEdgeDistance(0.0f, Cliff(30.0f, 465.0f), kCfg).has_value());
    CHECK(NearestEdgeDistance(0.0f, Cliff(30.0f, 466.0f), kCfg) == std::optional<float>(70.0f));
    // A void (known, no hit) is a very big drop: an edge.
    auto hits = Cliff(-1.0f);
    hits[1] = ProbeHit{};
    hits[1].hit = false;
    CHECK(NearestEdgeDistance(0.0f, hits, kCfg) == std::optional<float>(175.0f));
}

TEST_CASE("edge: an unknown probe is never an edge; a known point past one still counts")
{
    std::array<ProbeHit, 3> hits{ Unknown(), Unknown(), Unknown() };
    CHECK_FALSE(NearestEdgeDistance(0.0f, hits, kCfg).has_value());
    // The near point out of recasts, the middle over a cliff (core ProbeAll
    // leaves the next point known after an Exhausted one).
    hits = { Unknown(), Ground(-900.0f), Ground(-900.0f) };
    CHECK(NearestEdgeDistance(0.0f, hits, kCfg) == std::optional<float>(175.0f));
}

TEST_CASE("edge: a deep-water landing is no edge (kSafeLandingDepth); shallow water is measured to its surface")
{
    auto deep = Ground(-1200.0f);
    deep.waterKnown = true;
    deep.waterZ = -900.0f;  // 300 deep: a safe landing
    std::array<ProbeHit, 3> hits{ Ground(0.0f), deep, deep };
    CHECK_FALSE(NearestEdgeDistance(0.0f, hits, kCfg).has_value());
    CHECK(EdgeProximity(0.0f, hits, kCfg) < 0.01f);

    auto shallow = Ground(-950.0f);
    shallow.waterKnown = true;
    shallow.waterZ = -900.0f;  // 50 deep: a 900-unit drop to the surface
    hits = { Ground(0.0f), shallow, shallow };
    CHECK(NearestEdgeDistance(0.0f, hits, kCfg) == std::optional<float>(175.0f));
}

TEST_CASE("edge: the speed is the horizontal velocity along the probe direction")
{
    CHECK(SpeedToward({ 300.0f, 0.0f, -50.0f }, { 1.0f, 0.0f }) == doctest::Approx(300.0f));
    CHECK(SpeedToward({ 300.0f, 0.0f, 0.0f }, { 0.0f, 1.0f }) == doctest::Approx(0.0f));
    CHECK(SpeedToward({ -150.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }) == doctest::Approx(-150.0f));
    CHECK(SpeedToward({ std::nanf(""), 0.0f, 0.0f }, { 1.0f, 0.0f }) == 0.0f);
    CHECK(SpeedToward({ std::numeric_limits<float>::infinity(), 0.0f, 0.0f }, { 1.0f, 0.0f }) == 0.0f);
}

TEST_CASE("edge proximity: the mean of the drop curve over the three points; unknown is 0")
{
    CHECK(EdgeProximity(0.0f, Cliff(-1.0f), kCfg) < 0.001f);
    CHECK(EdgeProximity(0.0f, Cliff(250.0f), kCfg) == doctest::Approx(1.0f / 3.0f).epsilon(0.01));
    CHECK(EdgeProximity(0.0f, Cliff(100.0f), kCfg) == doctest::Approx(2.0f / 3.0f).epsilon(0.01));
    CHECK(EdgeProximity(0.0f, Cliff(0.0f), kCfg) == doctest::Approx(1.0f).epsilon(0.01));
    // A wall: every point unknown reads 0, not a drop.
    const std::array<ProbeHit, 3> wall{ Unknown(), Unknown(), Unknown() };
    CHECK(EdgeProximity(0.0f, wall, kCfg) == 0.0f);
    // A 300-unit step down is no edge and adds almost nothing.
    CHECK(EdgeProximity(0.0f, Cliff(0.0f, 300.0f), kCfg) < 0.01f);
}

TEST_CASE("time_to_edge: seconds = distance over the speed toward the edge; the curve falls with them")
{
    NeedSnapshot s;
    // No edge measured: never, 0.
    CHECK(Input(NeedId::time_to_edge, s) == kNever);
    CHECK(Value(NeedId::time_to_edge, s) < 1.0e-6f);
    // Running at the far point's edge: 280 / 300 = 0.93 s.
    s.edgeDistance = 280.0f;
    s.edgeSpeed = 300.0f;
    CHECK(Input(NeedId::time_to_edge, s) == doctest::Approx(280.0f / 300.0f));
    CHECK(Value(NeedId::time_to_edge, s) == doctest::Approx(0.846f).epsilon(0.01));
    // The centre: 1.5 s.
    s.edgeDistance = 175.0f;
    s.edgeSpeed = 175.0f / 1.5f;
    CHECK(Value(NeedId::time_to_edge, s) == doctest::Approx(0.5f));
    // Walking (100 units/s): the far point 2.8 s (~0.02), the near one 0.7 s (~0.92).
    s.edgeSpeed = 100.0f;
    s.edgeDistance = 280.0f;
    CHECK(Value(NeedId::time_to_edge, s) < 0.03f);
    s.edgeDistance = 70.0f;
    CHECK(Value(NeedId::time_to_edge, s) > 0.9f);
    // Standing beside the edge (what drop_ahead cannot tell): never.
    s.edgeSpeed = 0.0f;
    CHECK(Input(NeedId::time_to_edge, s) == kNever);
    CHECK(Value(NeedId::time_to_edge, s) < 1.0e-6f);
    // Moving away: never.
    s.edgeSpeed = -150.0f;
    CHECK(Input(NeedId::time_to_edge, s) == kNever);
    // A crawl so slow it would take longer than NEVER is NEVER, not more.
    s.edgeSpeed = 1.0e-6f;
    CHECK(Input(NeedId::time_to_edge, s) == kNever);
    // A NaN speed is no speed.
    s.edgeSpeed = std::nanf("");
    CHECK(Input(NeedId::time_to_edge, s) == kNever);
}

TEST_CASE("edge_approach: the need reads the trend as it is; the curve's centre is 0.3 per game second")
{
    NeedSnapshot s;
    CHECK(Input(NeedId::edge_approach, s) == 0.0f);
    // Still in front of a ledge: under the 0.05 signature step (hg needs, the [Needs] line).
    CHECK(SignatureLevel(Value(NeedId::edge_approach, s)) == 0);
    s.edgeApproach = 0.3f;
    CHECK(Value(NeedId::edge_approach, s) == doctest::Approx(0.5f));
    s.edgeApproach = 0.9f;
    CHECK(Value(NeedId::edge_approach, s) > 0.99f);
    s.edgeApproach = -0.8f;
    CHECK(Value(NeedId::edge_approach, s) < 1.0e-6f);
}

TEST_CASE("approach tracker: running at a cliff climbs, standing in front of it reads 0")
{
    EdgeApproachTracker t;
    float best = 0.0f;
    // From 600 units out at 300 units/s, a pass every 100 ms for 2 s.
    Approach(t, 600.0f, 300.0f, 0.1, 1.0, 2.0, &best);
    CHECK(best > 0.6f);
    CHECK(Evaluate(kNeeds[Index(NeedId::edge_approach)].curve, best) > 0.95f);

    // Standing 30 units from the edge for 2 s (every point over it): 0.
    t.Reset();
    const float still = Approach(t, 30.0f, 0.0f, 0.1, 1.0, 2.0);
    CHECK(still == doctest::Approx(0.0f));

    // Standing back from it (no point over it): 0 too.
    t.Reset();
    CHECK(Approach(t, 400.0f, 0.0f, 0.1, 1.0, 2.0) == doctest::Approx(0.0f));
}

TEST_CASE("approach tracker: walking at a cliff reads a smaller positive trend; backing off is negative")
{
    EdgeApproachTracker t;
    float best = 0.0f;
    Approach(t, 400.0f, 100.0f, 0.1, 1.0, 3.5, &best);
    CHECK(best > 0.2f);
    CHECK(best < 0.6f);

    // Backing away from the edge (the probes still look at it: the facing).
    t.Reset();
    float last = Approach(t, 0.0f, -300.0f, 0.1, 1.0, 1.0);
    CHECK(last < -0.3f);
}

TEST_CASE("approach tracker: game time, not wall time -- Wheeler's 0.2 reads the same trend")
{
    EdgeApproachTracker normal;
    EdgeApproachTracker slow;
    float bestNormal = 0.0f;
    float bestSlow = 0.0f;
    // The same walk in game time: 2 game s. Normal: 20 passes; at 0.2 the
    // probe still runs every 100 ms of WALL time, so 100 passes over 10 s.
    Approach(normal, 600.0f, 300.0f, 0.1, 1.0, 2.0, &bestNormal);
    Approach(slow, 600.0f, 300.0f, 0.1, 0.2, 10.0, &bestSlow);
    CHECK(slow.Size() > normal.Size());
    CHECK(bestSlow == doctest::Approx(bestNormal).epsilon(0.15));
    // On the wall clock the slow walk would read five times slower; it does not.
    CHECK(bestSlow > 0.6f);
    // 0.1 time scale: 150 passes in the window, under the capacity.
    EdgeApproachTracker slower;
    Approach(slower, 600.0f, 300.0f, 0.1, 0.1, 20.0);
    CHECK(slower.Size() <= EdgeApproachTracker::kCapacity);
    CHECK(slower.SpanSec() <= EdgeApproachTracker::kWindowSec + 1e-9);
}

TEST_CASE("approach tracker: a pass with no game time since the last adds nothing (time stopped)")
{
    EdgeApproachTracker t;
    for (int k = 0; k < 10; ++k) t.Add({ 10.0 + 0.1 * k, {}, 0.1f * static_cast<float>(k) });
    const float slope = t.Slope();
    const auto size = t.Size();
    CHECK(slope > 0.5f);
    // The clock did not move: the same slope, nothing added.
    CHECK(t.Add({ 10.9, {}, 0.0f }) == slope);
    CHECK(t.Size() == size);
}

TEST_CASE("approach tracker: too few passes or too short a span reads 0")
{
    EdgeApproachTracker t;
    CHECK(t.Add({ 1.0, {}, 0.0f }) == 0.0f);
    CHECK(t.Add({ 1.1, {}, 0.5f }) == 0.0f);   // two passes
    CHECK(t.Add({ 1.2, {}, 1.0f }) == 0.0f);   // three, 0.2 s
    CHECK(t.Add({ 1.4, {}, 1.0f }) == 0.0f);   // 0.4 s
    CHECK(t.Add({ 1.5, {}, 1.0f }) > 0.0f);    // 0.5 s
}

TEST_CASE("approach tracker: the window keeps 1.5 game seconds")
{
    EdgeApproachTracker t;
    for (int k = 0; k <= 50; ++k) t.Add({ 0.1 * k, {}, 0.0f });
    CHECK(t.SpanSec() <= EdgeApproachTracker::kWindowSec + 1e-9);
    CHECK(t.SpanSec() >= EdgeApproachTracker::kWindowSec - 0.1 - 1e-9);
    // An approach long gone is forgotten: a climb, then 1.5 s still.
    t.Reset();
    for (int k = 0; k <= 10; ++k) t.Add({ 0.1 * k, {}, 0.1f * static_cast<float>(k) });
    CHECK(t.Slope() > 0.5f);
    for (int k = 11; k <= 27; ++k) t.Add({ 0.1 * k, {}, 1.0f });
    CHECK(t.Slope() == doctest::Approx(0.0f));
}

TEST_CASE("approach tracker: a flip of the probe direction restarts the window")
{
    EdgeApproachTracker t;
    const Dir2 east{ 1.0f, 0.0f };
    const Dir2 north{ 0.0f, 1.0f };
    for (int k = 0; k < 10; ++k) t.Add({ 0.1 * k, east, 0.1f * static_cast<float>(k) });
    CHECK(t.Slope() > 0.5f);
    // Stopping after a strafe: the facing (north) takes over from the movement (east).
    CHECK(t.Add({ 1.0, north, 1.0f }) == 0.0f);
    CHECK(t.Size() == 1);
    // A switch that keeps the direction (stopping while facing the way one
    // walked) changes nothing.
    EdgeApproachTracker u;
    for (int k = 0; k < 10; ++k) u.Add({ 0.1 * k, east, 0.1f * static_cast<float>(k) });
    const Dir2 nearlyEast{ 0.98f, 0.199f };  // ~11 degrees
    CHECK(u.Add({ 1.0, nearlyEast, 1.0f }) > 0.5f);
    CHECK(u.Size() == 11);
}

TEST_CASE("approach tracker: a slow arc drops passes more than 45 degrees off the current direction")
{
    EdgeApproachTracker t;
    // Turning 10 degrees per pass: no flip between two passes, but after five
    // passes the first ones look somewhere else.
    for (int k = 0; k < 10; ++k) {
        const float a = static_cast<float>(k) * 10.0f * 3.14159265f / 180.0f;
        t.Add({ 0.1 * k, { std::sin(a), std::cos(a) }, 0.0f });
    }
    CHECK(t.Size() <= 5);
    CHECK(t.Size() >= 4);
}

TEST_CASE("approach tracker: a wall coming into reach reads the proximity falling, never an approach")
{
    EdgeApproachTracker t;
    // Flat ground, then every probe unknown: proximity stays ~0 (both are no drop).
    for (int k = 0; k < 10; ++k) {
        const std::array<ProbeHit, 3> hits = k < 5 ? Cliff(-1.0f) : std::array<ProbeHit, 3>{ Unknown(), Unknown(), Unknown() };
        t.Add({ 0.1 * k, {}, EdgeProximity(0.0f, hits, kCfg) });
    }
    CHECK(t.Slope() <= 1e-3f);
    // Over a cliff, then the probes go unknown (blocked): falling.
    t.Reset();
    for (int k = 0; k < 10; ++k) {
        const std::array<ProbeHit, 3> hits = k < 5 ? Cliff(0.0f) : std::array<ProbeHit, 3>{ Unknown(), Unknown(), Unknown() };
        t.Add({ 0.1 * k, {}, EdgeProximity(0.0f, hits, kCfg) });
    }
    CHECK(t.Slope() < 0.0f);
}

TEST_CASE("approach tracker: a clock running backwards, or a non-finite sample, restarts")
{
    EdgeApproachTracker t;
    for (int k = 0; k < 10; ++k) t.Add({ 5.0 + 0.1 * k, {}, 0.1f * static_cast<float>(k) });
    CHECK(t.Slope() > 0.5f);
    CHECK(t.Add({ 1.0, {}, 0.0f }) == 0.0f);  // a load's clock: back to the start
    CHECK(t.Size() == 1);
    for (int k = 1; k < 10; ++k) t.Add({ 1.0 + 0.1 * k, {}, 0.1f * static_cast<float>(k) });
    CHECK(t.Slope() > 0.5f);
    CHECK(t.Add({ 2.0, {}, std::nanf("") }) == 0.0f);
    CHECK(t.Size() == 0);
    CHECK(t.Add({ std::numeric_limits<double>::infinity(), {}, 0.0f }) == 0.0f);
    CHECK(t.Size() == 0);
}

TEST_CASE("approach tracker: Reset forgets the trend (a load, a cell change, a teleport, a skip)")
{
    EdgeApproachTracker t;
    for (int k = 0; k < 10; ++k) t.Add({ 0.1 * k, {}, 0.1f * static_cast<float>(k) });
    CHECK(t.Slope() > 0.5f);
    t.Reset();
    CHECK(t.Slope() == 0.0f);
    CHECK(t.Size() == 0);
    // Right after, a trend needs its minimum span again.
    CHECK(t.Add({ 2.0, {}, 1.0f }) == 0.0f);
    CHECK(t.Add({ 2.1, {}, 1.0f }) == 0.0f);
}

TEST_CASE("space key: a load door or a worldspace change differs; the same worldspace does not")
{
    const SpaceKey tamriel{ false, 0x3C };
    const SpaceKey tamriel2{ false, 0x3C };
    const SpaceKey cave{ true, 0x1234 };
    const SpaceKey otherCave{ true, 0x1235 };
    CHECK(tamriel == tamriel2);
    CHECK_FALSE(tamriel == cave);
    CHECK_FALSE(cave == otherCave);
    // An interior and an exterior with the same FormID are still different spaces.
    CHECK_FALSE((SpaceKey{ true, 0x3C }) == tamriel);
}
