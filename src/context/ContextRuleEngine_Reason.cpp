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

        // --- Vitals and weapon charge: invert the scoring curve ---------------
        Mark(R::CriticalHealth, weights.healingWeight >=
            CurveThreshold(State::VitalThreshold::CRITICAL, m_config.fHealthSmoothingExponent));
        Mark(R::LowHealth, weights.healingWeight >=
            CurveThreshold(State::VitalThreshold::LOW, m_config.fHealthSmoothingExponent));
        Mark(R::LowMagicka, weights.magickaRestoreWeight >=
            CurveThreshold(State::VitalThreshold::LOW, m_config.fMagickaSmoothingExponent));
        Mark(R::LowStamina, weights.staminaRestoreWeight >=
            CurveThreshold(State::VitalThreshold::LOW, m_config.fStaminaSmoothingExponent));
        Mark(R::WeaponLowCharge, weights.weaponChargeWeight >=
            CurveThreshold(State::VitalThreshold::WEAPON_CHARGE_LOW,
                           m_config.fWeaponChargeSmoothingExponent));

        // --- Environment ------------------------------------------------------
        // Already suppression-aware in EvaluateRules: waterbreathing active or
        // invisibility up means no weight, hence no explanation.
        Mark(R::Underwater,    Fires(weights.waterbreathingWeight, m_config.weightUnderwater));
        Mark(R::LookingAtLock, Fires(weights.unlockWeight, m_config.weightLookingAtLock));
        Mark(R::Falling,       Fires(weights.slowFallWeight, m_config.weightFallingHigh));
        Mark(R::AtForge,       Fires(weights.fortifySmithingWeight, m_config.weightAtForge));
        Mark(R::AtEnchanter,   Fires(weights.fortifyEnchantingWeight, m_config.weightAtEnchanter));
        Mark(R::AtAlchemy,     Fires(weights.fortifyAlchemyWeight, m_config.weightAtAlchemyLab));

        // --- Active elemental / status damage ---------------------------------
        Mark(R::OnFire,      Fires(weights.resistFireWeight, m_config.weightOnFire));
        Mark(R::Poisoned,    Fires(weights.resistPoisonWeight, m_config.weightPoisoned));
        Mark(R::Diseased,    Fires(weights.resistDiseaseWeight, m_config.weightDiseased));
        Mark(R::TakingFrost, Fires(weights.resistFrostWeight, m_config.weightFrozen));
        Mark(R::TakingShock, Fires(weights.resistShockWeight, m_config.weightShocked));

        // --- Equipment --------------------------------------------------------
        Mark(R::NeedsAmmo, Fires(weights.ammoWeight, m_config.weightNeedsAmmo));

        // --- Surroundings -----------------------------------------------------
        Mark(R::AllyInjured,  signals.allyInjured);
        Mark(R::LookingAtOre, signals.lookingAtOre);
        Mark(R::InDarkness,   signals.lightLevel < kDarknessLightLevel);
        Mark(R::Sneaking,     Fires(weights.stealthWeight, m_config.weightSneaking));

        // --- Target / combat ---------------------------------------------------
        Mark(R::TargetUndead,    Fires(weights.antiUndeadWeight, m_config.weightTargetUndead));
        Mark(R::TargetDaedra,    Fires(weights.antiDaedraWeight, m_config.weightTargetDaedra));
        Mark(R::TargetDragon,    Fires(weights.antiDragonWeight, m_config.weightTargetDragon));
        Mark(R::MultipleEnemies, Fires(weights.aoeWeight, m_config.weightMultipleEnemies));
        Mark(R::EnemyCasting,    Fires(weights.wardWeight, m_config.weightEnemyCasting));

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
