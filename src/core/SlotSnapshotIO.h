#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/SlotAllocCore.h"

// =============================================================================
// SLOT SNAPSHOTS: one allocation's input and the page it produced, as text
// =============================================================================
// The golden test's record (R7). The game writes one per allocation while a
// Debug build captures (SlotSnapshot.h: test mode with iCaptureSlotsSec, or
// `hg slots capture`); tests/core/fixtures/slots/ holds files of them, and
// SlotAllocGoldenTests.cpp replays each input through the core and checks the
// page against the recorded one.
//
// Line-oriented, space-separated, one block per snapshot, with an item
// dictionary between blocks (a candidate's fixed fields, written the first
// time the file needs them; ids run 0, 1, 2, ...):
//
//   ITEM <id> <formID> <uniqueID> <key> <tieBreak> <matchMask> <capClass> =<name>
//
//   SNAP <tag> page=<p> now=<ns> mem=<0|1> gen=<0|1> ovr=<0|1>
//   SET <keep> <hold> <margin> <fillJob> <discount> <free> <homeSec> <returnHome> <toJob>
//   SLOT <class> <regular> <wildcards> <skipEquipped> <remembrance> <priority> <overrideAccept>
//   C    <item id> <utility> <score> <flags>
//   CAND <formID> <uniqueID> <key> <utility> <score> <tieBreak> <flags> <matchMask> <capClass> =<name>
//   OVR  <formID> <uniqueID> <key> <condition> <category> <pinned> <equipped> <playerEquipped> <matchMask> <capClass> =<name>
//   HOLD <slot> <formID>                       (active Remembrance holds)
//   MEM <seats x10> | <lastPlaced x10> | <claims x10>
//   DEP <slot> <key> <leftAtNs> <key> <leftAtNs> <key> <leftAtNs>   (non-empty rows)
//   OUT gen=<0|1> cleared=<0|1>                (the recorded result, optional)
//   A <kind> <formID> <uniqueID> <key> <seatMoved> =<name>          (one per slot)
//   OMEM ... / ODEP ...                        (memory after, as MEM/DEP)
//   KEPT <formID>...                           (what the class cap kept off)
//   END
//
// formIDs and keys are hex; floats and doubles in the shortest form that reads
// back to the same bits (std::format "{}"), inf as "inf"; names percent-
// escaped (space, %, =, control and non-ASCII bytes), behind '=' so an empty
// name is still a token. C/CAND flags: 1 wildcard, 2 remembered-only, 4
// equipped, 8 equipped per the player state. A score of "~" is the bridge of
// the utility to the last bit (BridgeScore, or -inf for a remembered-only
// row); the writer uses it only when that holds.
// =============================================================================

namespace Huginn::Core::SlotAlloc
{
    /// One slot of a page as the player would see it: what is on it.
    struct PageSlot
    {
        Kind kind = Kind::Empty;
        std::uint32_t formID = 0;
        std::uint16_t uniqueID = 0;
        std::uint64_t key = 0;
        bool seatMoved = false;
        std::string name;
        friend bool operator==(const PageSlot&, const PageSlot&) = default;
    };

    struct Snapshot
    {
        std::string tag;                 // where it came from (tick, page, campaign, synthetic)
        Input in;

        bool hasResult = false;          // a recorded result follows
        std::vector<PageSlot> page;
        PageMemory memoryAfter;
        bool generationAfter = false;
        bool clearedAllPages = false;
        std::vector<std::uint32_t> keptOff;   // sorted formIDs
    };

    /// The page a core output describes.
    [[nodiscard]] std::vector<PageSlot> PageOf(const Input& in, const Output& out);

    /// Fill `snap`'s result fields from a core output.
    void SetResult(Snapshot& snap, const Output& out);

    /// A file's item dictionary (ITEM lines): one per writer of one file.
    struct SnapshotDictionary
    {
        std::map<std::string, std::uint32_t> items;
        std::uint32_t next = 0;
    };

    /// One snapshot as text. With `dict`, candidates are written as C lines
    /// against the file's dictionary, and the ITEM lines this snapshot adds
    /// come first (before its SNAP line); without, as self-contained CAND lines.
    [[nodiscard]] std::string WriteSnapshot(const Snapshot& snap, SnapshotDictionary* dict = nullptr);

    /// Every snapshot in `text`, in order. False (and `error` says where) on
    /// the first malformed line; the snapshots read before it are kept.
    [[nodiscard]] bool ReadSnapshots(std::string_view text, std::vector<Snapshot>& out, std::string* error = nullptr);

    [[nodiscard]] std::string EscapeName(std::string_view name);
    [[nodiscard]] bool UnescapeName(std::string_view token, std::string& out);
}  // namespace Huginn::Core::SlotAlloc
