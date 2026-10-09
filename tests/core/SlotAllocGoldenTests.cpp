// =============================================================================
// R7 golden test: the slot code made sign-safe gives the pages it gave before
// =============================================================================
// Three links, each checked here:
//
//  1. The core (src/core/SlotAllocCore.h) run with the OLD arithmetic
//     (LegacySlotPolicy) reproduces what the old game code did: every
//     snapshot in tests/core/fixtures/slots/*-old.txt was recorded in play by
//     SlotAllocator 0.23.9 (input, page, seating memory after, what the class
//     cap kept off), and replaying its input must give exactly that.
//  2. On the same inputs the sign-safe arithmetic (LogScorePolicy: scores =
//     ln utility, the cap as k ln d, the hold as a difference against ln m)
//     gives identical pages and memory. Checked on every recorded snapshot
//     and on synthetic ones (adversarial: ties, zeros, denormals, overrides,
//     holds, remembered-only rows, stale generations, lists past the sorted
//     prefix), with a DualPolicy run proving the decision paths agree
//     comparison by comparison.
//  3. Snapshots the NEW game code recorded (*-new.txt) replay through the
//     core with LogScorePolicy to exactly what the game showed: the adapter
//     feeds the core everything it decides on.
//
// The sort change (the scorer's top-10 partial sort becomes a full sort) is
// upstream of the slot code and is characterised separately below.
// =============================================================================

#include "LegacySlotPolicy.h"
#include "SlotAllocTestSupport.h"

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

using namespace Huginn::Test;
using Huginn::Core::SlotAlloc::Allocate;
using Huginn::Core::SlotAlloc::LogScorePolicy;

namespace
{
    bool EndsWith(const std::string& s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    struct PolicyComparison
    {
        std::size_t identical = 0;
        std::size_t differing = 0;
        std::size_t withDisagreement = 0;   // runs where some comparison went the other way
        std::size_t unexplained = 0;        // a disagreement that is not a rounding boundary
        std::vector<std::string> notes;
        Coverage cov;
    };

    /// Old vs new arithmetic on one input. Returns the legacy output (for
    /// sequences that carry the memory on).
    SA::Output CompareOnInput(const SA::Input& in, PolicyComparison& acc, const std::string& where)
    {
        const auto legacy = LegacySlotPolicy::From(in.settings);
        const auto fresh = LogScorePolicy::From(in.settings);
        const SA::Output oldOut = Allocate(in, legacy);
        const SA::Output newOut = Allocate(in, fresh);

        std::vector<Disagreement> log;
        const auto dual = DualPolicy::From(in.settings, &log);
        const SA::Output dualOut = Allocate(in, dual);
        // The dual run follows the old answers: it IS the old run.
        CHECK_MESSAGE(DiffOutputs(in, oldOut, dualOut).empty(), where);

        acc.cov.Add(in, newOut);
        const std::string diff = DiffOutputs(in, oldOut, newOut);
        if (diff.empty()) {
            ++acc.identical;
        } else {
            ++acc.differing;
        }
        if (!log.empty()) {
            ++acc.withDisagreement;
            for (const auto& d : log) {
                if (!IsRoundingBoundary(d)) {
                    ++acc.unexplained;
                    if (acc.notes.size() < 20) acc.notes.push_back(where + ": " + Describe(d));
                } else if (acc.notes.size() < 20) {
                    acc.notes.push_back(where + " (rounding boundary): " + Describe(d));
                }
            }
        } else if (!diff.empty()) {
            // No comparison went the other way, yet the pages differ: the
            // two runs must have taken the same path. Never acceptable.
            ++acc.unexplained;
            if (acc.notes.size() < 20) acc.notes.push_back(where + ": pages differ with no disagreement: " + diff);
        }
        return oldOut;
    }
}  // namespace

TEST_CASE("slot golden: recorded snapshots are present and parse")
{
    std::string error;
    const auto files = LoadSlotFixtures(&error);
    REQUIRE_MESSAGE(error.empty(), error);
    std::size_t old = 0, fresh = 0;
    for (const auto& f : files) {
        (EndsWith(f.name, "-old.txt") ? old : fresh) += f.snaps.size();
    }
    MESSAGE("recorded snapshots: ", old, " by the old game code, ", fresh, " by the new");
    CHECK(old > 0);
}

TEST_CASE("slot golden: recorded names are text (no freed registry strings captured)")
{
    // Round 1 found campaign snapshots whose names were read from freed
    // registry strings (=%CD%CD..., a NUL in 'Pretty Soul Gem'). The capture
    // now deep-copies names on the allocating thread; this keeps it honest.
    auto isText = [](const std::string& s) {
        std::size_t i = 0;
        while (i < s.size()) {
            // Printable, and valid UTF-8.
            const auto c = static_cast<unsigned char>(s[i]);
            if (c < 0x20 || c == 0x7F) return false;
            if (c < 0x80) {
                ++i;
                continue;
            }
            const std::size_t n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
            if (n == 0 || c < 0xC2 || i + n >= s.size()) return false;
            for (std::size_t k = 1; k <= n; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
            }
            i += n + 1;
        }
        return true;
    };
    std::string error;
    const auto files = LoadSlotFixtures(&error);
    REQUIRE(error.empty());
    std::size_t names = 0;
    for (const auto& f : files) {
        for (std::size_t i = 0; i < f.snaps.size(); ++i) {
            const auto& snap = f.snaps[i];
            for (const auto& c : snap.in.candidates) {
                CHECK_MESSAGE(isText(c.name), f.name, " #", i, ": candidate ", c.formID, " name ", SA::EscapeName(c.name));
                CHECK_MESSAGE(!c.name.empty(), f.name, " #", i, ": candidate ", c.formID, " has no name");
                ++names;
            }
            for (const auto& o : snap.in.overrides) {
                CHECK_MESSAGE(isText(o.name), f.name, " #", i, ": override ", o.formID, " name ", SA::EscapeName(o.name));
                ++names;
            }
        }
    }
    MESSAGE("recorded names checked: ", names);
}

TEST_CASE("slot golden: the core with the old arithmetic reproduces the old game code")
{
    std::string error;
    const auto files = LoadSlotFixtures(&error);
    REQUIRE(error.empty());
    std::size_t checked = 0;
    std::map<std::string, std::size_t> byTag;
    for (const auto& f : files) {
        if (!EndsWith(f.name, "-old.txt")) continue;
        for (std::size_t i = 0; i < f.snaps.size(); ++i) {
            const auto& snap = f.snaps[i];
            REQUIRE(snap.hasResult);
            const auto out = Allocate(snap.in, LegacySlotPolicy::From(snap.in.settings));
            const std::string diff = DiffResult(snap, snap.in, out);
            CHECK_MESSAGE(diff.empty(), f.name, " #", i, " (", snap.tag, "): ", diff);
            ++checked;
            ++byTag[snap.tag];
        }
    }
    for (const auto& [tag, n] : byTag) MESSAGE(tag, ": ", n);
    MESSAGE("old-code snapshots replayed bit for bit: ", checked);
    CHECK(checked > 0);
}

TEST_CASE("slot golden: the new game code's recorded pages replay through the core")
{
    std::string error;
    const auto files = LoadSlotFixtures(&error);
    REQUIRE(error.empty());
    std::size_t checked = 0;
    for (const auto& f : files) {
        if (!EndsWith(f.name, "-new.txt")) continue;
        for (std::size_t i = 0; i < f.snaps.size(); ++i) {
            const auto& snap = f.snaps[i];
            REQUIRE(snap.hasResult);
            const auto out = Allocate(snap.in);
            const std::string diff = DiffResult(snap, snap.in, out);
            CHECK_MESSAGE(diff.empty(), f.name, " #", i, " (", snap.tag, "): ", diff);
            ++checked;
        }
    }
    MESSAGE("new-code snapshots replayed: ", checked);
}

TEST_CASE("slot golden: old and new arithmetic give identical pages on every recorded snapshot")
{
    std::string error;
    const auto files = LoadSlotFixtures(&error);
    REQUIRE(error.empty());
    PolicyComparison acc;
    for (const auto& f : files) {
        for (std::size_t i = 0; i < f.snaps.size(); ++i) {
            const auto& snap = f.snaps[i];
            (void)CompareOnInput(snap.in, acc, std::format("{} #{} ({})", f.name, i, snap.tag));
        }
    }
    for (const auto& n : acc.notes) MESSAGE(n);
    MESSAGE("recorded: ", acc.identical, " identical, ", acc.differing, " differing, ", acc.withDisagreement,
        " with a comparison answered differently; coverage:", acc.cov.Report());
    CHECK(acc.unexplained == 0);
    CHECK(acc.differing == 0);
    CHECK(acc.withDisagreement == 0);
}

TEST_CASE("slot golden: the full sort -- what it changes against the old top-10 order (characterised)")
{
    // Not an identity: the point of the full sort is to change these pages.
    // The old scorer sorted only the top 10 (partial_sort) and the slot code
    // takes the first match in list order, so a job key (or an uncapped
    // Regular key) reaching past the prefix took whichever match came first
    // in the unsorted tail. Re-sorting each recorded play list (rank order:
    // utility, then DPS; remembered-only rows stay last) shows how often that
    // happened. Wildcard lists are left out: a wildcard's position is set
    // after the sort and cannot be re-derived from the snapshot.
    std::string error;
    const auto files = LoadSlotFixtures(&error);
    REQUIRE(error.empty());
    std::size_t considered = 0, alreadySorted = 0, differing = 0, reachedTail = 0, withWildcard = 0;
    for (const auto& f : files) {
        if (!EndsWith(f.name, "-old.txt")) continue;
        for (const auto& snap : f.snaps) {
            if (snap.tag != "tick") continue;
            const auto& in = snap.in;
            if (std::any_of(in.candidates.begin(), in.candidates.end(), [](const SA::CandidateRec& c) { return c.isWildcard; })) {
                ++withWildcard;
                continue;
            }
            ++considered;
            SA::Input sorted = in;
            std::stable_partition(sorted.candidates.begin(), sorted.candidates.end(),
                [](const SA::CandidateRec& c) { return !c.isRememberedOnly; });
            const auto ranked = std::find_if(sorted.candidates.begin(), sorted.candidates.end(),
                [](const SA::CandidateRec& c) { return c.isRememberedOnly; });
            std::stable_sort(sorted.candidates.begin(), ranked, [](const SA::CandidateRec& a, const SA::CandidateRec& b) {
                if (a.utility != b.utility) return a.utility > b.utility;
                return a.tieBreak > b.tieBreak;
            });
            const bool same = std::equal(in.candidates.begin(), in.candidates.end(), sorted.candidates.begin(),
                [](const SA::CandidateRec& a, const SA::CandidateRec& b) { return a.dedupKey == b.dedupKey && a.name == b.name; });
            const auto recorded = Allocate(in);
            const auto full = Allocate(sorted);
            if (same) {
                ++alreadySorted;
                CHECK(DiffOutputs(in, recorded, full).empty());
                continue;
            }
            if (SA::PageOf(in, recorded) != SA::PageOf(sorted, full)) {
                ++differing;
                bool tail = false;
                for (const auto& s : recorded.slots) {
                    tail = tail || ((s.kind == SA::Kind::Normal || s.kind == SA::Kind::Wildcard) && s.src >= 10);
                }
                reachedTail += tail ? 1 : 0;
            }
        }
    }
    MESSAGE("full sort on recorded play lists: ", considered, " considered (", withWildcard, " with a wildcard left out), ",
        alreadySorted, " already in full order (identical, checked), ", differing,
        " pages change; of those, the old page took an item from past the sorted top 10 in ", reachedTail);
}

TEST_CASE("slot golden: old and new arithmetic agree on synthetic snapshots (shipped settings)")
{
    // Sequences of passes on one layout, the seating memory carried from pass
    // to pass, so the hold, seating and home keys act on real history.
    PolicyComparison acc;
    std::size_t passes = 0;
    for (std::uint32_t seed = 1; seed <= 3000; ++seed) {
        SyntheticGenerator gen(seed);
        const auto settings = gen.RandomSettings();
        const auto layout = gen.RandomLayout();
        SA::PageMemory memory;
        bool generation = gen.Chance(90);
        const int n = gen.Int(2, 6);
        for (int p = 0; p < n; ++p) {
            const auto in = gen.Next(settings, layout, memory, generation);
            const auto out = CompareOnInput(in, acc, std::format("seed {} pass {}", seed, p));
            memory = out.memory;
            generation = out.generationMatches || !in.settings.keepSlotPositions;
            ++passes;
        }
    }
    for (const auto& n : acc.notes) MESSAGE(n);
    MESSAGE("synthetic (shipped settings): ", passes, " passes, ", acc.identical, " identical, ", acc.differing,
        " differing, ", acc.withDisagreement, " with a comparison answered differently; coverage:", acc.cov.Report());
    CHECK(acc.unexplained == 0);
    CHECK(acc.differing == 0);
    // Every path the allocator has was taken somewhere in the set.
    for (const char* path : { "override marked in place", "override placed", "override fallback", "override unplaced",
             "hold item not a candidate", "Remembrance hold shown", "slot with no candidate",
             "slot hold: holder gave way", "slot hold: gave way under the cap", "job key pulled from Regular",
             "returner: home", "returner: taken", "returner: waits", "returner: class",
             "class cap kept an item off", "stale generation cleared", "seatMoved", "wildcard shown",
             "remembered shown" }) {
        CHECK_MESSAGE(acc.cov.counts[path] > 0, path);
    }
}

TEST_CASE("slot golden: old and new arithmetic differ only at float-rounding boundaries (any settings)")
{
    // Any discount (0, non-powers of two), margin and switch. The old code
    // multiplied in float: d^k * u and u * m round, so where two products sit
    // within an ulp the old answer is the rounding's and the new one is the
    // exact comparison's. Every disagreement must be such a boundary.
    PolicyComparison acc;
    std::size_t passes = 0;
    for (std::uint32_t seed = 1; seed <= 3000; ++seed) {
        SyntheticGenerator gen(0x5000u + seed, SyntheticSpec{ .exoticSettings = true });
        const auto settings = gen.RandomSettings();
        const auto layout = gen.RandomLayout();
        SA::PageMemory memory;
        bool generation = gen.Chance(90);
        const int n = gen.Int(2, 6);
        for (int p = 0; p < n; ++p) {
            const auto in = gen.Next(settings, layout, memory, generation);
            const auto out = CompareOnInput(in, acc, std::format("exotic seed {} pass {}", seed, p));
            memory = out.memory;
            generation = out.generationMatches || !in.settings.keepSlotPositions;
            ++passes;
        }
    }
    for (const auto& n : acc.notes) MESSAGE(n);
    MESSAGE("synthetic (any settings): ", passes, " passes, ", acc.identical, " identical, ", acc.differing,
        " differing, ", acc.withDisagreement, " with a comparison answered differently; coverage:", acc.cov.Report());
    CHECK(acc.unexplained == 0);
}
