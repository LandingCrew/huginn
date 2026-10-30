#pragma once

#include "candidate/CandidateTypes.h"
#include "core/SlotScoreMath.h"
#include <array>
#include <cmath>
#include <format>
#include <vector>

namespace Huginn::Scoring
{
    // =============================================================================
    // SCORE BREAKDOWN - the OLD utility's parts (R8.4: legacy rows only)
    // =============================================================================
    // Set by the legacy pass (learning/legacy/) on the rows the old floors keep:
    // the D2 feature's inputs (ln learn mult = ln(1 + lambda * learningScore))
    // and the v2 / v3 logs' old columns. Nothing ranks or displays on it.
    struct ScoreBreakdown
    {
        // Input components
        float contextWeight = 0.0f;     // the old context weight (relevance to current situation)
        float rewardEstimate = 0.0f;            // the old learner (learned preference)
        float prior = 0.0f;             // the old prior (intrinsic quality heuristic)
        float ucb = 0.0f;               // Upper Confidence Bound (exploration bonus)
        float confidence = 0.0f;        // α: How much to trust reward estimate vs prior (0-1)

        // Computed components
        float learningScore = 0.0f;     // α*R + (1-α)*prior + β*UCB
        float lambda = 0.0f;            // λ(confidence) — learning amplification actually applied
        float recencyBoost = 0.0f;      // the old learner's recency (event-driven short-term recall)
        float correlationBonus = 0.0f;  // the old correlation bonus
        float potionMultiplier = 1.0f;  // the old potion multiplier
        float favoritesMultiplier = 1.0f; // the uniform favourites boost (fFavoritesBoostMax)

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
    // SCORED CANDIDATE - Wraps a candidate with its rank score (R8.3: mu)
    // =============================================================================
    struct ScoredCandidate
    {
        Candidate::CandidateVariant candidate;  // The actual candidate (spell/item/weapon/etc)
        // The old engine's utility (R8.3: NOT the ranking). Set on the rows the
        // old floors kept (legacyScored), 0 on the others; the v2 selection
        // log and the fallback's [Recs] line print it. Nothing orders on it
        // but the fallback ranking (legacy module); tools/prune/r8_check.py
        // budgets every read of it outside the files that log it.
        float utility = 0.0f;
        ScoreBreakdown breakdown;               // the old utility's parts (legacy rows only)
        bool isColdStartBoosted = false;        // True if scored via cold-start UCB boost
        // R8.3: the old floors kept this row (pass or cold start), so the D2
        // legacy feature (ln learn mult) applies to it. False with
        // [Scoring] bLegacyLearnFeature off.
        bool legacyScored = false;
        // R8.4: ranked by the FALLBACK (Legacy::Run::FallbackRanking: no
        // bootstrap): `score` is s = ln utility, not mu, and `choice` is unset.
        bool fallbackRanked = false;

        // R8.3: what the slot code ranks on (SlotScore()): the choice score mu
        // (core/ChoiceScore.h, core/ChoiceLive.h), any sign; or, when the
        // bootstrap is missing or invalid, the fallback ln(utility)
        // (RankOnUtility). -inf until scored.
        Core::SlotScore score = Core::kUnrankedScore;

        // R8.3: the choice score's parts, for [Recs], `hg recs`, the
        // explanation label and the reason hold. Unset (need -1) in the
        // fallback ranking.
        struct ChoiceParts
        {
            double var = 0.0;          // sigma^2 under the diagonal approximation
            double needMu = 0.0;       // the need terms' part of mu
            double b = 0.0;            // b as read now (the battery applied)
            double lnLearnMult = 0.0;  // the D2 feature as the score read it
            double utilMissing = 1.0;
            double covered = 0.0;      // covered(i) after the upgrade gate
            struct Term { int16_t need = -1; int16_t col = -1; double value = 0.0; };
            std::array<Term, 3> top{};  // the three largest theta n a terms; top[0] is the explanation
            double explainedNeed = 0.0; // n_k of the explanation's need (the label's threshold)
        };
        ChoiceParts choice;
        // The explanation's need as a context reason (display/NeedReason.h),
        // None when it maps to none, its need is below the label threshold,
        // or the row is "learned". Feeds the reason hold and the label.
        Context::ContextReason explainedReason = Context::ContextReason::None;
        // Not ranked: added to the list only so a Remembrance hold can show it
        // (PipelineCoordinator::ScoreCandidates). Nothing but that hold may
        // place it -- the fill and the slot hold skip it.
        bool isRememberedOnly = false;

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

        /// What the slot code ranks on (src/core/SlotScoreMath.h): a score of
        /// any sign. R8.3: the choice score mu (`score`); the bridge ln(utility)
        /// only in the fallback ranking (no bootstrap). A row that was never
        /// ranked (isRememberedOnly) is -inf, below every real score.
        [[nodiscard]] Core::SlotScore SlotScore() const noexcept {
            return isRememberedOnly ? Core::kUnrankedScore : score;
        }

        /// Rank on a utility through the R7 bridge, ln(u). R8.4: called only
        /// by the fallback ranking (Legacy::Run::FallbackRanking) and by the
        /// slot harness and tests, which state slot scores as utilities;
        /// tools/prune/r8_check.py keeps it out of every other file.
        void RankOnUtility(float u) noexcept {
            utility = u;
            score = Core::BridgeScore(u);
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
        // (score, dps) with dps 0 off-weapon is a proper lexicographic
        // ordering. The cost is that a spell tying a weapon exactly loses the
        // tie -- arbitrary, but it was arbitrary before too.
        [[nodiscard]] float TieBreakDps() const noexcept {
            const auto* weapon = TryAs<Candidate::WeaponCandidate>();
            return weapon ? weapon->damage * weapon->speed : 0.0f;
        }

        // Sort by the score descending (R8.3: mu; higher = better), then by
        // DPS (core/SlotScoreMath.h RanksBefore), and on a tie of both the
        // caller's std::stable_sort keeps the generation order.
        //
        // The DPS tie-break exists because two instances of ONE base form score
        // IDENTICALLY: b, cap(i) and the legacy feature are properties of the
        // form, so a tempered Iron Dagger and a plain one come out equal to
        // the last bit and the sort order alone decided which one the player
        // was offered (observed under the old scorer 2026-09-19: u=0.768 for
        // both, the plain one shown at 19:56 and the tempered one at 20:13).
        //
        // Exact equality, deliberately, not an epsilon: an epsilon comparison
        // is not transitive (a~b, b~c, a<c), which is undefined behaviour in
        // std::sort. No durability in this game, so the better copy of a
        // weapon is always the one to want.
        bool operator<(const ScoredCandidate& other) const noexcept {
            return Core::RanksBefore(score, TieBreakDps(), other.score, other.TieBreakDps());
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
