#pragma once

// =============================================================================
// CHOICE BELIEFS (R9) -- one ranking run's beliefs, for the challenger rule
// =============================================================================
// The slot hold (core/SlotAllocCore.h, PASS 1c) asks, for a challenger c and
// the holder i of one key, sigma_Delta(c, i): the spread of mu_c - mu_i under
// the learner's diagonal posterior (core/ChallengerRule.h has the rule):
//
//   sigma_Delta^2 = sum_j (a_j(c) - a_j(i))^2 W_j + sum_j (s_j(c) - s_j(i))^2 Ws_j
//                 + (covered(c) - covered(i))^2 var(theta_covered)
//                 + var(b_c) + var(b_i)
//
// with W_j = sum_k n_k^2 var(theta_kj) (+ recent) the tick's column variances
// (core/ChoiceScore.h, MakeTick). A weight the two rows share enters through
// the DIFFERENCE of their attributes, so it cancels where they agree; the
// features (the D2 legacy feature, positions) are fixed weights with no
// variance and do not enter. Each row's own sigma^2 (RowScore::var) is the
// same sum against an empty row with var(b) = 0: Var() below.
//
// PipelineCoordinator::RankOnChoice fills one table per run after the ranking
// pass (covered after the upgrade gate, as the score read it) and hands it to
// every row it ranked (ScoredCandidate::ChoiceParts::beliefs, with the row's
// index); the slot code reads it through Challenger::BeliefSource only.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/ChallengerRule.h"
#include "core/ChoiceScore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace Huginn::Core::Choice
{
    class BeliefTable final : public Challenger::BeliefSource
    {
    public:
        struct Row
        {
            Attr attr;
            double covered = 0.0;
            double bVar = 0.0;
        };

        BeliefTable() = default;

        /// The tick's column variances and theta_covered's variance.
        BeliefTable(const Tick& t, double varCovered) : m_W(t.W), m_Ws(t.Ws), m_varCovered(varCovered) {}

        void Reserve(std::size_t n) { m_rows.reserve(n); }

        /// Adds a row; returns its index.
        std::uint32_t Add(Attr attr, double covered, double bVar)
        {
            m_rows.push_back(Row{ std::move(attr), covered, bVar });
            return static_cast<std::uint32_t>(m_rows.size() - 1);
        }

        [[nodiscard]] std::size_t Size() const noexcept { return m_rows.size(); }
        [[nodiscard]] const Row& At(std::uint32_t r) const { return m_rows[r]; }

        /// sigma_Delta^2 of rows a and b (header); NaN for a row out of range.
        [[nodiscard]] double DeltaVar(std::uint32_t a, std::uint32_t b) const noexcept override
        {
            if (a >= m_rows.size() || b >= m_rows.size()) return std::numeric_limits<double>::quiet_NaN();
            const Row& x = m_rows[a];
            const Row& y = m_rows[b];
            const double dc = x.covered - y.covered;
            return Choice::DeltaVar(m_W, m_Ws, x.attr, y.attr, x.bVar, y.bVar) + dc * dc * m_varCovered;
        }

        /// A row's own sigma^2: RowScore::var for that row.
        [[nodiscard]] double Var(std::uint32_t r) const noexcept
        {
            if (r >= m_rows.size()) return std::numeric_limits<double>::quiet_NaN();
            const Row& x = m_rows[r];
            const Attr empty{};   // two empty vectors: no allocation
            return Choice::DeltaVar(m_W, m_Ws, x.attr, empty, x.bVar, 0.0) + x.covered * x.covered * m_varCovered;
        }

    private:
        std::array<double, J> m_W{};
        std::array<double, J> m_Ws{};
        double m_varCovered = 0.0;
        std::vector<Row> m_rows;
    };
}  // namespace Huginn::Core::Choice
