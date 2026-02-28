#pragma once

#include "StateFeatures.h"
#include "state/GameState.h"
#include <RE/Skyrim.h>

namespace Huginn::Learning
{
    // =========================================================================
    // EQUIP SOURCE - Identifies how the player equipped the item
    // =========================================================================
    enum class EquipSource : uint8_t
    {
        Hotkey = 0,       // Huginn keyboard shortcut (EquipManager)
        Wheeler = 1,      // Huginn Wheeler radial menu
        External = 2,     // Vanilla UI (inventory, favorites, console)
        Consumption = 3   // Item/scroll consumed (count delta detected)
    };

    [[nodiscard]] inline constexpr const char* EquipSourceToString(EquipSource source) noexcept
    {
        switch (source) {
        case EquipSource::Hotkey:      return "Hotkey";
        case EquipSource::Wheeler:     return "Wheeler";
        case EquipSource::External:    return "External";
        case EquipSource::Consumption: return "Consumption";
        default:                       return "Unknown";
        }
    }

    // =========================================================================
    // EQUIP EVENT - Published by equip sources, consumed by subscribers
    // =========================================================================
    // Pre-computed state avoids redundant StateManager lock acquisitions.
    // Each subscriber reads the fields it needs without re-evaluating state.
    // =========================================================================
    struct EquipEvent
    {
        RE::FormID      formID = 0;
        EquipSource     source = EquipSource::Hotkey;
        float           rewardMultiplier = 1.0f;   // Scaling factor (External uses attribution)
        bool            wasRecommended = false;     // Per-source semantics:
                                                    //   Hotkey: true if item was on the Huginn widget
                                                    //   Wheeler: always true (Huginn-managed wheel)
                                                    //   External: always false
                                                    //   Consumption: always false

        // Pre-computed state (evaluated once per event in EquipEventBus::BuildEvent)
        StateFeatures   features{};
        State::GameState gameState{};
    };

}  // namespace Huginn::Learning
