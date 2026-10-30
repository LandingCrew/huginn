# Selection log v3: the decision log the fit reads

**Status:** built in R4 (0.23.15). Logging only: nothing that scores, generates
candidates, allocates slots, trains the frozen learner or counts the soak
telemetry reads it. The v2 log (`Huginn_Selections.jsonl`) is written as before.

The offline fit (R6, [implementation map Phase 4](9-implementation-map.md#phase-4-fit-offline-toolsreplay))
and later the new learner (R8) need, for every decision the player made: the
situation (the need vector), every item they could have chosen with what it
does (sparse cap(i) and the runtime cross-features), the page Huginn allocated for display (`shown`, below), and an
explicit outcome among doc 9's three -- a key press from the page, a pick from
the held items off the page at a menu cost κ, or nothing
([doc 9](9-context-as-learner-input.md), theory page P11). This file is the
schema of that log, field by field.

| | |
|---|---|
| Code | `src/core/DecisionLog.h/.cpp` (records, encoder), `src/core/OldScore.h` (the old engine's score parts, 0.23.53), `src/core/NeedEpisodes.h` (episodes and their laps), `src/learning/SelectionLogV3*.cpp` (the game side), `src/pipeline/PipelineCoordinator.cpp` `CaptureEligible` (the eligible rows and their score parts) |
| Reader | `tools/replay/replay.py`: `iter_v3` / `load_v3`, `--v3 FILE...` prints a summary |
| Tests | `tests/core/DecisionLogTests.cpp` (encoder vs the golden file), `tests/core/NeedEpisodesTests.cpp`, `tests/core/CoverageTests.cpp` (covered(i), the filter reasons), `tools/replay/test_replay_v3.py` (CTest `replay_v3_roundtrip`) |
| Golden file | `tests/core/fixtures/decisions/synthetic_v3.jsonl`; beside it the same session as older builds wrote it, which the readers must keep decoding: `synthetic_v3_0.23.45.jsonl` (before `fb`, `cr`, `covered`, `summon`) and `synthetic_v3_0.23.52.jsonl` (before the old engine's columns `est`..`hk`, the `favorite` flag, `ctx.old` and `dec.rid`) |
| Fit loader | `tools/fit/v3load.py` (row tuple indices in `tools/fit/common.py`), tested by `tools/fit/test_fit.py` |

## Files

- `Huginn_Selections_v3.jsonl` in the SKSE log folder (`<Documents>/My
  Games/Skyrim.INI/SKSE`), appended across launches, modlists and characters.
  Every launch starts a **segment** with a `head` line.
- At 64 MiB the file is renamed `Huginn_Selections_v3-<launch>-<n>.jsonl` and
  a new one starts (with its own head). Nothing is ever deleted. If the rename
  fails (the file held open elsewhere), the writer keeps appending to the
  current file for the rest of the launch rather than retrying on every record.
- A file that does not end in a newline (a record torn by a crash or a full
  disk) is continued on a new line, so the torn record stays one bad line.
- Test mode (`run_tests.py --decision-session`) writes
  `Huginn_Selections_v3_test.jsonl` instead, truncated at the first record of
  the launch, so scripted presses never reach the player's data (the v2 log
  skips test-mode presses for the same reason).
- The reader takes `.jsonl` or `.jsonl.gz`: an old file can be gzipped by
  hand (JSON Lines of this kind compresses about tenfold).
- The reader skips a line that is not JSON and counts it (`stats["bad_lines"]`);
  until the next head, records that need a cap or context the torn line may
  have defined are skipped and counted too (`stats["skipped"]`). A later head
  resyncs. A torn head is detected by a cap id being defined twice
  (`stats["lost_heads"]`); until the next good head, decisions of another
  launch than the head in force are skipped. A truncated or corrupt `.gz` ends
  that file where it breaks (`stats["truncated"]`). None of this damage is
  raised. Without damage, a reference to an undefined id is a malformed file
  and an error -- except in a `.gz` that fails to decompress whole: there the
  reader cannot tell where the corruption starts (a flipped byte can decode
  to lines that parse, up to the CRC check at the stream's end), so every
  format error of that file is counted as a bad line instead (0.23.16). Its
  good records still decode; a plain `.jsonl` and an intact `.gz` stay
  strict.

## Versioning

- Every `head` and every `dec` line carries `"v": 3`. The reader refuses any
  other version and any record before a head.
- A field may be **added** to any line type within v3: readers ignore keys they
  do not know. A row column may be added only at the end, and the head's
  `row` list names the columns, so a reader can check.
- A row may be **shorter** than the head's `row` list (0.23.53): the old
  engine's columns (`est`..`hk`) are written only up to the last one that is
  set, and a column past a row's end reads null. The first twelve columns are
  always written. Both readers index the appended columns by the head's names
  and check the row's length. 0.23.53 stayed v3 for this reason: every
  addition is an appended column, a new flag bit, a new head list, a new
  context object or a new decision field, so files written before it read
  unchanged (`synthetic_v3_0.23.52.jsonl`), and a v4 would have made every
  existing reader refuse the new files for nothing.
- Removing a field, changing a field's meaning or unit, reordering row
  columns, or changing the episode definition in a way the head's `episode`
  object cannot express is **v4**.
- The column, need, cross-feature, kind and source names are written into
  every head; ids in the records index those lists, so a segment is readable
  against its own head even after `effects.csv` or `needs.csv` change.

## Line types

One JSON object per line (JSON Lines, UTF-8; a plugin string that is not valid
UTF-8 has each byte >= 0x80 escaped as its Latin-1 code point). Numbers: floats
to 4 significant digits, milliseconds as integers, seconds to 3 decimals; a
missing number is `null`.

| `t` | When | What |
|---|---|---|
| `head` | first line of every segment | the version and the name lists below |
| `cap` | before the first `ctx` (or `dec`) of the segment that uses it | one cap(i), by id |
| `ctx` | before the first `dec` of the segment that uses it | one logged context: needs, page, rows |
| `dec` | one per decision | the outcome, joined to a context by id |

Ids (`cap.id`, `ctx.id`) are **segment-local** for caps (they restart at 0
after every head) and **launch-unique** for contexts (a context re-emitted in a
new segment keeps its id). A reader keeps one table of caps and one of
contexts per segment and clears both at a head.

### `head`

| Field | Type | Meaning |
|---|---|---|
| `v` | int | 3 |
| `launch` | string | UTC start of the game launch, `YYYYMMDD-HHMMSS` |
| `list` | string | modlist folder (`LoreRim-5`, `simonrim-essentails`...) |
| `build` | string | `"<version> (<git sha>)"` |
| `cols` | string[243] | effect columns of `9-data/effects.csv`, in order (`cap.c` indexes it) |
| `needs` | string[97] | needs of `9-data/needs.csv`, in order (`ctx.need`, `ctx.in`, `dec.open`, `dec.ep.i` index it) |
| `cross` | string[8] | the runtime cross-features, in order (`row.x` indexes it). 7 before 0.23.46 (no `covered`) |
| `kinds` | string[10] | catalog kinds: `Spell Scroll Potion Poison Food Weapon Ammo Armor SoulGem Light` (`row.kind`) |
| `src` | string[10] | the old engine's candidate source types (`row.src`) |
| `row` | string[27] | the row columns, in order (below). 22 before 0.23.56 (no `mu`..`ex`), 12 before 0.23.53 (no `est`..`hk`), 10 before 0.23.46 (no `fb`, `cr`) |
| `flags` | object | row flag bits by name (`favorite` since 0.23.53) |
| `filters` | string[19] | (0.23.46) the candidate filters' reasons, in order (`row.fb` indexes it; `core/FilterReason.h`). 15 before 0.23.55 (no `no_follower` .. `favorite_suppressed`); absent before 0.23.46 |
| `floors` | string[5] | (0.23.53) how the old engine's floors treated an eligible row, in order: `pass`, `cold`, `ctx`, `util`, `suppressed` (`row.fl` indexes it; `core/OldScore.h`, [below](#the-old-engines-score-parts-02353)). `suppressed` is not written from 0.23.55 (a suppressed favourite is a filter drop, `favorite_suppressed`); the name stays so older rows read. Absent before 0.23.53 |
| `episode` | object | the episode definition in force: `onset`, `expiry`, `minSec`, `graceSec`, `answerSlackSec`, and `deathDrops: 1` (0.23.16: a death drops the open episodes and those in their grace; absent = 0.23.15, where a death could leave `nothing` records), and `lapSec`, `lapMaxStepSec`, `lapIdleSec` (0.23.51: the lap grid and the idle hold of its clock, [Laps](#laps); absent = every episode judged whole, no lap records). `replay.head_lap_sec(head)` reads it |
| `refreshPriorSec` | float | (0.23.55) `[Scoring] fRefreshPriorSec` in force: the centre of `covered`'s left(r) prior, and the active-buff filters' refresh window. The centre is read when a context is taken; a record whose context was taken under a different centre than the current head's starts a new segment naming that centre, so every record's `covered` values sit under the head that names their centre (a hot reload that changes it starts a new segment at the first record taken after it). Absent before 0.23.55: the centre was 5 s |
| `bootstrap` | object | (0.23.56, R8.2) the bootstrap file the shadow model was seeded from: `created_utc` (the file's own) and `sha256` (of the file's bytes), so a log names the exact file its `mu` columns came from (the bootstrap's `provenance` names its logs the same way). Absent when the new model is off (no file, or an invalid one) and before 0.23.56 |
| `curves` | [kind, p1, p2][] | (0.23.41) the `[Needs]` response curves in force, index-aligned with `needs`: `["logistic", 0.5, 10]` (`core/ResponseCurve.h`), so a fit knows which curve made every logged `need` value from its `in`. A hot reload that changes a curve starts a new segment (a new head line) before the next record. Absent before 0.23.41: those segments used their build's `needs.csv` defaults unless the INI overrode them. `replay.head_curves(head)` reads it |

### `cap`

`{"t":"cap","id":<int>,"c":[[<col>,<value>],...]}` -- the non-zero columns of
cap(i), `col` an index into `head.cols`, values as the effect catalog grades
them (percentiles within the load order, presence 1, see
`core/EffectMapper.h`). Caps are **content-addressed**: two items with the
same effects share one id, so a dynamic FormID that means something else after
a reload cannot mislabel a cap. Per-instance caps (a tempered or
player-enchanted weapon or armour piece; `EffectCatalog::InstanceEntry`) are
caps like any other.

### `ctx`

A situation as logged. Taken when one is needed: at a press (`why: "press"`),
when a selection menu opens (`"menu"`), at the onset of a need episode
(`"onset"`), at a lap of the open episodes (`"lap"`, 0.23.51; a tick with an
onset and a lap takes one context, tagged `"onset"`, so `why` alone does not
find every lap context: a `nothing` record's `ep.lap` says how its stretch
began, 0 at the onset, n at the episode's n-th lap). Several decisions can
share one: the picks of one menu visit (the Plackett–Luce co-picks), the
"nothing" records of needs that started on the same tick, and those of the
stretches one lap began.

| Field | Type | Meaning |
|---|---|---|
| `id` | int | unique within the launch |
| `utc` | string | when taken, `YYYY-MM-DD HH:MM:SS.mmm` UTC |
| `why` | string | `press`, `menu`, `onset`, `lap` (0.23.51) |
| `menu` | string | the menu's name, for `why: "menu"` (`InventoryMenu`, `MagicMenu`, `FavoritesMenu`) |
| `need` | [[i, v]] | the need vector: non-zero curve outputs, 0..1 (`needs/NeedMonitor`, R3) |
| `in` | [[i, v]] | what went into each curve, in the units of `needs.csv`'s `r3_input` (fractions, seconds, units of distance...) |
| `pipe.ok` | 0/1 | a pipeline run of **this game session** had been cached. 0 before the first run after a load (or `hg reset all`): the cache then still holds the previous save's page and candidates, so neither is logged (no `shown` rows, no `eligible` rows) |
| `pipe.page` | int | the page Huginn allocated for display, as for `shown` below (-1 when `pipe.ok` is 0) |
| `pipe.slots` | int | keys on that page |
| `pipe.ageMs` | int ms | how old that page was when the context was taken |
| `race` | string or null | editor ID of the hostile primary target's race (alive, hostile), else null |
| `wc.base`, `wc.max` | float | the wildcard odds in force: before 0.24.3 the per-slot probability base and cap; from 0.24.3 (R10) the roll's chance with nothing uncertain near the page and with an unknown item at its edge (`[Wildcards]`; `fUniformShare` is not logged -- `wp` is the exact propensity) |
| `env` | object | (0.23.41) where and when, for what the darkness need's input cannot say: `interior` 0/1 (an interior cell), `hour` (game hour, 0-24), `daylight` 0/1 (outdoors under a full sky in the climate's daytime: darkness reads 0), `light` (the raw light on the player, 0-1 in 0.1 steps, before the hold the darkness need reads -- `core/LightHold.h`). Absent before 0.23.41 |
| `summon` | object | (0.23.46) the player's summons: `n` the living actors the player commands (the count the summon filter compares), `limit` the commanded-actor limit the perks give (the perk entry point from 1; Twin Souls makes it 2; read only while a summon is up, so with `n` 0 it is the last value read in this launch, 1 before the first summon: meaningful only when `n` > 0), `act` one entry per living commanded actor, `[source, kind, remaining, elapsed, actor]`: the spell or scroll that cast it (the staff for a staff's enchantment; null if unknown), `"summon"` / `"reanimate"` / `"other"` (its effect's archetype: thralls apart from summons, doc 11 decision 3), seconds left on that effect (-1 = no timer), seconds since it was cast (a new cast dismisses the oldest), the actor's reference. Absent before 0.23.46 |
| `old` | object | (0.23.53) the old engine's pipeline run behind the eligible rows, taken with them (not necessarily the run `pipe` describes: `pipe` is read off the page as shown, and the two can be one run apart, as the eligible flags and `pipe` already could): `minCtx` and `minUtil` (`fMinimumContextWeight`, `fMinimumUtility`), `topN` (`iTopNCandidates`: the cold start runs when fewer rows than this pass), `coldBoost` (the cold start raises a row's context to `coldBoost` × `ucb`), `beta` (the exploration weight), `lambdaMin`, `lambdaMax`, `n0` (the learner's pseudo-observations), `fav` (`"boost"`, `"off"`, `"suppress"`), `favMin`, `favMax` (the favourites multiplier's range), `scored` (rows the run kept), `total` (the learner's total evidence, the UCB's), `phi` (the 18 state features the learner read, `StateFeatures`). Written when a run of this session is cached (`pipe.ok` 1). Absent before 0.23.53 |
| `heldFull` | 0/1 | 1: the held rows were read in full. 0: taken while inventory extra data must not be read, from kPreLoadGame until the post-load window ends (`Util::IsExtraListStable`, the registries' gate, plus a v3-local load-in-progress flag): one plain row per base form, no unique IDs, no per-instance caps or stack charges, `equipped` from the hands and the nocked ammo only (armour reads unworn) |
| `rows` | row[] | every item the player could choose (below) |

### Rows

Each row is an array, in the order `head.row` names:

| # | Column | Type | Meaning |
|---|---|---|---|
| 0 | `form` | string | FormID, 8 hex digits. Meaningful within its launch and load (`dec.gen`): a dynamic `FF` FormID can name another item after a reload |
| 1 | `uid` | int | the stack's `ExtraUniqueID` (weapons, armour), else 0. One row per item: two stacks of one weapon are two rows |
| 2 | `kind` | int or null | index into `head.kinds`; null = not in the effect catalog |
| 3 | `src` | int or null | index into `head.src` (the old engine's candidate type); null = not a candidate of the last pipeline run |
| 4 | `cap` | int | `cap` id; -1 = not in the catalog |
| 5 | `flags` | int | bits, below |
| 6 | `slot` | int | the key it was shown on (0-based), -1 = off the page |
| 7 | `util` | float or null | the old engine's utility, for rows that passed its floors; else null |
| 8 | `x` | [[i, v]] | non-zero runtime cross-features, `i` into `head.cross` |
| 9 | `wp` | float or null | wildcard propensity, for a row shown as a wildcard (below) |
| 10 | `fb` | int or null | (0.23.46) `filtered_by`: for a row that is not `eligible`, the candidate filter that dropped it in the same pipeline run as the eligible flags, an index into `head.filters`; null for an eligible row, and for one no registry gathered at all (not a candidate type, a registry that does not know it). Absent in older rows: read as null |
| 11 | `cr` | float or null | (0.23.46) the remaining seconds of the active effect that gave the row's `covered` (-1 = no timer: a constant effect, or a summon below the limit); null when `covered` is 0. Absent in older rows: read as null |
| 12 | `est` | float or null | (0.23.53) the old learner's estimate w·φ (`FeatureBanditLearner`) |
| 13 | `a` | float or null | (0.23.53) its confidence α = n_eff / (n_eff + n0) |
| 14 | `n` | float or null | (0.23.53) its stored evidence n for the item, before fading (n_eff = n × retention = n0·α / (1 − α)); 0 for an item it has no entry for |
| 15 | `ucb` | float or null | (0.23.53) its exploration bonus |
| 16 | `rec` | float or null | (0.23.53) `UsageMemory`'s recency boost |
| 17 | `prior` | float or null | (0.23.53) `PriorCalculator`'s prior |
| 18 | `cw` | float or null | (0.23.53) the context weight (`ContextRuleEngine`), before any cold-start boost |
| 19 | `fl` | int or null | (0.23.53) the floor, an index into `head.floors`; null for a row that is not eligible |
| 20 | `wu` | float or null | (0.23.53) for an unscored eligible row and a filter drop: the utility the scorer computes for it with no floor |
| 21 | `hk` | int or null | (0.23.53) the vanilla favourites hotkey bound to the item, 0-7 (keys 1-8) |
| 22 | `mu` | float or null | (0.23.56, R8.2) the new model's score of the row, in shadow ([below](#the-new-model-in-shadow-02356)): mu_i with no kappa |
| 23 | `sd` | float or null | (0.23.56) its standard deviation under the diagonal posterior |
| 24 | `lm` | float or null | (0.23.56) the D2 legacy feature `ln learn mult` the score read: ln(max(1 + λ·learn, 1e-6)) on a row the old floors passed, 0 on any other |
| 25 | `um` | int or null | (0.23.56) `util missing` the score read: 0 on a row the old floors passed, 1 on any other |
| 26 | `ex` | [k, j], "learned" or null | (0.23.56) the explanation: the need (`head.needs` index) and column (`head.cols` index) of the largest single θ·need·cap term, or `"learned"` when none is positive |

| Flag | Bit | Meaning |
|---|---|---|
| `eligible` | 1 | a candidate of the last pipeline run, **before** the old floors (`fMinimumContextWeight`, `fMinimumUtility`) dropped any: every item CandidateGenerator produced (after its hard filters: uncastable, on cooldown, equipped, buff already active; from 0.23.55 also the hard gates: no follower for a spell for others, no station for fortify apparel, not dark for a torch, a suppressed favourite) |
| `scored` | 2 | ...that also passed the old floors (`util` is set) |
| `held` | 4 | carried (inventory stacks the catalog describes) or a spell the player knows |
| `equipped` | 8 | in a hand, worn, or the nocked ammo, when the context was taken (a press's context is taken after the press's own equip: see `dec.preEquipped`) |
| `shown` | 16 | on the page Huginn allocated for display in its last run (`PipelineStateCache::TakeShown`), whether or not the widget or wheel was on screen; `slot` is its key |
| `wildcard` | 32 | shown as a wildcard |
| `override` | 64 | shown by an override |
| `remembered` | 128 | shown by a Remembrance hold |
| `addedAtPick` | 256 | the chosen item, missing from its context; carried in `dec.add` |
| `favorite` | 512 | (0.23.53) in the player's favourites: an inventory stack with `ExtraHotkey` (as `WeaponRegistry` reads it), a spell `MagicFavorites` lists (favourited or on a hotkey), or a candidate the old engine read as favourited (spells, weapons: the favourites multiplier's flag). An item's is read with the held set's extra data, so a `heldFull: 0` context has it only for spells and candidates |

The **choice sets** of doc 9, read off the flags: the page A = rows with
`shown`; the menu set H \ A = rows with `held`, without `shown` and not
equipped, where "equipped" is the `equipped` flag for every row **except the
chosen one, which uses `dec.preEquipped`**: a press's context is taken after
its own equip (a key equips before its callback; an outside equip's event
follows the equip), so the chosen row can carry `equipped` for the pick
itself. Every eligible item is a row whether or not it is held (spells the
player knows are held), and every held item the catalog describes is a row
whether or not it is a candidate -- all carried armour included, so an armour
menu pick has its alternatives (making all armour *candidates* is R8).

**H \ A in a `heldFull: 0` context.** Without the extra data, a held weapon or
armour piece is one plain row with `uid` 0, while the same stack can also be an
`eligible` row carrying its `uid` (the candidate knows it). They are one item.
So in such a context, for weapons and armour, take a held row with `uid` 0 and
an eligible row of the same FormID as one item: it is held, and it is shown or
equipped if either row says so. Count it once in H \ A. In a `heldFull: 1`
context every stack has its own row and nothing needs merging.

**Filter reasons** (`head.filters`, `core/FilterReason.h`; 0.23.46), in the
filters' order: `affordability` (a spell over the magicka on hand under the
uncastable policy Disallow, or a count of 0), `equipped`, `full_health`,
`full_magicka`, `full_stamina`; the active-buff filters `invisibility`,
`muffle`, `armor` (any armour-rating buff up), `summon_limit` (the player's
summon limit reached), `warming`, `waterbreathing`, `resist` (the element
already resisted past the threshold); then `cooldown`, `duplicate` (a second
candidate of one key: the first stays eligible) and `truncated` (past
`maxCandidatesAfterFilter`). The shadow fit of doc 11 groups the active-buff
seven as "covered" drops; a shown row with `fb` is still a key alternative
(choice sets come from `shown`, never from eligibility).

0.23.55 (R8.1) appends the hard gates, the old engine's context zeros moved
into the filters ([3-candidate-filtering.md §2.7](3-candidate-filtering.md#27-hard-gates-r81-02355)):
`no_follower` (a spell or scroll for someone else, no follower near),
`no_workstation` (fortify apparel, no station up for a craft it fortifies),
`not_dark` (a torch while darkness does not apply: daylight, or a light already
up) and `favorite_suppressed` (a favourite under `sFavoritesMode = Suppress`).
In the visitor they run before the active-buff filters, so an item both gated
and covered reads the gate. Appended, so the indices above do not move; a row's
`fb` indexes its own head's list, so older files read unchanged. In the same
build the active-buff seven let their item through in the buff's last
`[Scoring] fRefreshPriorSec` seconds (default 5; the refresh pass,
§2.8 there): a covered item can be eligible with `covered` > 0, and the
`covered` prior's centre is that key. Segment by build before pooling
eligibility across 0.23.55.

### The new model in shadow (0.23.56)

R8.2 runs the new model (doc 9; `core/ChoiceScore.h`) beside the old engine,
seeded from the R6 bootstrap (`Data/SKSE/Plugins/Huginn_Bootstrap.json`,
[9-bootstrap-format.md](9-bootstrap-format.md)); the old engine still ranks,
so these columns are logged only:

| Row | `mu` `sd` `ex` | `lm` `um` | computed |
|---|---|---|---|
| eligible | the run's (`PipelineCoordinator::ScoreShadow`, after the old scoring and the allocation): the run's need vector, recent_<family> at the run, the row's cap and cross-features, covered(i) with doc 11's greedy upgrade gate, b with its battery | off the same pass that ranked (`learning/legacy/LegacyLearnFeature.h`) | the run |
| held, never gathered | with the SAME run's tick (its v and W), the row's own cap, cross-features and covered (no upgrade gate) | 0 and 1 (the old engine did not score it) | the context |
| neither held nor eligible (a shown row missing from both) | null | null | -- |

A row's `mu` is the run's, as its `util` and old-engine parts are: the run is
`pipe.ageMs` old, and its need vector is the run's, not the context's `need`.
To score a row exactly as the fit would from the context, recompute it from
the context (`tools/fit/export_scores.py` does that from fitlib). The shadow
learner's state at the time is not logged (no cosave yet: every load resets it
to the bootstrap, so `mu` is the bootstrap's model after the picks of the same
load).

### The old engine's score parts (0.23.53)

The R6 component test found that R8 passes the gate only by keeping the old
learner's `learningScore` on the rows the old floors pass, and could not tell
whether the rest of the gap comes from the floors (the context weight acting
as a binary gate) or from the stale scores it had to carry forward for rows
the floors dropped. So every held or eligible row carries what the old engine
computes for it whatever the floors, logged only (nothing scores from it):

| Row | `est` `a` `n` `ucb` `rec` | `prior` `cw` | `fl` | `wu` | read when |
|---|---|---|---|---|---|
| eligible, `scored` | the parts its `util` was computed from (its breakdown); `n` read just after the scoring pass (see Limits) | yes | `pass` or `cold` | null | the run |
| eligible, not scored | computed again (`UtilityScorer::ExplainCandidate`: the scoring loop's own `ScoreCandidateInternal`) | yes | `ctx`, `util` (or `suppressed`, before 0.23.55) | yes | the run |
| dropped by a filter (`fb`) | as above | yes | null | yes | the run |
| held, never gathered | the learner's metrics and the recency, against the run's `old.phi` and state hash | null | null | null | the context |

"The run" is the pipeline run the eligible flags and `ctx.old` come from
(`PipelineCoordinator::CaptureEligible`); a held row the run never gathered
(24.5% of rows on the 2026-10-29 log: carried armour, potions and spells no
registry makes a candidate of) has no candidate to score, so it gets only
what the learner and the recency say of its FormID, read when the context is
taken (the learner's weights then, the run's state features). A chosen row
added at the pick is read the same way. A context taken before this
session's first run (`pipe.ok` 0) has no `old` and no parts.

**The floors** (`fl`), read off the same tests `UtilityScorer::ScoreCandidates`
applies, in its order: a favourite under favourites mode Suppress is never
scored (`suppressed`; written until 0.23.55, when such a favourite became a
filter drop, `favorite_suppressed`); a non-favourite under `minCtx` is dropped (`ctx`);
anything else not kept fell under `minUtil` (`util`). A kept row is `pass`, or
`cold` when the cold-start rescue kept it (fewer than `topN` rows passed, and
its context was raised to max(`cw`, `coldBoost` × `ucb`)). The cold start
also looks at `ctx` rows it does not keep: they stay `ctx`.
`tools/fit/v3load.py` checks every row's `fl` against these rules and the
context's `old` floors when it loads a log (`floor_check`) and prints any
violation: the floors are copied by hand from the scorer's tests, so a real
log is where the two drifting apart would show.

**What follows from them:**
- learn = a·est + (1 − a)·prior + beta·ucb + rec, λ = lambdaMin + a·(lambdaMax − lambdaMin), and
  util = c·(1 + λ·learn)·m with c = `cw` (`pass`) or max(`cw`, coldBoost·ucb) (`cold`), so the product
  m = corr × potion × fav of a scored row is util / (c·(1 + λ·learn)) (it includes the favourites rank
  scaling and the potion tier order applied after the loop).
- `wu` is c·(1 + λ·learn)·m at the raw `cw` with the favourites multiplier provisional (rank 0 of 1:
  `favMax` for a favourite under Boost, 0 under Suppress) and without the potion tier order: the
  utility the loop computes before its floor test.
- n_eff = n0·a / (1 − a); `n` − n_eff is the evidence the learner's fading has set aside.

**Limits.** A scored row's `n` and an unscored row's parts are read just after
the scoring pass under a fresh learner lock: a selection confirming on another
thread in between moves them by one update (the scored rows' other parts are
the breakdown's, exact). A row's parts are the run's, not the press's: the
run is `pipe.ageMs` old, as the page is. Rows that are neither held nor
eligible (a shown item missing from both) carry none.

**Record id** (`dec.rid`, 0.23.53): each pick the v2 log records (key,
wheel and menu picks the frozen learner takes, outside test mode) gets one
number per launch, from 1, written as `"rid"` into its v2 record
(`Huginn_Selections.jsonl`) and into its v3 decision; join on (`launch`,
`rid`). The breakdown of the scored candidates in the v2 record no longer
needs the form-and-time join. Null for `nothing`, for the picks the learner
skips (`learned: 0`: they write no v2 record), and when the v2 writer dropped
the record (its queue full). The rid is set once the record is queued, so if
the v2 writer then fails to write it (the file will not open, a write error)
the decision carries an rid that no v2 record has, and the join misses it.

**Runtime cross-features** (`effects.csv` item features; the math is
`core/CrossFeatures.h`, the inputs `effect/CrossFeatures.cpp`), computed every
update tick for the eligible rows over the live player state, and when a
context is taken for the held rows:

| `x` name | Range | Meaning |
|---|---|---|
| `overshoot_health/_magicka/_stamina` | -1..1 | (restored - deficit) / max, for a restore; 0 when the item restores nothing of it |
| `weapon_charge` | 0..1 | an enchanted weapon's or staff's charge left |
| `stack_count` | 0..1 | log1p(count) / log1p(20), saturating at 20 carried (the inventory's count) |
| `ammo_matches_launcher` | 0/1 | arrows with a bow, bolts with a crossbow in hand |
| `school_fortified` | 0/1 | a Fortify <school> of the item's school is active on the player |
| `covered` | 0..1 | (0.23.46) how much of the item's effect is already active on the player (below). Not an effects.csv column; set when a context is taken, for every row with a cap |

**`covered`** (doc 11 section 2.3, step 3; the math is `core/Coverage.h`, the
active-effect walk `StateManager_MagicEffects.cpp`), logging only:

- covered(i) = max over the **specific** columns j of cap(i) of
  min(1, active_j / cap_j(i)) × left(r_j).
- active_j: the strongest **beneficial** active effect on the player (not an
  ability; never a detrimental one) whose magic effect's R2 class is column j,
  graded as cap is: the value column j has in the cap of the item that cast it
  (the spell, potion or scroll, else the staff or worn piece for an
  enchantment). A source outside the catalog, or whose cap lacks the column (a
  perk row), counts at `kNeutralStrength` (0.5), as the mapper grades an
  unknown strength.
- r_j: the remaining seconds (duration - elapsed) of the longest-lived such
  effect; none for a constant effect (worn gear), which never runs out.
- left(r): the refresh prior of doc 11 decision 4, a logistic centred at
  `[Scoring] fRefreshPriorSec` (from 0.23.55; the head's `refreshPriorSec`,
  default 5 s; a fixed 5 s before) with scale 2 s (at the default: 0.08 at
  0 s, 0.5 at 5 s, 0.99 at 15 s), 1 with no timer. A
  buff with a few seconds left reads partly uncovered. It is a prior: `cr` is
  the r of the column that gave the value, so a fit can undo it exactly
  (covered / left(cr)) and apply its own curve.
- Summons (summon_creature*, summon_reanimate) are counted: min(1, n / L) with
  `summon.n` and `summon.limit`, times left(r) of the oldest summon once
  n >= L. Doc 11's greedy upgrade gate is not applied (it needs R8's score);
  `summon.act` carries the sources and timers to apply it offline. A bound
  weapon is an ordinary column.
- Equal-column buffs that do not stack (Oakflesh up, Stoneflesh offered) read
  partly covered (0.4 / 0.8 = 0.5); different columns never cover each other.
- Limits: a buff that targets others (Oakflesh on Target) reads covered by the
  player's own buff of its column; a worn enchanted piece covers itself.

**Wildcard propensity** (`wp`): the probability of the roll that put this item
on the page as a wildcard. From 0.24.3 (R10, `core/WildcardPick.h`,
`core/WildcardGate.h`): P(the roll came up) × P(the draw showed its FormID) --
the draw's shares of every pool row of that FormID (two stacks of one weapon
are two rows), the shares being `fUniformShare` / N plus the rest in
proportion to relevance × sd. It is a **per-window** propensity: each page
rolls once per refractory window (the first run after `fRefractorySeconds`
whose wildcard key can take one), and a roll that fails or draws nothing
restarts the refractory instead of trying again on the next run. So `wp` is
the probability that this window shows that FormID on the wildcard key, given
the state on the run that rolled; its complement for the window, 1 − P(roll),
is the chance the window showed no wildcard (not logged: a window without one
writes nothing). An inverse-propensity reader weights a wildcard pick by 1 /
`wp` per window, not per pipeline run. Before 0.24.3: P(its slot rolled a
wildcard) × (the entries of its FormID in the pool) / (the pool it was drawn
from), given the earlier rolls of the same pass, the pool holding one entry
per candidate row of the top row's type; those rolls retried on every run, so
that `wp` understates the chance per window. Either way it does not include
whether a roll was asked at all (rolls wait for no wildcard being active, the
refractory period, and no combat). Only rows shown as wildcards carry it.

### `dec`

| Field | Type | Meaning |
|---|---|---|
| `v` | int | 3 |
| `rid` | int or null | (0.23.53) the id this pick's v2 record carries ([Record id](#the-old-engines-score-parts-02353)); null when there is none. Absent before 0.23.53 |
| `seq` | int | per launch, in write order. The writer assigns it as it writes, so a record dropped from a full queue takes no number, and a record whose write the stream reports failed gives its number back to the next one: no gaps. (A failed write that still reached the disk leaves a torn line, which the reader skips, or in the rarest case a record whose `seq` repeats in the next one.) |
| `utc` | string | when the record was made: the confirmation for a pick; the end of the grace for nothing (of the episode, or of the lapped stretch). Since 0.23.51 the `nothing` records judged on one tick share one `utc`, so the stretches one lap tick cut carry the same `(launch, gen, utc)` (`fitlib.lap_tick`) |
| `launch`, `list` | string | as in the head |
| `char` | string | 16 hex digits: the character (`g_activeCharacterID`) |
| `gen` | int | the load within the launch (a reload abandons what came before) |
| `out` | string | **`key`**, **`wheel`**, **`menu`**, **`nothing`** (below) |
| `form` | string or null | the item chosen; null for nothing |
| `name` | string or null | its name |
| `row` | int | the chosen row: an index into `ctx.rows` followed by `add`; -1 for nothing |
| `add` | row[] | rows not in the context: the chosen item when it was missing |
| `src` | string | the old label: `Hotkey`, `Wheeler`, `External` |
| `via` | string | how: `key 3 (s2)`, `Huginn wheel`, `inventory menu`, `favorites menu`, `magic menu`, `menu (just closed)`, `vanilla hotkey`, `own wheel`, `Wheeler wheel` |
| `case` | string | the old A–E attribution of an outside pick. Unchanged by 0.23.46: why a case-A item was not a candidate is the chosen row's `fb` (the debug log's `[Reach]` and `[Selection] Confirmed` lines carry it as a separate `filtered_by=` field) |
| `how` | string | what confirmed it: `consumed`, `still equipped`, `used` (a soul gem) |
| `kind` | string | `consume` or `equip` |
| `confirmMs` | int ms | press to confirmation |
| `repeat` | 0/1 | the same item equipped again within the repeat window (30 s): the same decision to the learner, logged anyway |
| `learned` | 0/1 | the frozen learner took it |
| `skip` | string | why it did not: `stale` (the pipeline cache older than `fExternalEquipTimeWindow`), `disabled` (learning from outside equips off), `armour` (armour picks reach the v3 log only) |
| `ctx` | int | the context it is joined to |
| `ctxAgeMs` | int ms | press time minus the context's time (for nothing: record time minus the stretch's start -- the onset, or the lap that began the stretch) |
| `open` | int[] | needs with an open episode at the press (opportunity counting) |
| `preEquipped` | 0/1 | the chosen item was equipped before the press: in a hand or the nocked ammo on the newest update tick at least 0.25 s before it. 0 for anything else (an equip event says armour was not worn), **and 0 when no tick is that old** -- in the first 0.25 s after a load, when the snapshots were just cleared, it means "not known", not "not equipped" |
| `ep` | object or null | nothing only: `need` (id), `i` (index), `durSec` (the stretch's length: the whole episode when it never lapped), `peak` (the highest value inside the stretch), `onset` (UTC, the EPISODE's onset, kept across laps), and since 0.23.51 `why` (`"end"`: the episode expired and this is its last stretch; `"lap"`: the stretch reached the lap grid with the need still on), `lap` (the stretch's index, 0 = from the onset) and `epSec` (the episode's onset to the stretch's end). A record without `why` (before 0.23.51) is a whole episode: `"end"`, lap 0, `epSec` = `durSec` (`replay.nothing_stretch(d)` and `tools/fit/v3load.py` default them) |

## Outcomes

| `out` | Recorded when | Joined to |
|---|---|---|
| `key` | a Huginn key's selection confirms (`SelectionTracker`) | the context taken at the press |
| `wheel` | a pick on Huginn's own Wheeler wheel confirms | the context taken at the press |
| `menu` | any other selection the player made confirms: the inventory, magic or favourites menu, a vanilla favourites hotkey, one of the player's own Wheeler wheels. Includes the picks the frozen learner drops (`learned: 0`): a stale cache, the learning toggle off, armour | for a pick from the inventory or magic menu (open, or closed within 2 s): the context taken **when the menu opened**; otherwise (the favourites menu included) the context taken at the press |
| `nothing` | a need episode ends unanswered, or (0.23.51) a lapped stretch of a still-open one passes unanswered (below) | the context taken at the stretch's start: the episode's onset, or the lap that began the stretch |

A selection confirms as before (`SelectionTracker`): a consumable when its count
drops within 2.5 s, gear when still equipped 3 s later. An unconfirmed pick (a
misclick swapped away) writes nothing. Picks the learner drops are confirmed the
same way by the v3 log itself. Picks made by scripts or the engine (no player
input: an auto-quaff, the quiver refilling, an outfit at load) are not choices
and are not logged.

**Why menu picks join the menu-open context.** Reaching into the menu is the
decision the outcome records (doc 9: a pick from H \ A at cost κ instead of a
key): the player looked at the page, and chose the menu instead, at the moment
the menu opened. Inside the inventory and magic menus the game is paused (the
world does not move, the HUD and the page are hidden), so the situation then is
the situation of the pick, apart from wall-clock decays; the picks of one visit
share the context. `ctxAgeMs` says how long the player browsed. The favourites
menu joins the same way (0.23.16). 0.23.15 joined its picks at the press, on
the belief that it does not pause and leaves the widget in view; reverted
because a favourites pick is a reach-in like any menu pick ("if we are
reaching into the favorites menu huginn has failed in some way", the user,
2026-10-09), and because on LoreRim it pauses the game ("FavoritesMenu was
open 4830 ms: 45 update tick(s) inside (9.3/s, 45 with the game paused), 10
pipeline run(s)"): the pipeline repages inside it, so the page at the press is
not the page the player turned away from. Logs written by 0.23.15 have
favourites picks with `why: "press"` contexts; same v3 format.

**Staleness.** Measured in game (vanilla+, 0.23.15, test mode): the update loop
keeps ticking inside the inventory menu (57 ticks in 5.9 s, 9.6/s, every one
with the game paused) and the pipeline keeps running (4–5 runs), so the page
cache was 94–102 ms old when the menu closed; on LoreRim, 50 ticks in 5.5 s
(9.1/s), 4 runs, 73 ms at the close. `fExternalEquipTimeWindow`'s 500 ms
therefore does not trip in an ordinary menu visit; it trips when the loop
itself stalls -- on LoreRim the cache was already 581 ms old at the moment the
inventory opened (a pick at that instant would have been dropped by the old
path), and by the picks 1.5 and 2.5 s later it was fresh again. Either way the v3 log never drops a pick
for staleness: such a pick is written with `learned: 0, skip: "stale"` and the
age of its context.

## Need episodes ("nothing pressed")

Per need k, on its curve output v_k (`core/NeedEpisodes.h`; the values in force
are in every head):

| | Rule |
|---|---|
| onset | v_k ≥ 0.5 while no episode of k is open. 0.5 is the midpoint every curve is built around |
| expiry | v_k < 0.25. The gap is hysteresis: a need wobbling about 0.5 is one episode |
| length | an episode shorter than 1 s is dropped (no one can react to it) |
| answer | a confirmed selection (any outcome) whose press lies in [onset, expiry + 0.5 s] answers the episode. The slack is for a press that itself ends the episode: an update tick can see the need gone before the press is stamped (seen in game: 2 ms) |
| grace | an ended episode is judged 4 s after its expiry (the 3 s equip confirm, the slack and a few ticks), so a pick made inside it but confirmed later still answers it. It is judged on each update tick **after** the selections' confirmations: after a stall of the loop, a confirmation and the end of a grace can land on one tick, and the confirmation counts first |
| nothing | an ended episode, ≥ 1 s long, unanswered when its grace runs out: one `nothing` record, joined to the context taken at the onset |
| paused | episodes neither start nor end while the game is paused (a menu): the world is frozen, so a change then is a wall-clock decay or the player's own menu action, which a pick inside the still-open episode answers |
| death | (0.23.16) when the player dies, every open episode and every ended one still in its grace is dropped, as at a load, and none opens while the player is dead: a death is not a decision. Earlier logs can hold `nothing` records cut short by a death a few seconds before a reload. Since 0.23.51 the lapped stretches still in their grace are dropped too |
| lap | (0.23.51) an open episode is cut into stretches on a shared 60 s grid of active seconds, and each stretch is judged as an ended episode is: [Laps](#laps) |

Every need takes part, the always-on ones too (`loadout_*`, `downtime`): they
rarely end unanswered, since a weapon swap that ends a loadout episode is a
selection, and the fit can drop any need it does not want -- the record names
it. Several needs that start on one tick share one onset context. A press does
not answer an episode it started (the onset is after the press).

### Laps

**Why (0.23.51).** Before 0.23.51 a persistent need never wrote a `nothing`
record of its own. Hunger, thirst, diseased, cold, warmth_deficit and fatigue
stay on for minutes to hours: any confirmed selection anywhere in such an
episode answered all of it, and a load dropped it still open. The mage's log
(all three files, 2026-10-15 to 10-29, ~45 h of play, character
FEFD1B039AC78799: 1,908 picks, 14,014 `nothing` records) holds **not one**
`nothing` record for an episode of those six needs.

That does not keep them out of the `nothing` contexts. The three-outcome
fit's outside utility is u0 = θ_out · n, where n is the whole need vector of
the record's context (`tools/fit/fitlib.py`: `build` fills n, `Model.scores`
takes u0), and a `nothing` record's context is the onset of whichever need's episode it closes:
a persistent need that is on then is in n. What the old data lacks is
narrower: its `nothing` records are sampled where other needs start, not
across the time a persistent need stays on. On at ≥ 0.5, at picks against at
`nothing` records:

| need | at picks | at `nothing` |
|---|---|---|
| hunger | 167 / 1,908 = 8.8% | 1,025 / 14,014 = 7.3% |
| thirst | 128 = 6.7% | 704 = 5.0% |
| diseased | 225 = 11.8% | 1,475 = 10.5% |
| warmth_deficit | 149 = 7.8% | 777 = 5.5% |
| cold | 4 = 0.2% | 310 = 2.2% |
| fatigue | 9 = 0.5% | 96 = 0.7% |

So the old data's bias is mild and mixed: four of them lean a little toward a
pick, cold (and barely fatigue) the other way, and none is absent at
`nothing`. Laps sample the time itself: each unanswered stretch of an open
episode is a `nothing` record whose context has the need on.

**The rule** (`core/NeedEpisodes.h`):

| | |
|---|---|
| active clock | the time between two update ticks, at most 1 s per tick (`lapMaxStepSec`). Ticks happen only while the game runs unpaused with the player alive, so a pause, a death, a load door or a stall adds at most a second |
| hold | the clock also adds nothing on a tick when the player cannot or does not press (`LapHoldFor`): a dialogue menu is open (it does not pause the game), the movement controls are disabled (a carriage ride, a cutscene: `ControlMap::IsMovementControlsEnabled`), or the player's latest input -- any key, mouse or gamepad button, the mouse moving, a stick off centre -- is 30 s old or more (`lapIdleSec`; `UpdateHandler::SecondsSinceInput`). Onsets and expiries go on during a hold. The debug log has a `[SelectionV3] lap clock holds (<why>)` / `runs` line per change |
| grid | a grid point every 60 s of active time (`lapSec`), shared by every need; it restarts at 0 on a load |
| lap | at a grid point, every open episode whose current stretch began at or before the previous grid point laps: the stretch closes and a new one begins now, with a new context (`why: "lap"`, one for every need that lapped on that tick) |
| so | no stretch is shorter than 60 s except an episode's last one (cut by its expiry); the first stretch lasts 60-120 s; an episode that ends within 60 s of its onset is judged exactly as before 0.23.51; the laps of every need share one tick and one context |
| answer | a lapped stretch is answered by a confirmed selection pressed in [start, end] -- no slack: its end is the grid's cut, not something a press did, and a press after it lies in the next stretch. It waits the 4 s grace like an ended episode (a pick pressed just before the cut can confirm 3 s later) |
| record | an unanswered lapped stretch writes one `nothing` record with `ep.why = "lap"`, joined to the context taken at its start. The episode's last stretch is an ordinary ended episode (`ep.why = "end"`, slack included; dropped under 1 s); its `durSec` is the stretch's length and `epSec` the episode's |

**Why the hold.** A lap stands for time in which the player could have
pressed something. Unpaused time in which no press is possible -- the player
away from the keyboard with the game running, a carriage ride, a dialogue --
would otherwise lap like play, and nothing in the record would let a fit tell
it apart. The mage's log shows how much such time there could be: of the
51.8 h between consecutive contexts within a launch, 20.1 h lie in 261 silent
gaps over 120 s (no pick, no onset, no menu context), 10.4 h in gaps over
5 min, 5.5 h over 15 min and 2.8 h over 30 min. The log cannot say how much of
that was paused, played quietly or left unattended; unattended and unpaused,
the clock without the hold would have run through all of it. With the hold an
unattended gap adds at most 30 s. 30 s: input in play is near continuous
(movement and mouse-look send events every frame they are used, a held key
every frame it is held), so 30 s without any is a player who is not playing,
while a lull spent looking around still counts; and it is half a lap, so an
unattended spell can complete at most one grid point that the play before it
had nearly reached. Two known gaps: any non-zero stick value counts as
input, so a gamepad whose stick drifts past the engine's deadzone keeps the
clock running while the player is away; and a menu that a list makes
non-pausing (a crafting bench, say) passes input like play, so time in it
laps like play. Neither is tested in game. A brief disable (a kill move, a script that stops the
controls for a moment) only shifts the grid by its length.

**Why 60 s.** The lap is a time slice in which the player could have picked
something and did not; each unanswered slice is one "nothing" observation.
From the same log:

- *What "unanswered" means.* Between consecutive picks of one load the median
  gap is 17 s, the 75th percentile 48 s and the 90th 121 s; 34% of gaps exceed
  30 s, 19% exceed 60 s and 9% exceed 120 s. At 60 s a slice with no pick is
  the less usual case (about one gap in five), not every short lull.
- *Weight.* The ended episodes that write `nothing` records today and last
  30 s or more have a median of 75 s; a 60 s lap weighs about like one of them,
  and every episode under 60 s is untouched.
- *Data and volume.* A rough replay of the rule on the log's contexts (the
  need vector held between contexts, gaps of 120 s or less as play, 31.6
  active hours; only an estimate: the log has no per-tick values or input, so
  paused time inside a short gap counts as active) gives, at 30 / 60 / 120 s:
  80 / 31 / 11 lap ticks an hour with an unanswered stretch (the contexts that
  reach the file, ~13 KB each: about 1.1 / 0.4 / 0.15 MB/h), and 318 / 109 /
  32 per-need records an hour. Hunger would get 254 / 102 / 40 records over
  the log against ~170 picks with hunger on: 60 s gives each persistent need
  the same order of evidence as its picks; 120 s is thin, 30 s triples the
  volume for little.
- *The silent gaps, at 60 s.* Counted as breaks (no time), the replay gives
  985 lap ticks with an unanswered stretch (31/h of play). If every silent gap
  over 120 s had been unattended and unpaused, the hold lets each add at most
  30 s: 1,101 lap ticks (+116, +12%; hunger 102 -> 125). Without the hold the
  same case would have run the clock through all 20.1 h: 2,153 lap ticks
  (+1,168, more than doubling them; hunger 348), records of time in which
  no one could press. A silent gap in which the player did play on quietly
  laps as play, as it should; the log cannot say how many there were.

**Why active seconds, not game time.** Game time jumps with waiting,
sleeping, fast travel and load doors (and runs at the list's timescale), all
of it time in which no press is possible; a game-time lap would write a burst
of records after every wait. The choice the record stands for -- press or not
-- happens in real playing time, as the picks do; the active clock is that
time with the pauses taken out. Hunger and the other survival needs advance on
game time, but their values are in the context, not in the lap.

**Several needs per lap.** The laps of one tick write one record per need that
lapped unanswered (the record names the need; `sensors.py` and `audit.py`
count by need, and count lap records apart from episodes, `v3load.is_lap`, so
the churn and length stats stay per episode, the length read from `epSec`). They do not all share one context: a stretch begun by the
previous lap (`ep.lap` > 0) is joined to that lap's context, an episode's first
stretch (`ep.lap` 0) to its onset's, which can lie anywhere in the grid
interval before. What they share is the tick: they are judged together and
written with one `utc`. A fit that reads `nothing` as one observation per slice
should keep one per lap tick: `fitlib.build(laps="once")`, the default, keyed
on `(launch, gen, utc)` (`fitlib.lap_tick`), keeps the lowest-seq record begun
by a lap (its context is the slice's start), else the lowest-seq one (`"all"`
keeps every one, `"none"` drops them all -- the data a build before 0.23.51
would have logged).

**Logs written before 0.23.51** have no lap records and read a quarterstaff
as `loadout_two_handed`, so a three-outcome fit over them keeps both biases:
fit with `--since 0.23.51`, or split those needs at 0.23.51
([tools/fit/README.md, "Reading logs written before 0.23.51"](../../tools/fit/README.md#reading-logs-written-before-02351)).

## Perception line

Only what the player can perceive or what Huginn already reads: the player's
own inventory, spells and equipment, the need vector (R3's sensors), the page,
and the race of a hostile target in view (its look and name are on screen; the
race map keys on it). No enemy spell lists, inventories, levels or hidden
numbers. `covered` and `summon` (0.23.46) read the player's own active effects
(the Active Effects menu lists them), the summons at the player's side and the
perks the player chose.

## Volume and cost

Measured with the test session (`run_tests.py --decision-session`, Debug,
2026-10-09). The LoreRim save that ran was an early character (39 held items);
the last column scales the measured ~57 bytes per row to a soak-sized LoreRim
inventory: the October soak's v2 records held 107 scored candidates on average
and 185 at most (after the floors), so ~250 rows per context is the estimate.

| | vanilla+ (test character) | LoreRim, measured (early character) | LoreRim, soak-sized (estimate) |
|---|---|---|---|
| rows per context | 93 (66 eligible, 93 held) | 42 (27 eligible, 39 held) | ~250 |
| a new context, caps already defined | 5.8 KB | 3.4 KB | ~15 KB |
| a decision sharing its context | ~430 B | ~425 B | ~430 B |
| the first context of a segment (every cap defined) | 16 KB (93 caps) | 7.6 KB (38 caps) | ~40 KB |
| head | 6.7 KB | 6.7 KB | 6.7 KB |
| update tick (eligible rows' cross-features, episodes), mean / max | 89–123 µs / 1.4–3.2 ms | 84 µs / 5.9 ms | -- |
| taking a context, mean / max (fix round) | 1.0–1.2 ms / 1.4–2.0 ms; 0.3–0.45 ms with the held set cached | -- | -- |

The tick's maximum is an onset taking a context; most of a context is the held
read (the inventory walk: ~0.9 ms of ~1.3 ms on vanilla+). All Debug numbers.
To keep that off the moments that matter: a press always reads the held set
(its state then); a menu open or an onset reuses it until the player's
inventory or equipment changes (`TESContainerChangedEvent`, `TESEquipEvent`)
or 5 s pass; and the first full read
after a load -- which maps every per-instance cap through the effect mapper --
runs once on the first update tick the post-load window allows, not on the
first press.

Per hour of play: the October soak made about 54 selections an hour (412 page
picks plus 83 menu picks in 9.1 h); a menu visit adds one context however many
picks it holds; unanswered episodes add some more, sharing contexts when they
start together. At ~60 new contexts an hour that is **~0.4 MB/h on vanilla+**
and **~1 MB/h on a soak-sized LoreRim inventory** (~2 MB/h at 120 contexts
and 300 rows); a 64 MiB file then holds roughly 30–60 hours of LoreRim play.
The rate of `nothing` records in real play: ~310 an hour on the mage's log
(14,014 in ~45 h, LoreRim, 0.23.15-0.23.47), most of them short episodes of
darkness, magicka_burn and the distance bands. Laps (0.23.51) add an estimated
~110 records and ~30 contexts an hour of play, ~0.4 MB/h; time left unattended
adds at most 30 s of lap clock per spell ([Laps](#laps)).

**0.23.53's columns** (the old engine's score parts, `hk`, `ctx.old`,
`dec.rid`), projected over the 2026-10-29 log (1,041,112 rows, 5,389 contexts,
6,195 decisions in 30.4 h of LoreRim play; 2.25 MB/h as written) with values
drawn from the v2 log's candidate breakdowns and each row written as the
encoder writes it: +27.7 B per scored row (an independent estimate from the
same breakdowns, taking `n` as n_eff = 2a/(1 − a), gives 30.8 B), +34.6 per unscored eligible row,
+36.6 per filter drop, +10.9 per held row never gathered (mostly
`0,0,0,1,0`: an item the learner has no entry for), +293 B per context
(`old`), +11 B per decision (`rid`): **+0.89 MB/h (+40%), 3.1 MB/h in all**
(about 1.0 MB/h with the higher estimate: take the figure as up to ~10% low),
about 26 B per row on average; a 64 MiB file then holds about 20 hours of
LoreRim play. The synthetic golden session grew from 23,440 to 24,885 bytes.
The CPU side: the scored rows reuse their breakdown (one learner lookup each
for `n`); an unscored row or a filter drop costs one more pass of the
scorer's per-candidate parts (prior, correlation, potion multiplier) per
pipeline run; a held row never gathered costs one learner lookup and a
20-entry recency scan per context.

What bounds it:
- **Caps once per segment**, content-addressed: the 243-column vectors are
  written once and referenced by id (the first context of a segment carries
  them all).
- **Sparse everything**: caps, needs, inputs and cross-features list their
  non-zero entries only.
- **Shared contexts**: a menu visit's picks, the episodes that start on one
  tick, and the stretches one lap begins, write one context.
- **Rotation** at 64 MiB; old files can be gzipped (the reader reads `.gz`).
- **A background writer** (as the v2 log): the game thread builds the record
  (copies) and queues it; formatting and the file run on their own thread,
  1,024 records at most queued (it warns and drops past that).

## Known limits

- The chosen row is matched by FormID: a key press carries no stack id, so of
  two stacks of one weapon the shown one (key, wheel) or the first eligible
  one (menu) is taken. Shown rows are matched by FormID and unique ID (the
  page's assignment carries it), then by FormID alone.
- The `equipped` flags of the rows other than the chosen one come from the
  held read: for a press, the state just after its own equip; for a menu or an
  onset, the last read (re-done on any inventory or equipment change).
- `shown` is not evidence that the page was on screen: it is the page Huginn
  allocated, logged the same whether the widget was hidden or the wheel closed.
- `ctx.need` is the latest update tick's vector (the monitor runs every tick,
  ~100 ms); the page is the last pipeline run's (`pipe.ageMs`).
- Game data changed by scripts after the catalog was built is not seen (the
  catalog's own limit, R2). Items the build never saw -- a player-made potion
  or poison (a dynamic `FF` form) -- and spells the player knows that no tome
  teaches are mapped the first time they are held, graded against the build's
  populations (0.23.41, `EffectCatalog::FindOrMap`, `core/LateCatalog.h`);
  before 0.23.41 their rows had `kind` null and `cap` -1.
- A press inside a menu opened before a game load (none in practice) falls back
  to the press-time context.
- The old engine's score parts (0.23.53) are the pipeline run's, read with
  the limits in [their section](#the-old-engines-score-parts-02353); a held
  row never gathered has no prior, context weight or floor (it has no
  candidate). The favourite bit of an inventory item needs the held set's
  extra data (`heldFull: 1`).
