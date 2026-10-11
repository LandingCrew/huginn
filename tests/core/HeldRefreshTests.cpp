// Host tests for src/core/HeldRefresh.h.

#include "core/HeldRefresh.h"

#include <doctest/doctest.h>

#include <vector>

using namespace Huginn::Core;

TEST_CASE("held refresh: a held potion takes this run's candidate (fresh count)")
{
    const std::vector<ItemKey> scored{ { 0x111, 0 }, { 0x3EAE2, 0 }, { 0x222, 0 } };
    const auto i = FindHeldRefresh(0x3EAE2, 0, scored);
    REQUIRE(i.has_value());
    CHECK(*i == 1);
}

TEST_CASE("held refresh: an item not scored this run keeps its stored data")
{
    // An unaffordable spell the combat freeze holds: not a candidate this run.
    const std::vector<ItemKey> scored{ { 0x111, 0 } };
    CHECK_FALSE(FindHeldRefresh(0x12FD0, 0, scored).has_value());
    CHECK_FALSE(FindHeldRefresh(0, 0, scored).has_value());
}

TEST_CASE("held refresh: stacks must match -- the plain dagger never takes the tempered one's data")
{
    const std::vector<ItemKey> scored{ { 0x1397E, 22 }, { 0x1397E, 7 } };
    const auto tempered = FindHeldRefresh(0x1397E, 22, scored);
    REQUIRE(tempered.has_value());
    CHECK(*tempered == 0);
    const auto plain = FindHeldRefresh(0x1397E, 7, scored);
    REQUIRE(plain.has_value());
    CHECK(*plain == 1);
    CHECK_FALSE(FindHeldRefresh(0x1397E, 0, scored).has_value());   // stack-less matches stack-less only
}
