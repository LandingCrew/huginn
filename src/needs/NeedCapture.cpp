#include "NeedCapture.h"

#ifndef NDEBUG

#include "TestHarness.h"
#include "core/NeedSnapshotIO.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace Huginn::Needs::Capture
{
   namespace
   {
      std::mutex g_mutex;
      int g_written = 0;
      bool g_started = false;
   }

   void Record(const Core::Needs::NeedSnapshot& s, std::string_view tag)
   {
      if (!TestHarness::Active()) return;
      const int limit = TestHarness::NeedCaptureLimit();
      if (limit <= 0) return;
      std::lock_guard lock(g_mutex);
      if (g_written >= limit) return;
      const auto dir = SKSE::log::log_directory();
      if (!dir) return;
      const auto path = *dir / "Huginn_NeedSnapshots.txt";
      std::ofstream out(path, g_started ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc));
      if (!out) return;
      if (!g_started) {
         out << "# Huginn need snapshots (core/NeedSnapshotIO.h), recorded in test mode\n";
         g_started = true;
      }
      ++g_written;
      out << Core::Needs::WriteSnapshot(fmt::format("{}_{:03}", tag, g_written), s);
      if (g_written == limit) {
         logger::info("[NeedCapture] {} snapshot(s) written to {}"sv, g_written, path.string());
      }
   }
}

#else

namespace Huginn::Needs::Capture
{
   void Record(const Core::Needs::NeedSnapshot&, std::string_view) {}
}

#endif
