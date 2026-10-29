#pragma once

#include "learning/ChoiceLearner.h"

#include <cstdint>

namespace Huginn::Persist
{
   // =========================================================================
   // SKSE Cosave -- the choice learner (R8.3): THTA and BIAS
   // =========================================================================
   // Written and read inside BanditSerializer's callbacks (one SKSE unique ID
   // per plugin), beside HCID and BNDW. The byte formats are the pure codec's
   // (core/ChoiceCosave.h, host-tested):
   //   THTA v1  the bootstrap's identity (created_utc, SHA-256) and the keyed
   //            weights {key, mu, var} (theta, strength, recent, covered) that
   //            moved from the bootstrap -- the load gives every other key the
   //            file's value, so writing them all (~36k, 0.87 MB a save)
   //            would restore the same state;
   //   BIAS v1  b per item {formID, mu, var, n, play minutes since chosen,
   //            favourite bit}. The map's plan was {formID, b, n, minutes};
   //            the variance and the favourite bit are added (ChoiceCosave.h).
   //
   // BNDW STAYS. R6's D2 reads the old learner's per-item state, which goes
   // back before the logs; the map's "THTA/BIAS replace BNDW" waits until D2
   // is retired (a refit on 0.23.53+ logs where a variant without the old
   // scorer passes the gate).
   //
   // What a load does (ResolveLoadedChoice, after BanditSerializer decided
   // what kind of load it is, so both learners follow ONE decision):
   //   new game           theta from the bootstrap; b under sBootstrapBias
   //   failed load        nothing changes
   //   reload, same char  the in-memory learning is kept (as BNDW); b's
   //                      dynamic forms (0xFF) come from the save's BIAS
   //   anything else      the bootstrap, then THTA per key over it (keys the
   //                      save lacks keep the file's value, keys the build no
   //                      longer knows are dropped and counted), then BIAS in
   //                      place of b; with no THTA / BIAS (an older save) b
   //                      follows sBootstrapBias.
   // =========================================================================

   inline constexpr uint32_t kRecordType_Theta = 'ATHT';  // 'THTA' on disk
   inline constexpr uint32_t kRecordType_Bias  = 'SAIB';  // 'BIAS' on disk

   /// What kind of load ResolveLoadedLearner decided (BanditSerializer.h).
   enum class LoadDecision : uint8_t
   {
      NewGame,   // a fresh character
      Failed,    // the load restored nothing: keep everything
      Reload,    // the character in memory, the save not ahead: keep memory
      Replace,   // another character, the first load, or a later save
   };

   [[nodiscard]] const char* LoadDecisionName(LoadDecision d) noexcept;

   /// SaveCallback: the two records (nothing when the model is off).
   void SaveChoiceRecords(SKSE::SerializationInterface* a_intfc);
   /// LoadCallback: one record each; `length` is the SKSE record length.
   void LoadThetaRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length);
   void LoadBiasRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length);
   /// LoadCallback start and RevertCallback: drop the buffered records.
   void ResetPendingChoice();

   /// kPostLoadGame / kNewGame, once the learner exists and its bootstrap
   /// was read (Main.cpp step 6c). `characterID`: the character the load
   /// resolved to (b's sBootstrapBias match).
   void ResolveLoadedChoice(Learning::ChoiceLearner& learner, LoadDecision decision, uint64_t characterID);
}
