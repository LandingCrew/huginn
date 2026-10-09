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
// are computed here.
//
// Skyrim units: 1 m ~ 70 units. Heading: angle Z in radians, 0 = +Y (north),
// increasing clockwise, so forward = (sin z, cos z).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
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
        float rayLength = 4000.0f;
        float minMoveSpeed = 20.0f;  // units/s below which the facing is used
    };

    /// One ray's result, as the game read it. `waterZ` is the water surface
    /// at the probe's XY when `waterKnown`.
    struct ProbeHit
    {
        bool hit = false;
        float hitZ = 0.0f;
        bool waterKnown = false;
        float waterZ = 0.0f;
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
            surface = std::min(h.waterZ, startZ);
        }
        const float drop = feetZ - surface;
        return std::isfinite(drop) ? std::max(drop, 0.0f) : 0.0f;
    }

    /// drop_ahead: the largest drop over the probes.
    template <std::size_t N>
    [[nodiscard]] float DropAhead(float feetZ, const std::array<ProbeHit, N>& hits, const DropProbeConfig& cfg) noexcept
    {
        float drop = 0.0f;
        for (const auto& h : hits) drop = std::max(drop, DropAtProbe(feetZ, h, cfg));
        return drop;
    }
}
