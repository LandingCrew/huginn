#pragma once
#include "PCH.h"
#include <chrono>
#include <string_view>

namespace Huginn::Util
{
#ifndef NDEBUG
   // Lightweight profiling timer for debug builds
   // Automatically logs elapsed time when scope exits
   struct ScopedTimer
   {
      std::string_view name;
      std::chrono::steady_clock::time_point start;

      // Rule of Five: prevent copying/moving (would break timing semantics)
      ScopedTimer(const ScopedTimer&) = delete;
      ScopedTimer& operator=(const ScopedTimer&) = delete;
      ScopedTimer(ScopedTimer&&) = delete;
      ScopedTimer& operator=(ScopedTimer&&) = delete;

      // explicit: prevent accidental implicit conversions from string literals
      explicit ScopedTimer(std::string_view n) noexcept
      : name(n)
      , start(std::chrono::steady_clock::now())
      {
      }

      ~ScopedTimer() noexcept
      {
      auto duration = std::chrono::steady_clock::now() - start;
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration);
      logger::trace("PERF: {} took {}us"sv, name, us.count());
      }
   };

   // Convenience macro for scoped timing
   // Usage: SCOPED_TIMER("FunctionName");
   // Two-level indirection so __LINE__ expands to its value BEFORE being pasted
   // (a direct `##__LINE__` pastes the literal token, so every use collides on
   // one name — breaking two timers in the same scope on portable preprocessors).
   #define HUGINN_TIMER_CONCAT_(a, b) a##b
   #define HUGINN_TIMER_NAME_(line)   HUGINN_TIMER_CONCAT_(Huginn_scoped_timer_, line)
   #define SCOPED_TIMER(name) Huginn::Util::ScopedTimer HUGINN_TIMER_NAME_(__LINE__)(name)
#else
   // No-op in release builds
   #define SCOPED_TIMER(name) ((void)0)
#endif
}
