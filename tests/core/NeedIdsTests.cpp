// core/NeedIds.h is generated from docs/architecture/9-data/needs.csv by
// tools/needs/make_need_ids.py: this test fails when the two drift apart (an
// id renamed or moved, a curve default or priority changed in one and not the
// other, a need's deferred flag out of step with its r3_input).

#include "DumpCsv.h"
#include "core/NeedEvaluator.h"
#include "core/NeedIds.h"

#include <doctest/doctest.h>

#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <string>

using namespace Huginn::Core::Needs;

namespace
{
    float ParseFloat(const std::string& s)
    {
        float v = 0.0f;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        REQUIRE_MESSAGE((r.ec == std::errc{} && r.ptr == s.data() + s.size()), "bad float '" << s << "'");
        return v;
    }
}

TEST_CASE("need ids match needs.csv, in order, with their default curves")
{
    std::ifstream in(std::string(HUGINN_REPO_ROOT) + "/docs/architecture/9-data/needs.csv", std::ios::binary);
    REQUIRE(in.good());
    std::string line;
    REQUIRE(std::getline(in, line));
    const auto header = Huginn::Test::SplitCsv(line);
    std::map<std::string, std::size_t> col;
    for (std::size_t i = 0; i < header.size(); ++i) col[header[i]] = i;
    for (const char* name : { "id", "group", "priority", "curve_kind", "curve_p1", "curve_p2", "r3_input" }) {
        REQUIRE_MESSAGE(col.contains(name), "needs.csv has no column " << name);
    }

    std::size_t i = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        const auto f = Huginn::Test::SplitCsv(line);
        REQUIRE(f.size() == header.size());
        REQUIRE(i < kNeedCount);
        const auto& info = kNeeds[i];
        INFO("row " << i << " id " << f[col["id"]]);
        CHECK(info.id == f[col["id"]]);
        CHECK(info.group == f[col["group"]]);
        CHECK(std::string("P") + std::to_string(info.priority) == f[col["priority"]]);
        const auto kind = KindFromName(f[col["curve_kind"]]);
        REQUIRE(kind.has_value());
        CHECK(info.curve.kind == *kind);
        CHECK(info.curve.p1 == ParseFloat(f[col["curve_p1"]]));
        CHECK(info.curve.p2 == ParseFloat(f[col["curve_p2"]]));
        CHECK(info.deferred == f[col["r3_input"]].starts_with("0 (deferred"));
        CHECK(CurveProblem(info.curve).empty());
        CHECK(FromName(info.id) == static_cast<NeedId>(i));
        ++i;
    }
    CHECK(i == kNeedCount);
}

TEST_CASE("need ids: count, uniqueness, the deferred five")
{
    CHECK(kNeedCount == 93);  // 92 until 0.23.19 added deep_water_ahead
    std::set<std::string_view> ids;
    std::set<std::string_view> deferred;
    for (const auto& n : kNeeds) {
        ids.insert(n.id);
        if (n.deferred) deferred.insert(n.id);
    }
    CHECK(ids.size() == kNeedCount);
    CHECK(deferred == std::set<std::string_view>{ "healing_blocked", "thirst", "target_magicka_low",
                                                  "target_stamina_low", "sneak_detected" });
    CHECK(Name(NeedId::drop_ahead) == "drop_ahead");
    CHECK(Name(NeedId::deep_water_ahead) == "deep_water_ahead");
    CHECK(Index(NeedId::deep_water_ahead) == Index(NeedId::drop_ahead) + 1);
    CHECK(Info(NeedId::deep_water_ahead).group == "Environment");
    CHECK_FALSE(Info(NeedId::deep_water_ahead).deferred);
    CHECK(FromName("no_such_need") == std::nullopt);
}

TEST_CASE("every need has an input: none falls through NeedInputs' switch")
{
    const auto in = NeedInputs(NeedSnapshot{});
    for (std::size_t i = 0; i < kNeedCount; ++i) {
        INFO(kNeeds[i].id);
        CHECK(!std::isnan(in[i]));
    }
}
