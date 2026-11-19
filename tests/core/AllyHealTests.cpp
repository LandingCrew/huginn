// Host tests for src/core/AllyHeal.h (0.24.37): which teammate the
// follower-heal override is for -- the most hurt one in reach.

#include "core/AllyHeal.h"

#include <doctest/doctest.h>

#include <array>
#include <vector>

using namespace Huginn::Core::AllyHeal;

TEST_CASE("AllyHeal: the most hurt teammate in reach")
{
    SUBCASE("none listed: nobody to heal")
    {
        const auto pick = MostHurtInReach({}, 1500.0f);
        CHECK_FALSE(pick.found);
        CHECK(pick.health == doctest::Approx(1.0f));
    }

    SUBCASE("the most hurt of two in reach")
    {
        const std::array<Teammate, 2> ts{ { { 0.3f, false, 400.0f }, { 0.7f, false, 900.0f } } };
        const auto pick = MostHurtInReach(ts, 1500.0f);
        CHECK(pick.found);
        CHECK(pick.health == doctest::Approx(0.3f));
        CHECK_FALSE(pick.bleedout);
    }

    SUBCASE("one out of reach is left out, however hurt")
    {
        // The review's case: badly hurt in the next room, 2000 units away.
        const std::array<Teammate, 2> ts{ { { 0.9f, false, 2000.0f }, { 0.2f, false, 300.0f } } };
        const auto pick = MostHurtInReach(ts, 1500.0f);
        CHECK(pick.found);
        CHECK(pick.health == doctest::Approx(0.8f));
    }

    SUBCASE("only one, out of reach: not found")
    {
        const std::array<Teammate, 1> ts{ { { 0.9f, true, 2000.0f } } };
        CHECK_FALSE(MostHurtInReach(ts, 1500.0f).found);
    }

    SUBCASE("down on one knee reads 0, whatever the bar says")
    {
        const std::array<Teammate, 2> ts{ { { 0.1f, true, 500.0f }, { 0.8f, false, 500.0f } } };
        const auto pick = MostHurtInReach(ts, 1500.0f);
        CHECK(pick.health == doctest::Approx(0.0f));
        CHECK(pick.bleedout);
    }

    SUBCASE("at exactly the reach counts")
    {
        const std::array<Teammate, 1> ts{ { { 0.5f, false, 1500.0f } } };
        CHECK(MostHurtInReach(ts, 1500.0f).found);
    }

    SUBCASE("a missing share outside 0-1 is clamped")
    {
        const std::array<Teammate, 1> ts{ { { 1.4f, false, 10.0f } } };
        CHECK(MostHurtInReach(ts, 1500.0f).health == doctest::Approx(0.0f));
    }
}
