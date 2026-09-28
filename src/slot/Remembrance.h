#pragma once

#include "SlotSettings.h"  // MAX_PAGES, MAX_SLOTS_PER_PAGE
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Huginn::Slot
{
    // =============================================================================
    // REMEMBRANCE — a slot holds what you just took off
    // =============================================================================
    // Press a Huginn key (or pick an entry on a Huginn wheel) that equips a
    // weapon, spell or ammo, and whatever that equip displaced takes the slot
    // you pressed for fRemembranceDurationMs: a one-deep undo under the same
    // finger. Then the slot goes back to normal recommendations.
    //
    // It is not a recommendation. SlotAllocator places it after overrides and
    // before everything else, whatever the slot's classification, and it never
    // reaches the learner: pressing the remembered item ends the hold and
    // publishes no reward (EquipManager / Main.cpp's Wheeler wiring).
    //
    // No chaining: equipping the remembered item does not remember what IT
    // displaced, so two items cannot ping-pong in one slot forever.
    //
    // Capture is by observation, not by asking the engine at press time: the
    // press registers a PENDING capture, and Update() -- every tick, on the
    // main thread -- watches the hands and the quiver. When the pressed item
    // arrives in a hand, what that hand held before the change is the thing
    // to remember. Equips are asynchronous and Wheeler equips before telling
    // us, so this is the one way that works the same for both.
    //
    // One hand only: an equip that displaces two things (a bow replacing a
    // sword and a spell) remembers the right hand, or the left when the right
    // held nothing. Instances are not tracked -- the remembered weapon is a
    // form, and the allocator shows the best-ranked stack of it.
    //
    // THREAD SAFETY: Wheeler callbacks can reach OnSlotActivated off the main
    // thread; everything is under m_mutex.
    // =============================================================================

    class Remembrance
    {
    public:
        enum class Kind : uint8_t
        {
            Hand,  // weapon or spell: watch both hands
            Ammo,  // arrows / bolts: watch the quiver
        };

        struct Entry
        {
            RE::FormID formID = 0;     // what the press took off; 0 = nothing held
            float remainingMs = 0.0f;
            // Re-run the pipeline every tick for this long after the hold
            // starts. The hands are read directly here, but the pipeline's
            // player state and the ammo registry catch up a poll or two later,
            // and until they do the item can still look equipped and be
            // filtered out. A spell-for-spell swap may move no state bucket,
            // so nothing else would ever re-run it.
            float settleMs = 0.0f;
            [[nodiscard]] bool Active() const noexcept { return formID != 0 && remainingMs > 0.0f; }
        };
        using PageEntries = std::array<Entry, MAX_SLOTS_PER_PAGE>;

        static Remembrance& GetSingleton();

        /// A Huginn slot was activated. Before or after the equip lands both
        /// work -- capture watches for the change -- as long as the two are
        /// within kMatchWindowMs of each other.
        /// @return true when the activated item IS the slot's remembered item:
        ///   the hold ends here, nothing new is remembered, and the caller must
        ///   not reward the learner for the press.
        bool OnSlotActivated(size_t page, size_t slot, RE::FormID formID, Kind kind);

        /// Every tick, main thread: sample the hands, resolve pending captures,
        /// age the holds, and end any whose item is back in a hand (re-equipped
        /// some other way: it is no longer what you took off).
        /// @return true when a hold started or ended -- the caller forces a
        ///   pipeline run so the slot changes now, not at the next state change.
        [[nodiscard]] bool Update(float deltaMs, RE::PlayerCharacter* player);

        /// This page's holds, copied out.
        [[nodiscard]] PageEntries GetPage(size_t page) const;

        /// Every form held on any page, so the pipeline can make sure each one
        /// is a candidate even when the ranking filtered it out.
        [[nodiscard]] std::vector<RE::FormID> ActiveFormIDs() const;

        /// Forget everything (save load, `hg reset`, layout reload).
        void Reset();

    private:
        Remembrance() = default;

        struct Track
        {
            RE::FormID current = 0;
            RE::FormID before = 0;          // what `current` replaced
            int64_t changedAtMs = INT64_MIN;
        };

        struct Pending
        {
            size_t page = 0;
            size_t slot = 0;
            RE::FormID formID = 0;
            Kind kind = Kind::Hand;
            int64_t createdAtMs = 0;
        };

        // How far apart the press and the equip it caused may be seen. The
        // hand change can land before the press is reported (Wheeler) or a
        // frame or two after (EquipObject is queued).
        static constexpr int64_t kMatchWindowMs = 1500;
        static constexpr size_t kMaxPending = 4;
        static constexpr float kSettleMs = 1000.0f;

        static void Observe(Track& track, RE::FormID now, int64_t nowMs);
        [[nodiscard]] static bool ChangedTo(const Track& track, RE::FormID formID, int64_t sinceMs);

        mutable std::mutex m_mutex;
        std::array<PageEntries, MAX_PAGES> m_pages{};
        std::array<Pending, kMaxPending> m_pending{};
        size_t m_pendingCount = 0;
        Track m_right, m_left, m_ammo;
        bool m_sampled = false;  // first Update seeds the tracks without a change
    };

}  // namespace Huginn::Slot
