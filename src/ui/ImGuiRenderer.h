#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>

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

        /// Read from the render thread (PresentHook) and the input thread
        /// (DebugInputHook), which are hooked from SKSEPlugin_Load, long before
        /// Initialize() runs at kDataLoaded. Acquire pairs with the release
        /// store at the end of Initialize(), so a reader that sees true also
        /// sees the ImGui context and backends it set up.
        bool IsInitialized() const { return m_initialized.load(std::memory_order_acquire); }

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

        std::atomic<bool> m_initialized{ false };
        bool m_inputEnabled = true;  // Enable input by default in debug builds
    };
}
