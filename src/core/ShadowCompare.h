#pragma once

// =============================================================================
// SHADOW COMPARE (0.24.17) -- the D2 engine "A" mirrored beside the live "B"
// =============================================================================
// From 0.24.17 the live engine is B: the choice score without the D2 legacy
// feature ([Scoring] bLegacyLearnFeature = 0: ln learn mult 0 and util missing
// 1 on every row). R6's offline fit could not tell D2 ("A") from B, because
// every logged pick was made on a page the old engine built (display bias),
// so the user's call (2026-10-30) is to run B live and mirror A in the
// background: "see what it would provide as the B system goes through and
// gets tested". With [Scoring] bShadowLegacy = 1 (the default), every pipeline
// run that ranks on the choice score also computes, without touching anything
// the player sees, the slot state, the learners or the cosave:
//
//   A's mu     B's run with the D2 feature put back: the same tick, attrs,
//              covered, b and upgrade gate, and ln learn mult / util missing
//              as Choice::LegacyFeature(true, ...) gives them on the rows the
//              old floors keep -- the arithmetic the live engine runs with
//              bLegacyLearnFeature = 1 (PipelineCoordinator::RankOnChoice)
//   the ranks  each candidate's place in A's ranking and in B's (1 = first)
//   the pages  A's page and B's page built WITHOUT memory from the live
//              allocation's input (core/SlotAllocStateless.h): the same
//              layout, candidates and overrides, no slot hold, seating,
//              Remembrance hold, wildcard or lock. B's stateless page is the
//              control: A's page against B's stateless page differs only by
//              the ranking, which A's page against the live page does not
//
// They go on the eligible rows of the selection log v3 (row columns amu, ar,
// br, ak, bk; docs/architecture/9-selection-log-v3.md) and, per pick, into the
// [Soak] heartbeat's tally (ClassifyPick, Tally below). tools/shadow/shadow_compare.py
// reads the logs offline.
//
// This header is the pure part: the rank of each row under a ranking, the
// [Soak] tally of one pick, generic over the scores and keys they are given.
// It holds none of A's values. Those carry the legacy-shadow prefix in their
// names (the pipeline's run, the eligible rows' and the v3 rows' fields, the
// tally's pick and counts), and tools/prune/r8_check.py keeps
// every line that names one out of the live engine: a read of A's ranking
// anywhere but the shadow's own writer and the log fails the check.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/SlotScoreMath.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

namespace Huginn::Core::ShadowCompare
{
    /// A row with no shadow value (the shadow did not run, or the row is not a
    /// candidate of the run): the log writes null.
    inline constexpr std::int16_t kNoRank = 0;
    inline constexpr std::int8_t kNoKey = -2;    // -1 = off that page, 0..9 = its key

    /// The rank of each row (1 = first) when the rows are ordered by
    /// `score` the way UtilityScorer::Rank orders the live list: RanksBefore
    /// on (score, tie-break), exact ties in `liveOrder`'s order (the live
    /// ranking's, so a tie A cannot break is broken as B broke it). `liveOrder`
    /// lists row indices, best first; a row it does not list gets kNoRank.
    /// Index-aligned with `score` and `tieBreak`.
    inline void RanksOf(std::span<const double> score, std::span<const float> tieBreak,
                        std::span<const std::uint32_t> liveOrder, std::vector<std::int16_t>& rank)
    {
        const std::size_t n = score.size();
        rank.assign(n, kNoRank);
        std::vector<std::uint32_t> order;
        order.reserve(liveOrder.size());
        for (const std::uint32_t i : liveOrder) {
            if (i < n && i < tieBreak.size()) order.push_back(i);
        }
        std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            return RanksBefore(score[a], tieBreak[a], score[b], tieBreak[b]);
        });
        for (std::size_t p = 0; p < order.size(); ++p) {
            rank[order[p]] = static_cast<std::int16_t>(std::min<std::size_t>(p + 1, 32767));
        }
    }

    // -------------------------------------------------------------------------
    // The [Soak] tally of one pick
    // -------------------------------------------------------------------------
    /// What one pick says about A's page.
    enum class PickKind : std::uint8_t
    {
        None,     // not counted: a repeat, a nothing record, an outside pick that is not a menu
        NoData,   // a pick whose context carries no shadow (it was off, or no run of this session)
        Key,      // a Huginn key or Huginn wheel pick (the page had it)
        Reach,    // a menu reach-in: inventory, magic or favourites (the page lacked it)
    };

    struct Pick
    {
        PickKind kind = PickKind::None;
        bool onA = false;    // A's page (stateless) showed the chosen item
        bool onB0 = false;   // B's stateless page did
    };

    /// One pick, from its outcome and the chosen row's shadow keys (kNoKey on
    /// a row the run did not rank). `shadowRan`: some row of the context
    /// carries a shadow key (the run behind it ran the shadow). A chosen row
    /// with no key in such a context is not a candidate of the run, so
    /// neither page could hold it: counted, off both. A pick with no chosen
    /// row at all (`hasChosen` false) is not counted, as
    /// tools/shadow/shadow_compare.py skips it. `menuVia` is the pick's via
    /// naming a game menu (Core::Reach::IsMenuVia).
    [[nodiscard]] constexpr Pick ClassifyPick(bool nothing, bool repeat, bool keyOrWheel, bool menuVia,
                                              bool shadowRan, bool hasChosen, std::int8_t keyA,
                                              std::int8_t keyB0) noexcept
    {
        Pick p;
        if (nothing || repeat || !hasChosen || (!keyOrWheel && !menuVia)) return p;
        if (!shadowRan) {
            p.kind = PickKind::NoData;
            return p;
        }
        p.kind = keyOrWheel ? PickKind::Key : PickKind::Reach;
        p.onA = keyA >= 0;
        p.onB0 = keyB0 >= 0;
        return p;
    }

    /// The heartbeat's counts over a window.
    struct Tally
    {
        std::uint32_t keys = 0, keysOnA = 0, keysOnB0 = 0;
        std::uint32_t reach = 0, reachOnA = 0, reachOnB0 = 0, reachAOnly = 0, reachB0Only = 0;
        std::uint32_t noData = 0;

        void Add(const Pick& p) noexcept
        {
            switch (p.kind) {
            case PickKind::Key:
                ++keys;
                keysOnA += p.onA ? 1u : 0u;
                keysOnB0 += p.onB0 ? 1u : 0u;
                break;
            case PickKind::Reach:
                ++reach;
                reachOnA += p.onA ? 1u : 0u;
                reachOnB0 += p.onB0 ? 1u : 0u;
                reachAOnly += (p.onA && !p.onB0) ? 1u : 0u;
                reachB0Only += (p.onB0 && !p.onA) ? 1u : 0u;
                break;
            case PickKind::NoData: ++noData; break;
            case PickKind::None: break;
            }
        }
    };
}
