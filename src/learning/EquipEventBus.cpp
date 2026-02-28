#include "EquipEventBus.h"
#include "state/StateManager.h"
#include "state/StateEvaluator.h"
#include "Globals.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace Huginn::Learning
{
    void EquipEventBus::Subscribe(IEquipSubscriber* subscriber)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.push_back(subscriber);
    }

    void EquipEventBus::Unsubscribe(IEquipSubscriber* subscriber)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::erase(m_subscribers, subscriber);
    }

    void EquipEventBus::Publish(RE::FormID formID, EquipSource source,
                                 float rewardMultiplier, bool wasRecommended)
    {
        // 1. Build event OUTSIDE m_mutex (acquires StateManager shared locks, then releases)
        auto event = BuildEvent(formID, source, rewardMultiplier, wasRecommended);

        // 2. Snapshot subscriber list under m_mutex, then dispatch OUTSIDE it.
        //    Subscribers acquire their own internal locks (FQL::m_mutex, UsageMemory::m_mutex),
        //    so dispatching under m_mutex would create a lock-inversion risk if any code path
        //    ever holds those locks and calls Publish() or Subscribe().
        std::vector<IEquipSubscriber*> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_subscribers;
        }

        for (auto* subscriber : snapshot) {
            subscriber->OnEquipEvent(event);
        }

        logger::info("[EquipEventBus] Dispatched {} event for {:08X} (mult={:.2f}, rec={}) to {} subscribers"sv,
            EquipSourceToString(source), formID, rewardMultiplier, wasRecommended, snapshot.size());
    }

    EquipEvent EquipEventBus::BuildEvent(RE::FormID formID, EquipSource source,
                                          float rewardMultiplier, bool wasRecommended) const
    {
        EquipEvent event;
        event.formID = formID;
        event.source = source;
        event.rewardMultiplier = rewardMultiplier;
        event.wasRecommended = wasRecommended;

        // Evaluate state once for all subscribers.
        // StateFeatures are extracted directly from StateManager (continuous features for FQL).
        // GameState is the discretized version (for UsageMemory context hashing).
        auto& stateMgr = State::StateManager::GetSingleton();
        event.features = StateFeatures::FromState(
            stateMgr.GetPlayerState(), stateMgr.GetTargets());

        if (Huginn::GetStateEvaluator()) {
            // EvaluateCurrentGameState returns {GameState, PlayerActorState}.
            // We only need GameState here — PlayerActorState is not stored in the event
            // because subscribers use the pre-computed StateFeatures instead.
            auto [gameState, unused_] = EvaluateCurrentGameState();
            event.gameState = gameState;
        }

        return event;
    }

}  // namespace Huginn::Learning
