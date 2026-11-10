#pragma once

// =============================================================================
// LOCK REACH (0.24.29) -- can this unlock item open the lock in the crosshair?
// =============================================================================
// The lock in the crosshair has a level the activate prompt shows (Novice ..
// Master, or "Requires Key"), so the player knows it (needs.csv,
// lock_in_crosshair). An unlock spell or scroll opens locks up to a level of
// its own. Until 0.24.29 nothing compared the two: the catalog gave every
// Open Lock spell the same utility_unlock value (LoreRim's are scripted, their
// magnitude 0), and lock_in_crosshair is a 0/1 need, so Open Novice Lock was
// offered at an Expert chest (the user, 2026-11-09).
//
// The item's reach is read once, when the catalog maps it, from the text of
// its kept unlock rows: the effect's name and description. Requiem / LoreRim
// say "Open Apprentice Lock" and "opening locks up to apprentice level"; the
// Gray Cowl's Unlock Container "(novice and apprentice)"; Fenrik's Welcome
// "locks with level Expert and below". The highest level word named is the
// reach. A row that names no level (Detonate Lock: "a level 25 points or more
// below your Alteration skill") has an unknown reach, and an item with any
// unknown unlock row is never dropped for its level.
//
// The filter (CandidateFilters, Core::Filter::Reason::lock_out_of_reach):
// with a lock in the crosshair, an unlock item whose reach is below its level
// is dropped; a lock that requires a key drops every unlock item (no spell
// opens it). With no lock in the crosshair nothing is dropped: the score
// decides (lock_in_crosshair x utility_unlock is 0 then).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace Huginn::Core::Lock
{
    /// The lock levels, as state/StateConstants.h LockLevel numbers them
    /// (the game's lock levels kVeryEasy .. kVeryHard, then kRequiresKey).
    inline constexpr int kNovice = 0;
    inline constexpr int kApprentice = 1;
    inline constexpr int kAdept = 2;
    inline constexpr int kExpert = 3;
    inline constexpr int kMaster = 4;
    inline constexpr int kRequiresKey = 5;

    /// No lock in the crosshair (Gate::Inputs::lockLevel).
    inline constexpr int kNoLock = -1;
    /// An unlock item whose reach no text names; also "not an unlock item".
    inline constexpr int kUnknownReach = -1;

    /// The highest lock level a text names ("novice" .. "master", any case,
    /// as a whole word), or kUnknownReach for none.
    [[nodiscard]] inline int ReachFromText(std::string_view text)
    {
        static constexpr std::array<std::string_view, 5> kWords{ "novice", "apprentice", "adept", "expert", "master" };
        std::string low(text);
        std::ranges::transform(low, low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto letter = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
        int best = kUnknownReach;
        for (int level = 0; level < static_cast<int>(kWords.size()); ++level) {
            const auto w = kWords[static_cast<std::size_t>(level)];
            for (auto at = low.find(w); at != std::string::npos; at = low.find(w, at + 1)) {
                const bool startOk = at == 0 || !letter(low[at - 1]);
                const bool endOk = at + w.size() >= low.size() || !letter(low[at + w.size()]);
                if (startOk && endOk) {
                    best = std::max(best, level);
                    break;
                }
            }
        }
        return best;
    }

    /// One unlock row's reach: its description first (it says what the spell
    /// opens), then its name.
    [[nodiscard]] inline int RowReach(std::string_view name, std::string_view description)
    {
        const int d = ReachFromText(description);
        return d != kUnknownReach ? d : ReachFromText(name);
    }

    /// Folds one unlock row's reach into an item's (start from `first = true`):
    /// the highest of its rows, and unknown as soon as one row is unknown.
    struct ItemReach
    {
        int reach = kUnknownReach;
        bool any = false;      // the item keeps an unlock row
        bool unknown = false;  // one of them names no level

        void Add(int rowReach) noexcept
        {
            any = true;
            if (rowReach == kUnknownReach) unknown = true;
            else reach = std::max(reach, rowReach);
        }
        [[nodiscard]] int Value() const noexcept { return (!any || unknown) ? kUnknownReach : reach; }
    };

    /// lock_out_of_reach: an unlock item that cannot open the lock in the
    /// crosshair. `lockLevel` is kNoLock without one.
    [[nodiscard]] constexpr bool OutOfReach(bool unlocks, int reach, int lockLevel) noexcept
    {
        if (!unlocks || lockLevel == kNoLock) return false;
        if (lockLevel >= kRequiresKey) return true;
        return reach != kUnknownReach && reach < lockLevel;
    }
}
