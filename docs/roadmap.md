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
- [x] #10b: WildcardManager reached up for the live slot count. ScoreCandidates
      now takes the tick's displaySlotCount and passes it through; the legacy
      one-arg ApplyWildcards overload is deleted and slot/SlotAllocator.h is out
      of learning/. Resolves TODO(page-consistency) — wildcards can no longer be
      sized against a page the pipeline isn't allocating. **learning/ now has no
      code dependency on slot/ machinery.**
- [x] #10: extract UpdateLoop reward policy into ProcessInventoryChanges(registry,
      tag) — item/scroll consumption blocks were duplicated verbatim (PR #66).
      Also the enabling step for the weapon/ammo OnItemUsed hook below.
- [ ] #10: split WheelerClient.cpp (~1.3k lines) — the coupling behind the
      finding-3 race surface. **State-ownership pass done**; it settled three
      questions and changed the plan:
      * `m_api` → **Connection**. Single writer (TryConnect); everyone else
        reads. Never actually contested — a dependency, not shared state.
      * `m_pageWheels` / `m_pageDataMutex` / `m_addFailCooldowns` → **WheelSync**,
        exclusively. Callbacks get query+mutator methods, never a `PageWheel&`.
      * `m_wheelVisible` / `m_pendingWheelClose` / `m_pendingCloseWheelIndex` /
        `m_itemActivatedWhileOpen` / `m_callbackMutex` → **Session**.
      Gives `Connection ← WheelSync ← Session`: acyclic, and the existing
      `m_callbackMutex` → `m_pageDataMutex` order survives as "Session may call
      WheelSync, never the reverse". Seam-defining functions are
      `CheckPendingWheelClose` (touches all three), `HandleWheelOpened`,
      `OnItemActivated`, `HandleWheelClosed`.
      The finding that changed the plan: the callback layer is **not** a thin
      translator. Those four functions reach outward into seven subsystems
      (SlotLocker, SlotAllocator, CandidateGenerator, EquipSourceTracker,
      EquipEventBus, IntuitionMenu, WheelerSettings) — the same reach-up shape
      as #10a/#10b, and most of the 1.3k lines. Step 3 is therefore an
      inversion job (injected callbacks, `ExternalEquipLearner::Environment`
      pattern), not a file move.
      Three hazards a naive extraction reintroduces: `HandleWheelOpened` inlines
      `FindPageForWheel`'s loop specifically to avoid recursive locking;
      `m_api` was a plain non-atomic pointer read from Wheeler's callback thread;
      lock ordering had no enforcement beyond both mutexes being private to one
      class. Public surface is only 12 call sites in 7 files, so a forwarding
      facade keeps callers untouched and makes this three PRs, not one:
    - [x] Step 1 — Connection extraction (PR: wheeler-split-connection).
          `WheelerAPI.h` (ABI declarations) + `WheelerConnection` owning the
          handle, now `std::atomic`. Closes the version-reject publication
          window. Log output unchanged so the refactor diffs clean in-game.
    - [ ] Step 2 — WheelSync extraction, with `FindPageForWheel` given an
          explicit `…Locked()` variant so the recursive-lock hazard can't
          recur. Also fold in: make the load-once-per-function handle rule
          transitive by passing the handle into `SetEntrySubtext` /
          `ClearEntrySubtext` rather than having them re-load it — these call
          sites get rewritten by the extraction anyway (M)
    - [ ] Step 3 — Session + injected callbacks, cutting the seven outward
          reaches. The one with real behavioural risk (M)

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
