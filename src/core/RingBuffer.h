#pragma once

#include <array>
#include <cstddef>
#include <iterator>

// =============================================================================
// RING BUFFER (Extracted from StateTypes.h in v0.13.x)
// =============================================================================
// Fixed-size circular buffer for event history and usage tracking.
// Trivially copyable when T is trivially copyable — safe for cross-thread
// state transfer via copy-out pattern (no heap allocations, no iterators).
//
// Provides const-only iteration. Mutation is performed exclusively via
// push_back() and pop_front() to maintain circular invariants.
//
// PERFORMANCE:
// - O(1) push_back (overwrites oldest when full)
// - O(n) iteration (n = current count)
// - Cache-friendly contiguous storage
// =============================================================================

namespace Huginn
{
   template<typename T, size_t Capacity>
   class RingBuffer
   {
   public:
      constexpr RingBuffer() noexcept = default;

      // Add event to buffer (overwrites oldest if full)
      void push_back(const T& event) noexcept {
         size_t writeIdx = (m_start + m_count) % Capacity;
         m_buffer[writeIdx] = event;
         if (m_count < Capacity) {
            ++m_count;
         } else {
            // Buffer full - advance start to overwrite oldest
            m_start = (m_start + 1) % Capacity;
         }
      }

      // Remove oldest event
      void pop_front() noexcept {
         if (m_count > 0) {
            m_start = (m_start + 1) % Capacity;
            --m_count;
         }
      }

      // Access oldest event (undefined if empty)
      [[nodiscard]] const T& front() const noexcept {
         return m_buffer[m_start];
      }

      // Access newest event (undefined if empty)
      [[nodiscard]] const T& back() const noexcept {
         return m_buffer[(m_start + m_count - 1) % Capacity];
      }

      // Access by logical index (0 = oldest, count-1 = newest)
      [[nodiscard]] const T& operator[](size_t idx) const noexcept {
         return m_buffer[(m_start + idx) % Capacity];
      }

      [[nodiscard]] size_t size() const noexcept { return m_count; }
      [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
      [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

      // Clear all events
      void clear() noexcept {
         m_start = 0;
         m_count = 0;
      }

      // Iterator support for range-based for loops (const-only)
      class Iterator {
      public:
         using iterator_category = std::forward_iterator_tag;
         using value_type = T;
         using difference_type = std::ptrdiff_t;
         using pointer = const T*;
         using reference = const T&;

         Iterator(const RingBuffer* buf, size_t idx) noexcept
            : m_buf(buf), m_idx(idx) {}

         reference operator*() const noexcept { return (*m_buf)[m_idx]; }
         pointer operator->() const noexcept { return &(*m_buf)[m_idx]; }

         Iterator& operator++() noexcept { ++m_idx; return *this; }
         Iterator operator++(int) noexcept { Iterator tmp = *this; ++m_idx; return tmp; }

         bool operator==(const Iterator& other) const noexcept { return m_idx == other.m_idx; }
         bool operator!=(const Iterator& other) const noexcept { return m_idx != other.m_idx; }

      private:
         const RingBuffer* m_buf;
         size_t m_idx;
      };

      using const_iterator = Iterator;

      [[nodiscard]] Iterator begin() const noexcept { return Iterator(this, 0); }
      [[nodiscard]] Iterator end() const noexcept { return Iterator(this, m_count); }
      [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
      [[nodiscard]] const_iterator cend() const noexcept { return end(); }

   private:
      std::array<T, Capacity> m_buffer{};
      size_t m_start = 0;   // Index of oldest element
      size_t m_count = 0;   // Number of valid elements
   };
}
