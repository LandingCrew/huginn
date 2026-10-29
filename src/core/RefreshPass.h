#pragma once

// =============================================================================
// REFRESH PASS (R8.1, 0.23.55) -- a buff is offered again before it runs out
// =============================================================================
// Doc 11 decision 4. The active-buff filters (CandidateFilters: invisibility,
// muffle, armor, summon_limit, warming, waterbreathing, resist) drop an item
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
//   - resist: the element's resistance stays over the threshold until enough
//     of its timed parts have ended (ResistTimeLeft); the part no effect
//     explains (abilities, gear, a racial resistance) is taken as permanent;
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
        resist_fire,
        resist_frost,
        resist_shock,
        resist_poison,
        resist_magic,
        _Count
    };
    inline constexpr std::size_t kBlockCount = static_cast<std::size_t>(Block::_Count);
    using DueBits = std::uint16_t;
    static_assert(kBlockCount <= sizeof(DueBits) * 8);

    /// For the log: the filter (Core::Filter::Reason name) each block belongs to.
    inline constexpr std::array<std::string_view, kBlockCount> kBlockNames{
        "invisibility", "muffle", "armor", "summon_limit", "warming", "waterbreathing",
        "resist_fire", "resist_frost", "resist_shock", "resist_poison", "resist_magic",
    };

    /// The resistances the resist filter reads, in Block order from resist_fire.
    enum class Element : std::uint8_t { fire, frost, shock, poison, magic, _Count };
    inline constexpr std::size_t kElementCount = static_cast<std::size_t>(Element::_Count);

    [[nodiscard]] constexpr Block ResistBlock(Element e) noexcept
    {
        return static_cast<Block>(static_cast<std::uint8_t>(Block::resist_fire) + static_cast<std::uint8_t>(e));
    }

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

    /// One timed effect on a resistance: what it adds (negative for a
    /// weakness) and its remaining seconds.
    struct ResistPart
    {
        float magnitude = 0.0f;
        float remaining = 0.0f;
    };

    /// The timed parts of one element's resistance. A constant part is not
    /// kept: whatever no timed part explains counts as permanent. Past kMax
    /// parts the extra one is not kept either, so it too counts as permanent:
    /// the pass can only come later than it should, never early.
    struct ResistParts
    {
        static constexpr std::size_t kMax = 8;
        std::array<ResistPart, kMax> parts{};
        std::size_t n = 0;

        constexpr void Add(float magnitude, float remainingSec) noexcept
        {
            if (remainingSec < 0.0f || n >= kMax) return;
            parts[n++] = ResistPart{ magnitude, remainingSec };
        }
    };

    /// Seconds until a resistance of `total` falls to `threshold` or below and
    /// stays there as its timed parts end: 0 when it is not over the
    /// threshold; kNoTimer when it stays over after every timed part ended.
    [[nodiscard]] constexpr float ResistTimeLeft(float total, float threshold, const ResistParts& p) noexcept
    {
        if (!(total > threshold)) return 0.0f;
        std::array<ResistPart, ResistParts::kMax> sorted = p.parts;
        const std::size_t n = std::min(p.n, ResistParts::kMax);
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n),
                  [](const ResistPart& a, const ResistPart& b) { return a.remaining < b.remaining; });
        float after = total;   // the resistance once every part has ended
        for (std::size_t i = 0; i < n; ++i) after -= sorted[i].magnitude;
        if (after > threshold) return kNoTimer;
        // Walk back from the last end: the first part (from the end) whose end
        // takes the value from over the threshold to at or under it for good.
        for (std::size_t k = n; k-- > 0;) {
            const float before = after + sorted[k].magnitude;   // the value just before part k ends
            if (before > threshold) return sorted[k].remaining;
            after = before;
        }
        return kNoTimer;   // unreachable: before part 0 ends the value is `total`, over the threshold
    }

    /// What the poll reads to decide which blocks are due.
    struct Inputs
    {
        Timer invisibility, muffle, armor, warming, waterbreathing;
        std::array<ResistParts, kElementCount> resist{};
        std::array<float, kElementCount> resistTotal{};   // the player's resistance per element
        float resistThreshold = 50.0f;                    // the resist filter's threshold
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
        for (std::size_t e = 0; e < kElementCount; ++e) {
            const float total = in.resistTotal[e];
            if (!(total > in.resistThreshold)) continue;   // not blocking
            if (RefreshDue(ResistTimeLeft(total, in.resistThreshold, in.resist[e]), refreshSec)) {
                bits |= Bit(ResistBlock(static_cast<Element>(e)));
            }
        }
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
