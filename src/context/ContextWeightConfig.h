#pragma once

#include "ContextWeightSettings.h"  // For ContextWeightDefaults

namespace Huginn::State
{
    // =========================================================================
    // CONTEXT WEIGHT CONFIGURATION (Immutable snapshot)
    // =========================================================================
    // POD struct produced by ContextWeightSettings::BuildConfig().
    // Consumers store a copy via SetConfig() for consistent, race-free reads.
    //
    // A SUBSET of ContextWeightSettings, not a mirror. 35 float fields here
    // against 38 there, and the gap is deliberate: weightWeaponChargeModerate /
    // Low / Critical stayed behind when the weapon-charge weight became a
    // continuous curve (ContextRuleEngine.cpp, pow(chargeDeficit, exponent)).
    // Their keys were removed from the shipped INI on 2026-08-29; the three
    // fields in ContextWeightSettings are dead and tracked on the roadmap.
    //
    // AUDITING THIS IS A THREE-WAY CHECK, not two. Comparing INI keys against
    // the keys the loader reads finds keys nobody reads and keys nobody can
    // see — but it passes a key that is read into a settings field which then
    // reaches no consumer, which is exactly how the three above survived an
    // audit that declared [ContextWeights] clean. The third leg is: does the
    // loaded field reach ContextWeightConfig, and does anything read it there.
    // =========================================================================

    struct ContextWeightConfig
    {
        // =====================================================================
        // LEGACY WEIGHTS (OLD SCALE: 0-10)
        // =====================================================================

        // Elemental / status effects
        float weightOnFire = ContextWeightDefaults::ON_FIRE;
        float weightPoisoned = ContextWeightDefaults::POISONED;
        float weightFrozen = ContextWeightDefaults::FROZEN;
        float weightShocked = ContextWeightDefaults::SHOCKED;
        float weightDiseased = ContextWeightDefaults::DISEASED;

        // Environmental
        float weightUnderwater = ContextWeightDefaults::UNDERWATER;
        float weightFallingHigh = ContextWeightDefaults::FALLING_HIGH;
        float weightLookingAtLock = ContextWeightDefaults::LOOKING_AT_LOCK;

        // =====================================================================
        // NORMALIZED WEIGHTS [0,1] for ContextRuleEngine
        // =====================================================================

        // Health/resource restoration
        float weightCriticalHealth = ContextWeightDefaults::CRITICAL_HEALTH;
        float weightLowHealth = ContextWeightDefaults::LOW_HEALTH;
        float weightLowMagicka = ContextWeightDefaults::LOW_MAGICKA;
        float weightLowStamina = ContextWeightDefaults::LOW_STAMINA;

        // Combat/tactical
        float weightInCombat = ContextWeightDefaults::IN_COMBAT;
        float weightMultipleEnemies = ContextWeightDefaults::MULTIPLE_ENEMIES;
        float weightEnemyCasting = ContextWeightDefaults::ENEMY_CASTING;
        float weightSneaking = ContextWeightDefaults::SNEAKING;

        // Workstations
        float weightAtForge = ContextWeightDefaults::AT_FORGE;
        float weightAtEnchanter = ContextWeightDefaults::AT_ENCHANTER;
        float weightAtAlchemyLab = ContextWeightDefaults::AT_ALCHEMY_LAB;

        // Target-specific
        float weightTargetUndead = ContextWeightDefaults::TARGET_UNDEAD;
        float weightTargetDaedra = ContextWeightDefaults::TARGET_DAEDRA;
        float weightTargetDragon = ContextWeightDefaults::TARGET_DRAGON;

        // Equipment
        float weightNeedsAmmo = ContextWeightDefaults::NEEDS_AMMO;
        float weightNoWeapon = ContextWeightDefaults::NO_WEAPON;
        float weightWeapon = ContextWeightDefaults::WEAPON;
        float weightSpell = ContextWeightDefaults::SPELL;
        float weightSummon = ContextWeightDefaults::SUMMON;

        // Buff & resist potions
        float weightSoulGem = ContextWeightDefaults::SOUL_GEM;
        float weightBuffPotion = ContextWeightDefaults::BUFF_POTION;
        float weightBuffCombat = ContextWeightDefaults::BUFF_COMBAT;

        // Utility baseline
        float weightBaseRelevance = ContextWeightDefaults::BASE_RELEVANCE;

        // =====================================================================
        // CONTINUOUS FUNCTION SMOOTHING PARAMETERS
        // =====================================================================

        float fHealthSmoothingExponent = ContextWeightDefaults::HEALTH_SMOOTHING_EXPONENT;
        float fMagickaSmoothingExponent = ContextWeightDefaults::MAGICKA_SMOOTHING_EXPONENT;
        float fStaminaSmoothingExponent = ContextWeightDefaults::STAMINA_SMOOTHING_EXPONENT;
        float fWeaponChargeSmoothingExponent = ContextWeightDefaults::WEAPON_CHARGE_SMOOTHING_EXPONENT;
    };

    inline constexpr ContextWeightConfig DefaultContextWeightConfig{};

}  // namespace Huginn::State
