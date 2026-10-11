#pragma once

// What may be written into a Wheeler wheel while the player has it open.
//
// The Wheeler push normally skips while the wheel is open, but an urgent
// override (priority >= iAutoFocusMinPriority) lets it through so the urgent
// item appears. That bypass used to write the WHOLE page -- every re-seat,
// every lock expiry -- live into the open wheel, and a mage low on magicka has
// a Restore Magicka override up most fights: in one LoreRim session (2026-10-11)
// 15 of 45 wheel opens had a Restore Magicka/Stamina/Health override active.
// Entries moving under the cursor are misclicks.
//
// The rule: while the wheel is open, an entry is written only if it is
// `writable` (the caller marks the urgent override's own entry); every other
// entry gets what the wheel already holds, so the diff writes nothing there.
//
// Pure: standard library only, host-tested in tests/core/OpenWheelWritesTests.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Huginn::Core
{
    /// One page's wheel contents in the shape the Wheeler push takes.
    struct WheelPageContents
    {
        std::vector<uint32_t> formIDs;
        std::vector<bool> wildcard;
        std::vector<uint16_t> uniqueIDs;
        std::vector<std::string> subtexts;
    };

    /// Replace every non-writable entry of `incoming` with `cached`, in place,
    /// and return the indices whose content will actually change (what the
    /// push is about to write). An index past the end of `cached` has nothing
    /// to fall back on: it keeps the incoming value and is not reported, since
    /// the wheel has no entry there to write (WheelSync::UpdatePage stops at
    /// its cache size). `writable` shorter than `incoming` means "not
    /// writable" for the rest.
    inline std::vector<size_t> RestrictToWritable(
        WheelPageContents& incoming, const WheelPageContents& cached, const std::vector<bool>& writable)
    {
        std::vector<size_t> written;
        const size_t n = incoming.formIDs.size();
        const auto at = [](const auto& v, size_t i, auto fallback) { return i < v.size() ? v[i] : fallback; };
        for (size_t i = 0; i < n; ++i) {
            const bool canWrite = i < writable.size() && writable[i];
            const bool haveCache = i < cached.formIDs.size();
            if (!canWrite && haveCache) {
                incoming.formIDs[i] = cached.formIDs[i];
                if (i < incoming.wildcard.size()) incoming.wildcard[i] = at(cached.wildcard, i, false);
                if (i < incoming.uniqueIDs.size()) incoming.uniqueIDs[i] = at(cached.uniqueIDs, i, uint16_t{ 0 });
                if (i < incoming.subtexts.size()) incoming.subtexts[i] = at(cached.subtexts, i, std::string{});
                continue;
            }
            if (!haveCache) {
                continue;
            }
            const bool changes =
                incoming.formIDs[i] != cached.formIDs[i] ||
                at(incoming.uniqueIDs, i, uint16_t{ 0 }) != at(cached.uniqueIDs, i, uint16_t{ 0 }) ||
                at(incoming.wildcard, i, false) != at(cached.wildcard, i, false) ||
                at(incoming.subtexts, i, std::string{}) != at(cached.subtexts, i, std::string{});
            if (changes) {
                written.push_back(i);
            }
        }
        return written;
    }
}
