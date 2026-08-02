#include "ContextRuleEngine.h"
#include "state/StateConstants.h"   // State::VitalThreshold

#include <cmath>

namespace Huginn::Context
{
    // =============================================================================
    // DOMINANT REASON (architecture-critique #10)
    // =============================================================================
    // The display explanation is read off the SAME weight map the scorer uses —
    // there is no second derivation from game state. Two threshold families:
    //
    // CONTINUOUS RULES (vitals, weapon charge) invert the smoothing curve:
    //     weight >= (1 - pct)^exponent   ⟺   vital <= pct
    // so the reported boundary is exactly the old percentage threshold at ANY
    // usable exponent — tune fHealthSmoothingExponent and the label moves with
    // the curve instead of drifting away from it. (Both sides evaluate the same
    // expression, so the boundary is bit-for-bit consistent.)
    //
    // BINARY RULES fire at half their configured weight. For most rules the
    // weight is either 0 or the configured value, so this is just "the rule
    // fired"; for the resistance-scaled elemental rules it means a player with
    // 50%+ resistance gets no "Fire Damage" explanation — matching the fact that
    // the context is no longer meaningfully driving their scoring. A rule
    // configured to 0 (disabled in the INI) never reports.
    //
    // PRIORITY comes from ContextReason's declaration order, not from the order
    // of the Mark() calls below: every reason is marked, then the lowest
    // enumerator wins. Reordering the enum reorders the labels, with no second
    // list here to fall out of sync with it.
    // =============================================================================

    namespace
    {
        // Binary rules report once their weight reaches this fraction of the
        // configured value.
        constexpr float kBinaryReasonFraction = 0.5f;

        // Ambient light below which "Darkness" reports (WorldState::lightLevel).
        // Signal-only reason, so unlike every weight-backed reason below it has
        // no INI weight to disable or tune it — see the note on
        // ContextReasonSignals. Matches the old tag threshold.
        constexpr float kDarknessLightLevel = 0.3f;

        /// Weight a continuous rule reaches exactly at `pct` of the vital.
        /// Curve is (1 - pct)^exponent, so comparing weights compares deficits.
        [[nodiscard]] float CurveThreshold(float pct, float exponent) noexcept
        {
            return std::pow(1.0f - pct, exponent);
        }

        /// True when a binary rule is both enabled and currently firing.
        [[nodiscard]] constexpr bool Fires(float weight, float configured) noexcept
        {
            return configured > 0.0f && weight >= kBinaryReasonFraction * configured;
        }
    }

    ReasonWeightField WeightFieldFor(ContextReason reason) noexcept
    {
        // Tripwire: a new ContextReason belongs in this table (or is explicitly
        // fieldless below). Missing it makes the reason silently un-markable.
        static_assert(CONTEXT_REASON_COUNT == 27,
            "ContextReason changed — review WeightFieldFor");

        using R = ContextReason;
        using W = ContextWeightMap;
        switch (reason) {
            case R::CriticalHealth:  return &W::healingWeight;
            case R::Underwater:      return &W::waterbreathingWeight;
            case R::LookingAtLock:   return &W::unlockWeight;
            case R::OnFire:          return &W::resistFireWeight;
            case R::Poisoned:        return &W::resistPoisonWeight;
            case R::Diseased:        return &W::resistDiseaseWeight;
            case R::TakingFrost:     return &W::resistFrostWeight;
            case R::TakingShock:     return &W::resistShockWeight;
            case R::Falling:         return &W::slowFallWeight;
            case R::LowHealth:       return &W::healingWeight;
            case R::LowMagicka:      return &W::magickaRestoreWeight;
            case R::LowStamina:      return &W::staminaRestoreWeight;
            case R::WeaponLowCharge: return &W::weaponChargeWeight;
            case R::NeedsAmmo:       return &W::ammoWeight;
            case R::AtForge:         return &W::fortifySmithingWeight;
            case R::AtEnchanter:     return &W::fortifyEnchantingWeight;
            case R::AtAlchemy:       return &W::fortifyAlchemyWeight;
            case R::Sneaking:        return &W::stealthWeight;
            case R::TargetUndead:    return &W::antiUndeadWeight;
            case R::TargetDaedra:    return &W::antiDaedraWeight;
            case R::TargetDragon:    return &W::antiDragonWeight;
            case R::MultipleEnemies: return &W::aoeWeight;
            case R::EnemyCasting:    return &W::wardWeight;

            // Fieldless by definition — the three ContextReasonSignals facts
            // nothing scores on, plus the two non-reasons. Listed rather than
            // defaulted so a new enumerator is a compiler diagnostic here too.
            case R::AllyInjured:
            case R::LookingAtOre:
            case R::InDarkness:
            case R::None:
            case R::_Count:
                break;
        }
        return nullptr;
    }

    ContextReason ContextRuleEngine::DominantReason(
        const ContextWeightMap& weights,
        const ContextReasonSignals& signals) const
    {
        // Tripwire: a new ContextReason needs a Mark() below (and a label in
        // display/ExplanationLabel.h) or it can never report.
        static_assert(CONTEXT_REASON_COUNT == 27,
            "ContextReason changed — review DominantReason and Display::ReasonLabel");
        static_assert(CONTEXT_REASON_COUNT <= 32,
            "ContextReason outgrew the uint32_t mark set — widen `marked`");

        using R = ContextReason;

        uint32_t marked = 0;
        const auto Mark = [&marked](R reason, bool condition) noexcept {
            if (condition) {
                marked |= 1u << static_cast<uint32_t>(reason);
            }
        };

        // Every weight-backed rule reads its field through WeightFieldFor rather
        // than naming it inline, so the reason → weight pairing lives in exactly
        // one place. The display's attribution test (Context::ReasonAppliesTo)
        // reads the same table, and therefore cannot probe a different field than
        // the one that made the reason fire (#64).
        const auto Weight = [&weights](R reason) noexcept {
            const auto field = WeightFieldFor(reason);
            return field ? weights.*field : 0.0f;
        };
        const auto MarkCurve = [&](R reason, float pct, float exponent) noexcept {
            Mark(reason, Weight(reason) >= CurveThreshold(pct, exponent));
        };
        const auto MarkBinary = [&](R reason, float configured) noexcept {
            Mark(reason, Fires(Weight(reason), configured));
        };

        // --- Vitals and weapon charge: invert the scoring curve ---------------
        MarkCurve(R::CriticalHealth, State::VitalThreshold::CRITICAL, m_config.fHealthSmoothingExponent);
        MarkCurve(R::LowHealth,      State::VitalThreshold::LOW,      m_config.fHealthSmoothingExponent);
        MarkCurve(R::LowMagicka,     State::VitalThreshold::LOW,      m_config.fMagickaSmoothingExponent);
        MarkCurve(R::LowStamina,     State::VitalThreshold::LOW,      m_config.fStaminaSmoothingExponent);
        MarkCurve(R::WeaponLowCharge, State::VitalThreshold::WEAPON_CHARGE_LOW,
                                                                     m_config.fWeaponChargeSmoothingExponent);

        // --- Environment ------------------------------------------------------
        // Already suppression-aware in EvaluateRules: waterbreathing active or
        // invisibility up means no weight, hence no explanation.
        MarkBinary(R::Underwater,    m_config.weightUnderwater);
        MarkBinary(R::LookingAtLock, m_config.weightLookingAtLock);
        MarkBinary(R::Falling,       m_config.weightFallingHigh);
        MarkBinary(R::AtForge,       m_config.weightAtForge);
        MarkBinary(R::AtEnchanter,   m_config.weightAtEnchanter);
        MarkBinary(R::AtAlchemy,     m_config.weightAtAlchemyLab);

        // --- Active elemental / status damage ---------------------------------
        MarkBinary(R::OnFire,      m_config.weightOnFire);
        MarkBinary(R::Poisoned,    m_config.weightPoisoned);
        MarkBinary(R::Diseased,    m_config.weightDiseased);
        MarkBinary(R::TakingFrost, m_config.weightFrozen);
        MarkBinary(R::TakingShock, m_config.weightShocked);

        // --- Equipment --------------------------------------------------------
        MarkBinary(R::NeedsAmmo, m_config.weightNeedsAmmo);

        // --- Surroundings -----------------------------------------------------
        Mark(R::AllyInjured,  signals.allyInjured);
        Mark(R::LookingAtOre, signals.lookingAtOre);
        Mark(R::InDarkness,   signals.lightLevel < kDarknessLightLevel);
        MarkBinary(R::Sneaking, m_config.weightSneaking);

        // --- Target / combat ---------------------------------------------------
        MarkBinary(R::TargetUndead,    m_config.weightTargetUndead);
        MarkBinary(R::TargetDaedra,    m_config.weightTargetDaedra);
        MarkBinary(R::TargetDragon,    m_config.weightTargetDragon);
        MarkBinary(R::MultipleEnemies, m_config.weightMultipleEnemies);
        MarkBinary(R::EnemyCasting,    m_config.weightEnemyCasting);

        // Deliberately NOT marked: the always-on baselines (weapon/spell/buff
        // potion/base relevance) and the ambient in-combat weights (damage,
        // summon, buff-combat). They are true almost whenever anything is
        // happening, so reporting them would drown out the specific reasons
        // above and replace every "Favorite" label with "In Combat".

        // Enum order is priority: lowest marked enumerator wins.
        for (uint32_t i = 1; i < CONTEXT_REASON_COUNT; ++i) {
            if (marked & (1u << i)) {
                return static_cast<ContextReason>(i);
            }
        }
        return R::None;
    }
}
