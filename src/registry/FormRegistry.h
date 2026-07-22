#pragma once

#include "util/AlgorithmUtils.h"  // Util::SortTopK

#include <atomic>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Huginn::Registry
{
   // =============================================================================
   // FORM REGISTRY CORE (architecture-critique finding #8)
   // =============================================================================
   // CRTP base for the FormID-keyed registries (spells, items, scrolls, weapons).
   // Owns the dual-index storage, the shared_mutex, the loading flag, and the
   // generic query primitives that previously lived — hand-copied — in each
   // registry. See docs/refactor/form-registry.md for the consolidation plan.
   //
   // DERIVED CONTRACT:
   //   static RE::FormID FormIDOf(const Entry&);
   // This is the only thing the core cannot infer: InventoryItem/InventoryScroll
   // wrap a `data` member (entry.data.formID) while SpellData IS the entry
   // (entry.formID). Everything else about the machine is type-independent.
   //
   // THREAD SAFETY (unchanged from the hand-written registries):
   // - Read API below takes shared_lock internally and returns copies, or
   //   pointers-into-storage valid ONLY synchronously on the calling thread until
   //   the next mutation (identical contract to the accessors it replaces).
   // - Writer primitives assume the caller already holds the unique_lock; the
   //   derived scan/reconcile paths build outside the lock, then mutate under it.
   // =============================================================================
   template<typename Derived, typename Entry>
   class FormRegistry
   {
   public:
      FormRegistry() = default;
      ~FormRegistry() = default;

      // Non-copyable / non-movable (holds a mutex; used as a unique_ptr global).
      FormRegistry(const FormRegistry&) = delete;
      FormRegistry& operator=(const FormRegistry&) = delete;
      FormRegistry(FormRegistry&&) = delete;
      FormRegistry& operator=(FormRegistry&&) = delete;

      // Loading/rebuilding flag (M3: atomic acquire load).
      [[nodiscard]] bool IsLoading() const noexcept
      {
         return m_isLoading.load(std::memory_order_acquire);
      }

      // ---- Read API (locks internally) ----

      [[nodiscard]] size_t EntryCount() const
      {
         std::shared_lock lock(m_mutex);
         return m_entries.size();
      }

      // Copy of the whole store, for cross-thread / retained access.
      [[nodiscard]] std::vector<Entry> AllEntriesCopy() const
      {
         std::shared_lock lock(m_mutex);
         return m_entries;
      }

      // Zero-allocation visitor. Func may return void, or bool to early-exit on false.
      template<typename Func>
      void ForEachEntry(Func&& func) const
      {
         std::shared_lock lock(m_mutex);
         for (const auto& entry : m_entries) {
            if constexpr (std::is_same_v<std::invoke_result_t<Func, const Entry&>, bool>) {
               if (!func(entry)) return;
            } else {
               func(entry);
            }
         }
      }

      // All entries matching pred, unsorted. Pointers valid synchronously (see above).
      template<typename Pred>
      [[nodiscard]] std::vector<const Entry*> Collect(Pred&& pred) const
      {
         std::shared_lock lock(m_mutex);
         std::vector<const Entry*> result;
         result.reserve(m_entries.size() / 4 + 1);
         for (const auto& entry : m_entries) {
            if (pred(entry)) result.push_back(&entry);
         }
         return result;
      }

      // Top-K entries matching pred, sorted by key descending. O(n log k) via partial_sort.
      template<typename Pred, typename Key>
      [[nodiscard]] std::vector<const Entry*> QueryTopK(Pred&& pred, Key&& key, size_t topK) const
      {
         std::shared_lock lock(m_mutex);
         std::vector<const Entry*> result;
         result.reserve(m_entries.size() / 4 + 1);
         for (const auto& entry : m_entries) {
            if (pred(entry)) result.push_back(&entry);
         }
         Util::SortTopK(result, [&key](const Entry* a, const Entry* b) {
            return key(*a) > key(*b);
         }, topK);
         return result;
      }

      // Single highest-key entry matching pred. O(n) single pass, no allocation.
      template<typename Pred, typename Key>
      [[nodiscard]] const Entry* FindBest(Pred&& pred, Key&& key) const
      {
         std::shared_lock lock(m_mutex);
         const Entry* best = nullptr;
         std::invoke_result_t<Key, const Entry&> bestKey{};
         for (const auto& entry : m_entries) {
            if (!pred(entry)) continue;
            auto k = key(entry);
            if (!best || k > bestKey) {
               best = &entry;
               bestKey = k;
            }
         }
         return best;
      }

      template<typename Pred>
      [[nodiscard]] size_t CountMatching(Pred&& pred) const
      {
         std::shared_lock lock(m_mutex);
         size_t count = 0;
         for (const auto& entry : m_entries) {
            if (pred(entry)) ++count;
         }
         return count;
      }

   protected:
      // ---- Writer primitives. Caller MUST already hold m_mutex (unique_lock). ----

      // Swap-pop removal by FormID. Returns true if an entry was removed.
      bool RemoveEntryLocked(RE::FormID formID)
      {
         auto it = m_formIDIndex.find(formID);
         if (it == m_formIDIndex.end()) return false;

         const size_t index = it->second;
         if (index != m_entries.size() - 1) {
            m_entries[index] = std::move(m_entries.back());
            m_formIDIndex[Derived::FormIDOf(m_entries[index])] = index;
         }
         m_entries.pop_back();
         m_formIDIndex.erase(formID);
         return true;
      }

      // Append entry + index. Caller guarantees formID is not already present.
      void PushEntryLocked(Entry&& entry, RE::FormID formID)
      {
         const size_t index = m_entries.size();
         m_entries.push_back(std::move(entry));
         m_formIDIndex[formID] = index;
      }

      void ClearStoreLocked()
      {
         m_entries.clear();
         m_formIDIndex.clear();
      }

      // Storage — protected so derived scan/reconcile can operate under its own lock.
      std::vector<Entry> m_entries;
      std::unordered_map<RE::FormID, size_t> m_formIDIndex;
      mutable std::shared_mutex m_mutex;
      std::atomic<bool> m_isLoading{false};
   };
}
