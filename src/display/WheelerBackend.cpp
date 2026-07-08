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

        // Hoist per-tick invariants out of the page loop (E5): the current page,
        // page count, and post-activation policy don't change within one push.
        const size_t currentPage = slotAllocator.GetCurrentPage();
        const size_t pageCount = slotAllocator.GetPageCount();
        const bool emptyPolicy = Wheeler::WheelerSettings::GetSingleton().GetPostActivationPolicy()
            == Wheeler::PostActivationPolicy::Empty;

        for (size_t page = 0; page < pageCount; ++page) {
            Slot::SlotAssignments pageAssignments;
            if (page == currentPage) {
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
                if (stConfig.showLockTimerLabel && page == currentPage
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

            // Single-pass extraction into the four Wheeler arrays (E3/E6), with
            // the soul-gem and Empty-policy fixups folded in — replaces four
            // Extract* passes plus two separate fixup loops.
            const size_t n = pageAssignments.size();
            std::vector<RE::FormID> formIDs;      formIDs.reserve(n);
            std::vector<bool>       wildcardFlags; wildcardFlags.reserve(n);
            std::vector<uint16_t>   uniqueIDs;     uniqueIDs.reserve(n);
            std::vector<std::string> subtexts;     subtexts.reserve(n);

            for (size_t s = 0; s < n; ++s) {
                const auto& a = pageAssignments[s];

                RE::FormID formID = (!a.IsEmpty() && a.formID != 0) ? a.formID : 0;
                uint16_t   uid = a.HasCandidate() ? a.candidate->GetUniqueID() : 0;
                bool       wild = !a.IsEmpty() && a.IsWildcard();
                std::string sub = a.subtextLabel;

                // Soul gems are display-only — Wheeler can't handle TESSoulGem
                // forms. Zero them so Wheeler treats the slot as empty.
                if (a.HasCandidate() &&
                    a.candidate->GetSourceType() == Candidate::SourceType::SoulGem) {
                    formID = 0;
                    uid = 0;
                    wild = false;
                }

                // Empty post-activation policy: blank the activated slot and
                // relabel it "Equipped".
                if (emptyPolicy && wheelerClient.IsSlotActivationEmptied(page, s)) {
                    formID = 0;
                    uid = 0;
                    wild = false;
                    sub = "Equipped";
                }

                formIDs.push_back(formID);
                uniqueIDs.push_back(uid);
                wildcardFlags.push_back(wild);
                subtexts.push_back(std::move(sub));
            }

            wheelerClient.UpdateRecommendationsForPage(page, formIDs, wildcardFlags, uniqueIDs, subtexts);
        }
    }

}  // namespace Huginn::Display
