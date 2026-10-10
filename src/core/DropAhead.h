#pragma once

// =============================================================================
// DROP AHEAD -- the geometry of the drop_ahead sensor
// =============================================================================
// needs.csv drop_ahead (decided with the user 2026-10-08): a Havok ray cast
// straight down from 2-3 points ahead of the player (~1, 2.5 and 4 m along
// the movement, else the facing), from waist height, 16000 units long (0.23.22;
// was 4000, see DropProbeConfig::rayLength); drop = feet Z - hit Z, the
// largest over the points; no hit = a very big drop.
// Water (0.23.19): the down ray passes through water to the bed, so the depth
// under a point is the water surface down to the hit (to the ray's bottom when
// nothing was hit). A landing in water at least kSafeLandingDepth deep is no
// drop -- Skyrim takes no fall damage there -- and shallower water is measured
// to its surface. The same pass gives deep_water_ahead: the deepest water over
// the known points (MeasureAhead). The game side
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
#include <optional>

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
        // The down ray (0.23.22: 16000, was 4000). Its job past the ~700
        // units where drop_ahead saturates is only to reach the landing, so
        // a cliff over water reads as water. On 2026-10-10 (LoreRim) a cliff
        // stood 5474 over the sea (feet -8526, the sea at -14000): the
        // 4000-unit ray ended at -12462, above the water, and the landing
        // read as a void, a 3936 drop with no water; the player jumped and
        // landed in the sea unhurt. From waist height a 16000 ray reaches
        // 15936 under the feet, so water at least kSafeLandingDepth over the
        // ray's bottom is seen from any cliff up to 15808 over it (~225 m),
        // 2.9 times that cliff. A cliff higher still reads a void, a 15936
        // drop (drop_ahead 1, as any drop past ~700 does): the cost of a
        // ray too short is only a water landing read as a cliff. Water known
        // below the ray's bottom with no hit is water of unknown depth under
        // a void, and stays depth 0 and the void's drop (WaterDepthAtProbe,
        // DropAtProbe). Whether the length costs time is not measured: the
        // Tracy trace that put one ProbeAll (every pick) at ~74 us was taken
        // with the 4000 ray. A ray that hits ground near its top is expected
        // to cost about the same at any length, one over a void or deep
        // water to cast farther; a new trace would tell.
        float rayLength = 16000.0f;
        float minMoveSpeed = 20.0f;  // units/s below which the facing is used
    };

    /// One probe's result, as the game read it. `known` is false when the
    /// probe could not be read: its start point is not reachable from the
    /// player at waist height (a wall or a door in front, rising ground that
    /// buries the start), or the ray ran out of recasts through actors and
    /// clutter. An unknown probe counts as nothing -- never as a drop. A known
    /// probe with no `hit` is a real void under the start (a very big drop).
    /// `waterZ` is the water surface at the probe's XY when `waterKnown`.
    /// `startZ` is the Z the down ray was really cast from: ProbeAll casts
    /// each point from waist height above the PREVIOUS point's ground, not
    /// the player's, so the ray's bottom (a no-hit probe's surface) follows
    /// it. Empty means feet + waist, the first point's start (and what a
    /// hand-made hit in a test means).
    struct ProbeHit
    {
        bool hit = false;
        float hitZ = 0.0f;
        bool waterKnown = false;
        float waterZ = 0.0f;
        bool known = true;
        std::optional<float> startZ;
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

    /// A move no walk, sprint or horse makes between two probes (100 ms apart):
    /// a coc, a load door, fast travel. The heading then comes from the
    /// facing, not from the jump.
    inline constexpr float kTeleportDistance = 500.0f;

    [[nodiscard]] inline bool IsTeleport(Vec3 previous, Vec3 current) noexcept
    {
        const float d = std::hypot(current.x - previous.x, current.y - previous.y, current.z - previous.z);
        return !(d <= kTeleportDistance);  // NaN counts as a jump
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

    /// Water at least this deep is a safe landing: Skyrim takes no fall damage
    /// when the player lands in deep enough water, however far the fall. The
    /// game's own threshold is NOT known here. 128 units -- one actor height,
    /// ~1.8 m, water over a standing player's head -- is meant to err deep: a
    /// threshold too deep only leaves a cliff over shallower water reading as
    /// a drop (as every cliff over water did before 0.23.19); one too shallow
    /// would call a harmful landing safe. An in-game check (roadmap R3): a
    /// cliff over deep water must read drop_ahead 0, one onto a shallow stream
    /// a drop.
    inline constexpr float kSafeLandingDepth = 128.0f;

    /// A water height the game can report that is no water: a cell's XCLW
    /// "use the worldspace default" sentinel (>= 2147483600, CommonLib
    /// TESObjectCELL::GetExteriorWaterHeight; ResolveWaterHeight below swaps
    /// it for the worldspace default first) and the -infinity / -FLT_MAX of
    /// a cell without water. No real surface is anywhere near 1e6 units from
    /// the origin (a whole worldspace spans a few hundred thousand), so any
    /// height that is not finite or is that far out is not water.
    inline constexpr float kMaxWaterHeightAbs = 1.0e6f;

    [[nodiscard]] inline bool IsUsableWaterHeight(float z) noexcept
    {
        return std::isfinite(z) && std::fabs(z) < kMaxWaterHeightAbs;
    }

    /// XCLW's "use the worldspace default water" value. CommonLib v3.7.0
    /// src/RE/T/TESObjectCELL.cpp:95 takes a cell's height as its own only
    /// when `waterHeight < 2147483600.0f`, and otherwise falls back to the
    /// worldspace default; this is the same threshold. (CommonLib's test,
    /// `!(z < threshold)`, would also send a NaN to the default; here a NaN
    /// is no sentinel and stays unknown.)
    inline constexpr float kDefaultWaterSentinel = 2147483600.0f;

    [[nodiscard]] inline bool IsDefaultWaterSentinel(float z) noexcept
    {
        return z >= kDefaultWaterSentinel;
    }

    /// One probe point's water, decided from what the game returned.
    struct WaterHeightRead
    {
        bool known = false;        // a usable surface height
        float z = 0.0f;            // the surface, when known
        bool fromDefault = false;  // the raw read was the sentinel; z is the resolved default
    };

    /// `raw`: the height the cell's GetWaterHeight returned (it returned
    /// true). `resolvedDefault`: the cell's resolved default water (the game
    /// side's TESObjectCELL::GetExteriorWaterHeight), asked only when `raw`
    /// is the sentinel; nullopt when there is nothing to ask (an interior).
    /// A sentinel resolves to the default when that is a usable height and is
    /// unknown otherwise -- never the sentinel's own huge number, and never
    /// the -FLT_MAX GetExteriorWaterHeight returns for "no water". Any other
    /// raw height is used as is when usable, unknown when not.
    [[nodiscard]] inline WaterHeightRead ResolveWaterHeight(float raw, std::optional<float> resolvedDefault) noexcept
    {
        if (IsDefaultWaterSentinel(raw)) {
            if (resolvedDefault && IsUsableWaterHeight(*resolvedDefault)) return { true, *resolvedDefault, true };
            return {};
        }
        if (IsUsableWaterHeight(raw)) return { true, raw, false };
        return {};
    }

    /// The Z the probe's down ray was cast from (see ProbeHit::startZ), and
    /// its bottom: the surface of a probe with no hit.
    [[nodiscard]] inline float RayStartZ(float feetZ, const ProbeHit& h, const DropProbeConfig& cfg) noexcept
    {
        return (h.startZ && std::isfinite(*h.startZ)) ? *h.startZ : feetZ + cfg.waistHeight;
    }

    [[nodiscard]] inline float RayBottomZ(float feetZ, const ProbeHit& h, const DropProbeConfig& cfg) noexcept
    {
        return RayStartZ(feetZ, h, cfg) - cfg.rayLength;
    }

    /// Water at the probe that counts: known and a usable height.
    [[nodiscard]] inline bool HasWater(const ProbeHit& h) noexcept
    {
        return h.waterKnown && IsUsableWaterHeight(h.waterZ);
    }

    /// The water depth under one probe: the surface down to the hit, or with
    /// no hit down to the ray's bottom (the water is at least that deep). 0
    /// when no usable water is known there or it lies at or below the hit (a
    /// pool under a bridge deck). Never NaN.
    [[nodiscard]] inline float WaterDepthAtProbe(float feetZ, const ProbeHit& h, const DropProbeConfig& cfg) noexcept
    {
        if (!HasWater(h)) return 0.0f;
        const float bottom = h.hit ? h.hitZ : RayBottomZ(feetZ, h, cfg);
        const float depth = h.waterZ - bottom;
        return std::isfinite(depth) ? std::max(depth, 0.0f) : 0.0f;
    }

    /// The drop under one probe: feet Z down to the surface (the hit, or the
    /// water above it, or the ray's bottom when nothing was hit). Never
    /// negative: ground ahead that rises is no drop. A landing in water at
    /// least kSafeLandingDepth deep is no drop at all.
    [[nodiscard]] inline float DropAtProbe(float feetZ, const ProbeHit& h, const DropProbeConfig& cfg) noexcept
    {
        if (WaterDepthAtProbe(feetZ, h, cfg) >= kSafeLandingDepth) return 0.0f;
        float surface = h.hit ? h.hitZ : RayBottomZ(feetZ, h, cfg);
        if (HasWater(h) && h.waterZ > surface) {
            surface = h.waterZ;
        }
        const float drop = feetZ - surface;
        return std::isfinite(drop) ? std::max(drop, 0.0f) : 0.0f;
    }

    /// What one probe pass reads. Both -1 (not measured) when no probe is
    /// known; an unknown probe counts as nothing in either.
    struct AheadReading
    {
        float drop = -1.0f;        // drop_ahead: the largest drop over the known probes, units
        float waterDepth = -1.0f;  // deep_water_ahead: the deepest water over them, units; 0 = none
    };

    template <std::size_t N>
    [[nodiscard]] AheadReading MeasureAhead(float feetZ, const std::array<ProbeHit, N>& hits,
                                            const DropProbeConfig& cfg) noexcept
    {
        AheadReading r;
        for (const auto& h : hits) {
            if (!h.known) continue;
            r.drop = std::max(r.drop, DropAtProbe(feetZ, h, cfg));
            r.waterDepth = std::max(r.waterDepth, WaterDepthAtProbe(feetZ, h, cfg));
        }
        return r;
    }

    /// drop_ahead alone: MeasureAhead's drop.
    template <std::size_t N>
    [[nodiscard]] float DropAhead(float feetZ, const std::array<ProbeHit, N>& hits, const DropProbeConfig& cfg) noexcept
    {
        return MeasureAhead(feetZ, hits, cfg).drop;
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
    /// For each probe point in turn, two horizontal picks to it from the
    /// previous point (the player for the first), at waist and at knee height
    /// above the PREVIOUS point's ground (the player's feet for the first,
    /// then each reached point's down-ray hit), so the picks climb a walkable
    /// slope with it:
    ///   - the waist pick blocked: a wall, a door, a fence, a slope too steep
    ///     to call walkable (rise > waist over the spacing) -- this point and
    ///     every one beyond it are unknown;
    ///   - the knee pick blocked: a parapet or low wall under the waist, or
    ///     ground rising past the knee. The down ray from waist height at the
    ///     point tells them apart: ground there at or above the knee height is
    ///     rising ground (the point is known, the surface is that ground);
    ///     lower ground or a void behind means an obstacle between (unknown,
    ///     and every point beyond it).
    /// Then a ray straight down from the point at waist height above the
    /// previous ground: a hit is the surface, no hit a real void, Exhausted
    /// (out of recasts) makes this one point unknown. Water is not read here
    /// (the game adds it after, at each point's XY and recorded startZ).
    ///
    /// Known limit: an obstacle open between the knee and the waist (a railing
    /// with a gap there) is not seen, so a void behind it reads as a cliff.
    template <class Cast>
    [[nodiscard]] std::array<ProbeHit, 3> ProbeAll(Vec3 feet, Dir2 dir, const DropProbeConfig& cfg, Cast&& cast)
    {
        std::array<ProbeHit, 3> hits{};
        const auto starts = ProbeStarts(feet, dir, cfg);
        Vec3 previous = feet;  // XY of the last reached point
        float groundZ = feet.z;  // the ground under it
        bool blocked = false;
        const Vec3 kDown{ 0.0f, 0.0f, -1.0f };

        auto horizontal = [&](Vec3 to, float height) {
            const Vec3 from{ previous.x, previous.y, groundZ + height };
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float length = std::hypot(dx, dy);
            if (!(length > 0.0f)) return true;
            const Vec3 unit{ dx / length, dy / length, 0.0f };
            return cast(from, unit, length, RayKind::Horizontal).outcome == RayResult::Outcome::Clear;
        };

        for (std::size_t i = 0; i < starts.size(); ++i) {
            auto& h = hits[i];
            if (blocked) {
                h.known = false;
                continue;
            }
            const Vec3 start{ starts[i].x, starts[i].y, groundZ + cfg.waistHeight };
            if (!horizontal(start, cfg.waistHeight)) {
                blocked = true;
                h.known = false;
                continue;
            }
            const bool kneeClear = horizontal(start, cfg.kneeHeight);
            const RayResult down = cast(start, kDown, cfg.rayLength, RayKind::Down);
            h.startZ = start.z;  // the real start: the ray's bottom and the water read follow it
            const bool risingGround = down.outcome == RayResult::Outcome::Hit &&
                                      start.z - down.distance >= groundZ + cfg.kneeHeight;
            if (!kneeClear && !risingGround) {
                blocked = true;
                h.known = false;
                continue;
            }
            switch (down.outcome) {
                case RayResult::Outcome::Hit:
                    h.hit = true;
                    h.hitZ = start.z - down.distance;
                    previous = start;
                    groundZ = h.hitZ;
                    break;
                case RayResult::Outcome::Clear:
                    h.hit = false;
                    previous = start;  // a void: the next pick still goes from here, at this height
                    break;
                case RayResult::Outcome::Exhausted:
                    h.known = false;
                    previous = start;
                    break;
            }
        }
        return hits;
    }
    // =========================================================================
    // The age of the last reading, in UNPAUSED seconds (0.23.16)
    // =========================================================================
    // The rays are cast from the PlayerCharacter::Update hook, which the game
    // does not call while it is paused (the console, the inventory ...), while
    // the update loop keeps polling. Aged on the wall clock, every reading was
    // "not measured" one second into any menu. Aged here, a paused poll adds
    // nothing, so the last reading stands until the game runs again and the
    // hook replaces it; an unpaused second without a new reading (the hook
    // stopped: a load, a skip it did not store) still makes it stale.
    //
    // A gap between two polls counts at most kMaxPollStepSec of unpaused time:
    // a loop that did not poll through a pause must not age the reading by the
    // whole pause when it resumes.
    class ReadingAge
    {
    public:
        static constexpr double kMaxPollStepSec = 0.5;

        /// One poll at `nowSec`. `readingAtSec` is the reading's stamp (-1:
        /// none); `paused` whether the game is paused at this poll. Returns the
        /// reading's age in unpaused seconds, or -1 with no reading.
        double Update(double nowSec, double readingAtSec, bool paused) noexcept
        {
            if (readingAtSec < 0.0) {
                Reset();
                return -1.0;
            }
            if (readingAtSec != readingAt_) {
                // A new reading: the hook took it while the game ran.
                readingAt_ = readingAtSec;
                age_ = std::max(0.0, nowSec - readingAtSec);
            }
            else if (lastNow_ >= 0.0 && !paused) {
                age_ += std::clamp(nowSec - lastNow_, 0.0, kMaxPollStepSec);
            }
            lastNow_ = nowSec;
            return age_;
        }

        void Reset() noexcept
        {
            readingAt_ = -1.0;
            lastNow_ = -1.0;
            age_ = -1.0;
        }

        [[nodiscard]] double Age() const noexcept { return age_; }

    private:
        double readingAt_ = -1.0;
        double lastNow_ = -1.0;
        double age_ = -1.0;
    };
}
