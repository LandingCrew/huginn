#include "FitScorer.h"
#include <algorithm>
#include <cmath>

namespace Huginn::Scoring
{
    FitParams MakeFitParams(const ScorerConfig& config) noexcept
    {
        FitParams p;
        p.affordMin = config.fitAffordMin;
        p.affordFullCasts = config.fitAffordFullCasts;
        p.unaffordableMult = config.fitUnaffordableMult;
        p.concentrationSecondsPerCast = config.fitConcentrationSecondsPerCast;
        p.outOfRangeMult = config.fitOutOfRangeMult;
        p.clampMin = config.fitClampMin;
        p.clampMax = config.fitClampMax;
        return p;
    }

    FitResult ComputeSpellFit(const SpellFitInputs& in, const FitParams& p) noexcept
    {
        FitResult r;
        r.castsLeft = CastsLeft(in.currentMagicka, in.effectiveCost,
            in.isConcentration, p.concentrationSecondsPerCast);
        r.affordFit = AffordFit(r.castsLeft, in.policy, p);
        r.rangeFit = RangeFit(in.range, in.enemyDistance, p);

        // ScorerSettings guarantees min <= max; guard anyway so a hand-built
        // FitParams cannot trip std::clamp's precondition.
        const float lo = std::min(p.clampMin, p.clampMax);
        const float hi = std::max(p.clampMin, p.clampMax);
        r.multiplier = std::clamp(r.affordFit * r.rangeFit, lo, hi);
        return r;
    }

    void FitScorer::BeginFrame(const State::PlayerActorState& player,
                               const State::TargetCollection& targets) noexcept
    {
        // Absolute magicka points. Same derivation the scorer's inputs use
        // (vitals are 0-1 percentages plus a max).
        m_currentMagicka = std::max(0.0f, player.vitals.magicka * player.vitals.maxMagicka);

        // Same source as StateFeatures::distanceNorm: the nearest hostile,
        // not-dead target Huginn tracks. No line-of-sight test (that would be
        // a new sensor). Out of combat => no range judgement at all.
        m_enemyDistance = -1.0f;
        if (player.isInCombat) {
            if (const auto closest = targets.GetClosestEnemy(); closest.has_value()) {
                m_enemyDistance = std::sqrt(closest->distanceToPlayerSq);
            }
        }
    }

    FitResult FitScorer::Compute(const Candidate::CandidateVariant& candidate) const noexcept
    {
        const auto* spell = std::get_if<Candidate::SpellCandidate>(&candidate);
        if (!spell) {
            return {};
        }

        SpellFitInputs in;
        in.currentMagicka = m_currentMagicka;
        in.effectiveCost = spell->effectiveCost;
        in.isConcentration = spell->isConcentration;
        in.range = spell->range;
        in.enemyDistance = m_enemyDistance;
        // Written only under RunExclusive (settings reload), so a plain read
        // from the scoring pass is safe.
        in.policy = Candidate::g_candidateConfig.uncastableSpellPolicy;

        return ComputeSpellFit(in, MakeFitParams(m_config));
    }

}  // namespace Huginn::Scoring
