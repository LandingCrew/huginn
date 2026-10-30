#pragma once

#include "core/LearningClock.h"

#include <cstdint>
#include <optional>

namespace Huginn::Persist
{
   // =========================================================================
   // SKSE Cosave -- HCID: which character, and its learning clock (0.24.22:
   // moved out of BanditSerializer, which R12 retires with the old learner)
   // =========================================================================
   // Written and read inside BanditSerializer's callbacks (one SKSE unique ID
   // per plugin), beside THTA / BIAS / DISC and BNDW:
   //   v2: [characterID: uint64] [learning clock: uint64]
   // The ID is random at new game; a save without the record is treated as a
   // different character and given a new one. Lets a load tell "the same
   // character, reloaded" from "a different character", and "this save is
   // behind what is in memory" from "this save is ahead" (core/
   // LearningClock.h DecideLoad). The clock is g_learningClock, which both
   // learners and the resets tick (0.24.22; the old learner's own count
   // before); the format did not change, so older saves load as they are.
   // Older Huginn versions skip the record with one warning.
   // =========================================================================
   inline constexpr uint32_t kRecordType_CharacterID = 'DICH';  // 'HCID' on disk
   inline constexpr uint32_t kCharacterIDVersion     = 2;

   /// SaveCallback: the HCID record. Called AFTER the learners' records are
   /// written, so the clock it saves is at least the one their exports saw: a
   /// save never pairs an older clock with newer learning. (A clock read
   /// later than a record can only claim more learning than the save holds,
   /// which makes a same-session reload keep memory -- the safe side.)
   void SaveCharacterRecord(SKSE::SerializationInterface* a_intfc);
   /// LoadCallback: the record (`version` from the SKSE header).
   void LoadCharacterRecord(SKSE::SerializationInterface* a_intfc, uint32_t version);
   /// LoadCallback start and RevertCallback: forget the save's HCID.
   void ResetPendingCharacter();

   /// The load's decision (core/LearningClock.h DecideLoad) and the character
   /// it resolves to. A new ID is drawn here when the decision asks for one.
   struct CharacterLoad
   {
      Core::LearningClock::LoadVerdict verdict;
      uint64_t characterID = 0;            ///< the character after the load
      std::optional<uint64_t> savedClock;  ///< the save's clock, for the log
      uint64_t memClock = 0;               ///< the clock the decision read, for the log
   };

   /// kNewGame / kPostLoadGame, step 5: decide what the learning becomes.
   /// Consumes the buffered HCID and changes nothing else; the learners
   /// follow the decision, then CommitLoad.
   [[nodiscard]] CharacterLoad DecideCharacterLoad(bool isNewGame, bool loadSucceeded);

   /// Set the learning clock and the active character to the decision's, and
   /// log it. AFTER the old learner followed it (its Clear ticks the clock);
   /// the choice learner's load steps (step 6c) do not tick.
   void CommitLoad(const CharacterLoad& load);
}
