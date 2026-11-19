#pragma once

// 0.24.37: which teammate the follower-heal override (OverrideManager's
// CriticalAllyHealth) is for. Pure, so the host tests run it.
//
// The game layer (StateManager_Targets) lists the teammates a heal for
// others can work on: living, tracked this poll, and not undead (vanilla
// Heal Other and Healing Hands do nothing on an undead target: a vampire
// follower or a thrall). The override then takes the most hurt of them
// within reach (fCriticalAllyHealthMaxDistance): a follower hurt in the next
// room is not one the player can heal from here (/code-review #259).

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace Huginn::Core::AllyHeal
{
    /// One teammate a heal for others can work on.
    struct Teammate
    {
        float healthMissing = 0.0f;   // share of their health missing (0-1)
        bool bleedout = false;        // down on one knee
        float distance = 0.0f;        // from the player, game units
    };

    /// How many the target poll keeps; more teammates than this is rare, and
    /// the rest are left out (the first ones the poll walks are kept).
    inline constexpr std::size_t kMaxTeammates = 4;

    /// The most hurt teammate in reach: `health` is the share left (0 when
    /// down on one knee, whatever the bar reads), 1 when none is in reach.
    struct Pick
    {
        bool found = false;
        float health = 1.0f;
        bool bleedout = false;
    };

    [[nodiscard]] constexpr Pick MostHurtInReach(std::span<const Teammate> teammates, float maxDistance) noexcept
    {
        Pick pick;
        for (const auto& t : teammates) {
            if (!(t.distance <= maxDistance)) continue;
            const float health = t.bleedout ? 0.0f : 1.0f - std::clamp(t.healthMissing, 0.0f, 1.0f);
            if (!pick.found || health < pick.health) {
                pick.found = true;
                pick.health = health;
                pick.bleedout = t.bleedout;
            }
        }
        return pick;
    }
}
