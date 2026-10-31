#include "HandEquipGate.h"
#include "EquipManager.h"
#include "learning/SelectionTracker.h"
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

      // The busy hands of a reading, each with its caster state.
      std::string BusyText(std::uint8_t hands, std::uint32_t right, std::uint32_t left)
      {
         std::string out;
         if (hands & Core::kHandRight) out = std::format("right hand {}", Core::CasterStateName(right));
         if (hands & Core::kHandLeft) {
            if (!out.empty()) out += ", ";
            out += std::format("left hand {}", Core::CasterStateName(left));
         }
         return out.empty() ? std::string("no hand") : out;
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
      {
         std::lock_guard lock(m_mutex);
         if (m_pending) replaced = std::move(m_pending->req);
         Pending p;
         p.req = std::move(request);
         p.seq = m_nextSeq++;
         // A torch goes to the left hand whatever was pressed (EquipTorch).
         const auto target = p.req.type == UI::SlotContentType::Torch ? Core::EquipTarget::Left
                           : p.req.hand == EquipHand::Left            ? Core::EquipTarget::Left
                           : p.req.hand == EquipHand::Both            ? Core::EquipTarget::Both
                                                                       : Core::EquipTarget::Right;
         p.hands = Core::HandsToCheck(target, p.req.occupiesBoth);
         logger::debug("[HandEquip] parked {} (checks the {} hand{}); {:.0f} ms after the key went down"sv,
            Describe(p.req), Core::HandMaskName(p.hands), p.hands == Core::kHandsBoth ? "s" : "",
            MsBetween(p.req.pressedAt, p.req.parkedAt));
         m_pending = std::move(p);
         m_hasPending.store(true, std::memory_order_release);
      }
      if (replaced) {
         logger::info("[HandEquip] {} replaced by a newer press before it went ({:.0f} ms pending)"sv,
            Describe(*replaced), MsBetween(replaced->parkedAt, std::chrono::steady_clock::now()));
         WithdrawSelection(*replaced, "replaced by a newer press before its equip went");
      }
   }

   void HandEquipGate::Drop(std::string_view why)
   {
      std::optional<HandEquipRequest> dropped;
      {
         std::lock_guard lock(m_mutex);
         if (!m_pending) return;
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

   void HandEquipGate::Repark(Pending pending, EquipHand hand)
   {
      pending.req.hand = hand;
      ++pending.req.handSwapAttempt;
      {
         std::lock_guard lock(m_mutex);
         if (!m_pending) {
            const auto target = hand == EquipHand::Left ? Core::EquipTarget::Left
                              : hand == EquipHand::Both ? Core::EquipTarget::Both
                                                        : Core::EquipTarget::Right;
            pending.hands = Core::HandsToCheck(target, pending.req.occupiesBoth);
            pending.seq = m_nextSeq++;
            pending.lockMissLogged = false;
            logger::debug("[HandEquip] {}: the other hand's unequip not applied yet, retrying next frame (attempt {})"sv,
               Describe(pending.req), pending.req.handSwapAttempt + 1);
            m_pending = std::move(pending);
            m_hasPending.store(true, std::memory_order_release);
            return;
         }
      }
      logger::info("[HandEquip] {}: a newer press arrived during its weapon swap; the swap is not finished"sv,
         Describe(pending.req));
      WithdrawSelection(pending.req, "replaced by a newer press during its weapon swap");
   }

   void HandEquipGate::Tick(RE::PlayerCharacter* player)
   {
      const auto now = std::chrono::steady_clock::now();
      const auto lastTick = std::exchange(m_lastTick, now);
      if (!m_hasPending.load(std::memory_order_acquire) || !player) return;

      Pending p;
      {
         std::lock_guard lock(m_mutex);
         if (!m_pending) return;
         p = *m_pending;
      }

      // The hook does not run while the game is paused or loading, so this
      // may be the first frame after one. Parked before the gap: drop it, as
      // InputHandler would have on the transition had an input event come.
      const double gapMs = MsBetween(lastTick, now);
      if (lastTick.time_since_epoch().count() != 0 && gapMs > kGapMs && p.req.parkedAt < lastTick) {
         Pending gone;
         if (TakeIfCurrent(p.seq, gone)) {
            logger::info("[HandEquip] {} dropped: the game did not update for {:.0f} ms while it waited (a pause or a load)"sv,
               Describe(gone.req), gapMs);
            WithdrawSelection(gone.req, "its equip was dropped (the game paused or loaded while it waited)");
         }
         return;
      }

      const std::string_view blocked = SlotKeysBlockedBy(player);
      Core::HandEquipGateInput in;
      in.worldReady = blocked.empty();
      in.hands = p.hands;
      if (in.worldReady) {
         in.rightState = CasterState(player, RE::MagicSystem::CastingSource::kRightHand);
         in.leftState = CasterState(player, RE::MagicSystem::CastingSource::kLeftHand);
      }
      in.waitedMs = MsBetween(p.req.parkedAt, now);
      in.interrupted = p.interrupted;
      const auto decision = Core::DecideHandEquip(in);

      switch (decision.action) {
      case Core::HandEquipAction::Drop: {
         Pending gone;
         if (TakeIfCurrent(p.seq, gone)) {
            logger::info("[HandEquip] {} dropped after {:.0f} ms pending: {}"sv, Describe(gone.req), in.waitedMs, blocked);
            WithdrawSelection(gone.req, std::format("its equip was dropped ({})", blocked));
         }
         return;
      }
      case Core::HandEquipAction::Wait:
      case Core::HandEquipAction::InterruptThenEquip: {
         const bool interrupt = decision.action == Core::HandEquipAction::InterruptThenEquip;
         {
            std::lock_guard lock(m_mutex);
            if (!m_pending || m_pending->seq != p.seq) return;  // replaced or dropped meanwhile
            if (!m_pending->busySeen) {
               m_pending->busySeen = true;
               m_pending->firstBusyHands = decision.busyHands;
               m_pending->firstRightState = in.rightState;
               m_pending->firstLeftState = in.leftState;
               logger::debug("[HandEquip] {} held: {} busy"sv, Describe(p.req),
                  BusyText(decision.busyHands, in.rightState, in.leftState));
            }
            if (interrupt) m_pending->interrupted = true;
         }
         if (interrupt) {
            // Restores the magicka the charge took, as the swap itself did at
            // 00:38 (the refund at 21.293); the equip goes on the next frame.
            player->InterruptCast(true);
            logger::debug("[HandEquip] {}: {} still busy after {:.0f} ms; cast interrupted, equipping next frame"sv,
               Describe(p.req), BusyText(decision.busyHands, in.rightState, in.leftState), in.waitedMs);
         }
         return;
      }
      case Core::HandEquipAction::EquipNow:
         break;
      }

      // The act, under the update loop's mutex: never blocking the main
      // thread on a tick running elsewhere (a busy mutex: next frame).
      Pending taken;
      bool superseded = false;
      std::optional<EquipHand> retry;
      const bool ran = Update::UpdateHandler::GetSingleton()->TryRunExclusive([&] {
         if (!TakeIfCurrent(p.seq, taken)) {
            superseded = true;
            return;
         }
         if (taken.busySeen) {
            logger::info("[HandEquip] {} went after {:.0f} ms held ({:.0f} ms after the key went down): waited for {}{}"sv,
               Describe(taken.req), in.waitedMs, MsBetween(taken.req.pressedAt, now),
               BusyText(taken.firstBusyHands, taken.firstRightState, taken.firstLeftState),
               taken.interrupted ? std::format("; still busy at {:.0f} ms, so the cast was interrupted", Core::kHandBusyTimeoutMs)
                                 : std::string("; no interrupt (the hand came free)"));
         }
         const HandEquipResult result = EquipManager::GetSingleton().ActOnHandEquip(taken.req);
         if (!result.success) {
            WithdrawSelection(taken.req, "its equip failed");
         } else if (result.retry) {
            retry = result.retry;
         }
      });
      if (!ran) {
         std::lock_guard lock(m_mutex);
         if (m_pending && m_pending->seq == p.seq && !m_pending->lockMissLogged) {
            m_pending->lockMissLogged = true;
            logger::debug("[HandEquip] {}: an update tick holds the update mutex; equipping on a later frame"sv,
               Describe(p.req));
         }
         return;
      }
      if (!superseded && retry) {
         Repark(std::move(taken), *retry);
      }
   }
}
