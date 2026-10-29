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

The v3 file rotates at 64 MiB (`Huginn_Selections_v3-<launch>-1.jsonl` is an
older part; give every part, oldest first: on 2026-10-28 there are two,
`-20261015-180003-1` and `-20261029-022335-1`); `v3.pkl` is ~120 MB. `v3load.py` takes under a
minute; a 10-fold `cv.py picks` takes about half an hour, `three` a few hours.
Run them at low priority while the game is up (PowerShell: start the process,
then set its `PriorityClass` to `Idle`), and without `python -I`: the scripts
import their siblings (`common`, `fitlib`) by name.

## Run order

```sh
cd <scratch>
python <repo>/tools/fit/v3load.py <SKSE>/Huginn_Selections_v3-20261015-180003-1.jsonl <SKSE>/Huginn_Selections_v3-20261029-022335-1.jsonl <SKSE>/Huginn_Selections_v3.jsonl --list
python <repo>/tools/fit/v3load.py <same files> --char FEFD1B039AC78799
python <repo>/tools/fit/inspect_heads.py "<same v3 file>"   # optional: what changed between heads
python <repo>/tools/fit/audit.py        # -> audit.txt
python <repo>/tools/fit/sensors.py      # -> sensors.txt
python <repo>/tools/fit/sensors2.py     # -> sensors2.txt
python <repo>/tools/fit/evidence.py     # -> evidence.txt
python <repo>/tools/fit/cv.py picks     # -> cv_picks.pkl, cv_picks.txt (also on stdout); also: three, sens
python <repo>/tools/fit/pairs.py        # -> pairs.txt (reads cv_picks.pkl)
python <repo>/tools/fit/drift.py        # -> drift_picks_three.txt, fit_all_<mode>.pkl
python <repo>/tools/fit/gradcheck.py    # optional: fitlib's gradient against finite differences
python <repo>/tools/fit/honest.py       # -> honest_picks.txt/.pkl: online (train-on-past) beside CV, the R6 gate
python <repo>/tools/fit/curve.py        # -> curve_encumbrance.txt/.json: the encumbrance curve (R6)
python <repo>/tools/fit/export.py --curve curve_encumbrance.json --forms <SKSE>/Huginn_All.csv   # -> bootstrap.json (R8)
```

`<SKSE>` is `<Documents>/My Games/Skyrim.INI/SKSE`. `v3load.py` first; `pairs.py`
after `cv.py picks`; `export.py --curve` after `curve.py`. The rest only need
`v3.pkl` (`honest.py` reuses `cv_picks.pkl` when it was written for the same
decisions, folds and seed).

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
- `theta + recent + A* feature (no b)` (0.23.52): the rehearsal's best variant
  without the per-item b, which belongs to this character (the verifier of
  2026-10-28: b costs hits on items the training set never picked).

### honest.py -- the honest yardstick

`python honest.py [picks|three] [--every N] [--min-train N] [--spec TEXT ...] [--folds K] [--seed S] [--since V] [--until V] [--nboot N] [--maxiter N] [--no-cache]`

Grouped K-fold trains on launches after the held-out one too, so a held-out
pick of an item the player only took up later is fitted from its future (the
R6 verifier, 2026-10-28). `honest.py` adds what R8 would have scored live, a
**train-on-past online replay**, and reports it beside the CV on the same
decisions:

- **Schedule** (`fitlib.online_chunks`): decisions in log order (launch, then
  seq). A launch is tested once the earlier launches hold `--min-train` picks
  (default 150; never 0). Within it, every `--every` picks (default 5) the
  model is refitted on every earlier launch plus the launch's earlier
  decisions, warm-started from the previous refit, and scores the next
  `--every` picks. A\*'s page temperature is fitted on each chunk's past too;
  its ranking (so its hit@8) does not depend on the schedule.
- **Reported**, per spec and for A\*: key/wheel hit@8, menu hit@8, all picks
  pooled hit@8, page hit@1 and page NLL, each as the level with its
  bootstrap-over-launches interval and as model minus A\* with its interval,
  for `CV (all)` (cv.py's numbers), `CV (online)` (the same CV results on the
  decisions the online replay tested) and `online`; and **R6's gate** on each
  (`fitlib.gate`, roadmap R6: menu lower end > 0 and pooled upper end >= 0).
  The online verdict is the honest one; `CV (online)` against `online`
  separates the schedule from the decision set.
- **Specs**: the cv.py specs whose name contains a `--spec` text; by default
  `honest.HONEST_DEFAULT`: θ + b, θ + b + `recent_<family>`, θ + b + recent +
  A\* feature, and the same without b. Split and latest-caps specs are refused.
- **Cost**: one fit per chunk (about 380 on the mage's 1,865 picks at
  `--every 5`), 30 to 40 minutes per spec with numpy on 6 threads (the
  verifier's run: 1,780 to 2,360 s), longer on fewer. Each spec's online and
  CV results are cached in the work directory
  (`honest_cache_<kind>_<group>_<hash>.pkl`, keyed by `fitlib.data_key`: every
  array the fit reads, the obvious pairs and `fitlib.py`'s code; plus the
  spec, the schedule and `--maxiter`), so run one process per spec
  (`--spec`), then one naming them all to combine; a rebuilt `v3.pkl` or a
  `fitlib.py` change refits; `--no-cache` refits always. CV results come from
  `cv_<group>.pkl` only when cv.py wrote it with the same `data_key`, folds,
  seed and maxiter (cv.py fits at `CV_MAXITER`, 300; so `--maxiter 300`
  reuses it), and only for a spec whose definition there (`cv.spec_key`) is
  the current one: a spec redefined in cv.py under the same name refits. Specs that read the v2 log (`ctx`, `cheap`) are refused: honest.py
  does not load it.
- **Port of the verifier's scripts** (`r6-verify/online.py`, `fwd_launch.py`,
  `combine.py` in the 2026-10-28 session scratchpad): the same schedule (chunks
  of 5 picks, warm start, 150 picks before the first tested launch, launch
  bootstrap with 2,000 draws), with the A\* temperature fitted on the past
  instead of on the CV folds, and the yardstick's metrics. `--every` larger
  than any launch is the verifier's per-launch refit (`fwd_launch.py`).

### curve.py -- the encumbrance curve

`python curve.py [--need encumbrance] [--col utility_carry_weight] [--spec NAME] [--three] [--all-pairs] [--prior-centre 0.9] [--prior-centre-sd 0.05] [--prior-slope 40] [--prior-slope-sd 0.5] [--rounds 10] [--quick-profile] [--since V] [--until V]`

Roadmap R6 and doc 11 decision 1: the encumbrance curve is fitted from the
player's carry-weight picks, from a documented prior on its centre near 0.9.

- **Model**: the need's column reads `logistic(in; c, s)` of the logged input
  (`ctx.in`, now in `build()`'s `IN`), and (c, ln s) are fitted with doc 9's
  model: a coarse start over c, then coordinate ascent (model, then curve).
  Only the need's **obvious pairs** read the curve (encumbrance x
  `utility_carry_weight` and its `~strength` twin; `Model(theta_fixed=...)`
  holds the rest of the row at its prior centre). With every pair free
  (`--all-pairs`), a low centre turns the need into a "heavily loaded"
  indicator that its 243 dense weights fit to anything correlated with the load
  (the mage's log: the data's NLL fell monotonically to c 0.70).
- **The prior** (the user's answer, recorded as a statement, not derived):
  `c ~ Normal(0.9, 0.05^2)`; `ln s ~ Normal(ln 40, 0.5^2)` (the shipped slope).
  `--prior-*` override it for a test.
- **Spec**: default `theta + b, presence + strength` (doc 11 decision 5:
  Feather's carry-weight grade is 0.0046, which cancels the pair under the
  current grading).
- **Reported**: the carry-weight picks and their load ratios, the start scan,
  the fitted (c, s) with a Laplace interval, the pair's weights, and the
  profile over c with the model refitted at every point (`--quick-profile`:
  held at its fit, conditional), with a WARNING if its best centre is on the
  grid's edge. `curve_<need>.json` carries the curve, its interval, the prior
  and the `[Needs]` line (`encumbrance = logistic <c> <s>`).

### export.py -- the R8 bootstrap file

`python export.py [--spec NAME] [--three] [--forms Huginn_All.csv] [--curve JSON ...] [--out bootstrap.json] [--since V] [--until V] [--maxiter N]`

Fits one spec on every kept decision and writes the bootstrap file, format
version 1: [docs/architecture/9-bootstrap-format.md](../../docs/architecture/9-bootstrap-format.md)
(the fields, the score formula, the reader rules). `bootstrap.py` builds,
writes (deterministic text) and validates the document.

- θ (needs x effect columns) with its diagonal precision; θ's strength block
  for presence + strength; κ; u0 per need for a three-outcome spec (a bias
  need's u0 weight folded into `u0.bias`); the `recent_<family>` rows; the
  extra feature weights (`ln util`, `util missing`, `slot 0` .. `slot 7`);
  per-item b keyed by **the plugin that owns the FormID's load-order index +
  local FormID** (`LookupForm(local, plugin)` rebuilds it); the curves of
  `--curve` (a curve's need is re-read through it before the fit, so θ
  matches it).
- Items are keyed through `hg dump all`'s CSV (`--forms`; default
  `Huginn_All.csv` beside the v3 file). Its `plugin` is the defining plugin
  (`GetFile(0)`); `bootstrap.index_owners` maps each load-order index to its
  owner, so a record a plugin injects into a master's FormID space is keyed
  by the master (LoreRim's one case: `FE97B053`, `BOOB - Requiem.esp` in
  `BOOB.esp`). Dynamic (FF) forms, forms the dump lacks, forms at an index
  whose owner the dump cannot tell, and forms whose catalog kind (any item)
  or logged name (picked items) differs from the dump's are dropped and
  counted (`items_dropped`): run `hg dump all` on the load order the log was
  written with. That drops the wrong keys these checks can see, not all: a
  never-picked item whose kind matches by chance still gets one, so R8's
  reader cross-checks the resolved form's kind and name (the format doc).
- Refused: split and latest-caps specs (diagnostics), specs without θ (the
  baselines), the `ctx` and `cheap` features (they read the v2 log), a bias
  need with presence + strength.
- `source.builds` is [oldest, newest] by version, two builds of one version
  ordered by when they were logged.
- The default spec is `theta + b + recent_<family>`; pass the one R6's
  yardstick chose.

### Tests

`python tools/fit/test_fit.py` (numpy; exit 0 = pass): the loader's multi-file
read, `--char` and `--list`, new need ids and unknown fields (and older records
without them), the build segmentation and `recent_<family>`, the folds (no launch
in two folds, deterministic), the A\* temperature and the pooled yardstick; and
(0.23.52) the online schedule (each test chunk trained on exactly its past,
chunks of N picks inside one launch, no fit on no picks), no future leak (the
online results and A\*'s page temperature before the last launch do not change
when its choices do), `cv_results` against the former fold loop, R6's gate, the
curve (the plugin's logistic, the pair-only mask held exactly, a known centre
recovered, a tight prior held), the bootstrap format (the golden files are the
writer's output and validate; the reader rules reject broken documents; item
keys for full, light and dynamic forms, keyed by the index owner for an
injected record, an index of unknown owner not keyed; the form map's CSV), the
export (θ, b and κ round-trip; kind and name mismatches dropped; three
outcomes, recent and the strength block; the file read back as JSON and scored
by an independent reader that follows the format doc, cross-features and slot
features included, against `Model.scores` for graded + recent + features,
presence + strength with three outcomes, and a bias need with three outcomes;
the refusals; `source.builds`; a log end to end), `fitlib.data_key`, and
`honest.py` and `curve.py` end to end on a tiny log (the report; the caches
reused, and refitted for another `--maxiter` or a rebuilt `v3.pkl`; cv.py's
pickle reused only at the same key and maxiter; the curve file fed to
export.py). `--write-golden` rewrites
`tests/core/fixtures/bootstrap/` after a deliberate format change.

## The scripts

| Script | What it does |
|---|---|
| `v3load.py` | Reads the v3 file(s) into a compact pickle (several files in order, one character with `--char`, a listing with `--list`): columns, needs and cross-features mapped by name across heads (the 0.23.16, 0.23.19, 0.23.30 and 0.23.46 schema additions), the row columns past the tenth by the head's names (`fb`, `cr`; None on older rows), caps de-duplicated by content, contexts and decisions joined. Records the source path (for the v2 log). |
| `common.py` | Shared: the work directory, `load()` (v3.pkl with each decision's rows and chosen row), the row and flag constants, `v2_path()`. |
| `fitlib.py` | The dataset builder (with build segmentation, `recent_<family>` and the logged curve inputs `IN`), doc 9's conditional logit (θ need × effect, per-item b, menu cost κ, optional outside option u0, optional held θ weights), the baselines and variants, an L-BFGS optimiser, the folds by launch and the online schedule, A\* and the yardstick, R6's gate. |
| `inspect_heads.py` | Every head of the file and what changed between consecutive ones. |
| `audit.py` | Counts per launch and build, context joins, duplicates, every need's range and firing rate, curve inputs (present, sentinel), effect columns ever used. |
| `sensors.py` | Sensor sanity: curve output against input, target family against race, flicker, darkness, step needs that stay on, items missing from the catalog. |
| `sensors2.py` | Follow-ups: curve changes between builds, target families against the player's own summons, the names of forms missing from the catalog. |
| `evidence.py` | Per need: picks with the need active, and how often the chosen item answers it (an obvious pair). |
| `cv.py` | Cross-validation by launch (grouped 10-fold; leave-one-out with `--loo`) of the model and its baselines (`picks`, `three` outcomes, `sens`itivity), and the yardstick against A\*. |
| `pairs.py` | Paired differences (model − baseline) with launch-bootstrap intervals, from `cv_picks.pkl`. |
| `drift.py` | Fit on every launch: θ's drift from θ0 with its information, the u0 need weights, evidence per obvious pair. |
| `gradcheck.py` | fitlib's analytic gradient against finite differences, and one full loss evaluation's time. |
| `honest.py` | The honest yardstick: the online (train-on-past) replay beside grouped CV on the same decisions, the yardstick for each, R6's gate. |
| `curve.py` | The encumbrance curve fit from the carry-weight picks, from the documented prior; writes the `[Needs]` line and `curve_<need>.json`. |
| `export.py` | Fits a spec on every decision and writes the R8 bootstrap file (θ, u0, κ, recent, features, b by plugin + local FormID, curves). |
| `bootstrap.py` | The bootstrap format, version 1: document, deterministic text, validation, item keys. No numpy. |
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
  `warmth_deficit`, `fatigue`) wrote no `nothing` record for its own episodes:
  0 of the mage's 14,014 (all three files to 2026-10-29). They still appear in
  `nothing` contexts: u0 = θ_out · n reads the whole need vector of the
  record's context (`Model.scores`), and that context is the onset of another
  need's episode, with whatever persistent need is on then. What old data lacks
  is a sample of the time a persistent need stays on: its `nothing` records
  sit where other needs start. The bias that leaves is mild and mixed. On at
  >= 0.5, at picks / at `nothing` (1,908 / 14,014): hunger 8.8% / 7.3%, thirst
  6.7% / 5.0%, diseased 11.8% / 10.5%, warmth_deficit 7.8% / 5.5%, fatigue
  0.5% / 0.7%, and cold the other way, 0.2% / 2.2%. `--laps` cannot help: there
  are no lap records to keep.
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
  each gets one column for the old segment, which takes the old sampling (and the
  old quarterstaff reading), and one for 0.23.51 and later, which is the one to
  read. Add `0.23.41` to the cuts (`--split-at 0.23.41 0.23.51`) when the data
  reaches back before it.
- Not a plain fit over all the files. For `loadout_two_handed` the old
  quarterstaff reading is a real error, and in a pooled fit its weight is the
  old data's share (~45 h against whatever is new). For the persistent needs
  the old bias is small (the rates above differ by 1-2 points), but a pooled
  column mixes two ways of sampling `nothing` and cannot say which one it
  learned; the split costs one column per need and shows how far the two
  segments differ.

The `picks` group reads no `nothing` record; there only `loadout_two_handed` is
affected, and the same split applies.
