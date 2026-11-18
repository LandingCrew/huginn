// Host tests for src/core/HandEquipGate.h: a slot key's equip waits for a
// casting hand, goes through a transient state after a moment, is dropped
// when the cast outlasts the wait or gameplay ends, and is never interrupted
// (0.24.26).

#include "core/HandEquipGate.h"

#include <doctest/doctest.h>

#include <ostream>  // doctest prints a std::string_view through it

using namespace Huginn::Core;

namespace
{
    // The engine's caster states by value.
    constexpr std::uint32_t kIdle = 0;
    constexpr std::uint32_t kReady = 3;     // charged, held (the 00:38 Fireball)
    constexpr std::uint32_t kReleased = 4;
    constexpr std::uint32_t kCharging = 5;
    constexpr std::uint32_t kCasting = 6;   // a concentration spell or ward held
    constexpr std::uint32_t kInterrupt = 8;
    constexpr std::uint32_t kInterruptDeselect = 9;

    HandEquipGateInput At(double nowMs, std::uint8_t hands, std::uint32_t right, std::uint32_t left)
    {
        HandEquipGateInput in;
        in.worldReady = true;
        in.hands = hands;
        in.rightState = right;
        in.leftState = left;
        in.nowMs = nowMs;
        return in;
    }

    using A = HandEquipAction;
    using W = HandEquipWhy;
}

TEST_CASE("hand equip gate: a right-hand equip reads the left hand only when it can move it")
{
    // Right tap, the left hand holding something of its own: the right hand alone.
    CHECK(HandsToCheck(EquipTarget::Right, false, false) == kHandRight);
    // The left hand would change: it holds this same weapon (taken off it
    // first), or a two-hander / bow / two-handed spell is in either hand.
    CHECK(HandsToCheck(EquipTarget::Right, false, true) == kHandsBoth);
    // The item itself takes both hands.
    CHECK(HandsToCheck(EquipTarget::Right, true, false) == kHandsBoth);
    // A left-hand equip (or a torch) and the hold check both, always.
    CHECK(HandsToCheck(EquipTarget::Left, false, false) == kHandsBoth);
    CHECK(HandsToCheck(EquipTarget::Both, false, false) == kHandsBoth);
}

TEST_CASE("hand equip gate: idle hands equip at once")
{
    HandEquipWait w(0.0);
    const auto d = w.Update(At(16.0, kHandRight, kIdle, kIdle));
    CHECK(d.action == A::EquipNow);
    CHECK(d.why == W::HandsIdle);
    CHECK(d.busyHands == 0);
}

TEST_CASE("hand equip gate: states 1-6 are active, 7-9 transient")
{
    CHECK_FALSE(CasterActive(kIdle));
    CHECK_FALSE(CasterTransient(kIdle));
    for (std::uint32_t s = 1; s <= 6; ++s) {
        CAPTURE(s);
        CHECK(CasterActive(s));
        CHECK_FALSE(CasterTransient(s));
    }
    for (std::uint32_t s = 7; s <= 9; ++s) {
        CAPTURE(s);
        CHECK_FALSE(CasterActive(s));
        CHECK(CasterTransient(s));
    }
    CHECK(CasterTransient(42));  // a state this does not know: waited on briefly, never for long
}

TEST_CASE("hand equip gate: the 00:38 case -- a charged spell held: wait, never interrupt, equip on release")
{
    HandEquipWait w(0.0);
    auto d = w.Update(At(0.0, kHandRight, kReady, kIdle));
    CHECK(d.action == A::Wait);
    CHECK(d.why == W::Casting);
    CHECK(d.activeHands == kHandRight);
    // Past the first cut's 1 s interrupt: still waiting.
    CHECK(w.Update(At(1500.0, kHandRight, kReady, kIdle)).action == A::Wait);
    // The player lets go: released, then idle.
    CHECK(w.Update(At(1700.0, kHandRight, kReleased, kIdle)).action == A::Wait);
    d = w.Update(At(1800.0, kHandRight, kIdle, kIdle));
    CHECK(d.action == A::EquipNow);
    CHECK(d.why == W::HandsIdle);
}

TEST_CASE("hand equip gate: a cast held past 3 s drops the equip")
{
    HandEquipWait w(100.0);
    CHECK(w.Update(At(3099.0, kHandRight, kCasting, kIdle)).action == A::Wait);
    const auto d = w.Update(At(100.0 + kActiveWaitMaxMs, kHandRight, kCasting, kIdle));
    CHECK(d.action == A::Drop);
    CHECK(d.why == W::StillCasting);
    CHECK(d.activeHands == kHandRight);
    CHECK(w.WaitedMs(100.0 + kActiveWaitMaxMs) == doctest::Approx(kActiveWaitMaxMs));
}

TEST_CASE("hand equip gate: a transient state is waited on 250 ms, then gone through")
{
    HandEquipWait w(0.0);
    auto d = w.Update(At(10.0, kHandRight, kInterruptDeselect, kIdle));
    CHECK(d.action == A::Wait);
    CHECK(d.why == W::Transient);
    CHECK(d.busyHands == kHandRight);
    CHECK(d.activeHands == 0);
    CHECK(w.Update(At(259.0, kHandRight, kInterruptDeselect, kIdle)).action == A::Wait);
    d = w.Update(At(10.0 + kTransientWaitMs, kHandRight, kInterruptDeselect, kIdle));
    CHECK(d.action == A::EquipNow);
    CHECK(d.why == W::ThroughTransient);
}

TEST_CASE("hand equip gate: a transient state that clears equips at once")
{
    HandEquipWait w(0.0);
    CHECK(w.Update(At(0.0, kHandRight, kInterrupt, kIdle)).action == A::Wait);
    CHECK(w.Update(At(50.0, kHandRight, kIdle, kIdle)).action == A::EquipNow);
}

TEST_CASE("hand equip gate: the transient clock starts when the cast ends, not at the park")
{
    // A charge held 2 s, then a transient state: the 250 ms runs from the
    // first transient frame, so the equip does not go on that frame.
    HandEquipWait w(0.0);
    CHECK(w.Update(At(0.0, kHandRight, kCharging, kIdle)).action == A::Wait);
    CHECK(w.Update(At(2000.0, kHandRight, kInterrupt, kIdle)).action == A::Wait);
    CHECK(w.Update(At(2200.0, kHandRight, kInterrupt, kIdle)).action == A::Wait);
    // Back to active resets it.
    CHECK(w.Update(At(2210.0, kHandRight, kCharging, kIdle)).action == A::Wait);
    CHECK(w.Update(At(2300.0, kHandRight, kInterrupt, kIdle)).action == A::Wait);
    CHECK(w.Update(At(2549.0, kHandRight, kInterrupt, kIdle)).action == A::Wait);
    CHECK(w.Update(At(2550.0, kHandRight, kInterrupt, kIdle)).action == A::EquipNow);
}

TEST_CASE("hand equip gate: an active hand outranks a transient one")
{
    HandEquipWait w(0.0);
    CHECK(w.Update(At(0.0, kHandsBoth, kInterrupt, kCharging)).action == A::Wait);
    const auto d = w.Update(At(1000.0, kHandsBoth, kInterrupt, kCharging));
    CHECK(d.action == A::Wait);
    CHECK(d.why == W::Casting);
    CHECK(d.busyHands == kHandsBoth);
    CHECK(d.activeHands == kHandLeft);
}

TEST_CASE("hand equip gate: only the checked hands are read")
{
    HandEquipWait w(0.0);
    // Charging in the left hand does not hold a right-hand equip that leaves the left alone...
    CHECK(w.Update(At(0.0, kHandRight, kIdle, kCharging)).action == A::EquipNow);
    // ...but holds one that checks both.
    HandEquipWait both(0.0);
    const auto d = both.Update(At(0.0, kHandsBoth, kIdle, kCharging));
    CHECK(d.action == A::Wait);
    CHECK(d.activeHands == kHandLeft);
}

TEST_CASE("hand equip gate: outside gameplay the pending equip is dropped")
{
    HandEquipWait w(0.0);
    auto in = At(0.0, kHandRight, kIdle, kIdle);
    in.worldReady = false;
    const auto d = w.Update(in);
    CHECK(d.action == A::Drop);
    CHECK(d.why == W::NotInGameplay);
}

TEST_CASE("hand equip gate: a same-item re-press takes the selection over")
{
    constexpr std::uint32_t kFireball = 0x0001C789;
    constexpr std::uint32_t kFirebolt = 0x0012FD19;
    CHECK(OnReplaced(kFireball, true, kFireball) == ReplacedSelection::HandOver);
    CHECK(OnReplaced(kFireball, true, kFirebolt) == ReplacedSelection::Withdraw);
    CHECK(OnReplaced(kFireball, false, kFirebolt) == ReplacedSelection::None);
    CHECK(OnReplaced(kFireball, false, kFireball) == ReplacedSelection::None);
}

TEST_CASE("hand equip gate: the caster states are named")
{
    CHECK(CastPhaseName(kIdle).empty());
    CHECK(CasterStateName(kIdle) == "idle");
    CHECK(CastPhaseName(kReady) == "charged, held");
    CHECK(CastPhaseName(kReleased) == "just released");
    CHECK(CastPhaseName(7) == "unnamed state 7");
    CHECK(CastPhaseName(8) == "interrupt");
    CHECK(CastPhaseName(9) == "interrupt/deselect");
    CHECK(CastPhaseName(42) == "unknown state");
    CHECK(HandMaskName(kHandRight) == "right");
    CHECK(HandMaskName(kHandsBoth) == "both");
    CHECK(HandMaskName(0) == "none");
}
