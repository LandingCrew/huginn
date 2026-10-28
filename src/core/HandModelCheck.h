#pragma once

// =============================================================================
// HAND MODEL CHECK -- is the drawn weapon actually on the skeleton (0.23.44)
// =============================================================================
// A diagnostic, not a sensor. 2026-10-17: a staff picked from the Huginn wheel
// was equipped -- the game reported it in the right hand for the next minute
// -- but the player saw bare fists. The user sees the same "equipped,
// invisible" weapon now and then, sometimes right after a load. The log could
// not say whether the model was missing, so this judges it.
//
// The game layer (state/HandModelProbe.cpp) reads, for the weapon in a hand,
// the biped part the engine attached for it (BipedAnim::objects[i].partClone
// with item == the weapon) in the biped of the current view, and reduces it to
// a PartReading. Judged only while the weapon is drawn -- the complaint is a
// weapon missing from the hand, and sheathed parts vary by mod (Simple Dual
// Sheath, IED) and by hand (vanilla shows no sheathed left-hand weapon):
//
//   - no part, a part attached to nothing, or a part whose parents do not
//     reach the player's 3D root: a fault;
//   - a part culled, or under a culled parent below the 3D root: a fault;
//   - a part not under its own hand's node -- still on the sheath, or on the
//     other hand's node: a fault. WEAPON is the right hand's, SHIELD the
//     left's; a two-hander, bow or staff takes either (HandNode below).
//
// When the other hand holds the same form, or a variant sharing its
// first-person model object, a part could be either hand's: it is sorted by
// the hand node it hangs under (PartIsHands), not dropped and not counted
// for both.
//
// FaultWatch turns those judgements into transitions for the log: a fault is
// reported once it has held for kOnsetSec on the same weapon in the same view
// (a model still loading after a swap or a load is not one), then once more
// when it ends -- the same check passing again (drawn, in the view that
// raised it), or the weapon swapped out. A sheathe, a draw, a view change or
// a gap in the world (Restart: a door, fast travel) restarts an unreported
// fault and leaves a reported one standing: it is not judged again until the
// weapon is drawn in that view.
//
// Seconds on one monotonic clock (the caller's; the game layer uses the game
// clock, which stops while paused); nothing here reads a clock.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>
#include <string_view>

namespace Huginn::Core
{
    enum class HandPose : std::uint8_t
    {
        Sheathed,
        Drawn,
        Moving,  // drawing or sheathing
    };

    /// What the game layer found for the weapon in one hand, in one biped.
    struct PartReading
    {
        bool found = false;       // a biped part with a model for this weapon
        bool attached = false;    // the part has a parent node
        bool onSkeleton = false;  // its parents reach the player's 3D root
        bool culled = false;      // the part, or a parent below the root, is app-culled
        bool inHandNode = false;  // its own hand's node is among its parents (InOwnHandNode)
    };

    // The two nodes a drawn weapon hangs from, by name in both skeletons:
    // WEAPON in the right hand, SHIELD in the left (a shield, a left-hand
    // weapon, and a drawn bow, which the engine reports in the right hand).
    enum class HandNode : std::uint8_t
    {
        None,    // neither among the part's parents
        Weapon,
        Shield,
    };

    /// Whether a part whose nearest hand node is `found` is in its hand.
    /// `own` is the hand's node, or None for either: a two-hander, bow or
    /// staff keeps the rule from before the sides were told apart (a drawn
    /// bow hangs from SHIELD). A one-hander counts only under its own
    /// hand's node -- a left-hand dagger on WEAPON is not in its hand.
    [[nodiscard]] constexpr bool InOwnHandNode(HandNode found, HandNode own) noexcept
    {
        return found != HandNode::None && (own == HandNode::None || found == own);
    }

    /// Whether a part matched to a hand's weapon is that hand's. Only an
    /// ambiguous match is sorted -- the other hand holds the same form, or a
    /// variant sharing its first-person model object: a part under the other
    /// hand's node is the other hand's. One under neither node stays: it
    /// cannot be Ok (InOwnHandNode), so at worst it names a fault for a hand
    /// that has no part of its own, and never hides one.
    [[nodiscard]] constexpr bool PartIsHands(HandNode found, HandNode own, bool ambiguous) noexcept
    {
        return !ambiguous || found == HandNode::None || found == own;
    }

    enum class ModelState : std::uint8_t
    {
        Ok,
        NotInHand,  // the part hangs off a sheath, or the other hand's node
        Culled,     // the part or a parent is culled
        Orphaned,   // attached, but not under the player's 3D
        Detached,   // a part, attached to nothing
        NoPart,     // no biped part carries a model for the weapon
    };

    [[nodiscard]] constexpr std::string_view ModelStateName(ModelState s) noexcept
    {
        switch (s) {
        case ModelState::Ok: return "ok";
        case ModelState::NotInHand: return "not in its hand (on a sheath node, or the other hand's)";
        case ModelState::Culled: return "culled (the part or a parent)";
        case ModelState::Orphaned: return "orphaned (its parents never reach the player's 3D)";
        case ModelState::Detached: return "detached (part has no parent)";
        case ModelState::NoPart: return "missing (no biped part)";
        }
        return "?";
    }

    /// Lower is better: picks the best of several parts for one weapon (the
    /// same form can show in more than one biped slot).
    [[nodiscard]] constexpr int ModelStateRank(ModelState s) noexcept { return static_cast<int>(s); }

    /// Judge one reading of a drawn weapon.
    [[nodiscard]] constexpr ModelState JudgePart(const PartReading& r) noexcept
    {
        if (!r.found) return ModelState::NoPart;
        if (!r.attached) return ModelState::Detached;
        if (!r.onSkeleton) return ModelState::Orphaned;
        if (r.culled) return ModelState::Culled;
        if (!r.inHandNode) return ModelState::NotInHand;
        return ModelState::Ok;
    }

    class FaultWatch
    {
    public:
        static constexpr double kOnsetSec = 2.0;

        enum class EventKind : std::uint8_t
        {
            None,
            Onset,      // a fault held kOnsetSec: report it
            Recovered,  // a reported fault's check passed again
            SwappedOut, // a reported fault's weapon left the hand
        };

        struct Event
        {
            EventKind kind = EventKind::None;
            ModelState state = ModelState::Ok;  // Onset: the fault; Recovered/SwappedOut: the fault that ended
            double faultSec = 0.0;              // Recovered/SwappedOut: since the fault was first seen
            int poseChanges = 0;                // Recovered/SwappedOut: sheathes (drawn -> not drawn) since onset
        };

        FaultWatch() = default;
        explicit FaultWatch(double onsetSec) noexcept : m_onsetSec(onsetSec) {}

        /// One judgement. `formID` 0 = no modelled weapon in the hand (ends
        /// any fault). `view` is which biped was read (the caller's id: first
        /// or third person). `state` is read only when `pose` is Drawn.
        Event Update(std::uint32_t formID, HandPose pose, std::uint8_t view, ModelState state, double nowSec) noexcept
        {
            Event ev;
            if (formID != m_formID) {
                if (m_reported) {
                    ev = { EventKind::SwappedOut, m_state, nowSec - m_since, m_poseChanges };
                }
                ClearFault();
                m_formID = formID;
                m_pose = pose;
                if (formID != 0 && pose == HandPose::Drawn) Start(state, view, nowSec);
                return ev;
            }
            if (formID == 0) return ev;

            if (pose != m_pose) {
                if (m_reported) {
                    // Sheathes only: at 250 ms sampling the Moving phase can be missed.
                    if (m_pose == HandPose::Drawn) ++m_poseChanges;
                } else {
                    ClearFault();  // an unreported fault restarts on any pose change
                }
                m_pose = pose;
            }
            if (pose != HandPose::Drawn) return ev;

            if (m_reported) {
                // Only the check that raised it may end it.
                if (view != m_view || state != ModelState::Ok) return ev;
                ev = { EventKind::Recovered, m_state, nowSec - m_since, m_poseChanges };
                ClearFault();
                return ev;
            }
            if (m_faulted && view != m_view) ClearFault();  // a view change restarts it
            if (state == ModelState::Ok) {
                ClearFault();
                return ev;
            }
            if (!m_faulted) {
                Start(state, view, nowSec);
                return ev;
            }
            // A different fault on the same weapon keeps the clock running;
            // the reported state is the latest one seen before the report.
            m_state = state;
            if (nowSec - m_since >= m_onsetSec) {
                m_reported = true;
                m_poseChanges = 0;
                ev = { EventKind::Onset, m_state, nowSec - m_since, 0 };
            }
            return ev;
        }

        [[nodiscard]] bool Reported() const noexcept { return m_reported; }
        [[nodiscard]] bool Pending() const noexcept { return m_faulted && !m_reported; }
        /// The weapon the last Update was for (0 = none).
        [[nodiscard]] std::uint32_t FormID() const noexcept { return m_formID; }

        /// A gap in the world with no load (a door, fast travel, the player's
        /// 3D gone a while): the clock jumped, so an unreported fault starts
        /// over and must hold kOnsetSec again after it. A reported fault
        /// stands, as across a sheathe: it was real, and its end line is
        /// still owed -- the next drawn check in its view, or a swap.
        void Restart() noexcept
        {
            if (!m_reported) ClearFault();
        }

        /// A game load: nothing carries across.
        void Reset() noexcept
        {
            ClearFault();
            m_formID = 0;
            m_pose = HandPose::Sheathed;
        }

    private:
        void Start(ModelState state, std::uint8_t view, double nowSec) noexcept
        {
            if (state == ModelState::Ok) return;
            m_faulted = true;
            m_since = nowSec;
            m_state = state;
            m_view = view;
        }

        void ClearFault() noexcept
        {
            m_faulted = false;
            m_reported = false;
            m_poseChanges = 0;
        }

        double m_onsetSec = kOnsetSec;
        std::uint32_t m_formID = 0;
        HandPose m_pose = HandPose::Sheathed;
        std::uint8_t m_view = 0;
        bool m_faulted = false;
        bool m_reported = false;
        ModelState m_state = ModelState::Ok;
        double m_since = 0.0;
        int m_poseChanges = 0;
    };

    // -------------------------------------------------------------------------
    // A draw or sheathe that never finishes. 2026-10-17, the second sighting:
    // `hg hands` 5.5 s after a staff (a two-hander) replaced Firebolt while
    // drawn read "drawing/sheathing", the staff still on WeaponBack. The game
    // never finished the draw, so the model never moved to the hand -- and
    // FaultWatch never judged it, because a moving pose is not judged. A draw
    // takes about a second; kStuckSec away from a settled state is reported,
    // then the return to one.
    //
    // Timed from the last settled state, not from the last change: a draw
    // that flaps between the moving states (want-to-draw, drawing, back)
    // without settling is as stuck as one that sits in one of them, and
    // restarting on every change never reported it. The report names the
    // exact state it was in at kStuckSec.
    //
    // `state` is the caller's weapon-state id (the engine's enum); 0 is
    // sheathed and 3 drawn, the two settled states.
    // -------------------------------------------------------------------------
    class PoseStuckWatch
    {
    public:
        static constexpr double kStuckSec = 3.0;
        static constexpr std::uint8_t kSheathed = 0;
        static constexpr std::uint8_t kDrawn = 3;

        enum class EventKind : std::uint8_t
        {
            None,
            Stuck,    // kStuckSec without a settled state; `state` is the one it is in
            Settled,  // a reported stuck stretch ended; `state` is the settled one it ended in
        };

        struct Event
        {
            EventKind kind = EventKind::None;
            std::uint8_t state = 0;      // Stuck: the state at the report; Settled: the settled one
            std::uint8_t stuckIn = 0;    // Settled: the state the Stuck report named
            double heldSec = 0.0;        // since the last settled state (or since the watch started)
        };

        PoseStuckWatch() = default;
        explicit PoseStuckWatch(double stuckSec) noexcept : m_stuckSec(stuckSec) {}

        Event Update(std::uint8_t state, double nowSec) noexcept
        {
            Event ev;
            const bool settled = IsSettled(state);
            if (!m_started) {
                m_started = true;
                m_state = state;
                m_stateSince = nowSec;
                m_unsettledSince = nowSec;  // a moving state already under way: timed from here
                m_reported = false;
                return ev;
            }
            if (state != m_state) {
                // Leaving a settled state starts the clock; a change between
                // two moving states leaves it running.
                if (IsSettled(m_state)) m_unsettledSince = nowSec;
                m_state = state;
                m_stateSince = nowSec;
                if (settled) {
                    if (m_reported) ev = { EventKind::Settled, state, m_stuckIn, nowSec - m_unsettledSince };
                    m_reported = false;
                    return ev;
                }
            }
            if (!m_reported && !settled && nowSec - m_unsettledSince >= m_stuckSec) {
                m_reported = true;
                m_stuckIn = state;
                ev = { EventKind::Stuck, state, state, nowSec - m_unsettledSince };
            }
            return ev;
        }

        [[nodiscard]] bool Reported() const noexcept { return m_reported; }
        /// How long the current (exact) state has held.
        [[nodiscard]] double HeldSec(double nowSec) const noexcept { return m_started ? nowSec - m_stateSince : 0.0; }

        /// A gap in the world with no load (a door, fast travel): the clock
        /// jumped, so an unreported moving stretch is timed again from the
        /// next Update. A reported one stands until a settled state ends it.
        void Restart() noexcept
        {
            if (!m_reported) m_started = false;
        }

        void Reset() noexcept
        {
            m_started = false;
            m_reported = false;
        }

    private:
        [[nodiscard]] static constexpr bool IsSettled(std::uint8_t s) noexcept { return s == kSheathed || s == kDrawn; }

        double m_stuckSec = kStuckSec;
        bool m_started = false;
        bool m_reported = false;
        std::uint8_t m_state = 0;
        std::uint8_t m_stuckIn = 0;
        double m_stateSince = 0.0;
        double m_unsettledSince = 0.0;  // when the last settled state was left
    };
}

