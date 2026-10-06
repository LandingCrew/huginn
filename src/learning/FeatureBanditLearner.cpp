#include "FeatureBanditLearner.h"
#include "Config.h"
#include <cmath>
#include <algorithm>

namespace Huginn::Learning
{
   float FeatureBanditLearner::GetRewardEstimate(RE::FormID formID, const StateFeatures& features) const
   {
      // Compute feature array outside lock (no shared state needed)
      auto phi = features.ToArray();

      std::shared_lock lock(m_mutex);

      auto it = m_items.find(formID);
      if (it == m_items.end()) [[unlikely]] {
         return 0.0f;  // Unknown item → zero reward estimate
      }

      return DotProduct(it->second.weights, phi);
   }

   void FeatureBanditLearner::Update(RE::FormID formID, const StateFeatures& features, float reward,
      float step, bool countsAsTrain)
   {
      // Compute feature array outside lock (pure computation, no shared state)
      auto phi = features.ToArray();

      std::unique_lock lock(m_mutex);

      // Zero-init on first access (operator[] default-constructs the struct)
      auto& data = m_items[formID];
      auto& w = data.weights;

      // Prediction error: delta = reward - R(context, item)
      float prediction = DotProduct(w, phi);
      float error = reward - prediction;

      // Gradient step on the immediate-reward error, with L2 regularization:
      //   w[i] += alpha * error * phi[i] - alpha * lambda * w[i]
      const float rate = LEARNING_RATE * step;
      for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
         w[i] += rate * error * phi[i] - rate * L2_LAMBDA * w[i];
         w[i] = std::clamp(w[i], -WEIGHT_CLAMP, WEIGHT_CLAMP);
      }

      // Update train counts and last-update timestamp. A passed-over item's
      // update moves its weights but is not a train (see the header). The
      // clock ticks either way: the learning changed.
      if (countsAsTrain) {
         data.trainCount++;
         m_totalTrainCount++;
      }
      m_clock++;
      data.lastUpdate = std::chrono::steady_clock::now();

      logger::trace("Learner update: item={:08X}, reward={:.2f}, error={:.3f}, est {:.3f}->{:.3f}"sv,
         formID, reward, error, prediction, DotProduct(w, phi));

      // AFTER the weights are written, never before. The two orderings fail
      // differently and only one of them fails safe:
      //   late set  -> a run already in flight misses it, the flag stays set,
      //                and the next 100 ms tick forces another run. Costs one
      //                extra pass.
      //   early set -> the pipeline can see the flag, CLEAR it, and then score
      //                against the pre-update weights, because this function
      //                has not applied them yet. The reward is then never
      //                published until something unrelated moves the hash --
      //                which is precisely the bug the latch exists to fix.
      // The first version of this had it backwards, with a comment arguing the
      // race away. Raised in review of #122.
      m_weightsChanged.store(true, std::memory_order_release);
   }

   size_t FeatureBanditLearner::MaybeDecayBatch(
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

            logger::debug("Learner decay: item={:08X}, elapsed={:.1f}min, factor={:.4f}"sv,
               formID, elapsedMinutes, decayFactor);
         }
      }

      return decayed;
   }

   // Private helpers — formulas shared by GetConfidence/GetUCB/GetMetrics.
   // Callers MUST hold m_mutex before calling (ComputeUCB reads m_totalTrainCount).

   float FeatureBanditLearner::ComputeConfidence(uint32_t trains) const noexcept
   {
      // The prior as pseudo-observations (roadmap Phase 3 #3, 0.23.0):
      // n / (n + n0). The item's own evidence weighs against n0 imaginary
      // observations at the prior, so with no evidence the learned score IS
      // the prior, and the prior keeps n0's share however much is learned.
      // Replaced a sigmoid (50% at 5 trains, ~95% at 15) that gave the prior
      // nothing to say once an item was trained. tools/replay over the soak
      // run, modelling the game as it ships (repeats on equips only, deferred
      // passed-over updates): n0 = 2 put the chosen item in the top 8 on 74.1%
      // of picks and 29.2% of menu picks, against 71.7% / 27.0% for the sigmoid
      // under the same choice target (n0 1-3 within a point; 5 and 8 worse).
      // Code review of #173 tried the alternatives: lambda back on the sigmoid
      // 71.5% / 28.1%, an effective-sample-size alpha 72.3% / 29.2% -- none
      // clearly better on 502 scored picks (a point is ~5 picks).
      const float n = static_cast<float>(trains);
      return n / (n + PRIOR_PSEUDO_OBSERVATIONS);
   }

   float FeatureBanditLearner::ComputeUCB(uint32_t itemTrains) const noexcept
   {
      if (itemTrains == 0 || m_totalTrainCount == 0) [[unlikely]] {
         return 1.0f;
      }
      float ucb = std::sqrt((2.0f * std::log(static_cast<float>(m_totalTrainCount))) /
                            static_cast<float>(itemTrains));
      return std::clamp(ucb * UCB_NORMALIZATION_FACTOR, 0.0f, 1.0f);
   }

   float FeatureBanditLearner::GetConfidence(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      uint32_t trains = 0;
      if (auto it = m_items.find(formID); it != m_items.end()) {
         trains = it->second.trainCount;
      }
      return ComputeConfidence(trains);
   }

   float FeatureBanditLearner::GetUCB(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      uint32_t itemTrains = 0;
      if (auto it = m_items.find(formID); it != m_items.end()) {
         itemTrains = it->second.trainCount;
      }
      return ComputeUCB(itemTrains);
   }

   FeatureItemMetrics FeatureBanditLearner::GetMetrics(RE::FormID formID, const StateFeatures& features) const
   {
      // Compute feature array outside lock
      auto phi = features.ToArray();

      std::shared_lock lock(m_mutex);

      FeatureItemMetrics metrics{0.0f, 1.0f, 0.0f};  // Defaults: Q=0, UCB=max, confidence=0

      // ONE lookup yields weights + train count (previously two parallel maps)
      uint32_t itemTrains = 0;
      if (auto it = m_items.find(formID); it != m_items.end()) {
         metrics.rewardEstimate = DotProduct(it->second.weights, phi);
         itemTrains = it->second.trainCount;
      }

      metrics.confidence = ComputeConfidence(itemTrains);
      metrics.ucb = ComputeUCB(itemTrains);

      return metrics;
   }

   // ── LockedReader ──────────────────────────────────────────────────
   FeatureBanditLearner::LockedReader::LockedReader(const FeatureBanditLearner& owner)
      : m_owner(owner), m_lock(owner.m_mutex)
   {}

   FeatureItemMetrics FeatureBanditLearner::LockedReader::GetMetrics(
      RE::FormID formID,
      const std::array<float, StateFeatures::NUM_FEATURES>& phi) const
   {
      // Lock already held by m_lock — no acquire needed
      FeatureItemMetrics metrics{0.0f, 1.0f, 0.0f};

      // ONE lookup per candidate (previously two parallel-map finds)
      uint32_t itemTrains = 0;
      if (auto it = m_owner.m_items.find(formID); it != m_owner.m_items.end()) {
         metrics.rewardEstimate = DotProduct(it->second.weights, phi);
         itemTrains = it->second.trainCount;
      }

      metrics.confidence = m_owner.ComputeConfidence(itemTrains);
      metrics.ucb = m_owner.ComputeUCB(itemTrains);

      return metrics;
   }

   FeatureBanditLearner::LockedReader FeatureBanditLearner::AcquireReader() const
   {
      return LockedReader(*this);
   }

   size_t FeatureBanditLearner::GetItemCount() const
   {
      std::shared_lock lock(m_mutex);
      return m_items.size();
   }

   uint32_t FeatureBanditLearner::GetTotalTrainCount() const
   {
      std::shared_lock lock(m_mutex);
      return m_totalTrainCount;
   }

   uint32_t FeatureBanditLearner::GetTrainCount(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);

      auto it = m_items.find(formID);
      if (it == m_items.end()) {
         return 0;
      }
      return it->second.trainCount;
   }

   bool FeatureBanditLearner::HasItem(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      return m_items.contains(formID);
   }

   std::array<float, StateFeatures::NUM_FEATURES> FeatureBanditLearner::GetWeights(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);

      auto it = m_items.find(formID);
      if (it == m_items.end()) {
         return {};  // Zero-initialized array
      }
      return it->second.weights;
   }

   void FeatureBanditLearner::Clear()
   {
      std::unique_lock lock(m_mutex);

      const size_t itemCount = m_items.size();
      const uint32_t totalTrains = m_totalTrainCount;

      m_items.clear();
      m_totalTrainCount = 0;
      m_clock++;

      logger::info("FeatureBanditLearner cleared: {} items, {} total trains removed"sv,
         itemCount, totalTrains);
   }

   uint64_t FeatureBanditLearner::GetClock() const
   {
      std::shared_lock lock(m_mutex);
      return m_clock;
   }

   void FeatureBanditLearner::SetClock(uint64_t clock)
   {
      std::unique_lock lock(m_mutex);
      m_clock = clock;
   }

   size_t FeatureBanditLearner::ReplaceDynamicEntries(const std::vector<SerializedEntry>& saveEntries)
   {
      const auto isDynamic = [](RE::FormID id) { return (id >> 24) == 0xFF; };

      std::unique_lock lock(m_mutex);

      size_t removed = 0;
      for (auto it = m_items.begin(); it != m_items.end();) {
         if (isDynamic(it->first)) {
            m_totalTrainCount -= std::min(m_totalTrainCount, it->second.trainCount);
            it = m_items.erase(it);
            ++removed;
         } else {
            ++it;
         }
      }

      const auto now = std::chrono::steady_clock::now();
      size_t added = 0;
      for (const auto& entry : saveEntries) {
         if (!isDynamic(entry.formID)) continue;
         m_items[entry.formID] = ItemLearningData{
            entry.weights,
            entry.trainCount,
            now - std::chrono::minutes(entry.minutesSinceLastUpdate)};
         m_totalTrainCount += entry.trainCount;
         ++added;
      }

      if (removed || added) {
         logger::info("FeatureBanditLearner: dynamic-form entries replaced from the save ({} removed, {} restored)"sv,
            removed, added);
      }
      return removed;
   }

   void FeatureBanditLearner::ExportData(
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

   void FeatureBanditLearner::ImportData(
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

      logger::info("FeatureBanditLearner imported: {} items, {} total trains"sv,
         m_items.size(), m_totalTrainCount);
   }

   float FeatureBanditLearner::DotProduct(
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
