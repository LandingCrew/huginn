#pragma once

// =============================================================================
// NEED EPISODES (R4) -- when a need starts and stops, for "nothing pressed"
// =============================================================================
// The choice model has three outcomes (doc 9, theory P11): a key press from
// the page, a menu pick from the held items off the page, and nothing. A press
// leaves a record; doing nothing does not. So the selection log writes one
// "nothing" record per NEED EPISODE that ends with no selection made inside
// it, carrying the page and the situation as they stood at the episode's
// onset (the user, 2026-10-08).
//
// An episode, per need k, on the need's curve output v_k in [0, 1]:
//
//   onset   v_k rises to kOnset (0.5) or above while no episode of k is open.
//           0.5 is the midpoint every curve is built around (needs.csv: the
//           logistic centres, a step's 1): "the need is more on than off".
//   expiry  v_k falls below kExpiry (0.25). The gap is hysteresis: a need
//           wobbling around 0.5 (darkness under trees, a vital at its
//           centre) is one episode, not one per wobble. 0.25 is five 0.05
//           signature steps below the onset.
//   length  an episode shorter than kMinSec (1 s) is dropped: no one can
//           react to it, and flicker would flood the log.
//   answer  a CONFIRMED selection (key, wheel or menu; the outcome record of
//           which already carries the need vector) whose press time lies in
//           [onset, expiry + kAnswerSlackSec] answers the episode. The slack
//           (0.5 s, five update ticks) is for a press that itself ends the
//           episode: the equip lands, and an update tick can see the need
//           gone, before the press is stamped (measured in game: a key's
//           scroll ended loadout_archery 2 ms before its selection was
//           taken; Wheeler equips ~6 ms before its callback). The
//           confirmation may arrive after the expiry (a potion confirms when
//           its count drops, gear when it is still worn 3 s later), so an
//           ended episode waits kGraceSec (4 s: the longest confirm window,
//           3 s, plus the slack and a few ticks) before it is judged.
//   nothing an episode that ends, lasted kMinSec or more and is unanswered
//           when its grace runs out produces one record.
//   lap     (0.23.51) an episode that stays open is cut into STRETCHES on a
//           shared grid of lapSec (60 s) of active time, and each stretch is
//           judged as an ended episode is: unanswered when its grace runs
//           out, it produces one record too, marked a lap. Before 0.23.51 a
//           persistent need (hunger, thirst, diseased, cold, warmth_deficit,
//           fatigue) produced none: any selection anywhere in its hours-long
//           episode answered all of it, and a load dropped it still open.
//           The mage's log (2026-10-15..29, ~45 play hours) holds 14,014
//           nothing records and not one for an episode of those six needs.
//           They still sit in nothing contexts (the fit's u0 reads the whole
//           need vector of the record's context, the onset of another need),
//           so the gap is narrower: nothing was sampled where other needs
//           start, never across the time a persistent need stays on. The
//           bias is mild and mixed (on >= 0.5 at picks / at nothing: hunger
//           8.8% / 7.3%, diseased 11.8% / 10.5%, cold 0.2% / 2.2%; the rest
//           in the schema doc, Laps).
//
//           The grid: a monotonic ACTIVE clock adds the time between two
//           ticks, capped at lapMaxStepSec (1 s). The caller ticks only while
//           the game runs unpaused with the player alive, so a pause, a
//           death, a load door or a stall adds at most a second. On a tick
//           the clock also HOLDS (adds nothing; LapHoldFor) while the player
//           cannot or does not press: no input (a key, a button, the mouse, a
//           stick past the game's deadzone) for lapIdleSec (30 s) or more,
//           the movement controls disabled (a carriage ride, a cutscene), a
//           dialogue menu open (it does not pause the game), or (0.23.55)
//           any other open menu that blocks gameplay input without pausing
//           (the vanilla crafting menu; LapHoldInputs.h, which also holds
//           the stick rule). So a lap stands for time in which
//           the player could have pressed something; an AFK spell with the
//           game unpaused adds at most lapIdleSec. Every
//           lapSec of active time is a grid point. At a grid point every open
//           episode whose current stretch began at or before the PREVIOUS
//           grid point laps: its stretch closes (endSec = now) and a new one
//           begins now. So no stretch is shorter than lapSec except an
//           episode's last one (cut by its expiry); an episode's first
//           stretch lasts lapSec to 2 lapSec; an episode that ends within
//           lapSec of its onset is judged exactly as before 0.23.51; and the
//           laps of every need fall on the same ticks, so the game takes ONE
//           context for all of them.
//
//           A lapped stretch is answered by a selection pressed in [start,
//           end] -- no slack: its end is the grid's cut, not something a
//           press did, and a press after it lies in the next stretch. The
//           last stretch of a lapped episode is judged like any ended
//           episode (slack included), and dropped when shorter than kMinSec.
//           Why 60 s, and why active seconds rather than game time:
//           docs/architecture/9-selection-log-v3.md, "Laps".
//
// Time is the caller's clock in seconds (steady). The caller stops ticking
// while the game is paused (a menu open): nothing in the world moves then,
// so an onset or an expiry inside a pause could only come from wall-clock
// decays, or from what the player does in the menu -- which a pick inside the
// still-open episode answers.
//
// Every need takes part, the always-on ones too (loadout_*, downtime, the
// target families): they rarely end unanswered (a weapon swap that ends a
// loadout episode is itself a selection), and the fit can drop any need it
// does not want; the record names the need. Since 0.23.51 they lap like any
// other open episode.
//
// Header-only and templated on the payload each episode carries (the game
// keeps the stretch's logged context there; the tests an int).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "NeedEvaluator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace Huginn::Core::Needs
{
    struct EpisodeParams
    {
        float onset = 0.5f;
        float expiry = 0.25f;
        double minSec = 1.0;
        double graceSec = 4.0;
        double answerSlackSec = 0.5;
        /// 0.23.51: the lap grid in active seconds (0 = no laps: an episode is
        /// judged whole, as before 0.23.51), and the most one tick adds to the
        /// active clock.
        double lapSec = 60.0;
        double lapMaxStepSec = 1.0;
        /// The active clock holds once the player's latest input is this many
        /// seconds old (0 = never holds for idleness). See LapHoldFor.
        double lapIdleSec = 30.0;
    };

    /// What the caller knows on a tick about whether the player could press
    /// something (the lap clock's gate, "lap" above).
    struct LapGate
    {
        double idleSec = 0.0;    ///< seconds since the player's latest input (key, button, mouse, stick)
        bool controls = true;    ///< the movement controls are enabled (off: a carriage, a cutscene)
        bool dialogue = false;   ///< a dialogue menu is open (it does not pause the game)
        bool menu = false;       ///< 0.23.55: an open menu blocks gameplay input without pausing (LapHoldInputs.h MenuBlocksGameplay)
    };

    /// Why the active clock holds on a tick; kNone = it runs.
    enum class LapHold
    {
        kNone,
        kDialogue,
        kMenu,
        kControls,
        kIdle,
    };

    [[nodiscard]] constexpr const char* LapHoldName(LapHold h) noexcept
    {
        switch (h) {
        case LapHold::kDialogue: return "dialogue";
        case LapHold::kMenu: return "menu";
        case LapHold::kControls: return "controls disabled";
        case LapHold::kIdle: return "idle";
        default: return "none";
        }
    }

    /// The gate's verdict: the first reason that applies, in the order
    /// dialogue, menu, controls, idle. A dialogue blocks gameplay input too
    /// (its menu brings its own controls), so it would also count as a menu;
    /// it goes first so its hold keeps the name it had in 0.23.51.
    [[nodiscard]] constexpr LapHold LapHoldFor(const EpisodeParams& p, const LapGate& g) noexcept
    {
        if (g.dialogue) return LapHold::kDialogue;
        if (g.menu) return LapHold::kMenu;
        if (!g.controls) return LapHold::kControls;
        if (p.lapIdleSec > 0.0 && g.idleSec >= p.lapIdleSec) return LapHold::kIdle;
        return LapHold::kNone;
    }

    template <class Payload>
    class EpisodeTracker
    {
    public:
        /// One stretch of an episode: the whole episode when it never lapped.
        struct Episode
        {
            std::size_t need = 0;
            double onsetSec = 0.0;     // the episode's onset
            double startSec = 0.0;     // this stretch's start: the onset, or the lap that began it
            double endSec = 0.0;       // valid once ended: the expiry, or the lap that cut it
            float peak = 0.0f;         // highest v_k inside the stretch
            bool answered = false;
            int lap = 0;               // 0 = the stretch from the onset; n = begun by the episode's n-th lap
            bool byLap = false;        // ended by a lap (the episode still open), not by the expiry
            double startActive = 0.0;  // the active clock at the stretch's start
            Payload payload{};
        };

        explicit EpisodeTracker(EpisodeParams p = {}) : params_(p), nextGrid_(p.lapSec) {}

        [[nodiscard]] const EpisodeParams& Params() const noexcept { return params_; }

        /// One tick. Returns the needs whose episode started now; the caller
        /// attaches the onset's payload with SetPayload. The needs whose
        /// episode lapped on this tick are in Laps(): each began a new stretch
        /// that carries a COPY of the previous stretch's payload until the
        /// caller replaces it (MutablePayload, SetPayload). `gate` says
        /// whether the player could press on this tick: when LapHoldFor holds,
        /// the active clock adds nothing (onsets and expiries go on as usual).
        std::vector<std::size_t> Tick(const NeedArray& v, double nowSec, const LapGate& gate = {})
        {
            hold_ = LapHoldFor(params_, gate);
            if (hasLastTick_ && hold_ == LapHold::kNone) {
                const double step = nowSec - lastTickSec_;
                if (step > 0.0) active_ += std::min(step, params_.lapMaxStepSec);
            }
            hasLastTick_ = true;
            lastTickSec_ = nowSec;
            laps_.clear();

            std::vector<std::size_t> onsets;
            for (std::size_t k = 0; k < kNeedCount; ++k) {
                auto& slot = open_[k];
                const float x = v[k];
                if (!slot) {
                    if (x >= params_.onset) {
                        Episode e;
                        e.need = k;
                        e.onsetSec = nowSec;
                        e.startSec = nowSec;
                        e.peak = x;
                        e.startActive = active_;
                        slot = std::move(e);
                        onsets.push_back(k);
                    }
                    continue;
                }
                slot->peak = std::max(slot->peak, x);
                if (x < params_.expiry) {
                    slot->endSec = nowSec;
                    if (slot->endSec - slot->startSec >= params_.minSec) {
                        closing_.push_back(std::move(*slot));
                    }
                    else {
                        ++dropped_;
                    }
                    slot.reset();
                }
            }

            if (params_.lapSec > 0.0 && active_ >= nextGrid_) {
                const double previousGrid = lastGrid_;
                for (std::size_t k = 0; k < kNeedCount; ++k) {
                    auto& slot = open_[k];
                    if (!slot || slot->startActive > previousGrid) continue;
                    Episode next;
                    next.need = k;
                    next.onsetSec = slot->onsetSec;
                    next.startSec = nowSec;
                    next.peak = v[k];
                    next.lap = slot->lap + 1;
                    next.startActive = active_;
                    next.payload = slot->payload;
                    slot->endSec = nowSec;
                    slot->byLap = true;
                    closing_.push_back(std::move(*slot));
                    slot = std::move(next);
                    laps_.push_back(k);
                }
                lastGrid_ = active_;
                nextGrid_ = active_ + params_.lapSec;
            }
            return onsets;
        }

        /// The needs whose episode lapped on the latest Tick, ascending.
        [[nodiscard]] const std::vector<std::size_t>& Laps() const noexcept { return laps_; }

        void SetPayload(std::size_t need, Payload p)
        {
            if (need < kNeedCount && open_[need]) open_[need]->payload = std::move(p);
        }

        /// The open stretch's payload (to update after a lap), or nullptr
        /// when the need has no open episode.
        [[nodiscard]] Payload* MutablePayload(std::size_t need) noexcept
        {
            return need < kNeedCount && open_[need] ? &open_[need]->payload : nullptr;
        }

        /// A confirmed selection the player made at `pressSec`: it answers
        /// every open stretch that began at or before it, every ended one
        /// whose [start, end + answerSlackSec] holds it, and every lapped one
        /// whose [start, end] holds it.
        void OnSelection(double pressSec)
        {
            for (auto& slot : open_) {
                if (slot && slot->startSec <= pressSec) slot->answered = true;
            }
            for (auto& e : closing_) {
                const double slack = e.byLap ? 0.0 : params_.answerSlackSec;
                if (e.startSec <= pressSec && pressSec <= e.endSec + slack) e.answered = true;
            }
        }

        /// Ended episodes and lapped stretches whose grace has run out and
        /// that nothing answered. Each is returned once; answered ones are
        /// dropped silently.
        std::vector<Episode> TakeUnanswered(double nowSec)
        {
            std::vector<Episode> out;
            auto it = closing_.begin();
            while (it != closing_.end()) {
                if (nowSec - it->endSec >= params_.graceSec) {
                    if (!it->answered) out.push_back(std::move(*it));
                    it = closing_.erase(it);
                }
                else {
                    ++it;
                }
            }
            return out;
        }

        /// The needs with an open episode right now, ascending.
        [[nodiscard]] std::vector<std::size_t> OpenNeeds() const
        {
            std::vector<std::size_t> out;
            for (std::size_t k = 0; k < kNeedCount; ++k) {
                if (open_[k]) out.push_back(k);
            }
            return out;
        }

        [[nodiscard]] bool IsOpen(std::size_t need) const noexcept { return need < kNeedCount && open_[need].has_value(); }
        [[nodiscard]] std::size_t ClosingCount() const noexcept { return closing_.size(); }
        /// Episodes (or last stretches) dropped for being shorter than minSec,
        /// since the last Reset.
        [[nodiscard]] std::size_t DroppedShort() const noexcept { return dropped_; }
        /// The active clock, seconds (see "lap" above).
        [[nodiscard]] double ActiveSec() const noexcept { return active_; }
        /// Why the active clock held on the latest Tick (kNone: it ran).
        [[nodiscard]] LapHold Hold() const noexcept { return hold_; }

        /// The player died (0.23.16): drop every open episode and every ended
        /// one still in its grace, as a load does, and say how many. A death
        /// is not a decision: an episode cut short by it ("falling" into the
        /// ground, a loadout the corpse no longer holds) is not "nothing
        /// pressed", and the reload that follows starts a new session anyway.
        /// The short-episode count and the active clock are kept (unlike
        /// Reset); lapped stretches in their grace are dropped too.
        std::size_t Abandon()
        {
            std::size_t n = closing_.size();
            for (auto& slot : open_) {
                if (slot) ++n;
                slot.reset();
            }
            closing_.clear();
            laps_.clear();
            return n;
        }

        /// Forget everything (a game load): nothing open, nothing pending, the
        /// active clock and the lap grid back at 0.
        void Reset()
        {
            for (auto& slot : open_) slot.reset();
            closing_.clear();
            dropped_ = 0;
            laps_.clear();
            active_ = 0.0;
            lastGrid_ = 0.0;
            nextGrid_ = params_.lapSec;
            hasLastTick_ = false;
            lastTickSec_ = 0.0;
            hold_ = LapHold::kNone;
        }

    private:
        EpisodeParams params_;
        std::array<std::optional<Episode>, kNeedCount> open_{};
        std::vector<Episode> closing_;
        std::size_t dropped_ = 0;
        std::vector<std::size_t> laps_;  // the latest Tick's
        double active_ = 0.0;            // the active clock
        double lastGrid_ = 0.0;          // the active clock at the latest grid point
        double nextGrid_ = 0.0;          // set from params_.lapSec
        bool hasLastTick_ = false;
        double lastTickSec_ = 0.0;
        LapHold hold_ = LapHold::kNone;  // the latest Tick's
    };
}
