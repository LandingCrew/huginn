# Huginn

Huginn is an SKSE plugin that watches how your fight is going and keeps your hotkeys stocked with whatever suits the moment — the right spell, potion, weapon, or item, already on the key, before you have to go digging for it in the menus.

It also learns as you play. Huginn notices what you actually reach for in each kind of situation and starts offering that sooner. And it never acts on its own: it puts the item on the key, you decide whether to press it.

Huginn provides a HUD widget showing the rolling state of each hotkey.

## Why 

Huggin is an attempt to solve the issue of inventory and spell selection without having the player to remember if they have a potion, spell, or item for the job. Ideally Huggin abstracts away the inventory and ... 

No... Why Huggin? oh thats becuase [Huggin is the raven that sits on Odins shoulder bringing knowledge and gossip.](https://en.wikipedia.org/wiki/Huginn_and_Muninn) That and there are already one to many Mimirs named mods.

## Features

* **Context-aware recommendations** — analyzes health, magicka, stamina, distance, enemy type, combat state, and sneak to suggest relevant equipment
* **Learns as you play** — observes what you equip in each situation, bootstrapped with sensible defaults
* **On-screen widget** — a small overlay showing what is on each key; hides itself outside combat
* **Wheeler integration** — optional [Wheeler](https://www.nexusmods.com/skyrimspecialedition/mods/97345) radial menu support
* **A job for every key** — the default page gives each key a role (weapon, attack magic, heal, defend…) with emergencies on fixed keys; six ready-made templates for other builds
* **Multi-page slots** — organize recommendations by role (up to 10 pages, 10 slots each)
* **Swap back** — press a key that swaps what is in your hand (or your arrows), and what it replaced waits under that same key for 15 seconds; press again to swap back
* **Workstation awareness** — Fortify Smithing at forges, Fortify Enchanting at enchanters
* **INI-configurable** — context weights, scoring, slot layout, keybindings, display mode

## Dependencies and Installation

### Required Dependencies 

- [Skyrim SE 1.5.39+ or Skyrim AE](https://store.steampowered.com/app/489830/The_Elder_Scrolls_V_Skyrim_Special_Edition/)
- [SKSE](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

Huginn uses dMenu as its settings GUI

### Install

Huginn is entirely self contained with its own rendering pipeline. Install the dependencies then install Huginn using your favorite mod manager.

### Upgrading

[PLACEHOLDER] If you are uninstalling or upgradeing you might want to `hg reset weights` or press the dMenu button "Reset Q-Table" "Reset Learned Weights".  Huginn stores its learned preferences in an SKSE co-save under a record name.

### Uninstall

Huginn can be installed and uninstalled at anytime. Just delete or disable the mod from your mod manager. 

## Configuration and Usage

Huginn reads three files, each with a distinct job:

| File | Location | What it controls |
|---|---|---|
| `Huginn.ini` | `Data/SKSE/Plugins/` | Slots, pages, keybindings, scoring, Wheeler |
| `Huginn_Overrides.ini` | `Data/SKSE/Plugins/` | Classification fixes for modded items and spells |
| dMenu `Huginn.ini` | `Data/SKSE/Plugins/dmenu/customSettings/ini/` | Widget appearance and debug options |

### Huginn.ini

**Configuration Location:** `Data/SKSE/Plugins/Huginn.ini`

The majority of the recommendation algorithm is configured from this file. It is re-read every time you load a save, and can be hot-reloaded in game with `hg reload`.

> A note before you tune. The defaults below are chosen for steady keys: once something lands on a key, Huginn prefers to leave it there rather than chase the newest best pick. You can retune Huginn to react faster, but the keys will then change more often with newer recommendation; tune it the other way and it gets calmer but slower to catch up with lagging recommendations. The settings that decide this are under Steadiness — [SlotLocker] below. This is mostly user preference. 

#### Basics

Huginn is configured as **slots**, grouped into **pages**, with `[Keybindings]` tying a slot to a key. `[Page0.Slot0]` is driven by `iSlot1Key` — slots are numbered from 0, keys from 1 — meaning Huginn keeps that hotkey stocked with its current best pick, swapping it out as the situation changes. Alternatively, WheelerAPI can be used to surface each page as a Wheeler wheel and each slot as a wheel entry.

You can create up to 10 pages containing up to 10 slots per page. `[Pages]` sets `iPageCount`; each `[PageN]` sets `sName` and `iSlotCount` in the ini.

#### Slot configuration

Every slot takes the same six settings:

| Setting | What it does |
|---|---|
| `sClassification` | What is allowed in this slot — see the list below |
| `bWildcardsEnabled` | Let Huginn occasionally offer something outside its usual pick, so it can find out what else you like |
| `bOverridesEnabled` | Whether an emergency can take this slot over. `HP`, `MP` or `SP` for a health, magicka or stamina emergency; `Other` for the soul gem, low ammo and drowning prompts; `Any` for all of them; `None` to leave the slot alone |
| `bSkipEquipped` | Skip anything already in your hands, so the slot shows you an alternative instead. On by default; `bSkipEquipped = 0` lets a slot show a weapon you are holding (to double-tap it into the other hand, say) |
| `bRemembrance` | Swap back: when you press this slot to equip something, what it took off waits under the same key for a while (see Steadiness below). On by default; `bRemembrance = 0` turns it off for this slot |
| `iPriority` | Which slots get first pick of the good options. Higher fills first |

Here is the first slot of the first page in the default setup. It holds the weapon you are *not* holding, gets first pick, and hands itself over to a healing potion when your health drops dangerously low:

```
[Page0.Slot0]
sClassification = WeaponsAny
bWildcardsEnabled = true
bOverridesEnabled = HP
iPriority = 7
```

#### The default pages

By default Huggin comes with 2 default pages: Huggin and Kit

##### Huggins Page

The first page, **Huginn**, gives every key a job. Each key shows the best thing for that job that is not already in your hands:

| Key | Job | What you'll see |
|---|---|---|
| 1 | Weapon | The weapon you're not holding — the bow while the sword is out. Swap, and the sword waits on this key, so one key toggles between two weapons |
| 2 | Attack magic | Damage spells and damage scrolls for the target |
| 3 | Heal | Healing spells and health potions |
| 4 | Defend | Wards, armour spells, resist potions |
| 5 | Buff | Fortify, invisibility, muffle |
| 6 | Potion | Whatever potion suits the moment |
| 7 | Situational | Anything, plus the quieter prompts: arrows running low, a soul gem, waterbreathing |
| 8 | Wildcard | The best of everything else, and now and then something new |

Emergencies always land on the same keys: health on 1, magicka on 2, stamina on 3 (See Emergencies — [Overrides] below to change this).

##### Kit Page

The second page, **Kit**, is for between fights: potions, food, scrolls, utility spells, summons, arrows and crafting gear, one kind per key.

#### Template Pages

**Templates.** `configs/templates/` has six ready-made layouts: 

* battlemage  
* paladin
* pure mage
* stealth archer
* summoner
* survivalist

Each keeps the same emergency keys, so switching never moves "heal me". To use one, delete `[Pages]` and every `[PageN]` / `[PageN.SlotM]` section in `Huginn.ini`, paste the template in their place, and type `hg reload`.

One thing worth knowing about emergencies: a slot set to `Other` will *not* take health, magicka or stamina emergencies. That is deliberate — it keeps one slot free for the quieter prompts, so a soul gem or low-ammo warning still gets through when your health is also dropping.

#### What can fill a slot

`sClassification` is the one setting you'll change most, so it's worth reading this list. There are two ways to think about a slot: **by the job you want done**, or **by the kind of thing you want in it**.

**By job.** These mix everything you own together — a spell, a potion, and a scroll can all compete for the same slot, and Huginn picks whichever fits the moment best.

| Set this | And the slot holds |
|---|---|
| `DamageAny` | Anything that hurts something: attack spells, damage scrolls, poisons, and your weapons |
| `DamageMagic` | Attack magic only: damage spells and damage scrolls — no weapons, no poisons |
| `HealingAny` | Healing spells, healing scrolls, and health potions |
| `BuffsAny` | Things you use *before* the fight: armor and cloak spells, invisibility, muffle, and fortify potions |
| `DefensiveAny` | Wards, armor spells, and resist fire/frost/shock/magic/poison potions |
| `SummonsAny` | Conjuration — summoned creatures, raised undead, and bound weapons |
| `Utility` | Everything else useful: candlelight, detect life, telekinesis, unlock, waterbreathing, cure disease and cure poison |

**By kind.** These accept only one category of thing, which makes a page predictable — the same slot always holds the same sort of item.

| Set this | And the slot holds |
|---|---|
| `SpellsAny` | Any spell you know |
| `SpellsDestruction` | Destruction spells only |
| `SpellsRestoration` | Restoration spells only |
| `SpellsConjuration` | Conjuration spells only |
| `SpellsIllusion` | Illusion spells only |
| `SpellsAlteration` | Alteration spells only |
| `ScrollsAny` | Any scroll |
| `PotionsAny` | Any potion — restore, resist, fortify, cure, and poisons. Not food or drink |
| `PoisonsAny` | Poisons only |
| `FoodAny` | Food |
| `AlcoholAny` | Ale, mead, wine, skooma |
| `WeaponsAny` | Any weapon, including staves |
| `WeaponsMelee` | Swords, axes, maces, daggers |
| `WeaponsRanged` | Bows, crossbows, staves |
| `AmmoAny` | Arrows and bolts |
| `ApparelAny` | Gear that fortifies smithing, enchanting or alchemy — it only comes up at a workstation |
| `Regular` | No restriction — anything at all can land here |

A few things to note:

* **`Regular` is the catch-all.** Use it for slots you want Huginn to fill freely, and as overflow after your specific slots. If you typo a classification, Huginn falls back to `Regular` and notes it in the log.
* **Weapons count as damage.** A `DamageAny` slot can serve you a sword. If you want attack magic only there, use `DamageMagic`.
* **Held items are skipped.** Every slot leaves out what you are already holding (`bSkipEquipped`, on by default), which is what makes a weapon key show your *other* weapon.
* **Magic Wands and Staves are ranged.** They match `WeaponsRanged`, not the spell classifications.
* **The names have short forms.** `damage`, `healing`, `buffs`, `melee`, `destruction`, `drinks`, `any` and so on all work, and case doesn't matter.

#### Keybindings

Huginn uses DirectInput scancodes for key activation. Only single keypresses are supported at this time.

```
iSlot1Key = 2           ; the "1" key
iPreviousPageKey = 12
iNextPageKey = 13
iToggleWidgetKey = 45   ; 0 to unbind
```

Tap equips to the right hand, double-tap to the left hand, hold to both hands.

#### Emergencies — `[Overrides]`

Most of the time Huginn quietly ranks your options. Sometimes something is urgent enough that it should stop suggesting and just put the answer on your key. That is an override: it seizes a slot, ignoring the normal ranking, until the emergency passes.

A slot only accepts an override if you let it. That is the `bOverridesEnabled` setting on the slot — `HP`, `MP`, `SP`, `Other`, `Any`, or `None`. In the shipped layout, health takes `Page0.Slot0` (key 1), magicka takes `Slot1` (key 2), stamina takes `Slot2` (key 3), and everything else takes `Slot6` (key 7).

| Emergency | Fires when | What lands on the key |
|---|---|---|
| Health | Health drops below 35% | Best health potion; failing that, a healing spell you can cast right now |
| Magicka | Magicka drops below 35% | Best magicka potion; failing that, a restore spell you can afford |
| Stamina | Stamina drops below 35% | Best stamina potion; failing that, a restore spell you can afford |
| Weapon charge | An enchanted weapon or staff falls below 25% charge | A filled soul gem |
| Low ammo | You are down to 10 arrows or bolts | The best ammo you are carrying |
| Drowning | You are underwater with no waterbreathing | Waterbreathing potion |

Each one has its own on/off switch — `bEnableCriticalHealth`, `bEnableWeaponCharge`, `bEnableDrowning` and so on. Turn off what you don't want. `bHealthSpellFallback` and its magicka and stamina twins turn off the spell fallback.

**Same key, or where it already is.** By default health, magicka and stamina emergencies always use their own key, so "heal me" is muscle memory. If you'd rather nothing moved, set `bPinHealthToSlot`, `bPinMagickaToSlot` or `bPinStaminaToSlot` to `false`: then, when the potion is already on another key, that key pulses instead. Arrows, soul gems and waterbreathing always work that second way.

**Thresholds and the release gap.** Every emergency has a threshold and a hysteresis. The threshold is where it starts; the hysteresis is how far back you have to climb before it lets go. Health is `0.35` and `0.15`, so it fires below 35% and releases above 50%. That gap is deliberate — without it a potion would flash on and off your key every time your health wobbled around a single number. Widen the gap if it lets go too eagerly; narrow it if it lingers.

Vital thresholds are fractions (`0.35` = 35%). Ammo is a plain count (`10` arrows, releasing at 25).

`fMinOverrideDurationMs` is a floor on the whole thing: once an emergency takes a slot, it holds it for at least 2 seconds no matter what, so nothing can flicker.

**Impure potions.** `bAllowImpurePotions = true` lets Huginn fall back to something with a nasty side effect — skooma, say — when there is nothing clean left. It is a last resort, never a first pick.

**Soul gems have two switches, and they are separate on purpose.** `bEnableWeaponCharge` here controls the "your weapon is nearly dead" prompt. `bEnableSoulGemRecharge` under `[Candidates]` controls whether gems show up in ordinary recommendations at all. Turning off the second one does *not* stop the emergency prompt. Turn off both to stop seeing soul gems entirely.

> Not to be confused with `Huginn_Overrides.ini`, which is a different thing entirely — that file fixes how modded items get *categorised*. See [Classification Overrides](#classification-overrides) below.

#### Algorithm Configuration

These settings are the ones that decide how Huginn makes up its mind. Tuning them is guesswork even with a good sense of how it works, so change one thing at a time and see what happens.

Fiddle at your own risk. If you want to know what each number actually feeds into, [the full write-up is here](https://github.com/LandingCrew/huginn/blob/main/docs/README.md).

##### What Huginn pays attention to — `[ContextWeights]`

This is the section that decides *what matters right now*. Each setting is how loudly one condition shouts. Raise a number and Huginn reacts more strongly to that situation; set it to `0` and it stops caring about it altogether.

The loud ones are the emergencies you can see happening to you:

```
fWeightOnFire = 8.0
fWeightFrozen = 8.0
fWeightShocked = 8.0
fWeightPoisoned = 6.0
fWeightUnderwater = 10.0
fWeightFallingHigh = 8.0
fWeightLookingAtLock = 10.0
```

The rest sit on a 0-to-1 scale and cover the ordinary run of play: being in combat, facing several enemies, an enemy mid-cast, sneaking, standing at a forge or enchanter or alchemy lab, fighting undead or daedra or a dragon, being out of ammo, having no weapon drawn.

Two of them are worth knowing about because they exist to stop things disappearing:

* `fWeightBuffPotion = 0.15` is a constant low hum that keeps fortify and resist potions visible at all. Without it they would never score highly enough to be offered, and so Huginn would never get the chance to learn that you like them.
* `fWeightSoulGem = 0.15` does the same for filled soul gems, so you can see one even when you are not carrying a drained enchanted weapon. Set it to `0` to only ever see gems when a weapon actually needs one.

The three `SmoothingExponent` settings shape how sharply the health, magicka and stamina curves ramp up as the bar drains. `1.0` is a straight line, `2.0` (the default) stays calm until things get genuinely bad, `3.0` more so.

##### Ranking — `[Scoring]` and `[Favorites]`

`[Scoring]` is the machinery that turns "what matters now" plus "what do you like" into a final order. Most of it you can leave alone. The parts worth touching:

* **`fLambdaMin` / `fLambdaMax`** — how much weight Huginn gives your learned habits versus the situation in front of you. It starts near `fLambdaMin` while it barely knows you, and rises to `fLambdaMax` once it does. Raise them to have it lean harder on your habits; lower them to keep it more reactive.
* **The bonus settings** — small nudges for combinations that make sense together: matching arrows to your bow, a shield spell with a melee weapon, silver against undead, a staff when your magicka is low. Adjust or zero out individually.
* **`iTopNCandidates = 10`** — how many options get considered in detail each update.
* **`fMinimumUtility`** — the bar an option has to clear to be shown at all. Raise it if you are seeing filler you don't want.

`[Favorites]` decides what your favourites flag means. `Boost` (the default) pushes favourited items up the order. `Suppress` does the opposite and hides them, on the grounds that they are already one keypress away in your favourites menu. `Off` ignores the flag.

##### Learning and trying new things — `[Wildcards]` and `[Learning]`

Huginn can only learn that you like something if it offers it to you occasionally. That is what wildcards are: now and then a lower-ranked option takes a slot, so it gets a chance to be picked.

Wildcards never touch your top slot — that always holds the genuine best pick — and they get rarer the more you care about a slot. `fBaseProbability` and `fMaxProbability` control how often they appear; the two cooldown settings stop the same slot churning. You can also turn them off per slot with `bWildcardsEnabled = false`.

`[Learning]` covers the other half: Huginn also watches what you equip *outside* of it, through the vanilla inventory, the favourites menu, or a vanilla hotkey. Reaching past Huginn for something is a strong signal, and the reward settings say how strong. `bLearnFromExternalEquips = false` turns this off if you would rather it only learn from your use of its own keys.

##### Steadiness — `[SlotLocker]`

A recommendation that changes the instant before you press the key is worse than a merely decent one that stays put. When a slot fills, Huginn briefly locks it so it cannot be swapped out from under your thumb.

`fLockDurationMs = 3000` is that hold. Raise it for a calmer, slower display; lower it for one that reacts faster. `0` disables locking entirely. Emergencies are allowed to break a lock, which is what `bOverridesBreakLock` and `iImmediateBreakPriority` govern. `hg unlock` clears every lock immediately.

**Swap back.** Press a key that puts a weapon, spell or scroll in your hand, and whatever was in that hand appears under the *same* key, labelled "Swap Back", for `fRemembranceDurationMs` (15000 = 15 seconds). Press it to put the old item back; the key then returns to normal. Arrows work the same way, arrows for arrows: switch from Iron to Steel Arrows with a key and the Iron Arrows wait on it. Only a swap counts; equipping into an empty hand or quiver leaves nothing to swap back to. It works from Huginn's hotkeys and Huginn's own Wheeler wheels, not from the inventory menu or favourites. It is not a recommendation, so swapping back teaches Huginn nothing.

To turn it off for one slot, add `bRemembrance = 0` to that slot's section:

```
[Page0.Slot3]
bRemembrance = 0
```

To turn it off everywhere, set `fRemembranceDurationMs = 0` under `[SlotLocker]`. Either way, `hg reload` applies it without restarting.

A remembered item pulses for the last few seconds before it goes. When it doesn't fit the key you pressed — a spell took your dagger off, and the dagger now sits on the attack-magic key — it only stays for `fRemembranceMismatchDurationMs` (5 seconds). Set `sRemembranceTarget = Job` to send it to a key where it does fit instead (the dagger goes to your weapon key).

`bFillJobKeysFromRegular` covers the odd moment a job key would be empty while an "anything" key holds something that fits it: turn it on and the item moves to the job key. It is off by default, because it moves things between keys.

##### Spells you can't currently cast — `[Candidates]`

`sUncastableSpellPolicy` decides what happens to spells you cannot afford right now:

* `Disallow` (the default) — hide them; a key you cannot use is wasted space
* `Penalize` — keep them, ranked lower the further out of reach they are
* `Allow` — rank them normally, magicka be damned

`bEnableSoulGemRecharge` belongs to this section too — see the soul gem note under Emergencies above.

### Classification Overrides

**Configuration Location:** `Data/SKSE/Plugins/Huginn_Overrides.ini`

Huginn classifies vanilla items and spells automatically. Modded potions, foods, and spells sometimes need a hint. This file supplies it:

```
[Item:Battlemage's Elixir]
type = BuffPotion
tags = FortifyMagicSchool,FortifyHealth

[Spell:Scroll of Bolide]
type = Damage
tags = Fire,Ranged
```

The `Item:` / `Spell:` prefix matters. This one file covers both items and spells, and some type and tag names mean something in each, so a section without a prefix is tried against *both* — and can quietly reclassify a potion and a spell that happen to share a name. Prefix every section. The file itself lists every type and tag you can use, for items and for spells.

Changes take effect on `hg rebuild` — no game restart needed, and `hg reload` is not enough (it reloads settings, not classifications).

### dMenu UI controls

**Configuration Location:** `Data/SKSE/Plugins/dmenu/customSettings/ini/Huginn.ini`

This controls the in-game Intuition widget's appearance and, on a debug build, its debugging output. dMenu owns these settings exclusively — position, opacity, scale, display mode, and the widget on/off switch — and you edit them from Huginn's panel in the dMenu UI rather than by hand. Without dMenu installed, the widget uses its built-in defaults.

The panel also has four buttons: **Show/Hide Widget**, **Reset Learned Weights**
(makes Huginn forget everything it has learned about your preferences), **Reset to
Defaults**, and **Reload INI**.

Two controls turn the widget off:

* **Enable Widget** — the checkbox. A saved setting; off means the widget is never
  built. Leave it on unless you want it gone for good.
* **Show/Hide Widget** — the button. An ephemeral runtime toggle: same as the hotkey,
  not saved, back to shown on the next load.

### Console commands

Open the console with `~` and type `hg` (or `Huginn`):

| Command | Description |
|---|---|
| `hg help` | Show available commands |
| `hg status` | Show system status |
| `hg refresh` | Force an immediate recommendation update |
| `hg reload` | Hot-reload all settings from INI |
| `hg rebuild` | Re-read your spells and items from scratch, including `Huginn_Overrides.ini` |
| `hg page [N]` | Switch to page N, or show the current page |
| `hg unlock` | Unstick every slot, so all of them are free to change again |
| `hg recs [N]` | Write the top N picks, and why they scored as they did, to the log |
| `hg weights <FormID>` | Show what Huginn has learned about one specific spell or item |
| `hg reset weights` | Forget everything Huginn has learned about your preferences |
| `hg reset all` | Full system reset |

Log output goes to `Documents/My Games/Skyrim Special Edition/SKSE/Huginn.log`.

## Optional Extension

### WheelerAPI

Huginn can connect to the Wheeler plugin and drive its wheels directly, so each page becomes a wheel and each slot a wheel entry.

This needs [wheelerAPI](https://github.com/LandingCrew/wheelerAPI), available here as an optional download. It adds the connection point Huginn talks to, which the original Wheeler does not have on its own — without it Huginn simply stays disconnected and everything else keeps working as normal.

It also carries a few stability patches, and adds soul gem support.

#### Install

1. Download and install the original Wheeler mod — its supporting files are still needed.
2. Install wheelerAPI after Wheeler and let it overwrite.

> You do **not** need [Wheeler CTD-Fix](https://www.nexusmods.com/skyrimspecialedition/mods/132074). wheelerAPI already includes those fixes.

#### Compatibility

wheelerAPI works alongside:

* [Wheeler - Show in UI](https://www.nexusmods.com/skyrimspecialedition/mods/161698)
* [Dragonborn Reskin - Wheeler](https://www.nexusmods.com/skyrimspecialedition/mods/100043)
* [Wheeler Icon Embellishment - Naturally Exquisite Refinement](https://www.nexusmods.com/skyrimspecialedition/mods/100692)

If you would rather not use Huginn's Wheeler variant, [WHEELER - Refined](https://www.nexusmods.com/skyrimspecialedition/mods/167380) works too. You keep the labels under each entry, but give up two conveniences: Huginn loses track of its wheels once you rearrange them, so it will not remember where you dragged them — `sWheelPosition` stays in charge — and if another mod shuffles them mid-game, Huginn cannot put them back on its own.

#### Usage

Huginn attempts to connect to Wheeler automatically and manages its own wheels based on the `[Wheeler]` section of `Huginn.ini`. Two behaviours are worth knowing before you rearrange anything:

* **Opening Wheeler jumps you to Huginn's wheels.** Open Wheeler on any wheel that isn't Huginn's and it takes you straight to Huginn's first one. That stings most if you have moved one of your own wheels to the very front — Wheeler opens on it, Huginn moves you along, and the only way back is to scroll. You get an in-game notice the first time it happens. Set `bAutoFocusOnOpen = false` to stay on whichever wheel you opened.

* **Where you drag Huginn's wheels sticks until you quit.** Rearrange them in Wheeler's edit mode and they will still be there after you load a save. Two catches: restarting Skyrim puts them back where `sWheelPosition` says, and they always come back as one group, so if you had tucked your own wheels in among them they will not stay in between. To set the order deliberately, change `sWheelPosition` and run `hg reload`. None of this applies with WHEELER - Refined, which cannot tell Huginn where its wheels ended up — there `sWheelPosition` always wins.

The rest of the `[Wheeler]` settings:

| Setting | What it does |
|---|---|
| `sWheelPosition` | Where Huginn's wheels sit in the rotation — `First`, `Last`, or a number |
| `bAutoFocusOnOverride` | Jump to Huginn's wheel when an emergency fires. On by default |
| `iAutoFocusMinPriority` | How serious an emergency has to be before that jump happens. Drowning counts as 50, low magicka 70, low health 100 — so the default of 50 jumps for anything, and raising it to 100 jumps only when you are about to die |
| `sPostActivationPolicy` | What the slot does after you use what was in it. `Backfill` puts the next-best thing there, `Sticky` leaves the used item showing until the situation changes, `Empty` clears the slot and shows "Equipped" |

#### Wheel entry labels — `[Subtexts]`

Wheeler entries can carry a small line of text under the item name, explaining why it is there. `[Subtexts]` controls those: whether to mark wildcard picks and emergencies, whether to show the lock countdown, and whether to show Huginn's one-line reason for the pick. `fOffsetX` and `fOffsetY` nudge the label's position in pixels if it collides with your Wheeler skin.

Both wheelerAPI and WHEELER - Refined support these labels. If they never appear, check `Huginn.log` for the line beginning `Connected to Wheeler API` — it tells you what Huginn found.


## Mod Compability

### Spells Packs

### Lighting Mods

Huginn estiamtes how  dark it is from the light reported by game itself (measures on your character, the same value guards use to spot you sneaking). When it is dark, Night Eye, light spells such as Candlelight and Magelight, and torches come forward, and they drop back once one is active or a torch is out. A passing shadow does not count: it has to stay dark for about five seconds, and light for about three before the suggestions go away. Going through a door is the exception, so a dark cave counts the moment it loads.

Lighting mods change that value, so how often this happens depends on your load order. With a dark lighting setup, caves and moonless nights count as dark. With a bright one, or the vanilla game, the light may never fall low enough and Night Eye will not be suggested for darkness.

If your setup is bright and Night Eye never comes up, raise `fDarkLightLevel` under `[ContextWeights]` (default `0.35`; the debug panel shows the light level where you stand). To turn or disable the darkness suggestions off entirely, set `fWeightDarkness = 0` under `[ContextWeights]`.

### Survival Mode

With Survival Mode on (the Creation Club version, with or without Survival Mode Improved), Huginn reads your hunger and cold stages. From Hungry, food comes forward; from Very Cold, warm food and warming spells do, labelled "Hungry" or "Cold". Both get stronger a stage later (Famished, Freezing). Food is recognised by the survival keywords on its effects, which covers vanilla and mod-added dishes alike.

To turn either off, set `fWeightHungry = 0` or `fWeightCold = 0` under `[ContextWeights]`.

## Source Code

* [Huginn](https://github.com/LandingCrew/huginn)
* [WheelerAPI](https://github.com/LandingCrew/wheelerAPI)
