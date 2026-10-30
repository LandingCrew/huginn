// Host tests for R11: src/core/ThetaDrift.h -- the [Soak] heartbeat's and
// `hg theta`'s drift numbers (how far the live weights' means and variances
// moved from the bootstrap's).

#include "core/ThetaDrift.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <cstdint>
#include <utility>
#include <vector>

using namespace Huginn::Core::Choice;

TEST_CASE("theta drift: nothing moved")
{
    const std::vector<Weight> boot{ { 0.5, 1.0 }, { -0.2, 0.5 } };
    const auto d = ComputeDrift(boot, boot);
    CHECK(d.weights == 2);
    CHECK(d.meanMoved == 0);
    CHECK(d.varMoved == 0);
    CHECK(d.meanAbsDelta == 0.0);
    CHECK(d.maxAbsDelta == 0.0);
    CHECK(d.meanLnVar == 0.0);
    CHECK(d.maxSlot == std::numeric_limits<std::size_t>::max());
}

TEST_CASE("theta drift: means and variances apart")
{
    const std::vector<Weight> boot{ { 0.0, 1.0 }, { 1.0, 1.0 }, { -1.0, 2.0 }, { 0.3, 1.0 } };
    std::vector<Weight> live = boot;
    live[0].mu = 0.25;            // moved +0.25
    live[2].mu = -1.75;           // moved -0.75 (the largest)
    live[1].var = std::exp(-1.0); // narrowed: ln ratio -1
    live[3].var = std::exp(0.5);  // widened (the opportunity counter): +0.5
    const auto d = ComputeDrift(live, boot);
    CHECK(d.meanMoved == 2);
    CHECK(d.meanAbsDelta == doctest::Approx(0.5));
    CHECK(d.rmsDelta == doctest::Approx(std::sqrt((0.0625 + 0.5625) / 2.0)));
    CHECK(d.maxAbsDelta == doctest::Approx(0.75));
    CHECK(d.maxDelta == doctest::Approx(-0.75));
    CHECK(d.maxSlot == 2);
    CHECK(d.varMoved == 2);
    CHECK(d.meanLnVar == doctest::Approx(-0.25));
}

TEST_CASE("theta drift: a widened variance alone moves no mean (the opportunity counter)")
{
    const std::vector<Weight> boot(486, Weight{ 0.1, 1.0 });
    std::vector<Weight> live = boot;
    for (auto& w : live) w.var += 1e-4;   // one answered onset grows its need's 2 x J weights
    const auto d = ComputeDrift(live, boot);
    CHECK(d.meanMoved == 0);
    CHECK(d.varMoved == 486);
    CHECK(d.meanLnVar > 0.0);
}

TEST_CASE("theta drift: unequal lengths and bad variances")
{
    const std::vector<Weight> boot{ { 0.0, 1.0 }, { 0.0, 0.0 } };
    const std::vector<Weight> live{ { 0.0, 2.0 }, { 0.0, 1.0 }, { 9.0, 1.0 } };
    const auto d = ComputeDrift(live, boot);
    CHECK(d.weights == 2);                 // the shorter span
    CHECK(d.varMoved == 2);
    CHECK(d.meanLnVar == doctest::Approx(std::log(2.0)));   // the 0-variance pair left out of the mean
}

TEST_CASE("theta drift: b's spread")
{
    const std::vector<std::pair<std::uint32_t, double>> b{ { 0x12FCC, 0.4 }, { 0x3EB15, -0.9 }, { 0x7, 0.2 } };
    const auto d = ComputeBDrift(b);
    CHECK(d.entries == 3);
    CHECK(d.meanAbsMu == doctest::Approx(0.5));
    CHECK(d.maxAbsMu == doctest::Approx(0.9));
    CHECK(d.maxForm == 0x3EB15u);
    CHECK(ComputeBDrift(std::vector<std::pair<std::uint32_t, double>>{}).entries == 0);
}
