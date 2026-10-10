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

- **Most of the combo comes from the situation, not from the order.** In the
  log, the staff (a two-handed warhammer, not a magic staff) is picked in
  combat at a median magicka deficit of 0.75, against 0.26 to 0.43 for the
  spells. Summons, Oakflesh and Ice Spike come early in a fight. So "summon,
  then buff, then damage, then staff" is largely "summon not up yet", then
  "summon up", then "magicka spent". That is need × effect, which R6 already
  fits, plus one thing the design lacks: **what is already active on the
  player**.
- **Redundancy is the bigger gap, and it is already half built, as hard
  filters in the old engine.** One summon of any kind hides every summon spell,
  with no summon limit (so Twin Souls is ignored). Any armour-rating buff hides
  every armour spell. Cloaks are detected but nothing reads that. The need and
  effect tables have nothing that reads active effects, apart from
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
nearly all LoreRim. It holds 470 key or wheel picks and 94 menu picks (repeats
dropped), and 2,969 `nothing` records. It was read with `tools/replay/replay.py`
`iter_v3`. The analysis scripts are in the session scratchpad, not the repo.

**Picks come in bursts.** The median gap between one pick and the next in the
same load is 15 s (p25 6 s, p75 40 s). Of 552 gaps, 106 are under 5 s, 205
under 10 s and 318 under 20 s, so about 50 pairs an hour fall within 20 s.

**Item pairs are too sparse to learn.** There are 227 distinct item pairs within
20 s, and 185 of them occur once. The top pairs all end on the staff: Conjure
Spirit Wolf → Staff 8, Absorb Health → Staff 7, Circle of Strength → Staff 6,
Staff → Ice Spike 6. By effect family the counts concentrate: summon spell →
weapon 24, absorb → weapon 13, restore → summon 9, defense → summon 6, summon →
damage 5.

**The situation explains much of the order.** These are picks made in combat,
by role:

| Role | Picks | Seconds into the fight (median) | Magicka deficit (median) |
|---|---|---|---|
| Armour spell (Oakflesh) | 13 | 23 | 0.42 |
| Damage spell (Ice Spike, Sparks) | 34 | 31 | 0.26 |
| Summon | 54 | 33 | 0.32 |
| Absorb (Circle of Strength, Absorb Health) | 29 | 64 | 0.43 |
| Staff of Wandering Stars (warhammer) | 89 | 59 | **0.75** |

Of the 58 runs of combat picks, 16 open with a summon, 13 with a damage spell
and 12 with the weapon. `magicka_deficit` already lists "staff / weapon (attack
that costs no magicka)" as an obvious answer (`9-data/needs.csv:3`). The last
step of the combo is that pair.

**The page already leads a little.** In 174 of the 278 key or wheel picks made
within 20 s of a previous pick, the item was already on the page in the
previous pick's context. That is the feedback loop to watch (section 4.4).

## 2. Redundancy: what exists, what is missing

### 2.1 What exists (old engine, hard filters)

| Case | Sensor | Filter | Problem |
|---|---|---|---|
| Summon | `hasActiveSummon`: any living actor whose commanding actor is the player (`StateManager_MagicEffects.cpp:535-558`; the archetype test is skipped on purpose, `:510-514`) | every `SpellType::Summon` dropped (`CandidateFilters.cpp:71`); context rule only in combat with none up (`ContextRuleEngine.cpp:365`) | A bool, not a count, and no limit: under Twin Souls (2) the second summon is hidden. Reanimated thralls count as summons. 5 of the 90 summon picks in the log were made while the filter hid every summon. |
| Armour spell | `hasArmorBuff`: any ValueModifier on DamageResist (`StateManager_MagicEffects.cpp:397-404`) | `SpellTag::Armor` dropped (`CandidateFilters.cpp:69`) | Any armour-rating buff counts, a potion included. No strength comparison, so Oakflesh up still hides Stoneflesh. |
| Cloak | `hasCloakActive` and `activeCloakType` (`StateManager_MagicEffects.cpp:495-507`) | **none**. Only the debug widget reads it (`StateManagerDebugWidget.cpp:416`) | Cloak spells are not redundant-filtered at all. |
| Invisibility, Muffle, Waterbreathing, warming | flags in `ActorBuffs` (`PlayerActorState.h:197-208`) | `CandidateFilters.cpp:64-75`, items `:108-116` | Hand-listed per tag. |
| Resists | player resistances | `IsResistSpellRedundant` / `IsResistPotionRedundant` (`CandidateFilters.cpp:79,109`) | Rule thresholds. |

These filters run before the v3 log's `eligible` flag (`9-selection-log-v3.md`,
flag `eligible`). So in the fit data a covered item is simply missing from the
choice set. The fit cannot learn redundancy from that data, and it does not need
to while the filters stand.

### 2.2 What the rewrite has

- **Needs (93):** none says "a buff of kind X is up". The one active-effect
  need is `restore_pending_*` (`needs.csv:8-10`), which reads the remaining
  Restore magnitude in the active-effect walk (`StateManager_MagicEffects.cpp:121,371,617`)
  and pairs with a **negative** starting θ on Restore. That is exactly the
  pattern redundancy needs, built for one family.
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
- **Summons** use a count: `active_summon = living summons / limit`. The limit
  is 1 plus the perk entry point `kModCommandedActorLimit`
  (`CommonLibSSE-NG include/RE/B/BGSEntryPoint.h:81`). Count the player's
  `MiddleHighProcessData::commandedActors` (`include/RE/M/MiddleHighProcessData.h:147`)
  instead of scanning ProcessLists. With Twin Souls and one summon up, summons
  read 0.5, not hidden. Thralls versus summons: open question 3.
- **One shared weight, θ_covered**, negative from the start (an obvious pair,
  like `restore_pending`), learned like the rest. "Hero up → no wolf" is then
  `covered(wolf) = 1`. Whether "wolf up → hero is an upgrade" is learnable
  depends on the strength term: with wolf below hero, `active/cap` < 1.
- **Perception:** the Active Effects menu and the summon at the player's side
  are on screen, and the player chose the perk. Inside the Core Principle.
- **The hard filters stay until R8's learner shows θ_covered holds the page.**
  Then they become this feature's bootstrap. One exception: fix the summon
  filter's count and limit as soon as the sensor exists, because it is a bug
  today.

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
  (`EffectMapper.cpp:513-516`). A short self-cast spell therefore ranks below
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
- **Feather limits itself:** once cast, carry weight rises, the ratio falls and
  the need drops. It needs no covered term.

## 4. Positive sequences

### 4.1 Options

| Option | Signal | Fits as | Verdict |
|---|---|---|---|
| A. State only | needs (combat onset, magicka), `covered(i)` | what R6 fits, plus section 2.3 | **First.** Section 1 says it explains most of the combo, and it adapts to new spells for free |
| B. Recency need | `recent_<family>` = presence of family f in the **last pick's** cap × decay(seconds since) | new needs in the dense block, θ starting at 0 | **Second, if R6 shows lift over A** |
| C. Item transition matrix | P(next item ∣ last item), fitted offline | a per-item-pair term | Rejected: sparse (185 of 227 pairs seen once), and it does not survive a new spell list |
| D. "Just cast X" scorer bonus | hand rule | scorer term | Rejected: a hand-tuned shim, which the debt stance rules out |

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
   shown at A" subset (84 key picks and 36 menu picks so far), not the overall
   hit rate. This is doc 9's 23-point lesson.

## 5. Data

**What v3 already records** (`9-selection-log-v3.md`):

- every decision with `utc`, `seq`, `gen` and `char`, so the previous pick and
  the seconds since it can be rebuilt offline from consecutive `dec` records,
  with no format change;
- the chosen row's cap, so the previous pick's families are known;
- the page at each pick (`shown`), the outcome, and `nothing` per need episode.

**Missing:**

1. Active effects and summons: no need or cross-feature, and covered items are
   absent rather than marked. Add `covered` as a cross-feature at the end of
   `cross` (a v3-compatible addition), and a `summon_room` input.
2. The summon limit (the perk entry point) and the summon count.
3. A recency need on the live tick. The fit can derive it offline; R8 needs it
   live.
4. Casts. A spell pick is an equip, not a cast. For this question the equip is
   the decision, so this is fine *(guess: casts would mostly add noise)*.

**Volume.** R6 needs about 450–1,200 key or wheel picks to see a +2 to +4 hit@1
gain (roadmap R5 notes). The log now holds 470 key or wheel picks (the user's
~335 count is presumably the mage play alone). Sequence pairs come from the same
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
| 3 | Sensors, logged only: the summon count and limit, `covered(i)` from the active-effect walk through the effect mapper, written as a v3 cross-feature | between R6 and R8 (an R3-style PR) | Agent, then a short session in game |
| 4 | The summon filter counts against the limit (the Twin Souls bug) | with step 3 | Agent |
| 5 | θ_covered (negative start) and, if step 2 passes, the live `recent_*` needs in the scorer; the active-buff filters stay as a fallback | R8 | Agent, then **in game (you)** |
| 6 | Retire the active-buff filters once θ_covered keeps covered items off the page in the R8 session | R11 (Phase 10 "filters from cap") | Agent |

Steps 1–2 need no game code and no new play. Steps 3–4 are the only work before
R8.

## 7. Open questions for the user

1. **Feather threshold:** your five casts were at 99–103% load. Is "nearing" a
   real want, say 90%? If so, fit the curve from your carry-weight picks
   (recommended), or does the shipped c 0.95 stand?
2. **Summon upgrades:** with the wolf up and one summon allowed, should Huginn
   ever offer the Skeletal Hero as a replacement? (Learnable if covered compares
   strength; otherwise summons are simply covered.)
3. **Thralls:** does a reanimated corpse or a Dead Thrall use up the summon
   limit for your purposes? The old sensor counts it.
4. **Refresh:** should a buff with a few seconds left read as uncovered (the
   remaining-duration factor), or is a refresh always a menu action for you?
5. **Grading:** presence plus a strength term in R6 (no code), or grading
   spells within spells in R2 (fixtures regenerate)?
6. **Sequences into R8 only on R6 evidence**, or build `recent_*` regardless,
   at θ = 0 so it is harmless?
7. **Window:** is 5–30 s the right horizon for "just cast", or do some of your
   combos span a whole fight (buff at the start, finisher at the end)? A whole
   fight is `combat_onset`'s job, not this need's.
