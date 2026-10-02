#include "ExternalEquipLearner.h"
#include "PipelineStateCache.h"
#include "PlayerInputGate.h"
#include "SelectionTracker.h"
#include "telemetry/SoakMetrics.h"

// Deliberately does NOT include slot/SlotAllocator.h or wheeler/WheelerClient.h:
// both live above this class in the dependency order, and both are now reached
// through Environment, wired by Main.cpp. Re-adding either include is the
// regression this change exists to prevent.

namespace Huginn::Learning
{
    bool ExternalEquipLearner::EnvironmentReady() const
    {
        if (m_env.currentDisplayPage) {
            return true;
        }
        static bool s_warned = false;
        if (!s_warned) {
            logger::error("[ExternalEquipLearner] Environment unwired — suppressing "
                "external-equip learning (see Main.cpp SetEnvironment)"sv);
            s_warned = true;
        }
        return false;
    }

    void ExternalEquipLearner::OnExternalEquip(RE::FormID formID, const char* formType)
    {
        // Before the telemetry call below, deliberately: RecordEquipCase keys on
        // the case label's first letter, so attributing an unwired environment
        // to any real case would feed the soak accept% signal a wiring fault
        // dressed as a result.
        if (!EnvironmentReady()) {
            return;
        }

        std::string via;
        if (const char skip = ShouldSkip(formID, via); skip != SKIP_NONE) {
            // Record WHY. A skipped equip never reaches RecordEquipCase, so
            // without this the heartbeat reports accept=n/a identically for
            // "nobody equipped anything" and "every equip was filtered".
            Telemetry::SoakMetrics::GetSingleton().RecordEquipSkip(skip);
            return;
        }

        // The case label is the recommendation-quality signal (E = Huginn
        // displayed it and the player went past it anyway; A = never surfaced).
        // SelectionTracker records it to the soak telemetry once the selection
        // confirms.
        const char* caseLabel = ComputeAttribution(formID);

        logger::debug("[ExternalEquipLearner] {} {:08X} via {} -- case {}"sv,
            formType, formID, via, caseLabel);

        SelectionTracker::GetSingleton().Select(formID, EquipSource::External,
            std::move(via), caseLabel);
    }

    char ExternalEquipLearner::ShouldSkip(RE::FormID formID, std::string& via) const
    {
        // 1. Master toggle
        if (!m_config.learnFromExternalEquips) {
            logger::trace("[ExternalEquipLearner] Skipped (disabled) {:08X}", formID);
            return SKIP_DISABLED;
        }

        // 2. Player input. A TESEquipEvent with no Huginn mark and no menu,
        // hotkey or own-wheel pick behind it is a script acting on the player
        // (LoreRim's auto-quaff, 2026-09-21) -- not a selection.
        via = PlayerInputGate::GetSingleton().Explain(formID);
        if (via.empty()) {
            logger::info("[ExternalEquipLearner] Skipped (no player input -- a script?) {:08X} '{}'"sv,
                formID, [formID] {
                    const auto* form = RE::TESForm::LookupByID(formID);
                    return form && form->GetName() ? form->GetName() : "?";
                }());
            return SKIP_NO_INPUT;
        }

        // 3. Cache staleness — pipeline data too old to attribute
        auto& cache = PipelineStateCache::GetSingleton();
        if (cache.IsStale(m_config.externalEquipTimeWindow)) {
            logger::debug("[ExternalEquipLearner] Skipped (stale cache) {:08X}", formID);
            return SKIP_STALE;
        }

        return SKIP_NONE;
    }

    const char* ExternalEquipLearner::ComputeAttribution(RE::FormID formID) const
    {
        auto& cache = PipelineStateCache::GetSingleton();
        auto info = cache.GetCandidateInfo(formID);

        // Case A: Not a candidate — player went out of their way to equip something
        // the pipeline didn't even consider.
        if (!info.wasCandidate) {
            return "A (not candidate)";
        }

        // Case E: Displayed on current page — Huginn already surfaced it.
        //
        // info.displayPage is the page the cache snapshotted; the comparison is
        // against the LIVE page, so D means "player changed pages since that
        // snapshot", not "item lives on another page" (see the header note).
        // Reading the cached page on both sides would make this always equal.
        if (info.wasDisplayed) {
            if (info.displayPage == m_env.currentDisplayPage()) {
                return "E (displayed current page)";
            }

            // Case D: displayed at snapshot time, player has since switched pages
            return "D (different page)";
        }

        // Cases B/C: Candidate but not displayed — use slot-relative ranking.
        // Compare the item's rank against the number of display slots to determine
        // how close it was to being shown on the widget.
        size_t displayedCount = cache.GetDisplayedCount();
        size_t candidateCount = cache.GetCandidateCount();

        // "Overshoot" = how many ranks past the display cutoff this item is.
        // rank 5 with 5 display slots → overshoot 0 (just missed the widget)
        // rank 8 with 5 display slots → overshoot 3 (far from the widget)
        size_t overshoot = (info.rank > displayedCount) ? (info.rank - displayedCount) : 0;

        logger::trace("[ExternalEquipLearner] Attribution: rank={}, displayed={}, candidates={}, overshoot={}",
            info.rank, displayedCount, candidateCount, overshoot);

        if (overshoot <= NEAR_MISS_SLOTS) {
            // Case C: Near-miss — ranked just below the display cutoff
            return "C (near-miss)";
        } else if (overshoot <= FAR_MISS_SLOTS) {
            // Case B-med: Moderately ranked, not close to display
            return "B-med (mid rank)";
        } else {
            // Case B-low: Far from the display cutoff
            return "B-low (low rank)";
        }
    }

}  // namespace Huginn::Learning
