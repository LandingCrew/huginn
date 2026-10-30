// Host tests for R9, the Bayesian challenger rule: core/ChallengerRule.h (the
// rule, z, the churn buckets), core/ChoiceBeliefs.h (sigma_Delta from one
// ranking run, shared weights cancelling) and the slot hold that applies it
// (core/SlotAllocCore.h, PASS 1c). The roadmap's "Done when": no swap-back on
// fixed beliefs, and churn bucketed on Delta / sigma_Delta.

#include "SlotAllocTestSupport.h"

#include "core/ChallengerRule.h"
#include "core/ChoiceBeliefs.h"
#include "core/ChoiceScore.h"
#include "core/SlotAllocCore.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace Huginn::Core;
using namespace Huginn::Core::Challenger;
namespace C = Huginn::Core::Choice;
namespace SA = Huginn::Core::SlotAlloc;
namespace T = Huginn::Test;

namespace
{
    constexpr double kInf = std::numeric_limits<double>::infinity();
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

    /// A belief source with a fixed sigma_Delta per pair (symmetric), for the
    /// hold's named cases.
    class PairBeliefs final : public BeliefSource
    {
    public:
        void Set(std::uint32_t a, std::uint32_t b, double sd) { m_sd[{ std::min(a, b), std::max(a, b) }] = sd; }
        [[nodiscard]] double DeltaVar(std::uint32_t a, std::uint32_t b) const noexcept override
        {
            const auto it = m_sd.find({ std::min(a, b), std::max(a, b) });
            return it == m_sd.end() ? kNaN : it->second * it->second;
        }

    private:
        std::map<std::pair<std::uint32_t, std::uint32_t>, double> m_sd;
    };

    SA::SlotRec Regular()
    {
        SA::SlotRec s;
        s.classIndex = T::kRegular;
        s.regular = true;
        s.overrideAccept = 0;
        s.skipEquipped = false;
        return s;
    }

    SA::CandidateRec Cand(std::uint32_t id, double score, std::uint32_t row)
    {
        SA::CandidateRec c;
        c.formID = id;
        c.dedupKey = id;
        c.name = "C" + std::to_string(id);
        c.score = score;
        c.matchMask = 1u << T::kRegular;
        c.beliefRow = row;
        return c;
    }

    /// One Regular key seated on `holder`, the challenger ranked first.
    SA::Input HeldKey(double holderScore, double challengerScore, const BeliefSource* beliefs)
    {
        SA::Input in;
        in.nowNs = 5'000'000'000'000LL;
        in.slots = { Regular() };
        in.candidates = { Cand(2, challengerScore, 1), Cand(1, holderScore, 0) };
        in.settings.keepSlotPositions = true;
        in.settings.holdSeatedItems = true;
        in.settings.classDiscount = 1.0f;   // no class cap
        in.memory.seats[0] = 1;
        in.memory.lastPlaced[0] = 1;
        in.beliefs = beliefs;
        return in;
    }

    std::uint32_t ShownOn0(const SA::Input& in)
    {
        const auto out = SA::Allocate(in);
        return out.slots.empty() || out.slots[0].IsEmpty() ? 0 : in.candidates[out.slots[0].src].formID;
    }

    C::Model Blank()
    {
        C::Model m;
        m.theta.assign(C::K * C::J, C::Weight{ 0.0, 1.0 });
        m.covered = C::Weight{ 0.0, 1.0 };
        return m;
    }

    constexpr auto kHD = static_cast<std::size_t>(Needs::NeedId::health_deficit);
    constexpr auto kMD = static_cast<std::size_t>(Needs::NeedId::magicka_deficit);
    constexpr auto kRH = static_cast<std::uint16_t>(Effect::Col::restore_health);
    constexpr auto kRM = static_cast<std::uint16_t>(Effect::Col::restore_magicka);
    constexpr auto kOH = static_cast<std::uint16_t>(Effect::Col::overshoot_health);
}  // namespace

// =============================================================================
// The rule
// =============================================================================

TEST_CASE("challenger rule: z = Phi^-1(tau), clamped to [0.5, 0.999]")
{
    CHECK(ZForConfidence(0.5) == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(ZForConfidence(0.8) == doctest::Approx(0.8416212335729143).epsilon(1e-12));
    CHECK(ZForConfidence(0.9) == doctest::Approx(1.2815515655446004).epsilon(1e-12));
    CHECK(ZForConfidence(0.975) == doctest::Approx(1.959963984540054).epsilon(1e-12));
    CHECK(ZForConfidence(0.999) == doctest::Approx(3.090232306167813).epsilon(1e-10));
    // Out of range: a probably-worse challenger is never let in (z >= 0).
    CHECK(ZForConfidence(0.1) == ZForConfidence(0.5));
    CHECK(ZForConfidence(1.0) == ZForConfidence(0.999));
    CHECK(ZForConfidence(kNaN) == ZForConfidence(kDefaultConfidence));
    CHECK(ZForConfidence(kInf) == ZForConfidence(kDefaultConfidence));
    CHECK(NormalCdf(ZForConfidence(0.8)) == doctest::Approx(0.8).epsilon(1e-12));
}

TEST_CASE("challenger rule: ln m clamped to [0, 5]")
{
    CHECK(ClampMinGap(0.0) == 0.0);
    CHECK(ClampMinGap(0.405) == 0.405);
    CHECK(ClampMinGap(-1.0) == 0.0);
    CHECK(ClampMinGap(99.0) == kMaxMinGap);
    CHECK(ClampMinGap(kNaN) == 0.0);
    CHECK(ClampMinGap(-kInf) == 0.0);
}

TEST_CASE("challenger rule: swap when mu_c - mu_i > ln m + z sigma_Delta")
{
    const double z = ZForConfidence(0.8);
    // Doc 9's three cases. A challenger barely ahead of a well-known item
    // stays out; two well-learned items with a real gap swap; a new item
    // (wide error bar) needs a bigger lead.
    CHECK_FALSE(Exceeds(0.05, 0.0, 0.2, 0.0, z));   // P = Phi(0.25) = 0.60
    CHECK(Exceeds(0.5, 0.0, 0.2, 0.0, z));          // P = Phi(2.5) = 0.99
    CHECK_FALSE(Exceeds(0.5, 0.0, 1.0, 0.0, z));    // P = Phi(0.5) = 0.69
    CHECK(Exceeds(0.9, 0.0, 1.0, 0.0, z));          // P = Phi(0.9) = 0.82
    // Any sign: only the difference counts.
    CHECK(Exceeds(-3.0 + 0.5, -3.0, 0.2, 0.0, z) == Exceeds(0.5, 0.0, 0.2, 0.0, z));
    // ln m adds to the line.
    CHECK(Exceeds(0.5, 0.0, 0.2, 0.0, z));
    CHECK_FALSE(Exceeds(0.5, 0.0, 0.2, 0.4, z));
    // A certain gap (sigma 0, ln m 0): any real lead, but not a tie.
    CHECK(Exceeds(1e-3, 0.0, 0.0, 0.0, z));
    CHECK_FALSE(Exceeds(0.0, 0.0, 0.0, 0.0, z));
    CHECK_FALSE(Exceeds(kHoldTieEpsilon * 0.5, 0.0, 0.0, 0.0, z));
    // -inf, NaN: never a swap.
    CHECK_FALSE(Exceeds(-kInf, -kInf, 0.3, 0.0, z));
    CHECK(Exceeds(0.0, -kInf, 0.3, 0.0, z));
    CHECK_FALSE(Exceeds(kNaN, 0.0, 0.3, 0.0, z));
    CHECK_FALSE(Exceeds(1.0, 0.0, kNaN, 0.0, z));
    CHECK(Threshold(0.5, 0.1, 2.0) == doctest::Approx(1.1));
    CHECK(SigmaDelta(0.25) == doctest::Approx(0.5));
    CHECK(SigmaDelta(-1e-18) == 0.0);
    CHECK(std::isnan(SigmaDelta(kNaN)));
}

TEST_CASE("challenger rule: never both ways -- a swap cannot be answered by a swap back")
{
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> mu(-4.0, 4.0), sd(0.0, 2.0), gap(0.0, 0.5), tau(0.5, 0.999);
    int swaps = 0;
    for (int i = 0; i < 20000; ++i) {
        const double a = mu(rng), b = mu(rng), s = sd(rng), g = gap(rng), z = ZForConfidence(tau(rng));
        const bool ab = Exceeds(a, b, s, g, z), ba = Exceeds(b, a, s, g, z);
        CHECK_FALSE((ab && ba));
        swaps += ab || ba;
    }
    CHECK(swaps > 1000);   // the draw does produce swaps
}

// =============================================================================
// sigma_Delta from one ranking run: shared weights cancel
// =============================================================================

TEST_CASE("challenger rule: sigma_Delta -- what two items share cancels; disjoint items reach the bound")
{
    // Every other weight certain (variance 0), so each expected value below
    // names exactly the weights it reads.
    C::Model m = Blank();
    for (auto& w : m.theta) w.var = 0.0;
    m.Th(kHD, kRH) = { 1.5, 0.5 };
    m.Th(kMD, kRH) = { 0.0, 0.25 };
    m.Th(kHD, kOH) = { -0.5, 0.2 };
    m.Th(kMD, kRM) = { 1.0, 0.3 };
    m.covered = { -1.0, 0.4 };
    std::array<double, C::K> n{};
    n[kHD] = 0.9;
    n[kMD] = 0.5;
    const C::Tick t = C::MakeTick(m, n, {});
    const double wRH = 0.81 * 0.5 + 0.25 * 0.25, wOH = 0.81 * 0.2, wRM = 0.25 * 0.3;
    REQUIRE(t.W[kRH] == doctest::Approx(wRH));

    C::BeliefTable table(t, m.covered.var);
    // Two healing potions: the same column, different strength, each its own b.
    const auto minor = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.3 } }, {}), 0.0, 0.04);
    const auto plentiful = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.8 } }, {}), 0.0, 0.09);
    // A magicka potion: nothing in common with them.
    const auto magicka = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRM, 0.6 } }, {}), 0.0, 0.25);
    // A health potion with an overshoot cross-feature and a covered value.
    const auto over = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.8 } },
                                    std::vector<C::SparseCol>{ { kOH, 0.5 } }), 0.6, 0.09);

    // The two healing potions: theta[., restore_health] enters only through
    // the strength difference (0.8 - 0.3)^2, not 0.8^2 + 0.3^2.
    const double dv = table.DeltaVar(plentiful, minor);
    CHECK(dv == doctest::Approx(0.5 * 0.5 * wRH + 0.09 + 0.04));
    CHECK(table.DeltaVar(minor, plentiful) == dv);   // symmetric
    // Below the naive sqrt(sigma_c^2 + sigma_i^2), which counts the shared
    // weight twice (doc 9: "would make similar items swap too rarely"): its
    // theta part is 0.25 / 0.73 of the bound's; the b's are the same.
    const double bound = table.Var(plentiful) + table.Var(minor);
    CHECK(table.Var(plentiful) == doctest::Approx(0.64 * wRH + 0.09));
    CHECK(table.Var(minor) == doctest::Approx(0.09 * wRH + 0.04));
    CHECK(dv < bound);
    CHECK(dv - 0.13 == doctest::Approx((bound - 0.13) * 0.25 / 0.73));
    // Nothing shared: the bound is reached exactly.
    CHECK(table.DeltaVar(magicka, plentiful) == doctest::Approx(table.Var(magicka) + table.Var(plentiful)));
    CHECK(table.Var(magicka) == doctest::Approx(0.36 * wRM + 0.25));
    // covered enters through its difference too; the cross-feature column.
    CHECK(table.DeltaVar(over, plentiful) == doctest::Approx(0.25 * wOH + 0.36 * 0.4 + 0.09 + 0.09));
    // Rows with no b key keep independent b's, even against themselves:
    // 2 var(b). Out of range: NaN.
    CHECK(table.DeltaVar(minor, minor) == doctest::Approx(0.08));
    CHECK(std::isnan(table.DeltaVar(minor, 99)));
    // Two stacks of ONE form (another uniqueID or temper) read one b, keyed
    // by form: it cancels like a shared weight. The same attributes: 0.
    const auto stackA = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.3 } }, {}), 0.0, 0.04, 0x1234);
    const auto stackB = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.3 } }, {}), 0.0, 0.04, 0x1234);
    CHECK(table.DeltaVar(stackA, stackB) == doctest::Approx(0.0));
    // A tempered stack (stronger): only the strength difference is left.
    const auto tempered = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.5 } }, {}), 0.0, 0.04, 0x1234);
    CHECK(table.DeltaVar(tempered, stackA) == doctest::Approx(0.2 * 0.2 * wRH));
    // A favorite stack reads b's life with the favorite multiplier, so its
    // variance differs: one b read twice is perfectly correlated, the gap's
    // b part is (sd - sd')^2 = (0.3 - 0.2)^2.
    const auto favorite = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.3 } }, {}), 0.0, 0.09, 0x1234);
    CHECK(table.DeltaVar(favorite, stackA) == doctest::Approx(0.01));
    CHECK(table.DeltaVar(stackA, favorite) == table.DeltaVar(favorite, stackA));
    // Another form keeps its own b: both variances add.
    const auto other = table.Add(C::MakeAttr(C::Grading::Graded, std::vector<C::SparseCol>{ { kRH, 0.3 } }, {}), 0.0, 0.04, 0x5678);
    CHECK(table.DeltaVar(other, stackA) == doctest::Approx(0.08));
    // A row's own sigma^2 does not read the key.
    CHECK(table.Var(stackA) == doctest::Approx(table.Var(minor)));
    CHECK(std::isnan(table.Var(99)));

    // Each row's own sigma^2 is RowScore::var -- the table and the ranking
    // agree on what a belief is.
    for (const auto r : { minor, plentiful, magicka, over }) {
        C::RowExtra x;
        x.covered = table.At(r).covered;
        x.bVar = table.At(r).bVar;
        CHECK(table.Var(r) == doctest::Approx(C::Score(m, t, table.At(r).attr, x).var).epsilon(1e-12));
    }
}

TEST_CASE("challenger rule: sigma_Delta matches a dense brute force, presence+strength and recent included")
{
    std::mt19937 rng(17);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    C::Model m;
    m.grading = C::Grading::PresenceStrength;
    m.theta.assign(C::K * C::J, C::Weight{ 0.0, 0.0 });
    m.strength.assign(C::K * C::J, C::Weight{ 0.0, 0.0 });
    const std::uint16_t cols[] = { kRH, kRM, kOH, 7, 40, 41, 120 };
    const std::size_t needs[] = { kHD, kMD, 3, 11 };
    for (const auto k : needs) {
        for (const auto j : cols) {
            m.Th(k, j) = { u(rng) - 0.5, u(rng) };
            m.strength[k * C::J + j] = { u(rng) - 0.5, u(rng) * 0.5 };
        }
    }
    C::RecentRow rr;
    rr.col = kRH;
    rr.tau = 30.0;
    rr.theta.assign(C::J, C::Weight{ 0.0, 0.0 });
    rr.strength.assign(C::J, C::Weight{ 0.0, 0.0 });
    rr.theta[kRH] = { 0.3, 0.7 };
    rr.strength[kRM] = { 0.1, 0.2 };
    m.recent.push_back(rr);
    m.covered = { -0.5, 0.35 };
    std::array<double, C::K> n{};
    for (const auto k : needs) n[k] = u(rng);
    const std::vector<double> r{ 0.6 };
    const C::Tick t = C::MakeTick(m, n, r);

    C::BeliefTable table(t, m.covered.var);
    for (int i = 0; i < 12; ++i) {
        std::vector<C::SparseCol> cap, cross;
        for (const auto j : cols) {
            if (u(rng) < 0.45) cap.emplace_back(j, std::round(u(rng) * 100.0) / 100.0);
        }
        if (u(rng) < 0.3) cross.emplace_back(kOH, u(rng));
        table.Add(C::MakeAttr(m.grading, cap, cross), u(rng) < 0.3 ? u(rng) : 0.0, 0.25 * u(rng));
    }
    // Dense: d_j over every column of both blocks, against the posterior
    // variance of each weight as MakeTick sums it.
    auto dense = [&](std::uint32_t a, std::uint32_t b) {
        std::array<double, C::J> da{}, ds{};
        for (const auto& [j, v] : table.At(a).attr.a) da[j] += v;
        for (const auto& [j, v] : table.At(b).attr.a) da[j] -= v;
        for (const auto& [j, v] : table.At(a).attr.s) ds[j] += v;
        for (const auto& [j, v] : table.At(b).attr.s) ds[j] -= v;
        double var = table.At(a).bVar + table.At(b).bVar;
        for (std::size_t j = 0; j < C::J; ++j) {
            double wj = 0.0, wsj = 0.0;
            for (std::size_t k = 0; k < C::K; ++k) {
                wj += n[k] * n[k] * m.Th(k, j).var;
                wsj += n[k] * n[k] * m.strength[k * C::J + j].var;
            }
            wj += r[0] * r[0] * rr.theta[j].var;
            wsj += r[0] * r[0] * rr.strength[j].var;
            var += da[j] * da[j] * wj + ds[j] * ds[j] * wsj;
        }
        const double dc = table.At(a).covered - table.At(b).covered;
        return var + dc * dc * m.covered.var;
    };
    for (std::uint32_t a = 0; a < table.Size(); ++a) {
        for (std::uint32_t b = 0; b < table.Size(); ++b) {
            CHECK(table.DeltaVar(a, b) == doctest::Approx(dense(a, b)).epsilon(1e-12));
            // The bound: sigma_Delta^2 <= 2 (sigma_a^2 + sigma_b^2) always
            // ((x - y)^2 <= 2x^2 + 2y^2), and <= sigma_a^2 + sigma_b^2 when
            // the rows share no column.
            CHECK(table.DeltaVar(a, b) <= 2.0 * (table.Var(a) + table.Var(b)) + 1e-12);
        }
    }
}

// =============================================================================
// The hold applies it
// =============================================================================

TEST_CASE("challenger rule: the hold uses sigma_Delta with beliefs, the fixed margin without")
{
    // Challenger 0.3 ahead of the seated holder.
    PairBeliefs narrow;
    narrow.Set(0, 1, 0.2);   // line 0.84 * 0.2 = 0.17: the challenger clears it
    PairBeliefs wide;
    wide.Set(0, 1, 0.5);     // line 0.42: it does not

    // No beliefs (the fallback ranking): ln(1 + 0.5) = 0.405 > 0.3, the holder holds.
    CHECK(ShownOn0(HeldKey(-1.0, -0.7, nullptr)) == 1);
    CHECK(ShownOn0(HeldKey(-1.0, -0.7, &narrow)) == 2);
    CHECK(ShownOn0(HeldKey(-1.0, -0.7, &wide)) == 1);

    // The give-way records sigma_Delta; the fixed margin records < 0.
    {
        const auto in = HeldKey(-1.0, -0.7, &narrow);
        const auto out = SA::Allocate(in);
        bool seen = false;
        for (const auto& e : out.events) {
            if (e.kind != SA::EventKind::HoldGaveWay) continue;
            seen = true;
            CHECK(e.sd == doctest::Approx(0.2));
            CHECK(e.b == 1);   // holder: candidate index 1
            CHECK(e.c == 0);
        }
        CHECK(seen);
    }
    {
        const auto in = HeldKey(-1.0, 0.0, nullptr);
        const auto out = SA::Allocate(in);
        bool seen = false;
        for (const auto& e : out.events) {
            if (e.kind == SA::EventKind::HoldGaveWay) {
                seen = true;
                CHECK(e.sd < 0.0);
            }
        }
        CHECK(seen);
    }

    // A pair without a belief (rows of another run: NaN) falls back to the margin.
    PairBeliefs none;
    CHECK(ShownOn0(HeldKey(-1.0, -0.7, &none)) == 1);
    CHECK(ShownOn0(HeldKey(-1.0, -0.5, &none)) == 2);   // 0.5 > 0.405
    // One row without a belief row: the margin.
    {
        auto in = HeldKey(-1.0, -0.7, &narrow);
        in.candidates[0].beliefRow = SA::kNone;
        CHECK(ShownOn0(in) == 1);
    }

    // tau and ln m from the settings.
    {
        auto in = HeldKey(-1.0, -0.7, &narrow);
        in.settings.challengerConfidence = 0.99f;   // z 2.33: line 0.47
        CHECK(ShownOn0(in) == 1);
        in.settings.challengerConfidence = 0.5f;    // z 0: any lead
        CHECK(ShownOn0(in) == 2);
        in.settings.challengerMinGap = 0.35f;       // ln m 0.35 > 0.3
        CHECK(ShownOn0(in) == 1);
    }

    // The hold switch still rules: off, the fill alone decides (rank order).
    {
        auto in = HeldKey(-1.0, -0.7, &wide);
        in.settings.holdSeatedItems = false;
        CHECK(ShownOn0(in) == 2);
    }
}

TEST_CASE("challenger rule: the fixed-margin path is the R8 hold, bit for bit (no beliefs given)")
{
    // The golden tests replay recorded pages with no beliefs; here: a run of
    // synthetic snapshots where beliefRow is set on every row but no table is
    // given must equal the run with no rows at all.
    T::SyntheticGenerator gen(4242, T::SyntheticSpec{ .exoticSettings = true });
    for (int s = 0; s < 300; ++s) {
        const auto settings = gen.RandomSettings();
        const auto layout = gen.RandomLayout();
        auto in = gen.Next(settings, layout, SA::PageMemory{}, true);
        const auto a = SA::Allocate(in);
        for (std::uint32_t i = 0; i < in.candidates.size(); ++i) in.candidates[i].beliefRow = i;
        const auto b = SA::Allocate(in);
        CHECK(T::DiffOutputs(in, a, b).empty());
    }
}

// =============================================================================
// No swap-back on fixed beliefs
// =============================================================================

namespace
{
    /// A random page whose scores and beliefs never change, run pass after
    /// pass with the seating memory carried over, as the game runs it when
    /// nothing in the world moves.
    struct FixedWorld
    {
        SA::Input in;
        C::BeliefTable table;
    };

    FixedWorld MakeWorld(std::mt19937& rng, T::SyntheticGenerator& gen)
    {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        C::Model m;
        m.theta.assign(C::K * C::J, C::Weight{ 0.0, 0.0 });
        const std::uint16_t cols[] = { kRH, kRM, kOH, 7, 40, 41 };
        const std::size_t needs[] = { kHD, kMD, 3 };
        for (const auto k : needs) {
            for (const auto j : cols) m.Th(k, j) = { 0.0, 0.05 + u(rng) };
        }
        m.covered = { 0.0, 0.2 + u(rng) };
        std::array<double, C::K> n{};
        for (const auto k : needs) n[k] = u(rng);
        const C::Tick t = C::MakeTick(m, n, {});

        FixedWorld w{ {}, C::BeliefTable(t, m.covered.var) };
        auto settings = gen.RandomSettings();
        settings.keepSlotPositions = true;
        settings.holdSeatedItems = true;
        settings.challengerConfidence = static_cast<float>(0.5 + 0.49 * u(rng));
        settings.challengerMinGap = u(rng) < 0.5 ? 0.0f : static_cast<float>(0.3 * u(rng));
        auto& in = w.in;
        in.settings = settings;
        in.slots = gen.RandomLayout();
        in.nowNs = 1'000'000'000'000LL;
        // Every dedup key and name unique: two candidates sharing a key make
        // the seat-owner lookup (first match) read a taken key as free, and
        // the page can churn with nothing moving -- allocator behaviour that
        // predates R9 (the fixed margin churns more on such pages), not what
        // this test is about.
        const int wanted = gen.Int(2, 30);
        std::set<std::uint64_t> keys;
        std::set<std::string> names;
        for (int tries = 0; static_cast<int>(in.candidates.size()) < wanted && tries < 400; ++tries) {
            auto c = gen.RandomCandidate();
            if (keys.contains(c.dedupKey) || names.contains(c.name)) continue;
            keys.insert(c.dedupKey);
            names.insert(c.name);
            // (R10: no wildcard flag on a row any more; the input pins and rolls none.)
            // Scores close together, so the line decides often: a spread of
            // about one sigma_Delta.
            c.score = std::normal_distribution<double>(-1.0, 0.6)(rng);
            std::vector<C::SparseCol> cap;
            for (const auto j : cols) {
                if (u(rng) < 0.35) cap.emplace_back(j, u(rng));
            }
            c.beliefRow = w.table.Add(C::MakeAttr(C::Grading::Graded, cap, {}), u(rng) < 0.2 ? u(rng) : 0.0,
                0.01 + 0.25 * u(rng));
            in.candidates.push_back(std::move(c));
        }
        std::stable_sort(in.candidates.begin(), in.candidates.end(),
            [](const SA::CandidateRec& a, const SA::CandidateRec& b) { return a.score > b.score; });
        const int count = static_cast<int>(in.candidates.size());
        // The page as the beliefs before these ones left it: random items
        // seated, so the hold has holders to judge (with an empty memory the
        // first fill seats the best items and nothing is ever challenged).
        for (std::size_t j = 0; j < in.slots.size() && j < SA::kMaxSlots; ++j) {
            if (u(rng) < 0.8) {
                const auto& c = in.candidates[static_cast<std::size_t>(gen.Int(0, count - 1))];
                in.memory.seats[j] = c.dedupKey;
                in.memory.lastPlaced[j] = c.dedupKey;
            }
        }
        return w;
    }

    struct SwapBackCount
    {
        int giveWays = 0;
        int swapBacks = 0;       // a give-way that undoes an earlier one (holder and challenger reversed)
        int lateGiveWays = 0;    // give-ways in the last passes: the page has not settled
    };

    /// Runs `passes` allocations of one fixed world with `policy`, carrying
    /// the memory, and counts the give-ways that reverse an earlier one.
    template <class Policy>
    SwapBackCount RunFixed(FixedWorld& w, const Policy& policy, int passes, std::mt19937& rng)
    {
        SwapBackCount r;
        std::set<std::pair<std::uint32_t, std::uint32_t>> beaten;   // (winner row, loser row): candidate indices, fixed
        SA::Input in = w.in;
        in.beliefs = &w.table;
        std::uniform_int_distribution<int> dt(0, 30);
        for (int p = 0; p < passes; ++p) {
            in.nowNs += static_cast<std::int64_t>(dt(rng)) * 100'000'000LL;   // 0-3 s
            const auto out = SA::Allocate(in, policy);
            for (const auto& e : out.events) {
                if (e.kind != SA::EventKind::HoldGaveWay) continue;
                const std::uint32_t holder = e.b;
                const std::uint32_t challenger = e.c;
                ++r.giveWays;
                if (p >= passes - 3) ++r.lateGiveWays;
                if (beaten.contains({ holder, challenger })) ++r.swapBacks;
                beaten.insert({ challenger, holder });
            }
            in.memory = out.memory;
            in.generationMatches = out.generationMatches;
        }
        return r;
    }

    /// Thompson-style sampling, the rule doc 9 rules out for flicker: each
    /// comparison draws the gap from its posterior, so the same beliefs can
    /// answer yes, then no. The contrast that shows the property test can see
    /// a swap-back when there is one.
    struct SampledPolicy : SA::LogScorePolicy
    {
        std::mt19937* rng = nullptr;
        [[nodiscard]] bool ExceedsBelief(Score c, Score h, double sigmaDelta) const
        {
            const double draw = std::normal_distribution<double>(c - h, sigmaDelta)(*rng);
            return draw > minGap;
        }
    };
}  // namespace

TEST_CASE("challenger rule: no swap-back on fixed beliefs (property test over random pages)")
{
    std::mt19937 rng(20261029);
    T::SyntheticGenerator gen(77, T::SyntheticSpec{ .exoticSettings = true, .formPool = 40, .maxCandidates = 30 });
    int worlds = 0, giveWays = 0, unsettled = 0;
    for (int s = 0; s < 600; ++s) {
        FixedWorld w = MakeWorld(rng, gen);
        const auto policy = SA::LogScorePolicy::From(w.in.settings);
        const auto r = RunFixed(w, policy, 12, rng);
        INFO("world " << s);
        CHECK(r.swapBacks == 0);
        giveWays += r.giveWays;
        unsettled += r.lateGiveWays > 0;
        ++worlds;
    }
    // The worlds exercise the rule (give-ways happen), and every one settles:
    // with nothing moving, no key changes hands in the last three passes.
    // Evidence for the algebra in ChallengerRule.h, not the proof: it counts
    // direct (holder, challenger) reversals, not longer cycles, and settling
    // holds on unique dedup keys (MakeWorld) -- see the note there.
    MESSAGE("fixed beliefs: " << giveWays << " give-ways in " << worlds << " worlds, no swap-back");
    CHECK(worlds == 600);
    CHECK(giveWays > 200);
    CHECK(unsettled == 0);
}

TEST_CASE("challenger rule: the property test sees swap-backs under per-pass sampling (contrast)")
{
    std::mt19937 rng(20261029);
    T::SyntheticGenerator gen(77, T::SyntheticSpec{ .exoticSettings = true, .formPool = 40, .maxCandidates = 30 });
    std::mt19937 draws(5);
    int swapBacks = 0;
    for (int s = 0; s < 600; ++s) {
        FixedWorld w = MakeWorld(rng, gen);
        SampledPolicy policy;
        static_cast<SA::LogScorePolicy&>(policy) = SA::LogScorePolicy::From(w.in.settings);
        policy.rng = &draws;
        swapBacks += RunFixed(w, policy, 12, rng).swapBacks;
    }
    MESSAGE("per-pass sampling: " << swapBacks << " swap-backs on the same worlds");
    CHECK(swapBacks > 20);
}

TEST_CASE("challenger rule: a swap is never answered by a swap back, named case")
{
    // One key; the challenger clears the line, takes the key, and on every
    // later pass the old holder -- now the challenger -- is behind and stays
    // out, however long the beliefs stand.
    PairBeliefs b;
    b.Set(0, 1, 0.3);
    auto in = HeldKey(-1.0, -0.6, &b);   // 0.4 > 0.84 * 0.3 = 0.25
    // Candidate order is the rank order: the holder (form 1) is second.
    for (int p = 0; p < 20; ++p) {
        in.nowNs += 1'000'000'000LL;
        const auto out = SA::Allocate(in);
        REQUIRE_FALSE(out.slots.empty());
        CHECK(in.candidates[out.slots[0].src].formID == 2);
        in.memory = out.memory;
    }
}

// =============================================================================
// Churn telemetry on Delta / sigma_Delta
// =============================================================================

TEST_CASE("challenger rule: churn buckets on Delta / sigma_Delta")
{
    using B = DeltaSdBucket;
    CHECK(BucketDeltaSd(1.0, std::nullopt, 0.5) == B::Gone);
    CHECK(BucketDeltaSd(1.0, std::nullopt, std::nullopt) == B::Gone);
    CHECK(BucketDeltaSd(1.0, kUnrankedScore, 0.5) == B::Above4);   // as the ratio buckets' "<= 0"
    CHECK(BucketDeltaSd(1.0, 0.0, std::nullopt) == B::NoSd);
    CHECK(BucketDeltaSd(1.0, 0.0, kNaN) == B::NoSd);
    CHECK(BucketDeltaSd(-0.1, 0.0, 0.5) == B::Below0);
    CHECK(BucketDeltaSd(kNaN, 0.0, 0.5) == B::Below0);
    // Edges at 0.5, 1, 2, 4 sigma_Delta, any sign of the scores.
    CHECK(BucketDeltaSd(-2.0 + 0.0, -2.0, 0.5) == B::Below05);
    CHECK(BucketDeltaSd(-2.0 + 0.2499, -2.0, 0.5) == B::Below05);
    CHECK(BucketDeltaSd(-2.0 + 0.25, -2.0, 0.5) == B::Below1);
    CHECK(BucketDeltaSd(-2.0 + 0.4999, -2.0, 0.5) == B::Below1);
    CHECK(BucketDeltaSd(-2.0 + 0.5, -2.0, 0.5) == B::Below2);
    CHECK(BucketDeltaSd(-2.0 + 1.0, -2.0, 0.5) == B::Below4);
    CHECK(BucketDeltaSd(-2.0 + 1.9999, -2.0, 0.5) == B::Below4);
    CHECK(BucketDeltaSd(-2.0 + 2.0, -2.0, 0.5) == B::Above4);
    // A certain gap.
    CHECK(BucketDeltaSd(0.1, 0.0, 0.0) == B::Above4);
    CHECK(BucketDeltaSd(0.0, 0.0, 0.0) == B::Below05);
    // Names, for the heartbeat.
    CHECK(std::string(DeltaSdBucketName(B::Below05)) == "<0.5");
    CHECK(std::string(DeltaSdBucketName(B::Above4)) == ">=4");
    CHECK(std::string(DeltaSdBucketName(B::NoSd)) == "nosd");
}

TEST_CASE("challenger rule: under the line -- the changes the rule would not have let through")
{
    const double z = ZForConfidence(0.8);
    CHECK(UnderLine(0.2, 0.0, 0.5, 0.0, z));          // 0.2 <= 0.42
    CHECK_FALSE(UnderLine(0.5, 0.0, 0.5, 0.0, z));
    CHECK(UnderLine(-0.5, 0.0, 0.5, 0.0, z));         // worse: under
    CHECK(UnderLine(0.5, 0.0, 0.5, 0.2, z));          // ln m raises the line
    CHECK_FALSE(UnderLine(0.2, std::nullopt, 0.5, 0.0, z));     // gone
    CHECK_FALSE(UnderLine(0.2, kUnrankedScore, 0.5, 0.0, z));   // unranked incumbent
    CHECK_FALSE(UnderLine(0.2, 0.0, std::nullopt, 0.0, z));     // no belief
    // The hold's own test and the line agree: through the hold means not under.
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> mu(-3.0, 3.0), sd(0.0, 1.5);
    for (int i = 0; i < 5000; ++i) {
        const double a = mu(rng), b = mu(rng), s = sd(rng);
        CHECK(UnderLine(a, b, s, 0.0, z) == !Exceeds(a, b, s, 0.0, z));
    }
}
