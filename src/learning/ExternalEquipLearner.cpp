#include "ExternalEquipLearner.h"
#include "PipelineStateCache.h"
#include "EquipEventBus.h"
#include "slot/SlotAllocator.h"
#include "wheeler/WheelerClient.h"

namespace Huginn::Learning
{
    void ExternalEquipLearner::OnExternalEquip(RE::FormID formID, const char* formType)
    {
        if (ShouldSkip(formID)) {
            return;
        }

        auto attribution = ComputeAttribution(formID);
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

        // 3. Wheeler open — player might be mid-selection via Huginn wheel
        if (Wheeler::WheelerClient::GetSingleton().IsWheelOpen()) {
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

        // Case E: Displayed on current page — Huginn already surfaced it, skip
        if (info.wasDisplayed) {
            size_t currentPage = Slot::SlotAllocator::GetSingleton().GetCurrentPage();
            if (info.displayPage == currentPage) {
                return {0.0f, "E (displayed current page)"};
            }

            // Case D: Displayed but on a different page
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
