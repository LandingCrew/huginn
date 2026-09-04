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

        /// Recompute and apply widget visibility from current game state.
        /// Public because the hotkey toggle re-shows through here rather than
        /// calling SetVisible(true) directly — that way a paused game or a
        /// disabled widget still wins over an un-hide.
        void UpdateVisibility();

        /// Per-tick visibility check, called from the update loop.
        /// A camera change raises no MenuOpenCloseEvent, so the event path alone
        /// never sees a cut scene begin — the widget stayed up through the whole
        /// thing. Transition-gated: SetVisible queues a UI task, and this runs at
        /// ~10 Hz, so re-asserting every tick would be pure churn.
        void Poll();

    private:
        HudVisibilityManager() = default;

        /// The one predicate both paths use, so they cannot drift apart.
        [[nodiscard]] bool ComputeVisible() const;

        /// Last value Poll() acted on. Deliberately NOT consulted by
        /// UpdateVisibility, which still asserts unconditionally: other code
        /// (IntuitionBackend's wheel gate, IntuitionMenu::Show after a loading
        /// screen) moves the widget without going through here, so a cache
        /// treated as authoritative would eventually stop re-asserting against
        /// a state it never saw change.
        bool m_lastPolled = true;
    };
}
