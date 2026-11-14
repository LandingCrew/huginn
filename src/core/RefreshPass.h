#pragma once

// =============================================================================
// REFRESH PASS (R8.1, 0.23.55) -- a buff is offered again before it runs out
// =============================================================================
// Doc 11 decision 4. The active-buff filters (CandidateFilters: invisibility,
// muffle, armor, summon_limit, warming, waterbreathing) drop an item
// whose effect is already up on the player. From 0.23.55 each lets the item
// through once the buff that blocks it has less than fRefreshPriorSec
// ([Scoring], default 5 s) left: the player can recast before it drops. An
// effect with no duration (an ability, a worn enchantment, any constant
// effect) never runs out, so it never lets the item through.
//
// "The buff that blocks it", per filter:
//   - invisibility, muffle, armor, warming, waterbreathing: every active
//     effect that sets the filter's flag; the block ends when the LAST of them
//     ends (Timer: the longest remaining time; a constant one never ends);
//   - resist (0.24.29): not here. The threshold test reads only the permanent
//     resistance (gear, abilities, race), which never runs out; a running
//     resist potion or spell times its own block (core/ResistPotions.h);
//   - summon_limit: a cast at the limit dismisses the oldest commanded summon
//     or thrall, so the block is that one's timer: Coverage::OldestRemaining,
//     the same value covered(i) reads (a thrall or any untimed summon as the
//     oldest: never due).
//
// The state layer computes the bits on each magic-effects poll
// (StateManager_MagicEffects.cpp, ActorBuffs::refreshDue), so a crossing marks
// the state changed and the pipeline runs on it; the filters read the bits.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Huginn::Core::Refresh
{
    /// A remaining time that never runs out (the same value as Coverage::kNoTimer).
    inline constexpr float kNoTimer = -1.0f;

    /// [Scoring] fRefreshPriorSec's default: doc 11 decision 4's prior centre.
    inline constexpr float kDefaultRefreshSec = 5.0f;

    /// The blocks the active-buff filters apply, one bit each in DueBits.
    enum class Block : std::uint8_t
    {
        invisibility,
        muffle,
        armor,
        summon_limit,
        warming,
        waterbreathing,
        _Count
    };
    inline constexpr std::size_t kBlockCount = static_cast<std::size_t>(Block::_Count);
    using DueBits = std::uint16_t;
    static_assert(kBlockCount <= sizeof(DueBits) * 8);

    /// For the log: the filter (Core::Filter::Reason name) each block belongs to.
    inline constexpr std::array<std::string_view, kBlockCount> kBlockNames{
        "invisibility", "muffle", "armor", "summon_limit", "warming", "waterbreathing",
    };

    /// The resistances the resist filter reads (core/ResistPotions.h).
    enum class Element : std::uint8_t { fire, frost, shock, poison, magic, _Count };
    inline constexpr std::size_t kElementCount = static_cast<std::size_t>(Element::_Count);

    [[nodiscard]] constexpr DueBits Bit(Block b) noexcept
    {
        return static_cast<DueBits>(1u << static_cast<unsigned>(b));
    }

    [[nodiscard]] constexpr bool IsDue(DueBits bits, Block b) noexcept
    {
        return (bits & Bit(b)) != 0;
    }

    /// Is a block that ends in `remainingSec` due for its refresh? Never for
    /// kNoTimer (no end), and never with refreshSec <= 0 (the pass off).
    [[nodiscard]] constexpr bool RefreshDue(float remainingSec, float refreshSec) noexcept
    {
        return remainingSec >= 0.0f && remainingSec < refreshSec;
    }

    /// The effects behind one flag: the block ends when the last one does.
    struct Timer
    {
        bool any = false;
        float remaining = kNoTimer;   // valid while any; kNoTimer: a constant effect among them

        constexpr void Add(float remainingSec) noexcept
        {
            if (!any) {
                any = true;
                remaining = remainingSec < 0.0f ? kNoTimer : remainingSec;
            }
            else if (remaining >= 0.0f) {
                remaining = remainingSec < 0.0f ? kNoTimer : std::max(remaining, remainingSec);
            }
        }
    };

    /// What the poll reads to decide which blocks are due.
    struct Inputs
    {
        Timer invisibility, muffle, armor, warming, waterbreathing;
        bool summonBlocked = false;                       // the summon limit reached (the filter's test)
        float summonLeft = kNoTimer;                      // Coverage::OldestRemaining
    };

    /// The blocks due for their refresh: blocking now, and ending within refreshSec.
    [[nodiscard]] constexpr DueBits ComputeDue(const Inputs& in, float refreshSec) noexcept
    {
        DueBits bits = 0;
        const auto timer = [&](const Timer& t, Block b) {
            if (t.any && RefreshDue(t.remaining, refreshSec)) bits |= Bit(b);
        };
        timer(in.invisibility, Block::invisibility);
        timer(in.muffle, Block::muffle);
        timer(in.armor, Block::armor);
        timer(in.warming, Block::warming);
        timer(in.waterbreathing, Block::waterbreathing);
        if (in.summonBlocked && RefreshDue(in.summonLeft, refreshSec)) bits |= Bit(Block::summon_limit);
        return bits;
    }

    /// The due blocks' names, comma-separated ("none" for no bit), for the log.
    [[nodiscard]] inline std::string DueNames(DueBits bits)
    {
        std::string out;
        for (std::size_t i = 0; i < kBlockCount; ++i) {
            if (!IsDue(bits, static_cast<Block>(i))) continue;
            if (!out.empty()) out += ',';
            out += kBlockNames[i];
        }
        return out.empty() ? std::string("none") : out;
    }
}
