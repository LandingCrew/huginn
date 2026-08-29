#include "UtilityScorer.h"
#include "context/ContextWeightSettings.h"       // For BuildConfig() in constructor
#include "context/ContextWeightForCandidate.h"   // Context::WeightForCandidate (moved from here, #10)
#include "util/ScopedTimer.h"
#include <algorithm>
#include <chrono>

namespace Huginn::Scoring
{
    UtilityScorer::UtilityScorer(Learning::FeatureQLearner& featureLearner, Learning::UsageMemory& usageMemory, const ScorerConfig& config)
        : m_featureLearner(featureLearner)
        , m_usageMemory(usageMemory)
        , m_config(config)
        , m_correlationBooster(m_config)
        , m_potionDiscrim(m_config)
        , m_contextEngine(State::ContextWeightSettings::GetSingleton().BuildConfig())  // BuildConfig pattern
    {
        // Configure wildcard manager from scorer config
        m_wildcardMgr.SetEnabled(true);  // Always enabled by default
    }

    // =========================================================================
    // MAIN SCORING METHOD
    // =========================================================================

    ScoredCandidateList UtilityScorer::ScoreCandidates(
        const std::vector<Candidate::CandidateVariant>& candidates,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world,  // Stage 1f: Added WorldState
        const WildcardPage& displayPage,
        Context::ContextWeightMap* outWeights)
    {
        SCOPED_TIMER("UtilityScorer::ScoreCandidates");

        ScoredCandidateList scored;
        scored.reserve(candidates.size());

        // Stage 1f: Evaluate context rules ONCE for all candidates
        // This replaces per-candidate relevance from CandidateGenerator
        Context::ContextWeightMap weights = m_contextEngine.EvaluateRules(
            player, targets, world);

        // Hand the map back so the display explanation is read off the SAME
        // weights that ranked the list, not a second derivation (#10).
        if (outWeights) {
            *outWeights = weights;
        }

        // Phase 3.5c: Pre-compute StateFeatures for FeatureQLearner (once per scoring pass)
        auto stateFeatures = Learning::StateFeatures::FromState(player, targets);
        auto phi = stateFeatures.ToArray();  // Pre-compute once for locked reader

        // Lazy decay: apply time-based weight decay to candidates about to be scored.
        // Only decays items idle > DECAY_THRESHOLD_MINUTES. Batched: one shared-lock
        // pass over the pool instead of ~N per-candidate lock acquisitions.
        m_decayScratch.clear();
        m_decayScratch.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            m_decayScratch.push_back(Candidate::GetFormID(candidate));
        }
        m_featureLearner.MaybeDecayBatch(m_decayScratch);

        // Acquire locked readers once for the entire scoring loop.
        // Eliminates per-candidate mutex acquire/release (~200 lock ops → 2).
        auto qReader = m_featureLearner.AcquireReader();
        auto usageReader = m_usageMemory.AcquireReader(state);

        // Score all candidates
        float maxUtilitySeen = 0.0f;
        for (const auto& candidate : candidates) {
            // Early filter: Skip candidates with very low context weight
            // Stage 1f: Use GetContextWeight instead of Candidate::GetRelevance
            // Favorites always pass — they represent explicit player intent and must
            // remain observable by the learner even when context weight is low.
            float contextWeight = Context::WeightForCandidate(candidate, weights);
            if (contextWeight < m_config.minimumContextWeight && !IsCandidateFavorited(candidate)) {
                continue;
            }

            RE::FormID formID = Candidate::GetFormID(candidate);
            auto metrics = qReader.GetMetrics(formID, phi);
            float recencyBoost = usageReader.GetRecencyBoost(formID);

            ScoredCandidate result = ScoreCandidateInternal(
                candidate, state, player, targets, world, weights,
                contextWeight, metrics, recencyBoost);

            maxUtilitySeen = std::max(maxUtilitySeen, result.utility);

            // Post-filter: Skip candidates with very low utility
            if (result.utility >= m_config.minimumUtility) {
                scored.push_back(std::move(result));
            }
        }

        // Diagnostic: Rate-limited warning when 0 candidates pass the filter
        // NOTE: Single-threaded (ScoreCandidates called from update thread only)
        if (scored.empty() && !candidates.empty()) {
            static auto s_lastWarnTime = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastWarnTime).count() >= 5) {
                logger::warn("[UtilityScorer] WARNING: 0/{} candidates passed minimumUtility={:.2f} "
                    "(max utility seen: {:.4f}, baseRelevance: {:.4f})",
                    candidates.size(), m_config.minimumUtility,
                    maxUtilitySeen, weights.baseRelevanceWeight);
                s_lastWarnTime = now;
            }
        }

        // Cold-start fallback: if too few candidates passed to fill slots, boost context
        // via UCB so untried items can surface. Triggers when scored < topNCandidates (10),
        // which covers both "Top 0" (empty Q-table) and "Top 1" (only 1 item has real
        // context weight, e.g. a favorited weapon). The fallback self-heals as UCB decays.
        if (scored.size() < m_config.topNCandidates && m_config.coldStartUCBBoost > 0.0f) {
            // Dedup against already-scored candidates by linear scan — this branch
            // only runs when scored.size() < topNCandidates (<10), so a scan beats
            // allocating a hash set every cold-start tick.
            const auto alreadyScored = [&scored](RE::FormID id) {
                for (const auto& s : scored) {
                    if (s.GetFormID() == id) return true;
                }
                return false;
            };

            size_t coldStartCount = 0;
            for (const auto& candidate : candidates) {
                RE::FormID formID = Candidate::GetFormID(candidate);
                if (alreadyScored(formID)) continue;

                float contextWeight = Context::WeightForCandidate(candidate, weights);

                // UCB-driven context floor for untried/low-visit items (Phase 3.5c: FeatureQLearner)
                // Uses locked reader — no per-candidate lock acquisition
                auto metrics = qReader.GetMetrics(formID, phi);
                float boostedContext = std::max(contextWeight,
                    m_config.coldStartUCBBoost * metrics.ucb);

                if (boostedContext < m_config.minimumContextWeight) continue;

                float recencyBoost = usageReader.GetRecencyBoost(formID);

                // Score candidate normally, then recompute utility with boosted context
                ScoredCandidate result = ScoreCandidateInternal(
                    candidate, state, player, targets, world, weights,
                    contextWeight, metrics, recencyBoost);

                // Override context weight and recompute utility with the boosted
                // value via the shared formula helper (reads contextWeight from
                // the breakdown, so the assignment above must come first).
                result.breakdown.contextWeight = boostedContext;
                result.utility = ComputeUtility(result.breakdown);

                if (result.utility >= m_config.minimumUtility) {
                    result.isColdStartBoosted = true;
                    scored.push_back(std::move(result));
                    ++coldStartCount;
                }

                if (scored.size() >= m_config.topNCandidates) break;
            }

            if (coldStartCount > 0) {
                // Rate-limited: same cadence as the "0 candidates" diagnostic above
                // NOTE: Single-threaded (ScoreCandidates called from update thread only)
                static auto s_lastColdStartLog = std::chrono::steady_clock::time_point{};
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastColdStartLog).count() >= 5) {
                    logger::warn("[UtilityScorer] Cold start: {} candidates boosted via UCB context floor ({:.2f})",
                        coldStartCount, m_config.coldStartUCBBoost);
                    s_lastColdStartLog = now;
                }
            }
        }

        // Rank-scaled favorites boost: replace Step 7's provisional uniform max
        // boost with the documented rank-scaled value (ScorerConfig.h).
        ApplyFavoritesRankScaling(scored);

        // Partial sort for top N (much faster than full sort for large lists)
        size_t topN = std::min(m_config.topNCandidates, scored.size());
        if (topN > 0) {
            if (scored.size() > topN) {
                std::partial_sort(scored.begin(), scored.begin() + topN, scored.end());
            } else {
                std::sort(scored.begin(), scored.end());
            }
        }

        // Apply wildcards for exploration, keyed and sized against the page
        // THIS tick allocates. Previously this called an overload that read
        // SlotAllocator::GetSlotCount() live, so a page switch landing mid-tick
        // could size wildcards against the new page while the pipeline allocated
        // the snapshotted one — resolves TODO(page-consistency).
        m_wildcardMgr.ApplyWildcards(scored, displayPage);

        return scored;
    }

    // =========================================================================
    // SINGLE CANDIDATE SCORING (Public)
    // =========================================================================

    ScoredCandidate UtilityScorer::ScoreCandidate(
        const Candidate::CandidateVariant& candidate,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world)  // Stage 1f: Added WorldState
    {
        // Stage 1f: Evaluate context rules for single candidate
        Context::ContextWeightMap weights = m_contextEngine.EvaluateRules(
            player, targets, world);

        // Single candidate — no lock amortization benefit, use direct APIs
        RE::FormID formID = Candidate::GetFormID(candidate);
        auto stateFeatures = Learning::StateFeatures::FromState(player, targets);
        float contextWeight = Context::WeightForCandidate(candidate, weights);
        auto metrics = m_featureLearner.GetMetrics(formID, stateFeatures);
        float recencyBoost = m_usageMemory.GetRecencyBoost(formID, state);

        return ScoreCandidateInternal(candidate, state, player, targets, world, weights,
            contextWeight, metrics, recencyBoost);
    }

    // =========================================================================
    // INTERNAL SCORING IMPLEMENTATION
    // =========================================================================

    ScoredCandidate UtilityScorer::ScoreCandidateInternal(
        const Candidate::CandidateVariant& candidate,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world,
        const Context::ContextWeightMap& weights,
        float contextWeight,
        const Learning::FeatureItemMetrics& metrics,
        float recencyBoost)
    {
        ScoredCandidate result;
        result.candidate = candidate;

        // =====================================================================
        // Step 1: Use pre-computed context weight (caller already called GetContextWeight)
        // =====================================================================
        result.breakdown.contextWeight = contextWeight;

        // =====================================================================
        // Step 2: Use pre-computed learning metrics (from LockedReader or direct API)
        // =====================================================================
        result.breakdown.qValue = metrics.qValue;
        result.breakdown.ucb = metrics.ucb;
        result.breakdown.confidence = metrics.confidence;

        // =====================================================================
        // Step 3: Calculate prior from PriorCalculator
        // =====================================================================
        result.breakdown.prior = m_priorCalc.CalculatePrior(player, candidate);

        // =====================================================================
        // Step 4: Compute learning score: α*Q + (1-α)*prior + β*UCB
        // =====================================================================
        float alpha = result.breakdown.confidence;
        float beta = m_config.explorationWeight;

        result.breakdown.learningScore =
            alpha * result.breakdown.qValue +
            (1.0f - alpha) * result.breakdown.prior +
            beta * result.breakdown.ucb;

        // Record λ(confidence) so logged breakdowns reproduce the utility formula
        result.breakdown.lambda = ComputeAdaptiveLambda(alpha);

        // =====================================================================
        // Step 4b: Recency boost from UsageMemory (event-driven short-term recall)
        // =====================================================================
        // Additive to learningScore — context weight still gates final utility.
        // Uses pre-computed value from LockedReader (or direct API for single-candidate path).
        result.breakdown.recencyBoost = recencyBoost;
        result.breakdown.learningScore += recencyBoost;

        // =====================================================================
        // Step 5: Calculate correlation bonus
        // =====================================================================
        result.breakdown.correlationBonus =
            m_correlationBooster.CalculateBonus(player, targets, candidate);

        // =====================================================================
        // Step 6: Get potion multiplier (1.0 for non-potions)
        // =====================================================================
        result.breakdown.potionMultiplier =
            m_potionDiscrim.GetMultiplier(state, player, candidate);

        // =====================================================================
        // Step 7: Get favorites multiplier
        // =====================================================================
        // Boost mode: provisional uniform boost (rank 0 of 1 = favoritesBoostMax).
        // ScoreCandidates replaces it with the rank-scaled value once the whole
        // cohort is scored (see ApplyFavoritesRankScaling). The single-candidate
        // path has no cohort to rank against, so max is its final value.
        result.breakdown.favoritesMultiplier =
            GetFavoritesMultiplier(candidate, 0, 1);

        // =====================================================================
        // Step 8: Compute final utility via the shared formula helper.
        // NOTE: breakdown.learningScore already includes recencyBoost (added in
        //       Step 4b). The cold-start block uses breakdown.learningScore
        //       directly, so it inherits the boost automatically. Do NOT add
        //       a separate recencyBoost term in the cold-start path.
        // =====================================================================
        result.utility = ComputeUtility(result.breakdown);

        return result;
    }

    float UtilityScorer::ComputeUtility(const ScoreBreakdown& breakdown) const
    {
        const float lambda = ComputeAdaptiveLambda(breakdown.confidence);

        // Formula: utility = ctx × (1 + λ×learn) × corr × potion × fav
        return breakdown.contextWeight *                    // [0,1] gate
               (1.0f + lambda * breakdown.learningScore) *  // Learning boost
               breakdown.correlationBonus *                 // Multiplicative
               breakdown.potionMultiplier *
               breakdown.favoritesMultiplier;
    }

    // =========================================================================
    // ADAPTIVE LAMBDA CALCULATION (Stage 2b)
    // =========================================================================

    float UtilityScorer::ComputeAdaptiveLambda(float confidence) const
    {
        // λ(confidence) = λMin + confidence × (λMax - λMin)
        // At confidence=0: λ = 0.5 (context dominates)
        // At confidence=1: λ = 3.0 (learning amplified 6×)
        return m_config.lambdaMin +
               confidence * (m_config.lambdaMax - m_config.lambdaMin);
    }

    // =========================================================================
    // FAVORITES HANDLING
    // =========================================================================

    float UtilityScorer::GetFavoritesMultiplier(
        const Candidate::CandidateVariant& candidate,
        size_t rank,
        size_t totalItems) const
    {
        switch (m_config.favoritesMode) {
            case FavoritesMode::Boost:
                if (IsCandidateFavorited(candidate)) {
                    return m_config.GetFavoritesMultiplier(rank, totalItems);
                }
                return 1.0f;

            case FavoritesMode::Off:
                return 1.0f;

            case FavoritesMode::Suppress:
                if (IsCandidateFavorited(candidate)) {
                    return 0.0f;  // Effectively removes from consideration
                }
                return 1.0f;
        }

        return 1.0f;
    }

    bool UtilityScorer::IsCandidateFavorited(const Candidate::CandidateVariant& candidate) const
    {
        return Candidate::IsFavorited(candidate);
    }

    void UtilityScorer::ApplyFavoritesRankScaling(ScoredCandidateList& scored)
    {
        if (m_config.favoritesMode != FavoritesMode::Boost) {
            return;
        }

        m_favoriteRankScratch.clear();
        for (size_t i = 0; i < scored.size(); ++i) {
            if (IsCandidateFavorited(scored[i].candidate)) {
                m_favoriteRankScratch.push_back(i);
            }
        }
        // 0 favorites: nothing to scale. 1 favorite: rank 0 of 1 is defined as
        // favoritesBoostMax, which Step 7's provisional value already is.
        if (m_favoriteRankScratch.size() < 2) {
            return;
        }

        // Rank favorites by current utility. Step 7 gave every favorite the same
        // provisional multiplier, so this order equals their pre-boost order.
        std::sort(m_favoriteRankScratch.begin(), m_favoriteRankScratch.end(),
            [&scored](size_t a, size_t b) noexcept {
                return scored[a].utility > scored[b].utility;
            });

        // The multiplier is monotone non-increasing in rank, so rewriting
        // utilities cannot reorder favorites relative to each other — the ranks
        // derived above stay valid and no re-sort is needed here (the caller's
        // partial_sort establishes the final combined order).
        const size_t total = m_favoriteRankScratch.size();
        for (size_t rank = 0; rank < total; ++rank) {
            auto& entry = scored[m_favoriteRankScratch[rank]];
            entry.breakdown.favoritesMultiplier = m_config.GetFavoritesMultiplier(rank, total);
            entry.utility = ComputeUtility(entry.breakdown);
        }

        // Deliberately NO minimumUtility re-filter here: favorites stay in the
        // list even if rescaling drops them below the threshold. The scoring
        // loop's favorites-always-pass contract (they must remain observable by
        // the learner) applies to membership; rank scaling only corrects their
        // ORDER. Erasing here would also under-fill slots after the cold-start
        // fallback already ran, and near-threshold favorites would flicker in
        // and out of the list across ticks.
    }

    // =========================================================================
    // COMBAT STATE TRACKING
    // =========================================================================

    void UtilityScorer::OnCombatStart()
    {
        m_potionDiscrim.OnCombatStart();
    }

    void UtilityScorer::OnCombatEnd()
    {
        m_potionDiscrim.OnCombatEnd();
    }

    bool UtilityScorer::Update(float deltaSeconds)
    {
        m_potionDiscrim.Update(deltaSeconds);
        return m_wildcardMgr.UpdateExpiry();
    }

    void UtilityScorer::Reset()
    {
        m_wildcardMgr.Reset();
    }

    void UtilityScorer::SetContextWeightConfig(const State::ContextWeightConfig& config)
    {
        m_contextEngine.SetConfig(config);
    }

    // =========================================================================
    // DEBUG LOGGING
    // =========================================================================

    void UtilityScorer::LogTopCandidates(
        const ScoredCandidateList& ranked, size_t count, bool detail, bool force) const
    {
        size_t numToLog = std::min(count, ranked.size());

        if (!force) {
            // Dedup: skip the dump unless the ranking's membership/order changed.
            // Utilities are continuous in the vitals, so they drift every scoring
            // run — including them here would defeat the dedup. Forced dumps
            // (hg recs) bypass and don't update the signature.
            // NOTE: Single-threaded (called from update thread only)
            static std::string s_lastSignature{"<none>"};
            std::string signature;
            for (size_t i = 0; i < numToLog; ++i) {
                signature += fmt::format("{}|", ranked[i].GetName());
            }
            if (signature == s_lastSignature) {
                return;
            }
            s_lastSignature = std::move(signature);
        }

        if (numToLog == 0) {
            logger::info("[Recs] No candidates to display"sv);
            return;
        }

        logger::info("[Recs] === Top {}/{} candidates | u = ctx*(1+λ*learn)*mults ==="sv,
            numToLog, ranked.size());

        for (size_t i = 0; i < numToLog; ++i) {
            const auto& scored = ranked[i];
            const auto& bd = scored.breakdown;

            logger::info("[Recs] {}. {} ({}) u={:.3f} | {}{}{}"sv,
                i + 1,
                scored.GetName(),
                Candidate::SourceTypeToString(scored.GetSourceType()),
                scored.utility,
                detail ? bd.ToDetailString() : bd.ToCompactString(),
                scored.isWildcard ? " [WC]" : "",
                scored.isColdStartBoosted ? " [COLD]" : "");
        }
    }

}  // namespace Huginn::Scoring
