#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/SlotScoreMath.h"

// =============================================================================
// THE BAYESIAN CHALLENGER RULE (R9; pure, host-tested in
// tests/core/ChallengerRuleTests.cpp)
// =============================================================================
// Doc 9, "Keep the challenger margin; derive it from Bayesian confidence"; map
// Phase 7. The slot hold keeps a seated item on its key until a challenger
// beats it. Up to R8 "beats" was a fixed ratio, u_c > 1.5 u_i (a score gap of
// ln 1.5). From R9 every score is a belief with an error bar, and the
// challenger takes the key only when the posterior probability that it is the
// better item clears tau:
//
//     P(c > i) = Phi((mu_c - mu_i - ln m) / sigma_Delta) > tau
//     <=>  mu_c - mu_i > ln m + z sigma_Delta,      z = Phi^-1(tau)
//
// sigma_Delta is the spread of the GAP, not of either score:
//
//     sigma_Delta^2 = sum_k (x_c,k - x_i,k)^2 Var(theta_k) + Var(b_c) + Var(b_i)
//
// under the learner's diagonal posterior, so the weights the two items share
// cancel (two healing potions differ only in their strength and their own b).
// core/ChoiceBeliefs.h computes it from the ranking's tick; this file is the
// rule itself and knows nothing of the model.
//
// Settings ([SlotLocker], SlotSettings): fChallengerConfidence = tau (0.8, the
// doc's starting value; z = 0.8416) and fChallengerMinGap = ln m (0: confidence
// alone, doc 9 "m = 1"). The fixed margin fChallengerMargin stays for the rows
// that carry no belief: the FALLBACK ranking (no bootstrap: ln utility, no
// sigma) and the golden snapshots, which state their scores as utilities.
//
// NO SWAP-BACK ON FIXED BELIEFS. With ln m >= 0 and z >= 0 (both clamped so),
// the threshold is >= 0 and symmetric in the pair (sigma_Delta(c, i) =
// sigma_Delta(i, c)). If c took i's key, mu_c - mu_i > ln m + z sigma_Delta >= 0,
// so mu_i - mu_c < 0 <= ln m + z sigma_Delta and i cannot take it back while
// the beliefs stand. Nor can any cycle c1 -> c2 -> ... -> c1 close: summing
// the strict inequalities round the cycle gives 0 > 0. The lock still guards
// against flapping INPUTS (needs and beliefs that move); that is why it stays.
// =============================================================================

namespace Huginn::Core::Challenger
{
    /// tau: doc 9's starting value.
    inline constexpr double kDefaultConfidence = 0.8;
    /// tau's range. Below 0.5 the rule would let a challenger that is probably
    /// WORSE take the key (z < 0) and the no-swap-back argument fails, so 0.5
    /// is the floor (z = 0: any challenger ahead by ln m). 0.999 is z = 3.09.
    inline constexpr double kMinConfidence = 0.5;
    inline constexpr double kMaxConfidence = 0.999;
    /// ln m's range: 0 (confidence alone) to 5 (x148; effectively "never").
    inline constexpr double kMaxMinGap = 5.0;

    /// Phi(x), the standard normal CDF.
    [[nodiscard]] inline double NormalCdf(double x) noexcept
    {
        return 0.5 * std::erfc(-x / std::sqrt(2.0));
    }

    /// z = Phi^-1(tau), tau clamped to [kMinConfidence, kMaxConfidence]; a NaN
    /// or infinite tau reads as the default. Bisection on Phi (monotone, 80
    /// halvings of [0, 4]: far below double resolution); computed once per
    /// allocation, so the cost does not matter and the answer is exact to the
    /// last bits rather than to a rational approximation's 1e-9.
    [[nodiscard]] inline double ZForConfidence(double tau) noexcept
    {
        if (!std::isfinite(tau)) tau = kDefaultConfidence;
        if (tau < kMinConfidence) tau = kMinConfidence;
        if (tau > kMaxConfidence) tau = kMaxConfidence;
        double lo = 0.0, hi = 4.0;   // Phi(4) = 0.99997 > kMaxConfidence
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (NormalCdf(mid) < tau) lo = mid;
            else hi = mid;
        }
        return 0.5 * (lo + hi);
    }

    /// ln m clamped to [0, kMaxMinGap]; NaN or infinite reads as 0.
    [[nodiscard]] inline double ClampMinGap(double gap) noexcept
    {
        if (!std::isfinite(gap) || gap < 0.0) return 0.0;
        return gap > kMaxMinGap ? kMaxMinGap : gap;
    }

    /// sigma_Delta from its variance. A negative variance (rounding on a
    /// difference that is really 0) reads 0; NaN stays NaN (no belief).
    [[nodiscard]] inline double SigmaDelta(double deltaVar) noexcept
    {
        if (deltaVar != deltaVar) return deltaVar;
        return deltaVar > 0.0 ? std::sqrt(deltaVar) : 0.0;
    }

    /// The rule's threshold on mu_c - mu_i: ln m + z sigma_Delta.
    [[nodiscard]] inline double Threshold(double sigmaDelta, double minGap, double z) noexcept
    {
        return minGap + z * sigmaDelta;
    }

    /// The rule: the challenger takes the holder's key. Written as
    /// c > h + threshold + the hold's tie band (kHoldTieEpsilon, as the fixed
    /// margin's test), so -inf on both sides and any NaN read "no".
    [[nodiscard]] inline bool Exceeds(double challenger, double holder, double sigmaDelta, double minGap,
                                      double z) noexcept
    {
        const double t = Threshold(sigmaDelta, minGap, z);
        if (t != t) return false;
        return challenger > holder + t + kHoldTieEpsilon;
    }

    /// The scorer's beliefs as the slot code sees them: the variance of the
    /// score difference between two rows of one ranking run (rows are the
    /// run's own indices, CandidateRec::beliefRow). NaN = no belief for that
    /// pair (a row out of range); the hold then uses the fixed margin.
    class BeliefSource
    {
    public:
        [[nodiscard]] virtual double DeltaVar(std::uint32_t a, std::uint32_t b) const noexcept = 0;

    protected:
        BeliefSource() = default;
        BeliefSource(const BeliefSource&) = default;
        BeliefSource& operator=(const BeliefSource&) = default;
        ~BeliefSource() = default;
    };

    // -------------------------------------------------------------------------
    // Churn telemetry on Delta / sigma_Delta (R9: replaces the ratio buckets as
    // the measure of what a change was worth; the ratio buckets stay beside it
    // so a session compares against R8's heartbeat).
    // -------------------------------------------------------------------------
    enum class DeltaSdBucket : std::uint8_t
    {
        Gone,       // the incumbent is no longer a candidate
        Below0,     // the newcomer scores lower (the allocator's own moves)
        Below05,    // 0 <= Delta/sigma_Delta < 0.5
        Below1,     // 0.5-1 (the default line, z = 0.84, falls here)
        Below2,     // 1-2
        Below4,     // 2-4
        Above4,     // 4 or more; also a certain gap (sigma_Delta 0, Delta > 0) and an unranked incumbent
        NoSd,       // no belief for the pair (the fallback ranking, or rows of different runs)
        Count
    };

    [[nodiscard]] constexpr const char* DeltaSdBucketName(DeltaSdBucket b) noexcept
    {
        switch (b) {
        case DeltaSdBucket::Gone:    return "gone";
        case DeltaSdBucket::Below0:  return "<0";
        case DeltaSdBucket::Below05: return "<0.5";
        case DeltaSdBucket::Below1:  return "<1";
        case DeltaSdBucket::Below2:  return "<2";
        case DeltaSdBucket::Below4:  return "<4";
        case DeltaSdBucket::Above4:  return ">=4";
        case DeltaSdBucket::NoSd:    return "nosd";
        default:                     return "?";
        }
    }

    /// The bucket of one slot change: the newcomer's score against the item it
    /// replaced (empty: gone), and the pair's sigma_Delta (empty or NaN: no
    /// belief). An unranked incumbent (-inf) counts as 4 or more, as the ratio
    /// buckets count it 50%+.
    [[nodiscard]] inline DeltaSdBucket BucketDeltaSd(SlotScore challenger, std::optional<SlotScore> incumbent,
                                                     std::optional<double> sigmaDelta) noexcept
    {
        if (!incumbent) return DeltaSdBucket::Gone;
        if (!(*incumbent > kUnrankedScore)) return DeltaSdBucket::Above4;
        if (!sigmaDelta || *sigmaDelta != *sigmaDelta || *sigmaDelta < 0.0) return DeltaSdBucket::NoSd;
        const double delta = challenger - *incumbent;
        if (!(delta >= 0.0)) return DeltaSdBucket::Below0;   // NaN challenger lands here too
        if (*sigmaDelta == 0.0) return delta > 0.0 ? DeltaSdBucket::Above4 : DeltaSdBucket::Below05;
        const double t = delta / *sigmaDelta;
        if (t < 0.5) return DeltaSdBucket::Below05;
        if (t < 1.0) return DeltaSdBucket::Below1;
        if (t < 2.0) return DeltaSdBucket::Below2;
        if (t < 4.0) return DeltaSdBucket::Below4;
        return DeltaSdBucket::Above4;
    }

    /// A change the rule would NOT have let through: the newcomer is still a
    /// candidate's replacement (incumbent ranked) and its lead is at or under
    /// the line, mu_c - mu_i <= ln m + z sigma_Delta. Under R9 the hold passes
    /// none of these, so the ones the heartbeat counts came by another route
    /// (seating, a home-key return moving the gap-filler, the fill after the
    /// holder left its seat). False when there is no belief or no incumbent.
    [[nodiscard]] inline bool UnderLine(SlotScore challenger, std::optional<SlotScore> incumbent,
                                        std::optional<double> sigmaDelta, double minGap, double z) noexcept
    {
        if (!incumbent || !(*incumbent > kUnrankedScore)) return false;
        if (!sigmaDelta || *sigmaDelta != *sigmaDelta || *sigmaDelta < 0.0) return false;
        return !Exceeds(challenger, *incumbent, *sigmaDelta, minGap, z);
    }
}  // namespace Huginn::Core::Challenger
