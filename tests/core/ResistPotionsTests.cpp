// Host tests for src/core/ResistPotions.h (0.24.29): resist potions of one
// element do not stack -- none while one runs, the strongest one otherwise.

#include "core/ResistPotions.h"

#include <doctest/doctest.h>

#include <vector>

using namespace Huginn::Core;
using ResistPotions::Bit;
using E = Refresh::Element;

namespace
{
    Facets::Set ResistFire()
    {
        Facets::Set f;
        f.Add(Facets::F::ResistFire);
        return f;
    }
}

TEST_CASE("resist potions: a running potion blocks its element until its refresh window")
{
    std::array<Refresh::Timer, ResistPotions::kElementCount> t{};
    CHECK(ResistPotions::RunningBits(t, 5.0f) == 0);
    t[static_cast<std::size_t>(E::fire)].Add(100.0f);   // Fair, 100 s left
    const auto running = ResistPotions::RunningBits(t, 5.0f);
    CHECK(running == Bit(E::fire));
    CHECK(ResistPotions::Blocked(ResistFire(), running));

    Facets::Set frost;
    frost.Add(Facets::F::ResistFrost);
    CHECK_FALSE(ResistPotions::Blocked(frost, running));   // another element

    // Its last seconds: offered again (the refresh pass).
    std::array<Refresh::Timer, ResistPotions::kElementCount> ending{};
    ending[static_cast<std::size_t>(E::fire)].Add(4.0f);
    CHECK(ResistPotions::RunningBits(ending, 5.0f) == 0);
}

TEST_CASE("resist potions: a hostile or damaging item is not judged by its resist (a cloak's rider)")
{
    auto cloak = ResistFire();
    cloak.Add(Facets::F::Damage);
    CHECK_FALSE(ResistPotions::Blocked(cloak, Bit(E::fire)));
}

TEST_CASE("resist potions: greedy keeps the strongest per element")
{
    // LoreRim's Resist Fire: Faint 20 %, Diluted 30 %, Fair 40 %, Potion 50 % (cap(i) grades).
    const ResistPotions::Strengths faint{ 0.0766f }, diluted{ 0.0847f }, fair{ 0.0968f };
    ResistPotions::Strengths best{};
    for (const auto& s : { faint, diluted, fair }) ResistPotions::NoteBest(best, s);
    CHECK_FALSE(ResistPotions::Weaker(fair, best));
    CHECK(ResistPotions::Weaker(diluted, best));
    CHECK(ResistPotions::Weaker(faint, best));
    CHECK_FALSE(ResistPotions::Weaker(fair, best));   // a second stack of the best ties: kept

    // A potion that is the best for one element it resists stays.
    ResistPotions::Strengths frostAndFire{};
    frostAndFire[static_cast<std::size_t>(E::fire)] = 0.05f;
    frostAndFire[static_cast<std::size_t>(E::frost)] = 0.09f;
    ResistPotions::NoteBest(best, frostAndFire);
    CHECK_FALSE(ResistPotions::Weaker(frostAndFire, best));

    // No resist at all: never "weaker".
    CHECK_FALSE(ResistPotions::Weaker(ResistPotions::Strengths{}, best));
    CHECK_FALSE(ResistPotions::Any(ResistPotions::Strengths{}));
}

TEST_CASE("resist potions: greedy picks among the survivors of the other filters (review, PR #254)")
{
    // A frost potion runs, so the fire-and-frost potion (the strongest fire)
    // is blocked and never reaches the pick: the fire potions left are judged
    // among themselves, and the best of them is offered.
    ResistPotions::Strengths frostAndFire{}, fair{}, diluted{};
    frostAndFire[static_cast<std::size_t>(E::fire)] = 0.12f;
    frostAndFire[static_cast<std::size_t>(E::frost)] = 0.12f;
    fair[static_cast<std::size_t>(E::fire)] = 0.0968f;
    diluted[static_cast<std::size_t>(E::fire)] = 0.0847f;

    const std::vector<ResistPotions::Strengths> withBlocked{ frostAndFire, fair, diluted };
    const auto all = ResistPotions::WeakerAmong(withBlocked);
    REQUIRE(all.size() == 3);
    CHECK_FALSE(all[0]);
    CHECK(all[1]);   // judged against the blocked one, Fair would drop too
    CHECK(all[2]);

    const std::vector<ResistPotions::Strengths> survivors{ fair, diluted, fair };
    const auto kept = ResistPotions::WeakerAmong(survivors);
    REQUIRE(kept.size() == 3);
    CHECK_FALSE(kept[0]);   // Fair: the strongest that passed
    CHECK(kept[1]);         // Diluted: weaker
    CHECK_FALSE(kept[2]);   // a second stack of Fair ties: kept

    CHECK(ResistPotions::WeakerAmong({}).empty());
}

TEST_CASE("resist potions: a spell is blocked only by its own effect running (0.24.29)")
{
    ResistPotions::CastEffects running;
    running.Add(0x8D000817);   // Fire Shell's Fire Shield
    running.Add(0x8D000817);   // the same effect twice: kept once
    CHECK(running.n == 1);
    const std::uint32_t fireShell[] = { 0x8D000817 };
    const std::uint32_t frostShell[] = { 0x8D000900 };
    CHECK(ResistPotions::CastBlocked(fireShell, running));
    CHECK_FALSE(ResistPotions::CastBlocked(frostShell, running));   // a cloak's rider, another spell: stacks
    CHECK_FALSE(ResistPotions::CastBlocked({}, running));

    ResistPotions::CastEffects same;
    same.Add(0x8D000817);
    CHECK(same == running);
    same.Add(0x1);
    CHECK_FALSE(same == running);
}
