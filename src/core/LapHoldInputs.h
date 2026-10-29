#pragma once

// =============================================================================
// LAP HOLD INPUTS (0.23.55) -- the game's own rules behind two lap-clock holds
// =============================================================================
// The need episodes' lap clock (NeedEpisodes.h, "lap") holds while the player
// cannot or does not press. 0.23.51 left two gaps, both of which logged time
// in which no press was possible as the player declining a need:
//
//   stick drift   any non-zero thumbstick value counted as input, so a stick
//                 that rests a little off centre kept the idle hold from ever
//                 engaging. A stick event now counts only past the deadzone
//                 the game's own controls apply to that stick (StickIsInput).
//   menus         a menu that takes the player's input but does not pause the
//                 game (the vanilla crafting menu, by its constructor's flags)
//                 let the clock run as in play. The clock now holds while an
//                 open menu blocks gameplay input (MenuBlocksGameplay).
//
// Both rules read the game's state, not constants of ours: the deadzones are
// the engine's INI settings, read (and re-read) by the game-side caller, and
// the menu rule reads each open menu's own flags and input context. Only the
// fallback deadzone below is ours, for a setting that cannot be read.
//
// Pure: standard library only (src/core/README.md). The flag and context
// values mirror the game's menu flags and input-context ids; the game-side
// caller static_asserts that they match.
// =============================================================================

#include <cmath>
#include <cstdint>

namespace Huginn::Core::Needs
{
    // ---------------------------------------------------------------------
    // Stick deadzones
    // ---------------------------------------------------------------------

    /// The deadzone used when the game's setting cannot be read, or reads as
    /// nonsense: 0.15 of full deflection, the engine default of both
    /// fMovementAxisDeadzone:Controls and fHeadingAxisDeadzone:Controls (no
    /// LoreRim INI sets either; BethINI's dump of every SSE setting with its
    /// default lists 0.15 for both).
    inline constexpr float kFallbackStickDeadzone = 0.15f;

    /// A deadzone as read from the game, or the fallback when the read failed
    /// (`read` false) or the value is not a fraction of full deflection in
    /// [0, 1). NaN fails the range test too.
    [[nodiscard]] constexpr float SaneStickDeadzone(bool read, float value) noexcept
    {
        return (read && value >= 0.0f && value < 1.0f) ? value : kFallbackStickDeadzone;
    }

    /// Whether a thumbstick event is the player moving the stick: its
    /// deflection (x, y in [-1, 1], the game's normalized stick values) lies
    /// strictly beyond the deadzone. Radial: the magnitude, not each axis, so
    /// a diagonal push a per-axis test would miss still counts -- the error,
    /// when the game's own test is per axis, falls on the side of "input"
    /// (the clock runs), which is how every stick event counted before.
    /// A zero deadzone keeps the old rule: any non-zero value is input.
    [[nodiscard]] inline bool StickIsInput(float x, float y, float deadzone) noexcept
    {
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        return x * x + y * y > deadzone * deadzone;
    }

    // ---------------------------------------------------------------------
    // Menus that block gameplay input
    // ---------------------------------------------------------------------

    /// The game's menu flags this rule reads (the values of its menu flag
    /// enum; the caller static_asserts them).
    namespace MenuFlag
    {
        inline constexpr std::uint32_t kPausesGame = 1u << 0;
        inline constexpr std::uint32_t kUsesMenuContext = 1u << 3;
        inline constexpr std::uint32_t kModal = 1u << 4;
        inline constexpr std::uint32_t kInventoryItemMenu = 1u << 13;

        /// Any of these and the menu takes the player's input:
        ///   kPausesGame         the game stops for it (a menu that pauses
        ///                       never lets the clock tick anyway; kept so
        ///                       the rule says what it means)
        ///   kUsesMenuContext    it switches the controls to the menu's
        ///   kModal              the player must answer it before anything else
        ///   kInventoryItemMenu  a list of items to browse and pick from
        inline constexpr std::uint32_t kBlocking = kPausesGame | kUsesMenuContext | kModal | kInventoryItemMenu;
    }

    /// The game's input-context ids this rule reads: gameplay is 0, the menu
    /// contexts run up to kContextTotal (17), and kContextNone (18) is a menu
    /// that brings no controls of its own.
    inline constexpr std::uint32_t kContextGameplay = 0;
    inline constexpr std::uint32_t kContextTotal = 17;
    inline constexpr std::uint32_t kContextNone = 18;

    /// Whether an open menu blocks gameplay input, from its own flags and its
    /// own input context: one of MenuFlag::kBlocking, or a menu input context
    /// (an id below kContextTotal other than gameplay: the menu brings its
    /// controls, which replace the player's). An overlay -- the HUD, the
    /// cursor, the fader, Huginn's widget -- has none of these flags and no
    /// context of its own, so it never holds the clock. Other flags are left
    /// out on purpose: kDisablePauseMenu is also on the Mist menu (no context,
    /// no blocking flag), kUsesCursor and kUpdateUsesCursor can sit on an
    /// overlay that shows a pointer, kFreezeFrameBackground is a render flag;
    /// every vanilla menu that carries one of them and takes input is caught
    /// by the rule anyway (schema doc, Laps).
    [[nodiscard]] constexpr bool MenuBlocksGameplay(std::uint32_t flags, std::uint32_t inputContext) noexcept
    {
        if ((flags & MenuFlag::kBlocking) != 0) return true;
        return inputContext != kContextGameplay && inputContext < kContextTotal;
    }
}
