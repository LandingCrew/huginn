// Host tests for src/core/CombatUpsert.h.

#include "core/CombatUpsert.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace Huginn::Core;

namespace
{
    constexpr double kInf = std::numeric_limits<double>::infinity();

    CombatUpsertGateInput OpenKey()
    {
        CombatUpsertGateInput in;
        in.enabled = true;
        in.combatLocked = true;
        in.graceMs = 600.0;
        in.sincePressMs = 5000.0;
        in.sinceShownMs = 5000.0;
        in.cooldownMs = 10000.0;
        in.sinceUpsertMs = -1.0;
        return in;
    }
}

TEST_CASE("combat upsert: the barrier is the multiplier times ln(1 + margin)")
{
    CHECK(CombatUpsertBarrier(3.0f, 0.5f) == doctest::Approx(3.0 * std::log(1.5)));
    CHECK(CombatUpsertBarrier(3.0f, 0.5f) == doctest::Approx(1.2164).epsilon(1e-4));
    CHECK(CombatUpsertBarrier(2.0f, 0.5f) == doctest::Approx(2.0 * std::log(1.5)));
    CHECK(CombatUpsertBarrier(1.0f, 0.5f) == doctest::Approx(LogHoldMargin(0.5f)));
}

TEST_CASE("combat upsert: a multiplier of 0 (or below, or NaN) turns it off")
{
    CHECK(CombatUpsertBarrier(0.0f, 0.5f) == kInf);
    CHECK(CombatUpsertBarrier(-1.0f, 0.5f) == kInf);
    CHECK(CombatUpsertBarrier(std::numeric_limits<float>::quiet_NaN(), 0.5f) == kInf);
    CHECK(CombatUpsertBarrier(std::numeric_limits<float>::infinity(), 0.5f) == kInf);
    CHECK_FALSE(CombatUpsertBeats(100.0, -100.0, CombatUpsertBarrier(0.0f, 0.5f)));
}

TEST_CASE("combat upsert: a NaN margin means nothing beats the frozen item; margin 0 means any better one does")
{
    CHECK(CombatUpsertBarrier(3.0f, std::numeric_limits<float>::quiet_NaN()) == kInf);
    CHECK(CombatUpsertBarrier(3.0f, 0.0f) == 0.0);
    CHECK(CombatUpsertBeats(0.1, 0.0, CombatUpsertBarrier(3.0f, 0.0f)));
    CHECK_FALSE(CombatUpsertBeats(0.0, 0.0, CombatUpsertBarrier(3.0f, 0.0f)));
    // A negative margin (clamped away by SlotSettings) never makes a worse item win.
    CHECK_FALSE(CombatUpsertBeats(-0.1, 0.0, CombatUpsertBarrier(3.0f, -0.5f)));
}

TEST_CASE("combat upsert: the challenger must clear the barrier, not reach it")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    CHECK(CombatUpsertBeats(b + 0.001, 0.0, b));
    CHECK_FALSE(CombatUpsertBeats(b, 0.0, b));                // exactly on it holds
    CHECK_FALSE(CombatUpsertBeats(b - 0.001, 0.0, b));
    // Works the same for negative scores: a difference, not a ratio of signs.
    CHECK(CombatUpsertBeats(-1.0 + b + 0.01, -1.0, b));
    CHECK_FALSE(CombatUpsertBeats(-1.0 + b - 0.01, -1.0, b));
    // The [Hold] give-ways while frozen (margins 0.4-0.9) are far below 3 x ln 1.5.
    CHECK_FALSE(CombatUpsertBeats(0.467, -0.369, b));   // 12:45:03.586, +0.836
    // 12:54:47 replay example: Lesser Ward +2.202 over Oakflesh -0.475 (+2.677).
    CHECK(CombatUpsertBeats(2.202, -0.475, b));
}

TEST_CASE("combat upsert: no measurement against an unranked or pinned score")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    CHECK_FALSE(CombatUpsertBeats(1.0, kUnrankedScore, b));   // frozen item never scored
    CHECK_FALSE(CombatUpsertBeats(kUnrankedScore, -5.0, b));
    CHECK_FALSE(CombatUpsertBeats(kPinnedScore, 0.0, b));
    CHECK_FALSE(CombatUpsertBeats(1.0, kPinnedScore, b));
    CHECK_FALSE(CombatUpsertBeats(std::nan(""), 0.0, b));
}

TEST_CASE("combat upsert: an open key passes the gate")
{
    CHECK(DecideCombatUpsertGate(OpenKey()) == CombatUpsertGate::Open);
}

TEST_CASE("combat upsert: off, not combat-locked, wheel open")
{
    auto in = OpenKey();
    in.enabled = false;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Off);
    in = OpenKey();
    in.combatLocked = false;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::NotCombatLocked);
    in = OpenKey();
    in.wheelOpen = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::WheelOpen);
}

TEST_CASE("combat upsert: never an override, a Swap Back item or a key with a hold of its own")
{
    auto in = OpenKey();
    in.shownPinned = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Pinned);
    in = OpenKey();
    in.remembranceHold = true;   // a hold for this key not shown here (refused by the lock)
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::RemembranceHold);
}

TEST_CASE("combat upsert: never a wildcard, never a key that changes this run anyway")
{
    auto in = OpenKey();
    in.shownWildcard = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Wildcard);
    in = OpenKey();
    in.changingThisRun = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Changing);
}

TEST_CASE("combat upsert: press grace -- pressed or changed less than fPressGraceMs ago")
{
    auto in = OpenKey();
    in.sincePressMs = 599.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);
    in.sincePressMs = 600.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
    in.sincePressMs = -1.0;   // never pressed
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);

    in = OpenKey();
    in.sinceShownMs = 100.0;   // the key's item arrived 0.1 s ago
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);

    in.graceMs = 0.0;          // fPressGraceMs = 0: no grace gate
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
}

TEST_CASE("combat upsert: at most one per key per cooldown")
{
    auto in = OpenKey();
    in.sinceUpsertMs = 9999.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Cooldown);
    in.sinceUpsertMs = 10000.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
    in.sinceUpsertMs = 0.0;
    in.cooldownMs = 0.0;   // fCombatUpsertCooldownSec = 0: no limit
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
}

TEST_CASE("combat upsert: a key that just took an upsert is in grace and in cooldown")
{
    // The upsert's own change stamps the key's shown time: grace blocks the
    // next run's second change before the cooldown is even asked.
    auto in = OpenKey();
    in.sinceShownMs = 100.0;
    in.sinceUpsertMs = 100.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);
    in.sinceShownMs = 700.0;
    in.sinceUpsertMs = 700.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Cooldown);
}

TEST_CASE("combat upsert: planning pairs the widest margins, one challenger per key")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);   // 1.216
    // Keys 0 and 2 open, key 1 not.
    const std::vector<std::optional<SlotScore>> frozen{ -1.0, std::nullopt, 0.0 };
    const std::vector<SlotScore> challengers{ 1.5, 0.5, 2.0 };
    const std::vector<std::vector<bool>> fits{
        { true, true, true },
        { true, true, true },
        { true, true, true },
    };
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, b);
    REQUIRE(picks.size() == 2);
    // Widest first: challenger 2 (2.0) on key 0 (-1.0), +3.0.
    CHECK(picks[0].key == 0);
    CHECK(picks[0].challenger == 2);
    CHECK(picks[0].margin == doctest::Approx(3.0));
    // Then challenger 0 (1.5) on key 2 (0.0): +1.5 > 1.216. Challenger 1 never clears.
    CHECK(picks[1].key == 2);
    CHECK(picks[1].challenger == 0);
}

TEST_CASE("combat upsert: a challenger goes only where it fits, and none past the barrier means none")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    const std::vector<std::optional<SlotScore>> frozen{ -1.0, -1.0 };
    const std::vector<SlotScore> challengers{ 2.0 };
    const std::vector<std::vector<bool>> fits{ { false }, { true } };
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, b);
    REQUIRE(picks.size() == 1);
    CHECK(picks[0].key == 1);

    const std::vector<SlotScore> weak{ 0.2 };   // +1.2 < 1.216
    CHECK(PlanCombatUpserts(frozen, weak, fits, b).empty());
    // Off: nothing at all.
    CHECK(PlanCombatUpserts(frozen, challengers, fits, CombatUpsertBarrier(0.0f, 0.5f)).empty());
}

TEST_CASE("combat upsert: ties go to the lower key, then the lower challenger")
{
    const double b = 1.0;
    const std::vector<std::optional<SlotScore>> frozen{ 0.0, 0.0 };
    const std::vector<SlotScore> challengers{ 2.0, 2.0 };
    const std::vector<std::vector<bool>> fits{ { true, true }, { true, true } };
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, b);
    REQUIRE(picks.size() == 2);
    CHECK(picks[0].key == 0);
    CHECK(picks[0].challenger == 0);
    CHECK(picks[1].key == 1);
    CHECK(picks[1].challenger == 1);
}

TEST_CASE("combat upsert: short fits rows and missing keys read as no")
{
    const std::vector<std::optional<SlotScore>> frozen{ -5.0, -5.0, -5.0 };
    const std::vector<SlotScore> challengers{ 2.0, 3.0 };
    const std::vector<std::vector<bool>> fits{ { true } };   // key 0: challenger 0 only; keys 1, 2: none
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, 1.0);
    REQUIRE(picks.size() == 1);
    CHECK(picks[0].key == 0);
    CHECK(picks[0].challenger == 0);
}
