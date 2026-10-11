// core/FallTracker.h: the falling need's fall depth, the descent below the
// take-off point of a free fall (#60; slides re-anchor since 0.23.29). The
// jump / drop / relocation / unanchored / reset cases were in-game tests
// (Tests.cpp 6c-2) until the tracker moved here.

#include "core/FallTracker.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Huginn::Core::Needs;

namespace
{
    constexpr float kMaxDelta = 1500.0f;  // a 100 ms poll at MAX_FALL_SPEED (15000 units/s)
    constexpr double kPoll = 0.1;         // seconds between polls

    // Polls a Z profile kPoll of game time apart; the first sample
    // grounded, the rest airborne. Returns each poll's depth.
    std::vector<float> Fly(FallTracker& t, const std::vector<float>& zs)
    {
        std::vector<float> out;
        for (size_t i = 0; i < zs.size(); ++i) {
            out.push_back(t.Update(zs[i], /*airborne=*/i != 0, kMaxDelta, i == 0 ? 0.0 : kPoll));
        }
        return out;
    }

    // Z of a free fall from rest at `top` after `sec` game seconds.
    float FreeFallZ(float top, float g, double sec)
    {
        return top - 0.5f * g * static_cast<float>(sec * sec);
    }

    // A free fall from rest at take-off Z `top`, under gravity g, sampled
    // every poll for n polls after the grounded one.
    std::vector<float> FreeFall(float top, float g, int n)
    {
        std::vector<float> zs{ top };
        for (int k = 1; k <= n; ++k) {
            const float sec = static_cast<float>(k * kPoll);
            zs.push_back(top - 0.5f * g * sec * sec);
        }
        return zs;
    }

    // A constant descent of `speed` units/s (a slide down a slope).
    std::vector<float> Slide(float top, float speed, int n)
    {
        std::vector<float> zs{ top };
        for (int k = 1; k <= n; ++k) zs.push_back(top - speed * static_cast<float>(k * kPoll));
        return zs;
    }

    float Max(const std::vector<float>& v) { return *std::max_element(v.begin(), v.end()); }
}

TEST_CASE("fall tracker: a jump returns to its take-off height and never reads a depth")
{
    FallTracker t;
    for (const float d : Fly(t, { 100.0f, 140.0f, 170.0f, 176.0f, 150.0f, 110.0f, 100.0f })) {
        CHECK(d == 0.0f);
    }
}

TEST_CASE("fall tracker: a drop off a ledge measures the descent below take-off; landing re-anchors")
{
    FallTracker t;
    CHECK(t.Update(1000.0f, false, kMaxDelta, 0.0) == 0.0f);
    CHECK(t.Update(800.0f, true, kMaxDelta, 0.5) == doctest::Approx(200.0f));
    CHECK(t.Update(300.0f, true, kMaxDelta, 0.5) == doctest::Approx(700.0f));
    CHECK(t.Update(300.0f, false, kMaxDelta, 0.1) == 0.0f);
}

TEST_CASE("fall tracker: a free fall reads its full depth, at the logged gravity and at the floor's")
{
    // ~950 units/s^2: the logged falls' own gravity (2026-10-10). 686: the
    // bound itself (Havok's 9.8 m/s^2), from rest: the slowest free fall.
    for (const float g : { 950.0f, kFallGravity }) {
        FallTracker t;
        const auto zs = FreeFall(5000.0f, g, 14);
        const auto depths = Fly(t, zs);
        for (size_t i = 1; i < zs.size(); ++i) {
            CHECK(depths[i] == doctest::Approx(5000.0f - zs[i]));
        }
        CHECK(depths.back() > 600.0f);  // well past the falling need's full point
    }
}

TEST_CASE("fall tracker: a jump off a ledge (rising first) reads the descent below take-off")
{
    FallTracker t;
    // Up ~76 units and back through the take-off height, then down at 950.
    const std::vector<float> zs{ 1000.0f, 1060.0f, 1076.0f, 1050.0f, 990.0f, 905.0f, 800.0f, 680.0f, 540.0f };
    const auto depths = Fly(t, zs);
    CHECK(depths[3] == 0.0f);
    CHECK(depths[4] == doctest::Approx(10.0f));
    CHECK(depths.back() == doctest::Approx(460.0f));
}

TEST_CASE("fall tracker: a slide never reaches the 200-unit gate (trace-04 18:47:11)")
{
    // 18:47:11: depth up 136 units in 1.07 s, ~130 units/s, for 1.5 s; the
    // old tracker read 549. Steeper slides too, up to 300 units/s.
    for (const float speed : { 130.0f, 200.0f, 300.0f }) {
        FallTracker t;
        const auto depths = Fly(t, Slide(1000.0f, speed, 20));
        CAPTURE(speed);
        CHECK(Max(depths) < 200.0f);
    }
}

TEST_CASE("fall tracker: hops down a slope (depth shrinking mid-descent) re-anchor")
{
    FallTracker t;
    // Touches between polls: down, a bounce up, down again, never grounded
    // at a poll.
    const auto depths = Fly(t, { 1000.0f, 980.0f, 950.0f, 960.0f, 930.0f, 900.0f, 910.0f, 880.0f, 850.0f });
    CHECK(depths[3] == 0.0f);  // the bounce: not a free fall
    CHECK(Max(depths) < 200.0f);
}

TEST_CASE("fall tracker: a slide that ends at an edge measures the free fall from where it stopped counting")
{
    FallTracker t;
    auto zs = Slide(1000.0f, 130.0f, 12);  // 1.2 s of slide, ~156 units down
    std::vector<float> depths = Fly(t, zs);
    CHECK(Max(depths) < 200.0f);
    // Off the edge: a free fall from the slide's last Z, from rest.
    const float edge = zs.back();
    float last = 0.0f;
    for (int k = 1; k <= 10; ++k) {
        last = t.Update(FreeFallZ(edge, 950.0f, k * kPoll), true, kMaxDelta, kPoll);
    }
    // 475 units of free fall below the edge, plus at most the slide's tail
    // since its last re-anchor.
    CHECK(last >= 475.0f);
    CHECK(last < 475.0f + 200.0f);
}

TEST_CASE("fall tracker: a pausing menu mid-fall holds the depth (no game time passes)")
{
    FallTracker t;
    const auto zs = FreeFall(3000.0f, 950.0f, 6);
    const auto depths = Fly(t, zs);
    const float held = depths.back();
    REQUIRE(held > 150.0f);
    // Ten seconds of polls in a paused menu: game time stands still (the
    // game clock adds nothing), Z frozen, still midair.
    for (int k = 0; k < 100; ++k) {
        CHECK(t.Update(zs.back(), true, kMaxDelta, 0.0) == doctest::Approx(held));
    }
    // Unpaused, the fall resumes where it was.
    for (int k = 7; k <= 10; ++k) {
        const float z = FreeFallZ(3000.0f, 950.0f, k * kPoll);
        CHECK(t.Update(z, true, kMaxDelta, kPoll) == doctest::Approx(3000.0f - z));
    }
}

TEST_CASE("fall tracker: a relocation re-anchors instead of reading the world-space difference")
{
    FallTracker t;
    (void)t.Update(20000.0f, false, kMaxDelta, 0.0);
    CHECK(t.Update(100.0f, true, kMaxDelta, kPoll) == 0.0f);
    // ...and tracking resumes from the new anchor.
    CHECK(t.Update(-200.0f, true, kMaxDelta, kPoll) == doctest::Approx(300.0f));
}

TEST_CASE("fall tracker: unanchored first poll and Reset() never measure against a stale origin")
{
    {
        FallTracker t;
        CHECK(t.Update(-5000.0f, true, kMaxDelta, 0.0) == 0.0f);
    }
    {
        FallTracker t;
        (void)t.Update(20000.0f, false, kMaxDelta, 0.0);
        t.Reset();
        CHECK_FALSE(t.IsAnchored());
        CHECK(t.Update(100.0f, true, kMaxDelta, kPoll) == 0.0f);
    }
}

TEST_CASE("free-fall bound: the logged falls of 2026-10-10 against it")
{
    CHECK(kFallGravity == doctest::Approx(686.0f).epsilon(0.001));
    // From rest, 0 -> d takes sqrt(2 d / g).
    CHECK(FreeFallMaxSec(0.0f, kFallGravity) == doctest::Approx(std::sqrt(2.0f)));
    // The log's duration includes the landing poll (<= 0.12 s); the bound
    // is checked on the time between the two depth samples.
    // 13:51:16: 205 -> 639 units in 0.657 s, 35% of health: a free fall.
    CHECK_FALSE(SlowerThanFreeFall(205.0f, 639.0f, 0.657f - 0.12f));
    // 11:46:45: 237 -> 493 in 0.431 s, 9.8%: a free fall.
    CHECK_FALSE(SlowerThanFreeFall(237.0f, 493.0f, 0.431f - 0.12f));
    // 18:47:11: 247 -> 549 in 1.507 s, no damage: a slide.
    CHECK(SlowerThanFreeFall(247.0f, 549.0f, 1.507f - 0.12f));
    // 12:08:36: 202 -> 645 in 1.113 s, no damage: a slide.
    CHECK(SlowerThanFreeFall(202.0f, 645.0f, 1.113f - 0.12f));
    // Rising (d1 < d0) for longer than the slack is never a free fall.
    CHECK(SlowerThanFreeFall(300.0f, 290.0f, 0.1f));
}

// WHAT THE GAME-CLOCK TESTS BELOW PROVE, AND WHAT THEY DO NOT. They show the
// tracker keeps a free fall WHEN the clock it is handed is right: slow time,
// a hitch, a pausing menu, a long first frame after one. They do not prove
// the clock is right. DropAheadProbe's hook summing GameFrameSec per frame
// (from the real global time multiplier and update delta), and
// PollPlayerPosition's subtraction of two GameSeconds readings, are engine
// code; only the in-game check proves them: with Wheeler open mid-fall, the
// `[Falling] end` line's game time reads about a fifth of its wall time
// (roadmap R3).

namespace
{
    // One poll's game time from frames at 60 fps under a global time
    // multiplier, the update delta scaled the same way (GameFrameSec, as
    // DropAheadProbe's hook sums it).
    double PollGameSec(double timeMult, int frames = 6)
    {
        double sum = 0.0;
        for (int f = 0; f < frames; ++f) sum += GameFrameSec(1.0 / 60.0, timeMult, timeMult / 60.0);
        return sum;
    }
}

TEST_CASE("game clock: one frame is the wall frame times the time multiplier, or the update delta if smaller")
{
    CHECK(GameFrameSec(0.016, 1.0, 0.016) == doctest::Approx(0.016));
    CHECK(GameFrameSec(0.016, 0.2, 0.016) == doctest::Approx(0.0032));   // Wheeler slow time, delta unscaled
    CHECK(GameFrameSec(0.016, 0.2, 0.0032) == doctest::Approx(0.0032));  // ...or scaled: the same
    CHECK(GameFrameSec(0.300, 1.0, 0.050) == doctest::Approx(0.050));    // a hitch the game clamps
    CHECK(GameFrameSec(0.0, 1.0, 0.016) == 0.0);                         // a second update in one frame
    CHECK(GameFrameSec(5.0, 1.0, 5.0) == doctest::Approx(kFallMaxFrameSec));
    CHECK(kFallMaxFrameSec <= 0.1);  // ~3 frames at 30 fps, never a pause's length
    CHECK(GameFrameSec(-1.0, 1.0, 0.016) == 0.0);
    CHECK(GameFrameSec(std::nan(""), 1.0, 0.016) == 0.0);
}

TEST_CASE("fall tracker: Wheeler's slow time (0.2) opened mid-fall keeps the full depth")
{
    // A 3000-unit fall at the logged gravity; the wheel opens 0.6 s in and
    // stays open: the polls stay 0.1 s of wall time apart, the game (and
    // the fall) runs at a fifth of that. The wall-clock tracker of the
    // first 0.23.29 commit re-anchored this to ~0 within a few polls (PR
    // #202's verifier).
    constexpr float kTop = 3000.0f;
    constexpr float kG = 954.0f;
    FallTracker t;
    double game = 0.0;
    float depth = t.Update(kTop, false, kMaxDelta, 0.0);
    for (int poll = 1; poll <= 60; ++poll) {
        const double dt = PollGameSec(poll <= 6 ? 1.0 : 0.2);
        game += dt;
        const float z = FreeFallZ(kTop, kG, game);
        depth = t.Update(z, true, kMaxDelta, dt);
        CAPTURE(poll);
        CHECK(depth == doctest::Approx(kTop - z));
    }
    CHECK(depth > 600.0f);  // ~1.9 s of game time: past the falling need's full point
}

TEST_CASE("fall tracker: a hitch does not re-anchor a fall")
{
    // A 0.3 s frame early in the fall. The physics either advances the
    // game's clamped 0.05 s or the whole 0.3 s; the clock counts at most
    // kFallMaxFrameSec of it, which is never more than the physics did, so
    // the fall only looks faster.
    constexpr float kTop = 2000.0f;
    constexpr float kG = 954.0f;
    for (const double physicsSec : { 0.05, 0.3 }) {
        CAPTURE(physicsSec);
        FallTracker t;
        double physics = 0.0;
        (void)t.Update(kTop, false, kMaxDelta, 0.0);
        float depth = 0.0f;
        for (int poll = 1; poll <= 10; ++poll) {
            double step = kPoll;
            double clock = kPoll;
            if (poll == 3) {
                step = physicsSec;
                clock = GameFrameSec(0.3, 1.0, physicsSec);
            }
            physics += step;
            const float z = FreeFallZ(kTop, kG, physics);
            depth = t.Update(z, true, kMaxDelta, clock);
            CHECK(depth == doctest::Approx(kTop - z));
        }
        CHECK(depth > 400.0f);
    }
}

TEST_CASE("fall tracker: a long first frame after a pausing menu does not re-anchor a fall")
{
    // A pausing menu opened 0.5 s into a fall: no frames run, the clock
    // stands still. The first frame after it has the whole 8 s pause behind
    // it on the wall clock, and nobody has traced whether its update delta
    // is one frame or the gap. Either way it counts at most
    // kFallMaxFrameSec.
    constexpr float kTop = 3000.0f;
    constexpr float kG = 954.0f;
    for (const double firstDelta : { 1.0 / 60.0, 8.0 }) {
        CAPTURE(firstDelta);
        FallTracker t;
        double physics = 0.0;
        (void)t.Update(kTop, false, kMaxDelta, 0.0);
        float depth = 0.0f;
        for (int poll = 1; poll <= 5; ++poll) {
            physics += kPoll;
            depth = t.Update(FreeFallZ(kTop, kG, physics), true, kMaxDelta, kPoll);
        }
        // Paused: polls with no game time and the Z frozen.
        for (int poll = 0; poll < 80; ++poll) {
            CHECK(t.Update(FreeFallZ(kTop, kG, physics), true, kMaxDelta, 0.0) == doctest::Approx(depth));
        }
        // The first poll after: one long frame, then five normal ones; the
        // physics ran one normal frame plus those five.
        const double clock = GameFrameSec(8.0, 1.0, firstDelta) + 5.0 / 60.0;
        physics += 6.0 / 60.0;
        depth = t.Update(FreeFallZ(kTop, kG, physics), true, kMaxDelta, clock);
        CHECK(depth == doctest::Approx(kTop - FreeFallZ(kTop, kG, physics)));
        for (int poll = 0; poll < 5; ++poll) {
            physics += kPoll;
            depth = t.Update(FreeFallZ(kTop, kG, physics), true, kMaxDelta, kPoll);
            CHECK(depth == doctest::Approx(kTop - FreeFallZ(kTop, kG, physics)));
        }
    }
}
