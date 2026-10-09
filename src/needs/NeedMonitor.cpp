#include "NeedMonitor.h"

#include "NeedCapture.h"
#include "Profiling.h"

namespace Huginn::Needs
{
   void NeedMonitor::Tick(std::chrono::steady_clock::time_point now)
   {
      Huginn_ZONE_NAMED("NeedMonitor::Tick");
      auto live = ReadLiveNeeds();
      const auto sig = Core::Needs::Signature(live.vector.value);

      bool log = false;
      {
         std::lock_guard lock(m_mutex);
         if ((!m_lastLogged || Core::Needs::SignatureMoved(*m_lastLogged, sig, kLogDeadbandLevels)) &&
             now - m_lastLogAt >= kLogInterval) {
            m_lastLogged = sig;
            m_lastLogAt = now;
            log = true;
         }
         m_latest = live;
      }
      if (log) {
         logger::debug("[Needs] {}"sv, NeedsLine(live.vector));
         Capture::Record(live.snapshot, "monitor");
      }
   }

   std::optional<LiveNeeds> NeedMonitor::Latest() const
   {
      std::lock_guard lock(m_mutex);
      return m_latest;
   }
}
