// 0.24.17: the D2 engine "A" in shadow beside the live engine "B"
// (core/ShadowCompare.h, core/SlotAllocStateless.h, core/ChoiceLive.h RankMu).
//
// What the pipeline relies on (PipelineCoordinator::RankOnChoice and
// ShadowLegacyPages):
//   1. A's mu, computed on a COPY of B's extras with the D2 feature put back
//      and run through RankMu, is bit for bit the mu the live pass computes
//      with bLegacyLearnFeature on -- with the summon upgrade gate active,
//      whose s-hat reads the feature -- and computing it leaves B's own
//      scores unchanged;
//   2. RanksOf orders like UtilityScorer::Rank (RanksBefore on score and
//      tie-break), exact ties in the live order;
//   3. a stateless page ignores every piece of memory the live input carries
//      (seating, Remembrance holds, the wildcard), never shows a remembered
//      or wildcard slot, depends on the candidates' scores and not on their
//      order in the input, and on an input that had no memory to begin with
//      is the page the live allocation recorded;
//   4. the [Soak] tally counts what the heartbeat says it counts.
// And the cost of the shadow over 500 rows (reported, not asserted).

#include "SlotAllocTestSupport.h"
#include "core/Bootstrap.h"
#include "core/ChoiceLive.h"
#include "core/ChoiceScore.h"
#include "core/LegacyShadowPass.h"
#include "core/ShadowCompare.h"
#include "core/SlotAllocStateless.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace Huginn::Core;
namespace C = Huginn::Core::Choice;
namespace SC = Huginn::Core::ShadowCompare;
namespace SA = Huginn::Core::SlotAlloc;

namespace
{
    C::Model ShippedModel()
    {
        std::ifstream in(std::string(HUGINN_REPO_ROOT) + "/configs/Huginn_Bootstrap.json", std::ios::binary);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        const auto read = Bootstrap::Read(ss.str());
        REQUIRE(read.Ok());
        auto mapped = Bootstrap::MapToBuild(*read.doc);
        C::Model m = mapped.model;
        m.covered = C::Weight{ -1.0, 1.0 };   // the [Scoring] fCoveredPrior default: the gate's covered matters
        return m;
    }

    /// A synthetic run: rows with caps, b, covered; the legacy rows (pass /
    /// cold) with their ln learn mult; some summon rows under the gate and a
    /// comparison entry past the rows (a_out's source, not a row).
    struct Run
    {
        std::vector<C::Attr> attrs;
        std::vector<C::RowExtra> extras;   // B's: the D2 feature off (0 / 1)
        std::vector<C::GateRow> gates;
        std::vector<bool> scored;          // the old floors kept the row
        std::vector<double> lnLearnMult;
        std::size_t rows = 0;
        std::size_t outIdx = SIZE_MAX;
        double leftOut = 1.0;
    };

    Run MakeRun(const C::Model& m, std::size_t rows, bool gate, unsigned seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        Run r;
        r.rows = rows;
        for (std::size_t i = 0; i < rows; ++i) {
            std::vector<C::SparseCol> cap;
            const int n = 2 + static_cast<int>(rng() % 6);
            for (int c = 0; c < n; ++c) cap.emplace_back(static_cast<std::uint16_t>(rng() % C::J), u(rng));
            r.attrs.push_back(C::MakeAttr(m.grading, cap, std::vector<C::SparseCol>{}));
            C::RowExtra x;
            x.bMu = u(rng) - 0.5;
            x.bVar = 0.1 + u(rng);
            x.covered = (rng() % 4 == 0) ? u(rng) : 0.0;
            x.feat[static_cast<std::size_t>(C::Feature::eligible)] = 1.0;
            const bool kept = rng() % 3 != 0;
            const double lm = kept ? std::log(1.0 + 2.0 * u(rng)) : 0.0;
            C::SetLegacy(x.feat, C::LegacyFeature(false, kept, lm));   // B: 0 / 1 whatever the floors
            r.extras.push_back(x);
            r.scored.push_back(kept);
            r.lnLearnMult.push_back(lm);
            C::GateRow g;
            if (gate && rng() % 4 == 0) g = C::GateRow{ true, 0.1 * u(rng) };
            r.gates.push_back(g);
        }
        if (gate) {
            std::vector<C::SparseCol> cap{ { static_cast<std::uint16_t>(rng() % C::J), 0.8 } };
            r.attrs.push_back(C::MakeAttr(m.grading, cap, std::vector<C::SparseCol>{}));
            C::RowExtra x;
            x.bMu = 0.2;
            x.bVar = 0.3;
            x.feat[static_cast<std::size_t>(C::Feature::util_missing)] = 1.0;   // as RankOnChoice sets it
            r.extras.push_back(x);
            r.gates.push_back(C::GateRow{});
            r.outIdx = r.attrs.size() - 1;
            r.leftOut = 0.6;
        }
        return r;
    }

    C::Tick MakeTickFor(const C::Model& m, unsigned seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::array<double, C::K> n{};
        for (int k = 0; k < 10; ++k) n[rng() % C::K] = u(rng);
        std::vector<double> rec(m.recent.size(), 0.0);
        for (std::size_t f = 0; f < rec.size(); f += 5) rec[f] = u(rng);
        return C::MakeTick(m, n, rec);
    }
}

TEST_CASE("legacy shadow: A's mu is the live pass's mu with bLegacyLearnFeature on, and B's is untouched")
{
    const C::Model m = ShippedModel();
    // The shipped bootstrap weighs the D2 feature: the test means something.
    REQUIRE(m.w[static_cast<std::size_t>(C::Feature::ln_learn_mult)] != 0.0);

    for (const bool gate : { false, true }) {
        CAPTURE(gate);
        const C::Tick tick = MakeTickFor(m, gate ? 11u : 12u);
        const Run run = MakeRun(m, 300, gate, gate ? 21u : 22u);
        const std::size_t out = gate ? run.outIdx : SIZE_MAX;

        // The live pass under bLegacyLearnFeature = 1 (D2 live): the
        // feature as RankOnChoice sets it with the switch on.
        auto liveA = run.extras;
        for (std::size_t i = 0; i < run.rows; ++i)
            C::SetLegacy(liveA[i].feat, C::LegacyFeature(true, run.scored[i], run.lnLearnMult[i]));
        std::vector<C::LiveScore> liveAScores;
        C::ScoreForRanking(m, tick, run.attrs, liveA, run.gates, out, run.leftOut, run.rows, liveAScores);

        // The live pass under bLegacyLearnFeature = 0 (B), alone.
        auto bAlone = run.extras;
        std::vector<C::LiveScore> bAloneScores;
        C::ScoreForRanking(m, tick, run.attrs, bAlone, run.gates, out, run.leftOut, run.rows, bAloneScores);

        // RankOnChoice's order with the shadow on: the pure pass's ScoreMu on
        // the extras first (it copies them), then B's pass on the original.
        auto extras = run.extras;
        std::vector<C::LegacyValues> legacyShadowFeat(run.rows);
        for (std::size_t i = 0; i < run.rows; ++i)
            legacyShadowFeat[i] = C::LegacyFeature(true, run.scored[i], run.lnLearnMult[i]);
        Huginn::Core::LegacyShadow::Run shadow;
        Huginn::Core::LegacyShadow::ScoreMu(m, tick, run.attrs, extras, run.gates, out, run.leftOut, run.rows,
                                            legacyShadowFeat, shadow);
        REQUIRE(shadow.ran);
        const auto& muA = shadow.mu;
        // A's gated covered, for the check below: the same copy RankMu ran on.
        std::vector<C::RowExtra> shadowExtras = extras;
        for (std::size_t i = 0; i < run.rows; ++i) C::SetLegacy(shadowExtras[i].feat, legacyShadowFeat[i]);
        {
            std::vector<double> again;
            C::RankMu(m, tick, run.attrs, shadowExtras, run.gates, out, run.leftOut, run.rows, again);
            CHECK(again == muA);
        }
        std::vector<C::LiveScore> bScores;
        C::ScoreForRanking(m, tick, run.attrs, extras, run.gates, out, run.leftOut, run.rows, bScores);

        REQUIRE(muA.size() == run.rows);
        std::size_t differ = 0;
        for (std::size_t i = 0; i < run.rows; ++i) {
            CAPTURE(i);
            CHECK(muA[i] == liveAScores[i].s.mu);            // bit for bit: the same arithmetic
            CHECK(bScores[i].s.mu == bAloneScores[i].s.mu);  // B is untouched by the shadow
            CHECK(extras[i].covered == bAlone[i].covered);   // ...and so is B's gate
            if (muA[i] != bScores[i].s.mu) ++differ;
        }
        CHECK(differ > run.rows / 3);   // the feature moves the rows the floors kept
        if (gate) {
            // The gate read A's s-hat on A's copy: some summon row's covered
            // differs between the two engines.
            std::size_t gated = 0, moved = 0;
            for (std::size_t i = 0; i < run.rows; ++i) {
                if (!run.gates[i].summon) continue;
                ++gated;
                if (shadowExtras[i].covered != extras[i].covered) ++moved;
            }
            CHECK(gated > 0);
            CHECK(moved > 0);
        }
    }
}

TEST_CASE("legacy shadow: RankMu ranks exactly as ScoreForRanking (NaN as -inf)")
{
    const C::Model m = ShippedModel();
    const C::Tick tick = MakeTickFor(m, 5u);
    Run run = MakeRun(m, 120, true, 6u);
    run.extras[3].bMu = std::numeric_limits<double>::quiet_NaN();
    auto a = run.extras, b = run.extras;
    std::vector<double> mu;
    std::vector<C::LiveScore> full;
    C::RankMu(m, tick, run.attrs, a, run.gates, run.outIdx, run.leftOut, run.rows, mu);
    C::ScoreForRanking(m, tick, run.attrs, b, run.gates, run.outIdx, run.leftOut, run.rows, full);
    for (std::size_t i = 0; i < run.rows; ++i) {
        CAPTURE(i);
        CHECK(mu[i] == full[i].s.mu);
        CHECK(a[i].covered == b[i].covered);
    }
    CHECK(mu[3] == -std::numeric_limits<double>::infinity());
}

TEST_CASE("legacy shadow: RanksOf orders as the live sort, ties in the live order")
{
    // rows 0..5; live order 2, 0, 1, 3, 4 (row 5 not ranked)
    const std::vector<double> score{ 1.0, 1.0, 3.0, -0.5, 1.0, 9.0 };
    const std::vector<float> tie{ 0.0f, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    const std::vector<std::uint32_t> live{ 2, 0, 1, 3, 4 };
    std::vector<std::int16_t> rank;
    SC::RanksOf(score, tie, live, rank);
    REQUIRE(rank.size() == 6);
    CHECK(rank[2] == 1);   // 3.0
    CHECK(rank[1] == 2);   // 1.0, tie-break 5
    CHECK(rank[0] == 3);   // 1.0, tie 0: before row 4 in the live order
    CHECK(rank[4] == 4);
    CHECK(rank[3] == 5);
    CHECK(rank[5] == SC::kNoRank);   // not in the live list: never ranked
    // NaN ranks last, as RanksBefore reads it
    const std::vector<double> nanScore{ std::numeric_limits<double>::quiet_NaN(), 0.0 };
    const std::vector<float> zero{ 0.0f, 0.0f };
    const std::vector<std::uint32_t> order{ 0, 1 };
    SC::RanksOf(nanScore, zero, order, rank);
    CHECK(rank[1] == 1);
    CHECK(rank[0] == 2);
}

namespace
{
    /// What PipelineCoordinator::ShadowLegacyPages runs: the pure pass's
    /// RanksAndPages on its own copy of the input (B's stateless page in
    /// place, then A's on the result), one row per candidate; both keys
    /// returned indexed by the input's original order.
    struct TwoPages
    {
        std::vector<std::int8_t> keyA, keyB0;
    };
    TwoPages PipelineSequence(SA::Input in, const std::vector<double>& scoresA, const std::vector<double>& scoresB)
    {
        const std::size_t n = in.candidates.size();
        Huginn::Core::LegacyShadow::Run run;
        run.mu = scoresA;
        std::vector<std::uint32_t> rows(n), live(n);
        std::vector<float> tie(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            rows[i] = i;
            live[i] = i;
            tie[i] = in.candidates[i].tieBreak;
        }
        Huginn::Core::LegacyShadow::RanksAndPages(live, scoresB, tie, &in, rows, run);
        return { run.keyA, run.keyB0 };
    }

    std::vector<double> ScoresOf(const SA::Input& in)
    {
        std::vector<double> s;
        for (const auto& c : in.candidates) s.push_back(c.score);
        return s;
    }

    bool NoMemory(const SA::Input& in)
    {
        if (in.wildcard.pinned != SA::kNone || in.wildcard.roll) return false;
        for (const auto& h : in.holds)
            if (h.active) return false;
        if (in.memoryAvailable) {
            if (in.memory != SA::PageMemory{}) return false;
        }
        return true;
    }
}

TEST_CASE("legacy shadow: a stateless page ignores memory and matches a memoryless live allocation")
{
    std::string err;
    const auto files = Huginn::Test::LoadSlotFixtures(&err);
    REQUIRE_MESSAGE(!files.empty(), err);
    std::size_t total = 0, comparable = 0;
    std::mt19937 rng(3);
    for (const auto& f : files) {
        for (std::size_t si = 0; si < f.snaps.size(); ++si) {
            const SA::Input& in = f.snaps[si].in;
            if (in.slots.empty()) continue;
            CAPTURE(f.name);
            CAPTURE(si);
            ++total;
            const auto scores = ScoresOf(in);
            const auto page = SA::AllocateStateless(in, scores);
            REQUIRE(page.keyOf.size() == in.candidates.size());
            REQUIRE(page.page.size() == in.slots.size());
            for (const auto& p : page.page) {
                CHECK(p.kind != SA::Kind::Remembered);
                CHECK(p.kind != SA::Kind::Wildcard);
                CHECK_FALSE(p.seatMoved);
            }

            // Memory, holds and the wildcard make no difference.
            SA::Input noisy = in;
            noisy.memoryAvailable = true;
            for (auto& s : noisy.memory.seats) s = in.candidates.empty() ? 0 : in.candidates[rng() % in.candidates.size()].dedupKey;
            for (std::size_t j = 0; j < noisy.holds.size(); ++j) {
                if (!in.candidates.empty() && rng() % 2) noisy.holds[j] = { true, in.candidates[rng() % in.candidates.size()].formID };
            }
            noisy.wildcard.roll = true;
            noisy.wildcard.uRoll = 0.0;
            if (!in.candidates.empty()) noisy.wildcard.pinned = static_cast<std::uint32_t>(rng() % in.candidates.size());
            const auto page2 = SA::AllocateStateless(noisy, scores);
            CHECK(page2.page == page.page);
            CHECK(page2.keyOf == page.keyOf);

            // The candidates' order in the input does not matter, only their
            // scores (made distinct, so no tie falls back on the order).
            std::vector<double> distinct(scores.size());
            for (std::size_t i = 0; i < distinct.size(); ++i) distinct[i] = static_cast<double>(i) * 0.37 - 5.0;
            std::vector<std::uint32_t> perm(in.candidates.size());
            for (std::uint32_t i = 0; i < perm.size(); ++i) perm[i] = i;
            std::shuffle(perm.begin(), perm.end(), rng);
            SA::Input shuffled = in;
            std::vector<double> shuffledScores(distinct.size());
            for (std::size_t k = 0; k < perm.size(); ++k) {
                shuffled.candidates[k] = in.candidates[perm[k]];
                shuffledScores[k] = distinct[perm[k]];
            }
            const auto a = SA::AllocateStateless(in, distinct);
            const auto b = SA::AllocateStateless(shuffled, shuffledScores);
            CHECK(a.page == b.page);
            for (std::size_t k = 0; k < perm.size(); ++k) CHECK(b.keyOf[k] == a.keyOf[perm[k]]);

            // An input with no memory to begin with, its candidates already in
            // rank order: the stateless page is the page the game recorded.
            bool sorted = true;
            for (std::size_t i = 1; i < in.candidates.size(); ++i) {
                const auto& p = in.candidates[i - 1];
                const auto& c = in.candidates[i];
                if (RanksBefore(c.score, c.tieBreak, p.score, p.tieBreak)) sorted = false;
            }
            if (f.snaps[si].hasResult && NoMemory(in) && sorted) {
                ++comparable;
                CHECK_MESSAGE(page.page == f.snaps[si].page, Huginn::Test::Describe(page.page) << " vs recorded "
                                                                  << Huginn::Test::Describe(f.snaps[si].page));
            }

            // The pipeline's in-place sequence (B's page, then A's on the
            // reordered input) gives the pages the copying calls give.
            {
                std::vector<double> scoresA(scores.size());
                for (std::size_t i = 0; i < scoresA.size(); ++i) scoresA[i] = 3.0 * std::sin(1.7 * static_cast<double>(i) + 0.3);   // distinct: no tie leans on the input order
                const auto two = PipelineSequence(in, scoresA, scores);
                CHECK(two.keyB0 == SA::AllocateStateless(in, scores).keyOf);
                CHECK(two.keyA == SA::AllocateStateless(in, scoresA).keyOf);
            }

            // keyOf names the key showing each candidate's item.
            for (std::size_t i = 0; i < in.candidates.size(); ++i) {
                if (page.keyOf[i] < 0) continue;
                const auto& p = page.page[static_cast<std::size_t>(page.keyOf[i])];
                CHECK(p.formID == in.candidates[i].formID);
            }
        }
    }
    MESSAGE(total << " recorded allocations, " << comparable << " with no memory compared against the recorded page");
    CHECK(total > 50);
    CHECK(comparable > 0);
}

TEST_CASE("legacy shadow: A's page takes A's best rows")
{
    // Three Regular keys, six candidates that fit them: B ranks 0..5, A the
    // reverse. B's stateless page shows 0, 1, 2; A's shows 5, 4, 3.
    SA::Input in;
    for (int k = 0; k < 3; ++k) {
        SA::SlotRec s;
        s.classIndex = Huginn::Test::kRegular;
        s.regular = true;
        s.priority = static_cast<std::int8_t>(10 - k);
        s.wildcardsEnabled = false;
        in.slots.push_back(s);
    }
    in.settings.classDiscount = 1.0f;   // no class cap
    std::vector<double> b, a;
    for (std::uint32_t i = 0; i < 6; ++i) {
        SA::CandidateRec c;
        c.formID = 0x100 + i;
        c.dedupKey = 0x100 + i;
        c.name = "item" + std::to_string(i);
        c.matchMask = 1u << Huginn::Test::kRegular;
        c.score = 6.0 - i;
        in.candidates.push_back(c);
        b.push_back(6.0 - i);
        a.push_back(static_cast<double>(i));
    }
    const auto pageB = SA::AllocateStateless(in, b);
    const auto pageA = SA::AllocateStateless(in, a);
    CHECK(pageB.keyOf == std::vector<std::int8_t>{ 0, 1, 2, -1, -1, -1 });
    CHECK(pageA.keyOf == std::vector<std::int8_t>{ -1, -1, -1, 2, 1, 0 });
    // A remembered-only row is never on a stateless page, whatever its score.
    in.candidates[5].isRememberedOnly = true;
    const auto pageR = SA::AllocateStateless(in, a);
    CHECK(pageR.keyOf[5] == -1);
}

TEST_CASE("legacy shadow: the [Soak] tally")
{
    using SC::ClassifyPick;
    using SC::PickKind;
    // nothing, repeats and outside picks that are not a menu are not counted
    CHECK(ClassifyPick(true, false, false, false, true, true, 0, 0).kind == PickKind::None);
    CHECK(ClassifyPick(false, true, true, false, true, true, 0, 0).kind == PickKind::None);
    CHECK(ClassifyPick(false, false, false, false, true, true, 0, 0).kind == PickKind::None);   // vanilla hotkey, own wheel
    CHECK(ClassifyPick(false, false, true, false, true, false, SC::kNoKey, SC::kNoKey).kind == PickKind::None);   // no chosen row (shadow_compare.py skips it too)
    // no shadow behind the context
    CHECK(ClassifyPick(false, false, true, false, false, true, 2, 2).kind == PickKind::NoData);
    // a key pick on both pages; a reach-in on A's page only; a reach-in of a
    // row the run never ranked (no key) is off both
    const auto key = ClassifyPick(false, false, true, false, true, true, 3, 1);
    CHECK(key.kind == PickKind::Key);
    CHECK(key.onA);
    CHECK(key.onB0);
    const auto reach = ClassifyPick(false, false, false, true, true, true, 4, -1);
    CHECK(reach.kind == PickKind::Reach);
    CHECK(reach.onA);
    CHECK_FALSE(reach.onB0);
    const auto unranked = ClassifyPick(false, false, false, true, true, true, SC::kNoKey, SC::kNoKey);
    CHECK(unranked.kind == PickKind::Reach);
    CHECK_FALSE(unranked.onA);
    CHECK_FALSE(unranked.onB0);

    SC::Tally t;
    t.Add(key);
    t.Add(reach);
    t.Add(unranked);
    t.Add(ClassifyPick(false, false, false, true, true, true, -1, 0));   // B0 only
    t.Add(ClassifyPick(false, false, true, false, false, true, 0, 0));   // no data
    CHECK(t.keys == 1);
    CHECK(t.keysOnA == 1);
    CHECK(t.keysOnB0 == 1);
    CHECK(t.reach == 3);
    CHECK(t.reachOnA == 1);
    CHECK(t.reachOnB0 == 1);
    CHECK(t.reachAOnly == 1);
    CHECK(t.reachB0Only == 1);
    CHECK(t.noData == 1);
}

TEST_CASE("legacy shadow cost: A's mu, the ranks and two stateless pages over 500 rows (reported)")
{
    using Clock = std::chrono::steady_clock;
    const C::Model m = ShippedModel();
    const C::Tick tick = MakeTickFor(m, 9u);
    const Run run = MakeRun(m, 500, true, 10u);

    // A 10-key page with mixed classes, 500 candidates.
    std::string err;
    const auto files = Huginn::Test::LoadSlotFixtures(&err);
    REQUIRE(!files.empty());
    const SA::Input* layout = nullptr;
    for (const auto& f : files)
        for (const auto& s : f.snaps)
            if (!layout || s.in.slots.size() > layout->slots.size()) layout = &s.in;
    REQUIRE(layout != nullptr);
    SA::Input in = *layout;
    std::mt19937 rng(4);
    const auto base = in.candidates;
    REQUIRE(!base.empty());
    in.candidates.clear();
    for (std::uint32_t i = 0; i < 500; ++i) {
        SA::CandidateRec c = base[i % base.size()];
        c.formID = 0x1000 + i;
        c.dedupKey = 0x1000 + i;
        c.score = std::uniform_real_distribution<double>(-3.0, 3.0)(rng);
        in.candidates.push_back(c);
    }
    std::vector<double> b = ScoresOf(in), a(b.size());
    for (auto& v : a) v = std::uniform_real_distribution<double>(-3.0, 3.0)(rng);
    std::vector<float> tie(500, 0.0f);
    std::vector<std::uint32_t> live(500);
    for (std::uint32_t i = 0; i < 500; ++i) live[i] = i;

    const int reps = 20;
    double muUs = 0.0, pagesUs = 0.0;
    std::size_t sink = 0;
    for (int rep = 0; rep < reps; ++rep) {
        const auto t0 = Clock::now();
        auto extras = run.extras;
        std::vector<double> mu;
        C::RankMu(m, tick, run.attrs, extras, run.gates, run.outIdx, run.leftOut, run.rows, mu);
        const auto t1 = Clock::now();
        std::vector<std::int16_t> rankA, rankB;
        SC::RanksOf(a, tie, live, rankA);
        SC::RanksOf(b, tie, live, rankB);
        // The pipeline owns its input (the allocator moves it over): the
        // copy here stands for that move and is not timed.
        const auto copyStart = Clock::now();
        SA::Input own = in;
        const auto copyEnd = Clock::now();
        const auto two = PipelineSequence(std::move(own), a, b);
        const auto t2 = Clock::now();
        muUs += std::chrono::duration<double, std::micro>(t1 - t0).count();
        pagesUs += std::chrono::duration<double, std::micro>((t2 - t1) - (copyEnd - copyStart)).count();
        sink += mu.size() + static_cast<std::size_t>(rankA[0] + rankB[0]) + two.keyA.size() + two.keyB0.size();
    }
    // Where a stateless page's time goes: building its input, and the allocation on it.
    double inputUs = 0.0, allocUs = 0.0;
    for (int rep = 0; rep < reps; ++rep) {
        std::vector<std::uint32_t> order;
        const auto t0 = Clock::now();
        const SA::Input sin = SA::StatelessInput(in, a, order);
        const auto t1 = Clock::now();
        const auto out = SA::Allocate(sin);
        const auto t2 = Clock::now();
        inputUs += std::chrono::duration<double, std::micro>(t1 - t0).count();
        allocUs += std::chrono::duration<double, std::micro>(t2 - t1).count();
        sink += out.slots.size();
    }
    MESSAGE("one stateless page: its input " << inputUs / reps << " us, the allocation on it " << allocUs / reps
                                             << " us");
    // For scale: one allocation of the same input as the live one runs it (memory and all).
    double liveUs = 0.0;
    for (int rep = 0; rep < reps; ++rep) {
        const auto t0 = Clock::now();
        const auto out = SA::Allocate(in);
        liveUs += std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        sink += out.slots.size();
    }
    MESSAGE("legacy shadow over 500 rows: A's mu (extras copy + RankMu) " << muUs / reps << " us; ranks + two "
            << in.slots.size() << "-key stateless pages in place " << pagesUs / reps << " us; for scale, one live "
            << "Allocate of the same input " << liveUs / reps << " us (means of " << reps << ")");
    CHECK(sink > 0);
}
