// core/NeedEpisodes.h: when a need episode starts, ends, is answered by a
// selection, and when an unanswered one becomes a "nothing" record.

#include "core/NeedEpisodes.h"

#include <doctest/doctest.h>

#include <ostream>

using namespace Huginn::Core::Needs;

namespace
{
    constexpr std::size_t kH = static_cast<std::size_t>(NeedId::health_deficit);
    constexpr std::size_t kHunger = static_cast<std::size_t>(NeedId::hunger);

    NeedArray With(std::size_t k, float v)
    {
        NeedArray a{};
        a[k] = v;
        return a;
    }
}

TEST_CASE("episodes: onset at 0.5, expiry below 0.25, hysteresis between")
{
    EpisodeTracker<int> t;
    CHECK(t.Tick(With(kH, 0.49f), 0.0).empty());
    const auto on = t.Tick(With(kH, 0.5f), 1.0);
    REQUIRE(on.size() == 1);
    CHECK(on[0] == kH);
    CHECK(t.IsOpen(kH));
    // Wobbling between the thresholds is one episode.
    CHECK(t.Tick(With(kH, 0.3f), 2.0).empty());
    CHECK(t.Tick(With(kH, 0.7f), 3.0).empty());
    CHECK(t.Tick(With(kH, 0.25f), 4.0).empty());   // 0.25 is not below 0.25
    CHECK(t.IsOpen(kH));
    t.Tick(With(kH, 0.24f), 5.0);
    CHECK_FALSE(t.IsOpen(kH));
    CHECK(t.ClosingCount() == 1);
    // Judged only after the grace.
    CHECK(t.TakeUnanswered(8.9).empty());
    const auto out = t.TakeUnanswered(9.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].need == kH);
    CHECK(out[0].onsetSec == doctest::Approx(1.0));
    CHECK(out[0].endSec == doctest::Approx(5.0));
    CHECK(out[0].peak == doctest::Approx(0.7f));
    CHECK(t.ClosingCount() == 0);
    CHECK(t.TakeUnanswered(100.0).empty());   // returned once
}

TEST_CASE("episodes: shorter than a second are dropped")
{
    EpisodeTracker<int> t;
    t.Tick(With(kH, 0.9f), 10.0);
    t.Tick(With(kH, 0.0f), 10.9);
    CHECK(t.ClosingCount() == 0);
    CHECK(t.DroppedShort() == 1);
    t.Tick(With(kH, 0.9f), 20.0);
    t.Tick(With(kH, 0.0f), 21.0);   // exactly the minimum counts
    CHECK(t.ClosingCount() == 1);
}

TEST_CASE("episodes: a selection inside answers, one outside does not")
{
    EpisodeTracker<int> t;
    t.Tick(With(kH, 1.0f), 0.0);
    t.OnSelection(-0.5);   // before the onset: not this episode's
    t.Tick(With(kH, 0.0f), 5.0);
    CHECK(t.TakeUnanswered(9.0).size() == 1);

    t.Tick(With(kH, 1.0f), 10.0);
    t.OnSelection(12.0);   // inside, while open
    t.Tick(With(kH, 0.0f), 15.0);
    CHECK(t.TakeUnanswered(30.0).empty());
}

TEST_CASE("episodes: a pick made inside but confirmed after the end still answers (grace)")
{
    EpisodeTracker<int> t;
    t.Tick(With(kH, 1.0f), 0.0);
    t.Tick(With(kH, 0.0f), 4.0);   // the potion's restore ended the need ...
    t.OnSelection(3.8);            // ... and its count drop confirmed the pick afterwards
    CHECK(t.TakeUnanswered(10.0).empty());

    t.Tick(With(kH, 1.0f), 20.0);
    t.Tick(With(kH, 0.0f), 24.0);
    t.OnSelection(24.6);           // made more than the slack after the end: does not answer
    CHECK(t.TakeUnanswered(30.0).size() == 1);
}

TEST_CASE("episodes: a press that itself ended the episode answers it (the slack)")
{
    // In game a key's equip landed, and an update tick saw loadout_archery
    // end, 2 ms before the selection was stamped.
    EpisodeTracker<int> t;
    t.Tick(With(kH, 1.0f), 0.0);
    t.Tick(With(kH, 0.0f), 10.000);
    t.OnSelection(10.002);
    CHECK(t.TakeUnanswered(20.0).empty());

    t.Tick(With(kH, 1.0f), 30.0);
    t.Tick(With(kH, 0.0f), 40.0);
    t.OnSelection(40.0 + t.Params().answerSlackSec);   // the edge counts
    CHECK(t.TakeUnanswered(50.0).empty());

    // An episode the press itself STARTED is not answered by it.
    t.OnSelection(59.95);
    t.Tick(With(kH, 1.0f), 60.0);
    t.Tick(With(kH, 0.0f), 70.0);
    CHECK(t.TakeUnanswered(80.0).size() == 1);
}

TEST_CASE("episodes: a confirmation on the grace-end tick counts only when delivered first")
{
    // The ordering contract SelectionLogV3::TickAfterSelections keeps: after a
    // stall of the loop, the tick that ends an episode's grace can also be the
    // tick a pick made inside the episode confirms (SelectionTracker::Update,
    // or the v3-only picks' TickPicks). OnSelection must run before
    // TakeUnanswered on that tick.
    const double end = 4.0;
    const double graceEnd = end + EpisodeParams{}.graceSec + 0.6;   // a 0.6 s stall
    {
        EpisodeTracker<int> t;   // the order kept: confirmations, then judgement
        t.Tick(With(kH, 1.0f), 0.0);
        t.Tick(With(kH, 0.0f), end);
        t.OnSelection(3.9);
        CHECK(t.TakeUnanswered(graceEnd).empty());
    }
    {
        EpisodeTracker<int> t;   // the wrong order gives a false "nothing"
        t.Tick(With(kH, 1.0f), 0.0);
        t.Tick(With(kH, 0.0f), end);
        CHECK(t.TakeUnanswered(graceEnd).size() == 1);
        t.OnSelection(3.9);
    }
}

TEST_CASE("episodes: needs are independent and carry their payload")
{
    EpisodeTracker<int> t;
    NeedArray v{};
    v[kH] = 1.0f;
    v[kHunger] = 1.0f;
    const auto on = t.Tick(v, 0.0);
    REQUIRE(on.size() == 2);
    t.SetPayload(kH, 7);
    t.SetPayload(kHunger, 9);
    CHECK(t.OpenNeeds() == std::vector<std::size_t>{ kH, kHunger });
    v[kH] = 0.0f;
    t.Tick(v, 2.0);
    t.OnSelection(3.0);   // answers hunger (open), not health (ended at 2)
    v[kHunger] = 0.0f;
    t.Tick(v, 4.0);
    const auto out = t.TakeUnanswered(10.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].need == kH);
    CHECK(out[0].payload == 7);
}

TEST_CASE("episodes: reset forgets open and closing episodes")
{
    EpisodeTracker<int> t;
    t.Tick(With(kH, 1.0f), 0.0);
    t.Tick(With(kHunger, 1.0f), 1.0);   // health ends (closing), hunger opens
    CHECK(t.ClosingCount() == 1);
    t.Reset();
    CHECK(t.ClosingCount() == 0);
    CHECK(t.OpenNeeds().empty());
    CHECK(t.TakeUnanswered(100.0).empty());
}

TEST_CASE("episodes: a death drops the open ones and those in their grace, like a load (0.23.16)")
{
    EpisodeTracker<int> t;
    constexpr std::size_t kFall = static_cast<std::size_t>(NeedId::falling);
    t.Tick(With(kHunger, 0.9f), 0.0);  // open for 28 s
    NeedArray both = With(kHunger, 0.9f);
    both[kFall] = 1.0f;
    t.Tick(both, 26.5);                 // a fall starts
    t.Tick(With(kHunger, 0.9f), 28.0);  // ... and ends on impact: 1.5 s, now in its grace
    REQUIRE(t.IsOpen(kHunger));
    REQUIRE(t.ClosingCount() == 1);
    const std::size_t droppedShort = t.DroppedShort();

    CHECK(t.Abandon() == 2);  // the player died at 28.0
    CHECK_FALSE(t.IsOpen(kHunger));
    CHECK(t.ClosingCount() == 0);
    CHECK(t.TakeUnanswered(40.0).empty());    // no "nothing" for either
    CHECK(t.DroppedShort() == droppedShort);  // not a short episode, not a reset

    // The tracker goes on: an episode after the death is one like any other.
    t.Tick(With(kH, 0.9f), 50.0);
    t.Tick(With(kH, 0.0f), 52.0);
    const auto out = t.TakeUnanswered(56.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].need == kH);
}

// =============================================================================
// LAPS (0.23.51): an open episode is judged in stretches on a shared grid
// =============================================================================
namespace
{
    constexpr std::size_t kThirst = static_cast<std::size_t>(NeedId::thirst);

    /// Ticks need k at value v every `step` seconds over [from, to] and
    /// returns the times of the ticks on which k lapped. 0.5 s steps sum
    /// exactly in binary, so the grid falls on whole ticks.
    std::vector<double> Run(EpisodeTracker<int>& t, std::size_t k, float v, double from, double to, double step = 0.5)
    {
        std::vector<double> lapped;
        for (double s = from; s <= to + 1e-9; s += step) {
            t.Tick(With(k, v), s);
            for (const auto n : t.Laps()) {
                if (n == k) lapped.push_back(s);
            }
        }
        return lapped;
    }
}

TEST_CASE("laps: an episode that ends within lapSec of its onset is judged whole, as before")
{
    EpisodeTracker<int> t;
    CHECK(Run(t, kH, 1.0f, 0.0, 59.5).empty());
    t.Tick(With(kH, 0.0f), 60.0);   // the expiry comes first on the grid tick: no lap
    CHECK(t.Laps().empty());
    const auto out = t.TakeUnanswered(64.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].lap == 0);
    CHECK_FALSE(out[0].byLap);
    CHECK(out[0].startSec == doctest::Approx(0.0));
    CHECK(out[0].onsetSec == doctest::Approx(0.0));
    CHECK(out[0].endSec == doctest::Approx(60.0));
}

TEST_CASE("laps: a persistent need is cut on the grid; each unanswered stretch is one record")
{
    EpisodeTracker<int> t;
    // Hungry from the first tick (active 0, itself a grid point): laps every 60 s.
    CHECK(Run(t, kHunger, 0.8f, 0.0, 182.0) == std::vector<double>{ 60.0, 120.0, 180.0 });
    CHECK(t.IsOpen(kHunger));
    // The stretches cut at 60 and 120 are past their grace; the one cut at 180 is not.
    auto out = t.TakeUnanswered(182.0);
    REQUIRE(out.size() == 2);
    CHECK(out[0].lap == 0);
    CHECK(out[0].byLap);
    CHECK(out[0].startSec == doctest::Approx(0.0));
    CHECK(out[0].endSec == doctest::Approx(60.0));
    CHECK(out[1].lap == 1);
    CHECK(out[1].byLap);
    CHECK(out[1].startSec == doctest::Approx(60.0));
    CHECK(out[1].endSec == doctest::Approx(120.0));
    CHECK(out[1].onsetSec == doctest::Approx(0.0));   // the episode's onset, kept
    CHECK(out[1].peak == doctest::Approx(0.8f));
    // A selection at 190 lies in the stretch begun at 180: it answers that
    // one, not the one cut at 180 (no slack after a lap).
    t.OnSelection(190.0);
    Run(t, kHunger, 0.8f, 182.5, 250.0);   // laps again at 240
    out = t.TakeUnanswered(250.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].lap == 2);
    CHECK(out[0].startSec == doctest::Approx(120.0));
    CHECK(out[0].endSec == doctest::Approx(180.0));
}

TEST_CASE("laps: an episode's first stretch lasts lapSec to 2 lapSec, and needs lap together")
{
    EpisodeTracker<int> t;
    NeedArray v{};
    v[kHunger] = 1.0f;
    std::vector<std::pair<double, std::vector<std::size_t>>> laps;
    for (double s = 0.0; s <= 130.0; s += 0.5) {
        v[kThirst] = s >= 10.0 ? 1.0f : 0.0f;   // thirst starts 10 s after the grid point
        t.Tick(v, s);
        if (!t.Laps().empty()) laps.emplace_back(s, t.Laps());
    }
    REQUIRE(laps.size() == 2);
    CHECK(laps[0].first == doctest::Approx(60.0));
    CHECK(laps[0].second == std::vector<std::size_t>{ kHunger });   // thirst's stretch began after the grid point
    CHECK(laps[1].first == doctest::Approx(120.0));
    const std::vector<std::size_t> both = kHunger < kThirst ? std::vector<std::size_t>{ kHunger, kThirst }
                                                            : std::vector<std::size_t>{ kThirst, kHunger };
    CHECK(laps[1].second == both);   // one tick, one context for both
    const auto out = t.TakeUnanswered(130.0);
    REQUIRE(out.size() == 3);   // hunger 0-60 and 60-120, thirst 10-120
    for (const auto& e : out) {
        if (e.need == kThirst) {
            CHECK(e.startSec == doctest::Approx(10.0));
            CHECK(e.endSec == doctest::Approx(120.0));
            CHECK(e.lap == 0);
        }
    }
}

TEST_CASE("laps: a press just before a lap answers the cut stretch only; one just after, the next only")
{
    {
        EpisodeTracker<int> t;
        Run(t, kHunger, 1.0f, 0.0, 59.5);
        t.OnSelection(59.5);
        Run(t, kHunger, 1.0f, 60.0, 130.0);   // laps at 60 and 120
        const auto out = t.TakeUnanswered(130.0);
        REQUIRE(out.size() == 1);
        CHECK(out[0].startSec == doctest::Approx(60.0));   // 0-60 answered, 60-120 not
    }
    {
        EpisodeTracker<int> t;
        Run(t, kHunger, 1.0f, 0.0, 60.0);   // the lap at 60
        t.OnSelection(60.2);                // confirmed in the cut stretch's grace, pressed after the cut
        Run(t, kHunger, 1.0f, 60.5, 130.0);
        const auto out = t.TakeUnanswered(130.0);
        REQUIRE(out.size() == 1);
        CHECK(out[0].startSec == doctest::Approx(0.0));    // 0-60 not answered: no slack after a lap
    }
    {
        EpisodeTracker<int> t;   // a pick pressed in the stretch, confirmed 3 s after the cut, counts
        Run(t, kHunger, 1.0f, 0.0, 63.0);
        t.OnSelection(58.0);
        CHECK(t.TakeUnanswered(64.0).empty());
    }
}

TEST_CASE("laps: the last stretch ends like an episode; shorter than minSec it is dropped")
{
    {
        EpisodeTracker<int> t;
        Run(t, kHunger, 1.0f, 0.0, 60.0);   // the lap at 60
        const std::size_t shortBefore = t.DroppedShort();
        t.Tick(With(kHunger, 0.0f), 60.5);  // ate 0.5 s after the lap
        CHECK(t.DroppedShort() == shortBefore + 1);
        const auto out = t.TakeUnanswered(70.0);
        REQUIRE(out.size() == 1);
        CHECK(out[0].byLap);
    }
    {
        EpisodeTracker<int> t;
        Run(t, kHunger, 1.0f, 0.0, 74.5);
        t.Tick(With(kHunger, 0.0f), 75.0);
        t.OnSelection(75.3);   // the press that ended it: the slack holds for an expiry
        const auto out = t.TakeUnanswered(80.0);
        REQUIRE(out.size() == 1);   // only the lapped 0-60
        CHECK(out[0].endSec == doctest::Approx(60.0));
    }
    {
        EpisodeTracker<int> t;
        Run(t, kHunger, 1.0f, 0.0, 74.5);
        t.Tick(With(kHunger, 0.0f), 75.0);
        const auto out = t.TakeUnanswered(80.0);
        REQUIRE(out.size() == 2);
        CHECK(out[1].lap == 1);
        CHECK_FALSE(out[1].byLap);
        CHECK(out[1].startSec == doctest::Approx(60.0));
        CHECK(out[1].endSec == doctest::Approx(75.0));
        CHECK(out[1].onsetSec == doctest::Approx(0.0));
    }
}

TEST_CASE("laps: a pause, a death or a stall adds at most lapMaxStepSec to the active clock")
{
    EpisodeTracker<int> t;
    Run(t, kHunger, 1.0f, 0.0, 30.0);
    CHECK(t.ActiveSec() == doctest::Approx(30.0));
    // The game paused for 970 s (no ticks): the next tick adds 1 s.
    t.Tick(With(kHunger, 1.0f), 1000.0);
    CHECK(t.ActiveSec() == doctest::Approx(31.0));
    const auto laps = Run(t, kHunger, 1.0f, 1000.5, 1040.0);
    REQUIRE(laps.size() == 1);
    CHECK(laps[0] == doctest::Approx(1029.0));   // 30 + 1 + 29 active seconds
    // A clock that steps back adds nothing.
    const double a = t.ActiveSec();
    t.Tick(With(kHunger, 1.0f), 900.0);
    CHECK(t.ActiveSec() == doctest::Approx(a));
}

TEST_CASE("laps: the payload is carried to the next stretch until the caller replaces it")
{
    EpisodeTracker<int> t;
    t.Tick(With(kHunger, 1.0f), 0.0);
    t.SetPayload(kHunger, 7);
    Run(t, kHunger, 1.0f, 0.5, 60.0);   // the lap at 60
    int* p = t.MutablePayload(kHunger);
    REQUIRE(p != nullptr);
    CHECK(*p == 7);   // a copy of the cut stretch's
    *p = 8;           // the game: the lap's context
    CHECK(t.MutablePayload(kH) == nullptr);
    Run(t, kHunger, 1.0f, 60.5, 130.0);
    const auto out = t.TakeUnanswered(130.0);
    REQUIRE(out.size() == 2);
    CHECK(out[0].payload == 7);
    CHECK(out[1].payload == 8);
}

TEST_CASE("laps: lapSec 0 judges every episode whole; Reset restarts the grid")
{
    EpisodeParams p;
    p.lapSec = 0.0;
    EpisodeTracker<int> whole(p);
    CHECK(Run(whole, kHunger, 1.0f, 0.0, 300.0).empty());
    whole.Tick(With(kHunger, 0.0f), 300.5);
    const auto out = whole.TakeUnanswered(310.0);
    REQUIRE(out.size() == 1);
    CHECK(out[0].endSec - out[0].startSec == doctest::Approx(300.5));

    EpisodeTracker<int> t;
    Run(t, kHunger, 1.0f, 0.0, 45.0);
    t.Reset();   // a load at 45 s
    CHECK(t.ActiveSec() == doctest::Approx(0.0));
    const auto laps = Run(t, kHunger, 1.0f, 50.0, 115.0);
    REQUIRE(laps.size() == 1);
    CHECK(laps[0] == doctest::Approx(110.0));   // 60 active seconds after the load's first tick
}

TEST_CASE("laps: a death drops the lapped stretches in their grace too")
{
    EpisodeTracker<int> t;
    Run(t, kHunger, 1.0f, 0.0, 61.0);   // the lap at 60; that stretch is in its grace
    REQUIRE(t.ClosingCount() == 1);
    CHECK(t.Abandon() == 2);            // the cut stretch and the open one
    CHECK(t.TakeUnanswered(100.0).empty());
}
