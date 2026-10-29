#pragma once

// =============================================================================
// LEGACY SCORER (R8.4) -- the old engine's per-candidate score, kept for D2
// =============================================================================
// *** Part of the legacy module R8 KEEPS for D2 (see LegacyLearnFeature.h). ***
//
// Split out of UtilityScorer in R8.4 so the live scorer (rank sort,
// wildcards, envelopes, [Recs]) holds no old-engine code and sits outside
// the D2 allowlist (tools/prune/r8_check.py). What is here computes
//   utility = contextWeight × (1 + λ(confidence) × learningScore)
//             × correlationBonus × potionMultiplier × favoritesMultiplier
// with λ(confidence) = lambdaMin + confidence × (lambdaMax - lambdaMin),
// only so the old floors (fMinimumContextWeight, fMinimumUtility, the cold
// start) decide which rows get the choice score's
// ln learn mult = ln(1 + λ·learningScore), and for the v2 / v3 logs.
// favoritesMultiplier is the uniform favoritesBoostMax. Nothing ranks on it.
//
// The pipeline reaches it through Legacy::LegacyPass (LegacyRun.h), the v3
// log through Legacy::OldScoreCapture; g_legacyScorer (Globals) owns it.
//
// Retirement: a refit on 0.23.53+ logs where a variant without the old
// scorer passes R6's gate, or the user's call (roadmap, R8).
// =============================================================================

#include "LegacyLearnFeature.h"
#include "LegacyRun.h"
#include "candidate/CandidateTypes.h"
#include "context/ContextRuleEngine.h"
#include "context/ContextWeightConfig.h"
#include "learning/CorrelationBooster.h"
#include "learning/FeatureBanditLearner.h"
#include "learning/PotionDiscriminator.h"
#include "learning/PriorCalculator.h"
#include "learning/ScoredCandidate.h"
#include "learning/ScorerConfig.h"
#include "learning/UsageMemory.h"
#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"

#include <vector>

namespace Huginn::Scoring::Legacy
{
    class LegacyScorer
    {
    public:
        LegacyScorer(Learning::FeatureBanditLearner& featureLearner, Learning::UsageMemory& usageMemory,
            const ScorerConfig& config = DefaultScorerConfig);
        // The correlation booster and the potion multiplier hold a reference
        // to m_config: not copyable or movable.
        LegacyScorer(const LegacyScorer&) = delete;
        LegacyScorer& operator=(const LegacyScorer&) = delete;

        /// The legacy pass (R8.3, moved from UtilityScorer::LegacyPass in
        /// R8.4 unchanged): the context rules once (on `heldMagicka` /
        /// `heldStamina`, the live scorer's envelopes), the floor loop and the
        /// cold-start rescue, and the rows the floors keep (utility, breakdown;
        /// unsorted, no wildcards), returned as a Run with the row each
        /// candidate reads resolved and the weights the pass used.
        /// @param combatOnsetAgo R8.1: seconds since the current fight began,
        ///        kNever out of combat (core/CombatOnset.h), for the
        ///        PotionDiscriminator.
        [[nodiscard]] Run Pass(
            const std::vector<Candidate::CandidateVariant>& candidates,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const State::WorldState& world,
            float combatOnsetAgo,
            float heldMagicka,
            float heldStamina);

        /// 0.23.53, LOGGING ONLY (selection log v3): what the pass computes for
        /// one candidate, with no floor applied -- the same
        /// ScoreCandidateInternal the floor loop calls, at the context weight,
        /// learner metrics and recency the caller passes. Reads only.
        [[nodiscard]] ScoredCandidate ExplainCandidate(
            const Candidate::CandidateVariant& candidate,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            float combatOnsetAgo,
            float contextWeight,
            const Learning::FeatureItemMetrics& metrics,
            float recencyBoost) const
        {
            return ScoreCandidateInternal(candidate, state, player, targets, combatOnsetAgo, contextWeight, metrics,
                recencyBoost);
        }

        [[nodiscard]] const ScorerConfig& GetConfig() const noexcept { return m_config; }
        void SetConfig(const ScorerConfig& config) { m_config = config; }
        /// Replace the context rules' config snapshot (INI hot-reload).
        void SetContextWeightConfig(const State::ContextWeightConfig& config);

    private:
        // One candidate's old score at a pre-computed context weight, learner
        // metrics and recency (the floor loop computes those once per pass).
        [[nodiscard]] ScoredCandidate ScoreCandidateInternal(
            const Candidate::CandidateVariant& candidate,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            float combatOnsetAgo,
            float contextWeight,
            const Learning::FeatureItemMetrics& metrics,
            float recencyBoost) const;

        // The favourites multiplier: favoritesBoostMax for a favourite in
        // Boost mode, else 1 (ScorerConfig::FavoritesMultiplier); 0 for a
        // Suppress favourite, which the favorite_suppressed filter drops first.
        [[nodiscard]] float GetFavoritesMultiplier(const Candidate::CandidateVariant& candidate) const;

        // λ(confidence) = lambdaMin + confidence × (lambdaMax - lambdaMin)
        [[nodiscard]] float ComputeAdaptiveLambda(float confidence) const;

        // utility = ctx × (1 + λ(confidence)×learn) × corr × potion × fav, the
        // single formula both the floor loop and the cold start use.
        [[nodiscard]] float ComputeUtility(const ScoreBreakdown& breakdown) const;

        Learning::FeatureBanditLearner& m_featureLearner;
        Learning::UsageMemory& m_usageMemory;
        ScorerConfig m_config;                       // before the two members that reference it
        PriorCalculator m_priorCalc;
        CorrelationBooster m_correlationBooster;
        PotionDiscriminator m_potionDiscrim;
        Context::ContextRuleEngine m_contextEngine;
    };
}
