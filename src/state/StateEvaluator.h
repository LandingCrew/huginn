#pragma once

#include "GameState.h"
#include "WorldState.h"
#include "PlayerActorState.h"
#include "TargetActorState.h"

namespace Huginn::State
{
   // StateEvaluator queries the game world via SKSE and buckets the state
   class StateEvaluator
   {
   public:
      StateEvaluator() = default;
      ~StateEvaluator() = default;

      // Evaluate current state from state models
      [[nodiscard]] GameState EvaluateCurrentState(
      const WorldState& world,
      const PlayerActorState& player,
      const TargetCollection& targets) const;

      // Helper: Classify actor race into target type
      [[nodiscard]] TargetType ClassifyActor(RE::Actor* actor) const;

   private:
      // Existing methods (now accept parameters)
      [[nodiscard]] HealthBucket EvaluateHealth(const PlayerActorState& player) const;
      [[nodiscard]] MagickaBucket EvaluateMagicka(const PlayerActorState& player) const;
      [[nodiscard]] StaminaBucket EvaluateStamina(const PlayerActorState& player) const;
      [[nodiscard]] DistanceBucket EvaluateDistance(const TargetCollection& targets) const;
      [[nodiscard]] TargetType EvaluateTargetType(const TargetCollection& targets) const;

      // New Phase 3 methods
      [[nodiscard]] EnemyCountBucket EvaluateEnemyCount(const TargetCollection& targets) const;
      [[nodiscard]] AllyStatus EvaluateAllyStatus(const TargetCollection& targets) const;

      // Unchanged (can be simplified later)
      [[nodiscard]] CombatStatus EvaluateCombatStatus() const noexcept;
      [[nodiscard]] SneakStatus EvaluateSneakStatus() const noexcept;

      // Helper: Get player actor
      [[nodiscard]] RE::Actor* GetPlayer() const noexcept;
   };
}
