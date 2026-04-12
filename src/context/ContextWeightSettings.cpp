#include "ContextWeightSettings.h"
#include "ContextWeightConfig.h"

namespace Huginn::State
{
    void ContextWeightSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            logger::info("[ContextWeightSettings] INI file not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            logger::error("[ContextWeightSettings] Failed to load INI file: {}"sv, iniPath.string());
            return;
        }

        const char* section = "ContextWeights";

        // Elemental / status effects
        weightOnFire = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightOnFire", ContextWeightDefaults::ON_FIRE));
        weightPoisoned = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightPoisoned", ContextWeightDefaults::POISONED));
        weightFrozen = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightFrozen", ContextWeightDefaults::FROZEN));
        weightShocked = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightShocked", ContextWeightDefaults::SHOCKED));
        weightDiseased = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightDiseased", ContextWeightDefaults::DISEASED));

        // Environmental
        weightUnderwater = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightUnderwater", ContextWeightDefaults::UNDERWATER));
        weightFallingHigh = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightFallingHigh", ContextWeightDefaults::FALLING_HIGH));
        weightLookingAtLock = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightLookingAtLock", ContextWeightDefaults::LOOKING_AT_LOCK));

        // Weapon charge
        weightWeaponChargeModerate = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightWeaponChargeModerate", ContextWeightDefaults::WEAPON_CHARGE_MODERATE));
        weightWeaponChargeLow = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightWeaponChargeLow", ContextWeightDefaults::WEAPON_CHARGE_LOW));
        weightWeaponChargeCritical = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightWeaponChargeCritical", ContextWeightDefaults::WEAPON_CHARGE_CRITICAL));

        // Active buff suppression
        weightHasWaterbreathing = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightHasWaterbreathing", ContextWeightDefaults::HAS_WATERBREATHING));
        weightHasInvisibility = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightHasInvisibility", ContextWeightDefaults::HAS_INVISIBILITY));
        weightHasMuffle = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightHasMuffle", ContextWeightDefaults::HAS_MUFFLE));
        weightHasArmorBuff = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightHasArmorBuff", ContextWeightDefaults::HAS_ARMOR_BUFF));
        weightHasCloak = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightHasCloak", ContextWeightDefaults::HAS_CLOAK));
        weightHasSummon = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightHasSummon", ContextWeightDefaults::HAS_SUMMON));

        // =====================================================================
        // NEW: NORMALIZED WEIGHTS [0,1] for ContextRuleEngine
        // =====================================================================

        // Health/resource restoration
        weightCriticalHealth = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightCriticalHealth", ContextWeightDefaults::CRITICAL_HEALTH));
        weightLowHealth = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightLowHealth", ContextWeightDefaults::LOW_HEALTH));
        weightLowMagicka = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightLowMagicka", ContextWeightDefaults::LOW_MAGICKA));
        weightLowStamina = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightLowStamina", ContextWeightDefaults::LOW_STAMINA));

        // Combat/tactical
        weightInCombat = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightInCombat", ContextWeightDefaults::IN_COMBAT));
        weightMultipleEnemies = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightMultipleEnemies", ContextWeightDefaults::MULTIPLE_ENEMIES));
        weightEnemyCasting = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightEnemyCasting", ContextWeightDefaults::ENEMY_CASTING));
        weightSneaking = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightSneaking", ContextWeightDefaults::SNEAKING));

        // Workstations
        weightAtForge = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightAtForge", ContextWeightDefaults::AT_FORGE));
        weightAtEnchanter = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightAtEnchanter", ContextWeightDefaults::AT_ENCHANTER));
        weightAtAlchemyLab = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightAtAlchemyLab", ContextWeightDefaults::AT_ALCHEMY_LAB));

        // Target-specific
        weightTargetUndead = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightTargetUndead", ContextWeightDefaults::TARGET_UNDEAD));
        weightTargetDaedra = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightTargetDaedra", ContextWeightDefaults::TARGET_DAEDRA));
        weightTargetDragon = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightTargetDragon", ContextWeightDefaults::TARGET_DRAGON));

        // Equipment
        weightNeedsAmmo = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightNeedsAmmo", ContextWeightDefaults::NEEDS_AMMO));
        weightNoWeapon = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightNoWeapon", ContextWeightDefaults::NO_WEAPON));
        weightWeapon = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightWeapon", ContextWeightDefaults::WEAPON));
        weightSpell = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightSpell", ContextWeightDefaults::SPELL));

        // Utility baseline
        weightBaseRelevance = static_cast<float>(
            ini.GetDoubleValue(section, "fWeightBaseRelevance", ContextWeightDefaults::BASE_RELEVANCE));

        // =====================================================================
        // CONTINUOUS FUNCTION SMOOTHING PARAMETERS
        // =====================================================================

        fHealthSmoothingExponent = static_cast<float>(
            ini.GetDoubleValue(section, "fHealthSmoothingExponent", ContextWeightDefaults::HEALTH_SMOOTHING_EXPONENT));
        fMagickaSmoothingExponent = static_cast<float>(
            ini.GetDoubleValue(section, "fMagickaSmoothingExponent", ContextWeightDefaults::MAGICKA_SMOOTHING_EXPONENT));
        fStaminaSmoothingExponent = static_cast<float>(
            ini.GetDoubleValue(section, "fStaminaSmoothingExponent", ContextWeightDefaults::STAMINA_SMOOTHING_EXPONENT));

        logger::info("[ContextWeightSettings] Loaded legacy weights: fire={:.1f}, poison={:.1f}, frost={:.1f}, "
            "shock={:.1f}, disease={:.1f}, underwater={:.1f}, falling={:.1f}, lock={:.1f}",
            weightOnFire, weightPoisoned, weightFrozen, weightShocked,
            weightDiseased, weightUnderwater, weightFallingHigh, weightLookingAtLock);

        logger::info("[ContextWeightSettings] Loaded normalized weights: critHealth={:.2f}, lowHealth={:.2f}, "
            "combat={:.2f}, multiEnemy={:.2f}, forge={:.2f}, baseRelevance={:.2f}",
            weightCriticalHealth, weightLowHealth, weightInCombat, weightMultipleEnemies,
            weightAtForge, weightBaseRelevance);

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

        weightHasWaterbreathing = ContextWeightDefaults::HAS_WATERBREATHING;
        weightHasInvisibility = ContextWeightDefaults::HAS_INVISIBILITY;
        weightHasMuffle = ContextWeightDefaults::HAS_MUFFLE;
        weightHasArmorBuff = ContextWeightDefaults::HAS_ARMOR_BUFF;
        weightHasCloak = ContextWeightDefaults::HAS_CLOAK;
        weightHasSummon = ContextWeightDefaults::HAS_SUMMON;

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

        weightBaseRelevance = ContextWeightDefaults::BASE_RELEVANCE;

        // Smoothing parameters
        fHealthSmoothingExponent = ContextWeightDefaults::HEALTH_SMOOTHING_EXPONENT;
        fMagickaSmoothingExponent = ContextWeightDefaults::MAGICKA_SMOOTHING_EXPONENT;
        fStaminaSmoothingExponent = ContextWeightDefaults::STAMINA_SMOOTHING_EXPONENT;

        logger::info("[ContextWeightSettings] Reset to defaults (legacy + normalized weights)"sv);
    }

    ContextWeightConfig ContextWeightSettings::BuildConfig() const
    {
        ContextWeightConfig config;

        // Legacy weights (0-10 scale)
        config.weightOnFire = weightOnFire;
        config.weightPoisoned = weightPoisoned;
        config.weightFrozen = weightFrozen;
        config.weightShocked = weightShocked;
        config.weightDiseased = weightDiseased;

        config.weightUnderwater = weightUnderwater;
        config.weightFallingHigh = weightFallingHigh;
        config.weightLookingAtLock = weightLookingAtLock;

        config.weightWeaponChargeModerate = weightWeaponChargeModerate;
        config.weightWeaponChargeLow = weightWeaponChargeLow;
        config.weightWeaponChargeCritical = weightWeaponChargeCritical;

        config.weightHasWaterbreathing = weightHasWaterbreathing;
        config.weightHasInvisibility = weightHasInvisibility;
        config.weightHasMuffle = weightHasMuffle;
        config.weightHasArmorBuff = weightHasArmorBuff;
        config.weightHasCloak = weightHasCloak;
        config.weightHasSummon = weightHasSummon;

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

        config.weightBaseRelevance = weightBaseRelevance;

        // Smoothing exponents
        config.fHealthSmoothingExponent = fHealthSmoothingExponent;
        config.fMagickaSmoothingExponent = fMagickaSmoothingExponent;
        config.fStaminaSmoothingExponent = fStaminaSmoothingExponent;

        return config;
    }

}  // namespace Huginn::State
