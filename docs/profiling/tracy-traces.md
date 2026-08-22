# Tracy trace history

A running log of Tracy captures so we can compare hot-path costs across builds and
verify that refactors don't regress. Newest entry first.

## How to capture

- Build with Tracy on: CMake `-DHuginn_TRACY=ON` (defines `Huginn_TRACY_ENABLED`;
  zones come from `Huginn_ZONE_NAMED` / `SCOPED_TIMER` in `src/Profiling.h`).
- Play a representative slice (combat + inventory churn), then in Tracy:
  **Statistics** panel → **Timing: Self only** → sort by **MTPC** (mean time per call).
- The load line stamps the build: `Huginn vX.Y.Z (<git-sha>) [DEBUG BUILD] [TRACY]`.
  Always record that SHA — it's the provenance for the numbers.

## How to read these numbers

- **These are DEBUG + Tracy-instrumented builds.** Absolute times are inflated vs. a
  release build (no inlining/opt, plus Tracy capture overhead). Use them for **relative
  ranking and cross-build comparison**, never as absolute frame-budget claims.
- **MTPC** = mean time per call = per-invocation cost → what causes a visible *spike*.
- **Total time** = MTPC × Counts over the capture → cumulative CPU. A big total with a
  huge Count (e.g. a poll at ~1,941 ticks) is spread across every tick on the ~100 ms
  update budget, so it's cheap *per tick* even when the total looks large.
- Accessors on the registries are intentionally **un-zoned** — if you see no
  `QueryTopK`/`FindBest`/`Get*` zones, that's expected, not missing data.

## Entry template

```
## YYYY-MM-DD — <build sha> — <one-line context>
- Session: <exe @ capture time>, Tracy <ver>, <frames>, ~<program time>, <mem>
- Save: <inventory scale — items/spells/weapons/scrolls/ammo>
- Frame sampled: <ms> (<fps>), CPU ~<%>
- Notes: DEBUG+TRACY (relative only)

| Zone | MTPC | Count | Total | Note |
Top hot zones + analysis + finding mapping.
```

---

## 2026-07-25 — `99cbb48` — critique #9 (display abstraction) complete

- Session: SkyrimSE.exe @ 2026-07-25 14:19:22, Tracy 0.13.1, 53,835 frames, ~17:29 program time, 100.7 MB.
- Save: real playthrough (~78 FQL items; poll counts ~6,916 ticks — a long, dense session).
- Notes: DEBUG + TRACY (relative only). `99cbb48` = PR #56 HEAD (all four review passes). **0 page switches
  this session**, so the #9 race bail was not exercised (`pageBail=0` in the heartbeat, as expected).

### Biggest per-call cost (spike risk), by MTPC

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `Display::Wheeler` | **2.54 ms** | 176 | 447 ms | Still the biggest per-call — critique **#14** (re-allocates every non-current page each push). Unchanged by #9. |
| `Pipeline::ScoreCandidates` | 1.31 ms | 176 | 231 ms | Scoring pass (bigger candidate set on the real save). |
| `Inventory::DeltaScan` | **685 µs** | 1,118 | **766 ms** | Biggest cumulative — critique **#13** (`GetInventorySafe` deep-copy), scales with the hoarder inventory. |
| `ItemRegistry::Reconcile` | 610 µs | 20 | 12 ms | Consolidated registry (#8) — still cheap. ✅ |
| `Pipeline::AllocateAndLock` | 325 µs | 176 | 57 ms | Now includes the #9 `AllocateSlotsForPage` + mid-tick race check. Reasonable. |
| `WeaponRegistry::ReconcileWeapons` | 227 µs | 22 | 5 ms | #8 — cheap. ✅ |
| `PollPlayerMagicEffects` | 113 µs | 6,916 | **780 ms** | Biggest cumulative poll — **#12**-adjacent. |
| `PollTargets` | 94 µs | 6,916 | 649 ms | Under-lock scan — critique **#12**. |
| `Display::Intuition` | 84 µs | 176 | 15 ms | Intuition push. |
| `Pipeline::PushDisplay` | **1.85 µs** | 176 | 0.33 ms | **#9 win** — was doing potential mid-tick re-allocation; now just hands the context to backends. Trivial. |

### Takeaways

- **#9 is perf-positive.** `Pipeline::PushDisplay` dropped to **1.85 µs/call** — the mid-tick re-sync + raw
  `AllocateSlotsForPage` that used to live there is gone; page resolution now happens once in the (cheap)
  `AllocateAndLock`/`ResolveDisplayPage` path. The race-bail check adds nothing measurable.
- **#8 still perf-neutral** on a real, dense save — the consolidated `*Registry::Reconcile` zones stay in the
  hundreds-of-µs range.
- **Hot paths are unchanged and still the Tier-3 targets:** `Display::Wheeler` 2.54 ms/call (**#14**),
  `Inventory::DeltaScan` 766 ms cumulative (**#13**), `PollPlayerMagicEffects` + `PollTargets` (**#12**).
  On the larger inventory, `#13`'s per-call cost grew (161 µs → 685 µs vs the 2026-07-24 baseline), as predicted.
- Frame tooltip read 21.2 s (a paused/loading frame — ignore); CPU ~46%. No pathological steady-state cost.

---

## 2026-07-24 — `4791318` — post-Scroll/Item/Spell consolidation, pre-Weapon

- Session: SkyrimSE.exe @ 2026-07-24 21:08:30, Tracy 0.13.1, 12,990 frames, ~11:17 program time, 98.3 MB.
- Save: small (3 items / 0 spells / 2 weapons — the tiny test save; poll counts ~1,941 ticks).
- Frame sampled: 10.07 ms (99.3 FPS), CPU ~40%.
- Notes: DEBUG + TRACY, `4791318` = FormRegistry migration status commit (Scroll/Item/Spell
  migrated; Weapon accessors still pre-Option-C). Baseline for the registry-consolidation work.

### Biggest per-call cost (spike risk), by MTPC

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `Display::Wheeler` | **3.65 ms** | 3 | 10.96 ms | **Biggest per-call.** Wheeler push — critique **#14** (re-allocates every non-current page each push). |
| `Pipeline::ScoreCandidates` | 1.22 ms | 3 | 3.66 ms | Scoring pass. |
| `Pipeline::AllocateAndLock` | 443.7 µs | 3 | 1.33 ms | Slot allocation + locks. |
| `ReconcileWeapons::Apply` | 194.6 µs | 5 | 973 µs | Weapon reconcile write section. |
| `WeaponRegistry::ReconcileWeapons` | 176.3 µs | 5 | 882 µs | 30 s reconcile. |
| `Inventory::DeltaScan` | 161.3 µs | 157 | **25.32 ms** | `GetInventorySafe` deep-copy — critique **#13**. |
| `Display::Intuition` | 106.0 µs | 3 | 318 µs | Intuition push — **#14** (no change-gating). |
| `Pipeline::PushDisplay` | 93.9 µs | 3 | 282 µs | |
| `ItemRegistry::Reconcile` | 92.3 µs | 3 | 277 µs | Migrated registry — cheap. ✅ |
| `PollPlayerMagicEffects` | 73.9 µs | 1,941 | **143.4 ms** | Biggest cumulative — **#12**-adjacent per-tick actor scan. |
| `PollTargets` | 55.3 µs | 1,941 | **107.4 ms** | Full scan under write lock, up to 3×/tick — critique **#12**. |
| `PollPlayerVitals` | 35.4 µs | 1,941 | 68.6 ms | |
| `WeaponRegistry::RefreshCharges` | 34.4 µs | 157 | 5.41 ms | |
| `SpellRegistry::Reconcile` | 43.6 µs | 17 | 741 µs | Migrated — cheap. ✅ |
| `ScrollRegistry::Reconcile` | 20.7 µs | 3 | 62 µs | Migrated — cheap. ✅ |

(Full 40-zone list not reproduced; the tail is all sub-20 µs MTPC — `StateManager::Update`
8.9 µs, the vital polls 7–17 µs, `Update::*` orchestration 1.8–2.6 µs, `WeaponRegistry::Refresh`
791 ns, `EquippedWeapons::Query` 749 ns.)

### Takeaways

- **System is healthy** — sampled frame 10 ms / 99 FPS, nothing blowing the budget. Everything
  below is efficiency/debt, not a fire.
- **Registry consolidation is perf-neutral** — the migrated `*Registry::Reconcile` zones are all
  sub-100 µs, and no new accessor zones appeared (accessors are deliberately un-zoned).
- **Where the measurable cost actually is**, mapped to the critique:
  - `Display::Wheeler` 3.65 ms/call → **#14** (push-path redundant work). *Biggest spike, cheapest fix.*
  - `Inventory::DeltaScan` 25 ms cumulative → **#13** (per-item `InventoryEntryData` deep-copy).
  - `PollTargets` 107 ms + `PollPlayerMagicEffects` 143 ms cumulative → **#12** (under-lock scans).
- Caveat: cumulative poll totals are spread across ~1,941 ticks on a 100 ms budget → <1% per
  tick each. They dominate *aggregate* CPU but are not hitching frames today.

### Follow-up captures wanted

- A capture on a **large real save** (the 2026-07-24 in-game soak used 54 items / 13 spells /
  10 weapons) — hoarder inventories are where `#13`'s deep-copy and `#12`'s target scan scale worst.
- A **before/after** capture bracketing any `#14` change, comparing `Display::Wheeler` /
  `Display::Intuition` MTPC.

---

## 2026-06-13 — `slot-cleanup` branch — pre-O1 baseline (folded in from `refactor/optimizations.md`)

- Session: ~13.8 min, 44,854 frames, ~4,743 update ticks. DEBUG + TRACY, *Self-only*.
- Context: slot-subsystem cleanup applied. Recorded here because it is the **before** side of
  the O1 (`RefreshCharges`) fix — the 2026-07-24 capture is the after.

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `WeaponRegistry::Refresh` | **1.25 ms** | 736 | 918 ms | #1 cost — full inventory walk @ 2 Hz. **This is what O1 removed.** |
| `Pipeline::ScoreCandidates` | 759 µs | 65 | 49 ms | Rare path, expected. |
| `ItemRegistry::Reconcile` | 663 µs | 13 | 8.6 ms | Rare full reconcile. |
| `ItemRegistry::RefreshCounts` | 634 µs | 728 | 462 ms | O2 → now tracked as critique **#13**. |
| `Pipeline::AllocateAndLock` | 338 µs | 65 | 22 ms | Slightly lower than 06-07's 354 µs — no Tier-3 regression. |
| `PollPlayerMagicEffects` | 81 µs | 4,743 | 385 ms | O3 → now tracked as its own Tier-3 item. |
| `PollTargets` | 57 µs | 4,743 | 269 ms | #12. |
| `OnUpdate` (self) | 12 µs | 4,742 | 56 ms | ✅ |

Skip-check: 4,743 ticks → 1,195 `RunPipeline` (~75% skipped at dirty flag) → 65 `ScoreCandidates`
(~94.5% of pipeline runs skipped at hash compare).

Instrumentation added in this session is what made O1 actionable: `WeaponRegistry::Refresh` had been
one opaque zone wrapping `EquippedWeapons::Query` + `RefreshCharges` + `ReconcileWeapons`, with the
inner calls on log-based `SCOPED_TIMER` rather than Tracy. Nested `Huginn_ZONE_NAMED` zones were added
(`Query` at the `UpdateLoop.cpp` call site via a lambda wrapper, to avoid forcing `Profiling.h` into the
header where `Query` is inline; the other two inside the methods).

## 2026-06-07 — `learning-cleanup` branch — first stage-zone attribution

- Sessions: a quiet ~6.5 min capture and an active ~11.5 min capture (4,336 update ticks).
  DEBUG + TRACY, *Self-only*. Huginn ≈ 0.13% CPU overall.
- Context: adding stage zones to `OnUpdate` (`Update::Subsystems` / `Update::Registries` /
  `Update::PipelineCheck`) resolved a previously-unattributed 164 µs/call of `OnUpdate` self-time
  almost entirely into registry maintenance. `OnUpdate` self dropped to ~13 µs.

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `WeaponRegistry::Refresh` | **1.23 ms** | 705 | 866 ms | #1 cost. Lines up with the ~1 s orange sawtooth in the Memory plot (per-call `InventoryEntryData` map allocation). |
| `Pipeline::ScoreCandidates` | 981 µs | 52 | 51 ms | Rare (96.7% skip), expected. |
| `ItemRegistry::RefreshCounts` | 618 µs | 694 | 429 ms | Consumption detector needs the snapshot. |
| `Pipeline::AllocateAndLock` | 354 µs | 52 | 18 ms | ✅ |
| `PollPlayerMagicEffects` | 117 µs | 4,337 | 505 ms | Top poll, every tick — it is not gated by the skip-check, it *feeds* it. |
| `PollTargets` | 75 µs | 4,337 | 327 ms | ✅ |
| `PollPlayerVitals` | 28 µs | 4,337 | 123 ms | ✅ |
| `OnUpdate` (self) | 13 µs | 4,336 | 57 ms | ✅ resolved — was 164 µs. |
| stage zones (`Update::*`) | 2–3 µs | 4,336 | ~35 ms | ✅ instrumentation overhead negligible. |

Skip-check: 4,336 ticks → 1,559 `RunPipeline` (64% skipped at sensor dirty flag) → 52
`ScoreCandidates` (96.7% skipped at discretized-hash compare). Full scoring ran 52 times in 11.5 min.

Also confirmed healthy at the time: Tier-3 changes held up (`StateManager::Update` self 8.3 µs);
instrumentation overhead safe to leave in (no-op when `Huginn_TRACY` is off); learning pipeline
(consumption + external-equip attribution) fired correctly and exactly once per action, log-verified.
