#include "QLearnerSerializer.h"
#include "Globals.h"

namespace Huginn::Persist
{
   // Static buffer — populated by LoadCallback, consumed by ApplyPendingFQLData
   static std::optional<LoadedFQLData> s_pendingFQLData;

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

         // Collect entries
         std::vector<Learning::FeatureQLearner::SerializedEntry> fqlEntries;
         fqlEntries.reserve(g_featureQLearner->GetItemCount());
         uint32_t fqlTotalTrains = 0;

         g_featureQLearner->ExportData(
            [&](Learning::FeatureQLearner::SerializedEntry entry) {
               fqlEntries.push_back(std::move(entry));
            },
            fqlTotalTrains
         );

         // Write: version + numFeatures + totalTrainCount + numItems + [formID + weights[18] + trainCount]...
         a_intfc->WriteRecordData(kFQLSerializationVersion);
         uint32_t numFeatures = Learning::StateFeatures::NUM_FEATURES;
         a_intfc->WriteRecordData(numFeatures);
         a_intfc->WriteRecordData(fqlTotalTrains);
         uint32_t numItems = static_cast<uint32_t>(fqlEntries.size());
         a_intfc->WriteRecordData(numItems);

         for (const auto& e : fqlEntries) {
            a_intfc->WriteRecordData(e.formID);
            for (size_t i = 0; i < Learning::StateFeatures::NUM_FEATURES; ++i) {
               a_intfc->WriteRecordData(e.weights[i]);
            }
            a_intfc->WriteRecordData(e.trainCount);
            a_intfc->WriteRecordData(e.minutesSinceLastUpdate);  // v2
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

            uint32_t numFeatures;
            if (!a_intfc->ReadRecordData(numFeatures)) {
               logger::error("[Cosave] Failed to read FQLW numFeatures"sv);
               break;
            }
            if (numFeatures != Learning::StateFeatures::NUM_FEATURES) {
               logger::warn("[Cosave] FQLW feature count mismatch: expected {}, got {} — skipping"sv,
                  Learning::StateFeatures::NUM_FEATURES, numFeatures);
               break;
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

            for (uint32_t i = 0; i < numItems; ++i) {
               Learning::FeatureQLearner::SerializedEntry entry;
               if (!a_intfc->ReadRecordData(entry.formID)) {
                  logger::error("[Cosave] Failed to read FQLW formID at index {}"sv, i);
                  break;
               }

               bool weightsOk = true;
               for (size_t f = 0; f < Learning::StateFeatures::NUM_FEATURES; ++f) {
                  if (!a_intfc->ReadRecordData(entry.weights[f])) {
                     logger::error("[Cosave] Failed to read FQLW weight at item {}, feature {}"sv, i, f);
                     weightsOk = false;
                     break;
                  }
               }
               if (!weightsOk) break;

               if (!a_intfc->ReadRecordData(entry.trainCount)) {
                  logger::error("[Cosave] Failed to read FQLW trainCount at index {}"sv, i);
                  break;
               }

               // v2: read minutesSinceLastUpdate; v1: default to 0 (treat as fresh)
               if (recVersion >= 2) {
                  if (!a_intfc->ReadRecordData(entry.minutesSinceLastUpdate)) {
                     logger::error("[Cosave] Failed to read FQLW minutesSinceLastUpdate at index {}"sv, i);
                     break;
                  }
               } else {
                  entry.minutesSinceLastUpdate = 0;
               }

               // Resolve FormID for mod load order changes
               RE::FormID newFormID;
               if (a_intfc->ResolveFormID(entry.formID, newFormID)) {
                  entry.formID = newFormID;
                  fqlData.resolvedFormIDs++;
                  fqlData.entries.push_back(std::move(entry));
               } else {
                  fqlData.failedFormIDs++;
                  logger::debug("[Cosave] FQL FormID {:08X} failed to resolve"sv, entry.formID);
               }
            }

            logger::info("[Cosave] Loaded {} FQL entries ({} resolved, {} failed)"sv,
               fqlData.entries.size(), fqlData.resolvedFormIDs, fqlData.failedFormIDs);
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
