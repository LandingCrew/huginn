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
    // configured exponent — tune fHealthSmoothingExponent and the label moves
    // with the curve instead of drifting away from it.
    //
    // BINARY RULES fire at half their configured weight. For most rules the
    // weight is either 0 or the configured value, so this is just "the rule
    // fired"; for the resistance-scaled elemental rules it means a player with
    // 50%+ resistance gets no "Fire Damage" explanation — matching the fact that
    // the context is no longer meaningfully driving their scoring. A rule
    // configured to 0 (disabled in the INI) never reports.
    // =============================================================================

    namespace
    {
        // Binary rules report once their weight reaches this fraction of the
        // configured value.
        constexpr float kBinaryReasonFraction = 0.5f;

        // Enchanted-weapon charge percentage below which "Low Charge" reports.
        // Mirrors PlayerActorState::IsWeaponChargeLow() (0.25), which is the
        // threshold the old tag-based label used. NOTE: WeaponRegistry flags low
        // charge with the stricter Config::WEAPON_CHARGE_LOW_THRESHOLD (0.20) —
        // reconcile both if tuning.
        constexpr float kWeaponChargeLowPct = 0.25f;

        // Ambient light below which "Darkness" reports (WorldState::lightLevel).
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

    ContextReason ContextRuleEngine::DominantReason(
        const ContextWeightMap& weights,
        const ContextReasonSignals& signals) const
    {
        // Tripwire: a new ContextReason needs a threshold here (and a label in
        // display/ExplanationLabel.h) or it can never report.
        static_assert(CONTEXT_REASON_COUNT == 27,
            "ContextReason changed — review DominantReason and Display::ReasonLabel");

        using R = ContextReason;

        // --- Emergency ------------------------------------------------------
        if (weights.healingWeight >=
            CurveThreshold(State::VitalThreshold::CRITICAL, m_config.fHealthSmoothingExponent)) {
            return R::CriticalHealth;
        }

        // --- Environment / crosshair interaction ----------------------------
        // Underwater and workstation rules are already suppression-aware in
        // EvaluateRules (waterbreathing active → no weight → no explanation).
        if (Fires(weights.waterbreathingWeight, m_config.weightUnderwater))     return R::Underwater;
        if (Fires(weights.unlockWeight, m_config.weightLookingAtLock))          return R::LookingAtLock;
        if (Fires(weights.fortifySmithingWeight, m_config.weightAtForge))       return R::AtForge;
        if (Fires(weights.fortifyEnchantingWeight, m_config.weightAtEnchanter)) return R::AtEnchanter;
        if (Fires(weights.fortifyAlchemyWeight, m_config.weightAtAlchemyLab))   return R::AtAlchemy;

        // --- Active elemental / status damage -------------------------------
        if (Fires(weights.resistFireWeight, m_config.weightOnFire))        return R::OnFire;
        if (Fires(weights.resistPoisonWeight, m_config.weightPoisoned))    return R::Poisoned;
        if (Fires(weights.resistDiseaseWeight, m_config.weightDiseased))   return R::Diseased;
        if (Fires(weights.resistFrostWeight, m_config.weightFrozen))       return R::TakingFrost;
        if (Fires(weights.resistShockWeight, m_config.weightShocked))      return R::TakingShock;
        if (Fires(weights.slowFallWeight, m_config.weightFallingHigh))     return R::Falling;

        // --- Depleted resources ---------------------------------------------
        if (weights.healingWeight >=
            CurveThreshold(State::VitalThreshold::LOW, m_config.fHealthSmoothingExponent)) {
            return R::LowHealth;
        }
        if (weights.magickaRestoreWeight >=
            CurveThreshold(State::VitalThreshold::LOW, m_config.fMagickaSmoothingExponent)) {
            return R::LowMagicka;
        }
        if (weights.staminaRestoreWeight >=
            CurveThreshold(State::VitalThreshold::LOW, m_config.fStaminaSmoothingExponent)) {
            return R::LowStamina;
        }
        if (weights.weaponChargeWeight >=
            CurveThreshold(kWeaponChargeLowPct, m_config.fWeaponChargeSmoothingExponent)) {
            return R::WeaponLowCharge;
        }
        if (Fires(weights.ammoWeight, m_config.weightNeedsAmmo)) return R::NeedsAmmo;

        // --- Surroundings ----------------------------------------------------
        if (signals.allyInjured)                          return R::AllyInjured;
        if (signals.lookingAtOre)                         return R::LookingAtOre;
        if (signals.lightLevel < kDarknessLightLevel)     return R::InDarkness;
        if (Fires(weights.stealthWeight, m_config.weightSneaking)) return R::Sneaking;

        // --- Target / combat --------------------------------------------------
        if (Fires(weights.antiUndeadWeight, m_config.weightTargetUndead))   return R::TargetUndead;
        if (Fires(weights.antiDaedraWeight, m_config.weightTargetDaedra))   return R::TargetDaedra;
        if (Fires(weights.antiDragonWeight, m_config.weightTargetDragon))   return R::TargetDragon;
        if (Fires(weights.aoeWeight, m_config.weightMultipleEnemies))       return R::MultipleEnemies;
        if (Fires(weights.wardWeight, m_config.weightEnemyCasting))         return R::EnemyCasting;

        // Deliberately NOT reasons: the always-on baselines (weapon/spell/buff
        // potion/base relevance) and the ambient in-combat weights (damage,
        // summon, buff-combat). They are true almost whenever anything is
        // happening, so reporting them would drown out the specific reasons
        // above and replace every "Favorite" label with "In Combat".
        return R::None;
    }
}
