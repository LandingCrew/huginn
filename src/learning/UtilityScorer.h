#pragma once

#include "ScorerConfig.h"
#include "ScoredCandidate.h"
#include "PriorCalculator.h"
#include "CorrelationBooster.h"
#include "PotionDiscriminator.h"
#include "WildcardManager.h"
#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"  // Contains TargetCollection
#include "state/WorldState.h"        // For ContextRuleEngine (Stage 1f)
#include "state/ContextWeightConfig.h"    // For ContextRuleEngine config
#include "context/ContextRuleEngine.h"    // For ContextRuleEngine (Stage 1f)
#include "FeatureQLearner.h"
#include "UsageMemory.h"
#include "StateFeatures.h"
#include "candidate/CandidateTypes.h"

#include <vector>

namespace Huginn::Scoring
{
    // =============================================================================
    // UTILITY SCORER
    // =============================================================================
    // Unified scoring system for all candidate types (spells, potions, weapons, etc.)
    //
    // Scoring Formula (version-dependent):
    //
    // LEGACY (v0.12.x):
    //   utility = (contextWeight + λ * learningScore + correlationBonus)
    //             * potionMultiplier * favoritesMultiplier
    //
    // MULTIPLICATIVE (v1.0):
    //   utility = contextWeight × (1 + λ(confidence) × learningScore)
    //             × correlationBonus × potionMultiplier × favoritesMultiplier
    //
    // Where λ(confidence) = lambdaMin + confidence × (lambdaMax - lambdaMin)
    //
    // Where:
    //   - contextWeight: From ContextRuleEngine (how relevant now) [0,1]
    //   - learningScore: α*Q + (1-α)*prior + β*UCB (learned + heuristic + exploration)
    //   - correlationBonus: Equipment synergy bonuses (multiplicative in v1.0)
    //   - potionMultiplier: Combat timing and value discrimination
    //   - favoritesMultiplier: Boost/suppress favorited items
    //
    // The scorer:
    //   1. Takes raw candidates from CandidateGenerator
    //   2. Computes utility for each candidate
    //   3. Partial-sorts to get top N efficiently
    //   4. Applies wildcards for exploration
    //   5. Returns ranked ScoredCandidateList
    // =============================================================================

    class UtilityScorer
    {
    public:
        UtilityScorer(Learning::FeatureQLearner& featureLearner, Learning::UsageMemory& usageMemory, const ScorerConfig& config = DefaultScorerConfig);
        ~UtilityScorer() = default;

        // Main scoring method: Score all candidates and return ranked list
        // Stage 1f: Added WorldState parameter for ContextRuleEngine
        [[nodiscard]] ScoredCandidateList ScoreCandidates(
            const std::vector<Candidate::CandidateVariant>& candidates,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const State::WorldState& world);

        // Score a single candidate (useful for debugging)
        // Stage 1f: Added WorldState parameter for ContextRuleEngine
        [[nodiscard]] ScoredCandidate ScoreCandidate(
            const Candidate::CandidateVariant& candidate,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const State::WorldState& world);

        // Configuration access
        [[nodiscard]] const ScorerConfig& GetConfig() const noexcept { return m_config; }
        void SetConfig(const ScorerConfig& config) { m_config = config; }

        /// Replace the ContextRuleEngine's config snapshot (e.g., after INI hot-reload).
        void SetContextWeightConfig(const State::ContextWeightConfig& config);

        // Component access (for advanced configuration)
        [[nodiscard]] WildcardManager& GetWildcardManager() noexcept { return m_wildcardMgr; }
        [[nodiscard]] PotionDiscriminator& GetPotionDiscriminator() noexcept { return m_potionDiscrim; }

        // Combat state tracking (call from main update loop)
        void OnCombatStart();
        void OnCombatEnd();
        void Update(float deltaSeconds);

        // Reset state (e.g., on save load)
        void Reset();

        // Debug logging
        void LogTopCandidates(const ScoredCandidateList& ranked, size_t count = 5) const;

    private:
        // Internal scoring implementation
        // Stage 1f: Added WorldState and ContextWeightMap parameters
        // Perf: contextWeight, metrics, recencyBoost pre-computed by caller to
        //       avoid redundant GetContextWeight calls and per-candidate locking.
        [[nodiscard]] ScoredCandidate ScoreCandidateInternal(
            const Candidate::CandidateVariant& candidate,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const State::WorldState& world,
            const Context::ContextWeightMap& weights,
            float contextWeight,
            const Learning::FeatureItemMetrics& metrics,
            float recencyBoost);

        // Get favorites multiplier for a candidate
        [[nodiscard]] float GetFavoritesMultiplier(
            const Candidate::CandidateVariant& candidate,
            size_t rank,
            size_t totalItems) const;

        // Check if candidate is favorited
        [[nodiscard]] bool IsCandidateFavorited(const Candidate::CandidateVariant& candidate) const;

        // Stage 1f: Extract relevant weight from ContextWeightMap for a candidate
        [[nodiscard]] float GetContextWeight(
            const Candidate::CandidateVariant& candidate,
            const Context::ContextWeightMap& weights) const;

        // Stage 2b: Compute confidence-adaptive lambda for multiplicative formula
        [[nodiscard]] float ComputeAdaptiveLambda(float confidence) const;

        // Components
        Learning::FeatureQLearner& m_featureLearner;
        Learning::UsageMemory& m_usageMemory;
        ScorerConfig m_config;
        PriorCalculator m_priorCalc;
        CorrelationBooster m_correlationBooster;
        PotionDiscriminator m_potionDiscrim;
        WildcardManager m_wildcardMgr;
        Context::ContextRuleEngine m_contextEngine;  // Stage 1f: New component

        // Cached state
        bool m_wasInCombat = false;
    };

}  // namespace Huginn::Scoring
