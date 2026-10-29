// core/LapHoldInputs.h: the game's own rules behind the lap clock's stick
// and menu holds (0.23.55).

#include "core/LapHoldInputs.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

using namespace Huginn::Core::Needs;

TEST_CASE("stick: a deflection counts as input only beyond the deadzone, radially")
{
    constexpr float dz = 0.15f;
    CHECK_FALSE(StickIsInput(0.0f, 0.0f, dz));
    CHECK_FALSE(StickIsInput(0.05f, -0.04f, dz));    // a drifting stick at rest
    CHECK_FALSE(StickIsInput(0.15f, 0.0f, dz));      // on the edge: not beyond it
    CHECK(StickIsInput(0.16f, 0.0f, dz));
    CHECK(StickIsInput(0.0f, -1.0f, dz));            // full push
    CHECK(StickIsInput(0.11f, 0.11f, dz));           // diagonal: 0.156 radially, though each axis is under 0.15
    CHECK_FALSE(StickIsInput(0.1f, 0.1f, dz));       // 0.141
}

TEST_CASE("stick: a zero deadzone is the 0.23.51 rule (any non-zero value), and NaN is no input")
{
    CHECK(StickIsInput(0.001f, 0.0f, 0.0f));
    CHECK_FALSE(StickIsInput(0.0f, 0.0f, 0.0f));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(StickIsInput(nan, 0.0f, 0.15f));
    CHECK_FALSE(StickIsInput(0.0f, std::numeric_limits<float>::infinity(), 0.15f));
}

TEST_CASE("stick: the deadzone read from the game, or the fallback when it cannot be used")
{
    CHECK(SaneStickDeadzone(true, 0.15f) == 0.15f);
    CHECK(SaneStickDeadzone(true, 0.3f) == 0.3f);     // a drift fix raised it: followed
    CHECK(SaneStickDeadzone(true, 0.0f) == 0.0f);     // set to none: followed
    CHECK(SaneStickDeadzone(false, 0.3f) == kFallbackStickDeadzone);   // not found
    CHECK(SaneStickDeadzone(true, -0.1f) == kFallbackStickDeadzone);
    CHECK(SaneStickDeadzone(true, 1.0f) == kFallbackStickDeadzone);    // the whole range: no stick would count
    CHECK(SaneStickDeadzone(true, 15.0f) == kFallbackStickDeadzone);   // a percent, not a fraction
    CHECK(SaneStickDeadzone(true, std::numeric_limits<float>::quiet_NaN()) == kFallbackStickDeadzone);
    CHECK(kFallbackStickDeadzone == 0.15f);
}

namespace
{
    // The game's menu flags (the values the game-side caller static_asserts).
    constexpr std::uint32_t kPausesGame = 1u << 0;
    constexpr std::uint32_t kAlwaysOpen = 1u << 1;
    constexpr std::uint32_t kUsesCursor = 1u << 2;
    constexpr std::uint32_t kUsesMenuContext = 1u << 3;
    constexpr std::uint32_t kModal = 1u << 4;
    constexpr std::uint32_t kFreezeFrameBackground = 1u << 5;
    constexpr std::uint32_t kOnStack = 1u << 6;
    constexpr std::uint32_t kDisablePauseMenu = 1u << 7;
    constexpr std::uint32_t kRequiresUpdate = 1u << 8;
    constexpr std::uint32_t kTopmostRenderedMenu = 1u << 9;
    constexpr std::uint32_t kUpdateUsesCursor = 1u << 10;
    constexpr std::uint32_t kAllowSaving = 1u << 11;
    constexpr std::uint32_t kRendersOffscreenTargets = 1u << 12;
    constexpr std::uint32_t kInventoryItemMenu = 1u << 13;
    constexpr std::uint32_t kDontHideCursorWhenTopmost = 1u << 14;
    constexpr std::uint32_t kCustomRendering = 1u << 15;
    constexpr std::uint32_t kAssignCursorToRenderer = 1u << 16;
    constexpr std::uint32_t kApplicationMenu = 1u << 17;

    // The game's input contexts.
    constexpr std::uint32_t kMenuMode = 1, kConsole = 2, kItemMenu = 3, kFavorites = 6, kMap = 7, kStats = 8,
                            kBook = 10, kJournal = 12, kLockpicking = 15, kNone = 18;

    // Every menu on the stack carries kOnStack; the rule must not care.
    constexpr bool Blocks(std::uint32_t flags, std::uint32_t context)
    {
        return MenuBlocksGameplay(flags | kOnStack, context);
    }
}

TEST_CASE("menus: the mirrored flag and context values")
{
    CHECK(MenuFlag::kPausesGame == kPausesGame);
    CHECK(MenuFlag::kUsesMenuContext == kUsesMenuContext);
    CHECK(MenuFlag::kModal == kModal);
    CHECK(MenuFlag::kInventoryItemMenu == kInventoryItemMenu);
    CHECK(kContextGameplay == 0);
    CHECK(kContextNone == kNone);
}

// The flags and contexts below are CommonLibSSE-NG's notes on each vanilla
// menu's constructor (the "// flags =" and "// context =" lines in its menu
// headers). A menu's code can change them later; these are the starting set.
TEST_CASE("menus: the overlays never hold the clock")
{
    CHECK_FALSE(Blocks(kAlwaysOpen | kRequiresUpdate | kAllowSaving | kCustomRendering | kAssignCursorToRenderer, kNone));  // HUD Menu
    CHECK_FALSE(Blocks(kAllowSaving | kCustomRendering, kNone));                       // Cursor Menu
    CHECK_FALSE(Blocks(kAlwaysOpen | kAllowSaving | kCustomRendering, kNone));         // Fader Menu
    CHECK_FALSE(Blocks(kAlwaysOpen | kAllowSaving, kNone));                            // LoadWaitSpinner
    CHECK_FALSE(Blocks(kAllowSaving | kCustomRendering | kAssignCursorToRenderer, kNone));  // Kinect Menu
    CHECK_FALSE(Blocks(kDisablePauseMenu | kAllowSaving | kDontHideCursorWhenTopmost, kNone));  // Mist Menu
    CHECK_FALSE(Blocks(0, kNone));                                                     // SafeZoneMenu, TitleSequence Menu
    CHECK_FALSE(Blocks(kAlwaysOpen | kRequiresUpdate | kAllowSaving, kNone));          // Huginn's IntuitionMenu
    CHECK_FALSE(Blocks(0, kContextGameplay));                                          // a menu that names gameplay
    CHECK_FALSE(Blocks(0, 22));                                                        // WorldSpaceMenu's out-of-range id
}

TEST_CASE("menus: the non-pausing menus that take input hold it")
{
    // Crafting Menu: no kPausesGame, so the clock ticks in it.
    CHECK(Blocks(kUsesMenuContext | kDisablePauseMenu | kUpdateUsesCursor | kInventoryItemMenu | kDontHideCursorWhenTopmost, kItemMenu));
    // Dialogue Menu: no blocking flag; its menu-mode context catches it.
    CHECK(Blocks(kUpdateUsesCursor | kDontHideCursorWhenTopmost, kMenuMode));
    // A menu a mod made non-pausing keeps the rest of its flags:
    CHECK(Blocks(kDisablePauseMenu | kUpdateUsesCursor | kInventoryItemMenu | kCustomRendering, kNone));        // InventoryMenu
    CHECK(Blocks(kUsesMenuContext | kDisablePauseMenu | kUpdateUsesCursor | kInventoryItemMenu, kItemMenu));     // MagicMenu
    CHECK(Blocks(kDisablePauseMenu | kRequiresUpdate, kLockpicking));                                            // Lockpicking Menu
    CHECK(Blocks(kUsesCursor | kRendersOffscreenTargets | kCustomRendering, kMap));                             // MapMenu
    CHECK(Blocks(kUsesMenuContext | kDisablePauseMenu | kRequiresUpdate | kTopmostRenderedMenu | kRendersOffscreenTargets, kBook));  // Book Menu
    CHECK(Blocks(kUpdateUsesCursor | kInventoryItemMenu | kCustomRendering, kFavorites));                       // FavoritesMenu
    CHECK(Blocks(kUpdateUsesCursor | kAllowSaving | kDontHideCursorWhenTopmost | kCustomRendering, kMenuMode)); // TweenMenu
    CHECK(Blocks(kRequiresUpdate | kAllowSaving | kApplicationMenu, kMenuMode));                                // Sleep/Wait Menu
    CHECK(Blocks(kAlwaysOpen | kUsesCursor | kModal, kMenuMode));                                               // MessageBoxMenu
    CHECK(Blocks(kUsesMenuContext | kDisablePauseMenu | kRequiresUpdate | kCustomRendering, kStats));           // StatsMenu
    CHECK(Blocks(kUsesMenuContext | kFreezeFrameBackground | kRequiresUpdate | kTopmostRenderedMenu | kUpdateUsesCursor | kAllowSaving, kJournal));  // Journal Menu
    CHECK(Blocks(kAlwaysOpen | kUsesCursor | kAllowSaving, kConsole));                                          // Console
    CHECK(Blocks(kUsesMenuContext | kUpdateUsesCursor, kNone));                                                 // Training Menu
}

TEST_CASE("menus: each blocking flag alone, each menu context alone")
{
    CHECK(Blocks(kPausesGame, kNone));
    CHECK(Blocks(kUsesMenuContext, kNone));
    CHECK(Blocks(kModal, kNone));
    CHECK(Blocks(kInventoryItemMenu, kNone));
    for (std::uint32_t c = 1; c < kContextTotal; ++c) CHECK(Blocks(0, c));
    // The flags left out on purpose do not hold it on their own.
    CHECK_FALSE(Blocks(kDisablePauseMenu, kNone));
    CHECK_FALSE(Blocks(kUsesCursor | kUpdateUsesCursor, kNone));
    CHECK_FALSE(Blocks(kFreezeFrameBackground, kNone));
    CHECK_FALSE(Blocks(kTopmostRenderedMenu | kRequiresUpdate | kAllowSaving | kAlwaysOpen, kNone));
}
