#include "WelcomeBanner.h"
#include "ImGuiRenderer.h"
#include "../Config.h"

#include <imgui.h>

namespace Huginn::UI
{
    WelcomeBanner& WelcomeBanner::GetSingleton()
    {
        static WelcomeBanner instance;
        return instance;
    }

    void WelcomeBanner::Show()
    {
        m_lingerTime = DISPLAY_TIME;
        m_firstFrame = true;
        logger::info("[WelcomeBanner] Showing for {} seconds"sv, DISPLAY_TIME);
    }

    void WelcomeBanner::Draw()
    {
        if (m_lingerTime <= 0.0f) {
            return;
        }

        // Get delta time
        float deltaTime = ImGui::GetIO().DeltaTime;

        // Calculate alpha for fade in/out
        float alpha = 1.0f;
        if (m_lingerTime < FADE_TIME) {
            // Fade out
            alpha = m_lingerTime / FADE_TIME;
        } else if (m_firstFrame) {
            // Fade in on first frame
            alpha = 1.0f - (m_lingerTime - (DISPLAY_TIME - FADE_TIME)) / FADE_TIME;
            if (alpha > 1.0f) alpha = 1.0f;
            if (alpha < 0.0f) alpha = 0.0f;
            m_firstFrame = false;
        }

        // Get screen dimensions
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;

        // Create window at screen center
        ImGui::SetNextWindowPos(
            ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
            ImGuiCond_Always,
            ImVec2(0.5f, 0.5f)  // Anchor at center
        );

        ImGui::SetNextWindowBgAlpha(0.85f * alpha);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 15.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);

        if (ImGui::Begin("##WelcomeBanner", nullptr, flags)) {
            // Title
            ImGui::PushFont(nullptr);  // Use default font for now

            // Version string
#ifndef NDEBUG
            std::string versionText = std::format("{} v{} ({}) [DEBUG]", PROJECT_NAME, VERSION_STRING, GIT_COMMIT);
#else
            std::string versionText = std::format("{} v{} ({})", PROJECT_NAME, VERSION_STRING, GIT_COMMIT);
#endif
            const char* tagline = "";  // Zeroed out
            const char* subtitle = ""; // Zeroed out

            // Center the version text
            float textWidth = ImGui::CalcTextSize(versionText.c_str()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, alpha), "%s", versionText.c_str());

            // Center the tagline
            float taglineWidth = ImGui::CalcTextSize(tagline).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - taglineWidth) * 0.5f);
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.6f, alpha), "%s", tagline);

            // Center the subtitle (smaller, dimmer)
            float subtitleWidth = ImGui::CalcTextSize(subtitle).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - subtitleWidth) * 0.5f);
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, alpha), "%s", subtitle);

            ImGui::PopFont();
        }
        ImGui::End();

        ImGui::PopStyleVar(3);

        // Decrement timer
        m_lingerTime -= deltaTime;
    }
}
