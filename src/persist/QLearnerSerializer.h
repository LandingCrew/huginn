#pragma once

#include "learning/FeatureQLearner.h"
#include <optional>
#include <vector>

namespace Huginn::Persist
{
   // =========================================================================
   // SKSE Cosave Serialization — FeatureQLearner weights
   // =========================================================================
   // Persists FeatureQLearner weights alongside Skyrim saves via FQLW records.
   // =========================================================================

   // Record type (packed 4CC — SKSE interprets as little-endian uint32_t)
   inline constexpr uint32_t kRecordType_FQLWeights  = 'WLQF';  // 'FQLW' on disk
   inline constexpr uint32_t kSerializationVersion    = 1;
   inline constexpr uint32_t kFQLSerializationVersion = 2;
   inline constexpr uint32_t kUniqueID                = 'QCNO';  // 'ONCQ' on disk

   // Safety cap — prevent corrupted data from causing huge allocations
   inline constexpr uint32_t kMaxFQLItems        = 50'000;

   // Buffered FQL data from cosave Load callback
   struct LoadedFQLData {
      std::vector<Learning::FeatureQLearner::SerializedEntry> entries;
      uint32_t totalTrainCount = 0;
      uint32_t resolvedFormIDs = 0;
      uint32_t failedFormIDs = 0;
   };

   // Register Save/Load/Revert callbacks with SKSE serialization interface.
   // Must be called from SKSEPlugin_Load (before any save/load events fire).
   void RegisterSerialization();

   // Check if the Load callback has buffered FQL data waiting to be applied.
   bool HasPendingFQLData();

   // Move buffered cosave data into the FeatureQLearner instance.
   // Returns true on success. Clears the buffer regardless.
   bool ApplyPendingFQLData(Learning::FeatureQLearner& fql);
}
