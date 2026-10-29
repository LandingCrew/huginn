#pragma once

// =============================================================================
// LEGACY LEARN FEATURE (R8.2) -- the part of the old score R8 keeps (D2)
// =============================================================================
// *** This module isolates old-scorer code that R8 KEEPS. ***
//
// R6 chose D2 (configs/Huginn_Bootstrap.json, spec "theta + b + recent + learn
// mult (D2)"): no variant without the old scorer passed R6's gate (L1, the old
// learner's state alone, pools -5.0 [-9.1, -1.0] online), so the new model
// reads one feature of the old engine, on the rows the old floors pass:
//
//   ln learn mult = ln(max(1 + lambda(alpha) * learningScore, 1e-6))
//   util missing  = 1 when the old floors dropped the row (not pass, not cold)
//
// (docs/architecture/9-bootstrap-format.md, features). Which rows pass is
// decided by the old floors, and they need the whole old utility:
//   fMinimumContextWeight  against ContextRuleEngine's weight (favourites exempt)
//   fMinimumUtility        against ctx (1 + lambda learn) corr potion fav_provisional
//   fColdStartUCBBoost     the cold-start rescue below iTopNCandidates rows
// so FeatureBanditLearner, UsageMemory, PriorCalculator, CorrelationBooster,
// PotionDiscriminator::GetMultiplier, lambda, the context weights and the
// cold start stay live, as inputs to this feature only (R8.3 ranks by the new
// score). The floor loop and the cold-start rescue moved here from
// UtilityScorer::ScoreCandidates UNCHANGED IN ARITHMETIC: the same tests in the
// same order on the same values; UtilityScorer::LegacyPass calls FloorPass
// with its own ScoreCandidateInternal.
//
// The switch: [Scoring] bLegacyLearnFeature (default true, because the
// bootstrap's weight w = 1.36 was fitted with it). Off, the feature is 0 and
// util missing 1 on every row, so the model is plain theta + b + recent; that
// variant fails R6's gate, so the switch is a fallback, not a supported mode.
// R8.2 read the key and logged it. R8.3 (0.24.0) makes it a LIVE SCORE INPUT:
// the pipeline ranks every candidate on the choice score, which reads this
// feature (w = 1.36) on the rows the floors keep. ContextRuleEngine still runs
// every pipeline pass, but only to decide which rows the feature is computed
// on; none of this module's output ranks anything by itself.
//
// Retiring this code needs a refit on 0.23.53+ logs (the v3 rows log the old
// learner on every row) where a variant without the old scorer passes the gate.
// =============================================================================

#include "core/ChoiceLive.h"
#include "core/OldScore.h"
#include "Profiling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Huginn::Scoring::Legacy
{
    /// The old floors' parameters (ScorerConfig's minimumContextWeight,
    /// minimumUtility, topNCandidates, coldStartUCBBoost).
    struct FloorParams
    {
        float minimumContextWeight = 0.0f;
        float minimumUtility = 0.0f;
        std::size_t topNCandidates = 0;
        float coldStartUCBBoost = 0.0f;
    };

    /// What the floors said of one candidate, and the feature it gives.
    struct Row
    {
        Core::DecisionLog::Floor floor = Core::DecisionLog::Floor::ctx;
        float lnLearnMult = 0.0f;   // pass / cold rows; 0 otherwise
        bool utilMissing = true;    // !(pass || cold)
        int scoredIndex = -1;       // R8.3: its row in FloorPass's `scored` (pass / cold), else -1
        [[nodiscard]] bool Scored() const noexcept
        {
            return floor == Core::DecisionLog::Floor::pass || floor == Core::DecisionLog::Floor::cold;
        }
    };

    /// ln(max(1 + lambda * learningScore, 1e-6)), in double as the fit reads it.
    [[nodiscard]] inline float LnLearnMult(float lambda, float learningScore) noexcept
    {
        const double m = 1.0 + static_cast<double>(lambda) * static_cast<double>(learningScore);
        return static_cast<float>(std::log(std::max(m, 1e-6)));
    }

    /// The feature as the new score reads it: the row's values with the switch
    /// on; 0 and 1 with it off.
    struct Value
    {
        double lnLearnMult = 0.0;
        double utilMissing = 1.0;
    };
    [[nodiscard]] inline Value Feature(const Row& r, bool enabled) noexcept
    {
        // The one rule (core/ChoiceLive.h), shared with the replay test. A
        // kept row has utilMissing false by construction (FloorPass's keep).
        const auto v = Core::Choice::LegacyFeature(enabled, r.Scored(), static_cast<double>(r.lnLearnMult));
        return Value{ v.lnLearnMult, v.utilMissing };
    }

    struct PassResult
    {
        float maxUtilitySeen = 0.0f;   // for UtilityScorer's "0 candidates passed" warning
        std::size_t coldStartCount = 0;
        bool coldStartRan = false;
    };

    // -------------------------------------------------------------------------
    // FloorPass -- the old pass's floor loop and cold-start rescue
    // -------------------------------------------------------------------------
    // Hooks (UtilityScorer's; every call is the one ScoreCandidates made):
    //   bool  Favorited(c)                       Candidate::IsFavorited
    //   float ContextWeight(c)                   Context::WeightForCandidate(c, weights)
    //   uint32 FormID(c)
    //   Metrics LearnerMetrics(form)             the locked learner reader's GetMetrics(form, phi)
    //   float Recency(form)                      the usage reader's GetRecencyBoost(form)
    //   Scored Score(c, ctxWeight, metrics, rec) ScoreCandidateInternal
    //   float Utility(breakdown)                 ComputeUtility
    //   bool  HardGated(c)                       Context::IsHardContextGated
    //   float Ucb(metrics)                       metrics.ucb
    // `scored` receives the rows the floors keep, in the order the old code
    // pushed them; `rows`, when given, one Row per candidate (index-aligned).
    template <class Cand, class Scored, class Hooks>
    PassResult FloorPass(const std::vector<Cand>& candidates, const FloorParams& p, Hooks& h,
                         std::vector<Scored>& scored, std::vector<Row>* rows)
    {
        using Core::DecisionLog::Floor;
        PassResult res;
        if (rows) rows->assign(candidates.size(), Row{});
        auto keep = [&](std::size_t i, const Scored& s, Floor f) {
            if (!rows) return;
            auto& r = (*rows)[i];
            r.floor = f;
            r.lnLearnMult = LnLearnMult(s.breakdown.lambda, s.breakdown.learningScore);
            r.utilMissing = false;
            r.scoredIndex = static_cast<int>(scored.size());   // pushed right after keep()
        };

        // Score all candidates
        float maxUtilitySeen = 0.0f;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            Huginn_ZONE_NAMED("Score::Candidate");
            // Early filter: Skip candidates with very low context weight
            // Favorites always pass — they represent explicit player intent and must
            // remain observable by the learner even when context weight is low.
            //
            // R8.1 (0.23.55): no Suppress skip here any more. A favourite under
            // Suppress never reaches the scorer: the favorite_suppressed filter
            // drops it (CandidateFilters, Core::Gate::FavoriteSuppressed).
            const bool favorited = h.Favorited(candidate);
            float contextWeight = h.ContextWeight(candidate);
            if (contextWeight < p.minimumContextWeight && !favorited) {
                if (rows) (*rows)[i].floor = Floor::ctx;
                continue;
            }

            const auto formID = h.FormID(candidate);
            const auto metrics = [&] {
                Huginn_ZONE_NAMED("Score::LearnerMetrics");
                return h.LearnerMetrics(formID);
            }();
            float recencyBoost = h.Recency(formID);

            Scored result = h.Score(candidate, contextWeight, metrics, recencyBoost);

            maxUtilitySeen = std::max(maxUtilitySeen, result.utility);

            // Post-filter: Skip candidates with very low utility
            if (result.utility >= p.minimumUtility) {
                keep(i, result, Floor::pass);
                scored.push_back(std::move(result));
            }
            else if (rows) {
                (*rows)[i].floor = Floor::util;
            }
        }
        res.maxUtilitySeen = maxUtilitySeen;

        // Cold-start fallback: if too few candidates passed to fill slots, boost context
        // via UCB so untried items can surface. Triggers when scored < topNCandidates (10),
        // which covers both "Top 0" (empty weight table) and "Top 1" (only 1 item has real
        // context weight, e.g. a favorited weapon). The fallback self-heals as UCB decays.
        if (scored.size() < p.topNCandidates && p.coldStartUCBBoost > 0.0f) {
            Huginn_ZONE_NAMED("Score::ColdStart");
            res.coldStartRan = true;
            // Dedup against already-scored candidates by linear scan — this branch
            // only runs when scored.size() < topNCandidates (<10), so a scan beats
            // allocating a hash set every cold-start tick.
            const auto alreadyScored = [&scored](auto id) {
                for (const auto& s : scored) {
                    if (s.GetFormID() == id) return true;
                }
                return false;
            };

            std::size_t coldStartCount = 0;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                const auto& candidate = candidates[i];
                const auto formID = h.FormID(candidate);
                if (alreadyScored(formID)) continue;

                // R8.1 (0.23.55): no Suppress skip here either: a suppressed
                // favourite is a filter drop (favorite_suppressed) and never a
                // candidate, so it cannot come back as a cold-start pick.

                float contextWeight = h.ContextWeight(candidate);

                // A closed hard gate is not a cold start. For a source with no
                // baseline (apparel) a sub-minimum context weight means the
                // context FORBIDS the candidate, not that nobody has tried it —
                // and an untried piece has a high UCB by construction, so the
                // floor below would clear minimumContextWeight and put a fortify
                // ring in a dungeon slot. Above the minimum the gate is open and
                // the boost applies as it does everywhere else.
                if (h.HardGated(candidate) && contextWeight < p.minimumContextWeight) {
                    continue;
                }

                // UCB-driven context floor for untried/low-visit items (Phase 3.5c: FeatureBanditLearner)
                // Uses locked reader — no per-candidate lock acquisition
                auto metrics = h.LearnerMetrics(formID);
                float boostedContext = std::max(contextWeight, p.coldStartUCBBoost * h.Ucb(metrics));

                if (boostedContext < p.minimumContextWeight) continue;

                float recencyBoost = h.Recency(formID);

                // Score candidate normally, then recompute utility with boosted context
                Scored result = h.Score(candidate, contextWeight, metrics, recencyBoost);

                // Override context weight and recompute utility with the boosted
                // value via the shared formula helper (reads contextWeight from
                // the breakdown, so the assignment above must come first).
                result.breakdown.contextWeight = boostedContext;
                result.utility = h.Utility(result.breakdown);

                if (result.utility >= p.minimumUtility) {
                    result.isColdStartBoosted = true;
                    keep(i, result, Floor::cold);
                    scored.push_back(std::move(result));
                    ++coldStartCount;
                }

                if (scored.size() >= p.topNCandidates) break;
            }
            res.coldStartCount = coldStartCount;
        }
        return res;
    }

    /// [Scoring] bLegacyLearnFeature (R8.3: live and hot-reloaded). On, the
    /// pipeline runs the legacy pass (the context rules, the prior, the
    /// correlations, the potion multiplier, the floors) and the feature reads
    /// it; off, none of that runs and the feature is 0 / 1 on every row. The
    /// old learner and UsageMemory keep training either way, so switching back
    /// on is not stale.
    [[nodiscard]] bool Enabled() noexcept;
    /// Returns the previous value; a change logs one line.
    bool SetEnabled(bool on) noexcept;
}
