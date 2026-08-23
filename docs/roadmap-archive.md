# Huginn Roadmap — Archive

Completed items moved out of [roadmap.md](roadmap.md) to keep the live list
scannable. Kept rather than deleted: several entries record why an approach
was rejected or what a fix turned up, which is the part that stops it being
re-litigated. Section headings mirror the roadmap's.

## Known Bugs
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
      Q-learner state dimension (cosave bump). And the take-off Z survived save
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

## Known Recommendation Issues
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
