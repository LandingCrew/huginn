#pragma once

#ifdef _DEBUG

namespace Huginn::UI
{
    /// Hooks SKSE's InputEventDispatch to translate DirectInput events into ImGui
    /// input and provide an interaction toggle (Home key) for debug widgets.
    ///
    /// When interaction mode is active:
    ///   - All DirectInput events are translated to ImGui key/mouse/char events
    ///   - A dummy (empty) event list is passed to the game, blocking all game input
    ///   - ImGui renders a software cursor for widget interaction
    ///
    /// When interaction mode is inactive:
    ///   - Events still translate to ImGui (for hover detection, etc.)
    ///   - Real events pass through to the game as normal
    ///
    /// Debug-only: entire class is compiled out in release builds.
    class DebugInputHook
    {
    public:
        /// Install the dispatch hook. Call once at kDataLoaded, after D3D11Hook.
        static bool Install();

        /// Whether the user has toggled interaction mode (Home key).
        static bool IsInteracting() { return s_interacting; }

    private:
        /// The hooked dispatch function — replaces BSInputDeviceManager::DispatchEvents.
        static void DispatchHook(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher,
                                 RE::InputEvent** a_events);

        /// Translate a single RE::ButtonEvent into ImGui IO calls.
        static void TranslateButtonEvent(RE::ButtonEvent* a_button);

        /// Translate a DirectInput scan code → Windows VK → ImGuiKey.
        static ImGuiKey VirtualKeyToImGuiKey(uint32_t a_vkCode);

        /// Toggle flag — true while the user is interacting with debug widgets.
        static inline bool s_interacting = false;

        /// Trampoline storage for the original dispatch function.
        static inline REL::Relocation<decltype(DispatchHook)> _originalDispatch;
    };
}

#endif // _DEBUG
