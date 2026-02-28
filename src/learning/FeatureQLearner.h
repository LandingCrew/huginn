#pragma once

#include "StateFeatures.h"
#include <chrono>
#include <unordered_map>
#include <shared_mutex>
#include <array>
#include <functional>

namespace Huginn::Learning
{
   // Batched metrics for feature-based Q-learner (mirrors ItemMetrics shape)
   struct FeatureItemMetrics
   {
      float qValue;      // w . phi(s)
      float ucb;         // Exploration bonus (per-item train count)
      float confidence;  // Sigmoid on per-item train count
   };

   // =============================================================================
   // FEATURE Q-LEARNER (Phase 3.5b-d)
   // =============================================================================
   // Linear function approximation: Q(s, item) = w_item . phi(s)
   // Each item gets its own 18-element weight vector. Learning in one state
   // automatically generalizes to similar states because shared features
   // carry the knowledge.
   //
   // Update rule: w += alpha * (reward - w.phi) * phi - alpha * lambda * w
   //   - Semi-gradient TD(0) with L2 regularization
   //   - Weight clamping prevents unbounded drift
   //
   // Wired into UtilityScorer (Phase 3.5c). Cosave persistence (Phase 3.5d).
   // =============================================================================
   class FeatureQLearner
   {
   public:
      // Core API
      [[nodiscard]] float GetQValue(RE::FormID formID, const StateFeatures& features) const;
      void Update(RE::FormID formID, const StateFeatures& features, float reward);

      // Lazy decay: apply time-based weight decay to items idle > threshold.
      // Returns true if decay was actually applied.
      bool MaybeDecay(RE::FormID formID);

      // Metrics API (3.5d-compatible shape)
      [[nodiscard]] float GetConfidence(RE::FormID formID) const;
      [[nodiscard]] float GetUCB(RE::FormID formID) const;
      [[nodiscard]] FeatureItemMetrics GetMetrics(RE::FormID formID, const StateFeatures& features) const;

      // ── Locked reader for amortized scoring loops ────────────────────
      // Acquires shared_lock once; caller loops N candidates under it.
      // No intermediate vectors, no per-call lock overhead.
      class LockedReader
      {
      public:
         LockedReader(LockedReader&&) = default;

         // Same semantics as GetMetrics, but caller pre-computes phi once
         [[nodiscard]] FeatureItemMetrics GetMetrics(
            RE::FormID formID,
            const std::array<float, StateFeatures::NUM_FEATURES>& phi) const;

      private:
         friend class FeatureQLearner;
         explicit LockedReader(const FeatureQLearner& owner);

         const FeatureQLearner& m_owner;
         std::shared_lock<std::shared_mutex> m_lock;
      };

      [[nodiscard]] LockedReader AcquireReader() const;

      // ── Serialization support (cosave persistence, Phase 3.5d) ─────────

      // Compact entry for serialization — one per item
      struct SerializedEntry {
         RE::FormID formID;
         std::array<float, StateFeatures::NUM_FEATURES> weights;
         uint32_t trainCount;
         uint32_t minutesSinceLastUpdate = 0;  // v2: relative to save time
      };

      // Export all data for cosave save (acquires shared_lock)
      void ExportData(
         std::function<void(SerializedEntry entry)> entryCallback,
         uint32_t& outTotalTrainCount) const;

      // Import data from cosave load (acquires unique_lock, clears existing data first)
      void ImportData(
         const std::vector<SerializedEntry>& entries,
         uint32_t totalTrainCount);

      // Diagnostics
      [[nodiscard]] size_t GetItemCount() const;
      [[nodiscard]] uint32_t GetTotalTrainCount() const;
      [[nodiscard]] uint32_t GetTrainCount(RE::FormID formID) const;
      [[nodiscard]] std::array<float, StateFeatures::NUM_FEATURES> GetWeights(RE::FormID formID) const;
      void Clear();

   private:
      // Shared formula helpers — callers MUST hold m_mutex (ComputeUCB reads m_totalTrainCount)
      [[nodiscard]] float ComputeConfidence(uint32_t trains) const noexcept;
      [[nodiscard]] float ComputeUCB(uint32_t itemTrains) const noexcept;

      std::unordered_map<RE::FormID, std::array<float, StateFeatures::NUM_FEATURES>> m_weights;
      std::unordered_map<RE::FormID, uint32_t> m_trainCount;
      std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> m_lastUpdateTime;
      uint32_t m_totalTrainCount = 0;

      static constexpr float LEARNING_RATE = 0.1f;
      static constexpr float L2_LAMBDA = 0.01f;
      static constexpr float WEIGHT_CLAMP = 10.0f;
      static constexpr float CONFIDENCE_MIDPOINT = 5.0f;    // 50% confidence at 5 trains
      static constexpr float CONFIDENCE_STEEPNESS = 0.3f;   // ~90% confidence at 15 trains
      static constexpr float UCB_NORMALIZATION_FACTOR = 0.2f;

      mutable std::shared_mutex m_mutex;

      [[nodiscard]] static float DotProduct(
         const std::array<float, StateFeatures::NUM_FEATURES>& a,
         const std::array<float, StateFeatures::NUM_FEATURES>& b);
   };
}
