#pragma once
#include <algorithm>
#include <vector>

namespace Huginn::Util
{
   // =============================================================================
   // SORT TOP-K UTILITY (v0.7.21)
   // =============================================================================
   // Optimized sorting for top-K queries. Uses std::partial_sort when k < n
   // for O(n log k) complexity, falls back to std::sort for full results.
   //
   // Usage:
   //   std::vector<const Item*> result = ...;
   //   SortTopK(result, [](auto* a, auto* b) { return a->value > b->value; }, 3);
   //
   // @param container  Vector to sort (modified in place, resized to k elements)
   // @param comparator Comparison function for sorting order
   // @param topK       Maximum results to keep (0 = keep all, just sort)
   // =============================================================================

   template<typename T, typename Comparator>
   void SortTopK(std::vector<T>& container, Comparator&& cmp, size_t topK)
   {
      if (container.empty()) {
      return;
      }

      const size_t k = (topK > 0) ? std::min(topK, container.size()) : container.size();

      if (k > 0 && k < container.size()) {
      // Partial sort is O(n log k) - optimal for small k
      std::partial_sort(container.begin(), container.begin() + k, container.end(), cmp);
      container.resize(k);
      } else {
      // Full sort is O(n log n) - needed when k >= n
      std::sort(container.begin(), container.end(), cmp);
      }
   }

   // Convenience overload with default descending-by-member pattern
   // Usage: SortTopKByMember(items, &Item::magnitude, 3);
   template<typename T, typename MemberPtr>
   void SortTopKDescending(std::vector<T>& container, MemberPtr member, size_t topK)
   {
      SortTopK(container, [member](const auto& a, const auto& b) {
      return a->*member > b->*member;
      }, topK);
   }
}
