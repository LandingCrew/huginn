// Host tests for src/core/LightHold.h: the light the darkness need reads,
// held against torchlight and shadow flicker (0.23.41).

#include "core/LightHold.h"
#include "core/NeedEvaluator.h"

#include <doctest/doctest.h>

using namespace Huginn::Core::Needs;

TEST_CASE("light hold: the first reading and a new place are taken at once")
{
    LightHold h;
    CHECK(h.Update(0.3f, 1, 0.0) == doctest::Approx(0.3f));
    CHECK(h.Update(0.9f, 2, 0.1) == doctest::Approx(0.9f));   // a door: another cell
    CHECK(h.Update(0.1f, 3, 0.2) == doctest::Approx(0.1f));   // into a cave
    h.Reset();
    CHECK(h.Update(0.7f, 3, 0.3) == doctest::Approx(0.7f));   // a load: as a new place
}

TEST_CASE("light hold: darker after 5 s, lighter after 3 s, in the same place")
{
    LightHold h;
    h.Update(0.6f, 7, 0.0);
    CHECK(h.Update(0.3f, 7, 1.0) == doctest::Approx(0.6f));
    CHECK(h.Update(0.3f, 7, 5.9) == doctest::Approx(0.6f));
    CHECK(h.Update(0.3f, 7, 6.0) == doctest::Approx(0.3f));   // 5 s darker
    CHECK_FALSE(h.Pending());
    CHECK(h.Update(0.8f, 7, 10.0) == doctest::Approx(0.3f));
    CHECK(h.Update(0.8f, 7, 12.9) == doctest::Approx(0.3f));
    CHECK(h.Update(0.8f, 7, 13.0) == doctest::Approx(0.8f));  // 3 s lighter
}

TEST_CASE("light hold: torchlight flicker around the held level never moves it")
{
    // The swing the v3 log showed: 0.3 <-> 0.5 every second or so.
    LightHold h;
    h.Update(0.3f, 7, 0.0);
    double t = 0.0;
    for (int i = 0; i < 60; ++i) {
        t += 1.0;
        const float raw = (i % 2 == 0) ? 0.5f : 0.3f;
        CHECK(h.Update(raw, 7, t) == doctest::Approx(0.3f));
    }
    // Through the darkness curve: one steady value, not 0.68 <-> 0.10.
    NeedSnapshot s;
    s.light = h.Held();
    const auto v = EvaluateNeeds(s, DefaultCurves());
    CHECK(v.value[Index(NeedId::darkness)] > 0.6f);
}

TEST_CASE("light hold: a wait restarts when the reading crosses back; the value taken is the latest")
{
    LightHold h;
    h.Update(0.5f, 7, 0.0);
    h.Update(0.3f, 7, 1.0);    // darker: wait starts at 1
    h.Update(0.6f, 7, 4.0);    // lighter: the darker wait is dropped, a lighter one starts at 4
    CHECK(h.Update(0.6f, 7, 6.5) == doctest::Approx(0.5f));
    CHECK(h.Update(0.7f, 7, 7.0) == doctest::Approx(0.7f));   // 3 s lighter, the reading now
    h.Update(0.4f, 7, 8.0);    // darker from 8
    h.Update(0.7f, 7, 9.0);    // back at the held level: the wait ends
    CHECK(h.Update(0.4f, 7, 12.0) == doctest::Approx(0.7f));  // a fresh wait from 12
    CHECK(h.Update(0.4f, 7, 16.9) == doctest::Approx(0.7f));
    CHECK(h.Update(0.4f, 7, 17.0) == doctest::Approx(0.4f));
}
