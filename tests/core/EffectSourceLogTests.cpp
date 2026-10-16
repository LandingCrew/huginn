// Host tests for src/core/EffectSourceLog.h: the transition lines naming the
// effect that turned drain_health or regen_suppressed on (0.23.41).

#include "core/EffectSourceLog.h"

#include <doctest/doctest.h>

#include <string>

using namespace Huginn::Core::Needs;

TEST_CASE("effect source log: one line on, one line off, nothing while it holds")
{
    SourceWatch w("drain_health");
    EffectSource bleed{ "Bleeding", 0x0010F7E6u, "Bleed Hit", 0x2A012345u, "spell", 2.0f, 5.0f };
    auto line = w.Update(true, bleed);
    REQUIRE(line.has_value());
    CHECK(*line == "drain_health on: Bleeding [0010F7E6] from Bleed Hit [2A012345, spell], magnitude 2 over 5 s");
    CHECK_FALSE(w.Update(true, bleed).has_value());     // still on: no line
    CHECK_FALSE(w.Update(true, {}).has_value());
    line = w.Update(false, {});
    REQUIRE(line.has_value());
    CHECK(*line == "drain_health off");
    CHECK_FALSE(w.Update(false, {}).has_value());
}

TEST_CASE("effect source log: an effect with no spell, an unnamed one, an unread source")
{
    EffectSource script{ "", 0x00ABCDEFu, "", 0u, "", 0.5f, 12.0f };
    CHECK(DescribeSource(script) == "(unnamed) [00ABCDEF] (no spell: script-applied), magnitude 0.5 over 12 s");

    SourceWatch w("regen_suppressed");
    const auto line = w.Update(true, {});
    REQUIRE(line.has_value());
    CHECK(*line == "regen_suppressed on (source not read)");
    w.Reset();
    CHECK_FALSE(w.On());
    CHECK(w.Update(true, script).has_value());   // reported again after a load
}
