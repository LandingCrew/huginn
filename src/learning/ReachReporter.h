#pragma once

#include "EquipEvent.h"
#include <string_view>

namespace Huginn::Learning
{
    // =========================================================================
    // REACH REPORTER - the [Reach] line and the heartbeat's reach split
    // =========================================================================
    // A reach-in is the objective metric: the player went past Huginn to equip
    // or use something. Two sources, each one line and one count:
    //   - an outside selection that CONFIRMED (SelectionTracker::Confirm, the
    //     same place the heartbeat's reachIns and accept% are counted, so the
    //     lines and reachIns agree). A Huginn-wheel pick whose equip event came
    //     first is relabelled before it can get here.
    //   - enchanted gear put on through a menu (ExternalEquipLearner::
    //     OnArmourEquip). Logged and in the reach split, but NOT in reachIns or
    //     accept%, and never the learner: armour waits for R8 (decision R4).
    // The pure parts (the kind, the gear gate, the line) are core/ReachLog.h.
    // Logging and counting only: nothing here feeds a score.
    // =========================================================================
    namespace ReachReporter
    {
        /// SelectionTracker::Confirm, for an outside selection with an A-E case.
        void OnSelectionReach(const EquipEvent& event);

        /// ExternalEquipLearner::OnArmourEquip: armour Huginn did not equip,
        /// with player input behind it (`via`). Reported when it is a gear
        /// reach (enchanted, through a menu, not during a load).
        void OnArmourPick(RE::FormID formID, std::string_view via, std::string_view caseLabel);

        /// A game load: the gear repeat window is forgotten.
        void Reset();
    }
}
