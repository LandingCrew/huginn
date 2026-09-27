#pragma once

#include <chrono>

namespace Huginn::Context
{
   // =============================================================================
   // VITAL ENVELOPE -- drop fast, recover slow
   // =============================================================================
   // The vital fraction a restore weight is computed from. Follows a drop at
   // once, but holds the recent low for HOLD before following regeneration
   // back up. A jump bigger than INSTANT_RESTORE in one update is a potion or
   // spell, not regeneration, and resets it at once.
   //
   // Why: magicka falls as the player casts and climbs back as it regenerates,
   // and the continuous restore curve took restore-magicka potions in and out
   // of slots with it -- 15 of 40 swap-backs in a 50-minute soak followed a
   // magicka bucket change, a median 5.4 s apart (2026-09-27). Health was
   // left alone on purpose: its swap-backs were a median 68 s apart -- hurt,
   // then healed -- which is the recommendation being right, not flapping.
   //
   // Scoring input only. Published vitals are untouched, so the CRITICAL
   // overrides and the full-vitals filters still see the real value.
   // Single-threaded (update thread), like the rest of rule evaluation.
   // =============================================================================
   class VitalEnvelope
   {
   public:
      using Clock = std::chrono::steady_clock;

      static constexpr std::chrono::milliseconds HOLD{ 8000 };  // > the 5.4 s median cycle
      static constexpr float INSTANT_RESTORE = 0.20f;           // one update, 20% of the pool

      [[nodiscard]] constexpr float Follow(float raw, Clock::time_point now) noexcept
      {
         const bool jumped = m_valid && raw - m_lastRaw > INSTANT_RESTORE;
         m_lastRaw = raw;

         if (!m_valid || raw <= m_held || jumped) {
            m_valid = true;
            m_held = raw;
            m_heldAt = now;
            return raw;
         }
         if (now - m_heldAt < HOLD) {
            return m_held;  // regenerating: keep scoring the recent low
         }
         m_held = raw;
         m_heldAt = now;
         return raw;
      }

      constexpr void Reset() noexcept { m_valid = false; }

   private:
      bool m_valid = false;
      float m_held = 1.0f;
      float m_lastRaw = 1.0f;
      Clock::time_point m_heldAt{};
   };

   // Pinned at compile time: follow a drop, hold through regeneration, let
   // go after HOLD, and reset on a jump that only a potion makes.
   namespace VitalEnvelopeChecks
   {
      using namespace std::chrono_literals;
      constexpr bool HoldsThenReleases()
      {
         VitalEnvelope e;
         const auto t0 = VitalEnvelope::Clock::time_point{};
         if (e.Follow(0.60f, t0) != 0.60f) return false;          // first read
         if (e.Follow(0.40f, t0 + 1s) != 0.40f) return false;     // drop: at once
         if (e.Follow(0.55f, t0 + 4s) != 0.40f) return false;     // regen: held
         return e.Follow(0.58f, t0 + 10s) == 0.58f;               // HOLD passed
      }
      constexpr bool PotionResets()
      {
         VitalEnvelope e;
         const auto t0 = VitalEnvelope::Clock::time_point{};
         (void)e.Follow(0.30f, t0);
         return e.Follow(0.80f, t0 + 1s) == 0.80f;                // +50% in one step
      }
      static_assert(HoldsThenReleases());
      static_assert(PotionResets());
   }
}
