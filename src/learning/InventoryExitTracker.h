#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace Huginn::Learning
{
   // =============================================================================
   // INVENTORY EXIT TRACKER (v0.7.22)
   // =============================================================================
   // Distinguishes "count dropped because the player consumed/cast the item"
   // from "count dropped because the item left the inventory another way"
   // (dropped into the world, sold, stored in a container, given to a follower).
   //
   // PROBLEM:
   // - UpdateLoop's delta scan rewards ANY count decrease as a consumption,
   //   so dropping or selling scrolls/potions teaches the Q-learner the player
   //   favors items they are actually discarding.
   //
   // SOLUTION:
   // - TESContainerChangedEvent fires for every transfer out of the player's
   //   inventory. Consuming/casting removes the item with NO destination
   //   (newContainer == 0 and no dropped world reference); every other exit
   //   has a destination container or spawns a dropped reference.
   // - Non-consumption exits are recorded here; the delta scan claims them
   //   via ClaimExits() before publishing consumption rewards.
   //
   // THREAD SAFETY:
   // - Container events fire on the game thread; ClaimExits runs on the update
   //   loop. The pending map is mutex-guarded.
   // =============================================================================

   class InventoryExitTracker : public RE::BSTEventSink<RE::TESContainerChangedEvent>
   {
   public:
      /// Get singleton instance
      static InventoryExitTracker& GetSingleton();

      /// Register with SKSE event system (call at kDataLoaded)
      void Register();

      /// Claim up to `count` pending non-consumption exits for this form.
      /// Returns how many of the removals are explained by drop/sell/store;
      /// the remainder (if any) is genuine consumption.
      [[nodiscard]] int32_t ClaimExits(RE::FormID formID, int32_t count);

   protected:
      /// Process TESContainerChangedEvent - called on game thread
      RE::BSEventNotifyControl ProcessEvent(
      const RE::TESContainerChangedEvent* event,
      RE::BSTEventSource<RE::TESContainerChangedEvent>* source) override;

   private:
      InventoryExitTracker() = default;
      ~InventoryExitTracker() override = default;

      InventoryExitTracker(const InventoryExitTracker&) = delete;
      InventoryExitTracker(InventoryExitTracker&&) = delete;
      InventoryExitTracker& operator=(const InventoryExitTracker&) = delete;
      InventoryExitTracker& operator=(InventoryExitTracker&&) = delete;

      struct PendingExit
      {
      int32_t count = 0;
      std::chrono::steady_clock::time_point lastEvent;
      };

      /// Pending non-consumption exits keyed by base object FormID
      std::unordered_map<RE::FormID, PendingExit> m_pending;

      /// Mutex protecting the pending map
      mutable std::mutex m_mutex;

      /// Registration state (atomic for thread safety)
      std::atomic<bool> m_registered{false};

      // Entries the delta scan never claims (e.g. the decrement was absorbed by
      // a reconcile tick, which emits no change events) must expire so they
      // can't suppress a real consumption reward much later. The TTL is long
      // because menus pause the update loop: a barter session can hold claims
      // back for minutes while exit events keep arriving in real time.
      static constexpr auto ENTRY_TTL = std::chrono::minutes(5);

      /// Sweep expired entries when the map grows past this (prevents
      /// unbounded growth from never-claimed forms)
      static constexpr size_t PURGE_THRESHOLD = 64;
   };
}
