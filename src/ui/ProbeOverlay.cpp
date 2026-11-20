// Only compile in debug mode
#ifdef _DEBUG

#include "ProbeOverlay.h"
#include "ImGuiCommon.h"
#include "../state/DropAheadProbe.h"

#include <cstdio>

namespace Huginn::UI::ProbeOverlay
{
   namespace
   {
      namespace PD = Core::Needs::ProbeDebug;

      ImU32 ColorOf(PD::Look look)
      {
         switch (look) {
            case PD::Look::Okay: return IM_COL32(80, 220, 80, 255);
            case PD::Look::Fall: return IM_COL32(255, 60, 60, 255);
            case PD::Look::Water: return IM_COL32(60, 150, 255, 255);
            case PD::Look::Blocked: return IM_COL32(240, 200, 40, 255);
            case PD::Look::Skipped: return IM_COL32(150, 150, 150, 255);
         }
         return IM_COL32(255, 255, 255, 255);
      }

      constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 170);
   }

   void Draw()
   {
      if (!State::DropAheadProbe::DebugDrawOn()) {
         return;
      }
      auto* ui = RE::UI::GetSingleton();
      if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
          ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
         return;
      }
      const auto frame = State::DropAheadProbe::LatestDebugFrame();
      if (!frame.measured) {
         return;
      }
      auto* camera = RE::Main::WorldRootCamera();
      if (!camera) {
         return;
      }
      const auto& worldToCam = camera->GetRuntimeData().worldToCam;
      const auto& port = camera->GetRuntimeData2().port;
      const ImVec2 size = ImGui::GetIO().DisplaySize;
      ImDrawList* dl = ImGui::GetForegroundDrawList();

      // World point -> screen pixels; false behind the camera.
      auto project = [&](const Core::Needs::Vec3& p, ImVec2& out) {
         float x = 0.0f, y = 0.0f, z = 0.0f;
         if (!RE::NiCamera::WorldPtToScreenPt3(worldToCam, port, RE::NiPoint3{ p.x, p.y, p.z }, x, y, z, 1e-5f) ||
             z <= 0.0f) {
            return false;
         }
         out = ImVec2(x * size.x, (1.0f - y) * size.y);
         return true;
      };

      for (std::size_t i = 0; i < frame.segmentCount; ++i) {
         const auto& s = frame.segments[i];
         ImVec2 a, b;
         if (!project(s.a, a) || !project(s.b, b)) continue;
         dl->AddLine(a, b, kShadow, 4.0f);
         dl->AddLine(a, b, ColorOf(s.look), 2.0f);
      }

      for (std::size_t i = 0; i < frame.markCount; ++i) {
         const auto& m = frame.marks[i];
         ImVec2 at;
         if (!project(m.at, at)) continue;
         const ImU32 col = ColorOf(m.look);
         if (m.voidBelow) {
            // nothing below to the ray's end: a cross
            dl->AddLine(ImVec2(at.x - 6, at.y - 6), ImVec2(at.x + 6, at.y + 6), col, 2.0f);
            dl->AddLine(ImVec2(at.x - 6, at.y + 6), ImVec2(at.x + 6, at.y - 6), col, 2.0f);
         } else {
            dl->AddCircleFilled(at, 4.0f, col);
         }
         char text[48];
         if (m.farPoint && m.look == PD::Look::Okay) {
            text[0] = '\0';   // a far point that sees nothing: no label, the ray says it
         } else if (m.waterDepth > 0.0f) {
            std::snprintf(text, sizeof(text), "water %.0f", m.waterDepth);
         } else if (m.voidBelow) {
            std::snprintf(text, sizeof(text), "void (%.0f+)", m.drop);
         } else {
            std::snprintf(text, sizeof(text), "drop %.0f", m.drop);
         }
         if (text[0] != '\0') {
            const ImVec2 textAt(at.x + 8.0f, at.y - 7.0f);
            dl->AddText(ImVec2(textAt.x + 1, textAt.y + 1), kShadow, text);
            dl->AddText(textAt, col, text);
         }
      }

      // points the pass never reached (a pick before them was blocked)
      for (std::size_t i = 0; i < frame.unreachedCount; ++i) {
         ImVec2 at;
         if (!project(frame.unreached[i], at)) continue;
         const ImU32 col = ColorOf(PD::Look::Skipped);
         dl->AddLine(ImVec2(at.x - 5, at.y - 5), ImVec2(at.x + 5, at.y + 5), col, 2.0f);
         dl->AddLine(ImVec2(at.x - 5, at.y + 5), ImVec2(at.x + 5, at.y - 5), col, 2.0f);
      }
   }
}

#endif  // _DEBUG
