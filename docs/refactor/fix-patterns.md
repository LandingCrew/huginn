# Fix-Patterns Guide — critique-fixes campaign

**Audience:** every coding and reviewer agent on the `critique-fixes` campaign.
**Contract:** a diff that violates a pattern here is wrong even if it compiles and "works".
**Sources:** `docs/reviews/architecture-critique.md`, the ownership master doc (`logs/ownership-lifetimes.md`, rules O1–O18 / hazards HZ-01..27 — `logs/` is untracked, read it from the main checkout), and `CLAUDE.md`.

Each pattern: when it applies → the idiom → the anti-pattern it replaces, with an honored-at site in `src/` you can copy from.

---

## P1 — Bucket-compare for unbounded timers

**When:** any `operator==` (or dirty-flag comparison) over a field that grows without bound (`timeSince*`, accumulators).

**Idiom:** discretize both sides into coarse buckets and compare the buckets. Bucket boundaries are the points where *behavior* changes (e.g. "recent hit" vs "a while ago" vs "quiet"), typically 2/3/5 s — not epsilon windows.

```cpp
// Compare behavior-relevant buckets, never raw elapsed floats.
static constexpr int TimerBucket(float t) noexcept {
    return t < 2.0f ? 0 : t < 5.0f ? 1 : 2;
}
bool operator==(const HealthTrackingState& o) const {
    return TimerBucket(timeSinceHit) == TimerBucket(o.timeSinceHit) /* && ... */;
}
```

**Anti-pattern (the bug this campaign fixes):** `std::abs(timeSinceHit - o.timeSinceHit) < 0.2f` (`src/state/StateTypes.h:148-154,467-482`). The stored baseline only updates on inequality, so drift accumulates and the comparison fires ~5 Hz forever after the first event of a session (critique finding 1).

**Reviewer check:** after the fix, equality must be *stable* while the player state is behaviorally unchanged, and must still flip when a bucket boundary is crossed.

## P2 — Shared threshold constants

**When:** the same quantity is discretized/bucketed in more than one place (state digest + evaluator, scorer + UI, …).

**Idiom:** one named constant set in one header (`src/state/StateConstants.h` for state thresholds); both call sites include it. If a comment says "same thresholds as X", that is a smell — delete the comment by deleting the duplicate.

**Anti-pattern:** `StateEvaluator.cpp:41` buckets distance with a private `MID_MAX = 768` while `ComputeTargetDigest` uses `DistanceThresholds::MID_MAX = 2048` (`StateConstants.h:149`) — a Mid→Ranged transition produces no dirty signal (finding 1). Also the shadow constant `GAME_DAYS_TO_REAL_SECONDS` duplicated with a wrong value (`TargetActorState.h:144` vs `StateConstants.h:532`).

## P3 — Locked-public-wrapper / unlocked-private-impl split

**When:** a method that mutates lock-guarded state is callable both from outside (needs the lock) and from code paths that already hold the lock.

**Idiom:**

```cpp
public:
    void DestroyRecommendationWheels() {           // external callers
        std::scoped_lock lk(m_pageDataMutex);
        DestroyWheelsLocked();
    }
private:
    void DestroyWheelsLocked();                    // REQUIRES: m_pageDataMutex held
```

Name the private one `*Locked()` and state the lock precondition in a comment.

**Anti-pattern:** adding a naive `scoped_lock` to the existing single method — internal callers already hold `m_pageDataMutex`, so that fix **deadlocks** (`WheelerClient.cpp:329-377`, HZ-07). The other half of the same class of bug: checking `m_pageWheels.size()` before taking the lock and indexing after (`WheelerClient.cpp:209-211`, HZ-08) — the check must move inside the lock.

## P4 — RunExclusive inside the callee

**When:** any entry point that mutates what the pipeline reads per tick (settings reload, reset, wheel destroy/create, Q-table clear).

**Idiom:** the *callee* wraps its own mutation in `UpdateHandler::RunExclusive` (`UpdateHandler.h:72-81`), so no caller can forget:

```cpp
void SettingsReloader::ReloadAllSettings() {
    UpdateHandler::GetSingleton().RunExclusive([this] { ReloadAllSettingsExclusive(); });
}
```

Guard against re-entrancy if a console path already wraps the call (either remove the caller-side wrap in the same diff, or make RunExclusive re-entrant-safe — prefer removing the caller-side wrap).

**Honored:** console commands (`ConsoleCommands.cpp:83,139,279,303`). **Violated (to fix):** the entire dMenu path — `SettingsReloader.cpp:66,202,238` (HZ-06, rule O9). Concurrent `std::string` reassign/read is UB, not just a stale read.

## P5 — FormID, not pointer, across ticks

**When:** anything referencing a game object beyond the current call.

**Idiom:** store `RE::FormID`; re-resolve with `TESForm::LookupByID` + null-check at every use. For actors prefer handles. (Rule O1 — honored codebase-wide; keep it that way.)

**Anti-pattern:** caching `TESForm*`/`Actor*`/`InventoryEntryData*` in a member that survives the tick. Also remember `player->AsMagicTarget()`, never `player->As<RE::MagicTarget>()` (CLAUDE.md).

## P6 — extraList stabilization gating

**When:** any walk of `entry->extraLists` / `invChanges->entryList`.

**Idiom:** gate on the post-load stabilization window (`Config::EXTRALIST_STABILIZATION_MS`), invariant stated at `WeaponRegistry.h:353-357`; honored sites `WeaponRegistry.cpp:101,122,276,990`. The campaign hoists this gate to `src/util/` (unit U6) — new code uses the util version, never a fresh file-static copy.

**Anti-pattern:** `ItemRegistry.cpp:1041-1051` — soul-gem extraLists walk reachable ungated at kPostLoadGame (HZ-02; same crash class as PR #41's `GetItemCount` bug). Also never call `InventoryChanges::GetItemCount` — walk `entryList`/`countDelta` with the two-phase base-container pattern instead.

## P7 — Copy-out before the render thread

**When:** any data displayed by ImGui debug widgets (D3D Present hook = render thread, T2).

**Idiom:** on the update thread (or inside the widget's existing 250 ms `ShouldUpdateCache` throttle), copy the values into a by-value snapshot member; the render path reads only the snapshot. Honored example: `RegistryDebugWidget.cpp:578` uses copy-out `GetAllWeapons()`.

**Anti-pattern:** dereferencing a registry-internal pointer after the accessor's `shared_lock` released — `GetBestHealthPotion()->data.name.c_str()` on the render thread (`RegistryDebugWidget.cpp:280-312` etc., HZ-05). Registry accessor pointers are a **synchronous, same-thread loan** (rule O4, `WeaponRegistry.h:83-91`): valid only until the next registry write, never across a tick or thread.

## P8 — Stable-address strings for cross-DLL exports

**When:** any `c_str()` handed to the Wheeler API (labels, subtexts, client name).

**Rule (O11):** strings exported to Wheeler are **indefinite Huginn-owned borrows** — the backing `std::string` must stay alive *and address-stable* until cleared or replaced *through the API*.

**Idiom:** place the string in its final long-lived storage **first**, then export the pointer from that storage; replace by handing the new pointer to the API *before* (or without ever) freeing the old buffer. For containers of exporting objects, use storage whose growth doesn't relocate elements (`std::deque`, `unique_ptr<T>` elements, or reserve-then-never-grow) — MSVC SSO means even a `std::string` *move* relocates short-string bytes.

**Anti-patterns (both to fix in U9):**
- Reassign-then-hand-off: `m_cachedSubtext = newText;` frees the buffer Wheeler's renderer may still be reading, *then* `SetManagedWheelEntrySubtext(...)` delivers the new pointer (`WheelerClient.cpp:865-869`, HZ-13). Order must be: hand new pointer to API, then release old storage (or swap in place).
- Export-then-move: `CreateManagedWheel(label.c_str())` from a local `PageWheel`, then `m_pageWheels.push_back(std::move(pw))` relocates the SSO bytes; later `push_back`s can realloc and relocate *earlier* pages' labels (`WheelerClient.cpp:494,509,580`, HZ-14).

## P9 — View truncation at cross-tick boundaries

**When:** storing anything containing a `string_view` into registry-owned strings (candidates, `ScoredCandidate`) beyond the current pipeline run.

**Rule (O5):** candidate `name` views die with the tick. The owning copy is made exactly once, at `SlotAssignment::FromCandidate` (`SlotAssignment.h:116`); every cross-tick reader uses the owned `assignment.name` or POD fields.

**Idiom:** when an object carrying a view enters cross-tick storage (SlotLocker, caches), truncate the borrow: blank/re-point the view, drop the embedded candidate, or replace with an owned string. Registry swap-pop invalidates views into the *moved last entry* too, not just the removed one (HZ-16) — "my item wasn't removed" is not safety.

**Anti-pattern:** `SlotLocker::m_lockedSlots` holding `ScoredCandidate` copies whose views span up to 10 s across item consumption (HZ-15, fixed in U10). Never add a `candidate->GetName()` read on locked/cached/previous-tick content.

## P10 — Scaleform rules (verified clean — keep them)

- Every string argument to `GFxValue::Invoke` goes through `uiMovie->CreateString` (movie-managed copy). Numbers/bools are value types.
- All GFx work defers via `SKSE::GetTaskInterface()->AddUITask`; lambdas capture **by value** and re-resolve the `IntuitionMenu` singleton atomically inside the task, null-checking it (rule O3 — all 9 existing sites verified; copy them).
- Never call `Invoke`/`CreateString` from the update thread.

## P11 — Logging principles (CLAUDE.md, extended)

- Log **transitions, not ticks**; absence of a line is the signal. Dedup periodic logs with a last-value static.
- Levels: `info` = transitions/summaries (release), `debug` = diagnostics, `trace` = per-item/per-tick. Registration lines → trace, summary count → info.
- **Warn-dedup keys must not embed live counters** — `EvaluateLowAmmo` baking the arrow count into the reason string made every shot re-fire the warn (`OverrideManager.cpp:267-269`, finding 5). Key dedup by condition enum; and key `thread_local`/static dedup per page where pages ping-pong within a tick (`SlotAllocator.cpp:278-279`).
- No on-screen `DebugNotification` inside bulk loops (`SpellRegistry.cpp:608-609` toast spam).

## P12 — Build validation (hard gate, every branch)

```sh
cmake --preset vs2022-windows -DCOPY_OUTPUT=OFF
cmake --build build --config Debug
cmake --build build --config Release
```

- **`-DCOPY_OUTPUT=OFF` is mandatory in worktrees** — the default ON post-build step deploys to the shared `CompiledPluginsPath` game dir and parallel agents would race it (`CMakeLists.txt:30`). `CompiledPluginsPath` must still be set in the environment (configure hard-fails without it, `CMakeLists.txt:25-28`).
- Both configs must compile with **zero new warnings**. Debug-only code (`#ifdef _DEBUG`) still has to build in Release by being properly guarded.
- New source files are picked up by `GLOB_RECURSE` at configure time — re-run configure after adding files; never edit CMakeLists for that.

## P13 — Campaign conventions

- **Branches:** `fix/<slug>` off `critique-fixes`; PRs target `critique-fixes` (never `main`/`soak-testing` directly). Known merge order: U1→U10 (SlotLocker), U3→U9 (WheelerClient), U2→U6 (ItemRegistry) — the second of each pair rebases.
- **Commits:** imperative subject referencing the critique finding (e.g. `Gate soul-gem extraLists scan on stabilization window (critique #6)`), body explains the *why*; end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- **Diff discipline:** minimal scope — fix your unit's finding only; no drive-by refactors, renames, or formatting outside it. Match surrounding idiom (comment density, naming, PCH includes). Deleting dead code is in-scope only where your unit's brief says so.
- **Comments:** state constraints the code can't (`// REQUIRES: m_pageDataMutex held`), not narration of the change. Don't leave "was broken, now fixed" comments.
- **Forbidden information (CLAUDE.md):** no reads of enemy spell lists, hidden traps, NPC inventories, locked containers, future events — reviewers reject any new game-state read that violates this.
- **Create-once invariant (rule O12):** `g_*` globals are created once and never recreated; five borrower classes hold raw refs into them. Do not add `.reset()`/recreate paths, and document the invariant at any site adding a new raw ref into a global.

---

## Reviewer charter (wave 2)

You are adversarial: **assume the implementation is wrong.** For every hunk, attempt to construct a concrete failure scenario (inputs/state → wrong output, race, crash). Check specifically: which pattern above governs the hunk, and is it followed exactly; does the fix regress a neighboring behavior the anti-pattern was accidentally masking (finding 1's drift masked lock-freeze and elemental staleness — the campaign's canonical example); locks — order, coverage, re-entrancy; lifetimes — who owns every pointer/view the diff touches (consult the hazard register); both build configs. A finding needs a failure scenario, not a style objection.
