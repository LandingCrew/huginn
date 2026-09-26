#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string_view>

namespace Huginn::Telemetry
{
    // =========================================================================
    // SLOT CHURN
    // =========================================================================
    // Why the item SHOWN in a slot changed from one pipeline run to the next.
    // Classified by SlotLocker::ApplyLocks, which is the one place that sees a
    // slot's content before and after, locks and dedup included.
    //
    // The causes are "which gate was open", not a full causal chain: when an
    // override claims slot 6 and the item it evicted lands in slot 7, slot 6
    // reads Override and slot 7 reads whatever released slot 7's lock. Chains
    // like that are read from the per-change debug lines, not from the counts.
    enum class SlotChange : uint8_t
    {
        Fill,      // empty -> item
        Clear,     // item -> empty (the allocator had nothing for it)
        Dedup,     // item -> empty because DedupePreferLocked cleared it
        Override,  // replaced by an override assignment
        Wildcard,  // a wildcard arriving or leaving -- scheduled exploration,
                   // not the ranking, so it never feeds the challenger ratio
        Expired,   // replaced after the slot's lock ran out
        Used,      // replaced after OnItemUsed released the slot
        Page,      // page switch (UnlockAll) -- player-driven, kept out of peak
        Unheld,    // replaced with no lock ever in the way (locking disabled)
        Count
    };

    [[nodiscard]] constexpr std::string_view SlotChangeName(SlotChange c) noexcept
    {
        switch (c) {
        case SlotChange::Fill:     return "fill";
        case SlotChange::Clear:    return "clear";
        case SlotChange::Dedup:    return "dedup";
        case SlotChange::Override: return "override";
        case SlotChange::Wildcard: return "wildcard";
        case SlotChange::Expired:  return "expired";
        case SlotChange::Used:     return "used";
        case SlotChange::Page:     return "page";
        case SlotChange::Unheld:   return "unheld";
        default:                   return "?";
        }
    }

    // Pure classification of one slot's content change, so the precedence can
    // be tested without a live locker (static_asserts in SoakMetrics.cpp). `released` is the reason the slot's last
    // lock let go (Page, Expired, Used, Override), or Unheld if none was
    // recorded since the slot was last locked.
    //
    // Precedence: a page switch explains everything on the page; then the two
    // transitions through empty, which are what the player sees regardless of
    // what allowed them; then an override; then a wildcard at either end (its
    // arrival scores below the item it evicts by design, and the item that
    // returns when it leaves scores above it -- counting either as a ranking
    // change would pollute both ends of the ratio); then whatever released
    // the lock.
    [[nodiscard]] constexpr SlotChange ClassifySlotChange(bool wasEmpty, bool nowEmpty,
        bool dedupCleared, bool nowOverride, bool wildcardInvolved, SlotChange released) noexcept
    {
        if (released == SlotChange::Page) return SlotChange::Page;
        if (nowEmpty) return dedupCleared ? SlotChange::Dedup : SlotChange::Clear;
        if (wasEmpty) return SlotChange::Fill;
        if (nowOverride) return SlotChange::Override;
        if (wildcardInvolved) return SlotChange::Wildcard;
        switch (released) {
        case SlotChange::Expired:
        case SlotChange::Used:
        case SlotChange::Override:
            return released;
        default:
            return SlotChange::Unheld;
        }
    }

    // Mirrors Slot::MAX_SLOTS_PER_PAGE without pulling SlotSettings.h into
    // telemetry; SlotLocker.cpp static_asserts the two agree.
    inline constexpr std::size_t SLOT_CHURN_SLOTS = 10;

    // How much better the challenger scored than the item it replaced, for the
    // changes a challenger margin would govern (Expired, Unheld): the item
    // that took the slot, over the displaced item's utility in the SAME run.
    // The distribution is what sizes the margin -- a margin of m would have
    // blocked every change below 1+m whose incumbent was still a candidate.
    // Below 1.0 means the slot changed to something that scored WORSE: the
    // allocator moving items for reasons of its own (seating, refill,
    // classification), not the ranking.
    enum class ChallengerRatio : uint8_t
    {
        Gone,       // incumbent no longer a candidate: nothing to hold on to
        Below1,     // challenger scored lower than the incumbent
        Below110,   // < 10% better
        Below125,   // 10-25%
        Below150,   // 25-50%
        Above150,   // 50%+ (or incumbent scored <= 0)
        NotApplicable,
        Count = NotApplicable
    };

    [[nodiscard]] constexpr std::string_view ChallengerRatioName(ChallengerRatio r) noexcept
    {
        switch (r) {
        case ChallengerRatio::Gone:     return "gone";
        case ChallengerRatio::Below1:   return "<1";
        case ChallengerRatio::Below110: return "<1.1";
        case ChallengerRatio::Below125: return "<1.25";
        case ChallengerRatio::Below150: return "<1.5";
        case ChallengerRatio::Above150: return ">=1.5";
        default:                        return "n/a";
        }
    }

    // incumbentUtility < 0 means the incumbent is no longer a candidate.
    [[nodiscard]] constexpr ChallengerRatio BucketChallengerRatio(
        float challengerUtility, float incumbentUtility) noexcept
    {
        if (incumbentUtility < 0.0f) return ChallengerRatio::Gone;
        if (incumbentUtility == 0.0f) return ChallengerRatio::Above150;
        const float r = challengerUtility / incumbentUtility;
        if (r < 1.0f) return ChallengerRatio::Below1;
        if (r < 1.10f) return ChallengerRatio::Below110;
        if (r < 1.25f) return ChallengerRatio::Below125;
        if (r < 1.50f) return ChallengerRatio::Below150;
        return ChallengerRatio::Above150;
    }

    struct SlotChangeEvent
    {
        std::size_t slotIndex = 0;
        SlotChange cause = SlotChange::Unheld;
        ChallengerRatio ratio = ChallengerRatio::NotApplicable;
    };

    // =========================================================================
    // SOAK METRICS
    // =========================================================================
    // Long-play soak telemetry. Accumulates recommendation-quality and perf
    // counters over a rolling window and emits one [Soak] heartbeat summary
    // line per window (Config::SOAK_HEARTBEAT_INTERVAL_MS). Sampled across a
    // multi-hour session, the heartbeat line shows whether Huginn is surfacing
    // what the player actually reaches for (accept%), whether it thrashes
    // (recompute count), whether learning grows unbounded (learner item count), and
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

        // One ApplyLocks run's slot content changes (possibly none). Feeds the
        // heartbeat's churn field and, per run, the "Huginn/Slot Changes" plot.
        // Called on the update thread; the burst tracking behind peak5s is
        // guarded by its own mutex rather than relying on that.
        void RecordSlotChanges(std::span<const SlotChangeEvent> changes,
            std::chrono::steady_clock::time_point now);

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

        // Slot churn (window). Per-cause totals, plus the worst burst: the most
        // changes any one slot took inside SLOT_CHURN_BURST_MS. That burst is
        // the number that matches what the player sees -- "slot 6 changed four
        // times in a few seconds" -- which a window total averages away.
        // Page switches are counted but kept out of the burst: the player
        // asked for those.
        static constexpr int64_t SLOT_CHURN_BURST_MS = 5000;
        std::array<std::atomic<uint32_t>, static_cast<std::size_t>(SlotChange::Count)> m_slotChanges{};
        std::array<std::atomic<uint32_t>, static_cast<std::size_t>(ChallengerRatio::Count)> m_challengerRatios{};
        std::mutex m_churnMutex;
        std::array<std::deque<int64_t>, SLOT_CHURN_SLOTS> m_recentChanges;  // steady_clock ticks, per slot
        uint32_t m_churnPeak = 0;       // guarded by m_churnMutex
        std::size_t m_churnPeakSlot = 0;
    };
}
