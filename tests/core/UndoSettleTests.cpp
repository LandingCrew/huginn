// Host tests for src/core/UndoSettle.h.

#include "core/UndoSettle.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using namespace Huginn::Core;

TEST_CASE("undo settle: the key showing the undone item holds it")
{
    UndoSettleStartInput in;
    in.showsUndone = true;
    in.lockedOnUndone = true;
    CHECK(DecideUndoSettleStart(in) == UndoSettleStart::Hold);
    in.lockedOnUndone = false;   // shown unlocked (locking off): held all the same
    CHECK(DecideUndoSettleStart(in) == UndoSettleStart::Hold);
}

TEST_CASE("undo settle: a second report of the same undo changes nothing")
{
    // Wheeler: Update sees the item back in hand and settles the key; the
    // activation callback arrives after. It must not restart the settle.
    UndoSettleStartInput in;
    in.settling = true;
    in.showsUndone = true;
    in.lockedOnUndone = true;
    CHECK(DecideUndoSettleStart(in) == UndoSettleStart::None);
}

TEST_CASE("undo settle: whatever lock the key has is replaced, a Sticky activation lock included")
{
    // Wheeler under Sticky takes the 10 s activation lock just before it
    // reports the undo; the settle replaces it (1 s, then one fill). The
    // input carries no lock kind: no lock is exempt.
    UndoSettleStartInput in;
    in.showsUndone = true;
    in.lockedOnUndone = true;
    CHECK(DecideUndoSettleStart(in) == UndoSettleStart::Hold);
}

TEST_CASE("undo settle: a lock on the item that does not show it lets go; an unrelated key is left alone")
{
    UndoSettleStartInput in;
    in.lockedOnUndone = true;
    CHECK(DecideUndoSettleStart(in) == UndoSettleStart::Release);
    in.lockedOnUndone = false;
    CHECK(DecideUndoSettleStart(in) == UndoSettleStart::None);
}

TEST_CASE("undo settle: the first fill after it locks, even the same item")
{
    CHECK(LockThisFill(false, true, false, true));    // same item as before: locked anyway
    CHECK(LockThisFill(true, true, false, true));
    CHECK(LockThisFill(true, false, false, true));    // the normal rule still applies
    CHECK_FALSE(LockThisFill(false, false, false, true));
    CHECK_FALSE(LockThisFill(false, true, true, true));   // never an empty key
    CHECK_FALSE(LockThisFill(true, true, false, false));  // never with locking off
}

namespace
{
    // One key through an undo, modelled the way SlotLocker drives it: Hold
    // starts a settle lock that re-shows the held item; when the settle time
    // runs out the key is unlocked with lockNextFill; the next run's fill is
    // locked through LockThisFill; a locked key re-shows its item. Returns
    // what the key showed on each run.
    struct KeyModel
    {
        uint32_t shown = 0;
        bool locked = false;
        bool settling = false;
        bool lockNextFill = false;
        float remainingMs = 0.0f;

        void Undo(uint32_t undone, float settleMs)
        {
            UndoSettleStartInput in;
            in.settling = locked && settling;
            in.showsUndone = shown == undone;
            in.lockedOnUndone = locked && shown == undone;
            if (DecideUndoSettleStart(in) == UndoSettleStart::Hold) {
                locked = true;
                settling = true;
                lockNextFill = false;
                remainingMs = settleMs;
            }
        }

        void Tick(float dtMs)
        {
            if (locked && remainingMs > 0.0f) {
                remainingMs -= dtMs;
                if (remainingMs <= 0.0f) {
                    locked = false;
                    if (settling) {
                        settling = false;
                        lockNextFill = true;
                    }
                }
            }
        }

        void Run(uint32_t allocatorPick, float lockMs)
        {
            if (locked) return;   // re-shows its item
            const bool afterSettle = lockNextFill;
            lockNextFill = false;
            const bool shouldLock = allocatorPick != 0 && allocatorPick != shown;
            if (LockThisFill(shouldLock, afterSettle, allocatorPick == 0, true)) {
                locked = true;
                remainingMs = lockMs;
            }
            shown = allocatorPick;
        }
    };
}

TEST_CASE("undo settle: an allocator flipping every run during the settle makes one change")
{
    // 2026-10-11 12:45:03: after the undo of Circle of Strength the allocator
    // offered Staff of the Flame Atronach, then Raw Canine Meat; the key showed
    // both. Here the allocator flips every 100 ms run for 1.5 s.
    constexpr uint32_t kCircle = 1, kStaff = 2, kMeat = 3, kWard = 4;
    KeyModel key;
    key.shown = kCircle;    // the Swap Back item
    key.locked = true;
    key.remainingMs = 3000.0f;
    key.Undo(kCircle, 1000.0f);

    const std::vector<uint32_t> picks{ kStaff, kMeat, kStaff, kMeat, kWard, kStaff, kMeat, kStaff,
                                       kMeat, kWard, kWard, kStaff, kMeat, kStaff, kMeat };
    int changes = 0;
    uint32_t last = key.shown;
    for (const uint32_t pick : picks) {
        key.Tick(100.0f);
        key.Run(pick, 3000.0f);
        if (key.shown != last) {
            ++changes;
            last = key.shown;
        }
    }
    CHECK(changes == 1);
    CHECK(key.locked);   // locked on the one fill
    CHECK(key.shown == kWard);   // the run after the settle (10th tick) offered the Ward
}

TEST_CASE("undo settle: a fill that is the same item still ends up locked")
{
    constexpr uint32_t kCircle = 1;
    KeyModel key;
    key.shown = kCircle;
    key.locked = true;
    key.remainingMs = 3000.0f;
    key.Undo(kCircle, 1000.0f);
    for (int i = 0; i < 12; ++i) {
        key.Tick(100.0f);
        key.Run(kCircle, 3000.0f);
    }
    CHECK(key.shown == kCircle);
    CHECK(key.locked);
    CHECK_FALSE(key.lockNextFill);
}
