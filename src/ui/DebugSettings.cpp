#include "DebugSettings.h"
#include "IniLoad.h"
#include <SimpleIni.h>
#include <algorithm>

// Debug widgets only exist in debug builds
#ifdef _DEBUG
#include "StateManagerDebugWidget.h"
#include "RegistryDebugWidget.h"
#include "UtilityScorerDebugWidget.h"
#endif

namespace Huginn::UI
{
    DebugSettings& DebugSettings::GetSingleton() noexcept
    {
        static DebugSettings instance;
        return instance;
    }

    void DebugSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "DebugSettings"sv)) {
            LoadFromIni(ini);
            ApplyToWidgets();  // standalone load-then-apply convenience
        }
    }

    void DebugSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "Debug";

        stateManagerVisible = ini.GetBoolValue(section, "bShowStateManager", DebugDefaults::STATE_MANAGER_VISIBLE);
        registryVisible = ini.GetBoolValue(section, "bShowRegistry", DebugDefaults::REGISTRY_VISIBLE);
        utilityScorerVisible = ini.GetBoolValue(section, "bShowUtilityScorer", DebugDefaults::UTILITY_SCORER_VISIBLE);
        recLogVerbosity = std::clamp(
            static_cast<int>(ini.GetLongValue(section, "iRecommendationLog", DebugDefaults::REC_LOG_VERBOSITY)),
            0, 2);

        logger::info("[DebugSettings] Loaded: StateManager={}, Registry={}, UtilityScorer={}, RecLog={}"sv,
            stateManagerVisible, registryVisible, utilityScorerVisible, recLogVerbosity);
        // No ApplyToWidgets() here — LoadFromIni is a pure loader (Phase 1), like
        // every other settings class. Callers apply visibility explicitly: the
        // LoadFromFile wrapper, InitializeGameSystems, and the reload path's
        // ApplySideEffects (Phase 2).
    }

    void DebugSettings::ResetToDefaults()
    {
        stateManagerVisible = DebugDefaults::STATE_MANAGER_VISIBLE;
        registryVisible = DebugDefaults::REGISTRY_VISIBLE;
        utilityScorerVisible = DebugDefaults::UTILITY_SCORER_VISIBLE;
        recLogVerbosity = DebugDefaults::REC_LOG_VERBOSITY;

        logger::info("[DebugSettings] Reset to defaults"sv);

        // Apply to widgets
        ApplyToWidgets();
    }

    void DebugSettings::ApplyToWidgets()
    {
#ifdef _DEBUG
        // Only apply in debug builds where widgets exist
        StateManagerDebugWidget::GetSingleton().SetVisible(stateManagerVisible);
        RegistryDebugWidget::GetSingleton().SetVisible(registryVisible);
        UtilityScorerDebugWidget::GetSingleton().SetVisible(utilityScorerVisible);

        logger::debug("[DebugSettings] Applied visibility to debug widgets"sv);
#else
        // No-op in release builds
        logger::trace("[DebugSettings] ApplyToWidgets called in release build (no-op)"sv);
#endif
    }

}  // namespace Huginn::UI
