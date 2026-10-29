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

TEST_CASE("refresh pass: the resist block, timed parts against a permanent base")
{
    using Refresh::ResistParts;
    using Refresh::ResistTimeLeft;
    ResistParts none;
    CHECK(ResistTimeLeft(40.0f, 50.0f, none) == 0.0f);                    // not over the threshold
    CHECK(ResistTimeLeft(60.0f, 50.0f, none) == Refresh::kNoTimer);      // all permanent (gear, a race)

    ResistParts potion;
    potion.Add(50.0f, 12.0f);   // Resist Fire potion, 12 s left
    CHECK(ResistTimeLeft(60.0f, 50.0f, potion) == 12.0f);                 // 10 base + 50: falls to 10 at 12 s
    CHECK(ResistTimeLeft(110.0f, 50.0f, potion) == Refresh::kNoTimer);   // 60 base: over the threshold for good

    ResistParts two;
    two.Add(30.0f, 20.0f);
    two.Add(30.0f, 4.0f);
    // 20 base + 30 + 30 = 80: at 4 s it is 50 (at the threshold, not over): due at 4 s.
    CHECK(ResistTimeLeft(80.0f, 50.0f, two) == 4.0f);
    // 30 base: 60 after 4 s (still over), 30 after 20 s.
    CHECK(ResistTimeLeft(90.0f, 50.0f, two) == 20.0f);

    // A weakness that ends first raises the value again: the block ends at the
    // LAST crossing, not the first.
    ResistParts weak;
    weak.Add(50.0f, 30.0f);     // a potion, 30 s
    weak.Add(-40.0f, 5.0f);     // a weakness, 5 s
    // 60 base + 50 - 40 = 70; at 5 s 110; at 30 s 60: still over -> never.
    CHECK(ResistTimeLeft(70.0f, 50.0f, weak) == Refresh::kNoTimer);
    // 41 base: 51 -> 91 at 5 s -> 41 at 30 s.
    CHECK(ResistTimeLeft(51.0f, 50.0f, weak) == 30.0f);

    // A constant part is not kept (it is the permanent base), nor a part past kMax.
    ResistParts full;
    full.Add(10.0f, Refresh::kNoTimer);
    CHECK(full.n == 0);
    for (std::size_t i = 0; i < ResistParts::kMax + 2; ++i) full.Add(1.0f, 1.0f);
    CHECK(full.n == ResistParts::kMax);
}

TEST_CASE("refresh pass: the summon limit block")
{
    using Refresh::SummonTimeLeft;
    const std::vector<float> one{ 3.0f };
    CHECK(SummonTimeLeft(one, 1, 1) == 3.0f);                 // vanilla: the one summon's timer
    CHECK(SummonTimeLeft(one, 1, 2) == 0.0f);                 // Twin Souls, one up: not blocking
    const std::vector<float> two{ 40.0f, 6.0f };
    CHECK(SummonTimeLeft(two, 2, 2) == 6.0f);                 // the first to time out frees the limit
    CHECK(SummonTimeLeft(two, 3, 2) == 40.0f);                // over the limit: two must end
    const std::vector<float> thrall{ Refresh::kNoTimer };
    CHECK(SummonTimeLeft(thrall, 1, 1) == Refresh::kNoTimer); // a thrall never times out
    const std::vector<float> mixed{ Refresh::kNoTimer, 9.0f };
    CHECK(SummonTimeLeft(mixed, 2, 2) == 9.0f);               // the timed summon frees the second place
    CHECK(SummonTimeLeft(mixed, 2, 1) == Refresh::kNoTimer);  // limit 1: the thrall must end too
    const std::vector<float> unknown{};
    CHECK(SummonTimeLeft(unknown, 1, 1) == Refresh::kNoTimer);  // counted but not listed: never
    CHECK(SummonTimeLeft(one, 1, 0) == 3.0f);                 // a limit under 1 is 1
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
    in.resistThreshold = 50.0f;
    in.resistTotal[0] = 60.0f;                   // fire: 10 + a potion with 4 s
    in.resist[0].Add(50.0f, 4.0f);
    in.resistTotal[1] = 60.0f;                   // frost: all permanent
    in.resistTotal[2] = 20.0f;                   // shock: under the threshold
    in.resist[2].Add(20.0f, 1.0f);

    const auto bits = Refresh::ComputeDue(in, 5.0f);
    CHECK(IsDue(bits, Block::invisibility));
    CHECK_FALSE(IsDue(bits, Block::muffle));
    CHECK_FALSE(IsDue(bits, Block::armor));      // no duration: never
    CHECK(IsDue(bits, Block::warming));
    CHECK_FALSE(IsDue(bits, Block::waterbreathing));
    CHECK(IsDue(bits, Block::summon_limit));
    CHECK(IsDue(bits, Block::resist_fire));
    CHECK_FALSE(IsDue(bits, Block::resist_frost));
    CHECK_FALSE(IsDue(bits, Block::resist_shock));   // not blocking: no bit
    CHECK_FALSE(IsDue(bits, Block::resist_poison));

    // The summon bit needs the block itself.
    in.summonBlocked = false;
    CHECK_FALSE(IsDue(Refresh::ComputeDue(in, 5.0f), Block::summon_limit));
    // A wider window takes the muffle too; 0 turns every bit off.
    CHECK(IsDue(Refresh::ComputeDue(in, 31.0f), Block::muffle));
    CHECK(Refresh::ComputeDue(in, 0.0f) == 0);
    CHECK(Refresh::ResistBlock(Refresh::Element::magic) == Block::resist_magic);
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
