#pragma once

// =============================================================================
// HAND MODEL CHECK -- is the weapon in a hand actually on the skeleton (0.23.44)
// =============================================================================
// A diagnostic, not a sensor. 2026-10-17: a staff picked from the Huginn wheel
// was equipped -- the game reported it in the right hand for the next minute
// -- but the player saw bare fists. The user sees the same "equipped,
// invisible" weapon now and then, sometimes right after a load. The log could
// not say whether the model was missing, so this judges it.
//
// The game layer (state/HandModelProbe.cpp) reads, for the weapon in a hand,
// the biped part the engine attached for it (BipedAnim::objects[i].partClone
// with item == the weapon) and reduces it to a PartReading. Judged by pose:
//
//   - no part, or a part not attached to any node: a fault in any pose;
//   - drawn: the part must sit under a hand node (WEAPON or SHIELD) and not be
//     culled. A culled or sheath-hung part while drawn is a fault;
//   - sheathed: culled or off the hand is normal (sheathed is not in the hand,
//     and Immersive Equipment Displays may cull the vanilla sheath model);
//   - drawing or sheathing: not judged. The part moves between nodes partway
//     through the animation.
//
// FaultWatch turns those judgements into transitions for the log: a fault is
// reported once it has held for kOnsetSec on the same weapon (a model still
// loading after a swap or a load is not one), then once more when it ends --
// the model back, or the weapon swapped out. A draw or sheathe in between
// restarts an unreported fault and leaves a reported one standing.
//
// Seconds on one monotonic clock (the caller's); nothing here reads a clock.
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
        Moving,  // drawing or sheathing: not judged
    };

    /// What the game layer found for the weapon in one hand.
    struct PartReading
    {
        bool found = false;       // a biped part with a model for this weapon
        bool attached = false;    // the part has a parent node
        bool culled = false;      // the part itself is app-culled
        bool inHandNode = false;  // a WEAPON or SHIELD node is among its parents
    };

    enum class ModelState : std::uint8_t
    {
        Ok,
        NoPart,     // no biped part carries a model for the weapon
        Detached,   // a part, attached to nothing
        Culled,     // drawn, but the part is culled
        NotInHand,  // drawn, but the part hangs off a non-hand node (a sheath)
    };

    [[nodiscard]] constexpr std::string_view ModelStateName(ModelState s) noexcept
    {
        switch (s) {
        case ModelState::Ok: return "ok";
        case ModelState::NoPart: return "missing (no biped part)";
        case ModelState::Detached: return "detached (part has no parent)";
        case ModelState::Culled: return "culled while drawn";
        case ModelState::NotInHand: return "not in the hand while drawn";
        }
        return "?";
    }

    /// Lower is better: picks the best of several parts for one weapon (the
    /// same form can show in more than one biped slot).
    [[nodiscard]] constexpr int ModelStateRank(ModelState s) noexcept { return static_cast<int>(s); }

    /// Judge one reading. Not called for HandPose::Moving.
    [[nodiscard]] constexpr ModelState JudgePart(const PartReading& r, HandPose pose) noexcept
    {
        if (!r.found) return ModelState::NoPart;
        if (!r.attached) return ModelState::Detached;
        if (pose == HandPose::Drawn) {
            if (r.culled) return ModelState::Culled;
            if (!r.inHandNode) return ModelState::NotInHand;
        }
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
            Recovered,  // a reported fault judged ok again
            SwappedOut, // a reported fault's weapon left the hand
        };

        struct Event
        {
            EventKind kind = EventKind::None;
            ModelState state = ModelState::Ok;  // Onset: the fault; Recovered/SwappedOut: the fault that ended
            double faultSec = 0.0;              // Recovered/SwappedOut: since the fault was first seen
            int poseChanges = 0;                // Recovered/SwappedOut: pose changes since onset
        };

        FaultWatch() = default;
        explicit FaultWatch(double onsetSec) noexcept : m_onsetSec(onsetSec) {}

        /// One judgement. `formID` 0 = no modelled weapon in the hand (ends
        /// any fault). `state` is ignored when `pose` is Moving.
        Event Update(std::uint32_t formID, HandPose pose, ModelState state, double nowSec) noexcept
        {
            Event ev;
            if (formID != m_formID) {
                if (m_reported) {
                    ev = { EventKind::SwappedOut, m_state, nowSec - m_since, m_poseChanges };
                }
                ClearFault();
                m_formID = formID;
                m_pose = pose;
                if (formID == 0 || pose == HandPose::Moving) return ev;
                StartOrContinue(state, nowSec);
                return ev;
            }
            if (formID == 0) return ev;

            if (pose != m_pose) {
                m_pose = pose;
                if (m_reported) ++m_poseChanges;
                else ClearFault();  // an unreported fault restarts on any pose change
            }
            if (pose == HandPose::Moving) return ev;

            if (state == ModelState::Ok) {
                if (m_reported) {
                    ev = { EventKind::Recovered, m_state, nowSec - m_since, m_poseChanges };
                }
                ClearFault();
                return ev;
            }
            return StartOrContinue(state, nowSec);
        }

        [[nodiscard]] bool Reported() const noexcept { return m_reported; }
        [[nodiscard]] bool Pending() const noexcept { return m_faulted && !m_reported; }
        [[nodiscard]] ModelState State() const noexcept { return m_faulted ? m_state : ModelState::Ok; }

        /// A game load: nothing carries across.
        void Reset() noexcept
        {
            ClearFault();
            m_formID = 0;
            m_pose = HandPose::Sheathed;
        }

    private:
        Event StartOrContinue(ModelState state, double nowSec) noexcept
        {
            Event ev;
            if (state == ModelState::Ok) return ev;
            if (!m_faulted) {
                m_faulted = true;
                m_since = nowSec;
                m_state = state;
                return ev;
            }
            // A different fault on the same weapon keeps the clock running;
            // the reported state is the latest one seen before the report.
            if (!m_reported) {
                m_state = state;
                if (nowSec - m_since >= m_onsetSec) {
                    m_reported = true;
                    m_poseChanges = 0;
                    ev = { EventKind::Onset, m_state, nowSec - m_since, 0 };
                }
            }
            return ev;
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
        bool m_faulted = false;
        bool m_reported = false;
        ModelState m_state = ModelState::Ok;
        double m_since = 0.0;
        int m_poseChanges = 0;
    };
}
