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
    //   - 17 legacy weights (0-10 scale, for CandidateGenerator compatibility)
    //   - 15 normalized weights [0,1] (for ContextRuleEngine)
    //   - 3 smoothing exponents (for continuous vital curves)
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

        // Weapon charge
        float weightWeaponChargeModerate = ContextWeightDefaults::WEAPON_CHARGE_MODERATE;
        float weightWeaponChargeLow = ContextWeightDefaults::WEAPON_CHARGE_LOW;
        float weightWeaponChargeCritical = ContextWeightDefaults::WEAPON_CHARGE_CRITICAL;

        // Active buff suppression
        float weightHasWaterbreathing = ContextWeightDefaults::HAS_WATERBREATHING;
        float weightHasInvisibility = ContextWeightDefaults::HAS_INVISIBILITY;
        float weightHasMuffle = ContextWeightDefaults::HAS_MUFFLE;
        float weightHasArmorBuff = ContextWeightDefaults::HAS_ARMOR_BUFF;
        float weightHasCloak = ContextWeightDefaults::HAS_CLOAK;
        float weightHasSummon = ContextWeightDefaults::HAS_SUMMON;

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

        // Utility baseline
        float weightBaseRelevance = ContextWeightDefaults::BASE_RELEVANCE;

        // =====================================================================
        // CONTINUOUS FUNCTION SMOOTHING PARAMETERS
        // =====================================================================

        float fHealthSmoothingExponent = ContextWeightDefaults::HEALTH_SMOOTHING_EXPONENT;
        float fMagickaSmoothingExponent = ContextWeightDefaults::MAGICKA_SMOOTHING_EXPONENT;
        float fStaminaSmoothingExponent = ContextWeightDefaults::STAMINA_SMOOTHING_EXPONENT;
    };

    inline constexpr ContextWeightConfig DefaultContextWeightConfig{};

}  // namespace Huginn::State
