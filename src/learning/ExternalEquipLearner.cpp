#include "ExternalEquipLearner.h"
#include "PipelineStateCache.h"
#include "EquipEventBus.h"
#include "telemetry/SoakMetrics.h"

// Deliberately does NOT include slot/SlotAllocator.h or wheeler/WheelerClient.h:
// both live above this class in the dependency order, and both are now reached
// through Environment, wired by Main.cpp. Re-adding either include is the
// regression this change exists to prevent.

namespace Huginn::Learning
{
    void ExternalEquipLearner::OnExternalEquip(RE::FormID formID, const char* formType)
    {
        if (ShouldSkip(formID)) {
            return;
        }

        auto attribution = ComputeAttribution(formID);

        // Soak telemetry: the case label is the recommendation-quality signal
        // (E = Huginn displayed it and the player equipped it; A = never
        // surfaced). Record it before the reward early-return so hits count.
        Telemetry::SoakMetrics::GetSingleton().RecordEquipCase(attribution.caseLabel[0]);

        if (attribution.multiplier <= 0.0f) {
            logger::debug("[ExternalEquipLearner] Skipped ({}) {:08X} '{}'",
                attribution.caseLabel, formID, formType);
            return;
        }

        // Publish to EquipEventBus (subscribers handle FQL reward + UsageMemory + misclick)
        EquipEventBus::GetSingleton().Publish(
            formID, EquipSource::External, attribution.multiplier, false);

        // Update anti-spam timestamp + periodic cleanup
        {
            std::lock_guard lock(m_mutex);
            auto now = std::chrono::steady_clock::now();
            m_lastLearnTime[formID] = now;

            if (m_lastLearnTime.size() > MAX_ANTI_SPAM_ENTRIES) {
                std::erase_if(m_lastLearnTime, [now](const auto& pair) {
                    return std::chrono::duration<float>(now - pair.second).count() > CLEANUP_AGE_SECONDS;
                });
            }
        }

        logger::debug("[ExternalEquipLearner] Published: case={} mult={:.2f} "
            "form={:08X} '{}'",
            attribution.caseLabel, attribution.multiplier,
            formID, formType);
    }

    bool ExternalEquipLearner::ShouldSkip(RE::FormID formID) const
    {
        // 1. Master toggle
        if (!m_config.learnFromExternalEquips) {
            logger::trace("[ExternalEquipLearner] Skipped (disabled) {:08X}", formID);
            return true;
        }

        // 2. Cache staleness — pipeline data too old to attribute
        auto& cache = PipelineStateCache::GetSingleton();
        if (cache.IsStale(m_config.externalEquipTimeWindow)) {
            logger::debug("[ExternalEquipLearner] Skipped (stale cache) {:08X}", formID);
            return true;
        }

        // 3. Wheeler open — player might be mid-selection via Huginn wheel.
        // Must be read live: a wheel opened since the last pipeline tick is
        // exactly the case this filter exists for.
        if (!m_env.isWheelOpen) {
            static bool s_warned = false;
            if (!s_warned) {
                logger::error("[ExternalEquipLearner] Environment.isWheelOpen unwired — "
                    "suppressing external-equip learning"sv);
                s_warned = true;
            }
            return true;
        }
        if (m_env.isWheelOpen()) {
            logger::debug("[ExternalEquipLearner] Skipped (wheel open) {:08X}", formID);
            return true;
        }

        // 4. Anti-spam — same FormID learned too recently
        {
            std::lock_guard lock(m_mutex);
            auto it = m_lastLearnTime.find(formID);
            if (it != m_lastLearnTime.end()) {
                auto elapsed = std::chrono::steady_clock::now() - it->second;
                float elapsedSec = std::chrono::duration<float>(elapsed).count();
                if (elapsedSec < m_config.externalEquipMinInterval) {
                    logger::debug("[ExternalEquipLearner] Skipped (anti-spam, {:.1f}s < {:.1f}s) {:08X}",
                        elapsedSec, m_config.externalEquipMinInterval, formID);
                    return true;
                }
            }
        }

        // Note: Re-equip filter omitted — the anti-spam timer (3s default) already
        // prevents double-learning from rapid re-equips of the same item.

        return false;
    }

    ExternalEquipLearner::Attribution ExternalEquipLearner::ComputeAttribution(RE::FormID formID) const
    {
        auto& cache = PipelineStateCache::GetSingleton();
        auto info = cache.GetCandidateInfo(formID);

        // Case A: Not a candidate — player went out of their way to equip something
        // the pipeline didn't even consider. This is the strongest preference signal.
        if (!info.wasCandidate) {
            return {m_config.notCandidateRewardMult, "A (not candidate, boosted)"};
        }

        // Case E: Displayed on current page — Huginn already surfaced it, skip.
        //
        // info.displayPage is the page the cache snapshotted; the comparison is
        // against the LIVE page, so D means "player changed pages since that
        // snapshot", not "item lives on another page" (see the header note).
        // Reading the cached page on both sides would make this always equal.
        if (info.wasDisplayed) {
            if (!m_env.currentDisplayPage) {
                static bool s_warned = false;
                if (!s_warned) {
                    logger::error("[ExternalEquipLearner] Environment.currentDisplayPage unwired — "
                        "treating displayed items as already-surfaced"sv);
                    s_warned = true;
                }
                return {0.0f, "E (displayed, page provider unwired)"};
            }

            if (info.displayPage == m_env.currentDisplayPage()) {
                return {0.0f, "E (displayed current page)"};
            }

            // Case D: displayed at snapshot time, player has since switched pages
            return {m_config.differentPageRewardMult, "D (different page)"};
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
            return {m_config.highUtilityRewardMult, "C (near-miss)"};
        } else if (overshoot <= FAR_MISS_SLOTS) {
            // Case B-med: Moderately ranked, not close to display
            return {m_config.mediumUtilityRewardMult, "B-med (mid rank)"};
        } else {
            // Case B-low: Far from the display cutoff
            return {m_config.lowUtilityRewardMult, "B-low (low rank)"};
        }
    }

}  // namespace Huginn::Learning
