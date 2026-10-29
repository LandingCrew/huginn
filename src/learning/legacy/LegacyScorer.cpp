#include "LegacyScorer.h"

#include "Globals.h"                              // g_legacyScorer; the live scorer's envelopes
#include "Profiling.h"
#include "context/ContextWeightForCandidate.h"   // Context::WeightForCandidate, IsHardContextGated
#include "context/ContextWeightSettings.h"       // BuildConfig() in the constructor
#include "learning/StateFeatures.h"
#include "util/ScopedTimer.h"

#include <array>
#include <chrono>

namespace Huginn::Scoring::Legacy
{
    // R8.4: moved from UtilityScorer.cpp (the legacy pass, ScoreCandidateInternal,
    // ComputeUtility, ComputeAdaptiveLambda, the favourites helper) unchanged in
    // arithmetic. The magicka / stamina envelopes stay with the live scorer (the
    // needs read them); the pass takes their held values.

    LegacyScorer::LegacyScorer(Learning::FeatureBanditLearner& featureLearner, Learning::UsageMemory& usageMemory,
        const ScorerConfig& config)
        : m_featureLearner(featureLearner)
        , m_usageMemory(usageMemory)
        , m_config(config)
        , m_correlationBooster(m_config)
        , m_potionDiscrim(m_config)
        , m_contextEngine(State::ContextWeightSettings::GetSingleton().BuildConfig())
    {
    }

    void LegacyScorer::SetContextWeightConfig(const State::ContextWeightConfig& config)
    {
        m_contextEngine.SetConfig(config);
    }

    Run LegacyScorer::Pass(
        const std::vector<Candidate::CandidateVariant>& candidates,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world,
        float combatOnsetAgo,
        float heldMagicka,
        float heldStamina)
    {
        SCOPED_TIMER("Legacy::LegacyScorer::Pass");

        Run run;
        run.ran = true;
        ScoredCandidateList& scored = run.kept;
        scored.reserve(candidates.size());

        // Magicka and stamina are scored at their recent low while they
        // regenerate (VitalEnvelope, held by the live scorer), on a COPY: the
        // rule engine stays pure, and nothing but the context weights sees the
        // held value. Health is not held; see VitalEnvelope.
        State::PlayerActorState scoringPlayer = player;
        scoringPlayer.vitals.magicka = heldMagicka;
        scoringPlayer.vitals.stamina = heldStamina;

        // The context rules ONCE for all candidates. Tracy zones split this pass
        // (and Pipeline::ScoreCandidates, its caller) so a trace can say which
        // part costs: see the 2026-10-03 entry in docs/profiling/tracy-traces.md.
        Context::ContextWeightMap weights{};
        {
            Huginn_ZONE_NAMED("Score::ContextRules");
            weights = m_contextEngine.EvaluateRules(scoringPlayer, targets, world);
        }

        // Hand the map back: the v3 log's old parts read the SAME weights the
        // floors tested (Legacy::OldScoreCapture).
        run.weights = weights;

        // The old learner's state features, once per pass.
        auto stateFeatures = Learning::StateFeatures::FromState(player, targets);
        auto phi = stateFeatures.ToArray();

        // The forget sweep runs from UpdateLoop (Legacy::MaybeForgetFaded), so it
        // keeps running while bLegacyLearnFeature is off and this pass does not.

        // Locked readers once for the whole floor loop (~200 lock ops -> 2).
        auto qReader = m_featureLearner.AcquireReader();
        auto usageReader = m_usageMemory.AcquireReader(state);

        // The floor loop and the cold-start rescue (LegacyLearnFeature.h,
        // R8.2), called with this scorer's own ScoreCandidateInternal. The
        // per-candidate floors it reports are the D2 feature's input.
        struct Hooks
        {
            const LegacyScorer& self;
            const State::GameState& state;
            const State::PlayerActorState& player;
            const State::TargetCollection& targets;
            float combatOnsetAgo;
            const Context::ContextWeightMap& weights;
            const std::array<float, Learning::StateFeatures::NUM_FEATURES>& phi;
            Learning::FeatureBanditLearner::LockedReader& qReader;
            Learning::UsageMemory::SnapshotReader& usageReader;

            bool Favorited(const Candidate::CandidateVariant& c) const { return Candidate::IsFavorited(c); }
            float ContextWeight(const Candidate::CandidateVariant& c) const { return Context::WeightForCandidate(c, weights); }
            RE::FormID FormID(const Candidate::CandidateVariant& c) const { return Candidate::GetFormID(c); }
            Learning::FeatureItemMetrics LearnerMetrics(RE::FormID id) const { return qReader.GetMetrics(id, phi); }
            float Recency(RE::FormID id) const { return usageReader.GetRecencyBoost(id); }
            ScoredCandidate Score(const Candidate::CandidateVariant& c, float contextWeight,
                const Learning::FeatureItemMetrics& metrics, float recencyBoost) const
            {
                return self.ScoreCandidateInternal(c, state, player, targets, combatOnsetAgo, contextWeight, metrics,
                    recencyBoost);
            }
            float Utility(const ScoreBreakdown& b) const { return self.ComputeUtility(b); }
            bool HardGated(const Candidate::CandidateVariant& c) const { return Context::IsHardContextGated(c); }
            float Ucb(const Learning::FeatureItemMetrics& m) const { return m.ucb; }
        };
        Hooks hooks{ *this, state, player, targets, combatOnsetAgo, weights, phi, qReader, usageReader };
        const FloorParams floorParams{
            .minimumContextWeight = m_config.minimumContextWeight,
            .minimumUtility = m_config.minimumUtility,
            .topNCandidates = m_config.topNCandidates,
            .coldStartUCBBoost = m_config.coldStartUCBBoost,
        };
        const auto pass = FloorPass(candidates, floorParams, hooks, scored, &run.rows);
        const float maxUtilitySeen = pass.maxUtilitySeen;

        // Diagnostic, rate-limited: no row gets the D2 feature this run (every
        // candidate is still ranked on the choice score).
        // NOTE: Single-threaded (called from the update thread only)
        if (scored.empty() && !candidates.empty()) {
            static auto s_lastWarnTime = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastWarnTime).count() >= 5) {
                logger::debug("[UtilityScorer] Legacy pass: 0/{} candidates passed minimumUtility={:.2f} "
                    "(max utility seen: {:.4f}, baseRelevance: {:.4f}); no row gets ln learn mult"sv,
                    candidates.size(), m_config.minimumUtility,
                    maxUtilitySeen, weights.baseRelevanceWeight);
                s_lastWarnTime = now;
            }
        }

        if (pass.coldStartCount > 0) {
            // NOTE: Single-threaded (called from the update thread only)
            static auto s_lastColdStartLog = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastColdStartLog).count() >= 5) {
                logger::debug("[UtilityScorer] Legacy pass cold start: {} candidates boosted via UCB context floor ({:.2f})"sv,
                    pass.coldStartCount, m_config.coldStartUCBBoost);
                s_lastColdStartLog = now;
            }
        }

        // No post-floor steps: the utility a row carries is the floor pass's,
        // which the D2 feature was fitted on.
        run.ResolveRows(candidates);
        return run;
    }

    ScoredCandidate LegacyScorer::ScoreCandidateInternal(
        const Candidate::CandidateVariant& candidate,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        float combatOnsetAgo,
        float contextWeight,
        const Learning::FeatureItemMetrics& metrics,
        float recencyBoost) const
    {
        ScoredCandidate result;
        result.candidate = candidate;

        // Step 1: the context weight (pre-computed by the floor loop)
        result.breakdown.contextWeight = contextWeight;

        // Step 2: the old learner's metrics (from its LockedReader)
        result.breakdown.rewardEstimate = metrics.rewardEstimate;
        result.breakdown.ucb = metrics.ucb;
        result.breakdown.confidence = metrics.confidence;

        // Step 3: prior
        result.breakdown.prior = [&] {
            Huginn_ZONE_NAMED("Score::Prior");
            return m_priorCalc.CalculatePrior(player, candidate);
        }();

        // Step 4: learning score α*R + (1-α)*prior + β*UCB
        const float alpha = result.breakdown.confidence;
        const float beta = m_config.explorationWeight;
        result.breakdown.learningScore =
            alpha * result.breakdown.rewardEstimate +
            (1.0f - alpha) * result.breakdown.prior +
            beta * result.breakdown.ucb;

        // λ(confidence), so logged breakdowns reproduce the utility formula
        result.breakdown.lambda = ComputeAdaptiveLambda(alpha);

        // Step 4b: recency boost from UsageMemory, additive to the learning
        // score (the cold start reads breakdown.learningScore, so it inherits it)
        result.breakdown.recencyBoost = recencyBoost;
        result.breakdown.learningScore += recencyBoost;

        // Step 5: correlation bonus
        result.breakdown.correlationBonus = [&] {
            Huginn_ZONE_NAMED("Score::Correlation");
            return m_correlationBooster.CalculateBonus(player, targets, candidate);
        }();

        // Step 6: potion multiplier (1.0 for non-potions)
        result.breakdown.potionMultiplier = m_potionDiscrim.GetMultiplier(state, player, candidate, combatOnsetAgo);

        // Step 7: favourites multiplier (the uniform favoritesBoostMax)
        result.breakdown.favoritesMultiplier = GetFavoritesMultiplier(candidate);

        // Step 8: the shared formula
        result.utility = ComputeUtility(result.breakdown);
        return result;
    }

    float LegacyScorer::ComputeUtility(const ScoreBreakdown& breakdown) const
    {
        const float lambda = ComputeAdaptiveLambda(breakdown.confidence);
        // utility = ctx × (1 + λ×learn) × corr × potion × fav
        return breakdown.contextWeight *
               (1.0f + lambda * breakdown.learningScore) *
               breakdown.correlationBonus *
               breakdown.potionMultiplier *
               breakdown.favoritesMultiplier;
    }

    float LegacyScorer::ComputeAdaptiveLambda(float confidence) const
    {
        // λ(confidence) = λMin + confidence × (λMax - λMin)
        return m_config.lambdaMin + confidence * (m_config.lambdaMax - m_config.lambdaMin);
    }

    float LegacyScorer::GetFavoritesMultiplier(const Candidate::CandidateVariant& candidate) const
    {
        // Suppress: 0 for a favourite. Unreachable in a run since R8.1 (the
        // favorite_suppressed filter drops it before scoring); kept so
        // ExplainCandidate's arithmetic is unchanged.
        if (m_config.favoritesMode == FavoritesMode::Suppress) {
            return Candidate::IsFavorited(candidate) ? 0.0f : 1.0f;
        }
        return m_config.FavoritesMultiplier(Candidate::IsFavorited(candidate));
    }

    // -------------------------------------------------------------------------
    // The pipeline's one call (LegacyRun.h)
    // -------------------------------------------------------------------------

    Run LegacyPass(
        const std::vector<Candidate::CandidateVariant>& candidates,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world,
        float combatOnsetAgo)
    {
        if (!g_legacyScorer) {
            Run skipped;
            skipped.Skip(candidates.size());
            return skipped;
        }
        const float heldMagicka = g_utilityScorer ? g_utilityScorer->HeldMagicka(player.vitals.magicka) : player.vitals.magicka;
        const float heldStamina = g_utilityScorer ? g_utilityScorer->HeldStamina(player.vitals.stamina) : player.vitals.stamina;
        return g_legacyScorer->Pass(candidates, state, player, targets, world, combatOnsetAgo, heldMagicka, heldStamina);
    }
}
