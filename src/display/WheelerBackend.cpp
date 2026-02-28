#include "../PCH.h"
#include "WheelerBackend.h"
#include "../Profiling.h"

#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotUtils.h"
#include "wheeler/WheelerClient.h"
#include "wheeler/WheelerSettings.h"

namespace Huginn::Display
{
    bool WheelerBackend::IsEnabled() const
    {
        return Wheeler::WheelerClient::GetSingleton().IsConnected();
    }

    void WheelerBackend::Push(const DisplayContext& ctx)
    {
        Huginn_ZONE_NAMED("Display::Wheeler");
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
        auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
        auto& slotLocker = Slot::SlotLocker::GetSingleton();

        if (!wheelerClient.HasRecommendationWheel() ||
            (wheelerClient.IsWheelOpen() && !ctx.hasUrgentOverride)) {
            return;
        }

        const auto& stConfig = Wheeler::WheelerSettings::GetSingleton().GetSubtextLabels();

        for (size_t page = 0; page < slotAllocator.GetPageCount(); ++page) {
            Slot::SlotAssignments pageAssignments;
            if (page == slotAllocator.GetCurrentPage()) {
                pageAssignments = ctx.assignments;
            } else {
                pageAssignments = slotAllocator.AllocateSlotsForPage(
                    page, ctx.scoredCandidates, ctx.overrides, ctx.playerState, ctx.worldState);
            }

            // Populate subtext labels using priority order:
            //   Override > Lock Timer > Wildcard (handled in Wheeler) > Explanation
            for (auto& assignment : pageAssignments) {
                if (stConfig.showOverrideLabel && assignment.IsOverride()) {
                    assignment.subtextLabel = Slot::DeriveExplanationLabel(assignment);
                    continue;
                }
                if (stConfig.showLockTimerLabel && page == slotAllocator.GetCurrentPage()
                    && slotLocker.IsSlotLocked(assignment.slotIndex)) {
                    float remainingMs = slotLocker.GetRemainingLockTime(assignment.slotIndex);
                    if (remainingMs > 0.0f) {
                        if (stConfig.lockTimerShowSeconds) {
                            assignment.subtextLabel = std::format("{} {:.1f}s",
                                stConfig.lockTimerPrefix, remainingMs / 1000.0f);
                        } else {
                            assignment.subtextLabel = stConfig.lockTimerPrefix;
                        }
                        continue;
                    }
                }
                // Wildcard label — handled by WheelerClient (uses INI text)
                if (stConfig.showExplanationLabel && !assignment.IsWildcard()
                    && !assignment.IsEmpty()) {
                    assignment.subtextLabel = Slot::DeriveExplanationLabel(assignment);
                }
                if (assignment.IsEmpty()) {
                    if (assignment.classification != Slot::SlotClassification::Regular) {
                        assignment.subtextLabel = std::format("(No {})",
                            Slot::GetClassificationDisplayName(assignment.classification));
                    } else {
                        assignment.subtextLabel = "(Learning...)";
                    }
                }
            }

            auto formIDs = Slot::ExtractFormIDs(pageAssignments);
            auto wildcardFlags = Slot::ExtractWildcardFlags(pageAssignments);
            auto uniqueIDs = Slot::ExtractUniqueIDs(pageAssignments);
            auto subtexts = Slot::ExtractSubtexts(pageAssignments);

            // Soul gems are display-only — Wheeler can't handle TESSoulGem forms.
            // Zero out their FormIDs so Wheeler treats them as empty slots.
            for (size_t s = 0; s < pageAssignments.size(); ++s) {
                if (pageAssignments[s].HasCandidate() &&
                    pageAssignments[s].candidate->GetSourceType() == Candidate::SourceType::SoulGem) {
                    formIDs[s] = 0;
                    uniqueIDs[s] = 0;
                    wildcardFlags[s] = false;
                }
            }

            // Part B (Empty policy): Zero out activation-emptied slots
            auto postPolicy = Wheeler::WheelerSettings::GetSingleton().GetPostActivationPolicy();
            if (postPolicy == Wheeler::PostActivationPolicy::Empty) {
                for (size_t s = 0; s < formIDs.size(); ++s) {
                    if (wheelerClient.IsSlotActivationEmptied(page, s)) {
                        formIDs[s] = 0;
                        uniqueIDs[s] = 0;
                        wildcardFlags[s] = false;
                        subtexts[s] = "Equipped";
                    }
                }
            }

            wheelerClient.UpdateRecommendationsForPage(page, formIDs, wildcardFlags, uniqueIDs, subtexts);
        }
    }

}  // namespace Huginn::Display
