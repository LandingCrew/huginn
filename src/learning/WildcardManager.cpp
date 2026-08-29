#include "WildcardManager.h"
#include <algorithm>

namespace Huginn::Scoring
{
    WildcardManager::WildcardManager()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto& cache : m_pages) {
            cache.lastWildcardTime = now;
            cache.wildcardEndTime = now;
        }
    }

    void WildcardManager::ApplyWildcards(ScoredCandidateList& rankedCandidates, const WildcardPage& page)
    {
        if (!m_enabled || page.index >= MAX_WILDCARD_PAGES) {
            return;
        }

        // Record the on-screen page for UpdateExpiry's "worth a pipeline run?"
        // question. Set before any early-out: a page that rolls nothing is still
        // the page being displayed.
        m_lastAppliedPage = page.index;
        auto& cache = m_pages[page.index];

        // Clamp to what this class can track. Both bounds come from the same
        // snapshot, so wildcardSlots can never out-run slotCount, but clamp it
        // against the clamped slotCount rather than the raw one anyway.
        const size_t slotCount = std::min(page.slotCount, MAX_WILDCARD_SLOTS);
        const size_t wildcardSlots = std::min(page.wildcardSlots, slotCount);

        // Shape change invalidates this page's cache wholesale.
        //
        // This is all that remains of #70's bounds repair, and it now fires on
        // the only event that can still strand an entry: the page itself being
        // reshaped by an INI reload. A page SWITCH no longer needs repairing,
        // because pages no longer share an array — page A's entries stay under
        // page A's key and page B's liveness check never sees them.
        //
        // Deliberately does NOT stamp wildcardEndTime: these were never
        // displayed at the new shape, so no refractory is owed. Clearing the
        // last live entry therefore leaves refractoryElapsed measured from the
        // previous real expiry — usually long past — and the re-roll fires on
        // this same tick. Consequence worth knowing: a reshape now almost always
        // yields a wildcard promptly, where before it could yield none at all.
        // That is a wider swing than "fixes a stall", and it is intended.
        //
        // Not wrapped in #ifndef NDEBUG like the other debug logs in this file:
        // the defining property of this bug was that nothing recorded it, so the
        // drop is worth a line wherever debug logging is on. It stays at debug
        // (Release runs at info, Main.cpp:577) — same call as the
        // [Context]/[Subtext] diagnostics, which are Debug-build tools.
        if (cache.slotCount != slotCount || cache.wildcardSlots != wildcardSlots) {
            if (PageHasActiveWildcard(cache)) {
                logger::debug("[WildcardManager] Page {} reshaped ({} slots / {} wildcard-capable, "
                    "was {} / {}) — dropped {} cached wildcard(s)"sv,
                    page.index, slotCount, wildcardSlots,
                    cache.slotCount, cache.wildcardSlots, GetActiveWildcardCount(page.index));
                ClearPageEntries(cache);
            }
            cache.slotCount = slotCount;
            cache.wildcardSlots = wildcardSlots;
        }

        // A page every slot of which sets bWildcardsEnabled=false can display no
        // wildcard at all. Rolling one anyway is exactly the stall #70 fixed for
        // the index case: the liveness check reports it live, nothing shows it,
        // and no re-roll happens until its own cooldown lapses. The clear above
        // has already emptied the cache if this page just lost its last
        // wildcard-capable slot, so there is nothing left to strand here.
        if (wildcardSlots == 0) {
            return;
        }

        // SelectRandomCandidate skips index 0 (the top pick is never a wildcard
        // source), so a list of one has nothing to draw from. slotCount == 0 is
        // the only slot-count case worth refusing: the old guard rejected
        // slotCount < 2 as well, which is why a 1-slot page never reached the
        // repair path. Slot 0 is already unreachable on its own terms — excluded
        // by m_firstSlotExcluded, and scored at base * 0 == 0 even when it is
        // not — so a 1-slot page rolls nothing without needing a special case.
        if (rankedCandidates.size() < 2 || slotCount == 0) {
            return;
        }

        auto now = std::chrono::steady_clock::now();

        // Update expiry and check refractory period
        UpdateWildcardExpiry(cache, now);

        bool hasActiveWildcards = PageHasActiveWildcard(cache);
        float refractoryElapsed = std::chrono::duration<float>(now - cache.wildcardEndTime).count();

        // Roll for new wildcards if none active and refractory period has passed
        if (!hasActiveWildcards && refractoryElapsed >= m_refractorySeconds) {
            RollNewWildcards(cache, rankedCandidates, slotCount, wildcardSlots, now);
        }

        // Apply wildcards to ranking
        ApplyWildcardsToRanking(rankedCandidates, cache, slotCount);
    }

    bool WildcardManager::UpdateExpiry()
    {
        const auto now = std::chrono::steady_clock::now();

        // Whether the DISPLAYED page lapsed is the answer the caller wants; a
        // background page ageing out changes nothing on screen and must not
        // force a pipeline run.
        const auto& displayed = m_pages[m_lastAppliedPage];
        const bool displayedWasActive = PageHasActiveWildcard(displayed);

        for (auto& cache : m_pages) {
            UpdateWildcardExpiry(cache, now);
        }

        return displayedWasActive && !PageHasActiveWildcard(displayed);
    }

    bool WildcardManager::HasActiveWildcard(size_t pageIndex) const
    {
        if (pageIndex >= MAX_WILDCARD_PAGES) {
            return false;
        }
        return PageHasActiveWildcard(m_pages[pageIndex]);
    }

    RE::FormID WildcardManager::GetWildcardForSlot(size_t pageIndex, size_t slotIndex) const
    {
        if (pageIndex < MAX_WILDCARD_PAGES && slotIndex < MAX_WILDCARD_SLOTS) {
            return m_pages[pageIndex].slots[slotIndex].formID;
        }
        return 0;
    }

    size_t WildcardManager::GetActiveWildcardCount(size_t pageIndex) const
    {
        if (pageIndex >= MAX_WILDCARD_PAGES) {
            return 0;
        }

        size_t count = 0;
        for (const auto& slot : m_pages[pageIndex].slots) {
            if (slot.formID != 0) {
                ++count;
            }
        }
        return count;
    }

    void WildcardManager::Reset()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto& cache : m_pages) {
            ClearPageEntries(cache);
            // Forget the recorded shape too: a reset is a fresh start, and the
            // next ApplyWildcards should re-key rather than inherit whatever the
            // pre-reset config happened to be.
            cache.slotCount = SIZE_MAX;
            cache.wildcardSlots = SIZE_MAX;
            cache.lastWildcardTime = now;
            cache.wildcardEndTime = now;
        }
        m_lastAppliedPage = 0;
    }

    // =========================================================================
    // INTERNAL HELPERS
    // =========================================================================

    bool WildcardManager::PageHasActiveWildcard(const PageWildcards& cache)
    {
        // Safe to scan the whole array: every entry in it was rolled within the
        // bounds of the shape the cache currently records, and a shape change
        // empties it (see ApplyWildcards).
        for (const auto& slot : cache.slots) {
            if (slot.formID != 0) {
                return true;
            }
        }
        return false;
    }

    void WildcardManager::ClearPageEntries(PageWildcards& cache)
    {
        for (auto& slot : cache.slots) {
            slot.formID = 0;
        }
    }

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

    void WildcardManager::UpdateWildcardExpiry(PageWildcards& cache, std::chrono::steady_clock::time_point now)
    {
        bool hasActiveWildcards = PageHasActiveWildcard(cache);
        float elapsedSeconds = std::chrono::duration<float>(now - cache.lastWildcardTime).count();

        // Check if cooldown has expired
        if (hasActiveWildcards && elapsedSeconds >= m_cooldownSeconds) {
            // Clear this page's wildcards and mark end time for its refractory
            // period. Both are per-page, so ageing out a page the player is not
            // looking at cannot impose a refractory on the one they are.
            ClearPageEntries(cache);
            cache.wildcardEndTime = now;

#ifndef NDEBUG
            logger::debug("[WildcardManager] Wildcards expired - entering refractory period ({:.1f}s)"sv,
                m_refractorySeconds);
#endif
        }
    }

    void WildcardManager::RollNewWildcards(
        PageWildcards& cache,
        const ScoredCandidateList& rankedCandidates,
        size_t slotCount,
        size_t wildcardSlots,
        std::chrono::steady_clock::time_point now)
    {
        auto& rng = GetRNG();
        std::uniform_real_distribution<float> probDist(0.0f, 1.0f);

        bool anyRolled = false;
        size_t rolled = 0;
        auto& usedFormIDs = m_usedFormIDsBuffer;
        usedFormIDs.clear();

        // Get source type from top candidate for coherent wildcards
        auto topType = rankedCandidates[0].GetSourceType();

        // Roll for each slot (starting from 1 if first slot excluded)
        size_t startSlot = m_firstSlotExcluded ? 1 : 0;
        for (size_t i = startSlot; i < slotCount && i < rankedCandidates.size(); ++i) {
            // Never roll more wildcards than this page has slots willing to show
            // one. Allocation decides WHICH slot takes a given wildcard (by
            // classification and priority), so the surplus above this count has
            // no seat to be assigned to and would sit in the cache suppressing
            // re-rolls while displaying nothing.
            if (rolled >= wildcardSlots) {
                break;
            }

            float probability = GetProbabilityForSlot(i, slotCount);

            if (probDist(rng) < probability) {
                RE::FormID wildcardID = SelectRandomCandidate(rankedCandidates, topType, usedFormIDs);

                if (wildcardID != 0) {
                    cache.slots[i].formID = wildcardID;
                    cache.slots[i].sourceType = topType;
                    usedFormIDs.push_back(wildcardID);
                    anyRolled = true;
                    ++rolled;

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
            cache.lastWildcardTime = now;
        }
    }

    void WildcardManager::ApplyWildcardsToRanking(
        ScoredCandidateList& rankedCandidates,
        const PageWildcards& cache,
        size_t slotCount)
    {
        // Track which positions have been swapped to avoid double-swapping.
        // assign() reuses capacity — allocation-free after the first call.
        auto& swappedPositions = m_swappedBuffer;
        swappedPositions.assign(rankedCandidates.size(), false);

        // Apply wildcards for each slot
        for (size_t slotIdx = 0; slotIdx < slotCount && slotIdx < MAX_WILDCARD_SLOTS; ++slotIdx) {
            const auto& wildcardSlot = cache.slots[slotIdx];
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
