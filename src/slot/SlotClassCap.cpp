#include "SlotClassCap.h"
#include "SlotClassifier.h"

namespace Huginn::Slot
{
    SlotClassification SlotClassCap::ClassOf(const Scoring::ScoredCandidate& c) noexcept
    {
        if (c.GetSourceType() == Candidate::SourceType::Food) {
            return SlotClassification::FoodAny;
        }
        return SlotClassifier::Classify(c);
    }

}  // namespace Huginn::Slot
