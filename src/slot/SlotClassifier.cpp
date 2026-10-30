#include "SlotClassifier.h"
#include "effect/Facets.h"

namespace Huginn::Slot
{
    // Tripwire: a new SlotClassification needs an arm in core/CapFacets.h's
    // Matches and a place in its kClassPriority (whose own static_assert
    // fails first). Bump the count here after auditing them.
    static_assert(SLOT_CLASSIFICATION_COUNT == 24,
        "SlotClassification changed -- audit core/CapFacets.h Matches and kClassPriority");

    Core::Facets::Subject SlotClassifier::SubjectOf(const Scoring::ScoredCandidate& candidate)
    {
        return candidate.hasSubject ? candidate.subject : Effect::SubjectOf(candidate.candidate);
    }

    bool SlotClassifier::Matches(const Scoring::ScoredCandidate& candidate, SlotClassification classification)
    {
        if (classification == SlotClassification::Regular) return true;
        return Core::Facets::Matches(SubjectOf(candidate), classification);
    }

    bool SlotClassifier::Matches(const Candidate::CandidateVariant& candidate, SlotClassification classification)
    {
        if (classification == SlotClassification::Regular) return true;
        return Core::Facets::Matches(Effect::SubjectOf(candidate), classification);
    }

    SlotClassification SlotClassifier::Classify(const Scoring::ScoredCandidate& candidate)
    {
        return Core::Facets::Classify(SubjectOf(candidate));
    }

}  // namespace Huginn::Slot
