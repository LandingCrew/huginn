// Host tests for src/core/TargetFamilies.h -- the multi-hot family mask the
// need vector reads (needs.csv target_humanoid .. target_element_shock).
//
//   1. every row of docs/architecture/9-data/race_map.csv (539) reads
//      bits(family) | bits(also) -- `none` is no bits -- fed the race's
//      keywords from tools/races/lorerim_race_keywords.csv (joined on formID)
//      and a typical actor's keywords: the ActorType* keywords on more than
//      half of the race's NPC records (npcActorTypeKeywords), the same reading
//      ActorTypeClassifierTests.cpp uses;
//   2. named cases that pin rule parts.
//
// The expectation comes from the map alone (family and also columns, written
// by the race survey that measured doc 9's target types), not from this code.

#include "core/TargetFamilies.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef HUGINN_REPO_ROOT
#error "HUGINN_REPO_ROOT must be defined (tests/CMakeLists.txt)"
#endif

namespace
{
    using namespace Huginn::Core::Needs;
    using Row = std::map<std::string, std::string>;
    using Keywords = std::set<std::string, std::less<>>;

    std::vector<std::string> SplitCsvLine(std::istream& in, bool& ok)
    {
        // RFC 4180: quoted fields may hold commas, doubled quotes and newlines.
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false;
        char c;
        ok = false;
        while (in.get(c)) {
            ok = true;
            if (quoted) {
                if (c == '"') {
                    if (in.peek() == '"') { field += '"'; in.get(c); }
                    else quoted = false;
                } else {
                    field += c;
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == ',') {
                fields.push_back(std::move(field));
                field.clear();
            } else if (c == '\n') {
                break;
            } else if (c != '\r') {
                field += c;
            }
        }
        if (ok) fields.push_back(std::move(field));
        return fields;
    }

    std::vector<Row> ReadCsv(const std::string& relPath)
    {
        const std::string path = std::string(HUGINN_REPO_ROOT) + "/" + relPath;
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "cannot open " << path);
        bool ok = false;
        auto header = SplitCsvLine(in, ok);
        if (!header.empty() && header[0].starts_with("\xEF\xBB\xBF")) header[0].erase(0, 3);
        std::vector<Row> rows;
        while (true) {
            auto f = SplitCsvLine(in, ok);
            if (!ok) break;
            if (f.size() == 1 && f[0].empty()) continue;
            Row r;
            for (size_t i = 0; i < header.size() && i < f.size(); ++i) r[header[i]] = f[i];
            rows.push_back(std::move(r));
        }
        return rows;
    }

    Keywords Split(const std::string& s, char sep)
    {
        Keywords out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, sep)) {
            if (!item.empty()) out.insert(item);
        }
        return out;
    }

    std::uint32_t BitOf(const std::string& name)
    {
        static const std::map<std::string, Family, std::less<>> kNames{
            { "humanoid", Family::Humanoid }, { "undead", Family::Undead }, { "daedra", Family::Daedra },
            { "dragon", Family::Dragon }, { "construct", Family::Construct }, { "animal", Family::Animal },
            { "arthropod", Family::Arthropod }, { "troll", Family::Troll }, { "giant", Family::Giant },
            { "werebeast", Family::Werebeast }, { "monster", Family::Monster }, { "spectral", Family::Spectral },
            { "element_fire", Family::ElementFire }, { "element_frost", Family::ElementFrost },
            { "element_shock", Family::ElementShock },
        };
        if (name == "none") return 0;
        const auto it = kNames.find(name);
        REQUIRE_MESSAGE(it != kNames.end(), "unknown family " << name);
        return FamilyBit(it->second);
    }

    std::string Names(std::uint32_t mask)
    {
        static const char* kOrder[] = { "humanoid", "undead", "daedra", "dragon", "construct", "animal", "arthropod",
            "troll", "giant", "werebeast", "monster", "spectral", "element_fire", "element_frost", "element_shock" };
        std::string out;
        for (unsigned i = 0; i < static_cast<unsigned>(Family::_Count); ++i) {
            if (mask & (1u << i)) {
                if (!out.empty()) out += '+';
                out += kOrder[i];
            }
        }
        return out.empty() ? "(none)" : out;
    }

    std::uint32_t Read(std::string_view race, std::string_view name, bool flies, const Keywords& raceKw,
                       const Keywords& actorKw)
    {
        return ClassifyFamilies(
            race, name, flies,
            [&](std::string_view kw) { return raceKw.contains(kw); },
            [&](std::string_view kw) { return actorKw.contains(kw); });
    }

    struct Fixture
    {
        std::map<std::string, Row> byFormID;
        std::map<std::string, Row> byEditorID;
        std::set<std::string, std::less<>> ambiguousEditorIDs;
    };

    const Fixture& LoadFixture()
    {
        static const Fixture fixture = [] {
            Fixture f;
            for (auto& r : ReadCsv("tools/races/lorerim_race_keywords.csv")) {
                const std::string formID = r.at("formID");
                const std::string edid = r.at("editorID");
                if (!edid.empty() && !f.byEditorID.emplace(edid, r).second) {
                    f.ambiguousEditorIDs.insert(edid);
                }
                f.byFormID.emplace(formID, std::move(r));
            }
            return f;
        }();
        return fixture;
    }
}  // namespace

TEST_CASE("target families: every race_map.csv row reads its family and overlays")
{
    const auto& fixture = LoadFixture();
    const auto map = ReadCsv("docs/architecture/9-data/race_map.csv");
    CHECK(map.size() == 539);

    int matched = 0, multi = 0, none = 0;
    for (const auto& row : map) {
        const auto& formID = row.at("formID");
        const auto& edid = row.at("editorID");
        CAPTURE(formID);
        CAPTURE(edid);
        const auto kwIt = fixture.byFormID.find(formID);
        CHECK_MESSAGE(kwIt != fixture.byFormID.end(), "no keywords row for " << formID << " " << edid);
        if (kwIt == fixture.byFormID.end()) continue;

        const auto raceKw = Split(kwIt->second.at("keywords"), ';');
        const bool flies = kwIt->second.at("flies") == "1";

        // The typical actor: ActorType* keywords on MORE than half of the NPC records.
        Keywords actorKw;
        const long npcs = std::stol(row.at("npcCount").empty() ? "0" : row.at("npcCount"));
        for (const auto& part : Split(row.at("npcActorTypeKeywords"), ';')) {
            const auto eq = part.find('=');
            if (eq == std::string::npos) continue;
            if (npcs > 0 && std::stol(part.substr(eq + 1)) * 2 > npcs) actorKw.insert(part.substr(0, eq));
        }

        std::uint32_t expected = BitOf(row.at("family"));
        for (const auto& also : Split(row.at("also"), ';')) expected |= BitOf(also);
        if (expected == 0) ++none;
        if ((expected & (expected - 1)) != 0) ++multi;

        const std::uint32_t got = Read(edid, row.at("name"), flies, raceKw, actorKw);
        CHECK_MESSAGE(got == expected, edid << " (" << row.at("name") << "): read " << Names(got) << ", expected "
                                            << Names(expected) << " (" << row.at("reason_detail") << ")");
        if (got == expected) ++matched;
    }
    MESSAGE("race map rows " << map.size() << ": matched " << matched << " (multi-hot " << multi << ", none "
                             << none << ")");
    CHECK(matched == 539);
    CHECK(multi == 113);  // rows whose `also` is non-empty
    CHECK(none == 3);
}

namespace
{
    struct Case
    {
        const char* race;
        const char* raceOverride;  // nullptr: the fixture's keywords for `race`
        const char* actor;         // the actor's own keywords, ';'-separated
        const char* name;          // the race's display name
        const char* expected;      // '+'-joined family names, or "(none)"
        const char* why;
    };

    void RunCases(const Case* cases, size_t n)
    {
        const auto& fixture = LoadFixture();
        for (size_t i = 0; i < n; ++i) {
            const Case& c = cases[i];
            const std::string race = c.race;
            const std::string actor = c.actor;
            CAPTURE(race);
            CAPTURE(actor);
            Keywords raceKw;
            bool flies = false;
            if (c.raceOverride) {
                raceKw = Split(c.raceOverride, ';');
            } else {
                CHECK_MESSAGE(!fixture.ambiguousEditorIDs.contains(race), race << " is on more than one fixture row");
                const auto it = fixture.byEditorID.find(race);
                CHECK_MESSAGE(it != fixture.byEditorID.end(), "no fixture race " << race);
                if (it == fixture.byEditorID.end()) continue;
                raceKw = Split(it->second.at("keywords"), ';');
                flies = it->second.at("flies") == "1";
            }
            const std::string expected = c.expected;
            const std::string why = c.why;
            const std::string got = Names(Read(race, c.name, flies, raceKw, Split(actor, ';')));
            CHECK_MESSAGE(got == expected, race << " + actor [" << actor << "]: read " << got << ", expected "
                                                << expected << " -- " << why);
        }
    }
}  // namespace

TEST_CASE("target families: named cases pin each rule part")
{
    static const Case kCases[] = {
        { "NordRaceVampire", nullptr, "", "Nord", "humanoid+undead", "a vampire is undead and a person" },
        { "DLC1VampireBeastRace", nullptr, "ActorTypeUndead", "Vampire Lord", "undead",
          "a Vampire Lord (IsBeastRace) is not a person" },
        { "UndeadDragonRace", nullptr, "", "Dragon Race", "undead+dragon", "a skeletal dragon is dragon and undead" },
        { "AtronachFrostRace", nullptr, "", "Frost Atronach", "daedra+element_frost", "an elemental daedra" },
        { "TrollRace", nullptr, "", "Troll", "animal+troll", "trolls carry ActorTypeAnimal: troll primary, animal overlay" },
        { "zzzCHXivkynRace", nullptr, "", "Xivkyn Race", "humanoid+undead+daedra", "daedra first; undead and person overlays" },
        { "NordRace", nullptr, "ActorTypeGhost", "Nord", "humanoid+undead+spectral",
          "a ghost NPC record on a playable race: actor ghost keyword" },
        { "WerewolfBeastRace", nullptr, "", "Werewolf", "werebeast", "the lycanthrope keyword" },
        { "DLC2RieklingRace", nullptr, "", "Riekling", "humanoid", "goblinoids fold into humanoid" },
        { "MadeUpBlobRace", "ActorTypeCreature", "", "Blob", "monster", "a creature with nothing else: the catch-all" },
        { "MadeUpBareRace", "", "", "", "(none)", "no type keyword at all" },
        { "MadeUpFireImpRace", "ActorTypeCreature", "", "Fire Imp", "monster+element_fire",
          "an elemental by name: element read off the name" },
        { "MadeUpSpiderDaedraRace", "ActorTypeCreature;ActorTypeDaedra;ActorTypeAnimal", "", "Frostbite Spider",
          "daedra+arthropod", "the element is not read off a non-elemental name" },
        { "MadeUpDwemerFrostRace", "ActorTypeDwarven", "", "Frost Atronach", "construct",
          "constructs are never elementals, whatever the name" },
        { "MadeUpGuarRace", "ActorTypeCreature", "", "Guar", "animal", "Creature-only race named guar: animal" },
        { "MadeUpGiantRace", "ActorTypeCreature", "ActorTypeGiant", "Giant", "giant",
          "giant read off the NPC record (vanilla GiantRace)" },
        { "MadeUpWispRace", "ActorTypeCreature", "ActorTypeUndead", "Wisp", "undead+spectral",
          "a wisp-named undead is spectral" },
        { "MadeUpMinotaurRace", "ActorTypeNPC;ActorTypeAnimal", "", "Minotaur", "humanoid+animal",
          "a person with ActorTypeAnimal keeps the animal overlay" },
    };
    RunCases(kCases, std::size(kCases));
}
