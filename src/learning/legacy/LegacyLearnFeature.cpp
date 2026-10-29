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
    void SetEnabled(bool on) noexcept { g_enabled.store(on, std::memory_order_relaxed); }
}
