#pragma once

// =============================================================================
// CHOICE SCORE (R8.2) -- the new model's score of one item in one situation
// =============================================================================
// Doc 9's conditional logit as the R8 bootstrap file describes it
// (docs/architecture/9-bootstrap-format.md, "The score"), in the build's own
// need and column order (core/Bootstrap.h maps the file's names here):
//
//   v_j      = sum_k n_k theta[k][j] + sum_f r_f theta_f[j] + theta_bias[j]   once per tick
//   mu_i     = sum_j v_j a_j(i) + sum_j vs_j s_j(i)                           the need terms
//            + theta_covered * covered(i)                                     doc 11 step 5
//            + b_i + sum_f w_f feature_f(i)
//   logit_i  = mu_i - kappa * [i is off the page]
//   sigma_i^2 = sum_j a_j(i)^2 W_j + sum_j s_j(i)^2 Ws_j + covered(i)^2 var_cov + var_b(i)
//            with W_j = sum_k n_k^2 var[k][j] + sum_f r_f^2 var_f[j]           (the diagonal approximation)
//
// a_j(i) is the item's value in column j as the file's grading reads it, plus
// the runtime cross-feature that names column j (DecisionLog::kCrossColumns):
//   graded              a_j = cap_j + x_j
//   presence            a_j = 1[cap_j > 0] + x_j
//   presence+strength   a_j = 1[cap_j > 0] + x_j,  s_j = (cap_j - 0.5) 1[cap_j > 0]
// (s is empty for the other two). A cross-feature adds to the theta block only.
//
// The explanation of a row (R8 "Done when": the label is the largest term):
// the single largest theta[k][j] n_k a_j(i) over the needs k and columns j, b,
// recent, covered and the features excluded. Ties go to the lower (k, j),
// needs first, in the build's order; a row with no positive term is
// "learned" (exNeed = exCol = -1).
//
// recent_<family> (RecentTracker below) follows fitlib.recent_families: the
// latest usable pick strictly before the decision's time, in the same load
// generation; r_f = exp(-age / tau_f) when age <= window and the pick's cap has
// the family's column > 0, else 0.
//
// Every sum is in double: the replay test (tests/core/ChoiceReplayTests.cpp)
// reproduces fitlib's per-row logits to 1e-5.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/DecisionLog.h"
#include "core/EffectColumns.h"
#include "core/EffectMapper.h"
#include "core/NeedIds.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Huginn::Core::Choice
{
    inline constexpr std::size_t K = Needs::kNeedCount;
    inline constexpr std::size_t J = Effect::kColumnCount;

    enum class Grading : std::uint8_t { Graded, Presence, PresenceStrength };

    /// The extra row features of format v1 (`features[].name`), in the
    /// format doc's order. A feature the file does not weigh has w = 0.
    enum class Feature : std::uint8_t
    {
        ln_util,
        util_missing,
        slot0, slot1, slot2, slot3, slot4, slot5, slot6, slot7,
        ln_learn_mult,
        ln_fav,
        ln_corr,
        ln_potion,
        ln_learner,
        eligible,
        _Count
    };
    inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::_Count);
    inline constexpr std::array<std::string_view, kFeatureCount> kFeatureNames{
        "ln util", "util missing", "slot 0", "slot 1", "slot 2", "slot 3", "slot 4", "slot 5", "slot 6", "slot 7",
        "ln learn mult", "ln fav", "ln corr", "ln potion", "ln learner", "eligible",
    };
    using Features = std::array<double, kFeatureCount>;

    /// One weight's diagonal Gaussian belief: mean and variance (1 / precision).
    struct Weight
    {
        double mu = 0.0;
        double var = 1.0;
    };

    /// One recent_<family> feature: its row of theta (and strength), and its
    /// outside-option weight with outcomes "three".
    struct RecentRow
    {
        std::uint16_t col = 0;          // the family's column (build index)
        double tau = 0.0;               // seconds
        std::vector<Weight> theta;      // J
        std::vector<Weight> strength;   // J with presence+strength, else empty
        double u0 = 0.0;
    };

    /// The model in the build's index space (core/Bootstrap.h builds it).
    struct Model
    {
        Grading grading = Grading::Graded;
        std::vector<Weight> theta;      // K*J, need-major: theta[k*J + j]
        std::vector<Weight> strength;   // K*J with presence+strength, else empty
        std::vector<double> bias;       // theta_bias: J, or empty (fixed: format v1 gives it no precision)
        double window = 0.0;            // recent.window_s
        std::vector<double> taus;       // recent.taus_s
        std::vector<RecentRow> recent;
        Weight covered{ 0.0, 1.0 };     // theta_covered (cross-feature 7, doc 11 step 5)
        double kappa = 0.0;             // fixed: format v1 gives it no precision
        Features w{};                   // fixed: format v1 gives them no precision
        bool three = false;             // outcomes "three": u0 below is the outside option
        double u0Bias = 0.0;
        std::array<double, K> u0Need{};

        [[nodiscard]] Weight& Th(std::size_t k, std::size_t j) noexcept { return theta[k * J + j]; }
        [[nodiscard]] const Weight& Th(std::size_t k, std::size_t j) const noexcept { return theta[k * J + j]; }
        [[nodiscard]] bool HasStrength() const noexcept { return grading == Grading::PresenceStrength; }
    };

    /// A row's non-zero attribute values, ascending by column.
    struct Attr
    {
        std::vector<std::pair<std::uint16_t, double>> a;   // the theta block
        std::vector<std::pair<std::uint16_t, double>> s;   // the strength block (presence+strength)
    };

    using SparseCol = std::pair<std::uint16_t, double>;

    /// a(i) and s(i) from a cap and the cross-features that name columns, as
    /// fitlib builds them (a repeated cap column: the last value wins, as
    /// CapMat[g, j] = v does; cross-features add to a_j only).
    [[nodiscard]] inline Attr MakeAttr(Grading g, std::span<const SparseCol> cap, std::span<const SparseCol> crossCols)
    {
        // Sparse merge (no dense J arrays: this runs for every row of every
        // pipeline run). Entries: (col, order, cap?, value); sorted by column,
        // then by input order, so a repeated cap column keeps its last value.
        struct E
        {
            std::uint16_t col;
            std::uint16_t order;
            bool isCap;
            double v;
        };
        // A cap has a handful of columns: a stack buffer, a heap one past it
        // (Debug builds pay a checked allocation per container).
        constexpr std::size_t kStack = 48;
        std::array<E, kStack> stackBuf{};
        std::vector<E> heapBuf;
        const std::size_t total = cap.size() + crossCols.size();
        E* es = stackBuf.data();
        if (total > kStack) {
            heapBuf.resize(total);
            es = heapBuf.data();
        }
        std::size_t count = 0;
        std::uint16_t order = 0;
        for (const auto& [j, v] : cap) {
            if (j < J) es[count++] = E{ j, order++, true, v };
        }
        for (const auto& [j, v] : crossCols) {
            if (j < J) es[count++] = E{ j, order++, false, v };
        }
        std::sort(es, es + count, [](const E& a, const E& b) { return a.col != b.col ? a.col < b.col : a.order < b.order; });
        Attr out;
        out.a.reserve(count);
        for (std::size_t i = 0; i < count;) {
            const std::uint16_t j = es[i].col;
            double c = 0.0, x = 0.0;
            for (; i < count && es[i].col == j; ++i) {
                if (es[i].isCap) c = es[i].v;
                else x += es[i].v;
            }
            const double base = g == Grading::Graded ? c : (c > 0.0 ? 1.0 : 0.0);
            const double a = base + x;
            if (a != 0.0) out.a.emplace_back(j, a);
            if (g == Grading::PresenceStrength && c > 0.0) {
                const double sv = c - 0.5;
                if (sv != 0.0) out.s.emplace_back(j, sv);
            }
        }
        return out;
    }

    /// The same from the catalog's cap and a logged row's cross-features.
    [[nodiscard]] inline Attr MakeAttr(Grading g, const Effect::Cap* cap,
                                       const std::array<float, DecisionLog::kCrossCount>& cross)
    {
        constexpr std::size_t kStack = 40;
        std::array<SparseCol, kStack> cbuf{};
        std::vector<SparseCol> cheap;
        std::size_t nc = 0;
        SparseCol* c = cbuf.data();
        if (cap && cap->size() > kStack) {
            cheap.resize(cap->size());
            c = cheap.data();
        }
        if (cap) {
            for (const auto& v : *cap) c[nc++] = SparseCol{ static_cast<std::uint16_t>(v.col), static_cast<double>(v.v) };
        }
        std::array<SparseCol, DecisionLog::kCrossCount> x{};
        std::size_t nx = 0;
        for (std::size_t i = 0; i < DecisionLog::kCrossCount; ++i) {
            const auto col = DecisionLog::kCrossColumns[i];
            if (col == Effect::Col::_Count || cross[i] == 0.0f || !std::isfinite(cross[i])) continue;
            x[nx++] = SparseCol{ static_cast<std::uint16_t>(col), static_cast<double>(cross[i]) };
        }
        return MakeAttr(g, std::span<const SparseCol>(c, nc), std::span<const SparseCol>(x.data(), nx));
    }

    /// What is computed once per tick: the need vector, the recent values and
    /// the column vectors every row's score reads.
    struct Tick
    {
        std::array<double, K> n{};
        std::vector<double> r;            // per Model::recent
        std::array<double, J> v{}, W{};   // theta block: sum_k n_k mu_kj (+recent, +bias); sum_k n_k^2 var_kj (+recent)
        std::array<double, J> vs{}, Ws{}; // strength block
        std::vector<std::uint16_t> active;  // needs with n_k != 0, ascending (the explanation's search)
        double u0 = -std::numeric_limits<double>::infinity();   // the outside option (outcomes "three")
    };

    [[nodiscard]] inline Tick MakeTick(const Model& m, const std::array<double, K>& n, std::span<const double> r)
    {
        Tick t;
        t.n = n;
        t.r.assign(r.begin(), r.end());
        t.r.resize(m.recent.size(), 0.0);
        const bool st = m.HasStrength() && m.strength.size() == K * J;
        // A model without its K*J theta block (default-constructed) scores the
        // theta part as 0 rather than reading past an empty vector.
        const bool th = m.theta.size() == K * J;
        for (std::size_t k = 0; k < K && th; ++k) {
            const double nk = n[k];
            if (nk == 0.0) continue;
            t.active.push_back(static_cast<std::uint16_t>(k));
            const double n2 = nk * nk;
            const Weight* row = &m.theta[k * J];
            for (std::size_t j = 0; j < J; ++j) {
                t.v[j] += nk * row[j].mu;
                t.W[j] += n2 * row[j].var;
            }
            if (st) {
                const Weight* srow = &m.strength[k * J];
                for (std::size_t j = 0; j < J; ++j) {
                    t.vs[j] += nk * srow[j].mu;
                    t.Ws[j] += n2 * srow[j].var;
                }
            }
        }
        for (std::size_t f = 0; f < m.recent.size(); ++f) {
            const double rf = t.r[f];
            if (rf == 0.0) continue;
            const auto& rr = m.recent[f];
            for (std::size_t j = 0; j < J && j < rr.theta.size(); ++j) {
                t.v[j] += rf * rr.theta[j].mu;
                t.W[j] += rf * rf * rr.theta[j].var;
            }
            if (st) {
                for (std::size_t j = 0; j < J && j < rr.strength.size(); ++j) {
                    t.vs[j] += rf * rr.strength[j].mu;
                    t.Ws[j] += rf * rf * rr.strength[j].var;
                }
            }
        }
        if (m.bias.size() == J) {
            for (std::size_t j = 0; j < J; ++j) t.v[j] += m.bias[j];
        }
        if (m.three) {
            double u = m.u0Bias;
            for (std::size_t k = 0; k < K; ++k) u += m.u0Need[k] * n[k];
            for (std::size_t f = 0; f < m.recent.size(); ++f) u += m.recent[f].u0 * t.r[f];
            t.u0 = u;
        }
        return t;
    }

    /// One row's score.
    struct RowScore
    {
        double mu = 0.0;      // the item score (no kappa)
        double var = 0.0;     // sigma_i^2 under the diagonal approximation
        double shat = 0.0;    // mu without the covered term (doc 11's s-hat, the upgrade gate's)
        double logit = 0.0;   // mu - kappa * off page
        std::int16_t exNeed = -1;   // the explanation pair; -1 / -1 = "learned"
        std::int16_t exCol = -1;
        [[nodiscard]] double Sd() const noexcept { return std::sqrt(std::max(var, 0.0)); }
        [[nodiscard]] bool Learned() const noexcept { return exNeed < 0; }
    };

    /// What a row brings besides its attributes.
    struct RowExtra
    {
        double covered = 0.0;   // covered(i), after the upgrade gate where it applies
        double bMu = 0.0;       // b_i (0 for an item with no entry)
        double bVar = 0.0;      // its variance (the prior's for an item with no entry)
        Features feat{};        // the extra row features' values
        bool offPage = false;
    };

    /// The need terms of one row: sum_j v_j a_j + sum_j vs_j s_j, and its
    /// variance part, without covered, b or the features.
    struct NeedPart
    {
        double mu = 0.0;
        double var = 0.0;
    };

    [[nodiscard]] inline NeedPart NeedTerms(const Tick& t, const Attr& at) noexcept
    {
        NeedPart p;
        for (const auto& [j, a] : at.a) {
            p.mu += t.v[j] * a;
            p.var += a * a * t.W[j];
        }
        for (const auto& [j, s] : at.s) {
            p.mu += t.vs[j] * s;
            p.var += s * s * t.Ws[j];
        }
        return p;
    }

    /// The explanation: the largest theta[k][j] n_k a_j (header).
    inline void Explain(const Model& m, const Tick& t, const Attr& at, RowScore& out) noexcept
    {
        double best = 0.0;
        out.exNeed = out.exCol = -1;
        for (const std::uint16_t k : t.active) {
            const double nk = t.n[k];
            const Weight* row = &m.theta[static_cast<std::size_t>(k) * J];
            for (const auto& [j, a] : at.a) {
                const double term = row[j].mu * nk * a;
                if (term > best) {
                    best = term;
                    out.exNeed = static_cast<std::int16_t>(k);
                    out.exCol = static_cast<std::int16_t>(j);
                }
            }
        }
    }

    [[nodiscard]] inline double FeatureTerm(const Model& m, const Features& f) noexcept
    {
        double s = 0.0;
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            if (m.w[i] != 0.0) s += m.w[i] * f[i];
        }
        return s;
    }

    [[nodiscard]] inline RowScore Score(const Model& m, const Tick& t, const Attr& at, const RowExtra& x,
                                        bool explain = true) noexcept
    {
        RowScore r;
        const NeedPart p = NeedTerms(t, at);
        r.shat = p.mu + x.bMu + FeatureTerm(m, x.feat);
        r.mu = r.shat + m.covered.mu * x.covered;
        r.var = p.var + x.covered * x.covered * m.covered.var + x.bVar;
        r.logit = r.mu - (x.offPage ? m.kappa : 0.0);
        if (explain) Explain(m, t, at, r);
        return r;
    }

    // -------------------------------------------------------------------------
    // The greedy upgrade gate (doc 11 section 2.3, decision 2)
    // -------------------------------------------------------------------------
    // At the summon limit, a summon i that is not the source of a_out (the
    // summon a new cast would dismiss: the oldest) reads
    //     covered_summon(i) = (1 - logistic(Delta / sigma_Delta)) * left(r_out),
    // Delta = s-hat(i) - s-hat(a_out's source), s-hat the score without the
    // covered term, sigma_Delta^2 the variance of that difference under the
    // diagonal posterior: sum_j (a_j(i) - a_j(o))^2 W_j (+ strength) + var_b(i)
    // + var_b(o). A close call reads ~0.5 covered, a clear upgrade near 0, a
    // clear downgrade near 1. Recasting a_out's own source is a refresh:
    // covered 1 * left(r_out), as Coverage::Covered reads it.

    [[nodiscard]] inline double DeltaVar(const Tick& t, const Attr& i, const Attr& o, double bVarI, double bVarO) noexcept
    {
        double v = bVarI + bVarO;
        auto block = [&](const std::vector<SparseCol>& x, const std::vector<SparseCol>& y, const std::array<double, J>& W) {
            std::size_t p = 0, q = 0;
            while (p < x.size() || q < y.size()) {
                std::uint16_t j;
                double d;
                if (q >= y.size() || (p < x.size() && x[p].first < y[q].first)) {
                    j = x[p].first;
                    d = x[p].second;
                    ++p;
                }
                else if (p >= x.size() || y[q].first < x[p].first) {
                    j = y[q].first;
                    d = -y[q].second;
                    ++q;
                }
                else {
                    j = x[p].first;
                    d = x[p].second - y[q].second;
                    ++p;
                    ++q;
                }
                v += d * d * W[j];
            }
        };
        block(i.a, o.a, t.W);
        block(i.s, o.s, t.Ws);
        return v;
    }

    [[nodiscard]] inline double Logistic(double z) noexcept
    {
        return z >= 0.0 ? 1.0 / (1.0 + std::exp(-z)) : std::exp(z) / (1.0 + std::exp(z));
    }

    /// covered_summon(i) for a non-source summon at the limit.
    [[nodiscard]] inline double UpgradeCovered(double shatI, double shatOut, double deltaVar, double leftOut) noexcept
    {
        const double sd = std::sqrt(std::max(deltaVar, 0.0));
        const double delta = shatI - shatOut;
        double g;
        if (sd > 0.0) g = 1.0 - Logistic(delta / sd);
        else g = delta > 0.0 ? 0.0 : (delta < 0.0 ? 1.0 : 0.5);   // a certain posterior: a step
        return g * leftOut;
    }

    /// One row's part in the gate: `summon` when its cap has a summon count
    /// column, the summon count is at the limit, and it is not a_out's own
    /// source (a refresh keeps its count-covered value); `other` the best
    /// value of its other columns (covered never falls below it).
    struct GateRow
    {
        bool summon = false;
        double other = 0.0;
    };

    /// Rewrites x[i].covered for every gated row against entry `out` (a_out's
    /// source: a row, or an entry the caller appends for a source that is not
    /// one -- filtered at the limit -- whose own covered is never read; with
    /// no such entry nothing changes and the count stays).
    inline void ApplyUpgradeGate(const Model& m, const Tick& t, std::span<const Attr> attrs, std::span<RowExtra> x,
                                 std::span<const GateRow> g, std::size_t out, double leftOut)
    {
        if (out >= attrs.size() || out >= x.size()) return;
        const double shatOut = Score(m, t, attrs[out], x[out], false).shat;
        for (std::size_t i = 0; i < attrs.size() && i < x.size() && i < g.size(); ++i) {
            if (!g[i].summon || i == out) continue;
            const double shat = Score(m, t, attrs[i], x[i], false).shat;
            const double dv = DeltaVar(t, attrs[i], attrs[out], x[i].bVar, x[out].bVar);
            x[i].covered = std::max(g[i].other, UpgradeCovered(shat, shatOut, dv, leftOut));
        }
    }

    // -------------------------------------------------------------------------
    // recent_<family>: the live tracker (fitlib.recent_families)
    // -------------------------------------------------------------------------
    /// The LoreRim tome-learning spell's name (fitlib.TRANSIENT) and the
    /// SimonRim test list (fitlib.SIMONRIM): picks of either never count.
    inline constexpr std::string_view kTransientName = "Arcane Anchor";
    inline constexpr std::string_view kSimonRimList = "simonrim-essentails";

    /// Is a pick usable for recent_<family> (and for the update)? fitlib's
    /// test: an outcome other than nothing, a context, a chosen row, not the
    /// transient spell, not the SimonRim test list.
    [[nodiscard]] inline bool UsablePick(bool nothing, bool hasContext, bool hasChosenRow, std::string_view name,
                                         std::string_view list) noexcept
    {
        return !nothing && hasContext && hasChosenRow && name != kTransientName && list != kSimonRimList;
    }

    /// The decision moment of a pick: its confirmation time less the
    /// confirmation delay (fitlib.pick_time: utc - confirmMs). Seconds.
    [[nodiscard]] constexpr double PickTime(double confirmedAt, double confirmMs) noexcept
    {
        return confirmedAt - confirmMs / 1000.0;
    }

    struct RecentPick
    {
        double t = 0.0;                   // the decision moment (seconds, any epoch shared with the queries)
        std::uint32_t gen = 0;            // the load generation (one launch: the process)
        std::uint64_t id = 0;             // the decision's identity (excluded from its own query)
        bool hasCap = false;              // the chosen row had a cap (else every r is 0)
        std::vector<std::uint16_t> cols;  // the columns with cap > 0
    };

    class RecentTracker
    {
    public:
        static constexpr std::size_t kKeep = 64;

        /// Adds a usable pick (the caller tests UsablePick). Kept in time order.
        void Add(RecentPick p)
        {
            auto it = std::upper_bound(picks_.begin(), picks_.end(), p.t,
                                       [](double t, const RecentPick& q) { return t < q.t; });
            picks_.insert(it, std::move(p));
            if (picks_.size() > kKeep) picks_.erase(picks_.begin());
        }

        void Reset() noexcept { picks_.clear(); }
        [[nodiscard]] std::size_t Size() const noexcept { return picks_.size(); }

        /// The latest pick strictly before t in generation gen, not `exclude`.
        [[nodiscard]] const RecentPick* LatestBefore(double t, std::uint32_t gen, std::uint64_t exclude) const noexcept
        {
            for (auto it = picks_.rbegin(); it != picks_.rend(); ++it) {
                if (!(it->t < t) || it->gen != gen || it->id == exclude) continue;
                return &*it;
            }
            return nullptr;
        }

        /// r_f for every recent row of the model at time t (0 when none).
        [[nodiscard]] std::vector<double> Values(const Model& m, double t, std::uint32_t gen,
                                                 std::uint64_t exclude = UINT64_MAX) const
        {
            return ValuesFor(m, LatestBefore(t, gen, exclude), t);
        }

        [[nodiscard]] static std::vector<double> ValuesFor(const Model& m, const RecentPick* p, double t)
        {
            std::vector<double> r(m.recent.size(), 0.0);
            if (!p || !p->hasCap) return r;
            const double age = t - p->t;
            if (age > m.window) return r;
            for (std::size_t f = 0; f < m.recent.size(); ++f) {
                const auto col = m.recent[f].col;
                if (std::find(p->cols.begin(), p->cols.end(), col) == p->cols.end()) continue;
                r[f] = std::exp(-age / m.recent[f].tau);
            }
            return r;
        }

    private:
        std::vector<RecentPick> picks_;
    };

    /// The columns a cap has above 0 (RecentPick::cols).
    [[nodiscard]] inline std::vector<std::uint16_t> PositiveCols(std::span<const SparseCol> cap)
    {
        std::array<double, J> v{};
        for (const auto& [j, x] : cap) {
            if (j < J) v[j] = x;   // the last value of a repeated column, as CapMat
        }
        std::vector<std::uint16_t> out;
        for (std::size_t j = 0; j < J; ++j) {
            if (v[j] > 0.0) out.push_back(static_cast<std::uint16_t>(j));
        }
        return out;
    }
}
