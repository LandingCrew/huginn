#pragma once

#include "StateConstants.h"
#include <cmath>

namespace Huginn::State
{
   // =============================================================================
   // WORLD STATE (v0.6.1)
   // =============================================================================
   // Represents the state of the game world environment and interactable objects.
   // Separates world/environment concerns from actor-specific state (player/targets).
   //
   // CONTENTS:
   // - Time and lighting (time of day, light level, interior flag)
   // - Interactable objects (locks, ore veins, workstations)
   //
   // THREAD SAFETY:
   // - Trivially copyable struct (safe for copy-out pattern)
   // - All fields are POD types or constexpr-sized
   //
   // DESIGN NOTE:
   // This struct fixes the mixed concerns in CrosshairState (v0.6.0), which
   // combined target actors + world objects. WorldState now holds all world
   // objects, while TargetActorState handles actors.
   // =============================================================================

   struct WorldState
   {
      // =============================================================================
      // TIME AND LIGHTING
      // =============================================================================

      float timeOfDay = DefaultState::NOON_TIME;  // 0-24 hour format
      float lightLevel = DefaultState::DEFAULT_LIGHT;  // 0.0 = dark, 1.0 = bright
      bool isInterior = false;  // True if in interior cell

      // =============================================================================
      // INTERACTABLE OBJECTS (from crosshair detection)
      // =============================================================================

      // Lock detection
      bool isLookingAtLock = false;
      int lockLevel = 0;  // 0=Novice, 1=Apprentice, 2=Adept, 3=Expert, 4=Master, 5=Requires Key

      // Ore vein detection
      bool isLookingAtOreVein = false;

      // Workstation detection
      bool isLookingAtWorkstation = false;
      uint8_t workstationType = 0;  // RE::TESFurniture::WorkBenchData::BenchType
                                 // 0=None, 1=Forge, 2=Smithing, 3=Enchanting, 4=EnchantExp, 5=Alchemy, 6=AlchemyExp, 7=Tanning, 8=Smelter, 9=Cooking

      // =============================================================================
      // HELPER METHODS
      // =============================================================================

      // Time helpers
      [[nodiscard]] bool IsNightTime() const noexcept {
      return timeOfDay < LightLevel::NIGHT_END || timeOfDay > LightLevel::NIGHT_START;
      }

      [[nodiscard]] bool IsDayTime() const noexcept {
      return timeOfDay >= LightLevel::DAYTIME_START && timeOfDay <= LightLevel::DAYTIME_END;
      }

      // Light helpers
      [[nodiscard]] bool IsDark() const noexcept {
      return lightLevel < LightLevel::DARK_THRESHOLD;
      }

      [[nodiscard]] bool IsWellLit() const noexcept {
      return lightLevel > LightLevel::WELL_LIT_THRESHOLD;
      }

      // Lock helpers
      [[nodiscard]] bool IsNoviceLock() const noexcept {
      return isLookingAtLock && lockLevel == LockLevel::NOVICE;
      }

      [[nodiscard]] bool IsApprenticeLock() const noexcept {
      return isLookingAtLock && lockLevel == LockLevel::APPRENTICE;
      }

      [[nodiscard]] bool IsAdeptLock() const noexcept {
      return isLookingAtLock && lockLevel == LockLevel::ADEPT;
      }

      [[nodiscard]] bool IsExpertLock() const noexcept {
      return isLookingAtLock && lockLevel == LockLevel::EXPERT;
      }

      [[nodiscard]] bool IsMasterLock() const noexcept {
      return isLookingAtLock && lockLevel == LockLevel::MASTER;
      }

      [[nodiscard]] bool RequiresKey() const noexcept {
      return isLookingAtLock && lockLevel == LockLevel::REQUIRES_KEY;
      }

      // =============================================================================
      // DIAGNOSTIC LOGGING (Debug builds only)
      // =============================================================================

#ifdef _DEBUG
      void LogDifferences(const WorldState& other) const {
      if (std::abs(timeOfDay - other.timeOfDay) >= Epsilon::TIME_OF_DAY) {
        logger::trace("  WorldState.timeOfDay changed: {:.2f} -> {:.2f}", other.timeOfDay, timeOfDay);
      }
      if (std::abs(lightLevel - other.lightLevel) >= Epsilon::LIGHT_LEVEL) {
        logger::trace("  WorldState.lightLevel changed: {:.2f} -> {:.2f}", other.lightLevel, lightLevel);
      }
      if (isInterior != other.isInterior) {
        logger::trace("  WorldState.isInterior changed: {} -> {}", other.isInterior, isInterior);
      }
      if (isLookingAtLock != other.isLookingAtLock) {
        logger::trace("  WorldState.isLookingAtLock changed: {} -> {}", other.isLookingAtLock, isLookingAtLock);
      }
      if (lockLevel != other.lockLevel) {
        logger::trace("  WorldState.lockLevel changed: {} -> {}", other.lockLevel, lockLevel);
      }
      if (isLookingAtOreVein != other.isLookingAtOreVein) {
        logger::trace("  WorldState.isLookingAtOreVein changed: {} -> {}", other.isLookingAtOreVein, isLookingAtOreVein);
      }
      if (isLookingAtWorkstation != other.isLookingAtWorkstation) {
        logger::trace("  WorldState.isLookingAtWorkstation changed: {} -> {}", other.isLookingAtWorkstation, isLookingAtWorkstation);
      }
      if (workstationType != other.workstationType) {
        logger::trace("  WorldState.workstationType changed: {} -> {}", other.workstationType, workstationType);
      }
      }
#endif

      // =============================================================================
      // EQUALITY COMPARISON (epsilon-tolerant)
      // =============================================================================

      bool operator==(const WorldState& other) const {
      bool equal = std::abs(timeOfDay - other.timeOfDay) < Epsilon::TIME_OF_DAY &&
                   std::abs(lightLevel - other.lightLevel) < Epsilon::LIGHT_LEVEL &&
                   isInterior == other.isInterior &&
                   isLookingAtLock == other.isLookingAtLock &&
                   lockLevel == other.lockLevel &&
                   isLookingAtOreVein == other.isLookingAtOreVein &&
                   isLookingAtWorkstation == other.isLookingAtWorkstation &&
                   workstationType == other.workstationType;

#ifdef _DEBUG
      if (!equal) {
        logger::trace("WorldState changed:");
        LogDifferences(other);
      }
#endif
      return equal;
      }
   };
}
