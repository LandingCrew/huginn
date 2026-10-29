# The R8 bootstrap file (format version 1)

The offline fit (roadmap R6, `tools/fit`) hands its result to the new learner and
scorer (R8) as one JSON file: θ (needs × effect columns), u0 per need, κ, the
`recent_<family>` weights, the extra feature weights, the per-item b, and the
fitted response curves. This page is the format: what each field means, how a
score is computed from it, and the rules a reader applies. The writer is
`tools/fit/export.py` (fit and keying) with `tools/fit/bootstrap.py` (document,
text and validation); the model it describes is doc 9's
([9-context-as-learner-input.md](9-context-as-learner-input.md)) as
`tools/fit/fitlib.py` implements it.

Golden examples for R8's reader tests, written by the same code and checked by
`python tools/fit/test_fit.py`:

- [`tests/core/fixtures/bootstrap/bootstrap_v1_full.json`](../../tests/core/fixtures/bootstrap/bootstrap_v1_full.json):
  every field filled (presence + strength grading, three outcomes, `recent`,
  features, items including a light plugin, a curve).
- [`tests/core/fixtures/bootstrap/bootstrap_v1_minimal.json`](../../tests/core/fixtures/bootstrap/bootstrap_v1_minimal.json):
  the optional fields null or empty (graded, picks only).

## Versioning

- `format` is `"huginn.bootstrap"` and `version` is `1`. A reader rejects any
  other format or version: a version-2 file is not read as version 1.
- A writer may **add** fields (top level, or inside an object) without a
  version bump; a reader ignores fields it does not know.
- Removing a field, changing a field's meaning, unit or shape, or changing the
  score formula below is **version 2**.
- Needs and effect columns are matched **by name**, never by position, so a
  file stays readable when `needs.csv` or `effects.csv` gain rows (see Names).

## Text

UTF-8 JSON. Numbers are finite (no NaN or infinity), written at 6
significant digits; integers where the field says so. Objects are written one
key per line and number arrays on one line, in a fixed field order, so two
exports of the same fit are byte-identical apart from `created_utc`. On the
mage's log (76 launches, 1,865 picks, 94 needs × 243 columns, 459 items) the
file is about 650 KiB.

## Fields

| Field | Type | Meaning |
|---|---|---|
| `format` | string | `"huginn.bootstrap"` |
| `version` | int | `1` |
| `created_utc` | string | when the file was written, `YYYY-MM-DD HH:MM:SS` UTC (optional) |
| `source` | object | provenance, informational: `char` (the character id, 16 hex digits; null if several), `launches`, `first_launch`, `last_launch`, `decisions`, `picks`, `builds` ([oldest, newest] plugin build of the fitted decisions), `forms` (the form map's file name), `tool` |
| `model` | object | `spec` (the cv.py spec name fitted), `outcomes` (`"picks"`: key/wheel/menu picks; `"three"`: also the `nothing` outcome, so `u0` is present), `grading` (`"graded"`, `"presence"` or `"presence+strength"`: how an item's cap is read, below) |
| `needs` | string[K] | the need names of `theta`'s rows (`needs.csv` ids) |
| `cols` | string[J] | the effect column names of `theta`'s columns (`effects.csv` ids) |
| `theta` | number[K][J] | θ, need-major: `theta[k][j]` weighs need k against column j |
| `theta_precision` | number[K][J] | the diagonal precision of each θ weight (below) |
| `theta_strength` | number[K][J] or null | the strength block, only and always with `presence+strength` |
| `theta_strength_precision` | number[K][J] or null | its precision, with `theta_strength` |
| `theta_bias` | number[J] or null | a constant need's row (a bias-need spec); null otherwise |
| `kappa` | number | κ, the menu cost: subtracted from the score of every candidate off the page |
| `u0` | object or null | the outside option, only and always with `outcomes` `"three"`: `bias` (number) and `need` ({need name: weight}) |
| `recent` | object or null | the `recent_<family>` features (below): `window_s`, `taus_s` (number[]), `features` |
| `features` | object[] | extra row features, `{name, w}`; names from the fixed list below |
| `items` | object[] | the per-item b: `{plugin, local, name, b, precision, picks}`, keyed by the plugin that owns the FormID's load-order index (below) |
| `items_dropped` | object | {reason: count} of the fitted items not written (below); informational |
| `curves` | object | {need name: `["logistic", centre, slope]`}: response curves fitted by `tools/fit/curve.py` |

## The score

For a decision d (a context) and a candidate item i, with n(d) the need vector
(as the build's `[Needs]` curves compute it, but see `curves`):

```
s_i(d) = sum_k n_k(d) * T_k(i)                       needs x effect columns
       + sum_f r_f(d) * T_f(i)                       recent_<family>, if present
       + sum_j theta_bias[j] * x_ij                  if theta_bias is not null
       + b_i                                         items (0 for an item not in the file)
       + sum_f w_f * feature_f(i, d)                 features
logit_i(d) = s_i(d) - kappa * [i is off the page]    the menu: held, not shown
u0(d)      = u0.bias + sum_k u0.need[k] * n_k(d) + sum_f recent.features[f].u0 * r_f(d)
P(i | d)   = exp(logit_i) / (exp(u0) + sum_l exp(logit_l))      (u0 = -inf with outcomes "picks")
```

`T_k(i)` is the item's term for need k; how it reads the item's cap depends on
`model.grading`. With `cap_j(i)` the item's graded value in column j (the
effect catalog's cap, `core/EffectMapper.h`; 0 where the item has none) and
`x_j(i)` the runtime cross-feature that names column j, if any
(`effect/CrossFeatures.h`; 0 otherwise):

| `grading` | `T_k(i)` |
|---|---|
| `graded` | `sum_j theta[k][j] * (cap_j(i) + x_j(i))` |
| `presence` | `sum_j theta[k][j] * (1[cap_j(i) > 0] + x_j(i))` |
| `presence+strength` | `sum_j theta[k][j] * (1[cap_j(i) > 0] + x_j(i)) + theta_strength[k][j] * (cap_j(i) - 0.5) * 1[cap_j(i) > 0]` |

A cross-feature adds to the `theta` block only, never to `theta_strength`.
Cross-features that name no effect column (`covered`, 0.23.46) are in no term:
the fit does not read them yet.

`theta_bias` reads the item the same way, with the bias need fixed at 1
(`T_bias(i)` with `theta_bias` as its row). The writer refuses a bias need with
`presence+strength` (its strength row has no field), so `theta_bias` comes only
with `graded` or `presence`.

### `recent`

`r_f(d)` for each entry f of `recent.features` (`family` names an effect
column, `tau_s` one of `taus_s`; `fitlib.recent_families`, doc 11 step 2):

- **Times.** A pick's time is the moment it was decided: its `utc` less its
  `confirmMs` (the confirmation delay; 0 when absent), `fitlib.pick_time`. The
  decision d is timed the same way (a `nothing` record by its context's
  `utc`).
- **The previous pick.** The latest pick strictly before d's time in the same
  launch and load generation (`gen`), d itself excluded. Only usable picks
  count: an outcome other than `nothing`, with a context, with a chosen row
  (the log's `row` names one; a chosen item without a cap still counts, and
  then gives 0), not of a transient spell (LoreRim's Arcane
  Anchor), and not from the SimonRim test list (`list` `simonrim-essentails`).
  A live reader that filters otherwise computes other values than the ones
  each entry's `theta` was fitted on.
- **The value.** With `age` = d's time less that pick's time: if `age <=
  window_s` and the picked item's cap has `cap_family > 0`, then
  `r_f(d) = exp(-age / tau_s)`, else 0.
 Each entry carries its own row:
`theta` (number[J]), `theta_strength` (with `presence+strength`), `precision`,
and `u0` (with outcomes `"three"`). `T_f(i)` reads the item as `T_k(i)` does.
Only the families seen in the fitted log are listed; a family absent from the
file contributes 0.

### `features`

| `name` | `feature_f(i, d)` |
|---|---|
| `ln util` | the old engine's utility of i in d: `ln(max(utility, 1e-6))`, 0 when i has none |
| `util missing` | 1 when the old engine gave i no utility in d, else 0 |
| `slot 0` .. `slot 7` | 1 when i is shown in that slot of the page in d, else 0 (a position term: it describes a page that exists; it cannot build one) |

`ln util` and `util missing` read the old engine's ranking, the hand-tuned layer
R8 and R11 retire (roadmap R6, "Open for R8"); a file without them fits the
model on its own.

### `curves`

`curves[need] = ["logistic", centre, slope]` is that need's response curve,
`1 / (1 + exp(-slope * (x - centre)))` (`core/ResponseCurve.h`: kind
`logistic`, p1 = centre, p2 = slope). θ in the same file was fitted with the
need read through it (`export.py` re-reads the logged input), so a reader that
uses this θ uses this curve for that need (in place of the `[Needs]` line, or
by writing it there). Today: `encumbrance`, fitted from the player's
carry-weight picks from a documented prior on its centre near 0.9 (roadmap R6;
doc 11 section 3, decision 1).

## Precision

`theta_precision[k][j]` (and the strength, recent and item precisions) is the
diagonal of the posterior's curvature at the fit: the weight's Fisher
information over the fitted decisions (doc 9's P4, `fitlib.Model.info`:
`sum_d n_k(d)^2 Var_p(x_kj)`) plus the prior's precision `1 / sigma^2`
(`sigma_obv = 1` for an obvious pair, `sigma_dense = 0.3` otherwise, so a weight
no decision informs reads `1 / 0.09 = 11.1111`). An item's precision is
`sum P (1 - P)` over its choice-set rows plus `1 / sigma_b^2` (`sigma_b = 0.5`:
4). It seeds the learner's uncertainty: the cosave's `THTA` keeps {μ, σ²} per
weight ([9-implementation-map.md](9-implementation-map.md), Cosave), so
μ = θ and σ² = 1 / precision.

## Items

An item is keyed by the plugin that **owns its FormID's load-order index**
and its **local FormID** within that plugin, so the key survives a load-order
change. A runtime FormID is the owner's index in its high bits (the top byte
for a full plugin, `FE` plus a 12-bit light index for a light one) and the
local id in its low bits; CommonLib's `TESDataHandler::LookupForm(local,
plugin)` rebuilds it from the named plugin's own index:

- `plugin`: the index owner's file name as the game reports it; compare
  without case.
- `local`: 6 upper-case hex digits: the low 24 bits of the runtime FormID for a
  full plugin, the low 12 bits for a light one (`FE xxx yyy`: `"000yyy"`).
  CommonLib resolves it with `TESDataHandler::LookupForm(local, plugin)`; a
  null result (the plugin or the record is gone) means the item is skipped.

The owner is **not always the defining plugin** (`TESForm::GetFile(0)`, the
first file that has the record). A plugin can create a record inside a
master's FormID space (an injected record): its FormID carries the master's
index, while `GetFile(0)` names the plugin that created it. LoreRim's dump has
one: `FE97B053` "Drunkness Monitor" is defined by `BOOB - Requiem.esp` at
`BOOB.esp`'s index `FE97B` (`BOOB - Requiem.esp` owns `FE97C`); keyed by
`GetFile(0)`, `LookupForm(0x053, "BOOB - Requiem.esp")` would give `FE97C053`,
another or no record. It is keyed `{"plugin": "BOOB.esp", "local": "000053"}`.
A writer that reads `GetFile(0)` (as `hg dump all` does) must map each index to
its owner; a writer inside the game can take the owner straight from the load
order (`TESDataHandler`'s file at that index).
- `name`: the item's name in the form map, for reading only.
- `b`: the per-item term. It is this character's: it may not transfer to a new
  character (θ is meant to). Whether R8 applies b from another character is
  R8's decision.
- `precision`: as above. `picks`: how often the fitted log picked it.

The writer (`tools/fit/export.py`) reads the runtime FormID → defining plugin
map from `hg dump all`'s CSV (`Huginn_All.csv`, one row per effect: `kind`,
`formID`, `plugin` = `GetFile(0)`, `name`) and infers each index's owner from it
(`bootstrap.index_owners`): every plugin owns one index and appears at its own
index and at the indices it injects into, so an index goes to the one plugin
seen there that can own no other (every other index it appears at is owned
already), repeated until nothing changes. On LoreRim's dump every one of the
658 indices resolves, and the one injected record above is keyed by `BOOB.esp`.
One case the dump cannot show: a plugin whose dumped forms are all injected
into a master that has no dumped form of its own is taken as that index's
owner. Not written, and counted in `items_dropped` by reason:

- `dynamic (FF)`: a form made in game (player potions, enchanted gear): it has
  no plugin and its id is meaningful only within one load;
- `not in the form map`: the dump lacks the form (a dump older than the log, or
  a load order that changed);
- `index owner unknown`: two or more plugins could own the form's index (each
  is seen only there), so the key would be a guess; the writer prints a
  WARNING naming them;
- `kind differs from the form map`, `name differs from the form map`: the dump
  names another record at that FormID than the log did (a load order change;
  light plugins shift when a list update adds or removes one).

These checks drop every wrong key **they can see**, not every wrong key. The
kind is checked for every item, the name only for picked items (the log names
only what was picked). So after a load order change, an item never picked whose
kind matches the other record's by chance is keyed to that record, and the one
case `index_owners` cannot see (above) keys an index to the wrong plugin. On
the mage's log the load order did change mid-log (the 14 forms the 2026-10-10
dump lacks are all light forms first seen from launch 20261017 on), yet every
one of the 24 picked light items matches the dump by name. A reader should
therefore cross-check what `LookupForm` returns: its form type against the
item's kind, and its name against `name` (which the file carries), and skip an
item that disagrees.

## Names

`needs` and `cols` name `theta`'s rows and columns. A reader maps each name to
its build's index:

- a name the build does not know is skipped (its row or column contributes
  nothing);
- a need or column the build has and the file lacks takes its prior: θ0 =
  `+T0` (1) for an obvious pair of `effects.csv` (`-T0` for
  `combat_regen_ok × kind_food`, 0 for the `overshoot_*` pairs), 0 otherwise,
  with the prior's precision (1 for an obvious pair, 11.1111 otherwise);
- the same applies to `u0.need`, `recent.features[].family` and `curves`.

## Reader rules

What `bootstrap.validate` checks, and what a version-1 reader rejects:

1. `format` and `version` as above.
2. `model.grading` and `model.outcomes` from the lists above.
3. `needs` and `cols`: non-empty strings, no duplicates.
4. `theta` and `theta_precision`: K rows of J finite numbers, every
   precision above 0 (σ² = 1 / precision).
   `theta_strength` and `theta_strength_precision`: the same shape (precisions
   above 0), present exactly when `grading` is `presence+strength`. `theta_bias`: null or J
   numbers, and null with `presence+strength`.
5. `kappa`: a number.
6. `u0`: present (`bias`, and `need` with names from `needs`) exactly when
   `outcomes` is `three`; null otherwise.
7. `recent`: null, or `window_s` and `taus_s`, and each feature's `family` in
   `cols`, `tau_s` in `taus_s`, `theta` and `precision` (and `theta_strength`
   with `presence+strength`) of J numbers, every precision above 0, `u0`
   exactly with `three`.
8. `features[].name` from the list above, `w` a number.
9. `items[]`: `plugin` a non-empty string, `local` 6 upper-case hex digits,
   `b` and `precision` numbers, `precision` above 0, no two items with the
   same plugin (ignoring case) and local.
10. `curves`: needs from `needs`, each `["logistic", centre, slope]` with a
    slope other than 0.

## Producing one

```sh
cd <scratch work directory>                       # tools/fit/README.md: where files go
python <repo>/tools/fit/v3load.py <SKSE>/Huginn_Selections_v3-*-1.jsonl <SKSE>/Huginn_Selections_v3.jsonl --char <id>
python <repo>/tools/fit/curve.py                  # -> curve_encumbrance.json (optional)
python <repo>/tools/fit/export.py --spec "theta + b + recent_<family>" --curve curve_encumbrance.json \
       --forms <SKSE>/Huginn_All.csv              # -> bootstrap.json
```

`hg dump all` in game writes `Huginn_All.csv`; run it on the load order the log
was written with. The spec is the one R6's yardstick chose (`honest.py`); the
default is `theta + b + recent_<family>`.
