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
      Phase 2). (88 of 93 since 0.23.19 added `deep_water_ahead`, below.)
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
      probe's depth (`w<units>`). Host tests: a cliff into deep water, into a
      shallow stream, onto rock, a lake shore on flat ground, unknown probes,
      no hit over water.
- [ ] **In game (you):** the safe-landing depth (128 units) is a guess -- the
      game's threshold is not known -- and the deep-water need is untested in
      game. With `hg needs`: a cliff over deep water reads `drop_ahead` 0 and
      `deep_water_ahead` > 0; walking to a lake shore raises
      `deep_water_ahead` before the player is in the water; a cliff onto a
      shallow stream still reads a drop. If a jump into water the probe calls
      deep does hurt, raise `kSafeLandingDepth`.
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
  filter, from 870 stamped records), held out by launch. hit@1 level (+1.1 to +1.5, CI includes 0), NLL −0.25
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
  scratchpad and are not in the repo, and the filter from 870 records to 859
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
  belongs to R10 (wildcards by uncertainty).

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
- [ ] **Decide: which thread runs the update loop** (2026-10-09, two Tracy
      traces in `traces/202610/`, which is not checked in; the counts are
      trace09's unless marked). `OnUpdate`, driven by the InputEvent sink,
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
        loads (14 of them door or fast-travel loads) one of them is a full
        tick, `RunPipeline` and `Inventory::DeltaScan` included (trace04: 26
        full ticks, one load with two).
      `ForceUpdate` (`hg refresh`, `hg recs`, the test harness) runs a tick
      on its caller's thread; every path holds UpdateHandler's mutex, so no
      two ticks overlap. No job tick overlapped a main-thread zone (0 of
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
      (`UpdateLoop.cpp`, THREADS above `OnUpdate`; 0.23.18).
- Done when: a grep over `src/` and `tools/` for every pruned symbol finds
  nothing outside the new code (docs follow in R11); Debug and Release build
  clean; host tests cover the update (Var_p precision, step = variance,
  Plackett–Luce, opportunity counting), the explanation label is the largest
  term, the scorer reproduces R6's replay numbers on the logged data.
  **In game (you):** a session on the R6 bootstrap; cures, potions and gear
  rechecked (the old "cures reach the page" checks move here).
- [ ] The need signature joins the pipeline skip gate (moved from R3 by its
      verifier, round 1 on #188): a quantised (0.05) need signature, so
      continuous needs re-score; it then replaces `ambientSignature` and the
      elemental window. Not in R3 because it is not "logged only": modelled
      at 0.17 -> 4.6 pipeline runs/s in a melee fight and 0.01 -> 0.44 on a
      quiet dungeon walk, a wildcard on screen ~20% -> ~32% of quiet
      exploration (a roll happens on each run with none showing), Wheeler
      pushes ~4.6/s in fights, and the soak denominators move. Here, where
      the scorer reads the needs, re-scoring on a need step is the point;
      measure the wildcard share and the Wheeler push rate when it lands.
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
      lock and the challenger rule are R9.

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
| Feather Fall before the jump | `drop_ahead` need: a downward ray cast ahead (replaces "estimated altitude") | R3 |
| Waterbreathing before the dive | `deep_water_ahead` need: the water depth under the same probes (`underwater` only fires once the head is under) | R3 (0.23.19); in-game check pending |
| Soul Gem Fragment (LoreRim MISC item) | Find how LoreRim uses it first | Open |
| Food at a cooking pot or spit (the R3 session: a spit read as a forge) | A `workstation_cooking` need (the bench kind exists: `core/BenchKind.h` reads it from the workbench keyword, 0.23.16) x food effect columns; smelter and tanning rack likewise if a column ever answers them | Sensor exists (`NeedSensorState::bench`); no need row |
| Soups for the cold meter, apart from warming spells | Split `survival_warmth`: Restore Cold (Update.esm 01002EE5, a soup's; restores the cold meter, `cold`) from Fortify Warmth (01002EE6 / Variable09, warming spells, the Torch, soups; raises the warmth rating, `warmth_deficit`). Two mechanics, confirmed via LoreRim Discord (2026-10-09); one column today, so `cold` x `survival_warmth` also reaches a warming spell. A column change: fixtures regenerate from the dumps | Effect records tell them apart (MGEF, keyword `CCSM_RestoreCold`, name); the columns do not |
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

The transform cache and the 37 misread races: done in R0.

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
- **Wheeler push stalls after a load** (2026-10-09 LoreRim Tracy capture,
  0.23.16 Debug): re-seat pushes of `Display::Wheeler` in the first ~3 min
  after a load had a median of 4.3 ms, against 1.0 ms later; 15 of the 18
  pushes over 5 ms were re-seats. All 7 pushes over 16.6 ms came within
  100 s of the load, and 3 of them were not re-seats (a wheel close, and two
  with no re-seat). Wheeler-side report:
  `wheelerAPI/docs/reports/2026-10-09-huginn-push-spikes-after-load.md`.
  0.23.17 splits the cost with zones: `Wheeler::AllocateOtherPage`,
  `WheelSync::UpdatePage` (its `UnchangedCheck` and `WriteSlots`),
  `WheelSync::RecoverInvalidatedWheels`, `WheelSync::DetectVanishedWheels`.

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
