#include "ChoiceSerializer.h"
#include "Globals.h"

#include "core/ChoiceCosave.h"

#include <cstddef>
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

   void ResetPendingChoice()
   {
      s_pendingTheta.reset();
      s_pendingBias.reset();
      s_biasUnresolved = 0;
   }

   void SaveChoiceRecords(SKSE::SerializationInterface* a_intfc)
   {
      if (!g_choiceLearner || !g_choiceLearner->Ready()) {
         // Fallback ranking (no bootstrap): there is no theta to save. A save
         // made now carries no THTA/BIAS, so its next load starts from the
         // bootstrap (logged there).
         logger::warn("[Cosave] Choice model off (no bootstrap): no THTA / BIAS written"sv);
         return;
      }
      // THTA
      {
         CC::ThetaRecord rec;
         std::tie(rec.createdUtc, rec.sha256) = g_choiceLearner->FileIdentity();
         // Only the weights that moved from the bootstrap: a key the save
         // lacks takes the file's value on load (MergeTheta), so this writes
         // the same state in hundreds of entries rather than ~36k.
         const auto entries = g_choiceLearner->ExportEntries(/*movedOnly=*/true);
         rec.entries.reserve(entries.size());
         for (const auto& e : entries) rec.entries.push_back(CC::ThetaEntry{ e.key, e.mu, e.var });
         const auto blob = CC::EncodeTheta(rec);
         if (!a_intfc->OpenRecord(kRecordType_Theta, CC::kThetaVersion) ||
             !a_intfc->WriteRecordData(blob.data(), static_cast<uint32_t>(blob.size()))) {
            logger::error("[Cosave] Failed to write the THTA record ({} bytes)"sv, blob.size());
         } else {
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
         const auto blob = CC::EncodeBias(rec);
         if (!a_intfc->OpenRecord(kRecordType_Bias, CC::kBiasVersion) ||
             !a_intfc->WriteRecordData(blob.data(), static_cast<uint32_t>(blob.size()))) {
            logger::error("[Cosave] Failed to write the BIAS record ({} bytes)"sv, blob.size());
         } else {
            logger::info("[Cosave] Saved BIAS: {} b entries"sv, rec.entries.size());
         }
      }
   }

   static std::optional<std::vector<std::byte>> ReadBlob(SKSE::SerializationInterface* a_intfc, uint32_t length,
      const char* tag)
   {
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
      const auto blob = ReadBlob(a_intfc, length, "THTA");
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
      const auto blob = ReadBlob(a_intfc, length, "BIAS");
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

   void ResolveLoadedChoice(Learning::ChoiceLearner& learner, LoadDecision decision, uint64_t characterID)
   {
      auto theta = std::move(s_pendingTheta);
      auto bias = std::move(s_pendingBias);
      ResetPendingChoice();
      if (!learner.Ready()) {
         logger::warn("[Cosave] Choice model off (no bootstrap): THTA {} / BIAS {} not applied ({})"sv,
            theta ? "read" : "absent", bias ? "read" : "absent", LoadDecisionName(decision));
         return;
      }
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
