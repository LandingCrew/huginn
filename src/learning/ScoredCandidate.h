#pragma once

#include "candidate/CandidateTypes.h"
#include <vector>

namespace Huginn::Scoring
{
    // =============================================================================
    // SCORE BREAKDOWN - Transparency into how the final score was computed
    // =============================================================================
    struct ScoreBreakdown
    {
        // Input components
        float contextWeight = 0.0f;     // From ContextRuleEngine (relevance to current situation)
        float qValue = 0.0f;            // From FeatureQLearner (learned preference)
        float prior = 0.0f;             // From PriorCalculator (intrinsic quality heuristic)
        float ucb = 0.0f;               // Upper Confidence Bound (exploration bonus)
        float confidence = 0.0f;        // α: How much to trust Q vs prior (0-1)

        // Computed components
        float learningScore = 0.0f;     // α*Q + (1-α)*prior + β*UCB
        float recencyBoost = 0.0f;      // From UsageMemory (event-driven short-term recall)
        float correlationBonus = 0.0f;  // From CorrelationBooster
        float potionMultiplier = 1.0f;  // From PotionDiscriminator
        float favoritesMultiplier = 1.0f; // From favorites system

        // Debug: string representation
        [[nodiscard]] std::string ToString() const
        {
            return std::format(
                "[ctx={:.2f} Q={:.2f} P={:.2f} UCB={:.2f} α={:.2f}] "
                "learn={:.2f}{} corr={:.2f} potion={:.2f}x fav={:.2f}x",
                contextWeight, qValue, prior, ucb, confidence,
                learningScore,
                recencyBoost > 0.0f ? std::format(" rec={:.2f}", recencyBoost) : "",
                correlationBonus, potionMultiplier, favoritesMultiplier);
        }
    };

    // =============================================================================
    // SCORED CANDIDATE - Wraps a candidate with its computed utility score
    // =============================================================================
    struct ScoredCandidate
    {
        Candidate::CandidateVariant candidate;  // The actual candidate (spell/item/weapon/etc)
        float utility = 0.0f;                   // Final computed utility score
        ScoreBreakdown breakdown;               // Score component breakdown
        bool isWildcard = false;                // True if this is a wildcard exploration pick
        bool isColdStartBoosted = false;        // True if scored via cold-start UCB boost

        // ---------------------------------------------------------------------
        // Accessors for common candidate properties
        // ---------------------------------------------------------------------

        [[nodiscard]] RE::FormID GetFormID() const noexcept {
            return Candidate::GetFormID(candidate);
        }

        [[nodiscard]] uint16_t GetUniqueID() const noexcept {
            return Candidate::GetUniqueID(candidate);
        }

        [[nodiscard]] Candidate::SourceType GetSourceType() const noexcept {
            return Candidate::GetSourceType(candidate);
        }

        [[nodiscard]] const std::string& GetName() const noexcept {
            return Candidate::GetName(candidate);
        }

        [[nodiscard]] float GetContextWeight() const noexcept {
            // Return the context weight computed during scoring (from ContextRuleEngine)
            // NOTE: baseRelevance field removed in v0.12.x - use breakdown.contextWeight instead
            return breakdown.contextWeight;
        }

        // Check if this is a specific candidate type
        template<typename T>
        [[nodiscard]] bool Is() const noexcept {
            return Candidate::IsType<T>(candidate);
        }

        // Get as specific type (throws if wrong type)
        template<typename T>
        [[nodiscard]] const T& As() const {
            return Candidate::GetAs<T>(candidate);
        }

        // Try to get as specific type (returns nullptr if wrong type)
        template<typename T>
        [[nodiscard]] const T* TryAs() const noexcept {
            return Candidate::TryGetAs<T>(candidate);
        }

        // Check if candidate is favorited (where applicable)
        [[nodiscard]] bool IsFavorited() const noexcept {
            return std::visit([](const auto& c) -> bool {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                    return c.isFavorited;
                } else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                    return c.isFavorited;
                } else {
                    return false;  // Items, scrolls, ammo don't have favorites
                }
            }, candidate);
        }

        // ---------------------------------------------------------------------
        // Comparison operators (for sorting)
        // ---------------------------------------------------------------------

        // Sort by utility descending (higher utility = better)
        bool operator<(const ScoredCandidate& other) const noexcept {
            return utility > other.utility;
        }

        bool operator==(const ScoredCandidate& other) const noexcept {
            return GetFormID() == other.GetFormID() &&
                   GetSourceType() == other.GetSourceType();
        }
    };

    // =============================================================================
    // SCORED CANDIDATE LIST - Type alias for convenience
    // =============================================================================
    using ScoredCandidateList = std::vector<ScoredCandidate>;

}  // namespace Huginn::Scoring
