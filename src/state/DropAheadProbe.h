#pragma once

// =============================================================================
// DROP AHEAD PROBE (R3) -- the game half of the drop_ahead sensor
// =============================================================================
// Casts the rays core/DropAhead.h lays out, through the cell's Havok world
// (bhkWorld::PickObject): for each of three points ahead of the player, a
// horizontal pick at waist height from the previous point checks the point is
// reachable (rising ground or a wall makes it, and every point beyond it,
// unknown), then a ray straight down finds the surface; the water height at
// the point is read too. The geometry and the drop are core code; this file
// only talks to the engine.
//
// Threading: the physics world is read under its own read lock
// (bhkWorld::worldLock, a BSReadWriteLock: readers nest, a writer waits for
// zero readers, so taking it around PickObject is safe whether or not the
// engine also takes it inside), and only at a point the game sanctions:
// Measure() on the thread SKSEPlugin_Load ran on (the Debug suite, from an
// SKSE message), MeasureInTask() inside an SKSE task. The update loop runs on
// neither (it is an input-event sink on a game job thread; verifier round 1
// on #188 found every live reading was "not measured" because of it), so
// StateManager hands the rays to an SKSE task and uses the last result. In
// game the tasks ran on six different job threads, the update loop's one
// among them: thread identity is not the guarantee, the task point and the
// read lock are.
// =============================================================================

#include "core/DropAhead.h"

#include <array>
#include <string_view>

namespace Huginn::State::DropAheadProbe
{
   /// Remember the calling thread as the main thread (SKSEPlugin_Load).
   void NoteMainThread() noexcept;

   /// True on the thread NoteMainThread recorded.
   [[nodiscard]] bool OnMainThread() noexcept;

   enum class Status
   {
      Measured,       // at least one probe known (drop may still be 0)
      AllUnknown,     // every probe blocked or exhausted: drop -1
      NotMainThread,
      No3D,
      NoCell,
      NoWorld,
   };
   [[nodiscard]] std::string_view StatusName(Status s) noexcept;

   struct Result
   {
      Status status = Status::No3D;
      float drop = -1.0f;  // core::DropAhead over the known probes; -1 = not measured
      std::array<Core::Needs::ProbeHit, 3> hits{};
      Core::Needs::Dir2 dir{};
      int rejectedHits = 0;  // hits on layers that are not ground (actors, clutter), cast through
      int unknownProbes = 0;
   };

   /// One measurement for the player, on the thread NoteMainThread recorded
   /// (else NotMainThread). `velocity` is the player's horizontal velocity
   /// (units/s). The SKSE message handlers (the Debug suite) run there.
   [[nodiscard]] Result Measure(RE::PlayerCharacter* player, Core::Needs::Vec3 velocity,
      const Core::Needs::DropProbeConfig& cfg = {});

   /// The same, for a body run by SKSE's task interface (AddTask), which runs
   /// tasks at the game's own task point between frames -- SKSE's "main
   /// thread" for game access -- whatever thread id that turns out to be.
   /// The caller guarantees it is such a task.
   [[nodiscard]] Result MeasureInTask(RE::PlayerCharacter* player, Core::Needs::Vec3 velocity,
      const Core::Needs::DropProbeConfig& cfg = {});

   /// The collision layers a ray counts as ground or wall: terrain and
   /// statics (static, anim static, transparent, trees, props, terrain,
   /// ground). Not actors, clutter, water, triggers or invisible walls.
   [[nodiscard]] bool IsGroundLayer(RE::COL_LAYER layer) noexcept;
}
