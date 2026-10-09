// core/ResponseCurve.h: each curve kind against values worked out by hand
// from the formula table in the header, plus the parse/format round trip and
// the parameter checks the [Needs] INI relies on.

#include "core/ResponseCurve.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <ostream>

using namespace Huginn::Core::Needs;

namespace
{
    float Y(CurveKind k, float p1, float p2, float x) { return Evaluate(Curve{ k, p1, p2 }, x); }
}

TEST_CASE("logistic: centre is one half, slope sets the steepness, a negative slope falls")
{
    CHECK(Y(CurveKind::Logistic, 0.5f, 10.0f, 0.5f) == doctest::Approx(0.5));
    // 1 / (1 + e^-2) = 0.880797
    CHECK(Y(CurveKind::Logistic, 0.5f, 10.0f, 0.7f) == doctest::Approx(0.880797).epsilon(1e-5));
    CHECK(Y(CurveKind::Logistic, 0.5f, 10.0f, 0.3f) == doctest::Approx(0.119203).epsilon(1e-5));
    // e^-5 / (1 + e^-5) ~ 0.0066929: full health is not exactly zero need
    CHECK(Y(CurveKind::Logistic, 0.5f, 10.0f, 0.0f) == doctest::Approx(0.0066929).epsilon(1e-4));
    // enemy_close: falls with distance, c = 300
    CHECK(Y(CurveKind::Logistic, 300.0f, -0.02f, 300.0f) == doctest::Approx(0.5));
    CHECK(Y(CurveKind::Logistic, 300.0f, -0.02f, 100.0f) == doctest::Approx(0.982014).epsilon(1e-5));
    CHECK(Y(CurveKind::Logistic, 300.0f, -0.02f, 4096.0f) < 1e-30f);
    // overflow of exp is fine
    CHECK(Y(CurveKind::Logistic, 0.0f, 1000.0f, -10.0f) == 0.0f);
    CHECK(Y(CurveKind::Logistic, 0.0f, 1000.0f, 10.0f) == 1.0f);
}

TEST_CASE("linear and quadratic ramps, rising and falling")
{
    CHECK(Y(CurveKind::Linear, 200.0f, 600.0f, 100.0f) == 0.0f);
    CHECK(Y(CurveKind::Linear, 200.0f, 600.0f, 400.0f) == doctest::Approx(0.5));
    CHECK(Y(CurveKind::Linear, 200.0f, 600.0f, 900.0f) == 1.0f);
    // enemy_casting_*: 1 at 0 s, 0 at 2 s
    CHECK(Y(CurveKind::Linear, 2.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(Y(CurveKind::Linear, 2.0f, 0.0f, 0.5f) == doctest::Approx(0.75));
    CHECK(Y(CurveKind::Linear, 2.0f, 0.0f, 2.0f) == 0.0f);
    CHECK(Y(CurveKind::Linear, 2.0f, 0.0f, 1.0e6f) == 0.0f);
    CHECK(Y(CurveKind::Quadratic, 0.0f, 1.0f, 0.4f) == doctest::Approx(0.16));
    CHECK(Y(CurveKind::Quadratic, 0.0f, 1.0f, 1.2f) == 1.0f);
    CHECK(Y(CurveKind::Quadratic, 0.0f, 1.0f, -0.5f) == 0.0f);
}

TEST_CASE("saturating: 0 at 0, 1 at 1, diminishing returns between")
{
    CHECK(Y(CurveKind::Saturating, 5.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(Y(CurveKind::Saturating, 5.0f, 0.0f, -1.0f) == 0.0f);
    CHECK(Y(CurveKind::Saturating, 5.0f, 0.0f, 1.0f) == doctest::Approx(1.0));
    CHECK(Y(CurveKind::Saturating, 5.0f, 0.0f, 3.0f) == 1.0f);
    // one of six enemies: ln(1 + 5/6) / ln 6 = 0.338283
    CHECK(Y(CurveKind::Saturating, 5.0f, 0.0f, 1.0f / 6.0f) == doctest::Approx(0.338283).epsilon(1e-5));
    // concave: the second enemy adds less than the first
    const float one = Y(CurveKind::Saturating, 5.0f, 0.0f, 1.0f / 6.0f);
    const float two = Y(CurveKind::Saturating, 5.0f, 0.0f, 2.0f / 6.0f);
    CHECK(two - one < one);
}

TEST_CASE("gaussian, step and decay")
{
    CHECK(Y(CurveKind::Gaussian, 600.0f, 200.0f, 600.0f) == 1.0f);
    // exp(-(300^2) / (2 * 200^2)) = exp(-1.125) = 0.324652
    CHECK(Y(CurveKind::Gaussian, 600.0f, 200.0f, 300.0f) == doctest::Approx(0.324652).epsilon(1e-5));
    CHECK(Y(CurveKind::Gaussian, 600.0f, 200.0f, 900.0f) == doctest::Approx(0.324652).epsilon(1e-5));
    CHECK(Y(CurveKind::Gaussian, 0.0f, 10.0f, 10.0f) == doctest::Approx(0.606531).epsilon(1e-5));
    CHECK(Y(CurveKind::Step, 0.5f, 0.0f, 0.5f) == 1.0f);
    CHECK(Y(CurveKind::Step, 0.5f, 0.0f, 0.49f) == 0.0f);
    CHECK(Y(CurveKind::Step, 0.5f, 0.0f, 1.0f) == 1.0f);
    CHECK(Y(CurveKind::Decay, 15.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(Y(CurveKind::Decay, 15.0f, 0.0f, -3.0f) == 1.0f);
    CHECK(Y(CurveKind::Decay, 15.0f, 0.0f, 15.0f) == doctest::Approx(0.367879).epsilon(1e-5));
    CHECK(Y(CurveKind::Decay, 15.0f, 0.0f, 1.0e6f) == 0.0f);
}

TEST_CASE("every kind stays in [0, 1]; NaN reads 0")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (auto k : { CurveKind::Logistic, CurveKind::Linear, CurveKind::Quadratic, CurveKind::Saturating,
                    CurveKind::Gaussian, CurveKind::Step, CurveKind::Decay }) {
        INFO(KindName(k));
        const Curve c{ k, 1.0f, 2.0f };
        CHECK(Evaluate(c, nan) == 0.0f);
        for (float x : { -inf, -1.0e9f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f, 1.0e9f, inf }) {
            const float y = Evaluate(c, x);
            CHECK(y >= 0.0f);
            CHECK(y <= 1.0f);
        }
    }
}

TEST_CASE("parse and format: round trip, and the INI's bad values are refused")
{
    const auto c = ParseCurve("logistic 0.5 10");
    REQUIRE(c.has_value());
    CHECK(*c == Curve{ CurveKind::Logistic, 0.5f, 10.0f });
    CHECK(ParseCurve(FormatCurve(*c)) == c);
    CHECK(ParseCurve("  Gaussian,600,200 ") == Curve{ CurveKind::Gaussian, 600.0f, 200.0f });
    CHECK(ParseCurve("decay 15") == Curve{ CurveKind::Decay, 15.0f, 0.0f });
    CHECK(ParseCurve("step 0.5") == Curve{ CurveKind::Step, 0.5f, 0.0f });
    CHECK(ParseCurve("logistic 300 -0.02") == Curve{ CurveKind::Logistic, 300.0f, -0.02f });
    const Curve odd{ CurveKind::Linear, 0.1f, 1.0f / 3.0f };
    CHECK(ParseCurve(FormatCurve(odd)) == odd);

    CHECK_FALSE(ParseCurve("").has_value());
    CHECK_FALSE(ParseCurve("logistic").has_value());
    CHECK_FALSE(ParseCurve("logistic 0.5").has_value());       // needs both
    CHECK_FALSE(ParseCurve("logistic 0.5 0").has_value());     // flat
    CHECK_FALSE(ParseCurve("linear 1 1").has_value());         // no span
    CHECK_FALSE(ParseCurve("gaussian 0 0").has_value());       // no width
    CHECK_FALSE(ParseCurve("saturating 0").has_value());
    CHECK_FALSE(ParseCurve("decay -1").has_value());
    CHECK_FALSE(ParseCurve("sigmoid 1 2").has_value());        // unknown kind
    CHECK_FALSE(ParseCurve("logistic 0.5 ten").has_value());
    CHECK_FALSE(ParseCurve("logistic 0.5 10 3").has_value());  // too many
    CHECK_FALSE(ParseCurve("logistic nan 10").has_value());
}
