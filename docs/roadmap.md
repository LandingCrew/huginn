# Huginn Roadmap

Open work only. There is no completed-items archive any more — `roadmap-archive.md`
was deleted on 2026-09-07 — so a rejected approach is no longer recorded anywhere
once its entry leaves this file. Git history is the only record; check it before
re-opening something that looks obviously undone.

## Next up
Suggested order, each played and measured like #140-#148. Details are in the
entries named.

1. **Darkness scoring** (Known Recommendation Issues). Small and concrete:
   Night Eye, light spells and torches when `lightLevel < 0.3`.
2. **Survival cold scoring** (Known Recommendation Issues). Blocked on the
   Warming Aura spell's name or FormID, to check whether it is registered at
   all.
3. **Wildcards in combat** (Slot temporal memory, remaining churn). Decision
   pending from the user; the lean is an INI toggle, default on, possibly with
   a shorter combat expiry.
4. **Widget hidden in cut scenes** (Follow-ups). Written on
   `widget-hide-while-wheel-open`; needs a rebase and one real cut scene (S).
5. **Every-tick recompute** (Known Bugs). Needs what the player was doing at
   11:10-11:20 on 2026-09-25 before anything can be guessed.

## Known Bugs
- [ ] The pipeline recomputed on EVERY tick for ten idle minutes.
      simonrim-essentials 2026-09-25, 11:10-11:20: `recompute=2881/2881` in
      two consecutive heartbeats, tick avg 0.46 -> 2.36 ms, and no state
      transition logged. `CheckHashSkip` (`PipelineCoordinator.cpp:223`)
      refuses to skip while any unhashed state is active -- elemental,
      falling, underwater, workstation -- and its comment calls all of them
      bounded, which underwater and workstation are not. Workstation read 0
      throughout. What the player was doing is unknown; ask before guessing.
      Raised 2026-09-25.

- [ ] One weapon stack's ExtraHealth has read 0.00, then 1.00, then 1.30 across
      three sessions on the same character, and nothing explains the first two.
      uid87, the LoreRim Long Bow. Either the player tempered it between those
      sessions, or an early read returned zero for a reason of its own.
      Worth settling because #131 added an `ExtraHealth > 0` guard before
      trusting the temper factor, and that guard turns a zero into "untempered"
      -- so if a read CAN spuriously return zero, the guard hides it rather
      than reporting it, and the weapon silently ranks and displays at its
      base damage.
      Rescued 2026-09-24 from the temper-suffix entry, which closed around it.

- [ ] One weapon in the LoreRim load order classifies as nameless and is
      skipped: `870710C4`, logged as "Failed to classify weapon ..., skipping
      (won't retry)" on every session. ClassifyWeapon rejects a form whose
      GetName() AND GetFormEditorID() are both empty, because storing it would
      leave data.formID at 0 and corrupt RemoveWeapon's swap-pop re-keying.
      The rejection is sound; what is unknown is whether the form is genuinely
      nameless or merely unnamed AT SCAN TIME. The tombstone is cleared by
      RebuildRegistry, so `hg rebuild` retries it -- if a rebuild registers what
      the initial scan rejected, the condition is transient and the scan is too
      early rather than the form being bad.
      NOT the Woodcutter's Axe, which was the suspicion when this was raised.
      The axe registers normally (2026-09-24: `Woodcutter's Axe
      (0002F2F4/uid47): dmg=31.5`) and had simply not been in the player's
      inventory. 870710C4 is still unidentified.
      The kHandToHandMelee note that used to sit here is done: #131's log-noise
      pass gave DetermineWeaponType an explicit arm for it, so Unarmed types as
      Unknown silently and the default arm keeps meaning "a type nobody has
      seen before".
      Raised 2026-09-24.

- [ ] Short-lived TARGET and DISTANCE state still flaps; decide whether to
      debounce them too. The crosshair picks the primary target with no
      hostility filter (`StateManager_Targets.cpp`) and a 0.3 s sticky window
      sized for raycast jitter, so sweeping the view across townspeople moves
      `Target`/`Dist` on every pass. LoreRim 2026-09-25: a humanoid crossing
      the crosshair for 0.11 s put the player "in combat" and flipped two keys
      twice.
      Done so far: combat (500 ms in / 2000 ms out) and enemy casting (0 in /
      2000 out) are debounced by BoolDebouncer (v0.21.30), and VitalEnvelope
      holds the recent magicka/stamina low for 8 s while regenerating
      (v0.21.31). The 50-minute LoreRim soak after both: ~35 slot changes per
      5 min (154 before the hold), with target/distance behind 12 of 40
      swap-backs. Target type and distance were left alone because
      Beast/Humanoid carry no weight. Re-measure before adding a dwell; the
      remaining swaps may be cheap enough to leave.
      Raised 2026-09-19.

## Known Mod Compatability Issues
- [ ] The default slot keys are the number row, which half of Skyrim also uses.
      `iSlot1Key = 2` through `iSlot8Key = 9` are DirectInput codes for the
      keyboard 1-8 — Skyrim's own favourites hotkeys, and whatever else a list
      binds there. Huginn does not intercept the press, it acts on it IN
      ADDITION, so one keystroke fires two systems.
      Seen 2026-09-21 on LoreRim: three potions used in one fight, each one
      millisecond after a press of 1 or 3, including a Potion of Fortify Carry
      Weight mid-combat. The user's first reading was "Huginn is auto-consuming
      potions", which is exactly what it looks like from the player's seat, and
      the log needed careful reading to show the presses were theirs.
      Nothing here is wrong in the code: the presses landed on the right slots
      and activated what was displayed. It is the DEFAULT that is wrong, because
      it silently doubles up on the busiest keys in the game.
      Options, roughly in order of how much they cost: ship a default that is
      not the number row (F1-F8 or the numpad); require a modifier, which is
      already its own entry under Architecture Critique ("Modifier-key bindings
      (Ctrl+1, Alt+1, Shift+1)") and would fix this as a side effect; or the
      read-only widget mode that has been on the backlog for a while, where
      Huginn displays and something else activates. The last one also answers
      the Wheeler-driven player, so it may be the one worth building.
      Raised 2026-09-21.

- [ ] Vanilla-build integration pass — a set of contexts is only ever exercised
      on the Requiem-based list this is developed against, so anything vanilla
      ships and Requiem strips is verified by unit test alone. Workstation is the
      known case (test 6h stands in for it); the honest scope is "boot a vanilla
      profile once and walk the contexts", which would also cover the #79 four
      that no LoreRim character can carry. Wants a save with vanilla alchemy
      ingredients and vanilla survival needs — the simonrim-essentials profile
      is that save, and the sentence this replaces had been cut off mid-thought
      since before the profile existed.
      Still open, hence the unticked box: #79's four contexts, the fortify
      POTION payload from #63, and the rest of the context walk. What follows is
      what the profile has actually closed.
      Progress 2026-09-19, on the simonrim-essentials profile (vanilla+AE):
      workstation is DONE — all four bench types answered live (forge 1,
      grindstone 2, armor workbench 7, alchemy lab 5) with apparel payloads, so
      test 6h is no longer standing in alone. Survival is confirmed working on
      the vanilla CC path: all four globals resolve, the native warmth function
      caches, `SMI not installed` takes the 0-1000 threshold fallback, and
      Hunger/Cold/Fatigue/Warmth all read live in the debug widget.
      One thing to check while a vanilla survival save exists: the widget read
      `Fatigue: Slightly Tired (lvl 1)` while the game's Active Effects said
      `Fatigue - Drained` at the same moment. Our stage names come from Survival
      Mode Improved and the boundaries from UESP, so this may be nothing worse
      than a naming mismatch on a path where SMI is absent — or the 150-299
      window for level 1 may sit a stage low against vanilla CC. Settles with
      one console read of the Exhaustion global (Survival.esl 0x816, 0-1000)
      next to what the widget shows.
- [ ] #63: the workstation context has no fortify POTION to rank on Requiem-based
      lists — inert in the modlists that actually get play-tested. Vanilla path
      still needs its own regression test (test 6h is the unit coverage).
      Two corrections from the #65 work, both on the [LoreRim wiki page](https://github.com/LandingCrew/huginn/wiki/LoreRim):
      the alchemy overhaul is attributed to Alchemy Redone rather than Requiem
      alone; and a MUCH bigger cause was found and fixed — walking up to a bench
      changed no GameState hash dimension, so the pipeline skip-gate discarded
      the whole workstation context unless an unrelated dimension moved on the
      same tick. That hit fortify potions on vanilla too, so re-check how much
      of "inert" was ever about Requiem's content. Apparel now answers the
      alchemy lab (#65, PR #114); the forge may still have no live payload

## Platform and dependencies
- [ ] **Drop dMenu, move the settings UI to SKSE Menu Framework.** Raised
      2026-09-26. Bigger than a settings-UI swap, because Huginn already owns
      the stack the framework would host.

      - Mod: https://www.nexusmods.com/skyrimspecialedition/mods/120352
      - v2 usage: https://github.com/Thiago099/SKSE-Menu-Framework-2/blob/main/Usage.md
      - v3 example: https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example
      - v3 readme: https://github.com/QTR-Modding/SKSE-Menu-Framework-3

      **The prize is not the settings panel, it is deleting our own hook.**
      SKSE Menu Framework is ImGui-based: a plugin includes
      `SKSEMenuFramework.h`, calls `SKSEMenuFramework::IsInstalled()`,
      `SetSection("Huginn")` and `AddSectionItem("Name", &Render)`, and writes
      ordinary ImGui inside the render callbacks. Huginn already does the hard
      half of that for itself -- `src/ui/D3D11Hook.cpp` (65),
      `ImGuiRenderer.cpp` (249) and `DebugInputHook.cpp` (290), 604 lines of
      hooking and context bootstrap, plus `imgui` in vcpkg.json -- purely to
      draw four debug widgets nobody but a developer sees. Those widgets
      (`RegistryDebugWidget`, `StateManagerDebugWidget`,
      `UtilityScorerDebugWidget`, `DebugSettings`) are already ImGui render
      functions and would move across close to as-is.
      The second deletion is the two-INI split. It exists only because dMenu's
      `flush_ini()` writes a fresh file containing just the settings it tracks,
      which would destroy every other section of `Huginn.ini` -- hence
      `GetDMenuIniPath()`, its fallback, the `[Widget]`/`[Debug]` exclusive
      ownership rule and the whole "Why two files at all?" section of
      `docs/architecture/7-dmenu-integration.md`. A framework that does not own
      an INI lets Huginn write its own settings and collapses all of it.

      **The cost is the coupling, and it is a real trade.** The dMenu
      integration is zero-coupling by design: no headers, no linking, no
      compile-time dependency, communication entirely by JSON descriptor and
      SKSE `ModCallbackEvent`, with a documented degradation table for
      "dMenu absent", "dMenu present, no JSON" and "both present". SKSE Menu
      Framework needs its header compiled in. That header is reported to be
      permissively licensed and not to require dependants to be open source,
      and `IsInstalled()` gives a runtime absence check -- so graceful
      degradation is still reachable, but with one sharp edge: **if Huginn
      deletes its own D3D hook and the framework is not installed, there is no
      ImGui at all** and the debug widgets have nowhere to draw. Settings would
      still load from INI, so players lose only the panel; developers lose the
      widgets entirely. Decide whether that is acceptable or whether the hook
      stays as a fallback -- and note that keeping both is the ONE thing that
      must not happen casually, since two ImGui contexts in one process is a
      classic crash.

      **Declarative becomes procedural.** The descriptor is 3 groups and 19
      controls of JSON today; it becomes C++ ImGui calls. Loss: settings stop
      being data a player or packager can edit without a rebuild. Gain: adding
      a setting stops being a three-place edit (JSON descriptor, INI, settings
      class) with its own how-to section in the integration doc.

      **Decide v2 or v3 first.** v3 claims non-pausing windows, an input event
      system, foreground/HUD drawing, texture loading, translations, JSON
      themes and an in-game config menu. Non-pausing matters here more than it
      would for most mods: Huginn is a contextual tool and tuning weights while
      the world is live is exactly how you would tune it. Against that, v3 is
      newer and the example repo does not document the API inline.

      Treat everything in the two paragraphs above that describes the framework
      as vendor claims read off its docs on 2026-09-26, not as verified
      behaviour; the line counts, the widget list and the two-INI rationale are
      checked against this repo.

      Scope: the settings panel alone is M. Panel plus retiring
      D3D11Hook/ImGuiRenderer/DebugInputHook and the dMenu INI split is L, and
      it touches `docs/architecture/7-dmenu-integration.md` (which would be
      rewritten or deleted), `configs/Huginn.ini`,
      `Data/SKSE/Plugins/dmenu/customSettings/Huginn.json` (deleted),
      `SettingsReloader`, `Globals.cpp`'s two path helpers, and the 20 files
      under `src/` that mention dMenu (132 references).

## Known Recommendation Issues
- [ ] **Darkness is detected but never scored, so Night Eye and light spells
      never surface.** `InDarkness` is a display reason only
      (`ContextRuleEngine.h:198`: "...InDarkness) that nothing scores on");
      night and caves change nothing in the ranking. The player went looking
      for Night Eye in both and it never appeared (2026-09-27). Wants a
      darkness weight -- WorldState::lightLevel < 0.3 already reports it --
      for Night Eye, Candlelight/Magelight and torches, suppressed while one
      is active the way waterbreathing is.
      Raised 2026-09-27.

- [ ] **Survival cold is tracked but never scored.** `coldLevel`,
      `warmthRating` and `IsFreezing()` exist in PlayerActorState and nothing
      reads them; `PriorCalculator.cpp:96` names a `warmthWeight` that does
      not exist. So warming spells (the player's "Warming Aura"), hot food
      and warm gear never surface for cold, even at cold level 3. The
      Warming Aura spell also did not appear in the spell registry dump --
      get its exact name/FormID to check whether it is registered at all
      before building the rule.
      Raised 2026-09-27.

- [ ] **Two weight sets: exploration and combat.** Idea from play
      (2026-09-27), needs more thought before building. The context weights
      are one table serving two very different situations; out of combat the
      bar wants utility (light, warmth, waterbreathing, travel buffs), in
      combat it wants damage, wards and restores. Splitting the table (or the
      learner's context) by mode could make each sharper. Open questions:
      what decides the mode (the debounced combat flag is the obvious gate),
      whether the learner learns per mode, and how this interacts with page
      layouts that already separate the two by hand.
      Raised 2026-09-27.

- [ ] **Estimated altitude.** Idea from play (2026-09-27): add a fixed
      offset (e.g. +100000) to the player's world Z to get an estimated
      global altitude. Exteriors only -- interiors have their own Z space and
      do not need it. Featherfall already surfaces from FallTracker (descent
      below the take-off point: it appeared 0.3 s into a fall at 16:41:38
      with the "Falling" label), so altitude is not needed for that. Open
      question before building: what should altitude DRIVE -- pre-emptive
      slow-fall near high ground, cold at altitude in survival, something
      else?
      Raised 2026-09-27.

- [ ] Two counts for the same quiver can be on screen at once and disagree by
      one shot. A bow or crossbow slot prints `playerState.arrowCount` --
      polled at 10 Hz and, since v0.21.13, forcing a recompute on every shot --
      while an ammo slot prints the count on its AmmoCandidate, which comes
      from WeaponRegistry's own 500 ms refresh. During archery a widget holding
      both can read `Long Bow - [11]` beside `Iron Arrow - [12]` for up to half
      a second per shot.
      Pre-existing skew; only visible since v0.21.12 made Minimal -- the
      default mode -- print counts at all.
      NOT fixed by pointing both at PlayerActorState: an ammo candidate can be
      ammo the player has NOT equipped, which PlayerActorState knows nothing
      about, so the registry is the right source for that slot. The fix is
      either to push the equipped ammo's count into the candidate at generation
      time, or to let the equipment poll nudge the weapon registry's ammo index
      the way it now nudges the pipeline.
      Raised 2026-09-24, from the #133 review.

- [ ] #128 measured the spell classifier against 1,107 spells and silently
      excluded 1,200 scrolls. `hg dump spells` walked
      `GetFormArray<RE::SpellItem>()`, and GetFormArray keys on T::FORMTYPE --
      ScrollItem's is FormType::Scroll, SpellItem's is FormType::Spell -- so no
      scroll was ever in it. Fixed in v0.21.8, and the first dump that included
      them came back 6,224 rows against 5,024.
      Nothing in the #128 work saw a scroll: not the 375 -> 90 unclassified
      count, not the description corpus that rejected a text classifier, not
      the weak-evidence numbers. ScrollClassifier delegates straight to
      SpellClassifier, so every rule written there applies to scrolls and none
      of them was checked against one.
      First measurement (LoreRim, 2026-09-24): 1,200 scrolls, of which 107 come
      back Unknown. Worth re-running the #128 analyses over the wider set
      before trusting their conclusions.
      Raised 2026-09-24.

- [ ] Potions, apparel and weapons have no dump at all, and the item
      classifier has never had the measurement the spell one got.
      `hg dump spells` covers spells and (since v0.21.8) scrolls. ItemClassifier
      -- potions, poisons, food, soul gems -- ApparelClassifier and
      WeaponClassifier have nothing equivalent, so their rules have only ever
      been checked by eye against whatever the player happened to be carrying.
      Every real classifier bug this month was found by dumping the whole load
      order and grouping, not by looking at a registry: the 375 unclassified
      spells, the twenty-one weapon enchants a text rule would have broken, the
      kFame throwing knives. None of those was visible from a registry dump,
      because a registry only holds what one character owns.
      Wants one `hg dump forms` covering every classified form type, with the
      inputs beside the verdict, in the same throwaway spirit as the spell one.
      Raised 2026-09-24.

- [ ] A Defensive spell's element may be a word in its name, not a resistance
      it grants. Found by the #130 review, and the other half of the bug #130
      fixed. `DetermineElementType` reads the effect's `resistVariable`; when
      that is unset the element falls back to `DeriveElementFromTags`, which is
      a NAME keyword -- "ice"/"freeze" -> Frost, "fire"/"flame" -> Fire,
      "spark"/"thunder" -> Shock.
      `ContextWeightForCandidate` and `CandidateFilters::IsResistSpellRedundant`
      both take a Defensive spell's element to be what it protects against, so
      a spell named after an element is promoted while the player takes that
      damage and then suppressed once they resist it -- offered for the wrong
      reason, then withheld for the wrong reason.
      Measured on LoreRim (2026-09-23): ten Defensive spells carry an element.
      NINE have a matching resist actor value and are genuine -- Fire Shell and
      Shield (kResistFire), Frost Shell and Shield (kResistFrost), Shock Shell
      and Shield (kResistShock), three Resist Poisons (kPoisonResist). The tenth
      is **Ice Armor**: `kDamageResist`, a physical armour spell, Frost from the
      word "Ice".
      Blocked on an instrument, deliberately. All nine genuine ones ALSO have an
      element word in their names, so `hg dump spells` cannot say which of the
      two paths set each element -- and clearing name-derived elements on
      Defensive blind could take all ten. Wants a dump column reporting the
      API-derived element before the tag fallback, then the rule, then a count
      on both load orders. The same "measure before shipping a rule that touches
      many spells" that #128 settled on.
      Raised 2026-09-23.

- [ ] Arcane Mass Inhibition is typed Utility and should be Debuff.
      One spell, recorded so it is not rediscovered as a mystery. #128 types a
      self-delivered, non-hostile, detrimental spell as Utility — a cost you pay
      yourself rather than an attack, which is what rescued Equilibrium from
      being called Damage. This spell is an offensive AoE whose author left
      `kHostile` clear, so it is structurally identical to Equilibrium and lands
      in the same bucket. Every narrower rule tried during the review traded it
      for a spell that is correct today (health-only breaks Equilibrium
      (Stamina); dropping the delivery test breaks Force of Nature).
      An author flag error, one spell in 1,107, and the override file is the
      fix. Only worth revisiting if the shape turns out to be common.
      Raised 2026-09-23.

- [ ] Take craft gear back OFF when the crafting is done — the #65 follow-on.
      Apparel is the one source that CHANGES THE PLAYER and leaves it changed:
      every other recommendation is spent when used, but a fortify ring stays on
      the finger after you walk away from the bench, and Huginn deliberately
      tracks nothing about what it replaced ("taking it off again is the
      player's business", CandidateTypes.h). Gear up at an alchemy lab with a
      circlet and a ring and you leave wearing +6% alchemy and whatever armour
      those two slots used to hold is in your pack.
      Wants a decision before it wants code, roughly in order of nerve:
      remember the displaced piece and offer to restore it once the workstation
      context closes (a recommendation, so the player still chooses); surface a
      "take it off" entry in the same slot while the gear is worn and the bench
      is gone; or restore automatically on leaving, which is the only option
      that acts on the player without being asked and should probably stay off
      by default.
      Note the restore target is an INSTANCE, not a form — it needs the
      ExtraUniqueID plumbing from #65, and ApparelRegistry::MarkEquipped already
      knows which piece each equip displaced (that is what the slot sweep is).
      Raised 2026-09-18 after the swap loop was fixed.
- [ ] Recommend enchanted apparel beyond the three craft skills — the #65
      follow-up. #65 itself is DONE (PR #114): apparel is a candidate source,
      verified in-game, but deliberately narrow — only gear fortifying Alchemy,
      Smithing or Enchanting, because classification is what keeps the pool from
      growing by the size of the wardrobe.
      A real inventory has far more that is contextually useful. Sorted by cost:
      **Tier 1, free** — existing weight AND existing detection, pure
      classification: resist fire/frost/shock/poison/disease
      (`resistXWeight`), magicka/health/stamina regen (`magickaRestoreWeight`
      et al.), muffle/sneak (`stealthWeight`), waterbreathing.
      **Tier 2, new weight but the signal exists** — carry weight
      (`isOverencumbered` is already polled and `ItemTag::FortifyCarryWeight`
      already exists); per-school spell cost reduction (school is known, but
      there is no per-school weight); weapon-skill fortifies (equipped weapon
      type is tracked).
      **Tier 3, needs new detection** — haggling/speechcraft. There is NO
      merchant or barter context anywhere in `src/`; it wants BarterMenu
      detection, which is state plumbing rather than classification.
      **BLOCKER, settle before any of the above.** Apparel is currently safe
      only because it is narrow: one circlet, swapped deliberately at a bench,
      out of combat. Tier 1 is exactly the combat case — recommending resist
      robes while the player wears 258-armor Orcish Berserk means `EquipApparel`
      strips the armor mid-fight, and nothing restores it. Needs (a) slot
      grouping so two rings do not both surface — `ApparelData::slot` is already
      classified, the candidate field was dropped in PR #114 for having no
      consumer and comes back here; (b) a worn-vs-candidate comparison, is the
      enchantment worth the armor lost, which is scoring not filtering; and
      (c) a restore story, or an explicit decision not to have one (M/L)
- [ ] Scroll cold-start: all scrolls sit in the pool every tick but score
      `learn≈0` against trained items at `learn=7–8`, so one can never surface
      until used and can't be used until surfaced.
      **Not scroll-specific** — it is the general no-transfer-between-items
      problem, and it bites every newly acquired item. Scrolls are just where it
      is most visible, because a player rarely uses one unprompted. The two
      candidate fixes are under Follow-ups, "Share learning across similar
      items"; fixing either closes this.
      Concrete case (2026-09-24): LoreRim's throwing knives are ScrollItems --
      not WEAP, not AMMO -- and ScrollRegistry already tracks them correctly
      (Iron x45, Steel x15, Silver x15). A Silver Throwing Knife did reach the
      widget, but through WildcardManager at 50% probability rather than on
      merit. A stack of 45 is a real combat option and should be able to earn
      its slot

- [ ] Two duplication findings from the PR #114 review, neither blocking:
      `ItemClassifier::DetermineFortifySkillType` has no case for the
      "Modifier" actor-value series, so a POTION carrying `kAlchemyModifier`
      falls into its default and is never tagged — the same bug class that made
      apparel inert, still live for potions. `ApparelClassifier` copied that
      vocabulary rather than sharing it, and the two have already drifted in
      opposite directions; one shared AV -> craft-skill function fixes both.
      Separately, `ApparelRegistry` hand-copies the key-agnostic half of
      `Registry::FormRegistry` (`ForEachEntry`, `IsLoading`, `EntryCount`) — the
      CRTP base whose own comment says it exists to stop exactly that. Its
      composite (formID, uniqueID) key genuinely does not fit the FormID-keyed
      index, which is why it was copied, but the visitor half could be adopted.
      Both grow in value if the apparel expansion above lands, since it
      multiplies the effect types being classified (S each)

- [ ] **Review the slot classes as a whole.** They grew one at a time and
      overlap in ways a player cannot guess: DamageAny takes weapons and
      poisons as well as spells; BuffsAny and DefensiveAny both take armour
      spells; HealingAny and PotionsAny both take health potions; and
      nothing meant "attack magic" or "poisons" until DamageMagic and
      PoisonsAny were added for the flagship page and the templates
      (2026-09-28). Worth one pass that decides what each
      class is FOR (a job on a key) rather than what it happens to match,
      then trims or renames to fit.
      Raised 2026-09-28.

- [ ] **Equivalence cap: decide the default after play.** `[SlotLocker]
      bCapEquivalents` (one spell/scroll of each kind per page, spell over
      scroll while castable; `docs/architecture/5-slots.md`, Equivalence cap)
      shipped opt-in and has not been play-tested. Open: (a) turn it on by
      default if `[EquivCap]` debug lines show it removing real duplicates and
      nothing a player misses; (b) the key may be too fine (a cost-band edge
      splits two otherwise equal mod spells) or too coarse (different
      magnitudes merge) -- adjust from what the log shows; (c) a slot lock
      can keep a duplicate for up to `fLockDurationMs`, because SlotLocker
      runs after allocation and cannot refill -- accept or teach the locker
      (S)
      Raised 2026-09-29.

## Slot temporal memory
Seating fixed WHERE an item sits; these entries are about WHEN a slot may
change. The churn and override work (#140-#148) closed most of it: what is
left is the churn tail and the two deferred override steps; Remembrance
shipped (v0.21.47).
Raised 2026-09-24.

- [ ] **Remembrance follow-ups.** Remembrance itself -- a slot holds what
      you just took off -- SHIPPED in v0.21.47; design and mechanics are in
      `docs/architecture/5-slots.md` (Remembrance). Press a Huginn key or a
      Huginn wheel entry that puts a weapon, spell or scroll in a hand (or
      swaps one ammo for another), and what it replaced shows under the same key for `fRemembranceDurationMs` (15 s),
      labelled "Swap Back". Per-slot `bRemembrance`, on by default.
      Decided 2026-09-27 (with the user): Huginn keys and Huginn wheels only;
      external equips (menu, favourites, the player's own wheels) are out of
      scope; the item takes the pressed slot whatever its classification;
      one hand (right, else left); no chaining; no learner reward.
      Decided 2026-09-28: a plain UNEQUIP (hand or quiver left empty) is not
      remembered -- only a swap made through Huginn is.
      Played on LoreRim 2026-09-27 22:16-22:39: 18 holds, each the item the
      press took off (Battlestaff back under the knife's key, Long Bow back
      when the Battlestaff replaced it); swap-backs ended them, the rest
      expired at 15 s.
      Follow-ups, none started:
      - `fWeightNoWeapon` (0.40) still lifts every weapon when a spell goes in
        the weapon hand. With the one weapon you took off now on your key,
        that weight can probably drop a lot; measure the equip flood first.
      - A pair pseudo-item ("re-equip both") if two-item displacement turns
        out to be common in play.
      - External equips, if wanted: they have no source slot, so the item
        would need a home chosen by classification.
      - Instance tracking: a weapon owned twice shows its best-scoring stack,
        not necessarily the one you took off.
      - Untested with Wheeler's `Empty` post-activation policy.
      - No dMenu toggle, like the rest of `[SlotLocker]`.

- [ ] **Remaining slot churn.** Instrumentation shipped in v0.21.19: the
      `slotChurn=` heartbeat field with causes and challenger ratios, plus a
      debug `[SlotChurn]` line per change. The fixes that followed:
      - the slot hold (`bHoldSeatedItems`, `fChallengerMargin` = 25%, v0.21.27);
      - `sPotionTierPreference`;
      - the combat/casting debounce and VitalEnvelope (see Known Bugs);
      - a dedup-emptied slot refilled in the same pass (#146).
      Result: 154 changes per 5 min went to ~35, near-ties to ~0, and
      swap-backs from 1 in 5 to ~1 in 8-11. The last simonrim run had 12/12
      key presses matching the widget and no dedup blanks. What is left, none
      of it urgent:
      - **Wildcards.** They are churn by design, ~4 changes/min, and hold
        their slot for the whole 30 s term. Two questions: should they live
        only in dedicated slots, and should they roll DURING COMBAT? For
        combat the user is still deciding; the lean is an INI toggle,
        default ON (exploration is how the learner finds anything new),
        possibly with a shorter combat expiry.
      - **One potion in several slots.** Drowning put Waterbreathing Good
        (the override), Fair and Faint on screen together (2026-09-26
        14:56:43). The tier rule orders strengths; it does not say only one
        should show. Undecided whether it should.
      - **The equip flood.** Putting a spell in the weapon hand lifts every
        weapon (`fWeightNoWeapon` 0.40) and flooded six slots at x1.23-1.63.
        A 30% margin would stop most of it, but it has not been re-measured
        with the hold. Remembrance now puts the weapon you took off under your
        key; next is lowering the weight (see Remembrance's follow-ups).
      - **Blank slot at load.** LoreRim 2026-09-27 21:38:13: two duplicates
        cleared in the first frame while two used items moved. Slot 6 had
        nothing that was not already on another key and stayed empty for
        2.3 s. That is the designed behaviour; revisit only if it shows up
        mid-play.
      - **Counter nit.** `'Unarmed' -> 'Unarmed'` counts as a change: two
        Unarmed pseudo-items (likely one per hand) with different FormIDs.
        The counter should compare what is displayed, not only the form.
      Raised 2026-09-24.

- [ ] **Override rethink, step (c) deferred; (b) shipped.** The
      goal is to fix the "dumb smart" problem: an override is a RULE, not a
      recommendation, so sometimes you just get a health potion. Agreed with
      the user 2026-09-27:
      - it triggers per vital at the INI thresholds, any time;
      - it shows a fixed pick order, with no scoring;
      - it lives in the numbered slots, with one key per vital;
      - the pulse stays as the only presentation.
      An out-of-band override element was REJECTED; the reasoning is in git
      history (this entry before 2026-09-27).
      Shipped: health pinned to its slot, with no cascade and a stale lock
      released (#143). Then (a) in #144, v0.21.39: the pick order is potion,
      then a self-cast spell affordable now, then the slot's normal
      recommendation. Verified on simonrim (Potion of Healing, then Fast
      Healing, then Healing as magicka fell) and on LoreRim (Requiem's
      Healing, cost 53 of 146 magicka, placed on key 1).
      **(b) SHIPPED 2026-09-28** (branch `flagship-page`): magicka and
      stamina are pinned like health, and each vital is an INI switch
      (`[Overrides] bPin{Health,Magicka,Stamina}ToSlot`, all on; off = mark
      in place). Needed once the flagship page gave every key a job: the
      magicka emergency pulsed the Potion key, not key 2.
      Deferred:
      - **(c)** A `bOverridesOnly` emergency-key slot. This is already
        reachable by configuration: a PotionsAny slot with overrides enabled,
        bound to any key.
      Pick it up again if play shows a vital's emergency landing somewhere
      unexpected.

## Doc-migration findings (2026-08-29)
Surfaced by the one-agent-per-doc migration pass. Every one is a code or config
defect the docs exposed, not a documentation problem. Ordered by what a player
would notice.

- [ ] **`sUncastableSpellPolicy = Penalize` behaves identically to `Allow`
      (by default).**
      Split out of the `[Candidates]` wiring fix (0.19.13), which got the setting
      to `CandidateGenerator` but could not make `Penalize` mean anything: both
      `RunVisitorFilters` and `PassesAffordabilityFilter` branch only on
      `Disallow`. The old docs described a shortfall ratio and a `penaltyFloor`;
      neither was built, and `fUncastablePenaltyFloor` is gone from the INI.
      **The mechanism now exists** in `Scoring::FitScorer`
      (`src/learning/FitScorer.h`): below one cast of magicka, `Penalize` gets
      the flat `[Scoring] fFitUnaffordableMult` (0.3) while `Allow`/`Disallow`
      hold at `fFitAffordMin` (0.7, the one-cast value). It is not a shortfall
      ratio. But fit enters the utility only under `iFitMode = 2`, and the
      shipped default is `1` (shadow: computed and shown in `hg recs` as
      `fit~`, not applied), so with default settings `Penalize` still ranks
      exactly like `Allow`. Close this once soak data from shadow mode
      justifies making apply mode the default (M)
- [ ] **Three tag values have no writer.** `ItemTagExt::Ravage*` and
      `Damage*Regen` are read by `HasHarmfulSideEffects()` but `PopulateItemTags`
      never sets them; `WeaponTag::EnchantSilence` has no writer anywhere in
      `src/`. Either wire them or delete them — as they stand,
      `HasHarmfulSideEffects()` cannot fire on those grounds (S)
      A fourth, the other way round: `ItemTagExt::WeaknessElement` HAS a
      writer but no reader (`IsWeaknessTo` has no caller), and the writer
      misses most weakness poisons -- it reads `resistVariable` only, so
      Apothecary's "Weak Aversion to Frost" (Peak, primaryAV = kResistFrost,
      hostile) tags nothing. Resist POTIONS had the same gap and read
      primaryAV since the override-potion PR; wire the weakness side the same
      way if anything ever reads it, or delete it.
- [ ] **Dead work in Release builds:** `damageRate`, `healingRate`,
      `damageIncreasing` and `damageDecreasing` are computed unconditionally
      every tick (`StateManager_HealthTracking.cpp:283-330`, no `_DEBUG` guard)
      and their only consumer is an ImGui debug widget. Either guard it or wire
      it — it is also exactly the substrate a future trend feature would need.
      Adjacent dead code: `SlotAllocator::AllocateSlots` (both overloads), kept
      as a legacy/test entry point; `IntuitionMenu::SetUrgent` / `setUrgent` /
      `_urgentSlots`, vestigial with no caller;
      `maxCandidatesPerCycle` on `ScorerConfig`, still parsed by
      `ScorerSettings.cpp:47` and read by nothing (0.19.13 removed the INI key
      only, not the code); `weightWeaponChargeModerate/Low/Critical` on
      `ContextWeightSettings`, loaded but absent from `ContextWeightConfig` so
      they reach no consumer — the weapon-charge weight became a continuous
      `pow()` curve and these three tiers stayed behind (their INI keys were
      removed in 0.19.13); `FilterStats::filteredByRelevance`, never incremented
      but still summed;
      `StateEvaluator::EvaluateCurrentState()`, which takes a
      `const WorldState&` it never reads (S)
- [ ] **Code comments that actively mislead**, all found because a doc repeated
      them. Fixing these is what stops the docs rotting again.
      `StateManager_Targets.cpp:498` says "Scan ALL process levels (high,
      middleHigh, middleLow)" above a loop reading `highActorHandles` only — the
      middle lists were removed for cost, and the doc's headline ally-scanning
      claim came straight from this line. `ContextRuleEngine.cpp` says "Stage 1a
      (skeleton): Returns all zeros — no rules implemented yet" above a fully
      implemented method. `StateManager.h` says "3 locks" and "7 float
      accumulators"; it is 4 and 11. `StateFeatures.h:17` cites the stale
      36,288-state figure (it is 48,384 as of v0.21.16, and was 72,576 when
      this was written — and the file is `src/learning/`, not `src/state/`). `SettingsReloader.cpp:94` says the dMenu INI holds "Widget,
      Keybindings, Debug" — keybindings moved to the main INI in the 0.19.0
      split. `FeatureBanditLearner.h` says "~90% confidence at 15 trains"; the
      sigmoid gives 95.3%. Each verified still present 2026-09-07.
      Two are already fixed and dropped from this list: the "Semi-gradient
      TD(0)" claim (0.20.0, with the identifier rename) and
      `ContextWeightConfig.h`, whose field breakdown now correctly reads 35
      against 38 (XS each)
- [ ] Spell-pattern override file was proposed and never implemented — no
      `m_patterns`, no `pattern=true` parsing, no `Huginn_SpellPatterns.ini`.
      The proposal lived in `reviews/magic-classification.md`, deleted
      2026-09-07; this entry is now the only record. Its 29%-unknown figure is
      one 2026-02-07
      capture on one modlist, taken before the classification pipeline changed
      underneath it, so the current rate is genuinely unknown — measure before
      scheduling. Related: `Huginn_Overrides.ini` is shared by `SpellRegistry`
      and `ItemRegistry` but the shipped template documents only item types, so
      the spell-type vocabulary `SpellOverrides` parses is undocumented (M)
- [ ] **Suppress-mode favorites are fully scored every tick to be discarded.**
      Favorites bypass the `minimumContextWeight` early filter
      (`UtilityScorer.cpp:77`), then `GetFavoritesMultiplier` returns 0.0, then
      `minimumUtility` drops them. Low severity, but note that with
      `fMinimumUtility = 0` suppressed favorites would reappear at utility 0 (XS)

## Architecture Critique — Backlog
The critique itself (`reviews/architecture-critique.md`) was never committed and
does not exist in this repo or in the recovered docs snapshot. What survives is
the ledger below plus git history; Tier 1 and all of Tier 2 have landed, so the
only live work here is Tier 3.
**Landed:** Tier 1 (all); Tier 2 #8 registry consolidation (PR #55), #9 display
abstraction (PR #56), #10 safe pieces — GetContextWeight move + ComputeRelevanceTags
dedup (PR #57), #10 leftover — relevance-tag encoding unified on ContextRuleEngine
(PR #58, merged; verified in-game across 5 Debug sessions — all 26 reason labels
observed, threshold parity exact on both smoothing exponents). Critique #10 is
now closed; #59–#65 are follow-ups it surfaced, not remaining critique work.

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
- [ ] Modifier-key bindings (Ctrl+1, Alt+1, Shift+1) — every binding is a bare
      DirectInput scancode in a single `uint32_t` (`KeybindingSettings.cpp:22-31`,
      0 = unbound), so a slot key cannot be qualified and the ten slot keys must
      each own a whole key. Wants a modifier field per binding (or a packed
      scancode+modifier word, which keeps the INI a single value per slot but
      makes 0-means-unbound less obvious) plus modifier state in
      `InputHandler::ProcessButton`. The real design question is the interaction
      with the existing tap / double-tap / hold gestures: Ctrl+1 held is
      ambiguous against 1 held, and the modifier can be released mid-gesture, so
      decide whether the modifier is latched at key-down or sampled throughout.
      Also needs a conflict story for modifiers the game itself binds. Nexus page
      currently states "only single keypresses" — update it when this lands (M)
- [ ] `ValidateWheelState` emits ~11 desync warns during a Wheeler edit-mode
      session — stale by construction, since Huginn has no signal that indices
      moved until edit mode exits, and the exit re-resolve corrects everything
      1.4 s later. Observed 2026-08-29 13:03:18 against a 13:03:20 re-resolve.
      Skip the check while `IsInEditMode()` — the diagnostic cannot say anything
      true there (XS)
- [ ] Unit tests for Context::WeightForCandidate (Tests.cpp:2656/3374 currently
      hand-reimplement the weight mapping — call the real one). DominantReason /
      ReasonLabel are covered by unit test 17.
- [ ] The widget stays on screen through cut scenes, and the fix is written
      but unmerged. `origin/widget-hide-while-wheel-open`, one commit
      (`b752f38`, 2026-09-04), never opened as a PR.
      The widget's only visibility gate is `GameIsPaused()`, and a cut scene
      does not pause the game -- the camera is taken away and the controls go
      quiet, but as far as that predicate is concerned nothing has happened.
      The commit gates on two camera states instead, `kAnimated` (scripted
      cut scenes and killmoves) and `kAutoVanity` (the idle orbit), and
      deliberately excludes `kFurniture` -- which would hide the widget at an
      alchemy table or forge, exactly where the workstation context has
      something to recommend -- and `kBleedout`, as a separate question not
      worth answering silently. It polls from the update loop rather than
      using the event sink, because entering a cinematic camera raises no
      `MenuOpenCloseEvent`.
      Needs a rebase onto main (it is from before the display-backend split)
      and in-game confirmation on a real cut scene. Neither `kAnimated` nor
      `kAutoVanity` appears anywhere in `src/` today, so nothing about it has
      landed by another route.
      Raised 2026-09-24, out of the branch audit below (S)

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
- [ ] **Share learning across similar items** — two versions of one idea, pick
      one. Today every item learns alone: `m_items[formID]` zero-initialises on
      first access, so a new item's `learningScore` is 0 and cannot compete with
      a trained item at 7-8. Nothing a player teaches Huginn about one healing
      potion transfers to another. This is the mechanism behind the scroll
      cold-start entry under Known Recommendation Issues, and it will bite any
      newly acquired item, not just scrolls.
      Huginn already has the right SHAPE — `ContextRuleEngine` and
      `PriorCalculator` are shared across all items, `FeatureBanditLearner` is
      per-item — but the shared layer is hand-authored rules, not fitted to
      data, so nothing LEARNED is ever pooled.
      Prompted by poLinUCB (arXiv:2309.13896), whose post-serving-context
      machinery does NOT fit Huginn (its `z` must be arm-independent, and it
      adds a second per-arm vector when our arms are already starved — see the
      session note below). The transferable part is its structure: the shared
      mapping is fitted from every round regardless of arm, so it sees ~K times
      more data than any per-arm parameter. Find what is common, fit it from
      everything, leave only the genuinely item-specific part to each item's
      scarce data.
      **A. Warm start (cheap, no format change).** Seed a first-seen item's
      weights from the mean of trained items sharing its classification instead
      of zero. Huginn is well set up for this — the taxonomy already exists and
      is already load-bearing for slot filters. No model change and no cosave
      format change; it is a different initialisation where `operator[]`
      currently default-constructs, so the SHAPE of what gets serialised is
      unchanged.
      Wrinkle that decides whether it works: `trainCount` is still 0, and
      confidence gates the learned term (50% at 5 trains), so seeded weights
      would be suppressed anyway. It needs a small pseudo-count — "worth about
      two observations". That number is the whole risk: too low and nothing
      changes, too high and an untried item inherits confidence it has not
      earned. Measure before believing any specific value.
      Note it changes learning BEHAVIOUR even though it does not change the
      format, so it would invalidate a soak run in progress even though it is
      technically landable during one (S)
      **B. Hierarchical weights (the real fix, cosave change).** Split each
      item's vector into a class part and a deviation, `w_item = w_class +
      delta_item`. Fit `w_class` from every item sharing a classification, and
      `delta_item` as a small heavily regularised per-item correction. Same
      pooling asymmetry as (A) but learned continuously rather than only at
      first sight, and it keeps improving the shared part as play continues.
      Needs a cosave format bump to store per-class vectors alongside per-item
      deltas, so it sits in the same parked bucket as #15/#16 — NOT landable
      during an active soak run (M/L)
      Neither is measured. (A) is cheap enough to try and discard; (B) should
      not be started until (A) has shown that pooling helps at all on real play
      data.
- [ ] Addendum #15/#16 (Kalman learner / learnable context weights) — **parked**: needs a v3
      cosave bump, NOT landable during an active soak run
