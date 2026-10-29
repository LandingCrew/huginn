#pragma once

// =============================================================================
// CHOICE UPDATE (R8.2) -- the new learner's online update, pure
// =============================================================================
// One observed pick moves every weight its choice set informs (doc 9's P4, the
// diagonal Laplace step), in the order and form of tools/fit/fitlib.py's
// Model.info and its gradient:
//
//   P_i       = softmax over the choice set of logit_i = mu_i - kappa [off page]
//               (outcomes "picks": no outside option)
//   for each weight theta_kj (needs k with n_k != 0, recent rows with r_f != 0,
//   coefficient c = n_k or r_f; the strength block likewise; theta_covered with
//   c = 1 and x = covered):
//     E_j = sum_i P_i a_ij,  V_j = max(sum_i P_i a_ij^2 - E_j^2, 0)
//     precision_kj += c^2 V_j                 (sigma^2 = 1 / precision)
//     mu_kj        += sigma_kj^2 c (a_chosen,j - E_j)        step = variance
//   b (per item; export.item_precision's per-row form):
//     precision_b += sum_{rows of the item} P_i (1 - P_i)
//     mu_b        += sigma_b^2 sum_{rows of the item} (y_i - P_i)
// Every weight's step uses the same P (computed once, before any move), and
// the precision is raised before the step (the new variance sizes it).
// kappa and the feature weights do not move (format v1 gives them no
// precision).
//
// The choice set (ChoiceSet) is fitlib.build's, row for row: the context's
// rows then the decision's added rows; every row but the chosen one is
// skipped when its item was already taken from this context by an earlier
// pick (Plackett-Luce: co-picks are processed in order, each removing its
// item from the next pick's alternatives), when it is equipped, when it is
// neither shown, held nor eligible, or when its (form, uid) already entered.
// A row is off the page unless it was shown (or it is the chosen row of a key
// or wheel pick).
//
// The opportunity counter (OnsetCounter, GrowNeed): at a need's onset
// (rising to 0.5 with no episode open; an episode ends below 0.25, the
// episode tracker's hysteresis, core/NeedEpisodes.h) with an item on the page
// that answers it (its need term sum_j mu_kj a_j > 0), that need's theta
// variances grow by [Learning] fThetaProcessNoise. Counted per onset, not per
// second: a need that stays on adds nothing.
//
// The battery on b (BBelief): b keeps full strength for its useful life of
// play after the item's last pick (core/Battery.h, the old learner's curve),
// then fades back to its prior N(0, 0.25): the score reads
//   b_eff = r mu,   var_eff = r var + (1 - r) 0.25,   r = retention(play idle, n)
// A pick renews it from what is left (mu <- b_eff, var <- var_eff, n <- r n + 1,
// the clock restarts), as the old learner's pick does; an update that only
// passes an item over moves its stored mu and var but does not renew its life.
// A favourite in Boost mode lives fFavoriteLifeMult times longer (doc 9's open
// point: the lifetime route, no pseudo-picks).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/Battery.h"
#include "core/ChoiceScore.h"
#include "core/DecisionLog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace Huginn::Core::Choice
{
    // -------------------------------------------------------------------------
    // The choice set (fitlib.build)
    // -------------------------------------------------------------------------
    [[nodiscard]] inline std::uint64_t StackKey(std::uint32_t form, std::uint16_t uid) noexcept
    {
        return (static_cast<std::uint64_t>(uid) << 32) | form;
    }

    struct SetEntry
    {
        std::size_t row = 0;     // index into the rows given (context rows, then added rows)
        bool chosen = false;
        bool offPage = false;
    };

    /// rows: the context's rows followed by the decision's added rows.
    /// taken: the stacks earlier picks of the same context already took.
    [[nodiscard]] inline std::vector<SetEntry> ChoiceSet(std::span<const DecisionLog::Row* const> rows,
                                                         std::size_t chosen, bool keyOrWheel,
                                                         const std::set<std::uint64_t>& taken)
    {
        namespace F = DecisionLog::Flag;
        std::vector<SetEntry> out;
        std::set<std::uint64_t> seen;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const DecisionLog::Row& r = *rows[i];
            const bool isCh = i == chosen;
            const std::uint64_t key = StackKey(r.form, r.uid);
            if (!isCh) {
                if (taken.contains(key)) continue;
                if (r.flags & F::Equipped) continue;
                if (!(r.flags & (F::Shown | F::Held | F::Eligible))) continue;
                if (seen.contains(key)) continue;
            }
            seen.insert(key);
            const bool onPage = (r.flags & F::Shown) || (isCh && keyOrWheel);
            out.push_back(SetEntry{ i, isCh, !onPage });
        }
        return out;
    }

    // -------------------------------------------------------------------------
    // The update
    // -------------------------------------------------------------------------
    struct Member
    {
        Attr attr;
        double covered = 0.0;
        std::uint32_t form = 0;
        bool offPage = false;
        Features feat{};
        double bMu = 0.0;     // b as the score reads it at the decision
        double bVar = 0.25;
    };

    /// One item's b move: precision += dPrec, then mu += var * grad.
    struct BMove
    {
        std::uint32_t form = 0;
        double dPrec = 0.0;
        double grad = 0.0;
    };

    struct UpdateResult
    {
        std::vector<double> P;          // per member, before the update
        double logpChosen = 0.0;
        std::size_t weightsMoved = 0;   // theta, strength, recent and covered weights touched
        std::vector<BMove> b;           // one per distinct item, in order of first appearance
    };

    /// The members' probabilities under the model as it stands.
    [[nodiscard]] inline std::vector<double> Probabilities(const Model& m, const Tick& t, std::span<const Member> set)
    {
        std::vector<double> z(set.size());
        double mx = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < set.size(); ++i) {
            RowExtra x;
            x.covered = set[i].covered;
            x.bMu = set[i].bMu;
            x.bVar = set[i].bVar;
            x.feat = set[i].feat;
            x.offPage = set[i].offPage;
            z[i] = Score(m, t, set[i].attr, x, false).logit;
            mx = std::max(mx, z[i]);
        }
        if (m.three) mx = std::max(mx, t.u0);
        double sum = m.three ? std::exp(t.u0 - mx) : 0.0;
        for (double& v : z) {
            v = std::exp(v - mx);
            sum += v;
        }
        for (double& v : z) v /= sum;
        return z;
    }

    namespace Detail
    {
        inline void Step(Weight& w, double c, double V, double g) noexcept
        {
            const double prec = 1.0 / w.var + c * c * V;
            w.var = 1.0 / prec;
            w.mu += w.var * c * g;
        }
    }

    [[nodiscard]] inline UpdateResult Update(Model& m, const Tick& t, std::span<const Member> set, std::size_t chosen)
    {
        UpdateResult res;
        if (set.empty() || chosen >= set.size()) return res;
        res.P = Probabilities(m, t, set);
        res.logpChosen = std::log(std::max(res.P[chosen], 1e-300));

        // E_p a, E_p a^2 per column, both blocks.
        std::array<double, J> E{}, E2{}, Es{}, Es2{};
        std::array<bool, J> touched{}, touchedS{};
        for (std::size_t i = 0; i < set.size(); ++i) {
            const double p = res.P[i];
            for (const auto& [j, a] : set[i].attr.a) {
                E[j] += p * a;
                E2[j] += p * a * a;
                touched[j] = true;
            }
            for (const auto& [j, s] : set[i].attr.s) {
                Es[j] += p * s;
                Es2[j] += p * s * s;
                touchedS[j] = true;
            }
        }
        std::array<double, J> ach{}, sch{};
        for (const auto& [j, a] : set[chosen].attr.a) ach[j] = a;
        for (const auto& [j, s] : set[chosen].attr.s) sch[j] = s;
        std::vector<std::uint16_t> cols, colsS;
        std::array<double, J> V{}, G{}, Vs{}, Gs{};
        for (std::size_t j = 0; j < J; ++j) {
            if (touched[j]) {
                cols.push_back(static_cast<std::uint16_t>(j));
                V[j] = std::max(E2[j] - E[j] * E[j], 0.0);
                G[j] = ach[j] - E[j];
            }
            if (touchedS[j]) {
                colsS.push_back(static_cast<std::uint16_t>(j));
                Vs[j] = std::max(Es2[j] - Es[j] * Es[j], 0.0);
                Gs[j] = sch[j] - Es[j];
            }
        }
        const bool st = m.HasStrength() && m.strength.size() == K * J;
        for (const std::uint16_t k : t.active) {
            const double c = t.n[k];
            for (const std::uint16_t j : cols) Detail::Step(m.Th(k, j), c, V[j], G[j]);
            res.weightsMoved += cols.size();
            if (st) {
                for (const std::uint16_t j : colsS) Detail::Step(m.strength[static_cast<std::size_t>(k) * J + j], c, Vs[j], Gs[j]);
                res.weightsMoved += colsS.size();
            }
        }
        for (std::size_t f = 0; f < m.recent.size() && f < t.r.size(); ++f) {
            const double c = t.r[f];
            if (c == 0.0) continue;
            auto& rr = m.recent[f];
            for (const std::uint16_t j : cols) Detail::Step(rr.theta[j], c, V[j], G[j]);
            res.weightsMoved += cols.size();
            if (st && rr.strength.size() == J) {
                for (const std::uint16_t j : colsS) Detail::Step(rr.strength[j], c, Vs[j], Gs[j]);
                res.weightsMoved += colsS.size();
            }
        }
        // theta_covered: one weight, x = covered(i).
        {
            double Ec = 0.0, Ec2 = 0.0;
            bool any = false;
            for (std::size_t i = 0; i < set.size(); ++i) {
                const double c = set[i].covered;
                if (c == 0.0) continue;
                any = true;
                Ec += res.P[i] * c;
                Ec2 += res.P[i] * c * c;
            }
            if (any) {
                Detail::Step(m.covered, 1.0, std::max(Ec2 - Ec * Ec, 0.0), set[chosen].covered - Ec);
                ++res.weightsMoved;
            }
        }
        // b: per item, the per-row sums (export.item_precision; fitlib's bincount gradient).
        for (std::size_t i = 0; i < set.size(); ++i) {
            const std::uint32_t f = set[i].form;
            auto it = std::find_if(res.b.begin(), res.b.end(), [f](const BMove& b) { return b.form == f; });
            if (it == res.b.end()) {
                res.b.push_back(BMove{ f, 0.0, 0.0 });
                it = res.b.end() - 1;
            }
            const double p = res.P[i];
            it->dPrec += p * (1.0 - p);
            it->grad += (i == chosen ? 1.0 : 0.0) - p;
        }
        return res;
    }

    /// Apply one BMove to b's belief (mu, var).
    inline void ApplyB(double& mu, double& var, const BMove& mv) noexcept
    {
        const double prec = 1.0 / var + mv.dPrec;
        var = 1.0 / prec;
        mu += var * mv.grad;
    }

    // -------------------------------------------------------------------------
    // The battery on b
    // -------------------------------------------------------------------------
    inline constexpr double kPriorVarB = 0.25;   // sigma_b = 0.5 (fitlib.Model)

    struct BBelief
    {
        double mu = 0.0;
        double var = kPriorVarB;
        float n = 0.0f;            // the evidence (picks), as the old learner's trainCount
        double chosenAt = 0.0;     // play seconds at the last pick (at creation until one)
        bool favorite = false;     // last seen in the player's favourites
    };

    struct BRead
    {
        double mu = 0.0;
        double var = kPriorVarB;
        float retention = 1.0f;
    };

    [[nodiscard]] inline BRead ReadB(const BBelief& b, const Battery::MemoryLife& life, double now,
                                     double lifeMult = 1.0) noexcept
    {
        const float r = Battery::RetentionAt(life, now, b.chosenAt, b.n, lifeMult);
        const double rd = r;
        return BRead{ rd * b.mu, rd * b.var + (1.0 - rd) * kPriorVarB, r };
    }

    /// A pick of the item: its life restarts from the strength it has left.
    inline void RenewB(BBelief& b, const Battery::MemoryLife& life, double now, double lifeMult = 1.0) noexcept
    {
        const BRead e = ReadB(b, life, now, lifeMult);
        b.mu = e.mu;
        b.var = e.var;
        b.n = b.n * e.retention + 1.0f;
        b.chosenAt = now;
    }

    // -------------------------------------------------------------------------
    // The opportunity counter
    // -------------------------------------------------------------------------
    class OnsetCounter
    {
    public:
        static constexpr double kOnset = 0.5;    // core/NeedEpisodes.h EpisodeParams
        static constexpr double kExpiry = 0.25;

        /// The needs whose episode starts on this vector.
        [[nodiscard]] std::vector<std::uint16_t> Step(const std::array<double, K>& n)
        {
            std::vector<std::uint16_t> onsets;
            for (std::size_t k = 0; k < K; ++k) {
                if (!on_[k] && n[k] >= kOnset) {
                    on_[k] = true;
                    onsets.push_back(static_cast<std::uint16_t>(k));
                }
                else if (on_[k] && n[k] < kExpiry) {
                    on_[k] = false;
                }
            }
            return onsets;
        }
        void Reset() noexcept { on_.fill(false); }
        [[nodiscard]] bool On(std::size_t k) const noexcept { return k < K && on_[k]; }

    private:
        std::array<bool, K> on_{};
    };

    /// Does the item answer need k under the model as it stands (its need
    /// term sum_j mu_kj a_j, plus the strength block's, is above 0)?
    [[nodiscard]] inline bool Answers(const Model& m, std::size_t k, const Attr& at) noexcept
    {
        double s = 0.0;
        for (const auto& [j, a] : at.a) s += m.Th(k, j).mu * a;
        if (m.HasStrength() && m.strength.size() == K * J) {
            for (const auto& [j, x] : at.s) s += m.strength[k * J + j].mu * x;
        }
        return s > 0.0;
    }

    /// Process noise on need k's theta row (and its strength row): every
    /// variance grows by q.
    inline void GrowNeed(Model& m, std::size_t k, double q) noexcept
    {
        if (!(q > 0.0) || k >= K) return;
        for (std::size_t j = 0; j < J; ++j) m.Th(k, j).var += q;
        if (m.HasStrength() && m.strength.size() == K * J) {
            for (std::size_t j = 0; j < J; ++j) m.strength[k * J + j].var += q;
        }
    }
}
