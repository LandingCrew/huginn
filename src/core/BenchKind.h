#pragma once

// =============================================================================
// BENCH KIND -- which crafting station a piece of furniture is (R3 needs)
// =============================================================================
// The engine's bench type (the furniture's WBDT) does not tell a forge from a
// cooking spit: the forge, the cooking pot and spit, the smelter and the
// tanning rack are all "create object" benches. The crafting menu tells them
// apart by the furniture's workbench keyword, which each recipe (COBJ) names
// (CraftingSmithingForge, CraftingCookpot, CraftingSmelter,
// CraftingTanningRack, BYOHCraftingOven ...), and so does this, for the need
// vector only (needs/NeedSnapshotBuilder.cpp, 0.23.16: workstation_smithing
// fired at a cooking spit in the LoreRim R3 session).
//
// The old engine's CraftSkillForWorkstation (context/ContextRuleEngine.cpp)
// still reads every create-object bench as Smithing; it feeds scoring
// (fortifySmithingWeight) and is frozen until R8 replaces it.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/NeedSnapshot.h"

#include <cstdint>
#include <string_view>

namespace Huginn::Core::Needs
{
    enum class BenchKind : std::uint8_t
    {
        None,        // not a bench (or nothing in the crosshair)
        Smithing,    // forge, skyforge, grindstone, armour workbench
        Enchanting,
        Alchemy,
        Cooking,     // cooking pot, spit, oven
        Smelting,
        Tanning,
        Other        // a create-object bench no keyword names (a mod's station)
    };

    [[nodiscard]] constexpr std::string_view BenchKindName(BenchKind k) noexcept
    {
        switch (k) {
            case BenchKind::None: return "none";
            case BenchKind::Smithing: return "smithing";
            case BenchKind::Enchanting: return "enchanting";
            case BenchKind::Alchemy: return "alchemy";
            case BenchKind::Cooking: return "cooking";
            case BenchKind::Smelting: return "smelting";
            case BenchKind::Tanning: return "tanning";
            case BenchKind::Other: return "other";
        }
        return "";
    }

    // The engine's WBDT bench types (TESFurniture WorkBenchData BenchType in CommonLib).
    inline constexpr int kBenchNone = 0;
    inline constexpr int kBenchCreateObject = 1;
    inline constexpr int kBenchSmithingWeapon = 2;
    inline constexpr int kBenchEnchanting = 3;
    inline constexpr int kBenchEnchantingExperiment = 4;
    inline constexpr int kBenchAlchemy = 5;
    inline constexpr int kBenchAlchemyExperiment = 6;
    inline constexpr int kBenchSmithingArmor = 7;

    namespace Detail
    {
        [[nodiscard]] constexpr char LowerAscii(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        [[nodiscard]] constexpr bool ContainsNoCase(std::string_view hay, std::string_view needle) noexcept
        {
            if (needle.size() > hay.size()) return false;
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                bool same = true;
                for (std::size_t j = 0; j < needle.size() && same; ++j) {
                    same = LowerAscii(hay[i + j]) == LowerAscii(needle[j]);
                }
                if (same) return true;
            }
            return false;
        }
    }

    /// The station a furniture is, from its bench type and the editor IDs of
    /// its keywords (any range of things convertible to std::string_view).
    /// The enchanting, alchemy, grindstone and armour bench types say it on
    /// their own; a create-object bench is read by keyword: a smithing
    /// keyword first (a forge is a forge whatever else it carries), then
    /// cooking (cook, oven, spit), smelter, tanning; none of them: Other.
    template <class Keywords>
    [[nodiscard]] BenchKind ClassifyBench(int benchType, const Keywords& keywords) noexcept
    {
        switch (benchType) {
            case kBenchNone: return BenchKind::None;
            case kBenchEnchanting:
            case kBenchEnchantingExperiment: return BenchKind::Enchanting;
            case kBenchAlchemy:
            case kBenchAlchemyExperiment: return BenchKind::Alchemy;
            case kBenchSmithingWeapon:
            case kBenchSmithingArmor: return BenchKind::Smithing;
            case kBenchCreateObject: break;
            default: return BenchKind::Other;
        }
        const auto any = [&](std::string_view needle) {
            for (const auto& k : keywords) {
                if (Detail::ContainsNoCase(std::string_view(k), needle)) return true;
            }
            return false;
        };
        if (any("CraftingSmithing") || any("Forge")) return BenchKind::Smithing;
        if (any("Cook") || any("Oven") || any("Spit")) return BenchKind::Cooking;
        if (any("Smelter")) return BenchKind::Smelting;
        if (any("Tanning")) return BenchKind::Tanning;
        return BenchKind::Other;
    }

    /// The NeedSnapshot `workstation` a bench sets: the three craft needs
    /// (workstation_smithing / _enchanting / _alchemy). Cooking, smelting,
    /// tanning and unknown stations set none: needs.csv has no need for them
    /// (roadmap, "Needs and effects to add").
    [[nodiscard]] constexpr int NeedWorkstation(BenchKind k) noexcept
    {
        switch (k) {
            case BenchKind::Smithing: return static_cast<int>(Workstation::Smithing);
            case BenchKind::Enchanting: return static_cast<int>(Workstation::Enchanting);
            case BenchKind::Alchemy: return static_cast<int>(Workstation::Alchemy);
            default: return static_cast<int>(Workstation::None);
        }
    }
}
