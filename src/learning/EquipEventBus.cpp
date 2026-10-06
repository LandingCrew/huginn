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

    void EquipEventBus::Dispatch(const EquipEvent& event)
    {
        // Snapshot subscriber list under m_mutex, then dispatch OUTSIDE it.
        // Subscribers acquire their own internal locks (learner m_mutex, UsageMemory::m_mutex),
        // so dispatching under m_mutex would create a lock-inversion risk if any code path
        // ever holds those locks and calls Dispatch() or Subscribe().
        std::vector<IEquipSubscriber*> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_subscribers;
        }

        for (auto* subscriber : snapshot) {
            subscriber->OnEquipEvent(event);
        }
    }

    void EquipEventBus::Tick(std::chrono::steady_clock::time_point now)
    {
        std::vector<IEquipSubscriber*> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_subscribers;
        }
        for (auto* subscriber : snapshot) {
            subscriber->OnTick(now);
        }
    }

    void EquipEventBus::Reset()
    {
        std::vector<IEquipSubscriber*> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_subscribers;
        }
        for (auto* subscriber : snapshot) {
            subscriber->OnReset();
        }
    }

    EquipEvent EquipEventBus::Capture(RE::FormID formID, EquipSource source)
    {
        EquipEvent event;
        event.formID = formID;
        event.source = source;

        // Evaluate state once for all subscribers. StateFeatures are extracted
        // directly (continuous features for the learner); GameState is the discretized
        // version (for UsageMemory context hashing). Both derive from the SAME
        // player/targets copies below, so features and gameState can't disagree.
        // NOTE: each accessor takes its own shared_lock — this is NOT one atomic
        // StateManager snapshot, and world (fetched later) can skew slightly
        // relative to player/targets. That's acceptable; the invariant that
        // matters here is features/gameState consistency.
        auto& stateMgr = State::StateManager::GetSingleton();
        auto player = stateMgr.GetPlayerState();
        auto targets = stateMgr.GetTargets();

        event.features = StateFeatures::FromState(player, targets);

        if (auto* evaluator = Huginn::GetStateEvaluator()) {
            event.gameState = evaluator->EvaluateCurrentState(player, targets);
        }

        return event;
    }

}  // namespace Huginn::Learning
