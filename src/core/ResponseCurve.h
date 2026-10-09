#pragma once

// =============================================================================
// RESPONSE CURVE -- one sensor reading to one need value in [0, 1]
// =============================================================================
// Doc 9 "Response curves": every need passes through its own designer-set
// curve before it enters the model; curves hold facts about the game, theta
// holds facts about the player. The kinds are the ones needs.csv names in its
// curve column; each need's kind and two parameters are the csv's curve_kind,
// curve_p1 and curve_p2 columns (the defaults), overridable per need in the
// [Needs] INI section (NeedSettings).
//
//   kind        p1        p2        y(x)
//   logistic    centre c  slope s   1 / (1 + exp(-s (x - c)))   (s < 0 falls)
//   linear      x0        x1        clamp((x - x0) / (x1 - x0), 0, 1)
//                                   (x1 < x0 is a falling ramp)
//   quadratic   x0        x1        linear(x0, x1)^2
//   saturating  s > 0     --        0 for x <= 0, else
//                                   min(ln(1 + s x) / ln(1 + s), 1)
//                                   (the csv's "Logit / saturating":
//                                   diminishing returns, 0 at 0, 1 at 1)
//   gaussian    centre c  width w   exp(-(x - c)^2 / (2 w^2)), w > 0
//   step        edge      --        x >= edge ? 1 : 0
//   decay       tau > 0   --        1 for x <= 0, else exp(-x / tau)
//
// A NaN input reads 0 (a sensor that has nothing to say says nothing). The
// output is always in [0, 1].
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Huginn::Core::Needs
{
    enum class CurveKind : std::uint8_t
    {
        Logistic,
        Linear,
        Quadratic,
        Saturating,
        Gaussian,
        Step,
        Decay,
    };

    struct Curve
    {
        CurveKind kind = CurveKind::Step;
        float p1 = 0.5f;
        float p2 = 0.0f;

        friend constexpr bool operator==(const Curve&, const Curve&) = default;
    };

    [[nodiscard]] constexpr std::string_view KindName(CurveKind kind) noexcept
    {
        switch (kind) {
            case CurveKind::Logistic: return "logistic";
            case CurveKind::Linear: return "linear";
            case CurveKind::Quadratic: return "quadratic";
            case CurveKind::Saturating: return "saturating";
            case CurveKind::Gaussian: return "gaussian";
            case CurveKind::Step: return "step";
            case CurveKind::Decay: return "decay";
        }
        return "step";
    }

    [[nodiscard]] constexpr std::optional<CurveKind> KindFromName(std::string_view name) noexcept
    {
        for (auto k : { CurveKind::Logistic, CurveKind::Linear, CurveKind::Quadratic, CurveKind::Saturating,
                        CurveKind::Gaussian, CurveKind::Step, CurveKind::Decay }) {
            if (KindName(k) == name) return k;
        }
        return std::nullopt;
    }

    /// Why a curve's parameters cannot be used, or empty when they can. A
    /// bad curve from the INI is rejected (the default is kept), never
    /// evaluated: a zero width or span would divide by zero.
    [[nodiscard]] inline std::string_view CurveProblem(const Curve& c) noexcept
    {
        if (!std::isfinite(c.p1) || !std::isfinite(c.p2)) return "parameters must be finite";
        switch (c.kind) {
            case CurveKind::Logistic:
                return c.p2 == 0.0f ? "logistic slope must not be 0" : "";
            case CurveKind::Linear:
            case CurveKind::Quadratic:
                return c.p1 == c.p2 ? "ramp needs x0 != x1" : "";
            case CurveKind::Saturating:
                return c.p1 > 0.0f ? "" : "saturating needs s > 0";
            case CurveKind::Gaussian:
                return c.p2 > 0.0f ? "" : "gaussian needs width > 0";
            case CurveKind::Decay:
                return c.p1 > 0.0f ? "" : "decay needs tau > 0";
            case CurveKind::Step:
                return "";
        }
        return "unknown kind";
    }

    /// y(x) for one curve (table above). Callers pass a validated curve
    /// (CurveProblem empty); an invalid one still returns a value in [0, 1].
    [[nodiscard]] inline float Evaluate(const Curve& c, float x) noexcept
    {
        if (std::isnan(x)) return 0.0f;
        double y = 0.0;
        const double xd = x;
        switch (c.kind) {
            case CurveKind::Logistic: {
                const double z = -static_cast<double>(c.p2) * (xd - c.p1);
                // exp overflows to inf for z > ~709, and 1 / (1 + inf) is 0: fine.
                y = 1.0 / (1.0 + std::exp(z));
                break;
            }
            case CurveKind::Linear:
            case CurveKind::Quadratic: {
                const double span = static_cast<double>(c.p2) - c.p1;
                if (span == 0.0) {
                    y = xd >= c.p1 ? 1.0 : 0.0;
                } else {
                    y = std::clamp((xd - c.p1) / span, 0.0, 1.0);
                }
                if (c.kind == CurveKind::Quadratic) y *= y;
                break;
            }
            case CurveKind::Saturating: {
                if (xd <= 0.0 || c.p1 <= 0.0f) {
                    y = 0.0;
                } else {
                    y = std::log1p(static_cast<double>(c.p1) * xd) / std::log1p(static_cast<double>(c.p1));
                }
                break;
            }
            case CurveKind::Gaussian: {
                const double w = c.p2;
                if (w <= 0.0) {
                    y = xd == c.p1 ? 1.0 : 0.0;
                } else {
                    const double d = xd - c.p1;
                    y = std::exp(-(d * d) / (2.0 * w * w));
                }
                break;
            }
            case CurveKind::Step:
                y = xd >= c.p1 ? 1.0 : 0.0;
                break;
            case CurveKind::Decay:
                if (xd <= 0.0) {
                    y = 1.0;
                } else if (c.p1 <= 0.0f) {
                    y = 0.0;
                } else {
                    y = std::exp(-xd / c.p1);
                }
                break;
        }
        if (!(y >= 0.0)) y = 0.0;  // NaN guard
        return static_cast<float>(std::min(y, 1.0));
    }

    /// "logistic 0.5 10" (kind, p1, p2; p2 may be left out for the one-
    /// parameter kinds). Separators: spaces, tabs or commas. nullopt when the
    /// text does not parse or the curve is invalid (CurveProblem).
    [[nodiscard]] inline std::optional<Curve> ParseCurve(std::string_view text)
    {
        auto isSep = [](char ch) { return ch == ' ' || ch == '\t' || ch == ','; };
        std::string_view tokens[4];
        int n = 0;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && isSep(text[i])) ++i;
            if (i >= text.size()) break;
            const size_t start = i;
            while (i < text.size() && !isSep(text[i])) ++i;
            if (n == 4) return std::nullopt;
            tokens[n++] = text.substr(start, i - start);
        }
        if (n < 2 || n > 3) return std::nullopt;
        std::string kindName(tokens[0]);
        std::transform(kindName.begin(), kindName.end(), kindName.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const auto kind = KindFromName(kindName);
        if (!kind) return std::nullopt;
        auto num = [](std::string_view t, float& out) {
            const auto r = std::from_chars(t.data(), t.data() + t.size(), out);
            return r.ec == std::errc{} && r.ptr == t.data() + t.size();
        };
        Curve c{ *kind, 0.0f, 0.0f };
        if (!num(tokens[1], c.p1)) return std::nullopt;
        if (n == 3 && !num(tokens[2], c.p2)) return std::nullopt;
        if (n == 2 && (c.kind == CurveKind::Logistic || c.kind == CurveKind::Linear ||
                       c.kind == CurveKind::Quadratic || c.kind == CurveKind::Gaussian)) {
            return std::nullopt;  // these need both parameters
        }
        if (!CurveProblem(c).empty()) return std::nullopt;
        return c;
    }

    /// The text ParseCurve reads back to the same curve (shortest round-trip
    /// float text).
    [[nodiscard]] inline std::string FormatCurve(const Curve& c)
    {
        auto fmt = [](float v) {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            return std::string(buf, r.ptr);
        };
        return std::string(KindName(c.kind)) + " " + fmt(c.p1) + " " + fmt(c.p2);
    }
}
