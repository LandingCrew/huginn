// Host tests for src/core/HandEquipGate.h: a slot key's equip waits for a
// busy hand, interrupts the cast after the timeout, and is dropped outside
// gameplay (0.24.26).

#include "core/HandEquipGate.h"

#include <doctest/doctest.h>

#include <ostream>  // doctest prints a std::string_view through it

using namespace Huginn::Core;

namespace
{
    // The engine's caster states by value.
    constexpr std::uint32_t kIdle = 0;
    constexpr std::uint32_t kReady = 3;     // charged, held (the 00:38 Fireball)
    constexpr std::uint32_t kCharging = 5;
    constexpr std::uint32_t kCasting = 6;
    constexpr std::uint32_t kInterruptDeselect = 9;

    HandEquipGateInput Ready(std::uint8_t hands, std::uint32_t right, std::uint32_t left, double waitedMs = 0.0)
    {
        HandEquipGateInput in;
        in.worldReady = true;
        in.hands = hands;
        in.rightState = right;
        in.leftState = left;
        in.waitedMs = waitedMs;
        return in;
    }

    using A = HandEquipAction;
}

TEST_CASE("hand equip gate: the hands a key's equip waits for")
{
    CHECK(HandsToCheck(EquipTarget::Right, false) == kHandRight);
    CHECK(HandsToCheck(EquipTarget::Left, false) == kHandLeft);
    CHECK(HandsToCheck(EquipTarget::Both, false) == kHandsBoth);  // the hold gesture
    // A two-hander, bow, crossbow or both-hands spell takes both whatever the gesture.
    CHECK(HandsToCheck(EquipTarget::Right, true) == kHandsBoth);
    CHECK(HandsToCheck(EquipTarget::Left, true) == kHandsBoth);
}

TEST_CASE("hand equip gate: idle hands equip at once")
{
    const auto d = DecideHandEquip(Ready(kHandRight, kIdle, kIdle));
    CHECK(d.action == A::EquipNow);
    CHECK(d.busyHands == 0);
}

TEST_CASE("hand equip gate: every state but idle is busy")
{
    CHECK_FALSE(CasterBusy(kIdle));
    for (std::uint32_t s = 1; s <= 9; ++s) {
        CAPTURE(s);
        CHECK(CasterBusy(s));
        CHECK(DecideHandEquip(Ready(kHandRight, s, kIdle)).action == A::Wait);
    }
}

TEST_CASE("hand equip gate: only the hands the equip needs are read")
{
    // Charging in the left hand does not hold a right-hand equip...
    CHECK(DecideHandEquip(Ready(kHandRight, kIdle, kCharging)).action == A::EquipNow);
    // ...but holds a left-hand one, and a both-hands one.
    auto d = DecideHandEquip(Ready(kHandLeft, kIdle, kCharging));
    CHECK(d.action == A::Wait);
    CHECK(d.busyHands == kHandLeft);
    d = DecideHandEquip(Ready(kHandsBoth, kIdle, kCharging));
    CHECK(d.action == A::Wait);
    CHECK(d.busyHands == kHandLeft);
    d = DecideHandEquip(Ready(kHandsBoth, kCasting, kCharging));
    CHECK(d.busyHands == kHandsBoth);
}

TEST_CASE("hand equip gate: the 00:38 case -- a charged spell held: wait, then interrupt, then equip")
{
    // Key down 0.6 s into the charge; the tap fires 300 ms after key-up.
    CHECK(DecideHandEquip(Ready(kHandRight, kReady, kIdle, 0.0)).action == A::Wait);
    CHECK(DecideHandEquip(Ready(kHandRight, kReady, kIdle, 999.0)).action == A::Wait);
    // Still held at the timeout: interrupt now...
    const auto d = DecideHandEquip(Ready(kHandRight, kReady, kIdle, kHandBusyTimeoutMs));
    CHECK(d.action == A::InterruptThenEquip);
    CHECK(d.busyHands == kHandRight);
    // ...and the next frame equips, whatever state the interrupt left behind.
    auto next = Ready(kHandRight, kInterruptDeselect, kIdle, kHandBusyTimeoutMs + 16.0);
    next.interrupted = true;
    CHECK(DecideHandEquip(next).action == A::EquipNow);
}

TEST_CASE("hand equip gate: a release before the timeout equips without an interrupt")
{
    CHECK(DecideHandEquip(Ready(kHandRight, kReady, kIdle, 200.0)).action == A::Wait);
    CHECK(DecideHandEquip(Ready(kHandRight, 4, kIdle, 400.0)).action == A::Wait);  // just released
    CHECK(DecideHandEquip(Ready(kHandRight, kIdle, kIdle, 450.0)).action == A::EquipNow);
}

TEST_CASE("hand equip gate: outside gameplay the pending equip is dropped")
{
    auto in = Ready(kHandRight, kIdle, kIdle);
    in.worldReady = false;
    CHECK(DecideHandEquip(in).action == A::Drop);
    // Dropped even after an interrupt: a load or a pause wins.
    in.interrupted = true;
    in.rightState = kCasting;
    CHECK(DecideHandEquip(in).action == A::Drop);
}

TEST_CASE("hand equip gate: the timeout is the caller's")
{
    auto in = Ready(kHandRight, kCasting, kIdle, 300.0);
    in.timeoutMs = 250.0;
    CHECK(DecideHandEquip(in).action == A::InterruptThenEquip);
}

TEST_CASE("hand equip gate: the caster states are named, and 7-9 are not idle")
{
    CHECK(CastPhaseName(kIdle).empty());
    CHECK(CasterStateName(kIdle) == "idle");
    CHECK(CastPhaseName(kReady) == "charged, held");
    CHECK(CastPhaseName(4) == "just released");
    CHECK(CastPhaseName(7) == "unnamed state 7");
    CHECK(CastPhaseName(8) == "interrupt");
    CHECK(CastPhaseName(9) == "interrupt/deselect");
    CHECK(CastPhaseName(42) == "unknown state");
    CHECK(HandMaskName(kHandRight) == "right");
    CHECK(HandMaskName(kHandsBoth) == "both");
    CHECK(HandMaskName(0) == "none");
}
