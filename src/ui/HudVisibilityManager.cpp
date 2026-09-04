#include "HudVisibilityManager.h"
#include "IntuitionMenu.h"
#include "IntuitionSettings.h"

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

    // The game has taken the camera and the player is watching, not playing.
    //
    // kAnimated is the scripted/cinematic camera — quest cut scenes and
    // killmoves. kAutoVanity is the idle orbit that takes over when the controls
    // go untouched. Neither pauses the game, which is why GameIsPaused() alone
    // left the widget on screen through both.
    //
    // Two neighbours are deliberately NOT here. kFurniture (beds, chairs,
    // crafting stations) would hide the widget at an alchemy table, which is
    // precisely where the workstation context has something to say — hiding
    // there would suppress the feature, not a cut scene. kBleedout is not a cut
    // scene either; it is its own question, and folding it in here would decide
    // it silently.
    static bool IsCinematicCamera()
    {
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->currentState) {
            return false;
        }
        // QCameraEquals is private in CommonLibSSE-NG; currentState->id is the
        // public route to the same answer.
        const auto id = camera->currentState->id;
        return id == RE::CameraState::kAnimated
            || id == RE::CameraState::kAutoVanity;
    }

    bool HudVisibilityManager::ComputeVisible() const
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;

        // Widget visible when no game-pausing menu is open AND the master enable
        // toggle is on. Console and Favorites don't set kPausesGame, so the widget
        // correctly stays visible during those overlays. The IsEnabled() gate is
        // what keeps a user-disabled widget from being re-shown on every unpause.
        // IsUserHidden() is also enforced inside SetVisible(), so this line is
        // belt-and-braces — but computing it here keeps the predicate honest for
        // anyone reading UpdateVisibility to find out when the widget shows.
        return !ui->GameIsPaused()
            && !IsCinematicCamera()
            && IntuitionSettings::GetSingleton().IsEnabled()
            && !IntuitionMenu::IsUserHidden();
    }

    void HudVisibilityManager::Poll()
    {
        const bool visible = ComputeVisible();
        if (visible == m_lastPolled) {
            return;
        }
        m_lastPolled = visible;

        // A transition, so it logs — one line per cut scene, not per tick.
        logger::info("HudVisibilityManager: widget {} (cinematic camera: {})"sv,
            visible ? "shown"sv : "hidden"sv,
            IsCinematicCamera() ? "yes"sv : "no"sv);

        UpdateVisibility();
    }

    void HudVisibilityManager::UpdateVisibility()
    {
        auto* menu = IntuitionMenu::GetSingleton();
        if (!menu) {

            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;

        bool visible = ComputeVisible();

        // SetVisible already defers the GFx work via AddUITask internally, so
        // call it directly — wrapping it in another AddUITask here would add a
        // second task-queue round-trip (an extra frame of latency).
        menu->SetVisible(visible);
    }
}
