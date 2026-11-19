# Hand-tuned correlations

Every place where a person, not the fit, set how a need relates to an effect column, or how hard a rule pushes the score. The engine is meant to learn these ([doc 9](9-context-as-learner-input.md)). A hand-set value is debt to pay down: prefer a value the fit derives or the learner moves, and retire these when the data can replace them.

Each entry says what it is, its value, where it lives, when and why it was set, and what would retire it. The obvious pairs come first. They are the bulk of it, and their full table at the end is generated.

## How a hand-set weight enters the model

- **Prior widths:** `core/Bootstrap.h:54-57`, the same values as `tools/fit/fitlib.py` `Model`.
  - An obvious pair starts at its sign with sigma 1 (precision 1).
  - Every other need × column pair starts at 0 with sigma 0.3 (precision 11.1).
  - b (per item) has sigma 0.5.
  - The fit and the plugin read the same widths. These widths are hand-chosen too.
- **The bootstrap file:** `configs/Huginn_Bootstrap.json` carries R6's fitted θ. A pair the file lacks gets the obvious pair's prior from `core/ObviousPairs.h` (`Obvious::PriorOf`), or the dense prior.
  - So a hand edit to the file's θ, or a pair added to `effects.csv`, both start the learner somewhere a person chose.
  - The learner moves all of them from there.

## Obvious pairs

- **Source:** `docs/architecture/9-data/effects.csv`, column `obvious_needs`. 273 pairs since 0.24.34.
- **Sign:** +1. A need written `-need` gets −1 (0.24.33). `combat_regen_ok × kind_food` is −1, special-cased below. Every `overshoot_*` column gets 0.
- **Generated:** `tools/effects/make_obvious_pairs.py` writes `src/core/ObviousPairs.h` and the table at the end of this page. `--check` fails when either is stale.
- **Checked:**
  - `tests/core/ObviousPairsTests.cpp` checks the header against the csv on every build, and pins the count.
  - `--check` is this page's only guard; no test reads the doc.
- **The fit:** `tools/fit/fitlib.py` `obvious_pairs()` reads the same column, so a refit starts from the same pairs.
- **What retires a pair:** the fit and the learner both move it. A pair is a starting point, not a constraint. A pair the user's picks contradict should be dropped from `effects.csv`, as `rate_per_magicka × magicka_deficit` was (below), rather than left to be unlearned every new save.

## Other hand-set strengths

| What | Value | Where | Set | Why | Retired by |
|---|---|---|---|---|---|
| Element resist pairs: `enemy_casting_X`, `target_element_X` and `X_damage_rate` × `resist_X`, for fire, frost and shock (9 pairs) | 2.5 (the fit had ~1.0); precision ~1 kept | `configs/Huginn_Bootstrap.json` θ; `tools/fit/fitlib.py:172` `PAIR_STRENGTH`, so a refit keeps the start | 0.24.29, #254 (log of 2026-11-09) | At ~1.0 the pair's +0.8 was cancelled by the fit's consumable-in-combat terms. A Resist Fire potion ranked 21st–137th while a caster threw fire, and rose only once the player burned. At 2.5 it was in the top 8 in 52 of 78 in-fight moments with fire shown. | A refit on logs with element fights, with `PAIR_STRENGTH` dropped; meanwhile the learner moves it |
| `ally_injured × restore_health_other` | 4.19213, copied from `health_deficit × restore_health` (the fit left it at 0.9996, its prior); precision 1 | `configs/Huginn_Bootstrap.json`: θ and `provenance.hand_edits`. `core/Bootstrap.h:296` `kReseededTheta` re-seeds it in a save that learned from the older file | 0.24.31, #256, 2026-11-13 | The fit had no follower data. A heal for a downed follower ranked 202nd of 219 in game. | A refit with follower picks in the logs. **Not in `PAIR_STRENGTH`: a refit today puts it back to 1.0** |
| `covered` starting weight (`theta_covered`): how much of an item's effect is already running on you | −1.0, precision 1 | `configs/Huginn.ini:581` `[Scoring] fCoveredPrior` (range −10 to 10); default `core/Bootstrap.h:146`; applied in `ChoiceLearner.cpp:216` | 0.23.56, R8.2, #226, 2026-10-29 | A buff that's already up should be offered less | Learned online from the start value; only the start is hand-set |
| Owned rows: a pick's evidence about a heal for someone else (`restore_health_other`) moves only `ally_injured`'s weights, unless the row also carries `restore_health` | one entry: `restore_health_other` → `ally_injured`, unless `restore_health` | `src/core/ChoiceUpdate.h:213` `kOwnedColumns` | 0.24.33 (2026-11-14), unless clause 0.24.34; #258 | Passing over a follower heal mid-fight blamed the ten-odd fight needs active at once. "Not now" was learned as "never in a fight": −2 to −4 on Healing Aura on Target mid-fight. | A general rule that splits a pick's evidence across needs by each need's share of the row's score (review of #258) |
| `heal_slow`: a restore or regen-health row of 60 s or more makes the item a slow heal, judged on its fastest heal row | 60 s; its pairs (fight start +1, downtime +1, in combat −1) are in the table | `src/core/EffectMapper.h:222` `kSlowHealSec` | 0.24.33; fastest-row rule 0.24.34; #258 | LoreRim's Restore Health potions run 20 s and its healing auras 300 s. A heal over a minute saves nobody mid-fight. | The learner moves the pair weights. The 60 s cut is fixed; a cut derived from the population of heal durations would retire it |
| `combat_regen_ok × kind_food` | −1, special-cased in code rather than written `-combat_regen_ok` | `tools/effects/make_obvious_pairs.py:52` and `tools/fit/fitlib.py:187`, the same rule twice | 0.23.30 (per `ObviousPairsTests.cpp`) | No eating in combat, unless health regeneration is suppressed | Rewrite it as `-combat_regen_ok` in `effects.csv` (the 0.24.33 syntax) and drop both special cases; then the learner, as for any pair |
| `overshoot_*` pairs | 0 (wide prior, no sign) | `make_obvious_pairs.py`, `fitlib.py` (`overshoot_` prefix) | R6 | The logged overshoot is signed and has no response curve, so no sign is obvious | The learner |
| **Removed:** `rate_per_magicka × magicka_deficit` | was +1 | was `effects.csv` `obvious_needs` | added and dropped in 0.24.34, #258 (2026-11-15 and 2026-11-18) | It lifted every heal spell by about +1 exactly when magicka was too low to cast it, above the Restore Health potion the user drank. The user: potions are for the player character. | Dropped: the column starts at 0 and learns its sign |

## Thresholds that put an item on a key

These aren't θ, but they encode a need → item rule that bypasses the ranking.

| What | Value | Where | Set | Retired by |
|---|---|---|---|---|
| CriticalHealth, CriticalMagicka and CriticalStamina overrides: a vital below the threshold puts a restore potion on a key, until the vital is back above threshold + hysteresis | 0.35, hysteresis 0.15 (released at 50%) | `configs/Huginn.ini:316`, `:327`, `:338` `[Overrides] fCritical<Vital>Threshold` / `Hysteresis` | before the rewrite (in the 2026-02-28 migration) | The user's call. Since 0.24.34 the ranking puts a health potion on the page about 20 s before CriticalHealth fires (2026-11-18), so the override is now a safety net. |
| CriticalAllyHealth override: the most hurt teammate a heal can work on (not undead), within reach and in a fight, below the threshold or down on one knee puts the best heal-for-others spell the player can afford on the health key, until they are back above threshold + hysteresis. It sits below CriticalHealth on that key, never offers a potion, and keeps the spell already on the key while magicka is short | 0.35, hysteresis 0.15 (released at 50%); reach 1500 units (about 21 m); priority 65 | `configs/Huginn.ini:357`-`:359` `[Overrides] fCriticalAllyHealthThreshold` / `Hysteresis` / `MaxDistance`; `src/override/OverrideManager.cpp` `EvaluateCriticalAllyHealth`; `src/core/AllyHeal.h` | 0.24.35 / 0.24.37 (#259, 2026-11-19), after Healing Hands sat at #17-#110 for 30 s in the first fight after a weight reset (2026-11-18) | When the ranking reliably puts the follower heal on a key, without waiting on the combat freeze, in the first fight after a reset. On 2026-11-19 (0.24.34) it reached a key 6 s after the follower was hurt, all of it the freeze's wait. |

## Also hand-set, not covered here

- **Response curves:** the shape of each need's curve, for example `health_deficit` logistic with c about 0.5. They are the defaults in `docs/architecture/9-data/needs.csv`, overridable per need in `[Needs]` (`configs/Huginn.ini:741`). The bootstrap carries a fitted curve only for encumbrance. They set how strongly a need reads, not which effect it pairs with.
- **D2's old layer:** `[ContextWeights]`, `CorrelationBooster`, `PotionDiscriminator`, `fMinimumUtility` and the old floors. These reach only engine A in the shadow (`learning/legacy/`, confined by `tools/prune/r8_check.py`). They retire with D2 ([implementation map](9-implementation-map.md)).

## The obvious-pair table

<!-- BEGIN obvious-pairs: generated by tools/effects/make_obvious_pairs.py from effects.csv; do not edit by hand -->

273 pairs on 102 effect columns. +1 starts the weight at +1, -1 at -1 (written `-need` in effects.csv), 0 at 0 with the obvious pair's wider prior (sigma 1).

**restore** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `restore` | `regen_suppressed` |  |  |
| `restore_health` | `health_deficit`, `health_falling`, `restore_pending_health`, `fire_damage_rate`, `frost_damage_rate`, `shock_damage_rate`, `magic_damage_rate`, `physical_damage_rate`, `poisoned`, `drain_health`, `healing_blocked`, `vampire_sun_exposure`, `underwater`, `falling`, `combat_ended_recent` |  |  |
| `restore_magicka` | `magicka_deficit`, `magicka_burn`, `restore_pending_magicka`, `shock_damage_rate`, `drain_magicka`, `loadout_destruction`, `loadout_conjuration`, `loadout_restoration`, `loadout_alteration`, `loadout_illusion`, `combat_ended_recent` |  |  |
| `restore_stamina` | `stamina_deficit`, `stamina_burn`, `restore_pending_stamina`, `frost_damage_rate`, `drain_stamina`, `fatigue`, `loadout_two_handed` |  |  |
| `restore_health_other` | `ally_injured`, `follower_present` |  |  |

**fortify_vital** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `fortify_vital` | `combat_onset`, `boss_fight` |  |  |
| `fortify_vital_health` | `health_deficit` |  |  |
| `fortify_vital_magicka` | `magicka_deficit`, `magicka_burn` |  |  |
| `fortify_vital_stamina` | `stamina_deficit`, `stamina_burn`, `fatigue`, `swimming`, `loadout_two_handed` |  |  |

**regen** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `regen` | `regen_suppressed` |  |  |
| `regen_health` | `health_deficit`, `restore_pending_health`, `poisoned`, `drain_health`, `vampire_sun_exposure` |  |  |
| `regen_magicka` | `magicka_deficit`, `magicka_burn`, `restore_pending_magicka`, `drain_magicka`, `loadout_destruction`, `loadout_conjuration`, `loadout_restoration`, `loadout_alteration`, `loadout_illusion` |  |  |
| `regen_stamina` | `stamina_deficit`, `stamina_burn`, `restore_pending_stamina`, `drain_stamina` |  |  |

**damage** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `damage` | `in_combat`, `enemy_mid`, `target_health_low`, `hands_empty` |  |  |
| `damage_health_fire` | `target_troll` |  |  |
| `damage_health_sun` | `target_undead` |  |  |
| `damage_magicka` | `target_magicka_low` |  |  |
| `damage_stamina` | `target_stamina_low` |  |  |

**fortify_skill** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `fortify_skill` | `magicka_burn`, `combat_onset`, `boss_fight`, `loadout_staff` |  |  |
| `fortify_skill_one_handed` | `loadout_one_handed` |  |  |
| `fortify_skill_two_handed` | `loadout_two_handed` |  |  |
| `fortify_skill_archery` | `enemy_far`, `loadout_archery`, `mounted` |  |  |
| `fortify_skill_block` | `physical_damage_rate`, `enemy_close`, `target_archer`, `loadout_shield` |  |  |
| `fortify_skill_smithing` | `workstation_smithing` |  |  |
| `fortify_skill_alchemy` | `workstation_alchemy` |  |  |
| `fortify_skill_enchanting` | `workstation_enchanting` |  |  |
| `fortify_skill_sneak` | `sneaking`, `sneak_detected` |  |  |
| `fortify_skill_lockpicking` | `lock_in_crosshair` |  |  |
| `fortify_skill_speech` | `merchant_in_crosshair` |  |  |
| `fortify_skill_alteration` | `loadout_alteration` |  |  |
| `fortify_skill_conjuration` | `loadout_conjuration` |  |  |
| `fortify_skill_destruction` | `loadout_destruction` |  |  |
| `fortify_skill_illusion` | `loadout_illusion` |  |  |
| `fortify_skill_restoration` | `loadout_restoration` |  |  |

**fortify_combat** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `fortify_combat` | `in_combat` |  |  |
| `fortify_combat_sneak_attack` | `sneaking` |  |  |

**summon** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `summon` | `health_falling`, `in_combat`, `combat_onset`, `enemy_count`, `enemy_close`, `enemy_mid`, `boss_fight` |  |  |
| `summon_bound_weapon` | `ammo_low`, `hands_empty` |  |  |

**absorb** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `absorb_magicka` | `target_magicka_low` |  |  |

**resist** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `resist_fire` | `fire_damage_rate`, `enemy_casting_fire`, `target_dragon`, `target_element_fire` |  |  |
| `resist_frost` | `frost_damage_rate`, `enemy_casting_frost`, `target_dragon`, `target_element_frost` |  |  |
| `resist_shock` | `shock_damage_rate`, `enemy_casting_shock`, `target_element_shock` |  |  |
| `resist_poison` | `poisoned`, `target_arthropod` |  |  |
| `resist_magic` | `magic_damage_rate`, `enemy_casting`, `target_caster` |  |  |

**defense** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `defense_armor` | `health_deficit`, `health_falling`, `physical_damage_rate`, `in_combat`, `combat_onset`, `surrounded`, `target_archer`, `loadout_one_handed`, `loadout_two_handed` |  |  |
| `defense_ward` | `health_deficit`, `health_falling`, `fire_damage_rate`, `frost_damage_rate`, `shock_damage_rate`, `magic_damage_rate`, `enemy_casting_fire`, `enemy_casting_frost`, `enemy_casting_shock`, `enemy_casting`, `target_caster`, `target_magicka_low`, `loadout_shield` |  |  |
| `defense_spell_absorb` | `magic_damage_rate`, `enemy_casting_fire`, `enemy_casting_frost`, `enemy_casting_shock`, `enemy_casting`, `target_caster` |  |  |
| `defense_ethereal` | `falling`, `drop_ahead` |  |  |

**control** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `control_paralysis` | `health_falling`, `enemy_close`, `target_health_low`, `target_stamina_low`, `target_humanoid` |  |  |

**influence** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `influence_fear` | `health_falling`, `enemy_count`, `enemy_close`, `surrounded`, `target_humanoid`, `target_animal`, `sneak_detected` |  |  |
| `influence_calm` | `enemy_count`, `surrounded`, `target_humanoid`, `target_animal`, `sneak_detected` |  |  |
| `influence_rally` | `follower_present` |  |  |
| `influence_turn_undead` | `target_undead` |  |  |
| `influence_banish` | `target_summoned` |  |  |

**cure** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `cure_disease` | `diseased`, `drain_health`, `regen_suppressed`, `combat_ended_recent` |  |  |
| `cure_poison` | `poisoned`, `drain_health`, `combat_ended_recent`, `target_arthropod` |  |  |
| `cure_dispel` | `drain_health`, `drain_magicka`, `drain_stamina` |  |  |

**vision** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `vision_light` | `darkness` |  |  |
| `vision_night_eye` | `darkness` |  |  |

**stealth** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `stealth_invisibility` | `vampire_sun_exposure`, `enemy_count`, `target_archer`, `sneaking`, `sneak_detected` |  |  |
| `stealth_muffle` | `sneaking`, `sneak_detected` |  |  |

**movement** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `movement_jump_fall` | `falling`, `drop_ahead` |  |  |

**utility** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `utility_carry_weight` | `encumbrance` |  |  |
| `utility_water_breathing` | `underwater`, `swimming`, `deep_water_ahead` |  |  |
| `utility_unlock` | `lock_in_crosshair` |  |  |

**survival** (family)

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `survival_hunger` | `healing_blocked`, `hunger` |  |  |
| `survival_thirst` | `healing_blocked`, `thirst` |  |  |
| `survival_warmth` | `cold`, `warmth_deficit` |  |  |
| `survival_fatigue` | `fatigue` |  |  |

**Modifiers**

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `delivery_aimed` | `enemy_far` |  |  |
| `area` | `enemy_count`, `enemy_mid`, `surrounded` |  |  |
| `aura` | `combat_onset`, `enemy_close`, `surrounded` |  |  |
| `timing_over_time` | `boss_fight` |  |  |
| `heal_slow` | `combat_onset`, `downtime` | `in_combat` |  |
| `total_restore_health` | `downtime` |  |  |
| `total_restore_magicka` | `downtime` |  |  |
| `total_restore_stamina` | `downtime` |  |  |
| `total_restore_health_other` | `downtime` |  |  |

**Item features**

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `kind_spell` | `mounted` |  |  |
| `kind_staff` | `magicka_deficit` |  |  |
| `kind_poison` | `boss_fight`, `target_humanoid`, `loadout_one_handed` |  |  |
| `kind_food` | `downtime` | `combat_regen_ok` |  |
| `kind_weapon` | `hands_empty`, `magicka_deficit` |  |  |
| `kind_ammo` | `enemy_far`, `target_dragon`, `ammo_low`, `loadout_archery`, `mounted`, `target_giant` |  |  |
| `kind_light` | `darkness` |  |  |
| `overshoot_health` |  |  | `health_deficit` |
| `overshoot_magicka` |  |  | `magicka_deficit` |
| `overshoot_stamina` |  |  | `stamina_deficit` |
| `ammo_matches_launcher` | `loadout_archery` |  |  |
| `soulgem_charge` | `weapon_charge_deficit`, `loadout_staff`, `combat_ended_recent` |  |  |

**Weapon stats**

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `weapon_type_dagger` | `sneaking` |  |  |
| `weapon_type_bow` | `sneaking` |  |  |
| `weapon_melee` | `enemy_close`, `target_caster`, `target_archer`, `target_magicka_low`, `ammo_low` |  |  |
| `weapon_ranged` | `enemy_far`, `target_health_low`, `target_dragon`, `target_animal`, `mounted`, `target_giant` |  |  |
| `weapon_two_handed` | `target_stamina_low` |  |  |
| `weapon_damage` | `in_combat` |  |  |
| `weapon_enchanted` | `weapon_charge_deficit` |  |  |
| `weapon_silver` | `target_undead`, `target_daedra`, `target_werebeast` |  |  |
| `weapon_pickaxe` | `ore_vein_in_crosshair` |  |  |

**Armour stats**

| Column | +1 | -1 | 0 |
|---|---|---|---|
| `armour_slot_shield` | `physical_damage_rate`, `enemy_close`, `target_archer` |  |  |
| `armour_enchanted` | `downtime` |  |  |
| `armour_warm` | `warmth_deficit` |  |  |

<!-- END obvious-pairs -->
