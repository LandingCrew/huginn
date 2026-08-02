#include "ContextReasonAttribution.h"

#include "ContextRuleEngine.h"          // ContextWeightMap, WeightFieldFor
#include "ContextWeightForCandidate.h"  // WeightForCandidate

namespace Huginn::Context
{
    bool ReasonAppliesTo(
        const Candidate::CandidateVariant& candidate,
        ContextReason reason)
    {
        const auto field = WeightFieldFor(reason);
        if (!field) {
            return false;  // Signal-only reason (or None) — nothing scores on it.
        }

        // Probe map: every field zero except the one behind the reason. Asking
        // the real WeightForCandidate against it answers "does this candidate's
        // mapping read that field", with no second copy of the tag→weight table
        // to fall out of sync with the scorer.
        //
        // baseRelevanceWeight must be cleared explicitly — its 0.05 default is a
        // floor every branch takes std::max against, so leaving it in would make
        // every candidate answer yes to every reason and reinstate the bug.
        ContextWeightMap probe{};
        probe.baseRelevanceWeight = 0.0f;
        probe.*field = 1.0f;  // Sentinel, not a real weight: only >0 is read.

        return WeightForCandidate(candidate, probe) > 0.0f;
    }
}
