#pragma once

// =============================================================================
// WILDCARD PICK (R10, map Phase 8) -- which item the wildcard key shows
// =============================================================================
// Doc 9, "Exploration: uncertainty-ranked wildcards": a wildcard is "Huginn
// isn't sure about this one", not a random pick. Each page has ONE wildcard
// key (the seat, SeatOf below). When a roll comes up, the seat shows an item
// drawn from the ranked rows the page does not show, weighted by
//
//     w_i = rel_i * sd_i,    rel_i = logistic(mu_i - mu_cut)
//
//   mu_i    the row's choice score (core/ChoiceScore.h; a logit);
//   sd_i    its standard deviation under the diagonal posterior (sigma_i): it
//           is large for a pairing with little evidence (theta's variance
//           where an active need meets the row's effects), for an item never
//           picked or whose battery has run down (b back at its prior
//           N(0, 0.25)), for a newly learned spell (no b yet);
//   mu_cut  the score of the row the wildcard displaces (the seat's ranked
//           occupant), so rel_i is the model's own probability that the
//           player prefers i to what the key would otherwise show: logistic of
//           a logit difference is the pairwise choice probability.
//
// "Relevance x sigma" (roadmap R10) read that way: an item close to the page
// that the model is unsure of scores high; a sure loser (low mu, low sd) and a
// sure item far below the cut score low. Nothing here is hand-tuned beyond the
// two odds the [Wildcards] section already had and the random term's share:
//
//   - the random term ("plus a small random term"): the draw is a mixture,
//     with probability `uniformShare` uniform over the pool, else in
//     proportion to w. Every pool row keeps a propensity of at least
//     uniformShare / N, and the propensity stays exact (logged as `wp`); a
//     random number added INSIDE the weight would make it unknowable.
//   - the roll's chance ("wildcard chance = a base random term plus theta
//     uncertainty", doc 9): P(roll) = base + (max - base) * U with
//     U = min(1, max_i w_i / wRef). wRef = 0.5 * kSdRef is the weight of an
//     item tied with the cut (rel 0.5) carrying only the prior sd of b
//     (sqrt 0.25): an unknown item at the edge of the page. A list with
//     nothing uncertain near the page rolls at the base odds; one with an
//     unknown item at the edge rolls at the max.
//
// Rows with no sd (the fallback ranking: no bootstrap, so no posterior) weigh
// 0, and the draw is then uniform: the old behaviour, from the right pool.
//
// Pure: standard library only (src/core/README.md). The pool is the caller's
// (the slot core, core/SlotAllocCore.h: the rows the page does not show that
// the seat accepts).
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Huginn::Core::Wildcard
{
    /// The sd of an item's b at its prior, N(0, 0.25) (core/ChoiceUpdate.h
    /// kPriorVarB): an item the model knows nothing about.
    inline constexpr double kSdRef = 0.5;
    /// The weight of an item tied with the cut (rel 0.5) at that sd.
    inline constexpr double kWeightRef = 0.5 * kSdRef;

    /// The [Wildcards] odds (Globals.cpp LoadWildcardConfigFromINI).
    struct Params
    {
        double baseRoll = 0.165;     // fBaseProbability: P(roll) with nothing uncertain near the page
        double maxRoll = 0.5;        // fMaxProbability: P(roll) with an unknown item at the edge
        double uniformShare = 0.1;   // fUniformShare: the random term's share of the draw
        friend bool operator==(const Params&, const Params&) = default;
    };

    [[nodiscard]] inline double Logistic(double z) noexcept
    {
        return z >= 0.0 ? 1.0 / (1.0 + std::exp(-z)) : std::exp(z) / (1.0 + std::exp(z));
    }

    /// rel_i: the model's probability that the player prefers a row scored
    /// `mu` to one scored `muCut`. A row that cannot rank (-inf, NaN) is 0; an
    /// empty seat (no cut: muCut -inf) makes any ranked row 1.
    [[nodiscard]] inline double Relevance(double mu, double muCut) noexcept
    {
        if (!(mu == mu) || mu == -std::numeric_limits<double>::infinity()) return 0.0;
        if (!(muCut == muCut) || muCut == -std::numeric_limits<double>::infinity()) return 1.0;
        if (mu == std::numeric_limits<double>::infinity()) return 1.0;
        if (muCut == std::numeric_limits<double>::infinity()) return 0.0;
        return Logistic(mu - muCut);
    }

    /// w_i = rel_i * sd_i (a negative or NaN sd reads 0).
    [[nodiscard]] inline double Weight(double rel, double sd) noexcept
    {
        const double s = sd == sd && sd > 0.0 ? sd : 0.0;
        const double r = rel == rel && rel > 0.0 ? rel : 0.0;
        return r * s;
    }

    /// How uncertain the best of the pool is, 0..1 (header).
    [[nodiscard]] inline double Uncertainty(double topWeight) noexcept
    {
        if (!(topWeight > 0.0)) return 0.0;
        return std::min(1.0, topWeight / kWeightRef);
    }

    /// P(roll) for a pool whose largest weight is `topWeight` (header). The
    /// odds are clamped to [0, 1], and max below base reads as base.
    [[nodiscard]] inline double RollProbability(const Params& p, double topWeight) noexcept
    {
        const double base = std::clamp(p.baseRoll == p.baseRoll ? p.baseRoll : 0.0, 0.0, 1.0);
        const double max = std::clamp(p.maxRoll == p.maxRoll ? p.maxRoll : 0.0, base, 1.0);
        return base + (max - base) * Uncertainty(topWeight);
    }

    /// The draw's distribution over the pool: uniformShare / N plus
    /// (1 - uniformShare) * w_i / sum w. With no weight at all (every sd 0),
    /// uniform. Sums to 1 (empty for an empty pool).
    [[nodiscard]] inline std::vector<double> Distribution(std::span<const double> weights, double uniformShare)
    {
        std::vector<double> p(weights.size(), 0.0);
        if (weights.empty()) return p;
        const double n = static_cast<double>(weights.size());
        double sum = 0.0;
        for (const double w : weights) sum += w > 0.0 ? w : 0.0;
        const double share = sum > 0.0 ? std::clamp(uniformShare == uniformShare ? uniformShare : 1.0, 0.0, 1.0) : 1.0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            const double w = weights[i] > 0.0 ? weights[i] : 0.0;
            p[i] = share / n + (sum > 0.0 ? (1.0 - share) * w / sum : 0.0);
        }
        return p;
    }

    /// The entry a uniform `u` in [0, 1) picks (inverse CDF). A `u` at or past
    /// the total (rounding) takes the last entry with a share; npos for an
    /// empty or all-zero distribution.
    [[nodiscard]] inline std::size_t Draw(std::span<const double> probs, double u) noexcept
    {
        constexpr std::size_t npos = static_cast<std::size_t>(-1);
        std::size_t last = npos;
        double acc = 0.0;
        for (std::size_t i = 0; i < probs.size(); ++i) {
            if (!(probs[i] > 0.0)) continue;
            last = i;
            acc += probs[i];
            if (u < acc) return i;
        }
        return last;
    }

    /// The probability that the draw shows `form`: the shares of every pool
    /// entry naming it (two stacks of one weapon are two entries, and drawing
    /// either shows the same wildcard).
    [[nodiscard]] inline double FormShare(std::span<const double> probs, std::span<const std::uint32_t> forms,
                                          std::uint32_t form) noexcept
    {
        double s = 0.0;
        for (std::size_t i = 0; i < probs.size() && i < forms.size(); ++i) {
            if (forms[i] == form) s += probs[i];
        }
        return s;
    }
}
