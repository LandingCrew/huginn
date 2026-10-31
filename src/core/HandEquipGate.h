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
// the hands' magic casters on the main thread: both hands unless it is a
// right-hand equip that cannot move the left (HandsToCheck). One pending
// equip (a new press replaces it), checked every frame:
//   - a checked hand in an active state (1-6: starting, charging, charged
//     and held, releasing, casting) holds the equip until it is idle -- the
//     player letting go of the cast is what lets the swap through. Still
//     active kActiveWaitMaxMs after it was parked and the equip is DROPPED
//     (never interrupted: 0.24.26's first cut called InterruptCast at 1 s,
//     which is the 00:38 sequence again -- charge cancelled and refunded,
//     caster idle without a release -- split over two frames);
//   - a hand in a transient state (7-9, CommonLib's "Interrupt" and
//     "Interrupt/Deselect", or any state this does not know) is waited on
//     for at most kTransientWaitMs, then the equip goes through it: whether
//     one of them can rest there is not known, and a resting state treated
//     as busy would make every press wait;
//   - idle (0): equip now.
// Outside gameplay (a load, the main menu, a pausing menu, no player 3D) the
// pending equip is dropped.
//
// This header is the decision only: the caster states in, one action out
// (HandEquipWait). The game layer (input/HandEquipGate.cpp) reads the
// casters and equips. Milliseconds on one monotonic clock (the caller's;
// nothing here reads a clock).
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

    /// Active: part of a cast the player is making (starting, charging,
    /// charged and held, just released, casting). The equip waits for these.
    [[nodiscard]] constexpr bool CasterActive(std::uint32_t state) noexcept { return state >= 1 && state <= 6; }

    /// Transient: not idle and not active -- 7-9 (interrupt, interrupt and
    /// deselect, unnamed) or a value this does not know. Waited on briefly.
    [[nodiscard]] constexpr bool CasterTransient(std::uint32_t state) noexcept
    {
        return state != 0 && !CasterActive(state);
    }

    /// The hands an equip needs, as a mask.
    inline constexpr std::uint8_t kHandRight = 1;
    inline constexpr std::uint8_t kHandLeft = 2;
    inline constexpr std::uint8_t kHandsBoth = kHandRight | kHandLeft;

    /// Which hand the key asked for: tap, double tap, hold. A torch is Left:
    /// it always goes to the left hand.
    enum class EquipTarget : std::uint8_t
    {
        Right,
        Left,
        Both,
    };

    /// The hands whose casters an equip must wait for: both whenever the
    /// equip can change the other hand's contents.
    ///   - `itemOccupiesBoth`: a two-handed weapon, a bow or crossbow, a
    ///     both-hands spell;
    ///   - a left-hand equip (or a torch) can take a right-hand two-hander
    ///     off both hands, so it checks both;
    ///   - `otherHandMoves`: for a right-hand equip, the caller saw the left
    ///     hand would change too -- it holds this same form (EquipWeapon
    ///     takes it off first), or either hand holds an item taking both.
    /// Only a right-hand equip that leaves the left hand alone reads the
    /// right hand alone.
    [[nodiscard]] constexpr std::uint8_t HandsToCheck(EquipTarget target, bool itemOccupiesBoth,
        bool otherHandMoves) noexcept
    {
        if (itemOccupiesBoth || otherHandMoves || target != EquipTarget::Right) return kHandsBoth;
        return kHandRight;
    }

    /// How long after it was parked an active cast may hold the equip before it
    /// is dropped. Remembrance's capture window starts at the equip when it
    /// was held (Remembrance::RearmCapture), so it does not bound this.
    inline constexpr double kActiveWaitMaxMs = 3000.0;
    /// How long a transient state (7-9) may hold the equip before it goes
    /// through it anyway.
    inline constexpr double kTransientWaitMs = 250.0;

    struct HandEquipGateInput
    {
        bool worldReady = false;           ///< no load or main menu, not paused, the player has 3D
        std::uint8_t hands = 0;            ///< HandsToCheck, this frame
        std::uint32_t rightState = 0;      ///< the right hand's caster state (0 = idle)
        std::uint32_t leftState = 0;       ///< the left hand's
        double nowMs = 0.0;
    };

    enum class HandEquipAction : std::uint8_t
    {
        EquipNow,  ///< the checked hands are idle, or only transient past kTransientWaitMs
        Wait,      ///< check again next frame
        Drop,      ///< not in gameplay, or still casting at kActiveWaitMaxMs
    };

    enum class HandEquipWhy : std::uint8_t
    {
        HandsIdle,         ///< EquipNow: every checked hand idle
        ThroughTransient,  ///< EquipNow: a checked hand still in 7-9 after kTransientWaitMs
        Casting,           ///< Wait: a checked hand active
        Transient,         ///< Wait: a checked hand transient, under kTransientWaitMs
        NotInGameplay,     ///< Drop
        StillCasting,      ///< Drop: active at kActiveWaitMaxMs
    };

    struct HandEquipDecision
    {
        HandEquipAction action = HandEquipAction::Wait;
        HandEquipWhy why = HandEquipWhy::Casting;
        std::uint8_t busyHands = 0;     ///< checked hands not idle this frame
        std::uint8_t activeHands = 0;   ///< of those, the active ones (1-6)
    };

    /// One pending equip's wait: parked at `parkedMs`, then Update every
    /// frame. Holds the one thing a frame cannot see: since when the checked
    /// hands have been transient (and nothing active).
    class HandEquipWait
    {
    public:
        explicit HandEquipWait(double parkedMs = 0.0) noexcept : m_parkedMs(parkedMs) {}

        [[nodiscard]] HandEquipDecision Update(const HandEquipGateInput& in) noexcept
        {
            HandEquipDecision out;
            if (!in.worldReady) {
                out.action = HandEquipAction::Drop;
                out.why = HandEquipWhy::NotInGameplay;
                return out;
            }
            auto note = [&](std::uint8_t hand, std::uint32_t state) {
                if (!(in.hands & hand) || state == 0) return;
                out.busyHands |= hand;
                if (CasterActive(state)) out.activeHands |= hand;
            };
            note(kHandRight, in.rightState);
            note(kHandLeft, in.leftState);

            if (out.activeHands != 0) {
                m_transientSinceMs = -1.0;
                const bool timedOut = in.nowMs - m_parkedMs >= kActiveWaitMaxMs;
                out.action = timedOut ? HandEquipAction::Drop : HandEquipAction::Wait;
                out.why = timedOut ? HandEquipWhy::StillCasting : HandEquipWhy::Casting;
                return out;
            }
            if (out.busyHands != 0) {
                if (m_transientSinceMs < 0.0) m_transientSinceMs = in.nowMs;
                const bool through = in.nowMs - m_transientSinceMs >= kTransientWaitMs;
                out.action = through ? HandEquipAction::EquipNow : HandEquipAction::Wait;
                out.why = through ? HandEquipWhy::ThroughTransient : HandEquipWhy::Transient;
                return out;
            }
            m_transientSinceMs = -1.0;
            out.action = HandEquipAction::EquipNow;
            out.why = HandEquipWhy::HandsIdle;
            return out;
        }

        [[nodiscard]] double WaitedMs(double nowMs) const noexcept { return nowMs - m_parkedMs; }

    private:
        double m_parkedMs = 0.0;
        double m_transientSinceMs = -1.0;
    };

    /// A pending equip replaced by a newer press: what happens to the
    /// selection the first press opened (SelectionTracker keeps one record
    /// per form). The same form: the second press found the record already
    /// pending and opened none of its own, so it takes the record over --
    /// withdrawing it would leave the equip that lands with no selection at
    /// all. Another form: the first item was never equipped, so withdrawn.
    enum class ReplacedSelection : std::uint8_t
    {
        None,      ///< the first press opened no selection
        HandOver,  ///< the newer press takes it
        Withdraw,
    };

    [[nodiscard]] constexpr ReplacedSelection OnReplaced(std::uint32_t oldFormID, bool oldSelected,
        std::uint32_t newFormID) noexcept
    {
        if (!oldSelected) return ReplacedSelection::None;
        return oldFormID == newFormID ? ReplacedSelection::HandOver : ReplacedSelection::Withdraw;
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
