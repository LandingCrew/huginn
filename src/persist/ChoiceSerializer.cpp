#include "ChoiceSerializer.h"
#include "Globals.h"

#include "core/ChoiceCosave.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Huginn::Persist
{
   namespace CC = Core::ChoiceCosave;

   // Buffered by LoadCallback, consumed by ResolveLoadedChoice. The SKSE
   // callbacks and InitializeGameSystems run on the game thread.
   static std::optional<CC::ThetaRecord> s_pendingTheta;
   static std::optional<std::vector<Learning::ChoiceLearner::BEntry>> s_pendingBias;
   static std::size_t s_biasUnresolved = 0;
   // 0.24.14: DISC, FormIDs resolved to this session's load order.
   static std::optional<Core::Discovery::Saved> s_pendingDiscovery;

   // Fallback mode (no bootstrap, or an invalid one): the records the load
   // read, kept as they were and written back by every save in this session.
   // Without them one session with Huginn_Bootstrap.json missing would drop
   // theta and b from every save made in it. Set by ResolveLoadedChoice;
   // BIAS holds the FormIDs as resolved at load, so a save writes them in this
   // session's load order. b's play minutes stay as read (the battery does
   // not run while the model is off). Unused once the model is ready, which
   // is fixed for the session (LoadBootstrap runs once). A reset empties them
   // (ResetChoiceLearning, 0.24.18), from the console and the dMenu button;
   // a ModCallbackEvent may come on any thread, hence the mutex.
   // s_carriedResetHolds: a reset since the carried records last came from a
   // save (a replace) or a new game. A reload of the same character keeps the
   // carried records only then (CarryLoaded); otherwise it takes the save's.
   // Set by ResetCarried, cleared by a new game and a replace only (0.24.23):
   // the reset ticked the learning clock past the saves memory had reached, so
   // every load of one of those is a reload and must keep the emptied records,
   // not only the first (until 0.24.22 every decision but a failed load
   // cleared it, so a second load of a pre-reset save brought its learning
   // back). A save ahead of memory is a replace, as the ready model takes it.
   // NOT cleared on revert: SKSE reverts before every load, so the bit would
   // be gone before the reload could read it.
   static std::mutex s_carriedMutex;
   static std::optional<CC::ThetaRecord> s_carriedTheta;
   static std::optional<CC::BiasRecord> s_carriedBias;
   static bool s_carriedResetHolds = false;

   const char* LoadDecisionName(LoadDecision d) noexcept
   {
      switch (d) {
      case LoadDecision::NewGame: return "new game";
      case LoadDecision::Failed: return "failed load";
      case LoadDecision::Reload: return "reload of the same character";
      case LoadDecision::Replace: return "load";
      }
      return "?";
   }

   const char* ResetChoiceLearning(uint64_t characterID)
   {
      if (!g_choiceLearner) return "no choice learner yet";
      if (g_choiceLearner->Ready()) {
         // No sBootstrapBias seed: a reset forgets this character's b, the
         // file's included.
         g_choiceLearner->ResetToBootstrap(characterID, /*seedB=*/false);
         return "theta back to the bootstrap, b cleared";
      }
      // Fallback mode: no theta or b lives in memory, but the saves write the
      // carried records back. Without this the reset would not reach them,
      // and the learning from before it would return with the bootstrap.
      // Every reload of the same character keeps the emptied records until a
      // new game or a replace (CarryLoaded reads the reset bit this sets), as
      // the ready path keeps a reset model.
      std::size_t thetaWas = 0, biasWas = 0;
      {
         std::lock_guard lock(s_carriedMutex);
         thetaWas = s_carriedTheta ? s_carriedTheta->entries.size() : 0;
         biasWas = s_carriedBias ? s_carriedBias->entries.size() : 0;
         CC::ResetCarried(s_carriedTheta, s_carriedBias, s_carriedResetHolds);
      }
      logger::info("[Cosave] Reset with the choice model off (no usable bootstrap): the THTA / BIAS carried from "
                   "the load dropped ({} theta entries, {} b entries); this session's saves write theta unmoved and "
                   "b empty, and reloads of this character keep them so, until a new game or a later save / another "
                   "character is loaded"sv,
         thetaWas, biasWas);
      return "choice model off (no usable bootstrap): the save's theta and b dropped";
   }

   void ResetPendingChoice()
   {
      s_pendingTheta.reset();
      s_pendingBias.reset();
      s_biasUnresolved = 0;
      s_pendingDiscovery.reset();
   }

   // DISC (0.24.14): the discovery boost's sets, written whether or not the
   // model is ready (the record is the character's: what it has held).
   static void SaveDiscovery(SKSE::SerializationInterface* a_intfc)
   {
      if (!g_choiceLearner) return;
      const auto saved = g_choiceLearner->ExportDiscovery();
      CC::DiscoveryRecord rec;
      rec.seen = saved.seen;
      rec.boosted = saved.boosted;
      const auto blob = CC::EncodeDiscovery(rec);
      if (!a_intfc->OpenRecord(kRecordType_Discovery, CC::kDiscoveryVersion) ||
          !a_intfc->WriteRecordData(blob.data(), static_cast<uint32_t>(blob.size()))) {
         logger::error("[Cosave] Failed to write the DISC record ({} bytes)"sv, blob.size());
         return;
      }
      logger::info("[Cosave] Saved DISC: {} forms seen, {} discovery boosts"sv, rec.seen.size(), rec.boosted.size());
   }

   static bool WriteTheta(SKSE::SerializationInterface* a_intfc, const CC::ThetaRecord& rec)
   {
      const auto blob = CC::EncodeTheta(rec);
      if (!a_intfc->OpenRecord(kRecordType_Theta, CC::kThetaVersion) ||
          !a_intfc->WriteRecordData(blob.data(), static_cast<uint32_t>(blob.size()))) {
         logger::error("[Cosave] Failed to write the THTA record ({} bytes)"sv, blob.size());
         return false;
      }
      return true;
   }

   static bool WriteBias(SKSE::SerializationInterface* a_intfc, const CC::BiasRecord& rec)
   {
      const auto blob = CC::EncodeBias(rec);
      if (!a_intfc->OpenRecord(kRecordType_Bias, CC::kBiasVersion) ||
          !a_intfc->WriteRecordData(blob.data(), static_cast<uint32_t>(blob.size()))) {
         logger::error("[Cosave] Failed to write the BIAS record ({} bytes)"sv, blob.size());
         return false;
      }
      return true;
   }

   void SaveChoiceRecords(SKSE::SerializationInterface* a_intfc)
   {
      SaveDiscovery(a_intfc);
      if (!g_choiceLearner || !g_choiceLearner->Ready()) {
         // Fallback ranking (no bootstrap): no theta lives in memory. Write
         // back what the load read, unchanged (or emptied by a reset), so the
         // next load with the bootstrap restored finds the learning this
         // session could not use.
         std::lock_guard lock(s_carriedMutex);
         const bool theta = s_carriedTheta && WriteTheta(a_intfc, *s_carriedTheta);
         const bool bias = s_carriedBias && WriteBias(a_intfc, *s_carriedBias);
         logger::warn("[Cosave] Choice model off (no usable bootstrap): THTA {} / BIAS {} carried over from the load "
                      "({} theta entries, {} b entries; a reset since empties them)"sv,
            theta ? "written" : "not", bias ? "written" : "not",
            theta ? s_carriedTheta->entries.size() : 0, bias ? s_carriedBias->entries.size() : 0);
         return;
      }
      // THTA
      {
         CC::ThetaRecord rec;
         std::tie(rec.createdUtc, rec.sha256) = g_choiceLearner->FileIdentity();
         // Only the weights whose {mu, var} moved from the bootstrap: a key
         // the save lacks takes the file's value on load (MergeTheta), so this
         // writes the same state. The saving shrinks with play: every answered
         // onset grows the variance of all of its need's theta and strength
         // columns (GrowNeed, 2 x J entries), so each need that has had an
         // onset adds about 486 entries, and a long game nears the full set.
         const auto entries = g_choiceLearner->ExportEntries(/*movedOnly=*/true);
         rec.entries.reserve(entries.size());
         for (const auto& e : entries) rec.entries.push_back(CC::ThetaEntry{ e.key, e.mu, e.var });
         if (WriteTheta(a_intfc, rec)) {
            logger::info("[Cosave] Saved THTA: {} theta entries moved from the bootstrap (bootstrap {}, sha256 {})"sv,
               rec.entries.size(), rec.createdUtc.empty() ? "?" : rec.createdUtc, rec.sha256.substr(0, 16));
         }
      }
      // BIAS
      {
         CC::BiasRecord rec;
         const auto entries = g_choiceLearner->ExportB();
         rec.entries.reserve(entries.size());
         for (const auto& e : entries) {
            rec.entries.push_back(CC::BiasEntry{ e.form, e.mu, e.var, e.n, e.minutesSinceChosen,
               static_cast<uint8_t>(e.favorite ? CC::kBiasFavorite : 0) });
         }
         if (WriteBias(a_intfc, rec)) {
            logger::info("[Cosave] Saved BIAS: {} b entries"sv, rec.entries.size());
         }
      }
   }

   static std::optional<std::vector<std::byte>> ReadBlob(SKSE::SerializationInterface* a_intfc, uint32_t length,
      std::size_t maxLength, const char* tag)
   {
      // Before the allocation: a corrupt length must not throw bad_alloc on
      // the game thread. SKSE skips the unread rest of the record.
      if (length > maxLength) {
         logger::error("[Cosave] {} record of {} bytes, above the largest valid one ({}) -- skipping"sv, tag, length,
            maxLength);
         return std::nullopt;
      }
      std::vector<std::byte> blob(length);
      const uint32_t got = length ? a_intfc->ReadRecordData(blob.data(), length) : 0;
      if (got != length) {
         logger::error("[Cosave] {} read short: got {} of {} bytes -- skipping"sv, tag, got, length);
         return std::nullopt;
      }
      return blob;
   }

   void LoadThetaRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length)
   {
      const auto blob = ReadBlob(a_intfc, length, CC::kMaxThetaBlobBytes, "THTA");
      if (!blob) return;
      std::string why;
      auto rec = CC::DecodeTheta(*blob, version, &why);
      if (!rec) {
         logger::warn("[Cosave] THTA unreadable ({}): this load starts theta from the bootstrap"sv, why);
         return;
      }
      logger::info("[Cosave] Read THTA: {} theta entries ({} not finite or var <= 0, dropped), bootstrap {} sha256 {}"sv,
         rec->entries.size(), rec->dropped, rec->createdUtc.empty() ? "?" : rec->createdUtc, rec->sha256.substr(0, 16));
      s_pendingTheta = std::move(*rec);
   }

   void LoadBiasRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length)
   {
      const auto blob = ReadBlob(a_intfc, length, CC::kMaxBiasBlobBytes, "BIAS");
      if (!blob) return;
      std::string why;
      auto rec = CC::DecodeBias(*blob, version, &why);
      if (!rec) {
         logger::warn("[Cosave] BIAS unreadable ({}): b starts under sBootstrapBias"sv, why);
         return;
      }
      std::vector<Learning::ChoiceLearner::BEntry> out;
      out.reserve(rec->entries.size());
      s_biasUnresolved = 0;
      for (const auto& e : rec->entries) {
         // Load order changes move FormIDs (as BNDW's entries do).
         RE::FormID resolved = 0;
         if (!a_intfc->ResolveFormID(e.form, resolved)) {
            ++s_biasUnresolved;
            continue;
         }
         out.push_back(Learning::ChoiceLearner::BEntry{ resolved, e.mu, e.var, e.n, e.minutes,
            (e.flags & CC::kBiasFavorite) != 0 });
      }
      logger::info("[Cosave] Read BIAS: {} b entries ({} unresolved, {} not finite, dropped)"sv,
         out.size(), s_biasUnresolved, rec->dropped);
      s_pendingBias = std::move(out);
   }

   void LoadDiscoveryRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length)
   {
      const auto blob = ReadBlob(a_intfc, length, CC::kMaxDiscoveryBlobBytes, "DISC");
      if (!blob) return;
      std::string why;
      auto rec = CC::DecodeDiscovery(*blob, version, &why);
      if (!rec) {
         logger::warn("[Cosave] DISC unreadable ({}): this load starts the discovery boost's seen set from what the "
                      "character holds"sv, why);
         return;
      }
      Core::Discovery::Saved out;
      out.seen.reserve(rec->seen.size());
      std::size_t unresolved = 0;
      for (const auto f : rec->seen) {
         RE::FormID resolved = 0;
         if (a_intfc->ResolveFormID(f, resolved) && resolved != 0) out.seen.push_back(resolved);
         else ++unresolved;
      }
      for (const auto& [f, sec] : rec->boosted) {
         RE::FormID resolved = 0;
         if (a_intfc->ResolveFormID(f, resolved) && resolved != 0) out.boosted.emplace_back(resolved, sec);
         else ++unresolved;
      }
      logger::info("[Cosave] Read DISC: {} forms seen, {} discovery boosts ({} unresolved, {} form 0, dropped)"sv,
         out.seen.size(), out.boosted.size(), unresolved, rec->dropped);
      s_pendingDiscovery = std::move(out);
   }

   /// DISC under the load's decision (the header); before the model check.
   static void ResolveDiscovery(Learning::ChoiceLearner& learner, LoadDecision decision,
      std::optional<Core::Discovery::Saved> disc)
   {
      switch (decision) {
      case LoadDecision::Failed:
         return;
      case LoadDecision::NewGame:
         learner.ClearDiscovery();
         return;
      case LoadDecision::Reload: {
         const auto [removed, added] = learner.ReplaceDynamicDiscovery(disc ? *disc : Core::Discovery::Saved{});
         if (removed || added) {
            logger::info("[Cosave] Reload: the discovery boost's dynamic forms from the save ({} dropped, {} taken)"sv,
               removed, added);
         }
         return;
      }
      case LoadDecision::Replace:
         if (disc) {
            learner.ImportDiscovery(*disc);
         } else {
            learner.ClearDiscovery();
            logger::info("[Cosave] No DISC in this save (older than 0.24.14, or unreadable): the discovery boost's "
                         "seen set starts from what the character holds"sv);
         }
         return;
      }
   }

   void ResolveLoadedChoice(Learning::ChoiceLearner& learner, LoadDecision decision, uint64_t characterID)
   {
      auto theta = std::move(s_pendingTheta);
      auto bias = std::move(s_pendingBias);
      auto disc = std::move(s_pendingDiscovery);
      ResetPendingChoice();
      ResolveDiscovery(learner, decision, std::move(disc));
      std::unique_lock carriedLock(s_carriedMutex);
      if (!learner.Ready()) {
         // Fallback mode: nothing to apply them to. The carried records
         // follow the load's decision (core/ChoiceCosave.h CarryLoaded): this
         // save's records for a load, none for a new game, unchanged for a
         // failed load. A reload of the same character keeps them after a
         // reset, every time until a new game or a replace, so the reset
         // holds; otherwise it takes the save's, since the carried records do
         // not learn while the learning clock, which calls the load a reload,
         // runs on.
         std::optional<CC::BiasRecord> savedBias;
         if (bias) {
            CC::BiasRecord rec;
            rec.entries.reserve(bias->size());
            for (const auto& e : *bias) {
               rec.entries.push_back(CC::BiasEntry{ e.form, e.mu, e.var, e.n, e.minutesSinceChosen,
                  static_cast<uint8_t>(e.favorite ? CC::kBiasFavorite : 0) });
            }
            savedBias = std::move(rec);
         }
         const auto carry = CC::CarryLoaded(decision, s_carriedTheta, s_carriedBias, s_carriedResetHolds,
            std::move(theta), std::move(savedBias));
         const char* source = decision == LoadDecision::Failed ? "the carried records, unchanged"
                              : decision == LoadDecision::NewGame ? "nothing"
                              : carry.keptCarried ? "the carried records, kept (reset, no new game or replace since)"
                                                  : "this save's records";
         logger::warn("[Cosave] Choice model off (no usable bootstrap): {}, THTA / BIAS not applied; this session's "
                      "saves write back {}: THTA {} / BIAS {} ({} theta entries, {} b entries{})"sv,
            LoadDecisionName(decision), source, s_carriedTheta ? "present" : "absent",
            s_carriedBias ? "present" : "absent", s_carriedTheta ? s_carriedTheta->entries.size() : 0,
            s_carriedBias ? s_carriedBias->entries.size() : 0,
            carry.dynamicDropped || carry.dynamicTaken
               ? std::format("; dynamic-form b from the save: {} dropped, {} taken", carry.dynamicDropped,
                    carry.dynamicTaken)
               : std::string{});
         return;
      }
      // The model is ready for the whole session (LoadBootstrap runs once):
      // nothing is carried, and a reset never sets the bit.
      s_carriedTheta.reset();
      s_carriedBias.reset();
      s_carriedResetHolds = false;
      carriedLock.unlock();
      switch (decision) {
      case LoadDecision::Failed:
         logger::warn("[Cosave] Load failed -- the choice learner is left as it was"sv);
         return;
      case LoadDecision::NewGame:
         learner.ResetToBootstrap(characterID, /*seedB=*/true);
         return;
      case LoadDecision::Reload: {
         // As BNDW: the in-memory learning survives the reload; only the
         // dynamic forms (0xFF) come from the save, since the engine reuses
         // those IDs.
         learner.ClearTransient();
         const auto [removed, added] = bias ? learner.ReplaceDynamicB(*bias) : std::pair<std::size_t, std::size_t>{};
         const auto st = learner.GetStats();
         logger::info("[Cosave] Reload of the same character ({:016X}): kept the in-memory choice learner "
                      "(theta {} entries, {} moved from the bootstrap; b {} items); dynamic-form b from the save: "
                      "{} dropped, {} taken"sv,
            characterID, st.thetaEntries, st.thetaMoved, st.bEntries, removed, added);
         return;
      }
      case LoadDecision::Replace:
         break;
      }

      // The bootstrap first; b seeded under sBootstrapBias only when the
      // save carries no BIAS to replace it with.
      learner.ResetToBootstrap(characterID, /*seedB=*/!bias.has_value());
      if (theta) {
         std::vector<CC::ThetaEntry> saved = std::move(theta->entries);
         CC::MergeReport rep;
         const auto keys = learner.Keys();
         const auto merged = CC::MergeTheta(keys, saved, rep);
         std::vector<Learning::ChoiceLearner::Entry> entries;
         entries.reserve(merged.size());
         for (const auto& e : merged) entries.push_back(Learning::ChoiceLearner::Entry{ e.key, e.mu, e.var });
         const std::size_t applied = learner.ImportEntries(entries);
         const auto [fileUtc, fileSha] = learner.FileIdentity();
         const bool sameFile = theta->sha256 == fileSha;
         logger::info("[Cosave] THTA applied over the bootstrap: {} entries from the save, {} keys the save lacks at "
                      "the file's value, {} saved keys this build no longer knows (dropped){}{}"sv,
            applied, rep.fromFile, rep.unknown,
            rep.duplicate ? std::format(", {} duplicate key(s) (the last kept)", rep.duplicate) : std::string{},
            sameFile ? std::string{}
                     : std::format("; NOTE: the save learned from bootstrap {} ({}), this build reads {} ({})",
                          theta->createdUtc.empty() ? "?" : theta->createdUtc, theta->sha256.substr(0, 16),
                          fileUtc.empty() ? "?" : fileUtc, fileSha.substr(0, 16)));
      } else {
         logger::info("[Cosave] No THTA in this save (older than 0.24.0, or unreadable): theta from the bootstrap"sv);
      }
      if (bias) {
         learner.ImportB(*bias);
         logger::info("[Cosave] BIAS applied: {} b entries from the save"sv, bias->size());
      }
   }
}
