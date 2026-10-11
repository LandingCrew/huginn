// Host tests for src/core/HeldRefresh.h.

#include "core/HeldRefresh.h"

#include <doctest/doctest.h>

#include <vector>

using namespace Huginn::Core;

TEST_CASE("held refresh: (a) item in the scored list -> data, utility and score refreshed")
{
    const std::vector<ItemKey> scored{ { 0x111, 0 }, { 0x3EAE2, 0 }, { 0x222, 0 } };
    const std::vector<ItemKey> generated{ { 0x111, 0 }, { 0x3EAE2, 0 }, { 0x222, 0 }, { 0x333, 0 } };
    const auto plan = PlanHeldRefresh(0x3EAE2, 0, false, scored, generated);
    REQUIRE(plan.scoredIndex.has_value());
    CHECK(*plan.scoredIndex == 1);
    CHECK_FALSE(plan.generatedIndex.has_value());
    CHECK(plan.refreshUtility);
    CHECK(plan.refreshScore);
    // A pinned (override) key keeps its pinned score.
    CHECK_FALSE(PlanHeldRefresh(0x3EAE2, 0, true, scored, generated).refreshScore);
}

TEST_CASE("held refresh: (b) item generated but under the scorer's floors -> data refreshed, utility kept")
{
    // The healing potion after drinking one: health is back up, its context
    // weight sits on the floor, the scorer dropped it -- but the generator
    // still produced it, with the new count.
    const std::vector<ItemKey> scored{ { 0x111, 0 } };
    const std::vector<ItemKey> generated{ { 0x111, 0 }, { 0x3EAE2, 0 } };
    const auto plan = PlanHeldRefresh(0x3EAE2, 0, false, scored, generated);
    CHECK_FALSE(plan.scoredIndex.has_value());
    REQUIRE(plan.generatedIndex.has_value());
    CHECK(*plan.generatedIndex == 1);
    CHECK_FALSE(plan.refreshUtility);
    CHECK_FALSE(plan.refreshScore);
    CHECK(plan.Any());
}

TEST_CASE("held refresh: (c) in neither list -> everything kept")
{
    // For example a spell the affordability filter dropped while the freeze
    // holds it on its key: no list has it this run.
    const std::vector<ItemKey> scored{ { 0x111, 0 } };
    const std::vector<ItemKey> generated{ { 0x111, 0 } };
    const auto plan = PlanHeldRefresh(0x12FD0, 0, false, scored, generated);
    CHECK_FALSE(plan.Any());
    CHECK_FALSE(PlanHeldRefresh(0, 0, false, scored, generated).Any());
}

TEST_CASE("held refresh: (d) stacks must match -- the plain dagger never takes the tempered one's data")
{
    const std::vector<ItemKey> scored{ { 0x1397E, 22 } };
    const std::vector<ItemKey> generated{ { 0x1397E, 22 }, { 0x1397E, 7 } };
    CHECK(*PlanHeldRefresh(0x1397E, 22, false, scored, generated).scoredIndex == 0);
    const auto plain = PlanHeldRefresh(0x1397E, 7, false, scored, generated);
    CHECK_FALSE(plain.scoredIndex.has_value());
    REQUIRE(plain.generatedIndex.has_value());
    CHECK(*plain.generatedIndex == 1);
    CHECK_FALSE(PlanHeldRefresh(0x1397E, 0, false, scored, generated).Any());   // stack-less matches stack-less only
}

TEST_CASE("held refresh: a Remembrance-only scored row gives data, not its unranked score")
{
    std::vector<ItemKey> scored{ { 0x12FD0, 0 } };
    scored[0].rememberedOnly = true;
    const auto plan = PlanHeldRefresh(0x12FD0, 0, false, scored, {});
    REQUIRE(plan.scoredIndex.has_value());
    CHECK_FALSE(plan.refreshUtility);
    CHECK_FALSE(plan.refreshScore);
}
