// Host tests for R8.1 (0.23.55): src/core/HardGates.h (the old engine's
// context zeros as filters), src/core/RefreshPass.h (the active-buff filters'
// refresh pass, doc 11 decision 4) and src/core/CombatOnset.h (one combat clock).

#include "core/CombatOnset.h"
#include "core/Coverage.h"
#include "core/HardGates.h"
#include "core/RefreshPass.h"

#include <doctest/doctest.h>

#include <array>
#include <ostream>
#include <vector>

using namespace Huginn::Core;

// =============================================================================
// Hard gates
// =============================================================================

TEST_CASE("hard gates: no_follower")
{
    CHECK(Gate::NoFollower(true, false));          // a heal for others, no one near
    CHECK_FALSE(Gate::NoFollower(true, true));     // a follower to take it
    CHECK_FALSE(Gate::NoFollower(false, false));   // a spell for the caster: never this gate
    CHECK_FALSE(Gate::NoFollower(false, true));
}

TEST_CASE("hard gates: the bench mapping is the old engine's")
{
    using Gate::Craft;
    CHECK(Gate::CraftOfBench(Needs::kBenchNone) == Craft::None);
    CHECK(Gate::CraftOfBench(Needs::kBenchCreateObject) == Craft::Smithing);   // a cooking spit too (frozen engine)
    CHECK(Gate::CraftOfBench(Needs::kBenchSmithingWeapon) == Craft::Smithing);
    CHECK(Gate::CraftOfBench(Needs::kBenchSmithingArmor) == Craft::Smithing);
    CHECK(Gate::CraftOfBench(Needs::kBenchEnchanting) == Craft::Enchanting);
    CHECK(Gate::CraftOfBench(Needs::kBenchEnchantingExperiment) == Craft::Enchanting);
    CHECK(Gate::CraftOfBench(Needs::kBenchAlchemy) == Craft::Alchemy);
    CHECK(Gate::CraftOfBench(Needs::kBenchAlchemyExperiment) == Craft::Alchemy);
    CHECK(Gate::CraftOfBench(8) == Craft::None);     // a bench type outside the vanilla enum
    CHECK(Gate::CraftOfBench(-1) == Craft::None);
    // Only the bench in the crosshair counts.
    CHECK(Gate::StationCraft(true, Needs::kBenchAlchemy) == Craft::Alchemy);
    CHECK(Gate::StationCraft(false, Needs::kBenchAlchemy) == Craft::None);
}

TEST_CASE("hard gates: no_workstation, a ring that fortifies two crafts included")
{
    using Gate::Craft;
    const Gate::Crafts smithingRing{ .smithing = true };
    const Gate::Crafts dualRing{ .smithing = true, .alchemy = true };   // Fortify Smithing 20 / Fortify Alchemy 5
    const Gate::Crafts nothing{};

    // Away from any station every piece is gated.
    CHECK(Gate::NoWorkstation(smithingRing, Craft::None));
    CHECK(Gate::NoWorkstation(dualRing, Craft::None));
    // At its own station it passes; at another craft's it does not.
    CHECK_FALSE(Gate::NoWorkstation(smithingRing, Craft::Smithing));
    CHECK(Gate::NoWorkstation(smithingRing, Craft::Alchemy));
    CHECK(Gate::NoWorkstation(smithingRing, Craft::Enchanting));
    // The dual ring passes at EITHER of its stations, the weaker craft's too
    // (the old engine read every craft a piece fortifies, not the strongest).
    CHECK_FALSE(Gate::NoWorkstation(dualRing, Craft::Smithing));
    CHECK_FALSE(Gate::NoWorkstation(dualRing, Craft::Alchemy));
    CHECK(Gate::NoWorkstation(dualRing, Craft::Enchanting));
    // A piece that fortifies nothing passes nowhere (the old weight was 0 everywhere).
    for (const auto c : { Craft::None, Craft::Smithing, Craft::Enchanting, Craft::Alchemy }) {
        CHECK(Gate::NoWorkstation(nothing, c));
    }
}

TEST_CASE("hard gates: not_dark -- daylight, a light already up, a torch already out")
{
    // Dark, nothing to see by: a torch passes.
    CHECK(Gate::DarknessApplies(true, false, false, false));
    CHECK_FALSE(Gate::NotDark(Gate::DarknessApplies(true, false, false, false)));
    // Daylight: gated.
    CHECK(Gate::NotDark(Gate::DarknessApplies(false, false, false, false)));
    // A torch already out: darkness no longer applies, so a second torch is gated.
    CHECK_FALSE(Gate::DarknessApplies(true, false, false, true));
    CHECK(Gate::NotDark(Gate::DarknessApplies(true, false, false, true)));
    // Night Eye or a light spell up: gated as well.
    CHECK(Gate::NotDark(Gate::DarknessApplies(true, true, false, false)));
    CHECK(Gate::NotDark(Gate::DarknessApplies(true, false, true, false)));
}

TEST_CASE("hard gates: the light threshold boundary ([ContextWeights] fDarkLightLevel)")
{
    constexpr float enter = 0.35f;   // the shipped fDarkLightLevel
    constexpr float gap = 0.10f;
    CHECK(Gate::IsDarkReading(0.349f, enter, false, gap));        // just under: dark
    CHECK_FALSE(Gate::IsDarkReading(0.35f, enter, false, gap));   // AT the threshold: not dark (strict)
    CHECK_FALSE(Gate::IsDarkReading(0.36f, enter, false, gap));   // daylight above it
    // Once dark, it holds until enter + gap (DarknessGate's hysteresis).
    CHECK(Gate::IsDarkReading(0.40f, enter, true, gap));
    CHECK(Gate::IsDarkReading(0.449f, enter, true, gap));
    CHECK_FALSE(Gate::IsDarkReading(0.45f, enter, true, gap));
    // A configured threshold moves the boundary with it.
    CHECK(Gate::IsDarkReading(0.5f, 0.6f, false, gap));
    CHECK_FALSE(Gate::IsDarkReading(0.6f, 0.6f, false, gap));
}

TEST_CASE("hard gates: favorite_suppressed")
{
    CHECK(Gate::FavoriteSuppressed(true, true));
    CHECK_FALSE(Gate::FavoriteSuppressed(true, false));    // Boost or Off
    CHECK_FALSE(Gate::FavoriteSuppressed(false, true));    // not a favourite
    CHECK_FALSE(Gate::FavoriteSuppressed(false, false));
}

TEST_CASE("hard gates: the default inputs gate everything a gate can")
{
    const Gate::Inputs in{};
    CHECK(Gate::NoFollower(true, in.followerNear));
    CHECK(Gate::NoWorkstation(Gate::Crafts{ .enchanting = true }, in.station));
    CHECK(Gate::NotDark(in.darknessApplies));
    CHECK_FALSE(Gate::FavoriteSuppressed(true, in.suppressFavorites));
    // No lock in the crosshair: no unlock item is dropped for its reach.
    CHECK(in.lockLevel == Lock::kNoLock);
    CHECK_FALSE(Lock::OutOfReach(true, Lock::kNovice, in.lockLevel));
}

TEST_CASE("lock reach: the highest level word an unlock row's text names (0.24.29)")
{
    // Requiem / LoreRim: name and description agree.
    CHECK(Lock::ReachFromText("Open Novice Lock") == Lock::kNovice);
    CHECK(Lock::ReachFromText("The spell transmutes locked objects, opening locks up to apprentice level.") ==
          Lock::kApprentice);
    CHECK(Lock::ReachFromText("Open Master Lock") == Lock::kMaster);
    // Gray Cowl's Unlock Container names two: the higher one is the reach.
    CHECK(Lock::ReachFromText("Unlocks the nearest locked (novice and apprentice) container") == Lock::kApprentice);
    CHECK(Lock::ReachFromText("can open locks with level Expert and below") == Lock::kExpert);
    // No level word, or one inside another word: unknown.
    CHECK(Lock::ReachFromText("Open a lock with a level <25> points or more below your Alteration skill") ==
          Lock::kUnknownReach);
    CHECK(Lock::ReachFromText("Unlock Container") == Lock::kUnknownReach);
    CHECK(Lock::ReachFromText("Webmaster's key") == Lock::kUnknownReach);
    CHECK(Lock::ReachFromText("") == Lock::kUnknownReach);
    // A row: its description first, then its name.
    CHECK(Lock::RowReach("Open Adept Lock", "") == Lock::kAdept);
    CHECK(Lock::RowReach("Open Lock", "opening locks up to expert level") == Lock::kExpert);
}

TEST_CASE("lock reach: an item's reach over its unlock rows")
{
    Lock::ItemReach none;
    CHECK(none.Value() == Lock::kUnknownReach);   // no unlock row
    Lock::ItemReach two;
    two.Add(Lock::kNovice);
    two.Add(Lock::kAdept);
    CHECK(two.Value() == Lock::kAdept);            // the best row
    Lock::ItemReach mixed;
    mixed.Add(Lock::kMaster);
    mixed.Add(Lock::kUnknownReach);
    CHECK(mixed.Value() == Lock::kUnknownReach);   // one row of unknown reach: never dropped for a lock
}

TEST_CASE("hard gates: lock_out_of_reach (0.24.29)")
{
    // A lock above the item's reach drops it; at or below keeps it.
    CHECK(Lock::OutOfReach(true, Lock::kNovice, Lock::kExpert));
    CHECK(Lock::OutOfReach(true, Lock::kApprentice, Lock::kAdept));
    CHECK_FALSE(Lock::OutOfReach(true, Lock::kApprentice, Lock::kApprentice));
    CHECK_FALSE(Lock::OutOfReach(true, Lock::kMaster, Lock::kNovice));
    // A lock that requires a key: no unlock item opens it, known reach or not.
    CHECK(Lock::OutOfReach(true, Lock::kMaster, Lock::kRequiresKey));
    CHECK(Lock::OutOfReach(true, Lock::kUnknownReach, Lock::kRequiresKey));
    // Unknown reach (Detonate Lock: by skill): kept for any pickable lock.
    CHECK_FALSE(Lock::OutOfReach(true, Lock::kUnknownReach, Lock::kMaster));
    // No lock in the crosshair, or not an unlock item: never.
    CHECK_FALSE(Lock::OutOfReach(true, Lock::kNovice, Lock::kNoLock));
    CHECK_FALSE(Lock::OutOfReach(false, Lock::kNovice, Lock::kMaster));
}

// =============================================================================
// Refresh pass
// =============================================================================

TEST_CASE("refresh pass: due within fRefreshPriorSec, never without a timer")
{
    using Refresh::RefreshDue;
    CHECK(Refresh::kNoTimer == Coverage::kNoTimer);
    CHECK(RefreshDue(4.9f, 5.0f));
    CHECK(RefreshDue(0.0f, 5.0f));            // expired but still listed: due
    CHECK_FALSE(RefreshDue(5.0f, 5.0f));      // AT the window: not yet (strictly less)
    CHECK_FALSE(RefreshDue(30.0f, 5.0f));
    CHECK_FALSE(RefreshDue(Refresh::kNoTimer, 5.0f));   // an ability, a worn enchantment: never
    CHECK_FALSE(RefreshDue(0.0f, 0.0f));      // fRefreshPriorSec = 0 turns the pass off
    CHECK(RefreshDue(8.0f, 10.0f));           // a configured window
}

TEST_CASE("refresh pass: a flag's block ends with the last of its effects")
{
    Refresh::Timer t;
    CHECK_FALSE(t.any);
    t.Add(3.0f);
    CHECK(t.remaining == 3.0f);
    t.Add(40.0f);                       // a second cast, longer: the block lasts 40 s
    CHECK(t.remaining == 40.0f);
    t.Add(10.0f);
    CHECK(t.remaining == 40.0f);
    t.Add(Refresh::kNoTimer);           // a constant effect of the same kind: never ends
    CHECK(t.remaining == Refresh::kNoTimer);
    t.Add(1.0f);
    CHECK(t.remaining == Refresh::kNoTimer);

    Refresh::Timer c;
    c.Add(Refresh::kNoTimer);           // constant first, timed after: still never
    c.Add(2.0f);
    CHECK(c.remaining == Refresh::kNoTimer);
}

TEST_CASE("refresh pass: the summon limit block is the oldest summon's timer")
{
    // A cast at the limit dismisses the oldest commanded summon or thrall, so
    // the block ends with that one: Coverage::OldestRemaining, the value
    // covered(i) reads. ComputeDue reads it as Inputs::summonLeft.
    using K = Coverage::CommandedActor::Kind;
    const auto due = [](const std::vector<Coverage::CommandedActor>& list, int count, float limit) {
        Refresh::Inputs in;
        in.summonBlocked = Coverage::SummonLimitReached(count, limit);
        in.summonLeft = Coverage::OldestRemaining(list);
        return Refresh::IsDue(Refresh::ComputeDue(in, 5.0f), Refresh::Block::summon_limit);
    };

    // Vanilla: the one summon's timer.
    const std::vector<Coverage::CommandedActor> one{ { 1, 11, K::Summon, 3.0f, 57.0f } };
    CHECK(due(one, 1, 1.0f));
    CHECK_FALSE(due(one, 1, 2.0f));                     // Twin Souls, one up: not blocking

    // Twin Souls, two timed summons: the oldest (largest elapsed) decides,
    // even when the younger one would time out first.
    const std::vector<Coverage::CommandedActor> two{
        { 1, 11, K::Summon, 40.0f, 80.0f },             // the oldest: 40 s left
        { 2, 12, K::Summon, 3.0f, 10.0f },              // younger, 3 s left
    };
    CHECK(Coverage::OldestRemaining(two) == 40.0f);
    CHECK_FALSE(due(two, 2, 2.0f));

    // Twin Souls with a Dead Thrall (the oldest, no timer) and one timed
    // summon in its last seconds: a cast would dismiss the thrall, so the
    // block never ends on a timer -- never due.
    const std::vector<Coverage::CommandedActor> thrall{
        { 1, 11, K::Reanimate, Coverage::kNoTimer, 900.0f },
        { 2, 12, K::Summon, 3.0f, 57.0f },
    };
    CHECK(Coverage::OldestRemaining(thrall) == Coverage::kNoTimer);
    CHECK_FALSE(due(thrall, 2, 2.0f));

    // A commanded actor of another kind is not a summon slot: skipped.
    const std::vector<Coverage::CommandedActor> other{
        { 1, 11, K::Other, Coverage::kNoTimer, 900.0f },
        { 2, 12, K::Summon, 3.0f, 57.0f },
    };
    CHECK(due(other, 1, 1.0f));

    // Counted but not listed: no timer, never due.
    CHECK_FALSE(due({}, 1, 1.0f));
}

TEST_CASE("refresh pass: ComputeDue sets a bit only while the block holds and ends soon")
{
    using Refresh::Block;
    using Refresh::IsDue;
    Refresh::Inputs in;
    CHECK(Refresh::ComputeDue(in, 5.0f) == 0);   // nothing up

    in.invisibility.Add(3.0f);
    in.muffle.Add(30.0f);
    in.armor.Add(Refresh::kNoTimer);             // a worn armour enchantment
    in.warming.Add(4.0f);
    in.waterbreathing.Add(4.0f);
    in.waterbreathing.Add(60.0f);                // a second potion: 60 s
    in.summonBlocked = true;
    in.summonLeft = 2.0f;

    const auto bits = Refresh::ComputeDue(in, 5.0f);
    CHECK(IsDue(bits, Block::invisibility));
    CHECK_FALSE(IsDue(bits, Block::muffle));
    CHECK_FALSE(IsDue(bits, Block::armor));      // no duration: never
    CHECK(IsDue(bits, Block::warming));
    CHECK_FALSE(IsDue(bits, Block::waterbreathing));
    CHECK(IsDue(bits, Block::summon_limit));

    // The summon bit needs the block itself.
    in.summonBlocked = false;
    CHECK_FALSE(IsDue(Refresh::ComputeDue(in, 5.0f), Block::summon_limit));
    // A wider window takes the muffle too; 0 turns every bit off.
    CHECK(IsDue(Refresh::ComputeDue(in, 31.0f), Block::muffle));
    CHECK(Refresh::ComputeDue(in, 0.0f) == 0);
}

// =============================================================================
// Combat onset
// =============================================================================

TEST_CASE("combat onset: one clock, kNever out of combat, reset on re-entry")
{
    using namespace Needs;
    // Not in combat: kNever, whatever the clock says; no window, no fight time.
    CHECK(CombatOnsetAgo(false, 100.0, 105.0) == kNever);
    CHECK(CombatOnsetAgo(false, 2.0f) == kNever);
    CHECK_FALSE(InCombatStartWindow(kNever, 10.0f));
    CHECK_FALSE(InCombatStartWindow(kNever, 2.0e6f));   // even with an absurd window
    CHECK(CombatSeconds(kNever) == 0.0f);

    // In combat since t = 100.
    double startAt = 100.0;
    CHECK(CombatOnsetAgo(true, startAt, 104.0) == doctest::Approx(4.0f));
    CHECK(InCombatStartWindow(CombatOnsetAgo(true, startAt, 104.0), 10.0f));
    CHECK_FALSE(InCombatStartWindow(CombatOnsetAgo(true, startAt, 110.0), 10.0f));   // AT the window: out
    CHECK(CombatSeconds(CombatOnsetAgo(true, startAt, 135.0)) == doctest::Approx(35.0f));

    // Combat ends at 140 (the sensor writes combatEndAt; the start stays) and
    // is re-entered at 200: the sensor writes the new start, so the window opens again.
    CHECK(CombatOnsetAgo(false, startAt, 150.0) == kNever);
    startAt = 200.0;
    CHECK(CombatOnsetAgo(true, startAt, 201.0) == doctest::Approx(1.0f));
    CHECK(InCombatStartWindow(CombatOnsetAgo(true, startAt, 201.0), 10.0f));

    // In combat with no start recorded (-1): read as no fight, not as a fresh one.
    CHECK(CombatOnsetAgo(true, -1.0, 5.0) == kNever);
    // The need's form, from the snapshot's seconds-ago.
    CHECK(CombatOnsetAgo(true, 3.0f) == 3.0f);
}

TEST_CASE("lock: the crosshair's lock, and which ones lock_in_crosshair counts (0.24.29)")
{
    CHECK(Lock::CrosshairLock(false, Lock::kExpert) == Lock::kNoLock);
    CHECK(Lock::CrosshairLock(true, Lock::kExpert) == Lock::kExpert);
    CHECK(Lock::CrosshairLock(true, Lock::kNovice) == Lock::kNovice);   // Novice is 0, not "none"
    CHECK(Lock::Pickable(Lock::kNovice));
    CHECK(Lock::Pickable(Lock::kMaster));
    CHECK_FALSE(Lock::Pickable(Lock::kRequiresKey));
    CHECK_FALSE(Lock::Pickable(Lock::kNoLock));
}
