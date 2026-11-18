#pragma once

// =============================================================================
// COMBAT ONSET (R8.1, 0.23.55) -- one combat clock
// =============================================================================
// Map Phase 6 item 2. The old potion multiplier kept its own combat timer
// (start / end / update hooks, fed by a destructive combat-transition
// latch on StateManager); the need vector reads the combat_onset sensor
// (NeedSensorState::combatStartAt, written on the same published transition,
// StateManager_Position.cpp). From 0.23.55 both read this: the timer is gone,
// and the discriminator's start window, sustained-combat and lingering-poison
// tests read the sensor, the same clock as combat_onset (NeedEvaluator.cpp).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/NeedSensorMath.h"
#include "core/NeedSnapshot.h"

namespace Huginn::Core::Needs
{
    /// combat_onset's input: seconds since the current fight began; kNever out
    /// of combat (and in combat with no start recorded).
    [[nodiscard]] constexpr float CombatOnsetAgo(bool inCombat, float startAgo) noexcept
    {
        return inCombat ? startAgo : kNever;
    }

    /// The same from the sensor's clock: `startAt` the steady-clock second
    /// in_combat last went true (-1: never), `nowSec` the reading now.
    [[nodiscard]] inline float CombatOnsetAgo(bool inCombat, double startAt, double nowSec) noexcept
    {
        return CombatOnsetAgo(inCombat, SecondsSince(startAt, nowSec));
    }

    /// In the first `windowSec` of the current fight ([Scoring] fCombatStartWindow).
    [[nodiscard]] constexpr bool InCombatStartWindow(float onsetAgo, float windowSec) noexcept
    {
        return onsetAgo < kNever && onsetAgo < windowSec;
    }

    /// Seconds into the current fight; 0 out of combat (what the old timer read).
    [[nodiscard]] constexpr float CombatSeconds(float onsetAgo) noexcept
    {
        return onsetAgo < kNever ? onsetAgo : 0.0f;
    }
}
