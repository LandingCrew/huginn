# Selection log v3: the decision log the fit reads

**Status:** built in R4 (0.23.15). Logging only: nothing that scores, generates
candidates, allocates slots, trains the frozen learner or counts the soak
telemetry reads it. The v2 log (`Huginn_Selections.jsonl`) is written as before.

The offline fit (R6, [implementation map Phase 4](9-implementation-map.md#phase-4-fit-offline-toolsreplay))
and later the new learner (R8) need, for every decision the player made: the
situation (the need vector), every item they could have chosen with what it
does (sparse cap(i) and the runtime cross-features), the page they saw, and an
explicit outcome among doc 9's three -- a key press from the page, a pick from
the held items off the page at a menu cost κ, or nothing
([doc 9](9-context-as-learner-input.md), theory page P11). This file is the
schema of that log, field by field.

| | |
|---|---|
| Code | `src/core/DecisionLog.h/.cpp` (records, encoder), `src/core/NeedEpisodes.h` (episodes), `src/learning/SelectionLogV3*.cpp` (the game side) |
| Reader | `tools/replay/replay.py`: `iter_v3` / `load_v3`, `--v3 FILE...` prints a summary |
| Tests | `tests/core/DecisionLogTests.cpp` (encoder vs the golden file), `tests/core/NeedEpisodesTests.cpp`, `tools/replay/test_replay_v3.py` (CTest `replay_v3_roundtrip`) |
| Golden file | `tests/core/fixtures/decisions/synthetic_v3.jsonl` |

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
| `needs` | string[92] | needs of `9-data/needs.csv`, in order (`ctx.need`, `ctx.in`, `dec.open`, `dec.ep.i` index it) |
| `cross` | string[7] | the runtime cross-features, in order (`row.x` indexes it) |
| `kinds` | string[10] | catalog kinds: `Spell Scroll Potion Poison Food Weapon Ammo Armor SoulGem Light` (`row.kind`) |
| `src` | string[10] | the old engine's candidate source types (`row.src`) |
| `row` | string[10] | the row columns, in order (below) |
| `flags` | object | row flag bits by name |
| `episode` | object | the episode definition in force: `onset`, `expiry`, `minSec`, `graceSec`, `answerSlackSec`, and `deathDrops: 1` (0.23.16: a death drops the open episodes and those in their grace; absent = 0.23.15, where a death could leave `nothing` records) |

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
(`"onset"`). Several decisions can share one: the picks of one menu visit (the
Plackett–Luce co-picks), the "nothing" records of needs that started on the
same tick.

| Field | Type | Meaning |
|---|---|---|
| `id` | int | unique within the launch |
| `utc` | string | when taken, `YYYY-MM-DD HH:MM:SS.mmm` UTC |
| `why` | string | `press`, `menu`, `onset` |
| `menu` | string | the menu's name, for `why: "menu"` (`InventoryMenu`, `MagicMenu`, `FavoritesMenu`) |
| `need` | [[i, v]] | the need vector: non-zero curve outputs, 0..1 (`needs/NeedMonitor`, R3) |
| `in` | [[i, v]] | what went into each curve, in the units of `needs.csv`'s `r3_input` (fractions, seconds, units of distance...) |
| `pipe.ok` | 0/1 | a pipeline run of **this game session** had been cached. 0 before the first run after a load (or `hg reset all`): the cache then still holds the previous save's page and candidates, so neither is logged (no `shown` rows, no `eligible` rows) |
| `pipe.page` | int | the page shown (-1 when `pipe.ok` is 0) |
| `pipe.slots` | int | keys on that page |
| `pipe.ageMs` | int ms | how old that page was when the context was taken |
| `race` | string or null | editor ID of the hostile primary target's race (alive, hostile), else null |
| `wc.base`, `wc.max` | float | the wildcard odds in force (per-slot probability base and cap) |
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

| Flag | Bit | Meaning |
|---|---|---|
| `eligible` | 1 | a candidate of the last pipeline run, **before** the old floors (`fMinimumContextWeight`, `fMinimumUtility`) dropped any: every item CandidateGenerator produced (after its hard filters: uncastable, on cooldown, equipped, buff already active) |
| `scored` | 2 | ...that also passed the old floors (`util` is set) |
| `held` | 4 | carried (inventory stacks the catalog describes) or a spell the player knows |
| `equipped` | 8 | in a hand, worn, or the nocked ammo, when the context was taken (a press's context is taken after the press's own equip: see `dec.preEquipped`) |
| `shown` | 16 | on the page the player saw; `slot` is its key |
| `wildcard` | 32 | shown as a wildcard |
| `override` | 64 | shown by an override |
| `remembered` | 128 | shown by a Remembrance hold |
| `addedAtPick` | 256 | the chosen item, missing from its context; carried in `dec.add` |

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

**Wildcard propensity** (`wp`): the probability of the roll that put this item
on the page as a wildcard -- P(its slot rolled a wildcard) × (the entries of its
FormID in the pool) / (the pool it was drawn from), given the earlier rolls of
the same pass (`WildcardManager`). The pool holds one entry per candidate row,
so two stacks of one weapon are two entries of one FormID. It
does not include whether a roll happened at all (rolls wait for no wildcard
being active and the refractory period). Only rows shown as wildcards carry it.

### `dec`

| Field | Type | Meaning |
|---|---|---|
| `v` | int | 3 |
| `seq` | int | per launch, in write order. The writer assigns it as it writes, so a record dropped from a full queue takes no number, and a record whose write the stream reports failed gives its number back to the next one: no gaps. (A failed write that still reached the disk leaves a torn line, which the reader skips, or in the rarest case a record whose `seq` repeats in the next one.) |
| `utc` | string | when the record was made: the confirmation for a pick; the end of the grace for nothing |
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
| `case` | string | the old A–E attribution of an outside pick |
| `how` | string | what confirmed it: `consumed`, `still equipped`, `used` (a soul gem) |
| `kind` | string | `consume` or `equip` |
| `confirmMs` | int ms | press to confirmation |
| `repeat` | 0/1 | the same item equipped again within the repeat window (30 s): the same decision to the learner, logged anyway |
| `learned` | 0/1 | the frozen learner took it |
| `skip` | string | why it did not: `stale` (the pipeline cache older than `fExternalEquipTimeWindow`), `disabled` (learning from outside equips off), `armour` (armour picks reach the v3 log only) |
| `ctx` | int | the context it is joined to |
| `ctxAgeMs` | int ms | press time minus the context's time (for nothing: record time minus the onset) |
| `open` | int[] | needs with an open episode at the press (opportunity counting) |
| `preEquipped` | 0/1 | the chosen item was equipped before the press: in a hand or the nocked ammo on the newest update tick at least 0.25 s before it. 0 for anything else (an equip event says armour was not worn), **and 0 when no tick is that old** -- in the first 0.25 s after a load, when the snapshots were just cleared, it means "not known", not "not equipped" |
| `ep` | object or null | nothing only: `need` (id), `i` (index), `durSec`, `peak` (the highest value inside the episode), `onset` (UTC) |

## Outcomes

| `out` | Recorded when | Joined to |
|---|---|---|
| `key` | a Huginn key's selection confirms (`SelectionTracker`) | the context taken at the press |
| `wheel` | a pick on Huginn's own Wheeler wheel confirms | the context taken at the press |
| `menu` | any other selection the player made confirms: the inventory, magic or favourites menu, a vanilla favourites hotkey, one of the player's own Wheeler wheels. Includes the picks the frozen learner drops (`learned: 0`): a stale cache, the learning toggle off, armour | for a pick from the inventory or magic menu (open, or closed within 2 s): the context taken **when the menu opened**; otherwise (the favourites menu included) the context taken at the press |
| `nothing` | a need episode ends unanswered (below) | the context taken at the episode's onset |

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
| death | (0.23.16) when the player dies, every open episode and every ended one still in its grace is dropped, as at a load, and none opens while the player is dead: a death is not a decision. Earlier logs can hold `nothing` records cut short by a death a few seconds before a reload |

Every need takes part, the always-on ones too (`loadout_*`, `downtime`): they
rarely end unanswered, since a weapon swap that ends a loadout episode is a
selection, and the fit can drop any need it does not want -- the record names
it. Several needs that start on one tick share one onset context. A press does
not answer an episode it started (the onset is after the press).

## Perception line

Only what the player can perceive or what Huginn already reads: the player's
own inventory, spells and equipment, the need vector (R3's sensors), the page,
and the race of a hostile target in view (its look and name are on screen; the
race map keys on it). No enemy spell lists, inventories, levels or hidden
numbers.

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
The rate of `nothing` records in real play is not known yet: R5's session will
tell.

What bounds it:
- **Caps once per segment**, content-addressed: the 243-column vectors are
  written once and referenced by id (the first context of a segment carries
  them all).
- **Sparse everything**: caps, needs, inputs and cross-features list their
  non-zero entries only.
- **Shared contexts**: a menu visit's picks, and the episodes that start on one
  tick, write one context.
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
- `ctx.need` is the latest update tick's vector (the monitor runs every tick,
  ~100 ms); the page is the last pipeline run's (`pipe.ageMs`).
- Game data changed by scripts after the catalog was built is not seen (the
  catalog's own limit, R2).
- A press inside a menu opened before a game load (none in practice) falls back
  to the press-time context.
