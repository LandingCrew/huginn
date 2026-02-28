#pragma once

#include <SimpleIni.h>
#include <filesystem>

namespace Huginn::Override
{
    // =============================================================================
    // DEFAULT VALUES (compile-time constants)
    // =============================================================================
    // These are the fallback values used if INI loading fails or values are missing.
    // =============================================================================

    namespace Defaults
    {
        // Critical Health Override
        inline constexpr float CRITICAL_HEALTH_THRESHOLD = 0.10f;    // 10% - trigger activation
        inline constexpr float CRITICAL_HEALTH_HYSTERESIS = 0.15f;   // 15% - trigger deactivation

        // Weapon Charge Override
        inline constexpr float WEAPON_CHARGE_THRESHOLD = 0.0f;       // 0% - trigger activation
        inline constexpr float WEAPON_CHARGE_HYSTERESIS = 0.05f;     // 5% - trigger deactivation

        // Low Ammo Override (absolute counts, not percentages)
        inline constexpr float LOW_AMMO_THRESHOLD = 10.0f;           // 10 arrows/bolts - trigger activation
        inline constexpr float LOW_AMMO_HYSTERESIS = 15.0f;          // 15 arrows/bolts - trigger deactivation

        // Critical Magicka Override
        inline constexpr float CRITICAL_MAGICKA_THRESHOLD = 0.10f;   // 10% - trigger activation
        inline constexpr float CRITICAL_MAGICKA_HYSTERESIS = 0.15f;  // 15% - trigger deactivation

        // Critical Stamina Override
        inline constexpr float CRITICAL_STAMINA_THRESHOLD = 0.10f;   // 10% - trigger activation
        inline constexpr float CRITICAL_STAMINA_HYSTERESIS = 0.15f;  // 15% - trigger deactivation

        // Anti-flicker
        inline constexpr float MIN_OVERRIDE_DURATION_MS = 2000.0f;   // 2 seconds minimum

        // Enable flags
        inline constexpr bool ENABLE_CRITICAL_HEALTH = true;
        inline constexpr bool ENABLE_DROWNING = true;
        inline constexpr bool ENABLE_WEAPON_CHARGE = true;
        inline constexpr bool ENABLE_LOW_AMMO = true;
        inline constexpr bool ENABLE_CRITICAL_MAGICKA = true;
        inline constexpr bool ENABLE_CRITICAL_STAMINA = true;

        // Potion selection
        inline constexpr bool ALLOW_IMPURE_POTIONS = true;       // Fall back to potions with side effects (Skooma etc.)

        // Utility boost (not user-configurable)
        inline constexpr float OVERRIDE_UTILITY_BOOST = 1000.0f;
    }

    // =============================================================================
    // RUNTIME SETTINGS (v0.10.0)
    // =============================================================================
    // Singleton class that holds runtime-configurable override settings.
    // Settings are loaded from Data/SKSE/Plugins/Huginn.ini [Overrides] section.
    // =============================================================================

    class Settings
    {
    public:
        static Settings& GetSingleton()
        {
            static Settings instance;
            return instance;
        }

        // Load settings from INI file
        void LoadFromFile(const std::filesystem::path& iniPath);

        // Reset to defaults
        void ResetToDefaults();

        // =========================================================================
        // CRITICAL HEALTH OVERRIDE
        // =========================================================================
        float criticalHealthThreshold = Defaults::CRITICAL_HEALTH_THRESHOLD;
        float criticalHealthHysteresis = Defaults::CRITICAL_HEALTH_HYSTERESIS;

        // =========================================================================
        // WEAPON CHARGE OVERRIDE
        // =========================================================================
        float weaponChargeThreshold = Defaults::WEAPON_CHARGE_THRESHOLD;
        float weaponChargeHysteresis = Defaults::WEAPON_CHARGE_HYSTERESIS;

        // =========================================================================
        // LOW AMMO OVERRIDE (absolute counts, not percentages)
        // =========================================================================
        float lowAmmoThreshold = Defaults::LOW_AMMO_THRESHOLD;
        float lowAmmoHysteresis = Defaults::LOW_AMMO_HYSTERESIS;

        // =========================================================================
        // CRITICAL MAGICKA OVERRIDE
        // =========================================================================
        float criticalMagickaThreshold = Defaults::CRITICAL_MAGICKA_THRESHOLD;
        float criticalMagickaHysteresis = Defaults::CRITICAL_MAGICKA_HYSTERESIS;

        // =========================================================================
        // CRITICAL STAMINA OVERRIDE
        // =========================================================================
        float criticalStaminaThreshold = Defaults::CRITICAL_STAMINA_THRESHOLD;
        float criticalStaminaHysteresis = Defaults::CRITICAL_STAMINA_HYSTERESIS;

        // =========================================================================
        // ANTI-FLICKER SETTINGS
        // =========================================================================
        float minOverrideDurationMs = Defaults::MIN_OVERRIDE_DURATION_MS;

        // =========================================================================
        // ENABLE FLAGS
        // =========================================================================
        bool enableCriticalHealth = Defaults::ENABLE_CRITICAL_HEALTH;
        bool enableDrowning = Defaults::ENABLE_DROWNING;
        bool enableWeaponCharge = Defaults::ENABLE_WEAPON_CHARGE;
        bool enableLowAmmo = Defaults::ENABLE_LOW_AMMO;
        bool enableCriticalMagicka = Defaults::ENABLE_CRITICAL_MAGICKA;
        bool enableCriticalStamina = Defaults::ENABLE_CRITICAL_STAMINA;

        // =========================================================================
        // POTION SELECTION
        // =========================================================================
        bool allowImpurePotions = Defaults::ALLOW_IMPURE_POTIONS;

    private:
        Settings() = default;
        ~Settings() = default;
        Settings(const Settings&) = delete;
        Settings& operator=(const Settings&) = delete;
    };

    // =============================================================================
    // CONVENIENCE NAMESPACE (for backward compatibility)
    // =============================================================================
    // These provide easy access to current settings while maintaining the old
    // Config::CONSTANT syntax where possible.
    // =============================================================================

    namespace Config
    {
        // These are now functions that read from the runtime Settings singleton
        inline float CRITICAL_HEALTH_THRESHOLD() { return Settings::GetSingleton().criticalHealthThreshold; }
        inline float CRITICAL_HEALTH_HYSTERESIS() { return Settings::GetSingleton().criticalHealthHysteresis; }
        inline float WEAPON_CHARGE_THRESHOLD() { return Settings::GetSingleton().weaponChargeThreshold; }
        inline float WEAPON_CHARGE_HYSTERESIS() { return Settings::GetSingleton().weaponChargeHysteresis; }
        inline float MIN_OVERRIDE_DURATION_MS() { return Settings::GetSingleton().minOverrideDurationMs; }
        inline bool ENABLE_CRITICAL_HEALTH() { return Settings::GetSingleton().enableCriticalHealth; }
        inline bool ENABLE_DROWNING() { return Settings::GetSingleton().enableDrowning; }
        inline bool ENABLE_WEAPON_CHARGE() { return Settings::GetSingleton().enableWeaponCharge; }
        inline float LOW_AMMO_THRESHOLD() { return Settings::GetSingleton().lowAmmoThreshold; }
        inline float LOW_AMMO_HYSTERESIS() { return Settings::GetSingleton().lowAmmoHysteresis; }
        inline bool ENABLE_LOW_AMMO() { return Settings::GetSingleton().enableLowAmmo; }
        inline float CRITICAL_MAGICKA_THRESHOLD() { return Settings::GetSingleton().criticalMagickaThreshold; }
        inline float CRITICAL_MAGICKA_HYSTERESIS() { return Settings::GetSingleton().criticalMagickaHysteresis; }
        inline bool ENABLE_CRITICAL_MAGICKA() { return Settings::GetSingleton().enableCriticalMagicka; }
        inline float CRITICAL_STAMINA_THRESHOLD() { return Settings::GetSingleton().criticalStaminaThreshold; }
        inline float CRITICAL_STAMINA_HYSTERESIS() { return Settings::GetSingleton().criticalStaminaHysteresis; }
        inline bool ENABLE_CRITICAL_STAMINA() { return Settings::GetSingleton().enableCriticalStamina; }
        inline bool ALLOW_IMPURE_POTIONS() { return Settings::GetSingleton().allowImpurePotions; }

        // This one stays constexpr (not user-configurable)
        inline constexpr float OVERRIDE_UTILITY_BOOST = Defaults::OVERRIDE_UTILITY_BOOST;
    }

}  // namespace Huginn::Override
