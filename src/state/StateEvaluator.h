#pragma once

#include "GameState.h"
#include "WorldState.h"
#include "PlayerActorState.h"
#include "TargetActorState.h"

#include <mutex>

namespace Huginn::State
{
   // StateEvaluator queries the game world via SKSE and buckets the state
   class StateEvaluator
   {
   public:
      StateEvaluator() = default;
      ~StateEvaluator() = default;

      /// Hysteresis bands for the bucketed dimensions (core/BucketHysteresis.h).
      /// vitalBand: fraction of the maximum (0.03 = 3 points), applied on the
      /// way UP only -- a falling vital changes bucket at the boundary itself,
      /// so nothing urgent waits on the band. distanceFraction: fraction of the
      /// boundary distance, both ways. 0 turns either off.
      struct BucketBands
      {
         float vitalBand = 0.0f;
         float distanceFraction = 0.0f;
      };

      /// Evaluate current state from state models, with hysteresis against the
      /// buckets the pipeline last committed (EvaluateAndCommit) and the bands
      /// it used. Commits nothing, so the equip-event capture and the console
      /// see the same buckets the pipeline does without moving them.
      [[nodiscard]] GameState EvaluateCurrentState(
      const PlayerActorState& player,
      const TargetCollection& targets) const;

      /// The pipeline's evaluation (GatherState, once per run): the same, with
      /// these bands, and remembers the resulting buckets for the next call.
      /// The only caller that moves the memory.
      [[nodiscard]] GameState EvaluateAndCommit(
      const PlayerActorState& player,
      const TargetCollection& targets,
      BucketBands bands);

      /// Forget the remembered buckets (save load / reset): the next evaluation
      /// buckets the raw values.
      void ResetBucketMemory();

      // Helper: Classify actor race into target type
      [[nodiscard]] TargetType ClassifyActor(RE::Actor* actor) const;

   private:
      /// The last committed hysteretic buckets (-1 = no memory) and the bands
      /// they were computed with.
      struct BucketMemory
      {
         int health = -1;
         int magicka = -1;
         int stamina = -1;
         int distance = -1;
         BucketBands bands{};
      };

      [[nodiscard]] GameState Evaluate(
      const PlayerActorState& player,
      const TargetCollection& targets,
      const BucketMemory& memory,
      BucketMemory& next) const;

      [[nodiscard]] TargetType EvaluateTargetType(const TargetCollection& targets) const;

      // New Phase 3 methods
      [[nodiscard]] EnemyCountBucket EvaluateEnemyCount(const TargetCollection& targets) const;
      [[nodiscard]] AllyStatus EvaluateAllyStatus(const TargetCollection& targets) const;

      // Guards m_memory: committed from the update thread, read by the equip
      // capture (EquipEventBus) on whatever thread raises the equip event.
      mutable std::mutex m_memoryMutex;
      BucketMemory m_memory;
   };
}
