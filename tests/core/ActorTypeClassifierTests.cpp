// Host tests for src/core/ActorTypeClassifier.h -- the code the game runs to
// read a target's type from its race and actor keywords.
//
// Ported from tools/races/race_reading_host_check.cpp (R0, a standalone
// cl-built check before the host target existed; removed in 0.23.9):
//   1. every row of docs/architecture/9-data/race_map.csv (539) reads what the
//      map expects, fed the race's keywords from
//      tools/races/lorerim_race_keywords.csv and a typical actor's keywords;
//   2. the 20 actor-keyword cases (one actor's own NPC-record keywords);
//   3. made-up races that pin rule parts no real race exercises (R0 round-2
//      verifier: mutants that survived 1 and 2).
//
// Expected reading, from the map alone:
//   - a row the map flags in today_mismatch (the 37 misread races) must read
//     its primary family, folded onto today's six types (animal, arthropod,
//     troll, giant, werebeast and monster read Beast);
//   - every other row must read what it read before (the map's `today`).
//     That is the family too, except three werebeast races that carry
//     ActorTypeUndead: the map accepts Undead for them (undead is in `also`).
//   The two skeletal dragons are flagged "Dragon only -> dragon+undead": their
//   primary family is dragon, which they already read. The +undead needs the
//   multi-hot family bitmask (R3); a single reading cannot carry it.
//
// Actor side of a map row: a typical actor of the race carries the ActorType*
// keywords that more than half of its NPC records carry -- the rule
// `hg dump races` uses for its huginnReading column.

#include "core/ActorTypeClassifier.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
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
    using Huginn::State::TargetType;
    using Huginn::State::GetTargetTypeName;
    namespace ATC = Huginn::State::ActorTypeClassifier;
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

    Keywords SplitSemicolons(const std::string& s)
    {
        Keywords out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ';')) {
            if (!item.empty()) out.insert(item);
        }
        return out;
    }

    std::string FoldFamily(const std::string& family)
    {
        if (family == "humanoid" || family == "none") return "Humanoid";
        if (family == "undead") return "Undead";
        if (family == "daedra") return "Daedra";
        if (family == "dragon") return "Dragon";
        if (family == "construct") return "Construct";
        if (family == "animal" || family == "arthropod" || family == "troll" || family == "giant" ||
            family == "werebeast" || family == "monster") {
            return "Beast";
        }
        FAIL("unknown family " << family);
        return {};
    }

    std::string Read(std::string_view race, bool flies, const Keywords& raceKw, const Keywords& actorKw)
    {
        return GetTargetTypeName(ATC::Classify(
            race, flies,
            [&](std::string_view kw) { return raceKw.contains(kw); },
            [&](std::string_view kw) { return actorKw.contains(kw); }));
    }

    // The fixture's race keywords, by formID (the join the map uses) and by editorID.
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
                CHECK_MESSAGE(!f.byFormID.contains(formID), "duplicate formID " << formID << " in the fixture");
                // An editorID can repeat across plugins (three Lucien races do);
                // the map joins on formID. A case may not name an ambiguous one.
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

TEST_CASE("race reading: every race_map.csv row reads as the map expects")
{
    const auto& fixture = LoadFixture();
    const auto map = ReadCsv("docs/architecture/9-data/race_map.csv");
    CHECK(map.size() == 539);
    CHECK(fixture.byFormID.size() == 539);

    int matched = 0, flagged = 0, flaggedRead = 0;
    for (const auto& row : map) {
        const auto& formID = row.at("formID");
        const auto& edid = row.at("editorID");
        CAPTURE(formID);
        CAPTURE(edid);
        auto kwIt = fixture.byFormID.find(formID);
        CHECK_MESSAGE(kwIt != fixture.byFormID.end(), "no keywords row for " << formID << " " << edid);
        if (kwIt == fixture.byFormID.end()) continue;

        const auto raceKw = SplitSemicolons(kwIt->second.at("keywords"));
        const bool flies = kwIt->second.at("flies") == "1";

        Keywords actorKw;
        const long npcs = std::stol(row.at("npcCount").empty() ? "0" : row.at("npcCount"));
        for (const auto& part : SplitSemicolons(row.at("npcActorTypeKeywords"))) {
            const auto eq = part.find('=');
            if (eq == std::string::npos) continue;
            if (npcs > 0 && std::stol(part.substr(eq + 1)) * 2 > npcs) actorKw.insert(part.substr(0, eq));
        }

        const bool isFlagged = !row.at("today_mismatch").empty();
        const std::string expected = isFlagged ? FoldFamily(row.at("family")) : row.at("today");
        const std::string got = Read(edid, flies, raceKw, actorKw);
        CHECK_MESSAGE(got == expected, edid << ": read " << got << ", expected " << expected << " (family "
                                            << row.at("family") << ", today " << row.at("today") << ", flag '"
                                            << row.at("today_mismatch") << "')");
        if (got == expected) ++matched;
        if (isFlagged) {
            ++flagged;
            if (got == expected) ++flaggedRead;
        }
    }
    MESSAGE("race map rows " << map.size() << ": matched " << matched << ", flagged " << flagged
                                << " read their primary family " << flaggedRead);
    CHECK(matched == 539);
    CHECK(flagged == 37);
    CHECK(flaggedRead == 37);
}

namespace
{
    struct Case
    {
        const char* race;
        const char* raceOverride;  // nullptr: the fixture's keywords (and flies) for `race`
        const char* actor;         // the actor's own keywords, ';'-separated
        const char* expected;
        const char* why;
        bool flies = false;        // made-up races only
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
            bool flies = c.flies;
            if (c.raceOverride) {
                raceKw = SplitSemicolons(c.raceOverride);
            } else {
                CHECK_MESSAGE(!fixture.ambiguousEditorIDs.contains(race),
                    race << " is on more than one fixture row");
                auto it = fixture.byEditorID.find(race);
                CHECK_MESSAGE(it != fixture.byEditorID.end(), "no fixture race " << race);
                if (it == fixture.byEditorID.end()) continue;
                raceKw = SplitSemicolons(it->second.at("keywords"));
                flies = it->second.at("flies") == "1";
            }
            // std::string throughout: doctest prints a const char* as a pointer.
            const std::string expected = c.expected;
            const std::string why = c.why;
            const std::string got = Read(race, flies, raceKw, SplitSemicolons(actor));
            CHECK_MESSAGE(got == expected, race << " + actor [" << actor << "]: read " << got
                                                << ", expected " << expected << " -- " << why);
        }
    }
}  // namespace

TEST_CASE("race reading: actor-keyword cases")
{
    // The map rows feed a TYPICAL actor, so they never exercise a lone actor
    // keyword. These do: one actor's own NPC-record keywords on top of its
    // race's real keywords (or a made-up race's).
    static const Case kCases[] = {
        { "WerewolfBeastRace", nullptr, "ActorTypeNPC", "Beast",
          "transformed werewolf: its (person's) NPC record carries ActorTypeNPC, which is not read off the actor" },
        { "FoxRace", nullptr, "ActorTypeNPC", "Beast", "a FoxRace template record with ActorTypeNPC" },
        { "NordRace", nullptr, "ActorTypeGhost", "Undead", "a ghost on a playable race (actor undead)" },
        { "NordRace", nullptr, "ActorTypeUndead", "Undead", "an undead record on a playable race" },
        { "NordRace", nullptr, "Vampire", "Undead", "a vampire record on a plain race (pre-R0 rule, kept)" },
        { "WispRace", nullptr, "ActorTypeGhost;ActorTypeUndead", "Undead", "a wisp: race Creature only" },
        { "TestLichRace", "ActorTypeCreature;ActorTypeLich", "", "Undead", "ActorTypeLich on the race" },
        { "TestCreatureRace", "ActorTypeCreature", "ActorTypeLich", "Beast",
          "ActorTypeLich on the actor only: the map reads Lich off the race" },
        { "DLC2MiraakRace", nullptr, "ActorTypeNPC;ActorTypeDaedra;DLC2ActorTypeMiraak", "Humanoid",
          "Miraak: manual Humanoid; his record's ActorTypeDaedra is not read" },
        { "zzzCHFlameQueenRace", nullptr, "ActorTypeUndead", "Daedra", "race daedra beats actor undead" },
        { "IceWraithRace", nullptr, "ActorTypeGhost;ActorTypeUndead", "Beast",
          "a ghost ice wraith: the manual table (monster) beats actor undead; spectral is a facet (R3)" },
        { "NordRace", nullptr, "ActorTypeDaedra", "Humanoid", "actor daedra on a person race is not read" },
        { "FoxRace", nullptr, "ActorTypeDragon", "Beast", "actor dragon on an animal race is not read" },
        { "HighElfRace", nullptr, "ActorTypeDwarven", "Humanoid", "actor dwarven on a person race is not read" },
        { "GiantRace", nullptr, "ActorTypeGiant", "Beast", "vanilla giants carry Giant on the NPC only" },
        { "IniGiantRace", nullptr, "ActorTypeGiant", "Beast", "giant before the race's ActorTypeNPC" },
        { "TestBareRace", "", "ActorTypeCreature", "Beast", "no race type keyword: the actor's Creature is the fallback" },
        { "TestBareRace", "", "ActorTypeNPC", "Humanoid", "no race type keyword: name fallback (Humanoid)" },
        { "zzzCHIronSpiderRace", nullptr, "", "Construct", "manual beats the race's ActorTypeUndead" },
        { "UndeadDragonRace", nullptr, "", "Dragon", "dragon beats undead (primary only)" },
    };
    static_assert(std::size(kCases) == 20);
    RunCases(kCases, std::size(kCases));
    MESSAGE("actor-keyword cases: " << std::size(kCases));
}

TEST_CASE("race reading: made-up races pin the step-7 race-name words")
{
    // No type keyword on race or actor, so step 7 decides. One race per word,
    // so no word can be dropped (or every word replaced by Humanoid) unseen.
    // "Test...Race" contains none of the words itself.
    static const Case kCases[] = {
        { "TestDragonkinRace", "", "", "Dragon", "word: dragon" },
        { "TestDraugrRace", "", "", "Undead", "word: draugr" },
        { "TestSkeletonRace", "", "", "Undead", "word: skeleton" },
        { "TestVampireRace", "", "", "Undead", "word: vampire" },
        { "TestGhostRace", "", "", "Undead", "word: ghost" },
        { "TestZombieRace", "", "", "Undead", "word: zombie" },
        { "TestAtronachRace", "", "", "Daedra", "word: atronach" },
        { "TestDremoraRace", "", "", "Daedra", "word: dremora" },
        { "TestDaedraRace", "", "", "Daedra", "word: daedra" },
        { "TestScampRace", "", "", "Daedra", "word: scamp" },
        { "TestDaedrothRace", "", "", "Daedra", "word: daedroth" },
        { "TestSeekerRace", "", "", "Daedra", "word: seeker" },
        { "TestLurkerRace", "", "", "Daedra", "word: lurker" },
        { "TestDwarvenRace", "", "", "Construct", "word: dwarven" },
        { "TestDwemerRace", "", "", "Construct", "word: dwemer" },
        { "TestSphereRace", "", "", "Construct", "word: sphere" },
        { "TestCenturionRace", "", "", "Construct", "word: centurion" },
        { "TestBallistaRace", "", "", "Construct", "word: ballista" },
        { "TestWolfRace", "", "", "Beast", "word: wolf" },
        { "TestBearRace", "", "", "Beast", "word: bear" },
        { "TestSaberRace", "", "", "Beast", "word: saber" },
        { "TestSabreRace", "", "", "Beast", "word: sabre" },
        { "TestSpiderRace", "", "", "Beast", "word: spider" },
        { "TestTrollRace", "", "", "Beast", "word: troll" },
        { "TestMammothRace", "", "", "Beast", "word: mammoth" },
        { "TestSkeeverRace", "", "", "Beast", "word: skeever" },
        { "TestHorkerRace", "", "", "Beast", "word: horker" },
        { "TestMudcrabRace", "", "", "Beast", "word: mudcrab" },
        { "TestSlaughterfishRace", "", "", "Beast", "word: slaughterfish" },
        { "TestPlainRace", "", "", "Humanoid", "no word: Humanoid" },
        { "TestDRAUGRRace", "", "", "Undead", "the words are case-insensitive" },
        // Order inside step 7
        { "TestDragonSkeletonRace", "", "", "Dragon", "dragon before the undead words" },
        { "TestSkeletonWolfRace", "", "", "Undead", "undead words before the beast words" },
        { "TestDwemerAtronachRace", "", "", "Daedra", "daedra words before construct (atronachs are not automatons)" },
        { "TestDwemerSpiderRace", "", "", "Construct", "construct words before beast (Dwemer spiders)" },
        // The flies rule
        { "TestSkyRace", "", "", "Dragon", "kFlies on a race with no type keyword reads Dragon", true },
        { "TestSkyRace", "", "", "Humanoid", "the same race without kFlies", false },
        { "TestFlyingWolfRace", "", "", "Dragon", "kFlies before the beast words", true },
    };
    RunCases(kCases, std::size(kCases));
}

TEST_CASE("race reading: made-up races pin rule order and single keywords")
{
    static const Case kCases[] = {
        // Step 2 order (race family keywords)
        { "TestRaceA", "ActorTypeDragon;ActorTypeDaedra", "", "Dragon", "race dragon before race daedra" },
        { "TestRaceA", "ActorTypeDaedra;ActorTypeUndead", "", "Daedra", "race daedra before race undead (Xivkyn)" },
        { "TestRaceA", "ActorTypeUndead;ActorTypeDwarven", "", "Undead",
          "race undead before race dwarven: a race carrying both reads Undead" },
        { "TestRaceA", "ActorTypeDwarven;ActorTypeCreature", "", "Construct", "race dwarven before the catch-alls" },
        { "TestRaceA", "ActorTypeDwarven", "ActorTypeUndead", "Construct",
          "race dwarven (step 2) before actor undead (step 3)" },
        { "TestRaceA", "ActorTypeDaedra", "ActorTypeGhost", "Daedra", "race daedra before actor undead" },
        // Step 2 single keywords
        { "TestRaceA", "ActorTypeGhost", "", "Undead", "race ActorTypeGhost alone reads Undead" },
        { "TestRaceA", "Vampire;ActorTypeNPC", "", "Undead", "race Vampire reads Undead before the NPC catch-all" },
        // Step 4: giant, off the actor alone
        { "TestRaceA", "ActorTypeNPC", "ActorTypeGiant", "Beast",
          "giant read off the ACTOR, before the race's ActorTypeNPC" },
        // Step 5: goblinoids
        { "TestRaceA", "DLC2RieklingKeyword;ActorTypeCreature", "", "Humanoid",
          "DLC2RieklingKeyword folds a Creature race into humanoid" },
        { "TestRaceA", "ActorTypeCreature", "", "Beast", "the same race without DLC2RieklingKeyword" },
        { "TestGoblinRace", "ActorTypeCreature", "", "Humanoid", "goblinoid word: goblin" },
        { "TestFalmerRace", "ActorTypeCreature", "", "Humanoid", "goblinoid word: falmer" },
        { "TestRieklingRace", "ActorTypeCreature", "", "Humanoid", "goblinoid word: riekling" },
        { "TestGrummiteRace", "ActorTypeCreature", "", "Humanoid", "goblinoid word: grummite" },
        { "TestMinotaurRace", "ActorTypeCreature", "", "Humanoid", "goblinoid word: minotaur" },
        { "TestHagravenRace", "ActorTypeCreature", "", "Humanoid", "goblinoid word: hagraven" },
        { "TestLamiaRace", "ActorTypeCreature", "", "Humanoid", "goblinoid word: lamia" },
        { "TestGoblinRace", "", "ActorTypeAnimal", "Humanoid",
          "goblinoid before the actor-side Animal/Creature fallback" },
        { "TestGoblinRace", "ActorTypeCreature", "ActorTypeGhost", "Undead", "actor undead (step 3) before goblinoid" },
        { "TestGoblinRace", "ActorTypeGiant", "", "Beast", "giant (step 4) before goblinoid" },
        // Step 6: the actor-side Animal fallback (Creature has its own case above)
        { "TestRaceA", "", "ActorTypeAnimal", "Beast",
          "no race type keyword: the actor's ActorTypeAnimal is the fallback" },
        // Step 7 guard: an empty editorID reads Humanoid before the name rules,
        // even for a flying race (there is no name to read)
        { "", "", "", "Humanoid", "empty editorID, no keywords: Humanoid", true },
        // Step 1: the manual table comes before the race's family keywords
        { "DLC2MiraakRace", "ActorTypeDragon", "", "Humanoid", "manual before race dragon" },
        { "DLC2MiraakRace", "ActorTypeDaedra", "", "Humanoid", "manual before race daedra" },
        // Step 1: the manual table matches the whole editorID, not a substring
        { "zzzCHIronSpiderRaceVariant", "ActorTypeUndead", "", "Undead",
          "an editorID that only contains a manual one is not in the table" },
        { "IronSpiderRace", "ActorTypeUndead", "", "Undead",
          "an editorID that is only part of a manual one is not in the table" },
        // Step 1: the manual table is case-insensitive
        { "ZZZCHIRONSPIDERRACE", "ActorTypeUndead", "", "Construct",
          "manual lookup ignores case (an editorID in other case still hits the table)" },
        { "dlc2miraakrace", "ActorTypeCreature", "", "Humanoid",
          "manual lookup ignores case (lower case; the race's Creature would read Beast)" },
    };
    RunCases(kCases, std::size(kCases));
}
