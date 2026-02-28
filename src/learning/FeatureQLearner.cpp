#include "FeatureQLearner.h"
#include "Config.h"
#include <cmath>
#include <algorithm>

namespace Huginn::Learning
{
   float FeatureQLearner::GetQValue(RE::FormID formID, const StateFeatures& features) const
   {
      // Compute feature array outside lock (no shared state needed)
      auto phi = features.ToArray();

      std::shared_lock lock(m_mutex);

      auto it = m_weights.find(formID);
      if (it == m_weights.end()) [[unlikely]] {
         return 0.0f;  // Unknown item → zero Q-value
      }

      return DotProduct(it->second, phi);
   }

   void FeatureQLearner::Update(RE::FormID formID, const StateFeatures& features, float reward)
   {
      // Compute feature array outside lock (pure computation, no shared state)
      auto phi = features.ToArray();

      std::unique_lock lock(m_mutex);

      // Zero-init on first access (operator[] default-constructs the array)
      auto& w = m_weights[formID];

      // Prediction error: delta = reward - Q(s, item)
      float prediction = DotProduct(w, phi);
      float error = reward - prediction;

      // Semi-gradient update with L2 regularization:
      //   w[i] += alpha * error * phi[i] - alpha * lambda * w[i]
      for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
         w[i] += LEARNING_RATE * error * phi[i] - LEARNING_RATE * L2_LAMBDA * w[i];
         w[i] = std::clamp(w[i], -WEIGHT_CLAMP, WEIGHT_CLAMP);
      }

      // Update train counts and last-update timestamp
      m_trainCount[formID]++;
      m_totalTrainCount++;
      m_lastUpdateTime[formID] = std::chrono::steady_clock::now();

      logger::trace("FQL update: item={:08X}, reward={:.2f}, error={:.3f}, Q {:.3f}->{:.3f}"sv,
         formID, reward, error, prediction, DotProduct(w, phi));
   }

   bool FeatureQLearner::MaybeDecay(RE::FormID formID)
   {
      auto now = std::chrono::steady_clock::now();
      float elapsedMinutes = 0.0f;

      // Phase 1: shared_lock — check if decay is needed
      {
         std::shared_lock lock(m_mutex);

         auto timeIt = m_lastUpdateTime.find(formID);
         if (timeIt == m_lastUpdateTime.end()) {
            return false;  // No timestamp → never trained, nothing to decay
         }

         auto weightIt = m_weights.find(formID);
         if (weightIt == m_weights.end()) {
            return false;  // No weights → nothing to decay
         }

         elapsedMinutes = std::chrono::duration<float, std::ratio<60>>(
            now - timeIt->second).count();

         if (elapsedMinutes < Config::DECAY_THRESHOLD_MINUTES) {
            return false;  // Recently updated — skip
         }
      }

      // Phase 2: unique_lock — apply decay
      {
         std::unique_lock lock(m_mutex);

         // Re-check under unique_lock (another thread may have updated)
         auto timeIt = m_lastUpdateTime.find(formID);
         if (timeIt == m_lastUpdateTime.end()) {
            return false;
         }

         elapsedMinutes = std::chrono::duration<float, std::ratio<60>>(
            now - timeIt->second).count();

         if (elapsedMinutes < Config::DECAY_THRESHOLD_MINUTES) {
            return false;  // Became fresh between lock upgrade
         }

         auto weightIt = m_weights.find(formID);
         if (weightIt == m_weights.end()) {
            return false;
         }

         float elapsedHours = elapsedMinutes / 60.0f;
         float decayFactor = std::pow(1.0f - Config::DECAY_RATE_PER_HOUR, elapsedHours);

         auto& w = weightIt->second;
         for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
            w[i] *= decayFactor;
         }

         // Stamp to now so we don't re-decay on the next scoring pass
         timeIt->second = now;

         logger::debug("FQL decay: item={:08X}, elapsed={:.1f}min, factor={:.4f}"sv,
            formID, elapsedMinutes, decayFactor);

         return true;
      }
   }

   // Private helpers — formulas shared by GetConfidence/GetUCB/GetMetrics.
   // Callers MUST hold m_mutex before calling (ComputeUCB reads m_totalTrainCount).

   float FeatureQLearner::ComputeConfidence(uint32_t trains) const noexcept
   {
      // Sigmoid: 1 / (1 + exp(-steepness * (x - midpoint)))
      // 50% at 5 trains, ~90% at 15 trains
      float x = static_cast<float>(trains);
      return 1.0f / (1.0f + std::exp(-CONFIDENCE_STEEPNESS * (x - CONFIDENCE_MIDPOINT)));
   }

   float FeatureQLearner::ComputeUCB(uint32_t itemTrains) const noexcept
   {
      if (itemTrains == 0 || m_totalTrainCount == 0) [[unlikely]] {
         return 1.0f;
      }
      float ucb = std::sqrt((2.0f * std::log(static_cast<float>(m_totalTrainCount))) /
                            static_cast<float>(itemTrains));
      return std::clamp(ucb * UCB_NORMALIZATION_FACTOR, 0.0f, 1.0f);
   }

   float FeatureQLearner::GetConfidence(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      uint32_t trains = 0;
      if (auto it = m_trainCount.find(formID); it != m_trainCount.end()) {
         trains = it->second;
      }
      return ComputeConfidence(trains);
   }

   float FeatureQLearner::GetUCB(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      uint32_t itemTrains = 0;
      if (auto it = m_trainCount.find(formID); it != m_trainCount.end()) {
         itemTrains = it->second;
      }
      return ComputeUCB(itemTrains);
   }

   FeatureItemMetrics FeatureQLearner::GetMetrics(RE::FormID formID, const StateFeatures& features) const
   {
      // Compute feature array outside lock
      auto phi = features.ToArray();

      std::shared_lock lock(m_mutex);

      FeatureItemMetrics metrics{0.0f, 1.0f, 0.0f};  // Defaults: Q=0, UCB=max, confidence=0

      // Q-value
      if (auto it = m_weights.find(formID); it != m_weights.end()) {
         metrics.qValue = DotProduct(it->second, phi);
      }

      // Train count for this item
      uint32_t itemTrains = 0;
      if (auto it = m_trainCount.find(formID); it != m_trainCount.end()) {
         itemTrains = it->second;
      }

      metrics.confidence = ComputeConfidence(itemTrains);
      metrics.ucb = ComputeUCB(itemTrains);

      return metrics;
   }

   // ── LockedReader ──────────────────────────────────────────────────
   FeatureQLearner::LockedReader::LockedReader(const FeatureQLearner& owner)
      : m_owner(owner), m_lock(owner.m_mutex)
   {}

   FeatureItemMetrics FeatureQLearner::LockedReader::GetMetrics(
      RE::FormID formID,
      const std::array<float, StateFeatures::NUM_FEATURES>& phi) const
   {
      // Lock already held by m_lock — no acquire needed
      FeatureItemMetrics metrics{0.0f, 1.0f, 0.0f};

      if (auto it = m_owner.m_weights.find(formID); it != m_owner.m_weights.end()) {
         metrics.qValue = DotProduct(it->second, phi);
      }

      uint32_t itemTrains = 0;
      if (auto it = m_owner.m_trainCount.find(formID); it != m_owner.m_trainCount.end()) {
         itemTrains = it->second;
      }

      metrics.confidence = m_owner.ComputeConfidence(itemTrains);
      metrics.ucb = m_owner.ComputeUCB(itemTrains);

      return metrics;
   }

   FeatureQLearner::LockedReader FeatureQLearner::AcquireReader() const
   {
      return LockedReader(*this);
   }

   size_t FeatureQLearner::GetItemCount() const
   {
      std::shared_lock lock(m_mutex);
      return m_weights.size();
   }

   uint32_t FeatureQLearner::GetTotalTrainCount() const
   {
      std::shared_lock lock(m_mutex);
      return m_totalTrainCount;
   }

   uint32_t FeatureQLearner::GetTrainCount(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);

      auto it = m_trainCount.find(formID);
      if (it == m_trainCount.end()) {
         return 0;
      }
      return it->second;
   }

   std::array<float, StateFeatures::NUM_FEATURES> FeatureQLearner::GetWeights(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);

      auto it = m_weights.find(formID);
      if (it == m_weights.end()) {
         return {};  // Zero-initialized array
      }
      return it->second;
   }

   void FeatureQLearner::Clear()
   {
      std::unique_lock lock(m_mutex);

      const size_t itemCount = m_weights.size();
      const uint32_t totalTrains = m_totalTrainCount;

      m_weights.clear();
      m_trainCount.clear();
      m_lastUpdateTime.clear();
      m_totalTrainCount = 0;

      logger::info("FeatureQLearner cleared: {} items, {} total trains removed"sv,
         itemCount, totalTrains);
   }

   void FeatureQLearner::ExportData(
      std::function<void(SerializedEntry entry)> entryCallback,
      uint32_t& outTotalTrainCount) const
   {
      std::shared_lock lock(m_mutex);

      outTotalTrainCount = m_totalTrainCount;
      auto now = std::chrono::steady_clock::now();

      for (const auto& [formID, weights] : m_weights) {
         SerializedEntry entry;
         entry.formID = formID;
         entry.weights = weights;

         auto it = m_trainCount.find(formID);
         entry.trainCount = (it != m_trainCount.end()) ? it->second : 0;

         // v2: compute minutes since last update for decay persistence
         auto timeIt = m_lastUpdateTime.find(formID);
         if (timeIt != m_lastUpdateTime.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
               now - timeIt->second).count();
            entry.minutesSinceLastUpdate = (elapsed > 0)
               ? static_cast<uint32_t>(elapsed) : 0;
         } else {
            entry.minutesSinceLastUpdate = 0;  // No timestamp → treat as fresh
         }

         entryCallback(std::move(entry));
      }
   }

   void FeatureQLearner::ImportData(
      const std::vector<SerializedEntry>& entries,
      uint32_t totalTrainCount)
   {
      std::unique_lock lock(m_mutex);

      m_weights.clear();
      m_trainCount.clear();
      m_lastUpdateTime.clear();
      m_totalTrainCount = totalTrainCount;

      m_weights.reserve(entries.size());
      m_trainCount.reserve(entries.size());
      m_lastUpdateTime.reserve(entries.size());

      auto now = std::chrono::steady_clock::now();

      for (const auto& entry : entries) {
         m_weights[entry.formID] = entry.weights;
         if (entry.trainCount > 0) {
            m_trainCount[entry.formID] = entry.trainCount;
         }
         // Reconstruct last-update timestamp from saved minutes-ago offset
         m_lastUpdateTime[entry.formID] = now -
            std::chrono::minutes(entry.minutesSinceLastUpdate);
      }

      logger::info("FeatureQLearner imported: {} items, {} total trains"sv,
         m_weights.size(), m_totalTrainCount);
   }

   float FeatureQLearner::DotProduct(
      const std::array<float, StateFeatures::NUM_FEATURES>& a,
      const std::array<float, StateFeatures::NUM_FEATURES>& b)
   {
      float sum = 0.0f;
      for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
         sum += a[i] * b[i];
      }
      return sum;
   }
}
