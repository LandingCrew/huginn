#pragma once

#include "learning/FeatureBanditLearner.h"
#include "persist/ChoiceSerializer.h"   // LoadDecision (R8.3)
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
   // R8.3: the same callbacks carry the choice learner's THTA and BIAS
   // (persist/ChoiceSerializer.h). BNDW STAYS: R6's D2 reads the old learner's
   // per-item state; THTA/BIAS replace it only once D2 is retired.
   // =========================================================================

   // Record type (packed 4CC — SKSE interprets as little-endian uint32_t)
   inline constexpr uint32_t kRecordType_BanditWeights  = 'WDNB';  // 'BNDW' on disk
   // v4 (0.23.6): memory with a useful life. Fixed stride (SerializedEntry's
   // memory layout); the count is a float (the evidence n, fractional after a
   // fade) and the minutes are PLAY minutes since the item was last chosen.
   // Only v4 with NUM_FEATURES weights per entry loads: any other version or
   // feature count is skipped with a warning and the old learner starts empty
   // (v4 has been written since 2026-10-07 and NUM_FEATURES has always been
   // 18; one tester, no older saves to carry).
   inline constexpr uint32_t kBanditSerializationVersion = 4;
   inline constexpr uint32_t kUniqueID                = 'QCNO';  // 'ONCQ' on disk

   // HCID (the character and its learning clock) is persist/CharacterRecord.h
   // since 0.24.22: it outlives this record.

   // Safety cap — prevents corrupted data from causing huge allocations
   inline constexpr uint32_t kMaxBanditItems        = 50'000;

   // The uint32_t byte-length math in LoadCallback/DecodeEntryBlob relies on
   // the largest accepted blob staying below UINT32_MAX.
   static_assert(uint64_t{kMaxBanditItems} * sizeof(Learning::FeatureBanditLearner::SerializedEntry)
      <= UINT32_MAX,
      "kMaxBanditItems x entry stride must fit uint32_t byte lengths");

   // Whether a BNDW record's header (its in-data version and feature count)
   // is one LoadCallback reads. Anything but the current version with
   // NUM_FEATURES weights per entry is skipped (no migration).
   enum class BndwHeader : uint8_t { Accept, WrongVersion, WrongFeatureCount };
   [[nodiscard]] constexpr BndwHeader CheckBndwHeader(uint32_t recordVersion, uint32_t numFeatures) noexcept
   {
      if (recordVersion != kBanditSerializationVersion) return BndwHeader::WrongVersion;
      if (numFeatures != Learning::StateFeatures::NUM_FEATURES) return BndwHeader::WrongFeatureCount;
      return BndwHeader::Accept;
   }

   // Decode a v4 BNDW entry blob: exactly numItems entries of
   // sizeof(SerializedEntry) bytes each (the on-disk layout IS the struct's,
   // see the static_asserts in BanditSerializer.cpp). Returns empty if byteLen
   // does not match. Exposed for tests.
   // `unitTest` marks a call from the unit tests' negative case: the rejection
   // is then logged at info as a test, not at error as a save fault.
   [[nodiscard]] std::vector<Learning::FeatureBanditLearner::SerializedEntry>
   DecodeEntryBlob(const std::byte* data, size_t byteLen, uint32_t numItems, bool unitTest = false);

   // Buffered learner data from cosave Load callback
   struct LoadedBanditData {
      std::vector<Learning::FeatureBanditLearner::SerializedEntry> entries;
      uint32_t totalTrainCount = 0;   // the header's, for the log; ImportData sums the entries
      uint32_t resolvedFormIDs = 0;
      uint32_t failedFormIDs = 0;
   };

   // Register Save/Load/Revert callbacks with SKSE serialization interface.
   // Must be called from SKSEPlugin_Load (before any save/load events fire).
   void RegisterSerialization();

   // Move buffered cosave data into the FeatureBanditLearner instance.
   // Returns true on success. Clears the buffer regardless.
   bool ApplyPendingBanditData(Learning::FeatureBanditLearner& learner);

   // The old learner follows the load's decision (persist/CharacterRecord.h
   // DecideCharacterLoad; core/LearningClock.h DecideLoad), at kPostLoadGame /
   // kNewGame, before CommitLoad sets the learning clock (its Clear ticks it).
   //   - New game: cleared.
   //   - Failed load: unchanged.
   //   - Reload (the character in memory, the save not ahead): the in-memory
   //     learner kept, except dynamic-form (0xFF) entries, taken from the save.
   //   - Replace: the save's learner (cleared if the save has none).
   // R12 deletes this with the old learner; the decision stays. R12 must also
   // move the save/load callback registration (RegisterSerialization, unique
   // ID 'QCNO', whose callbacks call SaveCharacterRecord, LoadCharacterRecord
   // and ResetPendingCharacter beside THTA / BIAS / DISC) out of this file.
   void ResolveLoadedLearner(Learning::FeatureBanditLearner& learner, LoadDecision decision);
}
