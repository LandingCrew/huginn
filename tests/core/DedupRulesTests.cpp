// Host tests for src/core/DedupRules.h.

#include "core/DedupRules.h"

#include <doctest/doctest.h>

#include <vector>

using namespace Huginn::Core;

namespace
{
    DedupRow Row(const char* name, bool pinned = false, bool locked = false, bool incumbent = false)
    {
        DedupRow r;
        r.name = name;
        r.empty = false;
        r.pinned = pinned;
        r.locked = locked;
        r.incumbent = incumbent;
        return r;
    }
    DedupRow Empty() { return DedupRow{}; }
}

TEST_CASE("dedup: a pinned override is never cleared for a frozen same-name key at a lower index")
{
    // Key 0 is frozen (locked, incumbent) on a different form that shares the
    // override's display name; the override sits on key 3. Before, "first
    // seen" kept key 0 and cleared the override for the whole fight.
    const std::vector<DedupRow> rows{
        Row("Potion of Restore Health", false, true, true), Row("Ice Spike", false, true, true),
        Empty(), Row("Potion of Restore Health", true, true, false) };
    const auto v = DedupDecide(rows);
    CHECK(v[3] == DedupClear::Keep);
    CHECK(v[0] == DedupClear::PinnedElsewhere);
    CHECK(v[1] == DedupClear::Keep);
}

TEST_CASE("dedup: a locked incumbent beats a newcomer at a lower index, locked or not")
{
    const std::vector<DedupRow> rows{ Row("Scimitar", false, true, false), Row("Scimitar", false, true, true) };
    const auto v = DedupDecide(rows);
    CHECK(v[0] == DedupClear::IncumbentElsewhere);
    CHECK(v[1] == DedupClear::Keep);
    const std::vector<DedupRow> unlocked{ Row("Scimitar"), Row("Scimitar", false, true, true) };
    CHECK(DedupDecide(unlocked)[0] == DedupClear::IncumbentElsewhere);
}

TEST_CASE("dedup: an unlocked copy of a locked key's name goes; otherwise the first stays")
{
    const std::vector<DedupRow> rows{ Row("Fury"), Row("Fury", false, true, false) };
    const auto v = DedupDecide(rows);
    CHECK(v[0] == DedupClear::LockedElsewhere);
    CHECK(v[1] == DedupClear::Keep);
    const std::vector<DedupRow> plain{ Row("Fury"), Row("Fury"), Row("Sparks") };
    const auto w = DedupDecide(plain);
    CHECK(w[0] == DedupClear::Keep);
    CHECK(w[1] == DedupClear::Duplicate);
    CHECK(w[2] == DedupClear::Keep);
}

TEST_CASE("dedup: two pinned copies keep the first; empty rows are ignored")
{
    const std::vector<DedupRow> rows{ Empty(), Row("Potion", true), Row("Potion", true), Empty() };
    const auto v = DedupDecide(rows);
    CHECK(v[0] == DedupClear::Keep);
    CHECK(v[1] == DedupClear::Keep);
    CHECK(v[2] == DedupClear::PinnedDuplicate);
    CHECK(v[3] == DedupClear::Keep);
}

TEST_CASE("dedup: between two pinned copies the override wins wherever it sits")
{
    auto remembered = Row("Potion of Restore Health", true, true, true);
    auto overrideRow = Row("Potion of Restore Health", true, true, false);
    overrideRow.isOverride = true;
    // Remembrance first, override second: the override still stays.
    const auto v = DedupDecide({ remembered, overrideRow });
    CHECK(v[0] == DedupClear::PinnedDuplicate);
    CHECK(v[1] == DedupClear::Keep);
    // And the other order.
    const auto w = DedupDecide({ overrideRow, remembered });
    CHECK(w[0] == DedupClear::Keep);
    CHECK(w[1] == DedupClear::PinnedDuplicate);
}
