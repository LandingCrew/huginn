#pragma once

#include "EquipEvent.h"

namespace Huginn::Learning
{
    // =========================================================================
    // SELECTION LOG - what the pipeline thought when the player chose
    // =========================================================================
    // Logging only; changes nothing. Written at each CONFIRMED selection
    // (SelectionTracker), from the state and pipeline run captured when the
    // player chose, so it is the data the learning rework (roadmap Phase 3)
    // is sized and tested on. Two outputs:
    //
    // 1. The debug log -- a readable header and the page:
    //   [Selection] Confirmed 0003EADE 'Potion of Healing' src=Hotkey via=key s3
    //               kind=consume reward=+5.0 (consumed, 1320ms) rank=3 util=1.23
    //               ctx=0.80 pred=6.20 need=HealingAny over=0003EAE3 '...' shown=7
    //               page=0 age=40ms gen=2
    //   [Selection]   s2 * 0003EADE 'Potion of Healing' need=HealingAny rank=3 ...
    //   [Selection]   s0 = 0003EAE3 'Potion of Minor Healing' need=HealingAny ...
    //   [Selection]   s1   00012EB7 'Iron Sword' [O] need=WeaponsMelee rank=1 ...
    // `*` is the chosen item, `=` an item shown for the same need -- the
    // would-be negatives -- and `over` the best-ranked of those. [O] [W] [R]
    // mark an override, wildcard or Remembrance slot. `pred` is the learner's
    // estimate for the press-time state before this selection's update.
    // "Need" is the primary slot class (SlotClassifier::Classify), which pairs
    // a healing spell with a health potion. Coarse on purpose; the JSONL
    // record carries every candidate's class, so read-time grouping can differ.
    //
    // 2. Huginn_Selections.jsonl (SKSE log folder), one JSON object per
    //    selection: the press-time feature vector phi, the wildcard odds, the
    //    page with slot index and assignment type, and EVERY scored candidate
    //    with its full score breakdown -- the input for an offline harness
    //    that re-ranks logged selections under a new formula, so a Phase 3
    //    change costs minutes rather than play-hours. Each record names its
    //    candidate columns ("cols"), so the format describes itself. Appended,
    //    never truncated: one file spans sessions, `gen` (the load generation)
    //    separates loads within one game launch -- what a death-and-reload
    //    abandoned -- and `utc` separates launches. Written by a background
    //    thread; the game thread only takes the predictions and queues it.
    // =========================================================================
    namespace SelectionLog
    {
        /// Call immediately BEFORE dispatching `event`, so `pred` is the estimate
        /// the selection is about to correct. `how` = what confirmed it.
        void Write(const EquipEvent& event, const char* how);
    }
}
