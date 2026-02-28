#pragma once

#include "ScoredCandidate.h"
#include "Config.h"
#include "../slot/SlotAllocator.h"
#include "../slot/SlotSettings.h"
#include <chrono>
#include <random>

namespace Huginn::Scoring
{
    // =============================================================================
    // WILDCARD MANAGER (v0.12.0: Generalized for N slots)
    // =============================================================================
    // Manages wildcard exploration for recommendations.
    // Wildcards allow untried or lower-scored candidates to occasionally appear
    // in recommendation slots, promoting exploration of the Q-learning space.
    //
    // Design:
    //   - Slot 0 (index 0) is always the top-scored pick (no wildcard by default)
    //   - Subsequent slots have configurable wildcard probability
    //   - Probability increases with slot index: P(slot i) = baseProbability * i
    //   - Once selected, wildcards persist for a configurable duration (default 30s)
    //   - After wildcard expires, refractory period before new wildcard (default 5s)
    //
    // v0.12.0 Changes:
    //   - Generalized from fixed 3 slots to N slots
    //   - Replaced m_cachedWildcardSlot2/3 with vector
    //   - Probability scales with slot index using configurable base probability
    //   - Compatible with multi-page system (each page can have different slot count)
    //
    // This class is generic and works with any ScoredCandidate type (spells, items, etc.)
    // =============================================================================

    class WildcardManager
    {
    public:
        WildcardManager();
        ~WildcardManager() = default;

        // Apply wildcards to a ranked candidate list
        // Modifies the list in-place by swapping wildcard candidates into eligible slots
        // @param slotCount Number of slots to consider (from current page configuration)
        void ApplyWildcards(ScoredCandidateList& rankedCandidates, size_t slotCount);

        // Legacy version - gets slot count from current page in SlotAllocator
        void ApplyWildcards(ScoredCandidateList& rankedCandidates) {
            size_t slotCount = Slot::SlotAllocator::GetSingleton().GetSlotCount();
            ApplyWildcards(rankedCandidates, slotCount);
        }

        // Configuration
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        void SetBaseProbability(float probability) { m_baseProbability = std::clamp(probability, 0.0f, 1.0f); }
        void SetMaxProbability(float probability) { m_maxProbability = std::clamp(probability, 0.0f, 1.0f); }
        void SetCooldown(float seconds) { m_cooldownSeconds = std::max(0.0f, seconds); }
        void SetRefractoryPeriod(float seconds) { m_refractorySeconds = std::max(0.0f, seconds); }
        void SetFirstSlotExcluded(bool excluded) { m_firstSlotExcluded = excluded; }

        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        [[nodiscard]] float GetBaseProbability() const noexcept { return m_baseProbability; }
        [[nodiscard]] float GetMaxProbability() const noexcept { return m_maxProbability; }
        [[nodiscard]] float GetCooldown() const noexcept { return m_cooldownSeconds; }
        [[nodiscard]] float GetRefractoryPeriod() const noexcept { return m_refractorySeconds; }
        [[nodiscard]] bool IsFirstSlotExcluded() const noexcept { return m_firstSlotExcluded; }

        // Query current wildcard state
        [[nodiscard]] bool HasActiveWildcard() const;
        [[nodiscard]] RE::FormID GetWildcardForSlot(size_t slotIndex) const;
        [[nodiscard]] size_t GetActiveWildcardCount() const;

        // Legacy accessors for backwards compatibility
        [[nodiscard]] RE::FormID GetSlot2Wildcard() const noexcept { return GetWildcardForSlot(1); }
        [[nodiscard]] RE::FormID GetSlot3Wildcard() const noexcept { return GetWildcardForSlot(2); }

        // Legacy setters (map to base probability)
        void SetSlot2Probability(float p) { m_baseProbability = p; }
        void SetSlot3Probability(float p) { m_maxProbability = p; }
        [[nodiscard]] float GetSlot2Probability() const noexcept { return m_baseProbability; }
        [[nodiscard]] float GetSlot3Probability() const noexcept { return m_maxProbability; }

        // Reset wildcards (e.g., on save load)
        void Reset();

    private:
        // Internal helpers
        void UpdateWildcardExpiry(std::chrono::steady_clock::time_point now);
        void RollNewWildcards(const ScoredCandidateList& rankedCandidates,
                              size_t slotCount,
                              std::chrono::steady_clock::time_point now);
        void ApplyWildcardsToRanking(ScoredCandidateList& rankedCandidates, size_t slotCount);

        // Calculate wildcard probability for a given slot index
        [[nodiscard]] float GetProbabilityForSlot(size_t slotIndex, size_t slotCount) const;

        // Find a random candidate from the list (excluding already used)
        [[nodiscard]] RE::FormID SelectRandomCandidate(
            const ScoredCandidateList& candidates,
            Candidate::SourceType sourceType,
            const std::vector<RE::FormID>& excludeFormIDs);

        // Settings
        bool m_enabled = true;
        bool m_firstSlotExcluded = true;           // Slot 0 never gets wildcards by default
        float m_baseProbability = 0.165f;          // Base probability (slot 1 gets this)
        float m_maxProbability = 0.5f;             // Maximum probability cap
        float m_cooldownSeconds = Config::WILDCARD_COOLDOWN_SECONDS;
        float m_refractorySeconds = 5.0f;

        // Maximum slots to track — uses canonical constant from SlotSettings.h
        static constexpr size_t MAX_WILDCARD_SLOTS = Slot::MAX_SLOTS_PER_PAGE;

        // Wildcard persistence tracking (per slot)
        struct WildcardSlot {
            RE::FormID formID = 0;
            Candidate::SourceType sourceType = Candidate::SourceType::Spell;
        };
        std::array<WildcardSlot, MAX_WILDCARD_SLOTS> m_cachedWildcards;

        std::chrono::steady_clock::time_point m_lastWildcardTime;
        std::chrono::steady_clock::time_point m_wildcardEndTime;

        // Random number generator (thread-local for safety)
        std::mt19937& GetRNG();

        // Reusable buffer for candidate selection (avoids heap allocation in hot path)
        mutable std::vector<RE::FormID> m_eligibleBuffer;
    };

}  // namespace Huginn::Scoring
