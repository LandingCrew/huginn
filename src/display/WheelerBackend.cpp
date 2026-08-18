#include "../PCH.h"
#include "WheelerBackend.h"
#include "../Profiling.h"

#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotUtils.h"
#include "ExplanationLabel.h"
#include "wheeler/WheelerClient.h"
#include "wheeler/WheelerSettings.h"

namespace Huginn::Display
{
    bool WheelerBackend::IsEnabled() const
    {
        return Wheeler::WheelerClient::GetSingleton().IsConnected();
    }

    int WheelerBackend::GetDesiredPage() const
    {
        // Wheeler owns page selection when connected: the coordinator syncs the
        // allocator to whichever managed page the player is viewing, before
        // allocation. -1 when disconnected / not on a managed page (no opinion).
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
        return wheelerClient.IsConnected() ? wheelerClient.GetActiveManagedPage() : -1;
    }

    void WheelerBackend::Push(const DisplayContext& ctx)
    {
        Huginn_ZONE_NAMED("Display::Wheeler");
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
        auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
        auto& slotLocker = Slot::SlotLocker::GetSingleton();

        // Urgent-override handling is Wheeler-local: an override at/above the
        // auto-focus threshold both pulls focus to the Huginn wheel and lets the
        // push through even while the wheel is open (so an open wheel updates).
        const auto* topOverride = ctx.overrides.GetTopOverride();
        const int autoFocusThreshold =
            Wheeler::WheelerSettings::GetSingleton().GetAutoFocusMinPriority();
        // The candidate check is not belt-and-braces. An override activates on
        // its CONDITION and carries whatever item answers it, which may be
        // nothing: EvaluateDrowning fires on "underwater without waterbreathing"
        // and calls FindWaterbreathingItem(), so drowning with no potion — the
        // ordinary case — produces an active DROWNING override holding nullptr.
        // Focus-stealing the wheel to show the player an empty answer while they
        // drown is worse than staying out of the way. Latent until #61 made
        // isUnderwater fire at all.
        const bool hasUrgentOverride =
            topOverride && topOverride->candidate &&
            topOverride->priority >= autoFocusThreshold;

        if (hasUrgentOverride && wheelerClient.IsWheelOpen()) {
            wheelerClient.TryUrgentAutoFocus(topOverride->priority);
        }

        // #76: every page invalidated is indistinguishable from "wheels were
        // never created", and the guard below returns false in exactly that
        // state — so the push gives up and UpdatePage, the only thing that could
        // ever notice recovery, is never reached again. Dead wheel for the rest
        // of the session unless the player types `hg reload`. Attempt the
        // rebuild here, ahead of the guard, or it cannot happen at all.
        wheelerClient.RecoverInvalidatedWheels();

        if (!wheelerClient.HasRecommendationWheel() ||
            (wheelerClient.IsWheelOpen() && !hasUrgentOverride)) {
            return;
        }

        const auto& stConfig = Wheeler::WheelerSettings::GetSingleton().GetSubtextLabels();

        // Hoist per-tick invariants out of the page loop (E5): the current page,
        // page count, and post-activation policy don't change within one push.
        // Page/count come resolved on the context (ResolveDisplayPage) rather than
        // re-fetched from the allocator; AllocateSlotsForPage below still needs it.
        const size_t currentPage = ctx.pageIndex;
        const size_t pageCount = ctx.pageCount;
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
                // The coordinator pre-derives the explanation label onto the
                // CURRENT page's assignments (for `hg recs` and the [Subtext]
                // log), and this loop only ever assigns — never clears. Start
                // from a clean slate so Wheeler's own policy is authoritative:
                // otherwise a pre-derived label would survive every branch that
                // deliberately declines to write, silently defeating the
                // [Subtexts] toggles and stealing the wildcard label — on the
                // current page only, while other pages behaved correctly.
                assignment.subtextLabel.clear();

                if (stConfig.showOverrideLabel && assignment.IsOverride()) {
                    assignment.subtextLabel = Display::DeriveExplanationLabel(assignment, ctx.contextReason);
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
                    assignment.subtextLabel = Display::DeriveExplanationLabel(assignment, ctx.contextReason);
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

                // Soul gems used to be blanked here — Wheeler could not handle
                // TESSoulGem forms, so Huginn allocated a gem and then zeroed it
                // on the way out, leaving an empty tile where the widget showed
                // one. Wheeler renders them now (observed 2026-08-11: a user
                // wheel carrying Azura's Star and The Black Star), so the
                // suppression is gone and gems push like any other item.

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
