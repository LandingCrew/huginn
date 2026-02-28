#include "ImGuiRenderer.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

// Forward declare the ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Huginn::UI
{
    ImGuiRenderer& ImGuiRenderer::GetSingleton()
    {
        static ImGuiRenderer instance;
        return instance;
    }

    bool ImGuiRenderer::Initialize()
    {
        if (m_initialized) {
            logger::debug("[ImGuiRenderer] Already initialized"sv);
            return true;
        }

        if (!InitD3D11()) {
            logger::error("[ImGuiRenderer] Failed to initialize D3D11"sv);
            return false;
        }

        // Create ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // Don't change cursor
        io.IniFilename = nullptr;  // Don't save settings

        // Get window handle from renderer
        auto renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) {
            logger::error("[ImGuiRenderer] Failed to get BSGraphics::Renderer"sv);
            return false;
        }

        auto& renderData = renderer->data;
        m_hwnd = reinterpret_cast<HWND>(renderData.renderWindows[0].hWnd);

        // Initialize ImGui backends
        if (!ImGui_ImplWin32_Init(m_hwnd)) {
            logger::error("[ImGuiRenderer] ImGui_ImplWin32_Init failed"sv);
            return false;
        }

        if (!ImGui_ImplDX11_Init(m_device, m_context)) {
            logger::error("[ImGuiRenderer] ImGui_ImplDX11_Init failed"sv);
            ImGui_ImplWin32_Shutdown();
            return false;
        }

        // Hook the window procedure for input handling
        if (!HookWndProc()) {
            logger::warn("[ImGuiRenderer] Failed to hook WndProc - input will not work"sv);
            // Continue anyway - rendering will work, just no input
        }

        SetupImGuiStyle();

        m_initialized = true;
        logger::info("[ImGuiRenderer] Initialized successfully (input hook: {})"sv,
                     s_originalWndProc != nullptr ? "active" : "failed");
        return true;
    }

    void ImGuiRenderer::Shutdown()
    {
        if (!m_initialized) {
            return;
        }

        UnhookWndProc();

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        m_initialized = false;
        logger::info("[ImGuiRenderer] Shutdown"sv);
    }

    bool ImGuiRenderer::InitD3D11()
    {
        auto renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) {
            logger::error("[ImGuiRenderer] BSGraphics::Renderer not available"sv);
            return false;
        }

        auto& renderData = renderer->data;

        m_device = reinterpret_cast<ID3D11Device*>(renderData.forwarder);
        m_context = reinterpret_cast<ID3D11DeviceContext*>(renderData.context);
        m_swapChain = reinterpret_cast<IDXGISwapChain*>(renderData.renderWindows[0].swapChain);

        if (!m_device || !m_context || !m_swapChain) {
            logger::error("[ImGuiRenderer] Failed to get D3D11 resources"sv);
            return false;
        }

        logger::debug("[ImGuiRenderer] D3D11 resources acquired"sv);
        return true;
    }

    bool ImGuiRenderer::HookWndProc()
    {
        if (!m_hwnd) {
            logger::error("[ImGuiRenderer] Cannot hook WndProc - no HWND"sv);
            return false;
        }

        s_instance = this;

        // Get the original window procedure and replace with our hook
        s_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));

        if (!s_originalWndProc) {
            logger::error("[ImGuiRenderer] SetWindowLongPtrA failed: {}"sv, GetLastError());
            s_instance = nullptr;
            return false;
        }

        logger::debug("[ImGuiRenderer] WndProc hooked successfully"sv);
        return true;
    }

    void ImGuiRenderer::UnhookWndProc()
    {
        if (m_hwnd && s_originalWndProc) {
            SetWindowLongPtrA(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
            s_originalWndProc = nullptr;
            s_instance = nullptr;
            logger::debug("[ImGuiRenderer] WndProc unhooked"sv);
        }
    }

    LRESULT CALLBACK ImGuiRenderer::WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // Only process input if enabled
        if (s_instance && s_instance->m_inputEnabled) {
            // Let ImGui handle the message first
            if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
                return true;  // ImGui consumed the message
            }

            // Check if ImGui wants to capture input
            ImGuiIO& io = ImGui::GetIO();

            // Block mouse input from reaching the game if ImGui wants it
            if (io.WantCaptureMouse) {
                switch (msg) {
                    case WM_LBUTTONDOWN:
                    case WM_LBUTTONUP:
                    case WM_RBUTTONDOWN:
                    case WM_RBUTTONUP:
                    case WM_MBUTTONDOWN:
                    case WM_MBUTTONUP:
                    case WM_MOUSEWHEEL:
                    case WM_MOUSEMOVE:
                        return true;  // Block from game
                }
            }

            // Block keyboard input from reaching the game if ImGui wants it
            if (io.WantCaptureKeyboard) {
                switch (msg) {
                    case WM_KEYDOWN:
                    case WM_KEYUP:
                    case WM_CHAR:
                    case WM_SYSKEYDOWN:
                    case WM_SYSKEYUP:
                        return true;  // Block from game
                }
            }
        }

        // Pass to original handler
        return CallWindowProcA(s_originalWndProc, hWnd, msg, wParam, lParam);
    }

    void ImGuiRenderer::SetupImGuiStyle()
    {
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 4.0f;
        style.FrameRounding = 2.0f;
        style.Alpha = 0.95f;

        // Skyrim-ish colors
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.05f, 0.08f, 0.90f);
        colors[ImGuiCol_Border] = ImVec4(0.6f, 0.5f, 0.3f, 0.50f);  // Gold-ish border
        colors[ImGuiCol_Text] = ImVec4(0.95f, 0.90f, 0.80f, 1.00f);  // Warm white

        // Make interactive elements more visible
        colors[ImGuiCol_Header] = ImVec4(0.3f, 0.3f, 0.4f, 0.80f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.4f, 0.4f, 0.5f, 0.90f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.5f, 0.5f, 0.6f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.3f, 0.3f, 0.4f, 0.80f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.4f, 0.4f, 0.5f, 0.90f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.5f, 0.5f, 0.6f, 1.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.2f, 0.80f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.2f, 0.2f, 0.3f, 0.90f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    }

    void ImGuiRenderer::BeginFrame()
    {
        if (!m_initialized) {
            return;
        }

        // Save D3D11 state — other SKSE plugins (e.g. Wheeler) also render
        // ImGui on the same device context, so we must not assume our state
        // is preserved between frames.
        m_context->OMGetRenderTargets(1, &m_savedRTV, &m_savedDSV);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiRenderer::EndFrame()
    {
        if (!m_initialized) {
            return;
        }

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Restore D3D11 render targets to what the game/other plugins expect
        if (m_savedRTV || m_savedDSV) {
            m_context->OMSetRenderTargets(1, &m_savedRTV, m_savedDSV);
            if (m_savedRTV) { m_savedRTV->Release(); m_savedRTV = nullptr; }
            if (m_savedDSV) { m_savedDSV->Release(); m_savedDSV = nullptr; }
        }
    }
}
