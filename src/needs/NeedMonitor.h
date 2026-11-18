#pragma once

// =============================================================================
// NEED MONITOR (R3; R8.3: the scorer's input and a skip-gate key)
// =============================================================================
// Every update tick (UpdateLoop, after the sensors poll) the monitor builds the
// live need snapshot (NeedSnapshotBuilder::ReadLiveNeeds) and evaluates it.
// R8.3: the choice score reads the vector (PipelineCoordinator), and its
// 0.05-step signature (core/NeedEvaluator.h SignatureHash) joins both skip
// gates: a continuous need re-scores the page when it moves a step. It
// replaced the gate's ambient signature and elemental window. Modelled by
// verifier round 1 on #188 at 0.17 -> 4.6 pipeline runs/s in a melee fight,
// 0.01 -> 0.44 on a quiet walk, a wildcard on screen ~20% -> ~32% of quiet
// exploration and ~4.6 Wheeler pushes/s in fights; the [Soak] heartbeat's
// `rate` field measures all three.
//
// Logging: one [Needs] debug line when the 0.05-step signature moves -- a need
// turns on or off, or one moves kLogDeadbandLevels steps (0.25) from the line
// last written (0.23.16: torchlight flickered darkness 0.90 <-> 0.68 for half
// the session's lines) -- at most one a second (a fight moves it several
// times a second; the line shows the vector as it stands when written). The
// Debug capture records the same snapshots.
// =============================================================================

#include "NeedSnapshotBuilder.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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

      /// R8.3: the last tick's signature hash (core/NeedEvaluator.h
      /// SignatureHash); 0 before the first tick.
      [[nodiscard]] std::uint64_t LatestSignatureHash() const noexcept
      {
         return m_sigHash.load(std::memory_order_acquire);
      }

      static constexpr std::chrono::milliseconds kLogInterval{ 1000 };
      static constexpr int kLogDeadbandLevels = 5;  // 0.25: logging only

   private:
      NeedMonitor() = default;

      mutable std::mutex m_mutex;
      std::optional<LiveNeeds> m_latest;
      std::atomic<std::uint64_t> m_sigHash{ 0 };
      std::optional<Core::Needs::NeedSignature> m_lastLogged;
      std::chrono::steady_clock::time_point m_lastLogAt{};
   };
}
