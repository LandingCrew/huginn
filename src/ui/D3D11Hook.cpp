#include "D3D11Hook.h"
#include "ImGuiRenderer.h"
#include "WelcomeBanner.h"
#ifdef _DEBUG
#include "StateManagerDebugWidget.h"
#include "RegistryDebugWidget.h"  // v0.7.5
#include "UtilityScorerDebugWidget.h"  // v0.9.0
#include "DebugInputHook.h"
#endif

namespace Huginn::UI
{
    // Present hook - called after the game's Present call
    static void PresentHook(std::uint32_t a1)
    {
        // Call original Present first
        D3D11Hook::_originalPresent(a1);

        // Then render our ImGui overlay
        auto& renderer = ImGuiRenderer::GetSingleton();
        if (renderer.IsInitialized()) {
            renderer.BeginFrame();

#ifdef _DEBUG
            // Show ImGui software cursor when debug interaction mode is active.
            // NoMouseCursorChange is set during init to hide the cursor normally;
            // we temporarily override it here so ImGui draws its own cursor.
            ImGui::GetIO().MouseDrawCursor = DebugInputHook::IsInteracting();
#endif

            // Draw all UI elements
            WelcomeBanner::GetSingleton().Draw();

            // Debug widgets (only in debug builds)
#ifdef _DEBUG
            StateManagerDebugWidget::GetSingleton().Draw();
            RegistryDebugWidget::GetSingleton().Draw();    // v0.7.5
            UtilityScorerDebugWidget::GetSingleton().Draw();  // v0.9.0
#endif

            renderer.EndFrame();
        }
    }

    bool D3D11Hook::Install()
    {
        // Hook the D3D11 Present call
        // REL::ID 75461 (SE) / 77246 (AE) is BSGraphics::Renderer::End which calls Present
        // The Present call is at offset 0x9

        SKSE::AllocTrampoline(14);

        REL::Relocation<std::uintptr_t> presentTarget{ RELOCATION_ID(75461, 77246) };
        auto& trampoline = SKSE::GetTrampoline();

        _originalPresent = trampoline.write_call<5>(
            presentTarget.address() + REL::VariantOffset(0x9, 0x9, 0x15).offset(),
            PresentHook
        );

        logger::info("[D3D11Hook] Present hook installed at {:X}"sv, presentTarget.address());
        return true;
    }
}
