#pragma once

// =============================================================================
// NEED SNAPSHOT IO -- the text record of a NeedSnapshot
// =============================================================================
// Written by the Debug capture (needs/NeedCapture.cpp, to
// <SKSE log folder>/Huginn_NeedSnapshots.txt) and read by the host tests
// (tests/core/fixtures/needs/*.txt) and by tools/needs/expected_vectors.py.
//
//   # anything after a '#' at the start of a line is a comment
//   snapshot <label>
//   health 0.42
//   inCombat 1
//   families 3
//   ...
//   end
//
// One `name value` pair per line, names from kFields (NeedSnapshot.h). A field
// left out keeps the NeedSnapshot default; an unknown name, a bad value or a
// missing `end` is an error. Floats are written in the shortest text that
// reads back to the same float, bools as 0/1.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "NeedSnapshot.h"

#include <string>
#include <string_view>
#include <vector>

namespace Huginn::Core::Needs
{
    struct LabeledSnapshot
    {
        std::string label;
        NeedSnapshot snapshot;
    };

    /// One `snapshot ... end` block, every field written.
    [[nodiscard]] std::string WriteSnapshot(std::string_view label, const NeedSnapshot& s);

    /// Every block in `text`. On an error returns what parsed before it and
    /// sets `error` (line number and reason); `error` is left empty on success.
    [[nodiscard]] std::vector<LabeledSnapshot> ReadSnapshots(std::string_view text, std::string& error);

    /// The fields where a and b differ (exact compare), as "name a b" lines.
    [[nodiscard]] std::string DiffSnapshots(const NeedSnapshot& a, const NeedSnapshot& b);
}
