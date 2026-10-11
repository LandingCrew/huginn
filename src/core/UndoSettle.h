#pragma once

// The Swap Back undo settle: one change on the key, after the state caught up.
//
// Pressing a "Swap Back" key re-equips the remembered item and ends the hold
// (Remembrance). The first fill after that is made against state that has not
// caught up with the swap: the 2026-09-28 Skooma case (16:23:41-44) locked
// Skooma on the key for 3 s while the arrows it should have shown were still
// flagged equipped. So the key used to follow the allocator unlocked for the
// settle time (1 s) -- and in combat it changed two or three times in that
// second: 2026-10-11 12:45:03.1-03.6, key 1 went Circle of Strength -> Staff of
// the Flame Atronach -> Raw Canine Meat in 0.45 s (also 12:54:53, 12:55:15,
// 12:56:20, 13:01:40).
//
// Now the key holds what it showed at the undo -- as an ordinary item, so the
// "Swap Back" label goes on the next run as before -- for the settle time
// (SlotLocker::SettleAfterUndo), then fills once and locks that fill, even if
// it is the same item; in combat that is a combat lock. An urgent override or
// a new Remembrance hold on the key still takes it during the settle (the
// normal lock's rules). The same everywhere, in combat or not. The cost: the
// just-equipped item stays up, unlabelled, for the settle (1 s) instead of a
// stale pick changing under the player's finger.
//
// Pure: standard library only, host-tested in tests/core/UndoSettleTests.cpp.

#include <cstdint>

namespace Huginn::Core
{
    enum class UndoSettleStart : uint8_t
    {
        None,      ///< nothing to do: already settling, or the key is unrelated
        Hold,      ///< hold what the key shows, as an ordinary item, for the settle
        Release,   ///< the key's lock holds the undone item without showing it: let go
    };

    struct UndoSettleStartInput
    {
        bool settling = false;        ///< the key is settling already (a second report of one undo)
        bool showsUndone = false;     ///< the key shows the undone item right now
        bool lockedOnUndone = false;  ///< the key's lock holds the undone item
    };

    /// Hold replaces whatever lock the key has, a Sticky activation lock (10 s)
    /// included: Wheeler takes that lock just before it reports the undo, and
    /// the old per-tick release let go of it; kept, the undone item would stay
    /// up for 10 s.
    [[nodiscard]] constexpr UndoSettleStart DecideUndoSettleStart(const UndoSettleStartInput& in) noexcept
    {
        if (in.settling) return UndoSettleStart::None;
        if (in.showsUndone) return UndoSettleStart::Hold;
        if (in.lockedOnUndone) return UndoSettleStart::Release;
        return UndoSettleStart::None;
    }

    /// Whether this run locks an unlocked key's new fill: the normal rule
    /// (`shouldLock`, a change or a fill from empty), or the first fill after a
    /// settle ended (`afterSettle`), which locks even when it is the same item.
    /// Never an empty key, never with locking off.
    [[nodiscard]] constexpr bool LockThisFill(bool shouldLock, bool afterSettle, bool fillEmpty,
        bool lockingEnabled) noexcept
    {
        return !fillEmpty && lockingEnabled && (shouldLock || afterSettle);
    }
}
