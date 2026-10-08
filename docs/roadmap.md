# Huginn Roadmap

Open work only. Finished work leaves this file, and git history is the record.
A rejected approach keeps one line under [Decided against](#decided-against) so
it is not reopened by accident.

**The plan is the engine rewrite** (decided with the user 2026-10-08): the
hand-tuned `ctx × (1 + λ·learn) × multipliers` engine is replaced by a learned
choice model over needs and effects. Design:
[architecture/9-context-as-learner-input.md](architecture/9-context-as-learner-input.md).
Code map, phase by phase:
[architecture/9-implementation-map.md](architecture/9-implementation-map.md).
Theory and proofs: the Huginn Learning Theory page
(https://claude.ai/artifact/6g1yxgfNg146mX7hMiTrFs).

## How this roadmap is run

The rewrite is done mostly by agents in auto mode; the user tests in game only
where a task is marked **In game (you)**. So every task says what "done" means
in terms an agent can check without the game.

- **Branches.** The rewrite lives on its own branch, `engine-rewrite`, cut from
  `main` once the planning docs are merged. Each milestone is one or more PRs
  into it; it merges to `main` after the baseline soak (R12).
- **The old engine is frozen** on `main`: only fixes that block play. No
  tuning of multipliers, margins or context weights.
- **Two agents per task** (the user, 2026-10-08). One agent does the work. A
  second agent, in a fresh context, checks it on the assumption that it is
  wrong: it reads the diff and the task's done-criteria, tries to break each
  claim (re-runs the build and tests, reads the cited lines, looks for callers
  missed), and reports what it could not invalidate. Only then is the task
  done; findings go back to a worker, and the check repeats.
- **Agent-checkable means:** builds clean in Debug and Release; the host test
  target passes (R1); the replay tool's numbers; a dump or log diff against a
  checked-in expectation.
- **Every PR:** bump `CMakeLists.txt:5`; stage explicit paths only; never
  `git add` a directory.
- **Deploy hazard.** A Debug build copies the DLL into LoreRim-5 automatically
  (`CompiledPluginsPath`). A build on `engine-rewrite` therefore replaces the
  DLL the user plays. Agents must not leave a rewrite DLL deployed when the
  user expects `main`; say which build is deployed (MD5) at the end of a task.
- **simonrim** needs a manual DLL+PDB copy; verify by MD5.

---

## Now: the engine rewrite

| # | Milestone | Map phase | Gate |
|---|---|---|---|
| R0 | Cleanup, no behaviour change | 0 | Agent |
| R1 | Host test target | new | Agent |
| R2 | Effect extractor | 1 | Agent |
| R3 | Need vector, logged only | 2 | Agent, then a short session in game |
| R4 | Selection log v3 | 3 | Agent, then a short session in game |
| R5 | Data play: the mage character on R2–R4 | -- | **In game (you)** |
| R6 | Offline fit: go / no-go | 4 | Agent runs it; **you decide** |
| R7 | Slot code handles any score sign | 5 | Agent (can run any time after R1) |
| R8 | Cutover: new learner and scorer | 6 (+ cosave from 9) | Agent, then **in game (you)** |
| R9 | Bayesian challenger rule | 7 | Agent, then in game |
| R10 | Wildcards by uncertainty | 8 | Agent, then in game |
| R11 | Console, telemetry, docs; retire the classifiers | 9, 10 | Agent |
| R12 | Baseline soak, merge to `main`, release | -- | **In game (you)** |

R0 and R1 can run in parallel; R7 can run any time after R1. R2–R4 change no scores, so they ship as one
logging release before any play is needed.

### R0. Cleanup, no behaviour change

Done when: Debug and Release build clean, and the listed code is gone. Detail
and file:line in the [implementation map](architecture/9-implementation-map.md#phase-0-behaviour-neutral-cleanup-can-ship-now).
- [ ] Drop ingredients (eight sites; they never reach the registry today).
- [ ] Delete the A|B shadow arm (`src/learning/ShadowArm.*`) and `handsAtPress`,
      which only it reads.
- [ ] Remove the five dead INI weights: four read and never used, and
      `fWeightBaseRelevance` read into a field nothing uses (0.05 is hard-coded
      at `ContextRuleEngine.h:126`). Keys, settings, config fields, docs.
- [ ] Remove the enemy level read (not perceivable; debug widget only).
- [ ] Actor-type cache keyed on race: a transformed werewolf or vampire lord
      keeps reading Humanoid (`StateManager.h:476-479`).
- [ ] Race table before the keyword catch-alls: 37 LoreRim races misread
      (`StateEvaluator.cpp:141-142`, `9-data/race_map.csv`). Done when a
      `hg dump races` column with Huginn's reading matches the CSV.
- [ ] Read every queued hit, not only the last (`StateManager_HealthTracking.cpp:88,147`).
- [ ] Rename the slot "need" to slot class (`SlotClassifier`, `NeedCap.h:34`,
      `SelectionLog.h:27`) before the 92 needs arrive. Keep the JSONL `need`
      key or update `tools/replay` in the same change.

### R1. Host test target

The suites in `src/Tests.cpp` run only inside the game, so an agent cannot
prove a change. Add a test executable that builds and runs on the host.
- [ ] A CMake target (e.g. `huginn_core_tests`) over a `src/core/` of pure
      code: no `RE::` or SKSE includes. A small header-only framework from
      vcpkg, or plain asserts.
- [ ] Rule for the rewrite: every new piece of math lives in `src/core/`
      (response curves, the learner update, σ_Δ and the challenger rule, the
      effect mapper over plain records) and gets host tests. The game layer
      only reads forms and calls it.
- [ ] Done when: the target runs in CI-like fashion from the command line with
      a non-zero exit on failure, and one existing pure function (a curve, or
      `SlotClassCap::Factor`) is ported with tests as the pattern.

**And an unattended in-game run** for the code that needs real game data. The
Debug suites that run at the main menu (`RunUnitTests()` at kDataLoaded,
`Main.cpp:679`) need no save. About 20 more (SlotClassCap, SlotLocker, home keys,
cosave, the registries; `Main.cpp:443-466`) run only after a save loads,
including the ratio tests R7 must update:
- [ ] Huginn: after the suites, log one sentinel line with pass/fail counts;
      with a test flag set (INI or environment variable), quit the game.
- [ ] `tools/ingame/run_tests.py`: launch through MO2's command line
      (`ModOrganizer.exe -p "Simonrim Essentials" "moshortcut://:SKSE"`;
      LoreRim's executable is `LoreRim`, profile `Ultra`), wait for the
      sentinel in the Huginn log, kill the game on a timeout, exit non-zero on
      failure. simonrim first: it reaches the menu faster.
- [ ] Auto-load a named test save after the main menu, so the after-load
      suites run too (needed by R7).
- Rule: agents launch the game only when the user has said the machine is
  free; the game must not already be running.

### R2. Effect extractor

Map Phase 1. Describe every item as cap(i) from game data.
- [ ] `src/effect/`: a reader (game forms → plain records, kDataLoaded) and a
      mapper in `src/core/` (records → the 239 columns of `9-data/effects.csv`),
      with the layered actor-value resolution and load-order percentiles.
- [ ] Shared helpers move from `ConsoleCommands.cpp` to `src/util/FormRead.h`.
- [ ] Static cap in a catalog; runtime cross-features (`overshoot_*`,
      `weapon_charge`, `stack_count`, `ammo_matches_launcher`,
      `school_fortified`) computed per tick. Per-instance cap for tempered and
      player-enchanted weapons.
- [ ] **All carried armour is a candidate** (the user, 2026-10-08): lift the
      `ApparelClassifier` scope guard. Gear in combat is not a hard rule; the
      learner decides it. Armour menu picks are dropped today
      (`ExternalEquipListener.h:81-98`); lifting that shifts accept% (open:
      the user to confirm).
- [ ] `hg dump all` prints the catalog view and closes the eight dump gaps
      (doc 9, "Needs and effects, enumerated").
- Done when: the mapper's host tests pass on rows taken from the three dumps;
  coverage on the LoreRim dump ≥ 98.9% and on vanilla+/simonrim ≥ 99%; a
  coverage-diff report lists items that have a slot class today and an empty
  cap. Changes no scores.

### R3. Need vector, logged only

Map Phase 2. The rules keep scoring; the vector is computed and logged beside
them.
- [ ] `NeedId`/`NeedVector` from `9-data/needs.csv`; `ResponseCurve` in
      `src/core/`; a `[Needs]` INI section for curve parameters.
- [ ] Sensors: encumbrance ratio, per-element decaying damage rate, combat and
      submerged timers on `steady_clock`, the held multi-hot target families
      (union of combat hostiles; no line-of-sight logic, the user 2026-10-08),
      target summoned / casting / archer, restore pending, drop ahead.
- [ ] Computed once in `GatherState`; a quantised need signature joins the
      skip gate.
- [ ] `hg needs` prints the live vector.
- Done when: curve host tests pass; a replayed state snapshot gives the
  expected vector. **In game:** a 20-minute session where `hg needs` shows
  fire, darkness, hunger and combat onset firing and expiring.

### R4. Selection log v3

Map Phase 3. Log everything the fit needs.
- [ ] Per decision: the need vector, sparse cap per row, explicit outcome
      (key / wheel / menu / nothing), every eligible item (no floor), one row
      per item, wildcard propensity.
- [ ] "Nothing pressed": one record per need episode that expires with no
      press, with the page at the onset (the user, 2026-10-08).
- [ ] Menu choice set: the held items off the page.
- [ ] Settle whether the update loop ticks inside menus. A menu pick is
      dropped when the pipeline cache is older than `fExternalEquipTimeWindow`
      (500 ms shipped, `ExternalEquipLearner.cpp:103`), which a long menu
      session may trip.
- [ ] `tools/replay` reads v3; the schema is documented.
- Done when: replay parses a synthetic v3 file round-trip. **In game:** a
  30-minute session whose log holds all four outcomes.

### R5. Data play (in game, you)

The mage character, the planned next stress test, played on the R2–R4 build so
it doubles as the fit data. Several hours, mixed combat and town. Meanwhile an
agent runs the cheap test on today's log: one weight per slot class × logged
`ctx`; if it does not beat context alone, flag it before R6.

### R6. Offline fit: go / no-go

Map Phase 4, in `tools/replay`.
- [ ] Conditional logit with the three outcomes (key, menu at cost κ, nothing),
      u0 per need, sparse θ, small b, Plackett–Luce co-picks; split by launch.
- [ ] Report on held-out play: key hit rate against **76%** (arm A*, the plain
      ranking replay can reproduce; 81% was the live page with holds), and
      menu-pick hits against **7 of 83**.
- **You decide:** go to R8, or stop and rethink. θ from this fit is the
  bootstrap for R8.

### R7. Slot code handles any score sign

Map Phase 5. Lands before the new scorer.
- [ ] Bridge: score = ln(utility), σ = 0, m = 1.5, which reproduces today.
- [ ] The slot class cap, ratio logs, the `-1` "gone" sentinel, `utility = 0` for
      remembered-only rows, `kOverrideUtility`, the widget bar, the
      confidence payload: all made sign-safe.
- [ ] Full sort instead of the top-10 partial sort.
- Done when: a golden test feeds recorded pipeline snapshots through the old
  and new slot code and gets identical pages; the ratio tests are updated.

### R8. Cutover

Map Phase 6, plus the cosave from Phase 9 (θ must survive a load before any
soak). Order inside: hard zeros to `CandidateFilters` first; `combat_onset`
sensor from `PotionDiscriminator`'s timer; `ChoiceLearner`; scorer; cosave
`THTA`/`BIAS`; then the prune list in the map, with every consumer of a
pruned symbol (console, selection log, `ReasonHold`, debug widget, `Tests.cpp`)
changed in the same PR, and the shipped INI loses the dead keys.
- Done when: a grep over `src/` and `tools/` for every pruned symbol finds
  nothing outside the new code (docs follow in R11); Debug and Release build
  clean; host tests cover the update (Var_p precision, step = variance,
  Plackett–Luce, opportunity counting), the explanation label is the largest
  term, the scorer reproduces R6's replay numbers on the logged data.
  **In game (you):** a session on the R6 bootstrap; cures, potions and gear
  rechecked (the old "cures reach the page" checks move here).

### R9. Bayesian challenger rule

Map Phase 7: swap when μc − μi > ln m + z·σ_Δ, σ_Δ with shared weights
cancelling. Lock, home keys and seating stay. Done when: host tests prove no
swap-back on fixed beliefs; churn buckets on Δ/σ_Δ. **In game:** churn per
press in the heartbeat against the R8 session.

### R10. Wildcards by uncertainty

Map Phase 8: a reserved slot per page, ranked by relevance × σ plus a small
random term, propensity logged, flags per page (Wheeler leak). **In game:**
wildcard picks against the soak's 8 of 333.

### R11. Console, telemetry, docs; retire the classifiers

Map Phases 9–10. `hg theta`, `hg recs`, the dMenu reset text; θ-drift fields in
`[Soak]`; slot classes, overrides and labels re-derived from cap; per-list
override layer (and the [overrides directory](#an-overrides-directory-so-mod-authors-can-ship-their-own));
delete the classifiers and per-type dumps; rewrite `CLAUDE.md`,
`docs/README.md`, the architecture docs and the Nexus page; archive
`4-contextual-bandits.md`. Review the slot classes as a whole here (below).

### R12. Baseline soak and release (in game, you)

A multi-hour soak on the rewrite as the new baseline; merge `engine-rewrite`
to `main`; release notes say old learned weights are discarded.

---

## Needs and effects to add

Work that used to be hand-weighted rules becomes a sensor, a curve or an
effect column; the learner sets the weight. Most land in R2–R3; the rest
after R8.

| Want | Becomes | Status of the sensor |
|---|---|---|
| Buffs matched to the loadout (Fortify Destruction with Ember in hand) | `loadout_*` needs × fortify effects | Loadout flags exist |
| Resist potions by damage taken | Per-element damage-rate need | R0 + R3 |
| Smallest potion that covers the deficit | `overshoot_*` feature | R2 |
| No second restore while one ticks (over-time lists) | `restore_pending` need | R3 |
| Carry weight when encumbered | Encumbrance curve | R3 |
| Hunger, cold, fatigue as a ramp | Curves on the raw 0–1000 value; plumb SMI's continuous value | R3 |
| Thirst, only when a thirst system is found | `thirst` need + drink effects; scripted consumables need a second confirm (the effect appearing) | Missing |
| LoreRim's healing block | `healing_blocked` need | Missing; dump the active effects in that state first |
| Haggling gear at a merchant | `merchant` need; a worn-vs-offered cross-feature | Missing |
| Enchanted apparel beyond the craft skills | Armour effect columns + worn-vs-candidate cross-feature | R2 |
| Items matched to the enemy (silver, bane, sun, resist for a dragon's element, poisons by target) | Target families × effects | R3 |
| A summon when pressed in melee | Enemy-distance (gaussian) need | R3 |
| Damage over time on a boss | `boss_fight` need | Missing |
| Feather Fall before the jump | `drop_ahead` need (replaces "estimated altitude") | R3 |
| Soul Gem Fragment (LoreRim MISC item) | Find how LoreRim uses it first | Open |

## Kept outside the rewrite

Filters, display and slot features the rewrite does not replace. None should
start before R8 unless it blocks play.

- **Redundant gear: only the best piece per body slot.** A candidate filter
  (the user, 2026-10-06). Group by (effect, biped slot), keep the strongest
  unworn piece; ties to the piece learned or worn. Matters more now that all
  armour is a candidate.
- **Poison handling.** No poison for a weapon poisoned with another; more
  charges of the same are fine. Needs a poison playtest.
- **Food buff captions** (display only).
- **One potion in several slots** (Waterbreathing Good, Fair and Faint
  together). Undecided whether only one should show; the slot class cap and P1
  inheritance may settle it.
- **Remembrance follow-ups:** a pair pseudo-item, external equips, instance
  tracking, Wheeler `Empty` policy, a dMenu toggle. Design in
  [architecture/5-slots.md](architecture/5-slots.md).
- **Review the slot classes as a whole** -- do it in R11, when they are
  re-derived from cap.
- **Workstation gear holds its key at the bench**, and the bench flicker
  standing still (confirm with a still camera first). Recheck after R9.
- **`ApparelRegistry` copies half of `FormRegistry`** -- refactor when R2
  brings all armour in.
- **Behavioural modes as a recall stage** (design 2026-10-01): hand-written
  modes choose a short list by per-mode use counts; the scorer ranks it.
  Needs and θ may cover what the modes were for. Parked, not decided: revisit
  if R8's per-tick cost needs a short list.

## Waiting on an in-game test

- **uid87 ExtraHealth read 0, then 1.00, then 1.30** (LoreRim Long Bow). Watch
  only; close if it stays quiet.
- **A torch on a Huginn Wheeler wheel** is unverified (XS).

The old engine's pending checks (cures reach the page, Cure Greater over plain
Cure Disease, useful life, the Phase 3 comparison) test code that is frozen.
They move to R8's in-game session.

## How Huginn is measured

Two objective metrics (the user, 2026-10-01); everything else is a proxy:
1. **Did the player reach into the inventory** (or favourites menu) for
   something Huginn could have offered?
2. **Did the player need a slot-manager label or a custom slot** to get it?

The end state is one page of plain keys, with Huginn good enough that nothing
else is needed. Both are counted in the `[Soak]` heartbeat's `goals` field
(docs/playtest/LongPlaySoak.md). In the rewrite, metric 1 is also what the
menu cost κ is fitted to (theory page, P11).

## The perception line

What the HUD shows the player, Huginn may read (the user, 2026-10-02): the
target's health, magicka and stamina bars, its type and race, its equipped
weapons, a spell it visibly casts. Vanilla shows only the enemy health bar, so
a need on enemy magicka or stamina must check a HUD mod (TrueHUD on LoreRim)
draws them, or stay on health. Spell lists and hidden numbers -- resistances,
perks, level -- stay out (CLAUDE.md, Forbidden Information).

---

## Known bugs

- **`870710C4` warns at every load**: a non-playable, nameless Requiem weapon,
  correctly rejected. Quiet the warning for non-playable forms (XS).
- **Rate clocks use Calendar game time at 4320 s/day**
  (`StateConstants.h:605-618`): wrong under any timescale but 20. New decays in
  R3 use `steady_clock`; fix the old ones when they become needs.

The transform cache and the 37 misread races are in R0.

## Mod compatibility

### Default slot keys are the number row
**Status:** deferred (the user does not use Skyrim's hotkeys). `iSlot1Key = 2`
.. `iSlot8Key = 9` are also Skyrim's favourites hotkeys, so one keystroke fires
two systems. Not F1-F8 (F5 quicksave); ten slot keys, not eight; many keyboards
have no numpad; a new default does not change an existing INI. Fixes, cheapest
first: a different default; [modifier keys](#modifier-key-bindings); a
read-only widget mode. The double-fire counts as a reach-in for metric 1.

### An overrides directory, so mod authors can ship their own
**Status:** open (the user, 2026-10-02); lands with R11's per-list override
layer. `Huginn_Overrides.ini` becomes a directory, SPID `_DISTR.ini` style.
Plugin-relative keys (`[Spell:0x800~Mod.esp]`), never runtime FormIDs, names
or editor IDs; alphabetical precedence, the player's file last; missing plugins
skipped quietly; one bad file cannot break the rest; a documented vocabulary
with a format version. `hg dump all` ships in release for authors.

### Vanilla-build integration pass
**Status:** partly done on simonrim-essentials. Still open: #79's four contexts
no LoreRim character can carry, the fortify potion payload from #63, and the
`Fatigue: Slightly Tired (lvl 1)` vs `Fatigue - Drained` mismatch (one console
read of the Exhaustion global, Survival.esl 0x816). Redo on the rewrite.

## Platform and dependencies

### Move off CharmedBaryon CommonLibSSE-NG; support Skyrim 1.7.104
**Status:** paused, waiting on LoreRim 5.1. Local branch
`commonlib-ng-alandtse` (b348a9f) builds but does not load -- fix the
trampoline allocation first on resume. Target: alandtse/CommonLibSSE-NG. Keep
`Util::GetItemCountSafe` regardless. Do not mix with the rewrite branch.

### Drop dMenu, move the settings UI to SKSE Menu Framework
**Status:** parked (L; M for the panel alone). The prize is deleting our own
D3D/ImGui hook (604 lines); the cost is compile-time coupling, and declarative
JSON becoming C++. Decide framework v2 or v3 first.

### Modifier-key bindings
**Status:** open (M). Every binding is a bare scancode
(`KeybindingSettings.cpp:22-31`). The real question is tap / double-tap / hold:
is the modifier latched at key-down or sampled throughout?

## Performance

A budget list, not a work list: a felt stutter is the trigger. Latest capture
(0.22.14, Debug): `Inventory::DeltaScan` 7.32 s, `PollPlayerMagicEffects`
3.72 s, `PollTargets` 2.66 s, `Display::Wheeler` 2.49 s. Method:
[profiling/tracy-traces.md](profiling/tracy-traces.md).
- `Inventory::DeltaScan`: gate on `TESContainerChangedEvent` with a slow
  safety timer; compare counts in place.
- `PollPlayerMagicEffects`: early-out when the active-effect list is unchanged.
- `PollTargets`: build outside the write lock; `MAX_TRACKED_TARGETS` 50 → ~12.
- The rewrite adds per-tick work (need vector, cross-features, σ): measure it
  in R8 before optimising anything else.

---

## Decided against

One line each, so they are not reopened by accident. Dates are when the user
decided.

- **Tuning the old engine** -- frozen for the rewrite (2026-10-08). This covers
  the balance stopgap (per-type `lambdaMax`, context bands), the score-jumps
  churn lever, step-size normalisation and the workstation-in-combat
  suppression.
- **Surprise-weighted (inverse-propensity) updates** -- the logged page needs no
  reweighting, and propensity is undefined on a deterministic page (theory P5,
  2026-10-08).
- **Pooling by class (warm start, hierarchical)** -- replaced by the shared θ
  and family/specific effects (theory P10, 2026-10-08).
- **A tier preference for higher spell ranks** -- items with the same effects
  inherit the same weights (theory P1, 2026-10-08).
- **Grading the context rules** -- rules become needs; θ grades them
  (2026-10-08).
- **Enchanted gear out of combat only, as a hard rule** -- the learner decides
  it (2026-10-08; reverses 2026-09-30).
- **Powers and shouts** -- a grab bag across mods, kept on the favourites list,
  context hard to judge; powers maybe a bonus later (2026-10-08).
- **Line-of-sight logic for target type** -- combat is all or nothing in the
  engine, and the crosshair sees one actor (2026-10-08).
- **Ingredients** -- only used at an alchemy lab (2026-10-07).
- **A fixed hold margin** -- replaced by the Bayesian challenger rule
  (2026-10-07).
- **A hand-set minimum utility** -- under a logit any item beats a blank key; a
  floor would need a measured cost per shown item (theory P9).
- **Food as the last emergency fallback** -- food does not do what an emergency
  needs (2026-10-02).
- **Grading the learning target by outcome** (a potion drunk at 15% HP scores
  higher) -- duplicates what context knows (2026-10-02).
- **Two quick drinks of one potion count once** -- one choice; two different
  potions are already two (2026-10-02).
- **A wildcard combat toggle** -- per-slot `bWildcardsEnabled` covers it
  (2026-09-29).
- **A 10 s workstation hold after the crosshair leaves the bench** (#175) -- it
  kept craft context alive walking through town (2026-10-06).
- **A wait mode for home keys** -- a returner that cannot go home shows where it
  lands (2026-10-06).
- **Slot state in the cosave** -- slot management stays stateless across saves.
- **Source as a learning weight** -- source is a label only (2026-10-02). In the
  rewrite the menu has a learned cost κ, which is a choice-set fact, not a
  source weight.
- **Expiring learner entries by inventory, and decaying n** -- replaced by the
  useful life (0.23.6), which survives on b in the rewrite.
- **F1-F8 as default slot keys** -- F5 is quicksave.
- **Editor IDs as override keys** -- the game drops them without powerofthree's
  Tweaks.
- **Telling the player where a need can be met** (an innkeeper refill) -- a
  separate mod idea of the user's ("immersive hints"), not Huginn.
