#pragma once

#include "learning/FeatureQLearner.h"
#include <cstddef>
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
   inline constexpr uint32_t kFQLSerializationVersion = 2;
   inline constexpr uint32_t kUniqueID                = 'QCNO';  // 'ONCQ' on disk

   // Safety caps — prevent corrupted data from causing huge allocations
   inline constexpr uint32_t kMaxFQLItems        = 50'000;
   inline constexpr uint32_t kMaxFQLFeatures     = 256;

   // Feature-count migration: FQLW records store the feature count they were
   // written with. When it differs from the compiled NUM_FEATURES, entries are
   // migrated positionally at load — a smaller on-disk count zero-pads the
   // tail (new features start untrained), a larger one truncates it — instead
   // of discarding the whole learned table. This is only sound because the
   // feature vector is APPEND-ONLY (see StateFeatures.h): features may be
   // added at the end, never reordered or removed.

   // Decode a v2 FQLW entry blob written with diskFeatureCount weights per
   // entry into compiled-layout entries (positional pad/truncate migration).
   // data must hold numItems entries of on-disk stride
   //   sizeof(RE::FormID) + sizeof(float) * diskFeatureCount + 2 * sizeof(uint32_t).
   // Exposed for tests.
   [[nodiscard]] std::vector<Learning::FeatureQLearner::SerializedEntry>
   DecodeV2EntryBlob(const std::byte* data, uint32_t numItems, uint32_t diskFeatureCount);

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
