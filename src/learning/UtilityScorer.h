#pragma once

#include "ScorerConfig.h"
#include "context/VitalEnvelope.h"
#include "ScoredCandidate.h"
#include "PriorCalculator.h"
#include "CorrelationBooster.h"
#include "PotionDiscriminator.h"
#include "WildcardManager.h"
#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"  // Contains TargetCollection
#include "state/WorldState.h"        // For ContextRuleEngine (Stage 1f)
#include "context/ContextWeightConfig.h"    // For ContextRuleEngine config
#include "context/ContextRuleEngine.h"    // For ContextRuleEngine (Stage 1f)
#include "FeatureBanditLearner.h"
#include "UsageMemory.h"
#include "StateFeatures.h"
#include "candidate/CandidateTypes.h"
#include "legacy/LegacyLearnFeature.h"
#include "legacy/LegacyRun.h"

#include <vector>

namespace Huginn::Scoring
{
    // =============================================================================
    // UTILITY SCORER -- the old scorer's host (R8.4: on the D2 allowlist)
    // =============================================================================
    // Two jobs since the cutover (R8.3), neither of them ranking on utility:
    //
    // 1. THE LEGACY PASS (kept for D2, learning/legacy/LegacyLearnFeature.h):
    //      utility = contextWeight × (1 + λ(confidence) × learningScore)
    //                × correlationBonus × potionMultiplier × favoritesMultiplier
    //    with λ(confidence) = lambdaMin + confidence × (lambdaMax - lambdaMin),
    //    computed only so the old floors (fMinimumContextWeight, fMinimumUtility,
    //    the cold start) decide which rows get the choice score's
    //    ln learn mult = ln(1 + λ·learningScore), and for the v2 / v3 logs.
    //    favoritesMultiplier is the uniform favoritesBoostMax.
    //
    // 2. LIVE DUTIES OF THE NEW ENGINE: the rank sort and the wildcards
    //    (RankAndApplyWildcards, on the choice score; R10 replaces the
    //    wildcards), the magicka / stamina envelopes the needs read, and the
    //    [Recs] lines.
    //
    // Retirement of (1): a refit on 0.23.53+ logs where a variant without the
    // old scorer passes R6's gate, or the user's call (roadmap, R8).
    // =============================================================================

    class UtilityScorer
    {
    public:
        UtilityScorer(Learning::FeatureBanditLearner& featureLearner, Learning::UsageMemory& usageMemory, const ScorerConfig& config = DefaultScorerConfig);
        ~UtilityScorer() = default;

        // R8.3: the LEGACY PASS only -- what the old engine computes, as the
        // D2 feature's input (learning/legacy/LegacyLearnFeature.h): the
        // context rules once, the floor loop and the cold-start rescue, and
        // the rows the floors keep (utility, breakdown; unsorted, no
        // wildcards). Nothing ranks on it: PipelineCoordinator ranks every
        // candidate on the choice score and calls RankAndApplyWildcards.
        // @param combatOnsetAgo R8.1: seconds since the current fight began,
        //        kNever out of combat (core/CombatOnset.h), for the
        //        PotionDiscriminator: the combat_onset clock.
        // R8.4: returns the whole pass as a Legacy::Run (learning/legacy/
        // LegacyRun.h): the kept rows, one Legacy::Row per candidate
        // (index-aligned) with the row each candidate reads resolved, and the
        // context weight map this pass scored against (the v3 log's old parts
        // read it).
        [[nodiscard]] Legacy::Run LegacyPass(
            const std::vector<Candidate::CandidateVariant>& candidates,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const State::WorldState& world,
            float combatOnsetAgo);

        // R8.3: the whole list in rank order on SlotScore()'s score (stable;
        // R7 -- the slot fill reads past the top N), then the wildcards.
        // @param displayPage The page THIS tick allocates, from the pipeline
        //        snapshot (PipelineContext::displayPageIndex / displaySlotCount
        //        / displayWildcardSlots): wildcard eligibility is page-relative,
        //        so it comes from the tick's snapshot (critique #10).
        void RankAndApplyWildcards(ScoredCandidateList& scored, const WildcardPage& displayPage);

        // Configuration access
        [[nodiscard]] const ScorerConfig& GetConfig() const noexcept { return m_config; }
        void SetConfig(const ScorerConfig& config) { m_config = config; }

        /// Replace the ContextRuleEngine's config snapshot (e.g., after INI hot-reload).
        void SetContextWeightConfig(const State::ContextWeightConfig& config);

        // Component access (for advanced configuration)
        [[nodiscard]] WildcardManager& GetWildcardManager() noexcept { return m_wildcardMgr; }

        // R8.1 (0.23.55): OnCombatStart / OnCombatEnd are gone with the
        // PotionDiscriminator's own timer; ScoreCandidates takes the
        // combat_onset clock instead.
        // @return true if a displayed exploration wildcard expired this tick
        //         (caller must force a pipeline run so the slot content swaps)
        [[nodiscard]] bool Update(float deltaSeconds);

        // Reset state (e.g., on save load)
        void Reset();

        // R3, read-only: magicka / stamina as the envelopes hold them (what the
        // magicka_deficit / stamina_deficit needs read, needs.csv). No state
        // change; the envelopes tick in Update as before.
        [[nodiscard]] float HeldMagicka(float raw) const noexcept { return m_magickaEnvelope.Value(raw); }
        [[nodiscard]] float HeldStamina(float raw) const noexcept { return m_staminaEnvelope.Value(raw); }

        // Recommendation logging (release-available; see DebugSettings::recLogVerbosity).
        // detail: append the legacy feature's inputs (ctx, λ, learn and its
        // Q/P/UCB/α) to each line.
        // force: bypass the membership-change dedup (on-demand `hg recs` dumps).
        // fallback (R8.3): the list ranks on the old utility (no bootstrap).
        void LogTopCandidates(const ScoredCandidateList& ranked, size_t count = 5,
            bool detail = false, bool force = false, bool fallback = false) const;

        /// 0.23.53, LOGGING ONLY (selection log v3): what ScoreCandidates
        /// computes for one candidate, with no floor applied -- the same
        /// ScoreCandidateInternal the scoring loop calls, at the context
        /// weight, learner metrics and recency the caller passes (the uniform
        /// favourites multiplier, as the floor pass uses). Reads only: nothing
        /// scored is changed and nothing is kept.
        [[nodiscard]] ScoredCandidate ExplainCandidate(
            const Candidate::CandidateVariant& candidate,
            const State::GameState& state,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const State::WorldState& world,
            float combatOnsetAgo,
            const Context::ContextWeightMap& weights,
            float contextWeight,
            const Learning::FeatureItemMetrics& metrics,
            float recencyBoost)
        {
            return ScoreCandidateInternal(candidate, state, player, targets, world, combatOnsetAgo, weights,
                contextWeight, metrics, recencyBoost);
        }

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
            float combatOnsetAgo,
            const Context::ContextWeightMap& weights,
            float contextWeight,
            const Learning::FeatureItemMetrics& metrics,
            float recencyBoost);

        // The favourites multiplier for a candidate: favoritesBoostMax for a
        // favourite in Boost mode, else 1 (ScorerConfig::FavoritesMultiplier).
        [[nodiscard]] float GetFavoritesMultiplier(const Candidate::CandidateVariant& candidate) const;

        // Check if candidate is favorited
        [[nodiscard]] bool IsCandidateFavorited(const Candidate::CandidateVariant& candidate) const;

        // Stage 1f: context→candidate weight mapping moved to
        // Context::WeightForCandidate (context/ContextWeightForCandidate.h) — see
        // architecture-critique #10. UtilityScorer just calls it.

        // Stage 2b: Compute confidence-adaptive lambda for multiplicative formula
        [[nodiscard]] float ComputeAdaptiveLambda(float confidence) const;

        // Final utility formula — the single source of truth, used by both the
        // normal scoring path (ScoreCandidateInternal Step 8) and the cold-start
        // fallback in ScoreCandidates. Reads all factors from the breakdown:
        //   utility = ctx × (1 + λ(confidence)×learn) × corr × potion × fav
        [[nodiscard]] float ComputeUtility(const ScoreBreakdown& breakdown) const;

        // Components
        Learning::FeatureBanditLearner& m_featureLearner;
        Learning::UsageMemory& m_usageMemory;
        ScorerConfig m_config;
        PriorCalculator m_priorCalc;
        CorrelationBooster m_correlationBooster;
        PotionDiscriminator m_potionDiscrim;
        WildcardManager m_wildcardMgr;

        // Live-path hold on magicka/stamina for context weights (ScoreCandidates
        // only). Update thread, like the rest of scoring.
        Context::VitalEnvelope m_magickaEnvelope;
        Context::VitalEnvelope m_staminaEnvelope;
        Context::ContextRuleEngine m_contextEngine;  // Stage 1f: New component
    };

}  // namespace Huginn::Scoring
