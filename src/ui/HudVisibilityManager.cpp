#include "HudVisibilityManager.h"
#include "IntuitionMenu.h"

namespace Huginn::UI
{
    HudVisibilityManager& HudVisibilityManager::GetSingleton()
    {
        static HudVisibilityManager singleton;
        return singleton;
    }

    void HudVisibilityManager::Register()
    {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(&GetSingleton());
            logger::info("HudVisibilityManager registered for MenuOpenCloseEvent"sv);
        }
    }

    RE::BSEventNotifyControl HudVisibilityManager::ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        [[maybe_unused]] RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_eventSource)
    {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;

        // Re-show IntuitionMenu after loading screen closes.
        // Skyrim closes (but doesn't destroy) custom IMenu instances during cell
        // transitions.  The C++ singleton survives, but the menu is no longer "open"
        // in RE::UI — so SetVisible(true) has no effect on a closed menu.
        // Re-sending kShow reopens it.
        if (!a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
            auto* ui = RE::UI::GetSingleton();
            bool menuOpen = ui ? ui->IsMenuOpen(IntuitionMenu::MENU_NAME) : false;
            if (!menuOpen) {
                logger::info("HudVisibilityManager: Loading screen closed, IntuitionMenu not open — re-showing"sv);
                IntuitionMenu::Show();
            }
        }

        UpdateVisibility();
        return RE::BSEventNotifyControl::kContinue;
    }

    void HudVisibilityManager::UpdateVisibility()
    {
        auto* menu = IntuitionMenu::GetSingleton();
        if (!menu) {
    
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;

        // Widget visible when no game-pausing menu is open.
        // Console and Favorites don't set kPausesGame, so the widget
        // correctly stays visible during those overlays.
        bool visible = !ui->GameIsPaused();

        SKSE::GetTaskInterface()->AddUITask([visible]() {
            auto* m = IntuitionMenu::GetSingleton();
            if (m) m->SetVisible(visible);
        });
    }
}
