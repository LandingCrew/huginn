# Huginn Roadmap

Open work only. There is no completed-items archive any more — `roadmap-archive.md`
was deleted on 2026-09-07 — so a rejected approach is no longer recorded anywhere
once its entry leaves this file. Git history is the only record; check it before
re-opening something that looks obviously undone.

## Next up
Priority order, set with the user 2026-10-02. Details are in the entries
named. The rule that orders it: anything that changes learning or scoring
lands AFTER the soak run, so the run measures one frozen build; anything
that only logs or fixes config lands BEFORE it. One exception, decided the
same day: the reward-path fix (Phase 1 #2) changes learning but lands
BEFORE the soak -- it is a set of bugs, and a soak on the broken path would
mostly measure them.

**Phase 1 -- pre-soak cleanup (only #2 touches learning; see above):**
Merged in #163 (0.22.9): #2 through #6 (details on each), plus the AS2
`setUrgent` removal with a SWF rebuild and logging for both Known Bugs so the
soak run can settle them. Only #1 is left, deferred.
1. **Default slot keys off the number row** -- Known Mod Compatability
   Issues. XS, config only (`configs/Huginn.ini` `iSlot1Key`-`iSlot10Key`).
   First because the double-fire is what polluted the last save's weights.
   Not F1-F8: F5 is Skyrim's quicksave (see the entry).
   Pushed back 2026-10-02: the user does not use Skyrim's hotkeys.
2. ~~**One selection path: hold, then confirm**~~ -- done in 0.22.9
   (`src/learning/SelectionTracker`, `PlayerInputGate`; design in
   docs/architecture/4-contextual-bandits.md). One confirmed selection, one
   reward, on the press-time state, whatever the device; a script's equip
   or a count drop with no selection behind it teaches nothing. Misclick
   penalties, the attribution multipliers, the separate consumption reward,
   the wheel-open and anti-spam filters are gone. Heartbeat `skipped=` is
   now `(input/stale/off)`.
3. ~~**Reward-time logging**~~ -- done in 0.22.9, hung off the confirm
   point (`src/learning/SelectionLog`): a `[Selection] Confirmed` header
   and one line per item on the page in the debug log, and one JSON record
   per selection in `Huginn_Selections.jsonl` -- press-time phi, wildcard
   odds, the page with slot index and assignment type (override / wildcard
   / Remembrance), and the WHOLE scored list with every breakdown term and
   the learner's press-time prediction, tagged with the load generation.
4. ~~**`sUncastableSpellPolicy = Penalize`**~~ -- done in 0.22.9: removed,
   a legacy INI value loads as Allow.
5. ~~**Arcane Mass Inhibition override + spell-type vocabulary**~~ -- done
   in 0.22.9: shipped active in `Huginn_Overrides.ini`, which the build now
   deploys.
6. ~~**Thaumaturgy Fortify Poison Use** misread~~ -- done in 0.22.9:
   excluded by its effect keyword, `MAG_MagicEnchFortifyPoisonUse`.

**Phase 2 -- the soak run.** Clean save, `hg reset weights`, no test
sessions on it, played normally. `hg dump weights` / `potions` / `scrolls`
at both ends (and add a carried-and-trained section to
docs/reference/classifier-coverage.md from them). Checklist to ride along:
- torch on a Huginn Wheeler wheel (any key on page 1, which is all Regular);
- arrest, yield, re-engage -- watch `Enemies:` against `Combat:`;
- Restore Health/Stamina in a fight that is not a kite;
- `hg rebuild` once -- does `870710C4` register? (the rejection line now names
  its plugin; a retry logs "registered on retry" or "rejected again");
- uid87 Long Bow ExtraHealth at each session start (a zero read now logs
  "ExtraHealth reads 0", and a temper change logs at info);
- the equip flood with the slot hold in.

LoreRim's auto-quaff mod is meant to be off for the run (the user,
2026-10-02) -- but a LoreRim minor update re-enabled it the same day, so
check it after every list update. With the player-input gate (0.22.9) it no
longer trains the learner either way: its drinks log as `Skipped (no player
input -- a script?)` and count in the heartbeat's `skipped=input`, which is
the place to spot it. Seen in the first 0.22.9 test (08:57:34, a Fortify
Carry Weight with no key, menu or wheel behind it).
Raised in review 2026-10-02, to settle before the run starts:
- ~~**The soak doc contradicts "played normally".**~~ Done (0.22.10):
  `docs/playtest/LongPlaySoak.md` no longer asks for staged menu equips, says
  why (they inflate goal 1 and train the learner on staged picks), keeps the
  layout fixed for the run, and adds the selections file and the start/end
  dumps to the capture list.
- ~~**The layout sets goal 2.**~~ Decided (the user, 2026-10-02): page 1 is
  eight Regular keys -- the end state Huginn is measured against -- and
  page 2, "Jobs", is the old one-job-per-key page, kept as a MEASURED
  fallback: its presses (`labeled=`, `offPage=`) and the flips to reach it
  (`pageFlips=`) say whether the slot filters are still needed. Shipped as
  the default from 0.22.10; Kit is page 3, off.
- **A learning-off arm** -- decided (the user, 2026-10-02): not alternating
  sessions but a shadow arm on the SAME playthrough, as throwaway debug code
  with its own log. At each confirmed selection, rank the logged candidates
  with the learned term removed (what context-only Huginn would have put on
  the eight keys) and log whether the chosen item was on the live page (A),
  on the shadow page (B), both or neither. Goal 1 for B = the outside picks B
  would have shown. Built in 0.22.10 as `src/learning/ShadowArm` (Debug
  builds only, one call in SelectionLog): `Huginn_AB.log`, one line per
  confirmed selection with A / A* (live utility, ranked plainly) / B
  (context only, the whole learning factor removed) / B' (no learned
  weights but the prior and recency kept -- added in the #164 review, so the
  run does not credit the learner with what the prior does) and a running
  tally, overall and for outside picks. Plain-page selections only; the
  open slots skip what was in hand. Delete it after the run. Nothing has measured
  context-only Huginn, and the pooling pre-check hints the learner adds
  little.
- **Death and reload roll the learner back** to the last save, but the log
  keeps the abandoned rewards, so log counts will not match `hg dump
  weights`. Tag log lines with a load generation (done in 0.22.9: `gen=`
  on every selection record, bumped per load). **Should learning survive a
  reload? Yes** (the user, 2026-10-02): the fight that killed you should not
  be forgotten. Built in 0.22.11 (`Persist::ResolveLoadedLearner`): the
  cosave HCID record carries a character ID and the learner's learning
  clock (ticks per update and per reset). A load of the same character
  whose save is not ahead keeps the in-memory learning (dynamic 0xFF forms
  taken from the save); a different character, the first load since launch
  or a LATER save restores the save's learner; a failed load changes
  nothing. Saves from before 0.22.11 get an ID derived from name and race.
  Hardened in the #165 review -- the first cut kept memory on any
  same-character load, discarding a later save's learning.
- **Comparing Phase 3 against this run** -- decided (the user, 2026-10-02):
  the same characters, `hg reset weights` before the run starts, then a few
  hours of play. (Replaces "keep the run's starting save as a benchmark".)
- ~~**Selection-log cost**~~ (code review of #163). Done (0.22.10): the
  predictions are taken under one learner lock, and the JSONL record is
  formatted and appended by a background writer with a persistent stream,
  flushed per record. Still open, smaller: `PipelineStateCache::Update`
  classifies every candidate on every pipeline run, though the class is read
  only at selection time, and `Select` copies the snapshot even for picks
  that never confirm.
- ~~**Two quick drinks of one potion count once**~~ (#163 review). Decided
  (the user, 2026-10-02): leave it. Back-to-back chugging of one potion is
  one choice; two different potions (stamina regen, then resist shock) are
  already two selections, because the pending record is per item.
- One tester, who knows the internals. LoreRim 5.1 could land mid-run --
  decided (the user, 2026-10-02): hold the update, the run finishes on 1.6
  (LoreRim runs from Stock Game, so nothing updates under it).

**Phase 3 -- learning rework, in this order** (all on "Learning swamps
context" unless named):
1. **Choice target** -- learn "was this chosen" (1) against "shown for
   that need and passed over" (0, weighted ~0.25x), one target for equips
   and consumption alike, one reward per decision. Puts the learned term
   on 0-1: the prior's scale mismatch disappears and the learned boost
   tops out at ~4x instead of ~20x (~8x while the recency boost stays at
   1.5 -- see the review notes). Replaces three earlier items (fixed
   8/5 target, prior on the reward scale, most of the balance cap).
   Cosave bump. Soak logs set the negative weight.
2. **Fine-tune the balance** -- per-candidate-type lambdaMax (weapons high;
   potions, spells, food low) or rank within context bands, only if ~4x is
   still too much in play.
3. **Prior as pseudo-observations** -- n/(n+n0) instead of the sigmoid,
   decay n with the weights. Review note (2026-10-02): may need to ship
   WITH #1 -- see "zero now means rejected" on the entry.
4. **Surprise-weighted updates** -- capped inverse-propensity step size;
   an item Huginn was not showing ~2x, whichever device picked it (one
   selection path, decided 2026-10-02 -- source is never a weight).
5. **Share learning across similar items** (pooling) -- under Follow-ups.
   Was #1 here until 2026-10-02: pooling only raises the floor for untrained
   items and leaves the 12x multiplier on top, so the balance goes first.
   Class level for potions, spells and scrolls, per item for weapons; class
   = the context-weight tag. Only if the soak dump shows classes differ.
6. Later: **Behavioural modes as a recall stage**, then **#15/#16
   learnable context weights**.

**After the soak, not learning** (raised by the user during the run,
2026-10-02; not ordered against Phase 3 yet):
- **Haggling gear at a merchant.** Seen in play: at a vendor the user
  swapped Amulet of Zenithar for Necklace of Minor Haggling by hand, in the
  inventory -- a goal-1 reach-in. Promotes the haggling tier of "Recommend
  enchanted apparel beyond the three craft skills" (Known Recommendation
  Issues): there is still no merchant or barter context in `src/`. It is
  the craft-gear case again -- out of combat, one jewellery slot, no armour
  stripped -- so the combat half of the BLOCKER on that entry does not
  apply. Its part (a) still does: group by slot, so two necklaces or two
  rings never surface side by side. It needs:
  - **A merchant context** from what the player can see: looking at
    someone who trades (vendor faction / services) in the crosshair. Not
    talking to them -- dialogue is DialogueMenu, and Huginn's keys do not
    fire inside a menu, so the swap has to be offered before the player
    speaks, let alone opens the BarterMenu.
  - **Worn against offered.** Both pieces in that swap read "Prices are 5%
    more favorable", so the right answer was no swap. Compare the barter
    magnitude of what the slot holds now -- the worn-vs-candidate check
    the apparel entry lists as (b).
  - **Taking it back off** on Remembrance, as decided for craft gear ("Gear
    and poisons, decided direction").
- **The emergency potion: smallest that covers the deficit -- learned, not
  a rule.** Today `ItemRegistry::GetBestPotion` gives the emergency key the
  potion that restores the most within the urgent window (pure first), so
  30 missing health spends the 50 potion. The user (2026-10-02): this
  should be learnable rather than a fixed "optimal" rule, as in auto-potion
  mods (Swift Potion NG) -- some players top off, some hoard the big ones.
  The pick is deterministic and outside the learner today; it needs a
  restore-to-deficit fit term the learner can see, and the emergency key
  choosing among the qualifying potions by score.
- **No second restore while one is still working -- over-time lists
  only.** Vanilla restores are instant; LoreRim and other Requiem-style
  lists restore over time, so the emergency key can offer another potion
  while the first is still ticking. Read the player's active restore
  effects (the active-effect walk `StateManager_MagicEffects` already does)
  and hold the key back while what is left of them covers the deficit --
  unless the vital is still falling. Must change nothing on an instant
  list.
- Food as the last emergency fallback -- decided against (the user,
  2026-10-02): food does not do what an emergency needs in most cases.
- **Expire learner entries for items the player no longer has.** The
  learner keeps an entry for every item ever selected and drops one only
  on `hg reset weights` or a reload's dynamic-form swap, so `learn items`
  counts everything chosen since the reset (80 after ~7 h of the soak),
  not what is carried -- it grows without bound over a playthrough of
  picking up and dropping gear. Decided (the user, 2026-10-04): when the
  registries reconcile, an item missing from the inventory and spell list
  for N hours of play is removed. Needs a last-seen time per entry,
  saved in the cosave and counted in play time (the learning clock does
  not track time). Cases to settle: gear stored in a chest and fetched
  later loses its learning (N sets how long it survives); a potion type
  the player ran out of and restocks -- pooling (Phase 3 #5) would keep
  what its class learned. Replaces the `hg stress learner` idea: with
  expiry, the entry count is bounded by what one character uses in N
  hours.
- **Items matched to the enemy, beyond spells.** Raised in play,
  2026-10-02. Built today: `TargetType` (closest hostile: humanoid,
  undead, beast, dragon, construct, daedra) raises the anti-undead,
  anti-daedra and anti-dragon weights, which only spells answer; beast and
  construct are read and drive nothing. Still open: silver and other
  bane weapons against undead and daedra, resist potions and gear for a
  dragon's element, poisons by target (the poisons half of "Gear and
  poisons, decided direction"), and uses for beast and construct. Only
  what the player can perceive (the perception line below). The
  behaviour-modes idea lists enemy race and visible enemy weapon as
  count-only sensors; this is the rule-based half.
  **The perception line** (the user, 2026-10-02 -- earlier wording here
  and under "Gear and poisons" said "never its stats", which overstated
  it): what the HUD shows the player, Huginn may read. The target's
  health, magicka and stamina bars, its type and race, and the weapons it
  has equipped are all on screen. That assumes a HUD mod (TrueHUD, as on
  LoreRim): vanilla shows only the enemy health bar, so a rule on enemy
  magicka or stamina has to check that such a HUD is drawing them, or
  stay on health. Spells are the hard case: what it holds in hand is
  visible when cast, its spell list is not (CLAUDE.md,
  Forbidden Information). Hidden numbers -- resistances, perks, level --
  stay out.
- **Hunger weight: a ramp, not two steps.** Today `HungerTier` gives food
  nothing below Hungry (3), half at Hungry, full from Famished (4), so
  Starving (5) weighs no more than Famished, and the weight drops to zero
  the moment the player reaches Peckish. Seen on LoreRim, 2026-10-02
  21:45: the player ate six meals in eight seconds on one key, hunger
  5 -> 1; the food's context went 0.50, 0.50, 0.50, 0.50, 0.25, 0.05 --
  the last two meals were eaten past where Huginn thought hunger mattered.
  The user: the worse the survival need, the more weight food should get.
  Options: ramp on the 0-1000 hunger need value, with Starving above
  Famished -- but `StateManager_Survival` reads that value only on the
  vanilla CC fallback; with SMI installed (as on LoreRim) it reads SMI's 0-5
  stage globals, so the continuous value has to be plumbed through for SMI
  first, or the ramp falls back to the same stage steps; and once the player
  starts eating, keep food up until Fed rather than dropping it at Peckish.
  Same shape for cold (`ColdTier`) and fatigue. Scoring, so after the soak.
- **Fight shape: a summon when pressed, damage over time on a boss.** Two
  reach-ins from the LoreRim run, 2026-10-02, both explained by the user:
  - *Scroll of Conjure Spectral Warhound* (23:31:38): a heavy-weapons
    paladin in close with a faster enemy needs a distraction that keeps
    them alive and adds damage. Huginn's summon rule is "in combat and no
    summon active" at a flat weight (`summonWeight`), so the scroll ranked
    too low to show. Signals that already exist: distance to the closest
    hostile (v0.22.5), health falling (VitalEnvelope), melee build.
    "Pressed in melee, no summon up" should lift summons well above
    "in combat".
  - *Powder of Burning* (inventory): the damage-over-time effect was wanted
    for a boss. The user: hard to capture in Huginn today. Perceivable
    signals: one tough hostile rather than many, a long fight, its health
    bar falling slowly (the bar is on screen, so it may be read -- see the
    perception line above).
  Both are the context half; the learner cannot invent a situation the
  feature vector does not describe.
- **Score compression for repeated re-equips.** Seen the same run: the
  Wooden Battlestaff's utility went 2.2 -> 16.2 in one session at context
  0.2-0.3, nearly all learned weight -- a heavy-weapons build re-equips its
  main weapon after every scroll or spell, and each re-equip is a full
  selection. The user, 2026-10-02: needs score compression for continuous
  re-equips. Phase 3 #1 (choice target, learned term on 0-1, ~4x cap) and
  #2 (per-type lambdaMax, weapons high) are the planned answer; check them
  against this case. Decided (the user, 2026-10-02): a return to the main
  weapon within seconds of a scroll or spell is not a new choice -- no
  reward, or a very weak one. Remembrance already knows the displaced
  piece, so "putting back what the scroll took off" is detectable.

**Field notes from the soak run, 2026-10-03** (LoreRim, 1.5 h, Windward
Ruins and High Gate Ruins; the user's observations, checked against the log).
From `Huginn_AB.log` through 00:54:33 UTC (20:54 local): 134 selections --
94 Huginn presses (88 keys, 6 Wheeler) and 40 reach-ins (19 from the magic
menu, 12 from the inventory, 9 from other menus). 125 of the 134 were on the
plain page and tallied (the other 9 were on page 2, "Jobs", which the A|B
arm does not rank); the live page held the chosen item 92 of those 125
times, context-only 68. 40 reach-ins in 1.5 h is about 27 an hour; the four
earlier 2026-10-03 launches logged 5 in about 1 h 45 m, about 3 an hour.
That jump is not explained yet -- check whether it is the play (two ruins,
heavy combat, healing from the menu) or a change in what counts as a
reach-in before reading it as a regression. The reach-ins: healing spells 11
(Healing x6, Healing Touch x4, Wild Healing), non-restore potions 9, attack
and utility spells 8, raw food 4, weapons 4, scrolls and the waterskin 4.
All scoring, so all after the soak:
- **Darkness in daylight (bug).** Magelight on key 1 outdoors at in-game
  11:30, in snow. The light reading flipped between raw 220.9 and 27.1
  (tree shadow, most likely) and 27 read as dark (19:24:45-19:25:34). The
  sun being up is perceivable: outdoors in daytime should not be Darkness
  however the shadow falls.
- **Target type misses and flickers.** A Gloom Wraith read Beast, then
  Humanoid (19:35), never Undead, so nothing anti-undead surfaced; the
  user took Sunbeam from the magic menu. `StateEvaluator` matches words
  in the race editor ID ("draugr", "ghost", ...), and "wraith" is not one.
  Read the game's keywords first (`ActorTypeUndead`, `ActorTypeDaedra`,
  `ActorTypeAnimal`, `ActorTypeDwarven`, `ActorTypeDragon`), the name
  list as fallback. Where an undead WAS read (19:47-19:50), the target
  flipped Undead <-> None every few seconds, so Sunbeam reached the page
  once (19:49:19) and fell off again; the type wants to hold for the
  fight, not the crosshair moment. Correction to earlier notes: silver
  weapons, turn-undead enchantments and sun scrolls answer anti-undead
  too, not only spells.
- **Self versus target spells are one thing to Huginn.** `Oakflesh on
  Self` and `Oakflesh on Target` classify identically (type Defensive,
  tags 00040000; only the range differs), Stoneflesh likewise; Healing and
  Healing Touch differ by one tag. A targeted ally spell is useful only
  with an ally to aim at -- a follower or summon nearby, which is
  perceivable -- and does nothing for the caster. Separate them by
  delivery (`GetDelivery()`), and gate the targeted ones on an ally:
  `StateEvaluator::EvaluateAllyStatus` already gives that as
  `AllyStatus` (None / Present / InjuredPresent) -- read it, do not add a
  second ally scan.
  Healing spells were the biggest reach-in group of the session (11).
- **Oakflesh over Stoneflesh.** Same tags, Stoneflesh is the higher rank
  (cost 150 vs 100); Oakflesh carries the learned weight. Needs a "higher
  rank of the same spell" preference -- the tier selection the
  behaviour-modes entry lists -- or pooling (Phase 3 #5) so Stoneflesh
  inherits what Oakflesh learned.
  Confirmed by the A|B log the next session (2026-10-03 21:31-23:26): the
  first three cases where context-only had the chosen item and the live
  page did not were Stoneflesh on Self (context rank 2, live 11),
  Stoneflesh on Target (5 vs 17) and a bow (7 vs 11) -- the learned weight
  on Oakflesh and the battlestaff crowding them out.
- **Healing spells cast at full health are filtered out.** Healing Aura on
  Self was taken from the magic menu 7 times in one session, 6 of them
  "not a candidate": `CandidateFilters` drops every RestoreHealth spell
  while health is full (`filterHealingWhenFull`), and the user was at
  100%. Right for a direct heal; wrong for an aura or regeneration cast
  ahead of a fight, and for the player who heals at full health to train
  Restoration. Likely the same for the "Healing" picks reported as not
  candidates on 2026-10-03. Exempt effects with a duration (aura, regen)
  from the filter, or let the learner see them and decide. The filter has
  three branches in `CandidateFilters.cpp` -- spells, RestoreHealth
  potions (regen potions, soups) and scrolls (a healing-aura scroll) --
  and the exemption belongs in all three. `filterHealingWhenFull` is a
  `CandidateConfig` default with no INI key, so there is no workaround
  until then; exposing it under `[Candidates]` is a cheap first step.
- **Buffs matched to the loadout.** Fortify potions are matched to a
  situation only for the three crafting skills at a workbench; every other
  buff potion gets a flat baseline (`buffPotionWeight` /
  `buffCombatWeight`). Fortify Two-Handed, Speed and Fortify Destruction
  were never recommended in combat; at 20:49 the user equipped Ember and
  six seconds later took Fortify Destruction from the menu -- the user's
  own example. Link a fortify potion to what is in hand (school of the
  equipped spell, type of the equipped weapon). The same for enchanted
  armour in combat, the user's idea: a bow equipped brings bow-enchanted
  gear forward, two-handed brings two-handed gear (the apparel entry's
  combat blocker still applies).
- **Resist potions on a single hit.** Frost damage was detected five times
  (19:53:56 onward), each a single pulse at 87-97% health, scaled down by
  the player's 40% frost resistance (`resistScale`), and Resist Frost
  never outranked Undead or Darkness. The user took it from the menu at
  20:16. Weight the frost already taken -- health lost to frost, repeated
  hits, an enemy visibly casting frost -- rather than "a hit happened".
- **Poisons have no context weight at all.** `ContextWeightForCandidate`
  gives `ItemType::Poison` nothing above the noise floor, so poisons never
  surface. The "Gear and poisons" entry has the direction.
- **Encumbered, no potion.** `isOverencumbered` is polled and Fortify
  Carry Weight is tagged, but no weight reads either; the user carried
  three carry-weight potions and took one from the menu at 20:46.
- **Cure Disease on LoreRim is not instant.** LoreRim's changelog: a Cure
  Disease potion now cures at random within 1-4 days; three combine into
  Cure Greater Disease, which is instant. So when diseased, prefer Cure
  Greater Disease. That may also explain the 2026-10-02 Cure Disease that
  "did not work". Huginn never flagged a disease in either session, and
  `hg dump diseases` (#167) says why: its check sees **1 of LoreRim's 94
  diseases**. Disease effects carry no resistance AV -- the engine resists
  a disease by its spell type -- and most of them use the value-modifier
  archetypes, which land in the `kValueModifier` / `kPeakValueModifier` case
  of the archetype switch (`StateManager_MagicEffects.cpp`). That case only
  sets the drain flags (`hasHealthDrain`, `hasMagickaPoison`,
  `hasStaminaPoison`) for a `kDisease` spell; the resist-AV check that sets
  `isDiseased` is in the `default:` branch, which they never reach. Its
  other two hits are attack spells resisted by disease resistance (false
  positives). The fix: set `isDiseased` from the active effect's spell type,
  `kDisease`, before the archetype switch so every archetype sees it, and
  drop the resist-AV rule. `isPoisoned` has the same gap -- a value-modifier
  poison only sets `hasHealthDrain` -- so key it on `kPoison` in the same
  change. Decide what not to offer a cure for -- Wintersun's "Peryite's
  Gift" (34 variants, a worship gift), Sanguinare Vampiris (the player may
  want the vampirism) -- and note that some mods use the disease type for
  debuffs (Lock Bashing's "Sapped Grip", Alternate Perspective's
  "Wounded").

**Parked tracks** (not in the order): CommonLib migration (waiting on
LoreRim 5.1), dMenu -> SKSE Menu Framework, apparel expansion, Tier 3 perf.

**What Huginn is measured by** (the user, 2026-10-01). Two objective
metrics; everything else is a proxy:
- **Did the player reach into the inventory** (or favourites menu) to use
  something Huginn could have offered?
- **Did the player need a slot-manager label or a custom slot** to get what
  they wanted?
The end state is one page of plain, regular slots, with Huginn good enough
that nothing else is needed. Page layouts, slot classes and custom slots
exist because the recommender is not there yet. Both are counted since
v0.22.8, in the `goals` field of the `[Soak]` heartbeat
(docs/playtest/LongPlaySoak.md). Where a selection came from never weights
learning (one selection path, 0.22.9), but it stays on the event as a
label, because these two goals are defined by it. Both counts also move
with how the run is played and laid out -- see the Phase 2 review notes.

## Known Bugs
- [ ] One weapon stack's ExtraHealth has read 0.00, then 1.00, then 1.30 across
      three sessions on the same character, and nothing explains the first two.
      uid87, the LoreRim Long Bow. Either the player tempered it between those
      sessions, or an early read returned zero for a reason of its own.
      Worth settling because #131 added an `ExtraHealth > 0` guard before
      trusting the temper factor, and that guard turns a zero into "untempered"
      -- so if a read CAN spuriously return zero, the guard hides it rather
      than reporting it, and the weapon silently ranks and displays at its
      base damage.
      0.22.9 stops hiding it: a present ExtraHealth that reads 0 still counts
      as untempered, but the registry logs "ExtraHealth reads 0" for the stack
      (and "no longer reads 0" when it clears), and temper changes log at info.
      A session that shows 0 then 1.30 with no grindstone between is the
      spurious-read answer.
      Rescued 2026-09-24 from the temper-suffix entry, which closed around it.

- [ ] One weapon in the LoreRim load order classifies as nameless and is
      skipped: `870710C4`, logged as "Failed to classify weapon ..., skipping
      (won't retry)" on every session. ClassifyWeapon rejects a form whose
      GetName() AND GetFormEditorID() are both empty, because storing it would
      leave data.formID at 0 and corrupt RemoveWeapon's swap-pop re-keying.
      The rejection is sound; what is unknown is whether the form is genuinely
      nameless or merely unnamed AT SCAN TIME. The tombstone is cleared by
      RebuildRegistry, so `hg rebuild` retries it -- if a rebuild registers what
      the initial scan rejected, the condition is transient and the scan is too
      early rather than the form being bad.
      NOT the Woodcutter's Axe, which was the suspicion when this was raised.
      The axe registers normally (2026-09-24: `Woodcutter's Axe
      (0002F2F4/uid47): dmg=31.5`) and had simply not been in the player's
      inventory. 870710C4 is still unidentified.
      0.22.9: the rejection warn now names the defining plugin, the playable
      flag and any template (CNAM), and a rebuild's retry says which way it
      went -- "registered on retry ... unnamed at scan time" or "rejected
      again on retry, so genuinely nameless".
      The kHandToHandMelee note that used to sit here is done: #131's log-noise
      pass gave DetermineWeaponType an explicit arm for it, so Unarmed types as
      Unknown silently and the default arm keeps meaning "a type nobody has
      seen before".
      Raised 2026-09-24.

## Known Mod Compatability Issues
- [ ] The default slot keys are the number row, which half of Skyrim also uses.
      `iSlot1Key = 2` through `iSlot8Key = 9` are DirectInput codes for the
      keyboard 1-8 — Skyrim's own favourites hotkeys, and whatever else a list
      binds there. Huginn does not intercept the press, it acts on it IN
      ADDITION, so one keystroke fires two systems.
      Seen 2026-09-21 on LoreRim: three potions used in one fight, each one
      millisecond after a press of 1 or 3, including a Potion of Fortify Carry
      Weight mid-combat. The user's first reading was "Huginn is auto-consuming
      potions", which is exactly what it looks like from the player's seat, and
      the log needed careful reading to show the presses were theirs.
      Nothing here is wrong in the code: the presses landed on the right slots
      and activated what was displayed. It is the DEFAULT that is wrong, because
      it silently doubles up on the busiest keys in the game.
      Options, roughly in order of how much they cost: ship a default that is
      not the number row (F1-F8 or the numpad); require a modifier, which is
      already its own entry under Architecture Critique ("Modifier-key bindings
      (Ctrl+1, Alt+1, Shift+1)") and would fix this as a side effect; or the
      read-only widget mode that has been on the backlog for a while, where
      Huginn displays and something else activates. The last one also answers
      the Wheeler-driven player, so it may be the one worth building.
      Raised 2026-09-21.
      Review notes (2026-10-02): not F1-F8 -- F5 is Skyrim's quicksave and
      F9 quickload. There are ten slot keys (`iSlot10Key`), not eight. Many
      keyboards (TKL, laptops) have no numpad. A new shipped default does
      not change an INI a player already has -- check the soak install's own
      `Huginn.ini`. The double-fire has also inflated goal 1 since v0.22.8: a
      vanilla favourites-hotkey equip counts as an external equip, so as a
      reach-in. The read-only mode is no longer a learning dead end: under
      the one selection path a vanilla-hotkey pick of a displayed item is a
      full selection (it used to be attribution case E, worth 0).

- [ ] Vanilla-build integration pass — a set of contexts is only ever exercised
      on the Requiem-based list this is developed against, so anything vanilla
      ships and Requiem strips is verified by unit test alone. Workstation is the
      known case (test 6h stands in for it); the honest scope is "boot a vanilla
      profile once and walk the contexts", which would also cover the #79 four
      that no LoreRim character can carry. Wants a save with vanilla alchemy
      ingredients and vanilla survival needs — the simonrim-essentials profile
      is that save, and the sentence this replaces had been cut off mid-thought
      since before the profile existed.
      Still open, hence the unticked box: #79's four contexts, the fortify
      POTION payload from #63, and the rest of the context walk. What follows is
      what the profile has actually closed.
      Progress 2026-09-19, on the simonrim-essentials profile (vanilla+AE):
      workstation is DONE — all four bench types answered live (forge 1,
      grindstone 2, armor workbench 7, alchemy lab 5) with apparel payloads, so
      test 6h is no longer standing in alone. Survival is confirmed working on
      the vanilla CC path: all four globals resolve, the native warmth function
      caches, `SMI not installed` takes the 0-1000 threshold fallback, and
      Hunger/Cold/Fatigue/Warmth all read live in the debug widget.
      One thing to check while a vanilla survival save exists: the widget read
      `Fatigue: Slightly Tired (lvl 1)` while the game's Active Effects said
      `Fatigue - Drained` at the same moment. Our stage names come from Survival
      Mode Improved and the boundaries from UESP, so this may be nothing worse
      than a naming mismatch on a path where SMI is absent — or the 150-299
      window for level 1 may sit a stage low against vanilla CC. Settles with
      one console read of the Exhaustion global (Survival.esl 0x816, 0-1000)
      next to what the widget shows.
- [ ] #63: the workstation context has no fortify POTION to rank on Requiem-based
      lists — inert in the modlists that actually get play-tested. Vanilla path
      still needs its own regression test (test 6h is the unit coverage).
      Two corrections from the #65 work, both on the [LoreRim wiki page](https://github.com/LandingCrew/huginn/wiki/LoreRim):
      the alchemy overhaul is attributed to Alchemy Redone rather than Requiem
      alone; and a MUCH bigger cause was found and fixed — walking up to a bench
      changed no GameState hash dimension, so the pipeline skip-gate discarded
      the whole workstation context unless an unrelated dimension moved on the
      same tick. That hit fortify potions on vanilla too, so re-check how much
      of "inert" was ever about Requiem's content. Apparel now answers the
      alchemy lab (#65, PR #114); the forge may still have no live payload
- [ ] **An overrides directory, so mod authors can ship their own.** The
      user, 2026-10-02: make the overrides file a directory, load every file
      in it, and let mod authors publish theirs -- the SPID `_DISTR.ini` /
      KID pattern. Today it is one file, `Data/SKSE/Plugins/Huginn_Overrides.ini`,
      read by both SpellOverrides and ItemOverrides (path hard-coded in
      `SpellRegistry.cpp` and `ItemRegistry.cpp`), re-read on `hg rebuild` and
      `hg reset all`.
      What it takes beyond "read a folder":
      - **Keys that survive another install.** Sections key on a display name
        or a RUNTIME FormID, and the runtime id carries the load-order index,
        so an author cannot ship it; names break on translated games and on
        shared names. Published files need a plugin-relative key, e.g.
        `[Spell:0x800~Mod.esp]` (SPID's form), resolved with
        `TESDataHandler::LookupForm` at load. Not editor IDs -- the game does
        not keep them without powerofthree's Tweaks.
      - **Precedence.** Two files can claim one form. Load in a fixed order
        (alphabetical, as SPID does) with the player's own file last, so it
        wins, and log each conflict with both file names.
      - **Missing plugins are normal.** A section whose plugin is not loaded
        is skipped at debug level, not warned about. The unmatched-override
        report names the file each section came from.
      - **One bad file cannot break the rest.** Last-known-good on a parse
        failure is per file today; keep it per file inside the directory.
      - **The vocabulary becomes an API.** Once third-party files use the
        type and tag names, renaming one breaks them. Document the format on
        the wiki (Mod Compatibility) and give files a format version key.
      - Keep reading the old single file, or move the shipped one into the
        directory, so existing installs keep working. Since 0.22.9 the build
        deploys `configs/Huginn_Overrides.ini` and it carries a live section
        (Arcane Mass Inhibition), so moving it is a real migration.
      - `hg dump spells` has no plugin column (the potion, food and weights
        dumps do) -- add plugin and local id, so an author can build a file
        from a dump. Every `hg dump *` is registered for debug builds only
        (`#ifndef NDEBUG` in `ConsoleCommands.cpp`), and authors run the
        release DLL -- so at least `hg dump spells` has to ship in release
        for this to work.
      Why it pays: every spell the classifier cannot read is script-only
      ("Script-only powers and scrolls are unclassified": simonrim's 13
      learnable script-only spells, LoreRim's 105 tome-learnable ones), and
      their authors are the people who know what they do. Huginn can ship
      per-list files the same way -- LoreRim's 34 untyped potions, say. Of the
      two fixes this was first pictured for, the Thaumaturgy misread went into
      code instead (0.22.9: the effect keyword, plus "Thaumaturgy loaded means
      AV 106 is never alchemy"), and Arcane Mass Inhibition shipped in 0.22.9
      as the first ACTIVE section of the single file -- the one existing
      install to migrate. The unbuilt spell-pattern file (Doc-migration
      findings) could become a syntax inside these files.
      Not in the Next up order. It changes no learning or scoring until a new
      file appears -- but moving the live Arcane Mass Inhibition section
      changes a classification input, and the soak is now running, so it
      waits for the soak to end.
      Raised 2026-10-02.

## Platform and dependencies
- [ ] **Move off CharmedBaryon CommonLibSSE-NG; support Skyrim 1.7.104.**
      **PAUSED -- waiting on LoreRim 5.1** (the test install is still on 1.6).
      Work so far is on local branch `commonlib-ng-alandtse` (b348a9f): it
      builds against the fork, but does not load yet -- fix the trampoline
      allocation first (see that branch's copy of this entry).
      Raised 2026-10-01. Bethesda patched Skyrim after two and a half years:
      SKSE 2.3.1 targets game 1.7.104 (skse.silverlock.org; the
      `ianpatt/skse64` GitHub releases still stop at 2.2.6 and do not show
      it). GOG AE stays on 2.2.6/1.6.1179, SE on 2.0.20/1.5.97.

      **We build against a dead fork.** `CommonLibSSEPath_NG` points at
      CharmedBaryon/CommonLibSSE-NG v3.7.0 (2023-05-13), which is still that
      repo's latest release and knows nothing of runtime 1.7. Any address that
      moved in 1.7 is wrong in a plugin built on it. The maintained line is
      alandtse/CommonLibSSE-NG (GitHub API name `alandtse/CommonLibVR`),
      v10.1.0 on 2026-09-30, which adds 1.7 IDs and the extra arguments 1.7
      reads.

      **It may also explain #41.** v10.1.0 ships `inventorychanges: correct
      GetItemCount AE ID (#387)` -- the function that crashed on save-load in
      PR #41 and is quarantined behind `Util::GetItemCountSafe`
      (`src/util/InventoryUtil.h`). A wrong address fits "crashes on call"
      better than anything the bisect found. Keep the safe walker regardless;
      just stop describing the raw function as inherently unsafe once it is
      verified on the new library.
      Other v10.x fixes (SetWorn, ForEachActiveEffect, GetTargetActor,
      PlayReleaseSound) touch nothing Huginn calls directly, per a grep of
      `src/` on 2026-10-01. The one v10.0.0 break -- `CreatePackage`/
      `SetPackType` take `PACKAGE_TYPE` -- is likewise unused.

      Scope: unknown until a build is tried -- the jump is 3.7 to 10.1, so
      expect header and API churn beyond the release notes. First step is
      clone the fork, repoint `CommonLibSSEPath_NG`, build, and list what
      breaks. Then: in-game test on 1.7.104 and on a 1.6 install, check
      whether the fork's packaging (vcpkg/xmake) is better than a path
      variable, and update `CLAUDE.md` and `README.md`, which both pin
      v3.7.0.

- [ ] **Drop dMenu, move the settings UI to SKSE Menu Framework.** Raised
      2026-09-26. Bigger than a settings-UI swap, because Huginn already owns
      the stack the framework would host.

      - Mod: https://www.nexusmods.com/skyrimspecialedition/mods/120352
      - v2 usage: https://github.com/Thiago099/SKSE-Menu-Framework-2/blob/main/Usage.md
      - v3 example: https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example
      - v3 readme: https://github.com/QTR-Modding/SKSE-Menu-Framework-3

      **The prize is not the settings panel, it is deleting our own hook.**
      SKSE Menu Framework is ImGui-based: a plugin includes
      `SKSEMenuFramework.h`, calls `SKSEMenuFramework::IsInstalled()`,
      `SetSection("Huginn")` and `AddSectionItem("Name", &Render)`, and writes
      ordinary ImGui inside the render callbacks. Huginn already does the hard
      half of that for itself -- `src/ui/D3D11Hook.cpp` (65),
      `ImGuiRenderer.cpp` (249) and `DebugInputHook.cpp` (290), 604 lines of
      hooking and context bootstrap, plus `imgui` in vcpkg.json -- purely to
      draw four debug widgets nobody but a developer sees. Those widgets
      (`RegistryDebugWidget`, `StateManagerDebugWidget`,
      `UtilityScorerDebugWidget`, `DebugSettings`) are already ImGui render
      functions and would move across close to as-is.
      The second deletion is the two-INI split. It exists only because dMenu's
      `flush_ini()` writes a fresh file containing just the settings it tracks,
      which would destroy every other section of `Huginn.ini` -- hence
      `GetDMenuIniPath()`, its fallback, the `[Widget]`/`[Debug]` exclusive
      ownership rule and the whole "Why two files at all?" section of
      `docs/architecture/7-dmenu-integration.md`. A framework that does not own
      an INI lets Huginn write its own settings and collapses all of it.

      **The cost is the coupling, and it is a real trade.** The dMenu
      integration is zero-coupling by design: no headers, no linking, no
      compile-time dependency, communication entirely by JSON descriptor and
      SKSE `ModCallbackEvent`, with a documented degradation table for
      "dMenu absent", "dMenu present, no JSON" and "both present". SKSE Menu
      Framework needs its header compiled in. That header is reported to be
      permissively licensed and not to require dependants to be open source,
      and `IsInstalled()` gives a runtime absence check -- so graceful
      degradation is still reachable, but with one sharp edge: **if Huginn
      deletes its own D3D hook and the framework is not installed, there is no
      ImGui at all** and the debug widgets have nowhere to draw. Settings would
      still load from INI, so players lose only the panel; developers lose the
      widgets entirely. Decide whether that is acceptable or whether the hook
      stays as a fallback -- and note that keeping both is the ONE thing that
      must not happen casually, since two ImGui contexts in one process is a
      classic crash.

      **Declarative becomes procedural.** The descriptor is 3 groups and 19
      controls of JSON today; it becomes C++ ImGui calls. Loss: settings stop
      being data a player or packager can edit without a rebuild. Gain: adding
      a setting stops being a three-place edit (JSON descriptor, INI, settings
      class) with its own how-to section in the integration doc.

      **Decide v2 or v3 first.** v3 claims non-pausing windows, an input event
      system, foreground/HUD drawing, texture loading, translations, JSON
      themes and an in-game config menu. Non-pausing matters here more than it
      would for most mods: Huginn is a contextual tool and tuning weights while
      the world is live is exactly how you would tune it. Against that, v3 is
      newer and the example repo does not document the API inline.

      Treat everything in the two paragraphs above that describes the framework
      as vendor claims read off its docs on 2026-09-26, not as verified
      behaviour; the line counts, the widget list and the two-INI rationale are
      checked against this repo.

      Scope: the settings panel alone is M. Panel plus retiring
      D3D11Hook/ImGuiRenderer/DebugInputHook and the dMenu INI split is L, and
      it touches `docs/architecture/7-dmenu-integration.md` (which would be
      rewritten or deleted), `configs/Huginn.ini`,
      `Data/SKSE/Plugins/dmenu/customSettings/Huginn.json` (deleted),
      `SettingsReloader`, `Globals.cpp`'s two path helpers, and the 20 files
      under `src/` that mention dMenu (132 references).

## Known Recommendation Issues
- [ ] **Learning swamps context ~12x, so the bar converges on a few trained
      items.** Seen in testing (the user, 2026-10-02): a handful of trained
      items win almost every key. Fine for weapons, wrong for potions and
      spells, which are numerous and situational.
      The arithmetic, with the shipped constants (sigmoid midpoint 5,
      steepness 0.3; lambda 0.5-3.0; beta 0.2; prior ~0.5):
      `utility = ctx x (1 + lambda*learn) x ...`
      | item | alpha | learn | lambda | 1+lambda*learn |
      |---|---|---|---|---|
      | never trained | 0.18 | ~0.6 | 0.95 | ~1.6 |
      | 15 trains, R~7 | 0.95 | ~6.7 | 2.9 | ~20 |
      Break-even context is ~1.6/20 = 0.08. A trained weapon at its 0.2
      baseline (~4.0) beats an untrained Waterbreathing potion while drowning
      at ctx 1.0 (~1.6) -- overrides catch that one case, nothing catches the
      pattern -- and a trained item at the 0.05 noise floor (~1.0) matches an
      untrained one at ctx 0.65. The header calls context a gate; in practice
      it is a 1/12 tiebreaker once anything is trained. R is state-dependent
      (w.phi), but the pooling pre-check found the vectors dominated by
      always-on features, so it likely stays high everywhere.
      Why it converges fast: every used item regresses to the same fixed target
      (8 equip / 5 consume), only recommended items are rewarded on keys
      (`wasRecommended`), confidence hits 95% at 15 trains, and consumables
      rack up trains quickly -- partly because each drink is rewarded twice
      (fixed in 0.22.9 by the one selection path). Rich get richer.
      **Why weapons and consumables differ.** For a weapon the preference IS
      the item ("I use this sword"). For a potion or spell the preference is
      mostly the situation, and which of twenty healing potions is a
      detail. So per-item learning suits the few, and class-level learning
      plus context suits the many -- the same split pooling (Follow-ups) is
      reaching for from the other side.
      Per category (carried counts, not the load-order totals in
      docs/reference/classifier-coverage.md):
      - Weapons: a handful, each used constantly -- per item, high or no cap.
      - Apparel: craft gear only, out of combat -- small pool, per item.
      - Spells: dozens, situational -- class level, low cap.
      - Potions: ~69 carried, 63 untrained -- class level;
        `sPotionTierPreference` already orders tier siblings.
      - Scrolls: ~52 carried, 46 untrained, rarely used -- per-item learning
        never converges at that rate; class level is the only way in.
      - Food: already context-driven, vectors uncorrelated even within the
        class -- lowest cap, let hunger/cold decide.
      Three consequences:
      - **Unclassified items keep per-item learning at full strength.**
        Script-only spells, LoreRim's 34 untyped potions and simonrim's 13
        learnable script-only spells have no class to pool and draw only the
        0.05 baseline; under a cap a heavily used one could never climb. Never
        pool them into one Unknown bucket.
      - **Class grain is the real decision.** Probably the tag that maps to a
        context-weight field (what `WeightForCandidate` matches on), so
        learning and context share one grouping. Re-run the pooling pre-check
        grouped that way -- "classes do not differ" may have been the wrong
        grain.
      - **Classifier errors start to train the wrong class.** With less
        learning, context ranks more, and with pooling a misread (Ice Armor's
        name-derived Frost, the Thaumaturgy ring before 0.22.9) also trains the wrong
        class. The coverage checks become guarantees the learner relies on.
      **Decided direction (with the user, 2026-10-02): a choice target.**
      The root of the convergence is that the learner only ever sees
      positives. A used item gets an update, a shown-and-ignored item gets
      nothing, so a regression whose target is always 8 (or 5) learns
      "predict 8" from whatever features are present -- the pooling
      pre-check's scaled-copy vectors and tier siblings differing only in
      train count. Nothing item-specific can be learned without contrast.
      - **Target 1 = chosen, 0 = shown for that need and passed over.** The
        existing update `w += alpha*(target - w.phi)*phi` then sizes every
        move by surprise with nothing new to tune: a strong item passed over
        (predicted 0.9) drops a lot, a weak one (0.1) barely; a low-ranked
        item chosen over a strong one rises a lot, the incumbent chosen
        again barely moves. That is "reward proportional to what it beat",
        and the target itself stays fixed -- only the error varies, as it
        already does today.
      - **Negatives only at a real choice moment**: the player chose A and
        B was on the bar for the same need (same context-weight field or
        slot class). Never because B sat on screen while nothing was
        needed. A passed-over counts ~0.25x a chosen (weaker evidence: the
        player may not have looked); size it from the logging.
      - **No equip/consume split.** 8 vs 5 mixed preference with evidence
        strength. A choice is 1 either way. The real evidence problem is
        that one decision can fire several events (a weapon toggled back and
        forth), so: one reward per decision -- no reward for re-equipping the
        same item within a few seconds, Remembrance swap-backs do not count.
        Hold-then-confirm (SelectionTracker, 0.22.9) is what implements this.
        Base rates (weapons equipped often, potions rarely) are absorbed by
        each item's bias weight. The one weighting kept is surprise: an
        item Huginn was not showing ~2x (surprise weighting below). Source
        is not a weight -- one selection path, decided 2026-10-02.
      - **Scale falls out.** Predictions live on 0-1, the same scale as the
        PriorCalculator prior, so the prior-scale fix is not needed; and the
        learned boost tops out at 1 + lambda*1 = 4x (lambda 3) instead of
        ~20x (~8x while the recency boost stays at 1.5 -- review notes
        below), so most of the balance problem goes with it.
      - **Cost:** a cosave bump. Weights trained towards 8 are divided by 8
        on load, or reset.
      - **Not needed:** moving the vitals off the six buckets. The buckets
        (`GameState.h` HealthBucket etc.) feed only the pipeline skip gate;
        the learner reads continuous `healthPct` / `magickaPct` /
        `staminaPct` (`StateFeatures.h`) and the context curves are
        continuous too. Buckets decide WHEN the bar re-ranks, not what is
        learned. (Not quite, today: misclick detection and the recency
        boost both key on `GameState::GetHash()`. Misclick goes with
        hold-then-confirm; the recency boost needs rescaling anyway --
        review notes below.)
      - Rejected: grading the target by outcome (a potion drunk at 15% HP
        scores higher than at 90%). It duplicates what context already
        knows.
      - Later: a logistic link (true probabilities) instead of least squares
        on a 0/1 target -- same idea, better calibrated, a bigger change.
      Options still open on top of it:
      - Cap the learned boost (`lambda*learn` <= ~2-3x) so context stays a
        real gate; or a per-candidate-type lambdaMax (high for weapons, low
        for potions/spells).
      - Rank within a context band: learning orders items of similar ctx and
        cannot lift a baseline item over a triggered one.
      - Learn consumables and spells at class level (pooling B) and weapons
        per item.
      - Surprise-weighted updates (`w += alpha*k*(r - w.phi)*phi`, k larger
        when the chosen item ranked low or was not on the bar, clamped ~1-3x)
        -- inverse-propensity weighting, after Joachims et al. WSDM 2017 and
        Chen et al. WSDM 2019 (capped weights). Fights the feedback loop
        rather than the balance.
      - Prior as pseudo-observations: replace the sigmoid with n/(n+n0),
        decay n alongside the weights so idle items fall back towards the
        prior. (Putting the prior on the reward scale was part of this until
        the choice target made both 0-1.)
      Probably BEFORE pooling: pooling only makes untrained items look more
      trained, which raises the floor but leaves a 12x multiplier on top.
      All of this changes learning behaviour -- not landable during the soak
      run. What IS landable before it: log, at each reward, the chosen item's
      utility, rank and ctx, what it displaced, its source (key / wheel /
      menu), and what else was showing for the same need with its
      prediction. That changes nothing and lets the soak data size the
      negative weight, the cap and k (and supports offline evaluation --
      see the review note on replay below).
      Landed in 0.22.9 as the selection log (`src/learning/SelectionLog`,
      Phase 1 #3): at each CONFIRMED selection, the whole scored list from
      the press-time pipeline run, so a new formula can re-rank logged
      selections offline. Still current page only for the readable lines --
      the JSONL has every candidate, but "shown" means the current page, so
      a Wheeler pick from another page shows as not displayed.
      **Raised in review (2026-10-02), not decided:**
      - **Rescale what was sized for 0-8.** The recency boost adds 1.5
        inside lambda (`UtilityScorer.cpp`, Step 4b); on a 0-1 target that
        is bigger than the whole learned term, so the boost tops out near
        8x, not 4x. UCB's beta 0.2 becomes 20% of the learned range. (The
        -3 misclick penalty and the attribution multipliers leave learning
        with the one selection path.)
      - **Zero now means "rejected".** L2 and the 2%/hour decay both pull
        towards 0, so an idle item drifts towards "never chosen" instead of
        back to its prior. If a 0.25x negative counts as a whole train,
        confidence climbs four times faster than the evidence. Pseudo-
        observations (Phase 3 #3) fix both -- an argument for shipping them
        with #1.
      - **Substitutes, not complements.** Negatives fit items competing for
        one hand, body slot or need -- not circlet + ring, sword + ward or
        dual-wield. Delay negatives and cancel any whose item is chosen
        next; the confirm window is the natural place.
      - **The shipped layout rarely shows two items for one need** -- true of
        the old one-job-per-key default, where same-need items shared the
        screen only through class overlaps and the two Regular slots, so
        the target fell back to positives only. Changed in 0.22.10: the
        default is eight Regular keys, so two healing options can sit side
        by side and the passed-over one is a real negative. The strong
        negative is still a menu bypass: the bar's item loses to what the
        player fetched.
      - **Key position and overrides.** "Health on key 1" means the press
        picks the key, not the item; rewarding override-placed picks teaches
        the learner the override rule. Log slot index and override flag.
        (Logged since 0.22.9: every shown slot carries its index and
        assignment type in the selection log.)
      - **Migration.** Dividing by 8 keeps every trained item near 1 with
        its full train count (doubled for consumables) -- the convergence,
        imported. Reset, or divide and cap trainCount at 2-3.
      - **Step size.** `LEARNING_RATE` 0.1 times ||phi||^2 (about 7 out of
        combat) moves the prediction 45-70% of the way per update, so the
        estimate reflects the last two or three selections while
        confidence claims 95% at 15. Normalised or 1/n steps
        (pseudo-counts again).
      - **Balance is the whole product.** Favourites (up to 2.5x, on by
        default), correlation (up to 2x) and the potion multiplier (1.5x)
        still multiply in: a favourited, fully trained weapon at its 0.2
        baseline beats an untrained item at full context, ~1.9 vs ~1.5.
        Favourites are also a hand-set preference, through the menu goal 1
        counts.
      - **Context rules become load-bearing** once learning is capped.
        Grade them from the selection logs: did the player pick an item
        with lower ctx than a same-need item they passed over?
      - **Replay needs randomness Huginn lacks.** Li et al. WSDM 2011
        assumes randomised logging with known propensities; Huginn is
        deterministic apart from wildcards. The workable offline test is
        re-ranking the logged candidates under a new formula, which needs
        the WHOLE scored list at each selection (every breakdown term,
        phi, slot index, override / wildcard flags, the wildcard roll
        probability), one structured line each -- the input for an offline
        harness, so a Phase 3 change costs minutes rather than play-hours.
        (Logged since 0.22.9: `Huginn_Selections.jsonl`. The harness itself
        is not written yet.)
      - **"Soak logs set the negative weight" needs a procedure.** The logs
        cannot show whether the player looked. Fit on the first half of
        the run; keep the weight that best ranks the second half's
        selections.
      Raised 2026-10-02.

- [ ] **Restore Health and Restore Stamina trade one slot in a fight, below
      the hold margin.** LoreRim 2026-09-30 21:35:20-35, one beast: health
      crossed the Critical/VeryLow line every ~3 s and slot 5 went Health ->
      Stamina -> Health -> Stamina -> Health -> Stamina in 13 s, every change
      logged `expired`. Two of the wins were x1.12 and x1.08, under
      `fChallengerMargin` (x1.25), so the slot hold did not hold them. Open
      questions: why the hold let a x1.08 challenger in (is an expired lock
      outside its reach for potions, or was the incumbent not a candidate for
      that slot at that moment?), and whether the HP bucket itself wants the
      same dwell as combat and darkness. Re-measure first: the stamina side
      may have been a real need (sp 22% at 21:36:18).
      Raised 2026-09-30.
      **Root cause (the user, 2026-10-01): the play, not the sensor.** They
      were kiting at low health while trying to heal -- heal, get hit, drop,
      run (stamina spent), heal again -- so health and stamina really were
      trading places as the most urgent need every few seconds. The open
      question still stands: is it right for one slot to follow a need that
      flips that fast, or should a slot hold through a kite cycle? Re-measure
      in a fight that is not a kite before tuning the margin.

- [ ] **The enemy count drops to None and back mid-fight.** LoreRim
      2026-10-01 18:17:47-18:01, one humanoid: `Enemies:One->None` and back
      four times in 14 s, `Target` going None with it each time, while combat
      stayed on throughout (no `Combat:` flip until 18:18:03). One slot change
      rode on it (slot 6 Roasted Goat Leg -> Iron Throwing Knife at 18:17:47).
      Likely the hostile dropping out of the scan for a moment -- out of
      sight, or not in `highActorHandles` -- and being pruned or re-found;
      `LAST_SEEN_TIMEOUT` is 3 s, and the gaps were 0.2-2.9 s, so check which
      path removes it first. Same family as combat, casting and darkness: a
      count that flips and flips back. Options: hold the last enemy through a
      short gap while combat is on, or debounce `cachedEnemyCount == 0` like
      CASTING_EXIT. Measure how often it moves a slot before building.
      Raised 2026-10-01.
      **Likely cause found, fix in v0.22.8 -- confirm in play.** Not the
      prune: PollTargets read the RAW `player->IsInCombat()`, and on a poll
      where the engine dropped it for a moment it erased every hostile and
      skipped the closest-hostile scan. The published combat flag held through
      COMBAT_EXIT, hence no `Combat:` flip. Now raw to enter, published to
      leave. If `Enemies:` still flaps with combat steady, the remaining cause
      is the hostile falling out of `highActorHandles` for a poll.
      **Likely trigger (the user, 2026-10-01): the arrest process.** Guards
      attack, the player lowers their weapon to talk, then combat can start
      again -- so the hostile genuinely stops being hostile for a moment and
      comes back. That is a real state change, not a sensor flap, and would
      explain why no fight since has reproduced it. To confirm: get arrested,
      yield, then re-engage, and watch `Enemies:` against `Combat:`.

- [ ] **Thirst is not tracked -- UNPARKED 2026-10-03.** Parked 2026-09-30
      because there was no reliable way to buy water from innkeepers on
      LoreRim. The user found the way: LoreRim's waterskins are craftable,
      and once you own one an innkeeper refills it for free -- so water is
      easy to come by after all. Checked in play (2026-10-03 00:03, LoreRim):
      - `Waterskin (Full)` (FE350801, a light plugin) carries `Hydrated`,
        `Restore Thirst` and a scripted `Waterskin` effect. Drinking it left
        the count unchanged -- the script keeps the charges, there is no
        empty form to exclude -- so the selection path logged `Not confirmed
        ... count never dropped`, and the drink taught nothing. Scripted
        consumables need a second confirm signal: the effect appearing on
        the player (`Hydrated` here) rather than a count drop.
      - It is not a candidate at all today (`case=A (not candidate)`), and
        `hg dump food` leaves it out: it is not classified as food.
      - 95 items in the dump carry `Restore Thirst`, LoreRim's soups and
        stews among them, so the effect name alone tags a broad set.
      Aside, not Huginn: telling the player where a need can be met (a
      refill at the innkeeper) is a follow-on mod idea of the user's,
      "immersive hints".
      Original entry: **Thirst is not tracked.** LoreRim runs a thirst need ("Thirst -
      Parched" in Active Effects, from DVSMP Survival Tweaks per research,
      unverified), and waters, teas and waterskins carry `Hydrated` /
      `Restore Thirst` effects (`hg dump food`: Cup of Water, Waterskin,
      Glacier Water, Canis Root Tea). Huginn reads hunger, cold and fatigue
      only. Wants: find the thirst stage (a global, like SMI's), tag drinks
      (the effect names above; OCF's `OCF_AlchDrink_Water*`, SunHelm's
      `_SH_DrinkKeyword`, Last Seed's `VendorItemDrinkNonAlcohol`; exclude
      `_SHSaltWaterKeyword`), then a `thirstWeight` shaped like hunger's.
      Raised 2026-09-29.

- [ ] **Two weight sets: exploration and combat.** Idea from play
      (2026-09-27), needs more thought before building. The context weights
      are one table serving two very different situations; out of combat the
      bar wants utility (light, warmth, waterbreathing, travel buffs), in
      combat it wants damage, wards and restores. Splitting the table (or the
      learner's context) by mode could make each sharper. Open questions:
      what decides the mode (the debounced combat flag is the obvious gate),
      whether the learner learns per mode, and how this interacts with page
      layouts that already separate the two by hand.
      Raised 2026-09-27. Developed in the next entry.

- [ ] **Behavioural modes as a recall stage -- for later.** Design doc
      "Huginn: Behavioural Modes as a Recall Stage" (2026-10-01). Parked
      behind shared learning; filed so it can be picked at later.
      Idea, after YouTube's two-stage recommender: a handful of hand-written
      modes (combat, stealth, crafting/town, exploring; emergency probably
      an overlay on combat, which overrides already cover) decide which
      items are in play, then the existing scorer ranks a short list of ~15
      instead of all ~100. Each mode keeps per-item USE COUNTS (every equip
      counts as one, unlike rewards) plus a co-use graph of items used in the
      same fight. Counts suit the data budget where per-mode weight vectors
      do not. Extensions: richer count-only sensors (enemy race, visible
      enemy weapon, location keywords) and tier selection (spell rank by
      magicka, specialist vs generalist weapon).
      Review notes (2026-10-01):
      - Recall from usage alone hides never-used items -- the scroll cold
        start made worse -- and Huginn's best moments are context-driven
        (Waterbreathing when drowning). The short list must be "used in this
        mode OR strongly relevant now"; recall then only trims the low-context
        tail. It must also be slot-aware, so a slot never comes back empty.
      - Step 3 (recall live) needs step 4 (counts in the cosave), or every
        load starts recall cold.
      - The co-use graph will be sparse for a long time (2-5 items a fight).
        Record it early if cheap; do not read it until logs show structure.
      - Location counts learned from the player's own play are memory and are
        inside the Core Principle; a hand-written "crypt, so anti-undead" rule
        is prediction and is out.
      - Count decay wants a half-life of a few hours of PLAY time, not the
        learner's 2%/hour.
      Staging: (0) instrument the two metrics -- DONE in v0.22.8, the
      `[Soak]` `goals` field; (1) log the
      mode on transitions, change nothing; (2) record per-mode counts, show
      them in `hg recs`; (3) recall live; (4) persist counts (cosave bump);
      (5) learned clusters only if hand-written modes visibly misfit.
      Tiers are a separate track; their first step, recording spell magnitude
      and minimum skill level in the spell classifier, can start any time.

- [ ] **Estimated altitude.** Idea from play (2026-09-27): add a fixed
      offset (e.g. +100000) to the player's world Z to get an estimated
      global altitude. Exteriors only -- interiors have their own Z space and
      do not need it. Featherfall already surfaces from FallTracker (descent
      below the take-off point: it appeared 0.3 s into a fall at 16:41:38
      with the "Falling" label), so altitude is not needed for that. Open
      question before building: what should altitude DRIVE -- pre-emptive
      slow-fall near high ground, cold at altitude in survival, something
      else?
      Raised 2026-09-27.

- [ ] #128 measured the spell classifier against 1,107 spells and silently
      excluded 1,200 scrolls. `hg dump spells` walked
      `GetFormArray<RE::SpellItem>()`, and GetFormArray keys on T::FORMTYPE --
      ScrollItem's is FormType::Scroll, SpellItem's is FormType::Spell -- so no
      scroll was ever in it. Fixed in v0.21.8, and the first dump that included
      them came back 6,224 rows against 5,024.
      Nothing in the #128 work saw a scroll: not the 375 -> 90 unclassified
      count, not the description corpus that rejected a text classifier, not
      the weak-evidence numbers. ScrollClassifier delegates straight to
      SpellClassifier, so every rule written there applies to scrolls and none
      of them was checked against one.
      First measurement (LoreRim, 2026-09-24): 1,200 scrolls, of which 107 come
      back Unknown. Worth re-running the #128 analyses over the wider set
      before trusting their conclusions.
      Raised 2026-09-24.
      2026-10-01: all 107 Unknown scrolls are script-only (nothing to read),
      as are all 976 Unknown spells -- docs/reference/classifier-coverage.md.
      Still open: whether the scrolls that ARE typed are typed right.

- [ ] **Script-only powers and scrolls are unclassified -- PARKED 2026-10-01.**
      Not on the user's plate: powers are too much of a grab bag for a
      classifier right now. Found by the vanilla `hg dump spells`: all 50
      vanilla Unknowns have every effect scripted (archetype -1, AV -1), so
      the classifier has nothing to read. Most are rightly Unknown (quest and
      FX spells). The player-facing ones: racial powers (Berserker Rage,
      Night Eye, Command Animal, Vampire's Sight), earned powers (Shadowcloak
      of Nocturnal, the Dragonborn black-book Secrets and Root of Power,
      Bardic Knowledge, Secret Servant, Black Market), transformations (Beast
      Form, Werebear Form, Vampire Lord), and the five Shalidor's Insights
      scrolls (school cost/potency -- Buffs). Answer when picked up: whether
      powers should be candidates at all; if not, only the Shalidor scrolls
      matter, and an override entry or the unbuilt spell-pattern file covers
      them.
      Simonrim (same day): all 148 Unknowns are script-only too, and 13 are
      learnable SPELLS, not powers -- Night Eye, Chameleon, Mark, Shalidor's
      and Valerica's Beacon, Planar Anchor, Translimination, Silence and
      Burden Rune, Ash Form, Ash Cloud, The Unwelcome Guest, Daedric
      Invocation -- plus their 13 scrolls. Those are in scope even if powers
      stay out.
      LoreRim (same day): 976 Unknowns, all script-only -- 697 spells, 147
      lesser powers, 25 powers, 107 scrolls -- and 105 of the 1,107
      tome-learnable spells. Across all three load orders EVERY unclassified
      spell and scroll is script-only: the classifier covers every effect it
      can read, so this entry is the whole remaining classification gap.

- [ ] Take craft gear back OFF when the crafting is done — the #65 follow-on.
      Apparel is the one source that CHANGES THE PLAYER and leaves it changed:
      every other recommendation is spent when used, but a fortify ring stays on
      the finger after you walk away from the bench, and Huginn deliberately
      tracks nothing about what it replaced ("taking it off again is the
      player's business", CandidateTypes.h). Gear up at an alchemy lab with a
      circlet and a ring and you leave wearing +6% alchemy and whatever armour
      those two slots used to hold is in your pack.
      Wants a decision before it wants code, roughly in order of nerve:
      remember the displaced piece and offer to restore it once the workstation
      context closes (a recommendation, so the player still chooses); surface a
      "take it off" entry in the same slot while the gear is worn and the bench
      is gone; or restore automatically on leaving, which is the only option
      that acts on the player without being asked and should probably stay off
      by default.
      Note the restore target is an INSTANCE, not a form — it needs the
      ExtraUniqueID plumbing from #65, and ApparelRegistry::MarkEquipped already
      knows which piece each equip displaced (that is what the slot sweep is).
      Raised 2026-09-18 after the swap loop was fixed.
- [ ] Recommend enchanted apparel beyond the three craft skills — the #65
      follow-up. #65 itself is DONE (PR #114): apparel is a candidate source,
      verified in-game, but deliberately narrow — only gear fortifying Alchemy,
      Smithing or Enchanting, because classification is what keeps the pool from
      growing by the size of the wardrobe.
      A real inventory has far more that is contextually useful. Sorted by cost:
      **Tier 1, free** — existing weight AND existing detection, pure
      classification: resist fire/frost/shock/poison/disease
      (`resistXWeight`), magicka/health/stamina regen (`magickaRestoreWeight`
      et al.), muffle/sneak (`stealthWeight`), waterbreathing.
      **Tier 2, new weight but the signal exists** — carry weight
      (`isOverencumbered` is already polled and `ItemTag::FortifyCarryWeight`
      already exists); per-school spell cost reduction (school is known, but
      there is no per-school weight); weapon-skill fortifies (equipped weapon
      type is tracked).
      **Tier 3, needs new detection** — haggling/speechcraft. There is NO
      merchant or barter context anywhere in `src/`; it wants BarterMenu
      detection, which is state plumbing rather than classification.
      **BLOCKER, settle before any of the above.** Apparel is currently safe
      only because it is narrow: one circlet, swapped deliberately at a bench,
      out of combat. Tier 1 is exactly the combat case — recommending resist
      robes while the player wears 258-armor Orcish Berserk means `EquipApparel`
      strips the armor mid-fight, and nothing restores it. Needs (a) slot
      grouping so two rings do not both surface — `ApparelData::slot` is already
      classified, the candidate field was dropped in PR #114 for having no
      consumer and comes back here; (b) a worn-vs-candidate comparison, is the
      enchantment worth the armor lost, which is scoring not filtering; and
      (c) a restore story, or an explicit decision not to have one (M/L)
- [ ] **Gear and poisons, decided direction (with the user, 2026-09-30).**
      For the two apparel entries above:
      - Enchanted gear is recommended OUT OF COMBAT only. Mid-fight swaps
        strip armour, and choosing the right piece in combat needs context
        Huginn does not have yet (see poisons below).
      - Taking gear back off rides on Remembrance: an apparel swap made
        through Huginn remembers the displaced piece, on its OWN, longer
        expiry than the 15 s weapon hold -- a crafting session or a walk
        through a dungeon, not a weapon toggle. Needs the instance
        (ExtraUniqueID) the #65 plumbing already has.
      - In-combat gear, if ever: keyword lookups and a dump first, the same
        way `hg dump food` settled survival -- measure what the load order
        marks before writing a rule.
      Poisons are the same problem: the right poison depends on what is
      being fought (a paralysis poison on a dragon, frost on a fire atronach,
      damage-magicka on a mage). Only perceivable target facts may drive it
      -- race/type (undead, daedra, dragon, humanoid), its health bar
      (magicka and stamina only where a HUD mod draws them), what it is
      visibly casting or wielding --
      not its spell list or hidden numbers (CLAUDE.md, Forbidden
      Information; see "the perception line" under Next up). Wants a
      poison dump (effects, keywords) and a
      target-keyword survey before a rule.
      Raised 2026-09-30.

- [ ] Scroll cold-start: all scrolls sit in the pool every tick but score
      `learn≈0` against trained items at `learn=7–8`, so one can never surface
      until used and can't be used until surfaced.
      **Not scroll-specific** — it is the general no-transfer-between-items
      problem, and it bites every newly acquired item. Scrolls are just where it
      is most visible, because a player rarely uses one unprompted. The two
      candidate fixes are under Follow-ups, "Share learning across similar
      items"; fixing either closes this.
      Concrete case (2026-09-24): LoreRim's throwing knives are ScrollItems --
      not WEAP, not AMMO -- and ScrollRegistry already tracks them correctly
      (Iron x45, Steel x15, Silver x15). A Silver Throwing Knife did reach the
      widget, but through WildcardManager at 50% probability rather than on
      merit. A stack of 45 is a real combat option and should be able to earn
      its slot

- [ ] `ApparelRegistry` hand-copies the key-agnostic half of
      `Registry::FormRegistry` (`ForEachEntry`, `IsLoading`, `EntryCount`), the
      CRTP base whose own comment says it exists to stop exactly that. Its
      composite (formID, uniqueID) key does not fit the FormID-keyed index,
      which is why it was copied; the visitor half could be split into a
      key-agnostic base both use. Deferred to the apparel expansion, which is
      when it starts paying -- a refactor of the base four registries share,
      with nothing a player would see (S).
      (The other half of the PR #114 finding, the craft-skill vocabulary, is
      fixed: both classifiers share `Apparel::CraftSkillForActorValue`.)

- [ ] **Review the slot classes as a whole.** They grew one at a time and
      overlap in ways a player cannot guess: DamageAny takes weapons and
      poisons as well as spells; BuffsAny and DefensiveAny both take armour
      spells; HealingAny and PotionsAny both take health potions; and
      nothing meant "attack magic" or "poisons" until DamageMagic and
      PoisonsAny were added for the flagship page and the templates
      (2026-09-28). Worth one pass that decides what each
      class is FOR (a job on a key) rather than what it happens to match,
      then trims or renames to fit.
      Raised 2026-09-28.

## Slot temporal memory
Seating fixed WHERE an item sits; these entries are about WHEN a slot may
change. The churn and override work (#140-#148, #151) closed most of it, and
Remembrance shipped (#150); what is left is the churn tail and Remembrance's
follow-ups. The override rethink is done: its last step, an overrides-only
key, is already a PotionsAny slot with overrides on.

- [ ] **Remembrance follow-ups.** Design and mechanics are in
      `docs/architecture/5-slots.md` (Remembrance). Decided with the user and
      not to be reopened casually: Huginn keys and Huginn wheels only (menu,
      favourites and the player's own wheels are out of scope); no chaining;
      no learner reward; a plain UNEQUIP is not remembered, only a swap.
      Follow-ups, none started:
      - `fWeightNoWeapon` (0.40) still lifts every weapon when a spell goes in
        the weapon hand. With the one weapon you took off now on your key,
        that weight can probably drop a lot; measure the equip flood first.
      - A pair pseudo-item ("re-equip both") if two-item displacement turns
        out to be common in play.
      - External equips, if wanted: they have no source slot, so the item
        would need a home chosen by classification.
      - Instance tracking: a weapon owned twice shows its best-scoring stack,
        not necessarily the one you took off.
      - Untested with Wheeler's `Empty` post-activation policy.
      - No dMenu toggle, like the rest of `[SlotLocker]`.

- [ ] **Remaining slot churn.** Instrumentation shipped in v0.21.19: the
      `slotChurn=` heartbeat field with causes and challenger ratios, plus a
      debug `[SlotChurn]` line per change. The fixes that followed:
      - the slot hold (`bHoldSeatedItems`, `fChallengerMargin` = 25%, v0.21.27);
      - `sPotionTierPreference`;
      - the combat/casting debounce and VitalEnvelope (v0.21.30-31), then
        target type for hostiles only, distance from the closest hostile
        rather than the crosshair, and the darkness enter delay (v0.22.5);
      - a dedup-emptied slot refilled in the same pass (#146).
      Result: 154 changes per 5 min went to ~35, near-ties to ~0, and
      swap-backs from 1 in 5 to ~1 in 8-11. The last simonrim run had 12/12
      key presses matching the widget and no dedup blanks. What is left, none
      of it urgent:
      - **Wildcards.** Churn by design, ~4 changes/min. No combat toggle
        (decided 2026-09-29): per-slot `bWildcardsEnabled` already keeps
        the keys that matter steady, and no play session showed a wildcard
        in the way mid-fight. Revisit only if one does.
      - **One potion in several slots.** Drowning put Waterbreathing Good
        (the override), Fair and Faint on screen together (2026-09-26
        14:56:43). The tier rule orders strengths; it does not say only one
        should show. Undecided whether it should.
      - **The equip flood.** Putting a spell in the weapon hand lifts every
        weapon (`fWeightNoWeapon` 0.40) and flooded six slots at x1.23-1.63.
        A 30% margin would stop most of it, but it has not been re-measured
        with the hold. Remembrance now puts the weapon you took off under your
        key; next is lowering the weight (see Remembrance's follow-ups).
      - **Blank slot at load.** LoreRim 2026-09-27 21:38:13: two duplicates
        cleared in the first frame while two used items moved. Slot 6 had
        nothing that was not already on another key and stayed empty for
        2.3 s. That is the designed behaviour; revisit only if it shows up
        mid-play.
      Raised 2026-09-24.

## Doc-migration findings (2026-08-29)
Surfaced by the one-agent-per-doc migration pass. Every one is a code or config
defect the docs exposed, not a documentation problem. Ordered by what a player
would notice.

- [ ] Spell-pattern override file was proposed and never implemented — no
      `m_patterns`, no `pattern=true` parsing, no `Huginn_SpellPatterns.ini`.
      The proposal lived in `reviews/magic-classification.md`, deleted
      2026-09-07; this entry is now the only record. Its 29%-unknown figure is
      one 2026-02-07
      capture on one modlist, taken before the classification pipeline changed
      underneath it, so the current rate is genuinely unknown — measure before
      scheduling. Related: `Huginn_Overrides.ini` is shared by `SpellRegistry`
      and `ItemRegistry` but the shipped template documents only item types, so
      the spell-type vocabulary `SpellOverrides` parses is undocumented (M)

## Architecture Critique — Backlog
Tiers 1 and 2 landed (PRs #55-#58); the critique document itself was never
committed. The only live work here is Tier 3.

### Tier 3 — hot-path perf (trace-prioritized; see docs/profiling/tracy-traces.md)
- [ ] **`Pipeline::ScoreCandidates` is ~27x its 2026-09-19 cost per call**
      (Debug, 178 µs -> 4.82 ms; the 2026-10-03 23:30 trace), and
      `Inventory::DeltaScan` ~12x (136 µs -> 1.62 ms). Split the zone
      (generate / context weights / learner predict / wildcards) before
      guessing; candidates (125-181 on LoreRim) and trained learner items
      (80) both grew since. Memory was flat over the session.
**Nothing in this tier exceeds 0.10% of runtime** on the 44:40 capture of
2026-08-26, which is the only capture long enough to trust — the 5-15 minute
runs that set the original ranking were dominated by cold calls. #14 is archived
(parts 1-3 merged, part 4 dropped on that measurement). On CPU grounds the rest
is a budget list, not a work list; treat a felt stutter, not a µs figure, as the
trigger to pick any of it up.
- [ ] #13 (= O1's sibling "O2"): count-only two-phase GetInventorySafe variant —
      Inventory::DeltaScan deep-copies InventoryEntryData per item at 2 Hz; plus
      PipelineContext container reuse (documented-but-broken).
      **Overtaken by #14 on MTPC, but climbing on total — measure before
      scheduling.** 2026-08-23: 257 µs MTPC / 24.46 ms. 2026-08-24 (15:24
      capture): 218.35 µs / 96.95 ms / 444 calls. 2026-08-26 (44:40): **267.78 µs
      MTPC / 1.03 s over 3,848 calls** — third by total, and now AHEAD of
      Display::Wheeler (862 ms) rather than behind it, because it runs ~4x as
      often. Still only 0.04% of runtime. The numbers that
      motivated this item, for reference: 685 µs x 1,118 = 766 ms on the 2026-07-25
      real save, up from 161 µs/call on the small test save — it scales with
      inventory size, so hoarder saves are the worst case. Constrained, not
      eliminable: this is the consumption detector and it needs a count snapshot
      (verified firing exactly once per consumption). The two levers are a longer
      interval (trades detection latency) or a count-only query that skips
      InventoryEntryData construction; the scratch maps are already allocation-free
      (m_scanCounts reuse), so the remaining cost is the SKSE query itself (M)
- [ ] O3: PollPlayerMagicEffects early-out — 113 µs x 6,916 = 780 ms on the
      2026-07-25 capture, the biggest cumulative POLL and the steady-state floor
      (every other poll is single- to low-double-digit µs). Runs on every tick by
      necessity: it is not gated by the skip-check, it FEEDS it. Options are an
      early-out when the active-effect list is unchanged, or caching by effect-list
      revision. Adjacent to #12 but not covered by it — #12 is PollTargets.
      **RECONFIRMED as the #1 zone by total time** on the 2026-08-26 44:40
      capture: 136.3 µs x 19,179 = **2.61 s (0.10%)**, ahead of PollTargets
      (2.17 s) and 3x Display::Wheeler (862 ms). The 2026-08-23/24 captures that
      ranked #14 above it were 5-15 min and too short for the per-tick pollers
      to accumulate. At 0.10% CPU this still does not need fixing; it is simply
      the honest top of Tier 3 and where to look first if idle cost ever
      matters (S/M)
- [ ] #12: PollTargets — build outside the write lock, one classification pass (scanned
      up to 3×/tick), MAX_TRACKED_TARGETS 50→~12, squared-distance compares (M).
      **Second by total time**, behind O3 (2026-08-26, 44:40 capture, Self-only):
      2.17 s over 19,179 calls at 113.34 µs MTPC, vs Display::Wheeler's 862 ms.
      A steady per-tick cost, not a spike, so it never shows as a hitch — at
      0.08% of runtime it is a CPU-budget item and not an urgent one
- [ ] #11: cache SpellData.effectiveCost — CandidateGenerator calls LookupByID +
      CalculateMagickaCost per known spell per tick, inside the registry lock (M)

### Follow-ups
- [ ] Modifier-key bindings (Ctrl+1, Alt+1, Shift+1) — every binding is a bare
      DirectInput scancode in a single `uint32_t` (`KeybindingSettings.cpp:22-31`,
      0 = unbound), so a slot key cannot be qualified and the ten slot keys must
      each own a whole key. Wants a modifier field per binding (or a packed
      scancode+modifier word, which keeps the INI a single value per slot but
      makes 0-means-unbound less obvious) plus modifier state in
      `InputHandler::ProcessButton`. The real design question is the interaction
      with the existing tap / double-tap / hold gestures: Ctrl+1 held is
      ambiguous against 1 held, and the modifier can be released mid-gesture, so
      decide whether the modifier is latched at key-down or sampled throughout.
      Also needs a conflict story for modifiers the game itself binds. Nexus page
      currently states "only single keypresses" — update it when this lands (M)
- [ ] A torch on a Huginn Wheeler wheel is unverified. Torches (#157) reach
      Wheeler through `AddItemByFormID`, and whether Wheeler accepts a LIGH
      form was never seen in play; a refusal is retried and then suppressed
      by WheelSync, so the worst case is a blank wheel entry. Check once
      with a torch on any key of the Huginn wheel (XS)
- [ ] **Share learning across similar items** — two versions of one idea, pick
      one. Today every item learns alone: `m_items[formID]` zero-initialises on
      first access, so a new item's `learningScore` is 0 and cannot compete with
      a trained item at 7-8. Nothing a player teaches Huginn about one healing
      potion transfers to another. This is the mechanism behind the scroll
      cold-start entry under Known Recommendation Issues, and it will bite any
      newly acquired item, not just scrolls.
      Huginn already has the right SHAPE — `ContextRuleEngine` and
      `PriorCalculator` are shared across all items, `FeatureBanditLearner` is
      per-item — but the shared layer is hand-authored rules, not fitted to
      data, so nothing LEARNED is ever pooled.
      Prompted by poLinUCB (arXiv:2309.13896), whose post-serving-context
      machinery does NOT fit Huginn (its `z` must be arm-independent, and it
      adds a second per-arm vector when our arms are already starved — see the
      session note below). The transferable part is its structure: the shared
      mapping is fitted from every round regardless of arm, so it sees ~K times
      more data than any per-arm parameter. Find what is common, fit it from
      everything, leave only the genuinely item-specific part to each item's
      scarce data.
      **A. Warm start (cheap, no format change).** Seed a first-seen item's
      weights from the mean of trained items sharing its classification instead
      of zero. Huginn is well set up for this — the taxonomy already exists and
      is already load-bearing for slot filters. No model change and no cosave
      format change; it is a different initialisation where `operator[]`
      currently default-constructs, so the SHAPE of what gets serialised is
      unchanged.
      Wrinkle that decides whether it works: `trainCount` is still 0, and
      confidence gates the learned term (50% at 5 trains), so seeded weights
      would be suppressed anyway. It needs a small pseudo-count — "worth about
      two observations". That number is the whole risk: too low and nothing
      changes, too high and an untried item inherits confidence it has not
      earned. Measure before believing any specific value.
      Note it changes learning BEHAVIOUR even though it does not change the
      format, so it would invalidate a soak run in progress even though it is
      technically landable during one (S)
      **B. Hierarchical weights (the real fix, cosave change).** Split each
      item's vector into a class part and a deviation, `w_item = w_class +
      delta_item`. Fit `w_class` from every item sharing a classification, and
      `delta_item` as a small heavily regularised per-item correction. Same
      pooling asymmetry as (A) but learned continuously rather than only at
      first sight, and it keeps improving the shared part as play continues.
      Needs a cosave format bump to store per-class vectors alongside per-item
      deltas, so it sits in the same parked bucket as #15/#16 — NOT landable
      during an active soak run (M/L)
      Neither is measured. (A) is cheap enough to try and discard; (B) should
      not be started until (A) has shown that pooling helps at all on real play
      data.
      **2026-10-02: moved behind "Learning swamps context"** (Known
      Recommendation Issues) -- fix the 12x balance first; see Next up.
      **Picked as next up (with the user, 2026-10-01).** Three criticisms it
      has to answer, in order:
      - **Decay.** Lazy decay is 2%/hour once an item is idle for 5 minutes
        (`Config.h` `DECAY_RATE_PER_HOUR`, `MaybeDecayBatch`): after 20 idle
        hours an item keeps ~2/3 of its weight. Only the weights decay --
        `trainCount` does not, so a long-unused item keeps full confidence.
        With pooling, a decayed item should fall back TOWARDS its class
        rather than towards zero, which is a better answer than a faster rate.
      - **Cold start.** The (A)/(B) split above.
      - **Item differentiation** -- later; it is closer to optimisation than
        to learning. The worry for pooling: potions are not told apart much
        today, so a class mean may be the same number for every potion in
        it and pooling would have no visible effect. Check first: dump the
        trained weight vectors grouped by classification and see whether
        the classes actually differ from each other, and whether items
        within a class differ. If not, pooling needs finer classes, or the
        differentiation work first.
      Memory is not a constraint: the learner is under 300 KB.
      **Pre-check result (LoreRim, 2026-10-01, `hg dump weights`, v0.22.7):
      classes do NOT differ, so pooling as written would do little.**
      - Thin data: 66 items, 298 trains. 34 items have 2 trains or fewer and
        9 have 10 or more, so mean confidence is low and the learned term is
        mostly gated off anyway.
      - Class does not predict weight shape. Mean cosine similarity between
        weight vectors, excluding food: 0.54 within a class, 0.63 BETWEEN
        classes. Food within its class: ~0.
      - The vectors record the STATE an item was used in, not the item. The
        always-on features (bias, healthPct, magickaPct, staminaPct, all ~1
        outside a fight) move together, and 29 of 66 vectors are a scaled
        copy of one or two feature vectors. Tier siblings (Restore Magicka
        Diluted / Faint / Fair) differ only in train count.
      - So a class-mean warm start (A) would seed nearly the same "used out of
        combat at full resources" vector into every class: a popularity prior,
        not item knowledge. Situational relevance lives in ContextRuleEngine,
        not in the learner.
      Implication: the bottleneck is what the features can say, not the lack
      of pooling. Before (A), decide what the learner should learn that the
      rules do not -- and note that a per-class or per-mode USE COUNT
      ("Behavioural modes as a recall stage") may capture everything the
      learner currently does. One save, small sample: re-run after a longer
      session before treating this as settled.
      **The sample is not representative (the user):** weights were reset
      often during recent patch testing, to simulate a new character, and
      test sessions poison them with false activations. The thin-data and
      cold-start numbers are still what a NEW character sees; the "classes
      do not differ" finding needs a clean save played normally to confirm.
      The cold start, measured the same day (`hg dump potions` / `scrolls`):
      63 of 69 carried potions and 46 of 52 carried scrolls have NEVER been
      trained. Most of the inventory is invisible to the learner, so it is
      the cold start, not the weights, that matters most.
- [ ] Addendum #15/#16 (Kalman learner / learnable context weights) — **parked**: needs a v3
      cosave bump, NOT landable during an active soak run
