#pragma once

// The combat lock (fCombatLockDurationMs): while the player is in combat a
// key's lock lasts this long instead of fLockDurationMs, and -1 freezes the key
// until combat ends.
//
// The user, 2026-10-11, after a fight that changed page 0 119 times in 140 s
// ([SlotChurn], 08:55:40-08:58:00; "combat probably needs a longer lock or
// freeze; make it an ini setting"). Every change re-locks its key for 3 s, but
// eight keys coming free at different times changed the page about every
// second.
//
// Under a combat lock the challenger margin never reaches a locked key: only
// these change it --
//   - its item is gone (consumed to the last one, dropped, sold): the lock is
//     released at the source (SlotLocker::OnItemUsed with UseKind::Gone), so it
//     never gets here; a spell or shout no longer known, or an item now
//     equipped with no hold on the key, is caught on the run
//     (DecideCombatLockItemRelease);
//   - an override placed on THAT key (any priority; below
//     iImmediateBreakPriority it waits fMinLockDurationMs, as a normal lock's
//     high-priority break does not; an urgent one ignores
//     bOverridesBreakLock = false, which under a freeze would hide it all fight);
//   - a Remembrance hold shown on the key the player pressed;
//   - the end of the override or Remembrance hold the key itself is showing
//     (its own lifecycle: a frozen "Swap Back" for the rest of the fight would
//     be wrong), and a same-item flip in or out of override (Restyle).
// "In combat" for the lock is the game's flag, released after
// fCombatNoEnemyReleaseSec with no enemy tracked (CombatPresence).
// A spell the affordability filter drops stays on its locked key -- the lock
// re-shows it whatever the candidate list says -- instead of leaving and coming
// back with the magicka.
//
// When combat ends, locks taken under the combat duration fall back to the
// normal timer, counted from the end of combat (no re-shuffle on the last kill).
//
// Pure: standard library only, host-tested in tests/core/CombatLockTests.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

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

    /// Whether the combat lock should treat the player as in combat. The game's
    /// combat flag can outlive the enemies by minutes (2026-10-11: Enemies
    /// One->None at 09:23:56, combat until 09:26:05), which would leave every
    /// key frozen with nothing around. So once no enemy has been tracked for
    /// `releaseSec` while the flag is up, the lock counts combat as over; an
    /// enemy coming back restores it. releaseSec <= 0 = follow the flag alone.
    struct CombatPresence
    {
        float noEnemySec = 0.0f;

        [[nodiscard]] bool Update(bool inCombat, bool enemyPresent, float dtSec, float releaseSec) noexcept
        {
            if (!inCombat || enemyPresent) {
                noEnemySec = 0.0f;
                return inCombat;
            }
            noEnemySec += dtSec;
            return releaseSec <= 0.0f || noEnemySec < releaseSec;
        }
    };

    /// A combat-locked key checked against the game on every run, not only on
    /// events: the item can stop being usable without an inventory event.
    enum class CombatLockItemRelease : uint8_t
    {
        None,
        Gone,       ///< a spell or shout the player no longer knows
        Equipped,   ///< the key's item is now in the player's hands / on them
    };

    /// `allocatorKeepsIt`: the allocator still places this item on this key
    /// this run (a slot that shows equipped items). Releasing then would only
    /// re-lock the same item every run, so an equipped item is released only
    /// when the allocator has moved on from it. A Remembrance or override item
    /// is not released for being equipped -- the hold owns that key.
    [[nodiscard]] constexpr CombatLockItemRelease DecideCombatLockItemRelease(
        bool itemGone, bool itemEquipped, bool lockedIsPinned, bool allocatorKeepsIt) noexcept
    {
        if (itemGone) {
            return CombatLockItemRelease::Gone;
        }
        if (itemEquipped && !lockedIsPinned && !allocatorKeepsIt) {
            return CombatLockItemRelease::Equipped;
        }
        return CombatLockItemRelease::None;
    }

    /// Is the key's item the one equipped? `formMatches`: the equipped object
    /// is this form. A key naming a stack (storedUid != 0) matches only that
    /// stack's worn uniqueID: two identical daggers, one in hand, must not
    /// release the key showing the other. wornUid < 0 = the worn stack has no
    /// uniqueID or could not be read.
    [[nodiscard]] constexpr bool EquippedInstanceMatches(bool formMatches, uint16_t storedUid, int wornUid) noexcept
    {
        if (!formMatches) return false;
        if (storedUid == 0) return true;
        return wornUid >= 0 && static_cast<uint16_t>(wornUid) == storedUid;
    }

    /// The Wheeler lock-timer subtext. A frozen key has no timer to show, so it
    /// gets the prefix alone (the "locked" marker) rather than a countdown that
    /// never moves.
    [[nodiscard]] inline std::string LockTimerLabel(std::string_view prefix, bool showSeconds,
        float remainingMs, bool frozen)
    {
        if (frozen || !showSeconds) {
            return std::string(prefix);
        }
        const int secs = static_cast<int>(std::ceil(remainingMs / 1000.0f));
        return std::string(prefix) + " " + std::to_string(secs) + "s";
    }

    enum class CombatLockBreak : uint8_t
    {
        Hold,          ///< keep the locked item
        Expired,       ///< a combat duration (not a freeze) ran out
        Override,      ///< an override placed on this key
        Remembrance,   ///< a Remembrance hold on the key the player pressed
        PinnedEnded,   ///< the override or hold this key showed is over
        Restyle,       ///< same item, but it stopped (or started) being an override /
                       ///< Remembrance item: re-lock it as what it is now, so an
                       ///< ended override does not keep its urgent styling all fight
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
        bool newIsPinned = false;            ///< the new assignment is an override or Remembrance item
    };

    [[nodiscard]] constexpr CombatLockBreak DecideCombatLockBreak(const CombatLockBreakInput& in) noexcept
    {
        if (!in.frozen && in.remainingMs <= 0.0f) {
            return CombatLockBreak::Expired;
        }
        // The override first: one marking the item the key already shows
        // re-locks it AS the override (same item, so nothing visible moves) --
        // once: a key already locked on that override item just holds.
        // bOverridesBreakLock = false does not stop an URGENT override
        // (>= iImmediateBreakPriority) under a combat lock: with a freeze it
        // would hide the critical-health potion for the whole fight.
        const bool urgent = in.overridePriority >= in.immediateBreakPriority;
        if (in.newIsOverrideForThisKey && (in.overridesBreakLock || urgent) &&
            !(in.newSameItem && in.lockedIsPinned) &&
            (urgent || in.elapsedMs >= in.minLockMs)) {
            return CombatLockBreak::Override;
        }
        if (in.newSameItem) {
            return in.lockedIsPinned != in.newIsPinned ? CombatLockBreak::Restyle : CombatLockBreak::Hold;
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
