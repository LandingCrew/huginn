#include "SoakMetrics.h"

#include "Config.h"
#include "Globals.h"
#include "Profiling.h"
#include "learning/FeatureBanditLearner.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace Huginn::Telemetry
{
    // ClassifySlotChange's precedence, pinned at compile time.
    // Args: wasEmpty, nowEmpty, dedupCleared, nowOverride, wildcardInvolved, released.
    namespace
    {
        using enum SlotChange;
        // A page switch explains everything on the page, even an override.
        static_assert(ClassifySlotChange(false, false, false, true, false, Page) == Page);
        static_assert(ClassifySlotChange(true, false, false, false, false, Page) == Page);
        // Through empty: what the player sees, whatever allowed it.
        static_assert(ClassifySlotChange(false, true, true, false, false, Expired) == Dedup);
        static_assert(ClassifySlotChange(false, true, false, false, true, Used) == Clear);
        static_assert(ClassifySlotChange(true, false, false, true, false, Unheld) == Fill);
        // Item to item: an override names itself, over a lock that expired.
        static_assert(ClassifySlotChange(false, false, false, true, false, Expired) == Override);
        static_assert(ClassifySlotChange(false, false, false, true, true, Expired) == Override);
        // A wildcard at either end outranks whatever released the lock.
        static_assert(ClassifySlotChange(false, false, false, false, true, Expired) == Wildcard);
        static_assert(ClassifySlotChange(false, false, false, false, false, Expired) == Expired);
        static_assert(ClassifySlotChange(false, false, false, false, false, Used) == Used);
        static_assert(ClassifySlotChange(false, false, false, false, false, Seated) == Seated);
        static_assert(ClassifySlotChange(false, false, false, false, true, Seated) == Wildcard);
        static_assert(ClassifySlotChange(false, false, false, false, false, Override) == Override);
        static_assert(ClassifySlotChange(false, false, false, false, false, Unheld) == Unheld);
        // A remembrance hold names itself over a wildcard and a lock, not over
        // an override or a trip through empty.
        static_assert(ClassifySlotChange(false, false, false, false, true, Expired, true) == Remembrance);
        static_assert(ClassifySlotChange(false, false, false, true, false, Expired, true) == Override);
        static_assert(ClassifySlotChange(true, false, false, false, false, Unheld, true) == Fill);

        // BucketChallengerRatio works on scores now (std::log is not
        // constexpr); its edges are checked on the host, against the old
        // utility buckets (tests/core/SlotScoreMathTests.cpp).
    }

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

    void SoakMetrics::RecordEquipSkip(char reasonCode)
    {
        switch (reasonCode) {
        case 'n': m_skipInput.fetch_add(1, std::memory_order_relaxed); break;
        case 's': m_skipStale.fetch_add(1, std::memory_order_relaxed); break;
        case 'x': m_skipOff.fetch_add(1, std::memory_order_relaxed); break;
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

    void SoakMetrics::RecordPageRaceBail()
    {
        m_pageRaceBails.fetch_add(1, std::memory_order_relaxed);
    }

    void SoakMetrics::RecordSlotPress(std::size_t pageIndex, std::size_t slotIndex, bool regularSlot,
        float keyAgeSec, std::string_view wasName)
    {
        (regularSlot ? m_pressRegular : m_pressLabeled).fetch_add(1, std::memory_order_relaxed);
        if (pageIndex != 0) {
            m_pressOffPage.fetch_add(1, std::memory_order_relaxed);
        }
        if (keyAgeSec >= 0.0f) {
            m_pressAge[static_cast<std::size_t>(BucketPressAge(keyAgeSec))].fetch_add(1, std::memory_order_relaxed);
            if (keyAgeSec < KEY_AGE_LOG_SEC) {
                logger::debug("[KeyAge] page {} key {} pressed {:.2f}s after it changed (was '{}')"sv,
                    pageIndex, slotIndex + 1, keyAgeSec, wasName);
            }
        }
    }

    void SoakMetrics::RecordPageFlip()
    {
        m_pageFlips.fetch_add(1, std::memory_order_relaxed);
    }

    void SoakMetrics::RecordReturn(ReturnOutcome outcome)
    {
        auto& counter = outcome == ReturnOutcome::Home    ? m_returnsHome
                      : outcome == ReturnOutcome::Waiting ? m_returnsWaiting
                                                          : m_returnsAway;
        counter.fetch_add(1, std::memory_order_relaxed);
    }

    void SoakMetrics::RecordSlotChanges(std::span<const SlotChangeEvent> changes,
        std::chrono::steady_clock::time_point now)
    {
        uint32_t counted = 0;  // this run, page switches excluded
        if (!changes.empty()) {
            const int64_t nowTicks = now.time_since_epoch().count();
            const int64_t burstTicks = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(SLOT_CHURN_BURST_MS)).count();

            std::lock_guard<std::mutex> lock(m_churnMutex);
            for (const auto& change : changes) {
                m_slotChanges[static_cast<std::size_t>(change.cause)].fetch_add(1, std::memory_order_relaxed);
                if (change.ratio != ChallengerRatio::NotApplicable) {
                    m_challengerRatios[static_cast<std::size_t>(change.ratio)].fetch_add(1, std::memory_order_relaxed);
                }
                if (change.cause == SlotChange::Page || change.slotIndex >= SLOT_CHURN_SLOTS) {
                    continue;
                }
                ++counted;
                // Tenure measures juggling the player did not ask for: a change
                // the player's own press caused -- the Used backfill, a
                // Remembrance hold arriving or leaving -- is left out.
                if (change.tenureSec >= 0.0f && change.cause != SlotChange::Used &&
                    change.cause != SlotChange::Remembrance) {
                    m_tenure[static_cast<std::size_t>(BucketTenure(change.tenureSec))].fetch_add(1, std::memory_order_relaxed);
                }

                auto& recent = m_recentChanges[change.slotIndex];
                recent.push_back(nowTicks);
                while (!recent.empty() && nowTicks - recent.front() > burstTicks) {
                    recent.pop_front();
                }
                if (recent.size() > m_churnPeak) {
                    m_churnPeak = static_cast<uint32_t>(recent.size());
                    m_churnPeakSlot = change.slotIndex;
                }
            }
        }

        // Plotted every run, zeros included, so a burst in a capture stands
        // against a visible baseline rather than interpolating across gaps.
        Huginn_PLOT("Huginn/Slot Changes", static_cast<int64_t>(counted));
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
        const uint32_t skipInput = m_skipInput.exchange(0, std::memory_order_relaxed);
        const uint32_t skipStale = m_skipStale.exchange(0, std::memory_order_relaxed);
        const uint32_t skipOff   = m_skipOff.exchange(0, std::memory_order_relaxed);
        const uint32_t ticks = m_ticks.exchange(0, std::memory_order_relaxed);
        const uint32_t recomputes   = m_recomputes.exchange(0, std::memory_order_relaxed);
        const uint32_t overrideRuns = m_overrideRuns.exchange(0, std::memory_order_relaxed);
        const uint32_t pageBails    = m_pageRaceBails.exchange(0, std::memory_order_relaxed);
        const uint64_t sumMicros    = m_tickSumMicros.exchange(0, std::memory_order_relaxed);
        const uint32_t peakMicros   = m_tickPeakMicros.exchange(0, std::memory_order_relaxed);
        const uint32_t pressRegular = m_pressRegular.exchange(0, std::memory_order_relaxed);
        const uint32_t pressLabeled = m_pressLabeled.exchange(0, std::memory_order_relaxed);
        const uint32_t pressOffPage = m_pressOffPage.exchange(0, std::memory_order_relaxed);
        const uint32_t pageFlips    = m_pageFlips.exchange(0, std::memory_order_relaxed);
        const uint32_t returnsHome  = m_returnsHome.exchange(0, std::memory_order_relaxed);
        const uint32_t returnsWait  = m_returnsWaiting.exchange(0, std::memory_order_relaxed);
        const uint32_t returnsAway  = m_returnsAway.exchange(0, std::memory_order_relaxed);
        std::array<uint32_t, static_cast<std::size_t>(PressAgeBand::Count)> pressAge{};
        for (std::size_t b = 0; b < pressAge.size(); ++b) {
            pressAge[b] = m_pressAge[b].exchange(0, std::memory_order_relaxed);
        }
        std::array<uint32_t, static_cast<std::size_t>(TenureBand::Count)> tenure{};
        for (std::size_t b = 0; b < tenure.size(); ++b) {
            tenure[b] = m_tenure[b].exchange(0, std::memory_order_relaxed);
        }

        std::array<uint32_t, static_cast<std::size_t>(SlotChange::Count)> churn{};
        uint32_t churnTotal = 0;
        for (std::size_t c = 0; c < churn.size(); ++c) {
            churn[c] = m_slotChanges[c].exchange(0, std::memory_order_relaxed);
            churnTotal += churn[c];
        }
        std::array<uint32_t, static_cast<std::size_t>(ChallengerRatio::Count)> ratios{};
        for (std::size_t r = 0; r < ratios.size(); ++r) {
            ratios[r] = m_challengerRatios[r].exchange(0, std::memory_order_relaxed);
        }
        uint32_t churnPeak = 0;
        std::size_t churnPeakSlot = 0;
        {
            // The per-slot histories are NOT cleared: a burst straddling the
            // window boundary still counts in full on the side it peaks.
            std::lock_guard<std::mutex> lock(m_churnMutex);
            churnPeak = std::exchange(m_churnPeak, 0);
            churnPeakSlot = std::exchange(m_churnPeakSlot, 0);
        }

        const uint32_t totalEquips = hit + near_ + miss + novel;
        // No equips in the window: print n/a, not a fake 0% (reads as rejection)
        const float accept = 100.0f * static_cast<float>(hit) / std::max(totalEquips, 1u);
        const std::string acceptStr =
            totalEquips ? std::format("{:.0f}%", accept) : std::string("n/a");

        // Breakdown only when something was actually filtered — a bare skipped=0
        // keeps the common line short while still holding the column, so a soak
        // log stays parseable across windows.
        const uint32_t skipTotal = skipInput + skipStale + skipOff;
        const std::string skipStr = skipTotal
            ? std::format("{} (input={} stale={} off={})",
                  skipTotal, skipInput, skipStale, skipOff)
            : std::string("0");
        // Same convention as skipped=: a bare 0 when nothing changed, the full
        // breakdown (every cause, zeros included, so columns line up across
        // windows) when something did. peak5s is the worst single slot.
        std::string churnStr = "0";
        if (churnTotal) {
            churnStr = std::format("{} peak5s={}@slot{} (", churnTotal, churnPeak, churnPeakSlot);
            for (std::size_t c = 0; c < churn.size(); ++c) {
                churnStr += std::format("{}{}={}", c ? " " : "",
                    SlotChangeName(static_cast<SlotChange>(c)), churn[c]);
            }
            churnStr += ") ratio(";
            for (std::size_t r = 0; r < ratios.size(); ++r) {
                churnStr += std::format("{}{}={}", r ? " " : "",
                    ChallengerRatioName(static_cast<ChallengerRatio>(r)), ratios[r]);
            }
            churnStr += ") tenure(";
            for (std::size_t b = 0; b < tenure.size(); ++b) {
                churnStr += std::format("{}{}={}", b ? " " : "",
                    TenureBandName(static_cast<TenureBand>(b)), tenure[b]);
            }
            churnStr += ')';
        }
        // Press age rides with churn: how long each pressed key had held its
        // item. Printed whenever a key was pressed, so it shows even in a
        // window where nothing moved.
        if (pressRegular + pressLabeled) {
            churnStr += " pressAge(";
            for (std::size_t b = 0; b < pressAge.size(); ++b) {
                churnStr += std::format("{}{}={}", b ? " " : "",
                    PressAgeBandName(static_cast<PressAgeBand>(b)), pressAge[b]);
            }
            churnStr += ')';
        }
        // Returns ride with churn too: items back on the page within the
        // home-key memory, on the key they left or elsewhere.
        if (returnsHome + returnsWait + returnsAway) {
            churnStr += std::format(" returns(home={} wait={} away={})", returnsHome, returnsWait, returnsAway);
        }

        const float avgMs  = ticks ? (static_cast<float>(sumMicros) / 1000.0f / ticks) : 0.0f;
        const float peakMs = static_cast<float>(peakMicros) / 1000.0f;

        std::size_t learnerItems = 0;
        uint32_t learnerTrains = 0;
        uint64_t learnerForgot = 0;
        if (g_featureBanditLearner) {
            learnerItems  = g_featureBanditLearner->GetItemCount();
            learnerTrains = g_featureBanditLearner->GetTotalTrainCount();
            learnerForgot = g_featureBanditLearner->GetForgottenTotal();
        }

        const int64_t upTicks = now.time_since_epoch().count() -
                                m_processStart.load(std::memory_order_relaxed);
        const int64_t upSec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::duration(upTicks)).count();
        const int64_t upH = upSec / 3600;
        const int64_t upM = (upSec % 3600) / 60;
        const int64_t upS = upSec % 60;

        // The two objective metrics, last so every existing field keeps its
        // place. reachIns = every attributed external equip (the player went
        // past Huginn); candidate = the ones Huginn had scored, i.e. could have
        // offered (hit+near+miss). workaround = presses that needed a labeled
        // slot or a page past the first, plus page flips.
        const uint32_t pressTotal = pressRegular + pressLabeled;
        logger::info(
            "[Soak] up={}h{:02}m{:02}s | equips hit={} near={} miss={} novel={} accept={} skipped={} | "
            "recompute={}/{} ticks override={} pageBail={} | slotChurn={} | learn items={} trains={} forgot={} | tick avg={:.3f} peak={:.3f} ms | "
            "goals reachIns={} (candidate={}) presses={} (regular={} labeled={} offPage={}) pageFlips={}"sv,
            upH, upM, upS,
            hit, near_, miss, novel, acceptStr, skipStr,
            recomputes, ticks, overrideRuns, pageBails,
            churnStr,
            learnerItems, learnerTrains, learnerForgot,
            avgMs, peakMs,
            totalEquips, hit + near_ + miss,
            pressTotal, pressRegular, pressLabeled, pressOffPage, pageFlips);

        Huginn_PLOT("Huginn/Learner Items", static_cast<int64_t>(learnerItems));
        // Only plot windows that carry signal — zero-equip windows would drag the
        // Tracy trend line to 0 and read as rejection.
        if (totalEquips > 0) {
            Huginn_PLOT("Huginn/Accept %", static_cast<double>(accept));
        }
    }
}
