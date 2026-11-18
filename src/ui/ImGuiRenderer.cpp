#include "ImGuiRenderer.h"

#include "ImGuiCommon.h"
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <mutex>
#include <vector>

// Forward declare the ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Huginn::UI
{
    // =========================================================================
    // The window thread's messages, for the render thread
    // =========================================================================
    // The window procedure runs on the thread that owns the game's window,
    // and that is not the thread that renders: 2026-10-31's log had the first
    // window message on thread 42388 and the first overlay frame on 10836.
    // ImGui's io is not thread-safe (NewFrame empties the input queue that
    // ImGui_ImplWin32_WndProcHandler appends to; the race DebugInputHook's
    // queue fixed, imgui.h:2101), and in a release build the same race has no
    // assert to stop it. So WndProcHook only copies the messages the Win32
    // backend handles, and BeginFrame replays them through the backend on the
    // render thread, before NewFrame.
    //
    // Replayed off the window's thread, the backend's thread-bound calls
    // (SetCapture, TrackMouseEvent, GetMessageExtraInfo) answer for the
    // render thread: no mouse capture, no leave tracking, the mouse always
    // reported as a mouse. The overlay needs none of them; it reads the
    // position from WM_MOUSEMOVE, and NewFrame polls the cursor too.
    // WM_SETCURSOR is not replayed: its answer is due at once, and with
    // NoMouseCursorChange the backend leaves the cursor alone anyway.
    //
    // Keyboard messages are not replayed at all. The backend reads the
    // modifiers with GetKeyState, which on the render thread (it handles no
    // keyboard messages) reads every modifier as up: Ctrl, Shift and Alt
    // would be submitted released over DebugInputHook's correct state. The
    // keys reach ImGui from DebugInputHook in a Debug build; a release
    // build's overlay (the welcome banner) takes no keys.
    //
    // The queue merges a run of mouse moves into the latest, and when full
    // it drops the oldest mouse move first, then the oldest press, and a
    // release or focus change only if nothing else is left: a lost
    // button-up would leave ImGui holding the button.
    // =========================================================================

    namespace
    {
        struct PendingMessage
        {
            HWND hwnd;
            UINT msg;
            WPARAM wParam;
            LPARAM lParam;
        };

        // A frame's worth is a few messages; the cap only matters if no
        // frame drains for a while (see QueueMessage for what goes first).
        constexpr std::size_t kMaxPendingMessages = 512;

        std::mutex g_messageLock;
        std::vector<PendingMessage> g_messages;

        bool IsBackendMessage(UINT a_msg)
        {
            switch (a_msg) {
            case WM_MOUSEMOVE:
            case WM_NCMOUSEMOVE:
            case WM_MOUSELEAVE:
            case WM_NCMOUSELEAVE:
            case WM_DESTROY:
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP:
            case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
            case WM_DEVICECHANGE:
                return true;
            default:
                return false;
            }
        }

        bool IsMouseMove(UINT a_msg) { return a_msg == WM_MOUSEMOVE || a_msg == WM_NCMOUSEMOVE; }

        // A release, a leave or a focus change: losing one leaves ImGui's
        // state stuck (a held button, a hovered window, a focused game).
        bool MustKeep(UINT a_msg)
        {
            switch (a_msg) {
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            case WM_MOUSELEAVE: case WM_NCMOUSELEAVE:
            case WM_SETFOCUS: case WM_KILLFOCUS:
            case WM_DESTROY:
                return true;
            default:
                return false;
            }
        }

        void QueueMessage(const PendingMessage& a_message)
        {
            std::lock_guard lock(g_messageLock);
            if (IsMouseMove(a_message.msg) && !g_messages.empty()) {
                auto& last = g_messages.back();
                if (last.msg == a_message.msg && last.hwnd == a_message.hwnd) {
                    last = a_message;
                    return;
                }
            }
            if (g_messages.size() >= kMaxPendingMessages) {
                auto victim = std::find_if(g_messages.begin(), g_messages.end(),
                    [](const PendingMessage& m) { return IsMouseMove(m.msg); });
                if (victim == g_messages.end()) {
                    victim = std::find_if(g_messages.begin(), g_messages.end(),
                        [](const PendingMessage& m) { return !MustKeep(m.msg); });
                }
                if (victim == g_messages.end()) {
                    victim = g_messages.begin();
                }
                g_messages.erase(victim);
            }
            g_messages.push_back(a_message);
        }

        void ReplayMessages()
        {
            std::vector<PendingMessage> messages;
            {
                std::lock_guard lock(g_messageLock);
                messages.swap(g_messages);
            }
            for (const auto& m : messages) {
                ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wParam, m.lParam);
            }
        }
    }

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

        // Last, with release: the Present and input hooks start using ImGui
        // the moment they observe this (see IsInitialized()).
        m_initialized.store(true, std::memory_order_release);
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
        {
            std::lock_guard lock(g_messageLock);
            g_messages.clear();
        }
        s_wantCaptureMouse.store(false, std::memory_order_relaxed);
        s_wantCaptureKeyboard.store(false, std::memory_order_relaxed);

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
            // The window thread: io belongs to the render thread (see the
            // comment at the top), so the message is queued for BeginFrame,
            // and the capture answers are the last frame's.
            static std::atomic<bool> s_firstMessageLogged{ false };
            if (!s_firstMessageLogged.exchange(true, std::memory_order_relaxed)) {
                logger::info("[ImGuiRenderer] First window message with ImGui ready (thread {})"sv, GetCurrentThreadId());
            }

            if (IsBackendMessage(msg)) {
                QueueMessage({ hWnd, msg, wParam, lParam });
            }

            // Block mouse input from reaching the game if ImGui wants it
            if (s_wantCaptureMouse.load(std::memory_order_relaxed)) {
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
            if (s_wantCaptureKeyboard.load(std::memory_order_relaxed)) {
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

        // The window thread's messages, before the backend's NewFrame.
        ReplayMessages();

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

        // For WndProcHook's blocking, on the window thread.
        const ImGuiIO& io = ImGui::GetIO();
        s_wantCaptureMouse.store(io.WantCaptureMouse, std::memory_order_relaxed);
        s_wantCaptureKeyboard.store(io.WantCaptureKeyboard, std::memory_order_relaxed);

        // Restore D3D11 render targets to what the game/other plugins expect
        if (m_savedRTV || m_savedDSV) {
            m_context->OMSetRenderTargets(1, &m_savedRTV, m_savedDSV);
            if (m_savedRTV) { m_savedRTV->Release(); m_savedRTV = nullptr; }
            if (m_savedDSV) { m_savedDSV->Release(); m_savedDSV = nullptr; }
        }
    }
}
