#pragma once

#include "EquipEvent.h"
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
    };

    // =========================================================================
    // EQUIP EVENT BUS - Observer pattern singleton
    // =========================================================================
    // Publishers call Publish() with raw parameters. The bus evaluates state
    // ONCE (BuildEvent), then dispatches to all registered subscribers.
    //
    // Lock ordering (must be respected to avoid deadlocks):
    //   StateManager shared locks (acquired in BuildEvent)
    //   → m_mutex (acquired for subscriber dispatch)
    //   → subscriber internal locks (FQL m_mutex, UsageMemory m_mutex, etc.)
    //
    // BuildEvent runs OUTSIDE m_mutex to avoid holding the bus lock while
    // acquiring StateManager locks.
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

        // Publish an equip event. Evaluates state once, then dispatches to all subscribers.
        void Publish(RE::FormID formID, EquipSource source, float rewardMultiplier, bool wasRecommended);

    private:
        EquipEventBus() = default;
        ~EquipEventBus() = default;
        EquipEventBus(const EquipEventBus&) = delete;
        EquipEventBus& operator=(const EquipEventBus&) = delete;

        // Build event with pre-computed state (called OUTSIDE m_mutex)
        [[nodiscard]] EquipEvent BuildEvent(RE::FormID formID, EquipSource source,
                                             float rewardMultiplier, bool wasRecommended) const;

        std::mutex m_mutex;
        std::vector<IEquipSubscriber*> m_subscribers;
    };

}  // namespace Huginn::Learning
