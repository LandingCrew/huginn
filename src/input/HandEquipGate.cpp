#include "HandEquipGate.h"
#include "EquipManager.h"
#include "core/LapHoldInputs.h"  // MenuBlocksGameplay: a menu that takes the controls
#include "learning/SelectionTracker.h"
#include "slot/Remembrance.h"
#include "state/DropAheadProbe.h"  // GameSeconds: the game clock the waits run on
#include "update/UpdateHandler.h"

#include <format>
#include <utility>

namespace Huginn::Input
{
   namespace
   {
      double MsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
      {
         return std::chrono::duration<double, std::milli>(to - from).count();
      }

      std::uint32_t CasterState(RE::PlayerCharacter* player, RE::MagicSystem::CastingSource source)
      {
         // Main thread only: GetMagicCaster may create the caster on first use.
         const auto* caster = player->GetMagicCaster(source);
         return caster ? static_cast<std::uint32_t>(caster->state.get()) : 0;
      }

      std::string_view HandName(EquipHand hand)
      {
         switch (hand) {
         case EquipHand::Right: return "right";
         case EquipHand::Left: return "left";
         case EquipHand::Both: return "both";
         }
         return "?";
      }

      // The hands of a reading, each with its caster state; `prefix` before
      // each state ("still " for a drop).
      std::string HandsText(std::uint8_t hands, std::uint32_t right, std::uint32_t left, std::string_view prefix = {})
      {
         std::string out;
         if (hands & Core::kHandRight) out = std::format("right hand {}{}", prefix, Core::CasterStateName(right));
         if (hands & Core::kHandLeft) {
            if (!out.empty()) out += ", ";
            out += std::format("left hand {}{}", prefix, Core::CasterStateName(left));
         }
         return out.empty() ? std::string("no hand") : out;
      }

      // Held in both hands: a two-handed weapon, a bow or crossbow, a
      // both-hands spell.
      bool OccupiesBothHands(const RE::TESForm* form)
      {
         if (!form) return false;
         if (const auto* weapon = form->As<RE::TESObjectWEAP>()) {
            return weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow();
         }
         if (const auto* spell = form->As<RE::SpellItem>()) {
            return spell->IsTwoHanded();
         }
         return false;
      }

      // The hands to wait for this frame (Core::HandsToCheck). Read every
      // frame: what the hands hold can change while the equip waits. A
      // torch and a shield go to the left hand, so they check both (a
      // two-hander or two-handed spell held leaves both hands for them).
      std::uint8_t HandsFor(RE::PlayerCharacter* player, const HandEquipRequest& req)
      {
         const bool leftItem = req.type == UI::SlotContentType::Torch || req.type == UI::SlotContentType::Apparel;
         const auto target = (leftItem || req.hand == EquipHand::Left) ? Core::EquipTarget::Left
                           : req.hand == EquipHand::Both                ? Core::EquipTarget::Both
                                                                        : Core::EquipTarget::Right;
         bool otherHandMoves = false;
         if (target == Core::EquipTarget::Right && !req.occupiesBoth) {
            const auto* right = player->GetEquippedObject(false);
            const auto* left = player->GetEquippedObject(true);
            // The same form in the left hand is taken off it first
            // (EquipWeapon); an item held in both hands leaves both.
            otherHandMoves = (left && left->GetFormID() == req.formID) || OccupiesBothHands(right) ||
                             OccupiesBothHands(left);
         }
         return Core::HandsToCheck(target, req.occupiesBoth, otherHandMoves);
      }

      std::int64_t SteadyTicks(std::chrono::steady_clock::time_point t)
      {
         return t.time_since_epoch().count();
      }
   }

   std::string_view SlotKeysBlockedBy(RE::PlayerCharacter* player)
   {
      // The same reads as UpdateLoop's IsWorldLoaded, plus the menus: the
      // inventory, magic and favourites menus use the number row for the
      // favourites hotkeys, and the favourites menu does not pause the game
      // (HudVisibilityManager), so the pause alone would leave the keys live
      // there. A menu that takes the controls is read on the main thread
      // (ObserveMenus) and only its published result is read here: walking
      // the UI's menus from this thread would race their destruction.
      auto* ui = RE::UI::GetSingleton();  // IsMenuOpen is non-const
      if (!ui) return "no UI yet";
      if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return "loading screen";
      if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) return "main menu";
      if (ui->GameIsPaused()) return "game paused (a menu is open)";
      if (HandEquipGate::GetSingleton().BlockingMenuName()) return kMenuHasTheControls;
      if (!player || !player->Get3D()) return "no player 3D";
      return {};
   }

   HandEquipGate& HandEquipGate::GetSingleton()
   {
      static HandEquipGate singleton;
      return singleton;
   }

   double HandEquipGate::GameMs()
   {
      return State::DropAheadProbe::GameSeconds() * 1000.0;
   }

   std::string HandEquipGate::Describe(const HandEquipRequest& req)
   {
      return std::format("key {} '{}' ({:08X}) -> {} hand", req.slotIndex + 1, req.name, req.formID, HandName(req.hand));
   }

   void HandEquipGate::LeaveGate(const HandEquipRequest& req, bool withdraw, std::string_view why)
   {
      auto& tracker = Learning::SelectionTracker::GetSingleton();
      if (withdraw && req.selected) {
         tracker.Withdraw(req.formID, why);   // the record goes, its holds with it
      } else if (req.selectionHeld) {
         tracker.GateRelease(req.formID, /*retime=*/false);
      }
   }

   void HandEquipGate::ObserveMenus()
   {
      // Main thread: the UI creates and destroys its menus here. The
      // favourites menu by name (it does not pause), then any open menu
      // whose own flags and input context take the controls
      // (Core::Needs::MenuBlocksGameplay, the rule the v3 log's lap clock
      // uses: in the LoreRim logs HUD widgets, TrueHUD, QuickLoot read as
      // overlays, Dialogue and Crafting as blocking). These menus do not
      // pause, so the hook runs while they are open.
      const char* found = nullptr;
      if (auto* ui = RE::UI::GetSingleton()) {
         if (ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME)) {
            found = RE::FavoritesMenu::MENU_NAME.data();
         } else {
            for (const auto& [name, entry] : ui->menuMap) {
               const RE::IMenu* menu = entry.menu.get();
               if (!menu || !menu->OnStack()) continue;
               if (Core::Needs::MenuBlocksGameplay(menu->menuFlags.underlying(), menu->inputContext.underlying())) {
                  found = name.c_str();   // the menu map's key: lives as long as the registration
                  break;
               }
            }
         }
      }
      m_blockingMenu.store(found, std::memory_order_release);
   }

   bool HandEquipGate::UseGate()
   {
      if (!IsDriven()) return false;
      const auto now = std::chrono::steady_clock::now();
      const auto lastTick = std::chrono::steady_clock::time_point(
         std::chrono::steady_clock::duration(m_lastTickTicks.load(std::memory_order_acquire)));
      bool stalledNow = false;
      {
         std::lock_guard lock(m_mutex);
         // A press parked kStallAfter ago and no Tick since: the hook is not
         // running though the game is (this is asked from a live key press).
         stalledNow = m_pending && now - m_pending->req.parkedAt >= kStallAfter && lastTick < m_pending->req.parkedAt;
      }
      if (stalledNow && !m_stalled.exchange(true, std::memory_order_acq_rel)) {
         logger::error("[HandEquip] the PlayerCharacter::Update hook has not run for {} s with an equip parked "
                       "(another plugin may have replaced it): slot keys equip inline, without waiting for a "
                       "casting hand, until it runs again"sv,
            kStallAfter.count());
         Drop("the hand equip gate's hook stopped", /*force=*/true);
      }
      return !m_stalled.load(std::memory_order_acquire);
   }

   void HandEquipGate::Park(HandEquipRequest request)
   {
      // Its form's pending selection is not decided while the equip waits.
      request.selectionHeld = Learning::SelectionTracker::GetSingleton().GateHold(request.formID);

      std::optional<HandEquipRequest> replaced;
      Core::ReplacedSelection selection = Core::ReplacedSelection::None;
      bool refused = false;
      {
         std::lock_guard lock(m_mutex);
         if (m_pending && m_pending->req.swapInFlight) {
            refused = true;   // a weapon swap's second half is never replaced
         } else {
            if (m_pending) {
               replaced = std::move(m_pending->req);
               selection = Core::OnReplaced(replaced->formID, replaced->selected, request.formID);
               // The same item pressed again: SelectionTracker::Select found the
               // first press's record pending and opened none, so this press
               // takes that record over (Core::OnReplaced).
               if (selection == Core::ReplacedSelection::HandOver) request.selected = true;
            }
            Pending p;
            p.req = std::move(request);
            p.seq = m_nextSeq++;
            p.wait = Core::HandEquipWait(GameMs());
            logger::debug("[HandEquip] parked {}; {:.0f} ms after the key went down"sv, Describe(p.req),
               MsBetween(p.req.pressedAt, p.req.parkedAt));
            m_pending = std::move(p);
            m_hasPending.store(true, std::memory_order_release);
         }
      }
      if (refused) {
         logger::info("[HandEquip] {} dropped: a weapon swap is half done and goes first"sv, Describe(request));
         LeaveGate(request, true, "dropped: a weapon swap was half done");
         return;
      }
      if (replaced) {
         logger::info("[HandEquip] {} replaced by a newer press before it went ({:.0f} ms pending){}"sv,
            Describe(*replaced), MsBetween(replaced->parkedAt, std::chrono::steady_clock::now()),
            selection == Core::ReplacedSelection::HandOver ? "; the same item: its selection carries over" : "");
         LeaveGate(*replaced, selection == Core::ReplacedSelection::Withdraw,
            "replaced by a newer press before its equip went");
      }
   }

   void HandEquipGate::Drop(std::string_view why, bool force)
   {
      std::optional<HandEquipRequest> dropped;
      {
         std::lock_guard lock(m_mutex);
         if (force) ++m_forceGen;
         if (!m_pending) return;
         if (m_pending->req.swapInFlight && !force) {
            logger::debug("[HandEquip] {} kept through '{}': its weapon swap is half done"sv,
               Describe(m_pending->req), why);
            return;
         }
         dropped = std::move(m_pending->req);
         m_pending.reset();
         m_hasPending.store(false, std::memory_order_release);
      }
      logger::info("[HandEquip] {} dropped after {:.0f} ms pending: {}{}"sv, Describe(*dropped),
         MsBetween(dropped->parkedAt, std::chrono::steady_clock::now()), why,
         dropped->swapInFlight ? "; the weapon was left unequipped (the swap's first half had taken it off the other hand)"
                               : "");
      LeaveGate(*dropped, true, std::format("its equip was dropped ({})", why));
   }

   bool HandEquipGate::TakeIfCurrent(std::uint64_t seq, Pending& out)
   {
      std::lock_guard lock(m_mutex);
      if (!m_pending || m_pending->seq != seq) return false;
      out = std::move(*m_pending);
      m_pending.reset();
      m_hasPending.store(false, std::memory_order_release);
      return true;
   }

   void HandEquipGate::Repark(Pending pending, EquipHand hand, std::uint64_t forceGen)
   {
      pending.req.hand = hand;
      pending.req.swapInFlight = true;
      ++pending.req.handSwapAttempt;
      // The second half is a new wait of its own (and logs its own hold, if any).
      pending.busySeen = false;
      pending.lockMissLogged = false;
      pending.wait = Core::HandEquipWait(GameMs());
      std::optional<HandEquipRequest> displaced;
      bool lost = false;
      {
         std::lock_guard lock(m_mutex);
         lost = m_forceGen != forceGen;
         if (!lost) {
            // A press parked while the first half acted: the swap goes first.
            if (m_pending) displaced = std::move(m_pending->req);
            pending.seq = m_nextSeq++;
            logger::debug("[HandEquip] {}: the other hand's unequip not applied yet, retrying next frame (attempt {})"sv,
               Describe(pending.req), pending.req.handSwapAttempt + 1);
            m_pending = std::move(pending);
            m_hasPending.store(true, std::memory_order_release);
         }
      }
      if (displaced) {
         logger::info("[HandEquip] {} dropped: a weapon swap is half done and goes first"sv, Describe(*displaced));
         LeaveGate(*displaced, true, "dropped: a weapon swap was half done");
      }
      if (lost) {
         logger::info("[HandEquip] {}: a load or reset came during its weapon swap; the weapon was left unequipped"sv,
            Describe(pending.req));
         LeaveGate(pending.req, true, "a load or reset came during its weapon swap");
      }
   }

   void HandEquipGate::Act(Pending taken, double waitedMs, const char* how, std::uint64_t forceGen)
   {
      // Whatever way this returns -- an exception from the engine calls
      // included (the hook catches it) -- the request does not keep its
      // selection held: on any exit that is not handled below, it leaves
      // the gate as a failed equip.
      struct LeaveOnUnwind
      {
         const HandEquipRequest* req = nullptr;
         bool armed = true;
         ~LeaveOnUnwind()
         {
            if (!armed || !req) return;
            try {
               LeaveGate(*req, true, "its equip raised an exception");
            } catch (...) {
            }
         }
      } guard{ &taken.req };

      const auto now = std::chrono::steady_clock::now();
      // Once per wait: the second half of a weapon swap reports only a hold of
      // its own (Repark clears busySeen).
      if (taken.busySeen) {
         logger::info("[HandEquip] {} went after {:.0f} ms held (game clock; {:.0f} ms after the key went down): "
                      "waited for {}; {}"sv,
            Describe(taken.req), waitedMs, MsBetween(taken.req.pressedAt, now),
            HandsText(taken.firstBusyHands, taken.firstRightState, taken.firstLeftState), how);
         // The Remembrance capture was timed from the press, up to 3 s ago:
         // time it from this equip (never for an undelayed press).
         if (taken.req.remembers) {
            Slot::Remembrance::GetSingleton().RearmCapture(taken.req.page, taken.req.slotIndex, taken.req.formID,
               Slot::Remembrance::Kind::Hand);
         }
      }
      const HandEquipResult result = EquipManager::GetSingleton().ActOnHandEquip(taken.req);
      guard.armed = false;
      if (!result.success) {
         // A press of the same item parked while this acted opened no record
         // of its own (Select found this one pending): it inherits this one
         // rather than lose it with the failure.
         bool handedOver = false;
         if (taken.req.selected) {
            std::lock_guard lock(m_mutex);
            if (m_pending && m_pending->req.formID == taken.req.formID && !m_pending->req.selected) {
               m_pending->req.selected = true;
               handedOver = true;
            }
         }
         LeaveGate(taken.req, !handedOver, "its equip failed");
      } else if (result.retry) {
         Repark(std::move(taken), *result.retry, forceGen);
      } else if (taken.req.selectionHeld) {
         // Equipped: the selection may be decided again, its confirm window
         // from now (core/SelectionGate.h).
         Learning::SelectionTracker::GetSingleton().GateRelease(taken.req.formID, /*retime=*/true);
      }
   }

   void HandEquipGate::Tick(RE::PlayerCharacter* player)
   {
      m_lastTickTicks.store(SteadyTicks(std::chrono::steady_clock::now()), std::memory_order_release);
      if (m_stalled.load(std::memory_order_acquire) && m_stalled.exchange(false, std::memory_order_acq_rel)) {
         logger::info("[HandEquip] the PlayerCharacter::Update hook runs again: slot keys wait for a casting hand again"sv);
      }
      if (!m_hasPending.load(std::memory_order_acquire) || !player) return;

      Pending p;
      std::uint64_t forceGen = 0;
      {
         std::lock_guard lock(m_mutex);
         if (!m_pending) return;
         p = *m_pending;
         forceGen = m_forceGen;
      }
      const std::string_view blocked = SlotKeysBlockedBy(player);
      // A weapon swap's second half is kept through a pause or a menu (a load
      // or a reset removes it through a forced Drop): it waits for gameplay.
      if (p.req.swapInFlight && !blocked.empty()) return;

      Core::HandEquipGateInput in;
      in.worldReady = blocked.empty();
      in.nowMs = GameMs();
      if (in.worldReady) {
         in.hands = HandsFor(player, p.req);
         in.rightState = CasterState(player, RE::MagicSystem::CastingSource::kRightHand);
         in.leftState = CasterState(player, RE::MagicSystem::CastingSource::kLeftHand);
      }
      const auto decision = p.wait.Update(in);
      const double waitedMs = p.wait.WaitedMs(in.nowMs);

      // The act, under the update loop's mutex: never blocking the main
      // thread on a tick running elsewhere (a busy mutex: next frame).
      auto actNow = [&](const char* how) {
         const bool ran = Update::UpdateHandler::GetSingleton()->TryRunExclusive([&] {
            Pending taken;
            if (TakeIfCurrent(p.seq, taken)) {
               Act(std::move(taken), waitedMs, how, forceGen);
            }
         });
         if (!ran) {
            std::lock_guard lock(m_mutex);
            if (m_pending && m_pending->seq == p.seq) {
               m_pending->wait = p.wait;  // the transient clock, kept for the next frame
               if (!m_pending->lockMissLogged) {
                  m_pending->lockMissLogged = true;
                  logger::debug("[HandEquip] {}: an update tick holds the update mutex; equipping on a later frame"sv,
                     Describe(p.req));
               }
            }
         }
      };

      switch (decision.action) {
      case Core::HandEquipAction::Drop: {
         Pending gone;
         if (!TakeIfCurrent(p.seq, gone)) return;
         const std::string why = decision.why == Core::HandEquipWhy::StillCasting
            ? std::format("{} after {:.1f} s", HandsText(decision.activeHands, in.rightState, in.leftState, "still "),
                 waitedMs / 1000.0)
            : std::string(blocked);
         logger::info("[HandEquip] {} dropped after {:.0f} ms pending (game clock): {}{}"sv, Describe(gone.req),
            waitedMs, why,
            gone.req.swapInFlight ? "; the weapon was left unequipped (the swap's first half had taken it off the other hand)"
                                  : "");
         LeaveGate(gone.req, true, std::format("its equip was dropped ({})", why));
         return;
      }
      case Core::HandEquipAction::Wait: {
         std::lock_guard lock(m_mutex);
         if (!m_pending || m_pending->seq != p.seq) return;  // replaced or dropped meanwhile
         m_pending->wait = p.wait;  // the transient clock
         if (!m_pending->busySeen) {
            m_pending->busySeen = true;
            m_pending->firstBusyHands = decision.busyHands;
            m_pending->firstRightState = in.rightState;
            m_pending->firstLeftState = in.leftState;
            logger::debug("[HandEquip] {} held: {} (checking the {} hand{})"sv, Describe(p.req),
               HandsText(decision.busyHands, in.rightState, in.leftState), Core::HandMaskName(in.hands),
               in.hands == Core::kHandsBoth ? "s" : "");
         }
         return;
      }
      case Core::HandEquipAction::EquipNow:
         break;
      }

      if (decision.why == Core::HandEquipWhy::ThroughTransient) {
         logger::debug("[HandEquip] {}: went ahead through {} after {:.0f} ms"sv, Describe(p.req),
            HandsText(decision.busyHands, in.rightState, in.leftState), Core::kTransientWaitMs);
         actNow("went ahead through a transient caster state");
      } else {
         actNow(p.req.swapInFlight ? "the weapon swap's second half" : "the hand came free");
      }
   }
}
