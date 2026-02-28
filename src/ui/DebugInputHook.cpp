#ifdef _DEBUG

#include "DebugInputHook.h"

#include <dinput.h>   // DIK_* scan codes
#include <imgui.h>

namespace Huginn::UI
{
    // =========================================================================
    // DirectInput scan code → Windows Virtual Key mapping
    // =========================================================================
    // Skyrim uses DirectInput for keyboard input. DirectInput gives us scan
    // codes (DIK_*), but ImGui expects Windows virtual key codes (VK_*).
    // MapVirtualKeyEx performs this translation using the active keyboard layout.
    // =========================================================================

    static uint32_t DIKToVK(uint32_t a_dikCode)
    {
        // MapVirtualKeyEx with MAPVK_VSC_TO_VK_EX handles extended keys correctly
        // (e.g., distinguishing left/right Ctrl, numpad Enter vs main Enter).
        return MapVirtualKeyEx(a_dikCode, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
    }

    // =========================================================================
    // VK → ImGuiKey translation
    // =========================================================================
    // ImGui 1.87+ uses a named key API (io.AddKeyEvent) instead of the old
    // io.KeysDown[] array. This mapping covers the keys most relevant for
    // widget interaction: navigation, text input, and modifiers.
    // =========================================================================

    ImGuiKey DebugInputHook::VirtualKeyToImGuiKey(uint32_t a_vkCode)
    {
        // clang-format off
        switch (a_vkCode) {
        case VK_TAB:        return ImGuiKey_Tab;
        case VK_LEFT:       return ImGuiKey_LeftArrow;
        case VK_RIGHT:      return ImGuiKey_RightArrow;
        case VK_UP:         return ImGuiKey_UpArrow;
        case VK_DOWN:       return ImGuiKey_DownArrow;
        case VK_PRIOR:      return ImGuiKey_PageUp;
        case VK_NEXT:       return ImGuiKey_PageDown;
        case VK_HOME:       return ImGuiKey_Home;
        case VK_END:        return ImGuiKey_End;
        case VK_INSERT:     return ImGuiKey_Insert;
        case VK_DELETE:     return ImGuiKey_Delete;
        case VK_BACK:       return ImGuiKey_Backspace;
        case VK_SPACE:      return ImGuiKey_Space;
        case VK_RETURN:     return ImGuiKey_Enter;
        case VK_ESCAPE:     return ImGuiKey_Escape;
        case VK_OEM_7:      return ImGuiKey_Apostrophe;
        case VK_OEM_COMMA:  return ImGuiKey_Comma;
        case VK_OEM_MINUS:  return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_2:      return ImGuiKey_Slash;
        case VK_OEM_1:      return ImGuiKey_Semicolon;
        case VK_OEM_PLUS:   return ImGuiKey_Equal;
        case VK_OEM_4:      return ImGuiKey_LeftBracket;
        case VK_OEM_5:      return ImGuiKey_Backslash;
        case VK_OEM_6:      return ImGuiKey_RightBracket;
        case VK_OEM_3:      return ImGuiKey_GraveAccent;
        case VK_CAPITAL:    return ImGuiKey_CapsLock;
        case VK_SCROLL:     return ImGuiKey_ScrollLock;
        case VK_NUMLOCK:    return ImGuiKey_NumLock;
        case VK_SNAPSHOT:   return ImGuiKey_PrintScreen;
        case VK_PAUSE:      return ImGuiKey_Pause;
        case VK_NUMPAD0:    return ImGuiKey_Keypad0;
        case VK_NUMPAD1:    return ImGuiKey_Keypad1;
        case VK_NUMPAD2:    return ImGuiKey_Keypad2;
        case VK_NUMPAD3:    return ImGuiKey_Keypad3;
        case VK_NUMPAD4:    return ImGuiKey_Keypad4;
        case VK_NUMPAD5:    return ImGuiKey_Keypad5;
        case VK_NUMPAD6:    return ImGuiKey_Keypad6;
        case VK_NUMPAD7:    return ImGuiKey_Keypad7;
        case VK_NUMPAD8:    return ImGuiKey_Keypad8;
        case VK_NUMPAD9:    return ImGuiKey_Keypad9;
        case VK_DECIMAL:    return ImGuiKey_KeypadDecimal;
        case VK_DIVIDE:     return ImGuiKey_KeypadDivide;
        case VK_MULTIPLY:   return ImGuiKey_KeypadMultiply;
        case VK_SUBTRACT:   return ImGuiKey_KeypadSubtract;
        case VK_ADD:        return ImGuiKey_KeypadAdd;
        case VK_LSHIFT:     return ImGuiKey_LeftShift;
        case VK_LCONTROL:   return ImGuiKey_LeftCtrl;
        case VK_LMENU:      return ImGuiKey_LeftAlt;
        case VK_LWIN:       return ImGuiKey_LeftSuper;
        case VK_RSHIFT:     return ImGuiKey_RightShift;
        case VK_RCONTROL:   return ImGuiKey_RightCtrl;
        case VK_RMENU:      return ImGuiKey_RightAlt;
        case VK_RWIN:       return ImGuiKey_RightSuper;
        case VK_APPS:       return ImGuiKey_Menu;
        case '0':           return ImGuiKey_0;
        case '1':           return ImGuiKey_1;
        case '2':           return ImGuiKey_2;
        case '3':           return ImGuiKey_3;
        case '4':           return ImGuiKey_4;
        case '5':           return ImGuiKey_5;
        case '6':           return ImGuiKey_6;
        case '7':           return ImGuiKey_7;
        case '8':           return ImGuiKey_8;
        case '9':           return ImGuiKey_9;
        case 'A':           return ImGuiKey_A;
        case 'B':           return ImGuiKey_B;
        case 'C':           return ImGuiKey_C;
        case 'D':           return ImGuiKey_D;
        case 'E':           return ImGuiKey_E;
        case 'F':           return ImGuiKey_F;
        case 'G':           return ImGuiKey_G;
        case 'H':           return ImGuiKey_H;
        case 'I':           return ImGuiKey_I;
        case 'J':           return ImGuiKey_J;
        case 'K':           return ImGuiKey_K;
        case 'L':           return ImGuiKey_L;
        case 'M':           return ImGuiKey_M;
        case 'N':           return ImGuiKey_N;
        case 'O':           return ImGuiKey_O;
        case 'P':           return ImGuiKey_P;
        case 'Q':           return ImGuiKey_Q;
        case 'R':           return ImGuiKey_R;
        case 'S':           return ImGuiKey_S;
        case 'T':           return ImGuiKey_T;
        case 'U':           return ImGuiKey_U;
        case 'V':           return ImGuiKey_V;
        case 'W':           return ImGuiKey_W;
        case 'X':           return ImGuiKey_X;
        case 'Y':           return ImGuiKey_Y;
        case 'Z':           return ImGuiKey_Z;
        case VK_F1:         return ImGuiKey_F1;
        case VK_F2:         return ImGuiKey_F2;
        case VK_F3:         return ImGuiKey_F3;
        case VK_F4:         return ImGuiKey_F4;
        case VK_F5:         return ImGuiKey_F5;
        case VK_F6:         return ImGuiKey_F6;
        case VK_F7:         return ImGuiKey_F7;
        case VK_F8:         return ImGuiKey_F8;
        case VK_F9:         return ImGuiKey_F9;
        case VK_F10:        return ImGuiKey_F10;
        case VK_F11:        return ImGuiKey_F11;
        case VK_F12:        return ImGuiKey_F12;
        default:            return ImGuiKey_None;
        }
        // clang-format on
    }

    // =========================================================================
    // Button event → ImGui translation
    // =========================================================================

    void DebugInputHook::TranslateButtonEvent(RE::ButtonEvent* a_button)
    {
        if (!a_button) {
            return;
        }

        auto& io = ImGui::GetIO();
        const auto device = a_button->GetDevice();
        const bool isPressed = a_button->IsDown() || a_button->IsHeld();
        const uint32_t idCode = a_button->GetIDCode();

        switch (device) {
        case RE::INPUT_DEVICE::kKeyboard:
        {
            // Translate DirectInput scan code → VK → ImGuiKey
            uint32_t vk = DIKToVK(idCode);
            ImGuiKey key = VirtualKeyToImGuiKey(vk);
            if (key != ImGuiKey_None) {
                io.AddKeyEvent(key, isPressed);
            }

            // Modifier state — ImGui tracks these separately for shortcuts
            io.AddKeyEvent(ImGuiKey_ModCtrl,  (GetKeyState(VK_CONTROL) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiKey_ModShift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiKey_ModAlt,   (GetKeyState(VK_MENU) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiKey_ModSuper, (GetKeyState(VK_LWIN) & 0x8000) != 0);

            // Character input — needed for ImGui text fields
            // Only on key-down to avoid duplicate characters
            if (a_button->IsDown() && vk > 0) {
                // ToUnicode translates VK + scan code → character, respecting
                // the current keyboard layout, dead keys, and shift state.
                BYTE keyboardState[256];
                if (GetKeyboardState(keyboardState)) {
                    WCHAR buffer[4] = {};
                    int result = ToUnicode(vk, idCode, keyboardState, buffer, 4, 0);
                    if (result > 0) {
                        for (int i = 0; i < result; ++i) {
                            io.AddInputCharacterUTF16(buffer[i]);
                        }
                    }
                }
            }
            break;
        }
        case RE::INPUT_DEVICE::kMouse:
        {
            // Mouse button mapping: 0=Left, 1=Right, 2=Middle
            if (idCode < 3) {
                io.AddMouseButtonEvent(static_cast<int>(idCode), isPressed);
            }
            // Mouse wheel — encoded as button ID 8 (scroll up) / 9 (scroll down)
            // in Skyrim's input system. The value() gives the wheel delta.
            else if (idCode == 8) {
                io.AddMouseWheelEvent(0.0f, a_button->Value());
            } else if (idCode == 9) {
                io.AddMouseWheelEvent(0.0f, -a_button->Value());
            }
            break;
        }
        default:
            break;
        }
    }

    // =========================================================================
    // Dispatch hook — the core of the input interception
    // =========================================================================
    // This replaces BSInputDeviceManager's call to notify event sinks.
    // On every frame, Skyrim collects all input into a linked list of
    // RE::InputEvent and calls DispatchEvents on BSTEventSource<InputEvent*>.
    //
    // We intercept that call to:
    //   1. Translate all events to ImGui (always, for hover/cursor tracking)
    //   2. Check for Home key toggle
    //   3. If interacting: pass an empty event list to the game
    //   4. If not interacting: pass the real events through
    // =========================================================================

    void DebugInputHook::DispatchHook(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher,
                                       RE::InputEvent** a_events)
    {
        static RE::InputEvent* const dummy[] = { nullptr };

        if (a_events) {
            // Walk the event linked list
            for (auto* event = *a_events; event; event = event->next) {
                if (auto* button = event->AsButtonEvent()) {
                    // Check for Home key toggle (DIK_HOME = 0xC7)
                    if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
                        button->GetIDCode() == DIK_HOME &&
                        button->IsDown())
                    {
                        s_interacting = !s_interacting;
                        logger::info("[DebugInputHook] Interaction mode: {}"sv,
                            s_interacting ? "ON" : "OFF");
                    }

                    TranslateButtonEvent(button);
                }

                // Mouse position: ImGui_ImplWin32 already tracks absolute cursor
                // position via WndProc WM_MOUSEMOVE, so no translation needed here.
            }
        }

        // If interacting, block all game input by passing an empty event list.
        // This prevents both Skyrim's native input AND Huginn's own UpdateHandler
        // keybindings from firing — correct behavior while manipulating widgets.
        if (s_interacting) {
            _originalDispatch(a_dispatcher, const_cast<RE::InputEvent**>(dummy));
        } else {
            _originalDispatch(a_dispatcher, a_events);
        }
    }

    // =========================================================================
    // Installation
    // =========================================================================

    bool DebugInputHook::Install()
    {
        // Hook BSInputDeviceManager::DispatchEvents
        // RELOCATION_ID(67315, 68617) = BSInputDeviceManager::Poll
        // Offset 0x7B (SE) / 0x7B (AE) = the call to BSTEventSource::Notify
        SKSE::AllocTrampoline(14);

        REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(67315, 68617) };
        auto& trampoline = SKSE::GetTrampoline();

        _originalDispatch = trampoline.write_call<5>(
            target.address() + REL::VariantOffset(0x7B, 0x7B, 0x7B).offset(),
            DispatchHook
        );

        logger::info("[DebugInputHook] Dispatch hook installed at {:X} (toggle: Home key)"sv,
            target.address());
        return true;
    }
}

#endif // _DEBUG
