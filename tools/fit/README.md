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

The v3 file is ~60 MB after 16 hours of play and `v3.pkl` about the same;
`v3load.py` takes a minute, the CV runs take tens of minutes. Run them at low
priority while the game is up (PowerShell: start the process, then set its
`PriorityClass` to `Idle`).

## Run order

```sh
cd <scratch>
python <repo>/tools/fit/v3load.py "<Documents>/My Games/Skyrim.INI/SKSE/Huginn_Selections_v3.jsonl"
python <repo>/tools/fit/inspect_heads.py "<same v3 file>"   # optional: what changed between heads
python <repo>/tools/fit/audit.py        # -> audit.txt
python <repo>/tools/fit/sensors.py      # -> sensors.txt
python <repo>/tools/fit/sensors2.py     # -> sensors2.txt
python <repo>/tools/fit/evidence.py     # -> evidence.txt
python <repo>/tools/fit/cv.py picks     # -> cv_picks.pkl (and a table on stdout); also: three, sens
python <repo>/tools/fit/pairs.py        # -> pairs.txt (reads cv_picks.pkl)
python <repo>/tools/fit/drift.py        # -> drift_picks_three.txt, fit_all_<mode>.pkl
python <repo>/tools/fit/gradcheck.py    # optional: fitlib's gradient against finite differences
```

`v3load.py` first; `pairs.py` after `cv.py picks`. The rest only need `v3.pkl`.

## The scripts

| Script | What it does |
|---|---|
| `v3load.py` | Reads the v3 file into a compact pickle: columns, needs and cross-features mapped by name across heads (the 0.23.16, 0.23.19, 0.23.30 and 0.23.46 schema additions), the row columns past the tenth by the head's names (`fb`, `cr`; None on older rows), caps de-duplicated by content, contexts and decisions joined. Records the source path (for the v2 log). |
| `common.py` | Shared: the work directory, `load()` (v3.pkl with each decision's rows and chosen row), the row and flag constants, `v2_path()`. |
| `fitlib.py` | The dataset builder, doc 9's conditional logit (θ need × effect, per-item b, menu cost κ, optional outside option u0), the baselines, an L-BFGS optimiser and the metrics. |
| `inspect_heads.py` | Every head of the file and what changed between consecutive ones. |
| `audit.py` | Counts per launch and build, context joins, duplicates, every need's range and firing rate, curve inputs (present, sentinel), effect columns ever used. |
| `sensors.py` | Sensor sanity: curve output against input, target family against race, flicker, darkness, step needs that stay on, items missing from the catalog. |
| `sensors2.py` | Follow-ups: curve changes between builds, target families against the player's own summons, the names of forms missing from the catalog. |
| `evidence.py` | Per need: picks with the need active, and how often the chosen item answers it (an obvious pair). |
| `cv.py` | Leave-one-launch-out cross-validation of the model and its baselines (`picks`, `three` outcomes, `sens`itivity). |
| `pairs.py` | Paired differences (model − baseline) with launch-bootstrap intervals, from `cv_picks.pkl`. |
| `drift.py` | Fit on every launch: θ's drift from θ0 with its information, the u0 need weights, evidence per obvious pair. |
| `gradcheck.py` | fitlib's analytic gradient against finite differences, and one full loss evaluation's time. |

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
