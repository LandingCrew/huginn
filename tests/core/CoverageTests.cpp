// Host tests for src/core/Coverage.h (covered(i)) and src/core/FilterReason.h
// (the candidate filters' reasons), both 0.23.46.

#include "core/Coverage.h"
#include "core/FilterReason.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>
#include <string>
#include <vector>

using namespace Huginn::Core;

namespace
{
    // The refresh prior's centre as shipped ([Scoring] fRefreshPriorSec = 5).
    constexpr float kCentre = Coverage::kRefreshPriorSec;

    Effect::Col C(std::string_view name)
    {
        const auto c = Effect::FromName(name);
        REQUIRE(c.has_value());
        return *c;
    }

    Effect::Cap Sorted(Effect::Cap cap)
    {
        std::sort(cap.begin(), cap.end(), [](const Effect::Value& a, const Effect::Value& b) { return a.col < b.col; });
        return cap;
    }
}

TEST_CASE("coverage: the refresh prior")
{
    using Coverage::LeftPrior;
    CHECK(LeftPrior(Coverage::kNoTimer, kCentre) == 1.0f);         // constant: never runs out
    CHECK(LeftPrior(5.0f, kCentre) == doctest::Approx(0.5f));       // the prior's centre
    CHECK(LeftPrior(0.0f, kCentre) == doctest::Approx(0.0759f).epsilon(0.001));
    CHECK(LeftPrior(15.0f, kCentre) > 0.99f);
    CHECK(LeftPrior(60.0f, kCentre) == doctest::Approx(1.0f));
    CHECK(LeftPrior(3.0f, kCentre) < LeftPrior(4.0f, kCentre));               // monotone in r
}

TEST_CASE("coverage: the refresh prior's centre is the configured one (R8.1, fRefreshPriorSec)")
{
    using Coverage::LeftPrior;
    // Half uncovered at the configured centre, whatever it is; the scale stays 2 s.
    CHECK(LeftPrior(8.0f, 8.0f) == doctest::Approx(0.5f));
    CHECK(LeftPrior(10.0f, 8.0f) == doctest::Approx(LeftPrior(7.0f, 5.0f)));   // a shift, not a rescale
    CHECK(LeftPrior(Coverage::kNoTimer, 8.0f) == 1.0f);                       // still no timer, no factor
    CHECK(LeftPrior(5.0f, 8.0f) < LeftPrior(5.0f, kCentre));                  // a later centre reads 5 s as less covered

    // And covered(i) carries it: Oakflesh with 8 s left is half covered at centre 8.
    const Effect::Cap oakflesh = Sorted({ { C("defense"), 0.4f }, { C("defense_armor"), 0.4f }, { C("school_alteration"), 1.0f } });
    Coverage::Active a;
    a.Add(C("defense_armor"), 0.4f, 8.0f);
    CHECK(Coverage::Covered(oakflesh, a, 8.0f).value == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(Coverage::Covered(oakflesh, a, 8.0f).remaining == 8.0f);
    CHECK(Coverage::Covered(oakflesh, a, kCentre).value > 0.8f);
}

TEST_CASE("coverage: a buff of the same column covers by strength")
{
    const Effect::Cap oakflesh = Sorted({ { C("defense"), 0.4f }, { C("defense_armor"), 0.4f }, { C("school_alteration"), 1.0f } });
    const Effect::Cap stoneflesh = Sorted({ { C("defense"), 0.8f }, { C("defense_armor"), 0.8f }, { C("school_alteration"), 1.0f } });
    Coverage::Active a;
    CHECK(Coverage::Covered(oakflesh, a, kCentre).value == 0.0f);   // nothing active

    a.Add(C("defense_armor"), 0.4f, 60.0f);                // Oakflesh up, a minute left
    const auto self = Coverage::Covered(oakflesh, a, kCentre);
    CHECK(self.value == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(self.remaining == 60.0f);
    // A stronger spell of the column reads half covered: Oakflesh up does not
    // make Stoneflesh redundant (the armour filter's problem, doc 11 2.1).
    CHECK(Coverage::Covered(stoneflesh, a, kCentre).value == doctest::Approx(0.5f).epsilon(1e-3));

    SUBCASE("near expiry reads partly uncovered")
    {
        Coverage::Active b;
        b.Add(C("defense_armor"), 0.4f, 5.0f);
        CHECK(Coverage::Covered(oakflesh, b, kCentre).value == doctest::Approx(0.5f));
        CHECK(Coverage::Covered(oakflesh, b, kCentre).remaining == 5.0f);
    }
    SUBCASE("families and item features never cover")
    {
        Coverage::Active b;
        b.Add(C("defense"), 1.0f, Coverage::kNoTimer);           // a family: ignored by Covered
        b.Add(C("school_alteration"), 1.0f, Coverage::kNoTimer); // a feature: ignored
        CHECK(Coverage::Covered(oakflesh, b, kCentre).value == 0.0f);
    }
    SUBCASE("another column does not cover")
    {
        Coverage::Active b;
        b.Add(C("resist_fire"), 1.0f, Coverage::kNoTimer);
        CHECK(Coverage::Covered(oakflesh, b, kCentre).value == 0.0f);
    }
}

TEST_CASE("coverage: Add keeps the strongest strength and the longest time")
{
    Coverage::Active a;
    const auto col = C("resist_fire");
    a.Add(col, 0.3f, 20.0f);
    a.Add(col, 0.6f, 5.0f);
    CHECK(a.strength[static_cast<std::size_t>(col)] == 0.6f);
    CHECK(a.remaining[static_cast<std::size_t>(col)] == 20.0f);
    a.Add(col, 0.1f, Coverage::kNoTimer);    // a ring: constant outlasts any timer
    CHECK(a.remaining[static_cast<std::size_t>(col)] == Coverage::kNoTimer);
    a.Add(col, 0.2f, 90.0f);                 // ... and stays constant
    CHECK(a.remaining[static_cast<std::size_t>(col)] == Coverage::kNoTimer);
    a.Add(col, 0.0f, 1.0f);                  // no strength: not an effect
    CHECK(a.strength[static_cast<std::size_t>(col)] == 0.6f);
}

TEST_CASE("coverage: summons count against the limit")
{
    const Effect::Cap wolf = Sorted({ { C("summon"), 0.5f }, { C("summon_creature"), 0.5f } });
    const Effect::Cap thrall = Sorted({ { C("summon"), 0.7f }, { C("summon_reanimate"), 0.7f } });
    const Effect::Cap bound = Sorted({ { C("summon"), 0.5f }, { C("summon_bound_weapon"), 0.5f } });
    Coverage::Active a;
    CHECK(Coverage::Covered(wolf, a, kCentre).value == 0.0f);

    a.summons = 1;
    a.summonLimit = 2.0f;           // Twin Souls
    a.summonRemaining = 3.0f;       // the oldest is about to go, but there is room
    CHECK(Coverage::Covered(wolf, a, kCentre).value == doctest::Approx(0.5f));
    CHECK(Coverage::Covered(wolf, a, kCentre).remaining == Coverage::kNoTimer);   // no refresh factor below the limit
    CHECK(Coverage::Covered(thrall, a, kCentre).value == doctest::Approx(0.5f));
    CHECK(Coverage::Covered(bound, a, kCentre).value == 0.0f);   // a bound weapon is not a summon count

    a.summons = 2;                  // at the limit: the refresh factor of the one a cast would dismiss
    a.summonRemaining = 60.0f;
    CHECK(Coverage::Covered(wolf, a, kCentre).value == doctest::Approx(1.0f).epsilon(1e-4));
    a.summonRemaining = 5.0f;
    CHECK(Coverage::Covered(wolf, a, kCentre).value == doctest::Approx(0.5f));
    CHECK(Coverage::Covered(wolf, a, kCentre).remaining == 5.0f);

    SUBCASE("vanilla limit")
    {
        Coverage::Active b;
        b.summons = 1;
        CHECK(Coverage::Covered(wolf, b, kCentre).value == doctest::Approx(1.0f));
    }
}

TEST_CASE("coverage: the summon limit and the filter's test (the Twin Souls bug)")
{
    using Coverage::SummonLimitCount;
    using Coverage::SummonLimitReached;
    CHECK(SummonLimitCount(1.0f) == 1);
    CHECK(SummonLimitCount(2.0f) == 2);
    CHECK(SummonLimitCount(1.9999f) == 2);   // float noise from an entry point
    CHECK(SummonLimitCount(2.5f) == 2);      // a fraction rounds down
    CHECK(SummonLimitCount(0.0f) == 1);
    CHECK(SummonLimitCount(std::nanf("")) == 1);

    // Vanilla (limit 1): identical to the old "any summon up" test.
    CHECK_FALSE(SummonLimitReached(0, 1.0f));
    CHECK(SummonLimitReached(1, 1.0f));
    CHECK(SummonLimitReached(3, 1.0f));
    // Twin Souls (limit 2): one summon up leaves room for the second.
    CHECK_FALSE(SummonLimitReached(1, 2.0f));
    CHECK(SummonLimitReached(2, 2.0f));
    // A limit under 1 behaves as 1, as the old test did.
    CHECK(SummonLimitReached(1, 0.0f));
    CHECK_FALSE(SummonLimitReached(0, 0.0f));
}

TEST_CASE("coverage: the summon a cast would dismiss is the oldest")
{
    using K = Coverage::CommandedActor::Kind;
    std::vector<Coverage::CommandedActor> list;
    CHECK(Coverage::OldestRemaining(list) == Coverage::kNoTimer);
    list.push_back({ 1, 11, K::Summon, 50.0f, 10.0f });
    list.push_back({ 2, 12, K::Summon, 8.0f, 52.0f });
    list.push_back({ 3, 13, K::Other, 1.0f, 99.0f });   // a commanded enemy: not a summon slot
    CHECK(Coverage::OldestRemaining(list) == 8.0f);
}

TEST_CASE("filter reasons: names, groups and lookup")
{
    using Filter::Reason;
    CHECK(Filter::kReasonNames.size() == Filter::kReasonCount);
    CHECK(Filter::ReasonName(Reason::affordability) == "affordability");
    CHECK(Filter::ReasonName(Reason::summon_limit) == "summon_limit");
    CHECK(Filter::ReasonName(Reason::truncated) == "truncated");
    // Every name is distinct and snake_case (they are JSON and log tokens).
    for (std::size_t i = 0; i < Filter::kReasonCount; ++i) {
        for (const char ch : Filter::kReasonNames[i]) CHECK(((ch >= 'a' && ch <= 'z') || ch == '_'));
        for (std::size_t j = i + 1; j < Filter::kReasonCount; ++j) CHECK(Filter::kReasonNames[i] != Filter::kReasonNames[j]);
    }

    // R8.1 (0.23.55): the four hard gates, appended. The values below stay
    // pinned (old files index their own head's list, but the plugin reads the
    // reasons by value), and the head lists them in this order.
    CHECK(Filter::kReasonCount == 21);   // 0.24.29: lock_out_of_reach, resist_weaker
    CHECK(static_cast<int>(Reason::resist_weaker) == 20);
    CHECK(static_cast<int>(Reason::invisibility) == 5);
    CHECK(static_cast<int>(Reason::resist) == 11);
    CHECK(static_cast<int>(Reason::truncated) == 14);
    CHECK(static_cast<int>(Reason::no_follower) == 15);
    CHECK(Filter::ReasonName(Reason::no_follower) == "no_follower");
    CHECK(Filter::ReasonName(Reason::no_workstation) == "no_workstation");
    CHECK(Filter::ReasonName(Reason::not_dark) == "not_dark");
    CHECK(Filter::ReasonName(Reason::favorite_suppressed) == "favorite_suppressed");
    // 0.24.29: lock_out_of_reach, appended after them (the head lists 20).
    CHECK(static_cast<int>(Reason::lock_out_of_reach) == 19);
    CHECK(Filter::ReasonName(Reason::lock_out_of_reach) == "lock_out_of_reach");
    CHECK(Filter::ReasonName(Reason::_Count) == "?");

    const std::vector<Filter::Dropped> dropped{
        { 0x100, 0, Reason::affordability },
        { 0x200, 7, Reason::equipped },
        { 0x200, 9, Reason::cooldown },
    };
    CHECK(Filter::Find(dropped, 0x100, 0) == Reason::affordability);
    CHECK(Filter::Find(dropped, 0x200, 9) == Reason::cooldown);   // the exact stack first
    CHECK(Filter::Find(dropped, 0x200, 0) == Reason::equipped);   // a row without a unique ID: the first stack
    CHECK(Filter::Find(dropped, 0x100, 4) == Reason::affordability);   // a candidate without one
    CHECK_FALSE(Filter::Find(dropped, 0x200, 3).has_value());     // another stack, both known: not this one
    CHECK_FALSE(Filter::Find(dropped, 0x300, 0).has_value());     // never dropped
}
