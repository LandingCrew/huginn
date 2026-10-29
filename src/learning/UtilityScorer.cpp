#include "UtilityScorer.h"
#include "context/ContextWeightSettings.h"       // For BuildConfig() in constructor
#include "context/ContextWeightForCandidate.h"   // Context::WeightForCandidate (moved from here, #10)
#include "util/ScopedTimer.h"
#include "state/StateManager.h"                 // GetPlayerVitals: vital envelopes tick every frame
#include "override/OverrideConfig.h"                // URGENT_RESTORE_WINDOW_SEC: one "strongest" for slots and override
#include "Profiling.h"
#include "core/EffectColumns.h"   // [Recs]: the explanation's column names (R8.3)
#include "core/NeedIds.h"         // ... and need names
#include <algorithm>
#include <chrono>
#include <map>
#include <tuple>

namespace Huginn::Scoring
{
    UtilityScorer::UtilityScorer(Learning::FeatureBanditLearner& featureLearner, Learning::UsageMemory& usageMemory, const ScorerConfig& config)
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

    ScoredCandidateList UtilityScorer::LegacyPass(
        const std::vector<Candidate::CandidateVariant>& candidates,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world,  // Stage 1f: Added WorldState
        float combatOnsetAgo,
        Context::ContextWeightMap* outWeights,
        std::vector<Legacy::Row>* outLegacy)
    {
        SCOPED_TIMER("UtilityScorer::LegacyPass");

        ScoredCandidateList scored;
        scored.reserve(candidates.size());

        // Magicka and stamina are scored at their recent low while they
        // regenerate (VitalEnvelope), on a COPY: the rule engine stays pure --
        // a stateful engine let one evaluation's low leak into the next, which
        // the threshold tests caught -- and nothing but the context weights
        // sees the held value. Health is not held; see VitalEnvelope.
        // The envelopes are ticked every frame in Update(); this only reads them.
        State::PlayerActorState scoringPlayer = player;
        scoringPlayer.vitals.magicka = m_magickaEnvelope.Value(player.vitals.magicka);
        scoringPlayer.vitals.stamina = m_staminaEnvelope.Value(player.vitals.stamina);

        // Stage 1f: Evaluate context rules ONCE for all candidates
        // This replaces per-candidate relevance from CandidateGenerator
        // Tracy zones below split this function (and Pipeline::ScoreCandidates,
        // its caller) so a trace can say which part costs: see the 2026-10-03
        // entry in docs/profiling/tracy-traces.md. The per-candidate ones
        // (Score::Candidate, Score::LearnerMetrics, Score::Prior,
        // Score::Correlation) give MTPC per candidate; Counts / runs = pool size.
        Context::ContextWeightMap weights{};
        {
            Huginn_ZONE_NAMED("Score::ContextRules");
            weights = m_contextEngine.EvaluateRules(scoringPlayer, targets, world);
        }

        // Hand the map back so the display explanation is read off the SAME
        // weights that ranked the list, not a second derivation (#10).
        if (outWeights) {
            *outWeights = weights;
        }

        // Phase 3.5c: Pre-compute StateFeatures for FeatureBanditLearner (once per scoring pass)
        auto stateFeatures = Learning::StateFeatures::FromState(player, targets);
        auto phi = stateFeatures.ToArray();  // Pre-compute once for locked reader

        // Forget what faded past its useful life (roadmap Phase 3 #3a). At most
        // once a minute of play; the fading itself is read off the play clock
        // per candidate below, so nothing here touches the weights. Replaced
        // the 2%/hour weight decay.
        {
            Huginn_ZONE_NAMED("Score::Forget");
            m_featureLearner.MaybeForgetFaded();
        }

        // Acquire locked readers once for the entire scoring loop.
        // Eliminates per-candidate mutex acquire/release (~200 lock ops → 2).
        auto qReader = m_featureLearner.AcquireReader();
        auto usageReader = m_usageMemory.AcquireReader(state);

        // The floor loop and the cold-start rescue live in the legacy learn
        // feature module since R8.2 (learning/legacy/LegacyLearnFeature.h):
        // moved unchanged in arithmetic, called with this scorer's own
        // ScoreCandidateInternal, so the list is built exactly as before. The
        // per-candidate floors it reports are the D2 feature's input (the new
        // model in shadow reads them off this same pass).
        struct Hooks
        {
            UtilityScorer& self;
            const State::GameState& state;
            const State::PlayerActorState& player;
            const State::TargetCollection& targets;
            const State::WorldState& world;
            float combatOnsetAgo;
            const Context::ContextWeightMap& weights;
            const std::array<float, Learning::StateFeatures::NUM_FEATURES>& phi;
            Learning::FeatureBanditLearner::LockedReader& qReader;
            Learning::UsageMemory::SnapshotReader& usageReader;

            bool Favorited(const Candidate::CandidateVariant& c) const { return self.IsCandidateFavorited(c); }
            float ContextWeight(const Candidate::CandidateVariant& c) const { return Context::WeightForCandidate(c, weights); }
            RE::FormID FormID(const Candidate::CandidateVariant& c) const { return Candidate::GetFormID(c); }
            Learning::FeatureItemMetrics LearnerMetrics(RE::FormID id) const { return qReader.GetMetrics(id, phi); }
            float Recency(RE::FormID id) const { return usageReader.GetRecencyBoost(id); }
            ScoredCandidate Score(const Candidate::CandidateVariant& c, float contextWeight,
                const Learning::FeatureItemMetrics& metrics, float recencyBoost) const
            {
                return self.ScoreCandidateInternal(c, state, player, targets, world, combatOnsetAgo, weights,
                    contextWeight, metrics, recencyBoost);
            }
            float Utility(const ScoreBreakdown& b) const { return self.ComputeUtility(b); }
            bool HardGated(const Candidate::CandidateVariant& c) const { return Context::IsHardContextGated(c); }
            float Ucb(const Learning::FeatureItemMetrics& m) const { return m.ucb; }
        };
        Hooks hooks{ *this, state, player, targets, world, combatOnsetAgo, weights, phi, qReader, usageReader };
        const Legacy::FloorParams floorParams{
            .minimumContextWeight = m_config.minimumContextWeight,
            .minimumUtility = m_config.minimumUtility,
            .topNCandidates = m_config.topNCandidates,
            .coldStartUCBBoost = m_config.coldStartUCBBoost,
        };
        const auto pass = Legacy::FloorPass(candidates, floorParams, hooks, scored, outLegacy);
        const float maxUtilitySeen = pass.maxUtilitySeen;

        // Diagnostic, rate-limited. R8.3: no longer an empty page (every
        // candidate is ranked on the choice score); it means no row gets the
        // D2 feature this run.
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
            // Rate-limited: same cadence as the diagnostic above
            // NOTE: Single-threaded (called from the update thread only)
            static auto s_lastColdStartLog = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_lastColdStartLog).count() >= 5) {
                logger::debug("[UtilityScorer] Legacy pass cold start: {} candidates boosted via UCB context floor ({:.2f})"sv,
                    pass.coldStartCount, m_config.coldStartUCBBoost);
                s_lastColdStartLog = now;
            }
        }

        // R8.3: no favourites rank scaling and no potion tier preference here
        // any more. They only ever reordered the ranking, and nothing ranks on
        // utility now (the fallback ranking without a bootstrap included); the
        // utility a row carries is the floor pass's, which the D2 feature was
        // fitted on. ApplyFavoritesRankScaling / ApplyPotionTierPreference go
        // in R8's PR 4 with their INI keys.
        return scored;
    }

    void UtilityScorer::RankAndApplyWildcards(ScoredCandidateList& scored, const WildcardPage& displayPage)
    {
        // The whole list in rank order (R7), on SlotScore's score (R8.3: the
        // choice score mu; ln utility in the fallback ranking) -- until
        // ApplyWildcards below swaps a wildcard up into a rank position. The
        // slot code takes the FIRST match in list order for a job key or an
        // uncapped Regular key, so the tail must be sorted too. Stable, so
        // exact ties (same score, same DPS) keep the order the candidates were
        // generated in. Cost (R7 review, 500 rows): about 42 us a pass.
        if (!scored.empty()) {
            Huginn_ZONE_NAMED("Score::Sort");
            std::stable_sort(scored.begin(), scored.end());
        }

        // Apply wildcards for exploration, keyed and sized against the page
        // THIS tick allocates. WildcardManager works on rank POSITIONS (a draw
        // from below the slot, a swap up into it), never on score values, so
        // it is sign-safe as it stands (R8.3); R10 replaces it.
        {
            Huginn_ZONE_NAMED("Score::Wildcards");
            m_wildcardMgr.ApplyWildcards(scored, displayPage);
        }
    }

    void UtilityScorer::ApplyPotionTierPreference(ScoredCandidateList& scored) const
    {
        // Family identity: everything that makes two potions "the same potion
        // at a different strength". Different skills or schools are different
        // potions even under one tag (FortifyCombatSkill: Block vs Archery).
        using Key = std::tuple<Item::ItemType, Item::ItemTag, Item::ItemTagExt,
            Item::MagicSchool, Item::CombatSkill, Item::UtilitySkill>;
        struct Member { size_t index; float strength; };
        std::map<Key, std::vector<Member>> families;

        for (size_t i = 0; i < scored.size(); ++i) {
            const auto* item = std::get_if<Candidate::ItemCandidate>(&scored[i].candidate);
            if (!item) continue;
            switch (item->type) {
            case Item::ItemType::HealthPotion:
            case Item::ItemType::MagickaPotion:
            case Item::ItemType::StaminaPotion:
                // The override's measure, so "Higher" means the same potion in
                // a slot as the CRITICAL override would hand you
                families[{ item->type, item->tags, item->tagsExt, item->school,
                           item->combatSkill, item->utilitySkill }]
                    .push_back({ i, Item::RestoredWithin(item->magnitude, item->duration,
                                     Override::Defaults::URGENT_RESTORE_WINDOW_SEC) });
                break;
            case Item::ItemType::ResistPotion:
            case Item::ItemType::BuffPotion:
            case Item::ItemType::CurePotion:
                families[{ item->type, item->tags, item->tagsExt, item->school,
                           item->combatSkill, item->utilitySkill }]
                    // Magnitude x duration: a buff's strength can live in
                    // either. Waterbreathing is magnitude 0 at every strength
                    // and differs only in duration (180/240/300 s on LoreRim),
                    // so magnitude alone called Faint and Fair equal. With
                    // neither -- a cure -- the gold value is the only potency
                    // there is: LoreRim's Cure Disease (value 100) now cures
                    // within 1-4 days, its Cure Greater Disease (200) at once,
                    // and both read 'Cure Disease' 0/0s.
                    .push_back({ i, (item->magnitude <= 0.0f && item->duration <= 0.0f)
                        ? static_cast<float>(item->value)
                        : std::max(item->magnitude, 1.0f) * std::max(item->duration, 1.0f) });
                break;
            default:
                break;
            }
        }

        const auto preference = m_config.potionTierPreference;
        if (preference == PotionTierPreference::None) {
            return;  // natural scores stand; the slot hold stops near-ties trading
        }
        for (auto& [key, members] : families) {
            if (members.size() < 2) continue;

            float familyTop = 0.0f;
            for (const auto& m : members) {
                familyTop = std::max(familyTop, scored[m.index].utility);
            }

            // Strongest first for Higher, weakest first for Lower. Equal
            // strengths share a utility -- nothing to prefer between them.
            std::sort(members.begin(), members.end(), [preference](const Member& a, const Member& b) {
                return preference == PotionTierPreference::Lower ? a.strength < b.strength
                                                                 : a.strength > b.strength;
            });

            float u = familyTop;
            for (size_t k = 0; k < members.size(); ++k) {
                if (k > 0 && members[k].strength != members[k - 1].strength) {
                    u /= POTION_TIER_STEP;
                }
                auto& entry = scored[members[k].index];
                if (entry.utility > 0.0f) {
                    // Keep the [Recs] breakdown honest about why this moved
                    entry.breakdown.potionMultiplier *= u / entry.utility;
                }
                entry.utility = u;
            }
        }
    }

    // =========================================================================
    // INTERNAL SCORING IMPLEMENTATION
    // =========================================================================

    ScoredCandidate UtilityScorer::ScoreCandidateInternal(
        const Candidate::CandidateVariant& candidate,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& /*world*/,
        float combatOnsetAgo,
        const Context::ContextWeightMap& /*weights*/,
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
        result.breakdown.rewardEstimate = metrics.rewardEstimate;
        result.breakdown.ucb = metrics.ucb;
        result.breakdown.confidence = metrics.confidence;

        // =====================================================================
        // Step 3: Calculate prior from PriorCalculator
        // =====================================================================
        result.breakdown.prior = [&] {
            Huginn_ZONE_NAMED("Score::Prior");
            return m_priorCalc.CalculatePrior(player, candidate);
        }();

        // =====================================================================
        // Step 4: Compute learning score: α*R + (1-α)*prior + β*UCB
        // =====================================================================
        float alpha = result.breakdown.confidence;
        float beta = m_config.explorationWeight;

        result.breakdown.learningScore =
            alpha * result.breakdown.rewardEstimate +
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
        result.breakdown.correlationBonus = [&] {
            Huginn_ZONE_NAMED("Score::Correlation");
            return m_correlationBooster.CalculateBonus(player, targets, candidate);
        }();

        // =====================================================================
        // Step 6: Get potion multiplier (1.0 for non-potions)
        // =====================================================================
        result.breakdown.potionMultiplier =
            m_potionDiscrim.GetMultiplier(state, player, candidate, combatOnsetAgo);

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
        // full sort establishes the final combined order).
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

    bool UtilityScorer::Update(float deltaSeconds)
    {
        (void)deltaSeconds;   // R8.1: the PotionDiscriminator's timer, its one reader, is gone
        bool forceRun = m_wildcardMgr.UpdateExpiry();

        // Vital envelopes tick every frame, not just when the pipeline runs: a
        // hold ending, or a potion, changes the context weights without
        // changing anything the skip gate hashes. Release -> one re-run.
        const auto vitals = State::StateManager::GetSingleton().GetPlayerVitals();
        const auto now = Context::VitalEnvelope::Clock::now();
        forceRun |= m_magickaEnvelope.Tick(vitals.magicka, now);
        forceRun |= m_staminaEnvelope.Tick(vitals.stamina, now);
        return forceRun;
    }

    void UtilityScorer::Reset()
    {
        m_wildcardMgr.Reset();
        m_magickaEnvelope.Reset();
        m_staminaEnvelope.Reset();
    }

    void UtilityScorer::SetContextWeightConfig(const State::ContextWeightConfig& config)
    {
        m_contextEngine.SetConfig(config);
    }

    // =========================================================================
    // DEBUG LOGGING
    // =========================================================================

    void UtilityScorer::LogTopCandidates(
        const ScoredCandidateList& ranked, size_t count, bool detail, bool force, bool fallback) const
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

        // R8.3: the ranking is the choice score mu. Each line: mu (and sd),
        // the three largest theta[need][col] n a terms (the first is the
        // explanation), b, and the D2 legacy feature (lm = ln learn mult on a
        // row the old floors kept, "um" when they did not), then the old
        // utility and its parts on the rows the legacy pass kept, for
        // comparison only. The fallback ranking (no bootstrap) prints the old
        // line, flagged. Phase 9 / R11 expand this.
        logger::info("[Recs] === Top {}/{} candidates | {} ==="sv, numToLog, ranked.size(),
            fallback ? "FALLBACK: s = ln u, u = ctx*(1+λ*learn)*mults"
                     : "mu = Σ θ·n·cap + b + w·lm (+ recent, covered); terms need×col");

        for (size_t i = 0; i < numToLog; ++i) {
            const auto& scored = ranked[i];
            const auto& bd = scored.breakdown;
            const auto& p = scored.choice;

            std::string terms;
            for (const auto& t : p.top) {
                if (t.need < 0) break;
                const auto ni = static_cast<size_t>(t.need);
                const auto ci = static_cast<size_t>(t.col);
                terms += fmt::format("{}{}×{}={:+.3f}", terms.empty() ? "" : " ",
                    ni < Core::Needs::kNeedCount ? Core::Needs::kNeeds[ni].id : std::string_view("?"),
                    ci < Core::Effect::kColumnCount ? Core::Effect::kColumns[ci].id : std::string_view("?"), t.value);
            }
            if (terms.empty()) terms = "learned";
            const std::string legacy = scored.legacyScored
                ? fmt::format(" | old u={:.3f} {}", scored.utility, detail ? bd.ToDetailString() : bd.ToCompactString())
                : std::string(" | old: under the floors");

            if (fallback) {
                logger::info("[Recs] {}. {} ({}) s={:+.3f} u={:.3f} | {}{}{}"sv,
                    i + 1, scored.GetName(), Candidate::SourceTypeToString(scored.GetSourceType()),
                    scored.score, scored.utility,
                    detail ? bd.ToDetailString() : bd.ToCompactString(),
                    scored.isWildcard ? " [WC]" : "",
                    scored.isColdStartBoosted ? " [COLD]" : "");
                continue;
            }
            logger::info("[Recs] {}. {} ({}) mu={:+.3f} sd={:.3f} | {} | need={:+.3f} b={:+.3f} lm={:+.3f}{}{}{}{}{}"sv,
                i + 1,
                scored.GetName(),
                Candidate::SourceTypeToString(scored.GetSourceType()),
                scored.score, std::sqrt(std::max(p.var, 0.0)),
                terms, p.needMu, p.b, p.lnLearnMult, p.utilMissing != 0.0 ? " um" : "",
                p.covered > 0.0 ? fmt::format(" covered={:.2f}", p.covered) : std::string{},
                legacy,
                scored.isWildcard ? " [WC]" : "",
                scored.isColdStartBoosted ? " [COLD]" : "");
        }
    }

}  // namespace Huginn::Scoring
