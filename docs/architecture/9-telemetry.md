# Decision Log (opt-in telemetry)

Huginn can write a **decision log**: a file listing what it recommended and what
the player then picked. Players can **choose** to send the file to the developers.
Files from many players are pooled and used for offline training and to evaluate
new scoring rules offline, against real play, before they ship.

- **Off by default.** It is written only when `bEnabled = 1` under `[Telemetry]` in
  `Huginn.ini`.
- **Nothing is uploaded.** The file stays on the player's disk until they send it
  themselves.
- It records only inputs the scorer already uses (see the Core Principle in
  `CLAUDE.md`). The log reads nothing new from the game.

> **Related documentation:**
> - [0-pipeline.md](0-pipeline.md): where the log hooks into the tick
> - [4-contextual-bandits.md](4-contextual-bandits.md): the 18 features (φ), rewards, the utility formula
> - [5-slots.md](5-slots.md): slot classifications, assignment types, wildcards, locks

---

## How to send us your file

1. In `Data\SKSE\Plugins\Huginn.ini` (with a mod manager, the copy in Huginn's
   mod folder), set:
   ```ini
   [Telemetry]
   bEnabled = 1
   ```
   Then start the game, or type `hg reload` in the console if it is already running.
2. Play normally. `hg telemetry` in the console shows whether logging is on, how
   many records have been written or dropped, and how large the file is.
3. Find **`Huginn_Telemetry.jsonl`** in your SKSE log folder:
   `Documents\My Games\Skyrim Special Edition\SKSE\` (or `Skyrim VR\SKSE\`, or
   the GOG equivalent). If it grew past the size cap, older parts are next to it
   as `Huginn_Telemetry.1.jsonl`, `.2.jsonl` and so on.
4. Attach the file(s) to a GitHub issue or a Discord post. They compress well,
   so zip them first if they are large. Open them in any text editor first if
   you want to see exactly what you are sending.
5. To stop, set `bEnabled = 0` again (or `hg reload` after editing). You can
   delete the files at any time.

---

## Settings (`[Telemetry]` in `Huginn.ini`)

| Key | Default | Range | Meaning |
|---|---|---|---|
| `bEnabled` | `0` | 0/1 | Write the log. Hot-reloads with `hg reload` or dMenu. |
| `iMaxFileSizeMB` | `16` | 1–512 | Rotate the file when the next record would pass this size (MiB). |
| `iMaxRotatedFiles` | `3` | 0–10 | Rotated copies to keep (`<stem>.1.jsonl` is the newest). `0` truncates and starts over. |
| `iTopCandidates` | `20` | 0–50 | Best-scored candidates recorded per impression, in addition to whatever was displayed. |
| `sFileName` | `Huginn_Telemetry.jsonl` | name | **File name only**. Any directory part is dropped, `%` and control characters become `_`, and `.jsonl` is appended if missing. The file is always written in the SKSE log folder. |

Loaded by `Telemetry::TelemetrySettings` (`src/telemetry/TelemetrySettings.*`) in
`InitializeGameSystems` step 1 and in `SettingsReloader` (reload step 6b, reset).
`SettingsReloader::ApplySideEffects` step 7 then calls `DecisionLog::ApplyConfig`
to start the writer, stop it, or reopen it under a new file name. "Reset to defaults" turns
the log off.

---

## What is logged, and what is not

**Logged:**
- The 18-float state feature vector φ (`StateFeatures::ToArray()`: vitals %, in
  combat, sneaking, normalised distance to the nearest hostile target, target kind,
  equipped-hand flags, bias).
- For each **impression**: the top `iTopCandidates` scored candidates, plus any
  displayed item outside that prefix. Each carries its utility, every
  `ScoreBreakdown` term (including the `FitScorer` fit multiplier, whether or
  not it was applied), its equivalence key (spells and scrolls), and its
  fit-relevant properties (type, school, element, base and effective cost,
  range, delivery, skill level, concentration, magnitude, duration, count,
  weapon damage, speed, charge). Also recorded: which candidate each slot of
  the displayed page held, how it got there (`Normal` merit / `Wildcard` /
  `Override` / `Remembered`), and whether the slot was locked.
- The scorer, fit, wildcard and equivalence-cap parameters that shape the policy (`cfg` record).
- **Reward events**: equip via hotkey / Wheeler / external (vanilla menu,
  favourites), consumption, and misclick penalties, each with its reward value.
- A **stable item key** (below). The plugin **file name** is part of it.
- Plugin version, schema version, and a random session id.
- Time as offsets: real milliseconds since the log was first enabled in this
  game process, and game seconds since the last save load.

**Never logged:** item, spell, character or save names; file-system paths
(`hg telemetry` and the SKSE log print only the file name); Windows user,
machine or OS information; wall-clock dates or times.

**Accepted leak:** plugin file names appear in item keys, so a file reveals the
plugins that own the recommended items, and the name of a personal patch plugin
if it owns an item. The stable key cannot work without them.

The session id is 128 bits from `std::random_device`, generated once per game
process when the log is first enabled. Nothing about the player, the machine or
the time goes into it. It links the records of one play session and nothing else.

---

## Stable item keys

A runtime FormID's top byte is the owning plugin's load-order index. To pool files
across players, items are keyed by the **originating** plugin
(`form->GetFile(0)`) plus a load-order-independent local id
(`src/telemetry/ItemKey.h`):

| FormID top byte | Plugin kind | Key |
|---|---|---|
| `0x00`–`0xFD` | regular (ESM/ESP) | `"<file>\|%06X"` of `id & 0x00FFFFFF`, e.g. `"Skyrim.esm\|012FCD"` |
| `0xFE` | light (ESL / ESL-flagged) | `"<file>\|%03X"` of `id & 0xFFF`. Bits 12–23 are the ESL slot, which depends on load order. |
| `0xFF` | runtime-created (player-brewed potions, enchanted copies) | `"~dyn"`: there is no stable identity |

A static form that reports no file gets `"?"` as its plugin. Keys are resolved on
the producer thread and cached per FormID. Dynamic ids are not cached, because
the engine reuses them.

---

## File format

[JSON Lines](https://jsonlines.org/): one JSON object per line, UTF-8, `\n`
line endings. Every record has:

- `"n"`: a per-session sequence number. Numbers are assigned when a record is
  queued, so a **gap in `n` means records were lost** (queue full, file not
  writable). The order in the file may differ slightly from `n` order around
  `session` and `drop` records, which the writer numbers itself.
- `"t"`: the record type.

Floats have 4 significant digits. NaN and infinity are written as `null`.

Several game sessions can accumulate in one file (it is opened for append). Each
starts with a `session` record, and every record belongs to the most recent
`session` line above it. Rotation also writes a fresh `session` header at the top
of the new file, with the same `sid`.

### `session`: file / session header (schema v2)

```json
{"n":0,"t":"session","schema":2,"plugin":"0.20.63","sid":"3f9c…(32 hex)","rt":0,
 "features":["healthPct","magickaPct","staminaPct","inCombat","isSneaking","distanceNorm",
             "targetNone","targetHumanoid","targetUndead","targetBeast","targetConstruct",
             "targetDragon","targetDaedra","hasMeleeEquipped","hasBowEquipped",
             "hasSpellEquipped","hasShieldEquipped","bias"],
 "rewards":{"equip":8,"consume":5,"misclick":-3},"qcap":4096}
```

| Field | Meaning |
|---|---|
| `schema` | Record schema version. Readers must check it. |
| `plugin` | Huginn version (`Plugin::VERSION`) |
| `sid` | Random session id (see privacy) |
| `features` | Names of the 18 `phi` entries, in order |
| `rewards` | `Config::EQUIP_REWARD`, `CONSUME_REWARD`, `MISCLICK_PENALTY` |
| `qcap` | Writer queue capacity |

### `cfg`: policy parameters

Written before the first impression after startup, each `hg reload`, and each
save load.

| Field | Source |
|---|---|
| `lambdaMin`, `lambdaMax` | λ(confidence) range |
| `explore` | UCB exploration weight |
| `coldBoost`, `minUtil`, `minCtx`, `topN` | cold-start boost, utility floor, context floor, top-N sort prefix |
| `favMode` (`Boost`/`Off`/`Suppress`), `favMin`, `favMax` | favourites handling |
| `potionTier` (`Higher`/`None`/`Lower`) | potion tier preference |
| `wcOn`, `wcBase`, `wcMax`, `wcCooldown`, `wcRefractory`, `wcFirstExcluded` | `WildcardManager` settings. P(slot i) = `wcBase × i`, capped at `wcMax`. |
| `topCands` | `iTopCandidates` |
| `fitMode` (`Off`/`Shadow`/`Apply`) | `[Scoring] iFitMode`. Only under `Apply` did the candidates' `fit` enter `u`. |
| `fitAffordMin`, `fitAffordFull`, `fitUnaffordable`, `fitConcSecs`, `fitOutOfRange`, `fitClampMin`, `fitClampMax` | The remaining `[Scoring] fFit*` parameters |
| `uncastable` (`Disallow`/`Penalize`/`Allow`) | `[Candidates] sUncastableSpellPolicy` (selects the below-one-cast fit value) |
| `capOn`, `capMax`, `capBands` | `[SlotLocker] bCapEquivalents`, `iMaxPerEquivalenceKey`, `sEquivalenceCostBands` (array of 4) |

### `load`: a save was loaded or a new game started

`{"n":..,"t":"load","rt":..,"new":false}`: no save or character information.
It resets the game-time base (`gt`) and the impression chain. A reward after a
load never joins to an impression from before it.

### `imp`: impression (what the player was shown)

Written **only when the display changes**: the page, or any slot's
`(formID, uniqueID, assignment type)`. It is also written for the first display
after a reward or misclick, so each reward has an impression before it and one
after it. Identical ticks write nothing (CLAUDE.md "log transitions, not ticks").

| Field | Meaning |
|---|---|
| `id` | Impression id (1, 2, …), per game process. Rewards refer to it. |
| `rt` | Real ms since the session start |
| `gt` | Game seconds since the last `load` (`null` before the first load) |
| `page`, `npages` | Displayed page index and page count |
| `reason` | Dominant `ContextReason` name (`None`, `CriticalHealth`, `LowMagicka`, `TargetUndead`, …) |
| `ovr` | An override took a slot this tick |
| `phi` | 18 floats, the same `StateFeatures::FromState` call the scorer makes |
| `cands` | Candidate list (below) |
| `slots` | One entry per slot of the displayed page (below) |

`cands[j]`:

| Field | Meaning |
|---|---|
| `k` | Stable item key |
| `uid` | Inventory stack id (weapons and apparel), omitted when 0 |
| `src` | `Spell`, `Potion`, `Scroll`, `Weapon`, `Ammo`, `SoulGem`, `Food`, `Staff`, `Apparel` |
| `u` | Final utility |
| `ctx` | contextWeight |
| `prior` | PriorCalculator prior |
| `est` | Learner reward estimate |
| `ucb` | UCB bonus |
| `conf` | Confidence α |
| `learn` | learningScore |
| `lam` | λ actually applied |
| `rec` | UsageMemory recency boost |
| `corr` | Correlation bonus |
| `potion` | Potion multiplier |
| `fav` | Favourites multiplier |
| `wc` | Candidate was injected as a wildcard |
| `cold` | Scored with the cold-start UCB boost |
| `p` | Properties by kind (below) |
| `fit` | `FitScorer` multiplier (1 = neutral / not a spell / `iFitMode = 0`) |
| `fitOn` | `fit` was multiplied into `u` (`iFitMode = 2`) |
| `fitCasts`, `fitAfford`, `fitRange` | Casts left at current magicka (`null` = free spell), affordability and range factors. Spells only, and only when fit was computed; omitted otherwise. |
| `eqk` | Equivalence key (`type\|tagsHex\|tagsExtHex\|element\|delivery\|tier`, e.g. `"Damage\|0x12\|0x0\|Fire\|Ranged\|0"`) for spells and scrolls, `null` for everything else. Written whether or not `bCapEquivalents` is on. |

`p` by kind (tag bitfields are hex strings such as `"0x1A"`, taken from the
`SpellTag`/`ItemTag`/`WeaponTag` enums of the logged plugin version):

| Kind | Fields |
|---|---|
| Spell | `type`, `school`, `element`, `baseCost`, `effCost` (perk-adjusted; per second for concentration), `range` (0 self/touch, projectile range, 4096 fallback), `conc`, `afford`, `dlv` (`Self`/`Touch`/`Aimed`/`TargetActor`/`TargetLocation`/`Unknown`), `skill` (minimum skill level, 0 = Novice/unset), `tags`, `tagsExt` |
| Scroll | `type`, `school`, `element`, `baseCost`, `dlv`, `skill`, `mag`, `dur`, `count`, `tags`, `tagsExt` |
| Item (`src` Potion, Food, SoulGem) | `type`, `school`, `mag`, `dur`, `count`, `tags`, `tagsExt` |
| Weapon (`src` Weapon, Staff) | `type`, `dmg`, `speed`, `charge` (0–1), `ench`, `tags` |
| Ammo | `type`, `dmg`, `ench`, `count`, `tags` |
| Apparel | `craft`, `mag` |

Candidates are the top `iTopCandidates` by utility. Remembered-only extras are
excluded from that ranking. Displayed items outside it (overrides, Remembrance
holds, low-ranked wildcards) are appended, deduplicated by `(formID, uniqueID)`.

`slots[i]`:

| Field | Meaning |
|---|---|
| `i` | Slot index on the page |
| `cls` | `SlotClassification` name (`Regular`, `HealingAny`, `DamageAny`, …) |
| `as` | `Empty`, `Normal` (merit), `Wildcard`, `Override`, `Remembered` |
| `c` | Index into `cands`, or `-1` when empty |
| `lk` | The slot was lock-held (`SlotLocker`) at the time |

### `rew`: reward event

| Field | Meaning |
|---|---|
| `rt`, `gt` | As for `imp` |
| `imp` | Id of the last impression written before the event (`null` if none since the load). **Join key.** |
| `k` | Item key of the equipped or consumed item (misclick: the item being penalised) |
| `src` | `Hotkey`, `Wheeler`, `External`, `Consumption`, `Misclick` |
| `mult` | The publisher's reward multiplier (External attribution scaling) |
| `recd` | The event says the item was recommended (per-source semantics as in `EquipEvent::wasRecommended`) |
| `r` | Reward the learner was updated with. 0 when it was not trained. |
| `trained` | The learner was updated. The rule is `ComputeBanditReward`: Hotkey/Wheeler need `recd`, and `mult <= 0` never trains. |
| `phi` | The event's own feature vector (captured by the publisher, which may differ from the impression's) |
| `shown` | `{"page":p,"slot":s}` if the item was on the last impression's page, else `null` |

A misclick produces two `rew` lines: the penalty on the previous item
(`src:"Misclick"`) and the ordinary reward line for the new pick.

### `drop`: records lost

`{"n":..,"t":"drop","rt":..,"count":N}`: N records were lost since the last
report, because the queue was full or the file could not be opened. They also
show as gaps in `n`.

---

## Offline evaluation: what this data supports

Huginn's policy is **deterministic except for wildcards**: ranking, allocation and
locks give the same display for the same inputs. So:

- **Unbiased inverse-propensity (IPS) estimates are possible only on wildcard
  exposures** (`as:"Wildcard"`). The log records what is needed to reconstruct the
  per-slot wildcard probability (`wcBase`, `wcMax`, `wcFirstExcluded`, slot index).
  Reconstructing *which* candidate was drawn from the pool also needs the
  `WildcardManager` selection rule for that plugin version.
- For every other exposure, the propensity of what was shown is 1, and 0 for
  everything else. **Replay and direct-method estimates on these are biased**
  toward the logged policy. They are useful for regression checks ("would the new
  rule have shown what the player then picked?"), but they are not unbiased
  counterfactuals.
- The top-N `cands` list gives each item's full score breakdown, so an alternative
  scoring rule can be replayed against the same φ and properties without
  re-running the game.

---

## Threading and performance

- Producers: the game main thread (pipeline impressions, hotkey and external
  equips), the Wheeler callback thread, and the poll thread (consumption). Each
  **builds the JSON line itself**: producers are the only side that touches forms,
  for item keys and game time. The line then goes onto a bounded queue
  (`QUEUE_CAPACITY = 4096`).
- A single background **writer thread** (`std::jthread`) does all file IO: write,
  flush once per drained batch, and rotate. It makes **no `RE::` calls**.
- When the queue is full, the new record is dropped and counted, and a `drop`
  record reports it later.
- When the log is off, every producer returns after one atomic load. A producer
  that passed that check just before the log was turned off is turned away when
  it enqueues, and its record counts as dropped. So a record never lands in the
  next session's file after that session's header.
- Turning the log off (`hg reload`, dMenu, `reset all`) never joins the writer
  on the calling thread, which is the game main thread under the update mutex.
  The queue is closed and emptied, and the writer abandons its batch after the
  line it is writing. The stopped thread is joined when the writer next starts,
  so a file-name change waits for one line at most.
- `DecisionLog` is heap-allocated and never destroyed, because joining a thread
  from a static destructor would run under the loader lock. At process exit, at
  most the batch being written is lost.

## Known limitations

- **Only the displayed page.** The pipeline allocates only the page being
  displayed each tick. Wheeler's other pages are allocated from its callback,
  and they are not logged. A reward for an item that was not on the last
  impression's page carries `"shown":null`.
- `phi` in an impression is computed from the raw player state, as the scorer's
  own feature extraction does. It does not use the `VitalEnvelope`-smoothed copy.
- Item properties and tag bit meanings are those of the logged plugin version.
  Readers should key their interpretation on `plugin` / `schema`.

## Schema changes

Any change to a record's fields, an enum spelling, the feature order or the key
format bumps `SCHEMA_VERSION` in `src/telemetry/DecisionLog.h`.

- **v1**: initial format.
- **v2** (additive): per-candidate `fit`, `fitOn`, `fitCasts`, `fitAfford`,
  `fitRange` (`DecisionLog::AppendFitFields`) and `eqk`
  (`DecisionLog::AppendCapFields`); `dlv` and `skill` in spell/scroll `p`, and
  `baseCost` for scrolls; the fit, `uncastable` and cap parameters in `cfg`.
  A v1 reader that ignores unknown fields reads v2 unchanged.

## Reading the files

`tools/telemetry_aggregate.py` is a small reference reader. It validates
`schema`, joins each `rew` to its `imp` by `(sid, imp)`, and prints
per-item exposure and reward counts:

```sh
python tools/telemetry_aggregate.py Huginn_Telemetry*.jsonl
```
