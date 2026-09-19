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

    // =========================================================================
    // HARD CONTEXT GATE (#65)
    // =========================================================================
    // True for sources whose WeightForCandidate arm has NO baseline: a zero from
    // them means "the context forbids this", not "the context has nothing to say
    // about it yet". Every other arm floors at baseRelevanceWeight, so its zero
    // can only mean the latter.
    //
    // The distinction exists for UtilityScorer's cold-start pass, which re-admits
    // skipped candidates at max(contextWeight, coldStartUCBBoost * ucb). That
    // floor is the right answer for an untried spell nobody has cast; applied to
    // apparel it silently undoes the gate, because an untried piece has a high
    // UCB by definition and 0.2 * ucb clears minimumContextWeight. The observable
    // result is fortify-crafting gear offered in a dungeon -- the exact noise the
    // no-baseline arm exists to prevent.
    [[nodiscard]] bool IsHardContextGated(const Candidate::CandidateVariant& candidate) noexcept;
}
