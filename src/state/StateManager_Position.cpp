// =============================================================================
// StateManager_Position.cpp - Player position/state polling
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: underwater, swimming, falling, overencumbered, sneaking, combat, mounted
// Updates: PlayerActorState position fields
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   bool StateManager::PollPlayerPosition()
   {
      Huginn_ZONE_NAMED("PollPlayerPosition");
      // Pattern from EnvironmentSensor.cpp
      bool newIsUnderwater = false;
      bool newIsSwimming = false;
      bool newIsFalling = false;
      bool newIsOverencumbered = false;
      bool newIsSneaking = false;
      bool newIsInCombat = false;
      bool newIsMounted = false;
      bool newIsMountedOnDragon = false;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      std::unique_lock lock(m_playerMutex);
      if (m_playerState.isUnderwater != newIsUnderwater ||
          m_playerState.isSwimming != newIsSwimming ||
          m_playerState.isFalling != newIsFalling ||
          m_playerState.isOverencumbered != newIsOverencumbered ||
          m_playerState.isSneaking != newIsSneaking ||
          m_playerState.isInCombat != newIsInCombat ||
          m_playerState.isMounted != newIsMounted ||
          m_playerState.isMountedOnDragon != newIsMountedOnDragon) {
        m_playerState.isUnderwater = newIsUnderwater;
        m_playerState.isSwimming = newIsSwimming;
        m_playerState.isFalling = newIsFalling;
        m_playerState.isOverencumbered = newIsOverencumbered;
        m_playerState.isSneaking = newIsSneaking;
        m_playerState.isInCombat = newIsInCombat;
        m_playerState.isMounted = newIsMounted;
        m_playerState.isMountedOnDragon = newIsMountedOnDragon;
      }
      return false;
      }

      // Underwater check
      auto* cell = player->GetParentCell();
      if (cell) {
      float waterHeight = cell->GetExteriorWaterHeight();
      if (waterHeight > PhysicsConstants::INVALID_WATER_HEIGHT_VALUE) {
        auto playerPos = player->GetPosition();
        float headHeight = playerPos.z + PhysicsConstants::HEAD_HEIGHT;
        newIsUnderwater = (headHeight < waterHeight);
      }
      }

      // Swimming check
      newIsSwimming = player->AsActorState()->IsSwimming();

      // Falling check
      newIsFalling = player->IsInMidair();

      // Overencumbered check (pattern from EnvironmentSensor.cpp)
      auto* actorValueOwner = player->AsActorValueOwner();
      if (actorValueOwner) {
      float carryWeight = actorValueOwner->GetActorValue(RE::ActorValue::kCarryWeight);
      float inventoryWeight = player->GetWeightInContainer();
      newIsOverencumbered = (inventoryWeight > carryWeight);
      }

      // Sneaking check
      newIsSneaking = player->IsSneaking();

      // Combat check
      newIsInCombat = player->IsInCombat();

      // Mounted check (pattern from EnvironmentSensor.cpp)
      newIsMounted = player->IsOnMount();
      if (newIsMounted) {
      // Check if specifically on a dragon
      RE::NiPointer<RE::Actor> mount;
      if (player->GetMount(mount) && mount) {
        // Check if mount is a dragon by checking if race can fly
        auto* race = mount->GetRace();
        if (race) {
           // Dragons have the kFlies flag in their race data
           newIsMountedOnDragon = race->data.flags.all(RE::RACE_DATA::Flag::kFlies);
        }
      }
      }

      // Stage 3b: Update position state with change detection
      {
      std::unique_lock lock(m_playerMutex);
      bool changed = (m_playerState.isUnderwater != newIsUnderwater ||
                      m_playerState.isSwimming != newIsSwimming ||
                      m_playerState.isFalling != newIsFalling ||
                      m_playerState.isOverencumbered != newIsOverencumbered ||
                      m_playerState.isSneaking != newIsSneaking ||
                      m_playerState.isInCombat != newIsInCombat ||
                      m_playerState.isMounted != newIsMounted ||
                      m_playerState.isMountedOnDragon != newIsMountedOnDragon);

      if (changed) {
        m_playerState.isUnderwater = newIsUnderwater;
        m_playerState.isSwimming = newIsSwimming;
        m_playerState.isFalling = newIsFalling;
        m_playerState.isOverencumbered = newIsOverencumbered;
        m_playerState.isSneaking = newIsSneaking;
        m_playerState.isInCombat = newIsInCombat;
        m_playerState.isMounted = newIsMounted;
        m_playerState.isMountedOnDragon = newIsMountedOnDragon;
#ifdef _DEBUG
        logger::trace("[StateManager] PlayerPosition changed"sv);
#endif
      }

      // Combat transition tracking (single-writer, no lock needed for m_wasInCombat)
      if (newIsInCombat != m_wasInCombat) {
        m_combatTransition.store(
            newIsInCombat ? CombatTransition::Entered : CombatTransition::Exited,
            std::memory_order_release);
        m_isInCombat.store(newIsInCombat, std::memory_order_release);
        m_wasInCombat = newIsInCombat;
      }

      return changed;
      }
   }

} // namespace Huginn::State
