#include "IntuitionSettings.h"
#include "IniLoad.h"

namespace Huginn::UI
{
    void IntuitionSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "IntuitionSettings"sv)) {
            LoadFromIni(ini);
        }
    }

    // Every key falls back to the CURRENT member value, not the compile-time
    // default, so calls layer: load the dMenu INI, then overlay the main INI.
    // A key absent from the second file keeps what the first one set. String
    // keys use a nullptr probe for the same reason — GetValue with a default
    // cannot distinguish "absent" from "set to the default".
    void IntuitionSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "Widget";

        enabled   = ini.GetBoolValue(section, "bEnabled", enabled);
        positionX = static_cast<float>(ini.GetDoubleValue(section, "fPositionX", positionX));
        positionY = static_cast<float>(ini.GetDoubleValue(section, "fPositionY", positionY));
        alpha     = static_cast<float>(ini.GetDoubleValue(section, "fAlpha", alpha));
        scale        = static_cast<float>(ini.GetDoubleValue(section, "fScale", scale));
        childAlpha   = static_cast<float>(ini.GetDoubleValue(section, "fAlphaChild", childAlpha));

        if (const char* modeStr = ini.GetValue(section, "sDisplayMode", nullptr)) {
            if (_stricmp(modeStr, "normal") == 0) displayMode = DisplayMode::Normal;
            else if (_stricmp(modeStr, "verbose") == 0) displayMode = DisplayMode::Verbose;
            else displayMode = DisplayMode::Minimal;
        }

        if (const char* refreshStr = ini.GetValue(section, "sRefreshEffect", nullptr)) {
            if (_stricmp(refreshStr, "pulse") == 0 || _stricmp(refreshStr, "flash") == 0) refreshEffect = RefreshEffect::Pulse;
            else if (_stricmp(refreshStr, "none") == 0) refreshEffect = RefreshEffect::None;
            else refreshEffect = RefreshEffect::Tint;
        }

        refreshStrength = std::clamp(static_cast<float>(ini.GetDoubleValue(section, "fRefreshStrength", refreshStrength)), 0.0f, 100.0f);

        if (const char* slotStr = ini.GetValue(section, "sSlotEffect", nullptr)) {
            if (_stricmp(slotStr, "fade") == 0) slotEffect = SlotEffect::Fade;
            else if (_stricmp(slotStr, "instant") == 0) slotEffect = SlotEffect::Instant;
            else slotEffect = SlotEffect::Slide;
        }

        logger::info("[IntuitionSettings] Enabled: {}, Position: ({}%, {}%), Alpha: {}, Scale: {}%, ChildAlpha: {}, DisplayMode: {}, RefreshEffect: {} ({}%), SlotEffect: {}",
            enabled, positionX, positionY, alpha, scale, childAlpha,
            displayMode == DisplayMode::Verbose ? "verbose" : displayMode == DisplayMode::Normal ? "normal" : "minimal",
            refreshEffect == RefreshEffect::Pulse ? "pulse" : refreshEffect == RefreshEffect::None ? "none" : "tint",
            refreshStrength,
            slotEffect == SlotEffect::Fade ? "fade" : slotEffect == SlotEffect::Instant ? "instant" : "slide");
    }

    void IntuitionSettings::ResetToDefaults()
    {
        enabled   = IntuitionDefaults::ENABLED;
        positionX = IntuitionDefaults::POSITION_X;
        positionY = IntuitionDefaults::POSITION_Y;
        alpha     = IntuitionDefaults::ALPHA;
        scale        = IntuitionDefaults::SCALE;
        childAlpha     = IntuitionDefaults::CHILD_ALPHA;
        displayMode    = DisplayMode::Minimal;
        refreshEffect  = RefreshEffect::Tint;
        refreshStrength = IntuitionDefaults::REFRESH_STRENGTH;
        slotEffect     = SlotEffect::Slide;

        logger::info("[IntuitionSettings] Reset to defaults"sv);
    }

    IntuitionConfig IntuitionSettings::BuildConfig() const
    {
        IntuitionConfig config;
        config.enabled = enabled;
        config.positionX = positionX;
        config.positionY = positionY;
        config.alpha = alpha;
        config.scale = scale;
        config.childAlpha = childAlpha;
        config.displayMode = displayMode;
        config.refreshEffect = refreshEffect;
        config.refreshStrength = refreshStrength;
        config.slotEffect = slotEffect;
        return config;
    }

}  // namespace Huginn::UI
