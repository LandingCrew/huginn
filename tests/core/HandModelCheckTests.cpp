// Host tests for src/core/HandModelCheck.h: is the drawn weapon on the
// skeleton, and when a missing one is worth a log line (0.23.44).

#include "core/HandModelCheck.h"

#include <doctest/doctest.h>

using namespace Huginn::Core;

namespace
{
    constexpr std::uint32_t kStaff = 0xFECCF01D;
    constexpr std::uint32_t kSword = 0x00013991;
    constexpr std::uint8_t k1st = 1;
    constexpr std::uint8_t k3rd = 3;

    constexpr auto D = HandPose::Drawn;
    constexpr auto S = HandPose::Sheathed;
    constexpr auto M = HandPose::Moving;
    using K = FaultWatch::EventKind;
    using GK = GraphHandWatch::EventKind;
}

TEST_CASE("hand model: a drawn part is judged")
{
    CHECK(JudgePart({ true, true, true, false, true }) == ModelState::Ok);
    CHECK(JudgePart({ true, true, true, false, false }) == ModelState::NotInHand);
    CHECK(JudgePart({ true, true, true, true, true }) == ModelState::Culled);
    CHECK(JudgePart({ true, true, false, false, true }) == ModelState::Orphaned);  // a WEAPON node, but a stale skeleton
    CHECK(JudgePart({ true, false, false, false, false }) == ModelState::Detached);
    CHECK(JudgePart(PartReading{}) == ModelState::NoPart);
}

TEST_CASE("hand model: a fault is reported once it has held 2 s, then its end once")
{
    FaultWatch w;
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 0.0).kind == K::None);
    CHECK(w.Pending());
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 1.9).kind == K::None);
    const auto onset = w.Update(kStaff, D, k1st, ModelState::NoPart, 2.0);
    CHECK(onset.kind == K::Onset);
    CHECK(onset.state == ModelState::NoPart);
    CHECK(w.Reported());
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 30.0).kind == K::None);  // held: no second report
    const auto back = w.Update(kStaff, D, k1st, ModelState::Ok, 31.0);
    CHECK(back.kind == K::Recovered);
    CHECK(back.state == ModelState::NoPart);
    CHECK(back.faultSec == doctest::Approx(31.0));
    CHECK_FALSE(w.Reported());
    CHECK(w.Update(kStaff, D, k1st, ModelState::Ok, 32.0).kind == K::None);
}

TEST_CASE("hand model: a model still loading after a swap is not a fault")
{
    FaultWatch w;
    w.Update(kSword, D, k1st, ModelState::Ok, 0.0);
    w.Update(kStaff, D, k1st, ModelState::NoPart, 1.0);
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 2.5).kind == K::None);
    CHECK(w.Update(kStaff, D, k1st, ModelState::Ok, 2.9).kind == K::None);
    CHECK_FALSE(w.Pending());
}

TEST_CASE("hand model: sheathed is never judged")
{
    FaultWatch w;
    for (double t = 0.0; t < 10.0; t += 0.25) {
        CHECK(w.Update(kStaff, S, k3rd, ModelState::NoPart, t).kind == K::None);
    }
    CHECK_FALSE(w.Pending());
}

TEST_CASE("hand model: a draw or sheathe restarts an unreported fault")
{
    FaultWatch w;
    w.Update(kStaff, S, k3rd, ModelState::Ok, 0.0);
    CHECK(w.Update(kStaff, M, k3rd, ModelState::NotInHand, 0.5).kind == K::None);  // drawing
    w.Update(kStaff, D, k3rd, ModelState::NotInHand, 1.0);
    CHECK(w.Pending());
    w.Update(kStaff, M, k3rd, ModelState::NotInHand, 2.0);  // sheathing before 2 s
    CHECK_FALSE(w.Pending());
}

TEST_CASE("hand model: a sheathe ends nothing; only the same check passing does")
{
    // The verifier's case: a culled fault, sheathed while still culled, must
    // not log "back", and the next draw must not report it a second time.
    FaultWatch w;
    w.Update(kStaff, D, k1st, ModelState::Culled, 10.0);
    CHECK(w.Update(kStaff, D, k1st, ModelState::Culled, 12.0).kind == K::Onset);
    CHECK(w.Update(kStaff, M, k1st, ModelState::Ok, 13.0).kind == K::None);
    CHECK(w.Update(kStaff, S, k3rd, ModelState::Ok, 14.0).kind == K::None);
    CHECK(w.Reported());
    w.Update(kStaff, M, k1st, ModelState::Ok, 15.0);
    CHECK(w.Update(kStaff, D, k1st, ModelState::Culled, 16.0).kind == K::None);
    CHECK(w.Update(kStaff, D, k1st, ModelState::Culled, 20.0).kind == K::None);  // no second onset
    // Fixed by a later sheathe and draw: one line, one sheathe per cycle.
    w.Update(kStaff, M, k1st, ModelState::Ok, 21.0);
    w.Update(kStaff, S, k3rd, ModelState::Ok, 22.0);
    w.Update(kStaff, M, k1st, ModelState::Ok, 23.0);
    const auto back = w.Update(kStaff, D, k1st, ModelState::Ok, 24.0);
    CHECK(back.kind == K::Recovered);
    CHECK(back.poseChanges == 2);
    CHECK(back.faultSec == doctest::Approx(14.0));
}

TEST_CASE("hand model: a view change neither ends a reported fault nor carries a pending one")
{
    FaultWatch w;
    w.Update(kStaff, D, k1st, ModelState::NoPart, 0.0);
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 2.0).kind == K::Onset);
    // Third person, the third-person model fine: the first-person fault stands.
    CHECK(w.Update(kStaff, D, k3rd, ModelState::Ok, 3.0).kind == K::None);
    CHECK(w.Reported());
    CHECK(w.Update(kStaff, D, k1st, ModelState::Ok, 4.0).kind == K::Recovered);

    // Pending in first person, then third person: restarts there.
    w.Update(kStaff, D, k1st, ModelState::NoPart, 10.0);
    w.Update(kStaff, D, k3rd, ModelState::NoPart, 11.5);
    CHECK(w.Update(kStaff, D, k3rd, ModelState::NoPart, 12.5).kind == K::None);
    CHECK(w.Update(kStaff, D, k3rd, ModelState::NoPart, 13.5).kind == K::Onset);
}

TEST_CASE("hand model: a reported fault ends when its weapon leaves the hand")
{
    FaultWatch w;
    w.Update(kStaff, D, k1st, ModelState::NoPart, 0.0);
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 2.0).kind == K::Onset);
    const auto out = w.Update(kSword, D, k1st, ModelState::Ok, 5.0);
    CHECK(out.kind == K::SwappedOut);
    CHECK(out.state == ModelState::NoPart);
    CHECK(out.faultSec == doctest::Approx(5.0));
    CHECK_FALSE(w.Reported());

    // Emptied hand (a spell, fists): ends it too.
    w.Update(kStaff, D, k1st, ModelState::Culled, 10.0);
    w.Update(kStaff, D, k1st, ModelState::Culled, 12.0);
    CHECK(w.Update(0, D, k1st, ModelState::Ok, 13.0).kind == K::SwappedOut);
}

TEST_CASE("hand model: the clock runs across a change of fault on one weapon")
{
    FaultWatch w;
    w.Update(kStaff, D, k1st, ModelState::NoPart, 0.0);
    w.Update(kStaff, D, k1st, ModelState::Detached, 1.0);
    const auto onset = w.Update(kStaff, D, k1st, ModelState::NotInHand, 2.0);
    CHECK(onset.kind == K::Onset);
    CHECK(onset.state == ModelState::NotInHand);
}

TEST_CASE("hand model: reset forgets everything")
{
    FaultWatch w;
    w.Update(kStaff, D, k1st, ModelState::NoPart, 0.0);
    w.Update(kStaff, D, k1st, ModelState::NoPart, 2.0);
    w.Reset();
    CHECK_FALSE(w.Reported());
    // The same weapon after the load: a new fault, a new clock.
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 3.0).kind == K::None);
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 5.0).kind == K::Onset);
}

TEST_CASE("pose stuck: a draw that never finishes is reported once, then its end")
{
    constexpr std::uint8_t kWantToDraw = 1, kDrawing = 2, kDrawn = 3;
    PoseStuckWatch w;
    w.Update(kDrawn, 0.0);
    // A normal draw: about a second in the moving states.
    w.Update(0, 1.0);
    CHECK(w.Update(kWantToDraw, 1.1).kind == PoseStuckWatch::EventKind::None);
    CHECK(w.Update(kDrawing, 1.3).kind == PoseStuckWatch::EventKind::None);
    CHECK(w.Update(kDrawn, 2.0).kind == PoseStuckWatch::EventKind::None);
    // Settled states never report, however long.
    CHECK(w.Update(kDrawn, 60.0).kind == PoseStuckWatch::EventKind::None);

    // The sighting: drawing, and it stays there.
    w.Update(kDrawing, 100.0);
    CHECK(w.Update(kDrawing, 102.9).kind == PoseStuckWatch::EventKind::None);
    const auto stuck = w.Update(kDrawing, 103.0);
    CHECK(stuck.kind == PoseStuckWatch::EventKind::Stuck);
    CHECK(stuck.state == kDrawing);
    CHECK(w.Update(kDrawing, 130.0).kind == PoseStuckWatch::EventKind::None);  // once
    const auto settled = w.Update(0, 131.0);  // the player sheathes
    CHECK(settled.kind == PoseStuckWatch::EventKind::Settled);
    CHECK(settled.stuckIn == kDrawing);
    CHECK(settled.state == 0);
    CHECK(settled.heldSec == doctest::Approx(31.0));
    CHECK_FALSE(w.Reported());
}

TEST_CASE("hand model: a one-hander is in its hand only under its own side's node")
{
    using N = HandNode;
    CHECK(InOwnHandNode(N::Weapon, N::Weapon));
    CHECK(InOwnHandNode(N::Shield, N::Shield));
    CHECK_FALSE(InOwnHandNode(N::Weapon, N::Shield));  // a left-hand dagger on WEAPON
    CHECK_FALSE(InOwnHandNode(N::Shield, N::Weapon));
    CHECK_FALSE(InOwnHandNode(N::None, N::Weapon));     // on a sheath
    // Two-handers, bows, staffs: either node, as before (a drawn bow hangs from SHIELD).
    CHECK(InOwnHandNode(N::Shield, N::None));
    CHECK(InOwnHandNode(N::Weapon, N::None));
    CHECK_FALSE(InOwnHandNode(N::None, N::None));
}

TEST_CASE("hand model: an ambiguous part goes to the hand whose node it hangs under")
{
    using N = HandNode;
    // Unambiguous: every match is the hand's, wherever it hangs.
    CHECK(PartIsHands(N::Shield, N::Weapon, false));
    // Two Iron Daggers: the right hand keeps the WEAPON part, not the left copy's.
    CHECK(PartIsHands(N::Weapon, N::Weapon, true));
    CHECK_FALSE(PartIsHands(N::Shield, N::Weapon, true));
    CHECK(PartIsHands(N::Shield, N::Shield, true));
    CHECK_FALSE(PartIsHands(N::Weapon, N::Shield, true));
    // Under neither node: kept, and never Ok, so it cannot hide a missing model.
    CHECK(PartIsHands(N::None, N::Weapon, true));
    CHECK(JudgePart({ true, true, true, false, InOwnHandNode(N::None, N::Weapon) }) == ModelState::NotInHand);
}

TEST_CASE("hand model: a gap in the world restarts a pending fault and leaves a reported one standing")
{
    FaultWatch w;
    w.Update(kStaff, D, k1st, ModelState::NoPart, 0.0);
    CHECK(w.FormID() == kStaff);
    w.Update(kStaff, D, k1st, ModelState::NoPart, 1.5);
    w.Restart();  // a door at 1.5 s; back at 30 s, the clock having run on
    CHECK_FALSE(w.Pending());
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 30.0).kind == K::None);  // not an onset
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 31.9).kind == K::None);
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 32.0).kind == K::Onset);

    // Reported: the gap does not end it, nor report it again.
    w.Restart();
    CHECK(w.Reported());
    CHECK(w.Update(kStaff, D, k1st, ModelState::NoPart, 60.0).kind == K::None);
    const auto back = w.Update(kStaff, D, k1st, ModelState::Ok, 61.0);
    CHECK(back.kind == K::Recovered);
    CHECK(back.faultSec == doctest::Approx(31.0));
}

TEST_CASE("pose stuck: a draw that flaps between moving states is timed from the last settled state")
{
    using PK = PoseStuckWatch::EventKind;
    constexpr std::uint8_t kWantToDraw = 1, kDrawing = 2, kDrawn = 3;
    PoseStuckWatch w;
    w.Update(0, 0.0);
    // 2.5 s want-to-draw, then 2.5 s drawing: reported once, at 3 s, naming drawing.
    CHECK(w.Update(kWantToDraw, 10.0).kind == PK::None);
    CHECK(w.Update(kWantToDraw, 12.4).kind == PK::None);
    CHECK(w.Update(kDrawing, 12.5).kind == PK::None);
    CHECK(w.HeldSec(12.9) == doctest::Approx(0.4));  // the exact state's own time
    CHECK(w.Update(kDrawing, 12.9).kind == PK::None);
    const auto stuck = w.Update(kDrawing, 13.0);
    CHECK(stuck.kind == PK::Stuck);
    CHECK(stuck.state == kDrawing);
    CHECK(stuck.heldSec == doctest::Approx(3.0));
    CHECK(w.Update(kDrawing, 15.0).kind == PK::None);
    CHECK(w.Update(kWantToDraw, 15.0).kind == PK::None);  // flapping back: no settle, no second report
    CHECK(w.Update(kDrawing, 15.5).kind == PK::None);
    const auto settled = w.Update(kDrawn, 16.0);
    CHECK(settled.kind == PK::Settled);
    CHECK(settled.state == kDrawn);
    CHECK(settled.stuckIn == kDrawing);
    CHECK(settled.heldSec == doctest::Approx(6.0));
    CHECK_FALSE(w.Reported());

    // A settled state in between restarts the clock.
    w.Update(kWantToDraw, 20.0);
    w.Update(0, 22.0);
    w.Update(kWantToDraw, 22.5);
    CHECK(w.Update(kWantToDraw, 25.0).kind == PK::None);
    CHECK(w.Update(kWantToDraw, 25.5).kind == PK::Stuck);
}

TEST_CASE("pose stuck: a gap in the world restarts a pending clock and leaves a reported one standing")
{
    using PK = PoseStuckWatch::EventKind;
    constexpr std::uint8_t kDrawing = 2, kDrawn = 3;
    PoseStuckWatch w;
    w.Update(kDrawn, 0.0);
    w.Update(kDrawing, 1.0);
    w.Update(kDrawing, 3.5);
    w.Restart();  // a door; back at 20 s, still drawing
    CHECK(w.Update(kDrawing, 20.0).kind == PK::None);
    CHECK(w.Update(kDrawing, 22.9).kind == PK::None);
    CHECK(w.Update(kDrawing, 23.0).kind == PK::Stuck);

    w.Restart();
    CHECK(w.Reported());
    CHECK(w.Update(kDrawing, 40.0).kind == PK::None);
    const auto settled = w.Update(kDrawn, 41.0);
    CHECK(settled.kind == PK::Settled);
    CHECK(settled.heldSec == doctest::Approx(21.0));
}

TEST_CASE("graph hand: only the clear cases of the stuck graph are mismatches")
{
    // A spell in the hand: the graph must read magic.
    CHECK(JudgeGraphHand(HandHeld::Spell, kGraphHandMagic) == GraphHand::Agrees);
    CHECK(JudgeGraphHand(HandHeld::Spell, 0) == GraphHand::SpellNotMagic);
    CHECK(JudgeGraphHand(HandHeld::Spell, 2) == GraphHand::SpellNotMagic);
    // A weapon: anything but magic or fists (Goldbrand under iRightHandType 9).
    CHECK(JudgeGraphHand(HandHeld::Weapon, 2) == GraphHand::Agrees);
    CHECK(JudgeGraphHand(HandHeld::Weapon, 8) == GraphHand::Agrees);  // a staff
    CHECK(JudgeGraphHand(HandHeld::Weapon, kGraphHandMagic) == GraphHand::WeaponAsMagic);
    CHECK(JudgeGraphHand(HandHeld::Weapon, kGraphHandFists) == GraphHand::WeaponAsFists);
    // An empty hand: only magic is clearly wrong (the load-time drop: both
    // hands empty, iRightHandType 9).
    CHECK(JudgeGraphHand(HandHeld::Empty, 0) == GraphHand::Agrees);
    CHECK(JudgeGraphHand(HandHeld::Empty, kGraphHandMagic) == GraphHand::EmptyAsMagic);
    // Not judged.
    CHECK(JudgeGraphHand(HandHeld::Other, kGraphHandMagic) == GraphHand::Agrees);
    CHECK(JudgeGraphHand(HandHeld::Other, 0) == GraphHand::Agrees);
}

TEST_CASE("graph hand: a mismatch held 1 s drawn is reported once, then its end")
{
    constexpr std::uint32_t kGoldbrand = 0x0010AA19;
    GraphHandWatch w{ kGraphMismatchOnsetSec };
    CHECK(w.Update(kGoldbrand, D, 0, GraphHand::WeaponAsMagic, 0.0).kind == GK::None);
    CHECK(w.Update(kGoldbrand, D, 0, GraphHand::WeaponAsMagic, 0.75).kind == GK::None);
    const auto onset = w.Update(kGoldbrand, D, 0, GraphHand::WeaponAsMagic, 1.0);
    CHECK(onset.kind == GK::Onset);
    CHECK(onset.state == GraphHand::WeaponAsMagic);
    CHECK(w.Update(kGoldbrand, D, 0, GraphHand::WeaponAsMagic, 5.0).kind == GK::None);
    const auto back = w.Update(kGoldbrand, D, 0, GraphHand::Agrees, 6.0);
    CHECK(back.kind == GK::Recovered);
    CHECK(back.faultSec == doctest::Approx(6.0));
}

TEST_CASE("graph hand: equipping (not settled) restarts a pending mismatch")
{
    // The caller passes Moving while IsEquipping / IsUnequipping is set, so a
    // graph a beat behind a swap is not a mismatch.
    GraphHandWatch w{ kGraphMismatchOnsetSec };
    w.Update(kEmptyHandKey, D, 0, GraphHand::EmptyAsMagic, 0.0);
    w.Update(kEmptyHandKey, M, 0, GraphHand::Agrees, 0.5);
    CHECK_FALSE(w.Pending());
    w.Update(kEmptyHandKey, D, 0, GraphHand::EmptyAsMagic, 0.75);
    CHECK(w.Update(kEmptyHandKey, D, 0, GraphHand::EmptyAsMagic, 1.5).kind == GK::None);
    CHECK(w.Update(kEmptyHandKey, D, 0, GraphHand::EmptyAsMagic, 1.75).kind == GK::Onset);
}

TEST_CASE("graph hand: a reported mismatch ends when the hand's content changes or stops being judged")
{
    constexpr std::uint32_t kFirebolt = 0x0012FD19;
    GraphHandWatch w{ kGraphMismatchOnsetSec };
    w.Update(kEmptyHandKey, D, 0, GraphHand::EmptyAsMagic, 0.0);
    CHECK(w.Update(kEmptyHandKey, D, 0, GraphHand::EmptyAsMagic, 1.0).kind == GK::Onset);
    const auto swapped = w.Update(kFirebolt, D, 0, GraphHand::Agrees, 2.0);
    CHECK(swapped.kind == GK::SwappedOut);
    CHECK(swapped.state == GraphHand::EmptyAsMagic);

    w.Update(kFirebolt, D, 0, GraphHand::SpellNotMagic, 3.0);
    CHECK(w.Update(kFirebolt, D, 0, GraphHand::SpellNotMagic, 4.0).kind == GK::Onset);
    CHECK(w.Update(0, D, 0, GraphHand::Agrees, 5.0).kind == GK::SwappedOut);  // a torch: not judged
}

TEST_CASE("graph hand: the model watch keeps its own onset")
{
    // The template change leaves FaultWatch as it was: 2 s by default.
    CHECK(FaultWatch::kOnsetSec == doctest::Approx(2.0));
    FaultWatch m;
    m.Update(kSword, D, k1st, ModelState::NotInHand, 0.0);
    CHECK(m.Update(kSword, D, k1st, ModelState::NotInHand, 1.0).kind == K::None);
    CHECK(m.Update(kSword, D, k1st, ModelState::NotInHand, 2.0).kind == K::Onset);
}
