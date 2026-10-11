#pragma once

// Hysteresis for the coarse state buckets (the HP/MP/SP levels and the
// closest-enemy distance band) that feed the GameState hash.
//
// Why: the hash is what wakes the pipeline (CheckHashSkip) and what keys
// UsageMemory's recency boost, so a value sitting on a boundary re-scored the
// whole page every time it wobbled across. In one LoreRim fight (2026-10-11
// 08:55:40-08:58:00) 91 state transitions fired: 48 MP moves (most of them a
// cast draining through several levels, some up-and-back pairs as little as
// 0.11 s apart) and 37 distance moves, 28 of them a back-and-forth flip across
// one boundary, some 0.1-0.3 s apart.
//
// A bucket here moves only once the value has passed the boundary by a band.
// Each edge carries its own two thresholds, so a caller can make one direction
// immediate: the vitals do, for the falling side (see StateEvaluator).
//
// Pure: standard library only, host-tested in tests/core/BucketHysteresisTests.cpp.

#include <cstddef>
#include <span>

namespace Huginn::Core
{
    /// One boundary between bucket i and bucket i + 1.
    ///
    /// `bound` is the plain classification edge: a value <= bound is in bucket
    /// i or lower, a value > bound in bucket i + 1 or higher (the inclusive upper
    /// edge every bucket in GameState uses). With hysteresis the bucket only
    /// crosses UP once value > upAt, and only crosses DOWN once value <= downAt.
    /// Callers keep downAt <= bound <= upAt; downAt == bound makes the downward
    /// move immediate.
    struct BucketEdge
    {
        double bound = 0.0;
        double downAt = 0.0;
        double upAt = 0.0;
    };

    /// The bucket without hysteresis: the number of edges the value is above.
    /// Edges ascending. A NaN fails every `<=` and lands in the top bucket, as
    /// it did in the `if (x <= edge)` chains this replaces.
    [[nodiscard]] constexpr int RawBucket(double value, std::span<const BucketEdge> edges) noexcept
    {
        int bucket = 0;
        for (const auto& edge : edges) {
            if (value <= edge.bound) {
                return bucket;
            }
            ++bucket;
        }
        return bucket;
    }

    /// The bucket with hysteresis, given the bucket it was in last time.
    ///
    /// `previous` outside [0, edges.size()] means "no memory" (first reading, or
    /// a reset): the raw bucket is returned. Otherwise the bucket climbs past each
    /// edge the value is above `upAt` of, or, if it did not climb, falls past each
    /// edge the value is at or below `downAt` of -- so a long jump crosses several
    /// buckets in one reading, but stops at the first edge it has not cleared by
    /// the band. A NaN never satisfies either comparison and keeps `previous`.
    [[nodiscard]] constexpr int HystereticBucket(
        double value, int previous, std::span<const BucketEdge> edges) noexcept
    {
        const int top = static_cast<int>(edges.size());
        if (previous < 0 || previous > top) {
            return RawBucket(value, edges);
        }
        int bucket = previous;
        while (bucket < top && value > edges[static_cast<size_t>(bucket)].upAt) {
            ++bucket;
        }
        if (bucket == previous) {
            while (bucket > 0 && value <= edges[static_cast<size_t>(bucket - 1)].downAt) {
                --bucket;
            }
        }
        return bucket;
    }

    /// Where a vital stands against one override's thresholds: 0 below the
    /// activation threshold, 1 inside its release gap, 2 at or above
    /// threshold + gap. The pipeline wakes on any change of these zones
    /// (PipelineCoordinator::CheckHashSkip), so an override is evaluated where
    /// its own thresholds are crossed -- not only when a hashed bucket happens
    /// to move. The override thresholds (35% shipped) are not bucket edges, and
    /// once the combat lock stops lock expiries from forcing runs, nothing else
    /// would wake the pipeline for a fall from 39% to 30%.
    [[nodiscard]] constexpr int OverrideZone(double value, double threshold, double gap) noexcept
    {
        if (value < threshold) return 0;
        if (value < threshold + gap) return 1;
        return 2;
    }

    /// An edge whose band is the same absolute width both ways.
    [[nodiscard]] constexpr BucketEdge SymmetricEdge(double bound, double band) noexcept
    {
        return BucketEdge{ bound, bound - band, bound + band };
    }

    /// An edge that is crossed downward at the bound itself and upward only past
    /// bound + band: a fall registers at once, a recovery late.
    [[nodiscard]] constexpr BucketEdge RisingBandEdge(double bound, double band) noexcept
    {
        return BucketEdge{ bound, bound, bound + band };
    }

    /// An edge on a SQUARED distance, with a band that is a fraction of the
    /// boundary distance itself (0.1 = 10% of it, both ways): the band scales
    /// with the range, so the melee edge gets a narrower one than the far edge.
    /// Squared because the distance readings are kept squared (no sqrt per tick).
    [[nodiscard]] constexpr BucketEdge SquaredDistanceEdge(double boundDistance, double bandFraction) noexcept
    {
        const double down = boundDistance * (1.0 - bandFraction);
        const double up = boundDistance * (1.0 + bandFraction);
        return BucketEdge{ boundDistance * boundDistance, (down > 0.0 ? down * down : 0.0), up * up };
    }
}
