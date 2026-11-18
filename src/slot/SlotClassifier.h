#pragma once

#include "SlotConfig.h"
#include "candidate/CandidateTypes.h"
#include "core/CapFacets.h"
#include "learning/ScoredCandidate.h"

namespace Huginn::Slot
{
    // =============================================================================
    // SLOT CLASSIFIER
    // =============================================================================
    // Whether a candidate matches a slot's classification.
    //
    // R11: re-derived from cap(i). The rules are core/CapFacets.h's Matches and
    // Classify (pure, host-tested in tests/core/CapFacetsTests.cpp), over the
    // effect catalog's facets of the candidate's form (effect/Facets.h). The old
    // spell / item / scroll classifiers' types and tags are not read here any
    // more: they feed only the D2 legacy feature. docs/architecture/5-slots.md
    // ("Slot classes, re-derived from cap(i)") reviews each class against what
    // the classifiers did.
    // =============================================================================

    class SlotClassifier
    {
    public:
        /// The subject of the class questions: the row's own (filled once per
        /// pipeline run), or looked up now.
        [[nodiscard]] static Core::Facets::Subject SubjectOf(const Scoring::ScoredCandidate& candidate);

        // Check if a candidate matches a classification
        [[nodiscard]] static bool Matches(
            const Scoring::ScoredCandidate& candidate,
            SlotClassification classification);

        // Check if a candidate variant matches a classification (looks it up)
        [[nodiscard]] static bool Matches(
            const Candidate::CandidateVariant& candidate,
            SlotClassification classification);

        // Get the best classification for a candidate (for debugging/display)
        [[nodiscard]] static SlotClassification Classify(
            const Scoring::ScoredCandidate& candidate);
    };

}  // namespace Huginn::Slot
