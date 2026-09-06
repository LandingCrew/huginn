#pragma once

#include "learning/FeatureBanditLearner.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Huginn::Persist
{
   // =========================================================================
   // SKSE Cosave Serialization — FeatureBanditLearner weights
   // =========================================================================
   // Persists FeatureBanditLearner weights alongside Skyrim saves via BNDW records.
   // =========================================================================

   // Record type (packed 4CC — SKSE interprets as little-endian uint32_t)
   inline constexpr uint32_t kRecordType_BanditWeights  = 'WDNB';  // 'BNDW' on disk
   inline constexpr uint32_t kBanditSerializationVersion = 2;
   inline constexpr uint32_t kUniqueID                = 'QCNO';  // 'ONCQ' on disk

   // Safety caps — prevent corrupted data from causing huge allocations
   inline constexpr uint32_t kMaxBanditItems        = 50'000;
   inline constexpr uint32_t kMaxBanditFeatures     = 256;

   // The uint32_t byte-length math in LoadCallback/DecodeV2EntryBlob relies on
   // the worst-case v2 blob staying below UINT32_MAX.
   static_assert(uint64_t{kMaxBanditItems}
         * (sizeof(RE::FormID) + sizeof(float) * uint64_t{kMaxBanditFeatures} + 2 * sizeof(uint32_t))
      <= UINT32_MAX,
      "kMaxBanditItems x max entry stride must fit uint32_t byte lengths");

   // Feature-count migration: BNDW records store the feature count they were
   // written with. When it differs from the compiled NUM_FEATURES, entries are
   // migrated positionally at load — a smaller on-disk count zero-pads the
   // tail (new features start untrained), a larger one truncates it — instead
   // of discarding the whole learned table. This is only sound because the
   // feature vector is APPEND-ONLY (see StateFeatures.h): features may be
   // added at the end, never reordered or removed.

   // Decode a v2 BNDW entry blob written with diskFeatureCount weights per
   // entry into compiled-layout entries (positional pad/truncate migration).
   // data/byteLen must hold exactly numItems entries of on-disk stride
   //   sizeof(RE::FormID) + sizeof(float) * diskFeatureCount + 2 * sizeof(uint32_t);
   // returns empty if byteLen does not match. Exposed for tests.
   [[nodiscard]] std::vector<Learning::FeatureBanditLearner::SerializedEntry>
   DecodeV2EntryBlob(const std::byte* data, size_t byteLen,
      uint32_t numItems, uint32_t diskFeatureCount);

   // Buffered learner data from cosave Load callback
   struct LoadedBanditData {
      std::vector<Learning::FeatureBanditLearner::SerializedEntry> entries;
      uint32_t totalTrainCount = 0;
      uint32_t resolvedFormIDs = 0;
      uint32_t failedFormIDs = 0;
   };

   // Register Save/Load/Revert callbacks with SKSE serialization interface.
   // Must be called from SKSEPlugin_Load (before any save/load events fire).
   void RegisterSerialization();

   // Check if the Load callback has buffered learner data waiting to be applied.
   bool HasPendingBanditData();

   // Move buffered cosave data into the FeatureBanditLearner instance.
   // Returns true on success. Clears the buffer regardless.
   bool ApplyPendingBanditData(Learning::FeatureBanditLearner& learner);
}
