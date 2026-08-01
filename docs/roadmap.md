# Huginn Roadmap

## Known Bugs
- [ ] Intuition menu shown during "cut scenes"
- [ ] Intuition menu not hiding when commanded by external mod
- [ ] New game wheelerAPI integration seems to fail
- [ ] #64: context label stamped on slots that context never ranked
      (`Ore Vein(Green Apple)`) — four contexts reproduce it; smallest fix of
      the group and the one to do first
- [ ] #60: `Falling` reads `IsInMidair()`, true for any jump — spikes slow-fall
      weight in the live ranking, not just the label
- [ ] #62: context reason flickers for a single tick (`Sneaking`, `Undead`,
      `Falling`), repainting all 8 labels; momentary states also outrank a
      sustained `Low HP`
- [ ] #61: `isUnderwater` uses exterior-only water height — interior water never
      registers, so the drowning override and water-breathing weight are dead
      indoors. Needs research (breath meter may beat geometry)

## Known Mod Compatability Issues
- [ ] #63: Requiem (LoreRim et al.) strips Fortify Smithing/Enchanting from
      alchemy, so the workstation context has no potion to rank — inert in the
      modlists that actually get play-tested. Live targets are filled soul gems
      and fortify apparel (#65); vanilla path needs its own regression test

## Known Recommendation Issues
- [x] Need to split Alcohol and food for recommendation engine slots
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
- [ ] Hide/toggle the Intuition menu on a hotkey — default key `x`
- [ ] Finer Intuition menu granularity — config sliders (position / scale / alpha)
      to one-decimal steps (00.0)  <!-- confirm: sliders, or on-widget numeric display? -->

## Architecture Critique — Backlog
See [reviews/architecture-critique.md](reviews/architecture-critique.md).
**Landed:** Tier 1 (all); Tier 2 #8 registry consolidation (PR #55), #9 display
abstraction (PR #56), #10 safe pieces — GetContextWeight move + ComputeRelevanceTags
dedup (PR #57), #10 leftover — relevance-tag encoding unified on ContextRuleEngine
(PR #58, merged; verified in-game across 5 Debug sessions — all 26 reason labels
observed, threshold parity exact on both smoothing exponents). Critique #10 is
now closed; #59–#65 are follow-ups it surfaced, not remaining critique work.

### Tier 2 — remaining
- [x] #10a: ExternalEquipLearner reached *up* into SlotAllocator/WheelerClient —
      now an injected Environment of live queries, wired in Main.cpp. Both stay
      live rather than read from PipelineStateCache: the cache only records the
      page current at snapshot time, so sourcing both sides from it would
      collapse attribution case D into E.
- [ ] #10b: WildcardManager still reaches up — WildcardManager.h:5 includes
      slot/SlotAllocator.h for the legacy ApplyWildcards overload, which reads
      GetSlotCount() live (:48), and UtilityScorer.cpp:197 calls exactly that
      overload. Pass the pipeline's ctx.displaySlotCount at the call site and
      delete the legacy overload — also resolves the TODO(page-consistency) at
      UtilityScorer.cpp:191 (live slot count can disagree with the snapshotted
      page mid-switch). Behavior change, so its own PR (S)
- [ ] #10: split WheelerClient.cpp (~1.2k lines) into Connection / WheelSync / a thin
      callback translator (M/L) — the coupling behind the finding-3 race surface
- [ ] #10: extract UpdateLoop reward policy into ProcessInventoryChanges(changes, tag)
      (item/scroll consumption blocks are duplicated verbatim) (S/M)

### Tier 3 — hot-path perf (trace-prioritized; see docs/profiling/tracy-traces.md)
- [ ] #14: gate the display push paths — IntuitionBackend change-detect, WheelerBackend
      lazy per-page allocation, GetLockSnapshot, quantize lock-timer subtext.
      *Biggest per-call cost in the traces (Display::Wheeler).* Mostly S
- [ ] #13: count-only two-phase GetInventorySafe variant — Inventory::DeltaScan
      deep-copies InventoryEntryData per item at 2 Hz; plus PipelineContext container
      reuse (documented-but-broken) (M)
- [ ] #12: PollTargets — build outside the write lock, one classification pass (scanned
      up to 3×/tick), MAX_TRACKED_TARGETS 50→~12, squared-distance compares (M)
- [ ] #11: cache SpellData.effectiveCost — CandidateGenerator calls LookupByID +
      CalculateMagickaCost per known spell per tick, inside the registry lock (M)

### Follow-ups
- [ ] Unit tests for Context::WeightForCandidate (Tests.cpp:2656/3374 currently
      hand-reimplement the weight mapping — call the real one). DominantReason /
      ReasonLabel are covered by unit test 17.
- [ ] #59: get docs/ under version control — `docs/ARCHITECTURE.md` is linked from
      CLAUDE.md and is still untracked
- [ ] Cosave decode negative test logs `[E] DecodeV2EntryBlob: byteLen 83 != stride
      84` at every Debug startup. The test passes — the error is the assertion
      firing. Silence it so a real rejection stays visible (Tests.cpp:4137)
- [ ] Addendum #15/#16 (Kalman FQL / learnable context weights) — **parked**: needs a v3
      cosave bump, NOT landable during an active soak run
