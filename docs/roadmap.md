# Huginn Roadmap

Open work only. Completed items live in [roadmap-archive.md](roadmap-archive.md) — several record why an approach was
rejected, so check there before re-opening something.

## Known Bugs
- [ ] #76: loading a second save in one session (esp. a different character)
      leaves Huginn's stored wheel indices stale; `UpdatePage` finds every page
      unmanaged ~1s after creation and sets `wheelIndex=-1` permanently. Does
      NOT self-correct — the wheel is empty until `hg reload` or another
      save-load. Same index-shift hazard the v3 delete-by-label path dodges on
      the delete side; the read side has no equivalent. Ruled out as #75 fallout
      (same build produced clean sessions). Repro in the issue, unconfirmed.
      **Expected closed by the re-resolve below (0.18.34), unverified in-game:**
      `UpdatePage` no longer latches -1 as its first response — it re-derives the
      index from the client label and only invalidates when the lookup reports
      the wheel genuinely gone. Covers the shift case; a real deletion still
      falls through to the existing `RecoverInvalidatedWheels` rebuild, so both
      halves now have a path. Keep this open until a second-save-load session
      shows either a `Re-resolve: page N ... index X -> Y` line or a clean run
- [ ] Display push is not gated on Wheeler edit mode, so the ~7s a player spends
      rearranging wheels is spent writing subtexts to indices that are already
      stale. Observed in the same 2026-08-22 session that verified the
      re-resolve: the shift was detectable at 10:23:48 but not corrected until
      edit-mode exit at 10:23:55, and two pushes landed on the wrong wheel in
      between (`[Subtext] page 1` at 10:23:48.342, `page 2` at 10:23:51.874).
      NOT fixed by the re-resolve, which triggers on exit by design — mid-edit
      indices are transient and re-resolving each intermediate state would chase
      noise. The complement is to skip the push while `IsInEditMode()`: the
      player is rearranging wheels, not reading recommendations. Nothing gates on
      it today (`IsInEditMode` is called only by `LogAPIInfo`). Bounded and
      self-correcting — exit re-resolves and the next push repaints (XS/S)
- [ ] `DeleteManagedWheelsForClient` reported deleting ZERO wheels for a client
      that had one, orphaning it. Observed 2026-08-22 while testing #92: a
      'Huginn: Regulars' wheel created at 12:30:55 (index 2) was still present
      when the 12:32:39 teardown logged `Deleted 0 managed wheel(s) for client
      'Huginn: Regulars'`; the next CreateWheels then added a SECOND wheel under
      that name, and 12:34:48 found 2. Both were finally reaped at 12:34:59.
      Sequence detail worth keeping: the failing call was the third in one
      IssueWheelDeletes loop, after a call that deleted 2 wheels for a different
      label — but that is correlation, not a diagnosis, and the logs have since
      rotated. **Matters more under v4**, which no longer drops managed wheels on
      load, so anything a teardown misses persists across the session and
      accumulates. Huginn now survives it (PR #92's ambiguity handling keeps or
      invalidates by index membership rather than adopting blind), so this is a
      correctness/cleanliness issue, not a live breakage. Reproduce by cycling
      page layouts through `hg reload` and watching the `Deleted N managed
      wheel(s)` counts against what was created. Likely upstream (S)
- [ ] Intuition menu shown during "cut scenes"
- [ ] Intuition menu not hiding when commanded by external mod
- [ ] New game wheelerAPI integration seems to fail

## Known Mod Compatability Issues
- [ ] Vanilla-build integration pass — a set of contexts is only ever exercised
      on the Requiem-based list this is developed against, so anything vanilla
      ships and Requiem strips is verified by unit test alone. Workstation is the
      known case (test 6h stands in for it); the honest scope is "boot a vanilla
      profile once and walk the contexts", which would also cover the #79 four
      that no LoreRim character can carry. Wants a save with vanilla alchemy
- [ ] #63: Requiem (LoreRim et al.) strips Fortify Smithing/Enchanting from
      alchemy, so the workstation context has no potion to rank — inert in the
      modlists that actually get play-tested. Live targets are filled soul gems
      and fortify apparel (#65); vanilla path needs its own regression test

## Known Recommendation Issues
- [ ] #65: apparel is not a candidate source (`SourceType` has no armor entry),
      so fortify gear can never be recommended — blocks the Requiem answer to #63
- [ ] A wildcard can also go unreachable *without* being out of index range:
      SlotAllocator.cpp:571 skips wildcard candidates for slots with
      bWildcardsEnabled=false, and WildcardManager only ever receives a slot
      COUNT, never per-slot enablement — so it can roll one for a slot that
      forbids them. If no other slot picks it up, HasActiveWildcard() reports it
      live while nothing displays: the same stall #70 fixed for the index case.
      Unconfirmed — needs a config with wildcards disabled on some slots to
      reproduce. Root cause for both is one global position-indexed cache; a
      pageIndex → array cache would make stranding structurally impossible (S/M)
- [ ] WildcardManager's slotCount < 2 guard returns before the #70 drop, so a
      1-slot page leaves stranded entries in place. Bounded — nothing displays
      there and UpdateExpiry() still ages the cache out on its own timer — so
      the stall can't outlive the cooldown. Not worth restructuring the guard
      for; noted so the invariant isn't assumed unconditional (XS)
- [ ] #70: cached wildcards above a smaller page's slot count block re-rolls
      invisibly — HasActiveWildcard() scans the whole cache while
      ApplyWildcardsToRanking bounds by slotCount, so a wildcard stranded at
      index 6 suppresses rolling on a 4-slot page for 30s + refractory. Same
      differing-slot-count trigger as #10b, but not one tick and not
      self-correcting. Repro config (one 4-slot page) is in the issue and also
      re-verifies #69 (S)
- [ ] Scroll cold-start: all scrolls sit in the pool every tick but score
      `learn≈0` against trained items at `learn=7–8`, so one can never surface
      until used and can't be used until surfaced
- [ ] `lookingAtOre` did not surface a pickaxe even with one in inventory —
      whether that context ranks mining tools at all is unchecked (noted on #64)

## UX / Feature Backlog
- [ ] Read-only Intuition menu mode — display-only widget, hotkeys disabled, an
      external UI (Wheeler / 3rd-party) drives selection

## Architecture Critique — Backlog
See [reviews/architecture-critique.md](reviews/architecture-critique.md).
**Landed:** Tier 1 (all); Tier 2 #8 registry consolidation (PR #55), #9 display
abstraction (PR #56), #10 safe pieces — GetContextWeight move + ComputeRelevanceTags
dedup (PR #57), #10 leftover — relevance-tag encoding unified on ContextRuleEngine
(PR #58, merged; verified in-game across 5 Debug sessions — all 26 reason labels
observed, threshold parity exact on both smoothing exponents). Critique #10 is
now closed; #59–#65 are follow-ups it surfaced, not remaining critique work.

### Tier 2 — COMPLETE
All Tier 2 critique items have landed; see [roadmap-archive.md](roadmap-archive.md).

### Tier 3 — hot-path perf (trace-prioritized; see docs/profiling/tracy-traces.md)
- [ ] #14: gate the display push paths — IntuitionBackend change-detect, WheelerBackend
      lazy per-page allocation, GetLockSnapshot, quantize lock-timer subtext.
      **Parts 1-3 MERGED** in PR #96 at 0.19.2 (2026-08-26). Part 4 is DROPPED
      — see the measurement note below; reopen only if a felt stutter turns up
      that a sub-1 ms mean cannot explain. See docs/refactor/wheeler-push-spikes.md.
      **TRACED 2026-08-24 on @e81ff19, Self-only. Three captures:**
      | capture | Display::Wheeler | Display::Intuition |
      |---|---|---|
      | main baseline, 2026-08-23 | 3.23 ms MTPC / 100.21 ms / 31 | 84 µs (2026-08-22) |
      | branch, lock label OFF, 5:25 | 3.87 ms / 255.14 ms / 66 | 80.62 µs / 66 |
      | branch, lock label ON, 8:19 | 3.73 ms / 179.21 ms / 48 | 63.58 µs / 48 |
      | branch, lock label ON, 15:24 | 3.60 ms / 180.04 ms / 50 | 63.12 µs / 50 |
      Part 1 (Intuition change-detect) CONFIRMED: 92.76 µs in the broken cut →
      63.12 µs, comfortably under the 84 µs baseline. Note the zone wraps the
      whole of Push() including the m_scratch build (IntuitionBackend.cpp:68-84),
      which runs whether the cache hits or not — so this zone structurally cannot
      show the real win, which is UI-thread GFx work it never times. 3.16 ms of
      180 ms either way; not where the time is.
      Parts 2-3 (lock snapshot + quantization): enabling the label costs nothing
      measurable (3.87 off vs 3.60 on). **Weak evidence** — locks were live only
      ~44 s of the 15-minute capture (17:01:06-17:01:49, 43 events), so most
      pushes never touched the label. Also `fLockDurationMs = 1000` makes
      ceil(remaining/1000) yield only ever `1`, so the countdown never ticks;
      raise it to ~4000 to exercise the 3 → 2 → 1 transitions the quantization
      is designed around.
      **Part 4's deferral rationale is now CONTRADICTED.** It was held back on
      the theory that the quantization "may remove considerably more". It does
      not: it removes the COST OF ENABLING the label, and leaves the baseline
      untouched. With the label off — the shipped default — the whole Wheeler
      half of #14 is inert. Part 4 is the only remaining lever on this zone.
      **The per-call figure was a SMALL-SAMPLE ARTIFACT — #14's whole ranking
      premise is void.** A 44:40 playthrough on 2026-08-26 (874 pushes, vs
      31-66 in every earlier capture) puts Display::Wheeler at **986.54 µs
      MTPC** — under a third of the 3.23-3.87 ms the short runs reported. The
      push fires every few seconds, so a handful of cold calls (wheel creation,
      post-load) dominated those means. There is no 3.6 ms frame spike to fix.
      Ranking on the 44-min capture, by total, Self-only:
      | zone | total | % | MTPC | calls |
      |---|---|---|---|---|
      | PollPlayerMagicEffects (O3) | 2.61 s | 0.10% | 136.3 µs | 19,179 |
      | PollTargets (#12) | 2.17 s | 0.08% | 113.34 µs | 19,179 |
      | Inventory::DeltaScan (#13) | 1.03 s | 0.04% | 267.78 µs | 3,848 |
      | Display::Wheeler (#14) | 862 ms | 0.03% | 986 µs | 874 |
      Display::Wheeler is FOURTH, and O3 is confirmed as the real top cost —
      which is what the 2026-07-25 capture said before the short runs pulled
      attention here. Nothing in Tier 3 exceeds 0.10% of runtime; on CPU
      grounds this tier has no work left in it. Part 4 (lazy per-page
      allocation) should be dropped unless a felt stutter turns up that a
      986 µs mean does not explain.
      Two regressions in the first cut were caught ONLY by Tracy, not by the
      log: change-detect never fired because `confidence` (= `assignment.utility`)
      varies every run, and `GetLockSnapshot` was hoisted unconditionally while
      the old per-slot calls sat behind `stConfig.showLockTimerLabel` — turning
      zero lock acquisitions into one in the default config. Re-measure, don't
      assume
      Zone-count equality is NOT a diagnostic here, contrary to how the 188/188
      observation above reads: Huginn_ZONE_NAMED sits at the top of Push(), ahead
      of every early-out, so its count always equals the call count. MTPC is the
      only signal.
      The lock-timer label has NO log coverage: `[Subtext]` (PipelineCoordinator.cpp:645)
      logs the coordinator's pre-derived labels and is deduped against the last
      summary, while WheelerBackend clears and rewrites them on its own copy
      (WheelerBackend.cpp:127). Grepping the log for lock labels finds nothing
      even when the path ran — do not read that as the path being dead
- [ ] #13 (= O1's sibling "O2"): count-only two-phase GetInventorySafe variant —
      Inventory::DeltaScan deep-copies InventoryEntryData per item at 2 Hz; plus
      PipelineContext container reuse (documented-but-broken).
      **Overtaken by #14 on MTPC, but climbing on total — measure before
      scheduling.** 2026-08-23: 257 µs MTPC / 24.46 ms. 2026-08-24 (15:24
      capture): 218.35 µs / 96.95 ms / 444 calls. 2026-08-26 (44:40): **267.78 µs
      MTPC / 1.03 s over 3,848 calls** — third by total, and now AHEAD of
      Display::Wheeler (862 ms) rather than behind it, because it runs ~4x as
      often. Still only 0.04% of runtime. The numbers that
      motivated this item, for reference: 685 µs x 1,118 = 766 ms on the 2026-07-25
      real save, up from 161 µs/call on the small test save — it scales with
      inventory size, so hoarder saves are the worst case. Constrained, not
      eliminable: this is the consumption detector and it needs a count snapshot
      (verified firing exactly once per consumption). The two levers are a longer
      interval (trades detection latency) or a count-only query that skips
      InventoryEntryData construction; the scratch maps are already allocation-free
      (m_scanCounts reuse), so the remaining cost is the SKSE query itself (M)
- [ ] O3: PollPlayerMagicEffects early-out — 113 µs x 6,916 = 780 ms on the
      2026-07-25 capture, the biggest cumulative POLL and the steady-state floor
      (every other poll is single- to low-double-digit µs). Runs on every tick by
      necessity: it is not gated by the skip-check, it FEEDS it. Options are an
      early-out when the active-effect list is unchanged, or caching by effect-list
      revision. Adjacent to #12 but not covered by it — #12 is PollTargets.
      **RECONFIRMED as the #1 zone by total time** on the 2026-08-26 44:40
      capture: 136.3 µs x 19,179 = **2.61 s (0.10%)**, ahead of PollTargets
      (2.17 s) and 3x Display::Wheeler (862 ms). The 2026-08-23/24 captures that
      ranked #14 above it were 5-15 min and too short for the per-tick pollers
      to accumulate. At 0.10% CPU this still does not need fixing; it is simply
      the honest top of Tier 3 and where to look first if idle cost ever
      matters (S/M)
- [ ] #12: PollTargets — build outside the write lock, one classification pass (scanned
      up to 3×/tick), MAX_TRACKED_TARGETS 50→~12, squared-distance compares (M).
      **Second by total time**, behind O3 (2026-08-26, 44:40 capture, Self-only):
      2.17 s over 19,179 calls at 113.34 µs MTPC, vs Display::Wheeler's 862 ms.
      A steady per-tick cost, not a spike, so it never shows as a hitch — at
      0.08% of runtime it is a CPU-budget item and not an urgent one
- [ ] #11: cache SpellData.effectiveCost — CandidateGenerator calls LookupByID +
      CalculateMagickaCost per known spell per tick, inside the registry lock (M)

### Follow-ups
- [ ] Unit tests for Context::WeightForCandidate (Tests.cpp:2656/3374 currently
      hand-reimplement the weight mapping — call the real one). DominantReason /
      ReasonLabel are covered by unit test 17.
- [ ] #59: get docs/ under version control — `docs/ARCHITECTURE.md` is linked from
      CLAUDE.md and is still untracked, along with all of `docs/architecture/`,
      `docs/reference/`, `docs/reviews/`, `docs/compatibility/`, `docs/limitations/`
      and `docs/changelog/`. The 0.19.0 ownership split had to edit four of these
      (7-dmenu-integration.md, ARCHITECTURE.md, ConsoleCommands.md) and none of
      those edits are in the PR that made them necessary — the docs and the code
      they describe can now drift with nothing to catch it
- [ ] Cosave decode negative test logs `[E] DecodeV2EntryBlob: byteLen 83 != stride
      84` at every Debug startup. The test passes — the error is the assertion
      firing. Silence it so a real rejection stays visible (Tests.cpp:4137).
      Still firing every session as of 0.19.0; it has now cost real time twice
      while triaging unrelated logs (XS)
- [ ] `build-verify-preset` (1 commit) — no-deploy configure preset, rebased
      onto main and replaying cleanly, but **contents never reviewed**. The last
      unmerged branch holding work. Read the diff before merging; it needs a
      version bump per the per-PR convention, which it does not currently carry.
      Stale remote branches that can be deleted: `tier3-14-display-push` and
      `soak-skip-telemetry` (merged 2026-08-26 as PR #96 / #95),
      `docs-optimizations-fold` and `wheeler-index-reresolve` (merged earlier)
- [ ] Soak protocol needs deliberate MANUAL equips — accept% is fed only by
      equips made outside Huginn, so a burst played through the wheel/hotkeys
      produces no recommendation-quality data at all. Confirmed 2026-08-26: a
      44-min session reported accept=n/a in every window while 21
      external-equip events fired and were all filtered as wheel-open (each
      coinciding with a src=Wheeler reward in the same second). The filter is
      CORRECT — grading a wheel pick asks whether Huginn predicted the item the
      player chose off Huginn's own list. v0.19.1 adds `skipped=N (wheel=…)` to
      the heartbeat so n/a is self-explaining (branch `soak-skip-telemetry`
      @067397b), and docs/playtest/LongPlaySoak.md now lists manual equips as a
      coverage requirement and a void-run signal. Remaining: decide whether
      accept% is the right headline metric for a wheel-driven player at all,
      and whether to fold non-wheel consumption into it (5 of 50 events on that
      session, not 50 — wheel/hotkey rewards must stay out)
- [ ] Positive log line when the text-entry input gate engages — a
      transition-only `[InputHandler] Input suppressed — text entry active`.
      Today the gate is only verifiable by the ABSENCE of `KEY PRESS` lines,
      which is indistinguishable from any other reason input stopped, and
      unbound letters never log at all. Verified once by watching the widget
      instead; a log line makes it checkable and catches a regression (XS)
- [ ] Addendum #15/#16 (Kalman FQL / learnable context weights) — **parked**: needs a v3
      cosave bump, NOT landable during an active soak run
