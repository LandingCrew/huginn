# Soak run, October 2026 -- LoreRim 5

The Phase 2 soak run (roadmap, "Next up"), played to the protocol in
[LongPlaySoak.md](LongPlaySoak.md). This is the baseline the Phase 3 learning
rework is measured against.

## Summary

- **9.1 hours of play over 11 launches**, 2026-10-02 21:41 to 2026-10-04
  15:26, one character on LoreRim 5. **Zero errors, zero Huginn crashes.**
- **Goal 2 held:** 389 of 418 Huginn presses (93%) came from the page of
  eight Regular keys; the labeled Jobs page took 29.
- **Goal 1 did not fall over time:** 92 reach-ins. They tracked what each
  session asked of the kit -- 41 in the two-hour dungeon session, 1-2 in the
  first town-and-road sessions -- not how long the learner had trained. The
  causes are gaps in what Huginn can recognise and offer, listed under
  "What the run found".
- **The learner helps, and crowds:** the live page held the chosen item 81%
  of the time against 58% for the same pool ranked on context alone. But on
  the picks made from menus, context alone would have offered 13 where the
  live page offered 7 -- learned weight on a few items pushes others off.
- **Wildcards barely reach the player:** placed 333 times, chosen 8 times.

## Setup

| | |
|---|---|
| Character | `3F3E2A8817962D59` (derived ID: the save predates character IDs), LoreRim 5 on Skyrim 1.6 (Stock Game) |
| Start | `hg reset weights` + save, 2026-10-02 21:41:52 |
| Layout | Page 0 "Huginn": eight Regular keys, overrides HP/MP/SP/Other on keys 1/2/3/7. Page 1 "Jobs": the old one-job-per-key page, kept as a measured fallback |
| Builds | Debug + Tracy. 0.22.11 (`773744a`) -> 0.22.12 (`fdb0b95`, `hg dump diseases`) -> 0.22.13 (`42254bf`, Tracy zones) -> 0.22.14 (`be4e452` / `4616fd4`, spell-cost cache). None of the mid-run builds changed learning or scoring |
| Protocol | Played normally, no staged equips (LongPlaySoak.md) |

## Sessions

| Start | End | Min | Build | Selections (outside) | Presses (Regular / Jobs) | Page flips | Reach-ins | Tick avg (ms) | Learner items / trains |
|---|---|---|---|---|---|---|---|---|---|
| 10-02 21:41 | 22:34 | 53 | 0.22.11 | 21 (2) | 17 (4 / 13) | 9 | 1 | 0.97 | 11 / 13 |
| 10-02 22:48 | 22:53 | 4 | 0.22.11 | 5 (0) | -- | 0 | 0 | -- | quit unsaved |
| 10-02 23:11 | 00:08 | 57 | 0.22.11 | 50 (2) | 63 (61 / 2) | 5 | 2 | 1.18 | 31 / 71 |
| 10-03 19:23 | 21:21 | 118 | 0.22.11 | 138 (43) | 113 (109 / 4) | 6 | 41 | 1.50 | 64 / 206 |
| 10-03 21:31 | 23:26 | 115 | 0.22.12 | 80 (21) | 70 (66 / 4) | 8 | 21 | 1.42 | 71 / 288 |
| 10-03 23:51 | 00:54 | 63 | 0.22.12 | 45 (10) | 44 (43 / 1) | 6 | 10 | 1.58 | 80 / 334 |
| 10-04 09:55 | 09:58 | 3 | 0.22.12 | 0 | -- | 0 | 0 | -- | -- |
| 10-04 10:11 | 10:45 | 34 | 0.22.13 | 30 (4) | 25 (25 / 0) | 0 | 4 | 1.49 | 84 / 360 |
| 10-04 12:01 | 13:11 | 70 | 0.22.14 | 65 (7) | 63 (60 / 3) | 4 | 7 | 1.26 | 91 / 427, not saved |
| 10-04 14:48 | 15:16 | 28 | 0.22.14 | 23 (6) | 23 (21 / 2) | 8 | 6 | 1.21 | 88 / 383 |
| 10-04 15:24 | 15:26 | 2 | 0.22.14 | 0 | -- | 0 | 0 | -- | end-of-run dumps |
| **Total** | | **547** | | **457 (95)** | **418 (389 / 29)** | **46** | **92** | | **88 / 380** at the end |

Presses, flips, reach-ins and tick time come from the `[Soak]` heartbeats
(5-minute windows; the tail after a session's last heartbeat is not
counted). Selections are confirmed `[Selection]` records. The 10-03 19:23
session was Windward Ruins and High Gate Ruins; 10-04 12:01 the spider
fight; 10-04 14:48 an arrest. Two sessions were never saved -- 10-02 22:48
and 10-04 12:01 -- so their learning did not carry: the 14:48 load
restored the 10:45 state (84 items, clock 363), which is why the run ends
at 88 items and 380 trains rather than above the 91 / 427 the 12:01
session reached. The 10-04 00:54 session ended in an infinite
loading screen -- not Huginn: its log ends cleanly with `Pipeline suspended
(world unloaded)`, and CrashLogger wrote nothing.

## Goal 1 -- reach-ins

92 in 9.1 hours. Per hour by session: 1.1, 2.1, **20.8**, 10.9, 9.5, 7.1,
6.0, 12.9 -- driven by the content, not the clock. The early sessions were
roads and towns with a small kit; the dungeon session used the whole kit
and exposed every gap at once. The outside selections behind them (95
confirmed) came from the magic menu 40 times, the inventory 36, the
favourites/quick menu 20.

What they were, grouped (from the session notes in the roadmap's "Field
notes" and the per-session checks):

| Group | Why Huginn did not have it |
|---|---|
| Healing spells (Healing, Healing Touch, Healing Aura, Wild Healing) | Healing spells are filtered out at full health -- wrong for an aura or a pre-cast; self and target versions are one thing to Huginn |
| Non-restore potions (Fortify Destruction / Two-Handed / Marksman / Magicka, Speed, Resist Frost / Fire, Carry Weight, Dispel, Cure Poison / Disease) | Fortify potions are matched only to crafting; nothing links a buff to the equipped weapon or school; encumbrance has no rule; poison and disease are never detected |
| Higher-rank and situational spells (Stoneflesh, Sunbeam, Night Eye, summons) | Learned weight on Oakflesh and the battlestaff crowds them out; a Gloom Wraith read as Humanoid; the summon rule ignores being pressed in melee |
| Weapons (a second bow, swords) | Ranked behind the learned main weapon |
| Food, waterskin | Hunger weight drops at Peckish; the waterskin is scripted and never a candidate |

## Goal 2 -- labeled and regular presses

389 Regular, 29 labeled (93% Regular), 46 page flips. The first session
leaned on the Jobs page (13 of 17 presses, mostly food on the healing key);
from the second session on the eight Regular keys did nearly everything.
That is the end state the roadmap measures Huginn against, and the run
says the slot filters are rarely needed with this layout.

## A|B -- does the learner help?

`Huginn_AB.log`, page-0 selections, tallied per launch and summed over the
run (412 selections):

| Page | Chosen item on it |
|---|---|
| A -- the live page | 335 (81%) |
| A* -- the live utility, ranked plainly (no overrides or holds) | 315 (76%) |
| B -- context only (learning factor removed) | 237 (58%) |
| B' -- no learned weights, prior and recency kept | 231 (56%) |

The live page beats context alone by 23 points. B-not-A was 8 (B'-not-A
7): Stoneflesh on Self (context rank 2, live 11), Stoneflesh on Target,
the Ancient Nord Light Bow, the Elven Bow of the Blaze (context **1**,
live 15), Transmute Night Eye -- items context ranked high and the
learned weight on Oakflesh, the battlestaff and Soul Sword pushed down.

**On the 83 selections made from menus** the live page had the item 7
times; context alone would have had it 13. So the learner wins overall by
holding the player's habits, and loses where the player needs something
off-habit -- the case Phase 3's choice target (a capped learned term)
exists for.

## The learner at the end

`hg dump weights`, 2026-10-04 15:26: **88 items, 380 trains.**

| Kind | Items | Trains |
|---|---|---|
| Spell | 21 | 128 |
| Weapon | 10 | 116 |
| Potion | 26 | 71 |
| Food | 17 | 41 |
| Scroll | 9 | 14 |
| Soul gem / torch / apparel | 5 | 10 |

- **Concentrated:** Soul Sword alone has 61 trains (16%) -- the main weapon
  re-equipped after every scroll and spell, each a full selection. The top
  12 items hold 58%: Soul Sword 61, Healing 27, Wooden Battlestaff 23,
  Restore Health (Faint) 18, Oakflesh on Self 14, Restore Health (Fair) 13,
  Sunbeam 13, Stoneflesh on Self 13, Healing Aura on Self 10, Ancient Nord
  Light Bow 10, Elven Bow of the Blaze 10, Healing Touch 9.
- **A long tail:** 48 of the 88 were trained once.
- **Still growing:** 11 -> 31 -> 64 -> 71 -> 80 -> 84 -> 91 items across the
  sessions (91 in the unsaved 12:01 session; 88 at the end). Entries are never dropped, so the count is everything ever
  chosen, not what is carried (roadmap: expire missing items).

## Wildcards and overrides

- **Wildcards** were placed 333 times (slot changes) and chosen **8** times
  of 370 selections from the page (2%). The user avoided Fear, Courage and
  Rage on purpose to test them; exploration is not reaching the player.
- **Overrides** activated: low health 58, low magicka 68, low stamina 50,
  weapon charge 13, low ammo 16, drowning 2. They supplied 64 of the 370
  page selections (17%). Drowning put Waterbreathing on key 7 (10-04
  00:40); the emergency keys landed on 1/2/3/7 as configured.

## Stability and performance

- **Errors: 0** across the 11 launches. One infinite loading screen, not
  Huginn's (above).
- **Memory flat** at ~1.6 MB over two Tracy captures (1:25 and 1:15).
- **Tick average 0.97-1.58 ms** (Debug + Tracy), no upward trend across
  sessions or within the long ones.
- **Reloads and learning:** both same-character reloads in the run (10-02
  22:31, after the reset; 10-04 10:43) kept the in-memory learning, and the
  reset was not undone by reloading the older save. The other #165 cases
  (a later save, another character, a new game) were verified on simonrim
  before the run.
- **The arrest** (10-04 15:16): 101 potion and 51 scroll stacks confiscated
  in one scan tripped the teardown guard -- no false "consumed" rewards.
- **Performance:** `Pipeline::ScoreCandidates` went from 4.82 ms to ~1.5 ms
  a run (Debug) with the spell-cost cache (#168); `Inventory::DeltaScan` is
  now the largest cost. Details in
  [profiling/tracy-traces.md](../profiling/tracy-traces.md).

## What the run found

Each is a roadmap entry with its evidence ("After the soak, not learning",
"Field notes", "Enemy detection, a future release", Tier 3 perf):

- **Detection bugs:** disease detection sees 1 of LoreRim's 94 diseases;
  poison never fired (confirmed in a spider fight, Cure Poison taken from
  the menu); darkness fires outdoors in daylight (tree shadow); target type
  misses by race name (Gloom Wraith -> Humanoid) and flickers Undead <->
  None within a fight; healing spells filtered at full health; self and
  target spells classified alike.
- **Recommendation gaps:** buffs not matched to the loadout (fortify
  potions, enchanted gear by weapon type); resist potions weighted by a
  single hit; poisons with no weight; encumbrance with no rule; summon when
  pressed in melee; damage over time on a boss; haggling gear at a
  merchant; hunger as two steps, not a ramp; thirst (unparked: waterskins
  refill free); LoreRim's survival needs can block incoming healing; the
  emergency potion should learn smallest-that-covers; no second restore on
  over-time lists.
- **Learning:** learned weight crowds out higher ranks and off-habit items
  (A|B above); a re-equip of the main weapon counts as a full choice;
  entries never expire.
- **Outside Huginn:** WheelerAPI's soul-gem recharge cannot find a held bow
  (report in the wheelerAPI repo); LoreRim's Cure Disease now cures in 1-4
  days.

## Ride-along checklist (roadmap, Phase 2)

| Check | Result |
|---|---|
| Torch on a Huginn wheel | Shown on the Huginn page all along and pressed by key; never picked from the wheel |
| Arrest, yield, re-engage | Passed: combat ended cleanly, confiscation tripped the teardown guard |
| Restore Health/Stamina in a fight that is not a kite | Passed: emergency keys used in dungeon fights |
| `hg rebuild` -- does `870710C4` register? | No, correctly: a non-playable, nameless Requiem weapon. The warning at each load is noise |
| uid87 Long Bow ExtraHealth | Not seen; only the Unarmed entries read 0 |
| The equip flood with the slot hold in | Not checked directly. Slot churn: median 36 changes per 5 minutes (the post-fix level the roadmap records), peak 141 in dungeon combat |

## Not covered

Falmer; a second character; a single session over two hours; a Release +
Tracy capture (all captures were Debug); a drowning with no Waterbreathing
item carried.

## Using this as the Phase 3 baseline

The numbers to compare against are the A|B rates above (live 81%, context
58%, menu picks 7 vs 13) and the reach-in groups. The run's data:

- `Huginn_Selections.jsonl`, records with `"char":"3F3E2A8817962D59"` and
  `"launch"` >= `20261003-013602` -- 457 selections with the full scored
  candidate list each, for re-ranking under a new formula offline.
- `Huginn_AB.log`, lines with `char=3F3E2A8817962D59` from the same launch
  on.
- The kept debug logs in `SKSE\HuginnLogs\` (the newest 20 are kept --
  copy them out before more launches push the early ones off).
- The benchmark save from 2026-10-02 21:41, after `hg reset weights`, to
  replay Phase 3 against the same character.
