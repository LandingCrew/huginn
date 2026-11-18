#include "HostileSight.h"

#include "Profiling.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace Huginn::State::HostileSight
{
   namespace
   {
      constexpr auto kInterval = std::chrono::milliseconds(100);

      std::atomic<bool> g_gameLoaded{ false };
      std::mutex g_mutex;                      // guards the two lists below
      std::vector<std::uint32_t> g_requested;  // the poll's last request
      std::vector<std::uint32_t> g_seen;       // the hook's last answer: requested and in sight
      std::chrono::steady_clock::time_point g_lastPass{};   // the hook's thread only

      void ClearLists()
      {
         std::lock_guard lock(g_mutex);
         g_requested.clear();
         g_seen.clear();
      }
   }

   void Request(std::vector<std::uint32_t> formIDs)
   {
      std::lock_guard lock(g_mutex);
      g_requested = std::move(formIDs);
   }

   bool Sees(std::uint32_t formID)
   {
      std::lock_guard lock(g_mutex);
      return std::ranges::find(g_seen, formID) != g_seen.end();
   }

   void OnPlayerUpdate(RE::PlayerCharacter* player)
   {
      if (!g_gameLoaded.load(std::memory_order_acquire) || !player) return;
      const auto now = std::chrono::steady_clock::now();
      if (now - g_lastPass < kInterval) return;
      g_lastPass = now;
      Huginn_ZONE_NAMED("HostileSight::OnPlayerUpdate");

      if (auto* ui = RE::UI::GetSingleton();
          ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME))) {
         ClearLists();
         return;
      }
      if (!player->Get3D() || !player->GetParentCell()) {
         ClearLists();
         return;
      }

      std::vector<std::uint32_t> requested;
      {
         std::lock_guard lock(g_mutex);
         requested = g_requested;
      }
      // The rays are cast outside the lock: the poll never waits on them.
      std::vector<std::uint32_t> seen;
      for (const auto formID : requested) {
         auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
         if (!actor || actor->IsDead() || !actor->Get3D()) continue;
         bool unused = false;
         if (player->HasLineOfSight(actor, unused)) seen.push_back(formID);
      }
      std::lock_guard lock(g_mutex);
      g_seen = std::move(seen);
   }

   void SetGameLoaded(bool loaded) noexcept
   {
      g_gameLoaded.store(loaded, std::memory_order_release);
      try {
         ClearLists();
      } catch (...) {
      }
   }
}
