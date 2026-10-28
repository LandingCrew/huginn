// Host tests for src/core/SlicedWalk.h: the effect catalog's sliced load-order
// read (0.23.47) visits exactly what one unsliced walk visits, in the same
// order, whatever the slice sizes.

#include "core/SlicedWalk.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <random>
#include <utility>
#include <vector>

using Huginn::Core::SliceCursor;
using Huginn::Core::WalkSlice;

namespace
{
    using Visits = std::vector<std::pair<std::size_t, std::size_t>>;

    // A walk over `sizes` where the slice ends after `perSlice(slice)` visits.
    template <class PerSlice>
    Visits WalkInSlices(const std::vector<std::size_t>& sizes, PerSlice perSlice, std::size_t& slices)
    {
        Visits out;
        SliceCursor cursor;
        slices = 0;
        bool done = false;
        while (!done) {
            const std::size_t budget = perSlice(slices++);
            std::size_t used = 0;
            done = WalkSlice(
                cursor, sizes.size(), [&](std::size_t s) { return sizes[s]; },
                [&](std::size_t s, std::size_t i) { out.emplace_back(s, i); },
                [&]() { return ++used >= budget; });
            REQUIRE(slices < 100000);
        }
        return out;
    }

    Visits Unsliced(const std::vector<std::size_t>& sizes)
    {
        Visits out;
        for (std::size_t s = 0; s < sizes.size(); ++s) {
            for (std::size_t i = 0; i < sizes[s]; ++i) out.emplace_back(s, i);
        }
        return out;
    }
}

TEST_CASE("SlicedWalk: one unlimited slice visits every element in order")
{
    const std::vector<std::size_t> sizes{ 3, 0, 5, 1, 0, 0, 4 };
    std::size_t slices = 0;
    const auto got = WalkInSlices(sizes, [](std::size_t) { return static_cast<std::size_t>(-1); }, slices);
    CHECK(got == Unsliced(sizes));
    CHECK(slices == 1);
}

TEST_CASE("SlicedWalk: any slice size gives the unsliced sequence")
{
    const std::vector<std::size_t> sizes{ 7, 0, 13, 1, 0, 29, 2, 0 };
    const auto want = Unsliced(sizes);
    for (std::size_t k = 1; k <= want.size() + 2; ++k) {
        CAPTURE(k);
        std::size_t slices = 0;
        const auto got = WalkInSlices(sizes, [k](std::size_t) { return k; }, slices);
        CHECK(got == want);
        // ceil(n / k) slices, plus one empty closing slice when k divides n
        // (the last visit's stop() said yes before the walk saw the end).
        CHECK(slices == want.size() / k + 1);
    }
}

TEST_CASE("SlicedWalk: random slice sizes give the unsliced sequence")
{
    std::mt19937 rng(20261028u);
    for (int round = 0; round < 200; ++round) {
        std::vector<std::size_t> sizes(1 + rng() % 9);
        for (auto& n : sizes) n = rng() % 4 == 0 ? 0 : rng() % 50;
        std::size_t slices = 0;
        const auto got = WalkInSlices(sizes, [&](std::size_t) { return 1 + static_cast<std::size_t>(rng() % 17); },
            slices);
        CHECK(got == Unsliced(sizes));
    }
}

TEST_CASE("SlicedWalk: no sequences, or only empty ones, is done at once")
{
    SliceCursor cursor;
    int visits = 0;
    CHECK(WalkSlice(cursor, 0, [](std::size_t) { return std::size_t{ 0 }; },
        [&](std::size_t, std::size_t) { ++visits; }, []() { return true; }));
    const std::vector<std::size_t> empty{ 0, 0, 0 };
    SliceCursor c2;
    CHECK(WalkSlice(c2, empty.size(), [&](std::size_t s) { return empty[s]; },
        [&](std::size_t, std::size_t) { ++visits; }, []() { return true; }));
    CHECK(visits == 0);
    CHECK(c2.sequence == 3);
}

TEST_CASE("SlicedWalk: a done cursor visits nothing again")
{
    const std::vector<std::size_t> sizes{ 2, 2 };
    SliceCursor cursor;
    int visits = 0;
    auto size = [&](std::size_t s) { return sizes[s]; };
    auto visit = [&](std::size_t, std::size_t) { ++visits; };
    CHECK(WalkSlice(cursor, sizes.size(), size, visit, []() { return false; }));
    CHECK(visits == 4);
    CHECK(WalkSlice(cursor, sizes.size(), size, visit, []() { return false; }));
    CHECK(visits == 4);
}

TEST_CASE("SlicedWalk: a sequence that grows between slices is walked to its new end")
{
    std::vector<std::size_t> sizes{ 4, 3 };
    SliceCursor cursor;
    Visits got;
    auto size = [&](std::size_t s) { return sizes[s]; };
    auto visit = [&](std::size_t s, std::size_t i) { got.emplace_back(s, i); };
    std::size_t used = 0;
    CHECK_FALSE(WalkSlice(cursor, sizes.size(), size, visit, [&]() { return ++used >= 2; }));
    sizes[0] = 6;   // sequence 0 grows while the walk is inside it
    CHECK(WalkSlice(cursor, sizes.size(), size, visit, []() { return false; }));
    CHECK(got == Unsliced(sizes));
}
