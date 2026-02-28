#pragma once

#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/GameState.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Huginn::Learning
{
   // =============================================================================
   // STATE FEATURES (Phase 3.5a)
   // =============================================================================
   // 18-float normalized feature vector for linear function approximation.
   // Replaces the 36,288-state discrete hash with continuous features so that
   // learning in one state generalizes to similar states.
   //
   // All features are in [0, 1]. Extracted from existing state models —
   // no new sensors required.
   //
   // DESIGN NOTES:
   // - distanceNorm is derived from GetClosestEnemy() (searches targets map)
   // - Target type one-hot is derived from targets.primary (crosshair/combat target)
   // - These can refer to different actors — intentional. Distance encodes
   //   tactical proximity, while target type encodes what the player is focused on.
   // - inCombat and isSneaking can both be 1.0 simultaneously (sneak-combat)
   // =============================================================================

   struct StateFeatures
   {
      // [0-2] Vitals — continuous 0-1 (already normalized in ActorVitals)
      float healthPct   = 1.0f;
      float magickaPct  = 1.0f;
      float staminaPct  = 1.0f;

      // [3-5] Player state — binary + normalized distance
      float inCombat    = 0.0f;
      float isSneaking  = 0.0f;
      float distanceNorm = 1.0f;  // Closest enemy distance / MAX_DISTANCE, clamped [0,1]

      // [6-12] Target type — one-hot encoding (exactly one is 1.0)
      float targetNone      = 1.0f;
      float targetHumanoid  = 0.0f;
      float targetUndead    = 0.0f;
      float targetBeast     = 0.0f;
      float targetConstruct = 0.0f;
      float targetDragon    = 0.0f;
      float targetDaedra    = 0.0f;

      // [13-16] Equipment — binary
      float hasMeleeEquipped  = 0.0f;
      float hasBowEquipped    = 0.0f;
      float hasSpellEquipped  = 0.0f;
      float hasShieldEquipped = 0.0f;

      // [17] Bias — always 1.0 (intercept term for linear model)
      float bias = 1.0f;

      static constexpr size_t NUM_FEATURES = 18;

      // Max distance for normalization: 4096 game units ≈ 56m
      // Covers archery + destruction spell effective range
      static constexpr float MAX_DISTANCE = 4096.0f;

      // Factory: extract from existing state models
      // GetClosestEnemy() already filters out NO_TARGET sentinel entries (distanceToPlayerSq <= 0)
      [[nodiscard]] static StateFeatures FromState(
         const State::PlayerActorState& player,
         const State::TargetCollection& targets)
      {
         StateFeatures f;

         // Vitals (already 0-1 in ActorVitals, clamp guards against SKSE edge cases)
         f.healthPct  = std::clamp(player.vitals.health, 0.0f, 1.0f);
         f.magickaPct = std::clamp(player.vitals.magicka, 0.0f, 1.0f);
         f.staminaPct = std::clamp(player.vitals.stamina, 0.0f, 1.0f);

         // Binary state flags
         f.inCombat   = player.isInCombat ? 1.0f : 0.0f;
         f.isSneaking = player.isSneaking ? 1.0f : 0.0f;

         // Distance to closest enemy (sentinel filtering handled by GetClosestEnemy)
         auto closestEnemy = targets.GetClosestEnemy();
         if (closestEnemy.has_value()) {
            float dist = std::sqrt(closestEnemy->distanceToPlayerSq);
            f.distanceNorm = std::clamp(dist / MAX_DISTANCE, 0.0f, 1.0f);
         } else {
            f.distanceNorm = 1.0f;  // No enemy → max range
         }

         // Target type one-hot encoding (from primary target — crosshair/combat focus)
         State::TargetType type = State::TargetType::None;
         if (targets.primary.has_value()) {
            type = targets.primary->targetType;
         }

         // Reset all target fields (fresh struct has targetNone=1.0, must clear before switch)
         f.targetNone      = 0.0f;
         f.targetHumanoid  = 0.0f;
         f.targetUndead    = 0.0f;
         f.targetBeast     = 0.0f;
         f.targetConstruct = 0.0f;
         f.targetDragon    = 0.0f;
         f.targetDaedra    = 0.0f;

         switch (type) {
         case State::TargetType::Humanoid:  f.targetHumanoid  = 1.0f; break;
         case State::TargetType::Undead:    f.targetUndead    = 1.0f; break;
         case State::TargetType::Beast:     f.targetBeast     = 1.0f; break;
         case State::TargetType::Construct: f.targetConstruct = 1.0f; break;
         case State::TargetType::Dragon:    f.targetDragon    = 1.0f; break;
         case State::TargetType::Daedra:    f.targetDaedra    = 1.0f; break;
         default:                           f.targetNone      = 1.0f; break;
         }

         // Equipment booleans
         f.hasMeleeEquipped  = player.hasMeleeEquipped  ? 1.0f : 0.0f;
         f.hasBowEquipped    = player.hasBowEquipped    ? 1.0f : 0.0f;
         f.hasSpellEquipped  = player.hasSpellEquipped  ? 1.0f : 0.0f;
         f.hasShieldEquipped = player.hasShieldEquipped ? 1.0f : 0.0f;

         // Bias is always 1.0 (set by default initializer)

         return f;
      }

      // Convert to array for dot product with weight vectors
      [[nodiscard]] std::array<float, NUM_FEATURES> ToArray() const
      {
         static_assert(NUM_FEATURES == 18, "Update ToArray() when adding features");
         return {{
            healthPct, magickaPct, staminaPct,
            inCombat, isSneaking, distanceNorm,
            targetNone, targetHumanoid, targetUndead,
            targetBeast, targetConstruct, targetDragon, targetDaedra,
            hasMeleeEquipped, hasBowEquipped, hasSpellEquipped, hasShieldEquipped,
            bias
         }};
      }
   };
}
