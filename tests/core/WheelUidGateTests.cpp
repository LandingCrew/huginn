// Host tests for src/core/WheelUidGate.h.

#include "core/WheelUidGate.h"

#include <doctest/doctest.h>

using namespace Huginn::Core;

TEST_CASE("wheel uid gate: only a connected Wheeler page with known instance data hides")
{
    // wheelerConnected, pageHasWheel, instanceDetailKnown
    CHECK(HidesUidlessGear({ true, true, true }));
    // Hotkey-only play (no Wheeler): today's behaviour, EquipManager equips a
    // uid-0 weapon by its base form.
    CHECK_FALSE(HidesUidlessGear({ false, false, true }));
    CHECK_FALSE(HidesUidlessGear({ false, true, true }));
    // A page with no wheel of ours (a placeholder page, a failed creation).
    CHECK_FALSE(HidesUidlessGear({ true, false, true }));
    // The second after a load: every weapon reads uid 0 until the primed
    // reconcile reads the extra lists, so uid 0 says nothing yet.
    CHECK_FALSE(HidesUidlessGear({ true, true, false }));
}

TEST_CASE("wheel uid gate: a row is kept off only when it is instance gear with no uniqueID")
{
    // hides, instanceItem, uniqueID
    CHECK(KeptOffWheelPage(true, true, 0));          // the 2026-10-12 Iron Dagger, 0001397E/uid0
    CHECK_FALSE(KeptOffWheelPage(true, true, 4));    // equipped once: it has a uniqueID now
    CHECK_FALSE(KeptOffWheelPage(true, false, 0));   // a spell or potion: the form is the identity
    CHECK_FALSE(KeptOffWheelPage(false, true, 0));   // the page does not hide (no Wheeler, or not yet)
}

TEST_CASE("wheel uid gate: the log fires on a change only, per form")
{
    KeptOffLog log;
    auto c = log.Update({ 0x1397E });
    REQUIRE(c.added.size() == 1);
    CHECK(c.added[0] == 0x1397Eu);
    CHECK(c.removed.empty());

    // The same form again, every pass: nothing to log. Repeats (two plain
    // stacks of one form) and order do not matter.
    CHECK(log.Update({ 0x1397E }).Empty());
    CHECK(log.Update({ 0x1397E, 0x1397E }).Empty());

    // A second one joins: only it is new.
    c = log.Update({ 0x12EB7, 0x1397E });
    REQUIRE(c.added.size() == 1);
    CHECK(c.added[0] == 0x12EB7u);
    CHECK(c.removed.empty());

    // The dagger gains a uniqueID (or leaves): it is the one that went.
    c = log.Update({ 0x12EB7 });
    CHECK(c.added.empty());
    REQUIRE(c.removed.size() == 1);
    CHECK(c.removed[0] == 0x1397Eu);

    // The page stops hiding: everything left goes at once.
    c = log.Update({});
    CHECK(c.added.empty());
    REQUIRE(c.removed.size() == 1);
    CHECK(c.removed[0] == 0x12EB7u);
    CHECK(log.Update({}).Empty());
}
