// Host tests for the shipped defaults (0.24.10): the one source of every
// setting's default, and what dMenu's Reset to Defaults restores.
//
//   - the compiled-in files (cmake/EmbedShipped.cmake) are the repo's
//     configs/Huginn.ini and dMenu descriptor, byte for byte;
//   - settings/IniOverlay.h, the layering every settings INI is read through:
//     a key the player's file leaves out reads the shipped value, on the real
//     shipped Huginn.ini;
//   - core/DMenuDefaults.h: the descriptor's defaults as dMenu writes them;
//   - a source guard: no settings read bypasses the overlay, so a loader
//     constant can never again be the default of a key the shipped file sets.

#include "core/DMenuDefaults.h"
#include "settings/IniOverlay.h"
#include "huginn/ShippedFiles.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <ostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifndef HUGINN_REPO_ROOT
#error "HUGINN_REPO_ROOT must be defined (tests/CMakeLists.txt)"
#endif

namespace
{
    using Huginn::Settings::LoadOverlay;
    namespace DMenu = Huginn::Core::DMenu;

    std::string ReadRepoFile(const std::string& relPath)
    {
        const std::string path = std::string(HUGINN_REPO_ROOT) + "/" + relPath;
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "cannot open " << path);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    std::string Trim(std::string s)
    {
        const auto b = s.find_first_not_of(" \t\r");
        if (b == std::string::npos) return {};
        const auto e = s.find_last_not_of(" \t\r");
        return s.substr(b, e - b + 1);
    }

    struct IniLine
    {
        std::string section;
        std::string key;
        std::string value;
        std::size_t lineIndex = 0;   // into the file's lines
    };

    // The test's own reading of an INI's key lines, independent of SimpleIni:
    // `[section]` headers, `key = value` lines, `;` comment lines.
    std::vector<IniLine> KeyLines(const std::string& text, std::vector<std::string>* linesOut = nullptr)
    {
        std::vector<std::string> lines;
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
        std::vector<IniLine> out;
        std::string section;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string t = Trim(lines[i]);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            if (t.front() == '[' && t.back() == ']') {
                section = t.substr(1, t.size() - 2);
                continue;
            }
            const auto eq = t.find('=');
            REQUIRE_MESSAGE(eq != std::string::npos, "not a key line: " << t);
            out.push_back({ section, Trim(t.substr(0, eq)), Trim(t.substr(eq + 1)), i });
        }
        if (linesOut) *linesOut = std::move(lines);
        return out;
    }

    std::string Value(const CSimpleIniA& ini, const std::string& section, const std::string& key)
    {
        const char* v = ini.GetValue(section.c_str(), key.c_str(), nullptr);
        return v ? std::string(v) : std::string("<absent>");
    }
}

// =============================================================================
// The compiled-in files
// =============================================================================

TEST_CASE("ShippedFiles: the compiled-in Huginn.ini is configs/Huginn.ini, byte for byte")
{
    const std::string repo = ReadRepoFile("configs/Huginn.ini");
    const std::string_view embedded = Huginn::Shipped::MainIniText();
    CHECK(embedded.size() == repo.size());
    CHECK(embedded == std::string_view(repo));
}

TEST_CASE("ShippedFiles: the compiled-in dMenu descriptor is Huginn.json, byte for byte")
{
    const std::string repo = ReadRepoFile("Data/SKSE/Plugins/dmenu/customSettings/Huginn.json");
    CHECK(Huginn::Shipped::DMenuJsonText() == std::string_view(repo));
}

// =============================================================================
// The overlay
// =============================================================================

TEST_CASE("IniOverlay: the player's key replaces the shipped one; a key it leaves out keeps the shipped value")
{
    CSimpleIniA ini;
    const auto r = LoadOverlay(ini, "[A]\nx = 1\ny = 2\n[B]\nz = 3\n", "[A]\nx = 9\n[C]\nw = 4\n");
    REQUIRE(r.Ok());
    CHECK(Value(ini, "A", "x") == "9");
    CHECK(Value(ini, "A", "y") == "2");
    CHECK(Value(ini, "B", "z") == "3");
    CHECK(Value(ini, "C", "w") == "4");
    // Replaced, not added: one value for x, so no reader can see the shipped 1.
    CSimpleIniA::TNamesDepend values;
    REQUIRE(ini.GetAllValues("A", "x", values));
    CHECK(values.size() == 1);
}

TEST_CASE("IniOverlay: no player file reads the shipped file alone")
{
    CSimpleIniA ini;
    REQUIRE(LoadOverlay(ini, "[A]\nx = 1\n", {}).Ok());
    CHECK(Value(ini, "A", "x") == "1");
}

TEST_CASE("IniOverlay: a UTF-8 byte-order mark on the player's file is skipped")
{
    CSimpleIniA ini;
    REQUIRE(LoadOverlay(ini, "[A]\nx = 1\n", "\xEF\xBB\xBF[A]\nx = 2\n").Ok());
    CHECK(Value(ini, "A", "x") == "2");
}

TEST_CASE("IniOverlay: a second load starts over (the reload path reuses nothing)")
{
    CSimpleIniA ini;
    REQUIRE(LoadOverlay(ini, "[A]\nx = 1\n", "[A]\nx = 5\ny = 6\n").Ok());
    REQUIRE(LoadOverlay(ini, "[A]\nx = 1\n", {}).Ok());
    CHECK(Value(ini, "A", "x") == "1");
    CHECK(Value(ini, "A", "y") == "<absent>");
}

TEST_CASE("IniOverlay: on the shipped Huginn.ini, every key the player's file leaves out reads its shipped value")
{
    const std::string shipped(Huginn::Shipped::MainIniText());
    std::vector<std::string> lines;
    const auto keys = KeyLines(shipped, &lines);
    REQUIRE(keys.size() > 250);   // every section: pages, keys, overrides, ..., the 97 needs

    SUBCASE("an empty player file: the shipped file, key for key")
    {
        CSimpleIniA ini;
        REQUIRE(LoadOverlay(ini, shipped, {}).Ok());
        for (const auto& k : keys) {
            CHECK_MESSAGE(Value(ini, k.section, k.key) == k.value, "[" << k.section << "] " << k.key);
        }
    }

    SUBCASE("a player file missing every second key and changing every third")
    {
        // The player's Huginn.ini as a hand edit leaves it: lines deleted,
        // values changed, sections (and so whole pages) kept or dropped.
        std::vector<bool> drop(lines.size(), false);
        std::map<std::size_t, std::string> changed;
        for (std::size_t n = 0; n < keys.size(); ++n) {
            if (n % 2 == 0) {
                drop[keys[n].lineIndex] = true;
            } else if (n % 3 == 0) {
                changed[keys[n].lineIndex] = keys[n].key + " = PLAYER" + std::to_string(n);
            }
        }
        std::string player;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (drop[i]) continue;
            const auto c = changed.find(i);
            player += (c != changed.end() ? c->second : lines[i]) + "\n";
        }
        CSimpleIniA ini;
        REQUIRE(LoadOverlay(ini, shipped, player).Ok());
        for (std::size_t n = 0; n < keys.size(); ++n) {
            const auto& k = keys[n];
            const std::string want = (n % 2 != 0 && n % 3 == 0) ? "PLAYER" + std::to_string(n) : k.value;
            CHECK_MESSAGE(Value(ini, k.section, k.key) == want, "[" << k.section << "] " << k.key);
        }
    }
}

// =============================================================================
// The dMenu descriptor's defaults
// =============================================================================

TEST_CASE("DMenuDefaults: Huginn.json's defaults, as dMenu writes them on a fresh install")
{
    const auto r = DMenu::DefaultsFromDescriptor(Huginn::Shipped::DMenuJsonText());
    REQUIRE_MESSAGE(r.Ok(), r.error);
    // Every [Widget]/[Debug] key IntuitionSettings and DebugSettings read, at
    // the descriptor's default, in dMenu's serialization (a slider is C's %f
    // of the float, a dropdown its index). A changed default changes this list
    // on purpose.
    const std::vector<std::tuple<std::string, std::string, std::string>> want{
        { "Widget", "bEnabled", "true" },
        { "Widget", "fPositionX", "28.000000" },
        { "Widget", "fPositionY", "83.000000" },
        { "Widget", "fAlpha", "100.000000" },
        { "Widget", "fScale", "70.000000" },
        { "Widget", "fAlphaChild", "70.000000" },
        { "Widget", "bReadOnly", "false" },
        { "Widget", "bHideWhileWheelOpen", "true" },
        { "Widget", "sDisplayMode", "0" },
        { "Widget", "bMinimalCounts", "true" },
        { "Widget", "sSlotEffect", "0" },
        { "Debug", "iRecommendationLog", "1.000000" },
        { "Debug", "bShowStateManager", "false" },
        { "Debug", "bShowRegistry", "false" },
        { "Debug", "bShowUtilityScorer", "false" },
    };
    REQUIRE(r.entries.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        CHECK(r.entries[i].section == std::get<0>(want[i]));
        CHECK(r.entries[i].key == std::get<1>(want[i]));
        CHECK(r.entries[i].value == std::get<2>(want[i]));
    }
}

TEST_CASE("DMenuDefaults: the INI text, and the overlay over it")
{
    const auto r = DMenu::DefaultsFromDescriptor(Huginn::Shipped::DMenuJsonText());
    REQUIRE(r.Ok());
    const std::string text = DMenu::ToIniText(r.entries);
    CHECK(text.starts_with("[Widget]\nbEnabled = true\nfPositionX = 28.000000\n"));
    CHECK(text.find("\n\n[Debug]\niRecommendationLog = 1.000000\n") != std::string::npos);

    // dMenu's own file over it: what it sets wins, the rest is the default.
    CSimpleIniA ini;
    REQUIRE(LoadOverlay(ini, text, "[Widget]\nfScale=85.000000\nsDisplayMode=2\n").Ok());
    CHECK(Value(ini, "Widget", "fScale") == "85.000000");
    CHECK(Value(ini, "Widget", "sDisplayMode") == "2");
    CHECK(Value(ini, "Widget", "fPositionX") == "28.000000");
    CHECK(Value(ini, "Debug", "iRecommendationLog") == "1.000000");
}

TEST_CASE("DMenuDefaults: dMenu's fallbacks for a missing default, and what is refused")
{
    SUBCASE("missing defaults: false, 0, index 0; groups walked, buttons skipped")
    {
        const auto r = DMenu::DefaultsFromDescriptor(R"({"name":"X","data":[
            {"type":"group","entries":[
                {"type":"checkbox","ini":{"section":"S","id":"bA"}},
                {"type":"group","entries":[{"type":"slider","ini":{"section":"S","id":"fB"}}]}]},
            {"type":"button","id":"X_button"},
            {"type":"dropdown","options":["a","b"],"ini":{"section":"T","id":"sC"}},
            {"type":"textbox","default":"hi","ini":{"section":"T","id":"sD"}},
            {"type":"keymap","default":45,"ini":{"section":"T","id":"iE"}}]})");
        REQUIRE_MESSAGE(r.Ok(), r.error);
        REQUIRE(r.entries.size() == 5);
        CHECK(r.entries[0].value == "false");
        CHECK(r.entries[1].value == "0.000000");
        CHECK(r.entries[2].value == "0");
        CHECK(r.entries[3].value == "hi");
        CHECK(r.entries[4].value == "45");
        CHECK(DMenu::ToIniText(r.entries) == "[S]\nbA = false\nfB = 0.000000\n\n[T]\nsC = 0\nsD = hi\niE = 45\n");
    }
    SUBCASE("a slider default is printed as dMenu's float, not the JSON text")
    {
        const auto r = DMenu::DefaultsFromDescriptor(
            R"({"data":[{"type":"slider","default":0.1,"ini":{"section":"S","id":"f"}}]})");
        REQUIRE(r.Ok());
        CHECK(r.entries[0].value == "0.100000");
    }
    SUBCASE("refused: not JSON, no data, an unknown stored type, a wrong default type, no ini id")
    {
        CHECK_FALSE(DMenu::DefaultsFromDescriptor("{").Ok());
        CHECK_FALSE(DMenu::DefaultsFromDescriptor(R"({"name":"X"})").Ok());
        CHECK_FALSE(DMenu::DefaultsFromDescriptor(
            R"({"data":[{"type":"color","ini":{"section":"S","id":"c"}}]})").Ok());
        CHECK_FALSE(DMenu::DefaultsFromDescriptor(
            R"({"data":[{"type":"checkbox","default":1,"ini":{"section":"S","id":"b"}}]})").Ok());
        CHECK_FALSE(DMenu::DefaultsFromDescriptor(
            R"({"data":[{"type":"dropdown","default":1.5,"ini":{"section":"S","id":"d"}}]})").Ok());
        CHECK_FALSE(DMenu::DefaultsFromDescriptor(
            R"({"data":[{"type":"checkbox","ini":{"section":"S"}}]})").Ok());
    }
}

// =============================================================================
// The source guard
// =============================================================================

namespace
{
    // Every code line (a `//` comment cut off) of every .h/.cpp under src/
    // that matches `pattern`, as "path:line".
    std::vector<std::pair<std::string, std::string>> CodeMatches(const std::regex& pattern)
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(HUGINN_REPO_ROOT) / "src";
        std::vector<std::pair<std::string, std::string>> hits;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file()) continue;
            const auto ext = e.path().extension().string();
            if (ext != ".h" && ext != ".cpp" && ext != ".hpp" && ext != ".inl") continue;
            std::ifstream in(e.path(), std::ios::binary);
            const std::string rel = fs::relative(e.path(), fs::path(HUGINN_REPO_ROOT)).generic_string();
            std::string line;
            int n = 0;
            while (std::getline(in, line)) {
                ++n;
                if (const auto c = line.find("//"); c != std::string::npos) line.erase(c);
                if (std::regex_search(line, pattern)) hits.emplace_back(rel, std::to_string(n) + ": " + Trim(line));
            }
        }
        return hits;
    }

    // `allowed`: repo-relative files, or folders (an entry ending in '/').
    void RequireOnlyIn(const std::regex& pattern, const std::set<std::string>& allowed, const std::string& what)
    {
        for (const auto& [file, line] : CodeMatches(pattern)) {
            bool ok = allowed.contains(file);
            for (const auto& a : allowed) {
                ok = ok || (a.ends_with('/') && file.starts_with(a));
            }
            CHECK_MESSAGE(ok, what << ": " << file << ":" << line);
        }
    }
}

TEST_CASE("Source guard: every settings INI read goes through the shipped-defaults overlay")
{
    // The main Huginn.ini is opened only by Settings::LoadMainIni (and written
    // only by RestoreShippedFiles), so every loader reads it over the shipped
    // file. A new direct read here would make a loader constant the default of
    // a key the shipped file sets -- the drift 0.24.10 removed.
    RequireOnlyIn(std::regex(R"(\bGetMainIniPath\s*\()"),
        { "src/Globals.h", "src/Globals.cpp", "src/settings/ShippedDefaults.cpp" },
        "GetMainIniPath() outside the overlay");

    // dMenu's INI path is handed only to LoadDMenuIni or to ReloadAllSettings
    // (which hands it to LoadDMenuIni).
    RequireOnlyIn(std::regex(R"(\bGetDMenu(Own)?IniPath\s*\()"),
        { "src/Globals.h", "src/Globals.cpp", "src/settings/ShippedDefaults.cpp", "src/Main.cpp",
          "src/settings/SettingsReloader.cpp", "src/console/ConsoleCommands.cpp" },
        "a dMenu INI path outside the overlay's callers");

    // The plain parse front door is for the files that are not settings:
    // Huginn_Overrides.ini (read by the spell and item override loaders, in
    // their folders) and the test harness's INI.
    RequireOnlyIn(std::regex(R"(\bLoadIniFile\s*\()"),
        { "src/IniLoad.h", "src/Globals.cpp", "src/learning/item/", "src/spell/", "src/TestHarness.cpp" },
        "LoadIniFile on a settings file");

    // SimpleIni's own loaders: the overlay, LoadIniFile, and the effect
    // catalog's override directory (its own files, not settings).
    // Through `.` or `->`.
    RequireOnlyIn(std::regex(R"((\.|->)\s*Load(Data|File)\s*\()"),
        { "src/settings/IniOverlay.h", "src/Globals.cpp", "src/effect/EffectCatalog.cpp" },
        "a SimpleIni load outside the overlay");

    // The two settings files' paths are spelled once, in Globals.cpp
    // (GetMainIniPath, GetDMenuOwnIniPath): a hand-built path elsewhere would
    // be a way round the guards above. `Plugins` then a separator then
    // `Huginn.ini` is the main INI; `customSettings`, separator, `ini` is
    // dMenu's folder (Huginn.json's own "ini" value is not C++ and not here).
    RequireOnlyIn(std::regex(R"(Plugins(/|\\\\)+Huginn\.ini|customSettings(/|\\\\)+ini)"),
        { "src/Globals.cpp" },
        "a settings INI path spelled outside Globals.cpp");
}

TEST_CASE("DMenuDefaults: SameIniContent compares keys, not layout")
{
    const std::string ours = "[Widget]\nbEnabled = true\nfScale = 70.000000\n\n[Debug]\niRecommendationLog = 1.000000\n";
    // dMenu's own re-save: BOM, CRLF, no spaces, another order, a comment.
    CHECK(DMenu::SameIniContent(ours,
        "\xEF\xBB\xBF; dMenu\r\n[Debug]\r\niRecommendationLog=1.000000\r\n[Widget]\r\nfScale=70.000000\r\nbEnabled=true\r\n"));
    CHECK(DMenu::SameIniContent(ours, ours));
    CHECK_FALSE(DMenu::SameIniContent(ours, "[Widget]\nbEnabled = true\nfScale = 85.000000\n\n[Debug]\niRecommendationLog = 1.000000\n"));
    CHECK_FALSE(DMenu::SameIniContent(ours, "[Widget]\nbEnabled = true\n\n[Debug]\niRecommendationLog = 1.000000\n"));
    CHECK_FALSE(DMenu::SameIniContent(ours, ours + "[Widget]\nbExtra = true\n"));
    CHECK_FALSE(DMenu::SameIniContent(ours, "[widget]\nbEnabled = true\nfScale = 70.000000\n[Debug]\niRecommendationLog = 1.000000\n"));
    // The shipped defaults against themselves, through ToIniText.
    const auto r = DMenu::DefaultsFromDescriptor(Huginn::Shipped::DMenuJsonText());
    REQUIRE(r.Ok());
    CHECK(DMenu::SameIniContent(DMenu::ToIniText(r.entries), DMenu::ToIniText(r.entries)));
}
