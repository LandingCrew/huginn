#pragma once

#include <filesystem>
#include <SimpleIni.h>

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
        constexpr int REC_LOG_VERBOSITY = 1;  // 0=off, 1=compact top-5, 2=full detail
    }

    /**
     * @brief Debug widget visibility + diagnostic logging settings
     * Widget visibility only affects debug builds; recLogVerbosity works in
     * release builds too (recommendation troubleshooting on regular runs).
     */
    struct DebugSettings
    {
        bool stateManagerVisible = DebugDefaults::STATE_MANAGER_VISIBLE;
        bool registryVisible = DebugDefaults::REGISTRY_VISIBLE;
        bool utilityScorerVisible = DebugDefaults::UTILITY_SCORER_VISIBLE;

        // Recommendation log verbosity (both build configs):
        //   0 = off, 1 = compact top-5 on ranking change, 2 = + learn inputs (Q/P/UCB/α)
        int recLogVerbosity = DebugDefaults::REC_LOG_VERBOSITY;

        /**
         * @brief Load [Debug] from the parsed dMenu INI (Settings::LoadDMenuIni,
         * over the dMenu descriptor's defaults)
         */
        void LoadFromIni(const CSimpleIniA& ini);

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
