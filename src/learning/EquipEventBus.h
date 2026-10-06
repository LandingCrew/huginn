#pragma once

#include "EquipEvent.h"
#include <chrono>
#include <mutex>
#include <vector>

namespace Huginn::Learning
{
    // =========================================================================
    // IEQUIP SUBSCRIBER - Interface for equip event consumers
    // =========================================================================
    class IEquipSubscriber
    {
    public:
        virtual ~IEquipSubscriber() = default;
        virtual void OnEquipEvent(const EquipEvent& event) = 0;
        /// Called every update tick (SelectionTracker::Update), for work a
        /// subscriber defers -- the learner's delayed passed-over updates.
        virtual void OnTick(std::chrono::steady_clock::time_point /*now*/) {}
        /// Called on a game load and on `hg reset`: drop anything deferred.
        virtual void OnReset() {}
    };

    // =========================================================================
    // EQUIP EVENT BUS - Observer pattern singleton
    // =========================================================================
    // One event per CONFIRMED player selection. SelectionTracker captures the
    // state when the player chooses (Capture) and dispatches the event when
    // the choice confirms (Dispatch) -- so subscribers train on press-time
    // state, and a selection that never confirms never reaches them.
    //
    // Lock ordering (must be respected to avoid deadlocks):
    //   StateManager shared locks (acquired in Capture)
    //   → m_mutex (acquired to snapshot the subscriber list)
    //   → subscriber internal locks (learner m_mutex, UsageMemory m_mutex, etc.)
    //
    // Capture takes no bus lock; Dispatch calls subscribers outside m_mutex.
    // =========================================================================
    class EquipEventBus
    {
    public:
        static EquipEventBus& GetSingleton()
        {
            static EquipEventBus instance;
            return instance;
        }

        void Subscribe(IEquipSubscriber* subscriber);
        void Unsubscribe(IEquipSubscriber* subscriber);

        /// Evaluate the game state NOW into an event (no dispatch).
        [[nodiscard]] static EquipEvent Capture(RE::FormID formID, EquipSource source);

        /// Hand a confirmed selection to every subscriber.
        void Dispatch(const EquipEvent& event);
        void Tick(std::chrono::steady_clock::time_point now);
        void Reset();

    private:
        EquipEventBus() = default;
        ~EquipEventBus() = default;
        EquipEventBus(const EquipEventBus&) = delete;
        EquipEventBus& operator=(const EquipEventBus&) = delete;

        std::mutex m_mutex;
        std::vector<IEquipSubscriber*> m_subscribers;
    };

}  // namespace Huginn::Learning
