# Huginn Roadmap

Open work only. Completed items live in [roadmap-archive.md](roadmap-archive.md) — several record why an approach was
rejected, so check there before re-opening something.

## Known Bugs
- [ ] Wheeler's content-unchanged early-out goes stale when Wheeler itself
      mutates the wheel. With `PostActivation = Backfill`, activating an entry
      makes Wheeler shift the remaining entries internally — Huginn is never
      told, so `WheelSync`'s `slotFormIDs`/`slotWildcard`/`slotUniqueIDs`/
      `slotRawSubtexts` cache still describes the pre-activation wheel. The
      early-out (WheelSync.cpp:1081) then compares the incoming vectors against
      that cache, matches, and returns before repainting, so the wheel keeps a
      blank where the shifted-out entry was for as long as the recommendations
      stay stable. **Observed 2026-08-28, 0.19.6:** page 1 activation at
      20:33:33.456 (`entry=0, formID=00013986`); the last push to that wheel at
      20:33:38.501 carried `0:Steel Dagger, 1:Wildcard(Iron War Axe),
      2:Staff of the Skeletal Soldier, 3:empty`; the Intuition widget rendered
      all four correctly while the wheel showed slot 1 blank. `hg refresh`
      repaired it. NOT self-correcting, which is what separates this from the
      two deferrals the early-out's own comment already accepts as "rare and
      self-correcting" — those resolve on the next content change; this one
      survives it, because the content never changed. The edit-mode re-resolve
      already clears `slotRawSubtexts` for exactly this reason
      (WheelSync.cpp:350) — the activation path needs the same clear, keyed on
      the wheel Wheeler actually mutated. Only bites under Backfill: the `Empty`
      policy blanks the activated slot in WheelerBackend
      (WheelerBackend.cpp:264) so the pushed arrays differ and the early-out
      misses. Independent of the wildcard work — the assignment was correct
      throughout — but a wildcard is a low-scored pick the player is more likely
      to activate, so raising the wildcard rate raises exposure (S)
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
- [ ] Scroll cold-start: all scrolls sit in the pool every tick but score
      `learn≈0` against trained items at `learn=7–8`, so one can never surface
      until used and can't be used until surfaced
- [ ] `lookingAtOre` did not surface a pickaxe even with one in inventory —
      whether that context ranks mining tools at all is unchecked (noted on #64)

## UX / Feature Backlog
- [ ] Read-only Intuition menu mode — display-only widget, hotkeys disabled, an
      external UI (Wheeler / 3rd-party) drives selection
- [ ] Auto-focus makes a non-Huginn wheel unreachable by opening. With
      `bAutoFocusOnOpen=true`, every fresh open that lands on someone else's
      wheel is redirected to Huginn's first — so a player wheel dragged to
      position 0 is skipped on every open, forever, and reads as Huginn having
      eaten it. Wheeler opens at the front by default, which is why position 0
      is the case that gets reported. **Mitigated, not fixed (0.19.3, corrected
      in 0.19.9):** a one-shot log warn + `RE::DebugNotification` fires when a
      redirect actually skips a wheel — gated on `wheelIndex < autoFocusTarget`,
      once per run. The original fired on ANY redirect and re-armed on every
      edit-mode exit, so it warned about wheels BEHIND ours that were reachable
      by scrolling right, four times in nine minutes. The behaviour it warns
      about is still the default. Options if it stays a complaint: only
      auto-focus when the opened wheel is not adjacent to ours, honour a "the
      player scrolled here deliberately" signal, or default `bAutoFocusOnOpen`
      to false (XS/S)

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
**Nothing in this tier exceeds 0.10% of runtime** on the 44:40 capture of
2026-08-26, which is the only capture long enough to trust — the 5-15 minute
runs that set the original ranking were dominated by cold calls. #14 is archived
(parts 1-3 merged, part 4 dropped on that measurement). On CPU grounds the rest
is a budget list, not a work list; treat a felt stutter, not a µs figure, as the
trigger to pick any of it up.
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
- [ ] Wheeler leaves an empty unmanaged wheel behind when a client's wheels were
      the only ones, and it PERSISTS — `SerializeIntoJsonObj` skips only managed
      wheels, so the placeholder is written to the co-save as `{"entries": []}`
      and rebuilt on load like a user wheel. Bounded at one (the next teardown
      finds it still there, so the list never empties again), but permanent once
      a player has it, and confirmed in-game 2026-08-29 at index 3 after a
      teardown+recreate. Upstream introduced it in `ca2e2f2` because
      `MoveEntryForward/BackInCurrentWheel` (Wheeler.cpp ~911/926) deref
      `_wheels[_activeWheelIdx]` behind an `_activeWheelIdx != -1` test that
      never fires — a full audit confirmed those are the only two unguarded
      `_wheels` accesses. Fix: guard both on `_wheels.empty()`, then drop the
      `push_back`. NOT `_activeWheelIdx = -1` — `AddWheel`/`PushWheel` never
      touch the index and `API_CreateManagedWheel`'s `if (activeIdx >= index)`
      fixup misses -1, so a -1 would survive into a non-empty list. Dropping the
      push does not retroactively remove one already saved; decide whether that
      needs a cleanup path (S)
- [ ] `ValidateWheelState` emits ~11 desync warns during a Wheeler edit-mode
      session — stale by construction, since Huginn has no signal that indices
      moved until edit mode exits, and the exit re-resolve corrects everything
      1.4 s later. Observed 2026-08-29 13:03:18 against a 13:03:20 re-resolve.
      Skip the check while `IsInEditMode()` — the diagnostic cannot say anything
      true there (XS)
- [ ] Remembered wheel position does not survive a game restart — it is a
      session member (`WheelSync::m_rememberedAnchor`), so a player who drags
      the wheels and quits finds them back at `sWheelPosition` next launch.
      Fixing it means a cosave record for one int32_t, which is cheap in itself
      but is a serialization change and so NOT landable during an active soak
      run (same constraint parked #15/#16). The cheaper half-measure, if this
      turns out to be what people actually hit, is writing the anchor back to
      `sWheelPosition` in Huginn.ini on teardown — no cosave, but it edits the
      player's config file behind their back, which is its own surprise (S)
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
      firing. Silence it so a real rejection stays visible (the negative case is the
      byteLen-mismatch block in RunCosaveTests, Tests.cpp:5159).
      Still firing every session as of 0.19.0; it has now cost real time twice
      while triaging unrelated logs (XS)
- [ ] `build-verify-preset` (1 commit) — no-deploy configure preset, rebased
      onto main and replaying cleanly, but **contents never reviewed**. The last
      unmerged branch holding work. Read the diff before merging; it needs a
      version bump per the per-PR convention, which it does not currently carry.
      Stale remote branches that can be deleted, all merged: `tier3-14-display-push`
      and `soak-skip-telemetry` (PR #96 / #95, 2026-08-26),
      `wheeler-respect-moved-wheels` (PR #97), `docs-optimizations-fold` and
      `wheeler-index-reresolve` (earlier). Verified against `git ls-remote`
      2026-08-29 — `build-verify-preset` is the only one still holding work
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
