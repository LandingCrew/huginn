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
// For no_workstation, not_dark and favorite_suppressed the filter drops
// exactly the items the old context weight zeroed (the INI weights aside: a
// weight configured to 0 still zeroes in the old engine without the filter
// dropping the item; the shipped weights are all > 0). For no_follower the
// two sides share the predicate and the follower fact but not the "for
// someone else" input: since R11 the filter reads the effect catalog's
// TargetsOthers facet (core/CapFacets.h), the old weight the classifiers'
// TargetsOthers tag. The two differ on some spells, and the facet is empty
// while the catalog is not Ready, so some rows the old weight zeroes pass
// the filter (there the zero decides only whether the D2 floors keep them).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/BenchKind.h"
#include "core/LockReach.h"

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
        bool allyInCrosshair = false;    // 0.24.31: a living ally in the crosshair, or within kAllyAimHoldSec (no_ally_aimed)
        bool allyDown = false;           // 0.24.31: a teammate down on one knee (ally_injured's step)
        Craft station = Craft::None;     // StationCraft of the crosshair bench
        bool darknessApplies = false;    // DarknessApplies
        bool suppressFavorites = false;  // [Favorites] sFavoritesMode = Suppress
        int lockLevel = Lock::kNoLock;   // 0.24.29: the crosshair lock's level (core/LockReach.h), kNoLock for none
    };

    /// no_follower: a spell or scroll for someone else, with no one near to take it.
    [[nodiscard]] constexpr bool NoFollower(bool targetsOthers, bool followerNear) noexcept
    {
        return targetsOthers && !followerNear;
    }

    /// no_ally_aimed (0.24.31, the live filter only; the old engine keeps
    /// no_follower alone): a spell or scroll for someone else while no ally is
    /// in the crosshair. Its "on Self" twin is the buff then (the user,
    /// 2026-11-13: "on self is the preferred buff iff we are not looking at an
    /// ally"); one for someone else comes up when the player looks at a
    /// follower or their own summon or thrall, as they must to cast it.
    ///
    /// Two things keep it from flickering in a fight, where the player moves
    /// and the crosshair crosses an ally for a moment at a time (the user,
    /// 2026-11-14: "the temporal window is kinda tight"; a combat key also
    /// wants its challenger for 1.5 s without a break):
    ///  - an ally stays "looked at" for kAllyAimHoldSec after the crosshair
    ///    leaves them (AllyAimHeld);
    ///  - a heal (restore health) for someone else passes while a teammate is
    ///    down (`allyDown`), crosshair or not: the downed ally is who it is
    ///    for. A resist or another buff for someone else still waits for the
    ///    look.
    [[nodiscard]] constexpr bool NotAtAlly(bool targetsOthers, bool allyInCrosshair, bool healsHealth = false,
                                           bool allyDown = false) noexcept
    {
        return targetsOthers && !allyInCrosshair && !(healsHealth && allyDown);
    }

    /// How long an ally stays "looked at" after the crosshair (with its 0.3 s
    /// sticky target) last found them.
    inline constexpr double kAllyAimHoldSec = 4.0;

    /// The ally was in the crosshair within kAllyAimHoldSec of `nowSec`
    /// (steady-clock seconds; `lastSeenSec` < 0 = never).
    [[nodiscard]] constexpr bool AllyAimHeld(double lastSeenSec, double nowSec) noexcept
    {
        return lastSeenSec >= 0.0 && nowSec >= lastSeenSec && nowSec - lastSeenSec <= kAllyAimHoldSec;
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
