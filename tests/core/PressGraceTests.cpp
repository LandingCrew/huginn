// Host tests for src/core/PressGrace.h.

#include "core/PressGrace.h"

#include <doctest/doctest.h>

#include <string>

using namespace Huginn::Core;

namespace
{
    // The 2026-10-11 08:57:16 case, in ms from an arbitrary epoch: Staff of the
    // Flame Atronach up since 13 600, key 4 down at 16 357, slot changed to
    // Scimitar at 16 628 (after the key went down), no earlier press on key 4.
    PressGraceInput ScimitarCase()
    {
        PressGraceInput in;
        in.graceMs = 600.0;
        in.pressMs = 16357.0;
        in.hasChange = true;
        in.changedAtMs = 16628.0;
        in.previousSinceMs = 13600.0;
        in.previousEmpty = false;
        in.previousSameItem = false;
        in.hasLastAction = false;
        in.previousUsable = true;
        return in;
    }
}

TEST_CASE("press grace: a change after the key went down acts on what was shown")
{
    const auto d = DecidePressGrace(ScimitarCase());
    CHECK(d.usePrevious);
    CHECK(d.reason == PressGraceReason::Grace);
    CHECK(d.shownForMs == doctest::Approx(-271.0));
}

TEST_CASE("press grace: a change just before the press is inside the window")
{
    auto in = ScimitarCase();
    in.changedAtMs = in.pressMs - 599.0;
    CHECK(DecidePressGrace(in).usePrevious);
    in.changedAtMs = in.pressMs - 600.0;   // exactly graceMs old: settled
    CHECK_FALSE(DecidePressGrace(in).usePrevious);
    CHECK(DecidePressGrace(in).reason == PressGraceReason::Settled);
    in.changedAtMs = in.pressMs - 5000.0;
    CHECK(DecidePressGrace(in).reason == PressGraceReason::Settled);
}

TEST_CASE("press grace: off at 0 or below")
{
    auto in = ScimitarCase();
    in.graceMs = 0.0;
    CHECK(DecidePressGrace(in).reason == PressGraceReason::Off);
    in.graceMs = -10.0;
    CHECK(DecidePressGrace(in).reason == PressGraceReason::Off);
}

TEST_CASE("press grace: no recorded change (fresh page) acts on what is shown")
{
    auto in = ScimitarCase();
    in.hasChange = false;
    CHECK(DecidePressGrace(in).reason == PressGraceReason::NoChange);
}

TEST_CASE("press grace: a key that was empty acts on what is shown")
{
    auto in = ScimitarCase();
    in.previousEmpty = true;
    const auto d = DecidePressGrace(in);
    CHECK_FALSE(d.usePrevious);
    CHECK(d.reason == PressGraceReason::PreviousEmpty);
}

TEST_CASE("press grace: same item on both sides is not a change")
{
    auto in = ScimitarCase();
    in.previousSameItem = true;
    CHECK(DecidePressGrace(in).reason == PressGraceReason::SameItem);
}

TEST_CASE("press grace: an unusable previous item falls back to what is shown")
{
    auto in = ScimitarCase();
    in.previousUsable = false;
    const auto d = DecidePressGrace(in);
    CHECK_FALSE(d.usePrevious);
    CHECK(d.reason == PressGraceReason::PreviousUnusable);
}

TEST_CASE("press grace: Remembrance swap-back -- pressing the key again acts on the swap-back")
{
    // Key 4 pressed at 16 738 on Scimitar (up since 16 628); the press put
    // Firebolt on the key at 16 747 (Remembrance). Pressing again at 16 900
    // must cast Firebolt, not re-equip the Scimitar.
    PressGraceInput in;
    in.graceMs = 600.0;
    in.pressMs = 16900.0;
    in.hasChange = true;
    in.changedAtMs = 16747.0;
    in.previousSinceMs = 16628.0;
    in.previousEmpty = false;
    in.hasLastAction = true;
    in.lastActionMs = 16738.0;
    in.previousUsable = true;
    const auto d = DecidePressGrace(in);
    CHECK_FALSE(d.usePrevious);
    CHECK(d.reason == PressGraceReason::OwnPress);
}

TEST_CASE("press grace: a press long before an unrelated change does not block grace")
{
    auto in = ScimitarCase();
    // The player pressed key 4 at 10 000 (on whatever was there before the
    // staff); the staff came up at 13 600, so that press is not about it.
    in.hasLastAction = true;
    in.lastActionMs = 10000.0;
    CHECK(DecidePressGrace(in).usePrevious);
    // Pressed while the staff was up, but more than kOwnPressWindowMs before
    // the change: the change is not the press's doing.
    in.lastActionMs = in.changedAtMs - kOwnPressWindowMs - 1.0;
    CHECK(DecidePressGrace(in).usePrevious);
    // Within the window: own press.
    in.lastActionMs = in.changedAtMs - kOwnPressWindowMs;
    CHECK(DecidePressGrace(in).reason == PressGraceReason::OwnPress);
}

TEST_CASE("press grace: a second quick press after a graced press stays on the previous item")
{
    // The graced press itself (at 16 738, after the change at 16 628) does not
    // count as the cause of that change: a repeat press 200 ms later, still
    // inside the window and with no further change, is graced the same way.
    auto in = ScimitarCase();
    in.pressMs = 16900.0;
    in.hasLastAction = true;
    in.lastActionMs = 16738.0;
    CHECK(DecidePressGrace(in).usePrevious);
}

TEST_CASE("press grace: reason names")
{
    CHECK(std::string(PressGraceReasonName(PressGraceReason::Grace)) == "grace");
    CHECK(std::string(PressGraceReasonName(PressGraceReason::OwnPress)) == "own press");
}

TEST_CASE("press grace: never takes an override back off the key")
{
    // An HP override landed on the key during the double-tap wait (after the
    // key went down), or 400 ms before it: the press must drink the potion.
    PressGraceInput in;
    in.graceMs = 600.0;
    in.pressMs = 10000.0;
    in.hasChange = true;
    in.previousSinceMs = 2000.0;
    in.previousEmpty = false;
    in.previousUsable = true;
    in.shownIsOverride = true;
    in.changedAtMs = 10120.0;
    CHECK_FALSE(DecidePressGrace(in).usePrevious);
    CHECK(DecidePressGrace(in).reason == PressGraceReason::ShownOverride);
    in.changedAtMs = 9600.0;
    CHECK_FALSE(DecidePressGrace(in).usePrevious);
    CHECK(DecidePressGrace(in).reason == PressGraceReason::ShownOverride);
    // Without the override the same press is graced.
    in.shownIsOverride = false;
    CHECK(DecidePressGrace(in).usePrevious);
}

TEST_CASE("press grace: grace back TO an override that just left is allowed")
{
    // The previous item was the override potion; the shown one is ordinary.
    PressGraceInput in;
    in.graceMs = 600.0;
    in.pressMs = 10000.0;
    in.hasChange = true;
    in.changedAtMs = 9800.0;
    in.previousSinceMs = 5000.0;
    in.previousEmpty = false;
    in.previousUsable = true;
    in.shownIsOverride = false;
    CHECK(DecidePressGrace(in).usePrevious);
}
