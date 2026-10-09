// core/EffectColumns.h is generated from docs/architecture/9-data/effects.csv:
// this test fails when the two drift apart (an id renamed, a column added,
// the order changed, a family moved).

#include "DumpCsv.h"
#include "core/EffectColumns.h"
#include "core/EffectRules.h"

#include <doctest/doctest.h>

#include <fstream>
#include <ostream>
#include <set>
#include <string>

using namespace Huginn::Core::Effect;

TEST_CASE("effect columns match effects.csv, in order")
{
    std::ifstream in(std::string(HUGINN_REPO_ROOT) + "/docs/architecture/9-data/effects.csv", std::ios::binary);
    REQUIRE(in.good());
    std::string line;
    REQUIRE(std::getline(in, line));
    const auto header = Huginn::Test::SplitCsv(line);
    REQUIRE(header.size() >= 3);
    CHECK(header[0] == "id");
    CHECK(header[1] == "level");
    CHECK(header[2] == "family");

    std::size_t i = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto f = Huginn::Test::SplitCsv(line);
        REQUIRE(i < kColumnCount);
        const auto& info = kColumns[i];
        INFO("row " << i << " id " << f[0]);
        CHECK(info.id == f[0]);
        const std::string level = f[1];
        const Level expect = level == "family"      ? Level::Family
                             : level == "specific"  ? Level::Specific
                             : level == "modifier"  ? Level::Modifier
                             : level == "item_feature" ? Level::ItemFeature
                             : level == "weapon_stat" ? Level::WeaponStat
                                                      : Level::ArmourStat;
        CHECK(info.level == expect);
        if (level == "specific") {
            const auto fam = FromName(f[2]);
            if (fam && LevelOf(*fam) == Level::Family) {
                CHECK(info.family == *fam);
            }
            else {
                CHECK(info.family == Col::_Count);  // drain_skill, soul_trap
            }
        }
        else {
            CHECK(info.family == Col::_Count);
        }
        ++i;
    }
    CHECK(i == kColumnCount);
}

TEST_CASE("effect columns: counts per level and lookups")
{
    int fam = 0, spec = 0, mod = 0, item = 0, weap = 0, arm = 0;
    std::set<std::string_view> ids;
    for (const auto& c : kColumns) {
        ids.insert(c.id);
        switch (c.level) {
            case Level::Family: ++fam; break;
            case Level::Specific: ++spec; break;
            case Level::Modifier: ++mod; break;
            case Level::ItemFeature: ++item; break;
            case Level::WeaponStat: ++weap; break;
            case Level::ArmourStat: ++arm; break;
        }
    }
    CHECK(fam == 26);
    CHECK(spec == 136);
    CHECK(mod == 19);
    CHECK(item == 25);
    CHECK(weap == 23);
    CHECK(arm == 14);
    CHECK(ids.size() == kColumnCount);  // ids are unique

    CHECK(FromName("restore_health") == Col::restore_health);
    CHECK(FromName("nope") == std::nullopt);
    CHECK(Name(Col::armour_cold) == "armour_cold");
    CHECK(FamilyOf(Col::damage_health_fire) == Col::damage);
    CHECK(FamilyOf(Col::drain_skill) == Col::_Count);
    CHECK(FamilyOf(Col::self_harm_stamina) == Col::self_harm);
    CHECK(FamilyKey(Col::drain_skill) == Col::drain_skill);
    CHECK(IsEffect(Col::resist) == true);
    CHECK(IsEffect(Col::kind_potion) == false);
}

TEST_CASE("effect rules: every table pattern compiles and every spec has a column")
{
    CHECK(CheckRuleTables() == "");
}
