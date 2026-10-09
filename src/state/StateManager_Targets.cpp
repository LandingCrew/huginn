// =============================================================================
// StateManager_Targets.cpp - Target tracking polling (optimized)
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: crosshair target, nearby enemies/allies, target vitals
// Updates: TargetCollection (primary + targets map)
// Includes: GetCachedActorType(), GetActorByFormID(), RemoveTarget(),
//           PruneStaleTargets(), CalculateTargetPriority()
//
// Optimizations (v0.12.x):
// - Merged DetectPrimaryTarget() Priority 2 loop with combat enemy scan (single pass)
// - Distance check before IsHostileToActor (cheap rejection before expensive faction check)
// - Cached ClassifyActor results (race string matching → FormID lookup)
// - Promoted processedAllies to member (avoids per-tick heap allocation)
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "StateEvaluator.h"  // v0.6.11: For ClassifyActor
#include "../Profiling.h"
#include "core/TargetFamilies.h"

namespace Huginn::State
{
   namespace
   {
      // R3 enemy_casting_<element>: the element of the spell an actor is
      // casting RIGHT NOW (its glow is on screen) -- never its spell list.
      // Bits: 1 fire, 2 frost, 4 shock, read off the casting spell's
      // harmful effects' resist actor value, the engine's own element.
      std::uint8_t CastElementBits(RE::Actor* actor)
      {
         std::uint8_t bits = 0;
         for (auto source : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand,
                              RE::MagicSystem::CastingSource::kOther }) {
            auto* caster = actor->GetMagicCaster(source);
            if (!caster || caster->state.get() == RE::MagicCaster::State::kNone) continue;
            auto* spell = caster->currentSpell;
            if (!spell) continue;
            for (const auto* effect : spell->effects) {
               const auto* mgef = effect ? effect->baseEffect : nullptr;
               if (!mgef || !mgef->IsDetrimental()) continue;
               switch (mgef->data.resistVariable) {
               case RE::ActorValue::kResistFire: bits |= 1; break;
               case RE::ActorValue::kResistFrost: bits |= 2; break;
               case RE::ActorValue::kResistShock: bits |= 4; break;
               default: break;
               }
            }
         }
         return bits;
      }

      // R3 target_archer: a bow or crossbow in either hand (equipped weapons
      // are on the perception line's allowed list).
      bool HoldsRanged(RE::Actor* actor)
      {
         for (bool left : { false, true }) {
            const auto* obj = actor->GetEquippedObject(left);
            const auto* weap = obj ? obj->As<RE::TESObjectWEAP>() : nullptr;
            if (weap && (weap->IsBow() || weap->IsCrossbow())) return true;
         }
         return false;
      }

      // R3 target_summoned: a conjured creature (summon shader, a cast before it).
      bool IsSummon(RE::Actor* actor) { return actor->IsCommandedActor() || actor->IsSummoned(); }

      // Per poll, the readings of the living combat hostiles.
      struct HostileNeeds
      {
         std::uint32_t families = 0;
         bool summoned = false;
         std::uint8_t castBits = 0;
      };
   }

   std::uint32_t StateManager::GetCachedFamilies(RE::Actor* actor)
   {
      const RE::FormID formID = actor->GetFormID();
      const auto* race = actor->GetRace();
      const RE::FormID raceID = race ? race->GetFormID() : 0;
      if (auto it = m_familyCache.find(formID); it != m_familyCache.end() && it->second.raceID == raceID) {
         return it->second.mask;
      }
      std::uint32_t mask = 0;
      if (race) {
         const auto* base = actor->GetActorBase();
         const char* edid = race->GetFormEditorID();
         const char* name = race->GetFullName();
         mask = Core::Needs::ClassifyFamilies(
            edid ? std::string_view{ edid } : std::string_view{},
            name ? std::string_view{ name } : std::string_view{},
            race->data.flags.all(RE::RACE_DATA::Flag::kFlies),
            [&](std::string_view kw) { return race->HasKeywordString(kw); },
            [&](std::string_view kw) { return base && base->HasKeywordString(kw); });
      }
      if (m_familyCache.size() >= kActorTypeCacheMax && !m_familyCache.contains(formID)) {
         m_familyCache.clear();
      }
      m_familyCache[formID] = { raceID, mask };
      return mask;
   }
   // =============================================================================
   // TARGET MANAGEMENT HELPERS
   // =============================================================================

   TargetType StateManager::GetCachedActorType(RE::Actor* actor)
   {
      const RE::FormID formID = actor->GetFormID();
      const auto* race = actor->GetRace();
      const RE::FormID raceID = race ? race->GetFormID() : 0;
      if (auto it = m_actorTypeCache.find(formID);
          it != m_actorTypeCache.end() && it->second.raceID == raceID) {
      return it->second.type;
      }

      // New actor, or its race changed since it was classified (transform).
      const TargetType type = StateEvaluator{}.ClassifyActor(actor);
      if (m_actorTypeCache.size() >= kActorTypeCacheMax &&
          !m_actorTypeCache.contains(formID)) {
      m_actorTypeCache.clear();
      }
      m_actorTypeCache[formID] = { raceID, type };
      return type;
   }

   RE::Actor* StateManager::GetActorByFormID(RE::FormID formID) noexcept
   {
      auto* form = RE::TESForm::LookupByID(formID);
      if (!form) return nullptr;
      return form->As<RE::Actor>();
   }

   void StateManager::RemoveTarget(RE::FormID formID) noexcept
   {
      m_targets.Remove(formID);
   }

   void StateManager::PruneStaleTargets(float gameTime) noexcept
   {
      // Remove targets that haven't been seen recently
      std::vector<RE::FormID> toRemove;

      for (const auto& target : m_targets.targets) {
      float timeSinceLastSeen = gameTime - target.lastSeenTime;

      // Remove if out of range timeout
      if (timeSinceLastSeen > TargetTracking::LAST_SEEN_TIMEOUT) {
        toRemove.push_back(target.actorFormID);
      }
      // Remove if dead for too long
      else if (target.isDead && timeSinceLastSeen > TargetTracking::DEAD_ACTOR_TIMEOUT) {
        toRemove.push_back(target.actorFormID);
      }
      // Remove if out of detection range - apply differentiated ranges (v0.6.12)
      // Followers/hostiles: 2048 range, Non-follower allies: 512 range
      //
      // RELEASE radius, not the acquisition radius. Pruning at the same 512 an
      // ally is acquired at is what made the boundary flap: added on one poll,
      // dropped on the next, for an NPC doing nothing but standing there. See
      // TargetTracking::RANGE_RELEASE_MARGIN.
      else {
        float releaseRangeSq = (target.isHostile || target.isFollower)
                               ? TargetTracking::DETECTION_RELEASE_RANGE_SQ
                               : TargetTracking::ALLY_RELEASE_RANGE_SQ;
        if (target.distanceToPlayerSq > releaseRangeSq) {
           toRemove.push_back(target.actorFormID);
        }
      }
      }

      for (RE::FormID formID : toRemove) {
      RemoveTarget(formID);
      }
   }

   float StateManager::CalculateTargetPriority(const TargetActorState& target) noexcept
   {
      return TargetCollection::CalculatePriority(target);
   }

   // =============================================================================
   // TARGET TRACKING POLLING
   // =============================================================================

   bool StateManager::PollTargets()
   {
      Huginn_ZONE_NAMED("PollTargets");

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      std::unique_lock lock(m_targetsMutex);
      // Losing the player IS a change if we were tracking anything: report it
      // and reset the digest so the next real poll diffs against empty.
      const bool hadTargets = m_targets.primary.has_value() || !m_targets.targets.empty();
      m_targets.Clear();
      m_prevTargetDigest = TargetDigest{};
      return hadTargets;
      }

      float gameTime = 0.0f;
      auto* calendar = RE::Calendar::GetSingleton();
      if (calendar) {
      gameTime = calendar->GetCurrentGameTime();
      }

      // Cache player position once (used throughout)
      const RE::NiPoint3 playerPos = player->GetPosition();

      // =========================================================================
      // PRIORITY 1: Crosshair / Sticky target detection (inlined from DetectPrimaryTarget)
      // =========================================================================
      // This never touches highActorHandles — just crosshair raycast + FormID lookup.

      RE::Actor* crosshairOrStickyActor = nullptr;

      // Try crosshair first
      auto* crosshairData = RE::CrosshairPickData::GetSingleton();
      if (crosshairData) {
      if (auto targetRefPtr = crosshairData->targetActor.get()) {
        crosshairOrStickyActor = targetRefPtr.get()->As<RE::Actor>();
      }
      if (!crosshairOrStickyActor) {
        if (auto targetRef = crosshairData->target.get()) {
           crosshairOrStickyActor = targetRef.get()->As<RE::Actor>();
        }
      }
      }

      const bool hasValidTime = (calendar != nullptr);

      if (crosshairOrStickyActor) {
      // Crosshair detected an actor — update sticky state
#ifdef _DEBUG
      logger::trace("[StateManager] Crosshair HIT actor (FormID: {:08X})"sv, crosshairOrStickyActor->GetFormID());
#endif
      if (hasValidTime) {
        m_stickyTargetFormID = crosshairOrStickyActor->GetFormID();
        m_stickyTargetLastSeenTime = gameTime;
      }
      } else {
      // Crosshair missed — try sticky target recovery
#ifdef _DEBUG
      logger::trace("[StateManager] Crosshair MISSED, stickyFormID={:08X}, hasValidTime={}"sv,
                    m_stickyTargetFormID, hasValidTime);
#endif

      if (hasValidTime && m_stickyTargetFormID != 0) {
        float timeSinceLastSeen = gameTime - m_stickyTargetLastSeenTime;

        if (timeSinceLastSeen < 0.0f) {
#ifdef _DEBUG
           float timeSinceLastSeenMs = timeSinceLastSeen * VitalTracking::GAME_DAYS_TO_REAL_SECONDS * 1000.0f;
           logger::trace("[StateManager] Game time went backwards ({:.1f}ms), clearing sticky target"sv, timeSinceLastSeenMs);
#endif
           m_stickyTargetFormID = 0;
        } else if (timeSinceLastSeen < CrosshairHysteresis::PERSISTENCE_TIMEOUT_DAYS) {
           RE::Actor* stickyActor = GetActorByFormID(m_stickyTargetFormID);

           if (stickyActor && !stickyActor->IsDead() &&
               !stickyActor->IsDisabled() && !stickyActor->IsDeleted()) {
            RE::NiPoint3 actorPos = stickyActor->GetPosition();
            float dx = actorPos.x - playerPos.x;
            float dy = actorPos.y - playerPos.y;
            float dz = actorPos.z - playerPos.z;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq <= CrosshairHysteresis::MAX_STICKY_RANGE_SQ) {
#ifdef _DEBUG
              float timeSinceLastSeenMs = timeSinceLastSeen * VitalTracking::GAME_DAYS_TO_REAL_SECONDS * 1000.0f;
              logger::trace("[StateManager] STICKY SUCCESS (FormID: {:08X}, age: {:.1f}ms, dist: {:.0f})"sv,
                            m_stickyTargetFormID, timeSinceLastSeenMs, std::sqrt(distSq));
#endif
              crosshairOrStickyActor = stickyActor;
            }
#ifdef _DEBUG
            else {
              logger::trace("[StateManager] STICKY FAIL: out of range (dist: {:.0f} > {:.0f})"sv,
                            std::sqrt(distSq), std::sqrt(CrosshairHysteresis::MAX_STICKY_RANGE_SQ));
            }
#endif
           }
#ifdef _DEBUG
           else {
            logger::trace("[StateManager] STICKY FAIL: actor invalid (ptr={}, dead={}, disabled={}, deleted={})"sv,
                          stickyActor != nullptr,
                          stickyActor ? stickyActor->IsDead() : false,
                          stickyActor ? stickyActor->IsDisabled() : false,
                          stickyActor ? stickyActor->IsDeleted() : false);
           }
#endif
        }
#ifdef _DEBUG
        else {
           float timeSinceLastSeenMs = timeSinceLastSeen * VitalTracking::GAME_DAYS_TO_REAL_SECONDS * 1000.0f;
           logger::trace("[StateManager] STICKY FAIL: timeout expired ({:.1f}ms > {:.1f}ms)"sv,
                         timeSinceLastSeenMs,
                         CrosshairHysteresis::PERSISTENCE_TIMEOUT_SEC * 1000.0f);
        }
#endif

        // Clear sticky if recovery failed
        if (!crosshairOrStickyActor) {
           m_stickyTargetFormID = 0;
        }
      } else if (!hasValidTime) {
        m_stickyTargetFormID = 0;
      }
      }

      // =========================================================================
      // MAIN LOCK SECTION — update targets collection
      // =========================================================================
      bool changed = false;
      HostileNeeds hostileNeeds;        // R3: this poll's living combat hostiles
      bool followerBleedout = false;    // R3: a teammate down on one knee
      bool scoringArcher = false;
      float soleTtk = -1.0f;
      const double nowSec = NeedClock::Now();
      {
      std::unique_lock lock(m_targetsMutex);

      // Raw engine flag to ENTER, so hostiles are acquired the moment combat
      // starts rather than after COMBAT_ENTER; the published (debounced) flag
      // to LEAVE. The engine drops IsInCombat for a moment mid-fight -- an
      // enemy breaking line of sight -- and reading it raw here erased every
      // hostile and skipped the closest-hostile scan on that poll:
      // Enemies One->None->One four times in 14 s with Combat: never flipping
      // (LoreRim 2026-10-01 18:17:47). The published flag holds through
      // COMBAT_EXIT, which is the window those flips fit inside. Same thread
      // as PollPlayerPosition, the debouncer's only writer.
      const bool inCombat = player->IsInCombat() || m_combatDebounce.Value();

      // Clear SECONDARY ENEMY targets when leaving combat (preserve primary + allies)
      if (!inCombat && !m_targets.targets.empty()) {
        RE::FormID primaryFormID = crosshairOrStickyActor ? crosshairOrStickyActor->GetFormID() : 0;

        std::erase_if(m_targets.targets, [primaryFormID](const auto& target) {
           if (target.actorFormID == primaryFormID) return false;
           if (!target.isHostile) return false;
           return true;
        });

#ifdef _DEBUG
        logger::trace("[StateManager] Cleared hostile secondary targets (exited combat), primary and allies preserved"sv);
#endif
      }

      // =========================================================================
      // PRIORITY 2: Merged single-pass combat loop (Opt 1 + Opt 2)
      // =========================================================================
      // In combat with no crosshair/sticky target, we need to find the closest
      // hostile as primary. Previously this was a separate loop in DetectPrimaryTarget.
      // Now we find closestHostile AND build secondary TargetActorState entries in
      // one pass, with distance checked BEFORE IsHostileToActor for early rejection.
      // =========================================================================

      RE::Actor* closestHostile = nullptr;
      float closestHostileDistSq = FLT_MAX;

      if (inCombat) {
        auto* processLists = RE::ProcessLists::GetSingleton();
        if (processLists) {
           RE::FormID crosshairFormID = crosshairOrStickyActor ? crosshairOrStickyActor->GetFormID() : 0;

           for (auto& actorHandle : processLists->highActorHandles) {
            auto actorPtr = actorHandle.get();
            if (!actorPtr) continue;

            RE::Actor* actor = actorPtr.get();

            // Validate 3D loaded
            auto* actor3D = actor->Get3D();
            if (!actor3D) continue;

            if (actor == player || actor->IsDead() ||
                actor->IsDisabled() || actor->IsDeleted()) {
              continue;
            }

            // --- Opt 2: Distance BEFORE IsHostileToActor ---
            // GetPosition is cheap arithmetic; IsHostileToActor does faction/crime checks.
            // Most actors in cities are far away and fail the cheap distance check.
            RE::NiPoint3 actorPos = actor->GetPosition();
            float dx = actorPos.x - playerPos.x;
            float dy = actorPos.y - playerPos.y;

            // Quick 2D distance rejection, at the RELEASE range so the
            // hysteresis below is reachable at all (same correction as the
            // ally loop; see RANGE_RELEASE_MARGIN).
            float distSq2D = dx * dx + dy * dy;
            if (distSq2D > TargetTracking::DETECTION_RELEASE_RANGE_SQ) [[likely]] {
              continue;
            }

            // Full 3D distance
            float dz = actorPos.z - playerPos.z;
            float distSq = distSq2D + dz * dz;

            if (distSq > TargetTracking::DETECTION_RELEASE_RANGE_SQ) {
              continue;
            }

            // Now do the expensive hostility check (only for actors in range)
            if (!actor->IsHostileToActor(player)) {
              continue;
            }

            RE::FormID formID = actor->GetFormID();

            // Acquire at 2048, hold to 2176. PruneStaleTargets releases
            // hostiles at the wider radius, so without the matching hold here
            // an enemy at 2100 would stop refreshing lastSeenTime and die to
            // LAST_SEEN_TIMEOUT 3 s later carrying a stale distance -- the
            // acquire/prune disagreement this margin exists to remove.
            if (distSq > TargetTracking::DETECTION_RANGE_SQ &&
                m_targets.Find(formID) == nullptr) {
              continue;
            }

            // Track closest hostile for Priority 2 fallback
            if (distSq < closestHostileDistSq) {
              closestHostileDistSq = distSq;
              closestHostile = actor;
            }

            // R3: the union of the living COMBAT hostiles -- each in combat
            // itself, not merely hostile and near (a sleeping draugr in the
            // next room is not in the fight) -- with no line-of-sight logic
            // (the user 2026-10-08). The crosshair one included.
            if (actor->IsInCombat()) {
              hostileNeeds.families |= GetCachedFamilies(actor);
              hostileNeeds.summoned |= IsSummon(actor);
              if (actor->IsCasting(nullptr)) {
                hostileNeeds.castBits |= CastElementBits(actor);
              }
            }

            // Skip building secondary state if this is the crosshair target
            // (it will be fully built as the primary below)
            if (formID == crosshairFormID) {
              continue;
            }

            // Build secondary target state
            TargetActorState targetState;
            targetState.actorFormID = formID;
            targetState.source = TargetSource::NearbyEnemy;
            targetState.lastSeenTime = gameTime;
            targetState.distanceToPlayerSq = distSq;
            targetState.isHostile = true;
            targetState.isDead = false;

            // Vitals polling optimization for secondary targets
            const auto* existing = m_targets.Find(formID);
            const bool withinVitalsRange = (distSq <= TargetVitalsPolling::VITALS_POLL_DISTANCE_SQ);
            const bool needsVitalsPoll = !existing ||
              (withinVitalsRange && existing->NeedsVitalsPoll(gameTime, TargetVitalsPolling::SECONDARY_VITALS_INTERVAL_MS));

            if (needsVitalsPoll) {
              auto* actorValueOwner = actor->AsActorValueOwner();
              if (actorValueOwner) {
                float currentHealth = actorValueOwner->GetActorValue(RE::ActorValue::kHealth);
                float maxHealth = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kHealth);
                if (maxHealth > 0.0f) {
                   targetState.vitals.health = std::clamp(currentHealth / maxHealth, 0.0f, 1.0f);
                   targetState.vitals.maxHealth = maxHealth;
                }

                float currentMagicka = actorValueOwner->GetActorValue(RE::ActorValue::kMagicka);
                float maxMagicka = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kMagicka);
                if (maxMagicka > 0.0f) {
                   targetState.vitals.magicka = std::clamp(currentMagicka / maxMagicka, 0.0f, 1.0f);
                   targetState.vitals.maxMagicka = maxMagicka;
                }

                float currentStamina = actorValueOwner->GetActorValue(RE::ActorValue::kStamina);
                float maxStamina = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kStamina);
                if (maxStamina > 0.0f) {
                   targetState.vitals.stamina = std::clamp(currentStamina / maxStamina, 0.0f, 1.0f);
                   targetState.vitals.maxStamina = maxStamina;
                }
              }
              targetState.lastVitalsPollTime = gameTime;
            } else {
              targetState.vitals = existing->vitals;
              targetState.lastVitalsPollTime = existing->lastVitalsPollTime;
            }

            targetState.isCasting = actor->IsCasting(nullptr);

            // Mage detection
            {
              const auto* leftHand = actor->GetEquippedObject(true);
              const auto* rightHand = actor->GetEquippedObject(false);
              bool isMage = false;
              if (leftHand && leftHand->Is(RE::FormType::Spell)) {
                isMage = true;
              }
              if (rightHand && rightHand->Is(RE::FormType::Spell)) {
                isMage = true;
              }
              targetState.isMage = isMage;
            }

            // Opt 3: Cached actor type (avoids per-poll race string matching)
            targetState.targetType = GetCachedActorType(actor);

            // Stagger status
            auto* targetActorState = actor->AsActorState();
            if (targetActorState) {
              targetState.isStaggered = targetActorState->actorState2.staggered != 0;
            }

            targetState.priority = TargetCollection::CalculatePriority(targetState);

            // Evict lowest-priority target if at capacity (skip if new target is worse)
            if (!m_targets.Contains(formID) && !m_targets.EvictIfFull(targetState.priority)) {
              continue;
            }
            m_targets.InsertOrUpdate(formID, targetState);
           }
        }
      }

      // =========================================================================
      // Resolve primary actor: crosshair/sticky takes priority, then closestHostile
      // =========================================================================
      RE::Actor* primaryActor = crosshairOrStickyActor ? crosshairOrStickyActor : closestHostile;

      // Update primary target
      if (primaryActor) {
        RE::FormID formID = primaryActor->GetFormID();

        TargetActorState primaryState;
        primaryState.actorFormID = formID;
        primaryState.source = TargetSource::Crosshair;  // Simplified
        primaryState.lastSeenTime = gameTime;

        // Get vitals (health, magicka, stamina)
        auto* actorValueOwner = primaryActor->AsActorValueOwner();
        if (actorValueOwner) {
           float currentHealth = actorValueOwner->GetActorValue(RE::ActorValue::kHealth);
           float maxHealth = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kHealth);
           if (maxHealth > 0.0f) {
            primaryState.vitals.health = std::clamp(currentHealth / maxHealth, 0.0f, 1.0f);
            primaryState.vitals.maxHealth = maxHealth;
           }

           float currentMagicka = actorValueOwner->GetActorValue(RE::ActorValue::kMagicka);
           float maxMagicka = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kMagicka);
           if (maxMagicka > 0.0f) {
            primaryState.vitals.magicka = std::clamp(currentMagicka / maxMagicka, 0.0f, 1.0f);
            primaryState.vitals.maxMagicka = maxMagicka;
           }

           float currentStamina = actorValueOwner->GetActorValue(RE::ActorValue::kStamina);
           float maxStamina = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kStamina);
           if (maxStamina > 0.0f) {
            primaryState.vitals.stamina = std::clamp(currentStamina / maxStamina, 0.0f, 1.0f);
            primaryState.vitals.maxStamina = maxStamina;
           }
        }
        primaryState.lastVitalsPollTime = gameTime;

        // Get distance
        RE::NiPoint3 targetPos = primaryActor->GetPosition();
        float dx = targetPos.x - playerPos.x;
        float dy = targetPos.y - playerPos.y;
        float dz = targetPos.z - playerPos.z;
        primaryState.distanceToPlayerSq = dx * dx + dy * dy + dz * dz;

        // Get hostility
        primaryState.isHostile = primaryActor->IsHostileToActor(player);

        // Get dead status
        primaryState.isDead = primaryActor->IsDead();

        // Get follower status
        primaryState.isFollower = primaryActor->IsPlayerTeammate();

        // Get casting status
        primaryState.isCasting = primaryActor->IsCasting(nullptr);

        // Mage detection
        {
           const auto* leftHand = primaryActor->GetEquippedObject(true);
           const auto* rightHand = primaryActor->GetEquippedObject(false);
           bool isMage = false;
           if (leftHand && leftHand->Is(RE::FormType::Spell)) {
            isMage = true;
           }
           if (rightHand && rightHand->Is(RE::FormType::Spell)) {
            isMage = true;
           }
           primaryState.isMage = isMage;
        }

        // Opt 3: Cached actor type
        primaryState.targetType = GetCachedActorType(primaryActor);

        // Stagger status
        auto* actorState = primaryActor->AsActorState();
        if (actorState) {
           primaryState.isStaggered = actorState->actorState2.staggered != 0;
        }

        primaryState.priority = TargetCollection::CalculatePriority(primaryState);

        m_targets.InsertOrUpdate(formID, primaryState);
        m_targets.primary = primaryState;
      } else {
        if (m_targets.primary.has_value()) {
           m_targets.primary.reset();
        }
      }

      // =========================================================================
      // FOLLOWER SCANNING (v0.6.10 bugfix: moved outside combat)
      // =========================================================================
      // Scan for nearby allies (player teammates) ALWAYS (not just in combat).
      // Scans highActorHandles only: the middle-high and middle-low lists
      // were dropped for cost, so a distant ally in a lower process level is
      // not found (v0.6.12 scanned all three).
      // =========================================================================
      {
        auto* processLists = RE::ProcessLists::GetSingleton();
        if (processLists) {
           RE::FormID primaryFormID = primaryActor ? primaryActor->GetFormID() : 0;

           // Bonus: Reuse member set instead of per-tick heap allocation
           m_processedAllies.clear();

           // Lambda to process a single ally actor
           // Opt 2: Distance check BEFORE IsHostileToActor in ally scan
           auto processAlly = [&](RE::Actor* ally) {
            if (!ally) {
              return false;
            }

            // Validate 3D loaded (fixes EXCEPTION_ACCESS_VIOLATION crash v0.7.9)
            auto* actor3D = ally->Get3D();
            if (!actor3D) {
#ifdef _DEBUG
              logger::trace("[StateManager] Rejected ally FormID {:08X} - 3D not loaded (likely stale handle)"sv,
                            ally->GetFormID());
#endif
              return false;
            }

            if (ally == player || ally->IsDead() ||
                ally->IsDisabled() || ally->IsDeleted()) {
              return false;
            }

            // --- Opt 2: Coarse distance gate BEFORE IsHostileToActor ---
            // Use the RELEASE range as the cheap upper bound, not the
            // acquisition range. Gating at DETECTION_RANGE_SQ made the
            // teammate hysteresis below unreachable -- nothing past 2048 ever
            // got far enough to be offered the 2176 release radius, so a
            // tracked follower drifting to 2100 stopped refreshing
            // lastSeenTime and died to LAST_SEEN_TIMEOUT three seconds later,
            // which is the exact failure the hysteresis was added to prevent.
            // Raised in review of #122.
            RE::NiPoint3 allyPos = ally->GetPosition();
            float dx = allyPos.x - playerPos.x;
            float dy = allyPos.y - playerPos.y;

            float distSq2D = dx * dx + dy * dy;
            if (distSq2D > TargetTracking::DETECTION_RELEASE_RANGE_SQ) [[likely]] {
              return false;
            }

            float dz = allyPos.z - playerPos.z;
            float distSq = distSq2D + dz * dz;

            if (distSq > TargetTracking::DETECTION_RELEASE_RANGE_SQ) {
              return false;
            }

            // Now do the expensive checks (only for actors within max range)
            if (ally->IsHostileToActor(player)) {
              return false;
            }

            // Apply tighter range for non-followers after hostility check
            bool isTeammate = ally->IsPlayerTeammate();
            RE::FormID allyFormID = ally->GetFormID();

            // Hysteresis: acquire at the detection range, hold to the release
            // range. The prune below uses the same wider radius, so the two
            // agree; relaxing only the prune would not help, because an ally
            // that stops being re-acquired stops refreshing lastSeenTime and
            // dies to LAST_SEEN_TIMEOUT three seconds later instead.
            const bool alreadyTracked = m_targets.Find(allyFormID) != nullptr;
            float maxRangeSq;
            if (isTeammate) {
              maxRangeSq = alreadyTracked ? TargetTracking::DETECTION_RELEASE_RANGE_SQ
                                          : TargetTracking::DETECTION_RANGE_SQ;
            } else {
              maxRangeSq = alreadyTracked ? TargetTracking::ALLY_RELEASE_RANGE_SQ
                                          : TargetTracking::ALLY_DETECTION_RANGE_SQ;
            }

            if (distSq > maxRangeSq) {
              return false;
            }

            if (m_processedAllies.contains(allyFormID)) {
              return false;
            }
            m_processedAllies.insert(allyFormID);

            // R3 ally_injured, the visible step: a teammate down on one knee.
            // (The continuous health reading needs a HUD mod's ally bars.)
            if (isTeammate) {
              if (const auto* allyState = ally->AsActorState(); allyState && allyState->IsBleedingOut()) {
                followerBleedout = true;
              }
            }

            if (allyFormID == primaryFormID) {
              return false;
            }

            // Build follower state
            TargetActorState followerState;
            followerState.actorFormID = allyFormID;
            followerState.source = TargetSource::NearbyAlly;
            followerState.lastSeenTime = gameTime;
            followerState.distanceToPlayerSq = distSq;
            followerState.isHostile = false;
            followerState.isDead = false;
            followerState.isFollower = isTeammate;

            // Vitals polling optimization for followers
            const auto* existingFollower = m_targets.Find(allyFormID);
            const bool withinFollowerVitalsRange = (followerState.isFollower && TargetVitalsPolling::ALWAYS_POLL_FOLLOWER_VITALS) ||
              (distSq <= TargetVitalsPolling::VITALS_POLL_DISTANCE_SQ);
            const bool needsFollowerVitalsPoll = !existingFollower ||
              (withinFollowerVitalsRange && existingFollower->NeedsVitalsPoll(gameTime, TargetVitalsPolling::SECONDARY_VITALS_INTERVAL_MS));

            if (needsFollowerVitalsPoll) {
              auto* allyValueOwner = ally->AsActorValueOwner();
              if (allyValueOwner) {
                float currentHealth = allyValueOwner->GetActorValue(RE::ActorValue::kHealth);
                float maxHealth = allyValueOwner->GetPermanentActorValue(RE::ActorValue::kHealth);
                if (maxHealth > 0.0f) {
                   followerState.vitals.health = std::clamp(currentHealth / maxHealth, 0.0f, 1.0f);
                   followerState.vitals.maxHealth = maxHealth;
                }

                float currentMagicka = allyValueOwner->GetActorValue(RE::ActorValue::kMagicka);
                float maxMagicka = allyValueOwner->GetPermanentActorValue(RE::ActorValue::kMagicka);
                if (maxMagicka > 0.0f) {
                   followerState.vitals.magicka = std::clamp(currentMagicka / maxMagicka, 0.0f, 1.0f);
                   followerState.vitals.maxMagicka = maxMagicka;
                }

                float currentStamina = allyValueOwner->GetActorValue(RE::ActorValue::kStamina);
                float maxStamina = allyValueOwner->GetPermanentActorValue(RE::ActorValue::kStamina);
                if (maxStamina > 0.0f) {
                   followerState.vitals.stamina = std::clamp(currentStamina / maxStamina, 0.0f, 1.0f);
                   followerState.vitals.maxStamina = maxStamina;
                }
              }
              followerState.lastVitalsPollTime = gameTime;
            } else {
              followerState.vitals = existingFollower->vitals;
              followerState.lastVitalsPollTime = existingFollower->lastVitalsPollTime;
            }

            // Opt 3: Cached actor type
            followerState.targetType = GetCachedActorType(ally);

            followerState.priority = TargetCollection::CalculatePriority(followerState);

            // Evict lowest-priority target if at capacity (skip if new ally is worse)
            if (!m_targets.Contains(allyFormID) && !m_targets.EvictIfFull(followerState.priority)) {
              return false;
            }
            m_targets.InsertOrUpdate(allyFormID, followerState);
            return true;
           };

           // Scan high process list only for allies.
           // Actors within DETECTION_RANGE (2048) / ALLY_DETECTION_RANGE (512) are
           // always in the high list. Middle lists add iteration cost in modded games
           // (hundreds of distant actors) with no practical benefit.
           for (auto& allyHandle : processLists->highActorHandles) {
            if (auto allyPtr = allyHandle.get()) {
              processAlly(allyPtr.get());
            }
           }
        }
      }

      // =========================================================================
      // UPDATE DISTANCES FOR STALE TARGETS (v0.6.12 fix)
      // =========================================================================
      {
        for (auto& target : m_targets.targets) {
           if (target.lastSeenTime == gameTime) {
            continue;
           }

           RE::Actor* actor = GetActorByFormID(target.actorFormID);
           if (!actor) continue;

           auto* actor3D = actor->Get3D();
           if (!actor3D) continue;

           if (!actor->IsDead() && !actor->IsDisabled() && !actor->IsDeleted()) {
            RE::NiPoint3 actorPos = actor->GetPosition();
            float dx = actorPos.x - playerPos.x;
            float dy = actorPos.y - playerPos.y;
            float dz = actorPos.z - playerPos.z;
            target.distanceToPlayerSq = dx * dx + dy * dy + dz * dz;
           }
        }
      }

      // Prune stale targets
      PruneStaleTargets(gameTime);

      // Sync primary target with targets map
      m_targets.SyncPrimaryTarget();

      // Recompute cached aggregates after all mutations
      m_targets.UpdateCachedCounts();

      // Publish enemy casting debounced, before the digest reads it, so a
      // caster pausing between spells neither re-scores nor flips the ward
      // weights (see StateDebounce::CASTING_EXIT). The hold bridges a caster's
      // PAUSES; with no hostile left there is nobody to pause, and holding
      // "casting" for 2 s would keep a ward surfaced over a dead mage
      // (/code-review on #142).
      if (m_targets.cachedEnemyCount == 0) {
        m_castingDebounce.Reset(false);
        m_targets.cachedAnyCasting = false;
      } else {
        std::optional<BoolDebouncer::Suppressed> dropped;
        m_targets.cachedAnyCasting = m_castingDebounce.Update(m_targets.cachedAnyCasting,
            BoolDebouncer::Clock::now(), StateDebounce::CASTING_ENTER, StateDebounce::CASTING_EXIT, &dropped);
        if (dropped) {
           logger::debug("[Debounce] enemy casting {} for {} ms, not published"sv,
             dropped->rawValue ? "on" : "off", dropped->lasted.count());
        }
      }

      // R3: what the needs read off the scoring target (the crosshair's
      // living hostile, else the closest one -- ScoringTargetType's pick) and
      // off the only hostile, if there is exactly one.
      {
        std::optional<TargetActorState> scoringTarget;
        if (m_targets.primary.has_value() && m_targets.primary->isHostile && !m_targets.primary->isDead) {
          scoringTarget = m_targets.primary;
        } else {
          scoringTarget = m_targets.GetClosestEnemy();
        }
        if (scoringTarget) {
          if (auto* actor = GetActorByFormID(scoringTarget->actorFormID); actor && actor->Get3D()) {
            scoringArcher = HoldsRanged(actor);
          }
        }
        RE::FormID sole = 0;
        float soleHealth = -1.0f;
        if (m_targets.cachedEnemyCount == 1) {
          for (const auto& t : m_targets.targets) {
            if (t.isHostile && !t.isDead) {
              sole = t.actorFormID;
              soleHealth = t.vitals.health;
              break;
            }
          }
        }
        soleTtk = m_soleHostileTtk.Update(sole, soleHealth, nowSec);
      }

      // Change detection: compare lightweight digest against previous
      TargetDigest digest = ComputeTargetDigest();
      changed = !(digest == m_prevTargetDigest);
      m_prevTargetDigest = digest;
      }

      // R3: publish the need readings. Families are HELD for the fight: the
      // union over every poll while the published combat flag is up, cleared
      // when it drops (a hostile dying mid-fight does not unset its family).
      // Not part of `changed`: the need sensors never open a skip gate.
      const bool fighting = m_combatDebounce.Value();
      UpdateNeedSensors([&](NeedSensorState& n) {
        n.families = fighting ? (n.families | hostileNeeds.families) : 0u;
        n.hostileSummoned = hostileNeeds.summoned;
        n.targetArcher = scoringArcher;
        n.followerBleedout = followerBleedout;
        n.soleHostileTtk = soleTtk;
        if (hostileNeeds.castBits & 1) n.castFireAt = nowSec;
        if (hostileNeeds.castBits & 2) n.castFrostAt = nowSec;
        if (hostileNeeds.castBits & 4) n.castShockAt = nowSec;
      });

      return changed;
   }

   StateManager::TargetDigest StateManager::ComputeTargetDigest() const noexcept
   {
      TargetDigest digest;

      if (m_targets.primary.has_value()) {
      const auto& p = *m_targets.primary;
      digest.primaryFormID = p.actorFormID;
      // The type scoring sees, not the raw one: a townsperson under the
      // crosshair is None to GameState, so it must be None here too.
      digest.primaryTargetType = m_targets.ScoringTargetType();
      }

      // Outside the primary block: it is the closest hostile, not the primary.
      digest.closestEnemyDistance = m_targets.ClosestEnemyDistanceBucket();

      // Scoring consumes the ANY-hostile-casting aggregate (ContextRuleEngine
      // ward weights, GameState::anyCasting) — a background caster must produce
      // a dirty signal even when the primary target is unchanged.
      digest.anyCasting = m_targets.cachedAnyCasting;

      for (const auto& target : m_targets.targets) {
      if (target.isDead) continue;
      if (target.isHostile) {
        ++digest.enemyCount;
      } else {
        ++digest.allyCount;
        if (!digest.hasInjuredAlly && target.vitals.IsHealthLow()) {
           digest.hasInjuredAlly = true;
        }
      }
      }

      return digest;
   }

} // namespace Huginn::State
