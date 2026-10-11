// Host tests for src/core/HeldRefresh.h.

#include "core/HeldRefresh.h"

#include <doctest/doctest.h>

#include <vector>

using namespace Huginn::Core;

TEST_CASE("held refresh: (a) item in the scored list -> data, utility and score refreshed")
{
    const std::vector<ItemKey> scored{ { 0x111, 0 }, { 0x3EAE2, 0 }, { 0x222, 0 } };
    const auto plan = PlanHeldRefresh(0x3EAE2, 0, false, scored, ItemKey{ 0x3EAE2, 0 });
    REQUIRE(plan.scoredIndex.has_value());
    CHECK(*plan.scoredIndex == 1);
    CHECK_FALSE(plan.useCurrent);
    CHECK(plan.refreshUtility);
    CHECK(plan.refreshScore);
    // A pinned (override) key keeps its pinned score.
    CHECK_FALSE(PlanHeldRefresh(0x3EAE2, 0, true, scored, std::nullopt).refreshScore);
}

TEST_CASE("held refresh: (b) item filtered out this run -> current data refreshed, utility kept")
{
    // The healing potion just drunk: on cooldown and at full health, so the
    // filters and the scorer dropped it -- but the registry lookup (before any
    // filter) still has it, with the new count.
    const std::vector<ItemKey> scored{ { 0x111, 0 } };
    const auto plan = PlanHeldRefresh(0x3EAE2, 0, false, scored, ItemKey{ 0x3EAE2, 0 });
    CHECK_FALSE(plan.scoredIndex.has_value());
    CHECK(plan.useCurrent);
    CHECK_FALSE(plan.refreshUtility);
    CHECK_FALSE(plan.refreshScore);
    CHECK(plan.Any());
}

TEST_CASE("held refresh: (c) nothing current -> everything kept")
{
    // Gone (the Gone path frees the key) or never gathered: no fresher data.
    const std::vector<ItemKey> scored{ { 0x111, 0 } };
    CHECK_FALSE(PlanHeldRefresh(0x12FD0, 0, false, scored, std::nullopt).Any());
    CHECK_FALSE(PlanHeldRefresh(0, 0, false, scored, ItemKey{ 0, 0 }).Any());
}

TEST_CASE("held refresh: (d) stacks must match -- the plain dagger never takes the tempered one's data")
{
    const std::vector<ItemKey> scored{ { 0x1397E, 22 } };
    CHECK(*PlanHeldRefresh(0x1397E, 22, false, scored, std::nullopt).scoredIndex == 0);
    CHECK_FALSE(PlanHeldRefresh(0x1397E, 7, false, scored, ItemKey{ 0x1397E, 22 }).Any());
    CHECK(PlanHeldRefresh(0x1397E, 7, false, scored, ItemKey{ 0x1397E, 7 }).useCurrent);
    CHECK_FALSE(PlanHeldRefresh(0x1397E, 0, false, scored, ItemKey{ 0x1397E, 7 }).Any());
}

TEST_CASE("held refresh: a Remembrance-only scored row gives data, not its unranked score")
{
    std::vector<ItemKey> scored{ { 0x12FD0, 0 } };
    scored[0].rememberedOnly = true;
    const auto plan = PlanHeldRefresh(0x12FD0, 0, false, scored, std::nullopt);
    REQUIRE(plan.scoredIndex.has_value());
    CHECK_FALSE(plan.refreshUtility);
    CHECK_FALSE(plan.refreshScore);
}
