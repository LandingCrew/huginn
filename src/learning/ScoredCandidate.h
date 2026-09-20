#pragma once

#include "candidate/CandidateTypes.h"
#include <cmath>
#include <format>
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
        float rewardEstimate = 0.0f;            // From FeatureBanditLearner (learned preference)
        float prior = 0.0f;             // From PriorCalculator (intrinsic quality heuristic)
        float ucb = 0.0f;               // Upper Confidence Bound (exploration bonus)
        float confidence = 0.0f;        // α: How much to trust reward estimate vs prior (0-1)

        // Computed components
        float learningScore = 0.0f;     // α*R + (1-α)*prior + β*UCB
        float lambda = 0.0f;            // λ(confidence) — learning amplification actually applied
        float recencyBoost = 0.0f;      // From UsageMemory (event-driven short-term recall)
        float correlationBonus = 0.0f;  // From CorrelationBooster
        float potionMultiplier = 1.0f;  // From PotionDiscriminator
        float favoritesMultiplier = 1.0f; // From favorites system

        // Log string, compact: only the factors that enter the utility formula
        // (u = ctx*(1+λ*learn)*mults), with neutral 1.00x multipliers omitted.
        [[nodiscard]] std::string ToCompactString() const
        {
            return std::format("ctx={:.2f} λ={:.2f} learn={:.2f}{}",
                contextWeight, lambda, learningScore, MultiplierSuffix());
        }

        // Log string, detailed: compact plus the inputs that produced learn
        // (learn = α*R + (1-α)*P + β*UCB, rec additive when present).
        [[nodiscard]] std::string ToDetailString() const
        {
            return std::format("ctx={:.2f} λ={:.2f} learn={:.2f} (est={:+.2f} P={:.2f} UCB={:.2f} α={:.2f}{}){}",
                contextWeight, lambda, learningScore,
                rewardEstimate, prior, ucb, confidence,
                recencyBoost > 0.0f ? std::format(" rec={:.2f}", recencyBoost) : "",
                MultiplierSuffix());
        }

    private:
        [[nodiscard]] std::string MultiplierSuffix() const
        {
            std::string s;
            if (std::abs(correlationBonus - 1.0f) > 0.005f)
                s += std::format(" corr={:.2f}x", correlationBonus);
            if (std::abs(potionMultiplier - 1.0f) > 0.005f)
                s += std::format(" potion={:.2f}x", potionMultiplier);
            if (std::abs(favoritesMultiplier - 1.0f) > 0.005f)
                s += std::format(" fav={:.2f}x", favoritesMultiplier);
            return s;
        }

    public:
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

        [[nodiscard]] std::string_view GetName() const noexcept {
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
            return Candidate::IsFavorited(candidate);
        }

        // ---------------------------------------------------------------------
        // Comparison operators (for sorting)
        // ---------------------------------------------------------------------

        // DPS for the tie-break below: damage x attack speed, and 0 for
        // anything that is not a weapon.
        //
        // Zero rather than "skip the comparison for non-weapons", which would
        // break strict weak ordering: two tied weapons would order against each
        // other while each compared equal to a tied spell, and std::sort on an
        // intransitive comparator is undefined. Ordering on the pair
        // (utility, dps) with dps 0 off-weapon is a proper lexicographic
        // ordering. The cost is that a spell tying a weapon exactly loses the
        // tie -- arbitrary, but it was arbitrary before too.
        [[nodiscard]] float TieBreakDps() const noexcept {
            const auto* weapon = TryAs<Candidate::WeaponCandidate>();
            return weapon ? weapon->damage * weapon->speed : 0.0f;
        }

        // Sort by utility descending (higher utility = better), then by DPS.
        //
        // The tie-break exists because two instances of ONE base form score
        // IDENTICALLY. The learner is keyed on FormID so they share a weight
        // vector, the context weight is a property of the form, and
        // CalculateWeaponPrior does not look at damage at all -- so a tempered
        // Iron Dagger and a plain one come out equal to the last bit and the
        // sort order alone decided which one the player was offered. Observed
        // 2026-09-19: u=0.768 for both, with the plain one shown at 19:56 and
        // the tempered one at 20:13.
        //
        // Exact float equality, deliberately, not an epsilon. An epsilon
        // comparison is not transitive (a~b, b~c, a<c), which is undefined
        // behaviour in std::sort. Exact is also all that is needed: the two
        // instances reach this point through identical arithmetic on identical
        // inputs, so they are bitwise equal.
        //
        // No durability in this game, so the better copy of a weapon is always
        // the one to want; there is no reason to ration it.
        bool operator<(const ScoredCandidate& other) const noexcept {
            if (utility != other.utility) {
                return utility > other.utility;
            }
            return TieBreakDps() > other.TieBreakDps();
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
