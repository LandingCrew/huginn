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

      auto it = m_items.find(formID);
      if (it == m_items.end()) [[unlikely]] {
         return 0.0f;  // Unknown item → zero Q-value
      }

      return DotProduct(it->second.weights, phi);
   }

   void FeatureQLearner::Update(RE::FormID formID, const StateFeatures& features, float reward)
   {
      // Compute feature array outside lock (pure computation, no shared state)
      auto phi = features.ToArray();

      std::unique_lock lock(m_mutex);

      // Zero-init on first access (operator[] default-constructs the struct)
      auto& data = m_items[formID];
      auto& w = data.weights;

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
      data.trainCount++;
      m_totalTrainCount++;
      data.lastUpdate = std::chrono::steady_clock::now();

      logger::trace("FQL update: item={:08X}, reward={:.2f}, error={:.3f}, Q {:.3f}->{:.3f}"sv,
         formID, reward, error, prediction, DotProduct(w, phi));
   }

   size_t FeatureQLearner::MaybeDecayBatch(
      const std::vector<RE::FormID>& formIDs,
      std::chrono::steady_clock::time_point now)
   {
      // Phase 1: ONE shared_lock — collect items whose idle time crosses the
      // decay threshold. Most ticks collect nothing and never take the
      // unique lock at all.
      std::vector<RE::FormID> needsDecay;
      {
         std::shared_lock lock(m_mutex);

         for (RE::FormID formID : formIDs) {
            auto it = m_items.find(formID);
            if (it == m_items.end()) {
               continue;  // Never trained, nothing to decay
            }

            const float elapsedMinutes = std::chrono::duration<float, std::ratio<60>>(
               now - it->second.lastUpdate).count();

            if (elapsedMinutes >= Config::DECAY_THRESHOLD_MINUTES) {
               needsDecay.push_back(formID);
            }
         }
      }

      if (needsDecay.empty()) {
         return 0;
      }

      // Phase 2: ONE unique_lock — re-check and apply (an Update from the
      // game thread may have refreshed an item between the locks).
      size_t decayed = 0;
      {
         std::unique_lock lock(m_mutex);

         for (RE::FormID formID : needsDecay) {
            auto it = m_items.find(formID);
            if (it == m_items.end()) {
               continue;
            }

            auto& data = it->second;
            const float elapsedMinutes = std::chrono::duration<float, std::ratio<60>>(
               now - data.lastUpdate).count();

            if (elapsedMinutes < Config::DECAY_THRESHOLD_MINUTES) {
               continue;  // Became fresh between the locks
            }

            const float elapsedHours = elapsedMinutes / 60.0f;
            const float decayFactor = std::pow(1.0f - Config::DECAY_RATE_PER_HOUR, elapsedHours);

            for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
               data.weights[i] *= decayFactor;
            }

            // Stamp to now so we don't re-decay on the next scoring pass
            // (also feeds minutesSinceLastUpdate in cosave export)
            data.lastUpdate = now;
            ++decayed;

            logger::debug("FQL decay: item={:08X}, elapsed={:.1f}min, factor={:.4f}"sv,
               formID, elapsedMinutes, decayFactor);
         }
      }

      return decayed;
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
      if (auto it = m_items.find(formID); it != m_items.end()) {
         trains = it->second.trainCount;
      }
      return ComputeConfidence(trains);
   }

   float FeatureQLearner::GetUCB(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      uint32_t itemTrains = 0;
      if (auto it = m_items.find(formID); it != m_items.end()) {
         itemTrains = it->second.trainCount;
      }
      return ComputeUCB(itemTrains);
   }

   FeatureItemMetrics FeatureQLearner::GetMetrics(RE::FormID formID, const StateFeatures& features) const
   {
      // Compute feature array outside lock
      auto phi = features.ToArray();

      std::shared_lock lock(m_mutex);

      FeatureItemMetrics metrics{0.0f, 1.0f, 0.0f};  // Defaults: Q=0, UCB=max, confidence=0

      // ONE lookup yields weights + train count (previously two parallel maps)
      uint32_t itemTrains = 0;
      if (auto it = m_items.find(formID); it != m_items.end()) {
         metrics.qValue = DotProduct(it->second.weights, phi);
         itemTrains = it->second.trainCount;
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

      // ONE lookup per candidate (previously two parallel-map finds)
      uint32_t itemTrains = 0;
      if (auto it = m_owner.m_items.find(formID); it != m_owner.m_items.end()) {
         metrics.qValue = DotProduct(it->second.weights, phi);
         itemTrains = it->second.trainCount;
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
      return m_items.size();
   }

   uint32_t FeatureQLearner::GetTotalTrainCount() const
   {
      std::shared_lock lock(m_mutex);
      return m_totalTrainCount;
   }

   uint32_t FeatureQLearner::GetTrainCount(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);

      auto it = m_items.find(formID);
      if (it == m_items.end()) {
         return 0;
      }
      return it->second.trainCount;
   }

   std::array<float, StateFeatures::NUM_FEATURES> FeatureQLearner::GetWeights(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);

      auto it = m_items.find(formID);
      if (it == m_items.end()) {
         return {};  // Zero-initialized array
      }
      return it->second.weights;
   }

   void FeatureQLearner::Clear()
   {
      std::unique_lock lock(m_mutex);

      const size_t itemCount = m_items.size();
      const uint32_t totalTrains = m_totalTrainCount;

      m_items.clear();
      m_totalTrainCount = 0;

      logger::info("FeatureQLearner cleared: {} items, {} total trains removed"sv,
         itemCount, totalTrains);
   }

   void FeatureQLearner::ExportData(
      const std::function<void(SerializedEntry entry)>& entryCallback,
      uint32_t& outTotalTrainCount) const
   {
      std::shared_lock lock(m_mutex);

      outTotalTrainCount = m_totalTrainCount;
      auto now = std::chrono::steady_clock::now();

      for (const auto& [formID, data] : m_items) {
         SerializedEntry entry;
         entry.formID = formID;
         entry.weights = data.weights;
         entry.trainCount = data.trainCount;

         // v2: compute minutes since last update for decay persistence
         auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            now - data.lastUpdate).count();
         entry.minutesSinceLastUpdate = (elapsed > 0)
            ? static_cast<uint32_t>(elapsed) : 0;

         entryCallback(std::move(entry));
      }
   }

   void FeatureQLearner::ImportData(
      const std::vector<SerializedEntry>& entries,
      uint32_t totalTrainCount)
   {
      std::unique_lock lock(m_mutex);

      m_items.clear();
      m_totalTrainCount = totalTrainCount;
      m_items.reserve(entries.size());

      auto now = std::chrono::steady_clock::now();

      for (const auto& entry : entries) {
         // Reconstruct last-update timestamp from saved minutes-ago offset
         m_items[entry.formID] = ItemLearningData{
            entry.weights,
            entry.trainCount,
            now - std::chrono::minutes(entry.minutesSinceLastUpdate)};
      }

      logger::info("FeatureQLearner imported: {} items, {} total trains"sv,
         m_items.size(), m_totalTrainCount);
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
