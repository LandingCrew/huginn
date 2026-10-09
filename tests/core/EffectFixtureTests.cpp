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

// 0.23.16, from the LoreRim R3 session: the food the session surfaced, by
// FormID in the LoreRim fixture (hunger size, cooked riders, the "Fortify
// Magicka" food that is not a restore, raw food's Weak Stomach a side effect).
TEST_CASE("effect mapper: LoreRim food -- raw vs roasted, hunger size, Fortify Magicka food, side effects")
{
    Huginn::Test::Dump dump;
    Huginn::Test::DumpReader reader;
    reader.extraColumns = { "expectColumn", "expectNote", "expectInScope", "expectEffects", "expectValues" };
    std::string err;
    REQUIRE_MESSAGE(reader.Load(std::string(HUGINN_REPO_ROOT) + "/tests/core/fixtures/effects_lorerim.csv", dump, &err), err);
    const BuildResult r = BuildCaps(dump.items, dump.effects, nullptr);
    auto cap = [&](std::uint32_t formId) -> const Cap& {
        for (std::size_t i = 0; i < dump.items.size(); ++i) {
            if (dump.items[i].formId == formId && dump.items[i].kind == Kind::Food) return r.caps[i];
        }
        FAIL("food " << formId << " not in the LoreRim fixture");
        static const Cap none;
        return none;
    };

    const Cap& raw = cap(0x000669A4);      // Raw Mammoth Snout
    const Cap& roasted = cap(0x000722BB);  // Roasted Mammoth Snout
    CHECK(Get(raw, Col::survival_hunger) == doctest::Approx(0.5));       // Restore Hunger Small
    CHECK(Get(roasted, Col::survival_hunger) == doctest::Approx(0.75));  // Restore Hunger Medium
    CHECK(Get(raw, Col::self_harm_stamina) > 0.0f);                      // Weak Stomach
    CHECK(Get(raw, Col::damage_stamina) == 0.0f);
    CHECK(Get(raw, Col::damage) == 0.0f);
    CHECK(Get(raw, Col::fortify_vital_health) == 0.0f);
    CHECK(Get(raw, Col::regen_health) == 0.0f);
    CHECK(Get(roasted, Col::fortify_vital_health) > 0.0f);  // the cooked riders
    CHECK(Get(roasted, Col::regen_health) > 0.0f);
    CHECK(Get(roasted, Col::self_harm) == 0.0f);
    CHECK(Get(roasted, Col::restore_stamina) > Get(raw, Col::restore_stamina));  // Nutrition 40 vs 20

    // REQ_PVM_Food_FortifyMagicka (Recover=1): a fortify, not a restore.
    for (const std::uint32_t id : { 0x00064B43u /* Apple Pie */, 0x00064B38u /* Honey Nut Treat */ }) {
        INFO("food " << id);
        const Cap& c = cap(id);
        CHECK(Get(c, Col::fortify_vital_magicka) > 0.0f);
        CHECK(Get(c, Col::restore_magicka) == 0.0f);
        CHECK(Get(c, Col::self_harm) == 0.0f);
    }
    CHECK(Get(cap(0x00064B43), Col::survival_hunger) == doctest::Approx(0.75));
    CHECK(Get(cap(0x00064B38), Col::survival_hunger) == doctest::Approx(0.5));
    CHECK(Get(cap(0x00064B43), Col::fortify_vital_magicka) > Get(cap(0x00064B38), Col::fortify_vital_magicka));

    const Cap& strange = cap(0x87284894);  // Strange Meat: the session's damage_stamina=0.6
    CHECK(Get(strange, Col::self_harm_stamina) > 0.0f);
    CHECK(Get(strange, Col::damage_stamina) == 0.0f);
    CHECK(Get(strange, Col::damage) == 0.0f);
}
