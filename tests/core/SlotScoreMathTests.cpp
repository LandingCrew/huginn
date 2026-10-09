// Host tests for src/core/SlotScoreMath.h -- the sign-safe scalars the slot
// code ranks with (R7): the bridge, the additive class cap, the hold's margin
// as a difference, the sentinels, the churn buckets.

#include "core/SlotClassCapMath.h"
#include "core/SlotScoreMath.h"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>

using namespace Huginn::Core;

namespace
{
    constexpr double kInf = std::numeric_limits<double>::infinity();

    // SoakMetrics' BucketChallengerRatio as it was (0.23.9), on utilities.
    LogRatioBucket OldBucket(float challenger, float incumbent)
    {
        if (incumbent < 0.0f) return LogRatioBucket::Gone;
        if (incumbent == 0.0f) return LogRatioBucket::Above150;
        const float r = challenger / incumbent;
        if (r < 1.0f) return LogRatioBucket::Below1;
        if (r < 1.10f) return LogRatioBucket::Below110;
        if (r < 1.25f) return LogRatioBucket::Below125;
        if (r < 1.50f) return LogRatioBucket::Below150;
        return LogRatioBucket::Above150;
    }
}

TEST_CASE("slot score: the bridge is ln(utility), -inf for nothing")
{
    CHECK(BridgeScore(1.0f) == 0.0);
    CHECK(BridgeScore(0.5f) == std::log(0.5));
    CHECK(BridgeScore(0.0f) == -kInf);
    CHECK(BridgeScore(-0.0f) == -kInf);
    CHECK(BridgeScore(-1.0f) == -kInf);
    CHECK(BridgeScore(std::numeric_limits<float>::quiet_NaN()) == -kInf);
    CHECK(BridgeScore(1e-40f) > -kInf);   // a denormal utility is still a (very low) score
}

TEST_CASE("slot score: the bridge keeps every distinct utility distinct and in order")
{
    // A float log would merge neighbours (two utilities a ulp apart near e^2
    // share one float ln); the double score must not, or the bridge would
    // create ties the utilities did not have.
    std::mt19937 rng(7);
    std::uniform_int_distribution<std::uint32_t> bits(0x00000001u, 0x7F7FFFFFu);   // every positive finite float
    for (int i = 0; i < 200000; ++i) {
        const float u = std::bit_cast<float>(bits(rng));
        const float next = std::nextafter(u, std::numeric_limits<float>::infinity());
        REQUIRE(BridgeScore(u) < BridgeScore(next));
    }
}

TEST_CASE("slot score: display utility undoes the bridge exactly")
{
    std::mt19937 rng(11);
    std::uniform_int_distribution<std::uint32_t> bits(0x00800000u, 0x7F7FFFFFu);   // positive normal floats
    for (int i = 0; i < 200000; ++i) {
        const float u = std::bit_cast<float>(bits(rng));
        REQUIRE(DisplayUtility(BridgeScore(u)) == u);
    }
    CHECK(DisplayUtility(-kInf) == 0.0f);
    CHECK(DisplayUtility(kInf) == std::numeric_limits<float>::max());
    CHECK(DisplayUtility(std::numeric_limits<double>::quiet_NaN()) == 0.0f);
    CHECK(DisplayUtility(-3.0) > 0.0f);   // a negative score is a small positive display value
}

TEST_CASE("slot score: the class cap term is ln of the old factor")
{
    for (const float d : { 0.5f, 0.25f, 0.75f, 0.9f, 0.3f }) {
        for (std::uint32_t free = 0; free <= 4; ++free) {
            for (std::uint32_t shown = 0; shown <= 12; ++shown) {
                const double term = ClassCapTerm(d, free, shown);
                const double old = std::log(static_cast<double>(ClassCapFactor(d, free, shown)));
                // The old factor is a float product; the term is exact.
                CHECK(term == doctest::Approx(old).epsilon(1e-6));
                CHECK((term == 0.0) == (ClassCapFactor(d, free, shown) == 1.0f));
            }
        }
    }
    CHECK(ClassCapTerm(0.5f, 3, 2) == 0.0);                       // within the free allowance
    CHECK(ClassCapTerm(0.5f, 3, 3) == std::log(0.5));             // the 4th item
    CHECK(ClassCapTerm(0.5f, 3, 4) == 2.0 * std::log(0.5));       // the 5th
    CHECK(ClassCapTerm(1.0f, 0, 9) == 0.0);                       // off
    CHECK(ClassCapTerm(std::numeric_limits<float>::quiet_NaN(), 0, 9) == 0.0);
    CHECK(ClassCapTerm(0.0f, 3, 3) == -kInf);                     // d = 0: the old factor 0
    CHECK(ClassCapTerm(-0.0f, 3, 4) == -kInf);
}

TEST_CASE("slot score: a cap LOWERS a negative score (the old multiplier raised it)")
{
    // The reason for R7. Under log-odds most scores are negative: x0.5 on -2
    // gave -1, a promotion. k ln d always pushes down.
    const double s = -2.0;
    CHECK(s + ClassCapTerm(0.5f, 3, 3) < s);
    CHECK(s * ClassCapFactor(0.5f, 3, 3) > s);   // what the old arithmetic would have done
}

TEST_CASE("slot score: the hold's margin as a difference")
{
    const double lnM = LogHoldMargin(0.5f);
    CHECK(lnM == std::log(1.5));
    // Bridge: exactly the ratio test where the ratio is exact.
    CHECK_FALSE(ScoreExceedsByMargin(BridgeScore(1.5f), BridgeScore(1.0f), lnM));   // 1.5 > 1.0 * 1.5 is false
    CHECK(ScoreExceedsByMargin(BridgeScore(1.5001f), BridgeScore(1.0f), lnM));
    // Inside the tie band (kHoldTieEpsilon): one ulp over the margin holds.
    CHECK_FALSE(ScoreExceedsByMargin(BridgeScore(1.5000002f), BridgeScore(1.0f), lnM));
    // The recorded potion tiers (tier step 1.5 = the margin): the old code held.
    CHECK_FALSE(ScoreExceedsByMargin(BridgeScore(0.207614f), BridgeScore(0.13840933f), lnM));
    CHECK_FALSE(0.207614f > 0.13840933f * (1.0f + 0.5f));
    CHECK_FALSE(ScoreExceedsByMargin(BridgeScore(3.0f), BridgeScore(2.0f), lnM));   // a tie the logs compute ~1e-16 apart
    // Negative scores: the difference is what counts, not the ratio.
    CHECK(ScoreExceedsByMargin(-1.5, -2.0, lnM));          // 0.5 > ln 1.5
    CHECK_FALSE(ScoreExceedsByMargin(-1.7, -2.0, lnM));    // 0.3 < ln 1.5
    CHECK_FALSE(ScoreExceedsByMargin(-2.9, -2.0, lnM));    // worse; the old ratio test (-2.9 > -3.0) would swap
    // Unranked on both sides is "not greater", never NaN.
    CHECK_FALSE(ScoreExceedsByMargin(-kInf, -kInf, lnM));
    CHECK(ScoreExceedsByMargin(-50.0, -kInf, lnM));
    CHECK(LogHoldMargin(0.0f) == 0.0);
}

TEST_CASE("slot score: ties within the epsilon are ties")
{
    CHECK_FALSE(ScoreGreater(std::log(3.0) + std::log(0.5), std::log(1.5)));   // u = 3 capped x0.5 vs u = 1.5
    CHECK_FALSE(ScoreGreater(std::log(1.5), std::log(3.0) + std::log(0.5)));
    CHECK(ScoreGreater(1e-9, 0.0));
    CHECK_FALSE(ScoreGreater(-kInf, -kInf));
    CHECK(ScoreGreater(-1e300, -kInf));
}

TEST_CASE("slot score: sentinels sit outside every finite score")
{
    CHECK(kUnrankedScore < -std::numeric_limits<double>::max());
    CHECK(kPinnedScore > std::numeric_limits<double>::max());
    CHECK(DisplayUtility(kPinnedScore) == std::numeric_limits<float>::max());   // no overflow to inf
}

TEST_CASE("slot score: churn buckets on scores match the old ratio buckets on utilities")
{
    // "gone" is an empty optional now, never a value a score can take.
    CHECK(BucketLogRatio(0.0, std::nullopt) == LogRatioBucket::Gone);
    CHECK(BucketLogRatio(-5.0, std::nullopt) == LogRatioBucket::Gone);
    CHECK(BucketLogRatio(-1.0, kUnrankedScore) == LogRatioBucket::Above150);   // old: incumbent utility 0
    CHECK(BucketLogRatio(-kInf, 0.0) == LogRatioBucket::Below1);               // old: challenger utility 0

    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(0.01f, 5.0f);
    std::uniform_real_distribution<float> r(0.5f, 2.0f);
    int boundary = 0;
    for (int i = 0; i < 200000; ++i) {
        const float inc = u(rng);
        const float ch = inc * r(rng);
        const auto oldB = OldBucket(ch, inc);
        const auto newB = BucketLogRatio(BridgeScore(ch), BridgeScore(inc));
        if (oldB != newB) {
            // Only where the old float ratio rounded across a bound.
            const float ratio = ch / inc;
            bool near = false;
            for (const float bound : { 1.0f, 1.10f, 1.25f, 1.50f }) {
                near = near || std::fabs(ratio - bound) <= 2.0f * (std::nextafter(bound, 2.0f) - bound);
            }
            CHECK_MESSAGE(near, "ch=", ch, " inc=", inc);
            ++boundary;
        }
    }
    MESSAGE("bucket boundary disagreements (float ratio rounding): ", boundary, " of 200000");
    // Exact ratios land where they did (the old static_asserts in
    // SoakMetrics.cpp): edges are half-open, exactly 10% better is NOT "<1.1".
    CHECK(BucketLogRatio(BridgeScore(0.9f), BridgeScore(1.0f)) == LogRatioBucket::Below1);
    CHECK(BucketLogRatio(BridgeScore(1.2f), BridgeScore(1.0f)) == LogRatioBucket::Below125);
    CHECK(BucketLogRatio(BridgeScore(1.25f), BridgeScore(1.0f)) == LogRatioBucket::Below150);
    CHECK(BucketLogRatio(BridgeScore(1.10f), BridgeScore(1.0f)) == LogRatioBucket::Below125);
    CHECK(BucketLogRatio(BridgeScore(1.0f), BridgeScore(1.0f)) == LogRatioBucket::Below110);
    CHECK(BucketLogRatio(BridgeScore(1.5f), BridgeScore(1.0f)) == LogRatioBucket::Above150);
    CHECK(BucketLogRatio(BridgeScore(0.5f), BridgeScore(1.0f)) == LogRatioBucket::Below1);
    // Negative scores bucket on the difference.
    CHECK(BucketLogRatio(-1.0, -1.0) == LogRatioBucket::Below110);
    CHECK(BucketLogRatio(-1.0, -2.0) == LogRatioBucket::Above150);   // e^1 better
    CHECK(BucketLogRatio(-2.0, -1.0) == LogRatioBucket::Below1);
}
