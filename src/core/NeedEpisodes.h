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
//           [onset, expiry] answers the episode. The confirmation may arrive
//           after the expiry (a potion confirms when its count drops, gear
//           when it is still worn 3 s later), so an ended episode waits
//           kGraceSec (3.5 s, the longest confirm window plus 0.5 s) before
//           it is judged.
//   nothing an episode that ends, lasted kMinSec or more and is unanswered
//           when its grace runs out produces one record.
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
// does not want; the record names the need.
//
// Header-only and templated on the payload each episode carries (the game
// keeps the onset's logged context there; the tests an int).
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
        double graceSec = 3.5;
    };

    template <class Payload>
    class EpisodeTracker
    {
    public:
        struct Episode
        {
            std::size_t need = 0;
            double onsetSec = 0.0;
            double endSec = 0.0;    // valid once ended
            float peak = 0.0f;      // highest v_k inside the episode
            bool answered = false;
            Payload payload{};
        };

        explicit EpisodeTracker(EpisodeParams p = {}) : params_(p) {}

        [[nodiscard]] const EpisodeParams& Params() const noexcept { return params_; }

        /// One tick. Returns the needs whose episode started now; the caller
        /// attaches the onset's payload with SetPayload.
        std::vector<std::size_t> Tick(const NeedArray& v, double nowSec)
        {
            std::vector<std::size_t> onsets;
            for (std::size_t k = 0; k < kNeedCount; ++k) {
                auto& slot = open_[k];
                const float x = v[k];
                if (!slot) {
                    if (x >= params_.onset) {
                        slot = Episode{ k, nowSec, 0.0, x, false, Payload{} };
                        onsets.push_back(k);
                    }
                    continue;
                }
                slot->peak = std::max(slot->peak, x);
                if (x < params_.expiry) {
                    slot->endSec = nowSec;
                    if (slot->endSec - slot->onsetSec >= params_.minSec) {
                        closing_.push_back(std::move(*slot));
                    }
                    else {
                        ++dropped_;
                    }
                    slot.reset();
                }
            }
            return onsets;
        }

        void SetPayload(std::size_t need, Payload p)
        {
            if (need < kNeedCount && open_[need]) open_[need]->payload = std::move(p);
        }

        /// A confirmed selection the player made at `pressSec`: it answers
        /// every open episode that began at or before it, and every ended one
        /// whose [onset, end] holds it.
        void OnSelection(double pressSec)
        {
            for (auto& slot : open_) {
                if (slot && slot->onsetSec <= pressSec) slot->answered = true;
            }
            for (auto& e : closing_) {
                if (e.onsetSec <= pressSec && pressSec <= e.endSec) e.answered = true;
            }
        }

        /// Ended episodes whose grace has run out and that nothing answered.
        /// Each is returned once; answered ones are dropped silently.
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
        /// Episodes dropped for being shorter than minSec, since the last Reset.
        [[nodiscard]] std::size_t DroppedShort() const noexcept { return dropped_; }

        /// Forget everything (a game load): nothing open, nothing pending.
        void Reset()
        {
            for (auto& slot : open_) slot.reset();
            closing_.clear();
            dropped_ = 0;
        }

    private:
        EpisodeParams params_;
        std::array<std::optional<Episode>, kNeedCount> open_{};
        std::vector<Episode> closing_;
        std::size_t dropped_ = 0;
    };
}
