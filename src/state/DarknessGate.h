#pragma once

#include "BoolDebouncer.h"
#include "StateConstants.h"

#include <cstdint>
#include <optional>

namespace Huginn::State
{
   // =============================================================================
   // DARKNESS GATE
   // =============================================================================
   // Turns the quantized light reading into WorldState::isDark: a hysteresis
   // band (enter below `enter`, leave at enter + DARK_EXIT_GAP) and a dwell
   // both ways (BoolDebouncer).
   //
   // The enter delay is the new half. At noon in a LoreRim town the reading
   // swung 27 <-> 178 as the player passed under overhangs and through gates,
   // and darkness went on and off five times in 90 s, each dip lasting 0.6 to
   // 4 s (2026-09-30). A shadow is not the dark.
   //
   // A change of PLACE skips both delays: a door or a load screen is a real
   // change, and walking into a cave should surface Night Eye at once, not
   // five seconds in. A place is an interior cell, or an exterior WORLDSPACE --
   // not an exterior cell. Walking through a town crosses the exterior grid
   // every few seconds, and keying on the cell let three noon shadows of
   // 0.7-1.4 s through as dark (2026-09-30, first build of this gate).
   //
   // Poll thread only (PollWorldObjects), like the debouncers it wraps.
   // =============================================================================
   class DarknessGate
   {
   public:
      // `light` is the quantized 0..1 level, `enter` the dark threshold
      // ([ContextWeights] fDarkLightLevel), `placeID` the interior cell or the
      // exterior worldspace (see above).
      bool Update(float light, float enter, std::uint32_t placeID,
                  BoolDebouncer::Clock::time_point now,
                  std::optional<BoolDebouncer::Suppressed>* suppressed = nullptr) noexcept
      {
         const float threshold = m_debounce.Value() ? enter + LightLevel::DARK_EXIT_GAP : enter;
         const bool raw = light < threshold;
         if (placeID != m_placeID) {
            m_placeID = placeID;
            m_debounce.Reset(raw);
            return raw;
         }
         return m_debounce.Update(raw, now, LightLevel::DARK_ENTER_DELAY,
                                  LightLevel::DARK_EXIT_HOLD, suppressed);
      }

      // Save load: the next reading is taken at once, as for a new place.
      void Reset() noexcept
      {
         m_debounce.Reset();
         m_placeID = 0;
      }

   private:
      BoolDebouncer m_debounce;
      std::uint32_t m_placeID = 0;
   };
}
