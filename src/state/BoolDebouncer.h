#pragma once

#include <chrono>
#include <optional>

namespace Huginn::State
{
   // =============================================================================
   // BOOL DEBOUNCER
   // =============================================================================
   // Publishes a flag only once its raw value has held for a while, with
   // separate delays for turning on and turning off.
   //
   // Why here and not in the slots: with the slot hold in (#140), what still
   // swapped slots back and forth was state that flipped and flipped back --
   // combat on for 0.11 s as a humanoid crossed the crosshair, enemy casting
   // toggling every 1.2 s between spells. Every slot change that followed was
   // correct for the score at that moment, so only the state layer can tell a
   // real change from one about to undo itself.
   //
   // Single writer; not thread-safe on its own. Callers publish its value
   // under whatever lock guards the state it feeds.
   // =============================================================================
   class BoolDebouncer
   {
   public:
      using Clock = std::chrono::steady_clock;

      // A raw flip that reverted before it was published.
      struct Suppressed
      {
         bool rawValue;                       // the value that did not stick
         std::chrono::milliseconds lasted;    // how long it held
      };

      // Feed the raw value; returns the published value. `enterDelay` applies
      // to false -> true, `exitHold` to true -> false; zero publishes at once.
      // `suppressed` receives a flip that reverted before its delay ran out,
      // so the caller can log what was filtered.
      bool Update(bool raw, Clock::time_point now,
                  std::chrono::milliseconds enterDelay,
                  std::chrono::milliseconds exitHold,
                  std::optional<Suppressed>* suppressed = nullptr) noexcept
      {
         if (raw == m_value) {
            if (m_pending && suppressed) {
               *suppressed = Suppressed{ !m_value,
                  std::chrono::duration_cast<std::chrono::milliseconds>(now - m_pendingSince) };
            }
            m_pending = false;
            return m_value;
         }
         if (!m_pending) {
            m_pending = true;
            m_pendingSince = now;
         }
         const auto needed = raw ? enterDelay : exitHold;
         if (now - m_pendingSince >= needed) {
            m_value = raw;
            m_pending = false;
         }
         return m_value;
      }

      [[nodiscard]] bool Value() const noexcept { return m_value; }

      void Reset(bool value = false) noexcept
      {
         m_value = value;
         m_pending = false;
      }

   private:
      bool m_value = false;
      bool m_pending = false;
      Clock::time_point m_pendingSince{};
   };
}
