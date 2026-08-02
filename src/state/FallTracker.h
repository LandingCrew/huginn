#pragma once

#include <algorithm>
#include <cmath>

namespace Huginn::State
{
    // =========================================================================
    // FALL TRACKER (#60)
    // =========================================================================
    // How far the player has descended below the point they last left the
    // ground. `IsInMidair()` alone is true for every jump, kerb and knockback,
    // which is what drove slow-fall weight to full on any hop; a take-off-Z
    // delta clears that by construction, because a jump returns to its own
    // take-off height.
    //
    // Deliberately free of engine types and of StateManager. The take-off Z is
    // rolling state that outlives a single poll, and every interesting failure
    // is about it describing a place the player is no longer in — a save load,
    // a cell door, a fast travel. Keeping the policy pure is what lets those
    // cases be tested without a live PlayerCharacter.
    // =========================================================================

    struct FallTracker
    {
        /// Advance one poll; returns the current fall depth (0 when grounded,
        /// rising, unanchored, or just relocated).
        ///
        /// @param currentZ           Player Z this poll.
        /// @param airborne           Engine's midair flag, already excluding
        ///                           states that are not falling (swimming).
        /// @param maxPlausibleDelta  Largest Z change gravity could produce in
        ///                           one poll. A larger step is a relocation,
        ///                           not a fall — see Reset() for why that
        ///                           matters even mid-session.
        [[nodiscard]] float Update(float currentZ, bool airborne,
                                   float maxPlausibleDelta) noexcept
        {
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
                m_groundZ = currentZ;
                m_anchored = true;
                return 0.0f;
            }

            // Rising keeps the depth at 0 rather than going negative, so the
            // upward half of a jump contributes nothing.
            return std::max(0.0f, m_groundZ - currentZ);
        }

        /// Drop the anchor. Required on save load: the previous save's take-off
        /// Z describes a different world position, and ResetTrackingState()
        /// zeroes the poll timers, so the next poll runs immediately — possibly
        /// while still airborne as the cell settles.
        void Reset() noexcept { *this = FallTracker{}; }

        [[nodiscard]] bool IsAnchored() const noexcept { return m_anchored; }

    private:
        float m_groundZ = 0.0f;
        float m_lastZ = 0.0f;
        bool m_anchored = false;
    };
}
