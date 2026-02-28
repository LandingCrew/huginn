#include "UtilityScorer.h"
#include "state/ContextWeightSettings.h"  // For BuildConfig() in constructor
#include "util/ScopedTimer.h"
#include <algorithm>
#include <chrono>
#include <unordered_set>

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
        const State::WorldState& world)  // Stage 1f: Added WorldState
    {
        SCOPED_TIMER("UtilityScorer::ScoreCandidates");

        ScoredCandidateList scored;
        scored.reserve(candidates.size());

        // Stage 1f: Evaluate context rules ONCE for all candidates
        // This replaces per-candidate relevance from CandidateGenerator
        Context::ContextWeightMap weights = m_contextEngine.EvaluateRules(
            state, player, targets, world);

        // Phase 3.5c: Pre-compute StateFeatures for FeatureQLearner (once per scoring pass)
        auto stateFeatures = Learning::StateFeatures::FromState(player, targets);
        auto phi = stateFeatures.ToArray();  // Pre-compute once for locked reader

        // Lazy decay: apply time-based weight decay to candidates about to be scored.
        // Only decays items idle > DECAY_THRESHOLD_MINUTES. Cost is proportional to
        // the candidate pool (~50-200), not the entire Q-table.
        for (const auto& candidate : candidates) {
            m_featureLearner.MaybeDecay(Candidate::GetFormID(candidate));
        }

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
            float contextWeight = GetContextWeight(candidate, weights);
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
            // Build set of already-scored FormIDs for O(1) dedup
            std::unordered_set<RE::FormID> scoredIDs;
            scoredIDs.reserve(scored.size());
            for (const auto& s : scored) scoredIDs.insert(s.GetFormID());

            size_t coldStartCount = 0;
            for (const auto& candidate : candidates) {
                RE::FormID formID = Candidate::GetFormID(candidate);
                if (scoredIDs.contains(formID)) continue;

                float contextWeight = GetContextWeight(candidate, weights);

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

                // Override context weight and recompute utility with boosted value
                // SYNC: Must match ScoreCandidateInternal Step 8 formula
                result.breakdown.contextWeight = boostedContext;

                {
                    float lambda = ComputeAdaptiveLambda(result.breakdown.confidence);
                    result.utility =
                        boostedContext *
                        (1.0f + lambda * result.breakdown.learningScore) *
                        result.breakdown.correlationBonus *
                        result.breakdown.potionMultiplier *
                        result.breakdown.favoritesMultiplier;
                }

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

        // Partial sort for top N (much faster than full sort for large lists)
        size_t topN = std::min(m_config.topNCandidates, scored.size());
        if (topN > 0) {
            if (scored.size() > topN) {
                std::partial_sort(scored.begin(), scored.begin() + topN, scored.end());
            } else {
                std::sort(scored.begin(), scored.end());
            }
        }

        // Apply wildcards for exploration
        m_wildcardMgr.ApplyWildcards(scored);

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
            state, player, targets, world);

        // Single candidate — no lock amortization benefit, use direct APIs
        RE::FormID formID = Candidate::GetFormID(candidate);
        auto stateFeatures = Learning::StateFeatures::FromState(player, targets);
        float contextWeight = GetContextWeight(candidate, weights);
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
        result.breakdown.prior = m_priorCalc.CalculatePrior(state, player, candidate);

        // =====================================================================
        // Step 4: Compute learning score: α*Q + (1-α)*prior + β*UCB
        // =====================================================================
        float alpha = result.breakdown.confidence;
        float beta = m_config.explorationWeight;

        result.breakdown.learningScore =
            alpha * result.breakdown.qValue +
            (1.0f - alpha) * result.breakdown.prior +
            beta * result.breakdown.ucb;

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
        result.breakdown.favoritesMultiplier =
            GetFavoritesMultiplier(candidate, 0, 1);  // Rank 0 for now (recalculated later)

        // =====================================================================
        // Step 8: Compute final utility (version-dependent formula)
        // SYNC: Cold-start fallback in ScoreCandidates() duplicates this formula.
        //       If you change the formula here, update the fallback too.
        // NOTE: breakdown.learningScore already includes recencyBoost (added in
        //       Step 4b). The cold-start block uses breakdown.learningScore
        //       directly, so it inherits the boost automatically. Do NOT add
        //       a separate recencyBoost term in the cold-start path.
        // =====================================================================

        {
            // Compute confidence-adaptive lambda
            float lambda = ComputeAdaptiveLambda(result.breakdown.confidence);

            // Formula: utility = ctx × (1 + λ×learn) × corr × potion × fav
            result.utility =
                result.breakdown.contextWeight *                        // [0,1] gate
                (1.0f + lambda * result.breakdown.learningScore) *     // Learning boost
                result.breakdown.correlationBonus *                     // Multiplicative
                result.breakdown.potionMultiplier *
                result.breakdown.favoritesMultiplier;
        }

        return result;
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
        return std::visit([](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                return c.isFavorited;
            } else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                return c.isFavorited;
            } else {
                return false;  // Items, scrolls, ammo don't have favorites in the same way
            }
        }, candidate);
    }

    // =========================================================================
    // CONTEXT WEIGHT EXTRACTION (Stage 1f)
    // =========================================================================
    // Maps candidate type/tags to the relevant weight from ContextWeightMap.
    // Uses std::max to combine multiple applicable weights (e.g., AOE + Damage).
    // =========================================================================

    float UtilityScorer::GetContextWeight(
        const Candidate::CandidateVariant& candidate,
        const Context::ContextWeightMap& weights) const
    {
        using namespace Spell;
        using namespace Item;

        return std::visit([&weights](const auto& c) -> float {
            using T = std::decay_t<decltype(c)>;

            // =====================================================================
            // SPELL CANDIDATES
            // =====================================================================
            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                // Start with spell baseline (like weaponWeight for weapons)
                // Ensures spells surface on typed slots even without specific context
                float maxWeight = std::max(weights.spellWeight, weights.baseRelevanceWeight);

                // Restoration
                if (HasTag(c.tags, SpellTag::RestoreHealth)) {
                    maxWeight = std::max(maxWeight, weights.healingWeight);
                }
                if (HasTag(c.tags, SpellTag::RestoreMagicka)) {
                    maxWeight = std::max(maxWeight, weights.magickaRestoreWeight);
                }
                if (HasTag(c.tags, SpellTag::RestoreStamina)) {
                    maxWeight = std::max(maxWeight, weights.staminaRestoreWeight);
                }
                if (HasTag(c.tags, SpellTag::Ward)) {
                    maxWeight = std::max(maxWeight, weights.wardWeight);
                }

                // Combat/Damage
                if (c.type == SpellType::Damage) {
                    maxWeight = std::max(maxWeight, weights.damageWeight);
                }
                if (HasTag(c.tags, SpellTag::AOE)) {
                    maxWeight = std::max(maxWeight, weights.aoeWeight);
                }

                // Summons
                if (c.type == SpellType::Summon) {
                    maxWeight = std::max(maxWeight, weights.summonWeight);
                }
                if (HasTag(c.tags, SpellTag::BoundWeapon)) {
                    maxWeight = std::max(maxWeight, weights.boundWeaponWeight);
                }

                // Target-specific
                if (HasTag(c.tags, SpellTag::AntiUndead) || HasTag(c.tags, SpellTag::TurnUndead) ||
                    HasTag(c.tags, SpellTag::Sun)) {
                    maxWeight = std::max(maxWeight, weights.antiUndeadWeight);
                }
                if (HasTag(c.tags, SpellTag::AntiDaedra)) {
                    maxWeight = std::max(maxWeight, weights.antiDaedraWeight);
                }
                // TODO (Low priority): Add SpellTag::AntiDragon for Dragonrend
                // Currently falls back to base relevance + Q-learning

                // Stealth
                if (HasTag(c.tags, SpellTag::Stealth) || HasTag(c.tags, SpellTag::Invisibility) ||
                    HasTag(c.tags, SpellTag::Muffle)) {
                    maxWeight = std::max(maxWeight, weights.stealthWeight);
                }

                // Resist spells: Buff/Defensive spells with elemental element map to resist weights.
                // SpellTag is at 32/32 bits — no room for ResistFire/Frost/Shock tags.
                // Instead, use the existing SpellType + ElementType combo set by SpellClassifier.
                // Damage spells have SpellType::Damage and won't match this check.
                if (c.type == SpellType::Buff || c.type == SpellType::Defensive) {
                    switch (c.element) {
                    case Spell::ElementType::Fire:
                        maxWeight = std::max(maxWeight, weights.resistFireWeight);
                        break;
                    case Spell::ElementType::Frost:
                        maxWeight = std::max(maxWeight, weights.resistFrostWeight);
                        break;
                    case Spell::ElementType::Shock:
                        maxWeight = std::max(maxWeight, weights.resistShockWeight);
                        break;
                    case Spell::ElementType::Poison:
                        maxWeight = std::max(maxWeight, weights.resistPoisonWeight);
                        break;
                    default: break;
                    }
                }

                // TODO (Low priority): Environmental spells (Waterbreathing, Slow Fall, Unlock)
                // SpellTag enum is at capacity (32 bits used). These would need:
                //   1. SpellTagExt enum (like ItemTagExt), OR
                //   2. Name matching as fallback
                // For now, they fall back to base relevance + Q-learning

                return maxWeight;
            }
            // =====================================================================
            // POTION/ITEM CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::ItemCandidate>) {
                float maxWeight = weights.baseRelevanceWeight;  // Start with noise floor

                // Restoration
                if (HasTag(c.tags, ItemTag::RestoreHealth)) {
                    maxWeight = std::max(maxWeight, weights.healingWeight);
                }
                if (HasTag(c.tags, ItemTag::RestoreMagicka)) {
                    maxWeight = std::max(maxWeight, weights.magickaRestoreWeight);
                }
                if (HasTag(c.tags, ItemTag::RestoreStamina)) {
                    maxWeight = std::max(maxWeight, weights.staminaRestoreWeight);
                }

                // Elemental resistances
                if (HasTag(c.tags, ItemTag::ResistFire)) {
                    maxWeight = std::max(maxWeight, weights.resistFireWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistFrost)) {
                    maxWeight = std::max(maxWeight, weights.resistFrostWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistShock)) {
                    maxWeight = std::max(maxWeight, weights.resistShockWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistPoison)) {
                    maxWeight = std::max(maxWeight, weights.resistPoisonWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistDisease)) {
                    maxWeight = std::max(maxWeight, weights.resistDiseaseWeight);
                }

                // Workstation fortify potions (FIX: Stage 1g code review)
                // These are split across three different tag/field pairs:
                if (HasTag(c.tags, ItemTag::FortifyCombatSkill)) {
                    if (c.combatSkill == Item::CombatSkill::Smithing) {
                        maxWeight = std::max(maxWeight, weights.fortifySmithingWeight);
                    }
                }
                if (HasTag(c.tags, ItemTag::FortifyMagicSchool)) {
                    if (c.school == Item::MagicSchool::Enchanting) {
                        maxWeight = std::max(maxWeight, weights.fortifyEnchantingWeight);
                    }
                }
                if (HasTag(c.tags, ItemTag::FortifyUtilitySkill)) {
                    if (c.utilitySkill == Item::UtilitySkill::Alchemy) {
                        maxWeight = std::max(maxWeight, weights.fortifyAlchemyWeight);
                    }
                }

                // Environmental (Waterbreathing potions)
                if (HasTag(c.tags, ItemTag::Waterbreathing)) {
                    maxWeight = std::max(maxWeight, weights.waterbreathingWeight);
                }

                // Stealth (Invisibility potions - Muffle doesn't exist as a potion)
                if (HasTag(c.tags, ItemTag::Invisibility)) {
                    maxWeight = std::max(maxWeight, weights.stealthWeight);
                }

                // Soul gems (for weapon charging)
                // Check if this is a soul gem by source type
                if (c.sourceType == Candidate::SourceType::SoulGem) {
                    maxWeight = std::max(maxWeight, weights.weaponChargeWeight);
                }

                return maxWeight;
            }
            // =====================================================================
            // WEAPON CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                // Weapons get the best of: dedicated weapon weight, combat damage, or base relevance
                return std::max({weights.weaponWeight, weights.damageWeight, weights.baseRelevanceWeight});
            }
            // =====================================================================
            // SCROLL CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::ScrollCandidate>) {
                // Scrolls have spell tags, use same logic as spells
                float maxWeight = std::max(weights.spellWeight, weights.baseRelevanceWeight);

                if (HasTag(c.tags, Scroll::ScrollTag::RestoreHealth)) {
                    maxWeight = std::max(maxWeight, weights.healingWeight);
                }
                if (HasTag(c.tags, Scroll::ScrollTag::AOE)) {
                    maxWeight = std::max(maxWeight, weights.aoeWeight);
                }
                if (HasTag(c.tags, Scroll::ScrollTag::AntiUndead) || HasTag(c.tags, Scroll::ScrollTag::Sun)) {
                    maxWeight = std::max(maxWeight, weights.antiUndeadWeight);
                }
                // Add more scroll-specific mappings as needed

                return maxWeight;
            }
            // =====================================================================
            // AMMO CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::AmmoCandidate>) {
                return std::max(weights.ammoWeight, weights.baseRelevanceWeight);
            }
            else {
                // Unknown type - return base relevance
                return weights.baseRelevanceWeight;
            }
        }, candidate);
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

    void UtilityScorer::Update(float deltaSeconds)
    {
        m_potionDiscrim.Update(deltaSeconds);
    }

    void UtilityScorer::Reset()
    {
        m_wildcardMgr.Reset();
        m_wasInCombat = false;
    }

    void UtilityScorer::SetContextWeightConfig(const State::ContextWeightConfig& config)
    {
        m_contextEngine.SetConfig(config);
    }

    // =========================================================================
    // DEBUG LOGGING
    // =========================================================================

    void UtilityScorer::LogTopCandidates(const ScoredCandidateList& ranked, size_t count) const
    {
        size_t numToLog = std::min(count, ranked.size());

        if (numToLog == 0) {
            logger::info("[UtilityScorer] No candidates to display"sv);
            return;
        }

        logger::info("=== Top {} Candidates ==="sv, numToLog);

        for (size_t i = 0; i < numToLog; ++i) {
            const auto& scored = ranked[i];
            const auto& bd = scored.breakdown;

            logger::info("{}. {} ({}) - Utility: {:.3f} {}"sv,
                i + 1,
                scored.GetName(),
                Candidate::SourceTypeToString(scored.GetSourceType()),
                scored.utility,
                scored.isWildcard ? "[WILDCARD]" : "");

            logger::info("     {} "sv, bd.ToString());
        }
    }

}  // namespace Huginn::Scoring
