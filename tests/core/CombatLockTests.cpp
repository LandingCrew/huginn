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
    in.newSameItem = true;   // still the same item: nothing ended
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
}

TEST_CASE("combat lock: an override the key already shows holds; one marking its normal item re-locks it")
{
    auto in = Frozen();
    in.newIsOverrideForThisKey = true;
    in.overridePriority = 70;
    in.newSameItem = true;
    in.lockedIsPinned = true;    // locked on the override already
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Hold);
    in.lockedIsPinned = false;   // locked on the same potion as an ordinary pick
    CHECK(DecideCombatLockBreak(in) == CombatLockBreak::Override);
}
