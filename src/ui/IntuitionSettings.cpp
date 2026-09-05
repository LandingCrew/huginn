#include "IntuitionSettings.h"

#include <cstdlib>
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

    // [Widget] lives in ONE file: dMenu's own INI. Nothing overlays it, so each
    // key falls back to its compile-time default.
    void IntuitionSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "Widget";

        enabled   = ini.GetBoolValue(section, "bEnabled", IntuitionDefaults::ENABLED);
        positionX = static_cast<float>(ini.GetDoubleValue(section, "fPositionX", IntuitionDefaults::POSITION_X));
        positionY = static_cast<float>(ini.GetDoubleValue(section, "fPositionY", IntuitionDefaults::POSITION_Y));
        alpha     = static_cast<float>(ini.GetDoubleValue(section, "fAlpha", IntuitionDefaults::ALPHA));
        scale        = static_cast<float>(ini.GetDoubleValue(section, "fScale", IntuitionDefaults::SCALE));
        childAlpha   = static_cast<float>(ini.GetDoubleValue(section, "fAlphaChild", IntuitionDefaults::CHILD_ALPHA));
        readOnly     = ini.GetBoolValue(section, "bReadOnly", IntuitionDefaults::READ_ONLY);
        hideWhileWheelOpen = ini.GetBoolValue(section, "bHideWhileWheelOpen",
                                              IntuitionDefaults::HIDE_WHILE_WHEEL_OPEN);

        // These three are dMenu dropdowns, and dMenu serializes a dropdown as its
        // integer INDEX ("0"), not its label. Hand-written INIs and every version
        // of this file before dMenu owned [Widget] use the name ("minimal"), so
        // both spellings are accepted. DropdownIndex returns -1 for a non-numeric
        // value, which falls through to the name match.
        //
        // The index order below MUST match the "options" arrays in
        // Data/SKSE/Plugins/dmenu/customSettings/Huginn.json. Index 0 is the
        // default in each case, so an unrecognised value lands on the same
        // setting the name-matching fallback would have chosen.
        const auto dropdownIndex = [](const char* v) -> int {
            if (!v || !*v) return -1;
            char* end = nullptr;
            const long i = std::strtol(v, &end, 10);
            // Reject trailing junk so "1minimal" is treated as a name, not index 1
            return (end && *end == '\0' && i >= 0) ? static_cast<int>(i) : -1;
        };

        const char* modeStr = ini.GetValue(section, "sDisplayMode", IntuitionDefaults::DISPLAY_MODE);
        switch (dropdownIndex(modeStr)) {
        case 0:  displayMode = DisplayMode::Minimal; break;
        case 1:  displayMode = DisplayMode::Normal;  break;
        case 2:  displayMode = DisplayMode::Verbose; break;
        default:
            if (_stricmp(modeStr, "normal") == 0) displayMode = DisplayMode::Normal;
            else if (_stricmp(modeStr, "verbose") == 0) displayMode = DisplayMode::Verbose;
            else displayMode = DisplayMode::Minimal;
            break;
        }

        const char* refreshStr = ini.GetValue(section, "sRefreshEffect", IntuitionDefaults::REFRESH_EFFECT);
        switch (dropdownIndex(refreshStr)) {
        case 0:  refreshEffect = RefreshEffect::Tint;  break;
        case 1:  refreshEffect = RefreshEffect::Pulse; break;
        case 2:  refreshEffect = RefreshEffect::None;  break;
        default:
            if (_stricmp(refreshStr, "pulse") == 0 || _stricmp(refreshStr, "flash") == 0) refreshEffect = RefreshEffect::Pulse;
            else if (_stricmp(refreshStr, "none") == 0) refreshEffect = RefreshEffect::None;
            else refreshEffect = RefreshEffect::Tint;
            break;
        }

        refreshStrength = std::clamp(static_cast<float>(ini.GetDoubleValue(section, "fRefreshStrength", IntuitionDefaults::REFRESH_STRENGTH)), 0.0f, 100.0f);

        const char* slotStr = ini.GetValue(section, "sSlotEffect", IntuitionDefaults::SLOT_EFFECT);
        switch (dropdownIndex(slotStr)) {
        case 0:  slotEffect = SlotEffect::Slide;   break;
        case 1:  slotEffect = SlotEffect::Fade;    break;
        case 2:  slotEffect = SlotEffect::Instant; break;
        default:
            if (_stricmp(slotStr, "fade") == 0) slotEffect = SlotEffect::Fade;
            else if (_stricmp(slotStr, "instant") == 0) slotEffect = SlotEffect::Instant;
            else slotEffect = SlotEffect::Slide;
            break;
        }

        logger::info("[IntuitionSettings] Enabled: {}, Position: ({}%, {}%), Alpha: {}, Scale: {}%, ChildAlpha: {}, ReadOnly: {}, HideWhileWheelOpen: {}, DisplayMode: {}, RefreshEffect: {} ({}%), SlotEffect: {}",
            enabled, positionX, positionY, alpha, scale, childAlpha, readOnly,
            HideWhileWheelOpen(),
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
        readOnly       = IntuitionDefaults::READ_ONLY;
        hideWhileWheelOpen = IntuitionDefaults::HIDE_WHILE_WHEEL_OPEN;
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
