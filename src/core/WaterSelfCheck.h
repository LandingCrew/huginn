#pragma once

// =============================================================================
// WATER SELF-CHECK -- the drop-ahead probe tests its own water reading (0.23.23)
// =============================================================================
// The probe (state/DropAheadProbe.cpp) reads each point's water with the
// cell's TESObjectCELL::GetWaterHeight. On 2026-10-10 (LoreRim, 11:23-11:24)
// it read a surface at -3008 at points where the player then stood with the
// feet at -3242 and -3372 -- 234 and 364 units under that surface -- while the
// player's own water state never changed (no `[StateManager] Water:` line:
// the engine never had the player swimming or under). Three of six landings
// there hurt a little. So that plane was very likely false, and through the
// deep-water rule (core/DropAhead.h kSafeLandingDepth) it raised
// deep_water_ahead and zeroed real drops. It is not reproducible on demand, so
// the probe now checks the reading where it can be checked: at the player.
//
// Each probe also reads the water at the player's own XY. When that surface
// lies more than kFalseWaterDepth above the feet while the engine has the
// player neither swimming nor under water, standing on the ground (not
// airborne, not mounted), the plane cannot be water there. Held for
// kFalseWaterHoldSec on the same plane (not a landing's or a load's
// transient), the plane -- (cell, height) -- is proven false: the probe logs
// it once and, until the next game load, treats water from that cell at that
// height (within kWaterPlaneTolerance) as unknown (WaterPlaneBlacklist).
//
// The opposite check is the reading's confirmation: swimming, the probe's
// water at the player should be the engine's own water for the player
// (TESObjectREFR::GetWaterHeight, relevantWaterHeight) within
// kWaterPlaneTolerance. A swim in a blacklisted plane that matches the
// engine's water proves the blacklist wrong; the plane is taken off it.
//
// Pure: standard library only (src/core/README.md). The decisions are here;
// the game side only reads the engine and logs.
// =============================================================================

#include "core/DropAhead.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Huginn::Core::Needs
{
    /// A surface this far above the feet while the engine says the player is
    /// not in water cannot be water there. The session logs put swimming at
    /// feet ~83-100 under the surface: every `[StateManager] Water: ...
    /// swimming=true` line of 2026-10-10 (LoreRim, both sessions) has the
    /// head (feet + 120) 20-37 units over the water when the swim starts, and
    /// the swim ends with the head 36-74 over it (feet 46-84 under). A player
    /// standing, not swimming, in real water so has the feet at most ~100
    /// under its surface. 150 leaves 50 units of margin over that (a swim
    /// flag a frame late, a placed water object read a little off its
    /// plane), and is still well under the 234 and 364 of the false plane of
    /// 2026-10-10 11:23. A real plane wrongly called false costs that cell's
    /// water for the rest of the load: a cliff over it reads as a drop, as
    /// every cliff over water did before 0.23.19.
    inline constexpr float kFalseWaterDepth = 150.0f;

    /// How long the contradiction must hold, on one plane, before the plane
    /// is called false: past a landing (the swim flag and the feet settle
    /// within a few frames) and a load (the cell's water attaching). ~5
    /// probes at the 100 ms cadence.
    inline constexpr double kFalseWaterHoldSec = 0.5;

    /// A gap between two samples longer than this restarts the hold: the hook
    /// stopped (a menu paused the game, a load), and what held before the gap
    /// says nothing about what holds after it.
    inline constexpr double kFalseWaterMaxGapSec = 0.5;

    /// Two heights within this are one plane: the blacklist match, the hold's
    /// "same plane" and the swim check. A water plane is flat; the swim check
    /// of 2026-10-10 13:51 (a cave pool) read -1040 from both sides exactly.
    inline constexpr float kWaterPlaneTolerance = 4.0f;

    [[nodiscard]] inline bool SamePlane(float a, float b) noexcept
    {
        return std::fabs(a - b) <= kWaterPlaneTolerance;  // NaN: never the same
    }

    /// What one probe saw at the player's own position.
    struct OwnWaterSample
    {
        bool waterKnown = false;  // the probe's water read at the player's XY (resolved, not blacklisted)
        float waterZ = 0.0f;
        float feetZ = 0.0f;
        bool swimming = false;    // the engine's swim flag
        bool underwater = false;  // head under the engine's own water for the player
        bool airborne = false;
        bool mounted = false;     // the player's Z is the saddle, not the feet
    };

    /// The plane is more than kFalseWaterDepth over the feet of a player the
    /// engine has on the ground and out of the water.
    [[nodiscard]] inline bool WaterContradictsPlayer(const OwnWaterSample& s) noexcept
    {
        if (!s.waterKnown || !IsUsableWaterHeight(s.waterZ) || !std::isfinite(s.feetZ)) return false;
        if (s.swimming || s.underwater || s.airborne || s.mounted) return false;
        return s.waterZ - s.feetZ > kFalseWaterDepth;
    }

    /// A water plane: the cell it was read from (its form ID) and its height.
    struct WaterPlane
    {
        std::uint32_t cell = 0;
        float z = 0.0f;
    };

    [[nodiscard]] inline bool SamePlane(const WaterPlane& a, const WaterPlane& b) noexcept
    {
        return a.cell == b.cell && SamePlane(a.z, b.z);
    }

    /// The hold: Update once per probe; true exactly once when the
    /// contradiction has held kFalseWaterHoldSec on one plane with no gap
    /// over kFalseWaterMaxGapSec. Anything else (no contradiction, another
    /// plane, a gap) restarts it.
    class FalseWaterHold
    {
    public:
        bool Update(double nowSec, bool contradicts, WaterPlane plane) noexcept
        {
            if (!contradicts || !std::isfinite(nowSec)) {
                Reset();
                return false;
            }
            const bool gap = lastAt_ >= 0.0 && !(nowSec - lastAt_ <= kFalseWaterMaxGapSec);
            if (startAt_ < 0.0 || gap || !SamePlane(plane, plane_)) {
                startAt_ = nowSec;
                plane_ = plane;
                fired_ = false;
            }
            lastAt_ = nowSec;
            if (fired_ || nowSec - startAt_ < kFalseWaterHoldSec) return false;
            fired_ = true;
            return true;
        }

        void Reset() noexcept
        {
            startAt_ = -1.0;
            lastAt_ = -1.0;
            fired_ = false;
        }

        /// Seconds the current contradiction has held (0 when none).
        [[nodiscard]] double HeldSec(double nowSec) const noexcept
        {
            return startAt_ < 0.0 ? 0.0 : nowSec - startAt_;
        }

    private:
        WaterPlane plane_{};
        double startAt_ = -1.0;
        double lastAt_ = -1.0;
        bool fired_ = false;
    };

    /// The planes proven false this game load. Small and fixed: a load that
    /// finds more than kCapacity false planes has a problem a list will not
    /// fix, and the log says so (Add returns Full).
    class WaterPlaneBlacklist
    {
    public:
        static constexpr std::size_t kCapacity = 16;

        enum class AddResult { Added, Known, Full };

        AddResult Add(WaterPlane p) noexcept
        {
            if (Matches(p.cell, p.z)) return AddResult::Known;
            if (size_ >= kCapacity) return AddResult::Full;
            planes_[size_++] = p;
            return AddResult::Added;
        }

        /// Water from `cell` at `z` lies on a plane proven false.
        [[nodiscard]] bool Matches(std::uint32_t cell, float z) const noexcept
        {
            for (std::size_t i = 0; i < size_; ++i) {
                if (SamePlane(planes_[i], WaterPlane{ cell, z })) return true;
            }
            return false;
        }

        /// Take the plane off (a swim proved it real). True when one was.
        bool Remove(std::uint32_t cell, float z) noexcept
        {
            for (std::size_t i = 0; i < size_; ++i) {
                if (SamePlane(planes_[i], WaterPlane{ cell, z })) {
                    planes_[i] = planes_[--size_];
                    return true;
                }
            }
            return false;
        }

        void Clear() noexcept { size_ = 0; }
        [[nodiscard]] std::size_t Size() const noexcept { return size_; }

    private:
        std::array<WaterPlane, kCapacity> planes_{};
        std::size_t size_ = 0;
    };

    /// The swim check: swimming, the probe's water at the player against the
    /// engine's own water for the player. Unchecked when either is no usable
    /// height or the player is not swimming.
    enum class SwimWaterCheck { Unchecked, Match, Mismatch };

    [[nodiscard]] inline SwimWaterCheck CheckSwimWater(bool swimming, bool probeKnown, float probeZ,
                                                       float engineZ) noexcept
    {
        if (!swimming || !probeKnown || !IsUsableWaterHeight(probeZ) || !IsUsableWaterHeight(engineZ)) {
            return SwimWaterCheck::Unchecked;
        }
        return SamePlane(probeZ, engineZ) ? SwimWaterCheck::Match : SwimWaterCheck::Mismatch;
    }
}
