#pragma once

// =============================================================================
// OLD SCORE (0.23.53) -- the old engine's score parts, as the v3 log records them
// =============================================================================
// The component test of R6 found that R8 passes the gate only by keeping the
// old learner's learningScore on the rows the old floors pass, and that the
// logs could not say whether the old floors or stale imputation cause the
// rest of the gap. So the selection log v3 records, on every held or eligible
// row, what the old engine computes for it whatever the floors: the learner's
// estimate, confidence, evidence and UCB, the recency boost, the prior, the
// context weight, which floor kept or dropped it and, for an unscored
// candidate, the utility it would have had. The schema:
// docs/architecture/9-selection-log-v3.md (row columns est..wu, fl).
//
// LOGGING ONLY: the scorer decides exactly as before; these are read off it
// (Legacy::OldScoreCapture, LegacyScorer::ExplainCandidate; R8.4).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Huginn::Core::DecisionLog
{
    /// 0.23.53, row column "fl": how the old engine's floors treated an
    /// eligible row in its pipeline run (UtilityScorer::ScoreCandidates).
    enum class Floor : std::uint8_t
    {
        pass,         // scored by the main loop (context weight and utility over the floors)
        cold,         // scored by the cold-start rescue (context raised to coldBoost x UCB)
        ctx,          // under fMinimumContextWeight (not a favourite), and not rescued
        util,         // over the context floor, under fMinimumUtility
        suppressed,   // a favourite under favourites mode Suppress (never scored)
        _Count
    };
    inline constexpr std::size_t kFloorCount = static_cast<std::size_t>(Floor::_Count);
    inline constexpr std::array<std::string_view, kFloorCount> kFloorNames{ "pass", "cold", "ctx", "util", "suppressed" };
    inline constexpr std::uint8_t kNoFloor = 255;   // not an eligible row of the run

    inline constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

    /// 0.23.53: the old engine's score parts for one row (logging only; the
    /// row columns est..wu). For a candidate of the run (eligible, or dropped
    /// by a filter) they are the run's: the learner metrics against that
    /// run's state features, the prior, the raw context weight, and for an
    /// unscored eligible or filtered row the utility the scorer computes for
    /// it with no floor. For a held row the run never gathered, only the
    /// learner metrics and the recency, read when the context is taken
    /// (against the run's state features and state hash); the rest none.
    struct OldScore
    {
        float est = kNaN;         // FeatureBanditLearner: w . phi
        float alpha = kNaN;       // its confidence n_eff / (n_eff + n0)
        float trains = kNaN;      // its stored evidence n, before fading (n_eff = n x retention)
        float ucb = kNaN;         // its exploration bonus
        float rec = kNaN;         // UsageMemory's recency boost
        float prior = kNaN;       // PriorCalculator
        float ctx = kNaN;         // the context weight (ContextRuleEngine), before a cold-start boost
        float wouldUtil = kNaN;   // an unscored candidate: the scorer's utility with no floor
        std::uint8_t floor = kNoFloor;
        bool favorite = false;    // the candidate's favourites flag (spells, weapons): ORed into Flag::Favorite
        [[nodiscard]] bool HasLearner() const noexcept { return !std::isnan(est); }
    };

    /// 0.23.53: the old engine's pipeline run behind a context's eligible
    /// rows (the ctx object "old"): the floors and scorer parameters it used,
    /// how many rows it kept, the learner's total evidence and the state
    /// features the learner read.
    struct OldRun
    {
        bool valid = false;
        float minCtx = 0.0f, minUtil = 0.0f;   // fMinimumContextWeight, fMinimumUtility
        int topN = 0;                          // iTopNCandidates: the cold start runs below it
        float coldBoost = 0.0f;                // the cold start's context floor = coldBoost x UCB
        float beta = 0.0f;                     // learn = a*est + (1-a)*prior + beta*ucb + rec
        float lambdaMin = 0.0f, lambdaMax = 0.0f;   // lambda(a) = lambdaMin + a*(lambdaMax - lambdaMin)
        float n0 = 0.0f;                       // the learner's pseudo-observations: a = n_eff / (n_eff + n0)
        std::string favMode;                   // "boost" / "off" / "suppress"
        float favMin = 0.0f, favMax = 0.0f;    // the favourites multiplier's range
        int scored = 0;                        // rows the run kept (past the floors)
        float totalTrains = 0.0f;              // the learner's total n (the UCB's)
        std::vector<float> phi;                // the state features the learner read
    };
}
