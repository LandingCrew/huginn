#include "FeatureBanditLearner.h"
#include "Config.h"
#include <cmath>
#include <algorithm>
#include <format>

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
      const double now = PlayNow();

      // Zero-init on first access; a new entry's life is counted from now.
      auto [slot, created] = m_items.try_emplace(formID);
      auto& data = slot->second;
      if (created) {
         data.chosenAt = now;
      }
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

      // A pick restarts the item's life from the evidence it has LEFT: n_eff
      // + 1, so a sword abandoned past the knee and taken up again does not
      // get its old confidence back at once (roadmap Phase 3 #3a). A
      // passed-over item's update moves its weights but is not a train (see
      // the header) and does not renew its life. The learning clock ticks
      // either way: the learning changed.
      if (countsAsTrain) {
         const float before = data.trainCount;
         data.trainCount = before * RetentionAt(data, now) + 1.0f;
         m_totalTrains += data.trainCount - before;
         data.chosenAt = now;
      }
      m_clock++;

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

   void FeatureBanditLearner::SetMemoryLife(const MemoryLife& life)
   {
      MemoryLife clamped = life;
      clamped.lifeHours = std::clamp(life.lifeHours, 0.1f, 10000.0f);
      clamped.lifePerPickHours = std::clamp(life.lifePerPickHours, 0.0f, 1000.0f);
      clamped.fadeHours = std::clamp(life.fadeHours, 0.05f, 1000.0f);
      clamped.forgetBelow = std::clamp(life.forgetBelow, 0.0f, 0.5f);
      std::unique_lock lock(m_mutex);
      m_life = clamped;
   }

   void FeatureBanditLearner::AdvancePlayTime(float seconds) noexcept
   {
      if (seconds > 0.0f) {
         // One writer: a load and a store, not a read-modify-write.
         m_playSeconds.store(PlayNow() + seconds, std::memory_order_relaxed);
      }
   }

   double FeatureBanditLearner::GetPlaySeconds() const noexcept
   {
      return PlayNow();
   }

   float FeatureBanditLearner::RetentionAt(const ItemLearningData& data, double now) const noexcept
   {
      if (!m_life.enabled) {
         return 1.0f;
      }
      // Hours, in double: the clock runs for days.
      const double idle = std::max(0.0, now - data.chosenAt) / 3600.0;
      const double life = m_life.lifeHours + m_life.lifePerPickHours * std::log1p(std::max(0.0f, data.trainCount));
      const double s = m_life.fadeHours;
      const double x = (idle - life) / s;
      if (x > 60.0) {
         return 0.0f;
      }
      return static_cast<float>(std::min(1.0, (1.0 + std::exp(-life / s)) / (1.0 + std::exp(x))));
   }

   size_t FeatureBanditLearner::MaybeForgetFaded()
   {
      const double now = PlayNow();
      if (now - m_lastForgetSweep.load(std::memory_order_relaxed) < FORGET_SWEEP_INTERVAL_SEC) {
         return 0;
      }
      m_lastForgetSweep.store(now, std::memory_order_relaxed);
      return ForgetFaded();
   }

   size_t FeatureBanditLearner::ForgetFaded()
   {
      const double now = PlayNow();

      // Phase 1: ONE shared lock -- most sweeps find nothing and stop here.
      std::vector<RE::FormID> due;
      {
         std::shared_lock lock(m_mutex);
         if (!m_life.enabled) {
            return 0;
         }
         for (const auto& [formID, data] : m_items) {
            if (RetentionAt(data, now) < m_life.forgetBelow) {
               due.push_back(formID);
            }
         }
      }
      if (due.empty()) {
         return 0;
      }

      // Phase 2: re-check under the unique lock (a pick from the game thread
      // may have renewed one between the locks), then delete.
      size_t forgotten = 0;
      std::string named;
      {
         std::unique_lock lock(m_mutex);
         for (RE::FormID formID : due) {
            auto it = m_items.find(formID);
            if (it == m_items.end() || RetentionAt(it->second, now) >= m_life.forgetBelow) {
               continue;
            }
            if (forgotten < 6) {
               named += std::format("{}{:08X} (n={:.1f}, idle {:.1f}h)", forgotten ? ", " : "", formID,
                  it->second.trainCount, (now - it->second.chosenAt) / 3600.0);
            }
            m_totalTrains = std::max(0.0f, m_totalTrains - it->second.trainCount);
            m_items.erase(it);
            ++forgotten;
         }
         if (m_items.empty()) {
            m_totalTrains = 0.0f;   // no float residue once nothing is left
         }
      }

      if (forgotten) {
         m_forgottenTotal.fetch_add(forgotten, std::memory_order_relaxed);
         logger::info("[Learner] Forgot {} item(s) past their useful life: {}{}"sv,
            forgotten, named, forgotten > 6 ? ", ..." : "");
      }
      return forgotten;
   }

   float FeatureBanditLearner::GetRetention(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      auto it = m_items.find(formID);
      return it == m_items.end() ? 1.0f : RetentionAt(it->second, PlayNow());
   }

   // Private helpers — formulas shared by GetConfidence/GetUCB/LockedReader::GetMetrics.
   // Callers MUST hold m_mutex before calling (ComputeUCB reads m_totalTrains).

   float FeatureBanditLearner::ComputeConfidence(float effectiveTrains) const noexcept
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
      // Since 0.23.6 n is the evidence LEFT after fading (n_eff).
      const float n = std::max(0.0f, effectiveTrains);
      return n / (n + PRIOR_PSEUDO_OBSERVATIONS);
   }

   float FeatureBanditLearner::ComputeUCB(float effectiveTrains) const noexcept
   {
      if (effectiveTrains <= 0.0f || m_totalTrains <= 0.0f) [[unlikely]] {
         return 1.0f;
      }
      // ln of at least 1: a fractional total under 1 would make it negative.
      float ucb = std::sqrt((2.0f * std::log(std::max(1.0f, m_totalTrains))) / effectiveTrains);
      return std::clamp(ucb * UCB_NORMALIZATION_FACTOR, 0.0f, 1.0f);
   }

   float FeatureBanditLearner::GetConfidence(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      float trains = 0.0f;
      if (auto it = m_items.find(formID); it != m_items.end()) {
         trains = it->second.trainCount * RetentionAt(it->second, PlayNow());
      }
      return ComputeConfidence(trains);
   }

   float FeatureBanditLearner::GetUCB(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);
      float itemTrains = 0.0f;
      if (auto it = m_items.find(formID); it != m_items.end()) {
         itemTrains = it->second.trainCount * RetentionAt(it->second, PlayNow());
      }
      return ComputeUCB(itemTrains);
   }

   // ── LockedReader ──────────────────────────────────────────────────
   FeatureBanditLearner::LockedReader::LockedReader(const FeatureBanditLearner& owner)
      : m_owner(owner), m_lock(owner.m_mutex), m_now(owner.PlayNow())
   {}

   FeatureItemMetrics FeatureBanditLearner::LockedReader::GetMetrics(
      RE::FormID formID,
      const std::array<float, StateFeatures::NUM_FEATURES>& phi) const
   {
      // Lock already held by m_lock — no acquire needed
      FeatureItemMetrics metrics{0.0f, 1.0f, 0.0f};

      // ONE lookup per candidate (previously two parallel-map finds)
      float itemTrains = 0.0f;
      if (auto it = m_owner.m_items.find(formID); it != m_owner.m_items.end()) {
         metrics.rewardEstimate = DotProduct(it->second.weights, phi);
         itemTrains = it->second.trainCount * m_owner.RetentionAt(it->second, m_now);
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
      return static_cast<uint32_t>(std::lround(std::max(0.0f, m_totalTrains)));
   }

   float FeatureBanditLearner::GetTrainCount(RE::FormID formID) const
   {
      std::shared_lock lock(m_mutex);

      auto it = m_items.find(formID);
      if (it == m_items.end()) {
         return 0.0f;
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
      const float totalTrains = m_totalTrains;

      m_items.clear();
      m_totalTrains = 0.0f;
      m_clock++;

      logger::info("FeatureBanditLearner cleared: {} items, {:.0f} total trains removed"sv,
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
            m_totalTrains = std::max(0.0f, m_totalTrains - it->second.trainCount);
            it = m_items.erase(it);
            ++removed;
         } else {
            ++it;
         }
      }

      const double now = PlayNow();
      size_t added = 0;
      for (const auto& entry : saveEntries) {
         if (!isDynamic(entry.formID)) continue;
         const float n = std::max(0.0f, entry.trainCount);
         m_items[entry.formID] = ItemLearningData{
            entry.weights,
            n,
            now - 60.0 * entry.minutesSinceChosen};
         m_totalTrains += n;
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

      outTotalTrainCount = static_cast<uint32_t>(std::lround(std::max(0.0f, m_totalTrains)));
      const double now = PlayNow();

      for (const auto& [formID, data] : m_items) {
         SerializedEntry entry;
         entry.formID = formID;
         entry.weights = data.weights;
         entry.trainCount = data.trainCount;

         // v4: PLAY minutes since the last pick; the load counts back from
         // its own play clock.
         const double minutes = std::floor(std::max(0.0, now - data.chosenAt) / 60.0);
         entry.minutesSinceChosen = static_cast<uint32_t>(std::min(minutes, 4.0e9));

         entryCallback(std::move(entry));
      }
   }

   void FeatureBanditLearner::ImportData(
      const std::vector<SerializedEntry>& entries,
      [[maybe_unused]] uint32_t totalTrainCount)
   {
      std::unique_lock lock(m_mutex);

      m_items.clear();
      m_items.reserve(entries.size());

      // The total is summed from the entries rather than taken from the
      // header: since v4 it is a sum of fractions, and the header holds it
      // rounded.
      m_totalTrains = 0.0f;
      const double now = PlayNow();

      for (const auto& entry : entries) {
         // The last pick, counted back on this launch's play clock
         const float n = std::max(0.0f, entry.trainCount);
         m_items[entry.formID] = ItemLearningData{
            entry.weights,
            n,
            now - 60.0 * entry.minutesSinceChosen};
         m_totalTrains += n;
      }

      logger::info("FeatureBanditLearner imported: {} items, {:.1f} total trains"sv,
         m_items.size(), m_totalTrains);
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
