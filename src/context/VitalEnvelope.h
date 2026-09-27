#pragma once

#include <chrono>

namespace Huginn::Context
{
   // =============================================================================
   // VITAL ENVELOPE -- drop fast, recover slow
   // =============================================================================
   // The vital fraction a restore weight is computed from. Follows a drop at
   // once, but holds the recent low for HOLD before following regeneration
   // back up. A rise of INSTANT_RESTORE or more in ONE tick is a potion or a
   // spell, not regeneration, and releases the hold at once.
   //
   // Why: magicka falls as the player casts and climbs back as it regenerates,
   // and the continuous restore curve took restore-magicka potions in and out
   // of slots with it -- 15 of 40 swap-backs in a 50-minute soak followed a
   // magicka bucket change, a median 5.4 s apart (2026-09-27). Health was
   // left alone on purpose: its swap-backs were a median 68 s apart -- hurt,
   // then healed -- which is the recommendation being right, not flapping.
   //
   // Tick() runs EVERY update tick (UtilityScorer::Update), not only on
   // pipeline runs. Fed only from scoring, the hold's expiry was invisible to
   // the skip gate -- the held low could sit in the weights while the pool was
   // full -- and the potion test compared readings seconds apart, where
   // ordinary regeneration can look like a potion and a small potion like
   // regeneration (/code-review on #142). One tick apart, regeneration moves a
   // fraction of a percent and a potion moves several.
   //
   // Scoring input only. Published vitals are untouched, so the CRITICAL
   // overrides and the full-vitals filters still see the real value.
   // Single-threaded (update thread).
   // =============================================================================
   class VitalEnvelope
   {
   public:
      using Clock = std::chrono::steady_clock;

      static constexpr std::chrono::milliseconds HOLD{ 8000 };  // > the 5.4 s median cycle
      static constexpr float INSTANT_RESTORE = 0.05f;           // 5% of the pool in one tick

      // Feed this tick's raw value. Returns true when the held value was
      // released upward -- the scores are stale and want one re-run.
      [[nodiscard]] constexpr bool Tick(float raw, Clock::time_point now) noexcept
      {
         const bool jumped = m_valid && raw - m_lastRaw >= INSTANT_RESTORE;
         m_lastRaw = raw;

         if (!m_valid || raw <= m_held) {
            m_valid = true;
            m_held = raw;
            m_heldAt = now;
            return false;
         }
         if (jumped || now - m_heldAt >= HOLD) {
            m_held = raw;
            m_heldAt = now;
            return true;
         }
         return false;  // regenerating: keep scoring the recent low
      }

      // What scoring should use. Before the first tick, the raw value.
      [[nodiscard]] constexpr float Value(float raw) const noexcept
      {
         return m_valid ? m_held : raw;
      }

      constexpr void Reset() noexcept { m_valid = false; }

   private:
      bool m_valid = false;
      float m_held = 1.0f;
      float m_lastRaw = 1.0f;
      Clock::time_point m_heldAt{};
   };

   // Pinned at compile time: follow a drop, hold through regeneration, let
   // go after HOLD (and say so), and release on a one-tick jump.
   namespace VitalEnvelopeChecks
   {
      using namespace std::chrono_literals;
      constexpr bool HoldsThenReleases()
      {
         VitalEnvelope e;
         const auto t0 = VitalEnvelope::Clock::time_point{};
         if (e.Tick(0.60f, t0)) return false;
         if (e.Tick(0.40f, t0 + 1s) || e.Value(0.40f) != 0.40f) return false;   // drop: at once
         // Regeneration arrives a few percent at a time, never 5% in a tick
         if (e.Tick(0.41f, t0 + 1100ms) || e.Value(0.41f) != 0.40f) return false; // regen: held
         if (e.Tick(0.45f, t0 + 2s) || e.Tick(0.49f, t0 + 3s) || e.Tick(0.53f, t0 + 4s)) return false;
         if (e.Value(0.53f) != 0.40f) return false;
         if (e.Tick(0.56f, t0 + 8s) || e.Value(0.56f) != 0.40f) return false;     // 7 s after the drop
         return e.Tick(0.58f, t0 + 10s) && e.Value(0.58f) == 0.58f;               // HOLD passed
      }
      constexpr bool PotionReleases()
      {
         VitalEnvelope e;
         const auto t0 = VitalEnvelope::Clock::time_point{};
         (void)e.Tick(0.30f, t0);
         (void)e.Tick(0.31f, t0 + 100ms);
         // +17% in one tick -- the 50-point potion on a 300 pool from review
         return e.Tick(0.48f, t0 + 200ms) && e.Value(0.48f) == 0.48f;
      }
      static_assert(HoldsThenReleases());
      static_assert(PotionReleases());
   }
}
