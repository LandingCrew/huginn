// Host tests for core/ChoiceScore.h and core/ChoiceUpdate.h (R8.2): the new
// model's score (each grading, kappa off the page, the diagonal variance, the
// explanation label), recent_<family> and its exclusions, covered with the
// greedy upgrade gate, and the update (Var_p precision, step = variance,
// Plackett-Luce order and removal, opportunity counting by onsets, the b
// battery, the sBootstrapBias modes).

#include "core/Bootstrap.h"
#include "core/ChoiceScore.h"
#include "core/ChoiceUpdate.h"
#include "core/Coverage.h"

#include <doctest/doctest.h>

#include <ostream>

#include <cmath>
#include <set>
#include <vector>

using namespace Huginn::Core;
using namespace Huginn::Core::Choice;
namespace DL = Huginn::Core::DecisionLog;

namespace
{
    constexpr auto kHD = static_cast<std::size_t>(Needs::NeedId::health_deficit);
    constexpr auto kMD = static_cast<std::size_t>(Needs::NeedId::magicka_deficit);
    constexpr auto kENC = static_cast<std::size_t>(Needs::NeedId::encumbrance);
    constexpr auto kRH = static_cast<std::uint16_t>(Effect::Col::restore_health);
    constexpr auto kRM = static_cast<std::uint16_t>(Effect::Col::restore_magicka);
    constexpr auto kOH = static_cast<std::uint16_t>(Effect::Col::overshoot_health);
    constexpr auto kFOOD = static_cast<std::uint16_t>(Effect::Col::kind_food);

    /// A model with every weight at 0 and variance 1 (no prior signs), so a
    /// test sets exactly the weights it reads.
    Model Blank(Grading g = Grading::Graded)
    {
        Model m;
        m.grading = g;
        m.theta.assign(K * J, Weight{ 0.0, 1.0 });
        if (g == Grading::PresenceStrength) m.strength.assign(K * J, Weight{ 0.0, 1.0 });
        m.covered = Weight{ 0.0, 1.0 };
        return m;
    }

    std::array<double, K> Needs1(std::size_t k, double v)
    {
        std::array<double, K> n{};
        n[k] = v;
        return n;
    }

    Attr A(Grading g, std::vector<SparseCol> cap, std::vector<SparseCol> x = {})
    {
        return MakeAttr(g, cap, x);
    }
}

TEST_CASE("choice score: graded -- mu = sum_j v_j (cap_j + x_j) + b + features, kappa off the page")
{
    Model m = Blank();
    m.Th(kHD, kRH) = { 2.0, 0.5 };
    m.Th(kMD, kRH) = { -1.0, 0.25 };
    m.Th(kHD, kOH) = { 0.5, 0.1 };
    m.kappa = 3.0;
    m.w[static_cast<std::size_t>(Feature::ln_learn_mult)] = 1.5;
    m.w[static_cast<std::size_t>(Feature::util_missing)] = 0.25;
    std::array<double, K> n{};
    n[kHD] = 0.8;
    n[kMD] = 0.4;
    const Tick t = MakeTick(m, n, {});
    CHECK(t.v[kRH] == doctest::Approx(0.8 * 2.0 + 0.4 * -1.0));
    CHECK(t.W[kRH] == doctest::Approx(0.64 * 0.5 + 0.16 * 0.25));
    REQUIRE(t.active.size() == 2);
    // a potion: restore_health 0.6, overshoot_health (a cross-feature naming its column) 0.2
    const Attr at = A(Grading::Graded, { { kRH, 0.6 } }, { { kOH, 0.2 } });
    RowExtra x;
    x.bMu = 0.3;
    x.bVar = 0.2;
    x.feat[static_cast<std::size_t>(Feature::ln_learn_mult)] = 0.4;
    x.feat[static_cast<std::size_t>(Feature::util_missing)] = 0.0;
    const RowScore on = Score(m, t, at, x);
    const double need = (0.8 * 2.0 - 0.4) * 0.6 + 0.8 * 0.5 * 0.2;
    CHECK(on.mu == doctest::Approx(need + 0.3 + 1.5 * 0.4));
    CHECK(on.logit == doctest::Approx(on.mu));
    x.offPage = true;
    const RowScore off = Score(m, t, at, x);
    CHECK(off.mu == doctest::Approx(on.mu));
    CHECK(off.logit == doctest::Approx(on.mu - 3.0));
    // the diagonal variance: sum_j a_j^2 W_j + var_b
    // (magicka_deficit's overshoot_health weight keeps the blank variance 1)
    const double var = 0.36 * (0.64 * 0.5 + 0.16 * 0.25) + 0.04 * (0.64 * 0.1 + 0.16 * 1.0) + 0.2;
    CHECK(on.var == doctest::Approx(var));
    CHECK(on.Sd() == doctest::Approx(std::sqrt(var)));
}

TEST_CASE("choice score: presence and presence+strength read the cap as the format says")
{
    std::array<double, K> n = Needs1(kHD, 1.0);
    SUBCASE("presence: 1[cap > 0] + x")
    {
        Model m = Blank(Grading::Presence);
        m.Th(kHD, kRH) = { 2.0, 1.0 };
        m.Th(kHD, kOH) = { 1.0, 1.0 };
        const Tick t = MakeTick(m, n, {});
        const Attr at = A(Grading::Presence, { { kRH, 0.25 } }, { { kOH, -0.5 } });
        CHECK(Score(m, t, at, {}).mu == doctest::Approx(2.0 * 1.0 + 1.0 * -0.5));
    }
    SUBCASE("presence+strength: + theta_strength (cap - 0.5) 1[cap > 0], cross-features on the presence block only")
    {
        Model m = Blank(Grading::PresenceStrength);
        m.Th(kHD, kRH) = { 2.0, 1.0 };
        m.strength[kHD * J + kRH] = { 4.0, 0.5 };
        m.Th(kHD, kOH) = { 1.0, 1.0 };
        m.strength[kHD * J + kOH] = { 100.0, 1.0 };   // a cross-feature never reaches it
        const Tick t = MakeTick(m, n, {});
        const Attr at = A(Grading::PresenceStrength, { { kRH, 0.75 } }, { { kOH, 0.5 } });
        const auto r = Score(m, t, at, {});
        CHECK(r.mu == doctest::Approx(2.0 + 4.0 * 0.25 + 1.0 * 0.5));
        CHECK(r.var == doctest::Approx(1.0 * 1.0 + 0.0625 * 0.5 + 0.25 * 1.0));
    }
    SUBCASE("theta_bias: the bias need fixed at 1")
    {
        Model m = Blank();
        m.bias.assign(J, 0.0);
        m.bias[kRH] = 0.5;
        const Tick t = MakeTick(m, std::array<double, K>{}, {});
        CHECK(Score(m, t, A(Grading::Graded, { { kRH, 0.5 } }), {}).mu == doctest::Approx(0.25));
    }
    SUBCASE("three outcomes: u0 = bias + sum_k u0_k n_k + sum_f u0_f r_f")
    {
        Model m = Blank();
        m.three = true;
        m.u0Bias = -1.0;
        m.u0Need[kHD] = 0.5;
        m.recent.push_back(RecentRow{ kFOOD, 5.0, std::vector<Weight>(J), {}, 0.25 });
        const std::vector<double> r{ 0.5 };
        CHECK(MakeTick(m, n, r).u0 == doctest::Approx(-1.0 + 0.5 + 0.125));
    }
}

TEST_CASE("choice score: recent rows add r_f theta_f to v and r_f^2 var_f to W")
{
    Model m = Blank();
    RecentRow rr{ kFOOD, 5.0, std::vector<Weight>(J, Weight{ 0.0, 0.09 }), {}, 0.0 };
    rr.theta[kRH] = { -0.5, 0.2 };
    m.recent.push_back(rr);
    m.window = 30.0;
    const std::vector<double> r{ 0.5 };
    const Tick t = MakeTick(m, std::array<double, K>{}, r);
    CHECK(t.v[kRH] == doctest::Approx(-0.25));
    CHECK(t.W[kRH] == doctest::Approx(0.25 * 0.2));
    CHECK(t.active.empty());
}

TEST_CASE("choice score: the explanation is the largest single theta n cap term")
{
    Model m = Blank();
    m.Th(kHD, kRH) = { 1.0, 1.0 };
    m.Th(kMD, kRM) = { 3.0, 1.0 };
    m.Th(kENC, kRH) = { 0.5, 1.0 };
    std::array<double, K> n{};
    n[kHD] = 0.9;
    n[kMD] = 0.2;
    n[kENC] = 1.0;
    const Tick t = MakeTick(m, n, {});
    // terms: hd x rh 0.9*1*0.5 = 0.45; md x rm 0.2*3*0.5 = 0.3; enc x rh 1*0.5*0.5 = 0.25
    const Attr at = A(Grading::Graded, { { kRH, 0.5 }, { kRM, 0.5 } });
    auto r = Score(m, t, at, {});
    CHECK(r.exNeed == static_cast<std::int16_t>(kHD));
    CHECK(r.exCol == static_cast<std::int16_t>(kRH));
    // b, recent and the features are never the label, however large
    RowExtra big;
    big.bMu = 50.0;
    big.feat[static_cast<std::size_t>(Feature::ln_learn_mult)] = 10.0;
    m.w[static_cast<std::size_t>(Feature::ln_learn_mult)] = 10.0;
    r = Score(m, t, at, big);
    CHECK(r.exNeed == static_cast<std::int16_t>(kHD));
    // a tie goes to the lower (need, column): md x rm raised to 0.45 exactly
    m.Th(kMD, kRM) = { 4.5, 1.0 };
    r = Score(m, t, at, {});
    CHECK(r.exNeed == static_cast<std::int16_t>(kHD));   // health_deficit (0) before magicka_deficit (1)
    // no positive term: "learned"
    Model neg = Blank();
    neg.Th(kHD, kRH) = { -1.0, 1.0 };
    const Tick tn = MakeTick(neg, n, {});
    const auto rl = Score(neg, tn, at, big);
    CHECK(rl.Learned());
    CHECK(rl.exCol == -1);
    // with no need on at all: "learned" too
    const Tick t0 = MakeTick(m, std::array<double, K>{}, {});
    CHECK(Score(m, t0, at, {}).Learned());
}

TEST_CASE("choice score: recent_<family> -- latest usable pick, same generation, window, the family's column")
{
    Model m = Blank();
    m.window = 30.0;
    m.recent.push_back(RecentRow{ kFOOD, 5.0, std::vector<Weight>(J), {}, 0.0 });
    m.recent.push_back(RecentRow{ kFOOD, 30.0, std::vector<Weight>(J), {}, 0.0 });
    m.recent.push_back(RecentRow{ kRH, 5.0, std::vector<Weight>(J), {}, 0.0 });
    RecentTracker tr;
    const std::vector<SparseCol> food{ { kFOOD, 1.0 }, { kRH, 0.0 } };   // restore_health 0: not counted
    tr.Add(RecentPick{ 100.0, 1, 1, true, PositiveCols(food) });
    // 10 s later, same generation
    auto r = tr.Values(m, 110.0, 1);
    CHECK(r[0] == doctest::Approx(std::exp(-10.0 / 5.0)));
    CHECK(r[1] == doctest::Approx(std::exp(-10.0 / 30.0)));
    CHECK(r[2] == 0.0);
    // another generation (a load): nothing
    CHECK(tr.Values(m, 110.0, 2)[0] == 0.0);
    // past the window: nothing (and not an earlier pick instead)
    CHECK(tr.Values(m, 130.5, 1)[1] == 0.0);
    CHECK(tr.Values(m, 130.0, 1)[1] == doctest::Approx(std::exp(-1.0)));
    // strictly before: a decision at the pick's own time does not see it
    CHECK(tr.Values(m, 100.0, 1)[0] == 0.0);
    // a later pick replaces it; the decision itself is excluded from its own query
    tr.Add(RecentPick{ 105.0, 1, 2, true, { kRH } });
    r = tr.Values(m, 110.0, 1);
    CHECK(r[0] == 0.0);
    CHECK(r[2] == doctest::Approx(std::exp(-1.0)));
    r = tr.Values(m, 110.0, 1, /*exclude=*/2);
    CHECK(r[0] == doctest::Approx(std::exp(-2.0)));
    // a pick without a cap counts and gives 0 (no fallback to an earlier one)
    tr.Add(RecentPick{ 108.0, 1, 3, false, {} });
    r = tr.Values(m, 110.0, 1);
    CHECK(r[0] == 0.0);
    CHECK(r[2] == 0.0);
    // the exclusions of fitlib.recent_families
    CHECK(UsablePick(false, true, true, "Potion of Healing", "LoreRim-5"));
    CHECK_FALSE(UsablePick(true, true, true, "Potion of Healing", "LoreRim-5"));    // nothing
    CHECK_FALSE(UsablePick(false, false, true, "Potion of Healing", "LoreRim-5"));  // no context
    CHECK_FALSE(UsablePick(false, true, false, "Potion of Healing", "LoreRim-5"));  // no chosen row
    CHECK_FALSE(UsablePick(false, true, true, "Arcane Anchor", "LoreRim-5"));       // transient
    CHECK_FALSE(UsablePick(false, true, true, "Potion of Healing", "simonrim-essentails"));
    // the tracker keeps the latest picks in time order whatever order they arrive in
    RecentTracker t2;
    t2.Add(RecentPick{ 50.0, 1, 10, true, { kRH } });
    t2.Add(RecentPick{ 40.0, 1, 11, true, { kFOOD } });
    CHECK(t2.LatestBefore(60.0, 1, UINT64_MAX)->id == 10);
    CHECK(t2.LatestBefore(45.0, 1, UINT64_MAX)->id == 11);
}

TEST_CASE("choice score: covered enters as one weight; the greedy upgrade gate at the summon limit")
{
    Model m = Blank();
    m.covered = { -1.0, 1.0 };
    const Tick t = MakeTick(m, std::array<double, K>{}, {});
    RowExtra x;
    x.covered = 0.75;
    const auto r = Score(m, t, A(Grading::Graded, { { kRH, 0.5 } }), x);
    CHECK(r.mu == doctest::Approx(-0.75));
    CHECK(r.shat == doctest::Approx(0.0));
    CHECK(r.var == doctest::Approx(0.5625));
    // the gate: 1 - logistic(Delta / sigma_Delta), times left(r_out)
    CHECK(UpgradeCovered(1.0, 0.0, 1.0, 1.0) == doctest::Approx(0.2689414));   // a 1-sigma upgrade: 27%
    CHECK(UpgradeCovered(3.0, 0.0, 1.0, 1.0) == doctest::Approx(0.0474259));   // 3 sigma: 5%
    CHECK(UpgradeCovered(0.0, 0.0, 4.0, 1.0) == doctest::Approx(0.5));         // a close call
    CHECK(UpgradeCovered(-2.0, 0.0, 4.0, 0.5) == doctest::Approx(0.5 * 0.7310586));   // a downgrade, left 0.5
    CHECK(UpgradeCovered(0.1, 0.0, 0.0, 1.0) == 0.0);   // a certain posterior: a step
    // sigma_Delta^2 under the diagonal posterior: sum_j (a_j(i) - a_j(o))^2 W_j + var_b(i) + var_b(o)
    Model mv = Blank();
    mv.Th(kHD, kRH) = { 0.0, 2.0 };
    mv.Th(kHD, kRM) = { 0.0, 3.0 };
    const Tick tv = MakeTick(mv, Needs1(kHD, 1.0), {});
    const Attr i = A(Grading::Graded, { { kRH, 1.0 } });
    const Attr o = A(Grading::Graded, { { kRH, 0.5 }, { kRM, 1.0 } });
    CHECK(DeltaVar(tv, i, o, 0.1, 0.2) == doctest::Approx(0.25 * 2.0 + 1.0 * 3.0 + 0.3));
    // Coverage::CoveredParts agrees with Covered, and splits the summon term out
    Effect::Cap summonCap{ { Effect::Col::summon_creature, 0.5f }, { Effect::Col::restore_health, 0.5f } };
    Coverage::Active act;
    act.summons = 2;
    act.summonLimit = 2.0f;
    act.summonRemaining = 20.0f;
    act.Add(Effect::Col::restore_health, 0.25f, 30.0f);
    const auto parts = Coverage::CoveredParts(summonCap, act, 5.0f);
    const auto whole = Coverage::Covered(summonCap, act, 5.0f);
    CHECK(parts.summon);
    CHECK(parts.atLimit);
    CHECK(std::max(parts.other, parts.summonValue) == doctest::Approx(whole.value));
    CHECK(parts.summonLeft == doctest::Approx(Coverage::LeftPrior(20.0f, 5.0f)));
    CHECK(parts.other == doctest::Approx(0.5f * Coverage::LeftPrior(30.0f, 5.0f)));
    std::vector<Coverage::CommandedActor> cmd{
        { 0x100, 1, Coverage::CommandedActor::Kind::Summon, 20.0f, 40.0f },
        { 0x200, 2, Coverage::CommandedActor::Kind::Summon, 50.0f, 10.0f },
        { 0x300, 3, Coverage::CommandedActor::Kind::Other, -1.0f, 99.0f },
    };
    CHECK(Coverage::OldestSource(cmd) == 0x100);

    // ApplyUpgradeGate over rows: a clear upgrade reads near 0, a clear
    // downgrade near 1 (never under its other columns' value), a_out's own
    // source (a refresh, not gated) and non-summon rows keep their covered;
    // a_out's source may be an extra entry the caller appends (not a row).
    Model g = Blank();
    const auto kSC = static_cast<std::uint16_t>(Effect::Col::summon_creature);
    g.Th(kHD, kSC) = { 2.0, 0.01 };
    const Tick tg = MakeTick(g, Needs1(kHD, 1.0), {});
    std::vector<Attr> at{ A(Grading::Graded, { { kSC, 1.0 } }),     // a strong summon: an upgrade
                          A(Grading::Graded, { { kSC, 0.1 } }),     // a weak one: a downgrade
                          A(Grading::Graded, { { kRH, 1.0 } }),     // not a summon
                          A(Grading::Graded, { { kSC, 0.5 } }) };   // a_out's source (appended)
    std::vector<RowExtra> xs(4);
    for (auto& e : xs) {
        e.covered = 0.9;
        e.bVar = 0.01;
    }
    std::vector<GateRow> gr{ GateRow{ true, 0.0 }, GateRow{ true, 0.3 }, GateRow{}, GateRow{} };
    ApplyUpgradeGate(g, tg, at, xs, gr, 3, 0.9);
    CHECK(xs[0].covered < 0.01);                  // delta 1.0, sd ~0.14: a clear upgrade
    CHECK(xs[1].covered > 0.85);                  // delta -0.8: a clear downgrade, ~left(r_out)
    CHECK(xs[1].covered <= 0.9);
    CHECK(xs[2].covered == 0.9);                  // not gated
    CHECK(xs[3].covered == 0.9);                  // the source itself
    xs[1].covered = 0.9;
    std::vector<GateRow> gr2{ GateRow{ true, 0.0 }, GateRow{ true, 0.95 }, GateRow{}, GateRow{} };
    ApplyUpgradeGate(g, tg, at, xs, gr2, 3, 0.9);
    CHECK(xs[1].covered == 0.95);                 // never under its other columns
    std::vector<RowExtra> keep = xs;
    ApplyUpgradeGate(g, tg, at, xs, gr, 99, 0.9);   // no a_out entry: nothing changes
    for (std::size_t q = 0; q < xs.size(); ++q) CHECK(xs[q].covered == keep[q].covered);
}

TEST_CASE("choice update: the choice set is fitlib.build's, with Plackett-Luce removal")
{
    using namespace DL::Flag;
    std::vector<DL::Row> rows(7);
    rows[0].form = 1; rows[0].flags = Shown | Eligible;
    rows[1].form = 2; rows[1].flags = Held;                // off the page
    rows[2].form = 3; rows[2].flags = Held | Equipped;     // equipped: out
    rows[3].form = 4; rows[3].flags = 0;                   // neither shown, held nor eligible: out
    rows[4].form = 5; rows[4].flags = Eligible;            // the chosen row (key pick, not flagged shown)
    rows[5].form = 2; rows[5].flags = Held;                // a repeat of (2, 0): out
    rows[6].form = 6; rows[6].flags = Held | Shown;
    std::vector<const DL::Row*> ptr;
    for (const auto& r : rows) ptr.push_back(&r);
    std::set<std::uint64_t> taken;
    auto set = ChoiceSet(ptr, 4, /*keyOrWheel=*/true, taken);
    REQUIRE(set.size() == 4);
    CHECK(set[0].row == 0);
    CHECK_FALSE(set[0].offPage);
    CHECK(set[1].row == 1);
    CHECK(set[1].offPage);
    CHECK(set[2].row == 4);
    CHECK(set[2].chosen);
    CHECK_FALSE(set[2].offPage);    // the chosen row of a key pick is on the page
    CHECK(set[3].row == 6);
    // a menu pick: the chosen row off the page (not shown)
    set = ChoiceSet(ptr, 4, /*keyOrWheel=*/false, taken);
    CHECK(set[2].offPage);
    // the chosen row is always in, even when equipped or flagged nothing
    set = ChoiceSet(ptr, 2, false, taken);
    CHECK(std::any_of(set.begin(), set.end(), [](const SetEntry& e) { return e.row == 2 && e.chosen; }));
    // Plackett-Luce: a co-pick of the same context removes the earlier pick's item
    taken.insert(StackKey(5, 0));
    set = ChoiceSet(ptr, 6, true, taken);
    CHECK(std::none_of(set.begin(), set.end(), [](const SetEntry& e) { return e.row == 4; }));
    CHECK(set.back().row == 6);
    CHECK(set.back().chosen);
}

TEST_CASE("choice update: Var_p precision, step = variance (Model.info's form)")
{
    Model m = Blank();
    m.Th(kHD, kRH) = { 0.0, 0.5 };
    m.Th(kHD, kFOOD) = { 0.0, 0.25 };
    const double nk = 0.8;
    const Tick t = MakeTick(m, Needs1(kHD, nk), {});
    // two items, equal scores (all weights 0): P = 0.5 each
    std::vector<Member> set(2);
    set[0].attr = A(Grading::Graded, { { kRH, 1.0 } });
    set[0].form = 10;
    set[1].attr = A(Grading::Graded, { { kFOOD, 1.0 } });
    set[1].form = 11;
    const auto res = Update(m, t, set, 0);
    REQUIRE(res.P.size() == 2);
    CHECK(res.P[0] == doctest::Approx(0.5));
    // column rh: E = 0.5, E2 = 0.5, V = 0.25; chosen a = 1, g = 0.5
    const double precRH = 1.0 / 0.5 + nk * nk * 0.25;
    CHECK(m.Th(kHD, kRH).var == doctest::Approx(1.0 / precRH));
    CHECK(m.Th(kHD, kRH).mu == doctest::Approx((1.0 / precRH) * nk * 0.5));
    // column food: V = 0.25, g = 0 - 0.5
    const double precF = 1.0 / 0.25 + nk * nk * 0.25;
    CHECK(m.Th(kHD, kFOOD).var == doctest::Approx(1.0 / precF));
    CHECK(m.Th(kHD, kFOOD).mu == doctest::Approx(-(1.0 / precF) * nk * 0.5));
    // a need that is off, and a column no row has, do not move
    CHECK(m.Th(kMD, kRH).var == 1.0);
    CHECK(m.Th(kHD, kRM).var == 1.0);
    // b: per item, P (1 - P) and y - P
    REQUIRE(res.b.size() == 2);
    CHECK(res.b[0].form == 10);
    CHECK(res.b[0].dPrec == doctest::Approx(0.25));
    CHECK(res.b[0].grad == doctest::Approx(0.5));
    CHECK(res.b[1].grad == doctest::Approx(-0.5));
    double bmu = 0.0, bvar = 0.25;
    ApplyB(bmu, bvar, res.b[0]);
    CHECK(bvar == doctest::Approx(1.0 / (4.0 + 0.25)));
    CHECK(bmu == doctest::Approx(0.5 / 4.25));
    // a second pick of the same item moves it further, by a smaller step (the precision grew)
    const double before = m.Th(kHD, kRH).mu;
    const double v1 = m.Th(kHD, kRH).var;
    const Tick t2 = MakeTick(m, Needs1(kHD, nk), {});
    (void)Update(m, t2, set, 0);
    CHECK(m.Th(kHD, kRH).mu > before);
    CHECK(m.Th(kHD, kRH).var < v1);
}

TEST_CASE("choice update: recent rows, covered and kappa in the update")
{
    Model m = Blank();
    m.kappa = 2.0;
    m.recent.push_back(RecentRow{ kFOOD, 5.0, std::vector<Weight>(J, Weight{ 0.0, 0.09 }), {}, 0.0 });
    const std::vector<double> r{ 0.5 };
    const Tick t = MakeTick(m, std::array<double, K>{}, r);
    std::vector<Member> set(2);
    set[0].attr = A(Grading::Graded, { { kRH, 1.0 } });
    set[0].covered = 1.0;
    set[1].attr = A(Grading::Graded, { { kFOOD, 1.0 } });
    set[1].offPage = true;
    const auto res = Update(m, t, set, 1);
    // P: logit 0 vs -2 (off the page)
    const double p0 = 1.0 / (1.0 + std::exp(-2.0));
    CHECK(res.P[0] == doctest::Approx(p0));
    // the recent row moves with coefficient r = 0.5
    const double V = p0 * (1.0 - p0);
    const double prec = 1.0 / 0.09 + 0.25 * V;
    CHECK(m.recent[0].theta[kFOOD].var == doctest::Approx(1.0 / prec));
    CHECK(m.recent[0].theta[kFOOD].mu == doctest::Approx((1.0 / prec) * 0.5 * (1.0 - (1.0 - p0))));
    // covered: x = (1, 0), chosen 0: E = p0, V = p0 (1 - p0), g = -p0
    const double pc = 1.0 + V;
    CHECK(m.covered.var == doctest::Approx(1.0 / pc));
    CHECK(m.covered.mu == doctest::Approx(-(1.0 / pc) * p0));
    // kappa is fixed
    CHECK(m.kappa == 2.0);
}

TEST_CASE("choice update: opportunity counting is by onsets, not seconds")
{
    OnsetCounter oc;
    std::array<double, K> n{};
    n[kHD] = 0.6;
    auto on = oc.Step(n);
    REQUIRE(on.size() == 1);
    CHECK(on[0] == kHD);
    // the need stays on for many ticks: no new onset
    for (int i = 0; i < 100; ++i) CHECK(oc.Step(n).empty());
    // falls under the onset but above the expiry: still the same episode
    n[kHD] = 0.3;
    CHECK(oc.Step(n).empty());
    n[kHD] = 0.7;
    CHECK(oc.Step(n).empty());
    // under the expiry, then up again: a second onset
    n[kHD] = 0.2;
    CHECK(oc.Step(n).empty());
    n[kHD] = 0.5;
    CHECK(oc.Step(n).size() == 1);
    // GrowNeed adds the noise to that need's row only
    Model m = Blank();
    m.Th(kHD, kRH) = { 1.0, 0.5 };
    GrowNeed(m, kHD, 0.01);
    CHECK(m.Th(kHD, kRH).var == doctest::Approx(0.51));
    CHECK(m.Th(kHD, kFOOD).var == doctest::Approx(1.01));
    CHECK(m.Th(kMD, kRH).var == 1.0);
    // Answers: the item's need term under the model is above 0
    CHECK(Answers(m, kHD, A(Grading::Graded, { { kRH, 0.5 } })));
    CHECK_FALSE(Answers(m, kHD, A(Grading::Graded, { { kFOOD, 0.5 } })));
    CHECK_FALSE(Answers(m, kMD, A(Grading::Graded, { { kRH, 0.5 } })));
}

TEST_CASE("choice update: the b battery on the play clock")
{
    Battery::MemoryLife life;   // 8 h + 2 h ln(1 + n), knee 1 h
    BBelief b{ 1.0, 0.1, 3.0f, 0.0, false };
    // fresh: full strength
    auto e = ReadB(b, life, 0.0);
    CHECK(e.mu == doctest::Approx(1.0));
    CHECK(e.var == doctest::Approx(0.1));
    // long past its life (8 + 2 ln 4 = 10.8 h): back to the prior N(0, 0.25)
    e = ReadB(b, life, 3600.0 * 20.0);
    CHECK(e.retention < 0.001f);
    CHECK(e.mu == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(e.var == doctest::Approx(0.25).epsilon(1e-3));
    // a favourite (fFavoriteLifeMult 2) lives twice as long: at 15 h a plain
    // item is nearly gone, a favourite still at full strength
    CHECK(ReadB(b, life, 3600.0 * 15.0).retention < 0.02f);
    const auto fav = ReadB(b, life, 3600.0 * 15.0, 2.0);
    CHECK(fav.retention > 0.99f);
    // the curve is the old learner's (core/Battery.h)
    CHECK(e.retention == Battery::RetentionAt(life, 3600.0 * 20.0, 0.0, 3.0f));
    // a pick at the knee renews from what is left
    const double knee = 3600.0 * (8.0 + 2.0 * std::log1p(3.0));
    const auto half = ReadB(b, life, knee);
    RenewB(b, life, knee);
    CHECK(b.mu == doctest::Approx(half.mu));
    CHECK(b.var == doctest::Approx(half.var));
    CHECK(b.n == doctest::Approx(3.0f * half.retention + 1.0f));
    CHECK(b.chosenAt == knee);
    CHECK(ReadB(b, life, knee).retention == doctest::Approx(1.0f));
    // the life off: nothing fades
    Battery::MemoryLife off;
    off.enabled = false;
    CHECK(ReadB(BBelief{ 1.0, 0.1, 0.0f, 0.0, false }, off, 1e9).mu == 1.0);
    CHECK_FALSE(Battery::Faded(off, 1e9, 0.0, 0.0f));
    CHECK(Battery::Faded(life, 1e9, 0.0, 0.0f));
}

TEST_CASE("choice update: sBootstrapBias -- match, always, never")
{
    using Bootstrap::BiasMode;
    CHECK(Bootstrap::ParseBiasMode("match") == BiasMode::Match);
    CHECK(Bootstrap::ParseBiasMode("Always") == BiasMode::Always);
    CHECK(Bootstrap::ParseBiasMode("NEVER") == BiasMode::Never);
    CHECK(Bootstrap::ParseBiasMode("sometimes") == BiasMode::Match);
    CHECK(Bootstrap::BiasModeName(BiasMode::Never) == "never");
    constexpr std::uint64_t mage = 0xFEFD1B039AC78799ull, other = 0x3F3E2A8817962D59ull;
    CHECK(Bootstrap::UseFileBias(BiasMode::Match, mage, mage));
    CHECK_FALSE(Bootstrap::UseFileBias(BiasMode::Match, other, mage));
    CHECK_FALSE(Bootstrap::UseFileBias(BiasMode::Match, 0, mage));       // no character yet
    CHECK_FALSE(Bootstrap::UseFileBias(BiasMode::Match, mage, 0));       // a file of several characters
    CHECK(Bootstrap::UseFileBias(BiasMode::Always, other, mage));
    CHECK(Bootstrap::UseFileBias(BiasMode::Always, 0, 0));
    CHECK_FALSE(Bootstrap::UseFileBias(BiasMode::Never, mage, mage));
}
