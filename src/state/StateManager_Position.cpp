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
#include "Globals.h"  // g_loadGeneration: the blind-spot line's per-load flag

namespace Huginn::State
{
   namespace
   {
      // The [Falling] landed read: this long after the end line (0.23.23).
      constexpr double kLandedReadDelaySec = 1.0;

      // Health as a fraction of its effective max, as PollPlayerVitals
      // computes it (current / (current - damage modifier)), so it reads the
      // same as the [Context] lines' hp=. -1 when it cannot be read.
      float HealthFraction(RE::PlayerCharacter* player)
      {
         auto* av = player->AsActorValueOwner();
         if (!av) return -1.0f;
         const float current = av->GetActorValue(RE::ActorValue::kHealth);
         const float damage = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth);
         const float max = current - damage;
         if (!std::isfinite(current) || !std::isfinite(max) || max <= 0.0f) return -1.0f;
         return std::clamp(current / max, 0.0f, 1.0f);
      }

      std::string HpText(float fraction)
      {
         return fraction < 0.0f ? std::string("?") : fmt::format("{:.1f}%", fraction * 100.0f);
      }
   }

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
      //
      // Health (0.23.23): the end line carries the health before the fall
      // (the last grounded poll's) and at the landing poll; the damage may
      // land a frame later than that poll (2026-10-10 13:51:16: the end line
      // at .662, the first hp=64.9% at .664), so a `[Falling] landed` line
      // reads it again kLandedReadDelaySec after the end (or at the next
      // fall's start, if sooner). tools/needs/fit_drop_curve.py fits to that
      // measured loss when the line is there.
      const float hpNow = HealthFraction(player);
      const double fallNowSec = NeedClock::Now();
      if (m_pendingLanding.pending &&
          (fallNowSec - m_pendingLanding.endAt >= kLandedReadDelaySec || (newIsFalling && !m_wasFalling))) {
      logger::debug("[Falling] landed — peak depth {:.0f} | hp {} -> {} ({:.1f} s after the end)"sv,
          m_pendingLanding.peakDepth, HpText(m_pendingLanding.hpBefore), HpText(hpNow),
          fallNowSec - m_pendingLanding.endAt);
      m_pendingLanding.pending = false;
      }
      m_peakFallDepth = std::max(m_peakFallDepth, newFallDepth);
      if (newIsFalling != m_wasFalling) {
      if (newIsFalling) {
        m_fallHpBefore = m_hpGrounded;
        logger::debug("[Falling] start at depth {:.0f} (gate {:.0f})"sv,
            newFallDepth, PhysicsConstants::FALL_DEPTH_MIN);
      } else {
        logger::debug("[Falling] end — peak depth {:.0f} (reason needs {:.0f}, full at {:.0f}) | hp {} -> {}"sv,
            m_peakFallDepth,
            PhysicsConstants::FALL_DEPTH_MIN +
                0.5f * (PhysicsConstants::FALL_DEPTH_HIGH - PhysicsConstants::FALL_DEPTH_MIN),
            PhysicsConstants::FALL_DEPTH_HIGH, HpText(m_fallHpBefore), HpText(hpNow));
        m_pendingLanding = { true, fallNowSec, m_fallHpBefore, m_peakFallDepth };
      }
      m_wasFalling = newIsFalling;
      }
      if (newFallDepth == 0.0f) {
      m_peakFallDepth = 0.0f;
      }
      if (!airborne) {
      m_hpGrounded = hpNow;
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

      // The false-water self-check's blind spot (0.23.24, core/WaterSelfCheck.h):
      // GetWaterHeight above falls back to the exterior cell's own plane, so
      // a dry player under a false plane of that kind reads `underwater` and
      // the drop-ahead probe's check skips it. Its symptom is visible here:
      // under, not swimming, on the ground, the water more than 150 over the
      // head, held 0.5 s on one plane. One debug line per game load; this
      // thread's own state, the load seen through the atomic generation.
      if (const auto gen = g_loadGeneration.load(std::memory_order_relaxed); gen != m_blindSpotLoadGen) {
      m_blindSpotLoadGen = gen;
      m_blindSpotLogged = false;
      m_blindSpotHold.Reset();
      }
      if (!m_blindSpotLogged) {
      Core::Needs::EngineWaterSample blind;
      blind.underwater = newIsUnderwater;
      blind.swimming = newIsSwimming;
      blind.airborne = airborne;
      blind.mounted = newIsMounted;
      blind.headZ = currentZ + PhysicsConstants::HEAD_HEIGHT;
      blind.waterZ = waterHeight;
      auto* cell = player->GetParentCell();
      const std::uint32_t cellId = cell ? cell->GetFormID() : 0;
      const double blindNow = NeedClock::Now();
      if (m_blindSpotHold.Update(blindNow, Core::Needs::UnderwaterButDry(blind), { cellId, waterHeight })) {
        m_blindSpotLogged = true;
        // Which water GetWaterHeight returned: relevantWaterHeight, or the
        // cell plane it falls back to when that is -infinity.
        const float relevant = player->loadedData ? player->loadedData->relevantWaterHeight : -RE::NI_INFINITY;
        logger::debug("[StateManager] water blind spot: underwater, not swimming, on the ground, the water {:.0f} "
                      "above the head for {:.1f} s (head z {:.0f}, water {:.0f} from {}, cell {:08X} {}): likely a "
                      "false plane the drop-ahead false-water check cannot catch, since it is the engine's own water "
                      "for the player (once per load)"sv,
            waterHeight - blind.headZ, m_blindSpotHold.HeldSec(blindNow), blind.headZ, waterHeight,
            relevant != -RE::NI_INFINITY ? fmt::format("relevantWaterHeight {:.0f}", relevant)
                                         : std::string("the cell's plane (relevantWaterHeight none)"),
            cellId, !cell ? "?" : (cell->IsInteriorCell() ? "interior" : "exterior"));
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

      // R3 need sensors: encumbrance ratio, the submerged timer, drop ahead
      // and deep water ahead.
      // Kept out of PlayerActorState (scoring input) and out of `changed`.
      PollNeedPosition(newEncumbrance, newIsUnderwater);

      return changed;
      }
   }

   void StateManager::PollNeedPosition(float encumbrance, bool underwater)
   {
      const double nowSec = NeedClock::Now();
      // drop_ahead and deep_water_ahead: the last reading DropAheadProbe took
      // on the main thread (PlayerCharacter::Update). The game does not call
      // that hook while it is paused (the console, a menu) but this loop
      // keeps polling, so the reading ages on UNPAUSED time only
      // (Core::Needs::ReadingAge): it stands through a menu, and goes stale
      // ("not measured") after kMaxAgeSec of unpaused time without a new one
      // (the hook stopped). kPreLoadGame and the main menu clear the reading
      // (DropAheadProbe).
      const auto reading = DropAheadProbe::Latest();
      auto* ui = RE::UI::GetSingleton();
      const bool paused = ui && ui->GameIsPaused();
      const double age = m_dropAge.Update(nowSec, reading.atSec, paused);
      const bool fresh = age >= 0.0 && age <= DropAheadProbe::kMaxAgeSec;
      const float drop = fresh ? reading.drop : -1.0f;
      // deep_water_ahead (0.23.19): the same probe pass, the same staleness.
      // -1 while swimming (the probe is skipped): reads 0, see Reading.
      const float waterDepth = fresh ? reading.waterDepth : -1.0f;
      Huginn_PLOT("Needs drop_ahead (units, -1 unmeasured)", drop);
      Huginn_PLOT("Needs deep_water_ahead (units, -1 unmeasured)", waterDepth);
      UpdateNeedSensors([&](NeedSensorState& n) {
        n.encumbrance = encumbrance;
        n.dropAhead = drop;
        n.waterDepthAhead = waterDepth;
        n.dropAgeSec = age;
        if (underwater != m_wasUnderwaterForTimer) {
          n.submergedAt = underwater ? nowSec : -1.0;
        }
      });
      m_wasUnderwaterForTimer = underwater;
   }

} // namespace Huginn::State
