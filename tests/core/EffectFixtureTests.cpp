// The effect mapper against rows taken from three `hg dump all` CSVs (R2's
// done-criterion): vanilla+ (a 0.23.10 dump from the game, with payload rows
// and schools), Simonrim Essentials (0.23.6 dump) and LoreRim (0.23.7 dump).
//
// tests/core/fixtures/effects_<list>.csv keep the dump columns the mapper
// reads (no playerCount) plus expectations made by the Python reference
// extractor the doc 9 coverage was measured with, adapted to the effects.csv
// names and to the deviations listed in core/EffectRules.h (each such row says
// so in expectNote). The C++ was not used to write them.
//   expectColumn   per effect row: the column the MGEF maps to ("" = none)
//   expectInScope  per item (first row): in the catalog's scope
//   expectEffects  per item (first row): every family/specific column cap(i) sets
//   expectValues   per item (first row): those columns' values, graded by the
//                  generator over the fixture's own in-scope items

#include "DumpCsv.h"
#include "core/EffectMapper.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>
#include <set>
#include <string>

using namespace Huginn::Core::Effect;

namespace
{
    struct FixtureResult
    {
        int items = 0, inScope = 0, rows = 0, rowMismatch = 0, itemMismatch = 0, scopeMismatch = 0, values = 0, valueMismatch = 0;
    };

    FixtureResult RunFixture(const std::string& list)
    {
        Huginn::Test::Dump dump;
        Huginn::Test::DumpReader reader;
        reader.extraColumns = { "expectColumn", "expectNote", "expectInScope", "expectEffects", "expectValues" };
        std::string err;
        const std::string path = std::string(HUGINN_REPO_ROOT) + "/tests/core/fixtures/effects_" + list + ".csv";
        REQUIRE_MESSAGE(reader.Load(path, dump, &err), err);
        REQUIRE(dump.items.size() > 100);

        const BuildResult r = BuildCaps(dump.items, dump.effects, nullptr);
        FixtureResult fr;
        for (std::size_t i = 0; i < dump.items.size(); ++i) {
            const auto& it = dump.items[i];
            const auto& m = r.mappings[i];
            ++fr.items;
            INFO(list << " item " << KindName(it.kind) << " " << it.formId << " '" << it.name << "'");

            // Per row: the column the mapper gives the effect.
            for (std::size_t j = 0; j < it.effects.size(); ++j) {
                const auto& exp = dump.rowExtra[i][j];
                const auto& o = m.outcomes[j];
                const std::string got = o.cls.Mapped() ? std::string(Name(o.cls.col)) : std::string{};
                ++fr.rows;
                if (got != exp[0]) ++fr.rowMismatch;
                CHECK_MESSAGE(got == exp[0], "row " << j << " '" << dump.effects[it.effects[j].effect].name << "' got '"
                                                    << got << "' want '" << exp[0] << "' (" << exp[1] << ")");
            }

            const bool wantScope = dump.itemExtra[i][2] == "1";
            if (m.inScope != wantScope) ++fr.scopeMismatch;
            CHECK(m.inScope == wantScope);
            if (!m.inScope) continue;
            ++fr.inScope;

            std::set<std::string> want;
            for (const auto& c : Huginn::Test::SplitList(dump.itemExtra[i][3])) want.insert(c);
            std::set<std::string> got;
            for (const auto& v : r.caps[i]) {
                if (IsEffect(v.col)) got.insert(std::string(Name(v.col)));
            }
            if (got != want) ++fr.itemMismatch;
            std::string g, w;
            for (const auto& c : got) g += c + ";";
            for (const auto& c : want) w += c + ";";
            CHECK_MESSAGE(got == want, "effect columns got [" << g << "] want [" << w << "]");

            // The values: the generator grades the fixture's own population by
            // a second implementation of the documented rules.
            for (const auto& kv : Huginn::Test::SplitList(dump.itemExtra[i][4])) {
                const auto eq = kv.find('=');
                REQUIRE(eq != std::string::npos);
                const auto col = FromName(kv.substr(0, eq));
                REQUIRE(col.has_value());
                const float want = std::stof(kv.substr(eq + 1));
                const float have = Get(r.caps[i], *col);
                ++fr.values;
                const bool same = std::fabs(have - want) <= 1e-4f + 1e-4f * std::fabs(want);
                if (!same) ++fr.valueMismatch;
                CHECK_MESSAGE(same, kv.substr(0, eq) << " got " << have << " want " << want);
            }

            // Every in-scope item carries exactly one kind column, and every
            // value is in a sane range.
            int kinds = 0;
            for (const auto& v : r.caps[i]) {
                if (Name(v.col).substr(0, 5) == "kind_") ++kinds;
                CHECK(v.v > 0.0f);
                CHECK(v.v <= 2.0f);  // weapon_speed / weapon_reach ratios may exceed 1
            }
            CHECK(kinds == 1);
        }
        MESSAGE(list << ": " << fr.items << " items (" << fr.inScope << " in scope), " << fr.rows
                     << " effect rows, " << fr.values << " effect values; mismatches: rows " << fr.rowMismatch
                     << ", items " << fr.itemMismatch << ", scope " << fr.scopeMismatch << ", values " << fr.valueMismatch);
        return fr;
    }
}

TEST_CASE("effect mapper: rows from the vanilla+ dump")
{
    RunFixture("vanilla");
}

TEST_CASE("effect mapper: rows from the Simonrim Essentials dump")
{
    RunFixture("simonrim");
}

TEST_CASE("effect mapper: rows from the LoreRim dump")
{
    RunFixture("lorerim");
}
