#pragma once

// =============================================================================
// SLICED WALK -- a walk over several sequences, in order, that can stop
// between any two elements and resume where it stopped
// =============================================================================
// The effect catalog reads the whole load order on the main thread at the main
// menu: 3.1-3.25 s in one call on LoreRim (Tracy, 2026-10-16/17). Since
// 0.23.47 it reads in slices of a few milliseconds, one per frame, and a slice
// must visit exactly the elements an unsliced read would, in the same order,
// so the records -- and so the catalog -- are the same. This is that cursor.
//
// WalkSlice visits (sequence, index) pairs from `cursor` onwards: sequence 0's
// elements, then sequence 1's, and so on. After each visit it asks `stop()`;
// when that says yes it returns with the cursor on the next element. A
// sequence's size is read again before each element, not once per slice, so a
// sequence that grows between slices (or during one) is walked to its end at
// the time the walk reaches it, as an unsliced walk would at that moment.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstddef>

namespace Huginn::Core
{
    struct SliceCursor
    {
        std::size_t sequence = 0;
        std::size_t index = 0;
    };

    /// Visit from `cursor` until `stop()` (asked after each visit) says yes, or
    /// every sequence is done. `sizeOf(s)` is sequence s's size now;
    /// `visit(s, i)` reads its element i. True when the walk is done (the
    /// cursor past the last sequence), false when stopped early. A call on a
    /// done cursor visits nothing and returns true.
    template <class SizeOf, class Visit, class Stop>
    bool WalkSlice(SliceCursor& cursor, std::size_t sequences, SizeOf&& sizeOf, Visit&& visit, Stop&& stop)
    {
        while (cursor.sequence < sequences) {
            while (cursor.index < sizeOf(cursor.sequence)) {
                const std::size_t at = cursor.index++;
                visit(cursor.sequence, at);
                if (stop()) return false;
            }
            ++cursor.sequence;
            cursor.index = 0;
        }
        return true;
    }
}
