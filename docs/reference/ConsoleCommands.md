# Console Commands

Huginn registers a console command accessible from Skyrim's tilde (`~`) console. Available in both Debug and Release builds.

## Usage

```
hg <subcommand>
```

The command is registered as `Huginn` with the short alias `hg`.

## Commands

| Command | Description |
|---------|-------------|
| `hg help` | Show available commands |
| `hg refresh` | Force immediate recommendation update |
| `hg unlock` | Clear all slot locks |
| `hg status` | Show system status |
| `hg rebuild` | Force rebuild all registries |
| `hg page <N>` | Switch to page N |
| `hg reset qvalues` | Clear all contextual bandit learning tables |
| `hg reset all` | Full system reset |

---

### `hg help`

Prints all available subcommands to the console. This is the default if no subcommand is provided (i.e., typing just `hg` also shows help).

### `hg refresh`

Forces an immediate recommendation update cycle. Clears all slot locks and runs the full scoring pipeline (candidate generation, utility scoring, override evaluation, slot allocation) synchronously.

**When to use:** After any reset command to see results immediately, or if the widget appears stale.

**Output:** `Recommendations refreshed`

### `hg unlock`

Clears all slot locks. The SlotLocker normally holds recommendations in place for a few seconds to prevent visual flickering. This command releases all locks immediately.

**When to use:** When a slot appears stuck showing an old recommendation.

**Output:** `All slot locks cleared (N were active)`

### `hg status`

Prints a system health summary:
- Q-table size and state count
- Cache entry count and hit rate
- Registry counts (spells, items, weapons, scrolls)
- Current page, slot count, and active lock count

**When to use:** General debugging and health checks.

### `hg rebuild`

Forces a full rebuild of all registries (spells, items, weapons, scrolls). This re-scans the player's known spells and inventory from scratch. Does **not** clear reward estimates or the classification cache.

**When to use:** After installing or removing mods that add spells/items, if new content isn't showing up in recommendations.

**Output:** `Registries rebuilt (N spells, N items, N weapons, N scrolls)`

### `hg page <N>`

Switches to recommendation page N (1-based). Running `hg page` without a number shows the current page info. Also clears slot locks so the new page can populate immediately.

**Output:** `Switched to page N ('PageName')`

### `hg reset qvalues`

Clears all contextual bandit learning data: the Q-table, visit counts, and state visit counts. This erases all learned preference data. Also clears slot locks.

Alias: `hg reset q`

**When to use:** If recommendations feel biased by old learning data and you want a fresh baseline. Note that reward estimates are just one component of scoring — context relevance and priors also contribute. Use `hg refresh` after this command to see the effect immediately.

**Output:** `reward estimates cleared (N entries across M states)`

### `hg reset all`

Performs a complete system reset. This is the nuclear option — it resets every subsystem to a clean state as if you just loaded a save:

1. Clears all contextual bandit learning data
2. Resets the UtilityScorer (combat state, timers)
3. Resets the OverrideManager (hysteresis states)
4. Resets the CandidateGenerator (cooldowns)
5. Resets the SlotAllocator (page state)
6. Resets the SlotLocker (all lock timers)
7. Rebuilds all registries (spells, items, weapons, scrolls)

**When to use:** When something is clearly wrong and you want to start fresh without reloading a save.

**Output:** `Full reset complete (Q-table: M entries, all subsystems reset)`

## Technical Details

The command is registered by replacing the unused `ToggleDebugText` console command in Skyrim's script function table during `kDataLoaded`. This uses the standard `RE::SCRIPT_FUNCTION::LocateConsoleCommand()` API from CommonLibSSE-NG.

The command registers two optional `kChar` string parameters so Skyrim's console parser accepts multi-word input. The actual parsing reads the raw command text from `Script::text` for reliability.
