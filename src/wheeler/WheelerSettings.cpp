#include "WheelerSettings.h"
#include <stdexcept>

namespace Huginn::Wheeler
{
    // Parse PostActivationPolicy from string
    static PostActivationPolicy ParsePostActivationPolicy(const char* str)
    {
        if (_stricmp(str, "Sticky") == 0) return PostActivationPolicy::Sticky;
        if (_stricmp(str, "Empty") == 0)  return PostActivationPolicy::Empty;
        if (_stricmp(str, "Backfill") == 0) return PostActivationPolicy::Backfill;

        logger::warn("[WheelerSettings] Invalid sPostActivationPolicy '{}', using default (Backfill)", str);
        return WheelerDefaults::POST_ACTIVATION_POLICY;
    }

    // Parse wheel position from string: "First", "Last", or numeric index
    static int32_t ParseWheelPosition(const char* str)
    {
        if (_stricmp(str, "First") == 0) return 0;
        if (_stricmp(str, "Last") == 0)  return -1;

        // Try parsing as integer
        try {
            return std::stoi(str);
        } catch (const std::invalid_argument&) {
            logger::warn("[WheelerSettings] Invalid sWheelPosition '{}', using default (First)", str);
            return WheelerDefaults::WHEEL_POSITION;
        } catch (const std::out_of_range&) {
            logger::warn("[WheelerSettings] sWheelPosition '{}' out of range, using default (First)", str);
            return WheelerDefaults::WHEEL_POSITION;
        }
    }

    void WheelerSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            logger::info("[WheelerSettings] INI file not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            logger::error("[WheelerSettings] Failed to load INI file: {}"sv, iniPath.string());
            return;
        }

        const char* section = "Wheeler";

        // Parse position: "First", "Last", or numeric
        const char* posStr = ini.GetValue(section, "sWheelPosition", "First");
        wheelPosition = ParseWheelPosition(posStr);

        // Auto-focus on open
        autoFocusOnOpen = ini.GetBoolValue(section, "bAutoFocusOnOpen", WheelerDefaults::AUTO_FOCUS_ON_OPEN);

        // Auto-focus on override (snap to Huginn wheel when override fires while Wheeler is open)
        autoFocusOnOverride = ini.GetBoolValue(section, "bAutoFocusOnOverride", WheelerDefaults::AUTO_FOCUS_ON_OVERRIDE);
        autoFocusMinPriority = static_cast<int>(ini.GetLongValue(section, "iAutoFocusMinPriority", WheelerDefaults::AUTO_FOCUS_MIN_PRIORITY));

        // Post-activation policy
        const char* policyStr = ini.GetValue(section, "sPostActivationPolicy", "Backfill");
        postActivationPolicy = ParsePostActivationPolicy(policyStr);

        const char* policyNames[] = {"Backfill", "Sticky", "Empty"};
        logger::info("[WheelerSettings] Position: {} ({}), Auto-focus: open={} override={} (minPri={}), PostActivation: {}",
            wheelPosition, posStr, autoFocusOnOpen, autoFocusOnOverride, autoFocusMinPriority,
            policyNames[static_cast<int>(postActivationPolicy)]);

        // [Subtexts] section
        const char* stSection = "Subtexts";
        subtextLabels.showWildcardLabel = ini.GetBoolValue(stSection, "bShowWildcardLabel", WheelerDefaults::SHOW_WILDCARD_LABEL);
        subtextLabels.showOverrideLabel = ini.GetBoolValue(stSection, "bShowOverrideLabel", WheelerDefaults::SHOW_OVERRIDE_LABEL);
        subtextLabels.showLockTimerLabel = ini.GetBoolValue(stSection, "bShowLockTimerLabel", WheelerDefaults::SHOW_LOCK_TIMER_LABEL);
        subtextLabels.showExplanationLabel = ini.GetBoolValue(stSection, "bShowExplanationLabel", WheelerDefaults::SHOW_EXPLANATION_LABEL);
        subtextLabels.wildcardLabelText = ini.GetValue(stSection, "sWildcardLabelText", "Wildcard");
        subtextLabels.lockTimerShowSeconds = ini.GetBoolValue(stSection, "bLockTimerShowSeconds", WheelerDefaults::LOCK_TIMER_SHOW_SECONDS);
        subtextLabels.lockTimerPrefix = ini.GetValue(stSection, "sLockTimerPrefix", "Locked");
        subtextLabels.offsetX = static_cast<float>(ini.GetDoubleValue(stSection, "fOffsetX", WheelerDefaults::SUBTEXT_OFFSET_X));
        subtextLabels.offsetY = static_cast<float>(ini.GetDoubleValue(stSection, "fOffsetY", WheelerDefaults::SUBTEXT_OFFSET_Y));

        logger::info("[WheelerSettings] Subtexts: wildcard={} ('{}'), override={}, lockTimer={}, explanation={}, offset=({}, {})",
            subtextLabels.showWildcardLabel, subtextLabels.wildcardLabelText,
            subtextLabels.showOverrideLabel, subtextLabels.showLockTimerLabel,
            subtextLabels.showExplanationLabel, subtextLabels.offsetX, subtextLabels.offsetY);
    }

    void WheelerSettings::ResetToDefaults()
    {
        wheelPosition = WheelerDefaults::WHEEL_POSITION;
        autoFocusOnOpen = WheelerDefaults::AUTO_FOCUS_ON_OPEN;
        autoFocusOnOverride = WheelerDefaults::AUTO_FOCUS_ON_OVERRIDE;
        autoFocusMinPriority = WheelerDefaults::AUTO_FOCUS_MIN_PRIORITY;
        postActivationPolicy = WheelerDefaults::POST_ACTIVATION_POLICY;
        subtextLabels = SubtextLabelConfig{};

        logger::info("[WheelerSettings] Reset to defaults"sv);
    }

}  // namespace Huginn::Wheeler
