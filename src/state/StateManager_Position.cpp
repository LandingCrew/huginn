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
#include "DropAheadProbe.h"

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

      // Underwater check (#61). GetWaterHeight() reads the engine's
      // relevantWaterHeight for THIS reference — the height of the water the
      // player is actually in — and only falls back to the cell's plane when
      // the engine has none.
      //
      // The cell plane was the whole bug. It describes one water level for the
      // cell, so it is wrong wherever the local water is a placed object at its
      // own height: verified 2026-08-08 fully submerged in the river at
      // Graywinter Watch (exterior) with no Underwater reason logged all
      // session, because the comparison ran against a sea level far below.
      // Interiors were the documented half of that; rivers and ponds were the
      // larger, undocumented half. Both are the same fix.
      //
      // Still head-relative, not feet: standing ankle-deep in a stream is not
      // being underwater, and the +120 offset is what rejects it.
      const float waterHeight = player->GetWaterHeight();
      if (waterHeight > PhysicsConstants::INVALID_WATER_HEIGHT_VALUE) {
      const float headHeight = currentZ + PhysicsConstants::HEAD_HEIGHT;
      newIsUnderwater = (headHeight < waterHeight);
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
      float newEncumbrance = 0.0f;  // R3: the ratio, not just the bool
      if (actorValueOwner) {
      float carryWeight = actorValueOwner->GetActorValue(RE::ActorValue::kCarryWeight);
      float inventoryWeight = player->GetWeightInContainer();
      newIsOverencumbered = (inventoryWeight > carryWeight);
      newEncumbrance = Core::Needs::EncumbranceRatio(inventoryWeight, carryWeight);
      }

      // Sneaking check
      newIsSneaking = player->IsSneaking();

      // Combat check, debounced: everything downstream -- context rules,
      // learner features, the pipeline hash, the combat timer -- reads this
      // published value. Target tracking reads the engine flag raw to ENTER
      // combat and this published value to leave it (PollTargets).
      {
      std::optional<BoolDebouncer::Suppressed> dropped;
      newIsInCombat = m_combatDebounce.Update(player->IsInCombat(), BoolDebouncer::Clock::now(),
          StateDebounce::COMBAT_ENTER, StateDebounce::COMBAT_EXIT, &dropped);
      if (dropped) {
        logger::debug("[Debounce] combat {} for {} ms, not published"sv,
          dropped->rawValue ? "on" : "off", dropped->lasted.count());
      }
      }

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

      // Transition line for the water state, with the numbers that decide it.
      // The underwater check had no log above trace, so a player fully
      // submerged with nothing surfacing could not be told apart from one the
      // check never saw (2026-09-27). Swimming is printed beside it: swimming
      // while not "underwater" means the head-height comparison said no.
      if (m_playerState.isUnderwater != newIsUnderwater || m_playerState.isSwimming != newIsSwimming) {
        logger::debug("[StateManager] Water: underwater={} swimming={} | head z={:.0f} water={:.0f}",
          newIsUnderwater, newIsSwimming, currentZ + PhysicsConstants::HEAD_HEIGHT, waterHeight);
      }

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
        // R3: the combat timers, on the same published transition.
        const double nowSec = NeedClock::Now();
        UpdateNeedSensors([&](NeedSensorState& n) {
          (newIsInCombat ? n.combatStartAt : n.combatEndAt) = nowSec;
        });
      }

      // R3 need sensors: encumbrance ratio, the submerged timer, drop ahead.
      // Kept out of PlayerActorState (scoring input) and out of `changed`.
      PollNeedPosition(player, newEncumbrance, newIsUnderwater, player->IsInMidair(), newIsSwimming,
          newIsMounted);

      return changed;
      }
   }

   void StateManager::PollNeedPosition(RE::PlayerCharacter* player, float encumbrance, bool underwater,
                                       bool airborne, bool swimming, bool mounted)
   {
      const double nowSec = NeedClock::Now();
      const RE::NiPoint3 pos = player->GetPosition();
      const Core::Needs::Vec3 here{ pos.x, pos.y, pos.z };
      const float dt = m_lastProbeAt < 0.0 ? 0.0f : static_cast<float>(nowSec - m_lastProbeAt);
      const auto velocity = Core::Needs::HorizontalVelocity(m_lastProbePos, here, dt);
      m_lastProbePos = here;
      m_lastProbeAt = nowSec;

      UpdateNeedSensors([&](NeedSensorState& n) {
        n.encumbrance = encumbrance;
        if (underwater != m_wasUnderwaterForTimer) {
          n.submergedAt = underwater ? nowSec : -1.0;
        }
      });
      m_wasUnderwaterForTimer = underwater;

      // Drop ahead: rays only on the ground. Airborne the feet are on nothing
      // (the fall has its own need), swimming the surface is the water, and
      // mounted the player's Z is the saddle, not the horse's feet.
      // Reason codes for the transition log: 0 measured here, 1 airborne,
      // 2 swimming, 3 mounted, 4 marshalled to the main thread, 5 a task still
      // in flight, 6 + Status for Measure's own failure.
      int reason = 0;
      if (airborne) reason = 1;
      else if (swimming) reason = 2;
      else if (mounted) reason = 3;

      if (reason != 0) {
        UpdateNeedSensors([](NeedSensorState& n) { n.dropAhead = -1.0f; });
      } else if (DropAheadProbe::OnMainThread()) {
        const auto r = DropAheadProbe::Measure(player, velocity);
        if (r.status != DropAheadProbe::Status::Measured && r.status != DropAheadProbe::Status::AllUnknown) {
          reason = 6 + static_cast<int>(r.status);
        }
        UpdateNeedSensors([&](NeedSensorState& n) { n.dropAhead = r.drop; });
      } else if (!m_probeTaskPending.exchange(true)) {
        // The update loop is not on the thread SKSEPlugin_Load ran on (it
        // is an input-event sink on a game job thread). Hand the rays to an
        // SKSE task, run at the game's own task point; the stored value is
        // the last finished one (one frame old at the 10 Hz poll). Seen in
        // game (0.23.14, verifier round 1): the tasks themselves run on six
        // job threads, the update loop's among them -- so the guarantee is
        // the task point plus the world's read lock, not a thread identity.
        reason = 4;
        if (auto* tasks = SKSE::GetTaskInterface()) {
          tasks->AddTask([this, velocity]() {
            const auto r = DropAheadProbe::MeasureInTask(RE::PlayerCharacter::GetSingleton(), velocity);
            UpdateNeedSensors([&](NeedSensorState& n) { n.dropAhead = r.drop; });
            // The task's own outcome, on a change only (one task in flight at
            // a time, so the static is not shared).
            static int s_lastStatus = -1;
            if (static_cast<int>(r.status) != s_lastStatus) {
              s_lastStatus = static_cast<int>(r.status);
              logger::info("[DropAhead] task: {} (drop {:.0f}, unknown probes {}, rejected {}; thread {:x})"sv,
                DropAheadProbe::StatusName(r.status), r.drop, r.unknownProbes, r.rejectedHits,
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
            }
            m_probeTaskPending.store(false);
          });
        } else {
          m_probeTaskPending.store(false);
        }
      } else {
        reason = 5;
      }

      // Transition only (5, a task still in flight, is the same state as 4).
      const int logged = reason == 5 ? 4 : reason;
      if (logged != m_lastProbeReason) {
        static constexpr std::string_view kNames[] = { "measured on this thread", "skipped: airborne",
          "skipped: swimming", "skipped: mounted", "marshalled to the main thread" };
        const std::string why = logged < 5 ? std::string(kNames[logged])
            : "failed: " + std::string(DropAheadProbe::StatusName(static_cast<DropAheadProbe::Status>(logged - 6)));
        logger::info("[DropAhead] {} (thread {:x}; main thread: {})"sv, why,
          std::hash<std::thread::id>{}(std::this_thread::get_id()), DropAheadProbe::OnMainThread() ? "yes" : "no");
        m_lastProbeReason = logged;
      }
   }

} // namespace Huginn::State
