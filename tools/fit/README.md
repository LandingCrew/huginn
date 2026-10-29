# tools/fit -- the R6 trial fit over selection log v3

Offline scripts that read `Huginn_Selections_v3.jsonl` (the selection log v3,
[docs/architecture/9-selection-log-v3.md](../../docs/architecture/9-selection-log-v3.md))
and fit doc 9's choice model, its baselines and a set of sensor audits. They
were the trial fit of 2026-10-11 that found the sensor bugs fixed in 0.23.41.
Python 3 with numpy; nothing here runs in the game or the build.

## Where files go

Run every script from a **scratch work directory** outside the repository
(or set `HUGINN_FIT_WORK`): `v3load.py` writes `v3.pkl` there, the others read
it and write their `.txt` / `.pkl` outputs beside it. A work directory inside
the repository is refused, so no data or output is committed (`.gitignore`
here is a second guard).

The baselines that read the old engine's context weights also read the v2 log
(`Huginn_Selections.jsonl`): `HUGINN_FIT_V2`, else the file of that name next
to the v3 file `v3load.py` read, else none (those baselines then read NaN).
`effects.csv` (the obvious pairs) comes from `docs/architecture/9-data/`.

The v3 file rotates at 64 MiB (`Huginn_Selections_v3-<launch>-1.jsonl` is the
older part); `v3.pkl` for both parts is ~120 MB. `v3load.py` takes under a
minute; a 10-fold `cv.py picks` takes about half an hour, `three` a few hours.
Run them at low priority while the game is up (PowerShell: start the process,
then set its `PriorityClass` to `Idle`), and without `python -I`: the scripts
import their siblings (`common`, `fitlib`) by name.

## Run order

```sh
cd <scratch>
python <repo>/tools/fit/v3load.py <SKSE>/Huginn_Selections_v3-20261015-180003-1.jsonl <SKSE>/Huginn_Selections_v3.jsonl --list
python <repo>/tools/fit/v3load.py <SKSE>/Huginn_Selections_v3-20261015-180003-1.jsonl <SKSE>/Huginn_Selections_v3.jsonl --char FEFD1B039AC78799
python <repo>/tools/fit/inspect_heads.py "<same v3 file>"   # optional: what changed between heads
python <repo>/tools/fit/audit.py        # -> audit.txt
python <repo>/tools/fit/sensors.py      # -> sensors.txt
python <repo>/tools/fit/sensors2.py     # -> sensors2.txt
python <repo>/tools/fit/evidence.py     # -> evidence.txt
python <repo>/tools/fit/cv.py picks     # -> cv_picks.pkl, cv_picks.txt (also on stdout); also: three, sens
python <repo>/tools/fit/pairs.py        # -> pairs.txt (reads cv_picks.pkl)
python <repo>/tools/fit/drift.py        # -> drift_picks_three.txt, fit_all_<mode>.pkl
python <repo>/tools/fit/gradcheck.py    # optional: fitlib's gradient against finite differences
```

`<SKSE>` is `<Documents>/My Games/Skyrim.INI/SKSE`. `v3load.py` first; `pairs.py`
after `cv.py picks`. The rest only need `v3.pkl`.

### v3load.py

`python v3load.py <file> [<file> ...] [--char <id>] [--list]`

- **files, oldest first**: the rotated part(s), then the current file. Each is
  read on its own (every file starts with a head; a launch cut by the rotation
  starts again with a head in the next file). A torn line (the last one of a
  copy taken while the game writes) is skipped and counted (`bad`).
- `--list` prints every character and every launch with its list, build and
  decision counts by outcome, and writes nothing. Run it first to find the id.
- `--char <id>` keeps one character's decisions (`dec.char`) and its launches'
  contexts; the other characters never reach `v3.pkl`. The R6 fit is the mage,
  `FEFD1B039AC78799` (`3F3E2A8817962D59` is the earlier soak character).
- New need ids and record fields: needs, columns and cross-features are mapped
  by name, so a need a later build adds (`time_to_edge`, `edge_approach` in
  0.23.48) is a new column that reads 0 in contexts logged before it, and a
  record field the loader does not know is kept as written. Decision fields an
  older build did not write take the defaults in `DEC_DEFAULTS`.

### cv.py

`python cv.py [picks|three|sens] [--folds K] [--seed S] [--loo] [--spec TEXT ...] [--since V] [--until V] [--split-at V ...] [--split-needs all|NEED ...] [--laps once|all|none]`

- **Folds**: grouped K-fold by launch, `--folds 10` and `--seed 1` by default
  (`fitlib.launch_folds`): no launch is in two folds; the same seed gives the
  same folds; launches are dealt to the fold with the fewest key/wheel picks.
  `--loo` is the old leave-one-launch-out (~9 h on 288 launches).
- `--spec` keeps the specs whose name contains any of the texts. A smoke run:
  `cv.py picks --folds 2 --spec "A* fit (ln util)" "theta + b (doc 9)"`.
- **Build segmentation** (the head's `build`, `fitlib.version`): `--since` /
  `--until` keep the decisions logged by plugin versions in that range
  (inclusive). `--split-at 0.23.41` cuts them into segments: the yardstick is
  also reported per segment, and the `split` specs (only listed with
  `--split-at`) give the needs in `--split-needs` one column per segment
  (default: the needs whose sensor changed at 0.23.41, the target families,
  and the needs whose data changed at 0.23.51, `fitlib.BUILD_SPLIT_NEEDS`;
  `all` splits every need).
- **Laps** (0.23.51, the `three` group): an open need episode writes a
  `nothing` record for every unanswered stretch of 60 active seconds
  (`ep.why: "lap"`,
  [schema, Laps](../../docs/architecture/9-selection-log-v3.md#laps)), one per
  need open at the lap tick. The stretches one tick cuts do not all share a
  context: one begun by the previous lap (`ep.lap` > 0) carries that lap's
  context, an episode's first stretch (`ep.lap` 0) its onset's. They share the
  tick, which the records' `(launch, gen, utc)` names (`fitlib.lap_tick`; the
  game writes one utc per batch). `--laps once` (default) keeps one record per
  lap tick, so a slice weighs once however many needs were on (the lowest-seq
  one begun by a lap, whose context is the slice's start; else the lowest-seq
  one); `all` keeps every one; `none` drops them (the data a build before
  0.23.51 logged). `fitlib.build(laps=...)`; a decision's `meta` carries
  `ep_why`. `v3load.py --list` shows how many of the `nothing` records are
  laps. **Logs written before 0.23.51 have no laps and the old
  `loadout_two_handed`:** read [Reading logs written before
  0.23.51](#reading-logs-written-before-02351) before a `three` fit over them.
- **The yardstick**, for every spec against **A\*** (the old engine's ranking
  replayed from the logged utilities) on the same choice sets, with a
  bootstrap-over-launches 95% interval on model − A\*:
  `key/wheel hit@8`, `menu hit@8`, `all picks hit@8` (key + wheel + menu
  pooled), `page hit@1` and `page NLL` (key/wheel picks whose chosen row was on
  the page). A\*'s within-page probabilities are softmax(β · ln util), β fitted
  on the other folds' pages (`fitlib.page_temperature`): its page NLL is the old
  ranking at its best scale, not raw utilities read as logits.
- Specs added for R6: presence grading and presence + strength (doc 11 step 1),
  `recent_<family>` (doc 11 step 2: the families of the item picked last in the
  launch, `exp(-dt / 5 s)` and `exp(-dt / 30 s)` up to 30 s), and the old
  utility as a feature (`A* feature`), alone and with `recent`.
- Every result list in `cv_<group>.pkl` is aligned with its `meta`, one entry per
  decision; `yardstick` holds the tables, `fold_of` the folds.

### Tests

`python tools/fit/test_fit.py` (numpy; exit 0 = pass): the loader's multi-file
read, `--char` and `--list`, new need ids and unknown fields (and older records
without them), the build segmentation and `recent_<family>`, the folds (no launch
in two folds, deterministic), the A\* temperature and the pooled yardstick.

## The scripts

| Script | What it does |
|---|---|
| `v3load.py` | Reads the v3 file(s) into a compact pickle (several files in order, one character with `--char`, a listing with `--list`): columns, needs and cross-features mapped by name across heads (the 0.23.16, 0.23.19, 0.23.30 and 0.23.46 schema additions), the row columns past the tenth by the head's names (`fb`, `cr`; None on older rows), caps de-duplicated by content, contexts and decisions joined. Records the source path (for the v2 log). |
| `common.py` | Shared: the work directory, `load()` (v3.pkl with each decision's rows and chosen row), the row and flag constants, `v2_path()`. |
| `fitlib.py` | The dataset builder (with build segmentation and `recent_<family>`), doc 9's conditional logit (θ need × effect, per-item b, menu cost κ, optional outside option u0), the baselines and variants, an L-BFGS optimiser, the folds by launch, A\* and the yardstick. |
| `inspect_heads.py` | Every head of the file and what changed between consecutive ones. |
| `audit.py` | Counts per launch and build, context joins, duplicates, every need's range and firing rate, curve inputs (present, sentinel), effect columns ever used. |
| `sensors.py` | Sensor sanity: curve output against input, target family against race, flicker, darkness, step needs that stay on, items missing from the catalog. |
| `sensors2.py` | Follow-ups: curve changes between builds, target families against the player's own summons, the names of forms missing from the catalog. |
| `evidence.py` | Per need: picks with the need active, and how often the chosen item answers it (an obvious pair). |
| `cv.py` | Cross-validation by launch (grouped 10-fold; leave-one-out with `--loo`) of the model and its baselines (`picks`, `three` outcomes, `sens`itivity), and the yardstick against A\*. |
| `pairs.py` | Paired differences (model − baseline) with launch-bootstrap intervals, from `cv_picks.pkl`. |
| `drift.py` | Fit on every launch: θ's drift from θ0 with its information, the u0 need weights, evidence per obvious pair. |
| `gradcheck.py` | fitlib's analytic gradient against finite differences, and one full loss evaluation's time. |
| `test_fit.py` | The tests above. |

## Reading logs written by 0.23.46 and later

- Rows have two more columns, named by the head's `row` list: `fb` (`filtered_by`, the candidate
  filter that dropped a row that is not `eligible`, by name from the head's `filters`) and `cr`
  (the remaining seconds behind `covered`, -1 = no timer). `v3load.py` puts them at `FB` and `CR`
  (`common.py`) as a name and a float; rows written before 0.23.46 read `None` for both. Re-run
  `v3load.py` after updating: an older `v3.pkl` has 10-field rows.
- The cross-features end with `covered` (0..1, mapped by name like the others): how much of the
  item's effect is already up on the player, with doc 11's refresh prior applied
  (`covered / left(cr)` undoes it; `core/Coverage.h`). Absent from older rows (read 0).
  It is not an effect column, so `fitlib.build` keeps it out of `X`/`xcol` and returns it as
  `COV` (decisions x rows); no model term reads it yet.
- Contexts carry `summon` (`n`, `limit`, `act`): the summon count, the perk limit, and every
  commanded actor with its source spell, kind (summon / reanimate / other) and timer. `None` in
  `v3load.py`'s context dict before 0.23.46.
- Behaviour change in the same build: the summon filter compares the summon count with the limit
  (the Twin Souls bug), so with a limit above 1 summon spells stay eligible until the limit is
  reached. Segment by build before pooling summon eligibility across 0.23.46.
- `filtered_by` is the shadow fit's input (doc 11 step 6): the active-buff reasons (`invisibility`,
  `muffle`, `armor`, `summon_limit`, `warming`, `waterbreathing`, `resist`) are the covered drops.

## Reading logs written by 0.23.41 and later

- The head carries `curves` (the `[Needs]` curve of every need); `tools/replay/replay.py`'s
  `head_curves(head)` reads it. Older heads used their build's `needs.csv` defaults.
- `enemy_count` and `surrounded` take raw counts from 0.23.41 (were `/ 6` and `/ 3`),
  with linear curves; `boss_fight` reads 0 for the sensor's NEVER. Segment by build
  (or by the head's curves) before pooling inputs across 0.23.41.
- Contexts carry `env` (interior, game hour, open daylight, raw light) for the darkness need.
- Dead hostiles and the player's own summons stop counting in 0.23.41, so `enemy_count`,
  `surrounded`, the target families and `boss_fight` shift between builds.
- Three needs change meaning at 0.23.41 with the same input names: `follower_present` counts
  the player's own summons and thralls (it read teammates only), `regen_suppressed` drops
  zero-magnitude placeholder effects, and `darkness` reads the held light (`core/LightHold.h`:
  5 s darker / 3 s lighter) instead of each tick's reading.
- So segment by build (the head's `build`) before pooling any of these across 0.23.41.

## Reading logs written before 0.23.51

Every log that existed when 0.23.51 shipped (the R6 rehearsal's and the mage's,
2026-10-15..28) was written before it, and still carries both defects 0.23.51
fixed ([schema, Laps](../../docs/architecture/9-selection-log-v3.md#laps);
`core/WeaponLoadout.h`):

- **No laps.** A persistent need (`hunger`, `thirst`, `diseased`, `cold`,
  `warmth_deficit`, `fatigue`) wrote no `nothing` record: 0 of the mage's 13,961.
  In a `three` fit over old data they are on at picks and never at `nothing`,
  so they read as predicting a pick. `--laps` cannot help: there are no lap
  records to keep.
- **The quarterstaff read as two-handed.** `loadout_two_handed` is on whenever
  the Staff of Wandering Stars is in hand (in 3,991 of the 4,706 old contexts
  with it on, the equipped weapon row is FECCC01D or FECCF01D), and
  `loadout_quarterstaff` reads 0 (it did not exist). v3load does not backfill
  it: the log holds no keywords, the staff's cap reads `weapon_type_warhammer`
  like any warhammer's, and a light plugin's FormID moves with the load order.

So for the `three` group:

- **`--since 0.23.51`** when there is enough new data: no bias, nothing to undo.
- **Or `--split-at 0.23.51`** with the `split` specs. `fitlib.BUILD_SPLIT_NEEDS`
  (the default `--split-needs`) holds these seven needs (`fitlib.LAP_SPLIT_NEEDS`), so
  each gets one column for the old segment, which absorbs the old data's bias, and
  one for 0.23.51 and later, which is the one to read. Add `0.23.41` to the cuts
  (`--split-at 0.23.41 0.23.51`) when the data reaches back before it.
- Not a plain fit over all the files: most of the bias stays.

The `picks` group reads no `nothing` record; there only `loadout_two_handed` is
affected, and the same split applies.
