#pragma once

// =============================================================================
// LEARNING CLOCK (0.24.22) -- one count of the learning, and the load's decision
// =============================================================================
// Learning survives a reload (0.22.11, decided with the user): a load of the
// character in memory keeps the in-memory learning unless the save is AHEAD
// of it. "Ahead" is this clock: one count, saved in the cosave's HCID record
// beside the character ID, that every change to the learning a save writes
// ticks -- the choice learner's THTA / BIAS / DISC, the old learner's BNDW
// (until R12) and a reset. A load sets it (DecideLoad); nothing else does.
//
// Until 0.24.22 the clock was the old learner's, which only its own updates
// ticked. A change only the choice learner saw (an armour equip, a v3-only
// pick; a new item's discovery boost) left it still, so a later save carried
// the same clock as an earlier one, and a fresh session that loaded the
// earlier save and then the later one kept memory and lost the later save's
// learning. One clock, outside both learners, fixes that and
// outlives the old learner: R12 deletes its two tick calls and moves the
// save/load callback registration (SaveCharacterRecord, LoadCharacterRecord,
// ResetPendingCharacter, called from BanditSerializer's SKSE callbacks under
// its unique ID 'QCNO') out of BanditSerializer. Nothing of the decision
// changes, but the reset sites must then tick BEFORE the reset (or under the
// learner's lock): today the old learner's Clear ticks first, so a save taken
// between ResetChoiceLearning and the site's own tick already carries a
// clock past the save before the reset. Without that Clear, such a save
// would pair the reset learning with the old clock, and a fresh session that
// loaded the pre-reset save and then that one would keep memory and lose
// the reset.
//
// What ticks is a table (kEvents), so the host tests read the same list the
// plugin calls through (Note). The rule: a change to what the next save
// writes, taught by play, or a reset. A load's own changes do NOT tick -- the
// decision sets the clock for them, and a tick there would leave memory one
// past the save it loaded, so the next, later save of that session would read
// as "not ahead" (the bug above again). Settings (a hot reload) are not
// learning, and the play clock's passing is not either: the forget sweep's
// erasures (b entries past their life, discovery boosts run out) re-occur on
// their own. A save made after a sweep can carry the clock of one made before
// it, so a load of the earlier then the later save keeps memory, and memory's
// entries keep memory's ages (the earlier save's), not the later save's: they
// are not forgotten at the next sweep, but once they age past their life,
// which can be later by up to the play between the two saves. Nothing the
// player taught is lost by not ticking them.
//
// Threads: the update thread (picks, onsets; the equip bus), the
// container-event thread, the console and the dMenu reset (a mod event, any
// thread) tick it; the game thread reads it at a save and sets it at a load.
// One atomic. Note is the only tick: ChoiceLearner calls through it at every
// site; the old learner ticks only in Update and Clear (its ForgetFaded and
// load imports do not).
// Pure: standard library only, host-tested in tests/core/LearningClockTests.cpp.

#include "core/ChoiceCosave.h"   // LoadDecision

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Huginn::Core::LearningClock
{
    class Clock
    {
    public:
        [[nodiscard]] std::uint64_t Get() const noexcept { return m_count.load(std::memory_order_acquire); }
        /// A load's decision (DecideLoad's `clock`); nothing else sets it.
        void Set(std::uint64_t count) noexcept { m_count.store(count, std::memory_order_release); }
        /// Returns the count after the tick.
        std::uint64_t Tick() noexcept { return m_count.fetch_add(1, std::memory_order_acq_rel) + 1; }

    private:
        std::atomic<std::uint64_t> m_count{ 0 };
    };

    /// What happened to the learning. The table below says which tick.
    enum class Event : std::uint8_t
    {
        // ---- tick: play or a reset changed what the next save writes ----
        PickLearned,        // ChoiceLearner::OnPick moved theta / b (THTA, BIAS): the equip bus and v3-only picks
        BoostEnded,         // OnPick: an item's first pick ended its discovery boost (DISC)
        NeedGrown,          // OnPipelineRun: an answered onset grew theta's variances (THTA)
        NewlyGained,        // OnGained: a form new to the character, boosted (DISC)
        Reset,              // Reset Learned Weights, `hg reset weights`, `hg reset all`: at the reset site
                            // (with the old learner present a reset ticks twice: its Clear, then the site)
        OldLearnerUpdate,   // the old learner's Update (BNDW) -- R12 deletes the call
        OldLearnerClear,    // the old learner's Clear (BNDW) -- R12 deletes the call
        // ---- no tick ----
        BForgotten,         // MaybeForgetFaded: b entries past their life erased (BIAS) -- re-occurs on its own
        BoostsExpired,      // MaybeForgetFaded: discovery boosts ran out (DISC) -- re-occurs on its own
        RepeatPick,         // the same decision again (the timeline only; not saved)
        UnusablePick,       // a pick the update cannot use
        Transient,          // the timeline, the Plackett-Luce memory, the onset counter (not saved)
        Rebootstrap,        // ResetToBootstrap: a load's (new game, replace); a reset ticks at its site
        LoadImport,         // ImportEntries / ImportB / ImportDiscovery: a load's records
        LoadReplaceDynamic, // ReplaceDynamicB / ReplaceDynamicDiscovery: a reload's dynamic forms
        LoadClearDiscovery, // ClearDiscovery: a new character
        LoadPrime,          // PrimeDiscovery: what the load holds, seen
        LoadSuspend,        // SuspendDiscovery: the revert before a load
        Settings,           // SetSettings / SetMemoryLife / SetFavoritesBoost (a hot reload)
        PlayTime,           // AdvancePlayTime (the battery decays at read time)
    };

    struct EventRow
    {
        Event event;
        bool ticks;
        const char* name;
    };

    inline constexpr std::array kEvents{
        EventRow{ Event::PickLearned, true, "pick learned" },
        EventRow{ Event::BoostEnded, true, "discovery boost ended by a pick" },
        EventRow{ Event::NeedGrown, true, "answered onset grew theta" },
        EventRow{ Event::NewlyGained, true, "form newly gained" },
        EventRow{ Event::Reset, true, "reset" },
        EventRow{ Event::OldLearnerUpdate, true, "old learner update" },
        EventRow{ Event::OldLearnerClear, true, "old learner clear" },
        EventRow{ Event::BForgotten, false, "b entries forgotten" },
        EventRow{ Event::BoostsExpired, false, "discovery boosts expired" },
        EventRow{ Event::RepeatPick, false, "repeat pick" },
        EventRow{ Event::UnusablePick, false, "unusable pick" },
        EventRow{ Event::Transient, false, "transient state" },
        EventRow{ Event::Rebootstrap, false, "back to the bootstrap" },
        EventRow{ Event::LoadImport, false, "load: records imported" },
        EventRow{ Event::LoadReplaceDynamic, false, "reload: dynamic forms from the save" },
        EventRow{ Event::LoadClearDiscovery, false, "load: discovery cleared" },
        EventRow{ Event::LoadPrime, false, "load: discovery primed" },
        EventRow{ Event::LoadSuspend, false, "load: discovery suspended" },
        EventRow{ Event::Settings, false, "settings" },
        EventRow{ Event::PlayTime, false, "play time" },
    };

    /// The table is in the enum's order, one row per event.
    [[nodiscard]] constexpr bool TableInOrder() noexcept
    {
        for (std::size_t i = 0; i < kEvents.size(); ++i) {
            if (static_cast<std::size_t>(kEvents[i].event) != i) return false;
        }
        return static_cast<std::size_t>(Event::PlayTime) + 1 == kEvents.size();
    }
    static_assert(TableInOrder(), "kEvents: one row per Event, in the enum's order");

    [[nodiscard]] constexpr bool Ticks(Event e) noexcept { return kEvents[static_cast<std::size_t>(e)].ticks; }
    [[nodiscard]] constexpr const char* EventName(Event e) noexcept { return kEvents[static_cast<std::size_t>(e)].name; }

    /// Every tick goes through here (ChoiceLearner calls it at every learning
    /// site; the old learner in Update and Clear). Ticks `clock` if the event
    /// does (a null clock: a learner built without one, the Debug suite's).
    /// Returns whether it ticked.
    inline bool Note(Clock* clock, Event e) noexcept
    {
        if (!clock || !Ticks(e)) return false;
        clock->Tick();
        return true;
    }

    // =========================================================================
    // THE LOAD'S DECISION (moved out of BanditSerializer, 0.24.22)
    // =========================================================================
    using LoadDecision = ChoiceCosave::LoadDecision;

    /// Why DecideLoad decided what it did (the [Cosave] line).
    enum class LoadWhy : std::uint8_t
    {
        NewGame,          // a fresh character
        Failed,           // the load restored nothing
        NotAhead,         // the character in memory, the save's clock at or behind memory's
        Unidentified,     // no readable HCID record: a different character, given a new ID
        FirstLoad,        // nothing in memory yet (the first load since launch)
        OtherCharacter,   // another character's save
        SaveAhead,        // a later save of the character in memory
    };

    [[nodiscard]] constexpr const char* LoadWhyName(LoadWhy w) noexcept
    {
        switch (w) {
        case LoadWhy::NewGame: return "new game";
        case LoadWhy::Failed: return "failed load";
        case LoadWhy::NotAhead: return "the save is not ahead of memory";
        case LoadWhy::Unidentified: return "unidentified save";
        case LoadWhy::FirstLoad: return "first load";
        case LoadWhy::OtherCharacter: return "different character";
        case LoadWhy::SaveAhead: return "save is ahead of memory";
        }
        return "?";
    }

    struct LoadInput
    {
        bool isNewGame = false;
        bool loadSucceeded = true;
        std::uint64_t activeID = 0;                  ///< the character in memory (0 = none yet)
        std::optional<std::uint64_t> loadedID;       ///< the save's HCID (none: no readable record)
        std::optional<std::uint64_t> loadedClock;    ///< ... and its clock
        std::uint64_t memClock = 0;                  ///< the clock now
    };

    struct LoadVerdict
    {
        LoadDecision decision = LoadDecision::Failed;
        LoadWhy why = LoadWhy::Failed;
        std::uint64_t clock = 0;   ///< what the clock becomes
        bool newID = false;        ///< the caller assigns a fresh character ID (else: Replace takes loadedID)
    };

    /// What the learning becomes after a new game or a load. Learning survives
    /// a reload: the fight that killed you is not forgotten.
    ///   new game        fresh learning, a new ID, clock 0
    ///   failed load     nothing changes (so the next real load still matches)
    ///   same character, the save's clock <= memory's: Reload (keep memory;
    ///                   the learners take only dynamic forms from the save)
    ///   anything else   Replace: the save's learning and clock -- an
    ///                   unidentified save (new ID, clock 0), the first load
    ///                   since launch, another character, a later save
    [[nodiscard]] constexpr LoadVerdict DecideLoad(const LoadInput& in) noexcept
    {
        if (in.isNewGame) return { LoadDecision::NewGame, LoadWhy::NewGame, 0, true };
        if (!in.loadSucceeded) return { LoadDecision::Failed, LoadWhy::Failed, in.memClock, false };
        if (!in.loadedID || !in.loadedClock) return { LoadDecision::Replace, LoadWhy::Unidentified, 0, true };
        const std::uint64_t saved = *in.loadedClock;
        if (in.activeID == 0) return { LoadDecision::Replace, LoadWhy::FirstLoad, saved, false };
        if (*in.loadedID != in.activeID) return { LoadDecision::Replace, LoadWhy::OtherCharacter, saved, false };
        if (saved > in.memClock) return { LoadDecision::Replace, LoadWhy::SaveAhead, saved, false };
        return { LoadDecision::Reload, LoadWhy::NotAhead, in.memClock, false };
    }
}
