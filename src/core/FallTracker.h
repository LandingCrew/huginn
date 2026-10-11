#pragma once

#include <algorithm>
#include <cmath>

namespace Huginn::Core::Needs
{
    // =========================================================================
    // FALL TRACKER (#60; free falls only since 0.23.29)
    // =========================================================================
    // How far the player has descended in the current free fall: below the
    // point they last left the ground, or last stopped falling freely.
    // `IsInMidair()` alone is true for every jump, kerb and knockback, which
    // is what drove slow-fall weight to full on any hop; a take-off-Z delta
    // clears that by construction, because a jump returns to its own take-off
    // height.
    //
    // A SLIDE IS NOT A FALL (0.23.29). The take-off Z used to move only on a
    // poll that saw the player grounded, so a slide down a slope, or a run of
    // hops whose touches fell between polls, kept adding depth: trace-04
    // (2026-10-10 18:47:11) logged a 549-unit "fall" that took 1.51 s from
    // the 200-unit gate and did no damage, its depth growing 136 units in
    // 1.07 s where a free fall adds about 1,000. The falling need means a
    // free fall of a dangerous height, and the game's fall damage did not
    // count that one. So the descent is also checked against gravity: a free
    // fall from rest at the take-off height reaches depth d after
    // sqrt(2 d / g), and any downward speed at take-off (a jump coming back
    // down) only shortens that, so between two depth samples d0 < d1 of one
    // free fall at most
    //     FreeFallMaxSec(d0, d1) = sqrt(2 / g) * (sqrt(d1) - sqrt(d0))
    // can pass. A descent slower than that (less kFallTimingSlackSec) is not a
    // free fall, and the tracker re-anchors where the player is: the depth
    // starts again from 0 and a slide never reaches the 200-unit gate.
    // tools/needs/fit_drop_curve.py rejects slides from the drop-curve fit
    // with the same bound.
    //
    // The bound is measured from the first poll of the descent (the first
    // with depth > 0), so the time a jump spends rising does not count.
    //
    // GAME TIME, NOT WALL TIME. The descent's clock is the game's own: the
    // caller passes the game seconds since the last poll (GameFrameSec,
    // summed per frame on the game side), never the wall clock. Gravity acts
    // in game time, and the game's time runs slow in exactly the moments the
    // player reaches for Slow Fall: Wheeler opens its wheel at
    // SlowTimeScale (LoreRim: 0.2), and the Slow Time shout, bow zoom and
    // LoreRim's "time slows when detected" slow it too. On the wall clock a
    // fall seen through an open wheel descends five times slower than any
    // free fall and was re-anchored to 0 within a few polls (PR #202's
    // verifier). A pausing menu stops game time, so it adds nothing and the
    // depth holds; a hitch the game clamps adds only the clamped frame.
    //
    // Deliberately free of engine types and of StateManager. The take-off Z is
    // rolling state that outlives a single poll, and every interesting failure
    // is about it describing a place the player is no longer in — a save load,
    // a cell door, a fast travel. Keeping the policy pure is what lets those
    // cases be tested without a live PlayerCharacter (tests/core/
    // FallTrackerTests.cpp).
    // =========================================================================

    /// Skyrim's gravity, units/s^2: Havok's 9.8 m/s^2 at 69.99 units per
    /// metre. The logged falls of 2026-10-10 descend faster than this (their
    /// own gravity, from rest, gate to peak: a median of ~950, the 5471-unit
    /// sea fall ~960), and a smaller g only lengthens the allowed time, so
    /// it errs toward keeping a fall.
    inline constexpr float kFallGravity = 9.8f * 69.99f;

    /// Timing error of one descent measurement: each sample's Z is the last
    /// physics step's, up to a frame older than its poll, so the time between
    /// two samples is off by up to two frames, at the 30 fps floor.
    inline constexpr float kFallTimingSlackSec = 2.0f / 30.0f;

    /// A poll gap counts at most this much descent time (a loop that did not
    /// poll for a while must not charge the whole gap to the fall). Same as
    /// core/DropAhead.h ReadingAge::kMaxPollStepSec.
    inline constexpr double kFallMaxPollStepSec = 0.5;

    /// One frame's game time, for the descent clock: the smaller of the
    /// wall-clock frame scaled by the game's global time multiplier (what
    /// Wheeler's slow time writes) and the delta the game hands the player's
    /// update (which the game clamps through a hitch). Either one alone
    /// would do if it is what the physics steps on; the smaller is the safe
    /// choice when one of them is not, since a shorter clock only lengthens
    /// the time a free fall is allowed. A frame with no wall time behind it
    /// (a second update in the same frame) counts nothing. Negative or
    /// non-finite inputs count nothing; one frame counts at most
    /// kFallMaxPollStepSec.
    [[nodiscard]] inline double GameFrameSec(double wallFrameSec, double timeMult, double updateDeltaSec) noexcept
    {
        const double scaled = wallFrameSec * timeMult;
        double step = std::min(scaled, updateDeltaSec);
        if (!(step > 0.0)) return 0.0;  // also NaN
        return std::min(step, kFallMaxPollStepSec);
    }

    /// Longest time a free fall takes from depth d0 down to d1 below its
    /// take-off height (from rest there; faster with any downward speed).
    [[nodiscard]] inline float FreeFallMaxSec(float d0, float d1, float gravity = kFallGravity) noexcept
    {
        return std::sqrt(2.0f / gravity) *
            (std::sqrt(std::max(d1, 0.0f)) - std::sqrt(std::max(d0, 0.0f)));
    }

    /// True when descending from d0 to d1 took longer than any free fall can
    /// (less `slackSec` of timing error): a slide, a slope, hops.
    [[nodiscard]] inline bool SlowerThanFreeFall(float d0, float d1, float elapsedSec,
                                                 float slackSec = kFallTimingSlackSec) noexcept
    {
        return elapsedSec - slackSec > FreeFallMaxSec(d0, d1);
    }

    struct FallTracker
    {
        /// Advance one poll; returns the current fall depth (0 when grounded,
        /// rising, unanchored, just relocated, or just found not falling
        /// freely).
        ///
        /// @param currentZ           Player Z this poll.
        /// @param airborne           Engine's midair flag, already excluding
        ///                           states that are not falling (swimming).
        /// @param maxPlausibleDelta  Largest Z change gravity could produce in
        ///                           one poll. A larger step is a relocation,
        ///                           not a fall — see Reset() for why that
        ///                           matters even mid-session.
        /// @param gameDtSec          GAME seconds since the last poll (the
        ///                           summed GameFrameSec): 0 through a pausing
        ///                           menu, a fifth of the wall clock under
        ///                           Wheeler's slow time. Never the wall clock.
        [[nodiscard]] float Update(float currentZ, bool airborne, float maxPlausibleDelta,
                                   double gameDtSec) noexcept
        {
            // At most kFallMaxPollStepSec of descent per poll; NaN or
            // negative counts nothing (the lenient direction).
            const double stepSec = gameDtSec > 0.0 ? std::min(gameDtSec, kFallMaxPollStepSec) : 0.0;

            // A save load, a cell door or a fast travel moves the player
            // discontinuously, and the remembered take-off Z then describes a
            // different place entirely. Loading into an interior at Z≈100 after
            // last standing on a peak at Z≈20000 would otherwise report a
            // 19,900-unit fall: full weight, Falling dominant, and a louder
            // version of the very bug this tracker exists to remove.
            const bool relocated = m_anchored &&
                std::abs(currentZ - m_lastZ) > maxPlausibleDelta;

            m_lastZ = currentZ;

            // Re-anchor rather than measure when we have no trustworthy origin:
            // on the ground (the normal case), before the first grounded sample
            // (a session may begin midair while physics settles), or after a
            // relocation. Re-anchoring can only lose a fall already in
            // progress, which is the safe direction to be wrong in.
            if (!airborne || !m_anchored || relocated) {
                Anchor(currentZ);
                return 0.0f;
            }

            // Rising, or back at the take-off height, keeps the depth at 0
            // rather than going negative, so the upward half of a jump
            // contributes nothing, and the descent's clock has not started.
            const float depth = m_groundZ - currentZ;
            if (depth <= 0.0f) {
                m_descending = false;
                return 0.0f;
            }
            if (!m_descending) {
                m_descending = true;
                m_descentStartDepth = depth;
                m_descentSec = 0.0;
                return depth;
            }
            m_descentSec += stepSec;

            // Slower than any free fall: a slide or a slope, not a fall.
            // Re-anchor here; if the player then drops off an edge, that free
            // fall is measured from where the slide stopped counting.
            if (SlowerThanFreeFall(m_descentStartDepth, depth, static_cast<float>(m_descentSec))) {
                Anchor(currentZ);
                return 0.0f;
            }
            return depth;
        }

        /// Drop the anchor. Required on save load: the previous save's take-off
        /// Z describes a different world position, and ResetTrackingState()
        /// zeroes the poll timers, so the next poll runs immediately — possibly
        /// while still airborne as the cell settles.
        void Reset() noexcept { *this = FallTracker{}; }

        [[nodiscard]] bool IsAnchored() const noexcept { return m_anchored; }

    private:
        void Anchor(float z) noexcept
        {
            m_groundZ = z;
            m_anchored = true;
            m_descending = false;
        }

        float m_groundZ = 0.0f;
        float m_lastZ = 0.0f;
        bool m_anchored = false;
        // The current descent below the anchor: its first sample's depth and
        // the game seconds since that sample.
        bool m_descending = false;
        float m_descentStartDepth = 0.0f;
        double m_descentSec = 0.0;
    };
}
