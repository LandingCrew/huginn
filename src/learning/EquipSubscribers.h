#pragma once

#include "EquipEventBus.h"
#include "FeatureBanditLearner.h"
#include "UsageMemory.h"
#include "Config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <chrono>
#include <mutex>
#include <vector>

namespace Huginn::Learning
{
    // =========================================================================
    // BANDIT SUBSCRIBER - the choice target (roadmap Phase 3 #1, 0.23.0)
    // =========================================================================
    // Every event is a CONFIRMED player selection (SelectionTracker). It
    // teaches two things, both on the press-time features:
    //   - the chosen item -> 1 (CHOICE_TARGET), equip or consume alike;
    //   - each item shown on the page for the SAME slot class (the selection
    //     log's `need` column, SlotClassifier::Classify) and passed over -> 0, at a quarter
    //     step and not counted as a train (PASSED_OVER_*). Only plain
    //     recommendations: an override, a Remembrance hold or a wildcard was
    //     not the learner's offer, so passing it teaches the learner nothing.
    // Passed-over updates wait PASSED_OVER_DELAY_SEC and are cancelled when
    // their item is picked next -- companions (circlet then ring, sword then
    // off-hand dagger) share a slot class but are worn together. They are also
    // skipped for an item the learner has never seen: estimate 0, target 0,
    // nothing to learn, and an empty entry would only bloat the cosave.
    // The update `w += a*(target - w.phi)*phi` sizes every move by surprise.
    // A repeat pick (EquipEvent::repeatPick, decided in SelectionTracker)
    // teaches nothing. The device is a label, not a weight.
    // =========================================================================
    class BanditSubscriber final : public IEquipSubscriber
    {
    public:
        explicit BanditSubscriber(FeatureBanditLearner& learner) : m_learner(learner) {}

        void OnEquipEvent(const EquipEvent& event) override
        {
            const auto now = std::chrono::steady_clock::now();
            size_t cancelled = 0;
            size_t queued = 0;
            {
                std::scoped_lock lock(m_pendingMutex);
                // The chosen item was not passed over after all: a companion.
                // Before the repeat check, not after: B, then A (B queued as
                // passed over), then B again inside the repeat window is the
                // main weapon taken back -- B must not be trained toward 0 for
                // the pick it was just chosen again in (learner audit, 0.23.6).
                const auto before = m_pending.size();
                std::erase_if(m_pending, [&](const PendingNegative& p) { return p.formID == event.formID; });
                cancelled = before - m_pending.size();

                if (event.repeatPick) {
                    logger::info("[BanditSubscriber] {:08X} picked again within {:.0f}s -- the same decision, no update{}"sv,
                        event.formID, Config::REPEAT_PICK_WINDOW_SEC,
                        cancelled ? std::format("; {} passed-over update(s) for it cancelled", cancelled) : std::string{});
                    return;
                }

                const auto& snap = event.shown;
                if (const auto* chosen = snap.Find(event.formID)) {
                    for (const auto& slot : snap.shown) {
                        if (slot.formID == event.formID || slot.type != Slot::AssignmentType::Normal) continue;
                        const auto* row = snap.Find(slot.formID);
                        if (!row || row->slotClass != chosen->slotClass) continue;
                        if (std::any_of(m_pending.begin(), m_pending.end(),
                                [&](const PendingNegative& p) { return p.formID == slot.formID && p.chosenFor == event.formID; })) {
                            continue;   // shown twice on one page
                        }
                        m_pending.push_back({ slot.formID, event.formID, event.features,
                            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<float>(Config::PASSED_OVER_DELAY_SEC)) });
                        ++queued;
                    }
                }
            }

            m_learner.Update(event.formID, event.features, Config::CHOICE_TARGET);

            logger::info("[BanditSubscriber] Chosen {:08X} -> {:.0f}; {} passed over queued (x{:.2f} in {:.0f}s){} (src={}, {})"sv,
                event.formID, Config::CHOICE_TARGET, queued, Config::PASSED_OVER_STEP,
                Config::PASSED_OVER_DELAY_SEC,
                cancelled ? std::format(", {} cancelled -- picked next", cancelled) : std::string{},
                EquipSourceToString(event.source), SelectionKindToString(event.kind));
        }

        void OnTick(std::chrono::steady_clock::time_point now) override
        {
            std::vector<PendingNegative> due;
            {
                std::scoped_lock lock(m_pendingMutex);
                if (m_pending.empty()) return;
                for (auto it = m_pending.begin(); it != m_pending.end();) {
                    if (now >= it->dueAt) {
                        due.push_back(std::move(*it));
                        it = m_pending.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (const auto& p : due) {
                // An item the learner has never seen is skipped inside Update.
                m_learner.Update(p.formID, p.features, Config::PASSED_OVER_TARGET,
                    Config::PASSED_OVER_STEP, /*countsAsTrain=*/false);
            }
        }

        void OnReset() override
        {
            std::scoped_lock lock(m_pendingMutex);
            m_pending.clear();
        }

    private:
        struct PendingNegative
        {
            RE::FormID formID = 0;        // the item passed over
            RE::FormID chosenFor = 0;     // the pick that passed it over
            StateFeatures features{};     // that pick's press-time state
            std::chrono::steady_clock::time_point dueAt{};
        };

        FeatureBanditLearner& m_learner;
        // Dispatch reaches here from the update loop and from the soul-gem
        // instant confirm, OnTick from the update loop.
        std::mutex m_pendingMutex;
        std::vector<PendingNegative> m_pending;
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
            // A repeat pick is the same decision: no recency for it either, or
            // the re-equip pattern the window exists for would still be boosted
            // through this channel (code review of #172).
            if (event.repeatPick) return;
            m_memory.RecordUsage(event.formID, event.gameState);
        }

    private:
        UsageMemory& m_memory;
    };

}  // namespace Huginn::Learning
