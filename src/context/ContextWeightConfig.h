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
    // Mirrors all public fields from ContextWeightSettings (35 fields total):
    //   - 31 weights (legacy 0-10 for CandidateGenerator, normalized [0,1] for
    //     ContextRuleEngine; the split is not a clean 17/15 and never was)
    //   - 4 smoothing exponents: health, magicka, stamina, weapon charge
    //
    // Verified against [ContextWeights] 2026-08-29: the 36 keys the loader reads
    // and the keys the shipped configs/Huginn.ini defines are now in agreement
    // both directions. If you add a field here, add its key to that file too —
    // a read-but-undefined key silently takes its compile-time default, which is
    // how fWeightSummon and fWeaponChargeSmoothingExponent went unnoticed.
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
