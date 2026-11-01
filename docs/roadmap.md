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

All eight items are done in PR #183 (`r0-cleanup`, v0.23.8), waiting for the
second agent's check; this section leaves the file when that PR merges.
Detail in the PR and the [implementation map](architecture/9-implementation-map.md#phase-0-behaviour-neutral-cleanup-can-ship-now).
Left for the game: run `hg dump races` on LoreRim and
`python -I tools/races/check_race_reading.py <Huginn_Races.csv>`; the host
tests (`tests/core/ActorTypeClassifierTests.cpp` in `huginn_core_tests`, since
R1; the classifier is `src/core/ActorTypeClassifier.h`) match all 539 rows and
20 actor-keyword cases, and made-up races pin the rule parts no real race
exercises.

### R1. Host test target

The suites in `src/Tests.cpp` run only inside the game, so an agent cannot
prove a change. Add a test executable that builds and runs on the host.
- [x] A CMake target (e.g. `huginn_core_tests`) over a `src/core/` of pure
      code: no `RE::` or SKSE includes. A small header-only framework from
      vcpkg, or plain asserts. *(0.23.9: `huginn_core_tests`, doctest,
      `tests/CMakeLists.txt`; configure fails if a core file reaches for the
      game.)*
- [x] Rule for the rewrite: every new piece of math lives in `src/core/`
      (response curves, the learner update, σ_Δ and the challenger rule, the
      effect mapper over plain records) and gets host tests. The game layer
      only reads forms and calls it. *(`src/core/README.md`.)*
- [x] Done when: the target runs in CI-like fashion from the command line with
      a non-zero exit on failure, and one existing pure function (a curve, or
      `SlotClassCap::Factor`) is ported with tests as the pattern. *(`ctest -C Debug
      --test-dir build`; `SlotClassCap::Factor` -> `core/SlotClassCapMath.h`.)*

**And an unattended in-game run** for the code that needs real game data. The
Debug suites that run at the main menu (`RunUnitTests()` at kDataLoaded,
`Main.cpp:682`) need no save. 18 more (SlotClassCap, SlotLocker, home keys,
cosave, the registries; `Main.cpp:448-465`) run only after a save loads,
including the ratio tests R7 must update:
- [x] Huginn: after the suites, log one sentinel line with pass/fail counts;
      with a test flag set (INI or environment variable), quit the game.
      *(`src/TestHarness.h`: `[HuginnTest] RESULT` per batch, `DONE` in test
      mode; flag = one-shot `Huginn_TestMode.ini` or `HUGINN_TEST_MODE`.)*
- [x] `tools/ingame/run_tests.py`: launch through MO2's command line
      (`ModOrganizer.exe -p "Simonrim Essentials" "moshortcut://:SKSE"`;
      LoreRim's executable is `LoreRim`, profile `Ultra`), wait for the
      sentinel in the Huginn log, kill the game on a timeout, exit non-zero on
      failure. simonrim first: it reaches the menu faster.
- [x] Auto-load a named test save after the main menu, so the after-load
      suites run too (needed by R7). *(`BGSSaveLoadManager::Load(name, false)`
      3 s after the main menu opens. How to run:
      `docs/testing/TESTING-INDEX.md` section 1a.)*
- [x] Proven by a live run on simonrim (a `DONE` line from a real launch).
      *(2026-10-08: the save loads by name without `.ess`; Huginn ends the
      game and MO2 closes by itself. On the vanilla+ profile (the test
      character, the runner's default) the strict run -- no `--allow-skips`
      -- passes 19 of 19 in about 20 s from launch to DONE; test mode
      supplies the healing potions RunItemRegistryTests' sort check needs.)*
- Rule: agents launch the game only when the user has said the machine is
  free; the game must not already be running.

### R2. Effect extractor

Map Phase 1. Describe every item as cap(i) from game data. Done in 0.23.12
(`r2-effect-extractor`) except the two armour items, which change candidates
and accept% and so ship as their own small PR.
- [x] `src/effect/`: a reader (game forms → plain records, kDataLoaded) and a
      mapper in `src/core/` (records → the 239 columns of `9-data/effects.csv`; 243 since 0.23.16),
      with the layered actor-value resolution and load-order percentiles.
      *(`effect/EffectReader`, `core/EffectRules` + `core/EffectMapper`,
      `core/MiniRegex` for the rule tables; the mapping runs on a worker
      thread. Measured live, Debug build: vanilla+ read 0.27 s + map 0.9 s,
      Simonrim 0.55 s + 2.4 s, LoreRim 1.4 s + 7.3 s; Release on the host
      tool maps LoreRim in 0.5 s.)*
- [x] Shared helpers move from `ConsoleCommands.cpp` to `src/util/FormRead.h`.
- [x] Static cap in a catalog; runtime cross-features (`overshoot_*`,
      `weapon_charge`, `stack_count`, `ammo_matches_launcher`,
      `school_fortified`) as `src/core` functions + `hg cap`. Per-instance cap
      for tempered and player-enchanted weapons. *(`effect/EffectCatalog`,
      `core/CrossFeatures.h` + `effect/CrossFeatures`. Not computed per tick:
      nothing reads them yet. The per-tick computation moved to R4.)*
- [ ] **All carried armour is a candidate** (the user, 2026-10-08): lift the
      `ApparelClassifier` scope guard. Gear in combat is not a hard rule; the
      learner decides it. Armour menu picks are dropped today
      (`ExternalEquipListener.h:81-98`); lift the skip, though it shifts
      accept% (decided by the user 2026-10-08). *(R4 lifted it for the
      selection log v3 only; the learner and accept% take armour picks with
      this item.)*
- [x] `hg dump all` prints the catalog view and closes the eight dump gaps
      (doc 9, "Needs and effects, enumerated"). *(`effect/EffectDump`; gap 6
      reads the MGEF's VMAD from the winning plugin file, gap 7 is moot.)*
- [x] **The tables PR (0.23.34, before R6; the user approved one)**, from
      the 2026-10-10 whole-load-order audits of the LoreRim dump: the mapper
      reads each row's conditions (`core/EffectConditions`). Perk-gated rows
      (Impact's stagger, Deep Freeze, Respite, the "(Improved)" rows) leave
      cap(i) and ride beside it with their perk FormIDs (`hg cap` shows them
      and whether the player has the perk; `PerkAdjustedCap` adds the ones
      met, on the main thread only -- R8 copies the player's perks out with
      the StateManager snapshot); GetRandomPercent procs grade their expected
      strength;
      target-family "bane" rows leave the generic columns (bane columns
      after R6, below). Misroutes fixed: Turn Undead damage, Nox family
      keywords, the ash-pile helper, the mark / transmute / knock names,
      Potion of Dispel, Varla Stone. Cloak and hazard payloads grade over
      their wrapper's duration; weapon stats grade within their OCF sub-type
      (quarterstaff among quarterstaffs). Pairs: magicka_deficit ×
      kind_weapon; a negative combat_regen_ok × kind_food ("no eating in
      combat", off while health regeneration is suppressed). Coverage on the
      LoreRim dump 98.97% (25,602 of 25,869). Left: Mind Vision, Thoughtsteal
      and Gather Shadows stay misrouted (their text says nothing the table
      reads: an override-file job); the 223 "(Improved)" rows with no
      condition in the dump are counted as the item's own.
- Done when: the mapper's host tests pass on rows taken from the three dumps;
  coverage on the LoreRim dump ≥ 98.9% and on vanilla+/simonrim ≥ 99%; a
  coverage-diff report lists items that have a slot class today and an empty
  cap. Changes no scores. *(Host tests: `EffectFixtureTests`, expectations
  from the Python reference extractor. Coverage with `huginn_effect_report`
  (TESTING-INDEX section 0): numbers and the diff in the PR.)*

### R3. Need vector, logged only

Map Phase 2. The rules keep scoring; the vector is computed and logged beside
them. Built in PR #188 (`r3-need-vector`, 0.23.14); verifier round 1 fixes
in. What was built: the [implementation map](architecture/9-implementation-map.md#phase-2-describe-situations-need-vector).
- [x] `NeedId`/`NeedVector` from `9-data/needs.csv` (new columns `curve_kind`,
      `curve_p1`, `curve_p2`, `r3_input`; `tools/needs/make_need_ids.py`
      generates `core/NeedIds.h`, a host test fails on drift);
      `core/ResponseCurve.h`; a `[Needs]` INI section (one key per need,
      hot-reloadable).
- [x] Sensors: encumbrance ratio, per-element decaying damage rate, combat and
      submerged timers on `steady_clock`, the held multi-hot target families
      (union of hostiles in combat themselves; no line-of-sight logic, the
      user 2026-10-08), target summoned / casting / archer, restore pending,
      drop ahead. 87 of the 92 needs have a sensor; five are deferred (map
      Phase 2). (88 of 93 since 0.23.19 added `deep_water_ahead`, below; 89 since 0.23.31's thirst.)
- [x] Drop ahead (the user, 2026-10-08): a Havok ray cast straight down from
      2–3 points ahead of the player (`bhkWorld::PickObject`), not a guessed
      floor and not the terrain heightmap, which sees through rock meshes.
      Method in `9-data/needs.csv` (`drop_ahead`); each point first reached by
      horizontal picks at waist and knee height above the previous point's
      ground, so walkable slopes stay known and a wall, a parapet or an
      invisible wall reads "not measured", never a cliff. Cast on the main
      thread from a `PlayerCharacter::Update` hook under the world's read lock
      (in gameplay the update loop runs on job threads, traced; so do SKSE
      tasks, seen by an earlier verifier round but not in a Tracy trace). Proven
      live with the hook build: every monitor snapshot of two scripted sessions reads a
      measured drop, every plain run must see one to pass, and six `coc`
      cell changes across two worldspaces ran clean.
- [ ] **In game (you):** stand at cliff edges, on rock spires and bridges, and
      above deep water; `hg needs` shows `drop_ahead` high only at a real drop.
- [x] Deep water ahead (0.23.19; the user: "rework the water detector like
      the cliff/altitude detector"). `underwater` stays as it is (head below
      the water height: it cannot predict, so a pre-dive Waterbreathing never
      surfaced). The drop-ahead probe pass already read the water height at
      each reached point, and its down ray passes through water to the bed,
      so the depth there is the surface down to the hit (to the ray's bottom
      with no hit). Two changes, `core/DropAhead.h` `MeasureAhead`: a landing
      in water at least `kSafeLandingDepth` (128 units, one actor height)
      deep is no drop -- Skyrim takes no fall damage in deep water, and a
      cliff over the sea read as a lethal drop -- while shallower water is
      still measured to its surface; and a new need, `deep_water_ahead` (P3,
      Environment, logistic c 128 slope 0.05, answered by Waterbreathing),
      the deepest water over the known probes, logged only like every R3
      need. Same staleness and skips as `drop_ahead`; while swimming it reads
      0 (swimming and underwater cover a player in the water). `hg needs`
      prints it beside the drop; the `[DropAhead]` debug line prints each
      probe's hit, raw water height and depth (`[hit Z water Z depth N]`),
      and how many points read water from a neighbouring or an unknown cell.
      Water is read from the cell each point lies in (fix round); since
      0.23.21 a cell reporting the XCLW "use the worldspace default" sentinel
      reads the worldspace default water (the sea, many lakes), else unknown. Host
      tests: a cliff into deep water, into a shallow stream, onto rock, a
      lake shore on flat ground, unknown probes, no hit over water; mixed
      points, water at the hit, infinities, the no-water sentinel, a no-hit
      bottom from a ray cast off lower ground.
- [ ] **In game (you):** the safe-landing depth (128 units) is a guess -- the
      game's threshold is not known -- and the deep-water need is untested in
      game. With `hg needs`: a cliff over deep water reads `drop_ahead` 0 and
      `deep_water_ahead` > 0; walking to a lake shore raises
      `deep_water_ahead` before the player is in the water; a cliff onto a
      shallow stream still reads a drop. If a jump into water the probe calls
      deep does hurt, raise `kSafeLandingDepth`.
- [x] The LoreRim session of 2026-10-10 (0.23.21, ~155 max health; 0.23.22
      acts on it):
      - **The sea jump: safe; the probe was wrong, fixed.** At 12:08:40 the
        probe on a cliff over the sea read `drop 3936 | deep water 0` with
        `[hit void water -14000 depth 0]` at feet -8526: no ground within the
        4000-unit ray, whose bottom (-12462) lay above the sea, so the depth
        read 0 and the void a drop. The player jumped (a 5471 fall), landed
        swimming, unhurt at hp resolution (`health_falling` 0.13 and
        `physical_damage_rate` 0.04 appear 1.5 s after; hp read 100.0%). The
        water read was right (-14000, the
        player's own water while swimming). 0.23.22 lengthens the down ray to
        16000 (`DropProbeConfig::rayLength`: water is seen from cliffs up to
        15808 over it); a host test replays that cliff.
      - **Fall damage and the drop_ahead refit (data-derived).** 36 falls
        (`[Falling] end -- peak depth N` and the `[Needs]` lines in the 3 s
        after): none under 329 units hurt; 7 of 13 between 329 and 444 did a
        little (peak `health_falling` 0.07-0.58); every fitted one from 493
        hurt (493 0.82, 536 0.99, 613 and 686 1.00, taking 47% and 76% of
        health).
        The old curve (logistic c 600 slope 0.01) read 500 as 0.27.
        `tools/needs/fit_drop_curve.py` (least squares of the logistic to the
        peak `health_falling`, 32 falls; a slide at 12:08:36, the sea landing
        and two falls already losing health left out) fits centre 466, slope
        0.029 (bootstrap centre 445-485; the slope is loose), now the
        `needs.csv` default: 330 0.02, 400 0.13, 490 0.67, 600 0.98. A curve
        fitted from logs, not a hand-tuned one; it holds for LoreRim at ~155
        health (fall damage scales with the modlist's settings and max
        health), so refit with the script from another setup's log.
        0.23.23 hardens the script (PR #196's verifier: on the 2026-10-09 log
        it had silently fitted centre 791 slope 0.005, the grid's edge,
        crediting combat damage to a fall and reading falls during healing as
        harmless): a fall with `in_combat=` or `restore_pending_health=` in a
        `[Needs]` line within 3 s of its end, or the `[Context]` hp rising
        across that window, is left out; the damage window stops at the next
        `[Falling] start`; an optimum on the grid's edge prints a WARNING;
        `--exclude` takes a full date-time. Refit: the 2026-10-09 log keeps 2
        of its 12 falls, too few to fit (it now says so); the 2026-10-10 log
        (`--exclude 12:08:36`) keeps 28 (four more out for combat, 686 among
        them) and fits centre 465, slope 0.028 (bootstrap centre 440-490, 37
        of 200 resamples on the slope's edge) -- within a unit of the
        shipped 466 / 0.029, which stays. From 0.23.23 the `[Falling] end`
        line carries the health before the take-off and at the landing poll,
        and a `[Falling] landed` line ~1 s later the health after the
        damage; the script fits to that measured loss when it is there.
        0.23.29 (trace-04 verification §4: a 549-unit "fall" at 18:47:11 took
        1.51 s from the gate and did no damage, a slide): the script rejects
        slides itself, a fall whose gate-to-peak descent was slower than a
        free fall from rest under Havok's 686 units/s^2 allows (plus one poll
        and two frames; the logged falls' own gravity is ~950), and reads
        several logs at once. It judges game time, not wall time: Wheeler's
        wheel (SlowTimeScale, LoreRim 0.2), the Slow Time shout and bow zoom
        slow the game and the fall, so the end line logs the game time from
        0.23.29, and on older logs a fall with a wheel open inside it is
        kept, unjudged (11:32:35, a real fall that began with the wheel
        open). On the five logs of 2026-10-10, with no `--exclude`, it
        rejects five (12:08:36, 18:47:11 and three harmless ones) and fits
        centre 467, slope 0.0295: the curve stays. `FallTracker` (now
        core/FallTracker.h, host-tested) applies the same bound on a game
        clock (DropAheadProbe's PlayerCharacter::Update hook sums each
        frame's wall time times the global time multiplier Wheeler writes,
        or the update's delta if smaller): a descent slower than a free fall
        re-anchors the take-off Z, so a slide no longer accumulates depth,
        while a fall seen through an open wheel keeps its depth. One frame
        counts at most 0.1 s of game time (whether the first frame after a
        pausing menu hands the player's update the whole pause is not
        traced). The host tests show the tracker keeps a free fall when the
        clock is right; only the in-game check proves the clock (the hook's
        sum and the poll's subtraction). In-game check:
        - a slide down a steep slope never shows the Falling reason;
        - a real cliff fall still does, also with Wheeler opened mid-fall,
          and its end line's game time is about a fifth of the wall time
          while the wheel is open (and with the Slow Time shout, whose use
          of the same multiplier is not verified);
        - a pausing menu (favorites, inventory) opened mid-fall keeps the
          depth, and the fall's end line's game time leaves the pause out.
      - **OPEN: false water by a pond.** At 11:23-11:24 six jumps from a bank
        at feet -3005 read water at -3008 ahead; from the bank the probe read
        it shallow (`deep water` 28 and 91). The deep readings were taken
        standing at the bottom: 194 at feet -3242 and 332 at feet -3372 --
        234 and 364 under that "surface" -- yet the player was never swimming
        or under (no `Water:` transition until 11:38), and 3 of the 6 landings
        did minor fall damage. The false water also suppressed a drop: 3
        instead of ~94 on one line from the bank (the -3099 bed ahead, read
        to the water at -3008). That water was likely not there; one
        hypothesis is a placed water plane's height reported outside its
        extent. Not reproducible on demand, so 0.23.23 makes a recurrence
        self-documenting and stops trusting the plane: each probe also reads
        the water at the player's own XY, and a surface more than 150 units
        over the feet (`core/WaterSelfCheck.h` `kFalseWaterDepth`: the logs
        put swimming at feet 83-100 under) while the engine has the player on
        the ground, not swimming and the head not under its water, held 0.5
        s, logs one info line, `[DropAhead] false water: cell ... plane z ...
        is N above the feet at (x, y, z) but the player is not in water`, and
        the probe treats water from that cell at that height (+-4) as unknown
        until the next load (`(false: ignored)` on the status line).
        Swimming, the probe's water at the player is checked against the
        engine's own water for the player (debug, once per load each: a
        match, a MISMATCH). Stays open until a recurrence is logged.
      - **The pond tour (13:14 launch, 0.23.22).** The Eastmarch Imperial
        Camp pond read water -14000 (sea level) with 2-28 deep at the edge;
        no jump. An exterior grotto at 13:51:05 read the bank at -2810, water
        -3400 and the bed 29-64 under it (shallow), so drop 590; the player
        fell 639 and lost 35% of health (100 -> 64.9%), as shallow water
        should. Inside the cave (an interior) the probe's water, -1040,
        matched the player's own swimming water, -1040, exactly. The 11:23
        false-water spot was not revisited (a different exterior, water
        -3008).
- [x] 0.23.24 cleanup (PR #197's verifier): the false-water check is blind to
      an exterior cell's own water, its XCLW or the worldspace default
      (`GetWaterHeight` falls back to it, so a player under it reads under), now said in `core/WaterSelfCheck.h` and
      the map's Known limits with the swim/blacklist ping-pong and the fixed
      head height, and its symptom gets a debug line once per load
      (`[StateManager] water blind spot`); a full blacklist logs once per
      load; `fit_drop_curve.py` warns near the grid's edge, at rms ~0 and
      under 5 damaging falls (the 13:14 launch's 446 / 0.079 now warns) and
      takes combat from the `[Pipeline]` transitions (the morning refit is
      unchanged, 465 / 0.028 from the same 28 falls; 466 / 0.029 stays).
- [x] 0.23.26 cleanup (trace-03, PR #198's verifier): a cell's
      `GetWaterHeight(pos)` is the water at that point -- a placed water
      object or the cell's water -- not one plane per cell (one interior
      cell read -4080, -2944, -1690 and none at different points); the
      comments say so, and that the self-check tests only the water at the
      player's own position, not at the probes ahead. The
      `DropAhead::OwnWater` zone (p50 49.5 us in Debug) formats the engine's
      water only for a line it writes (the likely cost, not measured; the
      next trace tells). StateManager polls take a poll mutex: the Debug
      `ForceUpdate` at load and the debug test commands ran every poll on
      the main thread beside a tick. `fit_drop_curve.py` resets combat at
      each game load (trace-03: the same 16 falls, 4 fitted).
- [x] Computed and logged on its own cadence (`needs/NeedMonitor`, every
      update tick, a `[Needs]` line per signature change, at most one a
      second). **Not** in the skip gate: moved to R8 (below).
- [x] `hg needs` prints the live vector.
- [x] Done when (agent): curve host tests pass; a replayed state snapshot gives
  the expected vector (`tests/core/NeedFixtureTests.cpp`: 151 snapshots, 67
  synthetic (64 before 0.23.19) and 84 recorded in game with the hook build, against an oracle written apart from the
  evaluator); pipeline runs per second and `hg recs 40` match the old build.
- [ ] **In game (you):** a 20-minute session where `hg needs` shows fire,
  darkness, hunger and combat onset firing and expiring.
- [x] Thirst and hunger's curve (0.23.31; the user: "build it", after
      `analysis/2026-10-10/correlations/verification.md` found thirst a stub
      and hunger crossing 0.5 only at Famished). LoreRim's thirst is DVSMP
      Survival Tweaks (`DVSsurvivaltweaks.esp`): its DLL keeps one of six
      stage abilities on the player (0x80F-0x814); `thirst` reads the active
      one / 5, like hunger's SMI stage. Both curves are `logistic 0.5 11`:
      the centre is derived from the stage tables in `9-data/needs.csv`
      (hunger, thirst) -- stage 3 is the first with a regeneration penalty in
      both mods -- and the slope chosen so stage 3 reads 0.75 and stage 2
      0.25. `regen_suppressed` now also counts a survival ability that lowers
      a regeneration rate (the stages, Freezing Water, Addled); the
      snapshot's `healthRegenSuppressed` holds health regen blocked (a cut of
      100+) or debuffed by a spell, potion or disease. The old walk
      skips abilities, so Hungry's Health Regen Loss (HealRateMult) never
      registered (selection log after trace 05: hunger stage 3 read it at 12%
      of contexts, no more than other stages). `hg needs` prints the stages.
- [ ] **In game (you):** with the new `Huginn.ini` deployed (an old one keeps
      `hunger = quadratic 0 1`), get Thirsty and Hungry: `hg needs` shows
      thirst at Thirsty with input 0.60, value 0.75, and `regen suppressed yes (by a survival stage:
      yes)`; the log has `Thirst (DVSMP): stage` and `Survival regen debuff`
      lines at each change. Check Parched (stage 2, a zero-magnitude effect)
      reads stage 2, not 0.

### R4. Selection log v3

Map Phase 3. Log everything the fit needs. Built in 0.23.15
(`r4-selection-log-v3`); schema:
[architecture/9-selection-log-v3.md](architecture/9-selection-log-v3.md).
Logging only: `Huginn_Selections_v3.jsonl` beside the unchanged v2 log.
- [x] Compute the runtime cross-features per tick (`overshoot_*`,
      `weapon_charge`, `stack_count`, `ammo_matches_launcher`,
      `school_fortified`; `effect/CrossFeatures` from R2), for the candidates
      the log holds. First reader: this selection log. *(Every update tick for
      the eligible rows of the last pipeline run, `learning/SelectionLogV3`;
      for the held rows when a context is taken.)*
- [x] Per decision: the need vector, sparse cap per row, explicit outcome
      (key / wheel / menu / nothing), every eligible item (no floor), one row
      per item, wildcard propensity. *(Caps content-addressed and written
      once per file segment; contexts shared by the picks of one menu visit
      and by episodes that start together; propensity = P(slot rolled) ×
      copies/pool, recorded at the roll.)*
- [x] "Nothing pressed": one record per need episode that expires with no
      press, with the page at the onset (the user, 2026-10-08). *(Episode:
      onset at 0.5, expiry below 0.25, at least 1 s, answered by a confirmed
      selection pressed in [onset, expiry + 0.5 s], judged 4 s after the
      expiry; frozen while the game is paused. `core/NeedEpisodes.h`.)*
- [x] Menu choice set: the held items off the page. *(Every carried item and
      known spell the catalog describes is a row (`held`), all armour
      included; H \ A = held, not shown, not equipped.)*
- [x] Armour menu picks reach the log (the user, 2026-10-08: lift the
      `ExternalEquipListener` skip). *(To the v3 log only, `learned: 0`: the
      frozen learner and accept% would change scores. Making all carried
      armour candidates stays in R8.)*
- [x] Settle whether the update loop ticks inside menus. *(It does: 57 ticks
      in 5.9 s in the inventory menu, all with the game paused, and the
      pipeline ran 4–5 times; the page cache was ~100 ms old at the close
      (LoreRim: 9.1/s, 73 ms), so the 500 ms `fExternalEquipTimeWindow` does
      not trip in an ordinary menu visit -- only when the loop stalls (on
      LoreRim the cache was 581 ms old as the menu opened). The v3 log never drops a pick for staleness: a stale pick is
      written with `learned: 0, skip: "stale"`; inventory, magic and
      favourites menu picks join the context taken when the menu opened, with
      its age. 0.23.15 joined favourites picks at the press; 0.23.16 reverted
      it: a favourites pick is a reach-in (the user, 2026-10-09), and on
      LoreRim the favourites menu pauses the game -- 45 of 45 ticks paused,
      10 pipeline runs in a 4.8 s visit -- so the press-time page is not the
      one the player turned away from.)*
- [x] `tools/replay` reads v3; the schema is documented.
- [x] Done when (agent): replay parses a synthetic v3 file round-trip
  (`tests/core/DecisionLogTests.cpp` pins the encoder to
  `tests/core/fixtures/decisions/synthetic_v3.jsonl`;
  `tools/replay/test_replay_v3.py` decodes it and round-trips its own; CTest
  `replay_v3_roundtrip`); an unattended session writes every outcome
  (`run_tests.py --decision-session`: key 3, wheel 1, menu 2, nothing 4 on
  vanilla+, and key 3, wheel 1, menu 2, nothing 5 in one LoreRim run; the
  wheel pick runs Huginn's own Wheeler handler, not Wheeler's UI); volume
  ~0.4 MB/h on vanilla+, ~1 MB/h estimated for a soak-sized LoreRim
  inventory (schema doc); no score changes (`hg recs 40` identical to the base build, pipeline
  11.69 allocations/s against 11.85).
- [x] **In game (you):** a 30-minute session whose log holds all four
  outcomes. Checklist in the PR. *(Passed 2026-10-09 on e8507df. LoreRim:
  key 4, wheel 5 (the real Wheeler UI), menu 3 (2 armour, `learned: 0, skip:
  "armour"`; 1 favourites), nothing 35, no damage. vanilla+, a die-and-reload:
  load generation 2 at 10:54:38; the first context after it was basic
  (`heldFull: 0`, taken before the first pipeline run), the held set warmed
  0.54 s later, the generation-2 key and wheel records were normal (`pipe.ok:
  1`, 8 shown); no errors, no damage. Reload tests go on vanilla+ or
  Simonrim: LoreRim respawns the player at a tavern instead of letting them
  die. The death in that run wrote two `nothing` records 3 s before the
  reload (falling 1.0 s, loadout_restoration 28.7 s); since 0.23.16 a death
  drops the open episodes and those in their grace, as a load does.)*
- [x] **Data fixes before the real R6 fit (0.23.51, `r6-data-fixes`)**, two
      defects the rehearsal of 2026-10-28 found. Logging only: no score in the
      old engine changes.
  - *"Nothing" for persistent needs: laps.* Hunger, thirst, diseased, cold,
    warmth_deficit and fatigue never wrote a `nothing` record for their own
    episodes: any selection anywhere in their hours-long episodes answered all
    of it, and a load dropped them still open. 0 of the mage's 14,014 `nothing`
    records (~45 h, 2026-10-15..29). They are still in `nothing` contexts (the
    fit's u0 = θ_out · n reads the whole need vector of the record's context,
    another need's onset), so the gap is narrower: `nothing` was sampled where
    other needs start, not across the time a persistent need stays on, and the
    bias is mild and mixed (on at picks / at `nothing`: hunger 8.8% / 7.3%,
    thirst 6.7% / 5.0%, diseased 11.8% / 10.5%, warmth_deficit 7.8% / 5.5%,
    cold the other way, 0.2% / 2.2%). Now an open
    episode is cut into stretches on a shared 60 s grid of active seconds
    (unpaused, alive, at most 1 s per tick; the clock holds in a dialogue,
    in any other open menu that blocks gameplay input by its own flags (the
    crafting menu does not pause; 0.23.55 on engine-rewrite, 0.24.7 on rewrite-r8), with the movement controls
    disabled and after 30 s without input, a stick counting only past the
    game's own deadzone (0.23.55)), and
    each unanswered stretch writes
    a `nothing` record marked `ep.why: "lap"` (with `lap` and `epSec`), joined
    to the context the lap took (`ctx.why: "lap"`); an episode that ends
    within 60 s of its onset is judged as before. 60 s: one gap between picks
    in five exceeds it (median 17 s), long unanswered episodes run a median of
    75 s, and a replay on the log gives each persistent need about as many lap
    records as picks with it on (hunger ~100 against ~170), ~0.4 MB/h. Active
    seconds rather than game time: waits, sleep and fast travel jump the game
    clock while no press is possible. The hold (the fix round's verifier):
    20.1 h of the log lie in 261 silent gaps over 120 s; left unattended with
    the game unpaused, a clock without it would have lapped through them
    (2,153 lap ticks instead of 985), while with it each gap adds at most
    30 s (1,101 at most). `core/NeedEpisodes.h`; the rule and the
    numbers: [schema, Laps](architecture/9-selection-log-v3.md#laps).
    `tools/fit` reads them (`fitlib.build(laps=...)`: one per lap tick by
    default, keyed on the records' shared `(launch, gen, utc)`, since the
    needs open at one lap would otherwise weigh one slice k times; `cv.py --laps`;
    `v3load.py --list` counts them); `tools/replay` too
    (`replay.nothing_stretch`, `head_lap_sec`). Records written before
    0.23.51 read as whole episodes (`why: "end"`, lap 0).
  - *Old logs keep both defects.* Every log before 0.23.51 has no laps and
    reads the quarterstaff as `loadout_two_handed`. The R6 fit's three-outcome
    group runs with `--since 0.23.51` once there is enough new play, or with
    `--split-at 0.23.51`: the seven needs concerned are in
    `fitlib.BUILD_SPLIT_NEEDS`, so their old-segment columns take the bias
    ([tools/fit/README.md](../tools/fit/README.md#reading-logs-written-before-02351)).
  - *The quarterstaff is not a two-handed loadout.* `loadout_two_handed` was
    on at 27.7% of the mage's picks and in 33% of the contexts, 85% of those
    with the Staff of Wandering Stars in hand (LoreRim Traits.esp, a
    `kTwoHandAxe` warhammer with Requiem's and Animated Armoury's
    quarterstaff keywords). A two-handed weapon with
    `OCF_WeapTypeQuarterstaff2H`, `WeapTypeQuarterstaff` or
    `WeapTypeQtrStaff` (all 967 two-handed quarterstaffs in the LoreRim dump
    carry the first) is now its own need, `loadout_quarterstaff`, and leaves
    `loadout_two_handed` off (`core/WeaponLoadout.h`; needs.csv, NeedIds.h
    and the `[Needs]` block regenerated; 97 needs). The old engine's
    `hasTwoHandedEquipped` is unchanged. Not changed: the item's own cap still
    reads `weapon_type_warhammer` (the effect columns have no quarterstaff
    type; its stats already grade among quarterstaffs, 0.23.30).
  - [ ] **In game (you):** see the PR's checklist (a lap record in the log
        after a minute of standing hungry or diseased without a pick;
        `hg needs` with the staff in hand).

### R5. Data play (in game, you)

The mage character, the planned next stress test, played on the R2–R4 build so
it doubles as the fit data. Several hours, mixed combat and town. Meanwhile an
agent runs the cheap test on today's log: one weight per slot class × logged
`ctx`; if it does not beat context alone, flag it before R6.

- **Cheap test verdict (2026-10-09): passes weakly.** The test as doc 9
  defines it (`9-context-as-learner-input.md:330`; its "need class" is the
  slot class: the v2 log's `need` column IS the slot class,
  `tools/replay/replay.py:356`): one learned weight per slot class times the
  logged per-candidate context weight, against context alone (replay's B
  arm), on the v2 log (46 launches; 859 key/wheel picks after the analysis's
  filter, from 869 stamped records), held out by launch. hit@1 level (+1.1 to +1.5, CI includes 0), NLL −0.25
  (CI −0.30 to −0.20), hit@8 +4.0 to +4.3. A class × φ interaction on top
  beats context alone by +2 to +4 hit@1, depending on the folds. v3 (92
  picks, the same 4 launches as v2's last) is too small to say. A first run
  that put class × the whole need/φ vector on rows lost by 9 hit@1 on the
  scored set, because key presses are confined to a page the context weight
  built; on the page set it tied or beat context alone (+3 hit@1, NLL −0.11).
  Notes for R6:
  - judge on the page set and on menu picks, not the scored set;
  - compare against a fitted context baseline (a few weights on ln ctx,
    corr, potion, fav), which already captures most of the hit@1 gain;
  - detecting a +2 to +4 hit@1 gain needs roughly 450–1,200 key/wheel
    picks: several hours of mage play;
  - key position alone gets 41% hit@1 on the shown page, so consider a
    key-position term.
  The scripts (`r5-cheap-test/`, `r5-verify/`) lived in the session
  scratchpad and are not in the repo, and the filter from 869 records to 859
  picks is not documented; the rest of the method is reproducible from this
  description.
- **Newly learned spells were not on the HUD before their first menu pick**
  (LoreRim, 2026-10-09). Three spells were learned (SpellRegistry reconcile:
  +1 spell at 12:44:19, 12:45:00 and 12:46:06; the adds log no names).
  Neither of the two examined, Ice Spike and Muffle, was visible on the HUD
  before its first menu pick. Ice Spike (0002B96C) was already a candidate
  -- rolled as a wildcard at 12:48:18.708, and WildcardManager draws only
  from the ranked candidates -- but was not seated on the shown page. Its magic-menu pick at 12:51:25
  was case A (not a candidate in that context), and its context weight read
  ctx=0.00 for DamageMagic at 12:52:15. Muffle (0008F3EB) was seated on page
  0 at 12:49:01.838 ("Sneaking(Muffle)"), before its first pick at
  12:49:02.231 (case E), but only while the MagicMenu was open and the
  widget hidden (12:48:57.458–12:49:02.859), so the player never saw it
  there first. So candidacy depended on context (ctx=0.00 outside the right
  one), not a cold-start block. The user accepts this for now; discovery
  belongs to R10 (wildcards by uncertainty). *(R10, 0.24.3: a new spell has no
  b, so the wildcard draw weighs it at b's prior sd; checked in game under R10.)*

### R6. Offline fit: go / no-go

Map Phase 4, in `tools/fit` (the scripts and their flags: [tools/fit/README.md](../tools/fit/README.md)).
- **Decided by the user, 2026-10-28: R6 is a go; continue to R8 without
  waiting for level 40.** This supersedes the gate of 2026-10-10 ("keep
  playing until the character is level 40, might take a few days"). The
  user's reasons: LoreRim 5.1 will force a new character (both a mid-play one
  and one from scratch); the rewrite likely won't finish before 5.1 drops;
  and mid-game is where Huginn matters most. The fit runs on the mage's log as
  it stands (level 21 at the rehearsal).
- **The criterion** (the user, 2026-10-28; replaces "key hit rate against
  76%" below). The user's fitness function is menu reaches for items Huginn
  could have recommended.
  - Offline gate, held out by launch: menu picks caught (hit@8) above A*, and
    all picks pooled (key + wheel + menu) hit@8 not worse than A*. Both are
    read on the 95% interval of model minus A* (bootstrap over launches):
    "above" means its lower end is above 0, "not worse" means its upper end
    is at or above 0 (the rehearsal's pooled -1.1 [-4.0, +2.2] passes).
  - Key hit@8 alone is reported, not a gate: key presses exist only for items
    the old page showed, so they are biased toward A*, while menu picks are
    biased against it; pooling lets the two biases partly cancel.
  - Final test, after R8: `[Reach]` reach-ins per hour of play against the old
    engine's baseline. R8 rolls back if it loses. The baseline (the October
    soak, docs/playtest/Soak-2026-10-LoreRim.md): 92 reach-ins in 9.1 h, about
    10 an hour, 1.1 to 20.8 per session and driven by the content (roads and
    towns low, the dungeon session high), so compare like content.
- **The rehearsal (2026-10-28, level 21; verified 2026-10-28, not the final
  fit).** Grouped 10-fold by launch over the mage's log. A* (the old ranking
  from the logged utilities): key/wheel hit@8 87.4%, menu 18.5% (82 of 443).
  Doc 9's θ + b: key/wheel 59.3%, menu 53.3%, page hit@1 40.9% against A*'s
  20.7%. θ + b + `recent_<family>` + ln(old utility) as a feature: all picks
  pooled 69.9% against A*'s 71.0% (−1.1, CI [−4.0, +2.2]), menu 61.6%. The
  fixes the rehearsal needed are in the tools (0.23.49): several log files
  and one character (`v3load.py --char`), grouped K-fold by launch (the old
  leave-one-launch-out was ~9 h on 288 launches), build segmentation, the
  yardstick (hit@8 for key/wheel, menu and all picks, page hit@1, page NLL,
  each with a launch-bootstrap interval on model − A*).
  - *The verifier's honest numbers.* The CV above is optimistic: grouped
    K-fold trains on launches after the held-out one, so b learns an item the
    player only took up later from its future. On a train-on-past online
    replay (refit every 5 picks, warm start) the best variant, θ + b +
    `recent_<family>` + ln(old utility), caught menu picks +38 points above A*
    and pooled −2.0 [−5.1, +0.9] (the gate passes); pure θ + b (with or
    without `recent`) caught menu +25 to +29 and pooled −16 to −20 (fails).
- **Open for R8** (noted, not decided): the best rehearsal variant leans on the
  old engine's utility as an input -- the hand-tuned layer R8 and R11 retire.
  *The component test (2026-10-29) and the real fit below* narrowed it: of the
  old utility only the learner's multiplier ln(1 + λ(α) · learningScore) does
  the work (the context weights, favourite, correlation and potion multipliers
  add nothing that passes the gate alone), and no variant without an old-engine
  part passes.
- **The real fit (2026-10-29, 0.23.54, branch `r6-real-fit`).** The mage
  (`FEFD1B039AC78799`) over all three log parts (`-20261015-180003-1`,
  `-20261029-022335-1`, the current file; the soak character and the others
  excluded): 1,865 picks (1,026 key, 396 wheel, 443 menu) in 76 launches,
  2026-10-09 14:29 to 2026-10-28 17:29, builds 0.23.15 to 0.23.44. **These are
  the rehearsal's picks**: the launches logged since (0.23.47, 2026-10-29) hold
  `nothing` records only, and no log reaches 0.23.51 (no laps, the old
  `loadout_two_handed`). Grouped 10-fold CV (seed 1) and the online replay
  (`honest.py`: refit every 5 picks, warm start, 150 picks before the first
  tested launch; 1,715 picks tested in 71 launches, 1,300 key/wheel and 415
  menu), launch bootstrap of 2,000 draws. The component variants are ported
  into `tools/fit` (0.23.54; `fitlib.row_features` `learn`, `mults`,
  `learner`) and reproduce the component test's scratch numbers to 0.1 point.
  Picks model, hit@8 in % (model − A* [95% CI]); A* online: key/wheel 87.1,
  menu 18.3, pooled 70.4, page hit@1 20.2:

  | spec (all with κ) | online menu | online pooled | online gate | CV menu | CV pooled | CV gate |
  |---|---|---|---|---|---|---|
  | θ + b (doc 9) | 47.7 (+29.4 [+23.8, +35.8]) | 54.2 (−16.2 [−20.0, −12.7]) | fail | +35.0 | −13.1 [−17.1, −9.2] | fail |
  | θ + b, presence grading | 51.6 (+33.3 [+27.4, +39.2]) | 55.5 (−14.9 [−19.3, −10.9]) | fail | +38.1 | −11.7 | fail |
  | θ + b, presence + strength | 51.1 (+32.8 [+26.9, +38.8]) | 55.5 (−15.0 [−19.3, −11.1]) | fail | +37.9 | −12.2 | fail |
  | θ + b + `recent_<family>` | 56.1 (+37.8 [+33.3, +43.7]) | 58.0 (−12.4 [−17.0, −8.1]) | fail | +42.0 | −9.2 | fail |
  | + old learner state (L1, no old scorer) | 57.6 (+39.3 [+34.3, +45.2]) | 65.3 (−5.1 [−9.7, −1.0]) | fail | +44.2 | −3.8 [−7.6, −0.1] | fail |
  | + ln util, no b | 49.2 (+30.8 [+26.8, +36.6]) | 64.8 (−5.7 [−8.7, −2.6]) | fail | +38.6 | −3.3 [−6.2, +0.1] | pass |
  | + ln util (tbra) | 56.4 (+38.1 [+33.3, +43.9]) | 68.3 (−2.1 [−5.3, +0.8]) | **pass** | +42.9 | −1.4 [−4.5, +2.0] | pass |
  | **+ learn mult (D2)** | 58.6 (+40.2 [+35.3, +45.7]) | 68.1 (−2.3 [−6.6, +1.6]) | **pass** | +44.9 | −1.3 [−5.0, +2.2] | pass |
  | + learn mult + fav/corr/potion (D5) | 59.0 (+40.7 [+35.7, +46.6]) | 69.2 (−1.3 [−5.5, +2.6]) | **pass** | +43.6 | −0.8 [−4.5, +2.9] | pass |

  "+" rows are θ + b + κ + `recent_<family>` plus the named feature. Page
  hit@1 is 39.8 to 43.1 for every model against A*'s 20.2; key/wheel hit@8
  is 56 to 72 against A*'s 87 (key presses exist only for items the old page
  showed). Paired on the online replay (a − b, pooled hit@8): D2 − tbra −0.2
  [−2.0, +1.4]; D5 − D2 +1.0 [+0.5, +1.8]; D2 − L1 +2.8 [+1.8, +3.9]; L1 −
  θ+b+recent +7.3 [+5.8, +8.5]; recent − θ+b +3.8 [+2.2, +6.2] (menu +8.4
  [+5.0, +12.4]); presence − graded +1.3 [+0.1, +2.3] (menu +3.9 [+1.1, +6.0],
  page hit@1 −2.5 [−3.9, −1.2]); presence + strength − presence −0.1 [−0.7,
  +0.7]. The full tables: the PR's description; the bootstrap's `provenance`
  holds every row above.
  - *Joined picks only (the verifier of 2026-10-29: an evaluation leak).* 27
    of the 1,715 tested picks, all menu equips (armour picks write no v2
    record), have no v2 record, so `ln learn mult` (D2) and the
    favourite/correlation/potion features (D5) read 0 on every row of those
    picks: an all-zero decision tells the model the pick was an unscored item.
    On those 27, D2 and D5 hit 74.1%, tbra (ln util read from v3, no leak)
    51.9%, L1 59.3%, A* 0%. On the 1,688 joined picks, online: D2 menu +37.9
    [+32.4, +44.1], pooled −3.6 [−7.8, +0.3]; D5 pooled −2.5 [−6.6, +1.5];
    tbra menu +37.1 [+32.0, +43.4], pooled −3.0 [−6.1, +0.0]; L1 pooled −6.2
    [−10.6, −1.9]. The gate verdicts do not change, but D2's pooled upper end
    falls from +1.6 to +0.3, and paired D2 − tbra on menu goes from +2.2
    [−0.4, +4.3] to +0.8 [−1.9, +3.0] (pooled −0.6 [−2.3, +1.0]): **D2's menu
    lead over tbra is mostly this artefact.** CV (all) has the same leak (37
    unjoined picks); joined, D2 pools −2.7 [−6.2, +0.7] and tbra −2.6 [−5.5,
    +0.5]. The bootstrap's `provenance` carries the joined-only yardstick of
    every spec (`metrics_online_joined`). The shipped fit still trains on
    those decisions (not fixed here: carrying the learn score forward or
    dropping them is a refit); the train/serve side of the same zeros is in
    the format doc.
  - *Selection.* D2 was picked from about 25 component variants (the
    component test), all scored on this same online replay, so its pooled
    upper end after that selection (+1.6; +0.3 on joined picks) is weak
    evidence that it is not worse than A*. It ties with tbra, the variant the
    rehearsal fixed in advance (paired D2 − tbra pooled −0.2 [−2.0, +1.4];
    −0.6 [−2.3, +1.0] joined): the case for D2 is that it ties tbra with less
    of the old score, not that it beats it.
  - **Chosen for R8's bootstrap: D2**, θ + b + κ + `recent_<family>` +
    `ln learn mult` + `util missing`. It passes the gate on the online replay
    and is the smallest part of the old score that does: **no variant without
    the old scorer passes** (L1, which keeps only the old learner's per-item
    state, misses the pooled check by about one point; pure θ + b by 12 to 16).
    So R8 still needs, live, FeatureBanditLearner (estimate, confidence, UCB),
    PriorCalculator, UsageMemory's recency, λ(confidence) and the old floors
    that decide which rows are scored (and the floors read
    `fMinimumContextWeight`, so the context weights still enter as a gate).
    It retires the context weights as a score term and the favourite,
    correlation and potion multipliers. D5 pools a significant +1.0 point
    better but keeps those three multipliers; not chosen (open below).
  - **The file:** [`configs/Huginn_Bootstrap.json`](../configs/Huginn_Bootstrap.json)
    (format version 1, [architecture/9-bootstrap-format.md](architecture/9-bootstrap-format.md#the-shipped-file-r6-02354);
    the format gained the component feature names). R8 installs it to
    `Data/SKSE/Plugins/` with its reader; nothing installs or reads it yet.
    `provenance` names the files and their SHA-256, the range, the builds, the
    spec and the metrics. Fitted on every pick with two departures from the
    evaluated D2: encumbrance read through the fitted curve (below), and
    `loadout_two_handed` held at its prior (`--hold-needs`; logs before 0.23.51
    turned it on for the quarterstaff in 85% of its contexts; held, the fit's
    NLL on all picks rises 23.7 nats, 0.013 per pick). 459 items keyed, 31
    dynamic and 14 forms the 2026-10-10 dump lacks dropped. κ 2.69; `ln learn
    mult` weight 1.36, `util missing` 0.11.
  - **The encumbrance curve, for R8:** `encumbrance = logistic 0.9887 73.42`
    (centre sd 0.007, Laplace; on the profile the posterior is within 0.6
    nats of its best from 0.98 to 1.00, 2.2 at 0.96 and 5.9 at 0.90). The data moved the centre off the prior's 0.9:
    the profile's data NLL at 0.90 is 10.4 nats worse than at 1.00, so the
    player reaches for carry weight at the limit, not when nearing it
    (Feather on Self is 15 of the 28 carry-weight picks, 13 of them at a load
    ratio of 0.986 to 1.042; the other picks with the column are at 0.32 to
    0.99). The pair's weight is +3.27 (prior +1). The file's
    `curves` carries it; θ was fitted with it.
  - **Three outcomes:** the same picks plus 14,014 `nothing` records
    (15,879 decisions in 293 launches), fitted on the old logs as they are (no
    laps, the old `loadout_two_handed`; README, "Reading logs written before
    0.23.51"). Grouped 10-fold CV, hit@8 against A* on the same choice sets:
    θ + b + u0(n): menu 51.7 (+33.2 [+26.3, +40.9]), pooled 58.6 (−12.6
    [−16.3, −8.8]), page hit@1 40.6 (+19.8); θ + b + u0(n) + `recent`: menu
    60.1 (+41.5 [+35.5, +48.6]), pooled 62.1 (−9.0 [−13.5, −4.5]), page hit@1
    40.8 (+20.0); both fail the gate, as their picks-only twins do. The
    `nothing` outcome: held-out NLL per decision 0.614 [0.582, 0.643] with
    `recent` and 0.635 without, against 0.701 for κ + u0(n) alone and 0.687
    for A*'s ranking + u0(n). Online replay of θ + b + u0(n) + `recent`
    (refit every 25 picks, not 5: one fit on the 15,879 decisions takes 20 to
    60 s, and the 603 refits of `--every 5` would have taken about 5 h; 341
    refits, 2.4 h): menu 53.5 (+35.2 [+30.6, +40.2]), pooled 56.3 (−14.2
    [−19.1, −9.8]), fails, like its picks-only twin (−12.4 at every 5). The bootstrap is the picks model
    (`outcomes: "picks"`, no u0): R8 ranks, and a u0 learned from `nothing`
    records sampled only at other needs' onsets is the bias 0.23.51 fixed; refit
    u0 once there are lap records (`--since 0.23.51`).
- **Open for you (R8):** (1) D2 or D5: D5 pools +1.0 [+0.5, +1.8] better
  online but keeps the favourite, correlation and potion multipliers. (2)
  Presence grading: menu up, page hit@1 down (above); the bootstrap stays
  graded. (3) `loadout_two_handed`: held at its prior in the file; the
  alternative is to ship its fitted row as `loadout_quarterstaff` (85% of its
  old contexts were the staff). (4) b is this character's: whether R8 applies it
  to the next one (LoreRim 5.1) or starts b at 0 (θ + `recent` + ln util
  without b fails online: pooled −5.7 [−8.7, −2.6]). (5) D2 keeps the old
  learner and floors running. Since 0.23.53 (#222) every v3 row logs the old
  learner's state, the floor reason and the `favorite` bit (the gaps the
  component test listed); the fit cannot use them yet (no log here reaches
  0.23.53), but a refit on logs from 0.23.53 on can test whether the floors'
  scored membership or only the learner score carries D2's edge.
- **Tools for the real run and R8 (0.23.52,
  [tools/fit/README.md](../tools/fit/README.md)).** `honest.py`: the
  rehearsal's verifier found grouped K-fold flatters the model (it trains on
  launches after the held-out one), so the yardstick is now also read on a
  train-on-past online replay (refit every 5 picks, warm start), beside the CV
  on the same decisions, with the gate verdict for each; the online verdict is
  the honest one. `curve.py`: the encumbrance curve fit (below). `export.py`:
  the bootstrap file R8 reads, format version 1
  ([architecture/9-bootstrap-format.md](architecture/9-bootstrap-format.md);
  golden examples in `tests/core/fixtures/bootstrap/`), with b keyed by plugin
  + local FormID.
- [ ] Conditional logit with the three outcomes (key, menu at cost κ, nothing),
      u0 per need, sparse θ, small b, Plackett–Luce co-picks; split by launch.
      *Not done: the real fit ran it (above, "Three outcomes") only as a
      plain fit over the old logs, which the data note below rules out (none
      reaches 0.23.51, so neither `--since` nor `--split-at 0.23.51` had a new
      segment, and the bias stays in). Those numbers are a preview; the box
      waits for a run on 0.23.51+ data.*
      **Data (0.23.51):** logs before 0.23.51 have no lap records and read the
      quarterstaff as `loadout_two_handed`; run the three-outcome group with
      `--since 0.23.51`, or `--split-at 0.23.51` (the default
      `--split-needs` splits the seven needs concerned), never a plain fit
      over every file ([tools/fit/README.md](../tools/fit/README.md#reading-logs-written-before-02351)).
- [x] Report on held-out play against A* on the same choice sets, by the
      criterion above (`cv.py`'s yardstick). *The real fit's table above:
      online replay and CV.* *Superseded 2026-10-28:* "key
      hit rate against **76%** (arm A*, the plain ranking replay can
      reproduce; 81% was the live page with holds), and menu-pick hits against
      **7 of 83**" -- key hit@8 is now reported only, not a gate.
- [x] Sequences and redundancy, offline part (proposal,
      [architecture/11-sequences-and-redundancy.md](architecture/11-sequences-and-redundancy.md),
      steps 1-2): compare pairs on presence plus a strength term with the
      current grading (Feather's carry-weight grade is 0.0046), and test
      `recent_<family>` features (window up to 30 s), on held-out picks.
      Decided 2026-10-10: the grading switches, and `recent_*` reaches R8,
      only on held-out lift; the encumbrance curve is fitted from the
      player's carry-weight picks, starting from a documented prior on its
      centre near 0.9 (the user's "nearing" answer). *The fit is
      `tools/fit/curve.py` (0.23.52): prior c ~ N(0.9, 0.05²), ln slope ~
      N(ln 40, 0.5²); only the obvious pair (and its strength twin) reads the
      curve -- with every pair of the need free, the need's dense weights
      fitted the load itself and pulled the centre to the grid's floor.*
      *Results (the real fit): `recent_<family>` lifts held-out picks
      (online pooled +3.8 [+2.2, +6.2], menu +8.4), so it reaches R8 (it is in
      the bootstrap). Presence grading is mixed: menu +3.9 [+1.1, +6.0] and
      pooled +1.3 [+0.1, +2.3], but page hit@1 −2.5 [−3.9, −1.2]; presence +
      strength equals presence. The bootstrap stays graded; switching is
      yours to decide (open below). The encumbrance curve: logistic 0.9887
      73.42 (above).*
- [ ] **`bane_<family>` columns, after R6's first fit** (the user,
      2026-10-10). Rows that fire only on a target family (Requiem's triple
      shock damage to constructs 8719DC31 on 776 weapons, "deals 20 magic
      damage to undead" 8717D82B on 589, Banish's damage to daedra 8719DC30,
      Turn Undead's stagger) left the generic columns in 0.23.34 and wait
      here: one column per target family (undead, daedra, construct, dragon
      ...), paired with the `target_<family>` needs, graded within its own
      population. A column count change (EffectColumns static_assert, the
      golden log, the pair count). In-scope forms with only bane rows today
      have no effect column (Dragonbane, Bow of the Hunt, Purify); the bane
      columns describe them again. Command Daedra is exempt (its
      influence_command names its target, as Banish does).
- [ ] **Target-gated beneficial rows** (follow-up to the above, verifier of
      #205): some bane-only forms are heals or buffs for an ally of a family
      -- Heal Undead / Heal Daedra / Necromantic Healing, Arcane Mending,
      Daedric Mind -- which a bane column (harm to a family) does not
      describe. They need their own reading (an ally-family heal or buff),
      after the bane columns. Thundering Hooves belongs here too:
      movement_speed on a ridden horse, a bane-only beneficial row on 4
      forms (verifier of #205, 0.23.35).
- **You decide:** go to R8, or stop and rethink. *Decided 2026-10-28: go*
  (above). θ from this fit is the bootstrap for R8.

### R7. Slot code handles any score sign

Map Phase 5. Lands before the new scorer. Built in 0.23.11 ("R7: sign-safe
slot code"); detail in the map's "As built (R7)".
- [x] Bridge: score = ln(utility), σ = 0, m = 1.5, which reproduces today.
      One function, `ScoredCandidate::SlotScore` (`Core::BridgeScore`); the
      score is a double, so no two utilities collapse into one score.
- [x] The slot class cap, ratio logs, the `-1` "gone" sentinel, `utility = 0` for
      remembered-only rows, `kOverrideUtility`, the widget bar, the
      confidence payload: all made sign-safe. The allocator's decisions moved
      whole into `src/core/SlotAllocCore.h` (plain records, host-tested); the
      cap is `+ k ln d`, the hold `s_c − s_i > ln m`, "gone" an empty
      optional, unranked rows −inf, overrides +inf (never compared).
- [x] Full sort instead of the top-10 partial sort (`std::stable_sort`; the
      selection log still reports ranks for the top 10 only, so attribution
      is unchanged).
- [x] Done when: a golden test feeds recorded pipeline snapshots through the
  old and new slot code and gets identical pages; the ratio tests are updated.
  The old code's own pages and events, recorded in play on vanilla+ under the
  shipped [SlotLocker] settings and two variants plus a campaign (branch
  `r7-capture-old`: 0.23.9 decisions except b941d90's one clock reading per
  allocation, capture only; 2,559 snapshots, 425 checked in), replay bit for bit through the core with the old arithmetic;
  the sign-safe arithmetic gives identical pages and events on all of them
  and on 24k synthetic passes; 2,578 allocations the new code recorded (230
  checked in) replay exactly. Five seeded mutants of the core (pull, Remembrance
  to the job key, the hold's stale-generation guard, the refill after the
  pull, the override fallback's event) all fail against the checked-in
  fixtures; a sixth (the pull taking a wildcard) fails a named test. `RunSlotClassCapTest`, `RunSlotClassCapHoldTest` and
  `RunHomeKeyTest` derive their utilities in score space.
- Different by design: the full sort changes a page wherever the old fill
  reached past the sorted top 10 (on recorded play lists: 22 of 1,094 pages
  in a capture under the shipped settings only, 446 of 1,450 in the latest,
  half of it under the job-key variants); exact
  ties now keep generation order rather than the partial sort's. A
  challenger exactly on the hold's margin now always holds (a tie band of
  2.5e-7, two to four float ulps, `kHoldTieEpsilon`; the user's decision,
  2026-10-08): the old float comparison was decided
  by rounding there, and the old potion tier step (1.5) equals the margin, so
  adjacent potion tiers sit on it -- in a sweep the old code swapped 23 of 260
  such pairs where the new one holds. The cap's scan at non-power-of-two
  discounts and an underflowing discount have rounding boundaries of their
  own; tests classify them. No recorded snapshot differs.- For R8: `ScoredCandidate::operator<` still orders on utility (the same
  order as the score under the bridge); it moves to the score with the scorer.

### R8. Cutover

Map Phase 6, plus the cosave from Phase 9 (θ must survive a load before any
soak). Order inside: hard zeros to `CandidateFilters` first; `combat_onset`
sensor from `PotionDiscriminator`'s timer; `ChoiceLearner`; scorer; cosave
`THTA`/`BIAS`; then the prune list in the map, with every consumer of a
pruned symbol (console, selection log, `ReasonHold`, debug widget, `Tests.cpp`)
changed in the same PR, and the shipped INI loses the dead keys.
- [x] Before the cutover (doc 11, steps 3-4), moved ahead of R6 so the
      rest of the R5 data play (the mage, level 21 to 40) records it: a
      `covered(i)` cross-feature on held rows, a `filtered_by` reason on rows
      the filters drop (for a shadow estimate of its weight: choice sets by
      `shown`, a zero-centred prior), and the summon count and limit; the
      summon filter counts against the limit (the Twin Souls bug). Decided
      2026-10-10 (doc 11, section 7): at the limit a summon scoring above the
      one it would replace is still offered (greedy upgrade); a buff near expiry
      reads partly uncovered; thralls await the user's test.
      **Built in 0.23.46** (logging only, plus the summon limit fix):
      - `filtered_by` is the row column `fb`, for every filter, not only the
        active-buff ones (15 reasons, `core/FilterReason.h`); the `[Reach]`
        and `[Selection] Confirmed` debug lines add `filtered_by=` for a
        case-A pick (`unscored` = eligible but under the floors,
        `not-gathered` = no registry produced it), the case label unchanged.
      - `covered` is cross-feature 7, with the refresh prior (5 s, scale 2 s)
        applied and its remaining seconds in the row column `cr`
        (`core/Coverage.h`). The greedy upgrade gate is R8's (it needs ŝ).
      - Contexts carry `summon`: the count, the perk limit, and every
        commanded actor with its source, kind (summon / reanimate) and timer.
      - The summon filter hides summon spells once the count reaches the
        limit (`kModCommandedActorLimit` from 1); with the vanilla limit
        nothing changes. **In game (you):** with Twin Souls (or any limit of 2)
        and one summon up, a second summon spell stays on offer; at two it
        drops out with `filtered_by` `summon_limit` in the v3 log.
        **0.24.24:** the limit's entry point takes the spell being cast
        (condition tab 1); the call left it out, so a perk with a tab-1
        condition (`EPMagic_SpellHasKeyword`: Vigilant, Kitty's spell packs,
        Ancient Blood, the Necromancy CC) read the result float as a spell
        and crashed the game (2026-10-30, LoreRim, under
        PerkEntryPointExtender). The limit is now evaluated with each distinct
        spell of a live summon or reanimated thrall and the highest kept (order
        independent; actors commanded by other effects, such as Bend Will, left out); with none
        the last value stands, reset to 1 on every load.
- [ ] **Summon limit per candidate spell** (follow-up to 0.24.24): a tab-1 perk
      can raise one summon spell's limit and not another's (Undeath's Mass
      Reanimate +100, Vigilant's +4/6/8 for its conjuration-keyword summons), so one value for
      every summon spell is approximate. Evaluate the entry point per
      candidate summon spell, cached per form and cleared on a load or a perk
      change, where the summon filter (CandidateFilters), covered(i)
      (core/Coverage.h), the upgrade gate (PipelineCoordinator) and the legacy
      context weight test it; the v3 log's `summon.limit` keeps one value.
- [x] **Steps 1-2 of the order above, built in 0.23.55 (R8.1, the first PR on
      `rewrite-r8`)**, with doc 11 decision 4 (the filter half of step 5):
      - Hard zeros into `CandidateFilters`: four reasons appended to
        `core/FilterReason.h` (`no_follower`, `no_workstation`, `not_dark`,
        `favorite_suppressed`; the v3 head writes the names, so old files read
        the same). One predicate each in `core/HardGates.h`, on facts
        `Context::HardGateInputs` reads from the state with the same three
        functions `EvaluateRules` raises the old weights by (`FollowerNear`,
        `StationCraft`, `DarknessApplies`); run in the visitor before the
        active-buff filters. The two Suppress skips in `UtilityScorer` are gone;
        `WeightForCandidate`'s zero arms and `IsHardContextGated` stay for the
        legacy learn feature (PR 2), commented as unreachable in a run. A
        Remembrance hold's own gated item is set aside for the hold, as an
        unaffordable one is.
      - One combat clock: `PotionDiscriminator`'s timer, `UtilityScorer`'s
        `OnCombatStart` / `OnCombatEnd` and its `Update` pass-through, and
        `ConsumeCombatTransition` with its `StateManager` plumbing are gone.
        The discriminator's start window, sustained-combat and lingering-poison
        tests read `NeedSensorState::combatStartAt` (`core/CombatOnset.h`), the
        `combat_onset` need's clock; the multiplier stays (the legacy floors).
      - The refresh pass: `[Scoring] fRefreshPriorSec = 5` (hot-reloaded); each
        active-buff filter passes its item in the blocking buff's last
        `fRefreshPriorSec` seconds, never for an effect with no duration
        (`core/RefreshPass.h`; bits computed by the magic-effects poll,
        `ActorBuffs::refreshDue`, and compared by the skip gate). `covered(i)`'s
        `left(r)` is centred on the same key.
      - Behaviour against 0.23.54 changes only so: the gated items are
        `filtered_by` instead of eligible-unscored (`fl` `ctx`, or
        `suppressed`, which is no longer written); a spell for others at
        context weight 0 can no longer be raised by the cold start; buffs come
        back in their last `fRefreshPriorSec` seconds (a summon spell at the
        limit in the last seconds of the oldest summon, the one a cast
        dismisses: never while a thrall or an untimed summon is the oldest).
        The gates run before dedup and truncation, so a gated item no longer
        takes a dedup key or a `maxCandidatesAfterFilter` place (the 10.8k
        logged decisions have no `duplicate` or `truncated` row). They also run
        before the active-buff filters and the cooldown stage, so an item that
        hits a gate as well is logged with the gate's reason where 0.23.54
        logged the active-buff one (`armor`, `invisibility`, ...) or
        `cooldown`: a `TargetsOthers` armour spell while armoured with no
        follower near reads `no_follower`, a Suppress favourite caught by an
        active-buff filter reads `favorite_suppressed`. A shadow fit of
        `covered(i)` from the `IsActiveBuff` drops loses those rows. A
        Remembrance hold's gated item on cooldown is not set aside: 0.23.54
        dropped it at cooldown, so the hold could not place it then either.
      - The combat clock differs from the old timer. The old one advanced by
        the tick's `deltaSeconds` (`UtilityScorer::Update` from
        `UpdateSubsystems`): not at all while the world is unloaded (the tick
        returns early through load screens and every load door), and at most
        1 s across any gap between ticks (an alt-tab, a stall). The new one is
        `NeedClock` (`steady_clock`), so load-door, alt-tab and gap time count
        in full: `fCombatStartWindow` (10 s) can run out behind a load door
        where the old timer was still inside it. Pausing menus are no
        difference in kind: the loop ticks on the main thread while one is
        open, so the old timer ran there too, short only of what the 1 s clamp
        cut from each gap between ticks. And the old state outlived a
        load: `UtilityScorer::Reset` never reset the discriminator, so a save
        loaded mid-fight into a non-combat save kept `m_inCombat` true, and the
        sustained-combat regen multiplier stayed on until the next combat exit.
        The new clock reads `isInCombat`, so that is gone. Re-entering combat
        inside the window restarts both clocks alike.
      - The v3 head records `refreshPriorSec` (`covered(i)`'s `left(r)`
        centre); a hot reload that changes it starts a new segment.
- [x] **Steps 3-4 of the order above, built in 0.23.56 (R8.2, the second PR on
      `rewrite-r8`): the bootstrap reader, `ChoiceLearner` and the choice scorer,
      in shadow** (the old engine still ranks; the new model is computed and
      logged). The map's Phase 6, as built, says what each piece is
      ([9-implementation-map.md](architecture/9-implementation-map.md), "As
      built (R8.2)"). **The D2 deviation:** R6 chose `theta + b + recent + learn
      mult (D2)`, so R8 keeps the minimum old-scorer code that computes its
      `ln learn mult` feature exactly (the old learner, prior, recency, the
      context weights as a binary gate, the floors and the cold start), moved
      into `src/learning/legacy/LegacyLearnFeature` unchanged in arithmetic,
      behind `[Scoring] bLegacyLearnFeature = true` (read and logged here; PR 3
      makes it the live input; PR 4 keeps it out of the prune). Retiring it
      needs a refit on 0.23.53+ logs where a variant without the old scorer
      passes the gate.
      - Done-when items met here: the explanation label is the largest term
        (host test); the update's Var_p precision, step = variance,
        Plackett-Luce and opportunity counting (host tests); the scorer
        reproduces R6's per-row logits on the logged data (1,865 decisions,
        largest difference 9e-11, identical hit@8 / page hit@1 / NLL).
      - New INI keys: `[Scoring] bLegacyLearnFeature`, `fCoveredPrior`;
        `[Learning] sBootstrapBias` (default `match`, R6 open point 4, the
        user's), `fThetaProcessNoise`; `[Favorites] fFavoriteLifeMult`.
      - Not in this PR: the cosave (THTA/BIAS next to BNDW), the ranking by the
        new score, the prune.
      - **In game (you):** the shadow columns appear in the v3 log; the pages
        match 0.23.55's (the old engine still ranks); the bootstrap line in
        the log; `Pipeline::ScoreShadow` in a Tracy trace under 1 ms.
- [x] **Steps 4-5 of the order above, built in 0.24.0 (R8.3, the third PR on
      `rewrite-r8`): the cutover -- the pipeline ranks on the choice score --
      and the cosave.** The map's Phase 6 says what each piece is
      ([9-implementation-map.md](architecture/9-implementation-map.md), "As
      built (R8.3)"). **FLAG: the old scorer's learning score is now a LIVE
      SCORE INPUT** (D2's `ln learn mult`, w = 1.36, on the rows the old floors
      pass), behind `[Scoring] bLegacyLearnFeature` (default true).
      ContextRuleEngine still runs every pipeline pass, but only to decide
      which rows that feature is computed on.
      - The scorer: every candidate that passes the filters is ranked on mu
        (`ScoredCandidate::score`; `SlotScore()` returns it, remembered-only
        rows -inf, overrides +inf; `operator<` is `Core::RanksBefore`, score
        then DPS, generation order on a full tie). The old floors decide only
        where the legacy feature applies; they drop nothing from the ranking.
        kappa and the slot features enter only the logged logit, after the
        allocation (`core/ChoiceLive.h`). The explanation (largest theta n cap
        term) feeds the label and the reason hold in place of `DominantReason`.
        `[Recs]` / `hg recs` print mu, sd, the top three terms, b and the
        legacy feature. A missing or invalid bootstrap falls back to ranking on
        ln utility over the floors' rows (an error line, `hg status`), without
        the favourites rank scaling and the potion tier preference.
      - The legacy switch is live and hot-reloadable: off, no legacy pass runs
        (no context rules, prior, correlation or potion work), the feature is 0
        and util missing 1 on every row; the old learner and UsageMemory keep
        training.
      - The cosave: `THTA` (the bootstrap's created_utc and SHA-256; the keyed
        weights {key, mu, var} that moved from the bootstrap, since the load
        gives every other key the file's value; early only: an answered
        onset moves all 486 of its need's theta and strength variances, so
        long play nears the full ~36k) and `BIAS` ({formID, mu, var, n, play minutes,
        favourite bit}: the variance and the bit beyond the map's plan), pure
        codec `core/ChoiceCosave`, written in BanditSerializer's callbacks by
        `persist/ChoiceSerializer`. **BNDW STAYS** (D2 reads the old learner's
        per-item state); "THTA/BIAS replace BNDW" waits until D2 is retired.
        A load follows the old learner's decision: a reload of the same
        character keeps the in-memory learning (b's 0xFF forms from the save);
        otherwise THTA per key over the bootstrap (unknown keys dropped and
        counted), BIAS for b; no THTA = the bootstrap, b under sBootstrapBias.
        With the model off (no bootstrap), a save writes back the records
        its load read, so a session without the file loses no learning.
        `hg reset weights`, `hg reset all` and the dMenu reset return theta to
        the bootstrap and clear b; `hg status` and the `[Soak]` heartbeat
        (`choice theta=moved/entries b=`) report the counts; the learner latch
        also fires on a theta or b update.
      - The bootstrap's curves override the [Needs] line (encumbrance =
        logistic 0.988677 73.4157; logged once; `hg needs` says where each
        curve came from); configs/Huginn.ini's line matches, with a comment
        (`tools/needs/make_need_ids.py --check` reads the bootstrap).
      - Replay equivalence through the live path (the legacy switch, the
        ranking pass, then the allocation's kappa): synthetic fixture 1.1e-11;
        the mage's 1,865 decisions (219,049 rows) 9.3e-11, hit@8 0.7968, page
        hit@1 0.5457, page NLL 1.289071, pick NLL 1.966564 -- identical.
      - Behaviour changes to expect in play: pages are always full (nothing is
        under a floor any more); the wildcard draws from the whole ranked list
        below the slot, not only the floors' rows (R10 replaced it, 0.24.3); external
        equips of items the floors used to drop now classify as `miss` (a
        ranked candidate), not `novel`, so the soak's accept denominators move;
        the v3 rows' `util` lacks the favourites rank scaling and the potion
        tier step; the situational label speaks only for items whose own
        explanation is the page's reason (none in the fallback).
      - **In game (you):** see the R8.3 PR for the checks: a save/reload keeps
        the theta and b counts of `hg status`, `hg reset weights` returns theta
        to the bootstrap, `bLegacyLearnFeature = 0` through `hg reload` keeps the
        pages filled, a missing bootstrap ranks on the old utility, and the
        `[Soak]` `rate` field against the model below.
- [x] **Step 6 of the order above, built in 0.24.1 (R8.4, the fourth and
      last PR on `rewrite-r8`): the prune, SMALLER than the map's list on
      purpose (the D2 deviation).** The map's Phase 6 item 6 lists what went
      and what stayed ([9-implementation-map.md](architecture/9-implementation-map.md),
      "As built (R8.4)").
      - **Pruned, with every consumer:** the favourites rank scaling and
        `fFavoritesBoostMin` (the uniform `fFavoritesBoostMax` multiplier stays
        inside the legacy floors); the potion tier preference,
        `sPotionTierPreference` and its 1.5 step; the old weights' reason (the
        dominant reason, its weight-field table, the per-slot attribution and
        its two label-only signals: `ContextReasonAttribution.*` and
        `ContextRuleEngine_Reason.cpp` deleted); utility as a display value (the
        Verbose display mode shows mu and the explanation term, the scorer
        debug widget mu, sd, the top terms, b and lm, `[Reach]` prints the score
        (`score=`, was `util=`), `[Recs]` the score alone; `SlotAssignment`
        lost its utility copy); the debug widget's UsageMemory event buffer;
        tools/replay's old learner (`Learner`, `Current`, `ChoiceTarget`,
        `UsefulLife`: the v2 replay keeps the logged-utility and context-only
        policies); the Debug suite's tests of pruned code (the rank-scaling
        test; the reason derivation tests 17a-17h; what they checked of the
        weights stays). The skip gate's ambient signature and elemental
        window: R8.3 left no remnant (one local renamed). The shipped INI loses
        `sPotionTierPreference` and `fFavoritesBoostMin`; no other `[Scoring]`
        or `[ContextWeights]` key lost its only reader (each one checked by
        grep), and the dMenu JSON holds none of them.
      - **Kept for D2 (the allowlist).** R6 chose D2, which needs the old
        learner's learning score on the rows the old floors pass, so these stay,
        each used only to compute `ln learn mult` and `util missing` (and the
        audit trail): FeatureBanditLearner, StateFeatures, BanditSubscriber and
        the passed-over logic, UsageMemory and its subscriber; PriorCalculator,
        CorrelationBooster, PotionDiscriminator::GetMultiplier;
        ComputeAdaptiveLambda, ComputeUtility, the cold start; fMinimumUtility,
        fMinimumContextWeight, fColdStartUCBBoost, iTopNCandidates (the cold
        start only: the rank prefix `[Reach]` and the v2 / v3 logs report is
        `PipelineStateCache::kReportedRankPrefix` = 10 since the R8.4 review),
        fLambdaMin/Max, the exploration weight, the correlation keys and
        `[ContextWeights]`; IsHardContextGated, ContextRuleEngine::EvaluateRules,
        ContextWeightMap, WeightForCandidate; the Config.h learning constants;
        BNDW; the v2 selection log and the v3 OldScore columns; `hg weights`,
        `hg dump weights` and the dumps' train-count columns (`hg theta` comes
        in R11).
      - **Also kept, not for D2's feature (named here so the claim above is
        exact):** the R8.3 FALLBACK ranking -- ln utility over the kept rows,
        used only while the bootstrap is missing or invalid (an error line and
        `hg status` say so) -- now `Legacy::Run::FallbackRanking` in the legacy
        module, the one place utility still ranks; and the R7 bridge
        (`ScoredCandidate::RankOnUtility`, `Core::BridgeScore`,
        `Core::DisplayUtility`, deleted in 0.24.5), which the fallback, the R7
        golden snapshot format, the Debug capture harness and the slot tests use
        to state slot scores as utilities. `r8_check` confines both. Pruned in the review: the
        choice score's `ln util` feature (computed live from the old utility,
        off D2's path: the bootstrap reader now refuses a weight on it, read as
        0, and warns), the widget's `confidence` payload (exp(score), never read
        by AS2: now 0), `ScoredCandidate::GetContextWeight` and the
        `Globals::GetFeatureBanditLearner` / `GetUsageMemory` accessors (no
        callers).
      - **Confinement, mechanical:** `tools/prune/r8_check.py` (standard
        library): the PRUNED symbols appear nowhere in the code under `src/`,
        `tools/` and `tests/` (comments included, Markdown excluded), nor the
        pruned keys in the shipped INI, its templates or the dMenu JSON; the
        LEGACY lines appear only in the allowlisted files, each with its reason
        (the legacy module, the old learner's own files and subscribers,
        BanditSerializer, the v2 selection log, `core/OldScore.h` and the v3
        format, the settings loaders, the R7 snapshot format and capture
        harness, the legacy host tests), or in a BUDGETED file within its
        count, each line matching one of the file's allowed forms. A file that
        ranks or does slot math live is never allowed whole (review 3:
        `ScoredCandidate.h`, whose `operator<` and `SlotScore()` rank, and
        `core/SlotScoreMath.h` had been), nor is a file on the live learner's
        path (review 4: `ChoiceSubscriber` moved out of `EquipSubscribers.h`
        to its own header, budget 0; `SelectionLogV3.cpp`, which builds the v3
        rows the learner trains on, is budgeted). A LEGACY line names the old
        engine's types, globals or config fields, the utility -> rank-score
        bridge, the legacy module's facade (`Legacy::` / `Scoring::Legacy`, its
        include, `ctx.legacy`, `FallbackRanking`, `KeptFor`, `RowFor`,
        `OldScoreCapture`, ...), or reads the old utility's members (`.utility`,
        `.breakdown` and its fields; C++ only) -- so utility cannot rank or
        display again through a wrapper or a member read (review 2: the first
        version listed only the inner names, and nine probes passed it;
        review 3 added qualified reads, `X::utility`, and the cold-start flag
        `isColdStartBoosted`; review 4 the old utility's row feature
        `ln_util`, kept only on its enum line, the bootstrap's refusal and
        that refusal's host test, and a REQUIRED check that the refusal itself
        stays). A few words are LEGACY in one file only: a
        bare `utility` / `breakdown` in `ScoredCandidate.h`'s code (an
        accessor or comparator written on it) and `Suppress` in
        `PipelineCoordinator.cpp` (its one read is the favorite_suppressed
        filter's input). A STALE list fails comments that describe a pruned
        behaviour as live (the display reason derived from the context
        weights), and a missing shipped INI or dMenu JSON fails the INI-key
        check instead of passing it unread.
        Budgets, patterned: `PipelineCoordinator.cpp` 24 lines (the pass or its
        skip, the switch, the fallback call, the old learner's weights-changed
        latch, the D2 feature per candidate, the v2 / v3 log copies, the
        filter's Suppress input, the Debug widget's cold-start count, five
        comments) and its header 6; `ScoredCandidate.h` 9 (the old utility's
        fields, the cold-start flag, `RankOnUtility`, three comments);
        `core/SlotScoreMath.h` 2 (the bridge's two definitions); `UpdateLoop.cpp` 4 (the old learner's play
        clock and forget sweep, which run with the switch off so BNDW ages);
        `SoakMetrics.cpp` 2 (the `[Soak]` heartbeat's old-learner counts: the
        soak's check that the forget sweep keeps BNDW bounded; reads only, off
        the lm / um path); `UtilityScorer.cpp` 6 (only `[Recs]` lines logging
        lm's inputs, the fallback's line and the `[COLD]` tag) and its header
        0; `PipelineStateCache.h` 4 (the v2 log's cached rows);
        `UtilityScorerDebugWidget.cpp` 1 (the cold-start badge);
        `SlotSnapshot.cpp` 1 (`BuildAllocInput`, the live allocation's input:
        the utility token it carries for the R7 capture and the host legacy
        slot policy; no live core reads it); `SelectionLogV3.cpp` 10 (the v3
        row's utility copy and the OldScore columns' learner fill);
        `ChoiceSubscriber.h` 0; `core/ChoiceScore.h`, `core/Bootstrap.cpp` and
        `BootstrapTests.cpp` 1 each (`ln_util`). Counted: ConsoleCommands, Main,
        Globals, SettingsReloader and `Tests.cpp` on their R8.4 line counts, so
        a new use anywhere in them fails. To get there the legacy half of UtilityScorer
        (the pass, the per-candidate score, λ, the favourites helper and the
        old learner, prior, correlation, potion and context-rule members) moved
        to `learning/legacy/LegacyScorer.{h,cpp}` (`g_legacyScorer`, review
        fix), the pipeline reads the pass through `learning/legacy/LegacyRun.h`
        (`Legacy::Run`: the kept rows, the floors' rows, the row each candidate
        reads (pure since the review, host-tested in `LegacyRunTests`), the
        weights, the fallback ranking; `Legacy::OldScoreCapture`: the v3
        OldScore columns, moved unchanged from `CaptureEligible`; the old
        learner's clock, forget sweep, counts and weights-changed latch), and
        the facts the live filters and the skip gate share with the context
        rules moved to `context/ContextFacts.h`. `tools/prune/test_r8_check.py`
        runs 43 mutation probes in memory, each of which must fail the check
        (review 4 added: `ln util` back in the learner's RowFeatures or in
        RankOnChoice, the bootstrap's refusal dropped (two ways), the v3 row's
        lm taken from the old utility, the old learner read in
        ChoiceSubscriber, the alloc input's score taken from the old utility;
        review 3 added: utility ranking again through `operator<` or
        `SlotScore()`, an accessor on the bare utility, a bridge call in the
        slot math, the stale reason comment back, a pointer to the old
        utility, the cold-start flag or a Suppress skip in RankOnChoice, and
        a missing INI or dMenu JSON; the earlier set:
        utility ranking again in the pipeline (`sc.score = log(sc.utility)`),
        an `ln util` feature in RankOnChoice, the Verbose breakdown back in
        IntuitionMenu, `utility * contextWeight` in UtilityScorer, a facade
        call from SoakMetrics, a display backend or a `using namespace`, a
        changed allowed form, a breakdown read in SlotLocker, an old learner
        call in UtilityScorer, a `topNCandidates` or `lambdaMax` read, a
        `RankOnUtility` call or a dead accessor in the pipeline, a legacy
        global in a console command or test, `DisplayUtility` in the HUD menu,
        `g_legacyScorer` in a display backend, a pruned symbol (the tier
        preference, a Suppress branch, the discriminator's timer, the
        elemental window) and a pruned INI key). PRUNED also covers what R8.1
        pruned that has a name (the potion multiplier's own combat timer:
        `OnCombatStart` / `OnCombatEnd` / `ConsumeCombatTransition`, and its
        members scoped to PotionDiscriminator) and `Suppress` in
        UtilityScorer; the R8.1 Suppress skips had no name of their own, and
        `FavoritesMode::Suppress` stays (the INI mode, the filter's input, and
        the legacy scorer's fav = 0 arm that keeps a filter drop's v3 OldScore
        columns as they were). Stale comments in surviving code that named
        the old engine were edited (the classifiers, GameState.h,
        CandidateGenerator, WeaponData.h and about 30 other files).
        0.24.20 (the #242 review): a PRUNED or LEGACY name is caught in its
        house spellings too (`m_` / `g_` / `s_` before it, a `Get` accessor
        named for it), every name with `legacyShadow` anywhere in it, the v3
        row's `Flag::Scored` (any qualifier) and its OldScore member reads
        (`r.old.floor`, `r.old.wouldUtil`, ...; on the live learner's path any
        `.old` read, an alias included); 20 more probes.
      - **Behaviour against 0.24.0:** none in the ranking (the pruned steps were
        already uncalled since R8.3; `ln util` had weight 0 in the shipped
        bootstrap; the rank prefix constant equals the shipped
        `iTopNCandidates`). Visible: the Verbose mode's detail text (`mu:`, or
        `s:` in the fallback, none on a row only the Remembrance hold placed),
        the debug widget, `[Reach]`'s `score=` field, `[Recs]` without the old
        utility (with `detail`, lm's inputs), and the v3 head's `favMin` now
        equals `favMax` (the multiplier is uniform). `[Soak]`, the cosave and
        the v2 log are unchanged.
      - **Retirement condition for the allowlist:** a refit on 0.23.53+ logs
        (the v3 rows log the old learner on every row) where a variant without
        the old scorer passes R6's gate -- L1 / L7 with fresh per-row learner
        columns, or θ + b + recent with more data -- or the user's call. Then
        the LEGACY list in `r8_check.py` becomes PRUNED, BNDW gives way to
        THTA/BIAS, and the map's full Phase 6 list goes. The learning clock
        and the load's keep-or-replace decision stay (0.24.22: one shared
        clock in `core/LearningClock.h`, HCID in `persist/CharacterRecord.*`):
        the retirement deletes the old learner's two tick calls and its
        `ResolveLoadedLearner`, and moves the save/load callback registration
        (`SaveCharacterRecord`, `LoadCharacterRecord`,
        `ResetPendingCharacter`, called from BanditSerializer's SKSE
        callbacks under its unique ID `'QCNO'`) out of BanditSerializer.
        Nothing of the decision changes, but the reset sites must then
        tick before the reset (or under the learner's lock): today the old
        learner's `Clear` ticks first; without it, a save taken between
        `ResetChoiceLearning` and the site's tick would pair the reset
        learning with the pre-reset clock, and a fresh session loading the
        pre-reset save then that one would keep memory and lose the
        reset. 0.24.19: the fit
        tools read 0.24 logs for that refit (tools/fit/README.md): the v2
        log's rows the old floors dropped (written 0.24.0-0.24.18 with an
        all-zero breakdown) are skipped and no longer written, and a fit of
        the live engine B reads the D2 feature from `lm` / `um` (`learn_live`).
      - **Box status at R8.4:** steps 1-6 built ([x] above); the need signature
        in the skip gate [x]; doc 11 step 7 [ ] (not R8's code: the sensors are
        still to build); the update-loop thread [ ] (undecided, not blocking);
        the "Done when" below: mechanical part met (r8_check, both builds, the
        host tests), the in-game part open; the cure, churn and reach checks
        [ ] (the in-game session after this PR).
      - **In game (you):** the R8 session below -- the R6 bootstrap on the mage;
        cures, potions and gear; the churn, reach and rate comparisons; a
        save/reload; `bLegacyLearnFeature = 0` for a few minutes.
- [ ] With those sensors, doc 11 step 7: the attacker's race on every hit and
      `poison_damage_rate`, for anticipatory resists.
- [ ] **Decide: which thread runs the update loop** (2026-10-09, two Tracy
      traces in `traces/202610/`, which is not checked in; the counts and
      times are trace09's unless marked). `OnUpdate`, driven by the InputEvent sink,
      runs:
      - on a pool of 6 rotating game job threads in gameplay;
      - on the main thread in paused menus and the main menu (it does run
        while a pausing menu is open: 5,501 main-thread ticks in hook gaps),
        plus ~3–4 ticks in the ~0.3 s before many door loads;
      - on a loading-screen thread for every tick during a load, about 9 a
        second through each load (the startup load before the main menu
        included), concurrently with the main thread's load. Most return at
        `IsWorldLoaded` (1,289 of 1,304; trace04 3,742 of 3,768), after
        `UpdateHandler::ProcessEvent` has run `InputHandler::ProcessButton`
        and `Update` and the `IsWorldLoaded` UI reads. In 15 of 18 game
        loads (about 12 of them door or fast-travel loads, plus 2 likely
        LoreRim defeat teleports at 12:31:33 and 12:57:02, each right after
        `health_deficit` 0.99 in combat) one of them is a full tick,
        `RunPipeline` and `Inventory::DeltaScan` included (trace04: 26 full
        ticks, one load with two).
      `ForceUpdate` (`hg refresh`, `hg recs`, the test harness) runs a tick
      on its caller's thread; every path holds UpdateHandler's mutex, so no
      two ticks overlap. One poll does not: the Debug-only
      `StateManager::ForceUpdate` at kPostLoadGame / kNewGame
      (`Main.cpp:399-400`, `InitializeGameSystems`) polls every sensor on
      the main thread outside UpdateHandler's `m_mutex`.
      No job tick overlapped a main-thread zone (0 of
      22,047); job ticks end a flat ~2 ms before the main thread's player
      update finishes, so the main thread appears to wait for them
      (inferred: the hook zone opens after the original update returns, so
      an overlap with the very start of the update body is not excluded).
      Risk: loads are the clearly concurrent case. Every load-screen tick
      runs beside the main thread's load: mostly the input handler and the
      `IsWorldLoaded` UI reads, and once per load (usually) a full tick.
      No race is shown there; the traces show timing, not data access.
      Options:
      - (a) drive the loop from the `PlayerCharacter::Update` hook: the
        thread is guaranteed, but the hook does not fire while paused, and
        ~594 ticks a minute run in menus today, so menu-time confirmation
        would change;
      - (b) hook `Main::Update`: the main thread in menus too, but needs an
        Address Library id;
      - (c) an SKSE task per tick: **not** a fix, since tasks also run on
        job threads in gameplay (seen by an earlier verifier round,
        `9-implementation-map.md:62`, not in a Tracy trace; at the main
        menu a task ran on the main thread, traced: `EffectCatalog::Read`).
      Rejected for now, the cheap guard "skip ticks until the main thread has
      updated since the load". It must re-arm on door and fast-travel loads
      (no kPostLoadGame fires); the hook must record the update before its
      `!g_gameLoaded` early return; it must fail open if the hook install
      fails; it moves the ~43 ms save-load tick (cold `RunPipeline` 33 ms +
      `WarmHeld` 6.6 ms) onto the first gameplay frame; and a pausing on-load
      message box would suppress menu ticks. Not done now: the old engine is
      frozen and no race is shown. The code comments say what runs where
      (`UpdateLoop.cpp`, THREADS above `OnUpdate`; 0.23.18). Whichever
      option is chosen, the client API proposal
      ([architecture/10-client-api.md](architecture/10-client-api.md),
      section 3.6) would, once its steps C2 and C3 are done, take the display
      pushes off the loop's thread; the work after the push
      (`LogRecommendations`, the Debug widgets) stays on it.
- Done when: a grep over `src/` and `tools/` for every pruned symbol finds
  nothing outside the new code (docs follow in R11); Debug and Release build
  clean; host tests cover the update (Var_p precision, step = variance,
  Plackett–Luce, opportunity counting), the explanation label is the largest
  term, the scorer reproduces R6's replay numbers on the logged data.
  **In game (you):** a session on the R6 bootstrap; cures, potions and gear
  rechecked (the old "cures reach the page" checks move here).
  *R8.4 (0.24.1): the mechanical part is met -- `python -I
  tools/prune/r8_check.py` passes (the grep, with D2's allowlist for what R8
  keeps), Debug and Release build at /W4 /WX, the host tests are green and the
  replay on the mage's 1,865 logged decisions is unchanged; the in-game part
  is open.*
- [ ] **In game (you): a cure reaches the page while poisoned.** Accept when,
      while `poisoned` is on and a cure-poison item is held, the new engine
      shows it on the displayed page within one tick and keeps it for the
      episode, alongside health items. The case (LoreRim, 2026-10-10, launch
      13:14, 0.23.22): `poison=true` and `poisoned=1.00` from 14:03:48 to
      14:06:44, 2 Potion of Cure Poison held, yet the old engine seated it on
      no page while the CriticalHealth override (14:03:51-14:06:18.8) and
      health and food items held the slots; it appeared at 14:06:18.8, as
      CriticalHealth deactivated, and gave way at 14:06:29 to the Staff
      swap-back while still poisoned (`[Hold] Page 0 slot 1: 'Potion of
      Cure Poison' gives way to 'Staff of Wandering Stars'`). The old engine
      is frozen; not fixed there. It also labelled Potato Soup `Poisoned`:
      on LoreRim that soup carries Resist Poison (`[PopulateItemTags]`), no
      cure, so the new effect data should rank it as prevention, not a cure.
- [x] The need signature joins the pipeline skip gate (moved from R3 by its
      verifier, round 1 on #188): a quantised (0.05) need signature, so
      continuous needs re-score; it then replaces `ambientSignature` and the
      elemental window. **Built in 0.24.0 (R8.3):** `Core::Needs::SignatureHash`
      (FNV-1a over the K levels of `Signature`) of the vector NeedMonitor
      evaluates each tick (with the bootstrap's curves, so the vector the
      score reads) is compared in both gates (`NeedsForcedRun`,
      `CheckHashSkip`); `ambientSignature`, the coordinator's elemental latch,
      `StateManager::IsElementalWindowActive` and `m_elementalWindowActive`
      are gone (the enrichment of isOnFire and the like stays for the legacy
      context rules). The `[Soak]` heartbeat gained `rate runs/s=..
      wildcard=..% wheeler/s=..` for the comparison below. One known loss: the
      enrichment window's close (fixed 5 s) no longer forces a run by itself;
      the damage-rate needs decay on their own clock (kDamageDecayTauSec) and
      move the signature while they do, but not at that instant; the window
      only feeds the legacy feature's context gate, so the cost is a stale
      legacy feature until the next run. Not in R3 because it is not "logged only": modelled
      at 0.17 -> 4.6 pipeline runs/s in a melee fight and 0.01 -> 0.44 on a
      quiet dungeon walk, a wildcard on screen ~20% -> ~32% of quiet
      exploration (a roll happens on each run with none showing), Wheeler
      pushes ~4.6/s in fights, and the soak denominators move. Here, where
      the scorer reads the needs, re-scoring on a need step is the point;
      measure the wildcard share and the Wheeler push rate when it lands.
      **Flapping** (no hysteresis; the levels round at x.025): argued, not
      measured -- no need in needs.csv is known to sit on a boundary with
      noise (0/1 facts, darkness held 5 s / 3 s, one-way vitals and decays,
      distance needs that follow motion), and a run per tick (~10/s) is the
      ceiling. **Soak gate:** quiet-scene `runs/s` above 1 (model 0.44)
      stops the soak until `Signature` gets one level of hysteresis.
- [ ] **After the rework, measure slot churn against this baseline.** The
      user, on the R3 build's LoreRim session (2026-10-09): "slot churn
      definitely increased". Measured in that session: 4-6 slot changes a
      minute outdoors, 40.7 a minute in combat; the October soak's median was
      7.2 a minute, with a dungeon peak of 141 in 5 minutes. The drivers were
      context flips (magicka tiers 69, distance bands 28, target type 12),
      the magicka-food misread (Known bugs: LoreRim "Fortify Magicka" food
      read as a restore, which made pies "Low MP" candidates -- the main
      combat churn driver), and the 3 s lock expiring into x1.7-3 score
      swings. R7's hold tie band was checked and is refuted as a cause. The
      new scorer reads cap(i), where that food is `fortify_vital_magicka`
      (0.23.16 fixture), and a continuous need replaces the tier flips; the
      lock and the challenger rule are R9. **Accept (proposed at R8.4, the
      user's call):** read from `[SlotChurn]` and the heartbeat, not worse
      than the baseline on like content -- at most 6 a minute outdoors, at most
      40.7 a minute in combat, a session median at most 7.2 a minute, and no
      5-minute window above 141. Worse is not a rollback by itself (the
      ranking is reach-ins' test); it goes to R9 as the challenger rule's
      starting number.

### R9. Bayesian challenger rule

Map Phase 7: swap when μc − μi > ln m + z·σ_Δ, σ_Δ with shared weights
cancelling. Lock, home keys and seating stay. Done when: host tests prove no
swap-back on fixed beliefs; churn buckets on Δ/σ_Δ. **In game:** churn per
press in the heartbeat against the R8 session.
- [x] **The rule** (0.24.2): `core/ChallengerRule.h` -- swap when
      μc − μi > ln m + z·σ_Δ + the hold's tie band, z = Φ⁻¹(τ). `[SlotLocker]
      fChallengerConfidence` = τ (0.8, doc 9's starting value: z 0.842,
      clamped to 0.5-0.999, so z ≥ 0) and `fChallengerMinGap` = ln m (0:
      confidence alone; clamped to 0-5). The slot hold (PASS 1c,
      `SlotAllocCore.h`, `TakesKey`) asks it of the best challenger for each
      held key whenever the allocation's input carries the scorer's beliefs
      for both rows; the class cap's terms shift both scores and add nothing
      to σ_Δ. `fChallengerMargin` now rules only the rows with no belief: the
      FALLBACK ranking (no bootstrap) and the R7 golden snapshots, which is
      why every golden and adversarial test still passes unchanged. The combat
      upsert's barrier stayed `fCombatUpsertMarginMult × ln(1 +
      fChallengerMargin)` until 0.24.13 (closed below).
- [x] **σ_Δ** (`core/ChoiceBeliefs.h`): σ_Δ² = Σⱼ (aⱼ(c) − aⱼ(i))² Wⱼ (+ the
      strength block) + (covered(c) − covered(i))² var(θ_covered) + var(b_c) +
      var(b_i), Wⱼ = Σₖ nₖ² var(θₖⱼ) (+ recent) from the ranking's own tick.
      b is keyed by form, so two stacks of one form (another uniqueID or
      temper) share one b and it cancels: their b part is (sd(b_c) −
      sd(b_i))², 0 unless one stack reads b as a favorite.
      `RankOnChoice` builds one `BeliefTable` per run after the ranking pass
      (covered after the upgrade gate) and every ranked row carries it and
      its index (`ScoredCandidate::ChoiceParts::beliefs`, `beliefRow`), so
      Wheeler's other pages read the same beliefs. Host-tested: the healing
      potion pair (θ enters through (0.8 − 0.3)², not 0.8² + 0.3²), the
      disjoint pair reaching the √(σc² + σi²) bound exactly, a dense brute
      force with presence+strength, recent and covered, and each row's own
      σ² equal to `RowScore::var`.
- [x] **No swap-back on fixed beliefs** (`tests/core/ChallengerRuleTests.cpp`):
      the argument is in `ChallengerRule.h` (the line is ≥ 0 and symmetric in
      the pair, so a reversed swap, or any cycle, would need 0 > 0). That
      argument is the proof; the property test is evidence for it: 600
      random pages (random layouts, settings, τ, ln m, sparse attributes,
      scores about one σ_Δ apart, random holders seated, every candidate's
      dedup key and name unique), 12 passes each with the seating memory
      carried, no direct (holder, challenger) reversal, and no give-way in
      the last 3 passes on that seed. It counts direct reversals only, not
      longer cycles or visible A→B→A bounces (the verifier's adversarial run,
      3,000 worlds x 30 passes with the class cap forced, found neither with
      unique keys). 0.24.6: a variant keeps duplicate dedup keys and names
      (a 12-form pool, 518 of 600 worlds with a shared key) and settles on
      both hold paths, no swap-back (see the closed item below).
      The same worlds under per-pass sampling (Thompson, ruled out in doc 9)
      give 3,523 swap-backs (735 give-ways under the rule), so the test sees
      one when there is one.
- [x] **Churn buckets on Δ/σ_Δ:** `[SlotChurn]` prints the pair's σ_Δ and
      Δ/σ_Δ (and "under the line"); the heartbeat's slotChurn gains
      `dsd(gone <0 <0.5 <1 <2 <4 >=4 nosd)` beside `ratio(...)` (kept, so a
      session compares against R8's) and `under=` (changes at or under the
      rule's line, on raw scores: mostly seating, home-key moves and the
      fill; the hold decides on Regular keys after the class cap, so a
      holder past its class's free count can give way to a challenger whose
      raw lead is under the line, and those capped give-ways count here
      too). `[Hold]` prints the gap against ln m + z·sd and P(better).
- [x] **Churn per press in the heartbeat:** a last section, `| perPress
      churn=N presses=M churn/press=X bounce10s=B (hold=H)`. churn = every
      visible change but a page switch, a Used backfill, a Remembrance hold
      and an override (trace-04's "without player or safety causes"). R8's
      heartbeat has the same causes and presses, so
      `python -I tools/soak/churn_per_press.py <R8 log> <R9 log>` gives both
      sessions' figures the same way (the sum over the sum, per launch too).
      For scale, trace-04 (0.23.26, the old engine): **12.37 churn per press**
      (928 over 75 presses; hold causes 10.11 per press).
- [ ] **Slot juggling is a tracked problem** (the user, 2026-10-10: "need
      watch the slot juggling"): the allocator moves items between slots too
      often. The analysis is in local trace notes
      (`traces/2026-10-10/trace-04-analysis.md`, not checked in), on 0.23.26:
      - **Baseline (page 0, 77.2 unpaused minutes):** 1,092 changes; in combat
        39.9 a minute (24.4 without player or safety causes), out of combat
        9.6 (8.3). A→B→A on one slot within 5 s / 10 s: 37 / 63 in combat,
        23 / 47 out. Of the 795 expired/unheld changes, 361 (45%) were not a
        challenger clearing the margin (143 incumbent gone, 127 scored lower,
        91 at 1-1.5×).
      - **What R9 answers:** the hold's share -- near ties and score swings on
        band flips (pattern 4: 434 changes passed the old 1.5×). Under the
        rule a give-way needs P(better) > 0.8, never reverses on fixed
        beliefs, and `under=` counts every change that got past without
        clearing the line. The heartbeat's new `bounce10s` is the analysis's
        A→B→A at 10 s, live (`hold=` the ones the hold's causes made).
      - **What R9 does not answer** (each still counted, so the session shows
        what is left): the magicka affordability filter dropping and
        returning spells (pattern 1, the largest out-of-combat driver; a
        candidate filter, R8.1's territory); home-key returns moving the
        gap-filler (pattern 2: kept, "lock, home keys and seating stay");
        wildcards (pattern 3: R10); the Remembrance hold's end (pattern 5:
        the player's own press).
      - **Judged against both baselines** in the R9 session: R8's session
        (its heartbeat through `churn_per_press.py`) and trace-04's numbers
        above. Not a rollback by itself if worse, as for R8.
- [ ] **In game (you):** a session on 0.24.2 like the R8 one -- the
      `[Soak]` `perPress` section against R8's (the tool), `dsd(...)` and
      `under=`, `bounce10s`; whether a clearly better item is slow to take a
      key (`[Hold]` lines with P near 0.8 are the rule at work; raise or
      lower `fChallengerConfidence`). Recheck "workstation gear holds its key
      at the bench" (Kept outside the rewrite) here.
- Open: the lock's length is still 3 s (map Phase 7: shorten it after
  measuring the `Expired` buckets, now on Δ/σ_Δ); slot captures (Debug
  `--capture-slots`) do not record the beliefs, so a capture from a
  choice-ranked session replays with the fixed margin.
- [x] **Closed in 0.24.13 (`combat-upsert-bayes`): the combat upsert uses
  the challenger rule** (the user, 2026-10-30). A frozen key goes to a
  challenger when μc − μi > ln m + z_combat·σ_Δ, z_combat =
  Φ⁻¹(`fCombatChallengerConfidence`), its own key at 0.995 (z 2.576):
  mid-fight swaps cost more than calm ones, and the old upsert was
  deliberately 3× the calm margin -- 3 × z(0.8) = z(0.9942). The pair's
  σ_Δ comes from this run's table when the frozen item was scored this
  run; otherwise (an older run's row) and in the FALLBACK ranking the
  fixed barrier `fCombatUpsertMarginMult × ln(1 + fChallengerMargin)`
  stays, and 0 still turns the upsert off in both rankings. The upsert
  line prints σ_Δ and P like `[Hold]`. Host-tested
  (`tests/core/CombatUpsertTests.cpp`): no swap-back and every page
  settles on random fixed beliefs (1,500 worlds), a sure challenger takes
  the key and an uncertain one does not, and the fallback planner equals
  the pre-0.24.13 one on 4,000 random pages.
- [x] **Closed in 0.24.13: "only the best challenger is asked" is a
  switch.** `[SlotLocker] bTestAllChallengers` (0 = the best challenger
  only, as before; 1 = in rank order, the first past the line takes the
  key), for the hold and the combat upsert alike, hot-reloaded by `hg
  reload`. Under the fixed margin the switch changes nothing (a lower
  score cannot clear a line the best one missed), so the golden and
  fallback paths are bit-identical; host-tested on 300 synthetic pages,
  and the R9 property worlds (unique and duplicate keys) settle with no
  swap-back with it on. **In game (you):** leave it at 0 for the R9
  session; to try it, `bTestAllChallengers = 1`, `hg reload`, and look
  for `[Hold]` / combat upsert lines ending `higher challenger(s) missed
  the line`.
- [x] **Closed in 0.24.6 (`sweep-allocator`): two candidates with ONE dedup
  key on a page.** Seating and the hold found a key's owner by the first
  match (the `seats[j]` lookups), so a key already taken could read as free
  and the page churn with nothing moving -- the R9 verifier's synthetic
  worlds, on both the rule and the fixed margin. The pipeline cannot hand
  the allocator two such rows today: `CandidateFilters` step 6 drops a
  second candidate of a key as `duplicate` (two plain stacks of one weapon
  form with no `ExtraUniqueID`), and the Remembrance rows added after the
  ranking are only for a form no ranked row has, once per form
  (`PipelineCoordinator::ScoreCandidates`). Tempered or enchanted stacks with
  their own uniqueID are different keys of one form (one per page by the
  formID dedup); two forms of one name, one per page by the name dedup; a
  spell and a scroll of one effect, different keys. The core gives every key
  one row anyway (`Engine::MarkShadowed` / `OwnerOf`: the first ranked row
  with the key; the rest are never placed, found or drawn), so no caller can
  bring the churn back. Host-tested: the R9 property test with duplicate keys
  and names settles on both paths (fails without the fix: some worlds stay
  unsettled, how many depends on how the fix is taken out), a named case, and the golden test's page invariant (no shadowed
  row shown).

### R10. Wildcards by uncertainty

Map Phase 8: a reserved slot per page, ranked by relevance × σ plus a small
random term, propensity logged, flags per page (Wheeler leak). **In game:**
wildcard picks against the soak's 8 of 333.
- [x] **Built in 0.24.3 (`r10-wildcards`, on `rewrite-r8`).** As built:
      [9-implementation-map.md](architecture/9-implementation-map.md), Phase 8
      "As built"; the design: [5-slots.md](architecture/5-slots.md#wildcard-system).
      - **The reserved slot:** one wildcard key per page, the last key in fill
        order with `bWildcardsEnabled`, never the first (`Core::SlotAlloc::SeatOf`;
        key 8 on the shipped pages, named at load by `[SlotSettings]`). The
        cached wildcard is pinned there (a pass of its own, like a Remembrance
        hold); a roll draws on the finished page and puts its row there. The
        rank swap, the hold's and the fill's special cases for it, and the
        same-type draw are gone.
      - **Relevance × σ:** w = logistic(μ − μ_cut) × σ (μ_cut: the row the
        wildcard puts off the page -- the key's own item, or, when that item
        outranks the weakest row shown, the weakest row, whose key it moves
        to, so a wildcard never pushes the page's best rows off it; σ: the posterior sd, high for an item never
        picked, one whose battery ran down, a newly learned spell, a pairing
        with little evidence); the random term is a uniform share of the draw
        (`[Wildcards] fUniformShare = 0.1`), so the propensity stays exact;
        P(roll) runs from `fBaseProbability` (nothing uncertain near the page)
        to `fMaxProbability` (an unknown item at its edge), once per refractory
        window (a failed roll restarts it), so it sets how often wildcards
        appear (`core/WildcardGate.h`, host-tested).
      - **Propensity logged:** v3 `wp` = P(roll) × P(the draw shows the form),
        per window.
      - **Flags per page:** an off-screen Wheeler page shows its own cached
        wildcard and never rolls; it no longer inherits the displayed page's.
      - **Discovery (R5's note, newly learned spells not on the HUD):** a new
        spell has no b, so its σ carries b's prior sd (0.5), as does every
        never-picked item; it weighs high when its score is also near the
        page. Whether that brings new spells up often enough is the in-game
        check below.
        **0.24.14, the discovery boost** (the user's call, 2026-10-30: an INI
        option, default on): a spell learned or an item gained for the first
        time on the character draws with b's variance × m(age) =
        1 + (M − 1)·2^(−age/H), M = 4, H = 1 h of play, until it decays or its
        first pick; never what a load finds. Wildcard draw only: in the
        challenger rule a larger σ would make it take a key later, and in the
        summon gate it would move μ. Cosave record DISC.
        `docs/architecture/5-slots.md`, "The discovery boost".
      - **Telemetry:** `[Soak] ... wildcards rolled= relevant= picked=`
        (relevant: a need the player had at the draw values the item's
        effects); one `[Wildcard] Page P key K: 'name' (p=..)` info line per
        draw.
- [ ] **In game (you):** a session of ordinary play on the R10 build.
      - The load log names each page's wildcard key: `[SlotSettings] Page 0
        'Main': 8 slots, wildcard key 8` (and the `[WildcardConfig]` line shows
        the uniform share).
      - Out of combat a wildcard appears on key 8 only (never on another key),
        with the Wildcard subtext on the wheel; it stays about 30 s, then key
        8's own item comes back; none in combat.
      - When key 8's own item is better than the weakest item shown, a
        wildcard moves it to that item's key (the `[Wildcard]` line ends
        `its item moved to key J`) rather than taking it off the page; it goes
        back to key 8 when the wildcard ends.
      - Turn Wheeler to page 2 while page 1 shows a wildcard: page 2's key 8
        shows no wildcard label unless page 2 rolled its own.
      - The heartbeat's `wildcards rolled= relevant= picked=` over the session,
        against the soak's 8 picks of 333 placements (2%); and against the
        relevant rolls (doc 9's measure). Note any wildcard that made no sense.
      - Learn a new spell and play on: note whether, and after how many
        `[Wildcard]` lines, it comes up on key 8. It has no special boost:
        every never-picked carried item shares b's prior sd, so it competes on
        how close its score is to the page (open question in the PR).
- [x] **Closed in 0.24.6 (`sweep-allocator`), from R10's verifier:**
      - *A wildcard counted though the locker hid it.* `WildcardManager::Commit`
        ran before `SlotLocker::ApplyLocks`, so a draw the locker did not show
        was cached, logged (`[Wildcard] Page P key K`) and counted in
        `rolled=`. R10 already skipped the roll while key 8's lock held and
        kept lock-held forms out of the pool; the post-lock dedup compares
        NAMES, so a row named like a lock's item of another form was still
        drawn and cleared. Now the pool also leaves out lock-held names
        (`WildcardIn::heldNames`), and `Commit` runs after `ApplyLocks` and
        counts a draw only if the seat shows it (`Core::Wildcard::DrawnShown`;
        else `CommitFate::NotShown`: not cached, logged or counted, the roll
        waits). The `[Soak]` field order is unchanged.
      - *Two ordinary rows of one name on a page* (synthetic seeds 409 and
        459, both arithmetics). The code means one per name (the fill, the
        draw and the post-lock dedup all dedup by name: the player cannot
        tell two forms of one name apart); the slot hold's owners were the
        gap, and the override and Remembrance passes checked forms only. All
        check names now; the golden page invariant checks every row (one
        wildcard, on its key; no form, name or shadowed row twice). The
        PR's verifier found one more: a guest (an item an override pushed out
        of its seat, held where it stands) was checked against the overrides
        and holds only, not against the seat owners already held, so a page
        could read [Twin] [override] [Twin], or one form on two keys for two
        stacks (predates 0.24.6). Guests now check against every holder
        accepted before them too; a named adversarial test and 2000 built
        synthetic worlds pin it.
      - **In game (you), on the rewrite-r8 session:** no key shows the same
        name as another (two enchanted copies, or a potion and a hold of its
        twin); a `[Wildcard] Page P key K` line is always followed by that item
        on key K (the debug log's `drew '...' but the slot locks did not show
        it` line, if any, names one that was not, and no `[Wildcard]` info line
        or `rolled=` count goes with it).

### R11. Console, telemetry, docs; retire the classifiers

Map Phases 9–10. `hg theta`, `hg recs`, the dMenu reset text; θ-drift fields in
`[Soak]`; slot classes, overrides and labels re-derived from cap; per-list
override layer (and the [overrides directory](#an-overrides-directory-so-mod-authors-can-ship-their-own));
delete the classifiers and per-type dumps; rewrite `CLAUDE.md`,
`docs/README.md`, the architecture docs and the Nexus page; archive
`4-contextual-bandits.md`. Review the slot classes as a whole here (below).

**Built in 0.24.4 (R11, `r11-retire` on `rewrite-r8`), SMALLER than the map's
list on purpose: the D2 deviation again.** The map says "delete
`SpellClassifier`, `ItemClassifier`, the tag enums, the scroll classifier".
They cannot go while D2 stays: D2's `ln learn mult` applies on the rows the
old floors keep, the floors need the old context weight, and the context
weight (`ContextWeightForCandidate`), the correlations and the potion
multiplier read the classifiers' tags and types. Deleting them would change
which rows carry the feature, so D2 would no longer be R6's D2. **So R11
retires the classifiers from the live engine and confines them to D2**, the
way R8.4 confined the old learner; they go when D2 goes (R8's retirement
condition: a refit on 0.23.53+ logs where a variant without the old scorer
passes R6's gate, or the user's call).
- [x] **Re-derived from cap(i):** one pure module, `core/CapFacets.h` (facets:
      the yes/no questions, each from named effects.csv columns; the 24 slot
      classes' `Matches` and the `Classify` order), read through
      `effect/Facets.h` (the catalog's `FindOrMapId`). Its readers: the slot
      classes (`SlotClassifier`, the class cap, the slot settings' override
      probes, the snapshot's match masks), the candidate filters (full vitals,
      the active buffs, the resist check -- one `IsResistRedundant` for potions
      and spells -- and the no_follower gate), the override finders (the three
      restore potions by facet, ranked by the potion's own restore row
      (`PotionRestoreWithin`),
      `ItemRegistry::GetBestPotion` deleted; the restore-spell fallback; the
      cure; waterbreathing), the "Ally Support" label (TargetsOthers) and the
      widget's potion styling. Every ranked row carries its subject
      (`ScoredCandidate::subject`, filled once per run). The ammo / charge
      display reads `WeaponType` and the stack, form facts, unchanged.
      Host tests: `CapFacetsTests` (each facet's columns, each class arm, no
      dead class, and real items from the three dump fixtures).
- [x] **Review of the slot classes as a whole:**
      [5-slots.md](architecture/5-slots.md), "Slot classes, re-derived from
      cap(i)": per class, the facets, the change against the classifiers and
      why. The enum stays (players name it). Visible changes: FoodAny takes
      every food, alcohol included; AlcoholAny is food that intoxicates or
      costs its drinker something that is not a vital drain (the catalog has
      no alcohol column; raw fish's paralysis is a known stray); SummonsAny no
      longer takes every Conjuration spell; BuffsAny takes a Fortify
      <craft> potion; Utility takes slow fall and cure spells; a damage
      cloak or rune is DamageAny by its effect. An item the catalog does not
      map matches only what its source decides. 0.24.9: cap(i) gained an `alcohol`
      column (drink and drug marks of the records, effects.csv); AlcoholAny
      reads it and FoodAny excludes alcohol again (the user, 2026-10-30).
      0.24.16: drugs and alcohol are one group, narcotics (the user,
      2026-10-30): the column is `narcotic`, the class NarcoticsAny (never
      a poison); Sweet Skooma, vanilla's Sleeping Tree Sap and Redwater
      Skooma are caught by their drug MGEFs.
- [x] **Confined, mechanically:** `tools/prune/r8_check.py` gains CLASSIFIER
      (the classifiers, the tag and type enums, `HasTag`, the override
      loaders, `.tags` / `.tagsExt` / `.element` reads; code only, comments
      stripped) allowed only in the classifiers' own files, the candidate
      types that copy them, D2's inputs (the context rules and weight, the
      correlations, the potion multiplier, the prior, the legacy module), the
      override-section parser, the Debug registry widget, and budgets (the
      soul gem split in `CandidateGenerator.cpp`, 2 exact lines; `Tests.cpp`,
      counted); PRUNED gains the per-type dumps and the live helpers they
      replaced; REQUIRED keeps the facet readers wired. 21 new mutation probes
      (67 in all), each of which must fail it.
- [x] **Per-type dumps deleted** (`hg dump spells / food / potions / scrolls /
      weapons / apparel / diseases`); `hg dump all` ships in every build and
      its slot class is the cap-derived one; `hg dump weights` (the old
      learner, D2) describes items by the catalog's kind and facets.
- [x] **The override layer as a directory** (`core/OverrideFiles.h`):
      `Data/SKSE/Plugins/Huginn/EffectOverrides/*.ini` alphabetically, the
      player's `Huginn_EffectOverrides.ini` last; plugin-relative keys
      (`<plugin>|<local FormID>` = an effects.csv column); a file that cannot
      be read, or whose format version (`[Huginn] iFormatVersion`) is newer
      than this build reads or invalid, is skipped and named; entries for plugins not loaded are skipped quietly;
      later files' replacements are counted. `Huginn_Overrides.ini` (type /
      tag) is NOT replaced: it corrects the classifiers, so it now reaches D2
      only, and its one shipped fix (Arcane Mass Inhibition, Debuff) is part
      of what keeps D2 exact.
- [x] **Console:** `hg theta` (what ranks, θ / b counts, the drift from the
      bootstrap, b's spread, the outside option now), `hg theta <FormID>`
      (b, cap, facets, every θ·n·a term at the live need vector, the largest
      weights under any need), `hg dump theta` (every moved θ beside the
      bootstrap's, every b); `[Recs]`'s header carries the run's outside
      option u0; `hg weights` is labelled the old learner's. The dMenu reset
      button's text and toast say what it does now.
- [x] **Telemetry:** `[Soak]` appends `drift` (means moved, mean / rms /
      largest |Δμ| and where, variances moved, mean ln variance ratio, b's
      spread, θ_covered; `core/ThetaDrift.h`, host-tested); every field before
      it keeps its place (docs/playtest/LongPlaySoak.md).
- [x] **Docs:** CLAUDE.md and docs/README.md describe the new pipeline;
      `4-contextual-bandits.md` archived (`architecture/archive/`, banner);
      R11 banners on the pre-rewrite pages (0, 1, 2, 3, 6, 7, 8, 11,
      Performance, classifier-coverage, candidate-system, TESTING-INDEX) say
      what changed;
      5-slots.md's class section rewritten; the Nexus page (bbcode) rewritten
      where it described the old engine (how Huginn decides, effect overrides,
      the class list, the console, darkness); `page.md` flagged as not
      regenerated.
- [ ] Not in R11: the classifiers' deletion (with D2, above); lifting
      `ApparelClassifier`'s scope guard (all armour as candidates: a decision
      of 2026-10-08, not an R11 item); a full rewrite of the pre-rewrite
      architecture pages' bodies (they carry banners).
- **Behaviour against 0.24.3:** μ is computed as before (it reads cap(i) as
  before; the D2 pass reads the same tags), so it moves only through the
  candidate set (the D2 floors' cold start counts the rows that pass). What
  moves: the candidate set the filters pass, which slots accept
  an item (the review above), which candidates the filters drop (by facet:
  e.g. an armour-spell filter no longer drops a ward; a multi-resist spell is
  redundant when any one of its resists is covered; Resist Magic counts for
  spells; a hostile or damaging spell, an elemental cloak with its resist
  rider, is never resist-filtered), which items an override may surface (pure
  by the self_harm / intoxication / hostile facets; a restore potion ranked by its restore
  row, not its costliest effect), and the "Ally Support" label;
  the v3 rows' `fb` follows the filters.
- **In game (you):** see the R11 PR: a typed page (page 2 on the shipped
  layout) fills as before with the reviewed classes; `hg theta`, `hg theta
  <FormID>`, `hg dump theta`, `hg dump all` in a Release build; the `[Soak]`
  drift section; the dMenu reset text; an effect override file in
  `Huginn/EffectOverrides/` loads (log line); the critical-health override
  still finds a potion and the poisoned override a cure.


### R12. Baseline soak and release (in game, you)

A multi-hour soak on the rewrite as the new baseline; merge `engine-rewrite`
to `main`; release notes say old learned weights are discarded.

---

## Next: the client API (proposal)

Huginn as a recommendation server: a UI connects, says what it can show and do,
reads an immutable snapshot on its own thread, and reports what it showed and
what the player picked. Huginn never calls into a UI from its tick. Design,
evidence and open questions:
[architecture/10-client-api.md](architecture/10-client-api.md) (2026-10-09;
nothing decided). Not part of the rewrite, so, as for
[Kept outside the rewrite](#kept-outside-the-rewrite), nothing starts before R8
unless it blocks play.

| # | Step | Gate |
|---|---|---|
| C0 | Answer the open questions (doc 10, section 5) | **You decide** |
| C1 | In-process snapshot; both display backends read it (still on the tick) | Agent |
| C2 | The Intuition widget as the first client: reads in `AdvanceMovie`, reports impressions | Agent |
| C3 | Wheeler off the tick: a native client in wheelerAPI (recommended) or an adapter | Agent, then **in game (you)**: a LoreRim Tracy session |
| C4 | Client ids, impressions and action reports into the selection log v3 and the learner | Agent |
| C5 | The export (`HuginnRequestAPI`), header, a guide for mod authors, a test client | Agent |

Recommended timing: C1–C4 after R8 and before R12's soak, so the baseline soak
runs on the final display path and R8's learner gets its choice set from what
was on screen; C5 after R12. That enlarges what R12's soak gates: the new
display path and log fields become part of the baseline.

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
| Hunger, cold, fatigue as a ramp | Curves on the raw 0–1000 value; plumb SMI's continuous value | R3; hunger's curve from LoreRim's stage table (0.23.31) |
| Thirst, only when a thirst system is found | `thirst` need + drink effects; scripted consumables need a second confirm (the effect appearing) | R3 (0.23.31): DVSMP's stage abilities (LoreRim); SunHelm and Last Seed not read |
| LoreRim's healing block | `healing_blocked` need | Missing; dump the active effects in that state first |
| Haggling gear at a merchant | `merchant` need; a worn-vs-offered cross-feature | Missing |
| Enchanted apparel beyond the craft skills | Armour effect columns + worn-vs-candidate cross-feature | R2 |
| Items matched to the enemy (silver, bane, sun, resist for a dragon's element, poisons by target) | Target families × effects | R3 |
| A summon when pressed in melee | Enemy-distance (gaussian) need | R3 |
| Damage over time on a boss | `boss_fight` need | Missing |
| Feather Fall before the jump | `drop_ahead` need: a downward ray cast ahead (replaces "estimated altitude") | R3 |
| Waterbreathing before the dive | `deep_water_ahead` need: the water depth under the same probes (`underwater` only fires once the head is under) | R3 (0.23.19); in-game check pending |
| Soul Gem Fragment (LoreRim MISC item) | Find how LoreRim uses it first | Open |
| Food at a cooking pot or spit (the R3 session: a spit read as a forge) | A `workstation_cooking` need (the bench kind exists: `core/BenchKind.h` reads it from the workbench keyword, 0.23.16) x food effect columns; smelter and tanning rack likewise if a column ever answers them | Sensor exists (`NeedSensorState::bench`); no need row |
| Soups for the cold meter, apart from warming spells | Split `survival_warmth`: Restore Cold (Update.esm 01002EE5, a soup's; restores the cold meter, `cold`) from Fortify Warmth (01002EE6 / Variable09, warming spells, the Torch, soups; raises the warmth rating, `warmth_deficit`). Two mechanics, confirmed via LoreRim Discord (2026-10-09); one column today, so `cold` x `survival_warmth` also reaches a warming spell. A column change: fixtures regenerate from the dumps | Effect records tell them apart (MGEF, keyword `CCSM_RestoreCold`, name); the columns do not |
| A resist before the hit (falmer, spiders: the user, 2026-10-10) | `threat_<element>` needs: the element damage the visible hostiles are expected to deal, learned per race from the damage this player took, × `resist_<element>`; plus the missing `poison_damage_rate` ([doc 11, section 8](architecture/11-sequences-and-redundancy.md)). Spiders are designed as an obvious pair (`target_arthropod` × `resist_poison`), but the R3 needs are logged only and not live until R8; today the old engine offers Resist Poison only once poisoned and reads spiders as Beast. Falmer have no route | Missing: hits keep their element but not their attacker; poison is classified but not summed |
| Resist Disease before a fight with disease carriers | A `disease_exposure` need (hostiles of a disease-carrying race in the fight: skeevers, wolves, bears, sabre cats, vampires -- a race table, as the target families) x `resist_disease`. 0.23.16 removed `diseased` x Resist Disease: resisting does not cure a disease already caught | Missing; no existing need covers it (`target_animal` also holds deer and horses) |

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
- **Review the slot classes as a whole** -- done in R11 (0.24.4):
  [5-slots.md](architecture/5-slots.md), "Slot classes, re-derived from cap(i)".
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
- **0.24.26, the hand equip gate** (Known bugs, below). In game: press a spell
  key while holding a charged spell in that hand -- the swap waits for the
  release (an info `[HandEquip] ... went after N ms held` line), and a charge
  held past 3 s drops the press (`[HandEquip] ... dropped ... still charged,
  held after 3.0 s`); the cast is never cut short. Watch for a `went ahead
  through` debug line (a caster state 7-9 that did not clear in 250 ms: is
  one of them a resting state?). Then draw a weapon from
  the inventory and watch for a `[HandModel] ... behaviour graph disagrees`
  warning (there should be none). In the inventory the number keys must reach
  the favourites, not Huginn. Debug log: `[EquipCaller]` names the thread and
  call path of every hand change Huginn did not make (the load-time Fireball
  drop below).

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

The transform cache and the 37 misread races: done in R0.

**Fixed in v0.24.26 -- a slot key swapped a spell over a charged one.**
2026-10-31 00:38 (LoreRim): a tap fired 0.6 s into a Fireball charge and put
Calm Touch in the right hand; the charge was cancelled, the caster went idle
with no release, and from then on the behaviour graph ignored equips
(iRightHandType stayed 9, weapons stayed on the sheath node) until the game
froze. A slot key's equip into a hand (spell, scroll, weapon, staff, torch)
is now parked and done on the main thread once the player lets go of the
cast -- both hands checked unless a right-hand equip leaves the left alone
-- and never interrupts it: a cast still going after 3 s drops the press
(a first cut interrupted at 1 s, which only split the 00:38 sequence over
two frames). Caster states 7-9 are waited on 250 ms at most; both waits run
on the game clock (`core/HandEquipGate.h`, `input/HandEquipGate`). A shield
waits as a left-hand item. While an equip waits, its selection is held
undecided and its confirm window, Remembrance capture and own-press note
restart from the equip (`core/SelectionGate.h`). If the hook stops ticking
with an equip parked for 5 s, an error line says so and the keys equip inline
until it runs again. Potions, food,
poisons, ammo and other apparel do not wait. After a hand equip while drawn
(kDrawn, not sheathing) and with both casters idle, the hands are redrawn, as
Wheeler-Refined does (`[Equip] bRedrawAfterHandEquip`).
The slot keys, and a tap still waiting out the double-tap window, now do
nothing in a loading screen, the main menu, a pausing menu, a menu that takes
the controls (favourites, dialogue, crafting) or without the
player's 3D, and the key goes to the menu (the inventory's favourites). New
diagnostics: a debug line of both casters and the graph before each hand
equip; caster states 7-9 named (they read as idle before); a
`[HandModel]` warning when the graph's hand type clearly disagrees with the
hand for 1 s, drawn and settled; and `[EquipCaller]`, the thread and stack
of every hand item taken off not by Huginn. Open: the Fireball that leaves the
right hand 3-6 s after every load of one save (no Huginn path logs it; the
`[EquipCaller]` line should name who), and the other-hand weapon swap and
bows still equip as before (Wheeler unequips unqueued and cleans both hands
first).

**Old engine, frozen (R8 replaces it; left as they are because old-engine
scoring is frozen).** Found in the LoreRim R3 session (2026-10-09); the
effect catalog and the need vector read each of them correctly since 0.23.16:
- **LoreRim's "Fortify Magicka/Health" food reads as a restore.**
  `ItemClassifier::ClassifyVitalEffect` (`ItemClassifier.cpp:1087-1106`)
  knows only the vanilla `MagicAlch*` keywords; LoreRim's food carries
  `REQ_PVM_Food_FortifyMagicka` / `_FortifyHealth` (Recover=1). Pies became
  "Low MP" candidates, the main combat churn driver of that session (R8,
  churn). cap(i) has them as `fortify_vital_magicka` (Apple Pie, Honey Nut
  Treat: `EffectFixtureTests`).
- **Resist Disease is offered while diseased.** The `resistDiseaseWeight`
  rule (`ContextRuleEngine.cpp:200-203`, read for resist-disease and
  cure-disease items at `ContextWeightForCandidate.cpp:198-208`) put Roasted
  Leek and Cooked Potato on the page with the subtext "Diseased"
  (`[Subtext] page 0 | 3=Diseased(Cooked Potato) 6=Diseased(Roasted Leek)`).
  Resisting a disease does not cure one already caught; needs.csv no longer
  pairs them (0.23.16).
- **A cooking spit, pot, smelter or tanning rack reads as a forge.**
  `CraftSkillForWorkstation` (`ContextRuleEngine.cpp:22-25`) maps every
  create-object bench to Smithing, so `fortifySmithingWeight` still fires at a
  spit. The need vector tells them apart by workbench keyword
  (`core/BenchKind.h`).
- **A script's equip just after a menu closes is credited as a menu pick**
  (2026-10-09). `PlayerInputGate::Explain` (`PlayerInputGate.h:91-94`)
  answers "menu (just closed)" for any equip within 2 s of an Inventory,
  Favorites or Magic menu closing (`MENU_CLOSE_INPUT_WINDOW_MS`), and
  `ExternalEquipLearner` opens a pending selection for it
  (`ExternalEquipLearner.cpp:80`); when that selection confirms,
  `SelectionLog` logs it with a +1 reward (`SelectionLog.cpp:162`) and
  `BanditSubscriber` (`EquipSubscribers.h`) trains the learner on it as the
  player's pick. Seen at
  12:44:39 with LoreRim's transient "Arcane Anchor" spell-learning spell
  (37076FA6), which a mod equips after a spell tome is read. Its SpellRegistry
  add/remove churn is expected (the user ruled it not a bug); only the reward
  attribution matters, for R8's learner data. The v3 log reads the same `via`
  as a menu pick (`IsPausingMenuVia`, `SelectionLogV3.cpp:439-443`).
- **`coldWeight` still reaches warming spells** (2026-10-09). Under the
  user's ruling (via the LoreRim Discord: the cold meter is restored by
  soups; warmth is a separate rating), that targeting is wrong: Warming
  spells and scrolls draw `coldWeight`
  (`ContextWeightForCandidate.cpp:162-163`, `:391-392`). Unlike the items
  above, the new data does not fully separate it yet: needs.csv no longer
  lists warming spells for `cold` (0.23.17), but `survival_warmth` still
  pairs with both until that column is split (Needs and effects to add).
  Places that still describe the old targeting, left as they are:
  `docs/nexus/page.md:412`, `docs/architecture/0-pipeline.md:515`,
  `configs/Huginn.ini:572`.

**Fixed in v0.23.13 -- hook-install race (int3 on a load-screen job thread).**
CommonLib-NG 3.7.0's `write_5branch` patches the call site before it writes
the trampoline's jump, into a block `set_trampoline` filled with `0xCC`. The
D3D11 Present hook and the Debug input hook went in at kDataLoaded, while
load-screen job threads already ran `BSGraphics::Renderer::End` and
`BSInputDeviceManager::Poll`; a thread hitting the patched call inside that
window took `EXCEPTION_BREAKPOINT` in SKSE's branch pool (~1 in 13 Debug
launches on simonrim). Both hooks are now installed in `SKSEPlugin_Load`
(`InstallHooks` in `Main.cpp`), before the engine starts, from one
`AllocTrampoline` sized for every hook; their bodies pass straight through
until `ImGuiRenderer::IsInitialized()` (now an acquire/release atomic).
`main` (frozen) still has the race: its D3D11 hook in every build and its
input hook in Debug.

## Mod compatibility

### Default slot keys are the number row
**Status:** deferred (the user does not use Skyrim's hotkeys). `iSlot1Key = 2`
.. `iSlot8Key = 9` are also Skyrim's favourites hotkeys, so one keystroke fires
two systems. Not F1-F8 (F5 quicksave); ten slot keys, not eight; many keyboards
have no numpad; a new default does not change an existing INI. Fixes, cheapest
first: a different default; [modifier keys](#modifier-key-bindings); a
read-only widget mode. The double-fire counts as a reach-in for metric 1.

### An overrides directory, so mod authors can ship their own
**Status:** built in 0.24.4 (R11) for the effect overrides
(`core/OverrideFiles.h`: `Huginn/EffectOverrides/*.ini`, the player's file
last); the notes below were the brief. `Huginn_Overrides.ini` (the old
classifiers' type / tag file) stays a single file: it reaches only D2.
(The user, 2026-10-02.) `Huginn_Overrides.ini` becomes a directory, SPID `_DISTR.ini` style.
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
(2026-10-09, 0.23.18, Debug, 48.5 min of play; self totals on the job threads,
from the capture's analysis): `PollPlayerMagicEffects` 5.29 s, `PollTargets`
3.39 s, `Inventory::DeltaScan` 2.35 s, `OnUpdate` 1.61 s,
`Pipeline::AllocateAndLock` 1.58 s; `Display::Wheeler` 0.59 s self now that its
children have zones (`Wheeler::AllocateOtherPage` 0.97 s, `WheelSync::WriteSlots`
0.33 s). Method:
[profiling/tracy-traces.md](profiling/tracy-traces.md).
- `Inventory::DeltaScan`: gate on `TESContainerChangedEvent` with a slow
  safety timer; compare counts in place.
- `PollPlayerMagicEffects`: early-out when the active-effect list is unchanged.
- `PollTargets`: build outside the write lock; `MAX_TRACKED_TARGETS` 50 → ~12.
- The rewrite adds per-tick work (need vector, cross-features, σ): measure it
  in R8 before optimising anything else.
- **Wheeler push stalls after a load** (2026-10-09 LoreRim Tracy capture,
  0.23.16 Debug): re-seat pushes of `Display::Wheeler` in the first ~3 min
  after a load had a median of 4.3 ms, against 1.0 ms later; 15 of the 18
  pushes over 5 ms were re-seats. All 7 pushes over 16.6 ms came within
  100 s of the load, and 3 of them were not re-seats (a wheel close, and two
  with no re-seat). Wheeler-side report: a local, unpublished note in the wheelerAPI working
  tree (`docs/reports/2026-10-09-huginn-push-spikes-after-load.md`).
  0.23.17 splits the cost with zones: `Wheeler::AllocateOtherPage`,
  `WheelSync::UpdatePage` (its `UnchangedCheck` and `WriteSlots`),
  `WheelSync::RecoverInvalidatedWheels`, `WheelSync::DetectVanishedWheels`.
  The next capture (0.23.18) put the slow pushes inside `WriteSlots`, no longer
  tied to the load; the 6 that could be measured stretched their frames by
  20–29 ms. The structural fix
  proposed is to take display pushes off the tick
  ([architecture/10-client-api.md](architecture/10-client-api.md), section 2.2).

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
