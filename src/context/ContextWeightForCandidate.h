#pragma once

#include "candidate/CandidateTypes.h"   // Candidate::CandidateVariant
#include "context/ContextRuleEngine.h"  // Context::ContextWeightMap

namespace Huginn::Context
{
    // =========================================================================
    // CONTEXT WEIGHT FOR CANDIDATE (architecture-critique #10)
    // =========================================================================
    // Maps a candidate's type/tags to the relevant weight from a ContextWeightMap,
    // combining multiple applicable weights with std::max (e.g. AOE + Damage).
    //
    // Pure function of (candidate, weights) — moved out of UtilityScorer so the
    // context → candidate weight mapping lives in the Context layer rather than
    // the scoring god-file. Context changes now live entirely under src/context/.
    // =========================================================================
    [[nodiscard]] float WeightForCandidate(
        const Candidate::CandidateVariant& candidate,
        const ContextWeightMap& weights);
}
