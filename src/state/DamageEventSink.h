#pragma once

#include "StateTypes.h"
#include <atomic>
#include <queue>
#include <mutex>
#include <vector>

namespace Huginn::State
{
   // =============================================================================
   // DAMAGE EVENT SINK (v0.6.8)
   // =============================================================================
   // Event-based damage detection using TESHitEvent.
   // Solves the instant damage classification problem where ActiveEffects expire
   // before the 100ms poll window can read them.
   //
   // PROBLEM:
   // - Instant damage spells (Fireball, Firebolt, Ice Spike) have ~20-50ms effect lifetime
   // - PollHealthTracking runs at 100ms intervals
   // - By the time we poll, the ActiveEffect is gone and we can't classify the damage type
   //
   // SOLUTION:
   // - TESHitEvent fires at moment of impact with spell/projectile data
   // - We capture damage type immediately and queue it for StateManager
   // - PollHealthTracking merges queued events with detected health changes
   //
   // THREAD SAFETY:
   // - TESHitEvent fires on game thread (same as Update loop)
   // - Queue uses mutex for safe access between event handler and poll method
   // - DrainQueue() returns a copy to minimize lock duration
   // =============================================================================

   /// Queued damage event from TESHitEvent (captured at impact time)
   struct QueuedDamageEvent
   {
      float gameTime = 0.0f;           // Game time when hit occurred (from Calendar)
      float estimatedAmount = 0.0f;    // Estimated damage (0 if unknown - will be filled from health delta)
      DamageType type = DamageType::Unknown;  // Classified damage type from spell/projectile

      QueuedDamageEvent() = default;
      QueuedDamageEvent(float time, float amount, DamageType t)
      : gameTime(time), estimatedAmount(amount), type(t) {}
   };

   class DamageEventSink : public RE::BSTEventSink<RE::TESHitEvent>
   {
   public:
      /// Get singleton instance
      static DamageEventSink& GetSingleton();

      /// Register with SKSE event system (call at kDataLoaded)
      void Register();

      /// Unregister from SKSE event system (call at shutdown if needed)
      void Unregister();

      /// Check if currently registered
      [[nodiscard]] bool IsRegistered() const noexcept { return m_registered; }

      /// Drain all queued events (thread-safe, returns copy)
      /// Called by StateManager::PollHealthTracking() to get pending events
      [[nodiscard]] std::vector<QueuedDamageEvent> DrainQueue();

      /// Get queue size (for debugging)
      [[nodiscard]] size_t GetQueueSize() const;

   protected:
      /// Process TESHitEvent - called on game thread when any actor is hit
      RE::BSEventNotifyControl ProcessEvent(
      const RE::TESHitEvent* event,
      RE::BSTEventSource<RE::TESHitEvent>* source) override;

   private:
      DamageEventSink() = default;
      ~DamageEventSink() override = default;

      DamageEventSink(const DamageEventSink&) = delete;
      DamageEventSink(DamageEventSink&&) = delete;
      DamageEventSink& operator=(const DamageEventSink&) = delete;
      DamageEventSink& operator=(DamageEventSink&&) = delete;

      /// Classify damage type from hit event (spell, projectile, weapon)
      [[nodiscard]] DamageType ClassifyHitEvent(const RE::TESHitEvent* event) const;

      /// Classify damage type from a spell's effects
      [[nodiscard]] DamageType ClassifySpellDamageType(const RE::MagicItem* spell) const;

      /// Queue of pending damage events
      std::queue<QueuedDamageEvent> m_eventQueue;

      /// Mutex protecting the event queue
      mutable std::mutex m_queueMutex;

      /// Registration state (atomic for thread safety)
      std::atomic<bool> m_registered{false};

      /// Maximum queue size (prevent unbounded growth in edge cases)
      static constexpr size_t MAX_QUEUE_SIZE = 32;
   };
}
