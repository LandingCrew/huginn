#pragma once

#include "Config.h"
#include "StateFeatures.h"
#include "PipelineStateCache.h"
#include "state/GameState.h"
#include <RE/Skyrim.h>
#include <array>

namespace Huginn::Learning
{
    // =========================================================================
    // EQUIP SOURCE - Which device the player selected the item with
    // =========================================================================
    // A LABEL, never a weight (decided 2026-10-02, "one selection path"): a
    // selection teaches the same whatever device made it. The source feeds
    // the logs and the goal metrics -- goal 1 is defined by it.
    // =========================================================================
    enum class EquipSource : uint8_t
    {
        Hotkey = 0,       // Huginn keyboard shortcut (EquipManager)
        Wheeler = 1,      // Huginn's own Wheeler wheel
        External = 2,     // The player's own UI: inventory / favorites / magic menu,
                          // a vanilla hotkey, or one of their own Wheeler wheels
    };

    [[nodiscard]] inline constexpr const char* EquipSourceToString(EquipSource source) noexcept
    {
        switch (source) {
        case EquipSource::Hotkey:   return "Hotkey";
        case EquipSource::Wheeler:  return "Wheeler";
        case EquipSource::External: return "External";
        default:                    return "Unknown";
        }
    }

    // =========================================================================
    // SELECTION KIND - how a selection confirms (SelectionTracker)
    // =========================================================================
    enum class SelectionKind : uint8_t
    {
        Consumable,   // Potion, food, poison, soul gem: confirms when the count drops
        Equip,        // Weapon, spell, scroll, ammo, torch, apparel: confirms if still equipped
    };

    [[nodiscard]] inline constexpr const char* SelectionKindToString(SelectionKind kind) noexcept
    {
        return kind == SelectionKind::Consumable ? "consume" : "equip";
    }

    /// One confirmed selection, one target (BanditSubscriber, SelectionLog).
    /// The same for every kind since the choice target (0.23.0); the kind
    /// stays a parameter for the log.
    [[nodiscard]] inline constexpr float RewardFor(SelectionKind /*kind*/) noexcept
    {
        return Config::CHOICE_TARGET;
    }

    // =========================================================================
    // EQUIP EVENT - one CONFIRMED player selection, dispatched to subscribers
    // =========================================================================
    // Everything here is captured when the player CHOSE (SelectionTracker::
    // Select), not when the choice was confirmed: the state the learner
    // trains on is the state the player acted in, and the bar is what the
    // player was looking at.
    // =========================================================================
    struct EquipEvent
    {
        RE::FormID      formID = 0;
        EquipSource     source = EquipSource::Hotkey;
        SelectionKind   kind = SelectionKind::Equip;
        std::string     via;                 // How, in words: "key s3", "inventory menu", "own wheel"...
        std::string     attribution;         // External only: the A-E case label

        // Press-time state (EquipEventBus::Capture)
        StateFeatures   features{};
        State::GameState gameState{};

        // Press-time pipeline view, for the selection log
        PipelineStateCache::Snapshot shown{};
        uint32_t        loadGeneration = 0;  // g_loadGeneration at press time
        std::array<RE::FormID, 3> handsAtPress{};  // right, left, ammo when chosen (ShadowArm: skip-equipped)
        float           confirmMs = 0.0f;    // Press -> confirmation
    };

}  // namespace Huginn::Learning
