#include "ContextWeightSettings.h"
#include "IniLoad.h"
#include "ContextWeightConfig.h"

namespace Huginn::State
{
    void ContextWeightSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "ContextWeightSettings"sv)) {
            LoadFromIni(ini);
        }
    }

    void ContextWeightSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "ContextWeights";

        // Elemental / status effects
        weightOnFire = ReadClampedFloat(ini, section, "fWeightOnFire", ContextWeightDefaults::ON_FIRE, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightPoisoned = ReadClampedFloat(ini, section, "fWeightPoisoned", ContextWeightDefaults::POISONED, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightFrozen = ReadClampedFloat(ini, section, "fWeightFrozen", ContextWeightDefaults::FROZEN, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightShocked = ReadClampedFloat(ini, section, "fWeightShocked", ContextWeightDefaults::SHOCKED, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightDiseased = ReadClampedFloat(ini, section, "fWeightDiseased", ContextWeightDefaults::DISEASED, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Environmental
        weightUnderwater = ReadClampedFloat(ini, section, "fWeightUnderwater", ContextWeightDefaults::UNDERWATER, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightFallingHigh = ReadClampedFloat(ini, section, "fWeightFallingHigh", ContextWeightDefaults::FALLING_HIGH, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightLookingAtLock = ReadClampedFloat(ini, section, "fWeightLookingAtLock", ContextWeightDefaults::LOOKING_AT_LOCK, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Weapon charge
        weightWeaponChargeModerate = ReadClampedFloat(ini, section, "fWeightWeaponChargeModerate", ContextWeightDefaults::WEAPON_CHARGE_MODERATE, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightWeaponChargeLow = ReadClampedFloat(ini, section, "fWeightWeaponChargeLow", ContextWeightDefaults::WEAPON_CHARGE_LOW, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightWeaponChargeCritical = ReadClampedFloat(ini, section, "fWeightWeaponChargeCritical", ContextWeightDefaults::WEAPON_CHARGE_CRITICAL, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // =====================================================================
        // NEW: NORMALIZED WEIGHTS [0,1] for ContextRuleEngine
        // =====================================================================

        // Health/resource restoration
        weightCriticalHealth = ReadClampedFloat(ini, section, "fWeightCriticalHealth", ContextWeightDefaults::CRITICAL_HEALTH, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightLowHealth = ReadClampedFloat(ini, section, "fWeightLowHealth", ContextWeightDefaults::LOW_HEALTH, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightLowMagicka = ReadClampedFloat(ini, section, "fWeightLowMagicka", ContextWeightDefaults::LOW_MAGICKA, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightLowStamina = ReadClampedFloat(ini, section, "fWeightLowStamina", ContextWeightDefaults::LOW_STAMINA, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Combat/tactical
        weightInCombat = ReadClampedFloat(ini, section, "fWeightInCombat", ContextWeightDefaults::IN_COMBAT, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightMultipleEnemies = ReadClampedFloat(ini, section, "fWeightMultipleEnemies", ContextWeightDefaults::MULTIPLE_ENEMIES, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightEnemyCasting = ReadClampedFloat(ini, section, "fWeightEnemyCasting", ContextWeightDefaults::ENEMY_CASTING, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightSneaking = ReadClampedFloat(ini, section, "fWeightSneaking", ContextWeightDefaults::SNEAKING, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Workstations
        weightAtForge = ReadClampedFloat(ini, section, "fWeightAtForge", ContextWeightDefaults::AT_FORGE, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightAtEnchanter = ReadClampedFloat(ini, section, "fWeightAtEnchanter", ContextWeightDefaults::AT_ENCHANTER, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightAtAlchemyLab = ReadClampedFloat(ini, section, "fWeightAtAlchemyLab", ContextWeightDefaults::AT_ALCHEMY_LAB, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Target-specific
        weightTargetUndead = ReadClampedFloat(ini, section, "fWeightTargetUndead", ContextWeightDefaults::TARGET_UNDEAD, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightTargetDaedra = ReadClampedFloat(ini, section, "fWeightTargetDaedra", ContextWeightDefaults::TARGET_DAEDRA, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightTargetDragon = ReadClampedFloat(ini, section, "fWeightTargetDragon", ContextWeightDefaults::TARGET_DRAGON, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Equipment
        weightNeedsAmmo = ReadClampedFloat(ini, section, "fWeightNeedsAmmo", ContextWeightDefaults::NEEDS_AMMO, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightNoWeapon = ReadClampedFloat(ini, section, "fWeightNoWeapon", ContextWeightDefaults::NO_WEAPON, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightWeapon = ReadClampedFloat(ini, section, "fWeightWeapon", ContextWeightDefaults::WEAPON, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightSpell = ReadClampedFloat(ini, section, "fWeightSpell", ContextWeightDefaults::SPELL, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightSummon = ReadClampedFloat(ini, section, "fWeightSummon", ContextWeightDefaults::SUMMON, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Buff & resist potions
        weightBuffPotion = ReadClampedFloat(ini, section, "fWeightBuffPotion", ContextWeightDefaults::BUFF_POTION, 0.0f, 100.0f, "ContextWeightSettings"sv);
        weightBuffCombat = ReadClampedFloat(ini, section, "fWeightBuffCombat", ContextWeightDefaults::BUFF_COMBAT, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // Utility baseline
        weightBaseRelevance = ReadClampedFloat(ini, section, "fWeightBaseRelevance", ContextWeightDefaults::BASE_RELEVANCE, 0.0f, 100.0f, "ContextWeightSettings"sv);

        // =====================================================================
        // CONTINUOUS FUNCTION SMOOTHING PARAMETERS
        // =====================================================================

        fHealthSmoothingExponent = ReadClampedFloat(ini, section, "fHealthSmoothingExponent", ContextWeightDefaults::HEALTH_SMOOTHING_EXPONENT, 0.0f, 100.0f, "ContextWeightSettings"sv);
        fMagickaSmoothingExponent = ReadClampedFloat(ini, section, "fMagickaSmoothingExponent", ContextWeightDefaults::MAGICKA_SMOOTHING_EXPONENT, 0.0f, 100.0f, "ContextWeightSettings"sv);
        fStaminaSmoothingExponent = ReadClampedFloat(ini, section, "fStaminaSmoothingExponent", ContextWeightDefaults::STAMINA_SMOOTHING_EXPONENT, 0.0f, 100.0f, "ContextWeightSettings"sv);
        fWeaponChargeSmoothingExponent = ReadClampedFloat(ini, section, "fWeaponChargeSmoothingExponent", ContextWeightDefaults::WEAPON_CHARGE_SMOOTHING_EXPONENT, 0.0f, 100.0f, "ContextWeightSettings"sv);

        logger::info("[ContextWeightSettings] Loaded legacy weights: fire={:.1f}, poison={:.1f}, frost={:.1f}, "
            "shock={:.1f}, disease={:.1f}, underwater={:.1f}, falling={:.1f}, lock={:.1f}",
            weightOnFire, weightPoisoned, weightFrozen, weightShocked,
            weightDiseased, weightUnderwater, weightFallingHigh, weightLookingAtLock);

        logger::info("[ContextWeightSettings] Loaded normalized weights: critHealth={:.2f}, lowHealth={:.2f}, "
            "combat={:.2f}, multiEnemy={:.2f}, forge={:.2f}, buffPotion={:.2f}, buffCombat={:.2f}, baseRelevance={:.2f}",
            weightCriticalHealth, weightLowHealth, weightInCombat, weightMultipleEnemies,
            weightAtForge, weightBuffPotion, weightBuffCombat, weightBaseRelevance);

        logger::info("[ContextWeightSettings] Loaded smoothing: health^{:.1f}, magicka^{:.1f}, stamina^{:.1f}",
            fHealthSmoothingExponent, fMagickaSmoothingExponent, fStaminaSmoothingExponent);
    }

    void ContextWeightSettings::ResetToDefaults()
    {
        // Legacy weights (0-10 scale)
        weightOnFire = ContextWeightDefaults::ON_FIRE;
        weightPoisoned = ContextWeightDefaults::POISONED;
        weightFrozen = ContextWeightDefaults::FROZEN;
        weightShocked = ContextWeightDefaults::SHOCKED;
        weightDiseased = ContextWeightDefaults::DISEASED;

        weightUnderwater = ContextWeightDefaults::UNDERWATER;
        weightFallingHigh = ContextWeightDefaults::FALLING_HIGH;
        weightLookingAtLock = ContextWeightDefaults::LOOKING_AT_LOCK;

        weightWeaponChargeModerate = ContextWeightDefaults::WEAPON_CHARGE_MODERATE;
        weightWeaponChargeLow = ContextWeightDefaults::WEAPON_CHARGE_LOW;
        weightWeaponChargeCritical = ContextWeightDefaults::WEAPON_CHARGE_CRITICAL;

        // Normalized weights [0,1]
        weightCriticalHealth = ContextWeightDefaults::CRITICAL_HEALTH;
        weightLowHealth = ContextWeightDefaults::LOW_HEALTH;
        weightLowMagicka = ContextWeightDefaults::LOW_MAGICKA;
        weightLowStamina = ContextWeightDefaults::LOW_STAMINA;

        weightInCombat = ContextWeightDefaults::IN_COMBAT;
        weightMultipleEnemies = ContextWeightDefaults::MULTIPLE_ENEMIES;
        weightEnemyCasting = ContextWeightDefaults::ENEMY_CASTING;
        weightSneaking = ContextWeightDefaults::SNEAKING;

        weightAtForge = ContextWeightDefaults::AT_FORGE;
        weightAtEnchanter = ContextWeightDefaults::AT_ENCHANTER;
        weightAtAlchemyLab = ContextWeightDefaults::AT_ALCHEMY_LAB;

        weightTargetUndead = ContextWeightDefaults::TARGET_UNDEAD;
        weightTargetDaedra = ContextWeightDefaults::TARGET_DAEDRA;
        weightTargetDragon = ContextWeightDefaults::TARGET_DRAGON;

        weightNeedsAmmo = ContextWeightDefaults::NEEDS_AMMO;
        weightNoWeapon = ContextWeightDefaults::NO_WEAPON;
        weightWeapon = ContextWeightDefaults::WEAPON;
        weightSpell = ContextWeightDefaults::SPELL;
        weightSummon = ContextWeightDefaults::SUMMON;

        weightBuffPotion = ContextWeightDefaults::BUFF_POTION;
        weightBuffCombat = ContextWeightDefaults::BUFF_COMBAT;

        weightBaseRelevance = ContextWeightDefaults::BASE_RELEVANCE;

        // Smoothing parameters
        fHealthSmoothingExponent = ContextWeightDefaults::HEALTH_SMOOTHING_EXPONENT;
        fMagickaSmoothingExponent = ContextWeightDefaults::MAGICKA_SMOOTHING_EXPONENT;
        fStaminaSmoothingExponent = ContextWeightDefaults::STAMINA_SMOOTHING_EXPONENT;
        fWeaponChargeSmoothingExponent = ContextWeightDefaults::WEAPON_CHARGE_SMOOTHING_EXPONENT;

        logger::info("[ContextWeightSettings] Reset to defaults (legacy + normalized weights)"sv);
    }

    ContextWeightConfig ContextWeightSettings::BuildConfig() const
    {
        ContextWeightConfig config;

        // Legacy weights — pre-normalized from 0-10 INI scale to [0,1]
        // so the engine reads a uniform scale without runtime conversion.
        config.weightOnFire = weightOnFire * 0.1f;
        config.weightPoisoned = weightPoisoned * 0.1f;
        config.weightFrozen = weightFrozen * 0.1f;
        config.weightShocked = weightShocked * 0.1f;
        config.weightDiseased = weightDiseased * 0.1f;

        config.weightUnderwater = weightUnderwater * 0.1f;
        config.weightFallingHigh = weightFallingHigh * 0.1f;
        config.weightLookingAtLock = weightLookingAtLock * 0.1f;

        // Normalized weights [0,1]
        config.weightCriticalHealth = weightCriticalHealth;
        config.weightLowHealth = weightLowHealth;
        config.weightLowMagicka = weightLowMagicka;
        config.weightLowStamina = weightLowStamina;

        config.weightInCombat = weightInCombat;
        config.weightMultipleEnemies = weightMultipleEnemies;
        config.weightEnemyCasting = weightEnemyCasting;
        config.weightSneaking = weightSneaking;

        config.weightAtForge = weightAtForge;
        config.weightAtEnchanter = weightAtEnchanter;
        config.weightAtAlchemyLab = weightAtAlchemyLab;

        config.weightTargetUndead = weightTargetUndead;
        config.weightTargetDaedra = weightTargetDaedra;
        config.weightTargetDragon = weightTargetDragon;

        config.weightNeedsAmmo = weightNeedsAmmo;
        config.weightNoWeapon = weightNoWeapon;
        config.weightWeapon = weightWeapon;
        config.weightSpell = weightSpell;
        config.weightSummon = weightSummon;

        config.weightBuffPotion = weightBuffPotion;
        config.weightBuffCombat = weightBuffCombat;

        config.weightBaseRelevance = weightBaseRelevance;

        // Smoothing exponents
        config.fHealthSmoothingExponent = fHealthSmoothingExponent;
        config.fMagickaSmoothingExponent = fMagickaSmoothingExponent;
        config.fStaminaSmoothingExponent = fStaminaSmoothingExponent;
        config.fWeaponChargeSmoothingExponent = fWeaponChargeSmoothingExponent;

        return config;
    }

}  // namespace Huginn::State
