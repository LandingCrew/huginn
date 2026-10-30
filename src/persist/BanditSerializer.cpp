#include "BanditSerializer.h"
#include "ChoiceSerializer.h"   // R8.3: THTA / BIAS ride in the same callbacks
#include "Globals.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

#include <chrono>
#include <random>

namespace Huginn::Persist
{
   using BanditEntry = Learning::FeatureBanditLearner::SerializedEntry;

   // The batch (single-blob) cosave path memcpy's the whole entry array, so the
   // on-disk layout IS SerializedEntry's memory layout. These asserts lock that
   // contract: the entry must be trivially copyable and tightly packed. The size
   // check equals the exact field sum — if a field is added/reordered or padding
   // appears, this fails to compile, forcing a wire-format + version bump instead
   // of silently writing an incompatible blob. Byte order: formID,
   // weights[18], trainCount (float), minutesSinceChosen.
   static_assert(std::is_trivially_copyable_v<BanditEntry>,
      "SerializedEntry must be trivially copyable for batch cosave I/O");
   static_assert(sizeof(BanditEntry) ==
         sizeof(RE::FormID)
       + sizeof(float) * Learning::StateFeatures::NUM_FEATURES
       + sizeof(float)      // trainCount
       + sizeof(uint32_t),  // minutesSinceChosen
      "SerializedEntry layout changed — batch cosave I/O assumes tight packing; "
      "bump kBanditSerializationVersion and update the wire format");

   // Static buffer — populated by LoadCallback, consumed by ApplyPendingBanditData
   static std::optional<LoadedBanditData> s_pendingBanditData;

   // The character the in-memory learner belongs to (0 = none yet), and the
   // character ID / learning clock the save being loaded carries (none = no
   // readable HCID record: the save is treated as a different character). Touched
   // only from the SKSE callbacks and InitializeGameSystems, all on the game
   // thread. NOT under the update loop's exclusion -- InitializeGameSystems
   // does not run inside RunExclusive -- which is why the learner is replaced
   // in one ImportData call (one lock) rather than Clear() then import.
   static uint64_t s_activeCharacterID = 0;
   static std::optional<uint64_t> s_loadedCharacterID;
   static std::optional<uint64_t> s_loadedClock;

   static uint64_t NewCharacterID()
   {
      std::random_device rd;
      std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ rd() ^
         static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
      uint64_t id = 0;
      while (id == 0) id = gen();
      return id;
   }

   // The accept/skip decision, checked at compile time (the Debug suite's
   // Test 4 checks it at run time too).
   static_assert(CheckBndwHeader(kBanditSerializationVersion, Learning::StateFeatures::NUM_FEATURES)
      == BndwHeader::Accept);
   static_assert(CheckBndwHeader(3, Learning::StateFeatures::NUM_FEATURES) == BndwHeader::WrongVersion);
   static_assert(CheckBndwHeader(kBanditSerializationVersion, Learning::StateFeatures::NUM_FEATURES - 2)
      == BndwHeader::WrongFeatureCount);

   std::vector<BanditEntry> DecodeEntryBlob(
      const std::byte* data, size_t byteLen, uint32_t numItems, bool unitTest)
   {
      constexpr size_t stride = sizeof(BanditEntry);
      if (byteLen != stride * numItems) {
         // The unit tests feed a short blob on purpose. Logged at [E] like a
         // real one, it was read as a save dropping learned weights on load
         // (2026-10-01), so a test call says so on the line itself.
         if (unitTest) {
            logger::info("[Cosave Test] DecodeEntryBlob: byteLen {} != stride {} x numItems {} — "
               "rejecting (UNIT TEST: deliberate short blob, not a save fault)"sv,
               byteLen, stride, numItems);
         } else {
            logger::error("[Cosave] DecodeEntryBlob: byteLen {} != stride {} x numItems {} — rejecting"sv,
               byteLen, stride, numItems);
         }
         return {};
      }
      // The save side writes the entry array as one blob, so the read is one
      // memcpy back (trivially copyable and tightly packed: asserted above).
      std::vector<BanditEntry> out(numItems);
      if (byteLen > 0) {
         std::memcpy(out.data(), data, byteLen);
      }
      return out;
   }

   // =========================================================================
   // SaveCallback — serialize FeatureBanditLearner data into cosave records
   // =========================================================================
   static void SaveCallback(SKSE::SerializationInterface* a_intfc)
   {
      // ── HCID record: which character, and its learning clock ──────────────
      // Written first and on its own, so a failure in the learner record
      // below cannot take it down. The ID is always set by now --
      // ResolveLoadedLearner runs at every new game and load, and no save
      // precedes both -- so a zero here is a bug: logged, and given an ID so
      // the save is still identified.
      if (s_activeCharacterID == 0) {
         s_activeCharacterID = NewCharacterID();
         g_activeCharacterID.store(s_activeCharacterID, std::memory_order_relaxed);
         logger::warn("[Cosave] No character ID at save time (unexpected) -- assigned {:016X}"sv,
            s_activeCharacterID);
      }
      const uint64_t clock = g_featureBanditLearner ? g_featureBanditLearner->GetClock() : 0;
      if (!a_intfc->OpenRecord(kRecordType_CharacterID, kCharacterIDVersion) ||
          !a_intfc->WriteRecordData(s_activeCharacterID) ||
          !a_intfc->WriteRecordData(clock)) {
         logger::error("[Cosave] Failed to write the HCID record"sv);
      }

      // ── THTA / BIAS (R8.3): the choice learner, beside BNDW (which STAYS:
      // R6's D2 reads the old learner's state). Before BNDW, so a failure in
      // that record's early returns cannot take these down.
      SaveChoiceRecords(a_intfc);

      // ── BNDW record: FeatureBanditLearner weights + train counts ──────────
      if (g_featureBanditLearner) {
         if (!a_intfc->OpenRecord(kRecordType_BanditWeights, kBanditSerializationVersion)) {
            logger::error("[Cosave] Failed to open BNDW record"sv);
            return;
         }

         // Collect entries into a contiguous buffer for a single bulk write.
         std::vector<BanditEntry> banditEntries;
         banditEntries.reserve(g_featureBanditLearner->GetItemCount());
         uint32_t banditTotalTrains = 0;

         g_featureBanditLearner->ExportData(
            [&](BanditEntry entry) {
               banditEntries.push_back(std::move(entry));
            },
            banditTotalTrains
         );

         // Header: version + numFeatures + totalTrainCount + numItems
         // (version is also in the SKSE record header via OpenRecord; the
         // in-data copy is the original wire format and must stay — the load
         // side cross-checks the two.)
         uint32_t numFeatures = Learning::StateFeatures::NUM_FEATURES;
         uint32_t numItems = static_cast<uint32_t>(banditEntries.size());
         if (!a_intfc->WriteRecordData(kBanditSerializationVersion) ||
             !a_intfc->WriteRecordData(numFeatures) ||
             !a_intfc->WriteRecordData(banditTotalTrains) ||
             !a_intfc->WriteRecordData(numItems)) {
            logger::error("[Cosave] Failed to write BNDW header"sv);
            return;
         }

         // The write side trusts numItems: no cap and no byteLen overflow guard.
         // Both are bounded by reality — GetItemCount() is one entry per distinct
         // trained FormID, so numItems > kMaxBanditItems (50k), let alone the ~51M
         // where numItems * sizeof(BanditEntry) overflows uint32_t, cannot occur in a
         // real playthrough. If a corrupt/oversized count somehow reached here, the
         // load side rejects numItems > kMaxBanditItems wholesale (see LoadCallback).

         // Entries: one contiguous blob instead of 21 calls/item; the array's
         // memory layout is the wire format (see static_asserts above).
         if (numItems > 0) {
            const uint32_t byteLen = numItems * static_cast<uint32_t>(sizeof(BanditEntry));
            if (!a_intfc->WriteRecordData(banditEntries.data(), byteLen)) {
               logger::error("[Cosave] Failed to write BNDW entry blob ({} bytes)"sv, byteLen);
               return;
            }
         }

         logger::info("[Cosave] Saved {} learner weight entries, {} total trains"sv,
            numItems, banditTotalTrains);
      } else {
         logger::warn("[Cosave] SaveCallback: g_featureBanditLearner is null, skipping"sv);
      }
   }

   // =========================================================================
   // LoadCallback — deserialize cosave records into static buffer
   // =========================================================================
   static void LoadCallback(SKSE::SerializationInterface* a_intfc)
   {
      s_pendingBanditData = LoadedBanditData{};
      s_loadedCharacterID.reset();
      s_loadedClock.reset();
      ResetPendingChoice();

      uint32_t type, version, length;
      while (a_intfc->GetNextRecordInfo(type, version, length)) {
         switch (type) {
         case kRecordType_CharacterID:
         {
            // v2: the ID and the learning clock. (v1, pre-release 0.22.11
            // builds, is no longer read: one tester, no saves that old.)
            uint64_t id = 0;
            uint64_t clock = 0;
            const bool ok = version == kCharacterIDVersion && a_intfc->ReadRecordData(id) && id != 0 &&
                            a_intfc->ReadRecordData(clock);
            if (ok) {
               s_loadedCharacterID = id;
               s_loadedClock = clock;
            } else {
               logger::warn("[Cosave] Unreadable HCID record (version {}) -- treating the save as unidentified"sv,
                  version);
            }
            break;
         }
         case kRecordType_BanditWeights:
         {
            auto& banditData = *s_pendingBanditData;

            uint32_t recVersion;
            if (!a_intfc->ReadRecordData(recVersion)) {
               logger::error("[Cosave] Failed to read BNDW version"sv);
               break;
            }
            // Only the current version (v4) with NUM_FEATURES weights per
            // entry is read (CheckBndwHeader); anything else is skipped whole
            // and the old learner starts empty. No migration: no save holds
            // another format. A header/in-data version mismatch is logged
            // rather than rejected, and the in-data version is trusted. The
            // header check, the item cap and the exact-length read below
            // validate everything decode relies on.
            if (CheckBndwHeader(recVersion, Learning::StateFeatures::NUM_FEATURES) == BndwHeader::WrongVersion) {
               logger::warn("[Cosave] BNDW version unsupported: got {} (expected {}) — skipping"sv,
                  recVersion, kBanditSerializationVersion);
               break;
            }
            if (recVersion != version) {
               logger::warn("[Cosave] BNDW version mismatch: SKSE header {} vs in-data {} — trusting in-data version"sv,
                  version, recVersion);
            }

            uint32_t numFeatures;
            if (!a_intfc->ReadRecordData(numFeatures)) {
               logger::error("[Cosave] Failed to read BNDW numFeatures"sv);
               break;
            }
            if (CheckBndwHeader(recVersion, numFeatures) != BndwHeader::Accept) {
               logger::warn("[Cosave] BNDW feature count unsupported: got {} (expected {}) — skipping"sv,
                  numFeatures, Learning::StateFeatures::NUM_FEATURES);
               break;
            }

            if (!a_intfc->ReadRecordData(banditData.totalTrainCount)) {
               logger::error("[Cosave] Failed to read BNDW totalTrainCount"sv);
               break;
            }

            uint32_t numItems;
            if (!a_intfc->ReadRecordData(numItems)) {
               logger::error("[Cosave] Failed to read BNDW numItems"sv);
               break;
            }
            if (numItems > kMaxBanditItems) {
               logger::error("[Cosave] BNDW numItems {} exceeds cap {} — skipping"sv,
                  numItems, kMaxBanditItems);
               break;
            }

            banditData.entries.reserve(numItems);
            size_t droppedCorrupt = 0;

            // Validate + resolve one decoded entry, keeping only survivors.
            auto acceptEntry = [&](BanditEntry& entry) {
               // Reject non-finite data before it reaches the scorer. Since
               // v4 the train count is a float too: an Inf there made n_eff
               // Inf and the confidence NaN (code review of #180).
               if (!std::isfinite(entry.trainCount)) {
                  ++droppedCorrupt;
                  logger::warn("[Cosave] learner entry {:08X} has a non-finite train count — dropping"sv,
                     entry.formID);
                  return;
               }
               for (float w : entry.weights) {
                  if (!std::isfinite(w)) {
                     ++droppedCorrupt;
                     logger::warn("[Cosave] learner entry {:08X} has non-finite weight — dropping"sv,
                        entry.formID);
                     return;
                  }
               }
               // Resolve FormID for mod load order changes.
               RE::FormID newFormID;
               if (a_intfc->ResolveFormID(entry.formID, newFormID)) {
                  entry.formID = newFormID;
                  banditData.resolvedFormIDs++;
                  banditData.entries.push_back(entry);
               } else {
                  banditData.failedFormIDs++;
                  logger::debug("[Cosave] learner FormID {:08X} failed to resolve"sv, entry.formID);
               }
            };

            {
               // Fixed-stride entries — read the whole array in one call. A
               // short read rejects the record wholesale (no silent partial
               // import). byteLen cannot overflow: numItems <= kMaxBanditItems
               // (the static_assert in the header).
               std::vector<BanditEntry> raw;
               if (numItems > 0) {
                  const uint32_t byteLen = numItems * static_cast<uint32_t>(sizeof(BanditEntry));
                  std::vector<std::byte> blob(byteLen);
                  const uint32_t got = a_intfc->ReadRecordData(blob.data(), byteLen);
                  if (got != byteLen) {
                     logger::error("[Cosave] BNDW bulk read short: got {} of {} bytes — skipping"sv,
                        got, byteLen);
                     break;
                  }
                  raw = DecodeEntryBlob(blob.data(), blob.size(), numItems);
               }
               for (auto& entry : raw) {
                  acceptEntry(entry);
               }
            }

            logger::info("[Cosave] Loaded {} learner entries ({} resolved, {} failed, {} corrupt)"sv,
               banditData.entries.size(), banditData.resolvedFormIDs, banditData.failedFormIDs, droppedCorrupt);
            break;
         }
         case kRecordType_Theta:
            LoadThetaRecord(a_intfc, version, length);
            break;
         case kRecordType_Bias:
            LoadBiasRecord(a_intfc, version, length);
            break;
         case kRecordType_Discovery:
            LoadDiscoveryRecord(a_intfc, version, length);
            break;
         default:
            logger::warn("[Cosave] Unknown record type {:08X} — skipping"sv, type);
            break;
         }
      }
   }

   // =========================================================================
   // RevertCallback — drop the load buffer (new game / load). The learner is
   // NOT cleared here any more; ResolveLoadedLearner decides (0.22.11).
   // =========================================================================
   static void RevertCallback([[maybe_unused]] SKSE::SerializationInterface* a_intfc)
   {
      // Revert no longer clears the learner. It runs before EVERY load, and
      // clearing here is what made a death-and-reload forget everything
      // learned since the save. ResolveLoadedLearner decides at
      // kPostLoadGame / kNewGame, once it knows whose save this is.
      s_pendingBanditData.reset();
      s_loadedCharacterID.reset();
      s_loadedClock.reset();
      ResetPendingChoice();
      // 0.24.14: no discovery-boost gains until the load's prime (Main.cpp
      // step 6c): nothing this load restores may count as newly gained.
      if (g_choiceLearner) g_choiceLearner->SuspendDiscovery();
      logger::debug("[Cosave] Revert: learner kept until the load decides"sv);
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

   LoadDecision ResolveLoadedLearner(Learning::FeatureBanditLearner& learner, bool isNewGame,
      bool loadSucceeded)
   {
      if (isNewGame) {
         s_pendingBanditData.reset();
         learner.Clear();
         learner.SetClock(0);
         s_activeCharacterID = NewCharacterID();
         g_activeCharacterID.store(s_activeCharacterID, std::memory_order_relaxed);
         logger::info("[Cosave] New game: learner cleared, character ID {:016X}"sv, s_activeCharacterID);
         return LoadDecision::NewGame;
      }

      // A load that failed (a missing master, a corrupt save) restored
      // nothing: keep the learner and the active character exactly as they
      // were, so the next real load of this character still matches.
      if (!loadSucceeded) {
         s_pendingBanditData.reset();
         logger::warn("[Cosave] Load failed -- learner and character ID left as they were"sv);
         return LoadDecision::Failed;
      }

      // A save with no readable HCID record is a different character: it
      // gets a new ID. (Saves from before 0.22.11 used to get one derived
      // from name and race; one tester, no saves that old.)
      const bool identified = s_loadedCharacterID.has_value() && s_loadedClock.has_value();
      const uint64_t loadedID = identified ? *s_loadedCharacterID : NewCharacterID();
      const uint32_t saveTrains = s_pendingBanditData ? s_pendingBanditData->totalTrainCount : 0;

      // Is the save behind memory? By the learning clock.
      const uint64_t memClock = learner.GetClock();
      const bool saveNotAhead = identified && *s_loadedClock <= memClock;

      if (s_activeCharacterID != 0 && loadedID == s_activeCharacterID && saveNotAhead) {
         // A reload of the character in memory: memory holds the save's
         // learning plus whatever was learned since. Dynamic forms (0xFF) are
         // taken from the save, because the engine can reuse those IDs.
         const auto saveEntries = s_pendingBanditData ? std::move(s_pendingBanditData->entries)
                                                     : std::vector<Learning::FeatureBanditLearner::SerializedEntry>{};
         s_pendingBanditData.reset();
         learner.ReplaceDynamicEntries(saveEntries);
         logger::info("[Cosave] Reload of the same character ({:016X}): kept in-memory learning "
                      "({} items, {} trains, clock {}); the save is not ahead ({} trains, clock {})"sv,
            s_activeCharacterID, learner.GetItemCount(), learner.GetTotalTrainCount(), memClock,
            saveTrains, *s_loadedClock);
         return LoadDecision::Reload;
      }

      // A different character, the first load since launch, or a later save
      // of this one: the save's learner. ImportData replaces everything under
      // one lock; Clear() only when the save carries no learner at all.
      const char* why = !identified ? "unidentified save"
                      : (s_activeCharacterID == 0) ? "first load"
                      : (loadedID != s_activeCharacterID) ? "different character"
                      : "save is ahead of memory";
      if (!ApplyPendingBanditData(learner)) {
         learner.Clear();
      }
      learner.SetClock(s_loadedClock.value_or(0));
      s_activeCharacterID = loadedID;
      g_activeCharacterID.store(s_activeCharacterID, std::memory_order_relaxed);
      logger::info("[Cosave] Loaded character {:016X} ({}): learner from the save ({} items, clock {})"sv,
         s_activeCharacterID, why, learner.GetItemCount(), learner.GetClock());
      return LoadDecision::Replace;
   }

   bool ApplyPendingBanditData(Learning::FeatureBanditLearner& learner)
   {
      if (!s_pendingBanditData.has_value()) {
         return false;
      }

      auto data = std::move(*s_pendingBanditData);
      s_pendingBanditData.reset();

      if (data.entries.empty()) {
         logger::info("[Cosave] No learner data to apply (empty save or all FormIDs failed)"sv);
         return false;
      }

      learner.ImportData(data.entries);

      logger::info("[Cosave] Applied {} learner entries, {} total trains ({} resolved, {} failed)"sv,
         data.entries.size(), data.totalTrainCount, data.resolvedFormIDs, data.failedFormIDs);

      return true;
   }
}
