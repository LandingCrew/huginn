#pragma once

// =============================================================================
// TARGET FAMILIES -- race and actor keywords to the multi-hot family mask
// =============================================================================
// needs.csv target_humanoid .. target_element_shock (rows 45-59): eleven
// families and four facets, multi-hot (a vampire is undead AND humanoid, a
// skeletal dragon dragon AND undead). The rules are docs/architecture/9-data/
// target_types.csv's rule column; tests/core/TargetFamiliesTests.cpp checks
// every one of race_map.csv's 539 rows (family | also) against this code.
//
// This is the NEEDS reading only. Scoring keeps today's single TargetType
// from core/ActorTypeClassifier.h (unchanged; it folds six of these families
// onto Beast). The game side ORs this mask over the living combat hostiles
// and holds it for the fight (StateManager, PollTargets).
//
// Inputs: the race's editorID and display name, the race's keywords
// (raceHas) and the ACTOR's own NPC-record keywords (actorHas), as in
// ActorTypeClassifier::Classify.
//
// The primary family, first match wins:
//   0. The manual race table: race_map.csv's reason=manual rows (keywords that
//      say the wrong thing), read as the map lists them, overlays included.
//   1. dragon     race ActorTypeDragon
//   2. daedra     race ActorTypeDaedra
//   3. werebeast  race HRI_Lycan_Keyword_IsWerecreature or DLC2WerebearKeyword,
//                 or the editorID says Werewolf / Werebear / WereVampire
//   4. undead     race ActorTypeUndead / ActorTypeGhost / ActorTypeLich /
//                 Vampire, or the actor's ActorTypeUndead / ActorTypeGhost
//   5. construct  race ActorTypeDwarven
//   6. troll      race ActorTypeTroll
//   7. giant      race or actor ActorTypeGiant, or race reqRaceIsGiantRace
//   8. humanoid   goblinoid (race DLC2RieklingKeyword, or the editorID says
//                 Falmer / Riekling / Goblin / Grummite / Minotaur / Hagraven /
//                 Lamia), or race ActorTypeNPC
//   9. arthropod  the editorID says Spider / Chaurus / Scorpion / Shalk /
//                 Beetle / Kwama / Arachn / Elytra / AshHopper / Scrib
//  10. animal     race ActorTypeAnimal; or a creature (11) with a Requiem
//                 animal skeleton tag (reqRaceIs<animal>Race) or an editorID
//                 saying Guar / Kagouti / Alit / Nixhound
//  11. monster    a creature: race ActorTypeCreature or REQ_Spriggan, or (a
//                 race with none of NPC / Animal / Creature) the actor's
//                 ActorTypeCreature or ActorTypeAnimal
//      none       no type keyword at all: no bits (the map's three `none`)
//
// Overlays, added to any primary other than themselves:
//   undead     the undead test of 4 (Xivkyn, skeletal dragons, Iron spiders...)
//   humanoid   race ActorTypeNPC, or race Vampire without IsBeastRace (a
//              vampire is a person; a Vampire Lord is not)
//   giant      race or actor ActorTypeGiant
//   troll      race ActorTypeTroll (Vigilant's Ogrim)
//   arthropod  the arthropod name of 9 (Dwarven spiders, Arachne, spider daedra)
//   animal     race ActorTypeAnimal, on a humanoid or troll primary only
//              (minotaurs, trolls; an undead or daedric animal is not one)
// Facets:
//   spectral   race or actor ActorTypeGhost, or the actor's ActorTypeSpirit
//              (Requiem spirits); or, when undead is set, the editorID or name
//              says Ghost / Wraith / Wisp / Shade / Spectr
//   element_*  elementals only: a daedra or monster primary that is a daedra,
//              has REQ_ActorTypeDaedraNoFlesh, or is named Atronach / Wraith /
//              Wyrm / Imp (editorID or name). The element words are read off
//              the editorID, and off the name only when the name itself says
//              Atronach / Wraith / Wyrm / Imp (a Fire Wyrm, not a "Frostbite
//              Spider" daedra): fire Flame / Fire / Ember / Incinerat, frost
//              Frost / Ice / Snowray / Goron, shock Storm / Shock / Thunder /
//              Lightning. Constructs named after atronachs are not elementals.
//
// The actor-side keyword reading in the test is the race map's: the
// ActorType* keywords on more than half of a race's NPC records.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "ActorTypeClassifier.h"
#include "NeedSnapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace Huginn::Core::Needs
{
    namespace FamilyRules
    {
        using State::ActorTypeClassifier::ContainsIgnoreCase;
        using State::ActorTypeClassifier::EqualsIgnoreCase;

        inline constexpr std::uint32_t kHumanoid = FamilyBit(Family::Humanoid);
        inline constexpr std::uint32_t kUndead = FamilyBit(Family::Undead);
        inline constexpr std::uint32_t kDaedra = FamilyBit(Family::Daedra);
        inline constexpr std::uint32_t kDragon = FamilyBit(Family::Dragon);
        inline constexpr std::uint32_t kConstruct = FamilyBit(Family::Construct);
        inline constexpr std::uint32_t kAnimal = FamilyBit(Family::Animal);
        inline constexpr std::uint32_t kArthropod = FamilyBit(Family::Arthropod);
        inline constexpr std::uint32_t kTroll = FamilyBit(Family::Troll);
        inline constexpr std::uint32_t kGiant = FamilyBit(Family::Giant);
        inline constexpr std::uint32_t kWerebeast = FamilyBit(Family::Werebeast);
        inline constexpr std::uint32_t kMonster = FamilyBit(Family::Monster);
        inline constexpr std::uint32_t kSpectral = FamilyBit(Family::Spectral);
        inline constexpr std::uint32_t kFire = FamilyBit(Family::ElementFire);
        inline constexpr std::uint32_t kFrost = FamilyBit(Family::ElementFrost);
        inline constexpr std::uint32_t kShock = FamilyBit(Family::ElementShock);

        // Rule 0: race_map.csv reason=manual (17 rows), with their `also`.
        inline constexpr std::array<std::pair<std::string_view, std::uint32_t>, 17> kManualRaces{ {
            { "aaaLucDwemerChaurusRace", kConstruct },        // Lucien "Dwarven Tunneler", ActorTypeAnimal
            { "aaaLucDweOverseerRace", kConstruct },          // Lucien automaton, no ActorTypeDwarven
            { "aaaLucDwemerWerewolfBeastRace", kConstruct },  // Lucien "Dwarven Annihilator"
            { "HLIOInactiveDwarvenSphereRace", kConstruct },  // inactive sphere, no keywords
            { "zzzCHIronSpiderRace", kConstruct | kUndead },  // Vigilant iron spider, ActorTypeUndead
            { "zzzCHIronSpiderRaceLarge", kConstruct | kUndead },
            { "zzzCHIronSpiderRaceGiant", kConstruct | kUndead },
            { "IceWraithRace", kMonster | kFrost },           // Creature only; Ghost on 3/9 NPC records
            { "DLC2dunKarstaagIceWraithRace", kMonster | kUndead | kSpectral | kFrost },
            { "MagicAnomalyRace", kMonster },
            { "JRmihailimprace", kMonster },                  // Lucien imps, ActorTypeNPC
            { "JRmihailimpracechargedshock", kMonster | kShock },
            { "JRmihailimpracechargedfire", kMonster | kFire },
            { "JRmihailimpracebossshock", kMonster | kShock },
            { "JRmihailimpracebossfire", kMonster | kFire },
            { "GRVEHerrahDraugrRace", kUndead },              // "Wraithmother", draugr skeleton, Creature only
            { "DLC2MiraakRace", kHumanoid },                  // Miraak: a Nord; no type keyword on the race
        } };

        [[nodiscard]] inline bool AnyWord(std::string_view text, std::initializer_list<std::string_view> words) noexcept
        {
            return std::ranges::any_of(words, [&](std::string_view w) { return ContainsIgnoreCase(text, w); });
        }

        // Requiem's skeleton tags that name an animal race (rule 10).
        inline constexpr std::array<std::string_view, 18> kAnimalSkeletonTags{
            "reqRaceIsDogRace", "reqRaceIsWolfRace", "reqRaceIsDLC1DeathHoundRace", "reqRaceIsChickenRace",
            "reqRaceIsHareRace", "reqRaceIsMudcrabRace", "reqRaceIsSkeeverRace", "reqRaceIsSlaughterfishRace",
            "reqRaceIsHorkerRace", "reqRaceIsBearRace", "reqRaceIsSabreCatRace", "reqRaceIsDeerRace",
            "reqRaceIsElkRace", "reqRaceIsGoatRace", "reqRaceIsCowRace", "reqRaceIsFoxRace",
            "reqRaceIsHorseRace", "reqRaceIsMammothRace",
        };
    }

    /// The family mask (FamilyBit OR-ed) for one actor; rules in the header.
    /// @param raceEditorID  the race's editorID ("" when it has none)
    /// @param raceName      the race's display name ("" when it has none)
    /// @param raceFlies     RACE_DATA kFlies (unused by the rules; kept for
    ///                      parity with ActorTypeClassifier::Classify)
    /// @param raceHas       bool(std::string_view keyword): the race has it
    /// @param actorHas      bool(std::string_view keyword): the actor (NPC record) has it
    template <class RaceHas, class ActorHas>
    [[nodiscard]] std::uint32_t ClassifyFamilies(std::string_view raceEditorID, std::string_view raceName,
        [[maybe_unused]] bool raceFlies, RaceHas&& raceHas, ActorHas&& actorHas)
    {
        using namespace FamilyRules;

        // 0. Manual race table
        if (!raceEditorID.empty()) {
            for (const auto& [id, mask] : kManualRaces) {
                if (EqualsIgnoreCase(raceEditorID, id)) return mask;
            }
        }

        const std::string_view ed = raceEditorID;
        const bool undeadKw = raceHas("ActorTypeUndead") || raceHas("ActorTypeGhost") || raceHas("ActorTypeLich") ||
                              raceHas("Vampire") || actorHas("ActorTypeUndead") || actorHas("ActorTypeGhost");
        const bool giantKw = raceHas("ActorTypeGiant") || actorHas("ActorTypeGiant");
        const bool trollKw = raceHas("ActorTypeTroll");
        const bool npcKw = raceHas("ActorTypeNPC");
        const bool animalKw = raceHas("ActorTypeAnimal");
        const bool goblinoid = raceHas("DLC2RieklingKeyword") ||
            AnyWord(ed, { "falmer", "riekling", "goblin", "grummite", "minotaur", "hagraven", "lamia" });
        const bool arthropodName = AnyWord(ed, { "spider", "chaurus", "scorpion", "shalk", "beetle", "kwama", "arachn",
                                                 "elytra", "ashhopper", "scrib" });
        const bool were = raceHas("HRI_Lycan_Keyword_IsWerecreature") || raceHas("DLC2WerebearKeyword") ||
                          AnyWord(ed, { "werewolf", "werebear", "werevampire" });
        const bool raceTyped = npcKw || animalKw || raceHas("ActorTypeCreature");
        const bool creature = raceHas("ActorTypeCreature") || raceHas("REQ_Spriggan") ||
                              (!raceTyped && (actorHas("ActorTypeCreature") || actorHas("ActorTypeAnimal")));
        const bool animalTag = std::ranges::any_of(kAnimalSkeletonTags, [&](std::string_view t) { return raceHas(t); }) ||
                               AnyWord(ed, { "guar", "kagouti", "alit", "nixhound", "nix-hound" });

        // 1-11. The primary family
        std::uint32_t primary = 0;
        if (raceHas("ActorTypeDragon")) primary = kDragon;
        else if (raceHas("ActorTypeDaedra")) primary = kDaedra;
        else if (were) primary = kWerebeast;
        else if (undeadKw) primary = kUndead;
        else if (raceHas("ActorTypeDwarven")) primary = kConstruct;
        else if (trollKw) primary = kTroll;
        else if (giantKw || raceHas("reqRaceIsGiantRace")) primary = kGiant;
        else if (goblinoid || npcKw) primary = kHumanoid;
        else if (arthropodName) primary = kArthropod;
        else if (animalKw || (creature && animalTag)) primary = kAnimal;
        else if (creature) primary = kMonster;
        else return 0;  // none: no type keyword

        // Overlays
        std::uint32_t mask = primary;
        if (undeadKw) mask |= kUndead;
        if (npcKw || (raceHas("Vampire") && !raceHas("IsBeastRace"))) mask |= kHumanoid;
        if (giantKw) mask |= kGiant;
        if (trollKw) mask |= kTroll;
        if (arthropodName) mask |= kArthropod;
        if (animalKw && (primary == kHumanoid || primary == kTroll)) mask |= kAnimal;

        // Facets
        const auto spectralName = [](std::string_view t) { return AnyWord(t, { "ghost", "wraith", "wisp", "shade", "spectr" }); };
        if (raceHas("ActorTypeGhost") || actorHas("ActorTypeGhost") || actorHas("ActorTypeSpirit") ||
            ((mask & kUndead) != 0 && (spectralName(ed) || spectralName(raceName)))) {
            mask |= kSpectral;
        }
        if (primary == kDaedra || primary == kMonster) {
            const auto beingName = [](std::string_view t) { return AnyWord(t, { "atronach", "wraith", "wyrm", "imp" }); };
            const bool namedBeing = beingName(raceName);
            const bool being = primary == kDaedra || raceHas("REQ_ActorTypeDaedraNoFlesh") || beingName(ed) || namedBeing;
            if (being) {
                const auto says = [&](std::initializer_list<std::string_view> words) {
                    return AnyWord(ed, words) || (namedBeing && AnyWord(raceName, words));
                };
                if (says({ "flame", "fire", "ember", "incinerat" })) mask |= kFire;
                if (says({ "frost", "ice", "snowray", "goron" })) mask |= kFrost;
                if (says({ "storm", "shock", "thunder", "lightning" })) mask |= kShock;
            }
        }
        return mask;
    }
}
