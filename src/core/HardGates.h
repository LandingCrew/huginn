#pragma once

// =============================================================================
// HARD GATES (R8.1, 0.23.55) -- the old engine's context zeros, as filters
// =============================================================================
// Map Phase 6 item 1: before the new scorer ranks anything, the four cases in
// which the old engine gave a candidate a context weight of exactly 0 become
// candidate filters (CandidateFilters, Core::Filter::Reason no_follower,
// no_workstation, not_dark, favorite_suppressed). The new scorer never sees
// those items, so it cannot fill a page with fortify rings and torches.
//
// Each predicate is ONE function here, read by both sides:
//   - the filter (candidate/CandidateFilters.cpp), on facts read from the
//     game's state by Context::HardGateInputs (context/ContextFacts.h);
//   - the old engine's context code: the legacy context rules set
//     followerPresent, the fortify<Craft> weights and darknessWeight from the
//     same three facts (FollowerNear, StationCraft, DarknessApplies), and
//     the old context weight's zero arms read those.
// So the filter drops exactly the items the old context weight zeroed (the
// INI weights aside: a weight configured to 0 still zeroes in the old engine
// without the filter dropping the item; the shipped weights are all > 0).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/BenchKind.h"

#include <cstdint>

namespace Huginn::Core::Gate
{
    /// A craft a station serves and a fortify piece can raise.
    enum class Craft : std::uint8_t { None, Smithing, Enchanting, Alchemy };

    /// The crafts a piece of apparel fortifies (any number: a player-made ring
    /// can fortify two).
    struct Crafts
    {
        bool smithing = false;
        bool enchanting = false;
        bool alchemy = false;

        [[nodiscard]] constexpr bool Has(Craft c) const noexcept
        {
            switch (c) {
                case Craft::Smithing: return smithing;
                case Craft::Enchanting: return enchanting;
                case Craft::Alchemy: return alchemy;
                case Craft::None: return false;
            }
            return false;
        }
    };

    /// The craft of a bench type (the furniture's WBDT). The old engine's
    /// mapping, unchanged (it was the context rules'
    /// CraftSkillForWorkstation, which now reads this): every create-object
    /// bench counts as Smithing -- a cooking spit included. That is a known
    /// limit of the frozen engine (BenchKind.h); the filter keeps it so that it
    /// drops exactly what the old weight zeroed.
    [[nodiscard]] constexpr Craft CraftOfBench(int benchType) noexcept
    {
        using namespace Needs;
        switch (benchType) {
            case kBenchCreateObject:
            case kBenchSmithingWeapon:
            case kBenchSmithingArmor: return Craft::Smithing;
            case kBenchEnchanting:
            case kBenchEnchantingExperiment: return Craft::Enchanting;
            case kBenchAlchemy:
            case kBenchAlchemyExperiment: return Craft::Alchemy;
            default: return Craft::None;
        }
    }

    /// The station whose fortify weight is up: the bench in the crosshair, if any.
    [[nodiscard]] constexpr Craft StationCraft(bool lookingAtWorkstation, int benchType) noexcept
    {
        return lookingAtWorkstation ? CraftOfBench(benchType) : Craft::None;
    }

    /// Darkness that a torch (or Night Eye, or a light spell) answers: dark,
    /// and nothing to see by yet. The old darknessWeight's test, and so the
    /// torch gate's.
    [[nodiscard]] constexpr bool DarknessApplies(bool isDark, bool nightEye, bool lightSpell, bool torchOut) noexcept
    {
        return isDark && !nightEye && !lightSpell && !torchOut;
    }

    /// One light reading against the dark threshold, with the hysteresis of
    /// state/DarknessGate: dark below `enter`; once dark, until `enter + exitGap`.
    /// Strictly below: a reading AT the threshold is not dark.
    [[nodiscard]] constexpr bool IsDarkReading(float light, float enter, bool wasDark, float exitGap) noexcept
    {
        return light < (wasDark ? enter + exitGap : enter);
    }

    /// The facts the four gates read, taken from the game's state once per
    /// pipeline run (Context::HardGateInputs). Default: no follower, no
    /// station, not dark, favourites not suppressed.
    struct Inputs
    {
        bool followerNear = false;       // a living follower near (the old followerPresent)
        Craft station = Craft::None;     // StationCraft of the crosshair bench
        bool darknessApplies = false;    // DarknessApplies
        bool suppressFavorites = false;  // [Favorites] sFavoritesMode = Suppress
    };

    /// no_follower: a spell or scroll for someone else, with no one near to take it.
    [[nodiscard]] constexpr bool NoFollower(bool targetsOthers, bool followerNear) noexcept
    {
        return targetsOthers && !followerNear;
    }

    /// no_workstation: a fortify piece when no craft it fortifies has its station up.
    [[nodiscard]] constexpr bool NoWorkstation(Crafts fortifies, Craft station) noexcept
    {
        return !fortifies.Has(station);
    }

    /// not_dark: a torch while darkness does not apply (daylight, or a light already up).
    [[nodiscard]] constexpr bool NotDark(bool darknessApplies) noexcept
    {
        return !darknessApplies;
    }

    /// favorite_suppressed: a favourite under favourites mode Suppress.
    [[nodiscard]] constexpr bool FavoriteSuppressed(bool favorited, bool suppressFavorites) noexcept
    {
        return favorited && suppressFavorites;
    }
}
