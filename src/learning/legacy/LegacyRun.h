#pragma once

// =============================================================================
// LEGACY RUN (R8.4) -- the pipeline's one handle on the legacy pass
// =============================================================================
// *** Part of the legacy module R8 KEEPS for D2 (see LegacyLearnFeature.h). ***
//
// R8.4 confined the old engine to an allowlist (tools/prune/r8_check.py): the
// pipeline names it in one call, Legacy::LegacyPass (below; the pass itself is
// LegacyScorer.h, split out of UtilityScorer), and reads everything else
// through this header:
//   - Run: what one pass produced -- the rows the old floors kept (their
//     utility and breakdown, for the v2 log and the D2 feature), the floors'
//     verdict per candidate, the row each candidate reads (its own, or the
//     kept row of its dedup key), and the context weights the pass used;
//   - OldScoreCapture: the selection log v3's OldScore columns (0.23.53), the
//     audit trail a refit needs to retire this module: the old engine's
//     score parts of every candidate, whatever the floors;
//   - Run::FallbackRanking: the R8.3 fallback ranking, ln utility over the
//     kept rows, used only while the choice model is not live (no bootstrap,
//     or an invalid one; an error line says so). The one place utility still
//     ranks; it lives here because the pass is here anyway;
//   - the old learner's play clock, forget sweep and counts (UpdateLoop, the
//     [Soak] heartbeat), which run whether or not the switch is on, so BNDW
//     keeps ageing.
// With the choice model live nothing here ranks: the choice score reads the D2
// feature off Run's rows.
// =============================================================================

#include "LegacyLearnFeature.h"
#include "candidate/CandidateTypes.h"
#include "context/ContextRuleEngine.h"   // the weight map type the pass used
#include "core/OldScore.h"
#include "learning/ScoredCandidate.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Huginn::State
{
    struct GameState;
    struct PlayerActorState;
    struct TargetCollection;
    struct WorldState;
}

namespace Huginn::Scoring::Legacy
{
    /// One pipeline run's legacy pass (LegacyPass below), or a skipped one
    /// (bLegacyLearnFeature off with the choice model live).
    struct Run
    {
        bool ran = false;
        ScoredCandidateList kept;          // the rows the floors kept, in the floor pass's order
        std::vector<Row> rows;             // per candidate (index-aligned with the candidates)
        std::vector<std::size_t> rowOf;    // per candidate: the row it reads (see ResolveRows)
        Context::ContextWeightMap weights{};

        void Clear();
        /// The switch is off: no pass ran; every candidate reads an empty Row
        /// (floor ctx, lm 0, um 1).
        void Skip(std::size_t candidates);

        /// Each candidate reads its own row, or, when the floors kept another
        /// candidate of its dedup key (type, uid, form) and not it, that one's
        /// (R8.2's CaptureEligible rule, so the score and the v3 row agree;
        /// the floor pass's cold start scores only the first of a form).
        void ResolveRows(const std::vector<Candidate::CandidateVariant>& candidates);

        /// The row candidate i reads, or an empty Row.
        [[nodiscard]] Row RowFor(std::size_t i) const noexcept;
        /// The kept row candidate i reads (utility, breakdown), or nullptr.
        [[nodiscard]] const ScoredCandidate* KeptFor(std::size_t i) const noexcept;
        /// Kept rows, for the [Pipeline] and capture counts.
        [[nodiscard]] std::size_t KeptCount() const noexcept { return kept.size(); }

        /// The R8.3 FALLBACK ranking (the bootstrap is missing or invalid): a
        /// copy of the kept rows, each flagged legacyScored and ranked on
        /// ln utility (ScoredCandidate::RankOnUtility). Unsorted; the live
        /// scorer sorts it. The only place utility still ranks.
        [[nodiscard]] ScoredCandidateList FallbackRanking() const;
    };

    /// THE pipeline's one call into the old engine (R8.4): the legacy pass
    /// (LegacyScorer::Pass) over this run's candidates, on the live scorer's
    /// held magicka / stamina. A skipped Run if the scorer was never built.
    [[nodiscard]] Run LegacyPass(
        const std::vector<Candidate::CandidateVariant>& candidates,
        const State::GameState& state,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world,
        float combatOnsetAgo);

    /// The selection log v3's OldScore columns (0.23.53) for one run: what the
    /// old engine computes for a candidate whatever the floors. A kept row
    /// takes the parts its utility used (its breakdown); any other candidate,
    /// and every filter drop, is put through the legacy scorer's own
    /// per-candidate score again with no floor (LegacyScorer::ExplainCandidate).
    /// Reads only. Opens the old learner's and UsageMemory's readers for its
    /// lifetime when the pass ran.
    class OldScoreCapture
    {
    public:
        OldScoreCapture(const Run& run,
            const State::GameState& state, const State::PlayerActorState& player,
            const State::TargetCollection& targets, const State::WorldState& world,
            float combatOnsetAgo);
        ~OldScoreCapture();
        OldScoreCapture(const OldScoreCapture&) = delete;
        OldScoreCapture& operator=(const OldScoreCapture&) = delete;

        /// The pass ran and the legacy scorer exists (the old parts are captured at all).
        [[nodiscard]] bool Valid() const noexcept;
        /// The run head: the floors' parameters, the learner totals and the
        /// state features. `scored` = the kept rows that are ranked.
        [[nodiscard]] Core::DecisionLog::OldRun Head(int scored) const;
        /// One candidate's parts. `kept`: its kept row (the parts its utility
        /// used), or nullptr; `eligible`: a candidate of the run (it gets a
        /// floor), false for a filter drop.
        [[nodiscard]] Core::DecisionLog::OldScore Explain(const Candidate::CandidateVariant& c, bool eligible,
            const ScoredCandidate* kept) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    /// The old learner's play clock (loaded, unpaused play), forget sweep and
    /// counts. They run whatever bLegacyLearnFeature says, so BNDW ages and
    /// stays bounded while the switch is off.
    [[nodiscard]] bool HasLearner() noexcept;
    void AdvancePlayTime(float deltaSeconds);
    void MaybeForgetFaded();
    struct LearnerCounts
    {
        std::size_t items = 0;
        uint32_t trains = 0;
        uint64_t forgot = 0;
    };
    [[nodiscard]] LearnerCounts Counts();
    /// The old learner's weights-changed latch: a pick it trained on moves
    /// the D2 feature (ln learn mult), so the pipeline re-scores.
    [[nodiscard]] bool WeightsChanged() noexcept;
    void ClearWeightsChanged() noexcept;
}
