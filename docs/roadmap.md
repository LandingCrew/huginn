# Huginn Roadmap

## Known Bugs
- [ ] Intuition menu shown during "cut scenes"
- [ ] Intuition menu not hiding when commanded by external mod
- [ ] New game wheelerAPI integration seems to fail

## Known Mod Compatability Issues


## Known Recommendation Issues
- [x] Need to split Alcohol and food for recommendation engine slots

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
dedup (PR #57).

### Tier 2 — remaining
- [ ] #10 leftover: unify the relevance-tag encoding — derive the subtext label from
      ContextRuleEngine's continuous curves instead of ComputeRelevanceTags' hard
      thresholds (behavior-changing; needs design, was excluded from the "safe pieces")
- [ ] #10: WildcardManager / ExternalEquipLearner reach *up* into SlotAllocator/
      WheelerClient — pass slotCount/page state down from the coordinator (S)
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
- [ ] Unit tests for the now-free pure functions: Context::WeightForCandidate,
      Candidate::ComputeRelevanceTags, Slot::DeriveExplanationLabel (Tests.cpp:2656/3374
      currently hand-reimplement the weight mapping — call the real one)
- [ ] Addendum #15/#16 (Kalman FQL / learnable context weights) — **parked**: needs a v3
      cosave bump, NOT landable during an active soak run
