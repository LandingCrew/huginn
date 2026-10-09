#include "NeedSettings.h"

namespace Huginn::Needs
{
   void NeedSettings::LoadFromIni(const CSimpleIniA& ini)
   {
      auto curves = Core::Needs::DefaultCurves();
      int overridden = 0;
      int rejected = 0;
      for (std::size_t i = 0; i < Core::Needs::kNeedCount; ++i) {
         const auto& info = Core::Needs::kNeeds[i];
         const std::string key(info.id);
         const char* raw = ini.GetValue("Needs", key.c_str(), nullptr);
         if (!raw) continue;
         std::string_view text(raw);
         if (const auto semi = text.find(';'); semi != std::string_view::npos) {
            text = text.substr(0, semi);  // an inline comment
         }
         if (const auto c = Core::Needs::ParseCurve(text)) {
            if (*c != info.curve) ++overridden;
            curves[i] = *c;
         } else {
            ++rejected;
            logger::warn("[NeedSettings] [Needs] {} = '{}' is not a usable curve; keeping {}"sv, info.id, raw,
               Core::Needs::FormatCurve(info.curve));
         }
      }
      {
         std::lock_guard lock(m_mutex);
         m_curves = curves;
      }
      logger::info("[NeedSettings] Loaded: {} curve(s) differ from the defaults, {} rejected"sv, overridden, rejected);
   }

   void NeedSettings::ResetToDefaults()
   {
      std::lock_guard lock(m_mutex);
      m_curves = Core::Needs::DefaultCurves();
   }

   Core::Needs::CurveTable NeedSettings::GetCurves() const
   {
      std::lock_guard lock(m_mutex);
      return m_curves;
   }
}
