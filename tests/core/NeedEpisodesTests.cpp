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
    CHECK(t.TakeUnanswered(8.0).empty());
    const auto out = t.TakeUnanswered(8.5);
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
    t.OnSelection(24.5);           // made after the end: does not answer
    CHECK(t.TakeUnanswered(30.0).size() == 1);
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
