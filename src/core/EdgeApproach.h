#pragma once

// =============================================================================
// EDGE APPROACH -- time_to_edge and edge_approach, from the drop_ahead probes
// =============================================================================
// drop_ahead (core/DropAhead.h) is a snapshot of one probe pass: the largest
// drop over three points ~70 / 175 / 280 units ahead. It reads the same
// standing beside a ledge as walking off one, and every ledge the player
// stood near opened a drop_ahead episode in the selection log that ended in
// a "nothing" record. Two needs (0.23.50, logged only like every need since
// R3) tell the cases apart, both from the same probe pass:
//
// time_to_edge -- seconds until the player reaches the edge at the current
//   pace. Distance: along the probe direction to the NEAREST KNOWN probe point
//   whose drop is dangerous (NearestEdgeDistance). Speed: the player's
//   horizontal velocity, in units per GAME second, along that direction
//   (SpeedToward). needs.csv's r3_input divides the two, capped at NEVER;
//   standing still, moving away or no edge reads NEVER, so the need is 0.
//
//   "Dangerous" is a drop at or past kEdgeDropThreshold, the drop_ahead
//   curve's own centre (466 units, needs.csv drop_ahead; read from NeedIds.h
//   so the two cannot drift). That is where drop_ahead reads 0.5, the fit's
//   midpoint: a fall that cost ~7% of max health in the LoreRim session of
//   2026-10-10. Lower was considered and not taken: no fitted fall under 329
//   units hurt, and 401-444 lost 2-6%, so a lower threshold calls every
//   harmless ledge an edge -- the flood this need is meant to end. Higher
//   would miss falls that hurt (every fitted one from 493 did). The csv
//   default is used, not the [Needs] override of drop_ahead: the probe runs
//   on the main thread and does not read the INI.
//
//   Coarse by construction: three points, so the distance is 70, 175 or 280
//   units and the edge lies somewhere between that point and the one before
//   it (or the feet). The distance is the far side of that interval, so the
//   time errs late by up to one spacing (105 units, 70 for the first point).
//   Accepted (the task that asked for it, 2026-10-28). The drop is measured
//   from the feet, so a downhill steeper than ~59 degrees (466 over 280)
//   reads as an edge at the far point.
//
// edge_approach -- whether the drop ahead has been getting closer or larger
//   over the last kWindowSec of GAME time. Each probe pass gives one
//   proximity (EdgeProximity): the mean over the three points of the
//   drop_ahead curve at that point's drop, an unknown point counting 0. As
//   the player closes on a cliff the far point sees it first, then the
//   middle, then the near one, so the proximity climbs 0 -> 1/3 -> 2/3 -> 1;
//   a drop growing under the points (a slope steepening into a cliff) climbs
//   it too. The need's input is the least-squares slope of proximity
//   against game time over the window (EdgeApproachTracker), in proximity
//   per game second: positive closing, negative backing off, 0 standing
//   still in front of a ledge (the case drop_ahead cannot tell).
//
// Deep water: DropAtProbe already reads a landing in water at least
// kSafeLandingDepth deep as no drop (0), so it is no edge and adds nothing
// to the proximity. Unknown probes (blocked, out of recasts) are never an
// edge and add 0.
//
// GAME TIME, as core/FallTracker.h: the speed and the window run on the game
// clock (DropAheadProbe::GameSeconds, GameFrameSec summed per frame), so an
// open Wheeler wheel (time at 0.2) neither stretches time_to_edge five times
// nor makes a closing player look still, and a pausing menu (no samples, the
// clock stopped) adds nothing.
// That clock errs short for these needs (verifier of #219): GameFrameSec
// takes the smaller of wall time x the multiplier and the frame delta and
// caps a frame at 0.1 s, the safe side for FallTracker. Here a short clock
// reads speed and slope high: one pass after a hitch longer than 0.1 s, and
// for the whole effect if a slow-time effect ever leaves the player moving at
// full speed (not verified for the Slow Time shout or LoreRim's detection
// slow-down). Both err toward an early time_to_edge, never a missed one.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "DropAhead.h"
#include "NeedIds.h"
#include "ResponseCurve.h"
#include "RingBuffer.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Huginn::Core::Needs
{
    /// The drop_ahead curve as needs.csv sets it (not the INI override).
    inline constexpr Curve kEdgeDropCurve = Info(NeedId::drop_ahead).curve;
    static_assert(kEdgeDropCurve.kind == CurveKind::Logistic,
                  "kEdgeDropThreshold is the drop_ahead curve's centre: a logistic's p1");

    /// A drop at least this deep is an edge (time_to_edge): drop_ahead's 0.5.
    inline constexpr float kEdgeDropThreshold = kEdgeDropCurve.p1;

    /// The distance along the probe direction to the nearest known probe point
    /// whose drop is at least kEdgeDropThreshold: cfg.distances[i], or nullopt
    /// when no known point is an edge. Unknown points are skipped (never an
    /// edge); a known point past an unknown one still counts (an out-of-recasts
    /// point leaves the next one known, core ProbeAll).
    [[nodiscard]] inline std::optional<float> NearestEdgeDistance(float feetZ, const std::array<ProbeHit, 3>& hits,
                                                                  const DropProbeConfig& cfg) noexcept
    {
        for (std::size_t i = 0; i < hits.size(); ++i) {
            if (!hits[i].known) continue;
            if (DropAtProbe(feetZ, hits[i], cfg) >= kEdgeDropThreshold) return cfg.distances[i];
        }
        return std::nullopt;
    }

    /// The horizontal velocity's component along `dir` (a unit vector), units
    /// per second of whatever clock the velocity was measured on. 0 for a
    /// non-finite result.
    [[nodiscard]] inline float SpeedToward(Vec3 velocity, Dir2 dir) noexcept
    {
        const float s = velocity.x * dir.x + velocity.y * dir.y;
        return std::isfinite(s) ? s : 0.0f;
    }

    /// One probe pass's proximity to an edge, 0-1: the mean over the three
    /// points of kEdgeDropCurve at the point's drop; an unknown point is 0.
    [[nodiscard]] inline float EdgeProximity(float feetZ, const std::array<ProbeHit, 3>& hits,
                                             const DropProbeConfig& cfg) noexcept
    {
        float sum = 0.0f;
        for (const auto& h : hits) {
            if (!h.known) continue;
            sum += Evaluate(kEdgeDropCurve, DropAtProbe(feetZ, h, cfg));
        }
        return sum / static_cast<float>(hits.size());
    }

    /// Where the player is, for the reset on a change of space: an interior
    /// cell (its FormID) or an exterior worldspace (its FormID). Walking from
    /// one exterior cell into the next is the same space and keeps the window;
    /// a load door or a worldspace change is not.
    struct SpaceKey
    {
        bool interior = false;
        std::uint32_t formId = 0;

        friend constexpr bool operator==(const SpaceKey&, const SpaceKey&) = default;
    };

    /// One probe pass, as the tracker sees it.
    struct EdgeSample
    {
        double gameSec = 0.0;   // the game clock at the pass (DropAheadProbe::GameSeconds)
        Dir2 dir{};             // the probe direction (ProbeDirection)
        float proximity = 0.0f; // EdgeProximity
    };

    // =========================================================================
    // EDGE APPROACH TRACKER -- edge_approach's trend
    // =========================================================================
    // Holds the probe passes of the last kWindowSec of game time and returns
    // the least-squares slope of their proximity against the game clock.
    //
    // Which passes are comparable: the probes look where the probe direction
    // points, so two passes are only compared when they look the same way.
    //   - The direction flips (more than kMaxTurnDeg between this pass and
    //     the last): the window starts again from this pass. That covers the
    //     switch between the movement and the facing (core ProbeDirection's
    //     minMoveSpeed) when the two differ -- stopping after a strafe, a
    //     turn on the spot -- and a sharp turn while moving. A switch between
    //     movement and facing that keeps the direction (stopping while facing
    //     the way one walked) changes nothing: what makes passes comparable
    //     is where the probes look, not why.
    //   - A slow turn: passes at the front of the window more than
    //     kMaxTurnDeg off the current direction are dropped, so a long arc
    //     cannot compare ground ahead now with ground off to the side.
    // An unknown probe adds 0 to a pass's proximity (EdgeProximity), so a
    // wall coming into reach reads as the proximity falling, never rising.
    //
    // Reset (Reset()): the game side resets on a game load (and the main
    // menu or a loading screen), a change of space (SpaceKey: a load door, a
    // worldspace), a teleport (core IsTeleport: coc, fast travel, a 500-unit
    // jump), and every pass the probe skips (airborne, swimming, mounted, no
    // 3D, no cell, no physics world): the passes before a jump or a swim
    // describe ground the player has left. Resetting only loses a trend,
    // which is the safe side: edge_approach then reads 0, never a false
    // approach.
    //
    // Game time: a pass at the same game second as the last (time stopped,
    // no game time between two probes) adds nothing; a clock that runs
    // backwards resets. Fewer than kMinSamples passes or less than
    // kMinSpanSec of game time between the first and the last reads 0: a
    // slope over two passes 0.1 s apart is one flicker of a probe.
    class EdgeApproachTracker
    {
    public:
        static constexpr double kWindowSec = 1.5;   // the task's "~1-2 s", game seconds
        static constexpr double kMinSpanSec = 0.5;  // five passes at the 100 ms probe in normal time
        static constexpr std::size_t kMinSamples = 3;
        static constexpr float kMaxTurnDeg = 45.0f;
        static constexpr float kMaxTurnCos = 0.70710678f;  // cos 45 degrees
        // 1.5 game seconds of passes one per 100 ms of WALL time: 15 in normal
        // time, 75 under Wheeler's 0.2, 150 at 0.1. Past capacity the oldest
        // pass goes first (the window only shortens).
        static constexpr std::size_t kCapacity = 160;

        /// Add one pass; returns the slope (proximity per game second).
        float Add(const EdgeSample& s) noexcept
        {
            if (!std::isfinite(s.gameSec) || !std::isfinite(s.proximity) || !std::isfinite(s.dir.x) ||
                !std::isfinite(s.dir.y)) {
                Reset();
                return 0.0f;
            }
            if (!samples_.empty()) {
                const auto& last = samples_.back();
                if (s.gameSec < last.gameSec) {
                    Reset();
                }
                else if (s.gameSec == last.gameSec) {
                    return slope_;  // no game time passed: nothing new to trend
                }
                else if (Cos(last.dir, s.dir) < kMaxTurnCos) {
                    Reset();  // a flip: the probes now look somewhere else
                }
            }
            while (!samples_.empty() && (samples_.front().gameSec < s.gameSec - kWindowSec ||
                                         Cos(samples_.front().dir, s.dir) < kMaxTurnCos)) {
                samples_.pop_front();
            }
            samples_.push_back(s);
            slope_ = ComputeSlope();
            return slope_;
        }

        void Reset() noexcept
        {
            samples_.clear();
            slope_ = 0.0f;
        }

        [[nodiscard]] float Slope() const noexcept { return slope_; }
        [[nodiscard]] std::size_t Size() const noexcept { return samples_.size(); }
        [[nodiscard]] double SpanSec() const noexcept
        {
            return samples_.empty() ? 0.0 : samples_.back().gameSec - samples_.front().gameSec;
        }

    private:
        [[nodiscard]] static float Cos(Dir2 a, Dir2 b) noexcept { return a.x * b.x + a.y * b.y; }

        [[nodiscard]] float ComputeSlope() const noexcept
        {
            const std::size_t n = samples_.size();
            if (n < kMinSamples || SpanSec() < kMinSpanSec) return 0.0f;
            // Centred on the first pass's time, in double: game seconds since
            // the plugin loaded can be large, a window's differences are small.
            const double t0 = samples_.front().gameSec;
            double st = 0.0, sp = 0.0;
            for (const auto& x : samples_) {
                st += x.gameSec - t0;
                sp += x.proximity;
            }
            const double mt = st / static_cast<double>(n);
            const double mp = sp / static_cast<double>(n);
            double cov = 0.0, var = 0.0;
            for (const auto& x : samples_) {
                const double dt = x.gameSec - t0 - mt;
                cov += dt * (x.proximity - mp);
                var += dt * dt;
            }
            if (!(var > 0.0)) return 0.0f;
            const double slope = cov / var;
            return std::isfinite(slope) ? static_cast<float>(slope) : 0.0f;
        }

        RingBuffer<EdgeSample, kCapacity> samples_;
        float slope_ = 0.0f;
    };
}
