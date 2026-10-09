#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include "core/SlotScoreMath.h"
#include <optional>
#include <span>
#include <string>
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
        Remembrance,  // a Remembrance hold arriving or leaving -- the player's
                      // own press, not the ranking.
        Seated,    // seating moved the slot's locked item to another slot (a
                   // home-key return, a guest going home), releasing the
                   // lock. Last, so the existing columns keep their order.
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
        case SlotChange::Remembrance: return "remembrance";
        case SlotChange::Seated:   return "seated";
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
    // the lock. A Remembrance hold at either end ranks with the wildcard, and
    // ahead of it: the player pressed that key.
    [[nodiscard]] constexpr SlotChange ClassifySlotChange(bool wasEmpty, bool nowEmpty,
        bool dedupCleared, bool nowOverride, bool wildcardInvolved, SlotChange released,
        bool remembranceInvolved = false) noexcept
    {
        if (released == SlotChange::Page) return SlotChange::Page;
        if (nowEmpty) return dedupCleared ? SlotChange::Dedup : SlotChange::Clear;
        if (wasEmpty) return SlotChange::Fill;
        if (nowOverride) return SlotChange::Override;
        if (remembranceInvolved) return SlotChange::Remembrance;
        if (wildcardInvolved) return SlotChange::Wildcard;
        switch (released) {
        case SlotChange::Expired:
        case SlotChange::Used:
        case SlotChange::Override:
        case SlotChange::Remembrance:
        case SlotChange::Seated:
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
    // that took the slot against the displaced item in the SAME run, as
    // exp(score difference) -- the old utility ratio under the R7 bridge.
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

    // On SCORES (the slot code's, any sign): the ratio is exp(challenger -
    // incumbent), bucketed on the difference (Core::BucketLogRatio). No
    // incumbent (it is no longer a candidate) is an empty optional -- the old
    // -1 utility sentinel would be a valid score now. An unranked incumbent
    // (-inf, the old utility 0) counts as 50%+.
    [[nodiscard]] inline ChallengerRatio BucketChallengerRatio(
        Core::SlotScore challengerScore, std::optional<Core::SlotScore> incumbentScore) noexcept
    {
        static_assert(static_cast<int>(Core::LogRatioBucket::Gone) == static_cast<int>(ChallengerRatio::Gone) &&
                      static_cast<int>(Core::LogRatioBucket::Below1) == static_cast<int>(ChallengerRatio::Below1) &&
                      static_cast<int>(Core::LogRatioBucket::Below110) == static_cast<int>(ChallengerRatio::Below110) &&
                      static_cast<int>(Core::LogRatioBucket::Below125) == static_cast<int>(ChallengerRatio::Below125) &&
                      static_cast<int>(Core::LogRatioBucket::Below150) == static_cast<int>(ChallengerRatio::Below150) &&
                      static_cast<int>(Core::LogRatioBucket::Above150) == static_cast<int>(ChallengerRatio::Above150));
        return static_cast<ChallengerRatio>(Core::BucketLogRatio(challengerScore, incumbentScore));
    }

    // How long the item a change replaced had been on the slot. The churn
    // SPEED: a window of changes says how much the bar moves, the tenure of
    // what it threw away says how fast -- the "I pressed my staff and got a
    // spell" juggling is a pile of short tenures (the user, 2026-10-05: give
    // churn speed banded increments to optimise for).
    enum class TenureBand : uint8_t
    {
        Under1s, Under3s, Under10s, Under30s, Under2m, Over2m,
        Count
    };

    [[nodiscard]] constexpr const char* TenureBandName(TenureBand b) noexcept
    {
        switch (b) {
        case TenureBand::Under1s:  return "<1s";
        case TenureBand::Under3s:  return "1-3s";
        case TenureBand::Under10s: return "3-10s";
        case TenureBand::Under30s: return "10-30s";
        case TenureBand::Under2m:  return "30s-2m";
        case TenureBand::Over2m:   return ">=2m";
        default:                   return "?";
        }
    }

    [[nodiscard]] constexpr TenureBand BucketTenure(float seconds) noexcept
    {
        if (seconds < 1.0f) return TenureBand::Under1s;
        if (seconds < 3.0f) return TenureBand::Under3s;
        if (seconds < 10.0f) return TenureBand::Under10s;
        if (seconds < 30.0f) return TenureBand::Under30s;
        if (seconds < 120.0f) return TenureBand::Under2m;
        return TenureBand::Over2m;
    }

    // How long a key had held its item when it was pressed (the user,
    // 2026-10-05). A young key is one of two things: a MISSED press -- the
    // player aimed at what was there a moment ago -- or a very RELEVANT one:
    // Huginn put it there and the player took it at once. The heartbeat counts
    // the bands; a "[KeyAge]" debug line per young press names what the key
    // held before, and the selection's outcome (confirmed, or not) beside it
    // tells the two apart.
    enum class PressAgeBand : uint8_t
    {
        Under500ms, Under1500ms, Under5s, Under30s, Over30s,
        Count
    };

    [[nodiscard]] constexpr const char* PressAgeBandName(PressAgeBand b) noexcept
    {
        switch (b) {
        case PressAgeBand::Under500ms:  return "<0.5s";
        case PressAgeBand::Under1500ms: return "0.5-1.5s";
        case PressAgeBand::Under5s:     return "1.5-5s";
        case PressAgeBand::Under30s:    return "5-30s";
        case PressAgeBand::Over30s:     return ">=30s";
        default:                        return "?";
        }
    }

    [[nodiscard]] constexpr PressAgeBand BucketPressAge(float seconds) noexcept
    {
        if (seconds < 0.5f) return PressAgeBand::Under500ms;
        if (seconds < 1.5f) return PressAgeBand::Under1500ms;
        if (seconds < 5.0f) return PressAgeBand::Under5s;
        if (seconds < 30.0f) return PressAgeBand::Under30s;
        return PressAgeBand::Over30s;
    }

    struct SlotChangeEvent
    {
        std::size_t slotIndex = 0;
        SlotChange cause = SlotChange::Unheld;
        ChallengerRatio ratio = ChallengerRatio::NotApplicable;
        float tenureSec = -1.0f;   // how long the replaced item was shown; < 0 = the slot was empty
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
        // every one was filtered" — both print accept=n/a. (The wheel-open
        // filter that made this the normal case for a wheel player is gone:
        // Huginn-wheel picks never reach attribution, and own-wheel picks are
        // now attributed like any other outside selection.)
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

        // Huginn's second objective metric: did the player need a workaround
        // to get what they wanted? The end state is ONE page of Regular
        // (unrestricted) slots, so a press on a classified slot ("Heal",
        // "Potion"...) or on any page past the first is a workaround, and so
        // is flipping pages. One call per slot activation that resolved to an
        // item, from the Intuition keys (EquipManager::EquipSlot) and from a
        // Huginn wheel (the Wheeler activation callback).
        //
        // `keyAgeSec` and `wasName` feed the press-age bands: how long the key
        // had shown its item, and what it showed before. Both come from ONE
        // read of SlotLocker::GetKeyAge, the single owner of that fact (code
        // review of #174: a second copy here drifted on page switches, loads
        // and same-name swaps). keyAgeSec < 0 = unknown -- a wheel page that is
        // not displayed, or a key not yet filled since the load.
        void RecordSlotPress(std::size_t pageIndex, std::size_t slotIndex, bool regularSlot,
            float keyAgeSec, std::string_view wasName);

        // A page change that actually changed the page (SlotAllocator::
        // SetCurrentPage), whoever asked: Intuition cycle keys, Wheeler, `hg page`.
        void RecordPageFlip();

        // An item came back to the displayed page within the home-key memory
        // (SlotAllocator::ApplySeating): on the key it left, waiting for it
        // (an override or Remembrance item holds it), or elsewhere. The
        // before/after measure for home keys; counted with them off as well.
        enum class ReturnOutcome : uint8_t { Home, Waiting, Away };
        void RecordReturn(ReturnOutcome outcome);

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
        std::atomic<uint32_t> m_skipInput{0};  // n: no player input behind it (a script)
        std::atomic<uint32_t> m_skipStale{0};  // s: pipeline snapshot too old to attribute
        std::atomic<uint32_t> m_skipOff{0};    // x: external-equip learning disabled

        // Workarounds (window). Regular and labeled partition the presses;
        // offPage overlaps both (a labeled slot on page 2 counts in each).
        std::atomic<uint32_t> m_pressRegular{0};
        std::atomic<uint32_t> m_pressLabeled{0};
        std::atomic<uint32_t> m_pressOffPage{0};
        std::atomic<uint32_t> m_pageFlips{0};
        std::atomic<uint32_t> m_returnsHome{0};
        std::atomic<uint32_t> m_returnsWaiting{0};
        std::atomic<uint32_t> m_returnsAway{0};

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

        // Churn speed (window): tenure bands of what each change replaced, and
        // how long a pressed key had held its item. Page switches excluded --
        // the player asked for those. A "[KeyAge]" line is logged for presses
        // younger than KEY_AGE_LOG_SEC.
        static constexpr float KEY_AGE_LOG_SEC = 5.0f;
        std::array<std::atomic<uint32_t>, static_cast<std::size_t>(TenureBand::Count)> m_tenure{};
        std::array<std::atomic<uint32_t>, static_cast<std::size_t>(PressAgeBand::Count)> m_pressAge{};
    };
}
