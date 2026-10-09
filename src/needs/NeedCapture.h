#pragma once

// =============================================================================
// NEED CAPTURE (R3, Debug test mode only) -- recorded need snapshots
// =============================================================================
// The replay test (tests/core/NeedFixtureTests.cpp) checks recorded game
// states against the oracle's vectors. In test mode with iCaptureNeeds = N
// (TestHarness.h; run_tests.py --capture-needs N), every need snapshot the
// need monitor logs (a new signature, at most one a second) -- and the ones
// the Debug suite takes -- is appended to <SKSE log folder>/Huginn_NeedSnapshots.txt, up to N.
// The file is started fresh by the first record of a launch. Release: inert.
// =============================================================================

#include "core/NeedSnapshot.h"

#include <string_view>

namespace Huginn::Needs::Capture
{
   /// Append `s` under `tag` (a label is made from the tag and a counter).
   /// No-op unless a Debug build is in test mode with iCaptureNeeds > 0, and
   /// once the limit is reached.
   void Record(const Core::Needs::NeedSnapshot& s, std::string_view tag);
}
