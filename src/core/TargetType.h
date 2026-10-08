#pragma once

#include <cstdint>

// =============================================================================
// TARGET TYPE -- the six families a target reads as today (plus None)
// =============================================================================
// Moved here from state/GameState.h (which includes this file) so the pure
// actor-type classifier (core/ActorTypeClassifier.h) and its host tests can
// name it. Namespace kept: Huginn::State, as before the move.
// =============================================================================

namespace Huginn::State
{
   // Target type buckets (7 types)
   enum class TargetType : std::uint8_t
   {
      None = 0,       // No target
      Humanoid = 1,   // NPCs, bandits, etc.
      Undead = 2,     // Draugr, skeletons, vampires
      Beast = 3,      // Wolves, bears, sabre cats
      Dragon = 4,     // Dragons
      Construct = 5,  // Dwemer automatons (mechanical)
      Daedra = 6      // Atronachs, Dremora (from Oblivion) - affected by anti-daedra magic
   };

   // Helper to get target type name for UI display (v0.6.11)
   [[nodiscard]] inline constexpr const char* GetTargetTypeName(TargetType type) noexcept {
      switch (type) {
      case TargetType::Humanoid: return "Humanoid";
      case TargetType::Undead: return "Undead";
      case TargetType::Beast: return "Beast";
      case TargetType::Dragon: return "Dragon";
      case TargetType::Construct: return "Construct";
      case TargetType::Daedra: return "Daedra";
      default: return "None";
      }
   }
}  // namespace Huginn::State
