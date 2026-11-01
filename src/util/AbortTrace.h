#pragma once

namespace Huginn::Util::AbortTrace
{
   // =========================================================================
   // ABORT TRACE -- the stack of a failed assert, in the log (0.24.26, debug)
   // =========================================================================
   // A Debug build's failed assert (ours, or ImGui's IM_ASSERT, which is the
   // CRT's assert) shows the "Assertion failed!" box; Abort raises SIGABRT and
   // the process ends. Crash Logger does not see it (no exception), and the
   // box names only the header the check is in (imgui.h:2101, ImVector's
   // bounds check), not who called it.
   //
   // Install() sets a SIGABRT handler on the debug CRT Huginn links. On the
   // first abort it logs, at critical, flushed after each step:
   //   1. the thread (and whether it is the game's main thread) and up to 48
   //      frames as module+offset -- written before anything can fail;
   //   2. one line per frame, with function, file and line for the frames in
   //      Huginn.dll (DbgHelp over Huginn's own PDB; the other modules stay
   //      module+offset).
   // Then the abort goes on as before. A second abort while the first is
   // being written is not traced.
   //
   // Click Abort in the box (not Ignore) to get the trace. Debug builds only:
   // a release build compiles the asserts out.
   // =========================================================================

   void Install();
}
