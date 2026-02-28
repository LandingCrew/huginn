#pragma once

#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "candidate/CandidateTypes.h"

namespace Huginn::Scoring
{
    // =============================================================================
    // PRIOR CALCULATOR
    // =============================================================================
    // Provides INTRINSIC quality heuristics for all candidate types.
    // These priors bootstrap the Q-learning system before sufficient data is collected.
    //
    // IMPORTANT: Priors are NOT context-aware. Context is handled by ContextRuleEngine.
    // Priors evaluate INTRINSIC item properties only:
    //   - Magnitude (bigger potion = better quality)
    //   - Cost (expert spell = higher quality than novice)
    //   - Scarcity (low inventory count = slight penalty)
    //   - Charge state (depleted enchantment = lower quality)
    //
    // Returns values in [0.0, 1.0] representing intrinsic item quality.
    // =============================================================================

    class PriorCalculator
    {
    public:
        PriorCalculator() = default;
        ~PriorCalculator() = default;

        // Calculate heuristic prior score for any candidate type
        // Uses std::visit to dispatch to type-specific calculation
        [[nodiscard]] float CalculatePrior(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::CandidateVariant& candidate) const;

    private:
        // Type-specific prior calculations
        [[nodiscard]] float CalculateSpellPrior(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::SpellCandidate& spell) const;

        [[nodiscard]] float CalculateItemPrior(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::ItemCandidate& item) const;

        [[nodiscard]] float CalculateWeaponPrior(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::WeaponCandidate& weapon) const;

        [[nodiscard]] float CalculateAmmoPrior(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::AmmoCandidate& ammo) const;

        [[nodiscard]] float CalculateScrollPrior(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::ScrollCandidate& scroll) const;

        // =============================================================================
        // INTRINSIC PROPERTY CONSTANTS
        // =============================================================================
        // These constants evaluate ONLY intrinsic item properties.
        // NO context dependencies (health, combat, target type, etc.)
        // Context is handled by ContextRuleEngine which produces context weights.
        // =============================================================================

        // Base prior for all candidates (starting point before intrinsic adjustments)
        static constexpr float BASE_PRIOR = 0.3f;

        // Magnitude scaling (logarithmic to prevent huge potions from dominating)
        // Example: 200 HP potion vs 50 HP potion → ~0.1 difference in prior
        static constexpr float MAGNITUDE_REFERENCE_VALUE = 100.0f;  // Major healing potion
        static constexpr float MAGNITUDE_SCALE_FACTOR = 0.15f;      // Max bonus from magnitude

        // Spell cost scaling (linear - higher cost generally means more powerful spell)
        // Example: Expert spell (cost 200) vs Novice spell (cost 20) → ~0.09 difference
        static constexpr float MAX_REASONABLE_SPELL_COST = 200.0f;  // Expert-level spells
        static constexpr float COST_SCALE_FACTOR = 0.1f;            // Max bonus from high cost

        // Inventory scarcity penalty (prefer not to deplete last few items)
        // Example: 2 potions left → -0.06 penalty vs 50 potions → no penalty
        static constexpr float LOW_COUNT_THRESHOLD = 5.0f;          // Below this, reduce priority
        static constexpr float COUNT_PENALTY_SCALE = 0.1f;          // Max penalty for nearly depleted

        // Weapon charge penalty (intrinsic weapon state, not game context)
        // Example: 5% charge → -0.19 penalty vs 100% charge → no penalty
        static constexpr float CHARGE_PENALTY_SCALE = 0.2f;         // Max penalty for depleted charge

        // Ammo scarcity threshold (prefer to surface when running low)
        // Example: 5 arrows left → +0.075 bonus vs 50 arrows → no bonus
        static constexpr float AMMO_LOW_COUNT_THRESHOLD = 20.0f;    // Below this, boost priority
        static constexpr float AMMO_SCARCITY_SCALE = 0.1f;          // Max bonus for nearly depleted

        // Ammo type compatibility bonus (intrinsic: does this ammo FIT the equipped weapon?)
        // Example: Arrows when bow equipped → +0.15 vs Bolts when bow equipped → no bonus
        static constexpr float AMMO_TYPE_MATCH_BONUS = 0.15f;
    };

}  // namespace Huginn::Scoring
