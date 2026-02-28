#pragma once
#include <atomic>

namespace Huginn::Util
{
   // =============================================================================
   // ATOMIC RESET GUARD (v0.7.21)
   // =============================================================================
   // RAII guard that resets an atomic flag when scope exits.
   // Ensures flags like m_isLoading get cleared even on exception/early return.
   //
   // Usage:
   //   m_isLoading = true;
   //   AtomicBoolGuard guard{ m_isLoading };  // Will set to false on destruction
   // =============================================================================

   template<typename T>
   struct AtomicResetGuard
   {
      std::atomic<T>& flag;
      T resetValue;

      // Rule of Five: prevent copying/moving (would break RAII semantics)
      AtomicResetGuard(const AtomicResetGuard&) = delete;
      AtomicResetGuard& operator=(const AtomicResetGuard&) = delete;
      AtomicResetGuard(AtomicResetGuard&&) = delete;
      AtomicResetGuard& operator=(AtomicResetGuard&&) = delete;

      AtomicResetGuard(std::atomic<T>& f, T val) noexcept
      : flag(f)
      , resetValue(val)
      {
      }

      ~AtomicResetGuard() noexcept
      {
      flag.store(resetValue, std::memory_order_release);
      }
   };

   // Convenience alias for the common bool case
   // Usage: AtomicBoolGuard guard{ m_isLoading, false };
   using AtomicBoolGuard = AtomicResetGuard<bool>;
}
