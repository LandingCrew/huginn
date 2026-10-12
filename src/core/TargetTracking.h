#pragma once

// =============================================================================
// TARGET TRACKING -- when a tracked actor leaves the list, and whose side it is
// =============================================================================
// The decisions behind StateManager::PollTargets' tracked-actor list
// (state/StateManager_Targets.cpp), pulled out so the host tests can pin them:
//
//   PruneVerdict  whether an entry stays: an actor not seen for kNotSeenSec,
//                 a dead one kDeadSec after its death was noticed, or one
//                 past the release range leaves.
//   NoteDeath     the entry's dead flag and the time it was first seen dead,
//                 from the actor's own flag this poll.
//   IsPlayerSide  the player's teammates, the actors the player commands
//                 (summons, reanimated thralls) and the ones a teammate
//                 commands: never a hostile, whatever the faction test says.
//
// 0.23.41. Before it the two timeouts (3 and 5, "seconds") were compared
// against the game CALENDAR (days): an entry had to go unseen for three game
// days to time out, so none ever did. The hostile scan skips a dead actor
// and never refreshed its entry, which kept isDead = false, so a hostile
// killed off the crosshair was counted alive -- enemy_count, surrounded, the
// GameState Enemies bucket, the closest-enemy distance -- until combat ended
// and the hostiles were cleared. In the 0.23.38 LoreRim session launched
// 2026-10-12 10:45 a five-enemy fight ended (Combat InCombat -> OutOfCombat,
// 11:13:19.9, debug log line 7432) with enemy_count 0.82 (four) and
// surrounded 1.00 on the last [Needs] line before it (line 7429); over the
// 21 debug logs kept from 2026-10-09..12, 147 of 228 fights ended with one or more
// hostiles still counted.
//
// Times are seconds on one monotonic clock (the caller passes steady-clock
// seconds); nothing here reads a clock.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <cstdint>

namespace Huginn::Core::Targets
{
    /// An entry not refreshed by any scan for this long leaves (out of range,
    /// unloaded, no longer hostile and not an ally in range).
    inline constexpr double kNotSeenSec = 3.0;
    /// A dead actor's entry stays this long after its death was noticed (or
    /// after it was last refreshed, for a corpse kept under the crosshair),
    /// flagged dead: no count reads it.
    inline constexpr double kDeadSec = 5.0;

    enum class PruneReason : std::uint8_t
    {
        Keep,
        NotSeen,     // not refreshed for kNotSeenSec
        Dead,        // dead for kDeadSec
        OutOfRange,  // past its release range
    };

    struct TrackedTiming
    {
        double lastSeenSec = 0.0;    // when a scan last refreshed the entry
        bool dead = false;
        double deadSinceSec = -1.0;  // when it was first seen dead, -1 = not dead
        float distanceSq = 0.0f;
        float releaseRangeSq = 0.0f;
    };

    /// Whether the entry stays, and why not. A dead entry is judged by its
    /// death alone: the scans skip dead actors, so it is never "seen" again
    /// except as the crosshair's corpse, which refreshes lastSeen and keeps it.
    [[nodiscard]] constexpr PruneReason PruneVerdict(const TrackedTiming& e, double nowSec,
                                                     double notSeenSec = kNotSeenSec,
                                                     double deadSec = kDeadSec) noexcept
    {
        if (e.dead) {
            const double since = e.deadSinceSec >= 0.0 ? std::max(e.deadSinceSec, e.lastSeenSec) : e.lastSeenSec;
            return nowSec - since > deadSec ? PruneReason::Dead : PruneReason::Keep;
        }
        // A clock reading earlier than the last sighting (none on a steady
        // clock; a reset) is "just seen".
        if (nowSec - e.lastSeenSec > notSeenSec) return PruneReason::NotSeen;
        if (e.distanceSq > e.releaseRangeSq) return PruneReason::OutOfRange;
        return PruneReason::Keep;
    }

    struct DeathState
    {
        bool dead = false;
        double deadSinceSec = -1.0;
    };

    /// The entry's death bookkeeping after this poll read the actor's own dead
    /// flag: the first poll that sees it dead stamps the time, later ones keep
    /// it; an actor up again (resurrected, reanimated) is alive with no stamp.
    [[nodiscard]] constexpr DeathState NoteDeath(bool wasDead, double deadSinceSec, bool deadNow,
                                                 double nowSec) noexcept
    {
        if (!deadNow) return {};
        if (wasDead && deadSinceSec >= 0.0) return { true, deadSinceSec };
        return { true, nowSec };
    }

    /// The player's side: a teammate (a follower), an actor the player
    /// commands (a summon, a thrall), or one a teammate commands (a
    /// follower's summon). The engine's hostility test is a faction test and
    /// can call such an actor hostile to the player: in the 0.23.26 LoreRim
    /// session the Flame Atronach from the player's own staff read as a
    /// hostile daedra, fire elemental and summon the moment it appeared
    /// (HuginnLogs/_Huginn_Debug-20261010-213146.log lines 6294 and 6314:
    /// enemy_count 0.70 -> 0.82, target_daedra, target_element_fire and
    /// target_summoned on, Target Undead -> Daedra, in a draugr fight).
    [[nodiscard]] constexpr bool IsPlayerSide(bool teammate, bool commandedByPlayer, bool commanderIsTeammate) noexcept
    {
        return teammate || commandedByPlayer || commanderIsTeammate;
    }

    /// Whether a scanned actor counts as a hostile: the engine says hostile
    /// and it is not the player's side.
    [[nodiscard]] constexpr bool CountsAsHostile(bool hostileToPlayer, bool playerSide) noexcept
    {
        return hostileToPlayer && !playerSide;
    }
}
