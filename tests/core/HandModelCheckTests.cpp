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
    // Fixed by a later sheathe and draw: one line, four pose changes per cycle.
    w.Update(kStaff, M, k1st, ModelState::Ok, 21.0);
    w.Update(kStaff, S, k3rd, ModelState::Ok, 22.0);
    w.Update(kStaff, M, k1st, ModelState::Ok, 23.0);
    const auto back = w.Update(kStaff, D, k1st, ModelState::Ok, 24.0);
    CHECK(back.kind == K::Recovered);
    CHECK(back.poseChanges == 8);
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
