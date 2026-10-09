#pragma once

// =============================================================================
// NEED MONITOR (R3) -- the need vector on its own cadence, logged only
// =============================================================================
// Every update tick (UpdateLoop, after the sensors poll) the monitor builds the
// live need snapshot (NeedSnapshotBuilder::ReadLiveNeeds) and evaluates it.
// It is NOT part of the recommendation pipeline: it does not open either skip
// gate and nothing that scores, allocates or displays reads it, so the
// pipeline runs exactly when it did before R3. (Putting the need signature in
// the skip gate moved to R8, the cutover: verifier round 1 on #188 modelled it
// at 0.17 -> 4.6 pipeline runs/s in a melee fight and a 60% rise in wildcard
// screen time while exploring -- not "logged only".)
//
// Logging: one [Needs] debug line when the 0.05-step signature changes, at
// most one a second (a fight moves it several times a second; the line shows
// the vector as it stands when written). The Debug capture records the same
// snapshots.
// =============================================================================

#include "NeedSnapshotBuilder.h"

#include <chrono>
#include <mutex>
#include <optional>

namespace Huginn::Needs
{
   class NeedMonitor
   {
   public:
      static NeedMonitor& GetSingleton()
      {
         static NeedMonitor instance;
         return instance;
      }

      /// Update thread, every tick.
      void Tick(std::chrono::steady_clock::time_point now);

      /// The last tick's vector (a copy); empty before the first tick.
      [[nodiscard]] std::optional<LiveNeeds> Latest() const;

      static constexpr std::chrono::milliseconds kLogInterval{ 1000 };

   private:
      NeedMonitor() = default;

      mutable std::mutex m_mutex;
      std::optional<LiveNeeds> m_latest;
      std::optional<Core::Needs::NeedSignature> m_lastLogged;
      std::chrono::steady_clock::time_point m_lastLogAt{};
   };
}
