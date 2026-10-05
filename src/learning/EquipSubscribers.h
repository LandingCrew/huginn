#pragma once

#include "EquipEventBus.h"
#include "FeatureBanditLearner.h"
#include "UsageMemory.h"
#include "Config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Huginn::Learning
{
    // =========================================================================
    // BANDIT SUBSCRIBER - the choice target (roadmap Phase 3 #1, 0.23.0)
    // =========================================================================
    // Every event is a CONFIRMED player selection (SelectionTracker). It
    // teaches two things, both on the press-time features:
    //   - the chosen item -> 1 (CHOICE_TARGET), equip or consume alike;
    //   - each item shown on the page for the SAME need (the selection log's
    //     `need`, SlotClassifier::Classify) and passed over -> 0, at a quarter
    //     step and not counted as a train (PASSED_OVER_*). Only plain
    //     recommendations: an override, a Remembrance hold or a wildcard was
    //     not the learner's offer, so passing it teaches the learner nothing.
    // The update `w += a*(target - w.phi)*phi` sizes every move by surprise:
    // a strong item passed over drops a lot, a weak one barely; a low-ranked
    // item chosen rises a lot, the incumbent chosen again barely moves.
    // A pick of the same item within REPEAT_PICK_WINDOW_SEC teaches nothing --
    // one decision, one reward. No source filtering and no multipliers: the
    // device is a label, not a weight (one selection path, 2026-10-02).
    // =========================================================================
    class BanditSubscriber final : public IEquipSubscriber
    {
    public:
        explicit BanditSubscriber(FeatureBanditLearner& learner) : m_learner(learner) {}

        void OnEquipEvent(const EquipEvent& event) override
        {
            const auto now = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock(m_lastPickMutex);
                if (auto it = m_lastPick.find(event.formID); it != m_lastPick.end() &&
                    std::chrono::duration<float>(now - it->second).count() < Config::REPEAT_PICK_WINDOW_SEC) {
                    it->second = now;   // sliding: toggling back and forth never re-earns it
                    logger::info("[BanditSubscriber] {:08X} picked again within {:.0f}s -- the same decision, no update"sv,
                        event.formID, Config::REPEAT_PICK_WINDOW_SEC);
                    return;
                }
                m_lastPick[event.formID] = now;
            }

            m_learner.Update(event.formID, event.features, Config::CHOICE_TARGET);

            size_t passedOver = 0;
            const auto& snap = event.shown;
            if (const auto* chosen = snap.Find(event.formID)) {
                std::vector<RE::FormID> done;
                for (const auto& slot : snap.shown) {
                    if (slot.formID == event.formID || slot.type != Slot::AssignmentType::Normal) continue;
                    if (std::find(done.begin(), done.end(), slot.formID) != done.end()) continue;
                    const auto* row = snap.Find(slot.formID);
                    if (!row || row->need != chosen->need) continue;
                    m_learner.Update(slot.formID, event.features, Config::PASSED_OVER_TARGET,
                        Config::PASSED_OVER_STEP, /*countsAsTrain=*/false);
                    done.push_back(slot.formID);
                    ++passedOver;
                }
            }

            logger::info("[BanditSubscriber] Chosen {:08X} -> {:.0f}, {} passed over -> {:.0f} x{:.2f} (src={}, {})"sv,
                event.formID, Config::CHOICE_TARGET, passedOver, Config::PASSED_OVER_TARGET,
                Config::PASSED_OVER_STEP, EquipSourceToString(event.source),
                SelectionKindToString(event.kind));
        }

    private:
        FeatureBanditLearner& m_learner;
        // Dispatch reaches here from the update loop and from the soul-gem
        // instant confirm; not assumed to be one thread.
        std::mutex m_lastPickMutex;
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> m_lastPick;
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
