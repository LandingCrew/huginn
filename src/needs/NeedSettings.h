#pragma once

// =============================================================================
// NEED SETTINGS (R3) -- the [Needs] INI section: one response curve per need
// =============================================================================
//   [Needs]
//   health_deficit = logistic 0.5 10
//   ...
// Key = the need's id (needs.csv), value = "<kind> <p1> <p2>"
// (core/ResponseCurve.h ParseCurve). A key left out keeps the csv default
// (core/NeedIds.h); a value that does not parse, or is not a usable curve,
// is logged and the default kept. Loaded at kDataLoaded and on every
// hot reload (SettingsReloader, `hg reload`, dMenu).
// =============================================================================

#include "core/NeedEvaluator.h"

#include <SimpleIni.h>
#include <mutex>

namespace Huginn::Needs
{
   class NeedSettings
   {
   public:
      static NeedSettings& GetSingleton()
      {
         static NeedSettings instance;
         return instance;
      }

      void LoadFromIni(const CSimpleIniA& ini);
      void ResetToDefaults();

      /// A copy (93 curves): what the pipeline evaluates with this tick.
      [[nodiscard]] Core::Needs::CurveTable GetCurves() const;

   private:
      NeedSettings() : m_curves(Core::Needs::DefaultCurves()) {}

      mutable std::mutex m_mutex;
      Core::Needs::CurveTable m_curves;
   };
}
