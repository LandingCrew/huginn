#pragma once

// Has the weapon registry read the inventory's extra data since the last game
// load? (WeaponRegistry::HasInstanceDetail, 0.23.40)
//
// For about a second after every load the extra lists cannot be read
// (Util::IsExtraListStable, EXTRALIST_STABILIZATION_MS), so the registry's
// load-time scan degrades to one record per base form with uniqueID 0. Until a
// scan that read them has run, "uniqueID 0" says nothing about a stack, and
// the wheel-page filter (core/WheelUidGate.h) must not act on it.
//
// The registry keeps a STAMP: the load generation (g_loadGeneration) of the
// last scan that read extra data. Detail is known when the stamp equals the
// current generation.
//
// The ordering that makes it sound. The load (InitializeGameSystems) writes
// the clock stamp g_lastGameLoad FIRST and then bumps the generation (release);
// a scan reads the generation FIRST (acquire) and then the clock (in
// IsExtraListStable). So a scan that read the NEW generation also sees the new
// clock stamp, finds the lists unstable, and stamps nothing. A scan that read
// the OLD generation stamps at most that old generation, which can never
// equal the new one. A degraded scan therefore never counts as detail for a
// load it ran inside. tests/core/InstanceDetailTests.cpp walks every
// interleaving of the two pairs of steps.
//
// Pure: standard library only.

#include <cstdint>

namespace Huginn::Core
{
    struct InstanceDetail
    {
        /// No detailed scan since construction, or the last rebuild was degraded.
        static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

        /// The stamp after a reconcile. `scanGeneration` was read before the
        /// stability check; `stable` is that check. A degraded reconcile only
        /// adds records (it removes none), so it leaves the stamp alone.
        [[nodiscard]] static constexpr std::uint32_t AfterReconcile(std::uint32_t stamp, std::uint32_t scanGeneration,
            bool stable) noexcept
        {
            return stable ? scanGeneration : stamp;
        }

        /// The stamp after a rebuild, which replaces every record: detailed
        /// only if this scan was.
        [[nodiscard]] static constexpr std::uint32_t AfterRebuild(std::uint32_t scanGeneration, bool stable) noexcept
        {
            return stable ? scanGeneration : kNone;
        }

        /// Detail is known for the load in progress.
        [[nodiscard]] static constexpr bool Known(std::uint32_t stamp, std::uint32_t loadGeneration) noexcept
        {
            return stamp != kNone && stamp == loadGeneration;
        }

        /// The pass that moved the stamp onto the current load: the gate
        /// (core/WheelUidGate.h) has just opened, and the page must be
        /// re-allocated even if the reconcile itself changed nothing.
        [[nodiscard]] static constexpr bool Arrived(std::uint32_t before, std::uint32_t after,
            std::uint32_t loadGeneration) noexcept
        {
            return !Known(before, loadGeneration) && Known(after, loadGeneration);
        }
    };
}
