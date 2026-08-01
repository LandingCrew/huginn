#include "WildcardManager.h"
#include <algorithm>

namespace Huginn::Scoring
{
    WildcardManager::WildcardManager()
        : m_lastWildcardTime(std::chrono::steady_clock::now())
        , m_wildcardEndTime(std::chrono::steady_clock::now())
    {
        // Initialize all wildcard slots to empty
        for (auto& slot : m_cachedWildcards) {
            slot.formID = 0;
            slot.sourceType = Candidate::SourceType::Spell;
        }
    }

    void WildcardManager::ApplyWildcards(ScoredCandidateList& rankedCandidates, size_t slotCount)
    {
        if (!m_enabled || rankedCandidates.size() < 2 || slotCount < 2) {
            return;
        }

        // Clamp slot count to maximum
        slotCount = std::min(slotCount, MAX_WILDCARD_SLOTS);

        // Drop wildcards stranded above this page's slot count (#70).
        //
        // A switch to a smaller page leaves the two halves of this class
        // disagreeing: ApplyWildcardsToRanking below is bounded by slotCount so
        // it can never surface index >= slotCount, but HasActiveWildcard()
        // scans the whole cache — so a stranded entry keeps reporting "a
        // wildcard is active" and suppresses the re-roll that would produce a
        // usable one. The player then sees no wildcard at all, with no re-roll,
        // until the stranded entry's own cooldown expires.
        //
        // Clearing here rather than inside HasActiveWildcard() because
        // UpdateExpiry() also calls it and wants the unbounded meaning: a
        // wildcard on another page must still age out on its timer.
        //
        // Deliberately does NOT stamp m_wildcardEndTime: these were never
        // displayed, so no refractory is owed. Clearing the last live entry
        // therefore leaves refractoryElapsed measured from the previous real
        // expiry — usually long past — and the re-roll fires on this same tick.
        // Consequence worth knowing: a switch down to a small page now almost
        // always yields a wildcard promptly, where before it yielded none at
        // all. That is a wider swing than "fixes a stall", and it is intended.
        //
        // Not wrapped in #ifndef NDEBUG like the other debug logs in this file:
        // the defining property of this bug was that nothing recorded it, so
        // the drop is worth a line wherever debug logging is on. It stays at
        // debug (Release runs at info, Main.cpp:577) — same call as the
        // [Context]/[Subtext] diagnostics, which are Debug-build tools.
        for (size_t i = slotCount; i < MAX_WILDCARD_SLOTS; ++i) {
            if (m_cachedWildcards[i].formID != 0) {
                logger::debug("[WildcardManager] Dropped wildcard at slot {} — page has {} slots"sv,
                    i, slotCount);
                m_cachedWildcards[i].formID = 0;
            }
        }

        auto now = std::chrono::steady_clock::now();

        // Update expiry and check refractory period
        UpdateWildcardExpiry(now);

        // Reads the cache *after* the drop above, so it now describes only what
        // this page can actually display.
        bool hasActiveWildcards = HasActiveWildcard();
        float refractoryElapsed = std::chrono::duration<float>(now - m_wildcardEndTime).count();

        // Roll for new wildcards if none active and refractory period has passed
        if (!hasActiveWildcards && refractoryElapsed >= m_refractorySeconds) {
            RollNewWildcards(rankedCandidates, slotCount, now);
        }

        // Apply wildcards to ranking
        ApplyWildcardsToRanking(rankedCandidates, slotCount);
    }

    bool WildcardManager::HasActiveWildcard() const
    {
        for (const auto& slot : m_cachedWildcards) {
            if (slot.formID != 0) {
                return true;
            }
        }
        return false;
    }

    RE::FormID WildcardManager::GetWildcardForSlot(size_t slotIndex) const
    {
        if (slotIndex < MAX_WILDCARD_SLOTS) {
            return m_cachedWildcards[slotIndex].formID;
        }
        return 0;
    }

    size_t WildcardManager::GetActiveWildcardCount() const
    {
        size_t count = 0;
        for (const auto& slot : m_cachedWildcards) {
            if (slot.formID != 0) {
                ++count;
            }
        }
        return count;
    }

    void WildcardManager::Reset()
    {
        for (auto& slot : m_cachedWildcards) {
            slot.formID = 0;
        }
        m_lastWildcardTime = std::chrono::steady_clock::now();
        m_wildcardEndTime = std::chrono::steady_clock::now();
    }

    // =========================================================================
    // INTERNAL HELPERS
    // =========================================================================

    float WildcardManager::GetProbabilityForSlot(size_t slotIndex, size_t slotCount) const
    {
        // Bounds check - return 0% for out-of-range slots
        if (slotIndex >= slotCount || slotIndex >= MAX_WILDCARD_SLOTS) {
            return 0.0f;
        }

        // Slot 0 excluded by default
        if (m_firstSlotExcluded && slotIndex == 0) {
            return 0.0f;
        }

        // Linear scaling: higher slots have higher probability
        // Formula: baseProbability * slotIndex (capped at maxProbability)
        // Examples with base=0.165, max=0.5:
        //   Slot 0: 0% (excluded)
        //   Slot 1: 16.5%
        //   Slot 2: 33%
        //   Slot 3: 49.5% (capped at 50%)
        //   Slot 4+: 50% (capped)
        float probability = m_baseProbability * static_cast<float>(slotIndex);
        return std::min(probability, m_maxProbability);
    }

    void WildcardManager::UpdateWildcardExpiry(std::chrono::steady_clock::time_point now)
    {
        bool hasActiveWildcards = HasActiveWildcard();
        float elapsedSeconds = std::chrono::duration<float>(now - m_lastWildcardTime).count();

        // Check if cooldown has expired
        if (hasActiveWildcards && elapsedSeconds >= m_cooldownSeconds) {
            // Clear all wildcards and mark end time for refractory period
            for (auto& slot : m_cachedWildcards) {
                slot.formID = 0;
            }
            m_wildcardEndTime = now;

#ifndef NDEBUG
            logger::debug("[WildcardManager] Wildcards expired - entering refractory period ({:.1f}s)"sv,
                m_refractorySeconds);
#endif
        }
    }

    void WildcardManager::RollNewWildcards(
        const ScoredCandidateList& rankedCandidates,
        size_t slotCount,
        std::chrono::steady_clock::time_point now)
    {
        auto& rng = GetRNG();
        std::uniform_real_distribution<float> probDist(0.0f, 1.0f);

        bool anyRolled = false;
        auto& usedFormIDs = m_usedFormIDsBuffer;
        usedFormIDs.clear();

        // Get source type from top candidate for coherent wildcards
        auto topType = rankedCandidates[0].GetSourceType();

        // Roll for each slot (starting from 1 if first slot excluded)
        size_t startSlot = m_firstSlotExcluded ? 1 : 0;
        for (size_t i = startSlot; i < slotCount && i < rankedCandidates.size(); ++i) {
            float probability = GetProbabilityForSlot(i, slotCount);

            if (probDist(rng) < probability) {
                RE::FormID wildcardID = SelectRandomCandidate(rankedCandidates, topType, usedFormIDs);

                if (wildcardID != 0) {
                    m_cachedWildcards[i].formID = wildcardID;
                    m_cachedWildcards[i].sourceType = topType;
                    usedFormIDs.push_back(wildcardID);
                    anyRolled = true;

#ifndef NDEBUG
                    // Find name for logging
                    for (const auto& c : rankedCandidates) {
                        if (c.GetFormID() == wildcardID) {
                            logger::debug("[WildcardManager] Slot {} wildcard: {} (prob={:.1f}%, persists {:.1f}s)"sv,
                                i, c.GetName(), probability * 100.0f, m_cooldownSeconds);
                            break;
                        }
                    }
#endif
                }
            }
        }

        // Reset cooldown timer if any wildcard was rolled
        if (anyRolled) {
            m_lastWildcardTime = now;
        }
    }

    void WildcardManager::ApplyWildcardsToRanking(ScoredCandidateList& rankedCandidates, size_t slotCount)
    {
        // Track which positions have been swapped to avoid double-swapping.
        // assign() reuses capacity — allocation-free after the first call.
        auto& swappedPositions = m_swappedBuffer;
        swappedPositions.assign(rankedCandidates.size(), false);

        // Apply wildcards for each slot
        for (size_t slotIdx = 0; slotIdx < slotCount && slotIdx < MAX_WILDCARD_SLOTS; ++slotIdx) {
            const auto& wildcardSlot = m_cachedWildcards[slotIdx];
            if (wildcardSlot.formID == 0) {
                continue;
            }

            // Find the candidate with this FormID
            auto it = std::find_if(rankedCandidates.begin(), rankedCandidates.end(),
                [&wildcardSlot](const ScoredCandidate& s) {
                    return s.GetFormID() == wildcardSlot.formID &&
                           s.GetSourceType() == wildcardSlot.sourceType;
                });

            if (it == rankedCandidates.end()) {
                continue;  // Wildcard candidate no longer available
            }

            size_t foundIdx = std::distance(rankedCandidates.begin(), it);

            // Don't swap if already in a better or same position
            if (foundIdx <= slotIdx) {
                continue;
            }

            // Don't swap if target position already swapped
            if (slotIdx < swappedPositions.size() && swappedPositions[slotIdx]) {
                continue;
            }

            // Don't swap if source position already swapped
            if (foundIdx < swappedPositions.size() && swappedPositions[foundIdx]) {
                continue;
            }

            // Perform swap
            if (slotIdx < rankedCandidates.size()) {
                std::iter_swap(rankedCandidates.begin() + slotIdx, it);
                rankedCandidates[slotIdx].isWildcard = true;
                swappedPositions[slotIdx] = true;
                swappedPositions[foundIdx] = true;

#ifndef NDEBUG
                logger::trace("[WildcardManager] Slot {} using persistent wildcard: {}"sv,
                    slotIdx, rankedCandidates[slotIdx].GetName());
#endif
            }
        }
    }

    RE::FormID WildcardManager::SelectRandomCandidate(
        const ScoredCandidateList& candidates,
        Candidate::SourceType sourceType,
        const std::vector<RE::FormID>& excludeFormIDs)
    {
        // Reuse buffer to avoid heap allocation in hot path
        m_eligibleBuffer.clear();

        // Skip slot 0 (top pick) - start from index 1
        for (size_t i = 1; i < candidates.size(); ++i) {
            RE::FormID formID = candidates[i].GetFormID();

            // Check source type matches
            if (candidates[i].GetSourceType() != sourceType) {
                continue;
            }

            // Check not already used as wildcard
            if (std::find(excludeFormIDs.begin(), excludeFormIDs.end(), formID) != excludeFormIDs.end()) {
                continue;
            }

            m_eligibleBuffer.push_back(formID);
        }

        if (m_eligibleBuffer.empty()) {
            return 0;
        }

        auto& rng = GetRNG();
        std::uniform_int_distribution<size_t> indexDist(0, m_eligibleBuffer.size() - 1);
        return m_eligibleBuffer[indexDist(rng)];
    }

    std::mt19937& WildcardManager::GetRNG()
    {
        static thread_local std::mt19937 rng(std::random_device{}());
        return rng;
    }

}  // namespace Huginn::Scoring
