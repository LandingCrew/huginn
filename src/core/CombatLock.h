#pragma once

// The combat lock (fCombatLockDurationMs): while the player is in combat a
// key's lock lasts this long instead of fLockDurationMs, and -1 freezes the key
// until combat ends.
//
// The user, 2026-10-11, after a fight that changed page 0 117 times in 140 s
// ("combat probably needs a longer lock or freeze; make it an ini setting").
// Every change re-locks its key for 3 s, but eight keys coming free at
// different times changed the page about every half second.
//
// Under a combat lock the challenger margin never reaches a locked key: only
// these change it --
//   - its item is gone (consumed to the last one, dropped, sold): the lock is
//     released at the source (SlotLocker::OnItemUsed with UseKind::Gone), so it
//     never gets here;
//   - an override placed on THAT key (any priority; below
//     iImmediateBreakPriority it waits fMinLockDurationMs, as a normal lock's
//     high-priority break does not);
//   - a Remembrance hold shown on the key the player pressed;
//   - the end of the override or Remembrance hold the key itself is showing
//     (its own lifecycle: a frozen "Swap Back" for the rest of the fight would
//     be wrong).
// A spell the affordability filter drops stays on its locked key -- the lock
// re-shows it whatever the candidate list says -- instead of leaving and coming
// back with the magicka.
//
// When combat ends, locks taken under the combat duration fall back to the
// normal timer, counted from the end of combat (no re-shuffle on the last kill).
//
// Pure: standard library only, host-tested in tests/core/CombatLockTests.cpp.

#include <algorithm>
#include <cstdint>

namespace Huginn::Core
{
    /// fCombatLockDurationMs = -1: a combat lock never expires on its own.
    inline constexpr float kCombatLockFreeze = -1.0f;

    struct LockDuration
    {
        float ms = 0.0f;       ///< the timer a new lock gets (unused while frozen)
        bool frozen = false;   ///< no timer: held until combat ends
        bool combat = false;   ///< taken under the combat setting
    };

    /// The lock a key gets right now. combatMs: -1 freeze, 0 = the normal lock,
    /// > 0 that many ms. Outside combat, always the normal lock.
    [[nodiscard]] constexpr LockDuration EffectiveLockDuration(bool inCombat, float normalMs, float combatMs) noexcept
    {
        if (!inCombat || combatMs == 0.0f) {
            return { normalMs, false, false };
        }
        if (combatMs < 0.0f) {
            return { normalMs, true, true };
        }
        return { combatMs, false, true };
    }

    /// Whether locking happens at all: the normal lock can be off (0) while the
    /// combat lock is on.
    [[nodiscard]] constexpr bool LockingEnabled(const LockDuration& d) noexcept
    {
        return d.frozen || d.ms > 0.0f;
    }

    /// A combat lock's remaining time once combat ends: the normal timer from
    /// now, or what is left if that is shorter. 0 (or less) = unlock now --
    /// normal locking is off.
    [[nodiscard]] constexpr float RemainingAfterCombat(float remainingMs, bool frozen, float normalMs) noexcept
    {
        if (normalMs <= 0.0f) {
            return 0.0f;
        }
        return frozen ? normalMs : std::min(remainingMs, normalMs);
    }

    enum class CombatLockBreak : uint8_t
    {
        Hold,          ///< keep the locked item
        Expired,       ///< a combat duration (not a freeze) ran out
        Override,      ///< an override placed on this key
        Remembrance,   ///< a Remembrance hold on the key the player pressed
        PinnedEnded,   ///< the override or hold this key showed is over
    };

    struct CombatLockBreakInput
    {
        bool frozen = false;
        float remainingMs = 0.0f;
        float elapsedMs = 0.0f;              ///< since the lock was taken
        float minLockMs = 0.0f;              ///< fMinLockDurationMs
        bool overridesBreakLock = true;      ///< bOverridesBreakLock
        int immediateBreakPriority = 50;     ///< iImmediateBreakPriority
        bool newIsOverrideForThisKey = false;  ///< the new assignment is an active override's item
        int overridePriority = 0;            ///< that override's priority
        bool newIsRemembered = false;
        bool remembranceOnPressedKey = false;  ///< this key is the one the player pressed for it
        bool newSameItem = false;            ///< the new assignment is the locked item
        bool lockedIsPinned = false;         ///< the locked item is an override or Remembrance item
    };

    [[nodiscard]] constexpr CombatLockBreak DecideCombatLockBreak(const CombatLockBreakInput& in) noexcept
    {
        if (!in.frozen && in.remainingMs <= 0.0f) {
            return CombatLockBreak::Expired;
        }
        // The override first: one marking the item the key already shows
        // re-locks it AS the override (same item, so nothing visible moves) --
        // once: a key already locked on that override item just holds.
        if (in.newIsOverrideForThisKey && in.overridesBreakLock &&
            !(in.newSameItem && in.lockedIsPinned) &&
            (in.overridePriority >= in.immediateBreakPriority || in.elapsedMs >= in.minLockMs)) {
            return CombatLockBreak::Override;
        }
        if (in.newSameItem) {
            return CombatLockBreak::Hold;
        }
        if (in.newIsRemembered && in.remembranceOnPressedKey) {
            return CombatLockBreak::Remembrance;
        }
        if (in.lockedIsPinned) {
            return CombatLockBreak::PinnedEnded;
        }
        return CombatLockBreak::Hold;
    }
}
