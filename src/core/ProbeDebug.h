#pragma once

// =============================================================================
// THE DROP / EDGE PROBE, AS A PICTURE (0.24.40)
// =============================================================================
// What the debug overlay draws for one probe pass (core/DropAhead.h ProbeAll):
// every ray the pass cast, each a segment in world space with a colour that
// says what it found. Pure: the game layer records the casts
// (DropAheadProbe), this file colours them, the UI projects and draws them
// (ui/ProbeOverlay).
//
// Colours (the user, 2026-10-30): red is a fall -- a down ray whose drop is
// dangerous (kEdgeDropThreshold, the drop_ahead curve's centre, the line
// time_to_edge uses); blue is water under the probe (deep_water_ahead's
// reading); green is fine. A reachability pick that hit something, a down
// ray that ran out of recasts, and a point the pass never reached (a pick
// before it was blocked) are neutral, so red only ever means a fall.
// =============================================================================

#include "core/DropAhead.h"
#include "core/EdgeApproach.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Huginn::Core::Needs::ProbeDebug
{
    enum class Look : std::uint8_t
    {
        Okay,       // green: a clear pick, or ground within a safe drop
        Fall,       // red: a down ray whose drop is dangerous
        Water,      // blue: water under the probe
        Blocked,    // yellow: a pick that hit something (a wall, a parapet)
        Skipped,    // grey: out of recasts, or a point never reached
    };

    /// One cast as the game made it: ProbeAll's cast(from, unitDir, length, kind).
    struct Cast
    {
        Vec3 from;
        Vec3 dir;
        float length = 0.0f;
        RayKind kind = RayKind::Horizontal;
        RayResult result;
    };

    /// At most two picks and one down ray per point.
    inline constexpr std::size_t kMaxCasts = 9;

    struct Segment
    {
        Vec3 a;
        Vec3 b;
        Look look = Look::Okay;
    };

    /// A label at a down ray's end: the drop (or the water's depth) there.
    struct Mark
    {
        Vec3 at;
        Look look = Look::Okay;
        float drop = 0.0f;         // units; DropAtProbe
        float waterDepth = 0.0f;   // units; WaterDepthAtProbe (0: none)
        bool voidBelow = false;    // no hit: the ray found nothing to its end
    };

    struct Frame
    {
        std::array<Segment, kMaxCasts + 2> segments{};   // + the water surface ticks
        std::size_t segmentCount = 0;
        std::array<Mark, 3> marks{};
        std::size_t markCount = 0;
        std::array<Vec3, 3> unreached{};   // probe points the pass never reached
        std::size_t unreachedCount = 0;
        Vec3 feet;
        bool measured = false;   // false: a skipped pass, nothing to draw
    };

    /// A void's ray is drawn this far, not its full 16000 units: off screen
    /// it shows nothing, and its end is a cross that says "nothing below".
    inline constexpr float kVoidDrawLength = 1200.0f;

    /// The colour of a down ray at probe `h` (water filled in), from the feet.
    [[nodiscard]] inline Look DownLook(float feetZ, const ProbeHit& h, const DropProbeConfig& cfg) noexcept
    {
        if (!h.known) return Look::Skipped;
        if (WaterDepthAtProbe(feetZ, h, cfg) > 0.0f) return Look::Water;
        return DropAtProbe(feetZ, h, cfg) >= kEdgeDropThreshold ? Look::Fall : Look::Okay;
    }

    /// The probe point a down ray was cast at: the one whose XY it starts at.
    [[nodiscard]] inline std::size_t PointOf(const Vec3& from, const std::array<Vec3, 3>& starts) noexcept
    {
        std::size_t best = 0;
        float bestD = std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < starts.size(); ++i) {
            const float d = std::hypot(from.x - starts[i].x, from.y - starts[i].y);
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    }

    /// The picture of one pass: `casts` in the order ProbeAll made them, `hits`
    /// as the game finished them (water read in), `dir` the probe direction.
    template <std::size_t N>
    [[nodiscard]] Frame Build(Vec3 feet, Dir2 dir, const std::array<Cast, N>& casts, std::size_t castCount,
                              const std::array<ProbeHit, 3>& hits, const DropProbeConfig& cfg) noexcept
    {
        Frame f;
        f.feet = feet;
        f.measured = true;
        const auto starts = ProbeStarts(feet, dir, cfg);
        std::array<bool, 3> cast{};
        auto addSegment = [&](Vec3 a, Vec3 b, Look look) {
            if (f.segmentCount < f.segments.size()) f.segments[f.segmentCount++] = { a, b, look };
        };
        for (std::size_t c = 0; c < castCount && c < N; ++c) {
            const auto& k = casts[c];
            if (k.kind == RayKind::Horizontal) {
                const bool clear = k.result.outcome == RayResult::Outcome::Clear;
                const float len = clear ? k.length : k.result.distance;
                addSegment(k.from, { k.from.x + k.dir.x * len, k.from.y + k.dir.y * len, k.from.z + k.dir.z * len },
                           clear ? Look::Okay : Look::Blocked);
                continue;
            }
            const std::size_t i = PointOf(k.from, starts);
            cast[i] = true;
            const ProbeHit& h = hits[i];
            if (k.result.outcome == RayResult::Outcome::Exhausted) {
                addSegment(k.from, { k.from.x, k.from.y, k.from.z - cfg.waistHeight }, Look::Skipped);
                continue;
            }
            const Look look = DownLook(feet.z, h, cfg);
            const bool hit = k.result.outcome == RayResult::Outcome::Hit;
            const float len = hit ? k.result.distance : std::min(k.length, kVoidDrawLength);
            const Vec3 end{ k.from.x, k.from.y, k.from.z - len };
            addSegment(k.from, end, look);
            if (HasWater(h) && h.waterZ > end.z) {
                // the water surface: a short tick across the ray, along the probe direction
                // (above the ray's start too: a point reached down a slope casts from under it)
                const float t = 24.0f;
                addSegment({ k.from.x - dir.x * t, k.from.y - dir.y * t, h.waterZ },
                           { k.from.x + dir.x * t, k.from.y + dir.y * t, h.waterZ }, Look::Water);
            }
            if (f.markCount < f.marks.size()) {
                f.marks[f.markCount++] = { end, look, DropAtProbe(feet.z, h, cfg), WaterDepthAtProbe(feet.z, h, cfg), !hit };
            }
        }
        for (std::size_t i = 0; i < starts.size(); ++i) {
            if (!cast[i] && f.unreachedCount < f.unreached.size()) f.unreached[f.unreachedCount++] = starts[i];
        }
        return f;
    }
}
