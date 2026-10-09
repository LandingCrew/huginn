#include "ExternalEquipLearner.h"
#include "PipelineStateCache.h"
#include "PlayerInputGate.h"
#include "SelectionLogV3.h"
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

        // Part of a selection already pending -- most often a Huginn key's
        // potion, whose equip event lands ~0.85 s after the press, past the
        // EquipSourceTracker mark (LoreRim 2026-10-02: every key-drunk potion
        // was then logged as a script and counted in skipped=input). It is not
        // a new selection, and not a script.
        if (SelectionTracker::GetSingleton().IsPending(formID)) {
            logger::debug("[ExternalEquipLearner] {} {:08X} is part of a pending selection"sv,
                formType, formID);
            return;
        }

        std::string via;
        if (const char skip = ShouldSkip(formID, via); skip != SKIP_NONE) {
            // Record WHY. A skipped equip never reaches RecordEquipCase, so
            // without this the heartbeat reports accept=n/a identically for
            // "nobody equipped anything" and "every equip was filtered".
            Telemetry::SoakMetrics::GetSingleton().RecordEquipSkip(skip);

            // R4: the selection log v3 never drops a player's pick for a stale
            // pipeline cache (a long menu visit) or for the learning toggle;
            // it records it with the age of the context it is joined to. The
            // learner and accept% still skip it, as before.
            if (skip == SKIP_STALE || skip == SKIP_DISABLED) {
                if (via.empty()) via = PlayerInputGate::GetSingleton().Explain(formID);   // disabled returns before the gate
                if (!via.empty()) {
                    SelectionLogV3::OnUnlearnedPick(formID, std::move(via), skip == SKIP_STALE ? "stale" : "disabled",
                        ComputeAttribution(formID));
                }
            }
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

    void ExternalEquipLearner::OnArmourEquip(RE::FormID formID)
    {
        // R4: an armour pick the player made (the user, 2026-10-08: lift the
        // armour skip) reaches the selection log v3 -- and only it. The frozen
        // learner would train apparel weights on it and accept% would count
        // it, both of which change what the old engine does; R8 (all carried
        // armour a candidate) is where that changes. So: no SelectionTracker,
        // no telemetry, the player-input gate as for any outside equip.
        if (!EnvironmentReady()) {
            return;
        }
        if (SelectionTracker::GetSingleton().IsPending(formID)) {
            return;
        }
        std::string via = PlayerInputGate::GetSingleton().Explain(formID);
        if (via.empty()) {
            return;   // dressing by the engine or a script (an outfit at load): not a pick
        }
        SelectionLogV3::OnUnlearnedPick(formID, std::move(via), "armour", ComputeAttribution(formID));
    }

    char ExternalEquipLearner::ShouldSkip(RE::FormID formID, std::string& via) const
    {
        // 1. Master toggle
        if (!m_config.learnFromExternalEquips) {
            logger::trace("[ExternalEquipLearner] Skipped (disabled) {:08X}", formID);
            return SKIP_DISABLED;
        }

        // 2. Player input. A TESEquipEvent with no Huginn mark and no menu,
        // hotkey or own-wheel pick behind it is not a selection.
        //
        // Only a CONSUMABLE without input is reported as a script -- that is the
        // case the skipped=input counter exists for (LoreRim's auto-quaff,
        // 2026-09-21). Weapons, spells and ammo equip without input all the time
        // for ordinary engine reasons -- the quiver refilling, a bound weapon
        // arriving with its spell -- and counting those would bury the signal.
        via = PlayerInputGate::GetSingleton().Explain(formID);
        if (via.empty()) {
            const auto* form = RE::TESForm::LookupByID(formID);
            const char* name = form && form->GetName() ? form->GetName() : "?";
            if (form && form->Is(RE::FormType::AlchemyItem)) {
                logger::info("[ExternalEquipLearner] Skipped (no player input -- a script?) {:08X} '{}'"sv,
                    formID, name);
                return SKIP_NO_INPUT;
            }
            logger::debug("[ExternalEquipLearner] Skipped (no player input, engine equip) {:08X} '{}'"sv,
                formID, name);
            return SKIP_ENGINE;
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
