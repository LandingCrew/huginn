// Host tests for src/core/CombatLock.h.

#include "core/CombatLock.h"

#include <doctest/doctest.h>

using namespace Huginn::Core;

TEST_CASE("combat lock: duration out of combat is always the normal lock")
{
    const auto d = EffectiveLockDuration(false, 3000.0f, kCombatLockFreeze);
    CHECK(d.ms == 3000.0f);
    CHECK_FALSE(d.frozen);
    CHECK_FALSE(d.combat);
}

TEST_CASE("combat lock: -1 freezes, 0 uses the normal lock, > 0 is its own timer")
{
    const auto frozen = EffectiveLockDuration(true, 3000.0f, -1.0f);
    CHECK(frozen.frozen);
    CHECK(frozen.combat);
    const auto same = EffectiveLockDuration(true, 3000.0f, 0.0f);
    CHECK_FALSE(same.frozen);
    CHECK_FALSE(same.combat);
    CHECK(same.ms == 3000.0f);
    const auto longer = EffectiveLockDuration(true, 3000.0f, 20000.0f);
    CHECK_FALSE(longer.frozen);
    CHECK(longer.combat);
    CHECK(longer.ms == 20000.0f);
}

TEST_CASE("combat lock: locking can be on in combat with the normal lock off")
{
    CHECK_FALSE(LockingEnabled(EffectiveLockDuration(false, 0.0f, -1.0f)));
    CHECK(LockingEnabled(EffectiveLockDuration(true, 0.0f, -1.0f)));
    CHECK(LockingEnabled(EffectiveLockDuration(true, 0.0f, 5000.0f)));
    CHECK_FALSE(LockingEnabled(EffectiveLockDuration(true, 0.0f, 0.0f)));
}

TEST_CASE("combat lock: at combat end a lock falls back to the normal timer from now")
{
    CHECK(RemainingAfterCombat(0.0f, true, 3000.0f) == 3000.0f);        // a freeze gets the full timer
    CHECK(RemainingAfterCombat(15000.0f, false, 3000.0f) == 3000.0f);   // a long combat lock is cut to it
    CHECK(RemainingAfterCombat(1200.0f, false, 3000.0f) == 1200.0f);    // a shorter remainder is kept
    CHECK(RemainingAfterCombat(5000.0f, true, 0.0f) == 0.0f);           // normal locking off: unlock
}

namespace
{
    CombatLockBreakInput Frozen()
    {
        CombatLockBreakInput in;
        in.frozen = true;
        in.remainingMs = 3000.0f;
        in.elapsedMs = 10000.0f;
        in.minLockMs = 500.0f;
        in.overridesBreakLock = true;
        in.immediateBreakPriority = 50;
        return in;
    }
}

TEST_CASE("combat lock: a frozen key ignores ordinary challengers however long it has been up")
{
    auto in = Frozen();
    in.elapsedMs = 120000.0f;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
    in.remainingMs = 0.0f;   // a freeze has no timer to run out
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
}

TEST_CASE("combat lock: a timed combat lock expires")
{
    auto in = Frozen();
    in.frozen = false;
    in.remainingMs = 0.0f;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Expired);
    in.remainingMs = 1.0f;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
}

TEST_CASE("combat lock: an override on this key takes it; low priority after the minimum")
{
    auto in = Frozen();
    in.newIsOverrideForThisKey = true;
    in.overridePriority = 70;
    in.elapsedMs = 100.0f;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Override);
    in.overridePriority = 30;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
    in.elapsedMs = 600.0f;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Override);
    in.overridesBreakLock = false;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
}

TEST_CASE("combat lock: Remembrance only on the key the player pressed")
{
    auto in = Frozen();
    in.newIsRemembered = true;
    in.remembranceOnPressedKey = false;   // sRemembranceTarget = Job put it elsewhere
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
    in.remembranceOnPressedKey = true;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Remembrance);
}

TEST_CASE("combat lock: the end of the key's own override or hold releases it")
{
    auto in = Frozen();
    in.lockedIsPinned = true;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::PinnedEnded);
    in.newSameItem = true;   // still the same item AND still pinned: nothing ended
    in.newIsPinned = true;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
}

TEST_CASE("combat lock: an override that ends on an item still offered is restyled, not frozen as an override")
{
    // The potion stays on the key as an ordinary pick: same item, no longer
    // pinned. Holding the stored Override assignment kept the urgent styling,
    // the Wheeler override label and the [O] selection flag all fight.
    auto in = Frozen();
    in.lockedIsPinned = true;
    in.newSameItem = true;
    in.newIsPinned = false;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Restyle);
    // And the other way, when bOverridesBreakLock keeps the override branch out.
    in.lockedIsPinned = false;
    in.newIsPinned = true;
    in.overridesBreakLock = false;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Restyle);
}

TEST_CASE("combat lock: an urgent override takes a frozen key even with bOverridesBreakLock off")
{
    auto in = Frozen();
    in.newIsOverrideForThisKey = true;
    in.overridesBreakLock = false;
    in.overridePriority = 100;
    in.elapsedMs = 10.0f;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Override);
    in.overridePriority = 30;   // not urgent: the setting is honoured
    in.elapsedMs = 10000.0f;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
}

TEST_CASE("combat lock: an override the key already shows holds; one marking its normal item re-locks it")
{
    auto in = Frozen();
    in.newIsOverrideForThisKey = true;
    in.overridePriority = 70;
    in.newSameItem = true;
    in.lockedIsPinned = true;    // locked on the override already
    in.newIsPinned = true;
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
    in.lockedIsPinned = false;   // locked on the same potion as an ordinary pick
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Override);
}

TEST_CASE("combat lock: no enemy for releaseSec ends combat for the lock; an enemy brings it back")
{
    CombatPresence p;
    CHECK_FALSE(p.Update(false, false, 0.1f, 5.0f));
    CHECK(p.Update(true, true, 0.1f, 5.0f));
    // The last enemy dies; the game's flag stays up.
    float t = 0.0f;
    bool locked = true;
    while (t < 4.9f) { locked = p.Update(true, false, 0.1f, 5.0f); t += 0.1f; }
    CHECK(locked);
    for (int i = 0; i < 3; ++i) locked = p.Update(true, false, 0.1f, 5.0f);
    CHECK_FALSE(locked);
    // An enemy comes back while the flag is still up.
    CHECK(p.Update(true, true, 0.1f, 5.0f));
    // 0 = follow the flag alone.
    CombatPresence q;
    for (int i = 0; i < 1000; ++i) locked = q.Update(true, false, 0.1f, 0.0f);
    CHECK(locked);
    // The flag dropping ends it at once whatever the timer.
    CHECK_FALSE(q.Update(false, true, 0.1f, 0.0f));
}

TEST_CASE("combat lock: a frozen key is released when its spell is no longer known or its item is equipped")
{
    CHECK(DecideCombatLockItemRelease(true, false, false, false) == CombatLockItemRelease::Gone);
    CHECK(DecideCombatLockItemRelease(true, false, true, true) == CombatLockItemRelease::Gone);
    CHECK(DecideCombatLockItemRelease(false, true, false, false) == CombatLockItemRelease::Equipped);
    // A Remembrance/override item keeps its key; so does an item the
    // allocator still places there (equipped items shown on purpose).
    CHECK(DecideCombatLockItemRelease(false, true, true, false) == CombatLockItemRelease::None);
    CHECK(DecideCombatLockItemRelease(false, true, false, true) == CombatLockItemRelease::None);
    CHECK(DecideCombatLockItemRelease(false, false, false, false) == CombatLockItemRelease::None);
}

TEST_CASE("combat lock: a frozen key shows no countdown in the lock-timer label")
{
    CHECK(LockTimerLabel("Locked", true, 2400.0f, false) == "Locked 3s");
    CHECK(LockTimerLabel("Locked", true, 3000.0f, true) == "Locked");
    CHECK(LockTimerLabel("Locked", false, 2400.0f, false) == "Locked");
}

TEST_CASE("combat lock: the equipped check matches the stack when the key names one")
{
    CHECK(EquippedInstanceMatches(true, 0, -1));        // no stack named: the form is enough
    CHECK(EquippedInstanceMatches(true, 22, 22));       // this very dagger is in hand
    CHECK_FALSE(EquippedInstanceMatches(true, 22, 7));  // its twin is in hand, not this one
    CHECK_FALSE(EquippedInstanceMatches(true, 22, -1)); // worn stack unknown: do not release
    CHECK_FALSE(EquippedInstanceMatches(false, 0, -1));
}

TEST_CASE("combat lock: at onset a key on a normal lock becomes combat-locked on its item")
{
    // locked, alreadyCombat, holdsPinned, settling, empty
    CHECK(ConvertsAtCombatOnset(true, false, false, false, false));
    // The 12:59:32 case: Circle of Strength on its 3 s lock at onset expired
    // 1.7 s into the fight and became Sun Shards. Converted, it stays.
}

TEST_CASE("combat lock: at onset the exceptions keep what they have")
{
    CHECK_FALSE(ConvertsAtCombatOnset(false, false, false, false, false));  // unlocked: the sweep locks it
    CHECK_FALSE(ConvertsAtCombatOnset(true, true, false, false, false));    // already a combat lock
    CHECK_FALSE(ConvertsAtCombatOnset(true, false, true, false, false));    // override / Remembrance item
    CHECK_FALSE(ConvertsAtCombatOnset(true, false, false, true, false));    // Swap Back undo settle
    CHECK_FALSE(ConvertsAtCombatOnset(true, false, false, false, true));    // nothing on it
}
