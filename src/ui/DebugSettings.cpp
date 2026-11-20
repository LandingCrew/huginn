#include "DebugSettings.h"
#include "IniLoad.h"
#include <SimpleIni.h>
#include <algorithm>
#include <cmath>

// Debug widgets only exist in debug builds
#ifdef _DEBUG
#include "CharacterDebugWidget.h"
#include "EnvironmentDebugWidget.h"
#include "../state/DropAheadProbe.h"
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

    // [Debug] lives in ONE file: dMenu's own INI. See IntuitionSettings::LoadFromIni.
    void DebugSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "Debug";

        characterVisible = ini.GetBoolValue(section, "bShowCharacter", DebugDefaults::CHARACTER_VISIBLE);
        environmentVisible = ini.GetBoolValue(section, "bShowEnvironment", DebugDefaults::ENVIRONMENT_VISIBLE);
        drawEnvProbe = ini.GetBoolValue(section, "bDrawEnvProbe", DebugDefaults::DRAW_ENV_PROBE);
        registryVisible = ini.GetBoolValue(section, "bShowRegistry", DebugDefaults::REGISTRY_VISIBLE);
        utilityScorerVisible = ini.GetBoolValue(section, "bShowUtilityScorer", DebugDefaults::UTILITY_SCORER_VISIBLE);
        // A dMenu slider: dMenu writes it as a float ("2.000000"), and
        // GetLongValue returns its fallback for anything but integer text, so
        // read as a long the slider never took (it read 1 whatever was set).
        // Read the float and round it; integer text ("2") reads the same.
        const double recLog = ini.GetDoubleValue(section, "iRecommendationLog", DebugDefaults::REC_LOG_VERBOSITY);
        recLogVerbosity = std::isfinite(recLog)
            ? static_cast<int>(std::clamp(std::lround(recLog), 0L, 2L))
            : DebugDefaults::REC_LOG_VERBOSITY;

        logger::info("[DebugSettings] Loaded: Character={}, Environment={}, ProbeRays={}, Registry={}, UtilityScorer={}, RecLog={}"sv,
            characterVisible, environmentVisible, drawEnvProbe, registryVisible, utilityScorerVisible, recLogVerbosity);
        // No ApplyToWidgets() here — LoadFromIni is a pure loader (Phase 1), like
        // every other settings class. Callers apply visibility explicitly:
        // InitializeGameSystems (and its early kDataLoaded read), and the reload path's
        // ApplySideEffects (Phase 2).
    }

    void DebugSettings::ApplyToWidgets()
    {
#ifdef _DEBUG
        // Only apply in debug builds where widgets exist
        CharacterDebugWidget::GetSingleton().SetVisible(characterVisible);
        EnvironmentDebugWidget::GetSingleton().SetVisible(environmentVisible);
        State::DropAheadProbe::SetDebugDraw(drawEnvProbe);
        RegistryDebugWidget::GetSingleton().SetVisible(registryVisible);
        UtilityScorerDebugWidget::GetSingleton().SetVisible(utilityScorerVisible);

        logger::debug("[DebugSettings] Applied visibility to debug widgets"sv);
#else
        // No-op in release builds
        logger::trace("[DebugSettings] ApplyToWidgets called in release build (no-op)"sv);
#endif
    }

}  // namespace Huginn::UI
