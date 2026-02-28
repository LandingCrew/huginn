#pragma once

#include <SimpleIni.h>
#include <filesystem>

namespace Huginn::UI
{
    // =========================================================================
    // DISPLAY MODE
    // =========================================================================

    enum class DisplayMode : uint8_t {
        Minimal = 0,   // Name only (cleanest HUD)
        Normal  = 1,   // Name + contextual details (MP cost, damage, charge, count)
        Verbose = 2    // Name + details + score breakdown (debug)
    };

    // =========================================================================
    // REFRESH EFFECT MODE
    // =========================================================================

    enum class RefreshEffect : uint8_t {
        None  = 0,   // No collective refresh signal
        Pulse = 1,   // Alpha dip on idle slots
        Tint  = 2    // Color shift toward gray on idle slots (default)
    };

    // =========================================================================
    // SLOT EFFECT MODE
    // =========================================================================

    enum class SlotEffect : uint8_t {
        Slide   = 0,   // Old text slides up + fades, new text rises in (default)
        Fade    = 1,   // Crossfade in place (alpha only, no vertical movement)
        Instant = 2    // No animation, content swaps immediately
    };

    // =========================================================================
    // DEFAULT VALUES (compile-time constants)
    // =========================================================================

    namespace IntuitionDefaults
    {
        // Whether the Scaleform widget is enabled
        inline constexpr bool ENABLED = true;

        // Widget position as screen percentage (0 = left/top edge, 100 = right/bottom edge)
        // Converted to stage coordinates at runtime using viewport dimensions.
        inline constexpr float POSITION_X = 28.0f;
        inline constexpr float POSITION_Y = 83.0f;

        // Widget opacity (0 = invisible, 100 = fully opaque)
        inline constexpr float ALPHA = 100.0f;

        // Widget scale percentage (100 = native, 50 = half size, 200 = double)
        inline constexpr float SCALE = 70.0f;

        // Display mode: how much detail to show per slot
        inline constexpr const char* DISPLAY_MODE = "minimal";

        // Refresh effect: visual signal on idle slots when any slot's content changes
        inline constexpr const char* REFRESH_EFFECT = "tint";

        // Slot effect: animation style when a slot's content changes
        inline constexpr const char* SLOT_EFFECT = "slide";

        // Refresh effect strength: max percentage of tint blend or alpha dip (0 = invisible, 100 = full)
        inline constexpr float REFRESH_STRENGTH = 15.0f;

        // Child element opacity (0-100) for secondary elements like page labels
        inline constexpr float CHILD_ALPHA = 70.0f;

        // SWF stage dimensions (must match intuition.xml)
        inline constexpr float STAGE_WIDTH  = 1280.0f;
        inline constexpr float STAGE_HEIGHT = 720.0f;
    }

    // =========================================================================
    // INTUITION CONFIGURATION (Immutable snapshot)
    // =========================================================================
    // POD struct produced by IntuitionSettings::BuildConfig().
    // IntuitionMenu uses this for consistent hot-reload application.
    // =========================================================================

    struct IntuitionConfig
    {
        bool enabled = IntuitionDefaults::ENABLED;
        float positionX = IntuitionDefaults::POSITION_X;
        float positionY = IntuitionDefaults::POSITION_Y;
        float alpha = IntuitionDefaults::ALPHA;
        float scale = IntuitionDefaults::SCALE;
        float childAlpha = IntuitionDefaults::CHILD_ALPHA;
        DisplayMode displayMode = DisplayMode::Minimal;
        RefreshEffect refreshEffect = RefreshEffect::Tint;
        float refreshStrength = IntuitionDefaults::REFRESH_STRENGTH;
        SlotEffect slotEffect = SlotEffect::Slide;
    };

    inline constexpr IntuitionConfig DefaultIntuitionConfig{};

    // =========================================================================
    // RUNTIME SETTINGS
    // =========================================================================
    // Singleton class that holds runtime-configurable Intuition widget settings.
    // Settings are loaded from Data/SKSE/Plugins/Huginn.ini [Widget] section.
    // =========================================================================

    class IntuitionSettings
    {
    public:
        static IntuitionSettings& GetSingleton()
        {
            static IntuitionSettings instance;
            return instance;
        }

        void LoadFromFile(const std::filesystem::path& iniPath);
        void ResetToDefaults();

        /// Produce an immutable snapshot of all widget settings.
        [[nodiscard]] IntuitionConfig BuildConfig() const;

        [[nodiscard]] bool  IsEnabled() const noexcept { return enabled; }
        [[nodiscard]] float GetPositionX() const noexcept { return positionX; }
        [[nodiscard]] float GetPositionY() const noexcept { return positionY; }
        [[nodiscard]] float GetAlpha() const noexcept { return alpha; }
        [[nodiscard]] float GetScale() const noexcept { return scale; }
        [[nodiscard]] float GetChildAlpha() const noexcept { return childAlpha; }
        [[nodiscard]] DisplayMode GetDisplayMode() const noexcept { return displayMode; }
        [[nodiscard]] RefreshEffect GetRefreshEffect() const noexcept { return refreshEffect; }
        [[nodiscard]] float GetRefreshStrength() const noexcept { return refreshStrength; }
        [[nodiscard]] SlotEffect GetSlotEffect() const noexcept { return slotEffect; }

    private:
        IntuitionSettings() = default;
        ~IntuitionSettings() = default;
        IntuitionSettings(const IntuitionSettings&) = delete;
        IntuitionSettings& operator=(const IntuitionSettings&) = delete;

        bool  enabled   = IntuitionDefaults::ENABLED;
        float positionX = IntuitionDefaults::POSITION_X;
        float positionY = IntuitionDefaults::POSITION_Y;
        float alpha     = IntuitionDefaults::ALPHA;
        float scale        = IntuitionDefaults::SCALE;
        float childAlpha   = IntuitionDefaults::CHILD_ALPHA;
        DisplayMode displayMode = DisplayMode::Minimal;
        RefreshEffect refreshEffect = RefreshEffect::Tint;
        float refreshStrength = IntuitionDefaults::REFRESH_STRENGTH;
        SlotEffect slotEffect = SlotEffect::Slide;
    };

}  // namespace Huginn::UI
