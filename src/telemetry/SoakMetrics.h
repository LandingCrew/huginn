#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Huginn::Telemetry
{
    // =========================================================================
    // SOAK METRICS
    // =========================================================================
    // Long-play soak telemetry. Accumulates recommendation-quality and perf
    // counters over a rolling window and emits one [Soak] heartbeat summary
    // line per window (Config::SOAK_HEARTBEAT_INTERVAL_MS). Sampled across a
    // multi-hour session, the heartbeat line shows whether Huginn is surfacing
    // what the player actually reaches for (accept%), whether it thrashes
    // (recompute count), whether learning grows unbounded (FQL item count), and
    // whether per-tick cost holds (avg/peak).
    //
    // Thread-safety: RecordEquipCase fires on the game thread (equip events);
    // RecordPipelineRun / RecordTick fire on the update thread. All counters are
    // atomics. The heartbeat is emitted only from RecordTick (update thread), so
    // the window-roll and reset are single-threaded.
    //
    // With Tracy enabled (-DHuginn_TRACY=ON) the same values are also emitted as
    // TracyPlot time series for charting drift directly in the profiler.
    // =========================================================================

    class SoakMetrics
    {
    public:
        static SoakMetrics& GetSingleton();

        // Classify an attributed external equip by its ExternalEquipLearner case
        // label ('A'..'E'). E = Huginn displayed it and the player equipped it
        // (a hit); A = player equipped something never surfaced (a miss).
        void RecordEquipCase(char caseClass);

        // Count an external equip that a ShouldSkip filter caught BEFORE
        // attribution, so it never reached RecordEquipCase. Codes are
        // ExternalEquipLearner::SKIP_* (lowercase, so they cannot be confused
        // with the 'A'..'E' case labels above).
        //
        // Without this the heartbeat cannot distinguish "the player equipped
        // nothing this window" from "the player equipped a dozen things and
        // every one was filtered" — both print accept=n/a. The second is the
        // NORMAL case for a player who equips through the Huginn wheel: the
        // wheel-open filter fires on every activation by design, because
        // grading Huginn on items the player picked off Huginn's own
        // recommendation list would be circular. Measured 2026-08-26: a 44-min
        // session logged 21 external-equip events, all skipped (wheel open),
        // and reported accept=n/a for its whole duration with no way to tell
        // from the heartbeat alone that the path had run at all.
        void RecordEquipSkip(char reasonCode);

        // One pipeline recompute produced `candidateCount` scored candidates and
        // `displayedCount` widget items; `overrideActive` = a safety override was
        // the top recommendation this run.
        void RecordPipelineRun(std::size_t candidateCount, std::size_t displayedCount,
            bool overrideActive);

        // A pipeline tick was abandoned because a page switch landed mid-tick
        // (PipelineCoordinator::AllocateAndLock). Expected ~0; a nonzero heartbeat
        // value means the display-page race path actually executes on this build.
        void RecordPageRaceBail();

        // Called every update tick with the measured whole-tick duration (ms).
        // Rolls the window and emits the heartbeat when the interval elapses.
        void RecordTick(float tickMs, std::chrono::steady_clock::time_point now);

    private:
        SoakMetrics() = default;
        ~SoakMetrics() = default;
        SoakMetrics(const SoakMetrics&) = delete;
        SoakMetrics& operator=(const SoakMetrics&) = delete;

        void EmitHeartbeat(std::chrono::steady_clock::time_point now);

        std::atomic<bool>    m_started{false};
        std::atomic<int64_t> m_processStart{0};  // steady_clock epoch ticks
        std::atomic<int64_t> m_windowStart{0};

        // Recommendation-quality: external-equip attribution buckets (window).
        std::atomic<uint32_t> m_hit{0};    // E: displayed, current page
        std::atomic<uint32_t> m_near{0};   // C/D: near-miss or other page
        std::atomic<uint32_t> m_miss{0};   // B: candidate but low-ranked
        std::atomic<uint32_t> m_novel{0};  // A: not a candidate at all

        // External equips filtered before attribution (window). These are NOT
        // failures — wheel-open is the expected outcome for a wheel activation.
        // They exist to make accept=n/a self-explaining.
        std::atomic<uint32_t> m_skipWheel{0};  // w: Huginn/Wheeler wheel was open
        std::atomic<uint32_t> m_skipStale{0};  // s: pipeline snapshot too old to attribute
        std::atomic<uint32_t> m_skipSpam{0};   // a: same FormID re-equipped too soon
        std::atomic<uint32_t> m_skipOff{0};    // x: external-equip learning disabled

        // Pipeline / perf (window).
        std::atomic<uint32_t> m_ticks{0};
        std::atomic<uint32_t> m_recomputes{0};
        std::atomic<uint32_t> m_overrideRuns{0};
        std::atomic<uint32_t> m_pageRaceBails{0};  // ticks abandoned to a mid-tick page switch
        std::atomic<uint64_t> m_tickSumMicros{0};
        std::atomic<uint32_t> m_tickPeakMicros{0};
    };
}
