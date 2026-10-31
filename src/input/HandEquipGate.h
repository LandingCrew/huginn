#pragma once

#include "EquipHand.h"
#include "../ui/SlotTypes.h"
#include "core/HandEquipGate.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace Huginn::Input
{
   // =============================================================================
   // HAND EQUIP GATE -- the game half of core/HandEquipGate.h (0.24.26)
   // =============================================================================
   // A slot key that equips into a hand (spell, scroll, weapon, staff, torch)
   // no longer calls the engine where the key was decoded. EquipSlot does the
   // press's bookkeeping there (the press grace, Remembrance, the selection) and
   // parks the equip here; Tick, on the main thread every frame, reads the
   // hands' magic casters and equips once the checked hands are idle (or only
   // in a transient state for Core::kTransientWaitMs). A cast still going
   // Core::kActiveWaitMaxMs after the park drops the equip. The cast is never
   // interrupted: the player letting go of it is what lets the swap through.
   //
   // THREADS (established for this change, not assumed):
   //   - Park: from EquipSlot, i.e. from the slot callback InputHandler calls,
   //     on UpdateHandler's InputEvent sink. In gameplay that is one of the
   //     game's rotating job threads; in paused menus the main thread; during
   //     a load a loading-screen thread (UpdateLoop.cpp, THREADS above
   //     OnUpdate, from the Tracy traces). InputHandler::Update -- where a tap
   //     fires, 300 ms after key-up -- runs on the same sink, so a deferred
   //     tap's EquipSlot runs there too. Since 0.24.26 the slot keys do
   //     nothing in a loading screen, the main menu or a pausing menu
   //     (SlotKeysBlockedBy), so in practice Park runs on a job thread.
   //   - Tick: the PlayerCharacter::Update hook (DropAheadProbe.cpp,
   //     HookUpdate), after the game's own update: the main thread, every
   //     frame the player updates (not while the game is paused). The equip
   //     and the redraw run there. The act also takes UpdateHandler's mutex
   //     (try-lock: a tick holding it means next frame), so it never overlaps
   //     an update tick, as EquipSlot did not when it ran on the sink before
   //     the tick.
   //   - Drop: any thread (the sink, a load message, `hg reset all`).
   //
   // One pending equip: a new press replaces it (its selection withdrawn, or
   // handed over when it is the same item: Core::OnReplaced). Dropped as
   // gameplay ends -- a loading screen, the main menu, a pausing menu, no
   // player 3D (InputHandler on the transition, Tick on its own check) -- and
   // on kPreLoadGame and ResetPipelineSubsystems. Not on a mere gap in the
   // hook's frames (an autosave or a cell crossing stalls it for a moment
   // while gameplay goes on); a pause it never saw is caught by the
   // InputHandler transition, which every menu's input reaches.
   //
   // A weapon swap half done (the other hand's unequip queued, the equip
   // waiting a frame for it: EquipWeapon's retry) is finished, not dropped,
   // when gameplay pauses: dropping it would leave the weapon in neither
   // hand. Only a load or a reset (Drop's `force`) abandons it.
   //
   // Without the hook (its install failed) nothing would run Tick, so
   // IsDriven is false and EquipSlot equips inline as before 0.24.26.
   // =============================================================================

   /// What a parked equip needs to act, captured at the press: the item and
   /// stack as EquipManager equips them (never re-read from the slot, whose
   /// page can change while the equip waits).
   struct HandEquipRequest
   {
      size_t slotIndex = 0;
      size_t page = 0;                   ///< the page the key was pressed on
      EquipHand hand = EquipHand::Right;
      UI::SlotContentType type = UI::SlotContentType::Empty;
      RE::FormID formID = 0;             ///< resolved at the press (a spell found by name included)
      uint16_t uniqueID = 0;             ///< the weapon stack (ExtraUniqueID); 0 = the engine picks
      std::string name;
      bool occupiesBoth = false;         ///< a two-hander, bow, crossbow or both-hands spell
      bool selected = false;             ///< the press opened a selection (SelectionTracker): withdrawn if the equip never lands
      bool remembers = false;            ///< the press registered a Remembrance capture (not the undo)
      std::chrono::steady_clock::time_point pressedAt{};  ///< the gesture's first key-down (the call, without one)
      std::chrono::steady_clock::time_point parkedAt{};
      uint8_t handSwapAttempt = 0;       ///< EquipWeapon's other-hand swap, retried here a frame later
      bool swapInFlight = false;         ///< a retry: the other hand's unequip is already queued
   };

   /// What the act did: the equip's result, and a hand whose weapon swap must
   /// wait a frame for the engine to apply the unequip (EquipWeapon).
   struct HandEquipResult
   {
      bool success = false;
      std::optional<EquipHand> retry;
   };

   /// Why the slot keys may not act now, or "" when they may: a loading
   /// screen, the main menu, a pausing menu, no player 3D (or no UI yet).
   /// Read on the input sink and in Tick.
   [[nodiscard]] std::string_view SlotKeysBlockedBy(RE::PlayerCharacter* player);

   class HandEquipGate
   {
   public:
      static HandEquipGate& GetSingleton();

      /// The PlayerCharacter::Update hook is installed (InstallHooks).
      void SetDriven(bool driven) noexcept { m_driven.store(driven, std::memory_order_release); }
      [[nodiscard]] bool IsDriven() const noexcept { return m_driven.load(std::memory_order_acquire); }

      /// Park one hand equip. A pending one is replaced. Logged at debug.
      void Park(HandEquipRequest request);

      /// Forget the pending equip. Logged at info when there was one.
      /// `force` (a load, a reset) also abandons a weapon swap half done;
      /// without it (gameplay pausing or ending) that swap is kept to finish.
      void Drop(std::string_view why, bool force = false);

      /// Main thread, every frame: decide and act (core/HandEquipGate.h).
      void Tick(RE::PlayerCharacter* player);

   private:
      HandEquipGate() = default;
      HandEquipGate(const HandEquipGate&) = delete;
      HandEquipGate& operator=(const HandEquipGate&) = delete;

      struct Pending
      {
         HandEquipRequest req;
         std::uint64_t seq = 0;            ///< which press; a newer Park changes it
         Core::HandEquipWait wait;
         bool busySeen = false;            ///< a checked hand was busy at least once
         std::uint8_t firstBusyHands = 0;  ///< the first busy reading, for the line when it goes
         std::uint32_t firstRightState = 0;
         std::uint32_t firstLeftState = 0;
         bool lockMissLogged = false;      ///< the "update tick holds the mutex" line, once per press
      };

      /// Take the pending equip out if it is still press `seq`; false when a
      /// newer press replaced it or it was dropped.
      bool TakeIfCurrent(std::uint64_t seq, Pending& out);
      /// Put a weapon swap's retry back, unless a newer press arrived or a
      /// forced Drop ran since `forceGen` was read.
      void Repark(Pending pending, EquipHand hand, std::uint64_t forceGen);
      /// Act on `p` (taken out already) and handle what the act returns.
      void Act(Pending taken, double waitedMs, const char* how, std::uint64_t forceGen);
      static void WithdrawSelection(const HandEquipRequest& req, std::string_view why);
      static std::string Describe(const HandEquipRequest& req);

      std::mutex m_mutex;
      std::optional<Pending> m_pending;   // under m_mutex
      std::uint64_t m_nextSeq = 1;        // under m_mutex
      std::uint64_t m_forceGen = 0;       // under m_mutex: bumped by every forced Drop
      std::atomic<bool> m_hasPending{ false };   // Tick's fast path; written under m_mutex
      std::atomic<bool> m_driven{ false };
   };
}
