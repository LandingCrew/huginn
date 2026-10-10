// core/WaterSelfCheck.h: the drop-ahead probe's check of its own water reading
// at the player (0.23.23), run on the game side by state/DropAheadProbe.cpp.

#include "core/WaterSelfCheck.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

using namespace Huginn::Core::Needs;

namespace
{
    // 2026-10-10 11:23:48 (LoreRim): feet -3242, the probe read water -3008
    // ahead; the player was on the ground and never swimming.
    OwnWaterSample Standing(float feetZ, float waterZ)
    {
        OwnWaterSample s;
        s.waterKnown = true;
        s.waterZ = waterZ;
        s.feetZ = feetZ;
        return s;
    }
}

TEST_CASE("false water: a plane far over the feet of a dry, grounded player contradicts the engine")
{
    CHECK(WaterContradictsPlayer(Standing(-3242.0f, -3008.0f)));  // 234 over the feet
    CHECK(WaterContradictsPlayer(Standing(-3372.0f, -3008.0f)));  // 364
    // Swimming depth or less: a player standing (wading) in real water.
    CHECK_FALSE(WaterContradictsPlayer(Standing(-1100.0f, -1040.0f)));  // 60
    CHECK_FALSE(WaterContradictsPlayer(Standing(-1140.0f, -1040.0f)));  // 100
    CHECK_FALSE(WaterContradictsPlayer(Standing(-1190.0f, -1040.0f)));  // 150: at the threshold, not over it
    CHECK(WaterContradictsPlayer(Standing(-1191.0f, -1040.0f)));
    // Water under the feet (a bank over a pond) is no contradiction.
    CHECK_FALSE(WaterContradictsPlayer(Standing(-2810.0f, -3400.0f)));
}

TEST_CASE("false water: swimming, under water, airborne, mounted or no water: never a contradiction")
{
    auto s = Standing(-3242.0f, -3008.0f);
    s.swimming = true;
    CHECK_FALSE(WaterContradictsPlayer(s));
    s = Standing(-3242.0f, -3008.0f);
    s.underwater = true;
    CHECK_FALSE(WaterContradictsPlayer(s));
    s = Standing(-3242.0f, -3008.0f);
    s.airborne = true;
    CHECK_FALSE(WaterContradictsPlayer(s));
    s = Standing(-3242.0f, -3008.0f);
    s.mounted = true;
    CHECK_FALSE(WaterContradictsPlayer(s));
    s = Standing(-3242.0f, -3008.0f);
    s.waterKnown = false;
    CHECK_FALSE(WaterContradictsPlayer(s));
    // No-water heights and a NaN feet Z.
    CHECK_FALSE(WaterContradictsPlayer(Standing(-3242.0f, std::numeric_limits<float>::infinity())));
    CHECK_FALSE(WaterContradictsPlayer(Standing(-3242.0f, 2147483648.0f)));
    CHECK_FALSE(WaterContradictsPlayer(Standing(std::nanf(""), -3008.0f)));
    CHECK_FALSE(WaterContradictsPlayer(Standing(-3242.0f, std::nanf(""))));
}

TEST_CASE("false water: the hold fires once after 0.5 s on one plane, at the 100 ms cadence")
{
    FalseWaterHold hold;
    const WaterPlane plane{ 0x0001A2B3u, -3008.0f };
    int fired = 0;
    double firedAt = -1.0;
    for (int i = 0; i <= 20; ++i) {
        const double t = 100.0 + 0.1 * i;
        if (hold.Update(t, true, plane)) {
            ++fired;
            firedAt = t;
        }
    }
    CHECK(fired == 1);
    CHECK(firedAt == doctest::Approx(100.5));
    CHECK(hold.HeldSec(102.0) == doctest::Approx(2.0));
}

TEST_CASE("false water: a break, another plane or a gap restarts the hold")
{
    const WaterPlane plane{ 7u, -3008.0f };

    // A landing: 0.3 s of contradiction, one sample without (the swim flag), then 0.4 s: no fire.
    FalseWaterHold hold;
    for (double t : { 0.0, 0.1, 0.2, 0.3 }) CHECK_FALSE(hold.Update(t, true, plane));
    CHECK_FALSE(hold.Update(0.4, false, plane));
    CHECK(hold.HeldSec(0.4) == 0.0);
    for (double t : { 0.5, 0.6, 0.7, 0.8, 0.9 }) CHECK_FALSE(hold.Update(t, true, plane));
    CHECK(hold.Update(1.0, true, plane));

    // Another cell, or another height in the same cell: a new plane.
    FalseWaterHold h2;
    for (double t : { 0.0, 0.1, 0.2, 0.3, 0.4 }) CHECK_FALSE(h2.Update(t, true, plane));
    CHECK_FALSE(h2.Update(0.5, true, WaterPlane{ 8u, -3008.0f }));
    CHECK_FALSE(h2.Update(0.6, true, WaterPlane{ 8u, -3100.0f }));
    // Within the tolerance it is the same plane: the hold goes on.
    CHECK_FALSE(h2.Update(0.7, true, WaterPlane{ 8u, -3097.0f }));
    for (double t : { 0.8, 0.9, 1.0 }) CHECK_FALSE(h2.Update(t, true, WaterPlane{ 8u, -3100.0f }));
    CHECK(h2.Update(1.1, true, WaterPlane{ 8u, -3100.0f }));

    // A menu: the hook stops for 30 s. What held before the gap does not count.
    FalseWaterHold h3;
    for (double t : { 0.0, 0.1, 0.2, 0.3 }) CHECK_FALSE(h3.Update(t, true, plane));
    CHECK_FALSE(h3.Update(30.3, true, plane));
    CHECK_FALSE(h3.Update(30.7, true, plane));
    CHECK(h3.Update(30.8, true, plane));

    // A slow frame (200 ms between probes) is no gap.
    FalseWaterHold h4;
    CHECK_FALSE(h4.Update(0.0, true, plane));
    CHECK_FALSE(h4.Update(0.2, true, plane));
    CHECK_FALSE(h4.Update(0.4, true, plane));
    CHECK(h4.Update(0.6, true, plane));
}

TEST_CASE("false water: the hold fires again only after a restart")
{
    FalseWaterHold hold;
    const WaterPlane plane{ 7u, -3008.0f };
    for (int i = 0; i <= 5; ++i) hold.Update(0.1 * i, true, plane);
    for (int i = 6; i <= 30; ++i) CHECK_FALSE(hold.Update(0.1 * i, true, plane));
    hold.Reset();
    for (int i = 0; i < 5; ++i) CHECK_FALSE(hold.Update(10.0 + 0.1 * i, true, plane));
    CHECK(hold.Update(10.5, true, plane));
}

TEST_CASE("false water: the blacklist matches the cell and the height within a few units")
{
    WaterPlaneBlacklist list;
    CHECK(list.Add({ 0x1234u, -3008.0f }) == WaterPlaneBlacklist::AddResult::Added);
    CHECK(list.Matches(0x1234u, -3008.0f));
    CHECK(list.Matches(0x1234u, -3008.0f + kWaterPlaneTolerance));
    CHECK(list.Matches(0x1234u, -3008.0f - kWaterPlaneTolerance));
    CHECK_FALSE(list.Matches(0x1234u, -3008.0f - kWaterPlaneTolerance - 0.5f));
    CHECK_FALSE(list.Matches(0x1234u, -14000.0f));  // the same cell's sea is still water
    CHECK_FALSE(list.Matches(0x1235u, -3008.0f));   // the neighbour's plane at the same height too
    CHECK_FALSE(list.Matches(0x1234u, std::nanf("")));
    // The same plane again: one entry, one log line.
    CHECK(list.Add({ 0x1234u, -3006.0f }) == WaterPlaneBlacklist::AddResult::Known);
    CHECK(list.Size() == 1);

    // A swim proves it real: off the list.
    CHECK(list.Remove(0x1234u, -3007.0f));
    CHECK_FALSE(list.Matches(0x1234u, -3008.0f));
    CHECK_FALSE(list.Remove(0x1234u, -3007.0f));

    // A game load clears it.
    list.Add({ 1u, 0.0f });
    list.Clear();
    CHECK(list.Size() == 0);
    CHECK_FALSE(list.Matches(1u, 0.0f));
}

TEST_CASE("false water: the blacklist is small and says when it is full")
{
    WaterPlaneBlacklist list;
    for (std::uint32_t i = 0; i < WaterPlaneBlacklist::kCapacity; ++i) {
        CHECK(list.Add({ i, 100.0f }) == WaterPlaneBlacklist::AddResult::Added);
    }
    CHECK(list.Add({ 999u, 100.0f }) == WaterPlaneBlacklist::AddResult::Full);
    CHECK(list.Add({ 3u, 101.0f }) == WaterPlaneBlacklist::AddResult::Known);
    CHECK_FALSE(list.Matches(999u, 100.0f));
    // Remove keeps the others.
    CHECK(list.Remove(0u, 100.0f));
    for (std::uint32_t i = 1; i < WaterPlaneBlacklist::kCapacity; ++i) CHECK(list.Matches(i, 100.0f));
    CHECK(list.Add({ 999u, 100.0f }) == WaterPlaneBlacklist::AddResult::Added);
}

TEST_CASE("swim check: swimming, the probe's water at the player is the engine's water")
{
    // 2026-10-10 13:51 (a cave pool): both read -1040.
    CHECK(CheckSwimWater(true, true, -1040.0f, -1040.0f) == SwimWaterCheck::Match);
    CHECK(CheckSwimWater(true, true, -1037.0f, -1040.0f) == SwimWaterCheck::Match);
    CHECK(CheckSwimWater(true, true, -3008.0f, -3242.0f) == SwimWaterCheck::Mismatch);
    CHECK(CheckSwimWater(false, true, -3008.0f, -3242.0f) == SwimWaterCheck::Unchecked);
    CHECK(CheckSwimWater(true, false, -1040.0f, -1040.0f) == SwimWaterCheck::Unchecked);
    // The engine's "no water" (-FLT_MAX through GetExteriorWaterHeight) is unchecked, not a mismatch.
    CHECK(CheckSwimWater(true, true, -1040.0f, -std::numeric_limits<float>::max()) == SwimWaterCheck::Unchecked);
}
