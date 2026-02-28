#pragma once

#include "EquipEventBus.h"
#include "FeatureQLearner.h"
#include "UsageMemory.h"
#include "Config.h"
#include "candidate/CandidateGenerator.h"
#include "candidate/CandidateTypes.h"

#include <spdlog/spdlog.h>

namespace Huginn::Learning
{
    // =========================================================================
    // FQL SUBSCRIBER - Applies FeatureQLearner rewards
    // =========================================================================
    // Source filtering:
    //   Hotkey/Wheeler: reward only if wasRecommended (player chose our suggestion)
    //   External: always reward (attribution scaling via rewardMultiplier)
    //   Consumption: uses CONSUME_REWARD constant
    // =========================================================================
    class FQLSubscriber final : public IEquipSubscriber
    {
    public:
        explicit FQLSubscriber(FeatureQLearner& fql) : m_fql(fql) {}

        void OnEquipEvent(const EquipEvent& event) override
        {
            float reward = 0.0f;

            switch (event.source) {
            case EquipSource::Hotkey:
            case EquipSource::Wheeler:
                if (!event.wasRecommended) return;  // No reward for non-recommended items
                reward = Config::EQUIP_REWARD * event.rewardMultiplier;
                break;

            case EquipSource::External:
                // External always applies (attribution scaling already in rewardMultiplier)
                reward = Config::EQUIP_REWARD * event.rewardMultiplier;
                break;

            case EquipSource::Consumption:
                reward = Config::CONSUME_REWARD * event.rewardMultiplier;
                break;
            }

            m_fql.Update(event.formID, event.features, reward);

            logger::info("[FQLSubscriber] Reward {:08X} +{:.1f} (src={}, mult={:.2f})"sv,
                event.formID, reward,
                EquipSourceToString(event.source), event.rewardMultiplier);
        }

    private:
        FeatureQLearner& m_fql;
    };

    // =========================================================================
    // USAGE MEMORY SUBSCRIBER - Records usage + handles misclick detection
    // =========================================================================
    // Consolidates the duplicated misclick penalty blocks from 3 files into 1.
    // Fires for ALL equip sources (including Consumption — fixes the bug where
    // consuming a potion built no recency memory).
    // =========================================================================
    class UsageMemorySubscriber final : public IEquipSubscriber
    {
    public:
        UsageMemorySubscriber(UsageMemory& memory, FeatureQLearner& fql)
            : m_memory(memory), m_fql(fql) {}

        void OnEquipEvent(const EquipEvent& event) override
        {
            auto misclick = m_memory.RecordUsage(event.formID, event.gameState);

            if (misclick.detected) {
                m_fql.Update(misclick.previousFormID, event.features, Config::MISCLICK_PENALTY);
                logger::debug("[Misclick] Penalized {:08X} ({:.1f})"sv,
                    misclick.previousFormID, Config::MISCLICK_PENALTY);
            }
        }

    private:
        UsageMemory& m_memory;
        FeatureQLearner& m_fql;
    };

    // =========================================================================
    // COOLDOWN SUBSCRIBER - Starts candidate cooldown for consumed items
    // =========================================================================
    // Only fires for Consumption events. Wheeler handles its own cooldown
    // (policy-dependent: Sticky skips it). Hotkey/External don't need cooldown.
    // =========================================================================
    class CooldownSubscriber final : public IEquipSubscriber
    {
    public:
        void OnEquipEvent(const EquipEvent& event) override
        {
            if (event.source != EquipSource::Consumption) return;

            auto& candidateGen = Candidate::CandidateGenerator::GetSingleton();
            if (!candidateGen.IsInitialized()) return;

            // Determine source type from form
            Candidate::SourceType sourceType = Candidate::SourceType::Spell;
            if (auto* form = RE::TESForm::LookupByID(event.formID)) {
                if (form->Is(RE::FormType::AlchemyItem)) sourceType = Candidate::SourceType::Potion;
                else if (form->Is(RE::FormType::Scroll))  sourceType = Candidate::SourceType::Scroll;
            }

            candidateGen.StartCooldown(event.formID, sourceType);
            logger::debug("[CooldownSubscriber] Started cooldown for {:08X} (type {})"sv,
                event.formID, static_cast<int>(sourceType));
        }
    };

}  // namespace Huginn::Learning
