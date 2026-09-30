#pragma once

#include "DecisionLog.h"
#include "learning/EquipEventBus.h"
#include "learning/EquipSubscribers.h"   // ComputeBanditReward

namespace Huginn::Telemetry
{
    // =========================================================================
    // TELEMETRY SUBSCRIBER - Mirrors every equip/consume reward into the
    // decision log (opt-in, [Telemetry] bEnabled).
    // =========================================================================
    // Uses the SAME reward rule as BanditSubscriber (ComputeBanditReward), so
    // the logged "r"/"trained" are exactly what the learner applied. Misclick
    // penalties are logged from UsageMemorySubscriber, where they are decided.
    // Returns at once while telemetry is off. Runs on whichever thread
    // published the event; DecisionLog does no IO here.
    // =========================================================================
    class TelemetrySubscriber final : public Learning::IEquipSubscriber
    {
    public:
        void OnEquipEvent(const Learning::EquipEvent& event) override
        {
            auto& log = DecisionLog::GetSingleton();
            if (!log.IsEnabled()) {
                return;
            }
            const auto [reward, trains] = Learning::ComputeBanditReward(event);
            log.RecordReward(event, reward, trains);
        }
    };

}  // namespace Huginn::Telemetry
