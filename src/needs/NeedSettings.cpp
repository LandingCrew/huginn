#include "NeedSettings.h"

namespace Huginn::Needs
{
   void NeedSettings::LoadFromIni(const CSimpleIniA& ini)
   {
      auto curves = Core::Needs::DefaultCurves();
      std::array<Source, Core::Needs::kNeedCount> source{};
      source.fill(Source::Default);
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
            curves[i] = *c;
            // The INI is read over the shipped Huginn.ini (Settings::LoadMainIni),
            // so every need has a line here whether the player wrote one or not.
            // "[Needs]" therefore means a line that CHANGES the csv curve (the
            // shipped encumbrance line, or the player's own); one that repeats
            // it reports "default", which is what the curve is.
            if (*c != info.curve) {
               ++overridden;
               source[i] = Source::Ini;
            }
         } else {
            ++rejected;
            logger::warn("[NeedSettings] [Needs] {} = '{}' is not a usable curve; keeping {}"sv, info.id, raw,
               Core::Needs::FormatCurve(info.curve));
         }
      }
      std::size_t fromBootstrap = 0;
      {
         std::lock_guard lock(m_mutex);
         m_curves = curves;
         m_source = source;
         fromBootstrap = m_bootstrap.size();
      }
      logger::info("[NeedSettings] Loaded: {} curve(s) differ from the defaults, {} rejected{}"sv, overridden, rejected,
         fromBootstrap ? fmt::format("; {} need(s) keep the bootstrap's curve over their line", fromBootstrap)
                       : std::string{});
   }

   void NeedSettings::SetBootstrapCurves(const std::vector<Core::Bootstrap::BuildCurve>& curves)
   {
      std::vector<std::pair<std::size_t, Core::Needs::Curve>> list;
      std::string text;
      Core::Needs::CurveTable ini;
      {
         std::lock_guard lock(m_mutex);
         ini = m_curves;
      }
      for (const auto& c : curves) {
         if (c.need >= Core::Needs::kNeedCount) continue;
         const Core::Needs::Curve curve{ Core::Needs::CurveKind::Logistic, static_cast<float>(c.centre),
            static_cast<float>(c.slope) };
         if (!Core::Needs::CurveProblem(curve).empty()) continue;
         list.emplace_back(c.need, curve);
         text += fmt::format("{}{} = {} (was {})", text.empty() ? "" : "; ", Core::Needs::kNeeds[c.need].id,
            Core::Needs::FormatCurve(curve), Core::Needs::FormatCurve(ini[c.need]));
      }
      {
         std::lock_guard lock(m_mutex);
         m_bootstrap = list;
      }
      if (!list.empty()) {
         logger::info("[NeedSettings] The bootstrap's curves replace the [Needs] line for {} need(s) (theta was fitted "
                      "with them): {}"sv, list.size(), text);
      }
   }

   Core::Needs::CurveTable NeedSettings::GetCurves() const
   {
      std::lock_guard lock(m_mutex);
      auto out = m_curves;
      for (const auto& [need, curve] : m_bootstrap) out[need] = curve;
      return out;
   }

   NeedSettings::Source NeedSettings::CurveSource(std::size_t need) const
   {
      std::lock_guard lock(m_mutex);
      if (need >= Core::Needs::kNeedCount) return Source::Default;
      for (const auto& b : m_bootstrap) {
         if (b.first == need) return Source::Bootstrap;
      }
      return m_source[need];
   }

   const char* NeedSettings::SourceName(Source s) noexcept
   {
      switch (s) {
      case Source::Default: return "default";
      case Source::Ini: return "[Needs]";
      case Source::Bootstrap: return "bootstrap";
      }
      return "?";
   }
}
