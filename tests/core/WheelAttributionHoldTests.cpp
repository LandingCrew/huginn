// Host tests for src/core/WheelAttributionHold.h.

#include "core/WheelAttributionHold.h"

#include <doctest/doctest.h>

#include <ostream>

using Huginn::Core::WheelAttributionHold;
using Action = WheelAttributionHold::Action;

TEST_CASE("wheel hold: only an outside pick via \"Wheeler wheel\" is held")
{
    CHECK(WheelAttributionHold::ShouldHold(true, "Wheeler wheel"));
    CHECK_FALSE(WheelAttributionHold::ShouldHold(true, "own wheel"));        // the callback already named it
    CHECK_FALSE(WheelAttributionHold::ShouldHold(true, "magic menu"));
    CHECK_FALSE(WheelAttributionHold::ShouldHold(true, "vanilla hotkey"));
    CHECK_FALSE(WheelAttributionHold::ShouldHold(false, "Wheeler wheel"));   // Huginn's own source: nothing to wait for
    CHECK_FALSE(WheelAttributionHold::ShouldHold(false, "Huginn wheel"));
}

TEST_CASE("wheel hold: a pick that is not held is announced at once and confirms as before")
{
    WheelAttributionHold h;
    CHECK(h.Begin(1000, true, "magic menu") == Action::Announce);
    CHECK_FALSE(h.Held());
    CHECK(h.Tick(5000) == Action::None);
    CHECK(h.OnConsumed() == Action::Confirm);
    CHECK(h.OnHuginnSelect() == Action::None);   // a late relabel is the caller's ordinary path
}

TEST_CASE("wheel hold: Huginn's wheel callback inside the window announces it once, under Huginn's label")
{
    // LoreRim 2026-10-11 18:30:10: equip event .273, Pending .276, callback .277.
    WheelAttributionHold h;
    CHECK(h.Begin(276, true, "Wheeler wheel") == Action::None);
    CHECK(h.Held());
    CHECK(h.Tick(277) == Action::None);
    CHECK(h.OnHuginnSelect() == Action::Announce);
    CHECK_FALSE(h.Held());
    // Nothing more: no second announcement when the window would have run out.
    CHECK(h.Tick(276 + WheelAttributionHold::HOLD_MS) == Action::None);
    CHECK(h.OnHuginnSelect() == Action::None);
}

TEST_CASE("wheel hold: no callback inside the window -- the player's own wheel, announced as outside")
{
    WheelAttributionHold h;
    CHECK(h.Begin(0, true, "Wheeler wheel") == Action::None);
    CHECK(h.Tick(WheelAttributionHold::HOLD_MS - 1) == Action::None);
    CHECK(h.Tick(WheelAttributionHold::HOLD_MS) == Action::Announce);
    CHECK_FALSE(h.Held());
    CHECK(h.Tick(WheelAttributionHold::HOLD_MS + 1000) == Action::None);   // once
    // A callback after the window is a late relabel, not a second announcement.
    CHECK(h.OnHuginnSelect() == Action::None);
}

TEST_CASE("wheel hold: a count drop during the hold waits for the label, then confirms")
{
    SUBCASE("Huginn's callback resolves it")
    {
        WheelAttributionHold h;
        CHECK(h.Begin(0, true, "Wheeler wheel") == Action::None);
        CHECK(h.OnConsumed() == Action::Defer);   // not confirmed as an outside pick
        CHECK(h.OnHuginnSelect() == Action::AnnounceConfirm);
        CHECK(h.Tick(10000) == Action::None);
    }
    SUBCASE("the window resolves it")
    {
        WheelAttributionHold h;
        CHECK(h.Begin(0, true, "Wheeler wheel") == Action::None);
        CHECK(h.OnConsumed() == Action::Defer);
        CHECK(h.Tick(50) == Action::None);
        CHECK(h.Tick(WheelAttributionHold::HOLD_MS) == Action::AnnounceConfirm);
    }
}

TEST_CASE("wheel hold: Begin re-arms a reused hold")
{
    WheelAttributionHold h;
    CHECK(h.Begin(0, true, "Wheeler wheel") == Action::None);
    CHECK(h.OnConsumed() == Action::Defer);
    CHECK(h.Begin(10000, true, "Wheeler wheel") == Action::None);
    CHECK(h.Tick(10000 + WheelAttributionHold::HOLD_MS) == Action::Announce);   // the old drop is gone
}
