# Huginn Roadmap

Open work only. Finished work leaves this file; git history is the record
(there is no archive -- `roadmap-archive.md` was deleted 2026-09-07). A
rejected approach keeps one line under [Decided against](#decided-against)
so it is not reopened by accident.

How to read an entry: the heading says what is wrong or wanted, **Status**
says where it stands, **Next** is the one thing to do, and the rest is only
the evidence or decision that the next step needs. Design detail lives in
`docs/architecture/`; soak findings in `docs/playtest/`.

---

## Now

**The phase, in order** (set with the user 2026-10-04, after the soak; report:
[playtest/Soak-2026-10-LoreRim.md](playtest/Soak-2026-10-LoreRim.md)). The
soak's verdict: the main goal is met -- menu trips for potions and swaps are
seriously cut down, Remembrance pulls its weight, the widget and Wheeler are
fine -- and consumables and gear are where Huginn fails. The rule that orders
it: fix what is broken before tuning what is learned, and change the learning
before tuning anything that competes with it.

| # | Phase | Status |
|---|---|---|
| 1 | Detection fixes | **Done** -- 0.22.15 (#171), tested in game. Two checks left (below). |
| 2 | [Learning rework](#1-learning) | Choice target (#172), prior as pseudo-observations (#173), re-equip rule and passed-over weight (0.23.0) shipped; useful life (#180, 0.23.6) **test pending**. Open: [context as the learner's input](#context-as-the-learners-input) (proposal, doc 9), balance stopgap, surprise weighting, pooling. |
| 3 | [Slot stability](#2-slot-stability) | Need cap (0.23.4), home keys (0.23.5), margin 0.5 (0.23.6) shipped. **Next: score jumps** from switching multipliers. |
| 4 | [Potion recommendations](#3-potions) | Not started. |
| 5 | [Gear recommendations](#4-gear) | Not started. |
| 6 | [Context expansion](#5-context-expansion) | Not started. |
| 7 | [Wild cards](#6-wild-cards) | Not started. |

Then the next stress test: a new mage character quick-swapping spells as the
game state changes (the user expects spell issues to wash out). The soak
character was a deliberate stress test -- a level 10 LoreRim character should
not carry that kit -- so per-hour numbers from it are a baseline, not a norm.

### Waiting on an in-game test

- **Cures reach the page (0.22.15).** Detection gave Cure Poison and Cure
  Greater Disease their weight, but in the #171 test learned items outranked
  them (Cure Poison u 1.66 under Stoneflesh 61.5) and both were taken from the
  menu. The learning rework was meant to fix that crowding; recheck while
  poisoned and diseased.
- **Cure Greater Disease over plain Cure Disease (0.22.15).** Ranked by gold
  value when neither has a magnitude or duration; untested, because no plain
  Cure Disease was carried. Carry both and catch a disease.
- **Useful life (0.23.6).** Watch `forgot=` in the heartbeat. The risk is
  rarely used consumables -- a cure forgotten between diseases means a longer
  T0 for consumables.
- **Phase 3 against the soak.** Decided (the user, 2026-10-02): the same
  characters, `hg reset weights`, a few hours of play.

### How Huginn is measured

Two objective metrics (the user, 2026-10-01); everything else is a proxy:
1. **Did the player reach into the inventory** (or favourites menu) for
   something Huginn could have offered?
2. **Did the player need a slot-manager label or a custom slot** to get it?

The end state is one page of plain Regular keys, with Huginn good enough that
nothing else is needed; page layouts, slot classes and custom slots exist
because the recommender is not there yet. Both are counted in the `[Soak]`
heartbeat's `goals` field (docs/playtest/LongPlaySoak.md). Where a selection
came from never weights learning, but stays on the event as a label, because
the goals are defined by it.

### The perception line

What the HUD shows the player, Huginn may read (the user, 2026-10-02): the
target's health, magicka and stamina bars, its type and race, its equipped
weapons, a spell it visibly casts. Vanilla shows only the enemy health bar, so
a rule on enemy magicka or stamina must check a HUD mod (TrueHUD on LoreRim)
draws them, or stay on health. Its spell list and hidden numbers --
resistances, perks, level -- stay out (CLAUDE.md, Forbidden Information).

---

## 1. Learning

Shipped and documented in
[architecture/4-contextual-bandits.md](architecture/4-contextual-bandits.md):
the choice target (chosen 1, passed over 0 at a quarter step, delayed 10 s and
cancelled if picked next), one reward per decision (a re-equip within 30 s
teaches nothing), confidence n/(n+2), and forgetting (useful life, 0.23.6).

### Context as the learner's input
**Status:** proposal (2026-10-07), nothing built:
[architecture/9-context-as-learner-input.md](architecture/9-context-as-learner-input.md).
Instead of `ctx x (1 + lambda*learn) x mults`, rules give a need vector (each
need through a response curve), items give an effect vector read from game
data, and the learner fits a shared weight per need x effect pair plus a small
per-item taste term. The doc's open questions and its challenge
(2026-10-07) are on the doc. **Next, in order:**
1. **A cheap replay test on today's log**: one learned weight per need class
   times the logged `ctx`, no per-item learning. If it does not beat the
   context-only page (58%, menu picks 13 of 83), the full model probably
   will not either.
2. **Effect extractor + need-vector logging** (doc steps 1-2). Changes no
   scores, so it can ship any time. The soak log CANNOT fit the full model:
   it holds one `ctx` and one `need` label per candidate and the 18-float
   phi, which has no fire, darkness, hunger, workstation or encumbrance.
   The fit needs play hours logged with the need vector.
3. The rest of the doc's sequence, on that new log.

Cuts across the plan:
- **Slot stability is tuned on today's score scale.** The hold margin
  (x1.5), the need cap (x0.5, x0.25), `fMinimumUtility` and the override
  thresholds all assume a positive product; an additive score can be zero or
  negative. If this lands, re-derive each of them.
- **Build needs as curves now.** Most of phases 4-6 are need curves in
  disguise -- hunger as a ramp, resist by damage taken, a summon by
  distance, the smallest potion that covers, encumbrance, darkness on a
  soft edge. Built as response curves they carry over to either model; new
  hand-set weights and shims do not.

### Fine-tune the balance (stopgap)
**Status:** open, only if the ~4x learned cap is still too much in play.
Doc 9 rules both options out as permanent ("they give up the learner's
23-point gain"), but that gain is partly self-fulfilling -- the live page is
the one the player saw and pressed -- and on the 83 menu picks context alone
won 13 to 7. So they stay as a cheap stopgap while doc 9 is built.
**Next:** read the cure checks above and the Phase 3 comparison first.
Options: a per-candidate-type `lambdaMax` (weapons high; potions, spells, food
low), or rank within context bands (learning orders items of similar ctx and
cannot lift a baseline item over a triggered one).
Balance is the whole product, not just lambda: favourites (up to 2.5x, on by
default), correlation (up to 2x) and the potion multiplier (1.5x) all multiply
in -- a favourited, trained weapon at its 0.2 baseline beats an untrained item
at full context. Favourites are also a hand-set preference through a menu,
which goal 1 counts. The same multipliers are the next churn lever
([Score jumps](#score-jumps-the-next-churn-lever)).

### Surprise-weighted updates
**Status:** open. A capped inverse-propensity step: an item Huginn was not
showing moves ~2x (`w += alpha*k*(r - w.phi)*phi`, k clamped ~1-3x), whichever
device picked it -- source is never a weight. After Joachims et al. WSDM 2017,
Chen et al. WSDM 2019. Fights the feedback loop rather than the balance.

### Higher rank of the same spell (Oakflesh over Stoneflesh)
**Status:** open. Same tags, Stoneflesh the higher rank (cost 150 vs 100), but
Oakflesh holds the learned weight. The A|B log's first three context-only wins
were Stoneflesh on Self (context rank 2, live 11), Stoneflesh on Target (5 vs
17) and a bow (7 vs 11). Fix by a tier preference (the behaviour-modes tier
track) or by pooling, so Stoneflesh inherits what Oakflesh learned.

### Step size
**Status:** open review note (2026-10-02). `LEARNING_RATE` 0.1 times
||phi||^2 (~7 out of combat) moves the prediction 45-70% of the way per
update, so the estimate reflects the last two or three picks while confidence
claims more. Normalised or 1/n steps.

### Share learning across similar items (pooling)
**Status:** open, behind the balance. Doc 9's shared need x effect weight is
pooling by effect rather than by class, and would replace this if it lands. Today every item learns alone, so a new
item cannot compete with a trained one -- the cold start, which is most of the
inventory: 63 of 69 carried potions and 46 of 52 scrolls never trained
(2026-10-01). Scrolls show it worst (LoreRim's throwing knives are scrolls; a
stack of 45 reached the widget only as a wildcard).
- **A. Warm start** (S, no cosave change): seed a first-seen item from its
  class mean, with a small pseudo-count -- that number is the whole risk.
- **B. Hierarchical** (M/L, cosave bump): `w_item = w_class + delta_item`.
  Only if (A) shows pooling helps.
- **Pre-check said classes do not differ** (`hg dump weights`, 2026-10-01):
  cosine 0.54 within a class vs 0.63 between; the vectors record the state an
  item was used in, not the item. The sample was poisoned by test resets --
  re-run on the soak save before deciding.
- **Grain:** probably the context-weight tag, so learning and context share one
  grouping. Class level for potions, spells, scrolls, food; per item for
  weapons and apparel. Never pool unclassified items into one Unknown bucket.
- **Risk:** with pooling a classifier misread trains the wrong class; coverage
  checks become guarantees the learner relies on.

### Grade the context rules
**Status:** open. Context rules are load-bearing now that learning is capped.
From the selection logs: did the player pick an item with lower ctx than a
same-need item they passed over? `tools/replay` has the data.

### Later
- **Logistic link** instead of least squares on a 0/1 target -- same idea,
  better calibrated, a bigger change.
- **Behavioural modes as a recall stage** (design doc 2026-10-01). Hand-written
  modes (combat, stealth, crafting/town, exploring) choose a short list of ~15
  by per-mode use counts; the scorer ranks it. Absorbs the "two weight sets:
  exploration and combat" idea. Review notes: the short list must be "used in
  this mode OR strongly relevant now" and slot-aware; counts need the cosave;
  the co-use graph will be sparse; learned location counts are fine, a
  hand-written "crypt, so anti-undead" rule is prediction and is out.
  Staging: log mode transitions -> per-mode counts in `hg recs` -> recall live
  -> persist -> learned clusters only if needed. Its tier track (spell
  magnitude, minimum skill level) can start any time.

### Cleanup
- **Delete the A|B shadow arm** (`src/learning/ShadowArm`, Debug only). It was
  meant to go after the soak run; still in the tree. Keep it only if the
  Phase 3 comparison wants it.

---

## 2. Slot stability

Shipped: the slot hold (`fChallengerMargin` 0.5 from 0.23.6), the need cap
(`src/slot/NeedCap.h`, 0.23.4), home keys with first refusal under a hold
(0.23.5; [architecture/5-slots.md](architecture/5-slots.md)). The measure is
the heartbeat: `slotChurn=`, `returns(home= wait= away=)`, and #174's key-age
and tenure bands.

### Score jumps: the next churn lever
**Status:** open, next. With the margin at 0.5 no hold give-up was under x1.5
on either list (2026-10-07), against 35% of 1,966 at 0.25, and nothing better
was kept out -- but churn per press barely moved. What is left is scores
jumping by more than any margin: the correlation and food/potion multipliers
switching on and off with combat and distance (Oakflesh x2.5 -> x5.5; one key
changed four times in 11 s).
**Next:** find which multiplier moves at each change in the `[SlotChurn]`
lines, then smooth or debounce it at the source. Also check whether food should
draw the potion multiplier at all. Doc 9 folds these multipliers into learned
features and takes favourites out of utility (a battery bonus in Boost mode, a
filter in Suppress) -- so prefer a fix that removes a multiplier over one that
tunes it.

### Workstation flicker at the bench
**Status:** two problems, two decisions pending.
- **In combat** (vanilla+, 2026-10-06): the workstation context changed 109
  times in 18 min, 24 of them fighting beside a forge, each swapping craft gear
  in and out mid-fight. Proposed: **combat suppresses the workstation
  context.** Undecided whether it is still worth it now the hold, home keys and
  need cap damp the juggling. If built, the craft context must come back by
  itself when the fight ends (the user).
- **Standing still, out of combat:** probably the tester moving the camera.
  **Next:** confirm with a still camera. If it is real, look at the crosshair
  read itself (8 changes in 2.3 s at the forge; none / forge / workbench every
  100-500 ms between two benches) before any hold or debounce.

### Restore Health and Restore Stamina trade one slot in a kite
**Status:** waiting on a re-measure. LoreRim 2026-09-30: slot 5 went Health ->
Stamina six times in 13 s, two wins under the margin. Root cause (the user):
kiting at low health, so the two needs really were trading. Open: should a
slot hold through a kite cycle? Re-measure in a fight that is not a kite before
tuning.

### Smaller, not urgent
- **One potion in several slots.** Drowning showed Waterbreathing Good (the
  override), Fair and Faint together. Undecided whether only one should show.
- **The equip flood.** A spell in the weapon hand lifts every weapon
  (`fWeightNoWeapon` 0.40). Now Remembrance puts the weapon you took off under
  your key, the weight can probably drop a lot; measure the flood first.
- **Blank slot at load.** A slot stayed empty 2.3 s when everything left was
  already on another key. Designed behaviour; revisit only mid-play.

### Remembrance follow-ups
Design in [architecture/5-slots.md](architecture/5-slots.md). None started:
- a pair pseudo-item ("re-equip both") if two-item displacement is common;
- external equips, if wanted -- they need a home chosen by classification;
- instance tracking: a weapon owned twice shows its best stack, not
  necessarily the one taken off;
- untested with Wheeler's `Empty` post-activation policy;
- no dMenu toggle, like the rest of `[SlotLocker]`.

### Review the slot classes as a whole
**Status:** open. They grew one at a time and overlap in ways a player cannot
guess: DamageAny takes weapons and poisons as well as spells; BuffsAny and
DefensiveAny both take armour spells; HealingAny and PotionsAny both take
health potions. One pass that decides what each class is FOR (a job on a key),
then trims or renames to fit.

---

## 3. Potions

### Buffs matched to the loadout
**Status:** open. Fortify potions match a situation only for the three craft
skills at a bench; every other buff gets a flat shim. The user equipped Ember
and six seconds later took Fortify Destruction from the menu. Link a fortify
potion to what is in hand (the school of the equipped spell, the type of the
equipped weapon). **It replaces the shims** (the user: "I shimmed those in
there to get something going"): once a buff draws weight from the loadout, the
situation or the learner, delete `fWeightBuffPotion` (0.15, always) and
`fWeightBuffCombat` (0.35, in combat), keeping at most a small fallback. Watch
for the reverse meanwhile: a flat 0.35 in combat puts every carried buff --
Carry Weight, Fortify Block -- above the 0.2-0.3 weapon and spell baselines.

### Resist potions weighted by damage taken
**Status:** open. Frost was detected five times, each one pulse at 87-97%
health scaled down by the player's 40% resistance, and Resist Frost never
outranked Undead or Darkness; the user took it from the menu. Weight the
element already taken -- health lost to it, repeated hits, an enemy visibly
casting it -- not "a hit happened".

### The emergency potion: smallest that covers the deficit, learned
**Status:** open. `ItemRegistry::GetBestPotion` gives the emergency key the
biggest restore, so 30 missing health spends the 50 potion. The user: this
should be learnable, as in auto-potion mods (Swift Potion NG) -- some players
top off, some hoard. Needs a restore-to-deficit fit term the learner can see,
and the emergency key choosing among qualifying potions by score.

### No second restore while one is still working (over-time lists only)
**Status:** open. LoreRim and Requiem-style lists restore over time, so the
emergency key can offer another potion while the first ticks. Read the
player's active restore effects (the walk `StateManager_MagicEffects` already
does) and hold the key back while what is left covers the deficit, unless the
vital is still falling. Must change nothing on an instant list.

### Carry weight when encumbered
**Status:** open. `isOverencumbered` is polled and Fortify Carry Weight is
tagged, but no weight reads either; the user carried three and took one from
the menu.

### Food buff captions
**Status:** open, display only. Soups and cooked crab carry different buffs
and the key does not say which. Show a food's notable effect in its subtext.
Matters less for the user than for others.

---

## 4. Gear

### Decided direction (the user, 2026-09-30)
- Enchanted gear is recommended **out of combat only** -- a mid-fight swap
  strips armour, and choosing the right piece in combat needs context Huginn
  does not have.
- **Taking gear back off rides on Remembrance**: a swap made through Huginn
  remembers the displaced piece on its own, longer expiry (a crafting session,
  not a weapon toggle). Needs the instance (ExtraUniqueID) the #65 plumbing has;
  `ApparelRegistry::MarkEquipped` already knows what each equip displaced.
- In-combat gear, if ever: a keyword dump first, the way `hg dump food`
  settled survival.

### Workstation gear holds its key at the bench
**Status:** open. The smithing rings came and went on keys 1 and 2 with each
3 s lock expiry (2026-10-06 17:14); they stayed only when they outscored
everything enough to be held. Overlaps the bench flicker above.

### Dominated gear: only the best piece per body slot
**Status:** open (the user, 2026-10-06). Ring of Smithing and Ring of Minor
Smithing on keys 7 and 8 together (u 1.060 vs 1.056). A **candidate filter**,
before scoring -- a strictly weaker piece of the same job should not be
scored, logged, passed over in learning or put on a wheel. (The need cap is for
items that differ but are all useful; this is for items that are not.)
- Group craft apparel by (skill fortified, biped slot); keep the strongest
  unworn piece. A worn piece at least as strong offers nothing; a stronger
  unworn one is an upgrade. Different slots still stack (circlet + ring).
- Ties: the instance already learned or worn wins.
- Apparel only: weapons are preference; potions use the opposite rule.
- Same milestone: the prior barely separates the two rings (0.65 vs 0.64), so
  weight enchantment magnitude properly.

### Haggling gear at a merchant
**Status:** open. At a vendor the user swapped Amulet of Zenithar for Necklace
of Minor Haggling by hand -- a reach-in. Needs:
- **A merchant context** from the crosshair (vendor faction / services),
  before dialogue: Huginn's keys do not fire inside DialogueMenu or BarterMenu.
  There is no merchant context in `src/` today.
- **Worn against offered**: both pieces read "5% more favorable", so the right
  answer was no swap.
- **Taking it back off** via Remembrance (direction above).

### Enchanted apparel beyond the three craft skills
**Status:** open, blocked. #65 (PR #114) made apparel a candidate for
Alchemy/Smithing/Enchanting gear only; narrowness is what keeps it safe.
- **Tier 1, free** (existing weight and detection): resists, regen,
  muffle/sneak, waterbreathing.
- **Tier 2, signal exists**: carry weight, per-school cost reduction,
  weapon-skill fortifies -- including the user's idea of a bow bringing
  bow-enchanted gear forward.
- **Tier 3**: haggling (above).
- **Blocker:** Tier 1 is the combat case -- resist robes over 258-armour
  Orcish means stripping armour mid-fight. Needs (a) slot grouping (dominated
  gear above), (b) worn-vs-candidate scoring (is the enchantment worth the
  armour lost), (c) the restore story (M/L).

### `ApparelRegistry` copies half of `FormRegistry`
**Status:** deferred to the apparel expansion (S). Its composite (formID,
uniqueID) key does not fit the FormID index; split the visitor half into a
key-agnostic base both use.

---

## 5. Context expansion

### Enemy detection release
The target-type half shipped in 0.22.15. What is left, all inside
[the perception line](#the-perception-line):
- **Items matched to the enemy, beyond spells.** `TargetType` raises
  anti-undead, anti-daedra and anti-dragon weights. Still open: silver and
  bane weapons, turn-undead enchantments and sun scrolls against undead and
  daedra; resist potions and gear for a dragon's element; poisons by target;
  any use for beast and construct, which are read and drive nothing.
- **Fight shape.** Two reach-ins the user explained (LoreRim, 2026-10-02):
  - *A summon when pressed*: a heavy-weapons paladin in close with a faster
    enemy wanted a Spectral Warhound scroll as a distraction. The summon rule
    is a flat "in combat, no summon up". Lift it on distance to the closest
    hostile + health falling (VitalEnvelope) + melee build.
  - *Damage over time on a boss*: Powder of Burning. Signals: one tough
    hostile, a long fight, a slowly falling health bar.

### Hunger and cold as a ramp
**Status:** open. `HungerTier` gives food nothing below Hungry, half at
Hungry, full from Famished, so Starving weighs no more than Famished and the
weight drops to zero at Peckish -- the player ate six meals in eight seconds
and the last two were past where Huginn thought hunger mattered. The user: the
worse the need, the more weight. Ramp on the 0-1000 need value -- but with SMI
installed (LoreRim) `StateManager_Survival` reads only SMI's 0-5 stage
globals, so plumb the continuous value through for SMI first. Once the player
starts eating, keep food up until Fed. Same shape for cold and fatigue.

### Thirst, only when detected
**Status:** open (the user, 2026-10-05). Seen with the mage: thirsty, a full
waterskin, nothing offered. The feature turns on only when a thirst system is
found at load (LoreRim's DVSMP, SunHelm, Last Seed) and logs which; otherwise
no weight and no drink tagging.
- Tag drinks by effect (`Hydrated`, `Restore Thirst` -- 95 items carry it on
  LoreRim, soups included), OCF's `OCF_AlchDrink_Water*`, SunHelm's
  `_SH_DrinkKeyword`, Last Seed's `VendorItemDrinkNonAlcohol`; exclude
  `_SHSaltWaterKeyword`. Then a `thirstWeight` shaped like hunger's.
- LoreRim's `Waterskin (Full)` is not food to `hg dump food` and not a
  candidate today. Drinking it keeps the count (a script holds the charges),
  so the selection never confirms: **scripted consumables need a second
  confirm signal** -- the effect appearing on the player.

### LoreRim's healing block
**Status:** open. Thirst, hunger and dehydration can disable incoming healing
("my healing spell does 0 healing"). The effect is on Active Effects, so it is
fair to read. **Next:** dump the player's active effects in that state to find
it; then lower healing and raise food/drink while it is up.

### Soul Gem Fragment (Filled)
**Status:** open. LoreRim's fragment is a MISC item, so the soul-gem registry
never sees it; the user counts it as a soul gem. **Next:** find how LoreRim
uses it (recharge directly, or combined by recipe or script) before letting
the weapon-charge override offer it.

### Poisons have no context weight
**Status:** open. `ContextWeightForCandidate` gives `ItemType::Poison` nothing
above the noise floor, so poisons never surface. The right poison depends on
the target (paralysis on a dragon, frost on a fire atronach); wants a poison
dump and a target-keyword survey before a rule. See also
[Poison handling](#poison-handling).

---

## 6. Wild cards

### Poison handling
**Status:** open (the user, 2026-10-04). Do not offer a poison for a weapon
already poisoned with another; DO allow more charges of the same poison (a mod
enables it). The held weapon's poison is on its entry (`ExtraPoison`) and
visible in game. Needs a playtest with poisons -- the user does not run them
now.

### Estimated altitude
**Status:** open, undecided what it drives. A fixed offset (e.g. +100000) on
world Z gives an estimated altitude in exteriors. Featherfall already surfaces
from FallTracker without it. Pre-emptive slow-fall near high ground? Cold at
altitude in survival?

---

## Known bugs

- **uid87 ExtraHealth read 0, then 1.00, then 1.30** across three sessions
  (LoreRim Long Bow). Since 0.22.9 a zero read logs "ExtraHealth reads 0".
  Not seen in the soak (only Unarmed read 0). Watch only; close if it stays
  quiet.
- **A torch on a Huginn Wheeler wheel is unverified** (XS). The soak showed
  the torch on the Huginn page and pressed by key, never picked from the wheel.
  Worst case is a blank wheel entry.
- **`870710C4` warns at every load.** Settled in the soak: a non-playable,
  nameless Requiem weapon, correctly rejected. Left: quiet the warning for
  non-playable forms (XS).

---

## Mod compatibility

### Default slot keys are the number row
**Status:** deferred (the user does not use Skyrim's hotkeys). `iSlot1Key = 2`
.. `iSlot8Key = 9` are also Skyrim's favourites hotkeys; Huginn acts in
addition, so one keystroke fires two systems (three potions in one LoreRim
fight, 2026-09-21). Constraints: not F1-F8 (F5 quicksave, F9 quickload); ten
slot keys, not eight; many keyboards have no numpad; a new default does not
change an existing INI. Fixes, cheapest first: a different default;
[modifier keys](#modifier-key-bindings); the
read-only widget mode. The double-fire also counts as a reach-in for goal 1.

### An overrides directory, so mod authors can ship their own
**Status:** open (the user, 2026-10-02). Make `Huginn_Overrides.ini` a
directory, SPID `_DISTR.ini` style. Every script-only spell is unclassifiable
(LoreRim 105 tome-learnable, simonrim 13), and their authors know what they
do. Beyond reading a folder:
- **Keys that survive another install**: plugin-relative, e.g.
  `[Spell:0x800~Mod.esp]`, via `TESDataHandler::LookupForm`. Not runtime
  FormIDs, not display names, not editor IDs.
- **Precedence**: alphabetical, the player's file last; log each conflict
  with both files.
- **Missing plugins are normal** -- skip at debug level.
- **One bad file cannot break the rest** -- last-known-good per file.
- **The vocabulary becomes an API**: document it on the wiki, add a format
  version key.
- **Migration**: the shipped file has a live section (Arcane Mass Inhibition).
- **`hg dump spells` needs a plugin column and must ship in release** (every
  `hg dump` is Debug-only today).
- The unbuilt spell-pattern file (`pattern=true`, `Huginn_SpellPatterns.ini`,
  proposed 2026-02 and never built) could become a syntax here. The shipped
  template documents only item types; the spell-type vocabulary
  `SpellOverrides` parses is undocumented.

### Vanilla-build integration pass
**Status:** partly done on the simonrim-essentials profile (workstation and
the vanilla CC survival path verified 2026-09-19). Still open: #79's four
contexts no LoreRim character can carry, the fortify POTION payload from #63,
and the rest of the context walk. One check while a vanilla survival save
exists: the widget read `Fatigue: Slightly Tired (lvl 1)` while Active Effects
said `Fatigue - Drained` -- a naming mismatch, or the level-1 window sits a
stage low. One console read of the Exhaustion global (Survival.esl 0x816)
settles it.

### Workstation fortify potions on Requiem-based lists (#63)
**Status:** open. No fortify potion to rank at a bench on Requiem lists. A
bigger cause was fixed since (the skip gate dropped the workstation context),
which hit vanilla too, so re-check how much of "inert" was ever Requiem's
content. Apparel answers the alchemy lab (#65); the forge may still have no
live payload.

---

## Classification

### Script-only powers and scrolls are unclassified
**Status:** parked 2026-10-01 -- powers are too much of a grab bag. Across all
three load orders every unclassified spell and scroll is script-only (nothing
to read): this is the whole remaining classification gap
(docs/reference/classifier-coverage.md). In scope even if powers stay out:
simonrim's 13 learnable script-only spells (Night Eye, Chameleon, Mark, Ash
Form...) with their scrolls, and the five Shalidor's Insights scrolls. An
override file covers them.

### Are typed scrolls typed right?
**Status:** open. #128's classifier analysis never saw a scroll (the dump
missed them until v0.21.8). All 107 Unknown scrolls are script-only; whether
the typed ones are right is unchecked.

---

## Platform and dependencies

### Move off CharmedBaryon CommonLibSSE-NG; support Skyrim 1.7.104
**Status:** paused, waiting on LoreRim 5.1 (the test install is on 1.6). Local
branch `commonlib-ng-alandtse` (b348a9f) builds against the fork but does not
load -- **fix the trampoline allocation first** on resume.
We build against v3.7.0 (2023), which knows nothing of runtime 1.7. The
maintained line is alandtse/CommonLibSSE-NG (v10.1.0, 2026-09-30). It ships
`correct GetItemCount AE ID (#387)`, which may explain the #41 crash; keep
`Util::GetItemCountSafe` regardless. After it loads: test on 1.7.104 and 1.6,
consider vcpkg/xmake packaging, update `CLAUDE.md` and `README.md` (both pin
v3.7.0).

### Drop dMenu, move the settings UI to SKSE Menu Framework
**Status:** parked (L for the full version, M for the panel alone).
- **The prize is deleting our own hook**: `D3D11Hook`, `ImGuiRenderer` and
  `DebugInputHook` (604 lines) exist only to draw four debug widgets, which
  would move across nearly as-is. And the two-INI split exists only because
  dMenu's `flush_ini()` rewrites the file.
- **The cost is coupling**: dMenu is zero-coupling (JSON + mod events); the
  framework needs its header compiled in. If Huginn deletes its hook and the
  framework is absent, there is no ImGui at all. Never run two ImGui contexts.
- **Declarative becomes procedural**: 19 JSON controls become C++; adding a
  setting stops being a three-place edit.
- **Decide v2 or v3 first** -- v3 claims non-pausing windows, which matters
  for tuning while the world is live. Framework claims are from its docs
  (2026-09-26), unverified.
- Links: [mod](https://www.nexusmods.com/skyrimspecialedition/mods/120352),
  [v2 usage](https://github.com/Thiago099/SKSE-Menu-Framework-2/blob/main/Usage.md),
  [v3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3),
  [v3 example](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example).

### Modifier-key bindings
**Status:** open (M). Every binding is a bare scancode in one `uint32_t`
(`KeybindingSettings.cpp:22-31`). Wants a modifier per binding plus modifier
state in `InputHandler::ProcessButton`. The real question is the tap /
double-tap / hold gestures: is the modifier latched at key-down or sampled
throughout? Also a conflict story for modifiers the game binds. Update the
Nexus page ("only single keypresses") when it lands.

---

## Performance

A budget list, not a work list: a felt stutter, not a microsecond figure, is
the trigger. The latest capture (0.22.14, 2026-10-04 11:55, Debug, Self only)
ranks session totals: `Inventory::DeltaScan` 7.32 s, `PollPlayerMagicEffects`
3.72 s, `PollTargets` 2.66 s, `Display::Wheeler` 2.49 s,
`Pipeline::AllocateAndLock` 1.38 s, `Candidates::Filter` 1.15 s,
`Pipeline::UpdateCaches` 1.13 s. Captures and method:
[profiling/tracy-traces.md](profiling/tracy-traces.md).

- **`Inventory::DeltaScan`: scan only when the inventory changed.** 1.52 ms a
  run, ~1/4 of `OnUpdate`. `GetInventorySafe` copies each matching
  `InventoryEntryData` into a map every ~1 s, then diffs. (1) Gate it on
  `TESContainerChangedEvent` (already sunk by `InventoryExitTracker`) with a
  slow safety timer; (2) walk `entryList` and compare counts in place. It is
  the consumption detector, so it needs a count snapshot -- the cost is
  constrained, not removable. Scales with inventory size.
- **`PollPlayerMagicEffects` early-out** when the active-effect list is
  unchanged. Runs every tick because it feeds the skip gate (S/M).
- **`PollTargets`**: build outside the write lock, one classification pass,
  `MAX_TRACKED_TARGETS` 50 -> ~12 (M).
- **`Pipeline::UpdateCaches`** (358 us a run): `PipelineStateCache` classifies
  every candidate every run, though the class is read only at selection time.
  `Select` copies the snapshot even for picks that never confirm.
- **`Gather::Spells`** (259 us): likely mostly the 10 s magicka-cost refresh --
  lengthen it.

---

## Decided against

One line each, so they are not reopened by accident. Dates are when the user
decided.

- **Food as the last emergency fallback** -- food does not do what an emergency
  needs (2026-10-02).
- **Grading the learning target by outcome** (a potion drunk at 15% HP scores
  higher) -- duplicates what context knows (2026-10-02).
- **Two quick drinks of one potion count once** -- one choice; two different
  potions are already two (2026-10-02).
- **A wildcard combat toggle** -- per-slot `bWildcardsEnabled` covers it
  (2026-09-29).
- **A 10 s workstation hold after the crosshair leaves the bench** (#175, closed
  unmerged) -- not needed, and it kept craft context alive walking through town
  (2026-10-06).
- **A wait mode for home keys** -- a returner that cannot go home shows where it
  lands (2026-10-06); only a key under a hold is claimed.
- **Slot state in the cosave** -- slot management stays stateless across saves.
- **Source as a learning weight** -- one selection path; source is a label only
  (2026-10-02).
- **Expiring learner entries by inventory, and decaying n** -- replaced by the
  useful life (0.23.6).
- **F1-F8 as default slot keys** -- F5 is quicksave.
- **Editor IDs as override keys** -- the game drops them without powerofthree's
  Tweaks.
- **Telling the player where a need can be met** (an innkeeper refill) -- a
  separate mod idea of the user's ("immersive hints"), not Huginn.
