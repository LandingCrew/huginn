// R8.4: the row each candidate reads off the legacy pass (Legacy::Run's
// ResolveRows / RowFor / KeptFor, moved from PipelineCoordinator::ScoreCandidates
// in R8.3 and made pure here: ResolveRowOf, RowAt and KeptIndexAt in
// learning/legacy/LegacyLearnFeature.h). It decides which rows get the D2
// feature (ln learn mult) in game: a candidate reads its own row, or, when the
// floors kept another candidate of its dedup key (type, uid, form) and not it,
// the FIRST kept one's.

#include "core/ChoiceLive.h"
#include "learning/legacy/LegacyLearnFeature.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
    namespace L = Huginn::Scoring::Legacy;
    using Huginn::Core::DecisionLog::Floor;

    // The D2 feature off a row, as PipelineCoordinator::RankOnChoice reads it.
    Huginn::Core::Choice::LegacyValues Feature(const L::Row& r, bool enabled)
    {
        return Huginn::Core::Choice::LegacyFeature(enabled, r.Scored(), static_cast<double>(r.lnLearnMult));
    }

    L::Row Kept(int scoredIndex, float lm, Floor f = Floor::pass)
    {
        L::Row r;
        r.floor = f;
        r.lnLearnMult = lm;
        r.utilMissing = false;
        r.scoredIndex = scoredIndex;
        return r;
    }

    L::Row Dropped(Floor f = Floor::util)
    {
        L::Row r;
        r.floor = f;
        return r;
    }
}

TEST_CASE("legacy rows: a kept candidate reads its own row")
{
    const std::vector<std::uint64_t> keys{ 1, 2, 3 };
    const std::vector<L::Row> rows{ Kept(0, 0.5f), Kept(1, 0.25f, Floor::cold), Dropped() };
    std::vector<std::size_t> rowOf;
    L::ResolveRowOf(keys, rows, rowOf);
    REQUIRE(rowOf.size() == 3);
    CHECK(rowOf[0] == 0);
    CHECK(rowOf[1] == 1);
    CHECK(rowOf[2] == 2);   // dropped, no kept row of its key: itself
    CHECK(L::RowAt(rowOf, rows, 0).lnLearnMult == 0.5f);
    CHECK(L::RowAt(rowOf, rows, 1).floor == Floor::cold);
    CHECK_FALSE(L::RowAt(rowOf, rows, 2).Scored());
    CHECK(L::KeptIndexAt(rowOf, rows, 0, 2) == 0);
    CHECK(L::KeptIndexAt(rowOf, rows, 1, 2) == 1);
    CHECK(L::KeptIndexAt(rowOf, rows, 2, 2) == -1);
}

TEST_CASE("legacy rows: a dropped candidate reads the first kept row of its dedup key")
{
    // Keys 7 and 9 each have two candidates. Key 7: the second is kept, the
    // first dropped (the cold start scores only the first of a FORM, so a
    // later stack can be the kept one): the first reads the later row. Key 9:
    // both later candidates are kept; the dropped one reads the FIRST.
    const std::vector<std::uint64_t> keys{ 7, 7, 9, 9, 9, 4 };
    const std::vector<L::Row> rows{ Dropped(Floor::ctx), Kept(0, 1.0f), Dropped(), Kept(1, 2.0f), Kept(2, 3.0f),
                                    Dropped() };
    std::vector<std::size_t> rowOf;
    L::ResolveRowOf(keys, rows, rowOf);
    CHECK(rowOf == std::vector<std::size_t>{ 1, 1, 3, 3, 4, 5 });
    CHECK(L::RowAt(rowOf, rows, 0).lnLearnMult == 1.0f);
    CHECK(L::RowAt(rowOf, rows, 2).lnLearnMult == 2.0f);
    CHECK(L::RowAt(rowOf, rows, 4).lnLearnMult == 3.0f);   // kept itself: its own row, not the key's first
    CHECK(L::KeptIndexAt(rowOf, rows, 0, 3) == 0);
    CHECK(L::KeptIndexAt(rowOf, rows, 2, 3) == 1);
    CHECK(L::KeptIndexAt(rowOf, rows, 5, 3) == -1);
    // The D2 feature the score reads off those rows (switch on, then off)
    CHECK(Feature(L::RowAt(rowOf, rows, 0), true).lnLearnMult == 1.0);
    CHECK(Feature(L::RowAt(rowOf, rows, 0), true).utilMissing == 0.0);
    CHECK(Feature(L::RowAt(rowOf, rows, 5), true).utilMissing == 1.0);
    CHECK(Feature(L::RowAt(rowOf, rows, 0), false).lnLearnMult == 0.0);
    CHECK(Feature(L::RowAt(rowOf, rows, 0), false).utilMissing == 1.0);
}

TEST_CASE("legacy rows: short or missing rows read as an empty Row")
{
    const std::vector<std::uint64_t> keys{ 5, 5, 6 };
    const std::vector<L::Row> rows{ Kept(0, 0.75f) };   // fewer rows than candidates
    std::vector<std::size_t> rowOf;
    L::ResolveRowOf(keys, rows, rowOf);
    // Past the rows' end a candidate reads itself (as the moved code did),
    // and RowAt gives it an empty Row (floor ctx, lm 0, um 1).
    CHECK(rowOf == std::vector<std::size_t>{ 0, 1, 2 });
    CHECK_FALSE(L::RowAt(rowOf, rows, 1).Scored());
    CHECK(L::RowAt(rowOf, rows, 1).utilMissing);
    CHECK(L::RowAt(rowOf, rows, 2).lnLearnMult == 0.0f);
    // No rowOf at all (Run::Clear): RowAt falls back to the candidate's own index.
    CHECK(L::RowAt({}, rows, 0).lnLearnMult == 0.75f);
    CHECK_FALSE(L::RowAt({}, rows, 3).Scored());
}

TEST_CASE("legacy rows: a kept index the kept list does not hold reads as not kept")
{
    const std::vector<std::uint64_t> keys{ 1, 2 };
    const std::vector<L::Row> rows{ Kept(0, 0.5f), Kept(5, 0.5f) };
    std::vector<std::size_t> rowOf;
    L::ResolveRowOf(keys, rows, rowOf);
    CHECK(L::KeptIndexAt(rowOf, rows, 0, 1) == 0);
    CHECK(L::KeptIndexAt(rowOf, rows, 1, 1) == -1);   // scoredIndex 5 >= 1 kept row
    L::Row bad = Kept(-1, 0.5f);
    CHECK(L::KeptIndexAt({ 0 }, { bad }, 0, 4) == -1);
}

TEST_CASE("legacy rows: re-resolving resizes and overwrites the row map")
{
    std::vector<std::size_t> rowOf{ 9, 9, 9, 9, 9 };
    L::ResolveRowOf({ 3, 3 }, { Kept(0, 0.1f), Dropped() }, rowOf);
    CHECK(rowOf == std::vector<std::size_t>{ 0, 0 });
}
