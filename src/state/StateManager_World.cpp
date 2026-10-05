// =============================================================================
// StateManager_World.cpp - World object polling
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: locks, ore veins, workstations, time of day, light level, interior status
// Updates: WorldState
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   // Outdoors, under a full sky, between the end of the climate's sunrise and
   // the start of its sunset. A full sky and not just "exterior": Blackreach
   // and its like are exterior worldspaces that are dark at noon. Climate
   // timing is in 10-minute units; without a climate, 07:00-18:00.
   static bool InOpenDaylight(float hour, bool interior)
   {
      if (interior) return false;
      auto* sky = RE::Sky::GetSingleton();
      if (!sky || sky->mode.get() != RE::Sky::Mode::kFull) return false;
      float dayStart = 7.0f;
      float dayEnd = 18.0f;
      if (const auto* climate = sky->currentClimate) {
         dayStart = climate->timing.sunrise.end / 6.0f;
         dayEnd = climate->timing.sunset.begin / 6.0f;
      }
      return dayStart < dayEnd && hour >= dayStart && hour < dayEnd;
   }

   // =============================================================================
   // WORLD OBJECT DETECTION HELPERS
   // =============================================================================

   RE::TESObjectREFR* StateManager::GetCrosshairReference() noexcept
   {
      auto* crosshairData = RE::CrosshairPickData::GetSingleton();
      if (!crosshairData) return nullptr;

      auto targetRefHandle = crosshairData->target;
      if (!targetRefHandle) return nullptr;

      auto targetRefPtr = targetRefHandle.get();
      if (!targetRefPtr) return nullptr;

      return targetRefPtr.get();
   }

   void StateManager::DetectLockTarget(RE::TESObjectREFR* crosshairRef, WorldState& state) noexcept
   {
      if (!crosshairRef) return;

      // Check if actually locked (not just "has a lock object")
      if (!crosshairRef->IsLocked()) return;

      auto* refLock = crosshairRef->GetLock();
      if (!refLock) return;

      state.isLookingAtLock = true;
      auto lockLevel = refLock->GetLockLevel(crosshairRef);

      // Map LOCK_LEVEL enum to integer
      switch (lockLevel) {
      case RE::LOCK_LEVEL::kVeryEasy: state.lockLevel = LockLevel::NOVICE; break;
      case RE::LOCK_LEVEL::kEasy: state.lockLevel = LockLevel::APPRENTICE; break;
      case RE::LOCK_LEVEL::kAverage: state.lockLevel = LockLevel::ADEPT; break;
      case RE::LOCK_LEVEL::kHard: state.lockLevel = LockLevel::EXPERT; break;
      case RE::LOCK_LEVEL::kVeryHard: state.lockLevel = LockLevel::MASTER; break;
      case RE::LOCK_LEVEL::kRequiresKey: state.lockLevel = LockLevel::REQUIRES_KEY; break;
      default: state.lockLevel = 0; break;
      }
   }

   void StateManager::DetectOreVeinTarget(RE::TESObjectREFR* crosshairRef, WorldState& state) noexcept
   {
      if (!crosshairRef) return;

      RE::FormID formID = crosshairRef->GetFormID();

      // Check positive cache first
      if (m_oreVeinCache.contains(formID)) {
      state.isLookingAtOreVein = true;
      return;
      }

      // Check negative cache
      if (m_notOreVeinCache.contains(formID)) {
      return;  // Known not to be an ore vein
      }

      // Not in either cache - check if it's an ore vein
      auto* baseObject = crosshairRef->GetBaseObject();
      if (!baseObject) return;

      auto* keywordForm = baseObject->As<RE::BGSKeywordForm>();
      if (!keywordForm) return;

      bool isOreVein = false;
      for (uint32_t i = 0; i < keywordForm->numKeywords; ++i) {
      auto* keyword = keywordForm->keywords[i];
      if (keyword) {
        const char* editorID = keyword->GetFormEditorID();
        if (editorID && (std::strstr(editorID, "Ore") || std::strstr(editorID, "Vein"))) {
           isOreVein = true;
           break;
        }
      }
      }

      if (isOreVein) {
      m_oreVeinCache.insert(formID);
      state.isLookingAtOreVein = true;
      } else {
      m_notOreVeinCache.insert(formID);
      }
   }

   void StateManager::DetectWorkstationTarget(RE::TESObjectREFR* crosshairRef, WorldState& state) noexcept
   {
      if (!crosshairRef) return;

      auto* baseObj = crosshairRef->GetBaseObject();
      if (!baseObj) return;

      auto formType = baseObj->GetFormType();

      // Furniture includes workstations (pattern from CrosshairSensor)
      if (formType == RE::FormType::Furniture) {
      auto* furniture = baseObj->As<RE::TESFurniture>();
      if (furniture) {
        auto benchType = furniture->workBenchData.benchType.get();
        if (benchType != RE::TESFurniture::WorkBenchData::BenchType::kNone) {
           state.isLookingAtWorkstation = true;
           state.workstationType = static_cast<uint8_t>(benchType);
        }
      }
      }
   }

   // =============================================================================
   // WORLD OBJECTS POLLING
   // =============================================================================

   bool StateManager::PollWorldObjects()
   {
      Huginn_ZONE_NAMED("PollWorldObjects");
      // Unconditional first-call logging to diagnose poll issues
      static int pollCount = 0;
      pollCount++;
      if (pollCount <= 3) {
      logger::info("[StateManager] PollWorldObjects() called (count={})"sv, pollCount);
      }

      WorldState newState;

      // Get player for crosshair detection
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return UpdateStateIfChanged(m_worldMutex, m_worldState, newState);
      }

      // Time of day
      auto* calendar = RE::Calendar::GetSingleton();
      if (calendar) {
      newState.timeOfDay = calendar->GetHour();
      }

      // Interior flag
      auto* cell = player->GetParentCell();
      if (cell) {
      newState.isInterior = cell->IsInteriorCell();
      }

      // Light level. The game's own value comes first: it is the light on the
      // player, so a dark cave reads dark and a torch or Candlelight reads lit.
      // The clock estimate below it only ever knew the time of day, and called
      // every interior 0.5 -- a cave was never dark.
      // Quantized to 10% increments to reduce jitter
      float rawLight = -1.0f;
      if (auto* process = player->GetActorRuntimeData().currentProcess;
          process && process->high) {
      rawLight = process->high->lightLevel;
      }
      if (rawLight >= 0.0f) {
      newState.lightLevel = std::clamp(rawLight / LightLevel::GAME_LIGHT_SCALE, 0.0f, 1.0f);
      } else if (newState.isInterior) {
      newState.lightLevel = LightLevel::INTERIOR_DEFAULT;
      } else {
      float hoursSinceNoon = std::abs(newState.timeOfDay - LightLevel::NOON);
      float daylight = 1.0f - (hoursSinceNoon / 6.0f);  // Peak at noon
      newState.lightLevel = std::max(LightLevel::NIGHTTIME_BASE, daylight);
      }
      // Quantize to 10% increments
      newState.lightLevel = std::round(newState.lightLevel * LightLevel::QUANTIZATION_MULTIPLIER) / LightLevel::QUANTIZATION_MULTIPLIER;

      // Dark with a hysteresis band and a dwell both ways; a new place (an
      // interior cell, or an exterior worldspace -- not the exterior grid
      // cell) is taken at once (DarknessGate).
      {
      RE::FormID placeID = 0;
      if (cell && cell->IsInteriorCell()) {
        placeID = cell->GetFormID();
      } else if (auto* worldspace = player->GetWorldspace()) {
        placeID = worldspace->GetFormID();
      }
      std::optional<BoolDebouncer::Suppressed> dropped;
      // Outdoors under an open sky, in the climate's daytime, it is not dark
      // however the shadow falls: the light on the player read 220.9 and 27.1
      // by turns in snow at 11:30, tree shadow most likely, and Magelight took
      // key 1 (2026-10-03 19:24-19:25). The gate is fed "lit" then, so its
      // hysteresis still governs the edges; the published light level stays
      // the measured one.
      const bool openDaylight = InOpenDaylight(newState.timeOfDay, newState.isInterior);
      if (openDaylight != m_lastOpenDaylight) {
        logger::debug("[World] open daylight {} at {:.2f}h ({})"sv, openDaylight ? "began" : "ended",
          newState.timeOfDay, newState.isInterior ? "interior" : "exterior");
        m_lastOpenDaylight = openDaylight;
      }
      newState.isDark = m_darkGate.Update(openDaylight ? 1.0f : newState.lightLevel, m_darkLightLevel.load(),
          placeID, BoolDebouncer::Clock::now(), &dropped);
      if (dropped) {
        logger::debug("[Debounce] dark {} for {} ms, not published"sv,
          dropped->rawValue ? "on" : "off", dropped->lasted.count());
      }
      }

      // Crosshair detection for world objects (locks, ore veins, workstations)
      auto* crosshairRef = GetCrosshairReference();
      if (crosshairRef) {
      DetectLockTarget(crosshairRef, newState);
      DetectOreVeinTarget(crosshairRef, newState);
      DetectWorkstationTarget(crosshairRef, newState);
      }

      // Hold the workstation after the crosshair leaves it. The bench is read
      // off the crosshair only, so glancing at the ingredients, the inventory
      // or round the room dropped it: at an alchemy lab on 2026-10-05 it was
      // on for 39.6 s of 164 s in 37 on/off stints of about a second, and the
      // craft gear never settled on a key. The player is still standing at
      // the bench; the context stays for WORKSTATION_HOLD_SEC after the last
      // sighting, and a different bench replaces it at once.
      {
        const auto now = std::chrono::steady_clock::now();
        if (newState.workstationType != 0) {
          m_heldWorkstation = newState.workstationType;
          m_workstationSeenAt = now;
        } else if (m_heldWorkstation != 0 &&
                   now - m_workstationSeenAt < std::chrono::duration<float>(Config::WORKSTATION_HOLD_SEC)) {
          newState.workstationType = m_heldWorkstation;
          newState.isLookingAtWorkstation = true;
        } else {
          m_heldWorkstation = 0;
        }
      }

      // Stage 3b: Return change detection flag
      bool changed = UpdateStateIfChanged(m_worldMutex, m_worldState, newState);
#ifdef _DEBUG
      if (changed) {
      // workstationType is in the equality check that gates this line but was not
      // in the line itself, so a session spent walking on and off an alchemy
      // lab logged ten identical "changed" lines and the one field that moved
      // was invisible (it logs at trace, which is effectively off). 0 = not
      // looking at a bench; otherwise it is the BenchType the craft weight
      // comes from, so the apparel/potion gate can be read straight off this.
      // rawLight is the game's value before scaling (-1 = unavailable, clock
      // estimate used): read it here to calibrate GAME_LIGHT_SCALE.
      logger::info("[StateManager] WorldState changed - time:{:.1f} interior:{} light:{:.2f} (raw {:.1f}) dark:{} workstation:{}"sv,
        newState.timeOfDay, newState.isInterior, newState.lightLevel, rawLight, newState.isDark, newState.workstationType);
      }
#endif
      return changed;
   }

} // namespace Huginn::State
