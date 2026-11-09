#pragma once

// =============================================================================
// CONTEXT FACTS (R8.4) -- what the live filters and the skip gate read off the
// state, shared with the legacy context rules
// =============================================================================
// Split out of the context rules' header in R8.4 (0.24.1), so the live code
// (CandidateFilters' hard gates through HardGateInputs, the pipeline's skip
// gate) reads these facts without including the legacy context rules, which
// R8 keeps for D2 only (tools/prune/r8_check.py). The rules include this file
// and read the same functions, so the gates and the old context weight's zero
// arms see the same facts (no_follower's "for someone else" excepted: since
// R11 the filter reads the catalog facet, the weight the classifier tag;
// core/HardGates.h).
// =============================================================================

#include "apparel/ApparelData.h"   // Apparel::CraftSkill (#65)
#include "core/HardGates.h"        // R8.1: the hard gates' facts
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"

#include <cstdint>

namespace Huginn::Context
{
    // =============================================================================
    // WORKSTATION -> CRAFT SKILL (#63/#65)
    // =============================================================================
    // The single case list mapping RE::TESFurniture::WorkBenchData::BenchType to
    // the craft a bench fortifies. CraftSkill::None means "this furniture has a
    // bench type, but not one that produces a fortify weight" — a modded bench
    // outside the vanilla enum, or one of the cooking/other types.
    //
    // Two readers, deliberately sharing one list: the legacy context rules pick which
    // fortify weight to raise, and PipelineCoordinator decides whether standing
    // at this bench is worth defeating the hash skip for. Since R8.1 the list
    // itself is Core::Gate::CraftOfBench (core/HardGates.h), which the
    // no_workstation filter reads too. When those drifted, the
    // coordinator kept the full pipeline running at 100 ms for benches that
    // raised no weight at all.
    //
    // R8.4 (0.24.1): moved here, inline, from the context rules' source: the
    // pipeline's skip gate is a live reader, not only the legacy rules.
    [[nodiscard]] inline Apparel::CraftSkill CraftSkillForWorkstation(int32_t benchType) noexcept
    {
        // The case list is Core::Gate::CraftOfBench (R8.1), shared with the
        // no_workstation filter; this only converts the enum.
        switch (Core::Gate::CraftOfBench(benchType)) {
        case Core::Gate::Craft::Smithing: return Apparel::CraftSkill::Smithing;
        case Core::Gate::Craft::Enchanting: return Apparel::CraftSkill::Enchanting;
        case Core::Gate::Craft::Alchemy: return Apparel::CraftSkill::Alchemy;
        case Core::Gate::Craft::None:
        default: return Apparel::CraftSkill::None;
        }
    }

    /// True when darknessWeight fires: dark, and the player has no way to see
    /// yet -- no Night Eye, no light spell, no torch out. The legacy context
    /// rules and the torch gate (not_dark, R8.1, through HardGateInputs) both
    /// read this, so they cannot disagree.
    [[nodiscard]] inline bool DarknessApplies(
        const State::PlayerActorState& player, const State::WorldState& world) noexcept
    {
        return Core::Gate::DarknessApplies(world.isDark, player.buffs.hasNightEye,
                                           player.buffs.hasLightSpell, player.hasTorchEquipped);
    }

    /// A living follower near: what followerPresent says, and so what gates a
    /// spell for someone else (the legacy context rules and no_follower, R8.1).
    [[nodiscard]] inline bool FollowerNear(const State::TargetCollection& targets) noexcept
    {
        return targets.GetFollowerCount() > 0;   // a living follower near
    }

    /// The craft whose fortify weight is up: the bench in the crosshair
    /// (the legacy context rules and no_workstation, R8.1).
    [[nodiscard]] inline Core::Gate::Craft StationCraft(const State::WorldState& world) noexcept
    {
        return Core::Gate::StationCraft(world.isLookingAtWorkstation, world.workstationType);
    }

    /// R8.1 (0.23.55): the facts the hard gates in CandidateFilters read, from
    /// the same state and the same three functions the legacy context rules
    /// read (how far a gate and the old weight's zero agree: core/HardGates.h).
    /// Never from a context weight map: the filters run before scoring.
    [[nodiscard]] inline Core::Gate::Inputs HardGateInputs(const State::PlayerActorState& player,
        const State::TargetCollection& targets, const State::WorldState& world, bool suppressFavorites) noexcept
    {
        return Core::Gate::Inputs{
            .followerNear = FollowerNear(targets),
            .station = StationCraft(world),
            .darknessApplies = DarknessApplies(player, world),
            .suppressFavorites = suppressFavorites,
        };
    }

    /// Survival need as a tier: 0 none, 1 half weight, 2 full weight.
    /// Hunger: Hungry (3) is half, Famished (4) and worse full. Cold: Very Cold
    /// (3) half, Freezing (4) and worse full. Read by the legacy context rules
    /// (D2) only since R8.3; the skip gate reads the need signature.
    ///
    /// NOT silenced by a warming effect, though it was at first: a warm meal
    /// drops the cold level itself within half a second, and LoreRim's
    /// Warming Aura leaves a "Fortify Warmth" ability up for as long as it
    /// runs -- the player froze from level 3 to 4 with it on and no prompt
    /// (2026-09-29). The cold level is the signal. A running warming effect
    /// only stops the warming SPELL being offered again (CandidateFilters).
    [[nodiscard]] inline int HungerTier(const State::PlayerActorState& player) noexcept
    {
        if (!player.survivalModeActive) return 0;
        if (player.hungerLevel >= State::SurvivalThreshold::HUNGER_FAMISHED) return 2;
        if (player.hungerLevel >= State::SurvivalThreshold::HUNGER_HUNGRY) return 1;
        return 0;
    }

    [[nodiscard]] inline int ColdTier(const State::PlayerActorState& player) noexcept
    {
        if (!player.survivalModeActive) return 0;
        if (player.coldLevel >= State::SurvivalThreshold::COLD_FREEZING) return 2;
        if (player.coldLevel >= State::SurvivalThreshold::COLD_VERY_COLD) return 1;
        return 0;
    }
}
