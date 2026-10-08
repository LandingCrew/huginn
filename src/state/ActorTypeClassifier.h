#pragma once

// =============================================================================
// ACTOR TYPE CLASSIFIER -- race and actor keywords to TargetType
// =============================================================================
// Pure code: no RE:: or SKSE types, so it runs the same in the game
// (StateEvaluator::ClassifyActor), in `hg dump races` (one reading per race),
// and on the host against docs/architecture/9-data/race_map.csv
// (tools/races/race_reading_host_check.cpp).
//
// The rules are the race map's (docs/architecture/9-data/target_types.csv),
// folded onto today's six types: humanoid, undead, daedra, dragon and
// construct are themselves; animal, arthropod, troll, giant, werebeast and
// monster all read Beast until the eleven families arrive (R3). Multi-hot
// (a skeletal dragon is dragon AND undead) needs the family bitmask, so a
// reading here is the PRIMARY family only.
//
// Order, first match wins:
//   1. The manual race table (editorIDs whose keywords say the wrong thing:
//      Vigilant's iron spiders carry ActorTypeUndead, Lucien's Dwemer
//      automatons ActorTypeAnimal, Lucien's imps ActorTypeNPC, ...).
//   2. The race's own family keywords: dragon, daedra, undead, dwarven. Daedra
//      before undead: Vigilant's Xivkyn carry both and are daedra.
//   3. The ACTOR's undead keywords (ActorTypeUndead / ActorTypeGhost /
//      Vampire on the NPC record): wisps, ghosts and zombies on a plain race.
//      Only undead is read off the actor -- with giant below, the only family
//      the race map takes from NPC records. A dragon, daedra or dwarven
//      keyword on one NPC record of a person or animal race is not something
//      the player can see; the race is.
//   4. Giant (race or actor): vanilla GiantRace carries it only on its NPCs.
//   5. Goblinoids fold into humanoid: DLC2RieklingKeyword, or the race name
//      says Falmer / Riekling / Goblin / Grummite / Minotaur / Hagraven /
//      Lamia. Without this they read Beast through ActorTypeCreature.
//   6. The catch-alls, race or actor: ActorTypeNPC is humanoid, then
//      ActorTypeAnimal / ActorTypeCreature is beast. NPC first: Vigilant's
//      minotaurs carry both and are people.
//   7. Race-name words, for races with no type keyword at all; then Humanoid.
// =============================================================================

#include "GameState.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace Huginn::State::ActorTypeClassifier
{
   // Case-insensitive substring search - zero heap allocations
   [[nodiscard]] inline bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept
   {
      auto eq = [](char a, char b) noexcept {
         return std::tolower(static_cast<unsigned char>(a)) ==
                std::tolower(static_cast<unsigned char>(b));
      };
      return std::search(haystack.begin(), haystack.end(),
                         needle.begin(), needle.end(), eq) != haystack.end();
   }

   [[nodiscard]] inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
   {
      return a.size() == b.size() && ContainsIgnoreCase(a, b);
   }

   // Step 1. From race_map.csv, reason "manual" (17 LoreRim races).
   inline constexpr std::array<std::pair<std::string_view, TargetType>, 17> kManualRaces{{
      // Dwemer automatons whose keywords do not say so (construct)
      { "aaaLucDwemerChaurusRace",       TargetType::Construct },  // Lucien "Dwarven Tunneler", ActorTypeAnimal
      { "aaaLucDweOverseerRace",         TargetType::Construct },  // Lucien automaton, no ActorTypeDwarven
      { "aaaLucDwemerWerewolfBeastRace", TargetType::Construct },  // Lucien "Dwarven Annihilator"
      { "HLIOInactiveDwarvenSphereRace", TargetType::Construct },  // inactive sphere, no keywords
      { "zzzCHIronSpiderRace",           TargetType::Construct },  // Vigilant iron spider, ActorTypeUndead
      { "zzzCHIronSpiderRaceLarge",      TargetType::Construct },
      { "zzzCHIronSpiderRaceGiant",      TargetType::Construct },
      // Monsters (Beast until the monster family exists)
      { "IceWraithRace",                 TargetType::Beast },
      { "DLC2dunKarstaagIceWraithRace",  TargetType::Beast },
      { "MagicAnomalyRace",              TargetType::Beast },
      { "JRmihailimprace",               TargetType::Beast },      // Lucien imps, ActorTypeNPC
      { "JRmihailimpracechargedshock",   TargetType::Beast },
      { "JRmihailimpracechargedfire",    TargetType::Beast },
      { "JRmihailimpracebossshock",      TargetType::Beast },
      { "JRmihailimpracebossfire",       TargetType::Beast },
      // Others
      { "GRVEHerrahDraugrRace",          TargetType::Undead },     // "Wraithmother", draugr skeleton, Creature only
      { "DLC2MiraakRace",                TargetType::Humanoid },   // Miraak: a Nord; no type keyword on the race
   }};

   [[nodiscard]] inline bool LookupManual(std::string_view raceEditorID, TargetType& out) noexcept
   {
      for (const auto& [id, type] : kManualRaces) {
         if (EqualsIgnoreCase(raceEditorID, id)) {
            out = type;
            return true;
         }
      }
      return false;
   }

   [[nodiscard]] inline bool IsGoblinoidName(std::string_view raceEditorID) noexcept
   {
      constexpr std::array<std::string_view, 7> kWords{
         "falmer", "riekling", "goblin", "grummite", "minotaur", "hagraven", "lamia" };
      return std::ranges::any_of(kWords, [&](std::string_view w) { return ContainsIgnoreCase(raceEditorID, w); });
   }

   // Step 7: the race-name words (unchanged from the pre-R0 classifier).
   [[nodiscard]] inline TargetType ClassifyByRaceName(std::string_view raceID, bool raceFlies) noexcept
   {
      auto any = [&](std::initializer_list<std::string_view> words) {
         return std::ranges::any_of(words, [&](std::string_view w) { return ContainsIgnoreCase(raceID, w); });
      };
      if (ContainsIgnoreCase(raceID, "dragon")) return TargetType::Dragon;
      // Flying races (dragons have kFlies) even if the ID does not say dragon
      if (raceFlies) return TargetType::Dragon;
      if (any({ "draugr", "skeleton", "vampire", "ghost", "zombie" })) return TargetType::Undead;
      // Daedra before Construct so atronachs are not automatons
      if (any({ "atronach", "dremora", "daedra", "scamp", "daedroth", "seeker", "lurker" })) return TargetType::Daedra;
      // Construct before Beast so Dwemer spiders are not spiders
      if (any({ "dwarven", "dwemer", "sphere", "centurion", "ballista" })) return TargetType::Construct;
      if (any({ "wolf", "bear", "saber", "sabre", "spider", "troll", "mammoth", "skeever",
                "horker", "mudcrab", "slaughterfish" })) return TargetType::Beast;
      return TargetType::Humanoid;
   }

   /// @param raceEditorID  the race's editorID ("" when it has none)
   /// @param raceFlies     RACE_DATA kFlies
   /// @param raceHas       bool(std::string_view keyword): the race has it
   /// @param actorHas      bool(std::string_view keyword): the actor (NPC record) has it
   template <class RaceHas, class ActorHas>
   [[nodiscard]] TargetType Classify(std::string_view raceEditorID, bool raceFlies,
                                     RaceHas&& raceHas, ActorHas&& actorHas)
   {
      // 1. Manual race table
      if (TargetType manual{}; !raceEditorID.empty() && LookupManual(raceEditorID, manual)) {
         return manual;
      }

      // 2. The race's family keywords
      if (raceHas("ActorTypeDragon")) return TargetType::Dragon;
      if (raceHas("ActorTypeDaedra")) return TargetType::Daedra;
      if (raceHas("ActorTypeUndead") || raceHas("ActorTypeGhost") || raceHas("ActorTypeLich") ||
          raceHas("Vampire")) {
         return TargetType::Undead;
      }
      if (raceHas("ActorTypeDwarven")) return TargetType::Construct;

      // 3. The actor's undead keywords
      if (actorHas("ActorTypeUndead") || actorHas("ActorTypeGhost") || actorHas("Vampire")) {
         return TargetType::Undead;
      }

      // 4. Giant
      if (raceHas("ActorTypeGiant") || actorHas("ActorTypeGiant")) return TargetType::Beast;

      // 5. Goblinoids
      if (raceHas("DLC2RieklingKeyword") || IsGoblinoidName(raceEditorID)) return TargetType::Humanoid;

      // 6. Catch-alls
      if (raceHas("ActorTypeNPC") || actorHas("ActorTypeNPC")) return TargetType::Humanoid;
      if (raceHas("ActorTypeAnimal") || raceHas("ActorTypeCreature") ||
          actorHas("ActorTypeAnimal") || actorHas("ActorTypeCreature")) {
         return TargetType::Beast;
      }

      // 7. No type keyword at all
      if (raceEditorID.empty()) return TargetType::Humanoid;
      return ClassifyByRaceName(raceEditorID, raceFlies);
   }
}
