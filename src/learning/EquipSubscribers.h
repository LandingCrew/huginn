#pragma once

#include "EquipEventBus.h"
#include "FeatureBanditLearner.h"
#include "UsageMemory.h"
#include "Config.h"
#include "candidate/CandidateGenerator.h"
#include "candidate/CandidateTypes.h"
#include "util/InventoryUtil.h"

#include <spdlog/spdlog.h>

namespace Huginn::Learning
{
    // =========================================================================
    // BANDIT SUBSCRIBER - Applies FeatureBanditLearner rewards
    // =========================================================================
    // Source filtering:
    //   Hotkey/Wheeler: reward only if wasRecommended (player chose our suggestion)
    //   External: always reward (attribution scaling via rewardMultiplier)
    //   Consumption: uses CONSUME_REWARD constant
    // =========================================================================
    class BanditSubscriber final : public IEquipSubscriber
    {
    public:
        explicit BanditSubscriber(FeatureBanditLearner& learner) : m_learner(learner) {}

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

            // A zero multiplier means "this act teaches nothing", not "this item
            // is worthless". Updating with reward 0 is not the same as not
            // updating: it is an observation, it moves the weights down and it
            // counts as training. The publisher that zeroed the multiplier is
            // asking the learner to stay out of it, so stay out of it -- the
            // other subscribers (usage memory, cooldown) still want the event.
            if (event.rewardMultiplier <= 0.0f) {
                return;
            }

            m_learner.Update(event.formID, event.features, reward);

            logger::info("[BanditSubscriber] Reward {:08X} +{:.1f} (src={}, mult={:.2f})"sv,
                event.formID, reward,
                EquipSourceToString(event.source), event.rewardMultiplier);
        }

    private:
        FeatureBanditLearner& m_learner;
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
        UsageMemorySubscriber(UsageMemory& memory, FeatureBanditLearner& learner)
            : m_memory(memory), m_learner(learner) {}

        void OnEquipEvent(const EquipEvent& event) override
        {
            auto misclick = m_memory.RecordUsage(event.formID, event.gameState);

            // A piece you are STILL WEARING was not a misclick. UsageMemory
            // infers the penalty from "different item, same context, inside three
            // seconds", which reads as a switch - true for two spells in one hand
            // or two potions, false for gear in different body slots. A fortify
            // circlet and a fortify ring are worn TOGETHER; that is the point of
            // them. Without this, the ordinary act of gearing up for a crafting
            // session penalised whichever piece went on first by -3.0 against its
            // own +8.0 reward (observed 2026-09-18: 000FC000 then 0003B97C at an
            // alchemy lab, 2.2 s apart).
            //
            // Phrased as "still worn" rather than "both are apparel" because that
            // is the honest form of the rule: a switch means the first thing is no
            // longer on. It holds for weapons too.
            //
            // The body-slot test is what makes "still worn" trustworthy HERE.
            // ActorEquipManager has not applied this equip yet, so a piece being
            // displaced by it still reads as worn - swapping circlet A for circlet
            // B is a genuine switch that reported "still worn" and escaped its
            // penalty (seen 2026-09-18: 000166FE, displaced by 000FC000 in the same
            // instant, logged as still worn). Overlapping slot masks mean the two
            // cannot be worn together, so it was a switch whatever the game has
            // got round to yet.
            if (misclick.detected &&
                IsWornByPlayer(misclick.previousFormID) &&
                !SharesBodySlot(misclick.previousFormID, event.formID)) {
                logger::debug("[Misclick] {:08X} still worn elsewhere - not a switch, no penalty"sv,
                    misclick.previousFormID);
                return;
            }

            if (misclick.detected) {
                m_learner.Update(misclick.previousFormID, event.features, Config::MISCLICK_PENALTY);
                logger::debug("[Misclick] Penalized {:08X} ({:.1f})"sv,
                    misclick.previousFormID, Config::MISCLICK_PENALTY);
            }
        }

    private:
        /// Is this base form on the player right now? Reads ExtraWorn off the
        /// inventory rather than a registry, so it answers for every source, not
        /// just apparel. Safe to ask here: the equip that raised THIS event has
        /// not been applied yet (see ApparelRegistry::MarkEquipped), but the
        /// previous item went on seconds ago, so its worn flag has settled.
        [[nodiscard]] static bool IsWornByPlayer(RE::FormID formID)
        {
            if (formID == 0) return false;

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;

            auto inventory = Util::GetInventorySafe(player,
                [formID](RE::TESBoundObject& obj) { return obj.GetFormID() == formID; });

            for (const auto& [obj, data] : inventory) {
                const auto& [count, entry] = data;
                if (!entry || !entry->extraLists) continue;

                for (const auto* extraList : *entry->extraLists) {
                    if (!extraList) continue;
                    if (extraList->HasType<RE::ExtraWorn>() ||
                        extraList->HasType<RE::ExtraWornLeft>()) {
                        return true;
                    }
                }
            }

            return false;
        }

        /// Do these two forms compete for the same body slot? Two armors whose
        /// biped slot masks overlap cannot be worn at once, so choosing one right
        /// after the other IS a switch. Anything that is not armor - a potion, a
        /// spell, a weapon - answers true, which keeps the original penalty
        /// behaviour for every source that had it before.
        [[nodiscard]] static bool SharesBodySlot(RE::FormID previousID, RE::FormID newID)
        {
            const auto* previous = RE::TESForm::LookupByID<RE::TESObjectARMO>(previousID);
            const auto* current = RE::TESForm::LookupByID<RE::TESObjectARMO>(newID);
            if (!previous || !current) return true;

            return (static_cast<uint32_t>(previous->GetSlotMask()) &
                    static_cast<uint32_t>(current->GetSlotMask())) != 0;
        }

        UsageMemory& m_memory;
        FeatureBanditLearner& m_learner;
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
