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

      // Light level calculation (from EnvironmentSensor)
      // Quantized to 10% increments to reduce jitter
      // Note: Uses hoursSinceNoon directly; hoursSinceSunrise was unused
      if (newState.isInterior) {
      newState.lightLevel = LightLevel::INTERIOR_DEFAULT;
      } else {
      float hoursSinceNoon = std::abs(newState.timeOfDay - LightLevel::NOON);
      float daylight = 1.0f - (hoursSinceNoon / 6.0f);  // Peak at noon
      newState.lightLevel = std::max(LightLevel::NIGHTTIME_BASE, daylight);
      }
      // Quantize to 10% increments
      newState.lightLevel = std::round(newState.lightLevel * LightLevel::QUANTIZATION_MULTIPLIER) / LightLevel::QUANTIZATION_MULTIPLIER;

      // Crosshair detection for world objects (locks, ore veins, workstations)
      auto* crosshairRef = GetCrosshairReference();
      if (crosshairRef) {
      DetectLockTarget(crosshairRef, newState);
      DetectOreVeinTarget(crosshairRef, newState);
      DetectWorkstationTarget(crosshairRef, newState);
      }

      // Stage 3b: Return change detection flag
      bool changed = UpdateStateIfChanged(m_worldMutex, m_worldState, newState);
#ifdef _DEBUG
      if (changed) {
      logger::info("[StateManager] WorldState changed - time:{:.1f} interior:{} light:{:.2f}"sv,
        newState.timeOfDay, newState.isInterior, newState.lightLevel);
      }
#endif
      return changed;
   }

} // namespace Huginn::State
