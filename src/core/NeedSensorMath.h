#pragma once

// =============================================================================
// NEED SENSOR MATH -- the arithmetic behind the R3 sensors
// =============================================================================
// The game side reads the forms; everything it then computes lives here so the
// host tests can pin it (src/core/README.md):
//
//   DecayingSum     the per-element damage sum, decay tau 3 s (needs.csv
//                   fire_damage_rate .. physical_damage_rate)
//   SoleHostileTtk  boss_fight's time-to-kill from the only hostile's health
//                   bar over the time it has been the only one
//   SecondsSince    a steady-clock timer read as seconds, NEVER when unset
//   EncumbranceRatio
//
// Times are seconds on any monotonic clock (the caller passes steady_clock
// seconds); nothing here reads a clock.
//
// Pure: standard library only.
// =============================================================================

#include "NeedSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Huginn::Core::Needs
{
    /// sum(amount_i * exp(-(t - t_i) / tau)), kept as one value and the time
    /// it was last brought up to date. Add() and At() take a time that never
    /// goes backwards; an earlier time is treated as "now" (no growth).
    class DecayingSum
    {
    public:
        explicit constexpr DecayingSum(float tauSec = kDamageDecayTauSec) noexcept :
            m_tau(tauSec > 0.0f ? tauSec : kDamageDecayTauSec) {}

        void Add(double tSec, float amount) noexcept
        {
            m_value = At(tSec) + std::max(amount, 0.0f);
            m_t = std::max(m_t, tSec);
            m_started = true;
        }

        /// The sum as of tSec (no state change).
        [[nodiscard]] float At(double tSec) const noexcept
        {
            if (!m_started || m_value <= 0.0f) return 0.0f;
            const double dt = std::max(0.0, tSec - m_t);
            return static_cast<float>(m_value * std::exp(-dt / m_tau));
        }

        void Reset() noexcept
        {
            m_value = 0.0f;
            m_t = 0.0;
            m_started = false;
        }

    private:
        float m_tau;
        float m_value = 0.0f;
        double m_t = 0.0;
        bool m_started = false;
    };

    /// boss_fight's input (needs.csv: "estimated time-to-kill = target health
    /// fraction / bar fall rate, seconds, with exactly one hostile"). Fed each
    /// targets poll with the only living hostile (id 0 when there is not
    /// exactly one); the estimate needs kMinWindowSec of watching the same
    /// actor, then reads health / ((h0 - h) / (t - t0)). A bar that has not
    /// moved reads kNever (a fight going nowhere); -1 = no estimate.
    class SoleHostileTtk
    {
    public:
        static constexpr double kMinWindowSec = 5.0;
        static constexpr float kHealRestart = 0.05f;

        [[nodiscard]] float Update(std::uint32_t soleId, float healthFraction, double tSec) noexcept
        {
            if (soleId == 0 || !(healthFraction >= 0.0f)) {
                m_id = 0;
                return -1.0f;
            }
            if (soleId != m_id || tSec < m_t0 || healthFraction > m_h0 + kHealRestart) {
                // A new opponent, or the bar went clearly UP (it healed): start
                // again. A sliver of regeneration only reads as no progress.
                m_id = soleId;
                m_t0 = tSec;
                m_h0 = healthFraction;
                return -1.0f;
            }
            const double window = tSec - m_t0;
            if (window < kMinWindowSec) return -1.0f;
            const double fallen = static_cast<double>(m_h0) - healthFraction;
            if (fallen <= 0.0) return kNever;
            const double ttk = static_cast<double>(healthFraction) * window / fallen;
            return static_cast<float>(std::min(ttk, static_cast<double>(kNever)));
        }

        void Reset() noexcept { m_id = 0; }

    private:
        std::uint32_t m_id = 0;
        double m_t0 = 0.0;
        float m_h0 = 1.0f;
    };

    /// Seconds from `then` to `now`, or kNever when `then` is unset (< 0).
    [[nodiscard]] inline float SecondsSince(double thenSec, double nowSec) noexcept
    {
        if (thenSec < 0.0) return kNever;
        return static_cast<float>(std::clamp(nowSec - thenSec, 0.0, static_cast<double>(kNever)));
    }

    /// inventory weight / carry weight; 0 when the carry weight is not
    /// positive (no reading), so a broken read never reads "at the cap".
    [[nodiscard]] inline float EncumbranceRatio(float inventoryWeight, float carryWeight) noexcept
    {
        if (!(carryWeight > 0.0f) || !(inventoryWeight >= 0.0f)) return 0.0f;
        return inventoryWeight / carryWeight;
    }
}
