#pragma once

#include <SimpleIni.h>
#include <filesystem>
#include <string>

namespace Huginn::Wheeler
{
    // =============================================================================
    // DEFAULT VALUES (compile-time constants)
    // =============================================================================

    // =============================================================================
    // POST-ACTIVATION POLICY
    // =============================================================================
    // Controls what happens to a slot after the player activates (equips/uses) its item.
    // =============================================================================

    enum class PostActivationPolicy : uint8_t
    {
        Backfill,   // Immediately replace with next-best candidate (default)
        Sticky,     // Keep the activated item visible until context changes
        Empty       // Clear the slot (shows "Equipped" subtext label)
    };

    namespace WheelerDefaults
    {
        // Wheel position: 0 = First (prepend), -1 = Last (append), N = specific index
        inline constexpr int32_t WHEEL_POSITION = 0;

        // Auto-focus: snap to Huginn wheel when Wheeler opens
        inline constexpr bool AUTO_FOCUS_ON_OPEN = true;

        // Auto-focus on override: snap to Huginn wheel when an override fires while Wheeler is open
        inline constexpr bool AUTO_FOCUS_ON_OVERRIDE = true;
        inline constexpr int AUTO_FOCUS_MIN_PRIORITY = 50;  // DROWNING=50, CRITICAL_HEALTH=100

        // Post-activation policy
        inline constexpr PostActivationPolicy POST_ACTIVATION_POLICY = PostActivationPolicy::Backfill;

        // Subtext label defaults
        inline constexpr bool SHOW_WILDCARD_LABEL = true;
        inline constexpr bool SHOW_OVERRIDE_LABEL = true;
        inline constexpr bool SHOW_LOCK_TIMER_LABEL = false;   // Off by default (can be noisy)
        inline constexpr bool SHOW_EXPLANATION_LABEL = true;
        inline constexpr bool LOCK_TIMER_SHOW_SECONDS = true;

        // Subtext position defaults (pixels relative to entry center)
        inline constexpr float SUBTEXT_OFFSET_X = 0.0f;
        inline constexpr float SUBTEXT_OFFSET_Y = 20.0f;
    }

    // =============================================================================
    // SUBTEXT LABEL CONFIGURATION
    // =============================================================================
    // Per-label settings for Wheeler entry subtexts (v2 API only).
    // Controls which contextual labels are shown below items in the wheel.
    // =============================================================================

    struct SubtextLabelConfig
    {
        // Enable/disable toggles
        bool showWildcardLabel = WheelerDefaults::SHOW_WILDCARD_LABEL;
        bool showOverrideLabel = WheelerDefaults::SHOW_OVERRIDE_LABEL;
        bool showLockTimerLabel = WheelerDefaults::SHOW_LOCK_TIMER_LABEL;
        bool showExplanationLabel = WheelerDefaults::SHOW_EXPLANATION_LABEL;

        // Customizable label text
        std::string wildcardLabelText = "Wildcard";

        // Lock timer format
        bool lockTimerShowSeconds = WheelerDefaults::LOCK_TIMER_SHOW_SECONDS;
        std::string lockTimerPrefix = "Locked";

        // Position offsets (pixels relative to entry center)
        float offsetX = WheelerDefaults::SUBTEXT_OFFSET_X;
        float offsetY = WheelerDefaults::SUBTEXT_OFFSET_Y;
    };

    // =============================================================================
    // RUNTIME SETTINGS
    // =============================================================================
    // Singleton class that holds runtime-configurable Wheeler settings.
    // Settings are loaded from Data/SKSE/Plugins/Huginn.ini [Wheeler] section.
    // =============================================================================

    class WheelerSettings
    {
    public:
        static WheelerSettings& GetSingleton()
        {
            static WheelerSettings instance;
            return instance;
        }

        // Load settings from INI file
        void LoadFromFile(const std::filesystem::path& iniPath);

        // Reset to defaults
        void ResetToDefaults();

        // Returns the int32_t position value to pass to Wheeler API
        // 0 = First, -1 = Last (append), N = specific index
        [[nodiscard]] int32_t GetAPIPosition() const noexcept { return wheelPosition; }

        // Returns whether to auto-focus Huginn wheel when Wheeler opens
        [[nodiscard]] bool GetAutoFocusOnOpen() const noexcept { return autoFocusOnOpen; }

        // Returns whether to auto-focus Huginn wheel when an override fires while Wheeler is open
        [[nodiscard]] bool GetAutoFocusOnOverride() const noexcept { return autoFocusOnOverride; }

        // Returns minimum override priority required to trigger auto-focus
        [[nodiscard]] int GetAutoFocusMinPriority() const noexcept { return autoFocusMinPriority; }

        // Returns post-activation slot behavior policy
        [[nodiscard]] PostActivationPolicy GetPostActivationPolicy() const noexcept { return postActivationPolicy; }

        // Returns subtext label configuration (for Wheeler v2 entry labels)
        [[nodiscard]] const SubtextLabelConfig& GetSubtextLabels() const noexcept { return subtextLabels; }

    private:
        WheelerSettings() = default;
        ~WheelerSettings() = default;
        WheelerSettings(const WheelerSettings&) = delete;
        WheelerSettings& operator=(const WheelerSettings&) = delete;

        // =========================================================================
        // WHEEL POSITION
        // =========================================================================
        int32_t wheelPosition = WheelerDefaults::WHEEL_POSITION;

        // =========================================================================
        // AUTO-FOCUS
        // =========================================================================
        bool autoFocusOnOpen = WheelerDefaults::AUTO_FOCUS_ON_OPEN;
        bool autoFocusOnOverride = WheelerDefaults::AUTO_FOCUS_ON_OVERRIDE;
        int autoFocusMinPriority = WheelerDefaults::AUTO_FOCUS_MIN_PRIORITY;

        // =========================================================================
        // POST-ACTIVATION POLICY
        // =========================================================================
        PostActivationPolicy postActivationPolicy = WheelerDefaults::POST_ACTIVATION_POLICY;

        // =========================================================================
        // SUBTEXT LABELS
        // =========================================================================
        SubtextLabelConfig subtextLabels;
    };

}  // namespace Huginn::Wheeler
