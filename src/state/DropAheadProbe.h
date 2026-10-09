#pragma once

// =============================================================================
// DROP AHEAD PROBE (R3) -- the game half of the drop_ahead sensor
// =============================================================================
// Casts the rays core/DropAhead.h lays out (Core::Needs::ProbeAll: two
// horizontal reachability picks, then a ray down, for each of three points
// ahead of the player) through the cell's Havok world, bhkWorld::PickObject,
// and reads the water height at each point. The geometry, the reachability
// rules and the drop are core code; this file only talks to the engine.
//
// Where it runs (verifier rounds 1 and 2 on #188): NOT on the update loop. The
// update loop is an input-event sink on a game job thread, and SKSE's task
// queue is drained on job threads too, so neither is the main thread. The
// rays are cast from a hook on PlayerCharacter::Update (vtable index 0xAD),
// which the game calls from its main update -- the same place prior art
// casts camera rays from (SkyrimCameraDisocclusion, Hook.cpp, which also
// takes the world's read lock around PickObject). Throttled to the 100 ms
// poll cadence. Skipped, with the reason kept, while a LoadingMenu is open,
// before a game is loaded, without player 3D, a parent cell, an attached
// cell or a physics world, and while airborne, swimming or mounted. The
// physics world is held by an NiPointer and read under
// bhkWorld::worldLock (BSReadLockGuard) for the casts.
//
// The sensor (StateManager::PollNeedPosition) reads the last reading.
// =============================================================================

#include "core/DropAhead.h"

#include <array>
#include <string_view>

namespace Huginn::State::DropAheadProbe
{
   /// Install the PlayerCharacter::Update hook (SKSEPlugin_Load). Inert until
   /// SetGameLoaded(true).
   [[nodiscard]] bool InstallPlayerUpdateHook();

   /// kPostLoadGame / kNewGame: start probing (the hook is inert before).
   void SetGameLoaded(bool loaded) noexcept;

   enum class Status
   {
      Measured,       // at least one probe known (the drop may be 0)
      AllUnknown,     // every probe blocked or out of recasts: drop -1
      NotLoaded,      // no game loaded yet
      Loading,        // a LoadingMenu is open
      No3D,
      NoCell,
      CellDetached,
      NoWorld,
      Airborne,
      Swimming,
      Mounted,
   };
   [[nodiscard]] std::string_view StatusName(Status s) noexcept;

   struct Reading
   {
      Status status = Status::NotLoaded;
      float drop = -1.0f;   // -1 = not measured
      double atSec = -1.0;  // NeedClock seconds of the reading; -1 = none yet
   };

   /// The last reading (thread-safe copy).
   [[nodiscard]] Reading Latest() noexcept;

   /// Readings older than this are not used by the sensor (the hook stopped:
   /// a menu, a load).
   inline constexpr double kMaxAgeSec = 1.0;

   /// The collision layers a DOWN ray counts as ground: terrain and statics
   /// (static, anim static, transparent, trees, props, terrain, ground). The
   /// down ray casts on through anything else (actors, clutter). The
   /// horizontal reachability picks count any hit at all.
   [[nodiscard]] bool IsGroundLayer(RE::COL_LAYER layer) noexcept;
}
