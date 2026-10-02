#pragma once

#include "EquipEvent.h"
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace Huginn::Learning
{
    // =========================================================================
    // SELECTION TRACKER - one selection path: hold, then confirm
    // =========================================================================
    // Every player selection -- a Huginn key, Huginn's wheel, the player's own
    // menus, vanilla hotkeys or own wheels -- comes through Select(), which
    // records a PENDING selection: the item, the game state at that moment,
    // what the bar was showing, and the source (a label, never a weight).
    //
    // A pending selection confirms, and is dispatched to the learner exactly
    // once, when:
    //   - Consumable: its count drops for real (OnConsumed) within
    //     CONSUMPTION_HUGINN_WINDOW_MS;
    //   - Equip: it is still equipped SELECTION_CONFIRM_MS after the pick --
    //     or its count drops first (a scroll cast straight away).
    // Anything else expires unconfirmed and teaches nothing. That replaces
    // misclick detection: swapped away inside the window is never confirmed,
    // and a circlet and a ring both confirm because both are still worn.
    //
    // One pending record per item: both hands, a doubled TESEquipEvent, an
    // equip event plus the count drop -- all one selection.
    //
    // Before this (roadmap "The reward path", 2026-10-02) a drink trained
    // twice (+8 for the key, +5 for the count drop), late events fired false
    // misclick penalties, the learner saw the state at DETECTION time, and
    // scripted equips counted as the player.
    //
    // Thread: Select from the input sink, Wheeler callbacks and TESEquipEvent
    // sinks; OnConsumed and Update from the update loop. All game-thread in
    // practice; a mutex anyway, and Dispatch always runs outside it.
    // =========================================================================
    class SelectionTracker
    {
    public:
        static SelectionTracker& GetSingleton()
        {
            static SelectionTracker instance;
            return instance;
        }

        /// The player selected `formID`. `via` says how ("key s3", "inventory
        /// menu"...); `attribution` is the external A-E case label, if any.
        void Select(RE::FormID formID, EquipSource source, std::string via,
                    std::string attribution = {});

        /// A real consumption of `formID` (count dropped, not a drop/sell/store).
        /// Confirms a pending selection of it; returns false when there was none,
        /// in which case the drop teaches nothing.
        bool OnConsumed(RE::FormID formID);

        /// Expire and confirm by deadline. Update thread, every tick.
        void Update();

        /// Forget everything pending (game load). Nothing is confirmed.
        void Clear();

        [[nodiscard]] size_t PendingCount() const
        {
            std::lock_guard lock(m_mutex);
            return m_pending.size();
        }

    private:
        SelectionTracker() = default;
        SelectionTracker(const SelectionTracker&) = delete;
        SelectionTracker& operator=(const SelectionTracker&) = delete;

        struct Pending
        {
            EquipEvent event;
            std::chrono::steady_clock::time_point selectedAt;
            std::chrono::steady_clock::time_point deadline;
        };

        static SelectionKind KindOf(RE::FormID formID);
        static bool IsStillEquipped(RE::FormID formID);
        static void Confirm(EquipEvent& event, std::chrono::steady_clock::time_point selectedAt,
                            const char* how);

        mutable std::mutex m_mutex;
        std::vector<Pending> m_pending;
    };

}  // namespace Huginn::Learning
