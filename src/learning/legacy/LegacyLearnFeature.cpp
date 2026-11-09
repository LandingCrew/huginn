#include "LegacyLearnFeature.h"

#include <atomic>

namespace Huginn::Scoring::Legacy
{
    namespace
    {
        // [Scoring] bLegacyLearnFeature. 0.24.17: default off -- the live
        // engine is B (no D2 feature) and D2 runs in shadow (the user's call,
        // 2026-10-30). Before 0.24.17 it defaulted on: the bootstrap's weight
        // (w = 1.36) was fitted with the feature (R6's D2).
        std::atomic<bool> g_enabled{ false };
        // [Scoring] bShadowLegacy (0.24.17): D2 mirrored beside B.
        std::atomic<bool> g_shadow{ true };
    }

    bool Enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

    bool SetEnabled(bool on) noexcept
    {
        const bool was = g_enabled.exchange(on, std::memory_order_relaxed);
        if (was != on) {
            if (on) {
                logger::info("[Legacy] bLegacyLearnFeature on: the choice score reads ln learn mult on the rows the "
                             "old floors keep (D2, engine A, live); the shadow does not run"sv);
            }
            else {
                logger::info("[Legacy] bLegacyLearnFeature off: the choice score reads ln learn mult 0 and util "
                             "missing 1 on every row (engine B, live); D2 runs in shadow while bShadowLegacy is on"sv);
            }
        }
        return was;
    }

    bool ShadowEnabled() noexcept { return g_shadow.load(std::memory_order_relaxed); }

    bool SetShadowEnabled(bool on) noexcept
    {
        const bool was = g_shadow.exchange(on, std::memory_order_relaxed);
        if (was != on) {
            logger::info("[Legacy] bShadowLegacy {}"sv,
                on ? "on: with bLegacyLearnFeature off, the legacy pass runs and D2's score, ranks and page are "
                     "computed beside the live ones (logged, never shown)"
                   : "off: with bLegacyLearnFeature off, no legacy pass and no shadow");
        }
        return was;
    }
}
