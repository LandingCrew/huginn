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

namespace Huginn::State
{
   // =============================================================================
   // TARGET MANAGEMENT HELPERS
   // =============================================================================

   TargetType StateManager::GetCachedActorType(RE::Actor* actor)
   {
      RE::FormID formID = actor->GetFormID();
      auto it = m_actorTypeCache.find(formID);
      if (it != m_actorTypeCache.end()) {
      return it->second;
      }

      TargetType type = StateEvaluator{}.ClassifyActor(actor);
      m_actorTypeCache[formID] = type;
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
      m_targets.targets.erase(formID);
   }

   void StateManager::PruneStaleTargets(float gameTime) noexcept
   {
      // Remove targets that haven't been seen recently
      std::vector<RE::FormID> toRemove;

      for (const auto& [formID, target] : m_targets.targets) {
      float timeSinceLastSeen = gameTime - target.lastSeenTime;

      // Remove if out of range timeout
      if (timeSinceLastSeen > TargetTracking::LAST_SEEN_TIMEOUT) {
        toRemove.push_back(formID);
      }
      // Remove if dead for too long
      else if (target.isDead && timeSinceLastSeen > TargetTracking::DEAD_ACTOR_TIMEOUT) {
        toRemove.push_back(formID);
      }
      // Remove if out of detection range - apply differentiated ranges (v0.6.12)
      // Followers/hostiles: 2048 range, Non-follower allies: 512 range
      else {
        float maxRangeSq = (target.isHostile || target.isFollower)
                               ? TargetTracking::DETECTION_RANGE_SQ
                               : TargetTracking::ALLY_DETECTION_RANGE_SQ;
        if (target.distanceToPlayerSq > maxRangeSq) {
           toRemove.push_back(formID);
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
      m_targets.primary.reset();
      m_targets.targets.clear();
      return false;
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
      {
      std::unique_lock lock(m_targetsMutex);

      bool inCombat = player->IsInCombat();

      // Clear SECONDARY ENEMY targets when leaving combat (preserve primary + allies)
      if (!inCombat && !m_targets.targets.empty()) {
        RE::FormID primaryFormID = crosshairOrStickyActor ? crosshairOrStickyActor->GetFormID() : 0;

        std::erase_if(m_targets.targets, [primaryFormID](const auto& pair) {
           if (pair.first == primaryFormID) return false;
           if (!pair.second.isHostile) return false;
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

            // Quick 2D distance rejection
            float distSq2D = dx * dx + dy * dy;
            if (distSq2D > TargetTracking::DETECTION_RANGE_SQ) [[likely]] {
              continue;
            }

            // Full 3D distance
            float dz = actorPos.z - playerPos.z;
            float distSq = distSq2D + dz * dz;

            if (distSq > TargetTracking::DETECTION_RANGE_SQ) {
              continue;
            }

            // Now do the expensive hostility check (only for actors in range)
            if (!actor->IsHostileToActor(player)) {
              continue;
            }

            RE::FormID formID = actor->GetFormID();

            // Track closest hostile for Priority 2 fallback
            if (distSq < closestHostileDistSq) {
              closestHostileDistSq = distSq;
              closestHostile = actor;
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
            auto existingIt = m_targets.targets.find(formID);
            const bool hasExisting = (existingIt != m_targets.targets.end());
            const bool withinVitalsRange = (distSq <= TargetVitalsPolling::VITALS_POLL_DISTANCE_SQ);
            const bool needsVitalsPoll = !hasExisting ||
              (withinVitalsRange && existingIt->second.NeedsVitalsPoll(gameTime, TargetVitalsPolling::SECONDARY_VITALS_INTERVAL_MS));

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
              targetState.vitals = existingIt->second.vitals;
              targetState.lastVitalsPollTime = existingIt->second.lastVitalsPollTime;
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

            targetState.level = actor->GetLevel();

            // Opt 3: Cached actor type (avoids per-poll race string matching)
            targetState.targetType = GetCachedActorType(actor);

            // Stagger status
            auto* targetActorState = actor->AsActorState();
            if (targetActorState) {
              targetState.isStaggered = targetActorState->actorState2.staggered != 0;
            }

            targetState.priority = TargetCollection::CalculatePriority(targetState);

            m_targets.targets[formID] = targetState;
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

        primaryState.level = primaryActor->GetLevel();

        // Opt 3: Cached actor type
        primaryState.targetType = GetCachedActorType(primaryActor);

        // Stagger status
        auto* actorState = primaryActor->AsActorState();
        if (actorState) {
           primaryState.isStaggered = actorState->actorState2.staggered != 0;
        }

        primaryState.priority = TargetCollection::CalculatePriority(primaryState);

        m_targets.targets[formID] = primaryState;
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
      // v0.6.12: Scan ALL process levels (high, middleHigh, middleLow)
      // Distant allies may be in lower process levels when not in combat.
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
            // Use DETECTION_RANGE_SQ as a cheap upper bound. Actors beyond max
            // possible range fail here without the expensive faction check.
            RE::NiPoint3 allyPos = ally->GetPosition();
            float dx = allyPos.x - playerPos.x;
            float dy = allyPos.y - playerPos.y;

            float distSq2D = dx * dx + dy * dy;
            if (distSq2D > TargetTracking::DETECTION_RANGE_SQ) [[likely]] {
              return false;
            }

            float dz = allyPos.z - playerPos.z;
            float distSq = distSq2D + dz * dz;

            if (distSq > TargetTracking::DETECTION_RANGE_SQ) {
              return false;
            }

            // Now do the expensive checks (only for actors within max range)
            if (ally->IsHostileToActor(player)) {
              return false;
            }

            // Apply tighter range for non-followers after hostility check
            bool isTeammate = ally->IsPlayerTeammate();
            float maxRangeSq = isTeammate ? TargetTracking::DETECTION_RANGE_SQ
                                          : TargetTracking::ALLY_DETECTION_RANGE_SQ;

            if (distSq > maxRangeSq) {
              return false;
            }

            RE::FormID allyFormID = ally->GetFormID();

            if (m_processedAllies.contains(allyFormID)) {
              return false;
            }
            m_processedAllies.insert(allyFormID);

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
            auto existingFollowerIt = m_targets.targets.find(allyFormID);
            const bool hasExistingFollower = (existingFollowerIt != m_targets.targets.end());
            const bool withinFollowerVitalsRange = (followerState.isFollower && TargetVitalsPolling::ALWAYS_POLL_FOLLOWER_VITALS) ||
              (distSq <= TargetVitalsPolling::VITALS_POLL_DISTANCE_SQ);
            const bool needsFollowerVitalsPoll = !hasExistingFollower ||
              (withinFollowerVitalsRange && existingFollowerIt->second.NeedsVitalsPoll(gameTime, TargetVitalsPolling::SECONDARY_VITALS_INTERVAL_MS));

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
              followerState.vitals = existingFollowerIt->second.vitals;
              followerState.lastVitalsPollTime = existingFollowerIt->second.lastVitalsPollTime;
            }

            followerState.level = ally->GetLevel();

            // Opt 3: Cached actor type
            followerState.targetType = GetCachedActorType(ally);

            followerState.priority = TargetCollection::CalculatePriority(followerState);

            m_targets.targets[allyFormID] = followerState;
            return true;
           };

           // v0.6.12: Scan ALL process levels for allies
           for (auto& allyHandle : processLists->highActorHandles) {
            if (auto allyPtr = allyHandle.get()) {
              processAlly(allyPtr.get());
            }
           }
           for (auto& allyHandle : processLists->middleHighActorHandles) {
            if (auto allyPtr = allyHandle.get()) {
              processAlly(allyPtr.get());
            }
           }
           for (auto& allyHandle : processLists->middleLowActorHandles) {
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
        for (auto& [formID, target] : m_targets.targets) {
           if (target.lastSeenTime == gameTime) {
            continue;
           }

           RE::Actor* actor = GetActorByFormID(formID);
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
      }

      // Targets are volatile (enemies/allies move, appear, die) - assume changed
      return true;
   }

} // namespace Huginn::State
