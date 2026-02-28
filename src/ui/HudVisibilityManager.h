#pragma once

namespace Huginn::UI
{
    /// Listens for MenuOpenCloseEvent and hides/shows IntuitionMenu
    /// based on the active input context (gameplay = show, menus = hide).
    class HudVisibilityManager : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static HudVisibilityManager& GetSingleton();
        static void Register();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_eventSource) override;

    private:
        HudVisibilityManager() = default;
        void UpdateVisibility();
    };
}
