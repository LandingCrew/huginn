#pragma once

// =============================================================================
// THETA DRIFT (R11) -- how far the live weights moved from the bootstrap
// =============================================================================
// Map Phase 9's telemetry: the [Soak] heartbeat's θ-drift fields and `hg theta`.
// A moved count alone (what the heartbeat had since R8.3: `theta=moved/entries`)
// cannot tell a learner that settles from one that runs away: every answered
// need onset grows the variance of all 2 x J of its need's weights (the
// opportunity counter), so "moved" climbs towards the full set in long play
// whether or not any mean changed. These numbers separate the two:
//
//   meanMoved     weights whose MEAN differs from the bootstrap's
//   meanAbsDelta  mean |mu - mu_boot| over them (0 when none)
//   maxAbsDelta   the largest |mu - mu_boot|, and its slot (the console names it)
//   rmsDelta      sqrt(mean (mu - mu_boot)^2) over the mean-moved weights
//   meanLnVar     mean ln(var / var_boot) over every weight whose variance
//                 differs: < 0 the learner is surer than the file, > 0 the
//                 opportunity counter has widened more than picks narrowed
//   varMoved      weights whose variance differs
//
// And for b (the per-item bias, prior N(0, 0.25)): its entry count, mean |mu|
// and the largest |mu|.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/ChoiceScore.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Huginn::Core::Choice
{
    struct Drift
    {
        std::size_t weights = 0;        // compared slots
        std::size_t meanMoved = 0;
        std::size_t varMoved = 0;
        double meanAbsDelta = 0.0;
        double rmsDelta = 0.0;
        double maxAbsDelta = 0.0;
        double maxDelta = 0.0;          // signed, at maxSlot
        std::size_t maxSlot = std::numeric_limits<std::size_t>::max();
        double meanLnVar = 0.0;
    };

    /// Slot by slot over the shorter of the two spans (the same layout: the
    /// live model and the bootstrap it was copied from). A variance that is
    /// not finite or not positive on either side is left out of meanLnVar.
    [[nodiscard]] inline Drift ComputeDrift(std::span<const Weight> live, std::span<const Weight> boot) noexcept
    {
        Drift d;
        const std::size_t n = live.size() < boot.size() ? live.size() : boot.size();
        d.weights = n;
        double sumAbs = 0.0, sumSq = 0.0, sumLnVar = 0.0;
        std::size_t lnVarCount = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double dm = live[i].mu - boot[i].mu;
            if (dm != 0.0) {
                ++d.meanMoved;
                sumAbs += std::fabs(dm);
                sumSq += dm * dm;
                if (std::fabs(dm) > d.maxAbsDelta) {
                    d.maxAbsDelta = std::fabs(dm);
                    d.maxDelta = dm;
                    d.maxSlot = i;
                }
            }
            if (live[i].var != boot[i].var) {
                ++d.varMoved;
                if (live[i].var > 0.0 && boot[i].var > 0.0 && std::isfinite(live[i].var) && std::isfinite(boot[i].var)) {
                    sumLnVar += std::log(live[i].var / boot[i].var);
                    ++lnVarCount;
                }
            }
        }
        if (d.meanMoved > 0) {
            d.meanAbsDelta = sumAbs / static_cast<double>(d.meanMoved);
            d.rmsDelta = std::sqrt(sumSq / static_cast<double>(d.meanMoved));
        }
        if (lnVarCount > 0) d.meanLnVar = sumLnVar / static_cast<double>(lnVarCount);
        return d;
    }

    struct BDrift
    {
        std::size_t entries = 0;
        double meanAbsMu = 0.0;
        double maxAbsMu = 0.0;
        std::uint32_t maxForm = 0;
    };

    /// b's spread: entries as (form, mu) pairs.
    template <class Range>
    [[nodiscard]] BDrift ComputeBDrift(const Range& entries) noexcept
    {
        BDrift d;
        double sum = 0.0;
        for (const auto& [form, mu] : entries) {
            ++d.entries;
            const double a = std::fabs(mu);
            sum += a;
            if (a > d.maxAbsMu) {
                d.maxAbsMu = a;
                d.maxForm = form;
            }
        }
        if (d.entries > 0) d.meanAbsMu = sum / static_cast<double>(d.entries);
        return d;
    }
}
