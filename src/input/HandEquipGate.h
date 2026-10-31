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
   // A slot key that equips into a hand (spell, scroll, weapon, staff, torch,
   // shield) no longer calls the engine where the key was decoded. EquipSlot
   // does the press's bookkeeping there (the press grace, Remembrance, the
   // selection) and parks the equip here; Tick, on the main thread every
   // frame, reads the hands' magic casters and equips once the checked hands
   // are idle (or only in a transient state for Core::kTransientWaitMs). A
   // cast still going Core::kActiveWaitMaxMs after the park drops the equip.
   // The cast is never interrupted: the player letting go of it is what lets
   // the swap through. Both waits run on the game clock the hook advances
   // (DropAheadProbe::GameSeconds), which slows with slow time and stops
   // while the game is paused.
   //
   // THREADS (established for this change, not assumed):
   //   - Park: from EquipSlot, i.e. from the slot callback InputHandler calls,
   //     on UpdateHandler's InputEvent sink. In gameplay that is one of the
   //     game's rotating job threads; in paused menus the main thread; during
   //     a load a loading-screen thread (UpdateLoop.cpp, THREADS above
   //     OnUpdate, from the Tracy traces). InputHandler::Update -- where a tap
   //     fires, 300 ms after key-up -- runs on the same sink, so a deferred
   //     tap's EquipSlot runs there too. Since 0.24.26 the slot keys do
   //     nothing in a loading screen, the main menu, a pausing menu or a menu
   //     that takes the controls (SlotKeysBlockedBy), so in practice Park runs
   //     on a job thread.
   //   - Tick: the PlayerCharacter::Update hook (DropAheadProbe.cpp,
   //     HookUpdate), after the game's own update: the main thread, every
   //     frame the player updates (not while the game is paused). The equip
   //     and the redraw run there. The act also takes UpdateHandler's mutex
   //     (try-lock: a tick holding it means next frame), so it never overlaps
   //     an update tick, as EquipSlot did not when it ran on the sink before
   //     the tick.
   //   - Drop: any thread (the sink, a load message, `hg reset all`).
   //
   // The selection: while an equip is parked or acting, its form's pending
   // selection is held undecided (SelectionTracker::GateHold); the hold is
   // released when the request leaves -- re-timing the confirm window from
   // the equip when it went through. A selection the press opened (and only
   // then: SelectionTracker::Select's result) is withdrawn if the equip never
   // lands, unless a newer press of the same item took it over.
   //
   // One pending equip: a new press replaces it (its selection withdrawn, or
   // handed over when it is the same item: Core::OnReplaced). Dropped as
   // gameplay ends -- a loading screen, the main menu, a menu, no player 3D
   // (InputHandler on the transition, Tick on its own check) -- and on
   // kPreLoadGame and ResetPipelineSubsystems. Not on a mere gap in the
   // hook's frames (an autosave or a cell crossing stalls it while gameplay
   // goes on).
   //
   // A weapon swap half done (the other hand's unequip queued, the equip
   // waiting a frame for it: EquipWeapon's retry) is finished, not dropped,
   // when gameplay pauses -- dropping it would leave the weapon in neither
   // hand -- and a newer press waits behind it (one deep) instead of
   // replacing it. Its second half waits for a casting hand like any equip;
   // past kActiveWaitMaxMs it is dropped, the weapon left unequipped. Only a
   // load or a reset (Drop's `force`) abandons it at once.
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
      uint16_t uniqueID = 0;             ///< the weapon or shield stack (ExtraUniqueID); 0 = the engine picks
      std::string name;
      bool occupiesBoth = false;         ///< a two-hander, bow, crossbow or both-hands spell
      bool selected = false;             ///< this press opened its selection (SelectionTracker::Select): withdrawn if the equip never lands
      bool selectionHeld = false;        ///< SelectionTracker::GateHold held a record for the form (released when it leaves)
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
   /// screen, the main menu, a pausing menu, a menu that takes the controls
   /// (the favourites menu, dialogue, crafting: Core::Needs::MenuBlocksGameplay
   /// on each open menu's flags), no player 3D (or no UI yet). Read on the
   /// input sink and in Tick.
   [[nodiscard]] std::string_view SlotKeysBlockedBy(RE::PlayerCharacter* player);

   class HandEquipGate
   {
   public:
      static HandEquipGate& GetSingleton();

      /// The PlayerCharacter::Update hook is installed (InstallHooks).
      void SetDriven(bool driven) noexcept { m_driven.store(driven, std::memory_order_release); }
      [[nodiscard]] bool IsDriven() const noexcept { return m_driven.load(std::memory_order_acquire); }

      /// Park one hand equip. A pending one is replaced (a weapon swap half
      /// done is not: the new press waits behind it). Logged at debug.
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
         Core::HandEquipWait wait;         ///< on the game clock
         bool busySeen = false;            ///< a checked hand was busy at least once (this half)
         std::uint8_t firstBusyHands = 0;  ///< the first busy reading, for the line when it goes
         std::uint32_t firstRightState = 0;
         std::uint32_t firstLeftState = 0;
         bool lockMissLogged = false;      ///< the "update tick holds the mutex" line, once per press
      };

      /// The request acting right now (taken out of m_pending), for a press
      /// of the same item landing meanwhile: it takes the selection over.
      struct Acting
      {
         RE::FormID formID = 0;
         bool selected = false;
         bool handedOver = false;
      };

      /// Take the pending equip out if it is still press `seq`; false when a
      /// newer press replaced it or it was dropped. `acting`: mark it the
      /// acting request.
      bool TakeIfCurrent(std::uint64_t seq, Pending& out, bool acting);
      /// Put a weapon swap's retry back (ahead of a newer press, which waits
      /// behind it), unless a forced Drop ran since `forceGen` was read.
      void Repark(Pending pending, EquipHand hand, std::uint64_t forceGen);
      /// Act on `taken` (taken out already) and handle what the act returns.
      void Act(Pending taken, double waitedMs, const char* how, std::uint64_t forceGen);
      /// A press queued behind a finished (or dropped) swap becomes the
      /// pending one, its wait starting now. Under m_mutex.
      void PromoteQueuedLocked();
      /// A request leaving without its equip: withdraw the selection it
      /// opened (`withdraw`, and not handed over), or else release its hold.
      static void LeaveGate(const HandEquipRequest& req, bool withdraw, std::string_view why);
      static std::string Describe(const HandEquipRequest& req);
      /// The game clock in ms (DropAheadProbe::GameSeconds).
      static double GameMs();

      std::mutex m_mutex;
      std::optional<Pending> m_pending;   // under m_mutex
      std::optional<Pending> m_queued;    // under m_mutex: a press behind a swap's second half
      std::optional<Acting> m_acting;     // under m_mutex
      std::uint64_t m_nextSeq = 1;        // under m_mutex
      std::uint64_t m_forceGen = 0;       // under m_mutex: bumped by every forced Drop
      std::atomic<bool> m_hasPending{ false };   // Tick's fast path; written under m_mutex
      std::atomic<bool> m_driven{ false };
   };
}
