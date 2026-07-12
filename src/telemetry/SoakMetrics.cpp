#include "SoakMetrics.h"

#include "Config.h"
#include "Globals.h"
#include "Profiling.h"
#include "learning/FeatureQLearner.h"

namespace Huginn::Telemetry
{
    SoakMetrics& SoakMetrics::GetSingleton()
    {
        static SoakMetrics instance;
        return instance;
    }

    void SoakMetrics::RecordEquipCase(char caseClass)
    {
        switch (caseClass) {
        case 'E': m_hit.fetch_add(1, std::memory_order_relaxed); break;    // displayed, current page
        case 'D':                                                          // displayed, other page
        case 'C': m_near.fetch_add(1, std::memory_order_relaxed); break;   // near-miss
        case 'B': m_miss.fetch_add(1, std::memory_order_relaxed); break;   // low-ranked candidate
        case 'A': m_novel.fetch_add(1, std::memory_order_relaxed); break;  // not a candidate
        default: break;
        }
    }

    void SoakMetrics::RecordPipelineRun(std::size_t candidateCount, std::size_t displayedCount,
        bool overrideActive)
    {
        m_recomputes.fetch_add(1, std::memory_order_relaxed);
        if (overrideActive) {
            m_overrideRuns.fetch_add(1, std::memory_order_relaxed);
        }

        Huginn_PLOT("Huginn/Candidates", static_cast<int64_t>(candidateCount));
        Huginn_PLOT("Huginn/Displayed", static_cast<int64_t>(displayedCount));
    }

    void SoakMetrics::RecordTick(float tickMs, std::chrono::steady_clock::time_point now)
    {
        const int64_t nowTicks = now.time_since_epoch().count();

        // First tick: latch process/window start.
        bool expected = false;
        if (m_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            m_processStart.store(nowTicks, std::memory_order_relaxed);
            m_windowStart.store(nowTicks, std::memory_order_relaxed);
        }

        const uint32_t micros = tickMs > 0.0f ? static_cast<uint32_t>(tickMs * 1000.0f) : 0u;
        m_ticks.fetch_add(1, std::memory_order_relaxed);
        m_tickSumMicros.fetch_add(micros, std::memory_order_relaxed);

        uint32_t prevPeak = m_tickPeakMicros.load(std::memory_order_relaxed);
        while (micros > prevPeak &&
               !m_tickPeakMicros.compare_exchange_weak(prevPeak, micros, std::memory_order_relaxed)) {
        }

        // Roll the window / emit. RecordTick is update-thread-only, so this gate
        // and the reset in EmitHeartbeat are effectively single-threaded.
        const int64_t windowStart = m_windowStart.load(std::memory_order_relaxed);
        const auto elapsed = std::chrono::steady_clock::duration(nowTicks - windowStart);
        const float elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
        if (elapsedMs >= Config::SOAK_HEARTBEAT_INTERVAL_MS) {
            m_windowStart.store(nowTicks, std::memory_order_relaxed);
            EmitHeartbeat(now);
        }
    }

    void SoakMetrics::EmitHeartbeat(std::chrono::steady_clock::time_point now)
    {
        // Read-and-reset the window counters.
        const uint32_t hit   = m_hit.exchange(0, std::memory_order_relaxed);
        const uint32_t near_ = m_near.exchange(0, std::memory_order_relaxed);
        const uint32_t miss  = m_miss.exchange(0, std::memory_order_relaxed);
        const uint32_t novel = m_novel.exchange(0, std::memory_order_relaxed);
        const uint32_t ticks = m_ticks.exchange(0, std::memory_order_relaxed);
        const uint32_t recomputes   = m_recomputes.exchange(0, std::memory_order_relaxed);
        const uint32_t overrideRuns = m_overrideRuns.exchange(0, std::memory_order_relaxed);
        const uint64_t sumMicros    = m_tickSumMicros.exchange(0, std::memory_order_relaxed);
        const uint32_t peakMicros   = m_tickPeakMicros.exchange(0, std::memory_order_relaxed);

        const uint32_t totalEquips = hit + near_ + miss + novel;
        const float accept = totalEquips ? (100.0f * static_cast<float>(hit) / totalEquips) : 0.0f;
        const float avgMs  = ticks ? (static_cast<float>(sumMicros) / 1000.0f / ticks) : 0.0f;
        const float peakMs = static_cast<float>(peakMicros) / 1000.0f;

        std::size_t fqlItems = 0;
        uint32_t fqlTrains = 0;
        if (g_featureQLearner) {
            fqlItems  = g_featureQLearner->GetItemCount();
            fqlTrains = g_featureQLearner->GetTotalTrainCount();
        }

        const int64_t upTicks = now.time_since_epoch().count() -
                                m_processStart.load(std::memory_order_relaxed);
        const int64_t upSec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::duration(upTicks)).count();
        const int64_t upH = upSec / 3600;
        const int64_t upM = (upSec % 3600) / 60;
        const int64_t upS = upSec % 60;

        logger::info(
            "[Soak] up={}h{:02}m{:02}s | equips hit={} near={} miss={} novel={} accept={:.0f}% | "
            "recompute={}/{} ticks override={} | learn items={} trains={} | tick avg={:.3f} peak={:.3f} ms"sv,
            upH, upM, upS,
            hit, near_, miss, novel, accept,
            recomputes, ticks, overrideRuns,
            fqlItems, fqlTrains,
            avgMs, peakMs);

        Huginn_PLOT("Huginn/FQL Items", static_cast<int64_t>(fqlItems));
        Huginn_PLOT("Huginn/Accept %", static_cast<double>(accept));
    }
}
