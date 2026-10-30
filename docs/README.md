# Huginn - Architecture

*The right tool, right when you need it*

## Vision

A **contextual quick-access system** that surfaces the right spell, potion, weapon, or item exactly when you need it - without menu diving.

That's it.

It is not:
- an autonomous agent
- a planner
- a decision-maker that replaces the player

It is a just-in-time affordance surface.

> **Core Principle:** Only recommend based on information the player already has or could easily perceive. This is a convenience tool, not a cheat tool.

> **Architecture version:** verified against **v0.24.4** (R11, `CMakeLists.txt:5`), the engine rewrite on the `rewrite-r8` integration branch: the hand-tuned `ctx × (1 + λ·learn) × multipliers` engine was replaced by a learned choice model over needs and effects ([doc 9](architecture/9-context-as-learner-input.md), its [implementation map](architecture/9-implementation-map.md), the [roadmap](roadmap.md)'s R0-R12). The old learner's page is [archived](architecture/archive/4-contextual-bandits.md).

---

## The Problem

| Situation | Vanilla UX | With Huginn |
|-----------|------------|------------------------|
| Need specific spell from 50+ known | 3-15s menu dive | Widget + Wheeler show top recommendations |
| Weapon out of charge mid-combat | 5-20s finding (best) soul gem | "Press [hotkey] Recharge" |
| Standing in fire | 20s finding resist potion | "Press [hotkey] Resist Fire" |
| Out of mana, enemy in melee range | Menu -> Weapons -> Equip | "Press [hotkey] Equip Sword" |
| Looking at locked chest (Adept) | Menu -> Magic -> Find spell | "Press [hotkey] Open Lock" |
| On cliff edge, looking down | Hope you prepared earlier | "Press [hotkey] Slow Fall" |

> **Note:** Number of hotkeys is configurable (up to 10 per page, up to 10 pages). Integration with [Wheeler](https://www.nexusmods.com/skyrimspecialedition/mods/97345), via [wheelerAPI](https://github.com/LandingCrew/wheelerAPI), which exports the API base Wheeler lacks, provides radial menu support.
> **Note:** The issue is not searching the menus or their duration; it is the frequency of access, the disruption when you are in a flow state.

How Huginn is measured (the user, 2026-10-01): **did the player reach into the inventory** for something Huginn could have offered, and **did they need a labelled slot or a custom page** to get it. Both are counted in the `[Soak]` heartbeat (`goals`).

---

## The engine, one tick

```
Game (SKSE)
  |  StateManager: 11 polls -> 6 state types (world, player, targets, 3 trackers)   [observable state only]
  v
NeedMonitor: the need vector n -- K needs (health_deficit, poisoned, underwater, drop_ahead, encumbered, ...),
  each a response curve over a sensor ([Needs], docs/architecture/9-data/needs.csv)
  |
Registries: what the player knows and carries (spells, alchemy, scrolls, weapons/ammo, apparel, torches)
  |  CandidateGenerator
  v
Candidates --> CandidateFilters: hard gates (no follower, no workstation, not dark, suppressed favourite),
  |            full vitals, active buffs (with the refresh pass), redundant resists, the summon limit
  |            -- each read off the candidate's FACETS (core/CapFacets.h over cap(i))
  v
D2 legacy pass (bLegacyLearnFeature): the old learner's ln learn mult on the rows the old floors keep
  |
RankOnChoice: every candidate ranked on mu = sum_j (sum_k n_k theta[k][j] + recent) a_j + b + theta_cov covered + w lm
  |            a = cap(i) (EffectCatalog) + runtime cross-features (overshoot, charge, stack, ...)
  v
SlotAllocator: pages and slots; slot classes and the class cap from facets; one wildcard key per page
  |            (relevance x sd, R10); overrides (urgent restore, cure, ammo, waterbreathing, soul gem)
SlotLocker: the hold -- a challenger replaces a key only past Delta / sigma_Delta (R9); Remembrance; home keys
  v
Display: Intuition widget (Scaleform) + Wheeler wheels; the explanation label (the largest theta n cap term)
  |
Logs: [Recs], [Soak], the selection log v3 (one row per candidate per decision: the fit's data)
```

Picks come back through the equip event bus: a press on a key, a Wheeler pick, a menu pick. `ChoiceLearner::OnPick` updates θ (the need × column weights of what was picked and of what was passed over on the page; Plackett-Luce for co-picks) and b, with the variance as the step; a need onset the page answered grows its need's variances (opportunity counting); the menu is a second choice set at a learned cost κ.

### What an item is: the effect catalog

`EffectCatalog` reads every spell, scroll, potion, food, poison, weapon, ammo, armour piece, soul gem and light in the load order when the main menu first opens (in slices, one per frame), maps each effect (MGEF) to a column of [effects.csv](architecture/9-data/effects.csv) and grades it against the load order's population: cap(i), 244 columns (26 families, 136 specifics, modifiers, item features, weapon and armour stats). Tempered and player-enchanted stacks get a per-instance cap; player-made potions and spells no tome teaches are mapped late. `hg dump all` writes the catalog's view of the whole load order; `hg cap <FormID>` one item.

Since **R11** the live engine asks every yes/no question of an item through this catalog, in one place, `src/core/CapFacets.h`: the slot classes (`sClassification = HealingAny` still names the same classes; their rules are re-derived from cap(i), reviewed in [5-slots.md](architecture/5-slots.md)), the candidate filters, the override finders, the "Ally Support" label and the widget's potion styling.

**Effect overrides**, the per-load-order layer (doc 9's layer 2): a mod ships `Data/SKSE/Plugins/Huginn/EffectOverrides/<name>.ini`, the player keeps `Data/SKSE/Plugins/Huginn_EffectOverrides.ini` (applied last); each line pins an effect to a column by plugin and local FormID (`MyMod.esp|0x800 = resist_frost`), under `[Overrides]`, with `[Huginn] iFormatVersion = 1`. A file that cannot be read, or is for a newer format, is skipped and named; a malformed line is ignored and counted; entries for plugins not active in the load order are skipped quietly (`src/core/OverrideFiles.h`).

### What the situation needs: the need vector

Each need is a curve (`linear`, `logistic`, `gaussian`, ...) over a sensor reading -- a vital's deficit, a damage rate, a timer since combat began, the drop ahead of the player, encumbrance, the light level, survival stages -- in [0, 1]. `hg needs` prints the non-zero ones. The R6 bootstrap may carry its own curve for a need (encumbrance does), which then overrides the INI line.

### The learner

`ChoiceLearner` holds the model (`src/core/ChoiceScore.h`, `src/core/ChoiceUpdate.h`):
- θ[k][j]: one Gaussian belief {μ, σ²} per need × effect column (and per `recent_<family>` × column), shared by every item that carries the column;
- b(i): one belief per item, prior N(0, 0.25), with a battery: it fades with play time since the item was last chosen;
- the discovery boost (0.24.12, `src/core/DiscoveryBoost.h`): a spell learned or an item gained for the first time on this character reads b's variance × m(age) = 1 + (M − 1)·2^(−age/H) in the wildcard draw only (`[Learning] bDiscoveryBoost`, `fDiscoveryBoostVarMult` M = 4, `fDiscoveryBoostHalfLifeSec` H = 1 h of play), until it decays or the item's first pick ends it. What a load finds is never boosted. The mean, the challenger rule and the summon gate read the unboosted posterior (a larger σ would make a challenger take a key later, and would move μ at the summon limit);
- κ, the feature weights and the outside option u0 (θ[need, out] and an intercept): fixed by the file (format v1 gives them no precision).

It starts from `Huginn_Bootstrap.json`, R6's offline fit on the logged decisions ([9-bootstrap-format.md](architecture/9-bootstrap-format.md)), and is saved in the cosave as THTA (θ by stable key, only the weights that moved), BIAS (b per form) and DISC (the forms this character has held, and the boosted ones). `hg reset weights` returns it to the bootstrap. `hg theta` shows it: the drift from the bootstrap, and one item's terms.

### D2: the one part of the old engine kept

R6 (the offline fit, [roadmap](roadmap.md) R6) found that no variant without the old learner passed its gate, and chose **D2**: θ + b + recent **plus the old learner's learning score**, `ln learn mult = ln(1 + λ(α)·learningScore)`, on the rows the old floors keep, weight 1.36. So behind `[Scoring] bLegacyLearnFeature` (default on) the old engine still runs, in `src/learning/legacy/`, for that one feature: the FeatureBanditLearner (an 18-float contextual bandit, [archived page](architecture/archive/4-contextual-bandits.md)), UsageMemory, the prior, correlations and potion multiplier, the old context rules as a binary gate, the floors and the cold start, and -- because the context rules read their tags -- the old spell, item and scroll classifiers ([2-classifiers.md](architecture/2-classifiers.md)). None of it ranks or places anything by itself; `tools/prune/r8_check.py` confines it to its files (LEGACY, and CLASSIFIER since R11) and its mutation probes prove the check bites. It retires when a refit on newer logs passes the gate without it. Without a usable bootstrap the pipeline falls back to ranking on the old utility, and says so.

---

## Feedback and telemetry

- **The selection log v3** ([9-selection-log-v3.md](architecture/9-selection-log-v3.md)): one decision per pick or need episode, every eligible candidate a row (its cap, cross-features, covered, μ, sd, b, the D2 feature, the filter that dropped it), the need vector, the page as shown. The offline fit (`tools/fit`) and the replay (`tools/replay`) read it.
- **`[Soak]`** every 5 minutes ([playtest/LongPlaySoak.md](playtest/LongPlaySoak.md)): equips and accepts, slot churn and its causes, the goals (reach-ins, presses), the reach split, the choice learner's counts, rates (pipeline runs, wildcard share, Wheeler pushes, wildcards rolled / relevant / picked), churn per press, and since R11 **θ drift**: weights whose mean moved, the mean and largest |μ - μ_boot| and where, the mean ln(σ² / σ²_boot), b's spread, θ_covered.
- **`[Recs]`** (`hg recs`): the top of the ranking, each with μ, sd, its three largest terms, b and the D2 feature; the header carries the outside option u0.

## Settings management

`Data/SKSE/Plugins/Huginn.ini` holds every section; dMenu edits a second INI for the panels it exposes (`[Widget]`, `[Debug]`, ...) and `hg reload` / the dMenu "Reload INI" button re-read both ([7-dmenu-integration.md](architecture/7-dmenu-integration.md)). CLAUDE.md lists the sections.

---

## Documentation map

| Document | Contents |
|----------|----------|
| [9-context-as-learner-input.md](architecture/9-context-as-learner-input.md) | The engine's design: context as the learner's input (shared need × effect weights, response curves) |
| [9-implementation-map.md](architecture/9-implementation-map.md) | What was built, phase by phase (Phases 0-10, with "as built" notes) |
| [9-bootstrap-format.md](architecture/9-bootstrap-format.md) | The bootstrap file (format version 1): θ, u0, κ, recent, b, curves; the score and the reader's rules |
| [9-selection-log-v3.md](architecture/9-selection-log-v3.md) | The selection log v3 format |
| [11-sequences-and-redundancy.md](architecture/11-sequences-and-redundancy.md) | covered(i), the refresh prior, the summon limit, anticipatory resists |
| [0-pipeline.md](architecture/0-pipeline.md) | The tick in detail (pre-rewrite body; the banner says what changed) |
| [1-states.md](architecture/1-states.md) | State models and StateManager polling |
| [2-classifiers.md](architecture/2-classifiers.md) | The old spell / item / scroll / weapon classifiers: since R11 an input to D2 only |
| [3-candidate-filtering.md](architecture/3-candidate-filtering.md) | Candidate generation and filtering |
| [5-slots.md](architecture/5-slots.md) | Slots, classes (re-derived from cap(i) in R11), overrides, wildcards, multi-page, the hold |
| [6-ui-ux.md](architecture/6-ui-ux.md) | Intuition widget and Wheeler integration |
| [7-dmenu-integration.md](architecture/7-dmenu-integration.md) | dMenu integration and the two-INI architecture |
| [8-future-work.md](architecture/8-future-work.md) | Deferred ideas |
| [10-client-api.md](architecture/10-client-api.md) | Proposal: Huginn as a recommendation server |
| [archive/4-contextual-bandits.md](architecture/archive/4-contextual-bandits.md) | ARCHIVED: the old learner (FeatureBanditLearner), kept for D2's arithmetic |

### Reference

| Document | Contents |
|----------|----------|
| Console commands | The table in CLAUDE.md is the reference |
| [candidate-system.md](reference/candidate-system.md) | Candidate registries and generation |
| [Performance.md](reference/Performance.md) | Performance budget and measurements |
| [classifier-coverage.md](reference/classifier-coverage.md) | How much of each load order the old classifiers typed (historical; the catalog's coverage is in `hg dump all`'s summary) |
| [intuition-scaleform-build.md](reference/intuition-scaleform-build.md) | Building `Intuition.swf` from `src/swf/Intuition.as` |
| [WheelerAPI_minimal.h](reference/WheelerAPI_minimal.h), [WheelerAPIClient.h](reference/WheelerAPIClient.h) | Vendored Wheeler API headers for reference |

### Elsewhere

| Path | Contents |
|---|---|
| [Compatibility (wiki)](https://github.com/LandingCrew/huginn/wiki/Mod-Compatibility) | LoreRim, Survival Mode, unknown-spell handling, general mod compatibility |
| [testing/](testing/) | Test index (host tests, in-game suites, the unattended runner) and profiling guide |
| [refactor/](refactor/) | Wheeler push spikes |
| [profiling/](profiling/), [playtest/](playtest/) | Tracy captures, long-play soak protocol and reports |
| [roadmap.md](roadmap.md) | Open work |

---

## Provenance and staleness

**Read this before trusting a deep-dive doc.** Much of `architecture/` was recovered on 2026-08-29 from an untracked snapshot describing v0.13-v0.14, and several pages predate the engine rewrite. Where a doc and the code disagree, the code is right.

Maintained alongside the work, so current:

- This page and CLAUDE.md, verified against v0.24.4 (R11)
- [roadmap.md](roadmap.md) and the doc 9 set (9-*.md, 11-*.md)
- [5-slots.md](architecture/5-slots.md)'s R11 section on the slot classes
- [profiling/tracy-traces.md](profiling/tracy-traces.md), [playtest/LongPlaySoak.md](playtest/LongPlaySoak.md), [playtest/Soak-2026-10-LoreRim.md](playtest/Soak-2026-10-LoreRim.md)

The pre-rewrite pages (0, 1, 3, 6, 7, 8 and the reference pages) carry an R11 banner saying what of them the rewrite changed; their body text still describes the old engine where it speaks of context weights, utility or the bandit.
