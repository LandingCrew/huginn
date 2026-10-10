# Sequences and redundancy: what was just picked, what is already up

**Status: proposal (2026-10-10). Nothing here is built.** Sections 1 to 3
describe the code at `f04c1a9` (v0.23.24, branch `engine-rewrite`) and the
selection log as it stood at 16:42 that day, and cite them by file and line.
Section 4 onwards is a proposal. Guesses are marked *(guess)*.

The user's observation (2026-10-10):

> something i noticed - learned combos? some spells immidately combo into
> others like summon minion -> circle of strength -> ice spike -> staff. might
> need some spell correleration in the Needs and/or Effects. [...] some spells
> like feather on self should be an easy bootstrap for later as well... if
> encumbered or nearing encumbered recommend it. negative correlations should
> be fine. like if summon my skeleton hero, i dont need my summon wolf. same
> with armor or cloak spells

And later: "that combo is my goto right now. it will change as i aquire new
spells".

## Summary

- **The log is consistent with a combo driven by the situation, but cannot yet
  separate it from order.** The "staff" is Staff of Wandering Stars, very
  likely a quarterstaff typed as a warhammer: an attack that costs no magicka.
  In combat it is picked at a median magicka deficit of 0.70, against 0.26 to
  0.43 for the spells. Summons, Oakflesh and damage spells come early in a
  fight. So "summon, then buff, then damage, then staff" may be largely "summon
  not up yet", then "summon up", then "magicka spent". That is need × effect,
  which R6 already fits, plus one thing the design lacks: **what is already
  active on the player**. Two cautions. The magicka deficit is produced by the
  casts before it, so pooled per-role medians cannot tell order from
  situation. And Circle of Strength does not fit the stated order (median
  57 s into the fight over 8 picks, after the quarterstaff's 45 s). The test is a held-out
  comparison in R6: the fit's lift with and without recency features
  (section 4.1).
- **Redundancy is the bigger gap, and it is already half built, as hard
  filters in the old engine.** One summon of any kind hides every summon spell,
  with no summon limit (so Twin Souls is ignored). Any armour-rating buff hides
  every armour spell. Cloaks are detected but nothing reads that. In the need
  and effect tables, the only entries that read the player's own buffs are
  `restore_pending_*` and `school_fortified`.
  **Proposed:** one cross-feature, `covered(i)`: how much of item i's effect is
  already running on the player, summons counted against the player's own
  summon limit. It gets a negative starting θ, as `restore_pending_*` does.
- **Feather on Self: the pair, the mapping and the curve are all there, but the
  grade cancels them.** The spell maps to `utility_carry_weight`. The user cast
  it at load ratios of 0.986 to 1.029, where the curve reads 0.81 to 0.96. But
  its graded strength is 0.0046, the bottom of the load order, so θ·need·cap is
  about zero. The fix belongs in R6's feature design, not the curve.
- **Learned sequences: a recency need over effect families, and only if R6 shows
  the situation alone falls short.** The proposed form is `recent_<family>`,
  "an item of this family was picked t seconds ago", built from the last pick's
  cap(i). It enters the dense block at θ = 0 and learns online like every other
  pair. Because it is keyed by effect, not by item, a new summon or a better
  damage spell inherits the pattern on its first appearance. A transition
  matrix between items is ruled out: 185 of the 227 item pairs seen so far
  occur once.

---

## 1. What the log shows

The data is `Huginn_Selections_v3.jsonl` as of 2026-10-10 16:42: 11 launches,
one LoreRim character apart from 7 Simonrim records. It holds 470 key or wheel
picks and 94 menu picks with the 18 repeats dropped, and 2,971 `nothing`
records. The last launch (`20261010-184609`) alone added 185 picks (97 key, 46
wheel, 42 menu) plus 8 repeats. The log was read with
`tools/replay/replay.py` `iter_v3`. The analysis scripts are in the session
scratchpad, not the repo.

**Picks come in bursts.** The median gap between one pick and the next in the
same load is 15 s (p25 6 s, p75 40 s). Of 552 gaps, 106 are under 5 s, 205
under 10 s and 318 under 20 s, so about 50 pairs an hour fall within 20 s.

**Item pairs are too sparse to learn.** There are 227 distinct item pairs within
20 s, and 185 of them occur once. The top pairs mostly involve the
quarterstaff (Staff of Wandering Stars): Conjure Spirit Wolf → quarterstaff 8,
Absorb Health → quarterstaff 7, Circle of Strength → quarterstaff 6,
quarterstaff → Ice Spike 6. By effect family the counts concentrate: summon
spell → weapon 24, absorb → weapon 13, restore → summon 9, defense → summon 6,
summon → damage 5. "Weapon" here is mostly the quarterstaff.

**The situation is consistent with much of the order.** These are picks made
in combat, by role:

| Role | Picks | Seconds into the fight (median) | Magicka deficit (median) |
|---|---|---|---|
| Armour spell (Oakflesh) | 13 | 23 | 0.42 |
| Damage spell (Ice Spike, Sparks) | 34 | 31 | 0.26 |
| Summon | 54 | 33 | 0.32 |
| Staff of Wandering Stars (quarterstaff) | 74 | 45 | **0.70** |
| Absorb (Circle of Strength, Absorb Health) | 29 | 64 | 0.43 |
| Staff of Sparks (a magic staff) | 10 | 90 | 0.92 |

Of the 58 runs of combat picks (gaps under 60 s), 16 open with a summon, 13 with
a damage spell and 11 with the quarterstaff. `magicka_deficit` already lists
"staff / weapon (attack that costs no magicka)" as an obvious answer
(`9-data/needs.csv:3`), and the last step of the combo may be that pair.

**What this cannot show.** The deficit at a quarterstaff pick is produced by
the casts before it. So "magicka spent → quarterstaff" and "after the spells →
quarterstaff" predict the same medians. The absorb spells also break the
stated order: Circle of Strength alone comes 57 s into a fight at the median
(8 picks in combat; the absorb group's 64 s includes Absorb Health), after the
quarterstaff's 45 s. Either it is used later in fights than the user remembers, or
the combo is one fight-opening pattern among several. Only a held-out
comparison can separate order from situation: the R6 fit with and without the
`recent_*` features (section 4.1), scored on held-out picks.

**The page already leads a little.** In 174 of the 278 key or wheel picks made
within 20 s of a previous pick, the item was already on the page in the
previous pick's context. That is the feedback loop to watch (section 4.4).

## 2. Redundancy: what exists, what is missing

### 2.1 What exists (old engine, hard filters)

| Case | Sensor | Filter | Problem |
|---|---|---|---|
| Summon | `hasActiveSummon`: any living actor whose commanding actor is the player (`StateManager_MagicEffects.cpp:537-560`; the archetype test is skipped on purpose, `:510-514`) | every `SpellType::Summon` dropped (`CandidateFilters.cpp:71`); context rule only in combat with none up (`ContextRuleEngine.cpp:365`) | A bool, not a count, and no limit: under Twin Souls (2) the second summon is hidden. Reanimated thralls count as summons. Of 90 summon picks (89 spells, 1 scroll), **1** was made with every summon hidden: the menu Wraith (menu context 01:02:19.741, pick at about 01:02:22.6, decision record 01:02:25). The definition: no summon row of any kind (spell or scroll) was eligible or shown, the chosen row included. Counting only "the chosen row is not eligible" gives 5. Of the other 4, 2 are key presses of summons that were shown although ineligible, with every other summon ineligible too (section 5, caveat), and 2 are menu picks of the Skeletal Hero made while other summons were eligible, so the summon filter was not what dropped it. |
| Armour spell | `hasArmorBuff`: any ValueModifier on DamageResist (`StateManager_MagicEffects.cpp:397-404`) | `SpellTag::Armor` dropped (`CandidateFilters.cpp:69`) | Any armour-rating buff counts, a potion included. No strength comparison, so Oakflesh up still hides Stoneflesh. |
| Cloak | `hasCloakActive` and `activeCloakType` (`StateManager_MagicEffects.cpp:495-507`) | **none**. Only the debug widget reads it (`StateManagerDebugWidget.cpp:416`) | Cloak spells are not redundant-filtered at all. |
| Invisibility, Muffle, warming (spells); Waterbreathing, Invisibility (items) | flags in `ActorBuffs` (`PlayerActorState.h:197-208`) | spells `CandidateFilters.cpp:64-75`; items `:111-116` (Waterbreathing is item-only, `:112`) | Hand-listed per tag. |
| Resists | player resistances | `IsResistSpellRedundant` / `IsResistPotionRedundant` (`CandidateFilters.cpp:79,109`) | Rule thresholds. |

These filters run before the v3 log's `eligible` flag is set
(`9-selection-log-v3.md`, flag `eligible`). A covered item is still logged, as
a `held` row with `eligible` = 0, but nothing says why. "Buff already active"
looks the same as unaffordable, on cooldown or equipped. A filtered row can
still be on the page. A Remembrance hold seats an item that is no longer a
candidate, and some rows are shown and ineligible with no hold, override or
wildcard bit at all (section 5, caveat).
- **Press and menu contexts (570),** the ones that matter for choice sets: 39
  shown rows are not eligible, 25 of them Remembrance holds, 9 of those
  unequipped summon rows.
- **All 3,195 contexts** (2,625 of them episode onsets): 318, 225 and 107.
  These are row counts, and a held item counts once per context. They come
  from only 66 distinct launch × item pairs.

So which choice set a row belongs to is read from `shown`, never from
eligibility. The fit cannot
learn redundancy from rows with no reason attached, and it does not need to
while the filters stand.

### 2.2 What the rewrite has

- **Needs (93):** none says "a buff of kind X is up". Several needs read the
  active-effect walk: `regen_suppressed` (`needs.csv:24`), the disease and
  poison flags, and `restore_pending_*` (`needs.csv:8-10`). Only the last reads
  the player's own buffs: the remaining Restore magnitude
  (`StateManager_MagicEffects.cpp:121,371,617`), paired with a **negative**
  starting θ on Restore. That is exactly the pattern redundancy needs, built
  for one family.
- **Effects (243):** cap(i) describes the item, never the player. The one
  runtime cross-feature that reads the player's active effects is
  `school_fortified` (`effects.csv:202`; `effect/CrossFeatures.cpp:44`).
- **R8 plan:** "hard zeros to `CandidateFilters` first"
  (`9-implementation-map.md:118`) moves context gates there, and the active-buff
  filters stay. Phase 10 "re-derives filters from cap" (`:147`), with no design
  for it yet.

### 2.3 Proposed: `covered(i)`, one cross-feature

For each candidate, read the player's active effects (the same walk) and map
each ActiveEffect's MagicEffect through the R2 effect mapper. That walk already
classifies MagicEffects, so the active effect and the candidate's own effect
land in the same column space. Then:

```math
\text{covered}(i) = \max_{j \in \text{specifics}(i)} \min\!\left(1, \frac{\text{active}_j}{\text{cap}_j(i)}\right) \cdot \text{left}_j
```

- `active_j` is the strongest active effect of column j (graded as cap is), and
  `left_j` its remaining-duration fraction, so a buff about to run out reads
  low and a refresh becomes possible.
- **Summons** use a count: `active_summon = living summons / limit`.
  - **The limit:** start at `float limit = 1.0f` and call
    `BGSEntryPoint::HandleEntryPoint(kModCommandedActorLimit, player, &limit)`
    (`CommonLibSSE-NG include/RE/B/BGSEntryPoint.h:81` for the entry point;
    the `static void HandleEntryPoint(ENTRY_POINT, Actor*, Args...)`
    declaration is `:112`, under its `template <class... Args>` on `:111`). The perks decide the arithmetic: an entry point can
    set or multiply the value as well as add to it, so do not assume 1 + n.
  - **The count:** walk the player's
    `MiddleHighProcessData::commandedActors`
    (`include/RE/M/MiddleHighProcessData.h:147`) instead of scanning
    ProcessLists. Check each entry: the handle resolves, the actor is alive,
    and it came from a summon or reanimate effect, not a Command spell's
    target. This drops stale handles and commanded enemies.
  - With Twin Souls and one summon up, summons read 0.5 instead of being
    hidden. Thralls versus summons: open question 3.
- **One shared weight, θ_covered**, negative from the start (an obvious pair,
  like `restore_pending`), learned like the rest. "Hero up → no wolf" is then
  `covered(wolf) = 1`.
- **Limit: a count cannot express replacement.** At the limit, every summon
  reads covered, including the hero that would replace a wolf. The `active/cap`
  ratio above applies to effect strength, not to a summon count, so "the hero
  replaces the wolf" needs its own term (open question 2): for example, the
  candidate's summon grade minus the weakest active summon's.
- **Perception:** the Active Effects menu and the summon at the player's side
  are on screen, and the player chose the perk. Inside the Core Principle.
- **The hard filters stay until θ_covered can be estimated without them.**
  Filtered items are never candidates, so while the filters stand the learner
  sees almost no summon or armour item in a covered state, and θ_covered gets
  no evidence. A **shadow evaluation** gets around this, within limits:
  - **The `shown` flag assigns the choice set, not the filter.** In doc 9's
    model a key pick chooses only from the page.
    - A filtered row with `shown` is on the page and can be picked by key, so
      it belongs to the key choice set like any shown row. Mostly these are
      Remembrance holds, though no Remembrance-held row is the chosen row of
      any of the 470 key or wheel picks. Shown, ineligible rows with no
      `remembered` bit were pressed: the Wraith in the press context at
      15:21:54.504 (flags 28) and the Spirit Wolf at 01:19:30.053.
    - A filtered row without `shown` joins only the menu alternatives
      (H \ A, at cost κ).
    - If unshown filtered rows were put into key choices, θ_covered would
      absorb "never shown", turn negative by construction, and the gate would
      pass automatically.
    - The page is already logged (`shown`, `slot`), so this needs no new
      field.
  - **What it needs logged:** `covered(i)` for every held row (the step-3
    cross-feature), and a reason on rows the filters drop, `filtered_by`
    (`summon`, `armor`, `invisibility`, `resist`...). The reason keeps
    "covered" apart from unaffordable, on cooldown and equipped, which also
    leave `eligible` = 0.
  - **Fit the replacement term jointly with θ_covered** (the summon grade
    difference above). Otherwise summon picks made with summons covered (a
    replacement, or a second slot under Twin Souls) pull θ_covered toward
    zero, when what they show is an upgrade or a second slot. **Up to 3**
    such picks are in the log. The definition: no summon row other than the
    chosen one was eligible in the pick's context, which is what the summon
    filter leaves when a summon is up. The contexts are 01:02:19.741 (a menu
    context; pick at 01:02:22), 01:19:30.053 and 15:21:54.504 (press
    contexts). Section 2.1's count of 1 is a stricter
    definition (nothing eligible or shown).
  - **The evidence is thin.** There are 94 menu picks in total, and only the
    few made with something covered speak to θ_covered. If after R8's data
    play the estimate's interval still includes zero, **keep the filters** as
    they are, with the summon fix. The alternative, removing them on the
    negative starting θ alone, rests on no evidence: your call (open
    question 8).
  - **The interval must come from the evidence, not the prior.** Doc 9 starts
    obvious pairs at a nonzero value (`9-context-as-learner-input.md:355`), so
    a posterior for θ_covered could be confidently negative with no evidence
    at all. The shadow estimate therefore uses a zero-centred prior, or a
    likelihood-only interval (profile likelihood or bootstrap over launches).
    The negative start applies to the live learner only.
  - The filters go once that evidence-only estimate is confidently negative.
  - One exception: fix the summon filter's count and limit as soon as the
    sensor exists, because it is a bug today.

Same-effect buffs that do not stack (Oakflesh and Stoneflesh) fall out of the
same column. Different columns that stack (armour plus a cloak) do not cover
each other.

## 3. Feather on encumbrance

| Piece | State | Where |
|---|---|---|
| Need | `encumbrance` = inventoryWeight / carryWeight, logistic c 0.95, slope 40 | `needs.csv:26`; sensor `StateManager_Position.cpp:325-348`, `NeedEvaluator.cpp:78` |
| Mapping | Feather on Self (`8D005B42`) → `utility_carry_weight`, by actor value | `EffectRules.cpp:865` (`CarryWeight`); name `:300` |
| Obvious pair | encumbrance × `utility_carry_weight` | `effects.csv:142` |
| User's casts | 5 picks (4 from the magic menu, 1 key) at ratio 0.986, 1.029, 1.020, 1.006, 1.005; need 0.81 to 0.96 | v3 log, 20:21 to 20:31 UTC |
| Graded strength | **0.0046** for both `utility_carry_weight` and the `utility` family | v3 cap |

- **The curve fits this player's behaviour.** Every cast came at or past the
  limit. At 0.89 the curve reads 0.08, which is what the user saw. "Nearing
  encumbered" at 0.89 is not supported by these 5 casts. Rather than move c by
  hand, fit it from the player's carry-weight picks, as `drop_ahead` was fitted
  from falls (`needs.csv:69`, `tools/needs/fit_drop_curve.py`), or learn it
  through the logistic-basis option doc 9 gives for the vitals
  (`9-context-as-learner-input.md:149`).
- **The blocker is the grade.** 0.0046 = 1/217: the bottom of the load order's
  carry-weight population, either its lowest value
  (`EffectMapper.cpp:898`) or a zero magnitude, which grades at 1/(N+1)
  (`:211`). Carry weight is a Level column, graded as magnitude ×
  duration / 3600 for timed effects and as the full magnitude for constant ones
  (`EffectMapper.cpp:514-516`). A short self-cast spell therefore ranks below
  every enchanted ring and potion *(guess: Feather's record holds a modest
  magnitude for well under an hour, or none; confirm with `hg cap 8D005B42`)*.
  With need 0.9 the pair contributes 0.9 × 0.0046 × θ, which is nothing.
- **It is not only Feather.** Circle of Strength reads `absorb` 0.006, Absorb
  Health 0.011 and Oakflesh `defense_armor` 0.11. Spells sit at the bottom of
  populations shared with potions and constant enchantments.
- **Recommendation (an R6 design choice, no code):** let the pair term read
  presence, need_k × 1[cap_j(i) > 0], and give strength its own shared weight,
  need_k × (cap_j(i) − 0.5). Presence is recoverable from the logged caps, so
  the fit can test it on today's data. The alternative, grading within kind
  (spells against spells; `Populations` already takes a group,
  `EffectMapper.h:250`), is an R2 change that regenerates fixtures.
- **Compare before switching.** Presence plus strength changes every pair in
  the model, and the evidence for it is one Feather case and a few
  low-graded spells. R6 should fit both gradings, the current one and presence
  plus strength, and switch only if the new one wins on held-out picks.
- **Feather probably limits itself** *(guess)*: once cast, carry weight rises,
  the ratio falls and the need drops. That holds only if one cast brings the
  load below about 0.95. If it does not, the need stays high while Feather is
  active, and Feather needs the covered term like any buff.

## 4. Positive sequences

### 4.1 Options

| Option | Signal | Fits as | Verdict |
|---|---|---|---|
| A. State only | needs (combat onset, magicka), `covered(i)` | what R6 fits, plus section 2.3 | **First.** Section 1 is consistent with it explaining much of the combo, and it adapts to new spells for free |
| B. Recency need | `recent_<family>` = presence of family f in the **last pick's** cap × decay(seconds since) | new needs in the dense block, θ starting at 0 | **Second, if R6 shows lift over A** |
| C. Item transition matrix | P(next item ∣ last item), fitted offline | a per-item-pair term | Rejected: sparse (185 of 227 pairs seen once), and it does not survive a new spell list |
| D. "Just cast X" scorer bonus | hand rule | scorer term | Rejected: a hand-tuned shim, which the debt stance rules out |

**The deciding test** (R6): fit A, then A plus B, on the same launches, and
score both on held-out launches. Report key-pick hit@1 and log-likelihood, menu
picks, and the picks whose item was not on the page at the previous pick. B
goes into R8 only if it adds lift there. This is also the only test that can
separate order from situation (section 1).

### 4.2 B in detail

- **Families as roles.** Use the 26 family columns (`summon`, `damage`,
  `absorb`, `defense`, `restore`, `aura`...) plus the kind features
  (`kind_weapon`, `kind_potion`). The roles are derived from cap(i), never
  hand-listed per item. Pick-level presence: `recent_f = 1[cap_f(last) > 0] ·
  exp(−Δt / τ)`.
- **τ comes from the data, not a hand value:** a basis of two decays (5 s and
  30 s, roughly p25 and p75 of the gaps), each its own need, weighted by θ.
  Doc 9 gives the same option for curve centres.
- **It is a need, so the existing machinery applies unchanged:** episodes and
  "nothing pressed" (`NeedEpisodes.h`), opportunity counting, θ variance
  feeding the wildcards (R10), and the challenger rule (R9). A `recent_summon`
  episode that expires with a damage spell shown and unpressed is a negative
  for (`recent_summon`, `damage`).
- **Size:** ~28 recency needs × the item families is ~800 pairs in the dense
  block, all starting at 0 and held there by shrinkage. Only pairs with
  evidence move. At ~50 in-window pairs an hour that is a handful of live
  pairs per session, which is what the evidence supports.
- **Plackett–Luce already handles part of it.** Co-picks are processed in
  order, and each chosen item leaves the next pick's alternatives
  (`9-implementation-map.md:120,155`). `recent_*` adds "after a summon, what?"
  and nothing more.
- **Perception:** the player knows what they just equipped. A pick is the
  player's own input, so it is fine to read.
- **The old engine's opposite goes:** `UsageMemory` boosts an item used three
  times in the same context, which favours repeats (`UsageMemory.h:26-38`). It
  is on R8's prune list.

### 4.3 Adapting as the spell list changes

- **No fixed transitions.** Nothing is keyed by FormID except bᵢ. A new summon
  carries `summon` in its cap, so it inherits every (`recent_*`, `summon`) and
  (`recent_summon`, `*`) weight on first sight: zero fights to join the combo.
- **A new family** (the first cloak, say) starts at θ = 0 for its recency pairs.
  It enters by need × effect (`aura` × `combat_onset`, `enemy_close`), by menu
  picks, which teach its pairs at the learned menu cost κ, and by R10's
  uncertainty wildcards.
- **Retired spells fade by themselves.** Their caps stop appearing, and the
  shared θ is tracked online (doc 9 "Tracking, not converging": the step size is
  the knob, variance grows per opportunity). A pattern that stops getting
  evidence stops moving. Only bᵢ uses the battery.
- **Speed** *(guess, to measure in replay)*. A combo step happens about once per
  fight. With a variance-scaled logistic step, a pair at θ = 0 needs roughly
  5–10 consistent observations to carry a page position, so about 5–10 fights
  for a new family's transition and none for a new spell in an old family.
  Replay can measure it: re-run the online update over the log, and count the
  fights after a spell's first pick until its family pair ranks it on the page.

### 4.4 The feedback loop

The risk: Huginn shows B after A, the player presses B because it is there, and
θ(A→B) grows. Four guards, all already in the design:

1. **The choice model conditions on the page.** A key pick is evidence for B
   only against the other items shown (theory page P5). Showing B creates no
   evidence by itself; choosing it over the alternatives does.
2. **"Nothing pressed" is negative evidence.** B shown during a `recent_A`
   episode and not pressed counts against the pair. The old engine's
   passed-over updates never covered that case.
3. **Menu picks carry the new combo.** When the player reaches into the menu
   for a new spell after A (36 of the 40 menu picks within 20 s of a previous
   pick were not on the page at it), that pick teaches (`recent_A`, new
   family) at cost κ. This is how a new go-to replaces the old one.
4. **Judge on the picks the page did not steer:** menu picks and the "B not
   shown at A" subset (104 key or wheel picks, 84 of them key, and 36 menu
   picks so far), not the overall
   hit rate. This is doc 9's 23-point lesson.

## 5. Data

**What v3 already records** (`9-selection-log-v3.md`):

- every decision with `utc`, `seq`, `gen` and `char`, so the previous pick and
  the seconds since it can be rebuilt offline from consecutive `dec` records,
  with no format change;
- the chosen row's cap, so the previous pick's families are known;
- the page at each pick (`shown`), the outcome, and `nothing` per need episode.

**Missing:**

1. Active effects and summons: no need or cross-feature. Covered items are
   logged as `held` rows with `eligible` = 0, but with no reason. Add `covered`
   as a cross-feature at the end of `cross` (a v3-compatible addition), a
   `summon_room` input, and a `filtered_by` reason on rows the active-buff
   filters drop (section 2.3).
2. The summon limit (the perk entry point) and the summon count.
3. A recency need on the live tick. The fit can derive it offline; R8 needs it
   live.
4. Casts. A spell pick is an equip, not a cast. For this question the equip is
   the decision, so this is fine *(guess: casts would mostly add noise)*.

**Caveat for R6: the chosen row's eligibility has no pre-press value.**

- **`equipped` vs `eligible`.** For `equipped` the schema supplies
  `dec.preEquipped`, which is unknown only in the first 0.25 s after a load.
  `eligible` has no such pre-press field. It is the eligible set as of the
  last `SelectionLogV3::Tick`, which can be one pipeline run behind the page
  (next bullet).
- **The page can be one run newer than the eligibility.**
  - `PipelineStateCache::Update` writes both together
    (`PipelineStateCache.h:135-182`).
  - A context reads them at different times. `eligible` comes from `g_tick`,
    a copy taken in `SelectionLogV3::Tick` (`SelectionLogV3.cpp:1085-1087`,
    called at `UpdateLoop.cpp:656`).
  - The page is read when the context is taken (`TakeShown`,
    `SelectionLogV3.cpp:484`), and the pipeline runs after Tick
    (`UpdateLoop.cpp:670`).
  - So a press or menu context can show a page one run newer than its
    eligibility flags. Onset contexts are built inside Tick and cannot be
    skewed.
  - The finding below still holds: the onset context at 15:21:54.525 also
    shows the Wraith shown and not eligible (flags 28, slot 0).
- **Two key presses of shown, ineligible summons.** In the press contexts at
  01:19:30.053 and 15:21:54.504, the pressed summon was shown and not
  eligible, with no `remembered`, `override` or `wildcard` bit (flags 28,
  `preEquipped` 0, `ctxAgeMs` 0). Every other summon row was ineligible and
  unequipped too, which fits the summon filter with a summon already up.
  These are context times; the decision records, stamped at the confirmation
  about 3 s later, read 01:19:33 and 15:21:57.
- **Not the press's own equip.** The Wraith was already ineligible and
  unequipped in the context at 15:21:40.867, before its press.
- **How an ineligible item stays shown: a slot lock.**
  - The allocator places only real candidates as Normal. Items present only
    for a hold go in as Remembered (`SlotAllocCore.h:446,788,840`;
    `SlotAllocator.cpp:499-508`).
  - So the only route is `SlotLocker` restoring a locked assignment
    unchanged, its type included (`SlotLocker.cpp:175-182`).
  - A locked hold would keep its `remembered` bit (`SelectionLogV3.cpp:572`),
    so this was a lock, not a hold.
  - The Wraith was therefore a candidate within the lock window before that
    page's run: 3 s by default (`fLockDurationMs`, `configs/Huginn.ini:890`),
    or 10 s for a Wheeler activation lock (`SlotLocker.cpp:352-366`).
  - No logged context between 15:21:40.867 and 15:21:54.504 lets us check
    which.
- **What an analysis keyed on eligibility must do:**
  - Treat the chosen row's `eligible` as unknown.
  - Expect a press or menu context's eligibility to lag its page by up to one
    pipeline run (onset contexts do not).
  - Assign choice sets by `shown`, knowing that the key choice set then
    includes lock-held items: shown, but possibly no longer candidates.

**Volume.** R6 needs about 450–1,200 key or wheel picks to see a +2 to +4 hit@1
gain (roadmap R5 notes). The log now holds 470 key or wheel picks, 143 of them
from the last launch alone. Sequence pairs come from the same
picks: about 50 within-20 s pairs an hour, 318 so far. The 10 commonest role
pairs cover 91 of them. To estimate ~10–20 live family pairs at ~10–20
observations each takes about 300–600 in-window pairs, so 6–12 hours of play.
That is in R6's range, so **no separate data phase** *(guess on the
per-pair count)*. An item-level matrix would need an order of magnitude more
(91 items picked, 8,281 ordered pairs), and it resets with every new spell.

## 6. Recommended minimal path

| Step | What | Milestone | Gate |
|---|---|---|---|
| 1 | In the fit, test the pair term on presence plus a separate strength term (section 3). It fixes Feather and every under-graded spell | R6 | Agent; replay |
| 2 | In the fit, add offline `recent_<family>` features from consecutive decisions; report the lift over the plain fit on held-out key picks, menu picks and the "not shown at A" subset | R6 | Agent; **you decide** whether B goes into R8 |
| 3 | Sensors, logged only: the summon count and limit, `covered(i)` from the active-effect walk through the effect mapper, written as a v3 cross-feature on every held row, and a `filtered_by` reason on the rows the active-buff filters drop | between R6 and R8 (an R3-style PR) | Agent, then a short session in game |
| 4 | The summon filter counts against the limit (the Twin Souls bug) | with step 3 | Agent |
| 5 | θ_covered (negative start) and, if step 2 passes, the live `recent_*` needs in the scorer; the active-buff filters stay as a fallback | R8 | Agent, then **in game (you)** |
| 6 | Shadow evaluation: estimate θ_covered offline, jointly with the summon replacement term. Choice sets come from `shown`: a shown `filtered_by` row is a key alternative, an unshown one a menu alternative only. Use a **zero-centred prior or a likelihood-only interval**, so the nonzero starting value cannot pass the gate by itself. Retire the active-buff filters once that estimate is confidently negative; if the evidence stays too thin, keep them (open question 8) | R11 (Phase 10 "filters from cap") | Agent; replay |

Steps 1–2 need no game code and no new play. Steps 3–4 are the only work before
R8.

## 7. Open questions for the user

1. **Feather threshold:** your five casts were at 99–103% load. Is "nearing" a
   real want, say 90%? If so, fit the curve from your carry-weight picks
   (recommended), or does the shipped c 0.95 stand?
2. **Summon upgrades:** with the wolf up and one summon allowed, should Huginn
   ever offer the Skeletal Hero as a replacement? A count against the limit
   cannot express this: at the limit every summon reads covered. It would need
   a separate replacement term (section 2.3).
3. **Thralls:** does a reanimated corpse or a Dead Thrall use up the summon
   limit for your purposes? The old sensor counts it.
4. **Refresh:** should a buff with a few seconds left read as uncovered (the
   remaining-duration factor), or is a refresh always a menu action for you?
5. **Grading:** presence plus a strength term in R6 (no code), or grading
   spells within spells in R2 (fixtures regenerate)? Either way it changes every
   pair on thin evidence (one Feather case), so R6 should compare it with the
   current grading on held-out picks before switching.
6. **Sequences into R8 only on R6 evidence**, or build `recent_*` regardless,
   at θ = 0 so it is harmless?
7. **Window:** is 5–30 s the right horizon for "just cast", or do some of your
   combos span a whole fight (buff at the start, finisher at the end)? A whole
   fight is `combat_onset`'s job, not this need's.
8. **Too little evidence to retire the filters:** if the shadow estimate of
   θ_covered stays inconclusive (94 menu picks so far, few of them with
   something covered), keep the active-buff filters (recommended), or drop
   them and rely on the negative starting θ alone?
