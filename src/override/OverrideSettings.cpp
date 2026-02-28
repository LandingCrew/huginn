#include "OverrideConfig.h"

namespace Huginn::Override
{
    void Settings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            logger::info("[OverrideSettings] INI file not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            logger::error("[OverrideSettings] Failed to load INI file: {}"sv, iniPath.string());
            return;
        }

        logger::info("[OverrideSettings] Loading override settings from: {}"sv, iniPath.string());

        // Helper lambda for reading float values with defaults
        auto readFloat = [&ini](const char* section, const char* key, float defaultVal) -> float {
            return static_cast<float>(ini.GetDoubleValue(section, key, defaultVal));
        };

        // Helper lambda for reading bool values with defaults
        auto readBool = [&ini](const char* section, const char* key, bool defaultVal) -> bool {
            return ini.GetBoolValue(section, key, defaultVal);
        };

        // =============================================================================
        // [Overrides] section
        // =============================================================================

        const char* section = "Overrides";

        // Critical Health
        criticalHealthThreshold = readFloat(section, "fCriticalHealthThreshold", Defaults::CRITICAL_HEALTH_THRESHOLD);
        criticalHealthHysteresis = readFloat(section, "fCriticalHealthHysteresis", Defaults::CRITICAL_HEALTH_HYSTERESIS);

        // Weapon Charge
        weaponChargeThreshold = readFloat(section, "fWeaponChargeThreshold", Defaults::WEAPON_CHARGE_THRESHOLD);
        weaponChargeHysteresis = readFloat(section, "fWeaponChargeHysteresis", Defaults::WEAPON_CHARGE_HYSTERESIS);

        // Low Ammo
        lowAmmoThreshold = readFloat(section, "fLowAmmoThreshold", Defaults::LOW_AMMO_THRESHOLD);
        lowAmmoHysteresis = readFloat(section, "fLowAmmoHysteresis", Defaults::LOW_AMMO_HYSTERESIS);
        enableLowAmmo = readBool(section, "bEnableLowAmmo", Defaults::ENABLE_LOW_AMMO);

        // Critical Magicka
        criticalMagickaThreshold = readFloat(section, "fCriticalMagickaThreshold", Defaults::CRITICAL_MAGICKA_THRESHOLD);
        criticalMagickaHysteresis = readFloat(section, "fCriticalMagickaHysteresis", Defaults::CRITICAL_MAGICKA_HYSTERESIS);
        enableCriticalMagicka = readBool(section, "bEnableCriticalMagicka", Defaults::ENABLE_CRITICAL_MAGICKA);

        // Critical Stamina
        criticalStaminaThreshold = readFloat(section, "fCriticalStaminaThreshold", Defaults::CRITICAL_STAMINA_THRESHOLD);
        criticalStaminaHysteresis = readFloat(section, "fCriticalStaminaHysteresis", Defaults::CRITICAL_STAMINA_HYSTERESIS);
        enableCriticalStamina = readBool(section, "bEnableCriticalStamina", Defaults::ENABLE_CRITICAL_STAMINA);

        // Anti-flicker
        minOverrideDurationMs = readFloat(section, "fMinOverrideDurationMs", Defaults::MIN_OVERRIDE_DURATION_MS);

        // Potion selection
        allowImpurePotions = readBool(section, "bAllowImpurePotions", Defaults::ALLOW_IMPURE_POTIONS);

        // Enable flags
        enableCriticalHealth = readBool(section, "bEnableCriticalHealth", Defaults::ENABLE_CRITICAL_HEALTH);
        enableDrowning = readBool(section, "bEnableDrowning", Defaults::ENABLE_DROWNING);
        enableWeaponCharge = readBool(section, "bEnableWeaponCharge", Defaults::ENABLE_WEAPON_CHARGE);

        // Log loaded values
        logger::info("[OverrideSettings] Critical Health: threshold={:.0f}%, hysteresis={:.0f}%, enabled={}"sv,
            criticalHealthThreshold * 100.0f,
            criticalHealthHysteresis * 100.0f,
            enableCriticalHealth);

        logger::info("[OverrideSettings] Weapon Charge: threshold={:.0f}%, hysteresis={:.0f}%, enabled={}"sv,
            weaponChargeThreshold * 100.0f,
            weaponChargeHysteresis * 100.0f,
            enableWeaponCharge);

        logger::info("[OverrideSettings] Drowning: enabled={}"sv, enableDrowning);

        logger::info("[OverrideSettings] Low Ammo: threshold={:.0f}, hysteresis={:.0f}, enabled={}"sv,
            lowAmmoThreshold, lowAmmoHysteresis, enableLowAmmo);

        logger::info("[OverrideSettings] Critical Magicka: threshold={:.0f}%, hysteresis={:.0f}%, enabled={}"sv,
            criticalMagickaThreshold * 100.0f, criticalMagickaHysteresis * 100.0f, enableCriticalMagicka);

        logger::info("[OverrideSettings] Critical Stamina: threshold={:.0f}%, hysteresis={:.0f}%, enabled={}"sv,
            criticalStaminaThreshold * 100.0f, criticalStaminaHysteresis * 100.0f, enableCriticalStamina);

        logger::info("[OverrideSettings] Anti-flicker duration: {:.0f}ms"sv, minOverrideDurationMs);

        logger::info("[OverrideSettings] Allow impure potions: {}"sv, allowImpurePotions);
    }

    void Settings::ResetToDefaults()
    {
        criticalHealthThreshold = Defaults::CRITICAL_HEALTH_THRESHOLD;
        criticalHealthHysteresis = Defaults::CRITICAL_HEALTH_HYSTERESIS;
        weaponChargeThreshold = Defaults::WEAPON_CHARGE_THRESHOLD;
        weaponChargeHysteresis = Defaults::WEAPON_CHARGE_HYSTERESIS;
        lowAmmoThreshold = Defaults::LOW_AMMO_THRESHOLD;
        lowAmmoHysteresis = Defaults::LOW_AMMO_HYSTERESIS;
        criticalMagickaThreshold = Defaults::CRITICAL_MAGICKA_THRESHOLD;
        criticalMagickaHysteresis = Defaults::CRITICAL_MAGICKA_HYSTERESIS;
        criticalStaminaThreshold = Defaults::CRITICAL_STAMINA_THRESHOLD;
        criticalStaminaHysteresis = Defaults::CRITICAL_STAMINA_HYSTERESIS;
        minOverrideDurationMs = Defaults::MIN_OVERRIDE_DURATION_MS;
        enableCriticalHealth = Defaults::ENABLE_CRITICAL_HEALTH;
        enableDrowning = Defaults::ENABLE_DROWNING;
        enableWeaponCharge = Defaults::ENABLE_WEAPON_CHARGE;
        enableLowAmmo = Defaults::ENABLE_LOW_AMMO;
        enableCriticalMagicka = Defaults::ENABLE_CRITICAL_MAGICKA;
        enableCriticalStamina = Defaults::ENABLE_CRITICAL_STAMINA;
        allowImpurePotions = Defaults::ALLOW_IMPURE_POTIONS;

        logger::info("[OverrideSettings] Reset to defaults"sv);
    }

}  // namespace Huginn::Override
