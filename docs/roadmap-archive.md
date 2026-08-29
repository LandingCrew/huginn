# Huginn Roadmap — Archive

Completed items moved out of [roadmap.md](roadmap.md) to keep the live list
scannable. Kept rather than deleted: several entries record why an approach
was rejected or what a fix turned up, which is the part that stops it being
re-litigated. Section headings mirror the roadmap's.

## Known Bugs
- [x] Display push was not gated on Wheeler edit mode, so the seconds a player
      spends rearranging wheels were spent writing subtexts to indices that were
      already stale. NOT covered by the re-resolve, which triggers on edit-mode
      EXIT by design — mid-edit indices are transient and re-resolving each
      intermediate state would chase noise — so the entire editor session was a
      window where a stored index could name another client's wheel. And
      `IsManagedWheel()` cannot catch it: it answers "is this wheel managed",
      not "is it ours", so the write is accepted on someone else's wheel.
      **First observed 2026-08-22**, in the session that verified the re-resolve:
      the shift was detectable at 10:23:48 but not corrected until 10:23:55, with
      `[Subtext] page 1` at 10:23:48.342 and `page 2` at 10:23:51.874 in between.
      Those two lines were read at the time as pushes landing on the wrong wheel.
      They are not: `[Subtext] page N` is emitted by `PipelineCoordinator`
      UPSTREAM of `WheelerBackend::Push` and fires whether or not the push runs,
      so it is evidence the pipeline ticked, not that Wheeler was written to.
      Treat the 08-22 wrong-wheel writes as unproven — the hazard below stands on
      its own.
      **Much better evidence 2026-08-28.** `ValidateWheelState` caught the whole
      stale state in one tick at 13:27:46.652: `Page 0 wheel 1 is no longer
      managed!` followed by 13 slot desyncs whose FormIDs show a clean
      off-by-one — page 1 reading page 0's contents, page 2 reading page 1's.
      Edit mode opened 13:27:41.098, exit re-resolve repaired it at 13:27:49.849:
      a 3.2 s stale window inside an 8.7 s editor session. Note the wheel was
      CLOSED (`isOpen=false` at 13:27:46.594) while edit mode was still active,
      which is why the existing `IsWheelOpen()` early-out never covered this.
      No push happened to land inside that particular window, so the hazard was
      confirmed live while the harm stayed conditional.
      **Fixed in 0.19.4** by an `IsInEditMode()` early-out in
      `WheelerBackend::Push`, placed deliberately AFTER
      `RecoverInvalidatedWheels()` — recovery is what rescues a dead wheel, and
      gating it behind edit mode would let an editor session the player never
      exits strand the wheels for good. Bounded and self-correcting either way:
      the exit re-resolve clears the slot cache, so the next push repaints from
      scratch with no special case in the backend.
      **Verified in-game 0.19.5**, once the gate was given a transition log of
      its own — it had none, and the `[Subtext]` line cannot stand in for one
      (see above). 2026-08-28: `WheelStateChanged: wheel=3, isOpen=false` at
      14:56:44.801, `[Wheeler] Push suppressed — edit mode entered` at
      14:56:44.813. Note how narrow the gate's real scope is: the open-guard
      above it covers most of an editor session, leaving only the
      closed-but-still-editing tail (this one, plus 1.8 s and 3.9 s tails in the
      14:31 and 14:32 sessions) and the urgent-override path, which bypasses the
      open-guard by design. Both are real and neither is covered elsewhere, but
      the gate is not the whole-editor-session shield the original entry implied
- [x] Wheel ORDER was not preserved across a save load. Huginn deletes and
      recreates its wheels in `InitializeGameSystems`, and creation passed
      `sWheelPosition` (default `First`) — so any reordering the player did in
      Wheeler's edit mode was silently undone on the next load and our wheels
      jumped back to the front. **Observed 2026-08-28** across two sessions: the
      11:27:49 reorder put us at 1/2/3 and all four following loads recreated at
      0/1/2; the 13:04:52 reorder put us at 2/3/4 and the 13:05:54 and 13:06:44
      loads did the same. In-session reordering was already handled (that is
      what the re-resolve landed for) — this was purely the create path ignoring
      where the wheels had ended up.
      **Fixed in 0.19.4** by `WheelSync::m_rememberedAnchor`: `DetachWheelsLocked`
      records where the player left the block, and `CreateWheels` prefers that
      over `sWheelPosition`. Three decisions worth keeping:
      (1) ONE anchor for the block, not a position per page. Creation already
      assumes our wheels are contiguous (`basePosition + pageIndex`), and
      replaying an arbitrary interleaving through sequential inserts would need
      every recorded index corrected for each insert before it. An anchor cannot
      drift that way; a player who interleaved gets the block back grouped
      rather than scattered wrong.
      (2) The anchor is read at TEARDOWN, not creation — teardown is the last
      point that still knows. A generation where every page was invalidated
      yields nothing and deliberately does NOT clear an anchor already held:
      "I can't see where they were" is not "they were at the front".
      (3) `ForgetRememberedPosition()` fires from `SettingsReloader` whenever
      the captured `WheelLayout` changed — which includes `sWheelPosition` — so
      editing the INI still moves the wheels. Without it the setting would go
      inert the first time anyone dragged a wheel: a worse surprise than the one
      being fixed.
      **0.19.4 shipped (3) in the wrong order and it did not work.** The forget
      ran BEFORE `DestroyRecommendationWheels()`, and the destroy re-reads the
      live indices and records the anchor again — so the forget was undone in the
      same breath and the INI stayed inert, which is precisely the surprise (3)
      exists to prevent. Caught by reading the call order, then confirmed
      in-game: with `sWheelPosition = 3` and the block dragged to 1,
      `Forgetting remembered wheel position 1` at 14:42:13.256 was followed by
      `Remembering wheel position 1 (was -1)` in the same millisecond and the
      wheels were recreated at 1/2/3. **Fixed in 0.19.5** by moving the forget
      after the destroy, so it lands in the window between teardown and creation
      where nothing can re-record. Same setup now logs Remember -> Forget ->
      `Created wheel for page 0 'Smart': index=3` (14:56:28.359), wheels at
      3/4/5. Worth remembering as a shape, not just a bug: `DestroyWheels()` has
      a side effect that any "reset this state" call placed near it must be
      sequenced against.
      Session-scoped by choice. Surviving a restart needs a cosave record, which
      is a serialization change and not landable mid-soak; left as a follow-up
      on the roadmap with the INI-writeback alternative noted and rejected for
      editing the player's config behind their back
- [x] #76: loading a second save in one session (esp. a different character)
      left Huginn's stored wheel indices stale; `UpdatePage` found every page
      unmanaged ~1s after creation and set `wheelIndex=-1` permanently, with no
      self-correction — the wheel stayed empty until `hg reload` or another
      save-load. Root cause was the same one behind the edit-mode entry below:
      a positional index used as identity. Fixed in 0.18.34 by making
      `UpdatePage` re-derive the index from the client label before writing the
      wheel off, so -1 is latched only when `GetManagedWheelsForClient` reports
      the wheel genuinely gone (count == 0); a real deletion still falls through
      to `RecoverInvalidatedWheels`.
      **VERIFIED IN-GAME 2026-08-28**, one session, v0.19.2 (30a99a3) Debug,
      Wheeler API v4, `_Huginn_Debug.log` 11:17:46–11:40:20. The entry asked for
      "either a `Re-resolve: page N ... index X -> Y` line or a clean run"; the
      session produced both.
      *Re-resolve, edit-mode trigger* — 11:27:40 enter, 11:27:49.919 exit with a
      player wheel inserted at the front. All three pages shifted +1
      (`Smart 0 -> 1`, `Inventory 1 -> 2`, `Regulars 2 -> 3`), each with its
      paired `resetting … dropping N exported subtext(s)` / `reset done`
      bracket — no unpaired first line, so no dangling-pointer window. Drop
      counts 1, 6, 0: seven live `const char*` exports the old code would have
      left Wheeler rendering from. Post-shift the mapping was correct in both
      directions — opening wheel 0 logged `not an Huginn wheel` and auto-focused
      wheel 1 (11:27:55, 11:28:31), opening wheel 1 logged `Page 0 'Smart' wheel
      opened` (11:28:21/24/26) — and three activations on `wheel=1` resolved to
      `Item activated on page 0 wheel`, so the wheel repopulated and stayed
      usable at its new index.
      *Clean run, the actual #76 repro* — four loads alternating characters,
      identified by cosave restore size: 11:29:18 and 11:35:17 (17 FQL entries),
      11:30:12 and 11:37:17 (1 entry). All four recreated at 0/1/2. The
      11:37:17 load got ~3 minutes: wheel opens mapping correctly to pages 0 and
      1, four activations, five `[Subtext]` pushes across both pages (so the
      content path churned rather than sitting behind `UpdatePage`'s
      content-unchanged early-out), and 13 `ValidateWheelState: checked 3 pages`
      passes. Across all 3,612 lines: zero `no longer managed`, zero
      `#76 census`, zero `#76 recovery`, zero `Re-resolve … marking invalid`,
      zero ambiguity warnings.
      Worth keeping: the shift case was observed on the EDIT-MODE trigger, not
      the save-load one. The load path never went stale here — it destroys and
      recreates, and the indices came back identical every time — so the
      `UpdatePage` re-resolve that the fix was written for remains unexercised
      by a real shift. It is closed on the strength of the mechanism being
      proven on the other trigger plus four clean loads, not on having watched
      the original failure recover.
      One false positive turned up and is NOT a bug: `ValidateWheelState: Page 1
      slot 3 desync: cached=FE00E9E7, actual=00000000` at 11:40:11.829, 87 ms
      after an activation and before the deferred close handler ran at
      11:40:12.044. The diagnostic has no guard against the activation window,
      so it fires whenever it lands between an activation and
      `MarkActivationEmptied`. Debug-only, self-correcting; gating it on a
      pending close would silence the class
- [x] Wheel indices were never re-resolved after Wheeler reindexed, so subtext
      writes landed on the wrong managed wheel. Same root cause as #76
      (positional index used as identity), different trigger: #76 is a save-load
      rebuild, this was an in-session edit-mode reorder.
      **Observed 2026-08-21:** wheels created at 16:05:51 as Smart=0,
      Inventory=1, Regulars=2; three edit-mode cycles each reported
      `changeCount=0`; wheeler.log then showed Smart=1, Inventory=2, Regulars=3
      with a player wheel displacing 0, while Huginn kept writing to 0/1/2. The
      writes were ACCEPTED — `IsManagedWheel` answers "is this wheel managed",
      not "is it MINE", so the ownership pre-check in `UpdatePage` passes on
      another client's wheel and structurally cannot catch this.
      **The plan's premise was off by one API version.** `GetManagedWheelsForClient`
      is **v4**, not v3, and Huginn's mirrored `WheelerAPI.h` capped at
      `API_VERSION_MAX = 3` — the live log was already warning "API version 4 is
      newer than the 3 this build knows". The struct is append-only so nothing
      was broken, but the fix needed the mirror bumped first, which also pulls in
      v4's BEHAVIOUR CHANGE: managed wheels now survive Wheeler's load-time
      reset, so a client that recreates every load must delete first or
      accumulate duplicates. Huginn already deletes first (`InitializeGameSystems`
      → `DestroyRecommendationWheels`); that ordering is now load-bearing and
      documented at the version constant.
      **Landed as `WheelSync::ReResolveWheelIndices()` on two triggers:**
      (1) edit-mode EXIT, ignoring the payload entirely — `Wheeler::exitEditMode()`
      always passes `(nullptr, 0)` (upstream TODO), so gating on it would gate on
      a constant; (2) the `UpdatePage` invalidation site, which now tries to
      re-derive the index before writing the wheel off. That second trigger is
      what **also closes #76** — and it beats the planned `kPostLoadGame` hook,
      which would have been a no-op: load already does Destroy+Create and assigns
      fresh indices, while #76's failure lands ~1s LATER. Latching -1 as the
      first response was the actual bug; it is now the answer only when the
      lookup reports the wheel genuinely gone (count == 0).
      Cached slot state is deliberately NOT cleared on a move — the wheel's
      contents moved with it, only the address changed. `count > 1` warns
      (the v4 duplicate hazard). v3 and below no-op with one warning per
      generation: no v3 call distinguishes our managed wheels from another mod's.
      **Verified in-game 2026-08-22** (0.18.34 @9aae02d, md5-matched deploy).
      `ValidateWheelState` went clean -> broken -> clean across one edit-mode
      cycle: at 10:23:48 it reported `Page 1 wheel 1 is no longer managed` plus
      six page-2 slot desyncs in an unmistakable one-position shift (slots 1 and
      2 holding each other's forms, `cached=FE803801/actual=0401CDAD` and the
      exact mirror); at 10:23:55 exit fired `Re-resolve: page 2 ('Huginn:
      Regulars') wheel index 2 -> 3`; every later validation pass is clean with
      zero desyncs. wheeler.log confirms independently — three
      `GetManagedWheelsForClient` hits, one per page, then `Switched to managed
      wheel 3 (client: Huginn: Regulars)`. Both edit-mode events reported
      `changeCount=0` while the indices HAD moved, so ignoring the payload was
      load-bearing, not defensive. Inventory logged no re-resolve line (settled
      back at 1) — unchanged pages are silent by design.
      **Re-verified 2026-08-22 after review fixes** (0.18.35 @0bf6e99): a wheel
      inserted at position 0 shifted ALL THREE pages (0->1, 1->2, 2->3), so the
      reset-on-move path ran three times. Repopulate produced a correct wheel —
      8/8 slots `Confirmed`, subtexts rewritten, validation clean, and zero
      AddItem rejects or retry suppression, which was the specific risk of
      emptying rather than resyncing. Downstream mapping followed the move too,
      not just the push path: `synced page to 0 (wheel 1)` and `Page 0 'Smart'
      wheel opened` on `WheelStateChanged: wheel=1`.
      **Not log-verifiable:** the dangling-`const char*` fix. A freed-string read
      is silent corruption that may never fault, so a clean session is not
      evidence — it rests on the code reasoning (UpdatePage frees a retiring
      subtext right after exporting its replacement AT pw.wheelIndex, which only
      held while the index never moved). Re-check it by reasoning, not by log,
      if this code is touched again
- [x] #64: context label stamped on slots that context never ranked
      (`Ore Vein(Green Apple)`). A reason may now speak for a slot only where
      the candidate draws weight from the field that reason is read off
      (`Context::ReasonAppliesTo`), probed through the real
      `WeightForCandidate` so there is no second copy of the tag→weight table.
      The reason→field pairing moved out of `DominantReason`'s inline field
      names into one `WeightFieldFor` table that both consumers read, so a
      reason cannot fire off one field and be attributed off another.
      **Surfaced:** `unlockWeight`, `slowFallWeight` and `antiDragonWeight` are
      written by `EvaluateRules` and read by NO candidate mapping — the
      LookingAtLock / Falling / TargetDragon contexts have never influenced a
      ranking, and their label was the only evidence they existed. They now
      correctly label nothing. Tracked as #79 (PR #78)
- [x] #60: `Falling` read `IsInMidair()`, true for any jump, so each hop blanked
      the dominant reason for a tick (enumerator 9 outranks every target/combat
      reason). Now measures descent below the take-off Z: a jump returns to its
      own height so its depth stays ~0 by construction, `isFalling` gates at 200
      and the weight ramps to full at 600 (PR #82).
      **Two things it turned up.** `GameState::GetHash()` omits falling
      entirely, so `CheckHashSkip` skipped any tick where only the fall changed
      — a peak depth of 567 against a 400 threshold still reported nothing.
      Masked before, because jumps happen mid-combat where the hash moves
      anyway; a real fall changes no hashed bucket. Fixed with the same
      bypass + falling-edge run the elemental window uses, NOT by adding a
      learner state dimension (cosave bump). And the take-off Z survived save
      loads in the first draft — `ResetTrackingState` zeroes the poll timers, so
      loading into a low interior from a peak would have reported a
      ~20,000-unit fall at full weight.
      **Was inert downstream:** no slot wore `Falling`, because
      `slowFallWeight` was read by no candidate mapping. Closed by #79 —
      `SpellTagExt::SlowFall` draws it, though only a modded slow-fall spell
      can carry the tag (vanilla has none, and Become Ethereal is a shout)
- [x] #62: context reason flickered for a single tick (`Sneaking`, `Undead`,
      `Falling`), repainting all 8 labels before they could be read. Each firing
      was CORRECT — the player really was crouching — so this was display
      stability, not detection, and had to be fixed separately from #60.
      `Context::ReasonHold` damps the LABEL only; `ScoreCandidates` keeps using
      the instantaneous weights. Asymmetric and priority-aware: a more urgent
      reason (lower enumerator, the same ordering `DominantReason` resolves ties
      with) is adopted instantly, so `Critical HP` never waits behind a stale
      `Sneaking`; a release to `None` or a LESS urgent reason waits out
      `REASON_HOLD_MS` (1500, chosen below SlotLocker's 3000ms content lock so a
      label cannot outlive the contents it explains). Priority-awareness is what
      also covers the same-band swap (`Undead → Outnumbered → Undead`) that a
      plain release-delay would miss. Reset on save load — a held reason
      describes the character just unloaded
- [x] #61: `isUnderwater` compared the player's head against
      `cell->GetExteriorWaterHeight()` — one water plane for the whole cell — so
      it was wrong wherever the local water is a placed object at its own
      height. Worse than the "interior water never registers" it was filed as:
      observed 2026-08-08 fully submerged in the river at Graywinter Watch,
      exterior, with no Underwater reason logged all session.
      **The open question answered itself in CommonLibSSE, no breath meter
      needed.** `TESObjectREFR::GetWaterHeight()` reads the engine's
      `relevantWaterHeight` for that reference — the water it is actually in —
      and only falls back to the cell plane when there is none. The old call was
      literally the fallback path of the right one, so the fix is one line and
      closes both halves at once. Same head-vs-surface test, right surface; the
      +120 offset still rejects ankle-deep water (PR #85).
      **Verified in-game:** the same river that failed an hour earlier, and an
      interior cave pool — both labelling spell AND potion. That also closed
      #79's last unverified link, which had no other way to be tested.
      **Both dead consumers came back**, including the drowning override, which
      had never once run in production. It behaves correctly: what looked like
      double-placement was allocation running per page with a log line that
      omitted the page (now fixed).
      **Still open:** actionability — whether Skyrim lets the player use an item
      at all while swimming. Detection being right does not make the drowning
      override usable if the engine blocks interaction at that moment (same
      question #60 raised). Tracked on the issue

- [x] #74: 13–15 `AddItemByFormID` rejects (`-6 UnsupportedFormType`) ~1s after
      save-load, then clean for the session. **Root cause found:** every failing
      item is a weapon with `uid=0`, which Wheeler is documented to reject
      (changelog v0.11.3). Weapons load with `uniqueID=0` because the load-time
      scan skips extraLists (`EXTRALIST_STABILIZATION_MS`=500), and the primed
      reconcile that fills them runs at `WEAPON_RECONCILE_RETRY_MS`=1000. The
      first Wheeler push lands at 981–998ms across three observed loads — a coin
      flip on ~100ms tick phase against that 1s deadline, hence ~2-of-3.
      Not a Wheeler bug and not from the split. Fix: skip the call when a weapon
      has `uniqueID==0` rather than spending a retry; must not consume
      `slotRetries` (3 would poison the entry for 30s) nor log above trace (S)

- [x] `DeleteManagedWheelsForClient` reported deleting ZERO wheels for a client
      that had one, orphaning it, after which the next CreateWheels added a
      SECOND under the same name. Observed 2026-08-22 while testing #92.
      **Cause, upstream.** `API_DeleteManagedWheelsForClient` collected its
      matches, then broke out of the erase loop when the TOTAL wheel count
      reached 1 and returned however many it managed — a short count, possibly
      0, with no error and nothing to distinguish it from "you had none". The
      single-wheel `DeleteManagedWheel` enforces the same invariant honestly,
      returning `LastWheel`. The "third call, after a call that deleted 2
      wheels" detail was causation, not the correlation this was filed as: that
      call is what drove the count to 1. The arming state is ordinary —
      `clearUnmanagedLocked` keeps managed wheels and drops unmanaged ones from
      `_wheels`, so after a load every wheel present can belong to a client.
      **Ruled out on the way, and worth keeping:** `WheelManagedInfo::clientName`
      is a `std::string` and `EntrySubtext::text` is a `std::string` — Wheeler
      COPIES both exported strings. Huginn's comments had modelled them as
      indefinite borrows; that model was wrong, and adoption (below) silently
      depends on it being wrong. Corrected in place; the heap-stable storage
      stays because unwinding it would touch every export path to buy nothing.
      **Fixed upstream** (wheelerAPI `ca2e2f2`, merged `567f947`): every match is
      erased and the count is exactly the number that matched. `_wheels` is kept
      non-empty by pushing an empty UNMANAGED wheel when the last would go —
      `MoveEntryForward/BackInCurrentWheel` deref `_wheels[_activeWheelIdx]`
      behind an `_activeWheelIdx != -1` test that never fires, so emptying the
      vector outright was unsafe. See the open follow-up on that placeholder.
      **Fixed Huginn-side in PR #99** (0.19.7–0.19.10), kept regardless because
      the upstream fix ships inside API v4 with no version bump to detect it by:
      `IssueWheelDeletes` verifies each delete with `GetManagedWheelsForClient`
      instead of trusting the count; `CreateWheels` ADOPTS a wheel already
      answering to a page's label rather than creating a second one; anything no
      live page claimed is reaped after creation, where the count has risen past
      the old guard. v4 only.
      **The review round is the part worth remembering.** Both the retry and
      adoption could leave a stored index describing the wrong wheel — the retry
      by deleting a wheel below ours (shifting every higher index down), adoption
      by taking a wheel at an arbitrary index that later pages' inserts then push
      along. Neither self-corrects, because both leave the stale index pointing
      at a wheel that is still MANAGED, and `UpdatePage` only attempts recovery
      when `IsManagedWheel` says otherwise — so a page would write into the next
      page's wheel indefinitely. Fixed by re-deriving every index from its label
      after the retry and BEFORE `m_createdAnchor` is computed, which was reading
      the stale indices too. Any future insert/delete during creation needs the
      same treatment.
      Adoption and the retry cannot fire against a patched Wheeler (nothing is
      ever stranded), so they rest on review rather than in-game evidence. The
      always-on addition — the post-create re-resolve — was verified across nine
      page-resolutions in both `First` and `Last` positions
## Known Recommendation Issues
- [x] #70 + the wildcard cluster — three entries, one root cause: the wildcard
      cache was a single global position-indexed array shared by every page, so
      an entry could sit at an index nothing currently displayed could reach
      while the liveness scan still counted it and suppressed the re-roll that
      would have produced a usable one. Reachable three ways: **#70**, a switch
      to a smaller page stranding entries above its slot count (30s + refractory
      of no wildcard at all, not self-correcting); a page whose slots all set
      `bWildcardsEnabled=false`, which WildcardManager could not see because it
      received a slot COUNT and never per-slot enablement; and the `slotCount <
      2` guard returning before #70's bounds repair, so a 1-slot page kept
      whatever was already there.
      **Fixed in 0.19.6** by the pageIndex → array cache the open entries named.
      Each page owns its entries, its cooldown and its refractory timer, and
      records the page SHAPE (slot count + wildcard-capable slot count) it was
      rolled against; a shape change discards that page's cache wholesale. A
      page switch therefore needs no repair at all — page A's entries stay under
      page A's key — and the only surviving invalidation path is an INI reload
      actually reshaping a page, which is a real event rather than a routine one.
      Enablement arrives as `SlotAllocator::GetWildcardSlotCount(page)`,
      snapshotted into `PipelineContext::displayWildcardSlots` and passed with
      the index and slot count as one `Scoring::WildcardPage` value.
      Note it is a page-level CAPACITY, not a per-index map, and deliberately so:
      allocation walks slots in priority order and hands a wildcard to whichever
      slot's classification matches, so which slot takes it is unknowable in
      advance — what is knowable is that a page with N wildcard-accepting slots
      can never display more than N, and the roll loop stops there.
      The `slotCount < 2` guard is now `slotCount == 0`: slot 0 is already
      unreachable on its own terms (excluded by `bFirstSlotExcluded`, and scored
      at `base × 0 == 0` even when it is not), so a 1-slot page rolls nothing
      without needing a special case to say so.
      Behaviour change worth knowing: per-page timers mean one page's expiry no
      longer imposes a refractory on another. `UpdateExpiry()` still ages EVERY
      page — a wildcard on a page the player has switched away from must lapse
      on its own clock — and still REPORTS a lapse on any page. An intermediate
      cut narrowed that report to the page last applied, on the reasoning that
      ageing out a page nobody is looking at changes nothing on screen; review
      of PR #98 found the remembered page can be wrong, because
      `SlotAllocator::Initialize()` drops the display back to page 0 by
      assigning `m_currentPage` directly without raising `m_pageChanged`
      (SlotAllocator.cpp:70), so after an `hg reload` it stays stale for as long
      as the pipeline is hash-skipped — exactly the window the return value
      exists to break out of. Reverted: over-reporting costs one pipeline run
      that repaints the same content, under-reporting leaves an expired wildcard
      on screen.
      The `bWildcardsEnabled` case was logged as unconfirmed and "needs a config
      with wildcards disabled on some slots to reproduce"; it now has a unit test
      instead (Tests.cpp, "Wildcard page cache"), which pins probability to 1.0
      and refractory to 0 and asserts bounds across all four shapes plus the
      reshape and all-page expiry.
      **Verified in-game 2026-08-28**, 0.19.6 Debug. The suite passed at
      19:11:22 with the whole roll trace visible: page 0 (7 slots) rolled
      indices 1-6; page 1 (4 slots) rolled 1-3 with nothing above its bound —
      the #70 case; pages 2 (0 wildcard-capable) and 3 (1 slot) logged no roll
      at all; page 4 (6 slots / 2 capable) stopped at two; the reshape logged
      `Page 0 reshaped (3 slots / 3 wildcard-capable, was 7 / 7) — dropped 6
      cached wildcard(s)` and re-rolled within the new bound; and expiry fired
      once per live page, three lines for the three pages holding entries.
      Live play then exercised the normal path at 19:13:19 on the 3-page /
      8-slot LoreRim config: two rolls (Iron Battleaxe at ranking index 2, 33%;
      Unarmed at index 7, 50%), the battleaxe surfacing as `[WC]` in the Recs
      dump and displaying at `1:Wildcard` — display slot 1, not ranking index
      2, which is the capacity-not-index-map behaviour above showing up in the
      wild — then expiring at exactly 30 s into the configured 60 s refractory
      with the slot reverting to the ranked pick. No reshape log fired during
      play, so the invalidation check does not spam a stable config.
      **All five cases then verified against a live 3-page config** the same
      evening, with `fBaseProbability = 1.0` and a long cooldown so rolls were
      deterministic. Shapes: page 0 = 8 slots / 0 wildcard-capable, page 1 = 4
      slots / 4, page 2 = 8 slots / 8 (later 3).
      1. **#70** — 20:23:01.598 switched to page 2 (8 slots), rolled indices
         1-7; 20:23:02.674 switched to page 1 (4 slots) and it rolled indices
         1-3 within 2 ms, nothing above its bound. On the old shared array page
         2's entries at 4-7 would have kept `HasActiveWildcard()` true and
         suppressed that roll for the whole cooldown + refractory.
      2. **bWildcardsEnabled=false** — page 0 rolled ZERO wildcards across the
         entire session at probability 1.0, having rolled 7 at 19:43:43 and
         19:43:57 on the same page before the flags were flipped. Clean
         before/after on one page.
      3. **1-slot page** — page 1 at `iSlotCount = 1` with wildcards ENABLED on
         its only slot (so the slot-count path is isolated from the enablement
         one) rolled nothing at 19:57:45 and 19:58:06, and did not block pages
         0 or 2.
      4. **Per-page timers** — 20:23:20.831 logged `Wildcards expired` TWICE in
         the same millisecond: pages 1 and 2 ageing out on independent clocks
         while the player stood on page 0. A single global cache can only ever
         print that line once. Page 1's own 2 s refractory was then honoured on
         return (no roll at 20:23:21.532, roll at 20:23:23.375).
      5. **Reshape** — 20:34:53.507, `Page 2 reshaped (3 slots / 3
         wildcard-capable, was 8 / 8) — dropped 7 cached wildcard(s)`, followed
         immediately by rolls at indices 1-2 only and `[Recs] 3. Steel
         Rapier … [WC]`. Fired exactly once; no repeat on later ticks.
      Negative control worth keeping: at 20:33:11 a reload left page 1's shape
      untouched (4 slots before and after) and it correctly logged NO reshape
      line and did not re-roll — it kept the three wildcards it already held.
      Two gotchas for anyone re-running this. The cooldown is measured on
      `steady_clock`, so it keeps running while the game is alt-tabbed — a 10 s
      cooldown expires during any INI edit, and the reshape drop is gated on the
      page actually HOLDING wildcards, which is why that line took several
      attempts to catch. And the drop is lazy: it fires when `ApplyWildcards`
      next runs for that page, so reshaping a page you are not standing on logs
      nothing until you switch to it.
      One pre-existing behaviour this surfaced, and then fixed in review: the
      second roll (Unarmed, index 7) displayed as `7:Confirmed`, not as a
      wildcard. `ApplyWildcardsToRanking` only ever surfaces a wildcard by
      SWAPPING it up and skips the swap when `foundIdx <= slotIdx` — the
      candidate was already at or above that position on merit — and the
      `isWildcard` flag is set only by the swap. So a roll that drew a
      candidate already sitting at its own target cached an entry nothing
      displayed, while the cache read as active and suppressed the re-roll for
      a full cooldown. The origin predates this change; the CONSEQUENCE does
      not. The old loop rolled up to `slotCount - 1` entries, so a wasted draw
      was masked by its neighbours; under the new per-page capacity a page with
      one wildcard-capable slot rolls exactly once, and that draw IS the stall.
      `SelectRandomCandidate` now takes a `minIndex` and is called with `i + 1`,
      so every roll is surfaceable by construction rather than by luck. Note
      the trade this makes deliberately: when no eligible candidate exists below
      the target slot the roll produces nothing, which is correct — declining to
      roll is strictly better than caching something unshowable.
      **Post-fix verification, 2026-08-29 (Debug `e6e1174`)** — the trace above
      was taken before the review fixes, so the shipped code was re-walked.
      Unit suite green, and its own debug output corroborates each assertion
      independently: pages rolled 4 / 3 / 0 / 0 / 2 / 1 against shapes 7-of-7,
      4-of-4, 5-of-0, 1-slot, 6-of-2, 6-of-1, with nothing above page 1's bound;
      one reshape line; and exactly four `Wildcards expired` lines, pages 0, 1,
      4 and 5 — pages 2 and 3 had nothing to expire because they correctly
      cached nothing.
      In-game, 10:22:45 on an 8-slot / 8-capable page: four rolls at ranking
      indices 1-4, and `[VisualState]` the same millisecond reading
      `4:Wildcard … 5:Wildcard … 6:Wildcard … 7:Wildcard`, names matching
      one-for-one. Cached == surfaced, with the 1-4 → 4-7 offset showing
      ranking index and display slot diverging exactly as the capacity design
      assumes. Only four of a possible seven rolled: all four were potions, the
      draw filters to the top candidate's source type, and `minIndex` exhausted
      the pool below slots 5-7 — the decline-to-roll case above, observed.
      The `bWildcardsEnabled=false` case, logged as unconfirmed when the entries
      were opened, is now confirmed live: page 1 at 8 slots / 0 wildcard-capable
      displayed from 10:22:57 with every `[VisualState]` entry `Confirmed` and
      not one roll line for the rest of the session, while page 0 held four
      cached wildcards with ~290 s left. On the old shared array those entries
      were in-bounds for an 8-slot page, so `HasActiveWildcard()` read true
      while SlotAllocator refused to seat them on `w0` slots. A second negative
      control: `hg reload` at 10:22:54 with all three page shapes unchanged
      logged no reshape line and dropped nothing
- [x] Need to split Alcohol and food for recommendation engine slots
- [x] #80: weapons were outside contextual scoring entirely — the
      WeaponCandidate arm of `WeightForCandidate` read only `weaponWeight` /
      `damageWeight` / `baseRelevanceWeight` and checked ZERO tags (spells check
      ~12, items ~15), so a weapon's context weight was a constant: same in a
      barrow as in a shop. `WeaponTag::Silver` and `EnchantTurnUndead` were
      classified all along and `antiUndeadWeight` computed every tick; they had
      never been connected. Now `Silver | EnchantTurnUndead → antiUndeadWeight`
      and `EnchantBanish → antiDaedraWeight` (PR #81). Review caught Banish on
      the wrong axis: `Archetype::kBanish` returns summoned daedra to Oblivion
      and does nothing to draugr — mapped by name association, not by what the
      archetype does. `Bound → boundWeapon` dropped as unreachable: the weight
      fires only with nothing equipped, and conjuring a bound weapon auto-equips
      it. NOT the elemental enchants — keying on what a target resists is
      forbidden info, and there is a comment at the site saying so. Surfaced by
      #64 removing the label that hid it.
      **Also fixed a dormant classifier bug it activated:** `IsSilvered` falls
      back to a substring name match, and "Quicksilver" contains "silver", so
      every Quicksilver weapon carried `WeaponTag::Silver`. Inert while nothing
      read the tag; once #80 read it, an observed Quicksilver Greatsword took
      ctx 0.30 → 0.60 vs draugr. `NameContainsWord` now requires a leading word
      boundary ("Silvered Sword" must still match, so no trailing boundary).
      bound/daedric keep the loose match — no observed collision, tags still
      drive nothing. Verified in-game: `tags=00000019` → `00000009`
- [x] #79: `unlockWeight`, `slowFallWeight` and `antiDragonWeight` were computed
      by EvaluateRules and read by no candidate mapping — LookingAtLock,
      Falling and TargetDragon had never moved a ranking. `SpellTag` is at
      32/32 bits, so the spell side of those contexts was never wired
      (`ItemTag` solved the same squeeze with `ItemTagExt`). Waterbreathing was
      half-live for the same reason: the potion path worked, the spell path
      didn't. It was the bottleneck for three landed PRs — #60 made `Falling`
      fire correctly and it still surfaced nothing. Kept all four rather than
      deleting them; `SpellTagExt` closes all four in one shape (PR #84).
      **Detection is API-first, which the plan did not assume:** `kOpen` for
      Unlock, `kEtherealize` for SlowFall (Become Ethereal negates fall damage),
      `primaryAV == kWaterBreathing` for Waterbreathing. Only AntiDragon is
      name-matched, because nothing in a spell's data says "for dragons" — and
      it matches an explicit `dragonrend`/`dragonbane` list, since Dragonhide is
      a vanilla self-armour spell that bare "dragon" would have tagged (the #81
      Quicksilver shape, one enum over). Walks EVERY effect, not the costliest:
      these are typically the cheap rider on a multi-effect spell.
      **Two live bugs it turned up.** With no bit of their own, the classifier
      had been reaching the right SpellType by mislabelling — `waterbreath` was
      tagged `SpellTag::Stealth` and `open` was tagged `Telekinesis`. The first
      was not cosmetic: the spell arm reads Stealth into `stealthWeight`, so
      every waterbreathing spell was ranked as a SNEAKING tool. Waterbreathing
      was not dead, it was wrong.
      **Vanilla scope, honestly:** only Waterbreathing lights up unmodded.
      Vanilla ships no castable Open or Slow Fall spell and Dragonrend is a
      shout, which never reaches the spell registry. The other three are
      mod-facing (Apocalypse, Ordinator) — hence archetype-first, since a
      modded spell's name is unpredictable and its archetype is not.
      `NameContainsWord` moved to `util/NameMatch.h` on the way: the spell side
      needs the same word-boundary rule and must not depend on the weapon module

## UX / Feature Backlog
- [x] Two INI files defined `[Widget]`, `[Keybindings]` and `[Debug]` in BOTH
      `Huginn.ini` and dMenu's same-named copy, with nothing stating which won —
      and settings changed in the dMenu UI did not survive a restart.
      **The overlay was the cause, not dMenu.** A first attempt made `Huginn.ini`
      win by layering it over dMenu's file and added a `DMenuWriteBack` class to
      copy in-game changes back; that overwrote a dMenu change on the very reload
      dMenu fires to announce it. Both mechanisms were deleted in favour of
      removing the duplication: dMenu owns `[Widget]`+`[Debug]`, `Huginn.ini`
      owns `[Keybindings]` + the other 38 sections, nothing in both (verified
      against deployed configs: 9 dMenu keys, 225 main keys, zero overlap).
      Net -582 lines. Persistence verified across a game restart.
      **dMenu serializes a dropdown as its integer INDEX, never its label**, so
      `sDisplayMode`/`sSlotEffect`/`sRefreshEffect` needed a parser that accepts
      an index or a name before they could be exposed — added as dropdowns they
      would otherwise have failed the name match and silently taken the default.
      Switch order is pinned to the JSON `options` arrays with a comment on both
      sides. Five `[Widget]` keys were briefly homeless mid-change (deleted from
      `Huginn.ini`, not yet in dMenu's schema); all ten now live in the schema.
      **Trade-off accepted:** dMenu is required to configure the widget UI —
      without it `[Widget]`/`[Debug]` take compile-time defaults. Keybindings are
      unaffected. The hotkey/dMenu-button toggle is what keeps the widget
      switchable regardless (PR #94, 0.19.0)
- [x] Hide/toggle the Intuition menu on a hotkey — default `x` (scancode 45),
      `iToggleWidgetKey` under `[Keybindings]` in `Huginn.ini`, 0 to unbind.
      **Had to be a latch, not a SetVisible(false):** HudVisibilityManager
      recomputes visibility from scratch on every MenuOpenCloseEvent, so a bare
      hide is undone by the next menu the player opens — which is exactly the
      unmatched-hide behaviour seen while testing #14. Enforced inside
      `SetVisible` itself rather than at the call sites, because three
      independent paths re-show the widget (HudVisibilityManager, IntuitionBackend
      on wheel-close, ReapplySettings on hot-reload) and gating one would let the
      others undo the player. Hides are always allowed through.
      Session-scoped and reset on load: a latch that survived a restart is
      indistinguishable from a broken widget, with no on-screen affordance to
      find the key again. `bEnabled` remains the actual preference.
      Un-hiding routes through `UpdateVisibility()` rather than `SetVisible(true)`
      so a paused game or a disabled widget still wins.
      **Also reachable from dMenu** (`Huginn_toggle_widget` button), flipping the
      SAME flag rather than a parallel one — `bEnabled` gates only visibility
      (`IntuitionBackend::IsEnabled()` is unconditionally true), so a second
      "is the widget showing" flag would be this one renamed. That is what makes
      the widget switchable without dMenu installed.
      **Three latch holes found after the first cut, all the same shape** — some
      path re-showing the widget without consulting the latch, leaving it visible
      while the flag read "hidden" so the next press was dead:
      (1) `ResetUserHidden()` hung off the `markPageDirty` callback, whose only
      caller is `WheelerClient::CheckPendingWheelClose` — so it cleared on every
      Wheeler wheel close and never on an actual game load (caught in-game);
      (2) `IntuitionMenu::Show()` checked only `IsEnabled()`, and Skyrim destroys
      the menu across a cell transition, so walking through a door re-showed it —
      `UpdateVisibility()` cannot correct this, `GetSingleton()` is still null
      because kShow is only queued (caught by code review, verified fixed
      in-game: `Show() - hidden by hotkey, skipping` at 13:52:26);
      (3) input fired during text entry — characters arrive as CharEvents so
      clearing `userEvent` never suppressed them, but the bound action still ran.
      Harmless while every binding was a digit; a letter default made typing
      `player.additem` flicker the widget. Now gated on
      `ControlMap::textEntryCount`, for ALL bindings — the digits would equip
      slots while typing a FormID (verified in-game)
- [x] Finer Intuition menu granularity — every dMenu slider stepped to 0.1.
      The six-decimal form in the INI (`fPositionX = 22.300001`) is dMenu's own
      `std::to_string` and is NOT ours to change: its slider schema takes only
      min/max/step, and Huginn does not write that file. A display fix would be
      an upstream dMenu NG request

### Architecture Critique — Tier 2
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
- [x] #10: split WheelerClient.cpp (~1.3k lines) — the coupling behind the
      finding-3 race surface. **Complete across three PRs (#72, #73, and step 3
      below).** State-ownership pass settled three questions and changed the
      plan:
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
    - [x] Step 2 — WheelSync extraction (PR: wheeler-split-wheelsync).
          `m_pageWheels` / `m_pageDataMutex` / `m_addFailCooldowns` are now
          exclusive to `WheelSync`; nothing outside it ever holds a `PageWheel&`.
          The recursive-lock hazard is gone by construction rather than by an
          `…Locked()` variant: `HandleWheelOpened` no longer holds the mutex, and
          `DescribeOpenedWheel` answers page index + name + auto-focus target in
          one locked call, so three lookups can't interleave with a teardown.
          Also deleted rather than moved (zero callers): both
          `UpdateRecommendations` overloads, `GetCurrentWheelIndex`,
          `ClearActivationEmptied`, `IsWheelVisible`, `GetItemTypeName`, and the
          three legacy wheel-index aliases. `WheelerClient.cpp` 1236 → 355.
    - [x] Step 3 — injected callbacks, cutting the seven outward reaches
          (PR: wheeler-split-session). WheelerClient keeps the wheel-session
          state it already owned after step 2 — a fourth class would have been a
          pure forwarder — and the seven reaches into SlotLocker, SlotAllocator,
          CandidateGenerator, EquipSourceTracker, EquipEventBus, IntuitionMenu
          and WheelerSettings become an 8-callback `Environment` wired in
          Main.cpp, validated on assignment with an `EnvironmentReady()` gate as
          the first statement of every callback. Form→SourceType classification
          moved to the provider; it is candidate/'s vocabulary.
          **`wheeler/` now includes exactly one thing outside itself:
          `slot/SlotSettings.h` in WheelSync, for the page/slot layout it needs
          to size the wheels. That is a config read, not a policy reach-up.**

### Architecture Critique — Tier 3 (hot-path perf)
- [x] O1: WeaponRegistry::RefreshCharges walked the WHOLE inventory every 500 ms
      to refresh enchantment charge — 1.23 ms/call (2026-06-07) then 1.25 ms
      (2026-06-13), the single largest Huginn line, more than all 11 state polls
      combined, and the source of the ~1 s sawtooth in Tracy's memory plot (a
      per-call InventoryEntryData map allocation). Charge only drains on EQUIPPED
      weapons (<=2), so it now reads straight off GetEquippedEntryData: O(equipped)
      plus one guarded entryList pass for ammo counts. Favorite DISCOVERY moved to
      ReconcileWeapons' existing 30 s cadence, which already walks inventory.
      **Measured after: 34.4 µs** for RefreshCharges and 791 ns for Refresh
      (2026-07-24) — ~1.2 ms reclaimed per fire, sawtooth gone.
      Ammo counts deliberately kept per-tick: the low-ammo override gates on the
      cached count, so a stale one would surface ammo the player has run out of.
      And NOT via RE::InventoryChanges::GetItemCount — that crashes on save-load
      (bisected in PR #41; see the SKSE-gotcha note at WeaponRegistry.cpp:160)
- [x] #14: gate the display push paths. **Parts 1-3 merged** in PR #96 at 0.19.2
      (2026-08-26): IntuitionBackend change-detect, GetLockSnapshot hoist, lock-
      timer subtext quantization. **Part 4 (WheelerBackend lazy per-page
      allocation) DROPPED** — the whole ranking premise turned out to be a
      small-sample artifact.
      The short captures that motivated this item (31-66 pushes) put
      `Display::Wheeler` at 3.23-3.87 ms MTPC. A 44:40 playthrough on 2026-08-26
      with 874 pushes puts it at **986 µs** — the push fires every few seconds,
      so a handful of cold calls (wheel creation, post-load) dominated the short
      runs. There is no 3.6 ms frame spike to fix. Ranked by total on that
      capture: PollPlayerMagicEffects 2.61 s (0.10%), PollTargets 2.17 s (0.08%),
      Inventory::DeltaScan 1.03 s (0.04%), Display::Wheeler 862 ms (0.03%) —
      FOURTH, and nothing in Tier 3 exceeds 0.10% of runtime. Reopen only if a
      felt stutter turns up that a sub-1 ms mean cannot explain.
      Part 1 confirmed at 92.76 µs → 63.12 µs, though the zone wraps the whole of
      `Push()` including the m_scratch build, so it structurally cannot show the
      real win (UI-thread GFx work it never times). Parts 2-3: enabling the lock
      label costs nothing measurable, on weak evidence — locks were live only
      ~44 s of a 15-minute capture, and `fLockDurationMs = 1000` makes
      `ceil(remaining/1000)` yield only ever 1, so the countdown never ticks.
      Raise it to ~4000 to exercise the 3 → 2 → 1 transitions.
      **Two lessons worth keeping.** Both regressions in the first cut were
      caught ONLY by Tracy, never by the log: change-detect never fired because
      `confidence` (= `assignment.utility`) varies every run, and
      `GetLockSnapshot` was hoisted unconditionally while the old per-slot calls
      sat behind `stConfig.showLockTimerLabel`, turning zero lock acquisitions
      into one in the default config. And zone-count equality is NOT a
      diagnostic: `Huginn_ZONE_NAMED` sits at the top of `Push()` ahead of every
      early-out, so its count always equals the call count. MTPC is the only
      signal. See docs/refactor/wheeler-push-spikes.md
