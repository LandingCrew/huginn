#pragma once

#include <unordered_map>

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

        /// Is a selection of `formID` pending? An equip event for it is then part
        /// of that selection, not a new one (ExternalEquipLearner).
        [[nodiscard]] bool IsPending(RE::FormID formID) const
        {
            std::lock_guard lock(m_mutex);
            for (const auto& p : m_pending) {
                if (p.event.formID == formID) return true;
            }
            return false;
        }

        /// Drop a pending selection of `formID` unconfirmed: the player took it
        /// back (a Remembrance swap-back of the item). Logged with `why`.
        void Withdraw(RE::FormID formID, std::string_view why);

        /// Expire and confirm by deadline. Update thread, every tick.
        void Update();

        /// Forget everything pending (game load). Nothing is confirmed.
        void Clear();
        /// Forget the repeat window (`hg reset weights`): the next pick of
        /// anything is a new decision.
        void ForgetRepeats();

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

    public:
        /// Is `formID` in a hand, worn, or the nocked ammo? (Also used by the
        /// selection log v3 to confirm the picks it keeps on its own.)
        static bool IsStillEquipped(RE::FormID formID);

    private:
        void Confirm(EquipEvent& event, std::chrono::steady_clock::time_point selectedAt,
                            const char* how);

        mutable std::mutex m_mutex;
        std::vector<Pending> m_pending;
        // Last confirmed EQUIP of each item, for the repeat window (guarded by m_mutex).
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> m_lastEquipPick;
    };

}  // namespace Huginn::Learning
