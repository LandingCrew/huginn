#pragma once

// Press grace: the misclick guard for a hotkey pressed just as its slot changed.
//
// A slot that changes under the player's finger turns a deliberate press into
// a misclick. In a LoreRim fight (2026-10-11 08:57:16) key 4 went down while it
// showed Staff of the Flame Atronach; the slot changed to Scimitar 0.27 s later,
// while the tap waited out the double-tap window, and the press equipped the
// Scimitar, on screen for 0.11 s. The learner then booked it as a choice.
//
// The rule: if the slot's shown item changed less than graceMs before the press
// began (or after it began), the press acts on the item shown before that
// change -- provided that item is still usable. Exceptions, in order:
//   - the key shows an OVERRIDE (critical health, drowning, magicka, stamina):
//     the urgent item arriving under the player's finger is exactly what the
//     override is for, and acting on the item it displaced would undo it.
//     Grace back TO an override that just left is fine;
//   - nothing was shown before (an empty key that filled): act on what is shown;
//   - the change was the same item (a stack swapped for its twin);
//   - the player's own press on this key caused or followed the change: they
//     acted on this key while the previous item was up, at most kOwnPressWindowMs
//     before the change. That is a Remembrance swap-back appearing, or the
//     player pressing the key again after using it, and pressing again must act
//     on what is shown now (the swap-back);
//   - the previous item is no longer usable (gone, unaffordable, on cooldown).
//
// Times are milliseconds on one monotonic clock; the epoch does not matter.
// Pure: standard library only, host-tested in tests/core/PressGraceTests.cpp.

#include <cstdint>

namespace Huginn::Core
{
    /// How long after the player's own press a change on that key still counts
    /// as caused by it. A Remembrance hold shows up on the first pipeline run
    /// after the equip lands -- tens to a few hundred ms (a weapon hand swap
    /// waits up to two frames first) -- so 2 s is generous. Erring long is the
    /// safe side: a change it wrongly attributes to the press simply gets no
    /// grace, which is the behaviour before grace existed.
    inline constexpr double kOwnPressWindowMs = 2000.0;

    struct PressGraceInput
    {
        double graceMs = 0.0;            ///< fPressGraceMs; <= 0 turns grace off
        double pressMs = 0.0;            ///< when the press began (key down of the gesture)
        bool hasChange = false;          ///< the key changed at least once since its page went up
        double changedAtMs = 0.0;        ///< when the key's shown item last changed
        double previousSinceMs = 0.0;    ///< when the item shown before that change went up
        bool previousEmpty = true;       ///< the key showed nothing before the change
        bool previousSameItem = false;   ///< the previous item is the shown item (same form and stack)
        bool hasLastAction = false;      ///< the player has acted on this key before
        double lastActionMs = 0.0;       ///< when the player last acted on this key
        bool previousUsable = false;     ///< the previous item can still be used (caller's check)
        bool shownIsOverride = false;    ///< the key shows an override's item now
    };

    enum class PressGraceReason : uint8_t
    {
        Off,               ///< graceMs <= 0
        ShownOverride,     ///< the key shows an override: never taken back off it
        NoChange,          ///< the key has not changed since its page went up
        Settled,           ///< the change is older than graceMs: act on what is shown
        PreviousEmpty,     ///< nothing was shown before the change
        SameItem,          ///< the change did not change the item
        OwnPress,          ///< the player's own press on this key explains the change
        PreviousUnusable,  ///< the previous item can no longer be used
        Grace,             ///< act on the previous item
    };

    struct PressGraceDecision
    {
        bool usePrevious = false;
        PressGraceReason reason = PressGraceReason::Off;
        /// How long the shown item had been up when the press began; negative
        /// when the change landed after the press began. Meaningful only when
        /// hasChange.
        double shownForMs = 0.0;
    };

    [[nodiscard]] constexpr PressGraceDecision DecidePressGrace(const PressGraceInput& in) noexcept
    {
        PressGraceDecision out;
        if (!(in.graceMs > 0.0)) {
            out.reason = PressGraceReason::Off;
            return out;
        }
        if (in.shownIsOverride) {
            out.reason = PressGraceReason::ShownOverride;
            return out;
        }
        if (!in.hasChange) {
            out.reason = PressGraceReason::NoChange;
            return out;
        }
        out.shownForMs = in.pressMs - in.changedAtMs;
        if (out.shownForMs >= in.graceMs) {
            out.reason = PressGraceReason::Settled;
            return out;
        }
        if (in.previousEmpty) {
            out.reason = PressGraceReason::PreviousEmpty;
            return out;
        }
        if (in.previousSameItem) {
            out.reason = PressGraceReason::SameItem;
            return out;
        }
        if (in.hasLastAction && in.lastActionMs >= in.previousSinceMs &&
            in.lastActionMs <= in.changedAtMs && in.changedAtMs - in.lastActionMs <= kOwnPressWindowMs) {
            out.reason = PressGraceReason::OwnPress;
            return out;
        }
        if (!in.previousUsable) {
            out.reason = PressGraceReason::PreviousUnusable;
            return out;
        }
        out.usePrevious = true;
        out.reason = PressGraceReason::Grace;
        return out;
    }

    [[nodiscard]] constexpr const char* PressGraceReasonName(PressGraceReason r) noexcept
    {
        switch (r) {
        case PressGraceReason::Off:              return "off";
        case PressGraceReason::ShownOverride:    return "shown override";
        case PressGraceReason::NoChange:         return "no change";
        case PressGraceReason::Settled:          return "settled";
        case PressGraceReason::PreviousEmpty:    return "previous empty";
        case PressGraceReason::SameItem:         return "same item";
        case PressGraceReason::OwnPress:         return "own press";
        case PressGraceReason::PreviousUnusable: return "previous unusable";
        case PressGraceReason::Grace:            return "grace";
        }
        return "?";
    }
}
