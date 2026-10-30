#pragma once

#include "EquipEventBus.h"
#include "ChoiceLearner.h"

#include <atomic>
#include <chrono>

namespace Huginn::Learning
{
    // =========================================================================
    // CHOICE SUBSCRIBER - the new learner (R8.2, shadow; ChoiceLearner)
    // =========================================================================
    // Beside the old learner's two subscribers (EquipSubscribers.h), which
    // stay: the old learner feeds the D2 legacy feature
    // (learning/legacy/LegacyLearnFeature.h). This file is the live learner's
    // and names no old-engine code (tools/prune/r8_check.py checks it).
    // Its input is the decision the selection log v3 recorded for the pick
    // (EquipEvent::v3Decision, set at confirmation just before the dispatch):
    // the context, its rows (the page -- everything visible, wildcard,
    // override and Remembrance slots included -- and the held items off it)
    // and the chosen row, so the choice set is the fit's row for row. That
    // reverses the old learner subscriber's exclusion of wildcard, override and
    // Remembrance slots, for this subscriber only (doc 9 decision, the map's
    // Phase 6). A pick the frozen learner skips (armour, a stale cache, the
    // learning toggle off) never reaches the bus; the v3 log hands it to
    // Ingest directly. Repeats (REPEAT_PICK_WINDOW_SEC) do not update.
    // =========================================================================
    class ChoiceSubscriber final : public IEquipSubscriber
    {
    public:
        explicit ChoiceSubscriber(ChoiceLearner& learner) : m_learner(learner) { s_learner.store(&learner); }
        ~ChoiceSubscriber() override { s_learner.store(nullptr); }

        void OnEquipEvent(const EquipEvent& event) override
        {
            if (event.v3Decision) Feed(m_learner, *event.v3Decision);
        }

        /// A v3-only pick (it never reaches the bus).
        static void Ingest(const Core::DecisionLog::Decision& d)
        {
            if (auto* l = s_learner.load()) Feed(*l, d);
        }

    private:
        static void Feed(ChoiceLearner& learner, const Core::DecisionLog::Decision& d)
        {
            // The decision moment on the steady clock: the confirmation (now)
            // less the confirmation delay, as fitlib.pick_time reads utc - confirmMs.
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            learner.OnPick(d, Core::Choice::PickTime(now, static_cast<double>(d.confirmMs)));
        }

        ChoiceLearner& m_learner;
        static inline std::atomic<ChoiceLearner*> s_learner{ nullptr };
    };

}  // namespace Huginn::Learning
