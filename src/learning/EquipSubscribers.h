#pragma once

#include "EquipEventBus.h"
#include "FeatureBanditLearner.h"
#include "UsageMemory.h"
#include "Config.h"

#include <spdlog/spdlog.h>

namespace Huginn::Learning
{
    // =========================================================================
    // BANDIT SUBSCRIBER - Applies FeatureBanditLearner rewards
    // =========================================================================
    // Every event is a CONFIRMED player selection (SelectionTracker), so every
    // event is one reward: EQUIP_REWARD or CONSUME_REWARD by kind, trained on
    // the press-time features. No source filtering and no multipliers -- the
    // device the player used is a label, not a weight (one selection path,
    // 2026-10-02). Whether to learn from it at all was decided upstream.
    // =========================================================================
    class BanditSubscriber final : public IEquipSubscriber
    {
    public:
        explicit BanditSubscriber(FeatureBanditLearner& learner) : m_learner(learner) {}

        void OnEquipEvent(const EquipEvent& event) override
        {
            const float reward = RewardFor(event.kind);
            m_learner.Update(event.formID, event.features, reward);

            logger::info("[BanditSubscriber] Reward {:08X} +{:.1f} (src={}, {})"sv,
                event.formID, reward, EquipSourceToString(event.source),
                SelectionKindToString(event.kind));
        }

    private:
        FeatureBanditLearner& m_learner;
    };

    // =========================================================================
    // USAGE MEMORY SUBSCRIBER - Records confirmed usage for the recency boost
    // =========================================================================
    // One record per confirmed selection, in the press-time context. Misclick
    // detection used to live here ("a different item, same context, within
    // 3 s" cost the earlier item -3); the confirm window replaced it -- an item
    // swapped away inside the window never confirms, so it never reaches here.
    // The late consumption event that misclick logic misread as a switch is
    // gone with it.
    // =========================================================================
    class UsageMemorySubscriber final : public IEquipSubscriber
    {
    public:
        explicit UsageMemorySubscriber(UsageMemory& memory) : m_memory(memory) {}

        void OnEquipEvent(const EquipEvent& event) override
        {
            m_memory.RecordUsage(event.formID, event.gameState);
        }

    private:
        UsageMemory& m_memory;
    };

}  // namespace Huginn::Learning
