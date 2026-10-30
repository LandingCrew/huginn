#pragma once

#include "core/ChoiceCosave.h"   // LoadDecision
#include "learning/ChoiceLearner.h"

#include <cstdint>

namespace Huginn::Persist
{
   // =========================================================================
   // SKSE Cosave -- the choice learner (R8.3): THTA and BIAS; DISC (0.24.14)
   // =========================================================================
   // Written and read inside BanditSerializer's callbacks (one SKSE unique ID
   // per plugin), beside HCID and BNDW. The byte formats are the pure codec's
   // (core/ChoiceCosave.h, host-tested):
   //   THTA v1  the bootstrap's identity (created_utc, SHA-256) and the keyed
   //            weights {key, mu, var} (theta, strength, recent, covered) that
   //            moved from the bootstrap -- the load gives every other key the
   //            file's value, so writing them all (~36k, 0.87 MB a save)
   //            would restore the same state. The saving is early only: an
   //            answered onset grows the variance of all of its need's theta
   //            and strength columns (2 x J = 486 entries), so a fight's worth
   //            of needs is thousands of entries, and long play nears the
   //            full set;
   //   BIAS v1  b per item {formID, mu, var, n, play minutes since chosen,
   //            favourite bit}. The map's plan was {formID, b, n, minutes};
   //            the variance and the favourite bit are added (ChoiceCosave.h).
   //   DISC v1  the discovery boost (core/DiscoveryBoost.h): every form this
   //            character has held or known, and the boosted ones with their
   //            play seconds since the gain. The character's, not the model's:
   //            written and applied whether or not the bootstrap is usable.
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
   // DISC follows the same decision, before the model check: new game
   // cleared; failed load unchanged; reload of the same character the
   // in-memory sets kept, dynamic forms from the save; anything else the
   // save's sets (none in the save: cleared). Main.cpp then primes it with
   // what the load holds (seen, not boosted).
   //   model off          (no or invalid bootstrap) nothing is applied; the
   //                      records this save holds are kept and every save
   //                      this session makes writes them back unchanged, so
   //                      a session without the bootstrap loses no learning.
   //                      A reset empties them (ResetChoiceLearning). A reload
   //                      of the same character keeps the carried ones, as
   //                      it keeps the model, b's dynamic forms from the save
   //                      (core/ChoiceCosave.h CarryLoaded); a reset holds.
   // =========================================================================

   inline constexpr uint32_t kRecordType_Theta = 'ATHT';  // 'THTA' on disk
   inline constexpr uint32_t kRecordType_Bias  = 'SAIB';  // 'BIAS' on disk
   inline constexpr uint32_t kRecordType_Discovery = 'CSID';  // 'DISC' on disk

   /// What kind of load ResolveLoadedLearner decided (BanditSerializer.h);
   /// defined in core so the model-off carry (CarryLoaded) is host-tested.
   using LoadDecision = Core::ChoiceCosave::LoadDecision;

   [[nodiscard]] const char* LoadDecisionName(LoadDecision d) noexcept;

   /// SaveCallback: the two records (when the model is off, the records the
   /// load read, carried over unchanged).
   void SaveChoiceRecords(SKSE::SerializationInterface* a_intfc);
   /// LoadCallback: one record each; `length` is the SKSE record length.
   void LoadThetaRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length);
   void LoadBiasRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length);
   void LoadDiscoveryRecord(SKSE::SerializationInterface* a_intfc, uint32_t version, uint32_t length);
   /// LoadCallback start and RevertCallback: drop the buffered records.
   void ResetPendingChoice();

   /// Reset Learned Weights, `hg reset weights` and `hg reset all` (inside
   /// RunExclusive): theta back to the bootstrap and b cleared, no
   /// sBootstrapBias seed. With the model off (no bootstrap) there is no theta
   /// to reset, so the records carried from the load are emptied instead
   /// (core/ChoiceCosave.h ResetCarried, 0.24.18): the saves stop writing the
   /// learning from before the reset. Returns what was done, for the player's
   /// message.
   [[nodiscard]] const char* ResetChoiceLearning(uint64_t characterID);

   /// kPostLoadGame / kNewGame, once the learner exists and its bootstrap
   /// was read (Main.cpp step 6c). `characterID`: the character the load
   /// resolved to (b's sBootstrapBias match).
   void ResolveLoadedChoice(Learning::ChoiceLearner& learner, LoadDecision decision, uint64_t characterID);
}
