// Host tests for src/core/SelectionGate.h: a pending selection is held while
// its equip waits at the hand equip gate, and re-timed from the equip (0.24.26).

#include "core/SelectionGate.h"

#include <doctest/doctest.h>

using namespace Huginn::Core;

TEST_CASE("selection gate: held while a press's equip waits, free after the last release")
{
    SelectionGate g;
    CHECK_FALSE(g.Held());
    g.Hold();
    CHECK(g.Held());
    CHECK(g.Release());   // the only hold: the tracker may decide again
    CHECK_FALSE(g.Held());
}

TEST_CASE("selection gate: a second press of the same item holds it across the first's release")
{
    SelectionGate g;
    g.Hold();             // the first press, parked
    g.Hold();             // the same item pressed again while the first acts
    CHECK_FALSE(g.Release());   // the first goes: still held for the second
    CHECK(g.Held());
    CHECK(g.Release());
    CHECK_FALSE(g.Held());
}

TEST_CASE("selection gate: a stray release does not go negative")
{
    SelectionGate g;
    CHECK_FALSE(g.Release());
    CHECK_FALSE(g.Held());
    g.Hold();
    CHECK(g.Held());
}

TEST_CASE("selection gate: the deadline restarts from the equip, never earlier than the press's")
{
    // Pressed at 0 with a 3000 ms window, the equip went at 2400: confirm at 5400.
    CHECK(SelectionGate::RetimedDeadline(3000.0, 2400.0, 3000.0) == doctest::Approx(5400.0));
    // An equip at once keeps the press's deadline.
    CHECK(SelectionGate::RetimedDeadline(3000.0, -5.0, 3000.0) == doctest::Approx(3000.0));
}
