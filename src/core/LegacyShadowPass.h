#pragma once

// =============================================================================
// LEGACY SHADOW PASS (0.24.17) -- engine A's view of a run, from const inputs
// =============================================================================
// The D2 engine "A" mirrored beside the live engine "B" (core/ShadowCompare.h
// says why). Everything that computes A's values lives here, in pure core: it
// cannot see the game, the slot state, the learners or the cosave, and it
// reads the live run only through const arguments. What it produces goes into
// one Run, which the pipeline holds as ctx.legacyShadow and copies onto the
// eligible rows for the selection log v3.
//
//   ScoreMu        A's mu: B's extras (as RankOnChoice built them, before the
//                  live pass's upgrade gate wrote into them) copied, the D2
//                  feature put back from `feat` (Choice::LegacyFeature with
//                  the switch on, per row), the same gate and score
//                  (core/ChoiceLive.h RankMu). The mu the live pass computes
//                  with [Scoring] bLegacyLearnFeature = 1.
//   RanksAndPages  each row's rank under A and under B (ShadowCompare
//                  RanksOf), and its key on A's and on B's stateless page
//                  (core/SlotAllocStateless.h), both built in place on the
//                  live allocation's input, which the caller hands over.
//
// Outside this file A's values travel only under names carrying the
// legacy-shadow prefix (the Run, the pipeline's local feature vector, the
// allocation input RanksAndPages leaves holding A's order and mu, the
// eligible rows' and the v3 rows' fields, the [Soak] tally), so
// tools/prune/r8_check.py sees every line that names one; this file and its
// host test are allowlisted. RanksAndPages builds A's page last, in place on
// `in`: the caller clears it after the call.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/ChoiceLive.h"
#include "core/ChoiceScore.h"
#include "core/ShadowCompare.h"
#include "core/SlotAllocCore.h"
#include "core/SlotAllocStateless.h"
#include "core/SlotScoreMath.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Huginn::Core::LegacyShadow
{
    /// Engine A's view of one pipeline run, index-aligned with its candidates.
    struct Run
    {
        bool ran = false;
        std::vector<double> mu;                     // A's mu
        std::vector<std::int16_t> rankA, rankB;     // rank under A and under B (1 = first; 0 = none)
        std::vector<std::int8_t> keyA, keyB0;       // key on A's and B's stateless page (-1 off; kNoKey none)

        void Clear()
        {
            ran = false;
            mu.clear();
            rankA.clear();
            rankB.clear();
            keyA.clear();
            keyB0.clear();
        }
    };

    /// A's mu over `rows` rows (see the header). `liveExtras` holds B's extras
    /// before B's gate ran (one entry past the rows when the gate compares
    /// against a_out's source); `feat[i]` is the D2 feature engine A reads on
    /// row i. Sets out.mu and out.ran.
    inline void ScoreMu(const Choice::Model& m, const Choice::Tick& t, std::span<const Choice::Attr> attrs,
                        std::span<const Choice::RowExtra> liveExtras, std::span<const Choice::GateRow> gates,
                        std::size_t outIdx, double leftOut, std::size_t rows,
                        std::span<const Choice::LegacyValues> feat, Run& out)
    {
        std::vector<Choice::RowExtra> x(liveExtras.begin(), liveExtras.end());
        for (std::size_t i = 0; i < rows && i < x.size() && i < feat.size(); ++i) Choice::SetLegacy(x[i].feat, feat[i]);
        Choice::RankMu(m, t, attrs, x, gates, outIdx, leftOut, rows, out.mu);
        out.ran = true;
    }

    /// The ranks and the stateless pages (see the header), after ScoreMu.
    /// `liveOrder` lists the rows in B's rank order; `liveMu` and `tieBreak`
    /// are index-aligned with the rows (B's mu, the DPS tie-break). `in` is
    /// the live allocation's input, handed over (nullptr: no page this run);
    /// it is left stateless and reordered. `pageRunRows[k]` is the row of its
    /// candidate k (>= rows: remembered-only, never ranked).
    inline void RanksAndPages(std::span<const std::uint32_t> liveOrder, std::span<const double> liveMu,
                              std::span<const float> tieBreak, SlotAlloc::Input* in,
                              std::span<const std::uint32_t> pageRunRows, Run& out)
    {
        const std::size_t rows = out.mu.size();
        ShadowCompare::RanksOf(out.mu, tieBreak, liveOrder, out.rankA);
        ShadowCompare::RanksOf(liveMu, tieBreak, liveOrder, out.rankB);
        out.keyA.assign(rows, ShadowCompare::kNoKey);
        out.keyB0.assign(rows, ShadowCompare::kNoKey);
        if (!in || in->candidates.size() != pageRunRows.size()) return;

        const std::size_t n = in->candidates.size();
        std::vector<double> scoresA(n, kUnrankedScore), scoresB(n, kUnrankedScore);
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint32_t r = pageRunRows[k];
            if (r >= rows) continue;   // remembered-only: unranked in both
            scoresA[k] = out.mu[r];
            scoresB[k] = in->candidates[k].score;
        }
        // B's page first, in place; `in` is then in B's stateless order
        // (orderB[j] = its index k at entry), so A's scores follow that order.
        std::vector<std::uint32_t> orderB, orderA;
        const auto pageB0 = SlotAlloc::AllocateStatelessInPlace(*in, scoresB, orderB);   // keyOf by k
        std::vector<double> scoresANow(n);
        for (std::size_t j = 0; j < n; ++j) scoresANow[j] = scoresA[orderB[j]];
        const auto pageA = SlotAlloc::AllocateStatelessInPlace(*in, scoresANow, orderA);  // keyOf by j
        // Every ranked row starts off both pages (a row the page's list left
        // out stays off); a page row takes its key.
        for (std::size_t r = 0; r < rows; ++r) {
            out.keyA[r] = -1;
            out.keyB0[r] = -1;
        }
        for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t k = orderB[j];
            const std::uint32_t r = pageRunRows[k];
            if (r >= rows) continue;
            out.keyB0[r] = pageB0.keyOf[k];
            out.keyA[r] = pageA.keyOf[j];
        }
    }
}
