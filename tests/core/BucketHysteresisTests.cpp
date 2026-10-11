// Host tests for src/core/BucketHysteresis.h.

#include "core/BucketHysteresis.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <vector>

using namespace Huginn::Core;

namespace
{
    // The vital buckets in percent, as StateEvaluator builds them: Critical
    // <= 10 < VeryLow <= 25 < Low <= 40 < Medium <= 60 < High <= 80 < VeryHigh.
    std::array<BucketEdge, 5> VitalEdges(double band)
    {
        return { RisingBandEdge(10.0, band), RisingBandEdge(25.0, band), RisingBandEdge(40.0, band),
                 RisingBandEdge(60.0, band), RisingBandEdge(80.0, band) };
    }

    // The distance bands, squared: Melee <= 256 < Mid <= 768 < Ranged.
    std::array<BucketEdge, 2> DistanceEdges(double fraction)
    {
        return { SquaredDistanceEdge(256.0, fraction), SquaredDistanceEdge(768.0, fraction) };
    }

    // Feed a sequence through the hysteresis from no memory; count bucket changes.
    template <size_t N>
    int CountChanges(const std::vector<double>& values, const std::array<BucketEdge, N>& edges)
    {
        int bucket = -1;
        int changes = 0;
        for (double v : values) {
            const int next = HystereticBucket(v, bucket, edges);
            if (bucket >= 0 && next != bucket) ++changes;
            bucket = next;
        }
        return changes;
    }

    double Sq(double d) { return d * d; }
}

TEST_CASE("bucket hysteresis: raw bucket matches the inclusive-upper-edge chain")
{
    const auto edges = VitalEdges(3.0);
    CHECK(RawBucket(0.0, edges) == 0);
    CHECK(RawBucket(10.0, edges) == 0);     // <= 10 is Critical
    CHECK(RawBucket(10.01, edges) == 1);
    CHECK(RawBucket(25.0, edges) == 1);
    CHECK(RawBucket(40.0, edges) == 2);
    CHECK(RawBucket(60.0, edges) == 3);
    CHECK(RawBucket(80.0, edges) == 4);
    CHECK(RawBucket(80.5, edges) == 5);
    CHECK(RawBucket(100.0, edges) == 5);
    CHECK(RawBucket(std::numeric_limits<double>::quiet_NaN(), edges) == 5);
}

TEST_CASE("bucket hysteresis: no memory returns the raw bucket")
{
    const auto edges = VitalEdges(3.0);
    CHECK(HystereticBucket(81.0, -1, edges) == 5);
    CHECK(HystereticBucket(79.0, -1, edges) == 4);
    CHECK(HystereticBucket(79.0, 6, edges) == 4);    // out of range = no memory
    CHECK(HystereticBucket(5.0, -7, edges) == 0);
}

TEST_CASE("bucket hysteresis: band 0 is the raw bucket from any memory")
{
    const auto edges = VitalEdges(0.0);
    for (int i = 0; i <= 10000; ++i) {
        const double v = i / 100.0;
        for (int prev = 0; prev <= 5; ++prev) {
            REQUIRE(HystereticBucket(v, prev, edges) == RawBucket(v, edges));
        }
    }
}

TEST_CASE("bucket hysteresis: a rising vital crosses up only past bound + band")
{
    const auto edges = VitalEdges(3.0);
    CHECK(HystereticBucket(80.5, 4, edges) == 4);
    CHECK(HystereticBucket(83.0, 4, edges) == 4);    // exactly bound + band is not past it
    CHECK(HystereticBucket(83.01, 4, edges) == 5);
    CHECK(HystereticBucket(12.0, 0, edges) == 0);    // Critical holds until 13
    CHECK(HystereticBucket(13.5, 0, edges) == 1);
}

TEST_CASE("bucket hysteresis: a falling vital crosses down at the bound itself")
{
    // A fall from above a boundary changes the bucket exactly where it did
    // without hysteresis (a re-fall inside the band is no change at all: the
    // bucket never went up).
    const auto edges = VitalEdges(3.0);
    CHECK(HystereticBucket(80.0, 5, edges) == 4);
    CHECK(HystereticBucket(80.01, 5, edges) == 5);
    CHECK(HystereticBucket(10.0, 1, edges) == 0);    // Critical at once
    CHECK(HystereticBucket(10.01, 1, edges) == 1);
}

TEST_CASE("bucket hysteresis: every falling crossing matches the raw bucket exactly")
{
    const auto edges = VitalEdges(3.0);
    // Whenever the raw bucket is below the remembered one, the hysteretic
    // bucket IS the raw bucket: a fall is never delayed, at any value.
    for (int i = 10000; i >= 0; --i) {
        const double v = i / 100.0;
        const int raw = RawBucket(v, edges);
        for (int prev = raw + 1; prev <= 5; ++prev) {
            REQUIRE(HystereticBucket(v, prev, edges) == raw);
        }
    }
}

TEST_CASE("bucket hysteresis: never moves against the value, never further than raw")
{
    const auto edges = VitalEdges(3.0);
    for (int i = 0; i <= 10000; ++i) {
        const double v = i / 100.0;
        const int raw = RawBucket(v, edges);
        for (int prev = 0; prev <= 5; ++prev) {
            const int next = HystereticBucket(v, prev, edges);
            // Between the remembered bucket and the raw one, inclusive.
            const int lo = prev < raw ? prev : raw;
            const int hi = prev < raw ? raw : prev;
            REQUIRE(next >= lo);
            REQUIRE(next <= hi);
        }
    }
}

TEST_CASE("bucket hysteresis: a long jump crosses several buckets but stops at an uncleared edge")
{
    const auto edges = VitalEdges(3.0);
    CHECK(HystereticBucket(100.0, 0, edges) == 5);
    CHECK(HystereticBucket(26.0, 0, edges) == 1);    // past 13, not past 28
    CHECK(HystereticBucket(42.0, 1, edges) == 2);    // past 28, not past 43
    CHECK(HystereticBucket(5.0, 5, edges) == 0);     // a fall goes all the way
}

TEST_CASE("bucket hysteresis: a NaN keeps the remembered bucket")
{
    const auto edges = VitalEdges(3.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (int prev = 0; prev <= 5; ++prev) {
        CHECK(HystereticBucket(nan, prev, edges) == prev);
    }
}

TEST_CASE("bucket hysteresis: magicka wobbling across 80% by less than the band stops flapping")
{
    // Cast-regen sawtooth around the High/VeryHigh edge, as in the 2026-10-11
    // fight (VeryHigh<->High 0.4 s apart): 79.5 -> 82.5 -> 79.5 ...
    std::vector<double> mp;
    for (int i = 0; i < 20; ++i) {
        mp.push_back(i % 2 ? 82.5 : 79.5);
    }
    CHECK(CountChanges(mp, VitalEdges(0.0)) == 19);
    // 79.5 is High; 82.5 never clears 83, so the bucket stays High.
    CHECK(CountChanges(mp, VitalEdges(3.0)) == 0);
    // A swing that does clear the band still registers both ways.
    std::vector<double> big{ 79.0, 84.0, 79.0, 84.0 };
    CHECK(CountChanges(big, VitalEdges(3.0)) == 3);
}

TEST_CASE("bucket hysteresis: distance band is symmetric and scales with the boundary")
{
    const auto edges = DistanceEdges(0.1);
    // Melee edge 256: Mid -> Melee at <= 230.4, Melee -> Mid at > 281.6.
    CHECK(HystereticBucket(Sq(240.0), 1, edges) == 1);
    CHECK(HystereticBucket(Sq(230.0), 1, edges) == 0);
    CHECK(HystereticBucket(Sq(270.0), 0, edges) == 0);
    CHECK(HystereticBucket(Sq(282.0), 0, edges) == 1);
    // Mid edge 768: Ranged -> Mid at <= 691.2, Mid -> Ranged at > 844.8.
    CHECK(HystereticBucket(Sq(700.0), 2, edges) == 2);
    CHECK(HystereticBucket(Sq(690.0), 2, edges) == 1);
    CHECK(HystereticBucket(Sq(840.0), 1, edges) == 1);
    CHECK(HystereticBucket(Sq(850.0), 1, edges) == 2);
    // Raw classification is unchanged by the band.
    CHECK(RawBucket(Sq(256.0), edges) == 0);
    CHECK(RawBucket(Sq(256.5), edges) == 1);
    CHECK(RawBucket(Sq(768.0), edges) == 1);
    CHECK(RawBucket(Sq(769.0), edges) == 2);
}

TEST_CASE("bucket hysteresis: an enemy pacing at the melee edge stops flapping")
{
    std::vector<double> d;
    for (int i = 0; i < 30; ++i) {
        d.push_back(Sq(i % 2 ? 268.0 : 248.0));
    }
    CHECK(CountChanges(d, DistanceEdges(0.0)) == 29);
    CHECK(CountChanges(d, DistanceEdges(0.1)) == 0);
}

TEST_CASE("bucket hysteresis: edge helpers")
{
    const auto s = SymmetricEdge(40.0, 3.0);
    CHECK(s.bound == 40.0);
    CHECK(s.downAt == 37.0);
    CHECK(s.upAt == 43.0);
    const auto r = RisingBandEdge(40.0, 3.0);
    CHECK(r.downAt == 40.0);
    CHECK(r.upAt == 43.0);
    const auto d = SquaredDistanceEdge(100.0, 2.0);   // band past the origin clamps to 0
    CHECK(d.downAt == 0.0);
    CHECK(d.upAt == doctest::Approx(90000.0));
}

TEST_CASE("bucket hysteresis: override zones change exactly at the override's own thresholds")
{
    // Shipped critical health: activate below 0.35, release at 0.50.
    CHECK(OverrideZone(0.39, 0.35, 0.15) == 1);
    CHECK(OverrideZone(0.30, 0.35, 0.15) == 0);   // 39% -> 30%: a zone change, a run
    CHECK(OverrideZone(0.35, 0.35, 0.15) == 1);
    CHECK(OverrideZone(0.4999, 0.35, 0.15) == 1);
    CHECK(OverrideZone(0.50, 0.35, 0.15) == 2);
    // Both 39% and 30% are the Low bucket (25 < x <= 40): no bucket change,
    // which is why the zone is needed at all.
    const std::array<BucketEdge, 5> edges{ RisingBandEdge(10.0, 3.0), RisingBandEdge(25.0, 3.0),
        RisingBandEdge(40.0, 3.0), RisingBandEdge(60.0, 3.0), RisingBandEdge(80.0, 3.0) };
    CHECK(HystereticBucket(39.0, 2, edges) == HystereticBucket(30.0, 2, edges));
}
