#pragma once

#include <filesystem>

namespace Huginn::UI
{
    /**
     * @brief Default debug widget visibility values
     */
    namespace DebugDefaults
    {
        constexpr bool STATE_MANAGER_VISIBLE = false;
        constexpr bool REGISTRY_VISIBLE = false;
        constexpr bool UTILITY_SCORER_VISIBLE = false;
    }

    /**
     * @brief Debug widget visibility settings
     * Controls which debug overlays are shown (debug builds only)
     */
    struct DebugSettings
    {
        bool stateManagerVisible = DebugDefaults::STATE_MANAGER_VISIBLE;
        bool registryVisible = DebugDefaults::REGISTRY_VISIBLE;
        bool utilityScorerVisible = DebugDefaults::UTILITY_SCORER_VISIBLE;

        /**
         * @brief Load settings from INI file
         * @param iniPath Path to Huginn.ini
         */
        void LoadFromFile(const std::filesystem::path& iniPath);

        /**
         * @brief Reset all settings to compile-time defaults
         */
        void ResetToDefaults();

        /**
         * @brief Apply visibility settings to debug widgets
         * Only affects debug builds - no-op in release
         */
        void ApplyToWidgets();

        /**
         * @brief Get singleton instance
         */
        static DebugSettings& GetSingleton() noexcept;

    private:
        DebugSettings() = default;
        ~DebugSettings() = default;
        DebugSettings(const DebugSettings&) = delete;
        DebugSettings& operator=(const DebugSettings&) = delete;
    };
}
