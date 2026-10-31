#include "HandEquipGate.h"
#include "EquipManager.h"
#include "core/LapHoldInputs.h"  // MenuBlocksGameplay: a menu that takes the controls
#include "learning/SelectionTracker.h"
#include "slot/Remembrance.h"
#include "state/DropAheadProbe.h"  // GameSeconds: the game clock the waits run on
#include "update/UpdateHandler.h"

#include <format>
#include <utility>
#include <vector>

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

      // An open menu that takes the controls: by its own flags and input
      // context (Core::Needs::MenuBlocksGameplay, the rule the v3 log's lap
      // clock uses -- the overlays in the LoreRim logs, HUD widgets, TrueHUD,
      // QuickLoot, read as overlays; Dialogue and Crafting as blocking). Read
      // as SelectionLogV3::BlockingMenus does, off the main thread in
      // gameplay (no job tick overlapped a main-thread zone in the traces).
      bool AMenuHasTheControls(RE::UI* ui)
      {
         for (const auto& [name, entry] : ui->menuMap) {
            const RE::IMenu* menu = entry.menu.get();
            if (!menu || !menu->OnStack()) continue;
            if (Core::Needs::MenuBlocksGameplay(menu->menuFlags.underlying(), menu->inputContext.underlying())) {
               return true;
            }
         }
         return false;
      }
   }

   std::string_view SlotKeysBlockedBy(RE::PlayerCharacter* player)
   {
      // The same reads as UpdateLoop's IsWorldLoaded, plus the menus: the
      // inventory, magic and favourites menus use the number row for the
      // favourites hotkeys, and the favourites menu does not pause the game
      // (HudVisibilityManager), so the pause alone would leave the keys live
      // there. Named first; any other menu that takes the controls by its
      // flags is caught by AMenuHasTheControls.
      auto* ui = RE::UI::GetSingleton();  // IsMenuOpen is non-const
      if (!ui) return "no UI yet";
      if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return "loading screen";
      if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) return "main menu";
      if (ui->GameIsPaused()) return "game paused (a menu is open)";
      if (ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME)) return "favorites menu open";
      if (AMenuHasTheControls(ui)) return "a menu has the controls";
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

   void HandEquipGate::PromoteQueuedLocked()
   {
      if (m_pending || !m_queued) return;
      m_pending = std::move(m_queued);
      m_queued.reset();
      m_pending->seq = m_nextSeq++;
      m_pending->wait = Core::HandEquipWait(GameMs());
      m_hasPending.store(true, std::memory_order_release);
      logger::debug("[HandEquip] {}: the weapon swap ahead of it is done; its wait starts now"sv, Describe(m_pending->req));
   }

   void HandEquipGate::Park(HandEquipRequest request)
   {
      // Its form's pending selection is not decided while the equip waits.
      request.selectionHeld = Learning::SelectionTracker::GetSingleton().GateHold(request.formID);

      std::optional<HandEquipRequest> replaced;
      Core::ReplacedSelection selection = Core::ReplacedSelection::None;
      bool queued = false;
      {
         std::lock_guard lock(m_mutex);
         // Behind a weapon swap half done, the newer press waits its turn
         // (one deep) rather than leave the weapon in neither hand.
         queued = m_pending && m_pending->req.swapInFlight;
         auto& slot = queued ? m_queued : m_pending;
         if (slot) {
            replaced = std::move(slot->req);
            selection = Core::OnReplaced(replaced->formID, replaced->selected, request.formID);
            // The same item pressed again: SelectionTracker::Select found the
            // first press's record pending and opened none, so this press
            // takes that record over (Core::OnReplaced).
            if (selection == Core::ReplacedSelection::HandOver) request.selected = true;
         }
         if (!request.selected) {
            // The same hand-over from a request no longer in the slot: the
            // swap ahead of this one, or the one acting right now.
            if (queued && m_pending->req.formID == request.formID && m_pending->req.selected) {
               m_pending->req.selected = false;
               request.selected = true;
            } else if (!queued && m_acting && m_acting->formID == request.formID && m_acting->selected &&
                       !m_acting->handedOver) {
               m_acting->handedOver = true;
               request.selected = true;
            }
         }
         Pending p;
         p.req = std::move(request);
         p.seq = m_nextSeq++;
         p.wait = Core::HandEquipWait(GameMs());
         logger::debug("[HandEquip] parked {}{}; {:.0f} ms after the key went down"sv, Describe(p.req),
            queued ? " behind a weapon swap half done" : "", MsBetween(p.req.pressedAt, p.req.parkedAt));
         slot = std::move(p);
         if (!queued) m_hasPending.store(true, std::memory_order_release);
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
      std::vector<HandEquipRequest> dropped;
      {
         std::lock_guard lock(m_mutex);
         if (force) ++m_forceGen;
         if (m_queued) {
            dropped.push_back(std::move(m_queued->req));
            m_queued.reset();
         }
         if (m_pending) {
            if (m_pending->req.swapInFlight && !force) {
               logger::debug("[HandEquip] {} kept through '{}': its weapon swap is half done"sv,
                  Describe(m_pending->req), why);
            } else {
               dropped.push_back(std::move(m_pending->req));
               m_pending.reset();
               m_hasPending.store(false, std::memory_order_release);
            }
         }
      }
      for (const auto& req : dropped) {
         logger::info("[HandEquip] {} dropped after {:.0f} ms pending: {}{}"sv, Describe(req),
            MsBetween(req.parkedAt, std::chrono::steady_clock::now()), why,
            req.swapInFlight ? "; the weapon was left unequipped (the swap's first half had taken it off the other hand)"
                             : "");
         LeaveGate(req, true, std::format("its equip was dropped ({})", why));
      }
   }

   bool HandEquipGate::TakeIfCurrent(std::uint64_t seq, Pending& out, bool acting)
   {
      std::lock_guard lock(m_mutex);
      if (!m_pending || m_pending->seq != seq) return false;
      out = std::move(*m_pending);
      m_pending.reset();
      m_hasPending.store(false, std::memory_order_release);
      if (acting) m_acting = Acting{ out.req.formID, out.req.selected, false };
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
            if (m_pending) {
               // A newer press parked while the first half acted: it waits
               // behind the swap now.
               if (m_queued) displaced = std::move(m_queued->req);
               m_queued = std::move(m_pending);
               m_pending.reset();
               if (m_queued->req.formID == pending.req.formID && !m_queued->req.selected && pending.req.selected) {
                  pending.req.selected = false;     // the same item: the newer press owns the selection
                  m_queued->req.selected = true;
               }
            }
            pending.seq = m_nextSeq++;
            logger::debug("[HandEquip] {}: the other hand's unequip not applied yet, retrying next frame (attempt {})"sv,
               Describe(pending.req), pending.req.handSwapAttempt + 1);
            m_pending = std::move(pending);
            m_hasPending.store(true, std::memory_order_release);
         } else {
            logger::info("[HandEquip] {}: a load or reset came during its weapon swap; the weapon was left unequipped"sv,
               Describe(pending.req));
         }
      }
      if (displaced) {
         LeaveGate(*displaced, true, "replaced while a weapon swap was half done");
      }
      if (lost) {
         LeaveGate(pending.req, true, "a load or reset came during its weapon swap");
      }
   }

   void HandEquipGate::Act(Pending taken, double waitedMs, const char* how, std::uint64_t forceGen)
   {
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
      bool handedOver = false;
      {
         std::lock_guard lock(m_mutex);
         handedOver = m_acting && m_acting->handedOver;
         m_acting.reset();
      }
      // A newer press of the same item took the selection while this acted:
      // it is that press's now, whatever happens to this one.
      if (handedOver) taken.req.selected = false;
      if (!result.success) {
         LeaveGate(taken.req, true, "its equip failed");
      } else if (result.retry) {
         Repark(std::move(taken), *result.retry, forceGen);
      } else if (taken.req.selectionHeld) {
         // Equipped: the selection may be decided again, its confirm window
         // from now (core/SelectionGate.h).
         Learning::SelectionTracker::GetSingleton().GateRelease(taken.req.formID, /*retime=*/true);
      }
      std::lock_guard lock(m_mutex);
      PromoteQueuedLocked();
   }

   void HandEquipGate::Tick(RE::PlayerCharacter* player)
   {
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

      // The act, under the update loop's mutex: never blocking the main
      // thread on a tick running elsewhere (a busy mutex: next frame).
      auto actNow = [&](const char* how, double waitedMs) {
         const bool ran = Update::UpdateHandler::GetSingleton()->TryRunExclusive([&] {
            Pending taken;
            if (TakeIfCurrent(p.seq, taken, /*acting=*/true)) {
               Act(std::move(taken), waitedMs, how, forceGen);
            }
         });
         if (!ran) {
            std::lock_guard lock(m_mutex);
            if (m_pending && m_pending->seq == p.seq && !m_pending->lockMissLogged) {
               m_pending->lockMissLogged = true;
               logger::debug("[HandEquip] {}: an update tick holds the update mutex; equipping on a later frame"sv,
                  Describe(p.req));
            }
         }
      };

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

      switch (decision.action) {
      case Core::HandEquipAction::Drop: {
         Pending gone;
         if (!TakeIfCurrent(p.seq, gone, /*acting=*/false)) return;
         const std::string why = decision.why == Core::HandEquipWhy::StillCasting
            ? std::format("{} after {:.1f} s", HandsText(decision.activeHands, in.rightState, in.leftState, "still "),
                 waitedMs / 1000.0)
            : std::string(blocked);
         logger::info("[HandEquip] {} dropped after {:.0f} ms pending (game clock): {}{}"sv, Describe(gone.req),
            waitedMs, why,
            gone.req.swapInFlight ? "; the weapon was left unequipped (the swap's first half had taken it off the other hand)"
                                  : "");
         LeaveGate(gone.req, true, std::format("its equip was dropped ({})", why));
         std::lock_guard lock(m_mutex);
         PromoteQueuedLocked();
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
         actNow("went ahead through a transient caster state", waitedMs);
      } else {
         actNow(p.req.swapInFlight ? "the weapon swap's second half" : "the hand came free", waitedMs);
      }
   }
}
