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
      **Now the top cost by both measures** (2026-08-23, Self-only): Display::Wheeler
      100.21 ms total / **3.23 ms MTPC** over 31 calls — that is its OWN work on the
      main thread, not children. See docs/refactor/wheeler-push-spikes.md, which
      already describes this path. Mostly S.
      **Three of four parts are written but STRANDED** on branch
      `tier3-14-display-push` (0.18.40 @63b1a62): pushed, never PR'd, and now
      behind main at 0.19.0 — it also carries the earlier roadmap prune. Decide
      whether to rebase or redo before starting fresh work here.
      Part 4 (lazy per-page allocation) was deferred pending a fresh capture.
      Two regressions in the first cut were caught ONLY by Tracy, not by the
      log: change-detect never fired because `confidence` (= `assignment.utility`)
      varies every run, and `GetLockSnapshot` was hoisted unconditionally while
      the old per-slot calls sat behind `stConfig.showLockTimerLabel` — turning
      zero lock acquisitions into one in the default config. Re-measure, don't
      assume
- [ ] #13 (= O1's sibling "O2"): count-only two-phase GetInventorySafe variant —
      Inventory::DeltaScan deep-copies InventoryEntryData per item at 2 Hz; plus
      PipelineContext container reuse (documented-but-broken).
      **Overtaken by #14 — measure before scheduling.** The 2026-08-23 capture
      puts DeltaScan at 257 µs MTPC / 24.46 ms total, well down from the figures
      below and now behind Display::Wheeler on both axes. The numbers that
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
      revision. Adjacent to #12 but not covered by it — #12 is PollTargets. At
      ~0.07% CPU this does not need fixing; it is where to look if the idle cost
      ever matters (S/M)
- [ ] #12: PollTargets — build outside the write lock, one classification pass (scanned
      up to 3×/tick), MAX_TRACKED_TARGETS 50→~12, squared-distance compares (M)
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
- [ ] Unmerged branches holding real work — decide rebase vs redo before either
      area is touched again. Both predate 0.19.0 and will conflict on the version
      bump at CMakeLists.txt:5:
      * `tier3-14-display-push` (0.18.40 @63b1a62, 2 commits) — #14 parts 1-3 +
        the two Tracy-caught regression fixes. Its roadmap prune is now
        superseded, so take the `src/display/` changes only
      * `build-verify-preset` (1 commit) — contents unreviewed
      `docs-optimizations-fold` and `wheeler-index-reresolve` are fully merged
      (0 commits ahead) and can be deleted
- [ ] Positive log line when the text-entry input gate engages — a
      transition-only `[InputHandler] Input suppressed — text entry active`.
      Today the gate is only verifiable by the ABSENCE of `KEY PRESS` lines,
      which is indistinguishable from any other reason input stopped, and
      unbound letters never log at all. Verified once by watching the widget
      instead; a log line makes it checkable and catches a regression (XS)
- [ ] Addendum #15/#16 (Kalman FQL / learnable context weights) — **parked**: needs a v3
      cosave bump, NOT landable during an active soak run
