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
//
// R8.3: the bootstrap's curves (Huginn_Bootstrap.json `curves`) override the
// [Needs] line for the needs they name, because theta was fitted with them
// (R6: encumbrance = logistic 0.988677 73.4157). Set once when the bootstrap
// is read (logged once), kept across hot reloads; `hg needs` marks where
// each curve came from (CurveSource).
// =============================================================================

#include "core/Bootstrap.h"
#include "core/NeedEvaluator.h"

#include <SimpleIni.h>
#include <array>
#include <mutex>
#include <vector>

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

      /// R8.3: the bootstrap's curves, applied over the INI from now on.
      void SetBootstrapCurves(const std::vector<Core::Bootstrap::BuildCurve>& curves);

      /// A copy (93 curves): what the pipeline evaluates with this tick (the
      /// INI's, with the bootstrap's on top).
      [[nodiscard]] Core::Needs::CurveTable GetCurves() const;

      enum class Source : uint8_t { Default, Ini, Bootstrap };
      [[nodiscard]] Source CurveSource(std::size_t need) const;
      [[nodiscard]] static const char* SourceName(Source s) noexcept;

   private:
      NeedSettings() : m_curves(Core::Needs::DefaultCurves()) { m_source.fill(Source::Default); }

      mutable std::mutex m_mutex;
      Core::Needs::CurveTable m_curves;        // the INI's (defaults where it says nothing)
      std::array<Source, Core::Needs::kNeedCount> m_source{};
      std::vector<std::pair<std::size_t, Core::Needs::Curve>> m_bootstrap;   // need, curve
   };
}
