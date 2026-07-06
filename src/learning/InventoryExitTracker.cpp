#include "InventoryExitTracker.h"

namespace Huginn::Learning
{
   InventoryExitTracker& InventoryExitTracker::GetSingleton()
   {
      static InventoryExitTracker singleton;
      return singleton;
   }

   void InventoryExitTracker::Register()
   {
      if (m_registered.load(std::memory_order_acquire)) {
      logger::warn("[InventoryExitTracker] Already registered, skipping"sv);
      return;
      }

      auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
      if (!eventSource) {
      logger::error("[InventoryExitTracker] Failed to get ScriptEventSourceHolder"sv);
      return;
      }

      eventSource->AddEventSink<RE::TESContainerChangedEvent>(this);
      m_registered.store(true, std::memory_order_release);

      logger::info("[InventoryExitTracker] Registered for TESContainerChangedEvent (drop/sell/store vs consumption)"sv);
   }

   int32_t InventoryExitTracker::ClaimExits(RE::FormID formID, int32_t count)
   {
      if (count <= 0) {
      return 0;
      }

      std::lock_guard lock(m_mutex);

      auto it = m_pending.find(formID);
      if (it == m_pending.end()) {
      return 0;
      }

      if (std::chrono::steady_clock::now() - it->second.lastEvent > ENTRY_TTL) {
      m_pending.erase(it);
      return 0;
      }

      const int32_t claimed = std::min(it->second.count, count);
      it->second.count -= claimed;
      if (it->second.count == 0) {
      m_pending.erase(it);
      }

      return claimed;
   }

   RE::BSEventNotifyControl InventoryExitTracker::ProcessEvent(
      const RE::TESContainerChangedEvent* event,
      RE::BSTEventSource<RE::TESContainerChangedEvent>* /*source*/)
   {
      if (!event || event->itemCount <= 0 || event->baseObj == 0) {
      return RE::BSEventNotifyControl::kContinue;
      }

      // Only exits from the player's inventory
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player || event->oldContainer != player->GetFormID()) {
      return RE::BSEventNotifyControl::kContinue;
      }

      // Consuming/casting removes the item with no destination: no target
      // container and no dropped world reference. Everything else (drop, sell,
      // store, give) is a transfer and must not earn a consumption reward.
      const bool hasDestination = event->newContainer != 0;
      const bool droppedIntoWorld = static_cast<bool>(event->reference);
      if (!hasDestination && !droppedIntoWorld) {
      return RE::BSEventNotifyControl::kContinue;
      }

      {
      std::lock_guard lock(m_mutex);

      const auto now = std::chrono::steady_clock::now();

      // Opportunistic sweep of expired never-claimed entries
      if (m_pending.size() >= PURGE_THRESHOLD) {
        std::erase_if(m_pending, [&](const auto& kv) {
           return now - kv.second.lastEvent > ENTRY_TTL;
        });
      }

      auto& entry = m_pending[event->baseObj];
      entry.count += event->itemCount;
      entry.lastEvent = now;
      }

      logger::trace("[InventoryExitTracker] Non-consumption exit: {:08X} x{} (dest={:08X}, worldRef={})"sv,
      event->baseObj, event->itemCount, event->newContainer, droppedIntoWorld);

      return RE::BSEventNotifyControl::kContinue;
   }
}
