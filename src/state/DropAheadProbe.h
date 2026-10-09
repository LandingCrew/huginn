#pragma once

// =============================================================================
// DROP AHEAD PROBE (R3) -- the game half of the drop_ahead sensor
// =============================================================================
// Casts the rays core/DropAhead.h lays out: straight down from three points
// ahead of the player, through the cell's Havok world (bhkWorld::PickObject),
// and reads the water height at each point. The geometry and the drop are
// core code; this file only talks to the engine.
//
// Threading: the physics world is read under its own read lock
// (bhkWorld::worldLock, a BSReadWriteLock: readers nest, a writer waits for
// zero readers, so taking it around PickObject is safe whether or not the
// engine also takes it inside). Only on the main thread -- the thread the
// update loop runs on (UpdateHandler, an input-event sink) -- so the cell and
// its world cannot be torn down under the cast. Anything else returns "not
// measured".
// =============================================================================

#include "core/DropAhead.h"

#include <optional>

namespace Huginn::State::DropAheadProbe
{
   /// Remember the calling thread as the main thread (SKSEPlugin_Load).
   void NoteMainThread() noexcept;

   /// True on the thread NoteMainThread recorded.
   [[nodiscard]] bool OnMainThread() noexcept;

   struct Result
   {
      float drop = 0.0f;  // core::DropAhead over the probes
      std::array<Core::Needs::ProbeHit, 3> hits{};
      Core::Needs::Dir2 dir{};
      int rejectedHits = 0;  // hits on layers that are not ground (actors, clutter), cast through
   };

   /// One measurement for the player, or nullopt when it cannot be taken:
   /// not the main thread, no parent cell or physics world, or no 3D.
   /// `velocity` is the player's horizontal velocity (units/s).
   [[nodiscard]] std::optional<Result> Measure(RE::PlayerCharacter* player, Core::Needs::Vec3 velocity,
      const Core::Needs::DropProbeConfig& cfg = {});

   /// The collision layers a down ray counts as ground: terrain and statics
   /// (static, anim static, trees, props, ground, terrain, transparent). Not
   /// actors, clutter, water, triggers or invisible walls.
   [[nodiscard]] bool IsGroundLayer(RE::COL_LAYER layer) noexcept;
}
