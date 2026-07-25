#pragma once

#include "CandidateTypes.h"             // Candidate::RelevanceTag
#include "state/WorldState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"     // State::TargetCollection
#include "state/StateTypes.h"           // Health/Magicka/Stamina TrackingState

namespace Huginn::Candidate
{
    // =========================================================================
    // RELEVANCE TAGS (architecture-critique #10)
    // =========================================================================
    // Per-tick display relevance tags derived from game state. Their SOLE consumer
    // is the Wheeler subtext explanation label (Slot::DeriveExplanationLabel) — NOT
    // candidate filtering (CandidateFilters never reads relevanceTags). Computed
    // once per tick by the pipeline and stashed on PipelineContext, rather than
    // copied identically onto every candidate as it was when this lived in
    // CandidateGenerator. Override candidates still carry their own specific tag;
    // the label falls back to this per-tick set for everything else.
    //
    // NOTE: this intentionally re-derives context flags with hard thresholds
    // (IsHealthLow, etc.) for a compact label vocabulary; ContextRuleEngine's
    // continuous weights are a separate encoding used for scoring.
    // =========================================================================
    [[nodiscard]] RelevanceTag ComputeRelevanceTags(
        const State::WorldState& world,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::HealthTrackingState& healthTracking,
        const State::MagickaTrackingState& magickaTracking,
        const State::StaminaTrackingState& staminaTracking);
}
