#pragma once

// =============================================================================
// NEED SENSOR STATE (R3) -- the readings only the need vector uses
// =============================================================================
// Written by the StateManager polls, read (copy-out) by the need snapshot
// builder (needs/NeedSnapshotBuilder.cpp). Kept apart from PlayerActorState,
// TargetCollection and WorldState on purpose: those feed today's scoring, and
// R3 must change no score, so nothing new is added to them.
//
// Times are steady-clock seconds (NeedClock::Seconds); -1 = not set. The
// snapshot builder turns them into seconds-ago at the tick's own clock
// reading. Reset in StateManager::ResetTrackingState.
// =============================================================================

#include "core/NeedSensorMath.h"

#include <chrono>
#include <cstdint>

namespace Huginn::State
{
   namespace NeedClock
   {
      /// Steady-clock seconds since the clock's epoch (double: microsecond
      /// resolution for years).
      [[nodiscard]] inline double Seconds(std::chrono::steady_clock::time_point t) noexcept
      {
         return std::chrono::duration<double>(t.time_since_epoch()).count();
      }
      [[nodiscard]] inline double Now() noexcept { return Seconds(std::chrono::steady_clock::now()); }
   }

   struct NeedSensorState
   {
      // PollPlayerPosition
      float encumbrance = 0.0f;          // inventory weight / carry weight
      double combatStartAt = -1.0;       // published in_combat went true
      double combatEndAt = -1.0;         // published in_combat went false
      double submergedAt = -1.0;         // head went under (-1 while not under)
      float dropAhead = -1.0f;           // units; -1 = not measured (airborne, swimming, no physics world)

      // PollWorldObjects
      bool openDaylight = false;
      bool merchant = false;             // crosshair actor offers services

      // PollPlayerMagicEffects: remaining magnitude x duration of over-time restores
      float restoreHealthPending = 0.0f;
      float restoreMagickaPending = 0.0f;
      float restoreStaminaPending = 0.0f;

      // PollPlayerSurvival: the CC meters (0-1000), -1 when not read (SMI path)
      float hungerRaw = -1.0f;
      float coldRaw = -1.0f;
      float fatigueRaw = -1.0f;

      // PollHealthTracking: HP lost per element, decaying (tau 3 s)
      Core::Needs::DecayingSum dmgFire;
      Core::Needs::DecayingSum dmgFrost;
      Core::Needs::DecayingSum dmgShock;
      Core::Needs::DecayingSum dmgMagic;
      Core::Needs::DecayingSum dmgPhysical;

      // PollTargets
      double castFireAt = -1.0;          // last seen a living hostile casting the element
      double castFrostAt = -1.0;
      double castShockAt = -1.0;
      std::uint32_t families = 0;        // Core::Needs::Family bits, held for the fight
      bool hostileSummoned = false;
      bool targetArcher = false;         // the scoring target holds a bow or crossbow
      float soleHostileTtk = -1.0f;
      bool followerBleedout = false;
   };
}
