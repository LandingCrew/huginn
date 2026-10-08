# Doc 9 implementation map: what to build, what to prune

**Status:** plan, 2026-10-08. Nothing here is built. Merged from six read-only passes over the code, one per part of the redesign (scoring and context, learner and persistence, item description, state and sensors, slots and wildcards, tooling and config), with the claims that change the plan spot-checked against the source.

The target is the model in [doc 9](9-context-as-learner-input.md): `score = Σ θ·need·cap + b`, a conditional logit over the page with an outside option, a diagonal-Gaussian belief per weight, the Bayesian challenger rule and uncertainty-ranked wildcards.

**Size:** roughly 7–8k lines are pruned or replaced, most of it in the classifiers (~2,600), the context rules (~1,200), the multipliers and prior (~1,000) and the per-type dumps (~950). Counts are approximate, from the passes.

## Phases

Each phase lists what it builds and what it may prune. A phase only prunes what nothing later still reads.

### Phase 0: behaviour-neutral cleanup (can ship now)

| Item | Where | Note |
| --- | --- | --- |
| Drop ingredients | `ItemData.h:87,279`, `ItemOverrides.cpp:231`, `CandidateTypes.cpp:55`, `SelectionTracker.cpp:36`, `ItemClassifier.cpp:210-221`, `configs/Huginn_Overrides.ini:45,53-55`, docs `2-classifiers.md`, `3-candidate-filtering.md` | Ingredients never reach the registry (`ItemRegistry.cpp:790-792`), so nothing changes in play. Check `Tests.cpp` for SoulGem's ordinal |
| Delete ShadowArm | `src/learning/ShadowArm.*`, call at `SelectionLog.cpp:371` | Debug-only A/B arm; its arms assume the old formula |
| Remove the five dead weights | `ContextWeightSettings.cpp:37-40,80,191-194,224` | Four are read and never used; `fWeightBaseRelevance` is read into a field nothing consumes (0.05 is hard-coded at `ContextRuleEngine.h:126`) |
| Remove target level | `TargetActorState.h:65,130`, `StateManager_Targets.cpp:404,499,667` | Not perceivable; only the debug widget reads it |
| Actor-type cache keyed on race | `StateManager.h:476-479`, `StateManager_Targets.cpp:29-40` | Werewolf stays Humanoid today; the map also grows without bound |
| Race table before the catch-alls | `StateEvaluator.cpp:141-142` | The 37 misread LoreRim races (`9-data/race_map.csv`) |
| Read every queued hit | `StateManager_HealthTracking.cpp:88,147` | Only the last hit's element is read |
| Rename the slot "need" | `SlotClassifier`, `NeedCap.h:34`, `SelectionLog.h:27` | "Need" will mean the 92 needs; call this the slot class |

### Phase 1: describe items (effect extractor)

- **New `src/effect/`**: `EffectExtractor` and `EffectCatalog`, built once at kDataLoaded over the whole load order (percentiles need the full population, not the player's inventory).
- **Reuse:** move `AvName`, `KeywordList`, `PluginOf`, `CsvQuote` from `ConsoleCommands.cpp` into `src/util/FormRead.h`; take the MagicEffect helpers from `SpellClassifier.cpp` (`GetCostliestEffect` 1182, `ResistedElement` 1299, `DetermineMagicSchool` 1279, cloak/hazard resolution 310-621), keyword checks from `ItemClassifier.cpp`, weapon stat reads from `WeaponClassifier.cpp:241-431`, player enchantments from `WeaponRegistry.cpp:876-889` and `ApparelRegistry.cpp:226-254`.
- **Split cap into static and runtime.** Static columns go in the catalog. `weapon_charge`, `overshoot_*`, `stack_count`, `ammo_matches_launcher`, `school_fortified` are computed per tick (new `src/learning/CrossFeatures.*`). Tempered and player-enchanted weapons need a per-instance cap.
- **Candidates carry a catalog index**, not 242 floats (`CandidateTypes.h:382` asserts ≤ 56 bytes).
- **`hg dump all` becomes a view of the catalog** and absorbs the eight dump gaps in doc 9. Check coverage against `9-data/effects.csv`.
- Prunes nothing yet.

### Phase 2: describe situations (need vector)

- **New:** `NeedId`/`NeedVector` generated from `needs.csv`; `ResponseCurve` (linear, quadratic, logistic, logit, gaussian, step); a `[Needs]` INI section for curve parameters.
- **Sensors to add or fix:** encumbrance ratio (kept as a float, polled at 1 Hz), a decaying damage rate per element, combat-onset/ended and submerged timers on `steady_clock`, a held multi-hot target-family bitmask (union of primary and living hostiles, held until combat ends), target summoned / casting element / archer from the hostile loop, restore-pending from the active-effect walk, drop ahead (raycast; API unconfirmed).
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

Unsettled: whether the update loop ticks inside menus. `ExternalEquipLearner.cpp:103` drops picks staler than 2 s, which may lose menu picks.

### Phase 4: fit offline (`tools/replay`)

- **First, on today's log:** one weight per need class × logged `ctx`, no per-item term. If it does not beat context alone, the full model probably will not.
- **Then:** conditional logit with outside option, L2, sparse θ, small b; train/held-out split by launch; read v3 fields.
- **Needs new play logged with v3.** The October soak cannot rebuild need(s); doc 9's step 3 says "fit on the soak selections", which contradicts its own challenged item.
- **Restate the bar:** 81% is the live page with overrides and holds; replay's plain ranking matches arm A* at 76% (`Soak-2026-10-LoreRim.md:96-99`). Compare against 76%, or make replay model the page. Keep "menu-pick hits above 7 of 83".
- Reuse `load()`, the predict-then-learn loop, hit@8 and `capped_page`; retire `Learner`, `Current`, `ChoiceTarget`, `UsefulLife`.

### Phase 5: make the slot side sign-safe (lands before the scorer)

Log-odds scores can be zero or negative; the slot code assumes positive ratios.

- **Bridge:** feed `score = ln(utility)` with σ = 0 and m = 1.5. This reproduces today's behaviour exactly, so the slot code can change first.
- **Fix every positive-score assumption:** the need cap multiplies (helps a negative score) at `SlotAllocator.cpp:1162-1165,1219,1844,1850`, `NeedCap.cpp:111` → an additive `k·ln d`; ratio logs at `SlotAllocator.cpp:1171,1175`, `SlotLocker.cpp:273`; `-1` as "incumbent gone" at `SlotLocker.cpp:244`, `SoakMetrics.h:141-143`; `utility = 0` for remembered-only rows at `PipelineCoordinator.cpp:366-369`; the `kOverrideUtility = 1000` sentinel (overflows under `exp`); the 0–15 widget bar; the unused "confidence" payload (`SlotUtils.h:66`).
- **Full sort:** only the top 10 are sorted (`UtilityScorer.cpp:241-249`); with full pages, slots fill from the unsorted tail.
- Update the ratio tests at `Tests.cpp:6462-6615`.

### Phase 6: the cutover (new scorer and learner)

Order inside the phase matters:

1. **Move hard zeros to `CandidateFilters` first**: spells for others with no follower near, apparel away from a workstation, torches in daylight (`ContextWeightForCandidate.cpp:25-28,346-349,415-444`), and favorites Suppress. Otherwise apparel and torches flood the page.
2. **Turn `PotionDiscriminator`'s combat timer into the `combat_onset` sensor** (`UpdateLoop.cpp:219-225`).
3. **`ChoiceLearner`** behind the existing equip bus: sparse θ keyed by a stable (needId, effectId) hash, never by position (positional keys broke once, `StateFeatures.h:264-270`); each entry {μ, σ²}; b with prior N(0, σ_b²). The choice set is the page (everything visible, including wildcard, override and Remembrance slots) ∪ the held items off the page at a learned menu cost κ ∪ "nothing pressed". u0 depends on the situation through an outside-option effect column, θ[need, out]. Update: precision += Var_p(x), step = variance. Picks in quick succession are processed in order, each removing the chosen item from the next pick's alternatives (Plackett–Luce); the repeat window still drops re-equips. This replaces the 10 s passed-over delay. Opportunity counter on need onsets next to `PipelineStateCache::Update` (`PipelineCoordinator.cpp:545`). Battery code (`RetentionAt`, `ForgetFaded`) kept, retargeted to b. Favorites Boost becomes a battery bonus.
4. **Scorer:** v = θᵀ·need once per tick, μᵢ = cap(i)·v + bᵢ, σᵢ² under the diagonal approximation, outside option; explanation = the largest θ·need·cap term. Bootstrap θ from the Phase 4 fit.
5. **Prune at cutover:** `ComputeUtility` and λ, `CorrelationBooster`, `PriorCalculator`, `PotionDiscriminator`, `ApplyPotionTierPreference`, the favorites multiplier, cold start, `fMinimumUtility`, `fMinimumContextWeight`, `IsHardContextGated`, `ContextWeightMap`, `WeightForCandidate`, `DominantReason`, `ReasonAppliesTo`, the 18-float `StateFeatures`, `BanditSubscriber` and passed-over logic, `UsageMemory` recency, the `Config.h` learning constants except `REPEAT_PICK_WINDOW_SEC`, the old `ScoreBreakdown` fields, the `[Scoring]` keys except `sFavoritesMode` and `iTopNCandidates`, and `[ContextWeights]` except `fDarkLightLevel` and `bAlcoholSatisfiesHunger`.

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

- **Cosave:** new `THTA` (version, pairs {key, μ, σ²}, u0) and `BIAS` ({formID, b, n, minutes since chosen}) records; HCID unchanged; the old `BNDW` record falls to the unknown-record branch, a clean break. `ReplaceDynamicEntries` applies to BIAS only.
- **Console:** `hg weights` → `hg theta` plus b for an item; `hg dump weights` → a θ/b dump; `hg recs` prints the top need·θ·cap terms, b and the outside option; `hg reset weights` resets θ to the bootstrap and clears b (and the dMenu button text).
- **Telemetry:** θ-drift fields in the `[Soak]` heartbeat; keep `reachIns` and `presses`.
- **Docs to rewrite or archive:** `CLAUDE.md` (formula, learner, INI, console), `docs/README.md`, `4-contextual-bandits.md` (archive), `0-pipeline.md`, `1-states.md`, `3-candidate-filtering.md`, `7-dmenu-integration.md`, `TESTING-INDEX.md`, the Nexus page (`docs/nexus/page.bbcode.txt:241-271,436`), and retire one of `roadmap.md` / `roadmap.new.md`.

### Phase 10: retire the classifiers

- Re-derive what still reads classifier output from cap: the 24 `SlotClassification` predicates (`SlotClassifier.cpp:116-335`; the enum stays, users name it in INI templates), overrides (`OverrideManager.cpp:402-640`), filters, `ExplanationLabel.h:118-125`, the ammo/charge display (`IntuitionMenu.cpp:593-670`).
- Replace `Huginn_Overrides.ini` type/tag semantics with the per-load-order actor-value override (layer 2).
- Then delete `SpellClassifier`, `ItemClassifier`, the tag enums, the scroll classifier and the per-type dumps (`ConsoleCommands.cpp:435-1370`), and replace the classifier fixtures in `Tests.cpp`.

## Decisions (the user, 2026-10-08)

- [x] **"Nothing pressed" is recorded when a need expires.** One record per need episode that resets with no press, with the page at the onset. It feeds u0 and opportunity counting.
- [x] **u0 depends on the situation:** an outside-option effect column, θ[need, out], learned like any other weight. Needs that never fire keep their starting value.
- [x] **Co-picks are processed in sequence** (Plackett–Luce). The user's reading: co-picks and sequential picks covary, and processing them in order keeps that out of the update.
- [x] **The page set includes everything visible:** wildcard, override and Remembrance slots too (P5 needs the choice set as shown). This reverses today's exclusion at `EquipSubscribers.h:69`.
- [x] **The menu is a second choice set** at a learned cost κ (theory page, P11). A menu pick teaches the item picked through its need × effect pairs; κ settles at the observed reach-in rate. The favorites-always-pass rule (`UtilityScorer.cpp:98-110`) goes: menu picks carry that evidence now. Today a menu pick already trains the item's own vector (`ExternalEquipLearner.cpp:67` → `SelectionTracker::Select`); what was missing is the need and effect attribution.
- [x] **All carried armour is gear and enters the candidate pool.** Mid-fight swaps (gloves of Destruction ↔ gloves of Two-Handed) are free in the engine today; they are not expected to recur often, so the data per armour item will be thin, and b and the shared θ carry it. A future swap cost becomes a feature or part of κ_move. Lift `ApparelClassifier`'s scope guard (`ApparelClassifier.h:11-17`).
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
