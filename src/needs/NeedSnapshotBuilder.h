#pragma once

// =============================================================================
// NEED SNAPSHOT BUILDER (R3) -- the game's state into Core::Needs::NeedSnapshot
// =============================================================================
// Copies fields out of the state snapshots the pipeline already gathered
// (PlayerActorState, TargetCollection, WorldState, the trackers) and the R3
// sensors (NeedSensorState); turns the sensors' steady-clock stamps into
// seconds-ago at `nowSec`. The two lookups it makes itself (each hand's
// spell school, the bench's craft skill) are form reads, main thread.
//
// LiveNeeds() builds one from the StateManager as it stands -- for `hg needs`
// and the Debug suite; the pipeline builds its own in GatherState from the
// tick's snapshots.
// =============================================================================

#include "core/NeedEvaluator.h"
#include "state/NeedSensorState.h"
#include "state/PlayerActorState.h"
#include "state/StateTypes.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"

#include <string>

namespace Huginn::Needs
{
   struct NeedSources
   {
      const State::PlayerActorState& player;
      const State::TargetCollection& targets;
      const State::WorldState& world;
      const State::HealthTrackingState& health;
      const State::MagickaTrackingState& magicka;
      const State::StaminaTrackingState& stamina;
      const State::NeedSensorState& sensors;
      float magickaHeld;
      float staminaHeld;
      double nowSec;
   };

   [[nodiscard]] Core::Needs::NeedSnapshot BuildNeedSnapshot(const NeedSources& src);

   struct LiveNeeds
   {
      Core::Needs::NeedSnapshot snapshot;
      Core::Needs::NeedVector vector;
   };

   /// From the StateManager's current state, with the [Needs] curves.
   [[nodiscard]] LiveNeeds ReadLiveNeeds();

   /// The `hg needs` lines: every need whose 0.05-step level is not 0, as
   /// "id value (input)", the deferred ones marked. One line per need.
   [[nodiscard]] std::vector<std::string> FormatNeeds(const Core::Needs::NeedVector& v);

   /// One line for the log: "id=value" for the non-zero levels.
   [[nodiscard]] std::string NeedsLine(const Core::Needs::NeedVector& v);
}
