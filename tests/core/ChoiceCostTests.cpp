// R8.2 cost, measured on the host (the in-game number is the Tracy zone
// Pipeline::ScoreShadow; budget 1 ms per pipeline run at ~500 rows): the
// shipped bootstrap's parse and mapping, and one shadow pass over 500 rows
// (the tick, a(i) from cap and cross-features, the score with sigma and the
// explanation) and one update over a 500-row choice set, with the shipped
// model. Reported, not asserted (Debug builds are several times slower); the
// numbers in the PR come from the Release build of this test.

#include "core/Bootstrap.h"
#include "core/ChoiceScore.h"
#include "core/ChoiceUpdate.h"

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace Huginn::Core;
namespace C = Huginn::Core::Choice;

TEST_CASE("choice cost: bootstrap parse and one shadow pass over 500 rows (reported)")
{
    using Clock = std::chrono::steady_clock;
    std::ifstream in(std::string(HUGINN_REPO_ROOT) + "/configs/Huginn_Bootstrap.json", std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const auto t0 = Clock::now();
    const auto read = Bootstrap::Read(text);
    const auto t1 = Clock::now();
    REQUIRE(read.Ok());
    auto mapped = Bootstrap::MapToBuild(*read.doc);
    const auto t2 = Clock::now();
    C::Model& m = mapped.model;

    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    constexpr std::size_t kRows = 500;
    std::vector<std::vector<C::SparseCol>> caps(kRows), cross(kRows);
    for (std::size_t i = 0; i < kRows; ++i) {
        const int n = 3 + static_cast<int>(rng() % 6);
        for (int c = 0; c < n; ++c) caps[i].emplace_back(static_cast<std::uint16_t>(rng() % C::J), u(rng));
        if (rng() % 3 == 0) cross[i].emplace_back(static_cast<std::uint16_t>(Effect::Col::stack_count), u(rng));
    }
    std::array<double, C::K> n{};
    for (int k = 0; k < 8; ++k) n[rng() % C::K] = u(rng);
    std::vector<double> r(m.recent.size(), 0.0);
    for (std::size_t f = 0; f < r.size(); f += 7) r[f] = u(rng);

    const int reps = 20;
    double passUs = 0.0;
    double sink = 0.0;
    for (int rep = 0; rep < reps; ++rep) {
        const auto a = Clock::now();
        const C::Tick tick = C::MakeTick(m, n, r);
        for (std::size_t i = 0; i < kRows; ++i) {
            const C::Attr at = C::MakeAttr(m.grading, caps[i], cross[i]);
            C::RowExtra x;
            x.bMu = 0.1;
            x.bVar = 0.25;
            x.offPage = (i % 3) != 0;
            x.feat[static_cast<std::size_t>(C::Feature::ln_learn_mult)] = 0.3;
            const auto s = C::Score(m, tick, at, x);
            sink += s.mu + s.var + s.exNeed;
        }
        passUs += std::chrono::duration<double, std::micro>(Clock::now() - a).count();
    }
    // one update over a 500-row choice set
    std::vector<C::Member> set(kRows);
    for (std::size_t i = 0; i < kRows; ++i) {
        set[i].attr = C::MakeAttr(m.grading, caps[i], cross[i]);
        set[i].form = static_cast<std::uint32_t>(i);
        set[i].offPage = (i % 3) != 0;
    }
    const auto ua = Clock::now();
    const C::Tick tick = C::MakeTick(m, n, r);
    const auto res = C::Update(m, tick, set, 7);
    const double updateUs = std::chrono::duration<double, std::micro>(Clock::now() - ua).count();
    MESSAGE("bootstrap: parse " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms, map "
                                << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms ("
                                << text.size() / 1024 << " KiB); shadow pass over " << kRows << " rows: "
                                << passUs / reps << " us (mean of " << reps << "); one update over " << kRows
                                << " rows: " << updateUs << " us (" << res.weightsMoved << " weights)");
    CHECK(sink == sink);   // keep the work
    CHECK(res.P.size() == kRows);
}
