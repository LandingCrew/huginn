#pragma once

// =============================================================================
// FILTER REASON (0.23.46) -- why CandidateFilters dropped a gathered candidate
// =============================================================================
// The old engine's hard filters (candidate/CandidateFilters) drop a gathered
// candidate before scoring: unaffordable, already equipped, a full vital, its
// buff already active, on cooldown, a duplicate, past the size limit. The
// selection log v3 marks such an item `held` without `eligible`, and until
// 0.23.46 nothing said which filter it was ("buff already active" looked the
// same as unaffordable or equipped). Doc 11 (section 2.3, step 3) asks for the
// reason on the row, `filtered_by`, so the shadow fit can tell the active-buff
// filters' drops (a covered item) from the rest.
//
// LOGGING ONLY: the filters decide exactly as before; this names the branch
// that decided. The names are written into every v3 head ("filters"), and a
// row's `fb` column indexes that list.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Huginn::Core::Filter
{
    /// The filter that dropped a candidate, in the filters' own order.
    enum class Reason : std::uint8_t
    {
        affordability,    // a spell over the magicka on hand (uncastable policy Disallow), or a count of 0
        equipped,         // already in a hand / nocked / worn
        full_health,      // a heal on full health
        full_magicka,     // a magicka restore on full magicka
        full_stamina,     // a stamina restore on full stamina
        // --- the active-buff filters (doc 11's "covered" items) ---
        invisibility,     // invisibility already on
        muffle,           // muffle already on
        armor,            // an armour spell while any armour-rating buff is up
        summon_limit,     // a summon spell with the player's summon limit reached
        warming,          // a warming aura already running
        waterbreathing,   // water breathing already on
        resist,           // a resist whose element the player already resists past the threshold
        // --- after the visitor ---
        cooldown,         // used a moment ago
        duplicate,        // a second candidate with the same key (the first one stays eligible)
        truncated,        // past maxCandidatesAfterFilter
        _Count
    };
    inline constexpr std::size_t kReasonCount = static_cast<std::size_t>(Reason::_Count);

    inline constexpr std::array<std::string_view, kReasonCount> kReasonNames{
        "affordability", "equipped",     "full_health", "full_magicka",   "full_stamina",
        "invisibility",  "muffle",       "armor",       "summon_limit",   "warming",
        "waterbreathing", "resist",      "cooldown",    "duplicate",      "truncated",
    };

    [[nodiscard]] constexpr std::string_view ReasonName(Reason r) noexcept
    {
        const auto i = static_cast<std::size_t>(r);
        return i < kReasonCount ? kReasonNames[i] : std::string_view("?");
    }

    /// One of the filters that drop an item whose effect is already running on
    /// the player (CandidateFilters' "active buff" stage, resists included).
    [[nodiscard]] constexpr bool IsActiveBuff(Reason r) noexcept
    {
        return r >= Reason::invisibility && r <= Reason::resist;
    }

    [[nodiscard]] constexpr bool IsFullVital(Reason r) noexcept
    {
        return r >= Reason::full_health && r <= Reason::full_stamina;
    }

    /// One gathered candidate the filters dropped, and why.
    struct Dropped
    {
        std::uint32_t form = 0;
        std::uint16_t uid = 0;   // the stack's unique ID (weapons, armour), else 0
        Reason reason = Reason::affordability;
    };

    /// The reason the filters gave for (form, uid): an exact match first, then
    /// a match on the form where either side has no unique ID (a held row read
    /// without extra data, a candidate of a plain stack). nullopt: the item was
    /// not dropped by a filter in that run (it was eligible, or never gathered).
    [[nodiscard]] inline std::optional<Reason> Find(const std::vector<Dropped>& dropped, std::uint32_t form,
                                                    std::uint16_t uid) noexcept
    {
        const Dropped* loose = nullptr;
        for (const auto& d : dropped) {
            if (d.form != form) continue;
            if (d.uid == uid) return d.reason;
            if (!loose && (d.uid == 0 || uid == 0)) loose = &d;
        }
        if (loose) return loose->reason;
        return std::nullopt;
    }
}
