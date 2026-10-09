# Doc 9 implementation map: what to build, what to prune

**Status:** plan, 2026-10-08. Nothing here is built. Merged from six read-only passes over the code, one per part of the redesign (scoring and context, learner and persistence, item description, state and sensors, slots and wildcards, tooling and config), with the claims that change the plan spot-checked against the source. A fresh-context verifier then tried to invalidate it (2026-10-08); its corrections are applied.

The target is the model in [doc 9](9-context-as-learner-input.md): `score = Σ θ·need·cap + b`, a conditional logit over the page with an outside option, a diagonal-Gaussian belief per weight, the Bayesian challenger rule and uncertainty-ranked wildcards.

**Size:** roughly 7–8k lines are pruned or replaced, most of it in the classifiers (~2,600), the context rules (~1,200), the multipliers and prior (~1,000) and the per-type dumps (~950). Counts are approximate, from the passes.

## Phases

Each phase lists what it builds and what it may prune. A phase only prunes what nothing later still reads.

### Phase 0: behaviour-neutral cleanup (can ship now)

| Item | Where | Note |
| --- | --- | --- |
| Drop ingredients | `ItemData.h:87,279`, `ItemOverrides.cpp:231`, `CandidateTypes.cpp:55`, `SelectionTracker.cpp:36`, `ItemClassifier.cpp:210-221`, `configs/Huginn_Overrides.ini:45,53-55`, docs `2-classifiers.md`, `3-candidate-filtering.md` | Ingredients never reach the registry (`ItemRegistry.cpp:790-792`), so nothing changes in play. Check `Tests.cpp` for SoulGem's ordinal |
| Delete ShadowArm | `src/learning/ShadowArm.*`, call at `SelectionLog.cpp:371`; `handsAtPress` (`EquipEvent.h:82`, `SelectionTracker.cpp:116`), read only by ShadowArm | Debug-only A/B arm; its arms assume the old formula |
| Remove the five dead weights | `ContextWeightSettings.cpp:37-40,80,191-194,224` | Four are read and never used; `fWeightBaseRelevance` is read into a field nothing consumes (0.05 is hard-coded at `ContextRuleEngine.h:126`) |
| Remove target level | `TargetActorState.h:65,130,205`, `StateManager_Targets.cpp:404,499,667` | Not perceivable; only the debug widget reads it |
| Actor-type cache keyed on race | `StateManager.h:476-479`, `StateManager_Targets.cpp:29-40` | Werewolf stays Humanoid today; the map also grows without bound |
| Race table before the catch-alls | `StateEvaluator.cpp:141-142` | The 37 misread LoreRim races (`9-data/race_map.csv`) |
| Read every queued hit | `StateManager_HealthTracking.cpp:88,147` | Only the last hit's element is read |
| Rename the slot "need" | `SlotClassifier`, `NeedCap.h:34`, `SelectionLog.h:27` | "Need" will mean the 92 needs; call this the slot class. The JSONL `need` column is read by `tools/replay/replay.py:220,226,348`: keep the key, or update replay in the same change |

### Phase 1: describe items (effect extractor)

- **New `src/effect/`**: `EffectExtractor` and `EffectCatalog`, built once at kDataLoaded over the whole load order (percentiles need the full population, not the player's inventory).
- **Reuse:** move `AvName`, `KeywordList`, `PluginOf`, `CsvQuote` from `ConsoleCommands.cpp` into `src/util/FormRead.h`; take the MagicEffect helpers from `SpellClassifier.cpp` (`GetCostliestEffect` 1182, `ResistedElement` 1299, `DetermineMagicSchool` 1279, cloak/hazard resolution 310-621), keyword checks from `ItemClassifier.cpp`, weapon stat reads from `WeaponClassifier.cpp:241-431`, player enchantments from `WeaponRegistry.cpp:876-889` and `ApparelRegistry.cpp:226-254`.
- **Split cap into static and runtime.** Static columns go in the catalog. `weapon_charge`, `overshoot_*`, `stack_count`, `ammo_matches_launcher`, `school_fortified` are computed per tick (new `src/learning/CrossFeatures.*`). Tempered and player-enchanted weapons need a per-instance cap.
- **Candidates carry a catalog index**, not 239 floats (`CandidateTypes.h:382` asserts ≤ 56 bytes).
- **Armour menu picks must reach the learner.** `ExternalEquipListener.h:81-98` drops every armour equip made outside Huginn (its comment: most of it is ordinary dressing, and turning it on shifts accept%). With all carried armour a candidate and the menu a choice set, lift it; record the accept% shift in the soak baseline. *Decided by the user 2026-10-08: lift the skip* (the lift itself is R2 work).
- **`hg dump all` becomes a view of the catalog** and absorbs the eight dump gaps in doc 9. Check coverage against `9-data/effects.csv`.
- Prunes nothing yet.

**As built (R2, 0.23.10), where it differs from the plan above:**
- The rules live in `src/core/` (`EffectRules`, `EffectMapper`, `EffectColumns.h` generated from effects.csv, and `MiniRegex`: std::regex has no lookbehind and is slow in Debug) and are ported from the Python reference extractor that measured doc 9's coverage (`tools/effects/reference/`), not from the classifiers; `SpellClassifier`/`ItemClassifier`/`WeaponClassifier` code was not reused. The game side (`src/effect/`) only reads forms.
- The per-load-order override (layer 2) is checked **first**, before the keyword table, so an entry can correct a keyword misroute; it is an optional `Huginn_EffectOverrides.ini` (none ships yet).
- The catalog maps on a worker thread (seconds in Debug on LoreRim); forms are read on the main thread at kDataLoaded.
- The cross-features are `src/core/CrossFeatures.h` + `src/effect/CrossFeatures.*` (not `src/learning/`), wired only as far as `hg cap`: nothing computes them per tick while nothing reads them. R4 is the first per-tick reader.
- Candidates do not carry a catalog index yet (it would change `CandidateTypes`, and R2 changes no scores); the catalog is keyed by FormID. Do it with R4.
- The armour items (all carried armour a candidate; the `ExternalEquipListener` armour skip) are split into their own PR: they change candidates and accept%.
- A Cloak/hazard maps through its payload spell; with no payload it maps by its own description, and with neither it is left out of coverage like a helper.

### Phase 2: describe situations (need vector)

- **New:** `NeedId`/`NeedVector` generated from `needs.csv`; `ResponseCurve` (linear, quadratic, logistic, logit, gaussian, step); a `[Needs]` INI section for curve parameters.
- **Sensors to add or fix:** encumbrance ratio (kept as a float, polled at 1 Hz), a decaying damage rate per element, combat-onset/ended and submerged timers on `steady_clock`, a held multi-hot target-family bitmask (union of the living combat hostiles, held until combat ends; no line-of-sight logic), target summoned / casting element / archer from the hostile loop, restore-pending from the active-effect walk, drop ahead (raycast; API unconfirmed).
- **Compute the vector once in `GatherState`** (`PipelineCoordinator.cpp:143-150`) as a pure function of the snapshots, and cache it; reset new timers in `ResetTrackingState`.
- **Need-signature skip gate:** a quantised (0.05) signature, so continuous needs re-score. It replaces `ambientSignature`, the elemental window and, later, the GameState buckets.
- Rules stay as they are: the vector is computed and logged only.

### Phase 3: log what the fit needs (selection log v3)

| Gap today | Fix |
| --- | --- |
| No record when nothing is pressed (`SelectionTracker.cpp:261-265`) | One record per need episode that expires with nothing pressed, with the page as it stood at the onset (decided 2026-10-08); size against the 64-record queue (`SelectionLog.cpp:289-303`) |
| Menu picks carry no choice set beyond the page | Log the held items off the page too (sparse cap per held item, or an index the replay can rebuild): the menu is a second choice set (P11) |
| Choice set is filtered by the floors (`UtilityScorer.cpp:109,127`) | Log every eligible item |
| No need vector | Log the cached vector from Phase 2 |
| No effect vector | Log sparse cap per row, snapshotted at Select (dynamic FormIDs change meaning after reload) |
| Outcome only implicit in `src`/`via`/`case` | Explicit outcome: key / wheel / menu / nothing |
| Duplicate stacks share a FormID (`PipelineStateCache.h:120-124`) | One row per item |
| Wildcard logs only base/max (`SelectionLog.cpp:379-382`) | Log each shown wildcard's propensity |

Unsettled: whether the update loop ticks inside menus. `ExternalEquipLearner.cpp:103` skips an external pick when the pipeline cache is older than `fExternalEquipTimeWindow` (500 ms in the shipped `configs/Huginn.ini:663`; 2000 ms is only the code default). It measures the cache's age, not the pick's, so a long menu session may drop menu picks.

### Phase 4: fit offline (`tools/replay`)

- **First, on today's log:** one weight per slot class × logged `ctx`, no per-item term. If it does not beat context alone, the full model probably will not.
- **Then:** conditional logit with outside option, L2, sparse θ, small b; train/held-out split by launch; read v3 fields.
- **Needs new play logged with v3.** The October soak cannot rebuild need(s); doc 9's step 3 says "fit on the soak selections", which contradicts its own challenged item.
- **Restate the bar:** 81% is the live page with overrides and holds; replay's plain ranking matches arm A* at 76% (`Soak-2026-10-LoreRim.md:96-99`). Compare against 76%, or make replay model the page. Keep "menu-pick hits above 7 of 83".
- Reuse `load()`, the predict-then-learn loop, hit@8 and `capped_page`; retire `Learner`, `Current`, `ChoiceTarget`, `UsefulLife`.

### Phase 5: make the slot side sign-safe (lands before the scorer)

Log-odds scores can be zero or negative; the slot code assumes positive ratios.

- **Bridge:** feed `score = ln(utility)` with σ = 0 and m = 1.5. This reproduces today's behaviour exactly, so the slot code can change first.
- **Fix every positive-score assumption:** the slot class cap (`SlotClassCap`, the need cap before R0) multiplies (helps a negative score) at `SlotAllocator.cpp:1162-1165,1844,1850`, and `DropSkipsSince(itemScore*factor)` at `SlotAllocator.cpp:1219` with its threshold comparison at `SlotClassCap.cpp:111` → an additive `k·ln d`; ratio logs at `SlotAllocator.cpp:1171,1175`, `SlotLocker.cpp:273`; `-1` as "incumbent gone" at `SlotLocker.cpp:244`, `SoakMetrics.h:141-143`; `utility = 0` for remembered-only rows at `PipelineCoordinator.cpp:366-369`; the `kOverrideUtility = 1000` sentinel (overflows under `exp`); the 0–15 widget bar; the unused "confidence" payload (`SlotUtils.h:66`).
- **Full sort:** only the top 10 are sorted (`UtilityScorer.cpp:241-249`); with full pages, slots fill from the unsorted tail.
- Update the ratio tests at `Tests.cpp:6462-6615`.
- **As built (R7, 0.23.11).** The allocator's decision logic moved whole into `src/core/SlotAllocCore.h` (a line-by-line port over plain records, templated on the score arithmetic); `SlotAllocator` keeps the seating memory, builds the input (`src/slot/SlotSnapshot.*`) and turns the core's events into logs, telemetry and Remembrance notes. Where the plan's sites went: the cap's counting left `SlotClassCap` (only `ClassOf` stays) and its multiplier became `Core::ClassCapTerm`; `DropSkipsSince` uses the hold's own difference test; the `[Hold]` and `[SlotChurn]` lines print scores and differences; SlotLocker's "gone" is an empty `std::optional` and `BucketChallengerRatio` buckets on the score difference (`Core::BucketLogRatio`; its constexpr asserts moved to the host tests); remembered-only rows keep utility 0 for display, but `SlotScore()` is −inf by their flag; `kOverrideUtility` is gone, an override's assignment carries `kPinnedScore` (+inf) and is told apart by its type; "the widget bar" is the scorer debug widget's 0–15 bar, now `exp(score)/15`; the confidence payload is `exp(score)` (still unread by `Intuition.as`). `ScoredCandidate::operator<` still sorts on utility (same order under the bridge) -- it moves to the score in Phase 6. The golden test: `tests/core/SlotAllocGoldenTests.cpp`, recorded snapshots in `tests/core/fixtures/slots/`, captured by `run_tests.py --capture-slots` (the old code's by branch `r7-capture-old`). After review round 1: snapshots also record the allocation's events and are compared; captures vary the [SlotLocker] settings; and the hold has a tie band (`kHoldTieEpsilon`, 2.5e-7, two to four float ulps) because the old potion tier step (1.5) equals the hold's margin, which put adjacent tiers exactly on the old float comparison's rounding boundary; exact tier ties always hold (the user, 2026-10-08).

### Phase 6: the cutover (new scorer and learner)

Order inside the phase matters:

1. **Move hard zeros to `CandidateFilters` first**: spells for others with no follower near, apparel away from a workstation, torches in daylight (`ContextWeightForCandidate.cpp:25-28,346-349,415-444`), and favorites Suppress. Otherwise apparel and torches flood the page.
2. **Turn `PotionDiscriminator`'s combat timer into the `combat_onset` sensor** (`UpdateLoop.cpp:219-225`).
3. **`ChoiceLearner`** behind the existing equip bus: sparse θ keyed by a stable (needId, effectId) hash, never by position (positional keys broke once, `StateFeatures.h:63-67`); each entry {μ, σ²}; b with prior N(0, σ_b²). The choice set is the page (everything visible, including wildcard, override and Remembrance slots) ∪ the held items off the page at a learned menu cost κ ∪ "nothing pressed". u0 depends on the situation through an outside-option effect column, θ[need, out]. Update: precision += Var_p(x), step = variance. Picks in quick succession are processed in order, each removing the chosen item from the next pick's alternatives (Plackett–Luce); the repeat window still drops re-equips. This replaces the 10 s passed-over delay. Opportunity counter on need onsets next to `PipelineStateCache::Update` (`PipelineCoordinator.cpp:545`). Battery code (`RetentionAt`, `ForgetFaded`) kept, retargeted to b. Favorites Boost becomes a battery bonus.
4. **Scorer:** v = θᵀ·need once per tick, μᵢ = cap(i)·v + bᵢ, σᵢ² under the diagonal approximation, outside option; explanation = the largest θ·need·cap term. Bootstrap θ from the Phase 4 fit.
5. **The cosave moves in here** from Phase 9 (θ must survive a load before any soak): `THTA` and `BIAS` replace `BanditSerializer`'s `BNDW` in the same change.
6. **Prune at cutover, with every consumer in the same change**, or the Debug build breaks. **Done means mechanical, not a list:** a grep over `src/` and `tools/` (code, not docs) for every pruned symbol finds nothing outside the new code, and Debug and Release build clean. The shipped `configs/Huginn.ini` loses the dead keys in the same change: `fMinimumUtility`, `fMinimumContextWeight`, `fColdStartUCBBoost`, and the `[Scoring]` and `[ContextWeights]` keys listed below. The dMenu JSON holds none of them. Docs are rewritten in Phase 9. Comments count: the grep also hits comments in code that lives on (the classifiers, `GameState.h`, `CandidateGenerator`, `WeaponData.h`), so edit them here; `ContextReasonAttribution.*`, `ContextRuleEngine_Reason.cpp` and the old learner in `tools/replay/replay.py` go too. Consumers that are rewired here, not deleted: `hg reset` and the dMenu reset (`ConsoleCommands.cpp:162-164`, `SettingsReloader.cpp:258`) reset θ to the bootstrap and clear b; `hg status` (`ConsoleCommands.cpp:228-231`) and the heartbeat's learner counts (`SoakMetrics.cpp:314-317`) report θ entries and b count. Phase 9 adds the new commands and θ-drift fields on top. The other consumers found so far, beyond those below: the train-count columns of the potion, scroll and weapon dumps (`ConsoleCommands.cpp:1176,1238,1288`), dropped here although the dumps live until Phase 10; the Verbose display mode (`IntuitionMenu.cpp:705-707`); the selection log's ctx and breakdown columns (`SelectionLog.cpp:154-155,178-182,239-252`); `PipelineCoordinator.cpp:255-264` (the `WeightsChanged` latch, rewired to θ), `PipelineCoordinator.cpp:339` (`ContextWeightMap`), `PipelineCoordinator.cpp:783` and `PipelineStateCache.h:60,133` (cold start), `PipelineCoordinator.cpp:792-794` (`g_usageMemory`); construction and test calls in `Main.cpp:18-19,271-322,453-455`; `Globals.cpp:30,33,90-91,104` and `Globals.h:19-20,34-35,148-149`; the equip path (`EquipEvent.h:4,76`, `EquipEventBus.cpp:82`, `EquipSubscribers.h:147-162`); `SettingsReloader.cpp:377-378` (memory-life reload); `UpdateLoop.cpp:601-604` (`AdvancePlayTime`, kept and pointed at the new learner); `LearningSettings.h:3`. Consumers outside the scoring code: `hg weights` and `hg dump weights` (`ConsoleCommands.cpp:280-338` with a static_assert on the feature count, `ConsoleCommands.cpp:1024`), deleted here and replaced by `hg theta` in Phase 9; the selection log's φ and prediction fields (`SelectionLog.cpp:119-126,224-227`), replaced by the v3 fields, with `tools/replay` updated; `ReasonHold`, fed by `DominantReason` today (`PipelineCoordinator.cpp:388-410`), fed by the per-item explanation instead; `ReasonAppliesTo` at `ExplanationLabel.h:108`; `UtilityScorerDebugWidget` (breakdown and `UsageMemory`); and `Tests.cpp` (about 36 `WeightForCandidate`, 37 `StateFeatures`, 35 `FeatureBanditLearner` and 10 `UsageMemory` references), deleted or ported to host tests. The list: `FeatureBanditLearner` itself (its API takes the pruned `StateFeatures`; the play clock, battery and `WeightsChanged` code move to `ChoiceLearner`), `UsageMemory` and `UsageMemorySubscriber`, `ComputeUtility` and λ, `CorrelationBooster`, `PriorCalculator`, `PotionDiscriminator`, `ApplyPotionTierPreference`, the favorites multiplier, cold start, `fMinimumUtility`, `fMinimumContextWeight`, `IsHardContextGated`, `ContextWeightMap`, `WeightForCandidate`, `DominantReason`, `ReasonAppliesTo`, the 18-float `StateFeatures`, `BanditSubscriber` and passed-over logic, the `Config.h` learning constants except `REPEAT_PICK_WINDOW_SEC`, the old `ScoreBreakdown` fields, the `[Scoring]` keys except `iTopNCandidates` (`sFavoritesMode` lives in `[Favorites]` and stays), and `[ContextWeights]` except `fDarkLightLevel` and `bAlcoholSatisfiesHunger`.

### Phase 7: Bayesian challenger rule

- The scorer hands each candidate μ, σ_b², its sparse x and a snapshot of Var(θ); the allocator computes σ_Δ (or calls a scorer `SigmaDelta(c, i)`).
- Swap at `SlotAllocator.cpp:1165` when μc − μi > ln m + z·σ_Δ. It sits behind `bKeepSlotPositions && bHoldSeatedItems`.
- Churn telemetry buckets on Δ/σ_Δ instead of ratios.
- **Keep** the lock (it guards flapping inputs; the no-swap-back proof holds only for fixed beliefs), home keys and seating. Shorten the lock after measuring with the `Expired` buckets; `ReasonHold` is clamped to it (`PipelineCoordinator.cpp:407-408`).

### Phase 8: wildcards by uncertainty

- A reserved wildcard slot per page, placed explicitly instead of a rank swap; remove the special cases at `SlotAllocator.cpp:1142-1148,1836-1849` and the same-type draw at `WildcardManager.cpp:260`.
- Rank by relevance × σ with a small random term, and log each propensity.
- Make wildcard flags per page: non-display Wheeler pages reuse the display page's list (`WheelerBackend.cpp:183`).

### Phase 9: persistence, console, telemetry, docs

- **Cosave** (built in Phase 6, listed here for the format): new `THTA` (version, pairs {key, μ, σ²}, including the outside option's θ[need, out] entries and its intercept θ_out) and `BIAS` ({formID, b, n, minutes since chosen}) records; HCID unchanged; the old `BNDW` record falls to the unknown-record branch, a clean break. `ReplaceDynamicEntries` applies to BIAS only.
- **Console:** `hg theta` plus b for an item, and a θ/b dump (the old `hg weights` and `hg dump weights` went in Phase 6); `hg recs` prints the top need·θ·cap terms, b and the outside option; the dMenu reset button's text (the reset itself was rewired in Phase 6).
- **Telemetry:** θ-drift fields in the `[Soak]` heartbeat; keep `reachIns` and `presses`.
- **Docs to rewrite or archive:** `CLAUDE.md` (formula, learner, INI, console), `docs/README.md`, `4-contextual-bandits.md` (archive), `0-pipeline.md`, `1-states.md`, `3-candidate-filtering.md`, `6-ui-ux.md`, `7-dmenu-integration.md`, `8-future-work.md`, `docs/reference/Performance.md`, `TESTING-INDEX.md`, the Nexus page (`docs/nexus/page.bbcode.txt:241-271,436`), and the roadmap (rewritten 2026-10-08 around these phases).

### Phase 10: retire the classifiers

- Re-derive what still reads classifier output from cap: the 24 `SlotClassification` predicates (`SlotClassifier.cpp:116-335`; the enum stays, users name it in INI templates), overrides (`OverrideManager.cpp:402-640`), filters, `ExplanationLabel.h:108,118-125`, the ammo/charge display (`IntuitionMenu.cpp:593-670`).
- Replace `Huginn_Overrides.ini` type/tag semantics with the per-load-order actor-value override (layer 2).
- Then delete `SpellClassifier`, `ItemClassifier`, the tag enums, the scroll classifier and the per-type dumps (spells, food, potions, scrolls, weapons, apparel, diseases, within `ConsoleCommands.cpp:435-1370`; `Cmd_DumpWeights` went in Phase 6 and `CsvQuote`/`PluginOf` moved in Phase 1), and replace the classifier fixtures in `Tests.cpp`.

## Decisions (the user, 2026-10-08)

- [x] **"Nothing pressed" is recorded when a need expires.** One record per need episode that resets with no press, with the page at the onset. It feeds u0 and opportunity counting.
- [x] **u0 depends on the situation:** an outside-option effect column, θ[need, out], learned like any other weight. Needs that never fire keep their starting value.
- [x] **Co-picks are processed in sequence** (Plackett–Luce). The user's reading: co-picks and sequential picks covary, and processing them in order keeps that out of the update.
- [x] **The page set includes everything visible:** wildcard, override and Remembrance slots too (P5 needs the choice set as shown). This reverses today's exclusion at `EquipSubscribers.h:69`.
- [x] **The menu is a second choice set** at a learned cost κ (theory page, P11). A menu pick teaches the item picked through its need × effect pairs; κ settles at the observed reach-in rate. The favorites-always-pass rule (`UtilityScorer.cpp:98-110`) goes: menu picks carry that evidence now. Today a menu pick already trains the item's own vector (`ExternalEquipLearner.cpp:67` → `SelectionTracker::Select`); what was missing is the need and effect attribution.
- [x] **All carried armour is gear and enters the candidate pool; gear in combat is left to the learner, not a hard rule.** Mid-fight swaps (gloves of Destruction ↔ gloves of Two-Handed) are free in the engine today; they are not expected to recur often, so the data per armour item will be thin, and b and the shared θ carry it. A future swap cost becomes a feature or part of κ_move. Lift `ApparelClassifier`'s scope guard (`ApparelClassifier.h:11-17`).
- [x] **Powers stay out; a bonus only if the system works very well.** Mods use powers as a grab bag (debug tools, Weathersense, prayers for favour), players keep them on the favourites list, and their context is harder to judge than spells'. Shouts are skipped too (the user, 2026-10-08).
- [x] **Target type keeps the combat-hostile union, no line-of-sight logic.** The engine's combat state is all or nothing, the crosshair sees one actor while the player sees several in frame, and manual tagging per fight is not reasonable.
- [x] **Coverage loss is not a blocker.** Script-only spells were a back-of-the-envelope concern; unmapped items still get b. Run the coverage diff before Phase 10.

## Corrections found

- Doc 9 says `PotionDiscriminator` hard-codes the tier step; it is `ScorerConfig.h:28`, applied at `UtilityScorer.cpp:332`.
- Doc 9's step 3 fits on the soak log; its own challenged item says that log cannot. New play logged with v3 is needed.
- The step-3 bar (81%) is the live page; replay compares with A* (76%).
- `5-slots.md:733` says the lock is 1000 ms; the INI sets 3 s (`configs/Huginn.ini:769`).
- The disease fix (1 of 94) landed in #171 (`StateManager_MagicEffects.cpp:186-190`); it is not yet confirmed in a soak.
- Rates use a Calendar game-time clock at 4320 s/day (`StateConstants.h:605-618`), wrong under other timescales; new decays should use `steady_clock`.
