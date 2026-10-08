// Host tests for src/core/NeedCapMath.h -- the arithmetic of Slot::NeedCap.
// The in-game RunNeedCapTest (src/Tests.cpp) still covers the game side
// (classification, Add/Remove counting); these pin the math alone.

#include "core/NeedCapMath.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Huginn::Core;

TEST_CASE("need cap: items within the free allowance pay nothing")
{
    for (std::uint32_t shown = 0; shown < 3; ++shown) {
        CHECK(NeedCapFactor(0.5f, 3, shown) == 1.0f);
    }
}

TEST_CASE("need cap: each item past the allowance pays one more discount")
{
    // The shipped default: first 3 free, then x0.5 (NeedCap.h).
    CHECK(NeedCapFactor(0.5f, 3, 3) == 0.5f);     // 4th item
    CHECK(NeedCapFactor(0.5f, 3, 4) == 0.25f);    // 5th
    CHECK(NeedCapFactor(0.5f, 3, 5) == 0.125f);   // 6th
    CHECK(NeedCapFactor(0.8f, 1, 3) == doctest::Approx(0.8f * 0.8f * 0.8f));
}

TEST_CASE("need cap: no free items discounts the first one too")
{
    CHECK(NeedCapFactor(0.5f, 0, 0) == 0.5f);
    CHECK(NeedCapFactor(0.5f, 0, 1) == 0.25f);
}

TEST_CASE("need cap: a discount of 1 or more turns the cap off")
{
    CHECK_FALSE(NeedCapActive(1.0f));
    CHECK_FALSE(NeedCapActive(2.5f));
    CHECK(NeedCapActive(0.999f));
    CHECK(NeedCapActive(0.0f));
    for (std::uint32_t shown : { 0u, 3u, 7u, 200u }) {
        CHECK(NeedCapFactor(1.0f, 3, shown) == 1.0f);
        CHECK(NeedCapFactor(4.0f, 3, shown) == 1.0f);
    }
}

TEST_CASE("need cap: NaN reads as off, as the old member did")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isnan(ClampNeedCapDiscount(nan)));
    CHECK_FALSE(NeedCapActive(nan));
    CHECK(NeedCapFactor(nan, 3, 10) == 1.0f);
}

TEST_CASE("need cap: a negative discount clamps to 0 (hard cap)")
{
    CHECK(ClampNeedCapDiscount(-0.5f) == 0.0f);
    CHECK(NeedCapFactor(-0.5f, 3, 2) == 1.0f);   // still free
    CHECK(NeedCapFactor(-0.5f, 3, 3) == 0.0f);   // past it: nothing
}

TEST_CASE("need cap: the factor never rises as the page fills")
{
    float last = 1.0f;
    for (std::uint32_t shown = 0; shown < 64; ++shown) {
        const float f = NeedCapFactor(0.7f, 2, shown);
        CHECK(f <= last);
        CHECK(f >= 0.0f);
        last = f;
    }
}

TEST_CASE("need cap: matches the loop it replaced, bit for bit")
{
    // The body NeedCap::Factor ran before the port (0.23.7), shown as a uint8
    // count as NeedCap stores it.
    const auto old = [](float discount, std::uint32_t freePerNeed, std::uint8_t shown) {
        float factor = 1.0f;
        for (std::uint32_t n = freePerNeed; n <= shown; ++n) {
            factor *= discount;
        }
        return factor;
    };
    for (float d : { 0.0f, 0.1f, 0.33f, 0.5f, 0.75f, 0.9f, 0.999f }) {
        for (std::uint32_t free = 0; free <= 8; ++free) {
            for (std::uint32_t shown = 0; shown <= 255; ++shown) {
                const auto s = static_cast<std::uint8_t>(shown);
                CHECK(NeedCapFactor(d, free, s) == old(d, free, s));
            }
        }
    }
}

TEST_CASE("need cap: a huge count ends (and ends at 0)")
{
    CHECK(NeedCapFactor(0.5f, 0, std::numeric_limits<std::uint32_t>::max()) == 0.0f);
}

// For d > 0.5 the product never reaches 0: it sticks at a denormal where
// x * d rounds back to x. The early stop must give exactly what the plain
// loop gives, below the stick point, at it, and far past it.
TEST_CASE("need cap: the early stop matches the plain loop up to and past the fixed point")
{
    for (float d : { 0.75f, 0.9f, 0.999f }) {
        CAPTURE(d);
        // The plain loop, one step at a time: ref[k] = d^k as the old code
        // computed it. Run until the product has stood still for a while.
        std::vector<float> ref{ 1.0f };
        std::size_t stuckAt = 0;
        while (true) {
            const float next = ref.back() * d;
            ref.push_back(next);
            if (next == ref[ref.size() - 2]) {
                if (stuckAt == 0) stuckAt = ref.size() - 2;
                if (ref.size() - 1 >= stuckAt + 1000) break;   // 1000 steps unchanged
            } else {
                stuckAt = 0;
            }
            REQUIRE(ref.size() < 2'000'000);
        }
        REQUIRE(stuckAt > 0);
        const float stuck = ref[stuckAt];
        CHECK(stuck > 0.0f);                               // a denormal, not 0
        CHECK(stuck < std::numeric_limits<float>::min());

        // shown = k - 1 with no free items is k steps.
        const auto at = [&](std::size_t k) {
            return NeedCapFactor(d, 0, static_cast<std::uint32_t>(k - 1));
        };
        const std::size_t last = ref.size() - 1;
        for (std::size_t k = 1; k <= last; k += (k < 4096 ? 1 : 97)) {
            CHECK(at(k) == ref[k]);
        }
        for (std::size_t k = stuckAt - 3; k <= stuckAt + 3; ++k) {
            CHECK(at(k) == ref[k]);
        }
        CHECK(at(last) == ref[last]);
        // Far past it: the plain loop would only repeat `stuck`.
        CHECK(NeedCapFactor(d, 0, 100'000'000u) == stuck);
        CHECK(NeedCapFactor(d, 0, std::numeric_limits<std::uint32_t>::max()) == stuck);
        CHECK(NeedCapFactor(d, 7, std::numeric_limits<std::uint32_t>::max()) == stuck);
    }
}
