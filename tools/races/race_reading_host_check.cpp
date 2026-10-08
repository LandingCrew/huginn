// Host check: Huginn's actor-type reading against the race map.
//
// Runs src/state/ActorTypeClassifier.h -- the code the game runs -- over every
// row of docs/architecture/9-data/race_map.csv and compares it with the race
// map's verdict, on the host, without the game. Until R1 adds a host test
// target, build it by hand from a VS 2022 developer prompt at the repo root:
//
//   cl /nologo /std:c++latest /EHsc /W4 /permissive- tools\races\race_reading_host_check.cpp /Fe:%TEMP%\race_check.exe /Fo:%TEMP%\
//   %TEMP%\race_check.exe tools\races\lorerim_race_keywords.csv docs\architecture\9-data\race_map.csv
//
// Exit code 0 when every row matches, 1 otherwise.
//
// Inputs:
//   lorerim_race_keywords.csv  formID, editorID, flies, keywords for the 539
//                              races in the map, taken from the LoreRim
//                              `hg dump races` (2026-10-07 22:33) the map was
//                              built from. race_map.csv only lists the race's
//                              ActorType* keywords; the classifier also reads
//                              DLC2RieklingKeyword and Vampire.
//   race_map.csv               npcCount + npcActorTypeKeywords (the actor side),
//                              family / today / today_mismatch (the verdict).
//
// Actor side: a typical actor of the race carries the ActorType* keywords that
// more than half of its NPC records carry -- the same rule `hg dump races`
// uses for its huginnReading column.
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

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../src/state/ActorTypeClassifier.h"

using Huginn::State::TargetType;
using Row = std::map<std::string, std::string>;

static std::vector<std::string> SplitCsvLine(std::istream& in, bool& ok)
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

static std::vector<Row> ReadCsv(const char* path)
{
   std::ifstream in(path, std::ios::binary);
   if (!in) {
      std::cerr << "cannot open " << path << "\n";
      std::exit(2);
   }
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

static std::set<std::string> SplitSemicolons(const std::string& s)
{
   std::set<std::string> out;
   std::stringstream ss(s);
   std::string item;
   while (std::getline(ss, item, ';')) {
      if (!item.empty()) out.insert(item);
   }
   return out;
}

static TargetType FoldFamily(const std::string& family)
{
   if (family == "humanoid" || family == "none") return TargetType::Humanoid;
   if (family == "undead") return TargetType::Undead;
   if (family == "daedra") return TargetType::Daedra;
   if (family == "dragon") return TargetType::Dragon;
   if (family == "construct") return TargetType::Construct;
   if (family == "animal" || family == "arthropod" || family == "troll" || family == "giant" ||
       family == "werebeast" || family == "monster") {
      return TargetType::Beast;
   }
   std::cerr << "unknown family " << family << "\n";
   std::exit(2);
}

int main(int argc, char** argv)
{
   if (argc < 3) {
      std::cerr << "usage: race_check <lorerim_race_keywords.csv> <race_map.csv>\n";
      return 2;
   }
   std::map<std::string, Row> keywordsByFormID;
   for (auto& r : ReadCsv(argv[1])) keywordsByFormID[r["formID"]] = r;

   const auto map = ReadCsv(argv[2]);
   int mismatches = 0, flaggedFixed = 0, flagged = 0, unchangedOk = 0;
   std::map<std::string, int> readingCounts;
   for (const auto& row : map) {
      const auto& formID = row.at("formID");
      const auto& edid = row.at("editorID");
      auto kwIt = keywordsByFormID.find(formID);
      if (kwIt == keywordsByFormID.end()) {
         std::cout << std::format("MISSING  {} {}: no keywords row\n", formID, edid);
         ++mismatches;
         continue;
      }
      const auto raceKw = SplitSemicolons(kwIt->second.at("keywords"));
      const bool flies = kwIt->second.at("flies") == "1";

      // Actor side: ActorType* keywords on more than half the race's NPC records.
      std::set<std::string> actorKw;
      const long npcs = std::stol(row.at("npcCount").empty() ? "0" : row.at("npcCount"));
      for (const auto& part : SplitSemicolons(row.at("npcActorTypeKeywords"))) {
         const auto eq = part.find('=');
         if (eq == std::string::npos) continue;
         if (npcs > 0 && std::stol(part.substr(eq + 1)) * 2 > npcs) actorKw.insert(part.substr(0, eq));
      }

      const TargetType got = Huginn::State::ActorTypeClassifier::Classify(
         edid, flies,
         [&](std::string_view kw) { return raceKw.contains(std::string(kw)); },
         [&](std::string_view kw) { return actorKw.contains(std::string(kw)); });

      const bool isFlagged = !row.at("today_mismatch").empty();
      const std::string expectedName = isFlagged
         ? Huginn::State::GetTargetTypeName(FoldFamily(row.at("family")))
         : row.at("today");
      const std::string gotName = Huginn::State::GetTargetTypeName(got);
      ++readingCounts[gotName];

      if (gotName != expectedName) {
         ++mismatches;
         std::cout << std::format("MISMATCH {} {}: read {}, expected {} (family {}, today {}, flag '{}')\n",
            formID, edid, gotName, expectedName, row.at("family"), row.at("today"), row.at("today_mismatch"));
      } else if (isFlagged) {
         ++flaggedFixed;
         std::cout << std::format("{} {:<36} {:<10} -> {:<10} ({})\n",
            gotName != row.at("today") ? "fixed   " : "primary ", edid, row.at("today"), gotName,
            row.at("today_mismatch"));
      } else {
         ++unchangedOk;
      }
      if (isFlagged) ++flagged;
   }

   std::cout << std::format("\nrows {}  matched {}  mismatched {}\n", map.size(), map.size() - mismatches, mismatches);
   std::cout << std::format("flagged (today_mismatch) {}: read their primary family {}\n", flagged, flaggedFixed);
   std::cout << std::format("unflagged rows reading as before: {}\n", unchangedOk);
   std::cout << "readings:";
   for (const auto& [name, n] : readingCounts) std::cout << std::format(" {}={}", name, n);
   std::cout << "\n";
   return mismatches == 0 ? 0 : 1;
}
