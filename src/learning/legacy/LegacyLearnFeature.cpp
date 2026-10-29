#include "LegacyLearnFeature.h"

#include <atomic>

namespace Huginn::Scoring::Legacy
{
    namespace
    {
        // [Scoring] bLegacyLearnFeature. Default on: the bootstrap's weight
        // (w = 1.36) was fitted with the feature (R6's D2).
        std::atomic<bool> g_enabled{ true };
    }

    bool Enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

    bool SetEnabled(bool on) noexcept
    {
        const bool was = g_enabled.exchange(on, std::memory_order_relaxed);
        if (was != on) {
            if (on) {
                logger::info("[Legacy] bLegacyLearnFeature on: the legacy pass runs again and the choice score reads "
                             "ln learn mult on the rows the old floors keep"sv);
            }
            else {
                logger::warn("[Legacy] bLegacyLearnFeature off: no legacy pass (no context rules, prior, correlation "
                             "or potion work); ln learn mult 0 and util missing 1 on every row -- plain theta + b + "
                             "recent, which failed R6's gate (a fallback, not a supported mode)"sv);
            }
        }
        return was;
    }
}
