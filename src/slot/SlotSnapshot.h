#pragma once

#include "SlotAssignment.h"
#include "SlotConfig.h"
#include "core/SlotAllocCore.h"
#include "core/SlotSnapshotIO.h"
#include "learning/ScoredCandidate.h"
#include "override/OverrideConditions.h"
#include "state/PlayerActorState.h"
#include <chrono>
#include <cstdint>
#include <vector>

namespace Huginn::Slot
{
    // =========================================================================
    // SLOT SNAPSHOT -- the game's types as the slot core's plain records
    // =========================================================================
    // SlotAllocator runs Core::SlotAlloc (src/core/SlotAllocCore.h) on an
    // Input built here: per candidate the score the slot code ranks on (the
    // bridge, ScoredCandidate::SlotScore), which of the layout's slot classes
    // it matches (SlotClassifier::Matches), its class-cap class
    // (SlotClassCap::ClassOf) and the equipped flags; per override the same,
    // plus whether it is a vital pinned to its configured slot.
    //
    // Debug builds can also CAPTURE every allocation -- this Input and the page
    // it produced -- to <SKSE log folder>/Huginn_SlotSnapshots.txt, the
    // recorded snapshots the golden test replays (tests/core/fixtures/slots/).
    // Inert unless switched on: in test mode by iCaptureSlotsSec (TestHarness.h),
    // which also plays a scripted session meanwhile (SlotCapture.cpp).
    // =========================================================================

    /// Does this slot's filter accept the given override category?
    [[nodiscard]] constexpr bool AcceptsOverride(OverrideFilter filter, Override::OverrideCategory category) noexcept
    {
        switch (filter) {
            case OverrideFilter::None: return false;
            case OverrideFilter::Any:  return true;
            case OverrideFilter::HP:   return category == Override::OverrideCategory::HP;
            case OverrideFilter::MP:   return category == Override::OverrideCategory::MP;
            case OverrideFilter::SP:   return category == Override::OverrideCategory::SP;
            case OverrideFilter::Other: return category == Override::OverrideCategory::Other;
            default:                   return false;
        }
    }

    /// steady_clock as the core's nanosecond count (0 = the epoch, "never").
    [[nodiscard]] inline std::int64_t ToCoreNs(std::chrono::steady_clock::time_point t) noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    }
    [[nodiscard]] inline std::chrono::steady_clock::time_point FromCoreNs(std::int64_t ns) noexcept
    {
        return std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ns)));
    }

    /// The [SlotLocker] settings the allocation reads, read once.
    [[nodiscard]] Core::SlotAlloc::Settings ReadAllocSettings();

    /// The core's input for one allocation, without the seating memory (the
    /// allocator owns that and fills `memory`, `memoryAvailable` and
    /// `generationMatches`). `overrideIndex` receives, per core override, its
    /// index in overrides.activeOverrides (those without a candidate are left
    /// out, as the allocator always skipped them).
    [[nodiscard]] Core::SlotAlloc::Input BuildAllocInput(
        size_t pageIndex,
        std::chrono::steady_clock::time_point now,
        const std::vector<SlotConfig>& slotConfigs,
        const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides,
        const State::PlayerActorState* player,
        const Core::SlotAlloc::Settings& settings,
        std::vector<size_t>* overrideIndex);

    /// A page as the core describes it (for captures and tests).
    [[nodiscard]] std::vector<Core::SlotAlloc::PageSlot> PageOf(const SlotAssignments& assignments);

    namespace Capture
    {
        /// Capturing now (Debug builds only; always false in Release).
        [[nodiscard]] bool Enabled() noexcept;

        /// Start or stop writing snapshots. Starting truncates the file.
        /// False when capture could not start (no log folder, file not writable).
        bool SetEnabled(bool on);

        /// A capture session is playing (test mode): its scripted presses are
        /// not the player's, so the selection log leaves them out.
        [[nodiscard]] bool SessionActive() noexcept;

        /// Append one snapshot (thread-safe). No-op when not capturing.
        void Write(const Core::SlotAlloc::Snapshot& snap);

        /// Snapshots written since capture started.
        [[nodiscard]] size_t Count() noexcept;

        /// The tag this thread's snapshots carry ("tick" when none is set).
        void SetThreadTag(const char* tag) noexcept;
        [[nodiscard]] const char* ThreadTag() noexcept;

        /// Remember a real allocation's candidate list for the capture
        /// session's perturbation campaign (SlotCapture.cpp).
        void NoteRealList(size_t pageIndex, const Scoring::ScoredCandidateList& candidates,
            const std::vector<SlotConfig>& slotConfigs);

        /// Test mode: play a scripted session for `seconds` while capturing
        /// (slot presses, page flips, vitals dropped into the override range,
        /// under the shipped [SlotLocker] settings and two variants), then run
        /// the perturbation campaign on the main thread, then call `done`
        /// (any thread). Calls `done` at once if capture cannot start.
        void StartSession(int seconds, void (*done)());
    }
}  // namespace Huginn::Slot
