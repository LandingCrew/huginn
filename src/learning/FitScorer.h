#pragma once

#include "ScorerConfig.h"
// CandidateTypes first: CandidateConfig.h only forward-declares SourceType
// but uses its enumerators inline.
#include "candidate/CandidateTypes.h"
#include "candidate/CandidateConfig.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"  // TargetCollection
#include <limits>

namespace Huginn::Scoring
{
    // =============================================================================
    // FIT SCORER - item-vs-situation fit multiplier
    // =============================================================================
    // Generalises the PotionDiscriminator idea: a per-candidate multiplier read
    // from the item's own properties against state the player can see. Within
    // a category the context weight is identical and the prior differs by
    // ~0.1, so untrained items tie; fit breaks those ties on facts the player
    // already knows (the spell menu shows the cost, the magicka bar shows what
    // is left, the enemy is visibly far away). Nothing new is sensed.
    //
    // First cut, spells only:
    //   - Affordability: casts left at current magicka. Ramps from
    //     fitAffordMin at one cast to 1.0 at fitAffordFullCasts.
    //   - Range: an aimed spell whose projectile range is shorter than the
    //     nearest hostile Huginn tracks gets fitOutOfRangeMult.
    // Output clamped to [fitClampMin, fitClampMax]. With the default max of 1.0
    // fit only ever DEMOTES -- it breaks ties, it cannot promote.
    //
    // Mode (ScorerConfig::fitMode, INI iFitMode): Off = not computed; Shadow =
    // computed and recorded in the ScoreBreakdown but not applied; Apply =
    // multiplied into the utility (UtilityScorer::ComputeUtility).
    // =============================================================================

    // Mirrors SpellClassifier::GetEffectiveRange's fallback for aimed spells
    // with no projectile data. That value is a guess, not a range -- skip it.
    inline constexpr float RANGE_FALLBACK_SENTINEL = 4096.0f;

    struct FitParams
    {
        float affordMin = 0.7f;
        float affordFullCasts = 3.0f;
        float unaffordableMult = 0.3f;
        float concentrationSecondsPerCast = 2.0f;
        float outOfRangeMult = 0.6f;
        float clampMin = 0.2f;
        float clampMax = 1.0f;
    };

    [[nodiscard]] FitParams MakeFitParams(const ScorerConfig& config) noexcept;

    struct FitResult
    {
        float multiplier = 1.0f;   // clamped product; 1.0 = neutral / not applicable
        float castsLeft = -1.0f;   // -1 = n/a (not a spell); +inf = free spell
        float affordFit = 1.0f;
        float rangeFit = 1.0f;
    };

    // -------------------------------------------------------------------------
    // Pure math (testable, see RunFitScorerTests)
    // -------------------------------------------------------------------------

    // Casts the player can afford right now. Reusable helper.
    // effectiveCost <= 0 => +infinity (free spell: always affordable).
    // Concentration: CalculateMagickaCost returns the cost PER SECOND for a
    // concentration spell (the menu shows it as "/s"), so one "cast" is taken
    // to be secondsPerCast seconds of sustain. castsLeft = magicka /
    // (costPerSecond x secondsPerCast). A non-positive secondsPerCast falls
    // back to one second.
    [[nodiscard]] constexpr float CastsLeft(float currentMagicka, float effectiveCost,
                                            bool isConcentration, float secondsPerCast) noexcept
    {
        if (!(effectiveCost > 0.0f)) {
            return std::numeric_limits<float>::infinity();
        }
        const float perCast = isConcentration
            ? effectiveCost * (secondsPerCast > 0.0f ? secondsPerCast : 1.0f)
            : effectiveCost;
        const float magicka = currentMagicka > 0.0f ? currentMagicka : 0.0f;
        return magicka / perCast;
    }

    // castsLeft >= full         => 1
    // castsLeft in [1, full)    => affordMin + (1 - affordMin) * (c - 1) / (full - 1)
    //                              (full <= 1 => 1)
    // castsLeft < 1             => Penalize: unaffordableMult;
    //                              Allow/Disallow: hold at affordMin. Continuous
    //                              and monotonic -- Allow means "no EXTRA
    //                              punishment for being unaffordable", not "an
    //                              unaffordable spell beats a 1.5-cast one".
    //                              Disallow can land here: the filter reads
    //                              GetActorValue(kMagicka) and fit reads
    //                              vitals.magicka x maxMagicka, which can differ
    //                              by a hair.
    [[nodiscard]] constexpr float AffordFit(float castsLeft, Candidate::UncastableSpellPolicy policy,
                                            const FitParams& p) noexcept
    {
        if (castsLeft < 1.0f) {
            return policy == Candidate::UncastableSpellPolicy::Penalize ? p.unaffordableMult
                                                                        : p.affordMin;
        }
        if (p.affordFullCasts <= 1.0f || castsLeft >= p.affordFullCasts) {
            return 1.0f;
        }
        return p.affordMin + (1.0f - p.affordMin) * (castsLeft - 1.0f) / (p.affordFullCasts - 1.0f);
    }

    // range <= 0 (self/touch), range >= the 4096 fallback sentinel (unknown), or
    // no enemy (enemyDistance < 0: none tracked / out of combat) => 1.
    // Otherwise out of range => outOfRangeMult, within range => 1.
    [[nodiscard]] constexpr float RangeFit(float range, float enemyDistance, const FitParams& p) noexcept
    {
        if (range <= 0.0f || range >= RANGE_FALLBACK_SENTINEL || enemyDistance < 0.0f) {
            return 1.0f;
        }
        return enemyDistance > range ? p.outOfRangeMult : 1.0f;
    }

    struct SpellFitInputs
    {
        float currentMagicka = 0.0f;   // absolute magicka points
        float effectiveCost = 0.0f;    // SpellCandidate::effectiveCost (per second if concentration)
        bool isConcentration = false;
        float range = 0.0f;            // SpellCandidate::range
        float enemyDistance = -1.0f;   // game units; < 0 = no enemy / out of combat
        Candidate::UncastableSpellPolicy policy = Candidate::UncastableSpellPolicy::Disallow;
    };

    // multiplier = clamp(affordFit x rangeFit, clampMin, clampMax)
    [[nodiscard]] FitResult ComputeSpellFit(const SpellFitInputs& in, const FitParams& p) noexcept;

    // -------------------------------------------------------------------------
    // Per-frame wrapper used by UtilityScorer
    // -------------------------------------------------------------------------
    class FitScorer
    {
    public:
        // Holds a reference to the scorer's config, like PotionDiscriminator,
        // so SetConfig (hot reload) is picked up with no extra plumbing.
        explicit FitScorer(const ScorerConfig& config) : m_config(config) {}

        // Snapshot the per-frame inputs once per scoring pass. Takes the RAW
        // player, not the VitalEnvelope copy: fit is about what can be cast
        // now, not the recent low the context weights hold on to.
        void BeginFrame(const State::PlayerActorState& player,
                        const State::TargetCollection& targets) noexcept;

        // Non-spells => neutral FitResult{} (castsLeft = -1).
        [[nodiscard]] FitResult Compute(const Candidate::CandidateVariant& candidate) const noexcept;

    private:
        const ScorerConfig& m_config;
        float m_currentMagicka = 0.0f;
        float m_enemyDistance = -1.0f;
    };

}  // namespace Huginn::Scoring
