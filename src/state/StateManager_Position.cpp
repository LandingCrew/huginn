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
      float newFallDepth = DefaultState::ON_GROUND;
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
        m_playerState.fallDepth = newFallDepth;
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

      // Falling check (#60).
      //
      // IsInMidair() alone is true for any airborne moment — a jump, a step off
      // a kerb, a knockback — so taking it at face value drove slowFallWeight to
      // full on every hop, and put `Falling` (a high-priority reason) over
      // genuine ones for a tick each time.
      //
      // Instead: remember the Z the player left the ground at, and measure how
      // far BELOW it they now are. A jump rises and returns to its own take-off
      // height, so its depth stays ~0 and never reaches the gate. A real drop
      // ramps through it. This is the velocity/height threshold the old comment
      // in ContextRuleEngine claimed already existed.
      const bool inMidair = player->IsInMidair();
      const float currentZ = player->GetPosition().z;
      if (!inMidair) {
      m_groundZ = currentZ;
      newFallDepth = DefaultState::ON_GROUND;
      } else {
      // Rising (or level) keeps depth at 0 rather than going negative.
      newFallDepth = std::max(0.0f, m_groundZ - currentZ);
      }
      newIsFalling = newFallDepth >= PhysicsConstants::FALL_DEPTH_MIN;

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
      // fallDepth compared by bucket: the raw value changes every tick of a
      // fall, so an exact compare would report a change ~10×/second while
      // airborne. See PlayerActorState::FallDepthBucket.
      const int newFallBucket =
          static_cast<int>(newFallDepth / PhysicsConstants::FALL_DEPTH_BUCKET);

      bool changed = (m_playerState.isUnderwater != newIsUnderwater ||
                      m_playerState.isSwimming != newIsSwimming ||
                      m_playerState.isFalling != newIsFalling ||
                      m_playerState.FallDepthBucket() != newFallBucket ||
                      m_playerState.isOverencumbered != newIsOverencumbered ||
                      m_playerState.isSneaking != newIsSneaking ||
                      m_playerState.isInCombat != newIsInCombat ||
                      m_playerState.isMounted != newIsMounted ||
                      m_playerState.isMountedOnDragon != newIsMountedOnDragon);

      // Stored unconditionally, after `changed` has read the old bucket: the
      // curve reads the exact depth and stays smooth, while only a bucket
      // crossing marks the state dirty.
      m_playerState.fallDepth = newFallDepth;

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
