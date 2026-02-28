#include "IntuitionSettings.h"

namespace Huginn::UI
{
    void IntuitionSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            logger::info("[IntuitionSettings] INI file not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            logger::error("[IntuitionSettings] Failed to load INI file: {}"sv, iniPath.string());
            return;
        }

        const char* section = "Widget";

        enabled   = ini.GetBoolValue(section, "bEnabled", IntuitionDefaults::ENABLED);
        positionX = static_cast<float>(ini.GetDoubleValue(section, "fPositionX", IntuitionDefaults::POSITION_X));
        positionY = static_cast<float>(ini.GetDoubleValue(section, "fPositionY", IntuitionDefaults::POSITION_Y));
        alpha     = static_cast<float>(ini.GetDoubleValue(section, "fAlpha", IntuitionDefaults::ALPHA));
        scale        = static_cast<float>(ini.GetDoubleValue(section, "fScale", IntuitionDefaults::SCALE));
        childAlpha   = static_cast<float>(ini.GetDoubleValue(section, "fAlphaChild", IntuitionDefaults::CHILD_ALPHA));

        const char* modeStr = ini.GetValue(section, "sDisplayMode", IntuitionDefaults::DISPLAY_MODE);
        if (_stricmp(modeStr, "normal") == 0) displayMode = DisplayMode::Normal;
        else if (_stricmp(modeStr, "verbose") == 0) displayMode = DisplayMode::Verbose;
        else displayMode = DisplayMode::Minimal;

        const char* refreshStr = ini.GetValue(section, "sRefreshEffect", IntuitionDefaults::REFRESH_EFFECT);
        if (_stricmp(refreshStr, "pulse") == 0 || _stricmp(refreshStr, "flash") == 0) refreshEffect = RefreshEffect::Pulse;
        else if (_stricmp(refreshStr, "none") == 0) refreshEffect = RefreshEffect::None;
        else refreshEffect = RefreshEffect::Tint;

        refreshStrength = std::clamp(static_cast<float>(ini.GetDoubleValue(section, "fRefreshStrength", IntuitionDefaults::REFRESH_STRENGTH)), 0.0f, 100.0f);

        const char* slotStr = ini.GetValue(section, "sSlotEffect", IntuitionDefaults::SLOT_EFFECT);
        if (_stricmp(slotStr, "fade") == 0) slotEffect = SlotEffect::Fade;
        else if (_stricmp(slotStr, "instant") == 0) slotEffect = SlotEffect::Instant;
        else slotEffect = SlotEffect::Slide;

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
