#include "HandEquipGate.h"
#include "EquipManager.h"
#include "learning/SelectionTracker.h"
#include "slot/Remembrance.h"
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

      /// The clock HandEquipWait is fed: steady milliseconds (any epoch).
      double SteadyMs(std::chrono::steady_clock::time_point t)
      {
         return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
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

      // The hands of a reading, each with its caster state.
      std::string HandsText(std::uint8_t hands, std::uint32_t right, std::uint32_t left)
      {
         std::string out;
         if (hands & Core::kHandRight) out = std::format("right hand {}", Core::CasterStateName(right));
         if (hands & Core::kHandLeft) {
            if (!out.empty()) out += ", ";
            out += std::format("left hand {}", Core::CasterStateName(left));
         }
         return out.empty() ? std::string("no hand") : out;
      }

      // "right hand still charging", for the hands a cast still holds.
      std::string StillText(std::uint8_t hands, std::uint32_t right, std::uint32_t left)
      {
         std::string out;
         if (hands & Core::kHandRight) out = std::format("right hand still {}", Core::CasterStateName(right));
         if (hands & Core::kHandLeft) {
            if (!out.empty()) out += ", ";
            out += std::format("left hand still {}", Core::CasterStateName(left));
         }
         return out;
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
      // frame: what the hands hold can change while the equip waits.
      std::uint8_t HandsFor(RE::PlayerCharacter* player, const HandEquipRequest& req)
      {
         const auto target = (req.type == UI::SlotContentType::Torch || req.hand == EquipHand::Left)
                               ? Core::EquipTarget::Left
                           : req.hand == EquipHand::Both ? Core::EquipTarget::Both
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
   }

   std::string_view SlotKeysBlockedBy(RE::PlayerCharacter* player)
   {
      // The same reads as UpdateLoop's IsWorldLoaded, plus the pause: the
      // inventory, magic and favourites menus pause the game, and the number
      // keys are theirs there (favourites hotkeys).
      auto* ui = RE::UI::GetSingleton();  // IsMenuOpen is non-const
      if (!ui) return "no UI yet";
      if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return "loading screen";
      if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) return "main menu";
      if (ui->GameIsPaused()) return "game paused (a menu is open)";
      if (!player || !player->Get3D()) return "no player 3D";
      return {};
   }

   HandEquipGate& HandEquipGate::GetSingleton()
   {
      static HandEquipGate singleton;
      return singleton;
   }

   std::string HandEquipGate::Describe(const HandEquipRequest& req)
   {
      return std::format("key {} '{}' ({:08X}) -> {} hand", req.slotIndex + 1, req.name, req.formID, HandName(req.hand));
   }

   void HandEquipGate::WithdrawSelection(const HandEquipRequest& req, std::string_view why)
   {
      if (req.selected) {
         Learning::SelectionTracker::GetSingleton().Withdraw(req.formID, why);
      }
   }

   void HandEquipGate::Park(HandEquipRequest request)
   {
      std::optional<HandEquipRequest> replaced;
      Core::ReplacedSelection selection = Core::ReplacedSelection::None;
      {
         std::lock_guard lock(m_mutex);
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
         p.wait = Core::HandEquipWait(SteadyMs(p.req.parkedAt));
         logger::debug("[HandEquip] parked {}; {:.0f} ms after the key went down"sv, Describe(p.req),
            MsBetween(p.req.pressedAt, p.req.parkedAt));
         m_pending = std::move(p);
         m_hasPending.store(true, std::memory_order_release);
      }
      if (replaced) {
         logger::info("[HandEquip] {} replaced by a newer press before it went ({:.0f} ms pending{}){}"sv,
            Describe(*replaced), MsBetween(replaced->parkedAt, std::chrono::steady_clock::now()),
            replaced->swapInFlight ? "; its weapon swap was half done" : "",
            selection == Core::ReplacedSelection::HandOver ? "; the same item: its selection carries over" : "");
         if (selection == Core::ReplacedSelection::Withdraw) {
            WithdrawSelection(*replaced, "replaced by a newer press before its equip went");
         }
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
      logger::info("[HandEquip] {} dropped after {:.0f} ms pending: {}"sv, Describe(*dropped),
         MsBetween(dropped->parkedAt, std::chrono::steady_clock::now()), why);
      WithdrawSelection(*dropped, std::format("its equip was dropped ({})", why));
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
      const char* lost = nullptr;
      {
         std::lock_guard lock(m_mutex);
         if (m_forceGen != forceGen) {
            lost = "a load or reset came during its weapon swap";
         } else if (m_pending) {
            lost = "a newer press arrived during its weapon swap";
         } else {
            pending.seq = m_nextSeq++;
            pending.lockMissLogged = false;
            logger::debug("[HandEquip] {}: the other hand's unequip not applied yet, retrying next frame (attempt {})"sv,
               Describe(pending.req), pending.req.handSwapAttempt + 1);
            m_pending = std::move(pending);
            m_hasPending.store(true, std::memory_order_release);
            return;
         }
      }
      logger::info("[HandEquip] {}: {}; the swap is not finished"sv, Describe(pending.req), lost);
      WithdrawSelection(pending.req, lost);
   }

   void HandEquipGate::Act(Pending taken, double waitedMs, const char* how, std::uint64_t forceGen)
   {
      const auto now = std::chrono::steady_clock::now();
      if (taken.busySeen) {
         logger::info("[HandEquip] {} went after {:.0f} ms held ({:.0f} ms after the key went down): waited for {}; {}"sv,
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
      if (!result.success) {
         WithdrawSelection(taken.req, "its equip failed");
      } else if (result.retry) {
         Repark(std::move(taken), *result.retry, forceGen);
      }
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
      const auto now = std::chrono::steady_clock::now();
      const std::string_view blocked = SlotKeysBlockedBy(player);

      // The act, under the update loop's mutex: never blocking the main
      // thread on a tick running elsewhere (a busy mutex: next frame).
      auto actNow = [&](const char* how) {
         const bool ran = Update::UpdateHandler::GetSingleton()->TryRunExclusive([&] {
            Pending taken;
            if (TakeIfCurrent(p.seq, taken)) {
               Act(std::move(taken), MsBetween(p.req.parkedAt, now), how, forceGen);
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

      // A weapon swap's second half: already past the gate, finished as soon
      // as gameplay runs (a load or a reset removes it through a forced Drop).
      if (p.req.swapInFlight) {
         if (blocked.empty()) actNow("the weapon swap's second half");
         return;
      }

      Core::HandEquipGateInput in;
      in.worldReady = blocked.empty();
      in.nowMs = SteadyMs(now);
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
         if (!TakeIfCurrent(p.seq, gone)) return;
         const std::string why = decision.why == Core::HandEquipWhy::StillCasting
            ? std::format("{} after {:.1f} s",
                 StillText(decision.activeHands, in.rightState, in.leftState), waitedMs / 1000.0)
            : std::string(blocked);
         logger::info("[HandEquip] {} dropped after {:.0f} ms pending: {}"sv, Describe(gone.req), waitedMs, why);
         WithdrawSelection(gone.req, std::format("its equip was dropped ({})", why));
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
         actNow("the hand came free");
      }
   }
}
