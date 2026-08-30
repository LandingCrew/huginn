# Soul Gems — Behaviour and Known Limitations

**Verified against `src/` at v0.19.10 (2026-08-29).** The original of this
document described v0.13.x/v0.14.x behaviour; two of its three headline claims
were obsolete. See [../README.md](../README.md) for the provenance of this tree.

Huginn recommends filled soul gems when an enchanted weapon's charge is low, and
activating that recommendation recharges the weapon.

---

## The rule that matters most: `NAM0` does not mark a reusable gem

> **`TESSoulGem::linkedSoulGem` (the `NAM0` field) is NOT a reusability flag.**
> Do not reintroduce a `linkedSoulGem == soulGem` test. The Black Star failed
> that test in-game on 2026-08-12 and was **consumed** — a unique quest item
> destroyed by a single keypress.

What `NAM0` actually encodes is the *conversion target*: trapping a soul into an
ordinary gem converts the form, `SoulGemPetty` → `SoulGemPettyFilled`. That is
the relationship the field describes, and it says nothing about whether the gem
survives being spent.

Huginn instead decides reusability by **where the soul lives**, which is
observable rather than inferred (`src/input/EquipManager.cpp`, `UseSoulGem`):

| Where the soul is | What it means | What happens on use |
|---|---|---|
| On the **base form** (`TESSoulGem::GetContainedSoul()`) | A vendor/loot gem shipped as its own filled FormID — the game converted it | `RemoveItem` — the gem is spent |
| On the **instance** (`RE::ExtraSoul` in the entry's `ExtraDataList`) | The game declined to convert it, which is what a reusable gem is | `extraSoul->soul = kNone` — the gem is emptied, not destroyed |

`UseSoulGem` reads the base form first; only when that returns `kNone` does it
walk `player->GetInventoryChanges()->entryList` looking for the best `ExtraSoul`
in the stack. `isReusable` is simply "the soul came from an instance"
(`sourceInstance != nullptr`).

This rule **fails safe**. An exotic gem misread as reusable survives with its
soul spent — a bookkeeping error. The old rule failed by destroying items.

`linkedSoulGem` is still read, but only to log the FormID at `debug` level, so
the evidence exists to revisit the field later. Nothing branches on it.

### Corollary: player-filled gems are not visible on the base form

Reading only the base form reports `kNone` for every gem the player filled with
Soul Trap, which used to make each of those a dead tile — the slot rendered, the
key did nothing, and no reward ever reached the learner. Both the fill scan
(`ItemRegistry::ScanPlayerInventoryAll`) and the use path read `ExtraSoul`.

---

## What works

- **Intuition widget** — soul gem slots render like any other item. The
  "Low Charge" subtext appears when `WeaponLowCharge` is the dominant context
  reason (`src/display/ExplanationLabel.h`); it is a context label, not a
  gem-specific one.
- **Wheeler** — gems push to wheels like any other item (see below).
- **Slot hotkey recharge** — a slot key routes through
  `EquipManager::EquipSlot` → `UseSoulGem`, restoring charge, spending or
  emptying the gem, and awarding Enchanting XP.
- **INI toggles — there are TWO, and one is not enough.**
  `bEnableSoulGemRecharge` in `[Candidates]` gates soul gems in **normal
  ranking** only (`CandidateGenerator.cpp:323`). The urgent weapon-charge
  override has its own switch, `bEnableWeaponCharge` in `[Overrides]`, which
  gates the whole evaluator at `OverrideManager.cpp:91`. Turning off only the
  first leaves the "WEAPON EMPTY: Need Soul Gem!" prompt still forcing a gem
  into a slot.
  **To stop soul gems being suggested at all, turn off both.** That split is
  deliberate — wanting gems out of routine recommendations while keeping the
  emergency prompt is reasonable, and so is the reverse — but it is not
  guessable from either setting's name.

### Wheeler now renders soul gems — the old workaround is gone

The previous version of this page said Wheeler's `AddItemByFormID` rejected
`TESSoulGem` with result code `-6`, and that Huginn zeroed gem FormIDs on the
way out to avoid error spam. **Both statements are obsolete.** Wheeler renders
gems now — observed 2026-08-11 on a user wheel carrying Azura's Star and The
Black Star — and the suppression was removed. `src/display/WheelerBackend.cpp`
keeps a comment marking where the zeroing used to be; the only remaining
FormID-blanking there is the unrelated `Empty` post-activation policy.

<!-- UNVERIFIED: what Wheeler does when a gem entry is activated. Huginn does
not drive that path — WheelerClient::OnItemActivated only observes the event for
reward and post-activation policy, so the recharge is performed by Wheeler's own
item-use code, not by EquipManager::UseSoulGem. Whether the outcome matches the
hotkey path (including reusable-gem handling) has not been checked. -->

---

## The recharge path

`UseSoulGem` bypasses the game's recharge UI entirely:

```
Slot key → EquipManager::EquipSlot → UseSoulGem(formID)
  → resolve soul + owning instance (base form, else best ExtraSoul in stack)
  → pick hand: right (kRightItemCharge) then left (kLeftItemCharge),
    enchanted weapon or staff, first hand with headroom wins
  → player->AsActorValueOwner()->ModActorValue(chargeAV, restoreAmount)
  → reusable ? extraSoul->soul = kNone : player->RemoveItem(soulGem, 1, kRemove)
  → player->AddSkillExperience(kEnchanting, expValue)
```

`restoreAmount` is `min(chargeValue, maxCharge - currentCharge)`, so a hand
already at full charge falls through to the other hand rather than wasting the
gem. If neither hand can take charge, nothing is consumed and the call returns
`false`.

This is the same approach as
[AutoUseSoulgemsSSE](https://github.com/neogulcity/AutoUseSoulgemsSSE) —
modifying the charge ActorValue directly.

## Charge values by soul level

| Soul Level | Charge Restored | Enchanting XP |
|---|---|---|
| Petty | 250 | 1.0 |
| Lesser | 500 | 1.5 |
| Common | 1,000 | 2.0 |
| Greater | 1,500 | 3.0 |
| Grand | 3,000 | 5.0 |

An unrecognised `SOUL_LEVEL` falls back to the Petty row.

**Limitation:** these are vanilla engine constants hardcoded in `UseSoulGem`.
They are not GMSTs, so a mod that alters soul gem charge or XP via SKSE or
script will diverge from what Huginn applies.

## Candidate generation

**Every filled gem type is a candidate, not just the biggest one.**
`CandidateGenerator::GatherSoulGemCandidates` used to call `GetBestSoulGem()`
and emit exactly one candidate picked by capacity. That put a hard-coded
preference in front of the one component whose job is preferences: a Petty gem
could never be scored, activated, or rewarded, so `FeatureQLearner` could not
learn that a player tops up with small gems and saves the Grands. Gems are now
gathered wholesale and ranked like every other candidate type.

Cost stays small because gems stack — this is one candidate per filled *type*
(Petty/Lesser/Common/…), not per gem. Empty gems are excluded: they cannot
recharge anything.

The urgent path is unchanged and still decisive.
`OverrideManager::FindSoulGem()` makes its own single pick via
`GetBestSoulGem()`, because when a weapon dies mid-fight the answer is "the
biggest one, now", and overcharging is the right trade there.

This path is gated by `bEnableWeaponCharge` (`[Overrides]`), **not** by
`bEnableSoulGemRecharge` — see the toggle note above. It is a separate switch,
not an oversight.

Two ranking details follow from the soul-location rule:

- **Magnitude is the soul held, not the gem's capacity.** `ClassifySoulGem`
  reads `GetContainedSoul()` off the base form; `ItemRegistry::AddSoulGem` then
  overrides it with `bestSoulLevel` from the `ExtraSoul` scan, which is the only
  place a player-filled soul is visible. Capacity moved to `tagsExt`
  (`SoulGemPetty`…`SoulGemGrand`, plus `SoulGemBlack` as a separate flag).
  `SOUL_LEVEL` runs `kNone=0 … kGrand=5`; there is no 6.
- **Count is `filledCount`, not stack size.** A stack of ten petty gems holding
  one soul looked abundant and dodged the scarcity penalty in
  `PriorCalculator`, while a lone filled gem took it.

## Other known limitations

- **Post-load blind window.** The `ExtraSoul` scan is gated on
  `Util::IsExtraListStable()` — reading `entry->extraLists` within
  `Config::EXTRALIST_STABILIZATION_MS` (500ms) of a load can crash. Inside that
  window player-filled gems read as empty and are picked up on the next
  `RefreshCounts` pass.
- **Mod gems that are `AlchemyItem` forms, not `TESSoulGem`.** These take the
  keyword fallback in `ItemClassifier::ClassifyItem`, where there is no soul
  data to read at all — capacity from the keyword stands in for magnitude, and
  `filledCount` is 0, so `GatherSoulGemCandidates` falls back to the stack
  count. `UseSoulGem` rejects them outright: they fail the
  `form->As<RE::TESSoulGem>()` cast.
- **Workstation context.** Filled soul gems are one of the two live targets for
  the workstation context — see #63 and #65 in [../roadmap.md](../roadmap.md).
  Requiem-based lists strip Fortify Enchanting from alchemy, so gems (and
  fortify apparel, which is not yet a candidate source at all) are what is left
  to rank at an enchanter.

## Technical references

| Path | What it holds |
|---|---|
| `src/input/EquipManager.cpp` | `UseSoulGem()` — soul/instance resolution, spend-or-empty, charge and XP |
| `src/learning/item/ItemRegistry.cpp` | `ScanPlayerInventoryAll()` fill scan, `AddSoulGem()`, `GetBestSoulGem()` |
| `src/learning/item/ItemClassifier.cpp` | `ClassifySoulGem()` and the `AlchemyItem` keyword fallback |
| `src/candidate/CandidateGenerator.cpp` | `GatherSoulGemCandidates()` — every filled type |
| `src/override/OverrideManager.cpp` | `FindSoulGem()` — urgent single pick |
| `src/display/WheelerBackend.cpp` | Wheeler push; where the gem-zeroing workaround used to be |
| `src/state/StateManager_Equipment.cpp` | Weapon charge via `kRightItemCharge` / `kLeftItemCharge` |
| `src/util/ExtraListStability.h` | `IsExtraListStable()` — the post-load gate |
| `src/candidate/CandidateConfig.h`, `src/Globals.cpp` | `enableSoulGemRecharge` / `bEnableSoulGemRecharge` |
