#pragma once

#include "StateFeatures.h"
#include <chrono>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <array>
#include <functional>
#include <vector>
#include <algorithm>

namespace Huginn::Learning
{
   // Batched metrics for feature-based bandit learner (mirrors ItemMetrics shape)
   struct FeatureItemMetrics
   {
      float rewardEstimate;      // w . phi(s)
      float ucb;         // Exploration bonus (per-item evidence, n_eff)
      float confidence;  // n_eff / (n_eff + PRIOR_PSEUDO_OBSERVATIONS), n_eff = trains x retention
   };

   // Memory with a useful life (roadmap Phase 3 #3a, 0.23.6). What the player
   // stops choosing keeps full strength for a while, then fades fast and is
   // forgotten -- a battery's discharge curve, not an exponential:
   //
   //   retention(t) = (1 + e^(-T/s)) / (1 + e^((t - T)/s))   t = PLAY hours since last chosen
   //   T            = lifeHours + lifePerPickHours * ln(1 + n)
   //   n_eff        = n * retention                         confidence = n_eff / (n_eff + n0)
   //   forgotten when retention < forgetBelow               (entry deleted)
   //
   // The numerator makes retention exactly 1 at t = 0. What fades is the
   // confidence, not the weights: as n_eff falls the learned score slides back
   // to the PRIOR ("don't know any more"), not to 0 ("rejected"), and the UCB
   // rises, so a forgotten item is explored again. Replaces the 2%/hour weight
   // decay. `enabled` false: nothing fades and nothing is forgotten.
   struct MemoryLife
   {
      bool enabled = true;
      float lifeHours = 8.0f;          // T0: full strength this long after a pick
      float lifePerPickHours = 2.0f;   // k: each doubling of the picks adds ~1.4 k
      float fadeHours = 1.0f;          // s: the width of the knee
      float forgetBelow = 0.05f;       // retention under this deletes the entry

      // The one copy of the allowed ranges (the INI loader and the learner
      // both clamp through this).
      [[nodiscard]] MemoryLife Clamped() const noexcept
      {
         MemoryLife c = *this;
         c.lifeHours = std::clamp(lifeHours, 0.1f, 10000.0f);
         c.lifePerPickHours = std::clamp(lifePerPickHours, 0.0f, 1000.0f);
         c.fadeHours = std::clamp(fadeHours, 0.05f, 1000.0f);
         c.forgetBelow = std::clamp(forgetBelow, 0.0f, 0.5f);
         return c;
      }
   };

   // =============================================================================
   // FEATURE BANDIT LEARNER (Phase 3.5b-d)
   // =============================================================================
   // A linear contextual bandit: each item is an arm, the 18-float feature
   // vector is the context, and the target is the reward observed for that one
   // decision. No gamma, no successor state, no trajectory — the update never
   // bootstraps off a future estimate.
   //
   // Linear function approximation: R(context, item) = w_item . phi(context)
   // Each item gets its own 18-element weight vector. Learning in one state
   // automatically generalizes to similar states because shared features
   // carry the knowledge.
   //
   // Update rule: w += alpha * (reward - w.phi) * phi - alpha * lambda * w
   //   - Least-squares step on immediate reward, with L2 regularization.
   //   - Weight clamping prevents unbounded drift
   //
   // Wired into UtilityScorer (Phase 3.5c). Cosave persistence (Phase 3.5d).
   // =============================================================================
   class FeatureBanditLearner
   {
   public:
      // Core API
      [[nodiscard]] float GetRewardEstimate(RE::FormID formID, const StateFeatures& features) const;
      // `step` scales the gradient step; `countsAsTrain` false leaves the train
      // counts (confidence, UCB) alone. Both are for the choice target's
      // passed-over items: a quarter step, and no claim of evidence (0.23.0).
      // An update with `countsAsTrain` false never creates an entry: an item
      // the learner has not seen has estimate 0 against a target of 0.
      void Update(RE::FormID formID, const StateFeatures& features, float reward,
         float step = 1.0f, bool countsAsTrain = true);

      // ── Memory with a useful life (see MemoryLife above) ──────────────
      void SetMemoryLife(const MemoryLife& life);

      // The play clock: seconds of unpaused, loaded play since launch,
      // advanced by the update loop. Time with the game closed, paused or in
      // a load screen does not count -- a month away forgets nothing. One
      // clock for the process; each entry stores the clock at its last pick,
      // and the cosave carries "play minutes since" across loads.
      void AdvancePlayTime(float seconds) noexcept;
      [[nodiscard]] double GetPlaySeconds() const noexcept;

      // Delete every entry whose retention fell under forgetBelow. One
      // shared-lock pass finds them; the unique lock is taken only when one
      // is due. Returns the number forgotten. MaybeForgetFaded runs it at
      // most once a minute of play, from the scoring pass.
      size_t ForgetFaded();
      size_t MaybeForgetFaded();
      [[nodiscard]] uint64_t GetForgottenTotal() const noexcept {
         return m_forgottenTotal.load(std::memory_order_relaxed);
      }

      /// Retention of the item's evidence now (1 for an unknown item).
      [[nodiscard]] float GetRetention(RE::FormID formID) const;

      // Metrics API (3.5d-compatible shape)
      [[nodiscard]] float GetConfidence(RE::FormID formID) const;
      /// n0 in confidence = n / (n + n0): the prior's weight in observations.
      [[nodiscard]] static constexpr float PriorPseudoObservations() noexcept { return PRIOR_PSEUDO_OBSERVATIONS; }
      [[nodiscard]] float GetUCB(RE::FormID formID) const;

      // ── Locked reader for amortized scoring loops ────────────────────
      // Acquires shared_lock once; caller loops N candidates under it.
      // No intermediate vectors, no per-call lock overhead.
      class LockedReader
      {
      public:
         LockedReader(LockedReader&&) = default;

         // Estimate, UCB and confidence (n_eff) for one item; caller pre-computes phi once
         [[nodiscard]] FeatureItemMetrics GetMetrics(
            RE::FormID formID,
            const std::array<float, StateFeatures::NUM_FEATURES>& phi) const;

      private:
         friend class FeatureBanditLearner;
         explicit LockedReader(const FeatureBanditLearner& owner);

         const FeatureBanditLearner& m_owner;
         std::shared_lock<std::shared_mutex> m_lock;
         double m_now;   // the play clock, read once for the whole pass
      };

      [[nodiscard]] LockedReader AcquireReader() const;

      // ── Serialization support (cosave persistence, Phase 3.5d) ─────────

      // Compact entry for serialization — one per item
      struct SerializedEntry {
         RE::FormID formID;
         std::array<float, StateFeatures::NUM_FEATURES> weights;
         // v4: the evidence n, fractional -- a pick after a fade restarts from
         // n_eff + 1. (v3 held an integer count here; converted on load.)
         float trainCount;
         // v4: PLAY minutes since the item was last chosen, at save time.
         uint32_t minutesSinceChosen = 0;
      };

      // Export all data for cosave save (acquires shared_lock)
      void ExportData(
         const std::function<void(SerializedEntry entry)>& entryCallback,
         uint32_t& outTotalTrainCount) const;

      // Import data from cosave load (acquires unique_lock, clears existing data first)
      // The total is summed from the entries, not taken from the header.
      void ImportData(const std::vector<SerializedEntry>& entries);

      // ── Learning clock (learning survives a reload, 0.22.11) ───────────
      // Ticks on every Update() and every Clear(); saved with the character ID
      // (cosave HCID record). A load of the same character keeps the in-memory
      // learner only when its clock is not behind the save's -- so a reload
      // after a death keeps the fight, and loading a LATER save of the same
      // character restores that save's learning instead of discarding it. A
      // Clear() (hg reset weights) ticks it too, so a reload cannot undo a
      // reset with an older save.
      [[nodiscard]] uint64_t GetClock() const;
      void SetClock(uint64_t clock);

      // Same-character reload: replace every DYNAMIC-form entry (0xFF------:
      // brewed potions, player enchantments, created spells) with the save's.
      // The engine can hand those IDs to different forms after a reload, so
      // in-memory training on them may describe an item that no longer exists.
      // Static forms keep their in-memory learning. Returns entries removed.
      size_t ReplaceDynamicEntries(const std::vector<SerializedEntry>& saveEntries);

      // ── Pipeline wake-up latch ────────────────────────────────────────
      //
      // Set by Update(), read and cleared by PipelineCoordinator. A reward
      // changes what the top-N ranking WOULD be, but changes no game state, so
      // GameState::GetHash() cannot see it and CheckHashSkip skips the tick
      // that would have published it. Observed 2026-09-19: a hotkey equip
      // rewarded 00012EB7 at 20:24:25.459 and the new ranking did not reach the
      // widget until 20:24:32.185 -- 6.7 s later, and only because an unrelated
      // ally flap happened to move the hash. In a still scene nothing would
      // have moved it at all.
      //
      // Update() ONLY. Not MaybeForgetFaded, which runs from inside the scoring
      // loop (a forced run would re-enter it), and fading is gradual over
      // hours -- the next run publishes it. Not ImportData or Clear either -- the load
      // and reset paths already force a pass through
      // PipelineCoordinator::ResetCrossSaveState().
      //
      // Atomic because the equip event that calls Update() can arrive on the
      // game thread while the update thread is reading this.
      [[nodiscard]] bool WeightsChanged() const noexcept {
         return m_weightsChanged.load(std::memory_order_acquire);
      }
      void ClearWeightsChanged() noexcept {
         m_weightsChanged.store(false, std::memory_order_release);
      }

      // Diagnostics
      [[nodiscard]] size_t GetItemCount() const;
      /// Sum of the stored evidence n over every entry, rounded.
      [[nodiscard]] uint32_t GetTotalTrainCount() const;
      /// The stored evidence n (before fading; times GetRetention for n_eff).
      [[nodiscard]] float GetTrainCount(RE::FormID formID) const;
      /// True if the learner holds an entry for the item (chosen at least
      /// once and not forgotten since; a passed-over update never creates one).
      [[nodiscard]] bool HasItem(RE::FormID formID) const;
      [[nodiscard]] std::array<float, StateFeatures::NUM_FEATURES> GetWeights(RE::FormID formID) const;
      void Clear();

   private:
      // Shared formula helpers — callers MUST hold m_mutex (ComputeUCB reads m_totalTrains)
      [[nodiscard]] float ComputeConfidence(float effectiveTrains) const noexcept;
      [[nodiscard]] float ComputeUCB(float effectiveTrains) const noexcept;

      // Per-item learning state, colocated in one map: one hash lookup per
      // candidate instead of three parallel-map lookups (weights, trainCount,
      // last-update time previously lived in separate unordered_maps).
      // NOTE: the cosave format is unaffected — serialization goes exclusively
      // through SerializedEntry in ExportData/ImportData.
      struct ItemLearningData
      {
         std::array<float, StateFeatures::NUM_FEATURES> weights{};
         float trainCount = 0.0f;   // the evidence n
         double chosenAt = 0.0;     // play clock at the last pick (at creation until one)
      };

      // Retention of an entry at play time `now` -- callers MUST hold m_mutex.
      [[nodiscard]] float RetentionAt(const ItemLearningData& data, double now) const noexcept;
      // n_eff = n x retention: the evidence confidence and UCB see. Same lock rule.
      [[nodiscard]] float EffectiveTrains(const ItemLearningData& data, double now) const noexcept
      {
         return data.trainCount * RetentionAt(data, now);
      }
      // Play-clock reads for the locked paths.
      [[nodiscard]] double PlayNow() const noexcept {
         return m_playSeconds.load(std::memory_order_relaxed);
      }

      std::unordered_map<RE::FormID, ItemLearningData> m_items;
      float m_totalTrains = 0.0f;   // sum of n over m_items
      uint64_t m_clock = 0;   // Learning clock, see GetClock -- under m_mutex
      MemoryLife m_life;      // under m_mutex

      // One writer (the update loop); read anywhere, so atomic, not locked.
      std::atomic<double> m_playSeconds{ 0.0 };
      std::atomic<double> m_lastForgetSweep{ 0.0 };
      std::atomic<uint64_t> m_forgottenTotal{ 0 };
      static constexpr double FORGET_SWEEP_INTERVAL_SEC = 60.0;

      static constexpr float LEARNING_RATE = 0.1f;
      static constexpr float L2_LAMBDA = 0.01f;
      static constexpr float WEIGHT_CLAMP = 10.0f;
      // Confidence = n / (n + n0): 1 train 33%, 2 -> 50%, 6 -> 75%, 18 -> 90%.
      static constexpr float PRIOR_PSEUDO_OBSERVATIONS = 2.0f;
      static_assert(PRIOR_PSEUDO_OBSERVATIONS > 0.0f, "n0 = 0 makes an untrained item's confidence 0/0");
      static constexpr float UCB_NORMALIZATION_FACTOR = 0.2f;

      mutable std::shared_mutex m_mutex;

      // See WeightsChanged() above. Not guarded by m_mutex on purpose: readers
      // are on the update thread's skip check, which must not block behind a
      // scoring pass holding the shared lock.
      std::atomic<bool> m_weightsChanged{ false };

      [[nodiscard]] static float DotProduct(
         const std::array<float, StateFeatures::NUM_FEATURES>& a,
         const std::array<float, StateFeatures::NUM_FEATURES>& b);
   };
}
