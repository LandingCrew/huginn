#pragma once

// =============================================================================
// HAND EQUIP GATE -- a slot key waits for a busy hand (0.24.26)
// =============================================================================
// 2026-10-31 00:38 (LoreRim): a Huginn key swapped Calm Touch into the right
// hand while the player held a charged Fireball there. The tap fired 300 ms
// after key-up, 0.6 s into the charge; the charge was cancelled (the magicka
// came back), the caster dropped to idle without a release, and from then on
// the behaviour graph stopped following equips (iRightHandType stayed 9,
// weapons stayed on their sheath node) until the game froze. Wheeler equips
// with time slowed and cast input filtered, so a live charge is rare there;
// a number key is pressed mid-fight with the cast button down.
//
// So an equip into a hand (spell, scroll, weapon, staff, torch) first reads
// that hand's magic caster on the main thread -- both hands for a two-hander,
// bow, crossbow, a both-hands spell or the hold gesture. Anything but idle
// and the equip waits: one pending equip (a new press replaces it), checked
// every frame, done as soon as the hand is idle. Still busy after
// kHandBusyTimeoutMs and the cast is interrupted, then the equip goes on the
// next frame whatever the caster says. Outside gameplay (a load, the main
// menu, a pausing menu, no player 3D) the pending equip is dropped.
//
// This header is the decision only: the caster states in, one action out.
// The game layer (input/HandEquipGate.cpp) reads the casters, calls
// InterruptCast and equips. Milliseconds on one monotonic clock (the
// caller's; nothing here reads a clock).
//
// The caster states are the engine's MagicCaster::State values, passed as
// integers; the names below are CommonLib's guesses where it has them,
// corrected by the logs where they disagree (kUnk04).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>
#include <string_view>

namespace Huginn::Core
{
    /// The caster's state while it is part of a cast, or "" when idle (0).
    /// A charged fire-and-forget spell held in the hand sits in 3 (kReady),
    /// not 5 (kCharging): counting only charging and casting missed the swap
    /// made from the wheel with a charged spell still held (2026-10-18).
    [[nodiscard]] constexpr std::string_view CastPhaseName(std::uint32_t state) noexcept
    {
        switch (state) {
        case 0: return {};
        case 1: return "starting";
        case 2: return "starting a charge";
        case 3: return "charged, held";
        // 4 (kUnk04): CommonLib v3.7.0 guesses "PreStart?" (MagicCaster.h:35),
        // and this read "about to start" until 0.23.50. The LoreRim log of
        // 2026-10-28 puts it after the release: the player charged Conjure
        // Skeletal Hero from 13:27:50.5 (magicka 95 -> 19 by 13:27:51.4, the
        // charge done), the last non-idle state the probe saw was kUnk04 at
        // ~13:27:52.56 (a swap at 13:27:58.359 logged "last cast 5.8 s
        // before"), the summon effect was up at 13:27:52.684 and the skeleton
        // appeared at 13:27:52.803. All three swaps of that session logged
        // kUnk04 as the last cast state (two after that cast, one 0.7 s after
        // Circle of Strength). So it is the release, between letting go and
        // the spell landing; the name is still a reading of the log, not of
        // the engine.
        case 4: return "just released";
        case 5: return "charging";
        case 6: return "casting";
        // 7-9: CommonLib names 8 "Interrupt" and 9 "Interrupt/Deselect", and 7
        // not at all. Until 0.24.26 the probe counted all three as idle, so an
        // interrupted charge (the 00:38 Fireball went from "charged, held"
        // straight to "idle") could not be told from a finished one.
        case 7: return "unnamed state 7";
        case 8: return "interrupt";
        case 9: return "interrupt/deselect";
        default: return "unknown state";
        }
    }

    /// The caster's state for a log line: "idle" for 0, CastPhaseName otherwise.
    [[nodiscard]] constexpr std::string_view CasterStateName(std::uint32_t state) noexcept
    {
        return state == 0 ? std::string_view("idle") : CastPhaseName(state);
    }

    /// Busy is anything but idle: every state 1-9 is part of a cast starting,
    /// charging, held, releasing or being interrupted.
    [[nodiscard]] constexpr bool CasterBusy(std::uint32_t state) noexcept { return state != 0; }

    /// The hands an equip needs, as a mask.
    inline constexpr std::uint8_t kHandRight = 1;
    inline constexpr std::uint8_t kHandLeft = 2;
    inline constexpr std::uint8_t kHandsBoth = kHandRight | kHandLeft;

    /// Which hand the key asked for: tap, double tap, hold.
    enum class EquipTarget : std::uint8_t
    {
        Right,
        Left,
        Both,
    };

    /// The hands whose casters an equip must wait for. `occupiesBoth`: the
    /// item takes both hands whatever the gesture (a two-handed weapon, a bow
    /// or crossbow, a both-hands spell). A torch is the caller's Left: it
    /// always goes to the left hand.
    [[nodiscard]] constexpr std::uint8_t HandsToCheck(EquipTarget target, bool occupiesBoth) noexcept
    {
        if (occupiesBoth || target == EquipTarget::Both) return kHandsBoth;
        return target == EquipTarget::Left ? kHandLeft : kHandRight;
    }

    /// How long a pending equip waits for a busy hand before the cast is
    /// interrupted. Kept well under Remembrance's capture window (1500 ms,
    /// slot/Remembrance.h kMatchWindowMs), which starts at the press: the
    /// equip must land inside it for the press to hold what it displaced.
    inline constexpr double kHandBusyTimeoutMs = 1000.0;

    struct HandEquipGateInput
    {
        bool worldReady = false;           ///< no load or main menu, not paused, the player has 3D
        std::uint8_t hands = 0;            ///< HandsToCheck
        std::uint32_t rightState = 0;      ///< the right hand's caster state (0 = idle)
        std::uint32_t leftState = 0;       ///< the left hand's
        double waitedMs = 0.0;             ///< since the equip was parked
        bool interrupted = false;          ///< the cast was interrupted on an earlier frame
        double timeoutMs = kHandBusyTimeoutMs;
    };

    enum class HandEquipAction : std::uint8_t
    {
        EquipNow,            ///< the hands are idle (or the cast was interrupted last frame)
        Wait,                ///< a hand is busy: check again next frame
        InterruptThenEquip,  ///< busy past the timeout: interrupt now, equip next frame
        Drop,                ///< not in gameplay: forget the equip
    };

    struct HandEquipDecision
    {
        HandEquipAction action = HandEquipAction::Wait;
        std::uint8_t busyHands = 0;   ///< the needed hands whose caster is busy now
    };

    [[nodiscard]] constexpr HandEquipDecision DecideHandEquip(const HandEquipGateInput& in) noexcept
    {
        HandEquipDecision out;
        if (!in.worldReady) {
            out.action = HandEquipAction::Drop;
            return out;
        }
        if ((in.hands & kHandRight) && CasterBusy(in.rightState)) out.busyHands |= kHandRight;
        if ((in.hands & kHandLeft) && CasterBusy(in.leftState)) out.busyHands |= kHandLeft;
        // The frame after the interrupt the equip goes whatever the caster
        // reads: an interrupt that leaves a state behind (7-9) must not hold
        // the key forever.
        if (in.interrupted || out.busyHands == 0) {
            out.action = HandEquipAction::EquipNow;
            return out;
        }
        out.action = in.waitedMs >= in.timeoutMs ? HandEquipAction::InterruptThenEquip : HandEquipAction::Wait;
        return out;
    }

    [[nodiscard]] constexpr std::string_view HandEquipActionName(HandEquipAction a) noexcept
    {
        switch (a) {
        case HandEquipAction::EquipNow: return "equip now";
        case HandEquipAction::Wait: return "wait";
        case HandEquipAction::InterruptThenEquip: return "interrupt, then equip";
        case HandEquipAction::Drop: return "drop";
        }
        return "?";
    }

    /// "right", "left", "both" or "none", for a hand mask.
    [[nodiscard]] constexpr std::string_view HandMaskName(std::uint8_t hands) noexcept
    {
        switch (hands & kHandsBoth) {
        case kHandRight: return "right";
        case kHandLeft: return "left";
        case kHandsBoth: return "both";
        default: return "none";
        }
    }
}
