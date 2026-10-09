// The obvious need x effect pairs (doc 9, "Pairs"): the pairs that start
// nonzero. They are written down twice, by hand: effects.csv's `obvious_needs`
// (need ids, one list per column: the table counted in doc 9) and needs.csv's
// `obvious_effects` (prose, one list per need). No code reads either yet, so
// this test is what keeps a pair the user ruled out from coming back.
//
// Pinned here:
//   * cold (the Survival cold meter) is answered by soups -- warm food, their
//     Restore Cold effect -- and by nothing else: not Resist Frost (it lowers
//     frost damage only), not warming spells or warm apparel (they raise the
//     warmth rating, warmth_deficit). Confirmed via LoreRim Discord, the user,
//     2026-10-09 (0.23.17).
//   * warmth_deficit (the warmth rating) is answered by warm apparel, warming
//     spells and warm food -- soups do both: Restore Cold for cold, Fortify
//     Warmth (01002EE6) for warmth_deficit -- and never by Resist Frost.
//   * diseased x resist_disease is not a pair (removed 0.23.16): resisting a
//     disease does not cure one already caught.
//   * deep_water_ahead (0.23.19) is answered by Waterbreathing
//     (utility_water_breathing), as underwater and swimming are.
//   * every need id in effects.csv is a need of needs.csv, and the count doc 9
//     quotes.
//
// NOT separable in the columns today: a soup's Restore Cold (Update.esm
// 01002EE5) and the Fortify Warmth of warming spells (01002EE6, Variable09)
// both map to `survival_warmth` (EffectRules.cpp: the keyword table and the
// name table), so cold x survival_warmth also reaches a warming spell. The
// prose list and armour_warm are where "not warming spells" can be pinned
// until that column is split (roadmap "Needs and effects to add").

#include "DumpCsv.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace
{
    using Row = std::map<std::string, std::string>;

    /// id -> row (column name -> field) of a docs/architecture/9-data csv.
    std::map<std::string, Row> ReadTable(const std::string& name)
    {
        std::ifstream in(std::string(HUGINN_REPO_ROOT) + "/docs/architecture/9-data/" + name, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "cannot open " << name);
        auto strip = [](std::string& s) {
            if (!s.empty() && s.back() == '\r') s.pop_back();  // a CRLF checkout
        };
        std::string line;
        REQUIRE(std::getline(in, line));
        strip(line);
        const auto header = Huginn::Test::SplitCsv(line);
        std::map<std::string, Row> out;
        while (std::getline(in, line)) {
            strip(line);
            if (line.empty()) continue;
            const auto f = Huginn::Test::SplitCsv(line);
            REQUIRE_MESSAGE(f.size() == header.size(), name << ": row '" << line.substr(0, 40) << "'");
            Row r;
            for (std::size_t i = 0; i < header.size(); ++i) r[header[i]] = f[i];
            out[f[0]] = std::move(r);
        }
        return out;
    }

    /// "a; b; c" -> {a, b, c}
    std::set<std::string> SplitIds(const std::string& list)
    {
        std::set<std::string> out;
        std::size_t start = 0;
        while (start <= list.size()) {
            const auto end = std::min(list.find(';', start), list.size());
            std::string id = list.substr(start, end - start);
            id.erase(0, id.find_first_not_of(' '));
            id.erase(id.find_last_not_of(' ') + 1);
            if (!id.empty()) out.insert(id);
            start = end + 1;
        }
        return out;
    }

    bool ContainsNoCase(std::string text, std::string what)
    {
        auto lower = [](std::string& s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        };
        lower(text);
        lower(what);
        return text.find(what) != std::string::npos;
    }

    /// The effect columns whose `obvious_needs` lists `need`.
    std::set<std::string> ColumnsFor(const std::map<std::string, Row>& effects, const std::string& need)
    {
        std::set<std::string> out;
        for (const auto& [id, row] : effects) {
            if (SplitIds(row.at("obvious_needs")).contains(need)) out.insert(id);
        }
        return out;
    }
}

TEST_CASE("obvious pairs: cold is answered by soups, never by Resist Frost or warming spells")
{
    const auto effects = ReadTable("effects.csv");
    const auto needs = ReadTable("needs.csv");
    REQUIRE(effects.contains("resist_frost"));
    REQUIRE(needs.contains("cold"));

    // effects.csv, the pair table. survival_warmth is the column a soup's
    // Restore Cold maps to; it is the only answer. Not resist_frost, not
    // armour_warm (warm apparel raises the warmth rating).
    CHECK(ColumnsFor(effects, "cold") == std::set<std::string>{ "survival_warmth" });

    // resist_frost keeps its frost-damage needs and nothing survival.
    const auto frost = SplitIds(effects.at("resist_frost").at("obvious_needs"));
    CHECK(frost == std::set<std::string>{ "frost_damage_rate", "enemy_casting_frost", "target_dragon",
                                          "target_element_frost" });

    // needs.csv, the prose list: soups only.
    const auto& coldAnswers = needs.at("cold").at("obvious_effects");
    CHECK(ContainsNoCase(coldAnswers, "soup"));
    CHECK(ContainsNoCase(coldAnswers, "restore cold"));
    for (const char* not_ : { "resist frost", "warming spell", "warming aura", "apparel", "fortify warmth" }) {
        INFO("cold's answers mention '" << not_ << "'");
        CHECK_FALSE(ContainsNoCase(coldAnswers, not_));
    }
    for (const char* n : { "frost_damage_rate", "enemy_casting_frost" }) {
        INFO(n);
        CHECK(ContainsNoCase(needs.at(n).at("obvious_effects"), "resist frost"));
    }
}

TEST_CASE("obvious pairs: warmth_deficit is answered by warm apparel, warming spells and warm food, never by Resist Frost")
{
    const auto effects = ReadTable("effects.csv");
    const auto needs = ReadTable("needs.csv");
    REQUIRE(needs.contains("warmth_deficit"));

    const auto cols = ColumnsFor(effects, "warmth_deficit");
    CHECK_FALSE(cols.contains("resist_frost"));
    CHECK(cols == std::set<std::string>{ "armour_warm", "survival_warmth" });

    const auto& answers = needs.at("warmth_deficit").at("obvious_effects");
    CHECK_FALSE(ContainsNoCase(answers, "resist frost"));
    CHECK(ContainsNoCase(answers, "apparel"));
    CHECK(ContainsNoCase(answers, "warming spell"));
    CHECK(ContainsNoCase(answers, "warm food"));  // a soup's Fortify Warmth
}

TEST_CASE("obvious pairs: diseased is answered by Cure Disease, never by Resist Disease")
{
    const auto effects = ReadTable("effects.csv");
    const auto needs = ReadTable("needs.csv");
    CHECK_FALSE(SplitIds(effects.at("resist_disease").at("obvious_needs")).contains("diseased"));
    CHECK(SplitIds(effects.at("cure_disease").at("obvious_needs")).contains("diseased"));
    CHECK_FALSE(ContainsNoCase(needs.at("diseased").at("obvious_effects"), "resist disease"));
    CHECK(ContainsNoCase(needs.at("diseased").at("obvious_effects"), "cure disease"));
}

TEST_CASE("obvious pairs: every pair names a need, and doc 9's count")
{
    const auto effects = ReadTable("effects.csv");
    const auto needs = ReadTable("needs.csv");
    std::size_t pairs = 0;
    for (const auto& [id, row] : effects) {
        for (const auto& n : SplitIds(row.at("obvious_needs"))) {
            INFO(id << " x " << n);
            CHECK(needs.contains(n));
            ++pairs;
        }
    }
    // docs/architecture/9-context-as-learner-input.md, "Pairs": 264 since
    // 0.23.19 added deep_water_ahead x utility_water_breathing (263 since
    // 0.23.17 dropped cold x resist_frost and cold x armour_warm; 265 before;
    // 266 before 0.23.16).
    CHECK(pairs == 264);
}

TEST_CASE("obvious pairs: deep water ahead is answered by Waterbreathing")
{
    const auto effects = ReadTable("effects.csv");
    const auto needs = ReadTable("needs.csv");
    REQUIRE(needs.contains("deep_water_ahead"));
    CHECK(ColumnsFor(effects, "deep_water_ahead") == std::set<std::string>{ "utility_water_breathing" });
    CHECK(ColumnsFor(effects, "underwater").contains("utility_water_breathing"));
    CHECK(ContainsNoCase(needs.at("deep_water_ahead").at("obvious_effects"), "waterbreathing"));
}
