#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace Huginn::UI
{
    class ImGuiRenderer
    {
    public:
        static ImGuiRenderer& GetSingleton();

        bool Initialize();
        void Shutdown();

        void BeginFrame();
        void EndFrame();

        bool IsInitialized() const { return m_initialized; }

    private:
        ImGuiRenderer() = default;
        ~ImGuiRenderer() = default;
        ImGuiRenderer(const ImGuiRenderer&) = delete;
        ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

        bool InitD3D11();
        bool HookWndProc();
        void UnhookWndProc();
        void SetupImGuiStyle();

        // WndProc hook for input handling
        static LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        inline static WNDPROC s_originalWndProc = nullptr;
        inline static ImGuiRenderer* s_instance = nullptr;

        // D3D11 resources
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        IDXGISwapChain* m_swapChain = nullptr;
        HWND m_hwnd = nullptr;

        // Saved D3D11 state for coexistence with other ImGui renderers
        ID3D11RenderTargetView* m_savedRTV = nullptr;
        ID3D11DepthStencilView* m_savedDSV = nullptr;

        bool m_initialized = false;
        bool m_inputEnabled = true;  // Enable input by default in debug builds
    };
}
