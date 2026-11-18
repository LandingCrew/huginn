#include "CharacterRecord.h"
#include "Globals.h"

#include <chrono>
#include <random>

namespace Huginn::Persist
{
   namespace LC = Core::LearningClock;

   // The character in memory (0 = none yet), and the character ID / learning
   // clock the save being loaded carries (none = no readable HCID record: the
   // save is treated as a different character). Touched only from the SKSE
   // callbacks and InitializeGameSystems, all on the game thread.
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

   void SaveCharacterRecord(SKSE::SerializationInterface* a_intfc)
   {
      // The ID is always set by now -- the load decision runs at every new
      // game and load, and no save precedes both -- so a zero here is a bug:
      // logged, and given an ID so the save is still identified.
      if (s_activeCharacterID == 0) {
         s_activeCharacterID = NewCharacterID();
         g_activeCharacterID.store(s_activeCharacterID, std::memory_order_relaxed);
         logger::warn("[Cosave] No character ID at save time (unexpected) -- assigned {:016X}"sv,
            s_activeCharacterID);
      }
      const uint64_t clock = g_learningClock.Get();
      if (!a_intfc->OpenRecord(kRecordType_CharacterID, kCharacterIDVersion) ||
          !a_intfc->WriteRecordData(s_activeCharacterID) ||
          !a_intfc->WriteRecordData(clock)) {
         logger::error("[Cosave] Failed to write the HCID record"sv);
      }
   }

   void LoadCharacterRecord(SKSE::SerializationInterface* a_intfc, uint32_t version)
   {
      // v2: the ID and the learning clock. (v1, pre-release 0.22.11 builds,
      // is no longer read: one tester, no saves that old.)
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
   }

   void ResetPendingCharacter()
   {
      s_loadedCharacterID.reset();
      s_loadedClock.reset();
   }

   CharacterLoad DecideCharacterLoad(bool isNewGame, bool loadSucceeded)
   {
      LC::LoadInput in;
      in.isNewGame = isNewGame;
      in.loadSucceeded = loadSucceeded;
      in.activeID = s_activeCharacterID;
      in.loadedID = s_loadedCharacterID;
      in.loadedClock = s_loadedClock;
      in.memClock = g_learningClock.Get();

      CharacterLoad load;
      load.verdict = LC::DecideLoad(in);
      load.savedClock = s_loadedClock;
      load.memClock = in.memClock;
      load.characterID = load.verdict.newID ? NewCharacterID()
                       : load.verdict.decision == LC::LoadDecision::Replace ? *s_loadedCharacterID
                       : s_activeCharacterID;
      ResetPendingCharacter();
      return load;
   }

   void CommitLoad(const CharacterLoad& load)
   {
      const auto& v = load.verdict;
      if (v.decision == LC::LoadDecision::Failed) {
         logger::warn("[Cosave] Load failed -- learning, clock {} and character ID left as they were"sv,
            g_learningClock.Get());
         return;
      }
      // A reload keeps memory's clock as it is (DecideLoad's `clock` is the
      // value it read; a tick since must not be undone); the others set it.
      if (v.decision != LC::LoadDecision::Reload) g_learningClock.Set(v.clock);
      s_activeCharacterID = load.characterID;
      g_activeCharacterID.store(s_activeCharacterID, std::memory_order_relaxed);
      switch (v.decision) {
      case LC::LoadDecision::NewGame:
         logger::info("[Cosave] New game: character ID {:016X}, learning clock 0"sv, s_activeCharacterID);
         break;
      case LC::LoadDecision::Reload:
         logger::info("[Cosave] Reload of the same character ({:016X}): kept in-memory learning (clock {}); "
                      "the save is not ahead (clock {})"sv,
            s_activeCharacterID, g_learningClock.Get(), load.savedClock.value_or(0));
         break;
      case LC::LoadDecision::Replace:
         logger::info("[Cosave] Loaded character {:016X} ({}): learning from the save (clock {}; memory was at {})"sv,
            s_activeCharacterID, LC::LoadWhyName(v.why), v.clock, load.memClock);
         break;
      case LC::LoadDecision::Failed:   // returned above
         break;
      }
   }
}
