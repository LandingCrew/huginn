// Host tests for src/core/TargetTracking.h: the tracked-actor list's timeouts
// (seconds, not game days: 0.23.41) and the player's side.

#include "core/TargetTracking.h"

#include <doctest/doctest.h>

using namespace Huginn::Core::Targets;

namespace
{
    constexpr float kRange = 2176.0f * 2176.0f;

    TrackedTiming Alive(double lastSeen, float distSq = 100.0f)
    {
        return { lastSeen, false, -1.0, distSq, kRange };
    }
}

TEST_CASE("target tracking: an entry refreshed within 3 s stays; one unseen for longer leaves")
{
    CHECK(PruneVerdict(Alive(100.0), 100.0) == PruneReason::Keep);
    CHECK(PruneVerdict(Alive(100.0), 102.9) == PruneReason::Keep);
    CHECK(PruneVerdict(Alive(100.0), 103.0) == PruneReason::Keep);   // exactly 3 s: not yet
    CHECK(PruneVerdict(Alive(100.0), 103.1) == PruneReason::NotSeen);
}

TEST_CASE("target tracking: the timeouts are seconds -- the old game-day reading never fired")
{
    // The bug 0.23.41 fixed: 3.0 compared against a difference of game
    // DAYS. At a timescale of 20, ten real minutes are ~0.14 game days, far
    // under 3. In seconds the same ten minutes leave the list.
    const double tenMinutes = 600.0;
    CHECK(PruneVerdict(Alive(0.0), tenMinutes) == PruneReason::NotSeen);
    const double gameDaysInTenMinutes = tenMinutes * 20.0 / 86400.0;
    CHECK(gameDaysInTenMinutes < kNotSeenSec);
}

TEST_CASE("target tracking: past the release range leaves, inside it stays")
{
    CHECK(PruneVerdict(Alive(10.0, kRange), 10.0) == PruneReason::Keep);
    CHECK(PruneVerdict(Alive(10.0, kRange + 1.0f), 10.0) == PruneReason::OutOfRange);
    // Unseen wins over range: both say leave, the reason is the clock.
    CHECK(PruneVerdict(Alive(0.0, kRange + 1.0f), 10.0) == PruneReason::NotSeen);
}

TEST_CASE("target tracking: a dead entry stays 5 s from its death, whatever its last sighting alive")
{
    // Last seen alive at 50 (the scans skip it once it is dead), death noticed at 51.
    TrackedTiming e{ 50.0, true, 51.0, 100.0f, kRange };
    CHECK(PruneVerdict(e, 51.0) == PruneReason::Keep);
    CHECK(PruneVerdict(e, 55.5) == PruneReason::Keep);    // 4.5 s dead, 5.5 s unseen: still kept
    CHECK(PruneVerdict(e, 56.0) == PruneReason::Keep);
    CHECK(PruneVerdict(e, 56.1) == PruneReason::Dead);
    // A corpse under the crosshair is refreshed every poll: kept while it is.
    TrackedTiming corpse{ 80.0, true, 51.0, 100.0f, kRange };
    CHECK(PruneVerdict(corpse, 84.0) == PruneReason::Keep);
    CHECK(PruneVerdict(corpse, 85.1) == PruneReason::Dead);
    // A dead flag with no stamp falls back to the last sighting.
    TrackedTiming unstamped{ 50.0, true, -1.0, 100.0f, kRange };
    CHECK(PruneVerdict(unstamped, 55.0) == PruneReason::Keep);
    CHECK(PruneVerdict(unstamped, 55.1) == PruneReason::Dead);
}

TEST_CASE("target tracking: a death is stamped once, a resurrection clears it")
{
    auto d = NoteDeath(false, -1.0, false, 10.0);
    CHECK_FALSE(d.dead);
    CHECK(d.deadSinceSec < 0.0);
    d = NoteDeath(false, -1.0, true, 12.0);       // first poll that sees it dead
    CHECK(d.dead);
    CHECK(d.deadSinceSec == 12.0);
    d = NoteDeath(d.dead, d.deadSinceSec, true, 15.0);   // still dead: stamp kept
    CHECK(d.deadSinceSec == 12.0);
    d = NoteDeath(d.dead, d.deadSinceSec, false, 16.0);  // up again
    CHECK_FALSE(d.dead);
    CHECK(d.deadSinceSec < 0.0);
    d = NoteDeath(true, -1.0, true, 20.0);        // dead with no stamp: stamped now
    CHECK(d.deadSinceSec == 20.0);
}

TEST_CASE("target tracking: a killed hostile stops counting at once and leaves 5 s later")
{
    // The poll sequence the list goes through: alive and refreshed, then the
    // scan skips it (dead) and the stale pass notes the death.
    TrackedTiming e = Alive(0.0);
    for (double t = 0.0; t <= 4.0; t += 0.1) e.lastSeenSec = t;   // alive, refreshed
    const auto death = NoteDeath(e.dead, e.deadSinceSec, true, 4.1);
    e.dead = death.dead;
    e.deadSinceSec = death.deadSinceSec;
    CHECK(e.dead);   // counted out from this poll on (every count skips dead entries)
    CHECK(PruneVerdict(e, 7.2) == PruneReason::Keep);   // not the 3 s not-seen rule
    CHECK(PruneVerdict(e, 9.2) == PruneReason::Dead);
}

TEST_CASE("target tracking: the player's side is never a hostile")
{
    CHECK(IsPlayerSide(true, false, false));    // a follower
    CHECK(IsPlayerSide(false, true, false));    // the player's summon or thrall
    CHECK(IsPlayerSide(false, false, true));    // a follower's summon
    CHECK_FALSE(IsPlayerSide(false, false, false));

    CHECK(CountsAsHostile(true, false));
    CHECK_FALSE(CountsAsHostile(true, true));    // the staff's Flame Atronach the faction test calls hostile
    CHECK_FALSE(CountsAsHostile(false, false));
    CHECK_FALSE(CountsAsHostile(false, true));
}
