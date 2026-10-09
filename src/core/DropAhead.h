#pragma once

// =============================================================================
// DROP AHEAD -- the geometry of the drop_ahead sensor
// =============================================================================
// needs.csv drop_ahead (decided with the user 2026-10-08): a Havok ray cast
// straight down from 2-3 points ahead of the player (~1, 2.5 and 4 m along
// the movement, else the facing), from waist height, ~4000 units long; drop =
// feet Z - hit Z, the largest over the points; no hit = a very big drop;
// measured to the water surface when water lies above the hit. The game side
// (state/DropAheadProbe.cpp) only casts the rays (terrain and statics, not
// actors) and reads the water height; the points, the direction and the drop
// are computed here, the probe sequence (ProbeAll) included, over an
// injected ray cast. Each probe's start is first checked reachable by two
// horizontal picks, at waist and at knee height, from the previous point (the
// player for the first); a hit of any kind blocks, and the first blocked
// point and every one beyond it are unknown, so rising ground, a wall, a
// parapet or an invisible wall at an edge never reads as a cliff.
//
// Skyrim units: 1 m ~ 70 units. Heading: angle Z in radians, 0 = +Y (north),
// increasing clockwise, so forward = (sin z, cos z).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <cstddef>

namespace Huginn::Core::Needs
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct DropProbeConfig
    {
        std::array<float, 3> distances{ 70.0f, 175.0f, 280.0f };  // ~1, 2.5, 4 m ahead
        float waistHeight = 64.0f;   // above the feet; half a 128-unit actor
        float kneeHeight = 24.0f;    // the second reachability pick: a parapet under the waist
        float rayLength = 4000.0f;
        float minMoveSpeed = 20.0f;  // units/s below which the facing is used
    };

    /// One probe's result, as the game read it. `known` is false when the
    /// probe could not be read: its start point is not reachable from the
    /// player at waist height (a wall or a door in front, rising ground that
    /// buries the start), or the ray ran out of recasts through actors and
    /// clutter. An unknown probe counts as nothing -- never as a drop. A known
    /// probe with no `hit` is a real void under the start (a very big drop).
    /// `waterZ` is the water surface at the probe's XY when `waterKnown`.
    struct ProbeHit
    {
        bool hit = false;
        float hitZ = 0.0f;
        bool waterKnown = false;
        float waterZ = 0.0f;
        bool known = true;
    };

    /// Unit XY direction to probe along: the horizontal movement when the
    /// player moves at least minMoveSpeed, else the facing.
    struct Dir2
    {
        float x = 0.0f;
        float y = 1.0f;
    };

    [[nodiscard]] inline Dir2 ProbeDirection(Vec3 velocity, float facingYawRad, float minMoveSpeed) noexcept
    {
        const float speed = std::hypot(velocity.x, velocity.y);
        if (std::isfinite(speed) && speed >= minMoveSpeed && speed > 0.0f) {
            return { velocity.x / speed, velocity.y / speed };
        }
        if (!std::isfinite(facingYawRad)) return {};
        return { std::sin(facingYawRad), std::cos(facingYawRad) };
    }

    /// Horizontal velocity from two positions `dtSec` apart (zero when dt is
    /// not positive). Z is left 0: only the heading matters.
    [[nodiscard]] inline Vec3 HorizontalVelocity(Vec3 previous, Vec3 current, float dtSec) noexcept
    {
        if (!(dtSec > 0.0f)) return {};
        return { (current.x - previous.x) / dtSec, (current.y - previous.y) / dtSec, 0.0f };
    }

    /// Ray start points: `distances` ahead of the feet along `dir`, at waist height.
    [[nodiscard]] inline std::array<Vec3, 3> ProbeStarts(Vec3 feet, Dir2 dir, const DropProbeConfig& cfg) noexcept
    {
        std::array<Vec3, 3> out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = { feet.x + dir.x * cfg.distances[i], feet.y + dir.y * cfg.distances[i], feet.z + cfg.waistHeight };
        }
        return out;
    }

    /// The ray's end: straight down rayLength from the start.
    [[nodiscard]] inline Vec3 ProbeEnd(Vec3 start, const DropProbeConfig& cfg) noexcept
    {
        return { start.x, start.y, start.z - cfg.rayLength };
    }

    /// The drop under one probe: feet Z down to the surface (the hit, or the
    /// water above it, or the ray's bottom when nothing was hit). Never
    /// negative: ground ahead that rises is no drop.
    [[nodiscard]] inline float DropAtProbe(float feetZ, const ProbeHit& h, const DropProbeConfig& cfg) noexcept
    {
        const float startZ = feetZ + cfg.waistHeight;
        float surface = h.hit ? h.hitZ : startZ - cfg.rayLength;
        if (h.waterKnown && std::isfinite(h.waterZ) && h.waterZ > surface) {
            surface = h.waterZ;
        }
        const float drop = feetZ - surface;
        return std::isfinite(drop) ? std::max(drop, 0.0f) : 0.0f;
    }

    /// drop_ahead: the largest drop over the known probes; -1 (not measured)
    /// when no probe is known.
    template <std::size_t N>
    [[nodiscard]] float DropAhead(float feetZ, const std::array<ProbeHit, N>& hits, const DropProbeConfig& cfg) noexcept
    {
        float drop = -1.0f;
        for (const auto& h : hits) {
            if (h.known) drop = std::max(drop, DropAtProbe(feetZ, h, cfg));
        }
        return drop;
    }

    /// The waist-height point the reachability pick starts from: above the
    /// feet, at the player.
    [[nodiscard]] inline Vec3 ProbeOrigin(Vec3 feet, const DropProbeConfig& cfg) noexcept
    {
        return { feet.x, feet.y, feet.z + cfg.waistHeight };
    }

    // ------------------------------------------------------------------------
    // The probe sequence, over an injected ray cast (the game casts Havok
    // rays; the host tests a scripted world).
    // ------------------------------------------------------------------------

    /// Horizontal: a reachability pick -- ANY hit blocks, whatever it is (an
    /// invisible wall or a collision box at a cliff edge too). Down: the
    /// surface ray -- the caster casts through actors and clutter and reports
    /// the first ground hit, or Exhausted when it ran out of recasts.
    enum class RayKind { Horizontal, Down };

    struct RayResult
    {
        enum class Outcome { Clear, Hit, Exhausted } outcome = Outcome::Clear;
        float distance = 0.0f;  // along the ray, to the hit
    };

    /// cast(Vec3 from, Vec3 unitDir, float length, RayKind) -> RayResult.
    ///
    /// For each probe point in turn: two horizontal picks to it from the
    /// previous point (the player for the first), at waist and at knee height
    /// -- the knee pick catches a parapet or a low wall under the waist with a
    /// void behind it. Either blocked: this point and every one beyond it are
    /// unknown (rising ground that buries the point, a wall, a door, a fence).
    /// Otherwise a ray straight down from the point at waist height: a hit is
    /// the surface, no hit a real void, Exhausted makes this one point
    /// unknown. Water is not read here (the game adds it after).
    template <class Cast>
    [[nodiscard]] std::array<ProbeHit, 3> ProbeAll(Vec3 feet, Dir2 dir, const DropProbeConfig& cfg, Cast&& cast)
    {
        std::array<ProbeHit, 3> hits{};
        const auto starts = ProbeStarts(feet, dir, cfg);
        Vec3 previous = feet;  // XY of the last reached point
        bool blocked = false;
        for (std::size_t i = 0; i < starts.size(); ++i) {
            auto& h = hits[i];
            if (!blocked) {
                for (const float height : { cfg.waistHeight, cfg.kneeHeight }) {
                    const Vec3 from{ previous.x, previous.y, feet.z + height };
                    const float dx = starts[i].x - from.x;
                    const float dy = starts[i].y - from.y;
                    const float length = std::hypot(dx, dy);
                    if (!(length > 0.0f)) continue;
                    const Vec3 unit{ dx / length, dy / length, 0.0f };
                    if (cast(from, unit, length, RayKind::Horizontal).outcome != RayResult::Outcome::Clear) {
                        blocked = true;
                        break;
                    }
                }
            }
            if (blocked) {
                h.known = false;
                continue;
            }
            previous = starts[i];
            const RayResult down = cast(starts[i], Vec3{ 0.0f, 0.0f, -1.0f }, cfg.rayLength, RayKind::Down);
            switch (down.outcome) {
                case RayResult::Outcome::Hit:
                    h.hit = true;
                    h.hitZ = starts[i].z - down.distance;
                    break;
                case RayResult::Outcome::Clear:
                    h.hit = false;
                    break;
                case RayResult::Outcome::Exhausted:
                    h.known = false;
                    break;
            }
        }
        return hits;
    }
}
