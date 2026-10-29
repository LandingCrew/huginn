#pragma once

// =============================================================================
// CHOICE LIVE (R8.3) -- the choice score as the pipeline ranks with it
// =============================================================================
// From R8.3 the slot code ranks on mu (core/ChoiceScore.h): every candidate
// that passes the filters gets one, and ScoredCandidate::SlotScore() returns
// it. This header holds the parts of that pass that are not the score itself,
// so the pipeline and the replay test (tests/core/ChoiceReplayTests.cpp, "the
// live path") run the same code:
//
//   - the D2 legacy feature's values under [Scoring] bLegacyLearnFeature
//     (LegacyFeature; learning/legacy/LegacyLearnFeature.h's Feature() calls it);
//   - the ranking pass (ScoreForRanking): the greedy upgrade gate at the summon
//     limit, then each row's mu, variance, the three largest theta n a terms
//     (the explanation is the first) and its need part;
//   - what the allocation adds for the log (AfterAllocation): the slot feature
//     of the key a row landed on, and kappa off the page.
//
// Positions: format v1's `slot 0..7` features weigh WHERE a row sits on the
// page, which is the allocation's output, so the ranking cannot read them
// (it would rank on its own result). They enter only the logged mu and logit,
// after the allocation. The R6 bootstrap (D2) weighs none of them, so the
// ranked mu and the logged mu are the same number; ChoiceLearner warns once
// at load when a file does weigh them.
//
// kappa (the menu cost) is the learner's: it prices the held items off the
// page in a pick's choice set (core/ChoiceUpdate.h). It never enters the
// ranking, where every row is a candidate for the page.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/ChoiceScore.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Huginn::Core::Choice
{
    /// The D2 feature's two values in a row ("ln learn mult", "util missing").
    struct LegacyValues
    {
        double lnLearnMult = 0.0;
        double utilMissing = 1.0;
    };

    /// With the switch on: ln learn mult on a row the old floors kept (pass or
    /// cold start), 0 on the others; util missing 0 on a kept row, 1 on the
    /// others. With it off: 0 and 1 on every row (the model is then plain
    /// theta + b + recent; a fallback, not a supported mode: that variant fails
    /// R6's gate).
    [[nodiscard]] constexpr LegacyValues LegacyFeature(bool enabled, bool scored, double lnLearnMult) noexcept
    {
        if (!enabled || !scored) return LegacyValues{ 0.0, 1.0 };
        return LegacyValues{ lnLearnMult, 0.0 };
    }

    inline void SetLegacy(Features& f, LegacyValues v) noexcept
    {
        f[static_cast<std::size_t>(Feature::ln_learn_mult)] = v.lnLearnMult;
        f[static_cast<std::size_t>(Feature::util_missing)] = v.utilMissing;
    }

    /// The weight of the slot feature for a row on key `slot` (0..7), 0 off
    /// the page (slot < 0) or past key 7.
    [[nodiscard]] inline double SlotFeatureWeight(const Model& m, int slot) noexcept
    {
        if (slot < 0 || slot >= 8) return 0.0;
        return m.w[static_cast<std::size_t>(Feature::slot0) + static_cast<std::size_t>(slot)];
    }

    /// Does the model weigh any position feature (the ranking ignores them)?
    [[nodiscard]] inline bool WeighsSlots(const Model& m) noexcept
    {
        for (int s = 0; s < 8; ++s) {
            if (SlotFeatureWeight(m, s) != 0.0) return true;
        }
        return false;
    }

    /// One theta[k][j] n_k a_j(i) term.
    struct Term
    {
        std::int16_t need = -1;
        std::int16_t col = -1;
        double value = 0.0;
        [[nodiscard]] bool Valid() const noexcept { return need >= 0; }
    };
    using TopTerms = std::array<Term, 3>;

    /// The three largest POSITIVE terms, descending. Ties keep the lower
    /// (k, j), needs first, as Explain does, so top[0] is Explain's pair
    /// (both empty: "learned"). Unused entries are invalid.
    [[nodiscard]] inline TopTerms Top3(const Model& m, const Tick& t, const Attr& at) noexcept
    {
        TopTerms top{};
        if (m.theta.size() != K * J) return top;
        for (const std::uint16_t k : t.active) {
            const double nk = t.n[k];
            const Weight* row = &m.theta[static_cast<std::size_t>(k) * J];
            for (const auto& [j, a] : at.a) {
                const double v = row[j].mu * nk * a;
                if (!(v > 0.0)) continue;
                // first position this term beats strictly (an equal term
                // stays behind the one found first)
                std::size_t p = 0;
                while (p < top.size() && top[p].Valid() && !(v > top[p].value)) ++p;
                if (p >= top.size()) continue;
                for (std::size_t q = top.size() - 1; q > p; --q) top[q] = top[q - 1];
                top[p] = Term{ static_cast<std::int16_t>(k), static_cast<std::int16_t>(j), v };
            }
        }
        return top;
    }

    /// One row of the ranking pass.
    struct LiveScore
    {
        RowScore s;             // mu (the ranking's), var, s-hat, the explanation (= top[0])
        TopTerms top{};
        double needMu = 0.0;    // sum_j v_j a_j (+ strength): the need terms' part of mu
    };

    /// The ranking pass over `rows` rows. attrs / extras / gates may hold one
    /// entry more than `rows`: the gate's comparison entry for a_out's source
    /// when it is not a row (ChoiceScore.h ApplyUpgradeGate), never scored.
    /// extras[i].offPage must be false and the slot features 0 (positions are
    /// not known before the allocation; the header). The gate runs when
    /// `outIdx` names an entry. Every mu is finite or -inf (a NaN is ranked
    /// last, never compared as NaN).
    inline void ScoreForRanking(const Model& m, const Tick& t, std::span<const Attr> attrs, std::span<RowExtra> extras,
                                std::span<const GateRow> gates, std::size_t outIdx, double leftOut, std::size_t rows,
                                std::vector<LiveScore>& out)
    {
        out.assign(rows, LiveScore{});
        if (outIdx < attrs.size() && outIdx < extras.size()) {
            ApplyUpgradeGate(m, t, attrs, extras, gates, outIdx, leftOut);
        }
        for (std::size_t i = 0; i < rows && i < attrs.size() && i < extras.size(); ++i) {
            auto& o = out[i];
            o.s = Score(m, t, attrs[i], extras[i], false);
            o.needMu = NeedTerms(t, attrs[i]).mu;
            o.top = Top3(m, t, attrs[i]);
            o.s.exNeed = o.top[0].need;
            o.s.exCol = o.top[0].col;
            if (!(o.s.mu == o.s.mu)) {   // NaN
                o.s.mu = o.s.logit = o.s.shat = -std::numeric_limits<double>::infinity();
            }
        }
    }

    /// What the allocation adds to a ranked row for the log: the slot feature
    /// of the key it landed on (slot -1: off the page, or not known) and
    /// kappa when it is off the page.
    struct Logged
    {
        double mu = 0.0;
        double logit = 0.0;
    };
    [[nodiscard]] inline Logged AfterAllocation(const Model& m, double rankMu, bool offPage, int slot) noexcept
    {
        Logged l;
        l.mu = rankMu + SlotFeatureWeight(m, offPage ? -1 : slot);
        l.logit = l.mu - (offPage ? m.kappa : 0.0);
        return l;
    }
}
