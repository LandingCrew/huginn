// Host tests for src/core/InstanceDetail.h.

#include "core/InstanceDetail.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>

using namespace Huginn::Core;
using ID = InstanceDetail;

TEST_CASE("instance detail: a new game opens the gate at the primed reconcile, once")
{
    const std::uint32_t load = 1;   // the first kNewGame
    // RebuildRegistry inside the stabilization window: degraded.
    std::uint32_t stamp = ID::AfterRebuild(load, false);
    CHECK(stamp == ID::kNone);
    CHECK_FALSE(ID::Known(stamp, load));

    // The primed reconcile a second later reads the extra lists.
    const std::uint32_t before = stamp;
    stamp = ID::AfterReconcile(stamp, load, true);
    CHECK(ID::Known(stamp, load));
    CHECK(ID::Arrived(before, stamp, load));

    // The 30 s reconcile after that: still known, not a new arrival.
    const std::uint32_t again = ID::AfterReconcile(stamp, load, true);
    CHECK(ID::Known(again, load));
    CHECK_FALSE(ID::Arrived(stamp, again, load));
}

TEST_CASE("instance detail: a save load closes the gate until a detailed scan of THAT load")
{
    std::uint32_t stamp = 3;                 // detail known for load 3
    CHECK(ID::Known(stamp, 3));
    const std::uint32_t load = 4;            // the player loads a save
    CHECK_FALSE(ID::Known(stamp, load));     // closed at once, before any scan

    // The load-time reconcile runs inside the window: degraded, adds only,
    // leaves the stamp -- which still names load 3, so the gate stays shut.
    stamp = ID::AfterReconcile(stamp, load, false);
    CHECK_FALSE(ID::Known(stamp, load));

    // The primed reconcile.
    const std::uint32_t before = stamp;
    stamp = ID::AfterReconcile(stamp, load, true);
    CHECK(ID::Known(stamp, load));
    CHECK(ID::Arrived(before, stamp, load));
}

TEST_CASE("instance detail: a degraded rebuild clears the stamp, a detailed one sets it")
{
    CHECK(ID::AfterRebuild(7, false) == ID::kNone);
    CHECK(ID::AfterRebuild(7, true) == 7u);   // `hg rebuild` long after the load
    CHECK_FALSE(ID::Known(ID::kNone, ID::kNone));   // kNone never counts, even against itself
}

TEST_CASE("instance detail: a load landing mid-scan never counts the scan as detail for it")
{
    // The load writes the clock stamp (W1) and then bumps the generation
    // (W2, release). The scan reads the generation (R1, acquire) and then the
    // clock (R2: stable = the stamp it sees is the OLD one, long ago). Every
    // interleaving that respects W1 < W2 and R1 < R2; with release/acquire,
    // R1 seeing W2 implies R2 sees W1, which is what the interleavings below
    // encode (R2 sees the new stamp exactly when W1 came before it).
    const std::uint32_t oldLoad = 5, newLoad = 6;
    const std::uint32_t priorStamp = oldLoad;   // detail was known for the old load

    // Positions 0..3 of the four steps; enumerate the 6 orders.
    const std::array<std::array<int, 4>, 6> orders = { {
        // W1 W2 R1 R2 (positions)
        { 0, 1, 2, 3 },   // load, then scan
        { 0, 2, 1, 3 },   // W1, R1, W2, R2
        { 0, 3, 1, 2 },   // W1, R1, R2, W2
        { 1, 2, 0, 3 },   // R1, W1, W2, R2
        { 1, 3, 0, 2 },   // R1, W1, R2, W2
        { 2, 3, 0, 1 },   // scan, then load
    } };
    for (const auto& o : orders) {
        const int w1 = o[0], w2 = o[1], r1 = o[2], r2 = o[3];
        REQUIRE(w1 < w2);
        REQUIRE(r1 < r2);
        const std::uint32_t genRead = r1 > w2 ? newLoad : oldLoad;
        const bool stable = !(w1 < r2);   // saw the old stamp
        const std::uint32_t stamp = ID::AfterReconcile(priorStamp, genRead, stable);
        // Whatever the order, the scan is not detail for the NEW load: it
        // either ran before the load (stable, stamps the old one) or saw the
        // new clock stamp and stamped nothing.
        CHECK_FALSE(ID::Known(stamp, newLoad));
        CHECK_FALSE(ID::Arrived(priorStamp, stamp, newLoad));
    }
}

TEST_CASE("instance detail: kNone is the registry's initial stamp, and no load matches it")
{
    for (const std::uint32_t load : { 0u, 1u, 2u, 1000u }) {
        CHECK_FALSE(ID::Known(ID::kNone, load));
    }
}
