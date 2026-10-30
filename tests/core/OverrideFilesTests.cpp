// Host tests for R11: src/core/OverrideFiles.h -- the effect override layer as
// a directory (mods' files alphabetically, the player's file last; a bad file
// skipped whole; entries for plugins not loaded skipped quietly).

#include "core/OverrideFiles.h"

#include <doctest/doctest.h>

#include <cctype>
#include <ostream>
#include <set>
#include <string>
#include <vector>

using namespace Huginn::Core::Effect;

namespace
{
    OverrideFile File(std::string name, std::vector<std::pair<std::string, std::string>> lines, bool player = false,
                      int version = 0, bool ok = true)
    {
        OverrideFile f;
        f.name = std::move(name);
        f.lines = std::move(lines);
        f.player = player;
        f.version = version;
        f.readOk = ok;
        return f;
    }

    const auto kLoaded = [](std::string_view p) {
        static const std::set<std::string> loaded{ "skyrim.esm", "mymod.esp", "other.esp", "light.esl" };
        std::string l(p);
        for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return loaded.count(l) > 0;
    };
}

TEST_CASE("effect override files: one line")
{
    std::string why;
    auto p = ParseOverrideLine("Skyrim.esm|0x3EB15", "restore_health", &why);
    REQUIRE(p.has_value());
    CHECK(p->plugin == "Skyrim.esm");
    CHECK(p->localId == 0x3EB15u);
    CHECK(p->col == Col::restore_health);

    // The load-order byte is dropped; an ESL's FE xxx keeps its 12-bit id.
    CHECK(ParseOverrideLine("Skyrim.esm|00012FCC", "damage")->localId == 0x12FCCu);
    CHECK(ParseOverrideLine("light.esl|FE001800", "damage")->localId == 0x800u);
    CHECK(ParseOverrideLine("  MyMod.esp | 800 ", " resist_frost ")->col == Col::resist_frost);

    CHECK_FALSE(ParseOverrideLine("Skyrim.esm 0x3EB15", "restore_health", &why).has_value());
    CHECK(why.find("plugin") != std::string::npos);
    CHECK_FALSE(ParseOverrideLine("Skyrim.esm|0xZZ", "restore_health", &why).has_value());
    CHECK(why == "bad FormID");
    CHECK_FALSE(ParseOverrideLine("|0x800", "restore_health", &why).has_value());
    CHECK_FALSE(ParseOverrideLine("Skyrim.esm|0x800", "RestoreHealth", &why).has_value());   // a classifier tag is not a column
    CHECK(why.find("column") != std::string::npos);
    CHECK_FALSE(ParseOverrideLine("Skyrim.esm|", "restore_health").has_value());
}

TEST_CASE("effect override files: order -- mods alphabetically, the player last")
{
    std::vector<OverrideFile> files{
        File("Huginn_EffectOverrides.ini", {}, true),
        File("zeta.ini", {}),
        File("Alpha.ini", {}),
        File("beta.ini", {}),
    };
    const auto order = OverrideOrder(files);
    REQUIRE(order.size() == 4);
    CHECK(files[order[0]].name == "Alpha.ini");
    CHECK(files[order[1]].name == "beta.ini");
    CHECK(files[order[2]].name == "zeta.ini");
    CHECK(files[order[3]].name == "Huginn_EffectOverrides.ini");
}

TEST_CASE("effect override files: a later file wins, and the player wins over every mod")
{
    std::vector<OverrideFile> files{
        File("Huginn_EffectOverrides.ini", { { "MyMod.esp|0x800", "resist_shock" } }, true),
        File("b_second.ini", { { "MyMod.esp|0x800", "resist_frost" }, { "Other.esp|0x900", "cure_poison" } }),
        File("a_first.ini", { { "MyMod.esp|0x800", "resist_fire" }, { "Skyrim.esm|0x3EB15", "restore_health" } }),
    };
    const auto m = MergeOverrides(files, kLoaded);
    CHECK(m.table.Find("MyMod.esp", 0x05000800u) == Col::resist_shock);   // the player's
    CHECK(m.table.Find("mymod.esp", 0x00000800u) == Col::resist_shock);   // plugin case-insensitive
    CHECK(m.table.Find("Other.esp", 0x900u) == Col::cure_poison);
    CHECK(m.table.Find("Skyrim.esm", 0x0003EB15u) == Col::restore_health);
    CHECK(m.entries == 3);
    REQUIRE(m.files.size() == 3);
    CHECK(m.files[0].name == "a_first.ini");
    CHECK(m.files[0].replaced == 0);
    CHECK(m.files[1].replaced == 1);   // b_second replaced a_first's MyMod 0x800
    CHECK(m.files[2].replaced == 1);   // the player's replaced b_second's
    CHECK(m.files[2].loaded == 1);
}

TEST_CASE("effect override files: one bad file does not break the rest")
{
    std::vector<OverrideFile> files{
        File("broken.ini", { { "MyMod.esp|0x800", "resist_fire" } }, false, 0, /*ok=*/false),
        File("future.ini", { { "MyMod.esp|0x801", "resist_fire" } }, false, kOverrideFormatVersion + 1),
        File("good.ini", { { "MyMod.esp|0x802", "resist_fire" }, { "bad line", "x" }, { "MyMod.esp|0x803", "nope" } }),
        File("v1.ini", { { "MyMod.esp|0x804", "resist_fire" } }, false, 1),
    };
    const auto m = MergeOverrides(files, kLoaded);
    CHECK_FALSE(m.table.Find("MyMod.esp", 0x800u).has_value());
    CHECK_FALSE(m.table.Find("MyMod.esp", 0x801u).has_value());
    CHECK(m.table.Find("MyMod.esp", 0x802u) == Col::resist_fire);
    CHECK(m.table.Find("MyMod.esp", 0x804u) == Col::resist_fire);
    CHECK(m.entries == 2);
    CHECK(m.files[0].skipped);
    CHECK(m.files[0].why == "does not parse");
    CHECK(m.files[1].skipped);
    CHECK(m.files[1].why.find("newer") != std::string::npos);
    CHECK_FALSE(m.files[2].skipped);
    CHECK(m.files[2].loaded == 1);
    CHECK(m.files[2].ignored == 2);
    CHECK_FALSE(m.files[3].skipped);
}

TEST_CASE("effect override files: a plugin not in the load order is skipped quietly")
{
    std::vector<OverrideFile> files{
        File("mod.ini", { { "NotInstalled.esp|0x800", "resist_fire" }, { "MyMod.esp|0x800", "resist_frost" } }),
    };
    const auto m = MergeOverrides(files, kLoaded);
    CHECK_FALSE(m.table.Find("NotInstalled.esp", 0x800u).has_value());
    CHECK(m.table.Find("MyMod.esp", 0x800u) == Col::resist_frost);
    CHECK(m.files[0].notLoaded == 1);
    CHECK(m.files[0].ignored == 0);   // not an error: counted apart
    CHECK(m.files[0].loaded == 1);

    // With no plugin check (the host) every well-formed entry loads.
    const auto all = MergeOverrides(files, {});
    CHECK(all.entries == 2);
}
