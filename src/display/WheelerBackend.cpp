#include "../PCH.h"
#include "WheelerBackend.h"
#include "../Profiling.h"

#include <array>
#include <cmath>   // std::ceil (lock-timer quantization)

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
        // Detection has to run here too, or that recovery is inert for exactly
        // the session it is meant to rescue. UpdatePage — the only other thing
        // that sets wheelIndex = -1 — sits BELOW the edit-mode gate, so an
        // editor the player never exits means no page is ever marked invalid,
        // RecoverInvalidatedWheels finds a valid index on every tick and returns
        // false forever. Ordering recovery above the gate bought nothing without
        // this. Only while the editor is open, so it costs one cross-DLL lookup
        // per tick and nothing at all the rest of the time; it asks by CLIENT
        // NAME, so a reorder (which shifts indices but never renames our wheels)
        // reads as "still there" and is left alone.
        if (wheelerClient.IsInEditMode()) {
            wheelerClient.DetectVanishedWheels();
        }
        wheelerClient.RecoverInvalidatedWheels();

        if (!wheelerClient.HasRecommendationWheel() ||
            (wheelerClient.IsWheelOpen() && !hasUrgentOverride)) {
            return;
        }

        // Don't push while the player is rearranging wheels. Every move in the
        // editor shifts our indices, and nothing corrects them until edit-mode
        // EXIT — the re-resolve triggers there by design, because mid-edit
        // indices are transient and chasing each intermediate state would be
        // chasing noise. So the whole editor session is a window where our
        // stored indices may name someone else's wheel, and IsManagedWheel()
        // cannot tell us that: it answers "is this wheel managed", not "is it
        // ours", so the write is ACCEPTED on another client's wheel.
        //
        // NARROWER THAN IT LOOKS, and deliberately kept anyway. Wheeler's editor
        // is entered from an OPEN wheel, so for most of an edit session the
        // open-guard above has already returned and this is never reached. What
        // is left is exactly two cases: the tail where the player closes the
        // wheel without leaving the editor (2026-08-28 14:56:44.801 close ->
        // 14:56:44.813 suppressed, and 1.8 s / 3.9 s tails in the 14:31 and
        // 14:32 sessions), and an urgent override, which bypasses the
        // open-guard by design and would otherwise write mid-edit. Both are
        // real; neither is covered anywhere else.
        //
        // Observed 2026-08-28: edit mode opened 13:27:41.098, the player moved a
        // wheel, and at 13:27:46.652 ValidateWheelState reported page 0's wheel
        // unmanaged with pages 1 and 2 reading the contents of the pages below
        // them — a clean off-by-one, stale for the 3.2 s until the exit
        // re-resolve at 13:27:49.849 repaired it. The two "pushes" the 2026-08-22
        // session recorded were `[Subtext] page N` lines, which come from
        // PipelineCoordinator UPSTREAM of this function — so they are evidence
        // that the pipeline ran, not that Wheeler was written to. Treat the
        // wrong-wheel writes as unproven; the two windows above are the case.
        //
        // Cheap to skip: the player is rearranging wheels, not reading
        // recommendations, and the exit re-resolve clears the slot cache — so
        // the next push repaints from scratch with no special case here.
        //
        // Deliberately AFTER RecoverInvalidatedWheels() above: recovery is what
        // rescues a dead wheel, and gating it behind edit mode would let an
        // editor session the player never exits strand the wheels for good.
        //
        // Bracketed by a transition log because nothing else in this file logs,
        // and the gate is otherwise invisible: the `[Subtext] page N` line the
        // roadmap cited as evidence of wrong-wheel pushes lives upstream in
        // PipelineCoordinator and still fires with the gate in place, so its
        // absence proves nothing. Two lines per editor session, not one per
        // tick — at ~10 Hz the naive version buries the log. Single-threaded:
        // the push runs on the update thread only.
        static bool s_editGateActive = false;
        if (wheelerClient.IsInEditMode()) {
            if (!s_editGateActive) {
                s_editGateActive = true;
                spdlog::debug("[Wheeler] Push suppressed — edit mode entered");
            }
            return;
        }
        if (s_editGateActive) {
            s_editGateActive = false;
            spdlog::debug("[Wheeler] Push resumed — edit mode exited");
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

        // #14: one lock snapshot for the whole push. The lock-timer branch below
        // used to call IsSlotLocked() then GetRemainingLockTime() per slot, each
        // taking m_mutex — up to 2 round-trips x slots x pages every push, which
        // is exactly what GetLockSnapshot() was added to avoid (SlotLocker.h:147).
        // Hoisted above the page loop rather than per-page because only the
        // CURRENT page reads it, so one acquisition covers every use.
        //
        // Gated on the setting, which the first cut got wrong: the old per-slot
        // calls sat behind `stConfig.showLockTimerLabel &&`, so with the label OFF
        // (the default, and the config this was measured in) the old code took NO
        // lock at all — an unconditional snapshot traded zero acquisitions for one.
        const bool wantLockTimers = stConfig.showLockTimerLabel;
        const auto lockSnapshot = wantLockTimers
            ? slotLocker.GetLockSnapshot()
            : std::array<Slot::SlotLocker::SlotLockView, Slot::MAX_SLOTS_PER_PAGE>{};

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
                if (wantLockTimers && page == currentPage
                    && assignment.slotIndex < lockSnapshot.size()
                    && lockSnapshot[assignment.slotIndex].isLocked) {
                    const float remainingMs = lockSnapshot[assignment.slotIndex].remainingMs;
                    if (remainingMs > 0.0f) {
                        if (stConfig.lockTimerShowSeconds) {
                            // #14: WHOLE seconds, not tenths. The subtext feeds
                            // WheelSync::UpdatePage's content-unchanged early-out,
                            // and a tenths-place countdown changes on almost every
                            // push — so with bShowLockTimerLabel=true the early-out
                            // never fired while ANY slot was locked, and the whole
                            // page paid the full diff + cross-DLL write instead.
                            // Ceil so the label counts 3 -> 2 -> 1 and never shows
                            // a "0s" that is really 400ms of remaining lock.
                            assignment.subtextLabel = std::format("{} {:.0f}s",
                                stConfig.lockTimerPrefix, std::ceil(remainingMs / 1000.0f));
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
