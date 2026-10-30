// Host tests for src/core/LearningClock.h: the shared learning clock, its
// tick/no-tick table, and the load's keep-or-replace decision (0.24.22).

#include "core/LearningClock.h"

#include "core/Battery.h"
#include "core/DiscoveryBoost.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <thread>
#include <vector>

using namespace Huginn::Core::LearningClock;

namespace
{
    constexpr std::uint64_t kHero = 0x1234'5678'9ABC'DEF0ull;
    constexpr std::uint64_t kOther = 0x0FED'CBA9'8765'4321ull;

    /// One save's HCID: the character and the clock at the save.
    struct Save
    {
        std::uint64_t id = 0;
        std::uint64_t clock = 0;
    };

    /// What the plugin holds between loads: the clock and the active
    /// character (persist/CharacterRecord.cpp's s_activeCharacterID).
    struct Session
    {
        Clock clock;
        std::uint64_t activeID = 0;

        [[nodiscard]] Save SaveNow() const { return Save{ activeID, clock.Get() }; }

        /// DecideLoad, then what CommitLoad does with it.
        LoadVerdict Load(std::optional<Save> save, std::uint64_t freshID = 0xAAAA)
        {
            LoadInput in;
            in.activeID = activeID;
            if (save) {
                in.loadedID = save->id;
                in.loadedClock = save->clock;
            }
            in.memClock = clock.Get();
            const auto v = DecideLoad(in);
            if (v.decision != LoadDecision::Failed) {
                clock.Set(v.clock);
                activeID = v.newID ? freshID : v.decision == LoadDecision::Replace ? save->id : activeID;
            }
            return v;
        }

        LoadVerdict NewGame(std::uint64_t id)
        {
            LoadInput in;
            in.isNewGame = true;
            in.activeID = activeID;
            in.memClock = clock.Get();
            const auto v = DecideLoad(in);
            clock.Set(v.clock);
            activeID = id;
            return v;
        }

        /// Every load-path step the plugin runs after the decision (step 5's
        /// commit, step 6c): ResolveLoadedChoice's and the discovery prime.
        void RunLoadSteps()
        {
            for (const Event e : { Event::Rebootstrap, Event::LoadImport, Event::LoadReplaceDynamic,
                                   Event::LoadClearDiscovery, Event::LoadPrime, Event::Transient, Event::Settings }) {
                Note(&clock, e);
            }
        }
    };

    constexpr double kHour = 3600.0;

    /// ChoiceLearner::MaybeForgetFaded's body over the core pieces it calls
    /// (the learner itself is not host-built: it logs and takes the game's
    /// locks): the boosts that ran out (Discovery::Tracker::Expire), then the
    /// b entries past their life (Battery::Faded), each noted to the clock as
    /// the learner notes them. Returns {boosts expired, b entries forgotten}.
    struct SweepResult
    {
        std::size_t expired = 0;
        std::size_t forgotten = 0;
    };
    SweepResult Sweep(Clock& clock, Huginn::Core::Discovery::Tracker& discovery, std::map<std::uint32_t, double>& bChosenAt,
                      double now)
    {
        namespace Battery = Huginn::Core::Battery;
        SweepResult r;
        r.expired = discovery.Expire(Huginn::Core::Discovery::Params{}, now);
        if (r.expired) Note(&clock, Event::BoostsExpired);
        for (auto it = bChosenAt.begin(); it != bChosenAt.end();) {
            if (Battery::Faded(Battery::MemoryLife{}, now, it->second, 1.0f)) {
                it = bChosenAt.erase(it);
                ++r.forgotten;
            }
            else {
                ++it;
            }
        }
        if (r.forgotten) Note(&clock, Event::BForgotten);
        return r;
    }
}

TEST_CASE("learning clock: tick, set, get")
{
    Clock c;
    CHECK(c.Get() == 0);
    CHECK(c.Tick() == 1);
    CHECK(c.Tick() == 2);
    CHECK(c.Get() == 2);
    c.Set(40);
    CHECK(c.Get() == 40);
    CHECK(c.Tick() == 41);
    c.Set(0);
    CHECK(c.Get() == 0);
}

TEST_CASE("learning clock: concurrent ticks are not lost")
{
    // The update thread, the container-event thread and the console tick it.
    Clock c;
    constexpr int kThreads = 4;
    constexpr int kTicks = 20000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&c] {
            for (int i = 0; i < kTicks; ++i) Note(&c, Event::PickLearned);
        });
    }
    for (auto& th : threads) th.join();
    CHECK(c.Get() == static_cast<std::uint64_t>(kThreads) * kTicks);
}

TEST_CASE("learning clock: the table -- play and resets tick; the forget sweep, loads, settings and transients do not")
{
    for (const Event e : { Event::PickLearned, Event::BoostEnded, Event::NeedGrown, Event::NewlyGained, Event::Reset,
                           Event::OldLearnerUpdate, Event::OldLearnerClear }) {
        CAPTURE(EventName(e));
        CHECK(Ticks(e));
    }
    // the forget sweep's erasures re-occur on their own: nothing the player taught
    for (const Event e : { Event::BForgotten, Event::BoostsExpired, Event::RepeatPick, Event::UnusablePick,
                           Event::Transient, Event::Rebootstrap, Event::LoadImport, Event::LoadReplaceDynamic,
                           Event::LoadClearDiscovery, Event::LoadPrime, Event::LoadSuspend, Event::Settings,
                           Event::PlayTime }) {
        CAPTURE(EventName(e));
        CHECK_FALSE(Ticks(e));
    }
    // every row named, and the two lists above cover the table
    std::size_t ticking = 0;
    for (const auto& row : kEvents) {
        CHECK(row.name != nullptr);
        ticking += row.ticks ? 1 : 0;
    }
    CHECK(ticking == 7);
    CHECK(kEvents.size() == 20);
}

TEST_CASE("learning clock: a forget sweep that forgets b entries and expires boosts leaves the clock unchanged")
{
    namespace D = Huginn::Core::Discovery;
    Clock clock;
    D::Tracker discovery;
    discovery.Prime({});
    std::map<std::uint32_t, double> b;   // form -> chosenAt (play seconds)

    // Taught by play: a pick learned (b for A1, B2) and two forms gained.
    b[0xA1u] = 0.0;
    b[0xB2u] = 0.0;
    Note(&clock, Event::PickLearned);
    REQUIRE(discovery.OnGained(0xC1u, 0.0) == D::Tracker::Gain::New);
    Note(&clock, Event::NewlyGained);
    REQUIRE(discovery.OnGained(0xC2u, 0.0) == D::Tracker::Gain::New);
    Note(&clock, Event::NewlyGained);
    const std::uint64_t taught = clock.Get();
    REQUIRE(taught == 3);

    // An hour on: nothing due, nothing changes.
    auto r = Sweep(clock, discovery, b, 1.0 * kHour);
    CHECK(r.expired == 0);
    CHECK(r.forgotten == 0);
    const Save before{ kHero, clock.Get() };

    // A day of play on: both boosts ran out (~8.2 h) and both b entries are
    // past their life (8 h + 2 h ln 2, a 1 h knee). The sweep erased them all
    // and the clock did not move.
    r = Sweep(clock, discovery, b, 24.0 * kHour);
    CHECK(r.expired == 2);
    CHECK(r.forgotten == 2);
    CHECK(b.empty());
    CHECK(discovery.BoostedCount() == 0);
    CHECK(clock.Get() == taught);
    const Save after{ kHero, clock.Get() };

    // So a save after the sweep is not ahead of one before it: a fresh
    // session that loads the earlier save and then the later one keeps
    // memory, which forgets the same entries at its next sweep (their ages
    // travel with the save).
    Session two;
    CHECK(two.Load(before).why == LoadWhy::FirstLoad);
    two.RunLoadSteps();
    CHECK(two.Load(after).decision == LoadDecision::Reload);
    CHECK(two.clock.Get() == taught);
}

TEST_CASE("learning clock: Note ticks only the events that tick, and nothing without a clock")
{
    Clock c;
    CHECK(Note(&c, Event::NewlyGained));
    CHECK_FALSE(Note(&c, Event::LoadPrime));
    CHECK_FALSE(Note(&c, Event::PlayTime));
    CHECK(c.Get() == 1);
    CHECK_FALSE(Note(nullptr, Event::PickLearned));   // the Debug suite's own learners
}

TEST_CASE("load decision: new game and failed load")
{
    LoadInput in;
    in.isNewGame = true;
    in.activeID = kHero;
    in.loadedID = kHero;
    in.loadedClock = 7;
    in.memClock = 30;
    auto v = DecideLoad(in);
    CHECK(v.decision == LoadDecision::NewGame);
    CHECK(v.why == LoadWhy::NewGame);
    CHECK(v.clock == 0);
    CHECK(v.newID);

    in.isNewGame = false;
    in.loadSucceeded = false;
    v = DecideLoad(in);
    CHECK(v.decision == LoadDecision::Failed);
    CHECK(v.clock == 30);   // unchanged
    CHECK_FALSE(v.newID);
}

TEST_CASE("load decision: replace -- no HCID, the first load, another character, a later save")
{
    LoadInput in;
    in.activeID = kHero;
    in.memClock = 30;

    // no readable HCID: a new character, clock 0
    auto v = DecideLoad(in);
    CHECK(v.decision == LoadDecision::Replace);
    CHECK(v.why == LoadWhy::Unidentified);
    CHECK(v.clock == 0);
    CHECK(v.newID);
    in.loadedID = kHero;   // an ID without a clock is unidentified too
    CHECK(DecideLoad(in).why == LoadWhy::Unidentified);

    // the first load since launch, even of a save behind the clock in memory
    in.loadedClock = 5;
    in.activeID = 0;
    v = DecideLoad(in);
    CHECK(v.decision == LoadDecision::Replace);
    CHECK(v.why == LoadWhy::FirstLoad);
    CHECK(v.clock == 5);
    CHECK_FALSE(v.newID);

    // another character, behind memory or not
    in.activeID = kOther;
    v = DecideLoad(in);
    CHECK(v.decision == LoadDecision::Replace);
    CHECK(v.why == LoadWhy::OtherCharacter);
    CHECK(v.clock == 5);

    // the same character, the save ahead
    in.activeID = kHero;
    in.loadedClock = 31;
    v = DecideLoad(in);
    CHECK(v.decision == LoadDecision::Replace);
    CHECK(v.why == LoadWhy::SaveAhead);
    CHECK(v.clock == 31);
}

TEST_CASE("load decision: reload -- the same character, the save equal or behind")
{
    LoadInput in;
    in.activeID = kHero;
    in.loadedID = kHero;
    in.memClock = 30;
    for (const std::uint64_t saved : { std::uint64_t{ 30 }, std::uint64_t{ 29 }, std::uint64_t{ 0 } }) {
        CAPTURE(saved);
        in.loadedClock = saved;
        const auto v = DecideLoad(in);
        CHECK(v.decision == LoadDecision::Reload);
        CHECK(v.why == LoadWhy::NotAhead);
        CHECK(v.clock == 30);   // memory's
        CHECK_FALSE(v.newID);
    }
}

TEST_CASE("learning clock: a choice-only change makes the later save ahead (the 0.24.22 regression)")
{
    // Session 1: S1 at c; an armour equip (a pick only the choice learner
    // learns) ticks; S2 at c+1. Before 0.24.22 the clock was the old
    // learner's, S2 carried c too, and the second load below kept S1's
    // learning, dropping S2's.
    Session one;
    one.NewGame(kHero);
    for (int i = 0; i < 12; ++i) Note(&one.clock, Event::OldLearnerUpdate);
    const Save s1 = one.SaveNow();
    Note(&one.clock, Event::PickLearned);
    const Save s2 = one.SaveNow();
    REQUIRE(s2.clock == s1.clock + 1);

    // Session 2 (a fresh launch): load S1, then S2.
    Session two;
    auto v = two.Load(s1);
    CHECK(v.why == LoadWhy::FirstLoad);
    two.RunLoadSteps();
    v = two.Load(s2);
    CHECK(v.decision == LoadDecision::Replace);
    CHECK(v.why == LoadWhy::SaveAhead);
    CHECK(two.clock.Get() == s2.clock);

    // A discovery-only change (a new item picked up) is the same case.
    Session three;
    three.activeID = kHero;
    three.clock.Set(s2.clock);
    Note(&three.clock, Event::NewlyGained);
    const Save s3 = three.SaveNow();
    Session four;
    four.Load(s2);
    four.RunLoadSteps();
    CHECK(four.Load(s3).why == LoadWhy::SaveAhead);
}

TEST_CASE("learning clock: a load's own steps add no ticks")
{
    // Session 1: S2 at 20, a new item picked up (21), S3 at 21. Session 2:
    // load S2 (Replace, clock 20), the load steps run, load S3: if a load step
    // ticked, memory would be at 21 and S3 a Reload that dropped its gain.
    Session one;
    one.activeID = kHero;
    one.clock.Set(20);
    const Save s2 = one.SaveNow();
    Note(&one.clock, Event::NewlyGained);
    const Save s3 = one.SaveNow();

    Session two;
    two.Load(s2);
    Note(&two.clock, Event::LoadSuspend);   // the revert before the next load
    two.RunLoadSteps();
    for (int i = 0; i < 100; ++i) Note(&two.clock, Event::PlayTime);
    Note(&two.clock, Event::RepeatPick);
    Note(&two.clock, Event::UnusablePick);
    CHECK(two.clock.Get() == 20);
    const auto v = two.Load(s3);
    CHECK(v.decision == LoadDecision::Replace);
    CHECK(v.why == LoadWhy::SaveAhead);
}

TEST_CASE("learning clock: a reset holds when an older save of the character is reloaded")
{
    Session s;
    s.NewGame(kHero);
    for (int i = 0; i < 5; ++i) Note(&s.clock, Event::PickLearned);
    const Save before = s.SaveNow();
    // Reset Learned Weights with nothing learned since the save: the reset's
    // own tick puts memory ahead, so the reload keeps the reset learning.
    Note(&s.clock, Event::Reset);
    const auto v = s.Load(before);
    CHECK(v.decision == LoadDecision::Reload);
    CHECK(v.why == LoadWhy::NotAhead);
    CHECK(s.clock.Get() == before.clock + 1);
}

TEST_CASE("learning clock: a reload keeps memory's clock, so a later save from before it still wins")
{
    // Session: S1 at 3, two picks (5), death, reload S1: memory kept at 5.
    Session s;
    s.NewGame(kHero);
    for (int i = 0; i < 3; ++i) Note(&s.clock, Event::PickLearned);
    const Save s1 = s.SaveNow();
    Note(&s.clock, Event::PickLearned);
    Note(&s.clock, Event::NeedGrown);
    const Save s2 = s.SaveNow();
    CHECK(s.Load(s1).decision == LoadDecision::Reload);
    CHECK(s.clock.Get() == 5);
    // S2 (also at 5) is not ahead of what memory holds: memory is kept
    CHECK(s.Load(s2).decision == LoadDecision::Reload);
    // another character's save replaces, and its clock comes with it
    const auto v = s.Load(Save{ kOther, 2 });
    CHECK(v.why == LoadWhy::OtherCharacter);
    CHECK(s.clock.Get() == 2);
    CHECK(s.activeID == kOther);
}
