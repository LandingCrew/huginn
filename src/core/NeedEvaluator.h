#pragma once

// =============================================================================
// NEED EVALUATOR -- NeedSnapshot to the need vector
// =============================================================================
// need_k = curve_k(input_k(snapshot)), for the 92 needs of needs.csv. The
// input of each need is the csv's r3_input column, written out in
// NeedInputs (NeedEvaluator.cpp, one case per need, in csv order); the curve
// is the csv's default (curve_kind, curve_p1, curve_p2) or the [Needs] INI
// override.
//
// Logged only in R3: the rules keep scoring and nothing reads the vector but
// the log and `hg needs` (needs/NeedMonitor.h).
//
// Signature() quantises each need to 0.05 (21 levels): the monitor logs a
// line when it changes, and from R8 it joins the pipeline's skip gate (map
// Phase 2), so a continuous need re-runs the pipeline when it moves a step,
// not on every float wobble. TimeDriven() says whether the signature would
// still change if no sensor moved at all -- a decaying damage sum, a combat
// timer, a submerged timer -- which a gate needs to keep ticking until the
// vector settles.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "NeedIds.h"
#include "NeedSnapshot.h"
#include "ResponseCurve.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Huginn::Core::Needs
{
    using NeedArray = std::array<float, kNeedCount>;
    using CurveTable = std::array<Curve, kNeedCount>;

    struct NeedVector
    {
        NeedArray value{};  // curve output, [0, 1]
        NeedArray input{};  // what went into the curve (r3_input)
    };

    /// The csv defaults (NeedIds.h).
    [[nodiscard]] CurveTable DefaultCurves() noexcept;

    /// input_k for every need: needs.csv's r3_input column.
    [[nodiscard]] NeedArray NeedInputs(const NeedSnapshot& s) noexcept;

    [[nodiscard]] NeedVector EvaluateNeeds(const NeedSnapshot& s, const CurveTable& curves) noexcept;

    // --- Skip-gate signature --------------------------------------------------
    inline constexpr int kSignatureLevels = 20;  // step 1/20 = 0.05
    using NeedSignature = std::array<std::uint8_t, kNeedCount>;

    /// floor(v * 20 + 0.5) per need: 0..20.
    [[nodiscard]] std::uint8_t SignatureLevel(float v) noexcept;
    [[nodiscard]] NeedSignature Signature(const NeedArray& values) noexcept;

    /// The snapshot `dt` seconds later with no sensor change: the seconds-ago
    /// timers and the submerged timer advance, the damage sums decay with
    /// kDamageDecayTauSec. Nothing else moves by itself.
    [[nodiscard]] NeedSnapshot Advance(const NeedSnapshot& s, float dtSec) noexcept;

    /// True when the signature of `s` differs from the signature it settles
    /// to with time alone (Advance by a very long time): the vector is still
    /// moving although no sensor will.
    [[nodiscard]] bool TimeDriven(const NeedSnapshot& s, const CurveTable& curves) noexcept;
}
