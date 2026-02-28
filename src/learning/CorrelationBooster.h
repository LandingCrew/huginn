#pragma once

#include "ScorerConfig.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"  // Contains TargetCollection
#include "candidate/CandidateTypes.h"

namespace Huginn::Scoring
{
    // =============================================================================
    // CORRELATION BOOSTER
    // =============================================================================
    // Calculates multiplicative bonuses for equipment synergies and correlations.
    //
    // Examples:
    //   - Bow equipped → boost arrows (×3.0)
    //   - Silver weapon + undead target → boost weapon (×3.0)
    //   - Melee + no shield → boost defensive items (×2.5)
    //   - Multiple bonuses compound multiplicatively: bow+arrow+fortify = ×9.0
    //
    // These correlations capture synergies that aren't captured by individual
    // context weights or priors.
    // =============================================================================

    class CorrelationBooster
    {
    public:
        explicit CorrelationBooster(const ScorerConfig& config);
        ~CorrelationBooster() = default;

        // Calculate correlation bonus for a candidate based on current equipment
        [[nodiscard]] float CalculateBonus(
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const Candidate::CandidateVariant& candidate) const;

    private:
        // Type-specific correlation calculations
        [[nodiscard]] float CalculateSpellCorrelation(
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const Candidate::SpellCandidate& spell) const;

        [[nodiscard]] float CalculateItemCorrelation(
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const Candidate::ItemCandidate& item) const;

        [[nodiscard]] float CalculateWeaponCorrelation(
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const Candidate::WeaponCandidate& weapon) const;

        [[nodiscard]] float CalculateAmmoCorrelation(
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const Candidate::AmmoCandidate& ammo) const;

        [[nodiscard]] float CalculateScrollCorrelation(
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const Candidate::ScrollCandidate& scroll) const;

        // Helper: Check if target is undead
        [[nodiscard]] bool HasUndeadTarget(const State::TargetCollection& targets) const;

        const ScorerConfig& m_config;
    };

}  // namespace Huginn::Scoring
