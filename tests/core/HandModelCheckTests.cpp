// Host tests for src/core/HandModelCheck.h: is the weapon in a hand on the
// skeleton, and when a missing one is worth a log line (0.23.44).

#include "core/HandModelCheck.h"

#include <doctest/doctest.h>

using namespace Huginn::Core;

namespace
{
    constexpr std::uint32_t kStaff = 0xFECCF01D;
    constexpr std::uint32_t kSword = 0x00013991;

    constexpr PartReading kInHand{ true, true, false, true };
    constexpr PartReading kOnSheath{ true, true, false, false };
}

TEST_CASE("hand model: judged by pose")
{
    CHECK(JudgePart(kInHand, HandPose::Drawn) == ModelState::Ok);
    CHECK(JudgePart(kOnSheath, HandPose::Sheathed) == ModelState::Ok);
    CHECK(JudgePart(kOnSheath, HandPose::Drawn) == ModelState::NotInHand);
    CHECK(JudgePart(PartReading{}, HandPose::Sheathed) == ModelState::NoPart);
    CHECK(JudgePart(PartReading{}, HandPose::Drawn) == ModelState::NoPart);
    CHECK(JudgePart({ true, false, false, false }, HandPose::Sheathed) == ModelState::Detached);
    // Culled: a fault in the hand, normal on the back (IED may cull the sheath).
    CHECK(JudgePart({ true, true, true, true }, HandPose::Drawn) == ModelState::Culled);
    CHECK(JudgePart({ true, true, true, false }, HandPose::Sheathed) == ModelState::Ok);
}

TEST_CASE("hand model: a fault is reported once it has held 2 s, then its end once")
{
    FaultWatch w;
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 0.0).kind == FaultWatch::EventKind::None);
    CHECK(w.Pending());
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 1.9).kind == FaultWatch::EventKind::None);
    const auto onset = w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 2.0);
    CHECK(onset.kind == FaultWatch::EventKind::Onset);
    CHECK(onset.state == ModelState::NoPart);
    CHECK(w.Reported());
    // Held: no second report.
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 30.0).kind == FaultWatch::EventKind::None);
    const auto back = w.Update(kStaff, HandPose::Drawn, ModelState::Ok, 31.0);
    CHECK(back.kind == FaultWatch::EventKind::Recovered);
    CHECK(back.state == ModelState::NoPart);
    CHECK(back.faultSec == doctest::Approx(31.0));
    CHECK_FALSE(w.Reported());
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::Ok, 32.0).kind == FaultWatch::EventKind::None);
}

TEST_CASE("hand model: a model still loading after a swap is not a fault")
{
    FaultWatch w;
    w.Update(kSword, HandPose::Drawn, ModelState::Ok, 0.0);
    w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 1.0);
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 2.5).kind == FaultWatch::EventKind::None);
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::Ok, 2.9).kind == FaultWatch::EventKind::None);
    CHECK_FALSE(w.Pending());
}

TEST_CASE("hand model: a draw or sheathe restarts an unreported fault, not a reported one")
{
    FaultWatch w;
    w.Update(kStaff, HandPose::Sheathed, ModelState::Ok, 0.0);
    // Drawing: not judged, whatever the part reads.
    CHECK(w.Update(kStaff, HandPose::Moving, ModelState::NotInHand, 0.5).kind == FaultWatch::EventKind::None);
    CHECK_FALSE(w.Pending());
    w.Update(kStaff, HandPose::Drawn, ModelState::NotInHand, 1.0);
    w.Update(kStaff, HandPose::Moving, ModelState::NotInHand, 2.0);  // sheathing before 2 s
    CHECK_FALSE(w.Pending());
    w.Update(kStaff, HandPose::Sheathed, ModelState::Ok, 3.0);

    // Reported, then the player sheathes and draws to fix it: the end is
    // logged with four pose changes (Moving, Sheathed, Moving, Drawn).
    w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 10.0);
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 12.0).kind == FaultWatch::EventKind::Onset);
    w.Update(kStaff, HandPose::Moving, ModelState::Ok, 13.0);
    CHECK(w.Reported());
    // Sheathed and still no part: the fault stands, no new report.
    CHECK(w.Update(kStaff, HandPose::Sheathed, ModelState::NoPart, 14.0).kind == FaultWatch::EventKind::None);
    w.Update(kStaff, HandPose::Moving, ModelState::Ok, 15.0);
    const auto back = w.Update(kStaff, HandPose::Drawn, ModelState::Ok, 16.0);
    CHECK(back.kind == FaultWatch::EventKind::Recovered);
    CHECK(back.poseChanges == 4);
    CHECK(back.faultSec == doctest::Approx(6.0));
}

TEST_CASE("hand model: a reported fault ends when its weapon leaves the hand")
{
    FaultWatch w;
    w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 0.0);
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 2.0).kind == FaultWatch::EventKind::Onset);
    const auto out = w.Update(kSword, HandPose::Drawn, ModelState::Ok, 5.0);
    CHECK(out.kind == FaultWatch::EventKind::SwappedOut);
    CHECK(out.state == ModelState::NoPart);
    CHECK(out.faultSec == doctest::Approx(5.0));
    CHECK_FALSE(w.Reported());

    // Emptied hand (a spell, fists): ends it too.
    w.Update(kStaff, HandPose::Drawn, ModelState::Culled, 10.0);
    w.Update(kStaff, HandPose::Drawn, ModelState::Culled, 12.0);
    CHECK(w.Update(0, HandPose::Drawn, ModelState::Ok, 13.0).kind == FaultWatch::EventKind::SwappedOut);
}

TEST_CASE("hand model: the clock runs across a change of fault on one weapon")
{
    FaultWatch w;
    w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 0.0);
    w.Update(kStaff, HandPose::Drawn, ModelState::Detached, 1.0);
    const auto onset = w.Update(kStaff, HandPose::Drawn, ModelState::NotInHand, 2.0);
    CHECK(onset.kind == FaultWatch::EventKind::Onset);
    CHECK(onset.state == ModelState::NotInHand);
}

TEST_CASE("hand model: reset forgets everything")
{
    FaultWatch w;
    w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 0.0);
    w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 2.0);
    w.Reset();
    CHECK_FALSE(w.Reported());
    // The same weapon after the load: a new fault, a new clock.
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 3.0).kind == FaultWatch::EventKind::None);
    CHECK(w.Update(kStaff, HandPose::Drawn, ModelState::NoPart, 5.0).kind == FaultWatch::EventKind::Onset);
}
