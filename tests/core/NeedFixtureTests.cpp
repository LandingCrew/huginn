// Replayed state snapshots give the expected need vector.
//
// tests/core/fixtures/needs/<name>.txt holds NeedSnapshot records (the text
// format of core/NeedSnapshotIO.h): synthetic.txt was designed by hand to
// make every need with a sensor fire, captured_*.txt were recorded in game by
// the Debug capture (needs/NeedCapture.cpp). Each has <name>.expected.csv
// (label,need,input,value), written by tools/needs/expected_vectors.py -- a
// separate implementation written from needs.csv (curve_kind, curve_p1,
// curve_p2, r3_input), NeedSnapshot.h and the curve formulas alone, by an
// agent that was not shown core/NeedEvaluator.* or core/ResponseCurve.h. So
// a match is two readings of the csv agreeing, not the code agreeing with
// itself. Regenerate an expectation only by running that script.

#include "DumpCsv.h"
#include "core/NeedEvaluator.h"
#include "core/NeedSnapshotIO.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

using namespace Huginn::Core::Needs;

namespace
{
    std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool Close(double got, double want, double absTol, double relTol)
    {
        if (std::isnan(want)) return std::isnan(got);
        return std::abs(got - want) <= absTol + relTol * std::abs(want);
    }
}

TEST_CASE("need fixtures: every replayed snapshot gives the oracle's vector")
{
    const std::filesystem::path dir = std::filesystem::path(HUGINN_REPO_ROOT) / "tests/core/fixtures/needs";
    REQUIRE(std::filesystem::is_directory(dir));
    const auto curves = DefaultCurves();
    int files = 0, snapshots = 0, compared = 0;
    bool sawSynthetic = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto& path = entry.path();
        if (path.extension() != ".txt") continue;
        const auto expectedPath = path.parent_path() / (path.stem().string() + ".expected.csv");
        INFO("fixture " << path.filename().string());
        REQUIRE_MESSAGE(std::filesystem::exists(expectedPath), "no " << expectedPath.filename().string());
        ++files;
        if (path.stem() == "synthetic") sawSynthetic = true;

        std::string err;
        const auto recs = ReadSnapshots(Slurp(path), err);
        REQUIRE_MESSAGE(err.empty(), err);
        REQUIRE(!recs.empty());

        // label -> need -> (input, value)
        std::map<std::string, std::map<std::string, std::pair<double, double>>> expected;
        std::istringstream csv(Slurp(expectedPath));
        std::string line;
        REQUIRE(std::getline(csv, line));
        CHECK(Huginn::Test::SplitCsv(line) == std::vector<std::string>{ "label", "need", "input", "value" });
        while (std::getline(csv, line)) {
            if (line.empty() || line == "\r") continue;
            const auto f = Huginn::Test::SplitCsv(line);
            REQUIRE(f.size() == 4);
            expected[f[0]][f[1]] = { std::stod(f[2]), std::stod(f[3]) };
        }
        CHECK(expected.size() == recs.size());

        for (const auto& rec : recs) {
            ++snapshots;
            INFO("snapshot " << rec.label);
            const auto it = expected.find(rec.label);
            REQUIRE_MESSAGE(it != expected.end(), "no expectation for " << rec.label);
            CHECK(it->second.size() == kNeedCount);
            const auto v = EvaluateNeeds(rec.snapshot, curves);
            for (std::size_t i = 0; i < kNeedCount; ++i) {
                const auto e = it->second.find(std::string(kNeeds[i].id));
                REQUIRE_MESSAGE(e != it->second.end(), "no row for " << kNeeds[i].id);
                const auto [wantIn, wantVal] = e->second;
                INFO("need " << kNeeds[i].id << " input " << v.input[i] << " (want " << wantIn << ") value "
                             << v.value[i] << " (want " << wantVal << ")");
                // The game computes in float, the oracle in double from the
                // same float inputs: agree to float precision.
                CHECK(Close(v.input[i], wantIn, 1e-5, 1e-6));
                CHECK(Close(v.value[i], wantVal, 2e-6, 1e-5));
                ++compared;
            }
        }
    }
    MESSAGE("need fixtures: " << files << " file(s), " << snapshots << " snapshot(s), " << compared
                              << " need values compared");
    CHECK(sawSynthetic);
    CHECK(files >= 1);
}
