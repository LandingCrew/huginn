#pragma once

#include "Config.h"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace Huginn::Learning
{
    // =========================================================================
    // PLAYER INPUT GATE - did the PLAYER make this outside equip?
    // =========================================================================
    // An equip Huginn did not trigger (a TESEquipEvent with no Huginn mark)
    // counts as a selection only with player input behind it. A script equip
    // or drink has none: LoreRim's auto-quaff trained the learner as the
    // player until this gate (2026-09-21; see "the reward path" on the
    // roadmap). Player input is one of:
    //   - an open Inventory, Favorites or Magic menu, or one that closed in the
    //     last MENU_CLOSE_INPUT_WINDOW_MS (LoreRim shuts the inventory as the
    //     player drinks, before the potion's equip event arrives);
    //   - a vanilla favourites hotkey (Hotkey1-8) pressed just before;
    //   - a pick off one of the player's OWN Wheeler wheels, for that item;
    //   - any Wheeler wheel open. Needed because the order of Wheeler's equip
    //     and its activation callback is not something Huginn controls: if
    //     the equip lands first, neither the own-wheel note nor Huginn's
    //     wheel selection exists yet. A Huginn-wheel pick caught this way is
    //     upgraded when its own selection arrives (SelectionTracker::Select).
    // The console is deliberately not on the list.
    //
    // Thread: the notes come from the input sink and the Wheeler callback,
    // the query from TESEquipEvent sinks -- all game-thread, but the state
    // is atomic / locked anyway, as it is cheap.
    // =========================================================================
    class PlayerInputGate
    {
    public:
        static PlayerInputGate& GetSingleton()
        {
            static PlayerInputGate instance;
            return instance;
        }

        /// A vanilla favourites hotkey (user event Hotkey1-8) went down.
        void NoteVanillaHotkey()
        {
            m_lastHotkey.store(Now(), std::memory_order_release);
        }

        /// An Inventory, Favorites or Magic menu just closed (HudVisibilityManager's
        /// menu sink -- it fires while the update loop is paused).
        void NoteMenuClosed()
        {
            m_lastMenuClose.store(Now(), std::memory_order_release);
        }

        [[nodiscard]] static bool IsSelectionMenu(const RE::BSFixedString& menuName)
        {
            return menuName == RE::InventoryMenu::MENU_NAME ||
                   menuName == RE::FavoritesMenu::MENU_NAME ||
                   menuName == RE::MagicMenu::MENU_NAME;
        }

        /// The player activated `formID` on a Wheeler wheel that is not Huginn's.
        void NoteOwnWheelPick(RE::FormID formID)
        {
            std::lock_guard lock(m_mutex);
            m_wheelPicks[m_next] = { formID, Now() };
            m_next = (m_next + 1) % m_wheelPicks.size();
        }

        /// Wired by Main.cpp: is any Wheeler wheel open right now?
        void SetWheelOpenQuery(std::function<bool()> query)
        {
            std::lock_guard lock(m_mutex);
            m_isWheelOpen = std::move(query);
        }

        /// How the player made this equip, or empty when nothing says they did.
        [[nodiscard]] std::string Explain(RE::FormID formID) const
        {
            if (auto* ui = RE::UI::GetSingleton()) {
                if (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) return "inventory menu";
                if (ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME)) return "favorites menu";
                if (ui->IsMenuOpen(RE::MagicMenu::MENU_NAME))     return "magic menu";
            }

            const int64_t now = Now();
            if (Recent(m_lastMenuClose.load(std::memory_order_acquire), now,
                       Config::MENU_CLOSE_INPUT_WINDOW_MS)) {
                return "menu (just closed)";
            }
            if (Recent(m_lastHotkey.load(std::memory_order_acquire), now)) {
                return "vanilla hotkey";
            }

            std::lock_guard lock(m_mutex);
            for (const auto& pick : m_wheelPicks) {
                if (pick.formID == formID && formID != 0 && Recent(pick.when, now)) {
                    return "own wheel";
                }
            }
            if (m_isWheelOpen && m_isWheelOpen()) {
                return "Wheeler wheel";
            }
            return {};
        }

    private:
        PlayerInputGate() = default;

        static int64_t Now()
        {
            return std::chrono::steady_clock::now().time_since_epoch().count();
        }

        static bool Recent(int64_t then, int64_t now,
                           float windowMs = Config::PLAYER_INPUT_WINDOW_MS)
        {
            if (then == 0) return false;
            const float ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::duration(now - then)).count();
            return ms <= windowMs;
        }

        struct WheelPick
        {
            RE::FormID formID = 0;
            int64_t when = 0;
        };

        std::atomic<int64_t> m_lastHotkey{ 0 };
        std::atomic<int64_t> m_lastMenuClose{ 0 };
        mutable std::mutex m_mutex;
        std::array<WheelPick, 4> m_wheelPicks{};
        std::function<bool()> m_isWheelOpen;
        size_t m_next = 0;
    };

}  // namespace Huginn::Learning
