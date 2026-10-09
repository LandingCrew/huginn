#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/SlotClassCapMath.h"

// =============================================================================
// SLOT SCORE MATH (pure; host-tested in tests/core/SlotScoreMathTests.cpp)
// =============================================================================
// The slot code ranks on a SCORE that may have any sign (engine rewrite, map
// Phase 5): the new scorer hands it log-utilities -- log-odds under the choice
// model -- which are zero or negative for most items. Everything here is
// written so that a negative score means "worse", never "helped":
//
//   - a multiplicative discount d on a utility becomes an additive k * ln d on
//     the score (the class cap);
//   - a ratio test u_c > m * u_i becomes a difference test s_c - s_i > ln m
//     (the slot hold), written as s_c > s_i + ln m so -inf on both sides is
//     "not greater", never NaN;
//   - sentinels sit outside the finite range: -inf for a row that was never
//     ranked, +inf for an override (pinned by a rule, never compared).
//
// THE BRIDGE (R7, until the new scorer lands in R8): score = ln(utility). The
// old engine's utilities are >= 0 (fMinimumUtility is clamped to >= 0), so the
// bridge is defined on every row the slot code ever sees; ln 0 = -inf. With
// sigma = 0 and m = 1 + fChallengerMargin (1.5 shipped), the slot code on the
// bridged scores makes the decisions the old multiplicative code made on the
// utilities (tests/core/SlotAllocGoldenTests.cpp proves it on recorded and
// synthetic pipeline snapshots, and names the one place float rounding can
// split them).
//
// Scores are double. ln of two adjacent floats differs by at least ~6e-8,
// which a float score could not always hold apart (two utilities a ulp apart
// near e^2 can share one float log); a double keeps every distinct utility a
// distinct score, so the bridge preserves the ranking exactly.
// =============================================================================

namespace Huginn::Core
{
    using SlotScore = double;

    /// A row added only so a Remembrance hold can show it: never ranked. Below
    /// every finite score, so "the best-scoring stack" never picks it over a
    /// ranked one, whatever sign the ranked scores have.
    inline constexpr SlotScore kUnrankedScore = -std::numeric_limits<double>::infinity();

    /// What an override's slot assignment carries. Overrides are told apart by
    /// their assignment type and never compared on score; +inf only makes a
    /// stray comparison read "pinned above everything" rather than overflow
    /// the way the old 1000.0 utility would under exp().
    inline constexpr SlotScore kPinnedScore = std::numeric_limits<double>::infinity();

    /// Two scores closer than this are a tie. Exact real ties come out of the
    /// log arithmetic a few 1e-16 apart (ln(2u) + ln 0.5 vs ln u); the
    /// smallest real gap between two different float utilities is ~6e-8 in
    /// log space. Anything between the two works; this is far from both.
    inline constexpr double kScoreTieEpsilon = 1e-12;

    /// The slot hold's tie band: a challenger has to clear the margin by more than
    /// float noise -- 2.5e-7 in log space, two to four float ulps of a utility
    /// depending on where it sits in its binade -- or the holder holds. Measured
    /// (R7 review): a challenger 3 float steps above float(1.5 h) still held in
    /// about 48% of cases, 4 steps above in about 10%, 5 or more never. Why this
    /// size: with a non-power-of-two discount the old cap products carry noise of
    /// up to ~1.4e-7 in log space, and the band has to sit above it. It can drop
    /// to kScoreTieEpsilon at R8, when scores stop being float utilities and the
    /// tier preference is gone. Not a tuning knob: under the bridge the old engine's
    /// potion tier preference divides a family's next tier by exactly 1.5
    /// (POTION_TIER_STEP), the same 1.5 as the hold's margin, so adjacent tiers sit
    /// ON the margin and only the rounding of u / 1.5 * 1.5 decided the old
    /// comparison (a recorded play snapshot hit it: Potion of Plentiful Magicka at
    /// 0.207614 against Minor at 0.13840933). With the band both arithmetics hold
    /// there (the user's decision, 2026-10-08: exact tier ties always hold). It is
    /// far below any real difference a learned score will make.
    inline constexpr double kHoldTieEpsilon = 2.5e-7;

    /// THE BRIDGE: the old engine's utility as a score. ln(u) for u > 0; -inf
    /// for u <= 0 and for NaN (no ranked row has either: utilities are >= the
    /// INI's fMinimumUtility, which is clamped to >= 0).
    [[nodiscard]] inline SlotScore BridgeScore(float utility) noexcept
    {
        if (!(utility > 0.0f)) {
            return kUnrankedScore;
        }
        return std::log(static_cast<double>(utility));
    }

    /// The old engine's utility back from a score: exp(score), for display
    /// only (the debug widget's bar, the widget's unused confidence payload).
    /// Defined for any sign: 0 for -inf, finite for +inf (clamped to the
    /// largest float), 0 for NaN. Under the bridge it returns the utility.
    [[nodiscard]] inline float DisplayUtility(SlotScore score) noexcept
    {
        if (!(score == score)) {
            return 0.0f;   // NaN
        }
        const double u = std::exp(score);
        if (!(u < static_cast<double>(std::numeric_limits<float>::max()))) {
            return std::numeric_limits<float>::max();
        }
        return static_cast<float>(u);
    }

    /// a beats b: greater by more than the tie epsilon. -inf never beats -inf.
    [[nodiscard]] inline bool ScoreGreater(SlotScore a, SlotScore b) noexcept
    {
        return a > b + kScoreTieEpsilon;
    }

    /// ln m for the slot hold's margin m = 1 + margin. The float sum is the
    /// one the old code multiplied by, so the bridge keeps its rounding.
    /// Outside the INI's range (SlotSettings clamps it to [0, 10] and reads a
    /// NaN as the default): NaN gives +inf -- nothing beats the holder, as
    /// every comparison against u * NaN was false -- and m <= 0 gives -inf, so
    /// any ranked challenger wins, as u_c > u_i * m did for positive
    /// utilities (the old code also let a zero-utility challenger win against
    /// m < 0; under a log score there is no such case to keep).
    [[nodiscard]] inline double LogHoldMargin(float margin) noexcept
    {
        if (margin != margin) {
            return std::numeric_limits<double>::infinity();
        }
        const float m = 1.0f + margin;
        if (!(m > 0.0f)) {
            return -std::numeric_limits<double>::infinity();
        }
        return std::log(static_cast<double>(m));
    }

    /// The slot hold's test: the challenger beats the holder by the margin.
    /// The ratio u_c > u_i * m of the old code, as a difference.
    [[nodiscard]] inline bool ScoreExceedsByMargin(SlotScore challenger, SlotScore holder, double logMargin) noexcept
    {
        return challenger > holder + logMargin + kHoldTieEpsilon;
    }

    /// The class cap as an additive term on the score: 0 while the item's
    /// class has fewer than `freePerClass` items on the page (or the cap is
    /// off), then k * ln d for the k-th item past the free ones. The old
    /// multiplier ClassCapFactor (d^k) in log space: d = 0 gives -inf, as the
    /// old factor gave 0.
    [[nodiscard]] inline SlotScore ClassCapTerm(float discount, std::uint32_t freePerClass, std::uint32_t shown) noexcept
    {
        const float d = ClampClassCapDiscount(discount);
        if (!(d < 1.0f) || shown < freePerClass) {
            return 0.0;
        }
        const double steps = static_cast<double>(static_cast<std::uint64_t>(shown) - freePerClass + 1);
        if (d == 0.0f) {
            return -std::numeric_limits<double>::infinity();
        }
        return steps * std::log(static_cast<double>(d));
    }

    // -------------------------------------------------------------------------
    // Churn telemetry: how much better the challenger that took a slot scored
    // than the item it replaced (SoakMetrics' ChallengerRatio buckets). Was a
    // ratio of utilities with -1 as "the incumbent is gone"; now a difference
    // of scores, with "gone" as an empty optional, so no valid score can be
    // mistaken for it.
    // -------------------------------------------------------------------------
    enum class LogRatioBucket : std::uint8_t
    {
        Gone,       // the incumbent is no longer a candidate
        Below1,     // the challenger scored lower
        Below110,   // < 10% better
        Below125,   // 10-25%
        Below150,   // 25-50%
        Above150,   // 50%+, or the incumbent was unranked (-inf)
    };

    [[nodiscard]] inline LogRatioBucket BucketLogRatio(SlotScore challenger, std::optional<SlotScore> incumbent) noexcept
    {
        if (!incumbent) {
            return LogRatioBucket::Gone;
        }
        if (!(*incumbent > kUnrankedScore)) {
            return LogRatioBucket::Above150;   // the old "incumbent utility <= 0" case
        }
        const double delta = challenger - *incumbent;   // incumbent finite: never NaN unless challenger is
        // The old float bounds, so a ratio on the bridge lands in the same bucket.
        if (!(delta >= 0.0)) return LogRatioBucket::Below1;
        if (delta < std::log(static_cast<double>(1.10f))) return LogRatioBucket::Below110;
        if (delta < std::log(static_cast<double>(1.25f))) return LogRatioBucket::Below125;
        if (delta < std::log(static_cast<double>(1.50f))) return LogRatioBucket::Below150;
        return LogRatioBucket::Above150;
    }
}  // namespace Huginn::Core
