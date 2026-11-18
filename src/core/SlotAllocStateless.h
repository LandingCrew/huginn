#pragma once

// =============================================================================
// STATELESS PAGE (0.24.17) -- what the allocation builds from a ranking alone
// =============================================================================
// The live allocation (Core::SlotAlloc::Allocate, run by Slot::SlotAllocator)
// carries memory from run to run: the seating memory (the slot hold, home
// keys, seating), the Remembrance holds, the wildcard key's cached draw, and
// SlotLocker's locks after it. A second ranking -- the D2 engine "A" mirrored
// beside the live engine "B" (core/ShadowCompare.h) -- has none of that
// history, so its page is built here from the live allocation's own input
// with every piece of memory taken out:
//
//   kept      the layout (keys, classes, priorities, override filters), the
//             candidates (their classes, equipped flags, dedup keys, cap
//             classes), the active overrides, the [SlotLocker] settings the
//             allocation reads (class cap, fill job keys from Regular)
//   replaced  each candidate's score by `scores` (index-aligned with the live
//             input's candidates), and the candidates re-ordered by it the way
//             UtilityScorer::Rank orders the live list: RanksBefore on
//             (score, tie-break), exact ties in the live list's order
//   dropped   the seating memory (memoryAvailable false: no slot hold, home
//             keys or seating), the Remembrance holds, the wildcard (no pin,
//             no roll: the seat fills by rank like any key), the beliefs
//
// The same function on the live scores gives the live ranking's page without
// memory, which is the like-for-like control for A's page: two stateless pages
// differ only by their ranking (docs/architecture/9-selection-log-v3.md,
// "The D2 engine in shadow").
//
// Pure: standard library only (src/core/README.md). Reads nothing but its
// arguments and writes nothing but its result.
// =============================================================================

#include "core/SlotAllocCore.h"
#include "core/SlotScoreMath.h"
#include "core/SlotSnapshotIO.h"   // PageSlot, PageOf

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace Huginn::Core::SlotAlloc
{
    /// The key each live candidate sits on in a stateless page.
    struct StatelessPage
    {
        /// Index-aligned with the live input's candidates: the key that shows
        /// the candidate's item (by its own placement or an override's, the
        /// same item by form and unique ID as PipelineCoordinator::LogChoiceRows
        /// matches a page row), -1 when the page does not show it.
        std::vector<std::int8_t> keyOf;
        /// The page itself, for tests and logs: one entry per key.
        std::vector<PageSlot> page;
    };

    /// In place, on the caller's own copy of an input: take the memory out
    /// (see the header) and rank the candidates on `scores` (indexed by the
    /// candidates' order at entry; a `scores` shorter than the candidates
    /// reads the unranked score past its end). The candidates are moved, not
    /// copied. `order` receives, for the candidate now at k, its index at
    /// entry.
    inline void MakeStateless(Input& in, std::span<const double> scores, std::vector<std::uint32_t>& order)
    {
        const std::size_t n = in.candidates.size();
        auto scoreOf = [&](std::size_t i) { return i < scores.size() ? scores[i] : kUnrankedScore; };
        order.resize(n);
        std::iota(order.begin(), order.end(), std::uint32_t{ 0 });
        std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            return RanksBefore(scoreOf(a), in.candidates[a].tieBreak, scoreOf(b), in.candidates[b].tieBreak);
        });
        std::vector<CandidateRec> ranked;
        ranked.reserve(n);
        for (const std::uint32_t i : order) {
            ranked.push_back(std::move(in.candidates[i]));
            ranked.back().score = scoreOf(i);
            ranked.back().beliefRow = kNone;
        }
        in.candidates = std::move(ranked);

        in.memoryAvailable = false;     // no seating memory: no slot hold, home keys or seating
        in.generationMatches = true;    // (read only with memory; true keeps clearedAllPages false)
        in.memory = PageMemory{};
        in.holds = {};                  // no Remembrance hold
        in.wildcard = Input::WildcardIn{};   // no pinned or rolled wildcard: the seat fills by rank
        // No beliefs: the hold that reads them needs the seating memory,
        // which this input does not have.
        in.beliefs = nullptr;
    }

    /// The input Allocate runs for a stateless page of `live` ranked on
    /// `scores` (index-aligned with live's candidates): a copy, made
    /// stateless (MakeStateless). `order` as there.
    [[nodiscard]] inline Input StatelessInput(const Input& live, std::span<const double> scores,
                                              std::vector<std::uint32_t>& order)
    {
        Input in = live;
        MakeStateless(in, scores, order);
        return in;
    }

    /// The page and keys of `out`, an allocation of `in` after MakeStateless
    /// gave it `order`: keyOf is indexed by the candidates' order BEFORE
    /// MakeStateless. For a caller that allocated the stateless input itself
    /// (SlotAllocator's memoryless page, which the shadow then reuses as B's).
    [[nodiscard]] inline StatelessPage StatelessPageOf(const Input& in, const Output& out,
                                                       std::span<const std::uint32_t> order)
    {
        StatelessPage res;
        res.page = PageOf(in, out);
        res.keyOf.assign(in.candidates.size(), std::int8_t{ -1 });
        for (std::size_t k = 0; k < in.candidates.size(); ++k) {
            const auto& c = in.candidates[k];
            if (c.isRememberedOnly) continue;   // never ranked: only a hold shows it, and there is none here
            for (std::size_t s = 0; s < res.page.size(); ++s) {
                const auto& p = res.page[s];
                if (p.kind == Kind::Empty || p.formID != c.formID) continue;
                if (p.uniqueID == c.uniqueID || p.uniqueID == 0 || c.uniqueID == 0) {
                    res.keyOf[order[k]] = static_cast<std::int8_t>(std::min<std::size_t>(s, 127));
                    break;
                }
            }
        }
        return res;
    }

    /// The stateless page of `in` ranked on `scores`, made in place: `in` is
    /// left stateless with its candidates in the new order (`order`, as
    /// MakeStateless gives it). keyOf is indexed by the candidates' order AT
    /// ENTRY. For a caller that owns the input and builds more than one
    /// page from it (the pipeline's two shadow pages: no copy of 500 rows).
    [[nodiscard]] inline StatelessPage AllocateStatelessInPlace(Input& in, std::span<const double> scores,
                                                                std::vector<std::uint32_t>& order)
    {
        MakeStateless(in, scores, order);
        return StatelessPageOf(in, Allocate(in), order);
    }

    /// The stateless page of `live` ranked on `scores` (see the header); keyOf
    /// index-aligned with live's candidates. `live` is not changed.
    [[nodiscard]] inline StatelessPage AllocateStateless(const Input& live, std::span<const double> scores)
    {
        Input in = live;
        std::vector<std::uint32_t> order;
        return AllocateStatelessInPlace(in, scores, order);
    }
}
