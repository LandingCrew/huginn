// =============================================================================
// StateManager_Resistances.cpp - Player resistances polling
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: fire, frost, shock, poison, magic resistances
// Updates: PlayerActorState.resistances
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   bool StateManager::PollPlayerResistances()
   {
      Huginn_ZONE_NAMED("PollPlayerResistances");
      ActorResistances newResistances;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
         return UpdateStateIfChanged(m_playerMutex, m_playerState.resistances, newResistances);
      }

      auto* actorValueOwner = player->AsActorValueOwner();
      if (!actorValueOwner) {
         return UpdateStateIfChanged(m_playerMutex, m_playerState.resistances, newResistances);
      }

      // Get resistance values from actor values
      newResistances.fire = actorValueOwner->GetActorValue(RE::ActorValue::kResistFire);
      newResistances.frost = actorValueOwner->GetActorValue(RE::ActorValue::kResistFrost);
      newResistances.shock = actorValueOwner->GetActorValue(RE::ActorValue::kResistShock);
      newResistances.poison = actorValueOwner->GetActorValue(RE::ActorValue::kPoisonResist);
      newResistances.magic = actorValueOwner->GetActorValue(RE::ActorValue::kResistMagic);

      // Stage 3b: Update with change detection and return result
      bool changed = UpdateStateIfChanged(m_playerMutex, m_playerState.resistances, newResistances);
#ifdef _DEBUG
      if (changed) {
      logger::trace("[StateManager] PlayerResistances changed: fire={:.0f}% frost={:.0f}% shock={:.0f}% poison={:.0f}% magic={:.0f}%",
        newResistances.fire, newResistances.frost, newResistances.shock,
        newResistances.poison, newResistances.magic);
      }
#endif
      return changed;
   }

} // namespace Huginn::State
