#pragma once

// =============================================================================
// LIGHT HOLD -- the light level the darkness need reads (0.23.41)
// =============================================================================
// The darkness need (needs.csv: 1 - light, logistic c 0.65) read the light
// on the player every tick, quantised to 0.1. Torchlight, passing shadows and
// the player's own movement swung it between 0.3 and 0.5 (darkness 0.68 <->
// 0.10) every second or two: 57% of the 1165 darkness "nothing" episodes in
// the 0.23.15-0.23.38 v3 log lasted under 5 s, the most frequent unanswered
// episode of all. The old engine's dark flag had already met this
// (state/DarknessGate.h: a dwell both ways, a new place at once); this is the
// same rule on the continuous reading:
//
//   - a new place (an interior cell, or an exterior worldspace -- not the
//     exterior grid cell) or the first reading: taken at once. Walking into
//     a cave reads dark at once; a load screen is a real change;
//   - otherwise a reading darker than the held one is taken once readings
//     have stayed darker for `darkerSec` (5 s, DARK_ENTER_DELAY), a lighter
//     one once they have stayed lighter for `lighterSec` (3 s,
//     DARK_EXIT_HOLD); the value taken is the reading at that moment;
//   - a reading back at the held level, or on the other side of it, restarts
//     the wait.
//
// Seconds on one monotonic clock (the caller's); nothing here reads a clock.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>

namespace Huginn::Core::Needs
{
    class LightHold
    {
    public:
        static constexpr double kDarkerSec = 5.0;
        static constexpr double kLighterSec = 3.0;

        LightHold() = default;
        LightHold(double darkerSec, double lighterSec) noexcept : m_darkerSec(darkerSec), m_lighterSec(lighterSec) {}

        /// Feed one reading; returns the held light.
        float Update(float light, std::uint32_t placeID, double nowSec) noexcept
        {
            if (!m_started || placeID != m_placeID) {
                m_started = true;
                m_placeID = placeID;
                m_held = light;
                m_dir = 0;
                return m_held;
            }
            const int dir = light < m_held ? -1 : light > m_held ? 1 : 0;
            if (dir == 0) {
                m_dir = 0;
                return m_held;
            }
            if (dir != m_dir || nowSec < m_since) {
                m_dir = dir;
                m_since = nowSec;
            }
            const double wait = dir < 0 ? m_darkerSec : m_lighterSec;
            if (nowSec - m_since >= wait) {
                m_held = light;
                m_dir = 0;
            }
            return m_held;
        }

        [[nodiscard]] float Held() const noexcept { return m_held; }
        [[nodiscard]] bool Pending() const noexcept { return m_dir != 0; }

        /// Save load: the next reading is taken at once, as for a new place.
        void Reset() noexcept
        {
            m_started = false;
            m_dir = 0;
        }

    private:
        double m_darkerSec = kDarkerSec;
        double m_lighterSec = kLighterSec;
        bool m_started = false;
        std::uint32_t m_placeID = 0;
        float m_held = 1.0f;
        int m_dir = 0;          // -1 waiting on darker, +1 on lighter, 0 none
        double m_since = 0.0;
    };
}
