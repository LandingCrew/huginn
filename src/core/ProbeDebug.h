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
// time_to_edge uses; a far point's drop is its step, EdgeReferenceZ), shallow
// water under it or not; blue is water and no dangerous drop (a safe landing,
// deep_water_ahead's reading); green is fine. A reachability pick that hit something, a down
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
#include <span>

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

    /// At most two picks and one down ray per point, near and far (0.24.40).
    inline constexpr std::size_t kMaxCasts = 3 * kMaxPoints;

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
        bool farPoint = false;     // a far point (time_to_edge only, 0.24.40)
    };

    struct Frame
    {
        std::array<Segment, kMaxCasts + kMaxPoints> segments{};   // + the water surface ticks
        std::size_t segmentCount = 0;
        std::array<Mark, kMaxPoints> marks{};
        std::size_t markCount = 0;
        std::array<Vec3, kMaxPoints> unreached{};   // probe points the pass never reached
        std::size_t unreachedCount = 0;
        float reach = 0.0f;   // the layout's farthest point (0.24.40)
        Vec3 feet;
        bool measured = false;   // false: a skipped pass, nothing to draw
    };

    /// A void's ray is drawn this far, not its full 16000 units: off screen
    /// it shows nothing, and its end is a cross that says "nothing below".
    inline constexpr float kVoidDrawLength = 1200.0f;

    /// The colour of a down ray at probe `h` (water filled in), its drop from
    /// `fromZ` (EdgeReferenceZ). A fall first (/code-review #262): a dangerous
    /// drop into shallow water is a fall, as time_to_edge counts it; blue only
    /// for water with no dangerous drop (DropAtProbe is 0 over a safe landing).
    [[nodiscard]] inline Look DownLook(float fromZ, const ProbeHit& h, const DropProbeConfig& cfg) noexcept
    {
        if (!h.known) return Look::Skipped;
        if (DropAtProbe(fromZ, h, cfg) >= kEdgeDropThreshold) return Look::Fall;
        return WaterDepthAtProbe(fromZ, h, cfg) > 0.0f ? Look::Water : Look::Okay;
    }

    /// The probe point a down ray was cast at: the one whose XY it starts at.
    [[nodiscard]] inline std::size_t PointOf(const Vec3& from, std::span<const Vec3> starts) noexcept
    {
        std::size_t best = 0;
        float bestD = std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < starts.size(); ++i) {
            const float dx = from.x - starts[i].x;
            const float dy = from.y - starts[i].y;
            const float d = dx * dx + dy * dy;   // squared: only the order matters
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    }

    /// The picture of one pass: `casts` in the order ProbeChain made them over
    /// `layout`, `hits` as the game finished them (water read in; one per
    /// layout point), `dir` the probe direction.
    template <std::size_t N>
    [[nodiscard]] Frame Build(Vec3 feet, Dir2 dir, const ProbeLayout& layout, const std::array<Cast, N>& casts,
                              std::size_t castCount, std::span<const ProbeHit> hits, const DropProbeConfig& cfg) noexcept
    {
        Frame f;
        f.feet = feet;
        f.measured = true;
        f.reach = layout.reach;
        const std::size_t points = std::min(layout.count, hits.size());
        std::array<Vec3, kMaxPoints> startAt{};
        for (std::size_t i = 0; i < points; ++i) startAt[i] = ProbeStartAt(feet, dir, layout.distances[i], cfg);
        const std::span<const Vec3> starts(startAt.data(), points);
        std::array<bool, kMaxPoints> cast{};
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
            if (points == 0) break;
            const std::size_t i = PointOf(k.from, starts);
            cast[i] = true;
            const ProbeHit& h = hits[i];
            if (k.result.outcome == RayResult::Outcome::Exhausted) {
                addSegment(k.from, { k.from.x, k.from.y, k.from.z - cfg.waistHeight }, Look::Skipped);
                continue;
            }
            const float fromZ = EdgeReferenceZ(feet.z, hits, i);
            const Look look = DownLook(fromZ, h, cfg);
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
            // No label for a point the pass could not read (a knee pick blocked
            // it after its down ray: /code-review #262 saw "drop 16050").
            if (h.known && f.markCount < f.marks.size()) {
                f.marks[f.markCount++] = { end, look, DropAtProbe(fromZ, h, cfg), WaterDepthAtProbe(fromZ, h, cfg), !hit,
                                           i >= kNearPoints };
            }
        }
        for (std::size_t i = 0; i < starts.size(); ++i) {
            if (!cast[i] && f.unreachedCount < f.unreached.size()) f.unreached[f.unreachedCount++] = starts[i];
        }
        return f;
    }
}
