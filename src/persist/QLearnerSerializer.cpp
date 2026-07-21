#include "QLearnerSerializer.h"
#include "Globals.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace Huginn::Persist
{
   using FQLEntry = Learning::FeatureQLearner::SerializedEntry;

   // The batch (single-blob) cosave path memcpy's the whole entry array, so the
   // on-disk layout IS SerializedEntry's memory layout. These asserts lock that
   // contract: the entry must be trivially copyable and tightly packed. The size
   // check equals the exact field sum — if a field is added/reordered or padding
   // appears, this fails to compile, forcing a wire-format + version bump instead
   // of silently writing an incompatible blob. The byte order also matches the
   // legacy per-field format (formID, weights[18], trainCount, minutes), so v2
   // saves round-trip identically between the batch and per-field code paths.
   static_assert(std::is_trivially_copyable_v<FQLEntry>,
      "SerializedEntry must be trivially copyable for batch cosave I/O");
   static_assert(sizeof(FQLEntry) ==
         sizeof(RE::FormID)
       + sizeof(float) * Learning::StateFeatures::NUM_FEATURES
       + sizeof(uint32_t)   // trainCount
       + sizeof(uint32_t),  // minutesSinceLastUpdate (v2)
      "SerializedEntry layout changed — batch cosave I/O assumes tight packing; "
      "bump kFQLSerializationVersion and update the wire format");

   // Static buffer — populated by LoadCallback, consumed by ApplyPendingFQLData
   static std::optional<LoadedFQLData> s_pendingFQLData;

   std::vector<FQLEntry> DecodeV2EntryBlob(
      const std::byte* data, size_t byteLen, uint32_t numItems, uint32_t diskFeatureCount)
   {
      constexpr auto compiled = static_cast<uint32_t>(Learning::StateFeatures::NUM_FEATURES);
      const size_t stride = sizeof(RE::FormID)
                          + sizeof(float) * diskFeatureCount
                          + sizeof(uint32_t) * 2;
      if (byteLen != stride * numItems) {
         logger::error("[Cosave] DecodeV2EntryBlob: byteLen {} != stride {} x numItems {} — rejecting"sv,
            byteLen, stride, numItems);
         return {};
      }
      const uint32_t copyCount = std::min(diskFeatureCount, compiled);

      // Value-init zeroes every weight, so positions >= copyCount stay 0
      // (new features start untrained).
      std::vector<FQLEntry> out(numItems);
      const std::byte* p = data;
      for (uint32_t i = 0; i < numItems; ++i, p += stride) {
         auto& e = out[i];
         std::memcpy(&e.formID, p, sizeof(e.formID));
         std::memcpy(e.weights.data(), p + sizeof(e.formID), sizeof(float) * copyCount);
         const std::byte* tail = p + sizeof(e.formID) + sizeof(float) * diskFeatureCount;
         std::memcpy(&e.trainCount, tail, sizeof(e.trainCount));
         std::memcpy(&e.minutesSinceLastUpdate, tail + sizeof(e.trainCount),
            sizeof(e.minutesSinceLastUpdate));
      }
      return out;
   }

   // =========================================================================
   // SaveCallback — serialize FeatureQLearner data into cosave records
   // =========================================================================
   static void SaveCallback(SKSE::SerializationInterface* a_intfc)
   {
      // ── FQLW record: FeatureQLearner weights + train counts ──────────
      if (g_featureQLearner) {
         if (!a_intfc->OpenRecord(kRecordType_FQLWeights, kFQLSerializationVersion)) {
            logger::error("[Cosave] Failed to open FQLW record"sv);
            return;
         }

         // Collect entries into a contiguous buffer for a single bulk write.
         std::vector<FQLEntry> fqlEntries;
         fqlEntries.reserve(g_featureQLearner->GetItemCount());
         uint32_t fqlTotalTrains = 0;

         g_featureQLearner->ExportData(
            [&](FQLEntry entry) {
               fqlEntries.push_back(std::move(entry));
            },
            fqlTotalTrains
         );

         // Header: version + numFeatures + totalTrainCount + numItems
         // (version is also in the SKSE record header via OpenRecord; the
         // in-data copy is the original wire format and must stay — the load
         // side cross-checks the two.)
         uint32_t numFeatures = Learning::StateFeatures::NUM_FEATURES;
         uint32_t numItems = static_cast<uint32_t>(fqlEntries.size());
         if (!a_intfc->WriteRecordData(kFQLSerializationVersion) ||
             !a_intfc->WriteRecordData(numFeatures) ||
             !a_intfc->WriteRecordData(fqlTotalTrains) ||
             !a_intfc->WriteRecordData(numItems)) {
            logger::error("[Cosave] Failed to write FQLW header"sv);
            return;
         }

         // The write side trusts numItems: no cap and no byteLen overflow guard.
         // Both are bounded by reality — GetItemCount() is one entry per distinct
         // trained FormID, so numItems > kMaxFQLItems (50k), let alone the ~51M
         // where numItems * sizeof(FQLEntry) overflows uint32_t, cannot occur in a
         // real playthrough. If a corrupt/oversized count somehow reached here, the
         // load side rejects numItems > kMaxFQLItems wholesale (see LoadCallback).

         // Entries: one contiguous blob instead of 21 calls/item. The array is
         // byte-identical to the old per-field layout (see static_asserts above),
         // so existing v2 saves remain compatible in both directions.
         if (numItems > 0) {
            const uint32_t byteLen = numItems * static_cast<uint32_t>(sizeof(FQLEntry));
            if (!a_intfc->WriteRecordData(fqlEntries.data(), byteLen)) {
               logger::error("[Cosave] Failed to write FQLW entry blob ({} bytes)"sv, byteLen);
               return;
            }
         }

         logger::info("[Cosave] Saved {} FQL weight entries, {} total trains"sv,
            numItems, fqlTotalTrains);
      } else {
         logger::warn("[Cosave] SaveCallback: g_featureQLearner is null, skipping"sv);
      }
   }

   // =========================================================================
   // LoadCallback — deserialize cosave records into static buffer
   // =========================================================================
   static void LoadCallback(SKSE::SerializationInterface* a_intfc)
   {
      s_pendingFQLData = LoadedFQLData{};

      uint32_t type, version, length;
      while (a_intfc->GetNextRecordInfo(type, version, length)) {
         switch (type) {
         case kRecordType_FQLWeights:
         {
            auto& fqlData = *s_pendingFQLData;

            uint32_t recVersion;
            if (!a_intfc->ReadRecordData(recVersion)) {
               logger::error("[Cosave] Failed to read FQLW version"sv);
               break;
            }
            if (recVersion != 1 && recVersion != 2) {
               logger::warn("[Cosave] FQLW version unsupported: got {} (expected 1 or 2) — skipping"sv,
                  recVersion);
               break;
            }
            // The two copies have been written from the same constant since the
            // repo import, but pre-import v1 writers are not provably identical —
            // so a mismatch is only logged, and the in-data version (the original
            // wire format) is trusted. The version check, bounds checks, and
            // exact-length reads below validate everything decode relies on.
            if (recVersion != version) {
               logger::warn("[Cosave] FQLW version mismatch: SKSE header {} vs in-data {} — trusting in-data version"sv,
                  version, recVersion);
            }

            uint32_t numFeatures;
            if (!a_intfc->ReadRecordData(numFeatures)) {
               logger::error("[Cosave] Failed to read FQLW numFeatures"sv);
               break;
            }
            if (numFeatures == 0 || numFeatures > kMaxFQLFeatures) {
               logger::error("[Cosave] FQLW numFeatures {} out of range [1, {}] — corrupt record, skipping"sv,
                  numFeatures, kMaxFQLFeatures);
               break;
            }
            constexpr auto compiledFeatures = static_cast<uint32_t>(Learning::StateFeatures::NUM_FEATURES);
            if (numFeatures != compiledFeatures) {
               // Positional migration (append-only convention, see header/StateFeatures.h):
               // smaller on-disk count → tail zero-pads (new features untrained);
               // larger → tail truncates (removed features dropped).
               logger::info("[Cosave] FQLW feature-count migration: {} on disk -> {} compiled ({})"sv,
                  numFeatures, compiledFeatures,
                  numFeatures < compiledFeatures ? "zero-padding new features"sv
                                                 : "truncating removed features"sv);
            }

            if (!a_intfc->ReadRecordData(fqlData.totalTrainCount)) {
               logger::error("[Cosave] Failed to read FQLW totalTrainCount"sv);
               break;
            }

            uint32_t numItems;
            if (!a_intfc->ReadRecordData(numItems)) {
               logger::error("[Cosave] Failed to read FQLW numItems"sv);
               break;
            }
            if (numItems > kMaxFQLItems) {
               logger::error("[Cosave] FQLW numItems {} exceeds cap {} — skipping"sv,
                  numItems, kMaxFQLItems);
               break;
            }

            fqlData.entries.reserve(numItems);
            size_t droppedCorrupt = 0;

            // Validate + resolve one decoded entry, keeping only survivors.
            auto acceptEntry = [&](FQLEntry& entry) {
               // Reject non-finite weights before corrupt data reaches the scorer.
               for (float w : entry.weights) {
                  if (!std::isfinite(w)) {
                     ++droppedCorrupt;
                     logger::warn("[Cosave] FQL entry {:08X} has non-finite weight — dropping"sv,
                        entry.formID);
                     return;
                  }
               }
               // Resolve FormID for mod load order changes.
               RE::FormID newFormID;
               if (a_intfc->ResolveFormID(entry.formID, newFormID)) {
                  entry.formID = newFormID;
                  fqlData.resolvedFormIDs++;
                  fqlData.entries.push_back(entry);
               } else {
                  fqlData.failedFormIDs++;
                  logger::debug("[Cosave] FQL FormID {:08X} failed to resolve"sv, entry.formID);
               }
            };

            if (recVersion >= 2) {
               // v2: fixed-stride entries — read the whole array in one call, using
               // the stride the record was WRITTEN with (differs from sizeof(FQLEntry)
               // during feature-count migration). A short read rejects the record
               // wholesale (no silent partial import). byteLen cannot overflow:
               // numItems <= 50k and stride <= 4 + 4*256 + 8, product < 52 MB.
               const size_t diskStride = sizeof(RE::FormID)
                                       + sizeof(float) * numFeatures
                                       + sizeof(uint32_t) * 2;
               std::vector<FQLEntry> raw;
               if (numItems > 0) {
                  const uint32_t byteLen = numItems * static_cast<uint32_t>(diskStride);
                  std::vector<std::byte> blob(byteLen);
                  const uint32_t got = a_intfc->ReadRecordData(blob.data(), byteLen);
                  if (got != byteLen) {
                     logger::error("[Cosave] FQLW bulk read short: got {} of {} bytes — skipping"sv,
                        got, byteLen);
                     break;
                  }
                  raw = DecodeV2EntryBlob(blob.data(), blob.size(), numItems, numFeatures);
               }
               for (auto& entry : raw) {
                  acceptEntry(entry);
               }
            } else {
               // v1 legacy: entries lack minutesSinceLastUpdate — read per field.
               // Reads the on-disk feature count; positions beyond the compiled
               // count are discarded (truncation), missing tail stays zero (pad).
               // Like v2, an incomplete read rejects the record wholesale (no
               // silent partial import): entries are buffered and committed only
               // after every read succeeded.
               std::vector<FQLEntry> raw;
               raw.reserve(numItems);
               bool readOk = true;
               for (uint32_t i = 0; i < numItems && readOk; ++i) {
                  FQLEntry entry{};                  // weights zeroed for migration pad
                  entry.minutesSinceLastUpdate = 0;  // v1: treat as fresh
                  if (!a_intfc->ReadRecordData(entry.formID)) {
                     logger::error("[Cosave] Failed to read FQLW formID at index {}"sv, i);
                     readOk = false;
                     break;
                  }
                  for (uint32_t f = 0; f < numFeatures; ++f) {
                     float w;
                     if (!a_intfc->ReadRecordData(w)) {
                        logger::error("[Cosave] Failed to read FQLW weight at item {}, feature {}"sv, i, f);
                        readOk = false;
                        break;
                     }
                     if (f < compiledFeatures) {
                        entry.weights[f] = w;
                     }
                  }
                  if (!readOk) break;
                  if (!a_intfc->ReadRecordData(entry.trainCount)) {
                     logger::error("[Cosave] Failed to read FQLW trainCount at index {}"sv, i);
                     readOk = false;
                     break;
                  }
                  raw.push_back(entry);
               }
               if (!readOk) {
                  logger::error("[Cosave] FQLW v1 read incomplete — skipping record"sv);
                  break;
               }
               for (auto& entry : raw) {
                  acceptEntry(entry);
               }
            }

            // Recompute totalTrainCount from surviving entries so trains belonging
            // to failed/dropped FormIDs don't inflate the UCB exploration term.
            // Invariant: m_totalTrainCount == sum of per-item trainCounts (the
            // learner increments both together on every train).
            uint32_t survivingTrains = 0;
            for (const auto& e : fqlData.entries) {
               survivingTrains += e.trainCount;
            }
            if (survivingTrains != fqlData.totalTrainCount) {
               logger::info("[Cosave] Adjusted totalTrainCount {} -> {} ({} failed, {} corrupt)"sv,
                  fqlData.totalTrainCount, survivingTrains, fqlData.failedFormIDs, droppedCorrupt);
               fqlData.totalTrainCount = survivingTrains;
            }

            logger::info("[Cosave] Loaded {} FQL entries ({} resolved, {} failed, {} corrupt)"sv,
               fqlData.entries.size(), fqlData.resolvedFormIDs, fqlData.failedFormIDs, droppedCorrupt);
            break;
         }
         default:
            logger::warn("[Cosave] Unknown record type {:08X} — skipping"sv, type);
            break;
         }
      }
   }

   // =========================================================================
   // RevertCallback — clear buffer and FeatureQLearner on revert (new game / load)
   // =========================================================================
   static void RevertCallback([[maybe_unused]] SKSE::SerializationInterface* a_intfc)
   {
      s_pendingFQLData.reset();

      if (g_featureQLearner) {
         g_featureQLearner->Clear();
      }

      logger::info("[Cosave] FeatureQLearner cleared on revert"sv);
   }

   // =========================================================================
   // Public API
   // =========================================================================

   void RegisterSerialization()
   {
      auto* intfc = SKSE::GetSerializationInterface();
      intfc->SetUniqueID(kUniqueID);
      intfc->SetSaveCallback(SaveCallback);
      intfc->SetLoadCallback(LoadCallback);
      intfc->SetRevertCallback(RevertCallback);
   }

   bool HasPendingFQLData()
   {
      return s_pendingFQLData.has_value() && !s_pendingFQLData->entries.empty();
   }

   bool ApplyPendingFQLData(Learning::FeatureQLearner& fql)
   {
      if (!s_pendingFQLData.has_value()) {
         return false;
      }

      auto data = std::move(*s_pendingFQLData);
      s_pendingFQLData.reset();

      if (data.entries.empty()) {
         logger::info("[Cosave] No FQL data to apply (empty save or all FormIDs failed)"sv);
         return false;
      }

      fql.ImportData(data.entries, data.totalTrainCount);

      logger::info("[Cosave] Applied {} FQL entries, {} total trains ({} resolved, {} failed)"sv,
         data.entries.size(), data.totalTrainCount, data.resolvedFormIDs, data.failedFormIDs);

      return true;
   }
}
