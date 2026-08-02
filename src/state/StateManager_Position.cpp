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
      float newFallDepth = 0.0f;
      bool newIsOverencumbered = false;
      bool newIsSneaking = false;
      bool newIsInCombat = false;
      bool newIsMounted = false;
      bool newIsMountedOnDragon = false;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      // No player means no trustworthy Z next poll either — drop the anchor so
      // the tracker re-anchors rather than measuring against a stale take-off.
      m_fallTracker.Reset();
      std::unique_lock lock(m_playerMutex);
      if (m_playerState.isUnderwater != newIsUnderwater ||
          m_playerState.isSwimming != newIsSwimming ||
          m_playerState.isFalling != newIsFalling ||
          m_playerState.FallDepthBucket() != PlayerActorState::FallDepthBucketOf(newFallDepth) ||
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

      const float currentZ = player->GetPosition().z;

      // Underwater check
      auto* cell = player->GetParentCell();
      if (cell) {
      float waterHeight = cell->GetExteriorWaterHeight();
      if (waterHeight > PhysicsConstants::INVALID_WATER_HEIGHT_VALUE) {
        float headHeight = currentZ + PhysicsConstants::HEAD_HEIGHT;
        newIsUnderwater = (headHeight < waterHeight);
      }
      }

      // Swimming check
      newIsSwimming = player->AsActorState()->IsSwimming();

      // Falling check (#60). IsInMidair() alone is true for any airborne moment
      // — a jump, a kerb, a knockback — so taking it at face value drove
      // slowFallWeight to full on every hop. FallTracker measures the descent
      // below the point the player left the ground instead; see its header.
      //
      // Swimming is excluded before the tracker sees it: diving is airborne to
      // nobody, but a 400-unit descent underwater would otherwise read as a
      // fall if the engine ever reports midair while submerged.
      const bool airborne = player->IsInMidair() && !newIsSwimming;
      const float maxPlausibleDelta =
          PhysicsConstants::MAX_FALL_SPEED * (m_playerPositionPollInterval / 1000.0f);
      newFallDepth = m_fallTracker.Update(currentZ, airborne, maxPlausibleDelta);
      newIsFalling = newFallDepth >= PhysicsConstants::FALL_DEPTH_MIN;

      // Transition only — the gate is otherwise invisible in a log, which makes
      // "correctly quiet" and "never fires at all" look identical.
      //
      // PEAK depth is the number that matters, not the depth at the crossing:
      // the crossing sample is just the first one past the gate, and by the end
      // transition the tracker has already re-anchored to 0. Without the peak
      // there is no way to tell a fall that never reached the ramp's upper half
      // from one that did and failed to report — see the 0.18.17 session, where
      // a fall deep enough to deal damage produced no Falling reason at all.
      m_peakFallDepth = std::max(m_peakFallDepth, newFallDepth);
      if (newIsFalling != m_wasFalling) {
      if (newIsFalling) {
        logger::debug("[Falling] start at depth {:.0f} (gate {:.0f})"sv,
            newFallDepth, PhysicsConstants::FALL_DEPTH_MIN);
      } else {
        logger::debug("[Falling] end — peak depth {:.0f} (reason needs {:.0f}, full at {:.0f})"sv,
            m_peakFallDepth,
            PhysicsConstants::FALL_DEPTH_MIN +
                0.5f * (PhysicsConstants::FALL_DEPTH_HIGH - PhysicsConstants::FALL_DEPTH_MIN),
            PhysicsConstants::FALL_DEPTH_HIGH);
      }
      m_wasFalling = newIsFalling;
      }
      if (newFallDepth == 0.0f) {
      m_peakFallDepth = 0.0f;
      }

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
      const int newFallBucket = PlayerActorState::FallDepthBucketOf(newFallDepth);

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
