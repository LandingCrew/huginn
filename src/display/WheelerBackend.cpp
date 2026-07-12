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
            // Skip placeholder pages (zero-slot or transient creation failure): they
            // hold no wheel, so AllocateSlotsForPage would be wasted work and
            // UpdateRecommendationsForPage would no-op on wheelIndex < 0 anyway.
            if (wheelerClient.GetWheelIndexForPage(page) < 0) {
                continue;
            }

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
            m_formIDs.clear();       m_formIDs.reserve(n);
            m_wildcardFlags.clear(); m_wildcardFlags.reserve(n);
            m_uniqueIDs.clear();     m_uniqueIDs.reserve(n);
            m_subtexts.clear();      m_subtexts.reserve(n);

            for (size_t s = 0; s < n; ++s) {
                const auto& a = pageAssignments[s];

                RE::FormID formID = (!a.IsEmpty() && a.formID != 0) ? a.formID : 0;
                uint16_t   uid = a.HasCandidate() ? a.candidate->GetUniqueID() : 0;
                bool       wild = !a.IsEmpty() && a.IsWildcard();
                std::string sub = a.subtextLabel;

                // Soul gems are display-only — Wheeler can't handle TESSoulGem
                // forms. Zero them so Wheeler treats the slot as empty, and clear the
                // subtext too — otherwise the now-empty slot would keep the soul gem's
                // explanation label, showing an empty slot with a stale description.
                if (a.HasCandidate() &&
                    a.candidate->GetSourceType() == Candidate::SourceType::SoulGem) {
                    formID = 0;
                    uid = 0;
                    wild = false;
                    sub.clear();
                }

                // Empty post-activation policy: blank the activated slot and
                // relabel it "Equipped".
                if (emptyPolicy && wheelerClient.IsSlotActivationEmptied(page, s)) {
                    formID = 0;
                    uid = 0;
                    wild = false;
                    sub = "Equipped";
                }

                m_formIDs.push_back(formID);
                m_uniqueIDs.push_back(uid);
                m_wildcardFlags.push_back(wild);
                m_subtexts.push_back(std::move(sub));
            }

            wheelerClient.UpdateRecommendationsForPage(page, m_formIDs, m_wildcardFlags, m_uniqueIDs, m_subtexts);
        }
    }

}  // namespace Huginn::Display
