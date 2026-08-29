# Console Commands

> **Module:** [`src/console/`](../../src/console/ConsoleCommands.cpp)
> **Verified against:** v0.19.10

Huginn registers a console command accessible from Skyrim's tilde (`~`) console.
Registration happens in `kDataLoaded` and is not build-gated, so the command is
available in both Debug and Release builds.

## Usage

```
hg <subcommand> [argument]
```

The command is registered as `Huginn` with the short alias `hg`. Both spellings
work (`huginn status` is the same as `hg status`), and input is lower-cased
before dispatch.

## Commands

| Command | Description |
|---------|-------------|
| `hg help` | Show available commands |
| `hg refresh` | Force immediate recommendation update |
| `hg recs [N]` | Dump top-N recommendation breakdown to the log (default 10, max 50) |
| `hg unlock` | Clear all slot locks |
| `hg status` | Show system status |
| `hg weights <FormID>` | Show the FeatureQLearner weight vector for a form |
| `hg rebuild` | Force rebuild all registries |
| `hg reload` | Hot-reload all settings from INI |
| `hg page [N]` | Switch to page N, or show the current page |
| `hg reset qvalues` | Clear learned preference data (alias: `hg reset q`) |
| `hg reset all` | Full system reset |

The help text is generated from the same table that dispatches the commands, so
`hg help` cannot drift from what is implemented. `hg reset q` is hidden from the
help listing because it is an alias.

---

### `hg help`

Prints all available subcommands to the console. This is also the default when
no subcommand is given — typing just `hg` shows help.

Anything unrecognised prints `Unknown command: '<x>'. Type 'hg help' for
available commands.`; a bare `hg reset` prints `Usage: hg reset <qvalues|all>`.

### `hg refresh`

Clears all slot locks, then runs one full update cycle immediately through
`UpdateHandler::ForceUpdate()` (candidate generation, utility scoring, override
evaluation, slot allocation, display push).

**When to use:** After any reset command to see results immediately, or if the
widget appears stale.

**Output:** `Recommendations refreshed`

### `hg recs [N]`

Queues a one-shot, full-detail dump of the top N scored candidates and forces a
pipeline pass so it is written now. `N` defaults to 10 and is clamped to 50; a
non-numeric or non-positive `N` prints `Usage: hg recs [N]  (N = 1-50, default 10)`.

The dump goes to the Huginn log, **not** to the console. It contains:

- A `[Recs] === Top n/m candidates | u = ctx*(1+λ*learn)*mults ===` header
- One line per candidate: rank, name, source type, final utility, and the full
  score breakdown, with `[WC]` appended for a wildcard pick and `[COLD]` for a
  cold-start boosted one
- A `[Recs] Slots (page N 'Name'):` section listing each occupied slot's item,
  FormID, utility and display subtext label

The log lives in `Documents/My Games/Skyrim Special Edition/SKSE/` —
`Huginn.log` in Release, `_Huginn_Debug.log` in Debug.

**Output (console):** `Top N recommendations dumped to Huginn log`

### `hg unlock`

Clears all slot locks. The SlotLocker normally holds recommendations in place
for a few seconds to prevent visual flickering. This command releases all locks
immediately.

**When to use:** When a slot appears stuck showing an old recommendation.

**Output:** `All slot locks cleared (N were active)`

### `hg status`

Prints a three-line system summary:

- `FQL: N items, M total trains` — FeatureQLearner size and lifetime training
  count (omitted entirely if the learner is not yet initialized)
- `Registries: N spells, N items, N weapons, N scrolls`
- `Page: C of T ('Name'), N slots, L locked`

**When to use:** General debugging and health checks.

### `hg weights <FormID>`

Prints the FeatureQLearner's learned weight vector for one form. The FormID is
parsed as **hex**, e.g. `hg weights 12FCC`.

The header line reports the form name, training count, the learned score
recomputed against the *live* state features (printed as `Q=`), the confidence,
and the UCB exploration bonus. It is followed by one line per feature, in
`StateFeatures::ToArray()` order:

```
healthPct  magickaPct  staminaPct
inCombat   isSneaking  distNorm
tgtNone    tgtHumanoid tgtUndead  tgtBeast  tgtConstruct  tgtDragon  tgtDaedra
melee      bow         spell      shield
bias
```

If the form has never been trained, the output is
`<FormID> '<Name>': no training data`. If no game is loaded,
`FeatureQLearner not initialized (load a game first)`.

### `hg rebuild`

Forces a full rebuild of all registries (spells, items, weapons, scrolls),
re-scanning the player's known spells and inventory from scratch. Runs under the
update mutex. Does **not** clear learned weights.

**When to use:** After installing or removing mods that add spells/items, if new
content isn't showing up in recommendations.

**Output:** `Registries rebuilt (N spells, N items, N weapons, N scrolls)`

### `hg reload`

Re-reads every settings section from INI and re-applies the side effects
(scorer config, context weights, wildcard config, learning config, slot
allocator re-init, slot locker reset, and a Wheeler wheel rebuild if — and only
if — the page layout actually changed).

This delegates to `SettingsReloader::ReloadAllSettings()`, the same entry point
the dMenu reload button uses, so the two paths cannot diverge. Non-dMenu
sections come from `Data/SKSE/Plugins/Huginn.ini`; `[Widget]` and `[Debug]` come
from the dMenu INI when dMenu is installed, so a reload does not reset
dMenu-managed customizations.

**Output:** `All settings reloaded from INI` (plus the in-game "Settings
reloaded" notification that `ReloadAllSettings` emits).

### `hg page [N]`

Switches to recommendation page N (1-based) and clears slot locks so the new
page can populate immediately. Running `hg page` with no argument reports the
current page instead.

**Output:** `Switched to page N ('PageName')`, or
`Current page: N of T ('PageName')`. An out-of-range page prints
`Invalid page number. Use 1-T`.

### `hg reset qvalues`

Clears all learned preference data — the FeatureQLearner's per-item weight
vectors and training counts — and resets the SlotLocker so the next scoring
cycle can reassign immediately. Runs under the update mutex.

Alias: `hg reset q`

The command name keeps the historical "qvalues" spelling; see the
[terminology note](../README.md#terminology). This is the same operation as the
dMenu "reset learning data" button — both call
`SettingsReloader::ResetLearningData()`.

**When to use:** If recommendations feel biased by old learning data and you
want a fresh baseline. Note that the learned term is only one component of
scoring — context relevance and priors also contribute. Use `hg refresh`
afterwards to see the effect immediately.

**Output:** `Learning data cleared (N FQL items)`, or
`FeatureQLearner not initialized (load a game first)`.

### `hg reset all`

Performs a complete system reset under the update mutex — every subsystem
returns to the state it would have just after a save load:

1. Clears the FeatureQLearner (all learned weights)
2. Rebuilds all registries (spells, items, weapons, scrolls)
3. Calls `ResetPipelineSubsystems()`, which resets the UtilityScorer, usage
   memory, OverrideManager, CandidateGenerator, SlotAllocator, SlotLocker
   (re-reading its config from INI), the PipelineCoordinator's cross-save state,
   and the StateManager's tracking state

Step 3 is shared with the game-load path in `Globals.cpp`, so a new stateful
subsystem added there is reset by this command too.

**When to use:** When something is clearly wrong and you want to start fresh
without reloading a save.

**Output:** `Full reset complete (FQL: N items, all subsystems reset)`

## Technical Details

The command is registered by replacing the unused `ToggleDebugText` console
command in Skyrim's script function table during `kDataLoaded`, using
`RE::SCRIPT_FUNCTION::LocateConsoleCommand()` from CommonLibSSE-NG.

Two optional `kChar` string parameters are declared so Skyrim's console parser
accepts multi-word input (`hg reset qvalues`), but the actual parsing reads the
raw command text from `Script::text` and tokenizes it directly, which is more
reliable than walking the parsed chunks.

Dispatch is a linear scan over a static `CommandEntry[]` table of
`{name, helpText, takesArg, execute}` records. Entries flagged `takesArg` also
match a `"<name> "` prefix and receive the remainder as their argument.
