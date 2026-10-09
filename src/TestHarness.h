#pragma once

// =============================================================================
// TEST HARNESS -- counts the in-game Debug suites and drives an unattended run
// =============================================================================
// The suites in Tests.cpp report a failure by logging at error level (there is
// no shared counter; each one logs "TEST FAIL ..." or "<name> FAIL ..." and
// usually returns). RunSuite runs one suite with a counting sink armed on the
// calling thread: a suite FAILED if it logged at error or above, or threw;
// SKIPPED if it called MarkSkipped (a registry not ready, a setting off, too
// few potions in the save) and logged no error; else PASSED. Lines other
// threads log meanwhile are not counted.
//
// RunSuite catches exceptions in every Debug session, not only in test mode: a
// suite that throws is logged FAILED and the next suite runs. Before 0.23.9 an
// exception from a suite went up into the SKSE message handler.
//
// After each batch, EndPhase logs one sentinel line:
//   [HuginnTest] RESULT phase=menu suites=1 passed=1 failed=0 skipped=0 fail_lines=0 failed_suites=- skipped_suites=-
//
// TEST MODE (unattended run, tools/ingame/run_tests.py). Off unless asked for:
//   - environment: HUGINN_TEST_MODE=1, optional HUGINN_TEST_SAVE=<save name>
//   - or a one-shot file Huginn_TestMode.ini in the SKSE log folder:
//       [Test]
//       bEnabled=1
//       sSaveName=<save name, no .ess>     ; optional
//       iExpiresUnix=<unix seconds>        ; optional; ignored once past
//       iLoadTimeoutSec=300                ; optional
//       iCaptureSlotsSec=<seconds>         ; optional (env HUGINN_CAPTURE_SLOTS):
//                                          ; after the load suites, record slot
//                                          ; snapshots that long (SlotSnapshot.h)
//     Read once at plugin load and deleted, so it applies to one launch only.
// In test mode, after the main-menu suites Huginn loads the named save (so the
// after-load suites run), then logs
//   [HuginnTest] DONE result=PASS|FAIL suites=.. passed=.. failed=.. skipped=.. fail_lines=.. reason=..
// and ends the process (exit code 0 on PASS, 1 on FAIL). With no save named it
// ends after the main-menu suites.
//
// Everything here is Debug-only: in Release the suites do not exist, and these
// functions do nothing (test mode is never read).
// =============================================================================

#include <memory>
#include <string_view>

namespace spdlog::sinks
{
    class sink;
}

namespace Huginn::TestHarness
{
    enum class Phase
    {
        Menu,  // RunUnitTests at kDataLoaded
        Load   // the suites after a save loads (InitializeGameSystems)
    };

    /// The counting sink for the global logger (OpenLog). nullptr in Release.
    [[nodiscard]] std::shared_ptr<spdlog::sinks::sink> MakeCountingSink();

    /// Read the test-mode flag (environment, then the one-shot file). Call once
    /// after the logger exists.
    void ReadTestMode();

    /// Test mode is on for this launch.
    [[nodiscard]] bool Active() noexcept;

    /// kPostLoadGame arrived (call first thing): stands the load watchdog down.
    void OnGameLoaded() noexcept;

    /// The running suite did not (fully) run its checks: call at every early
    /// return or skipped block, next to the existing log line. The first
    /// reason is kept. No-op outside RunSuite, and in Release.
    void MarkSkipped(std::string_view reason);

    /// Run one suite and tally it.
    void RunSuite(const char* name, void (*suite)());

    /// Log the phase's RESULT line; in test mode, go on (load the save) or end.
    /// `gameLoaded` is kPostLoadGame's success flag (Phase::Load only).
    void EndPhase(Phase phase, bool gameLoaded = true);
}

#define HUGINN_RUN_SUITE(fn) ::Huginn::TestHarness::RunSuite(#fn, &fn)
