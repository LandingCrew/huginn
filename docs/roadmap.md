# Huginn Roadmap

Open work only. There is no completed-items archive any more — `roadmap-archive.md`
was deleted on 2026-09-07 — so a rejected approach is no longer recorded anywhere
once its entry leaves this file. Git history is the only record; check it before
re-opening something that looks obviously undone.

## Known Bugs
- [ ] WeaponData::damage is not the number the game shows, and never was.
      The TEMPER half is verified: `hg status` on 2026-09-19 read ExtraHealth
      1.10 for all three "- Okay" weapons, giving 9.0->9.9, 7.0->7.7,
      4.0->4.4, and the relative order within a base form is now right.
      The ABSOLUTE number is not: the player's inventory showed 11, 9 and 6
      for those same three. The gap is the skill/perk term -- the UNTEMPERED
      Iron Sword already read 7.0 here against the game's 8 -- so it predates
      instance tracking and is unchanged by it.
      It matters in exactly one place: the widget's "N dmg" detail text,
      which claims to be what the player would see. Ranking does not care,
      since every comparison it makes is between two numbers with the same
      term missing. Fixing it means asking the actor rather than the form.
      Raised 2026-09-19.

- [ ] AllyStatus still flaps, 57% less than it did, and nothing reads it.
      `RANGE_RELEASE_MARGIN` (acquire at 512, release at 640) killed the
      pathological case -- four `Ally:None<->Present` transitions inside 1.1 s,
      caused by acquisition and the prune testing the same threshold. Matched
      quiet-town logs: 12 transitions in 266 s before, 11 in 565 s after, so
      0.045/s -> 0.020/s.
      What survives is two 0.31 s pairs (2026-09-19, 20:52:53.014->.324 and
      20:55:09.174->.487). Distance cannot explain them: crossing the 128-unit
      margin that fast needs ~413 units/s, about a sprint. So it is either a
      running NPC or something that is not distance at all -- `Get3D()` going
      null, the actor leaving `highActorHandles`, hostility flickering. A
      distance band structurally cannot cover those.
      Each one costs a full pipeline pass (~1.8 ms, 68% of it the Wheeler push)
      to produce an identical result, for a field with NO CONSUMER: `allyStatus`
      is written by `StateEvaluator.cpp:54` and read only by
      `GameState::GetHash`, `ToString` and the diff. No ContextRuleEngine rule,
      no learner feature, no candidate filter.
      Two ways to finish it, if it is ever worth finishing. Drop `allyStatus`
      from the hash until a rule needs it -- all 11 go, including the two the
      band cannot catch. Or add a time-based hold like
      `CrosshairHysteresis::PERSISTENCE_TIMEOUT_SEC`, which covers every cause
      and keeps the signal honest for a future ally-aware rule, at the price of
      more code for something nothing reads.
      Deliberately left: 57% was judged enough, because the crosshair target now
      dwarfs it (below).
      Raised 2026-09-19.

- [ ] The crosshair target is the dominant state flap, and it may not be a bug.
      Same two logs: `Dist:Ranged<->Melee, Target:None<->Humanoid` went from 6 of
      19 transitions (0.023/s) to 34 of 46 (0.060/s) -- 74% of all state
      transitions, and the rise is camera movement, not a regression.
      The primary target is whatever the crosshair is on
      (`StateManager_Targets.cpp`, Priority 1, NO hostility filter; the
      closest-hostile fallback is gated behind `if (inCombat)`). Sticky window is
      `PERSISTENCE_TIMEOUT_SEC = 0.3f`, sized for raycast jitter rather than for
      looking away. So standing in a town and sweeping the view across
      townspeople re-scores the whole pipeline on every pass of the crosshair.
      The visible effect is mild -- two adjacent widget rows trading places, same
      six items, roughly 7 times in 4 minutes -- and the user reported not
      noticing it in play. And it is arguably CORRECT: the crosshair is the
      player pointing at something.
      Recorded because it is where the remaining churn lives, and because the
      three candidate framings should be decided rather than drifted into:
      (1) correct as-is, the cost is cosmetic; (2) out of combat, a non-hostile
      should need dwell time before it becomes the primary target; (3) slot
      assignment should be sticky even when the ranking is not -- an item already
      in a slot and still in the top N keeps its slot. Framing (3) SHIPPED in
      #124 (bKeepSlotPositions, on by default), on its own account rather than
      as a crosshair fix -- so what is left here is (1) against (2), and the
      cosmetic cost that argued for (1) is now smaller than it was.
      Raised 2026-09-19.

- [ ] Log noise: three sites break the rules CLAUDE.md sets for them.
      Measured on a 13-minute LoreRim-5 session (2026-09-20, v0.20.35, 2556
      lines, ~3.3 lines/sec overall — inside budget, but a third of it is these
      three). Counts from
      `grep -oE '\[[A-Za-z_]+\.(cpp|h)[ ]*:[0-9]+' log | sort | uniq -c | sort -rn`.
      `ApparelRegistry.cpp:65` — 90 lines, and every one a repeat. The same 8
      rejections ('Blue Mage Robes', 'Amulet of Zenithar', ...) re-print on each
      30 s reconcile, which is "log ticks, not transitions" exactly. Wants a
      last-value dedup so it fires when the REJECTED SET changes; a count at
      info on change would be better than per-item at debug.
      `ItemClassifier.cpp:488` and `:321` — 249 lines in one burst at registry
      build, two per item ([PopulateItemTags] and [DetermineFortifySkillType]).
      These are per-item registration lines and the rule for those is `trace`,
      with a summary count at info.
      `WeaponClassifier.cpp:121` — 'Unknown weapon type 0 for weapon: Unarmed'
      three times. WeaponRegistry marks the form rejected and says "won't
      retry", but the guard is in AddWeapon and the line is logged from the
      classify path upstream of it, so the two disagree about whether a retry
      happened. Small, and the 3-per-session rate makes it cosmetic, but it is
      a guard that does not cover what it claims to.
      None of this is new; it surfaced because a LoreRim inventory is big enough
      for the per-item paths to show up in a log-source histogram, where a
      simonrim-essentials save is not.
      Raised 2026-09-20.

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

## Known Recommendation Issues
- [ ] A third of the spells a LoreRim player can LEARN classify as Unknown.
      Measured 2026-09-21 with `hg dump spells` (debug-only console command,
      itself throwaway) on LoreRim v5: 5,025 castable spells in the load order,
      1,106 of them taught by a tome — the honest test of "a player can have
      this" — and 375 of those 1,106 come back `type=Unknown`. 34%.
      The raw 45%-unclassified figure over all 5,025 is mostly noise: draugr,
      dragon and quest spells are kSpell too and no widget will ever offer them.
      The tome filter is what makes the number mean something, and it is the
      column to sort on when re-measuring.
      By school, and this is the actionable part:
        Alteration 149 | Restoration 103 | Illusion 55 | Conjuration 37 |
        Destruction 22
      Destruction is nearly solved; the gap is everything that is not damage.
      That is the shape of an API-first classifier whose rules were written
      against damage archetypes — DetermineSpellType reads the costliest
      effect's archetype, and utility-ish Alteration/Restoration effects fall
      through to the tag fallback, which has nothing to say about them.
      The names are ordinary player spells, not exotica: Ash Rune, Ash Shell,
      Ash Storm, Bend Time, Burden, Boulder Strike, Control Weather,
      Clairvoyance, Featherwalking, Equilibrium (Stamina). LoreRim is built on
      Mysticism, so this is a third of a spellcaster's book invisible to the
      ranking on the list Huginn is developed against.
      Worth knowing before fixing: an Unknown spell is not dropped, it is ranked
      without a type, so the cost is bad ordering rather than absence — which is
      why this went unnoticed until something counted it.
      Raised 2026-09-21.

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
- [ ] Scroll cold-start: all scrolls sit in the pool every tick but score
      `learn≈0` against trained items at `learn=7–8`, so one can never surface
      until used and can't be used until surfaced.
      **Not scroll-specific** — it is the general no-transfer-between-items
      problem, and it bites every newly acquired item. Scrolls are just where it
      is most visible, because a player rarely uses one unprompted. The two
      candidate fixes are under Follow-ups, "Share learning across similar
      items"; fixing either closes this

- [ ] Two duplication findings from the PR #114 review, neither blocking:
      `ItemClassifier::DetermineFortifySkillType` has no case for the
      "Modifier" actor-value series, so a POTION carrying `kAlchemyModifier`
      falls into its default and is never tagged — the same bug class that made
      apparel inert, still live for potions. `ApparelClassifier` copied that
      vocabulary rather than sharing it, and the two have already drifted in
      opposite directions; one shared AV -> craft-skill function fixes both.
      Separately, `ApparelRegistry` hand-copies the key-agnostic half of
      `Registry::FormRegistry` (`ForEachEntry`, `IsLoading`, `EntryCount`) — the
      CRTP base whose own comment says it exists to stop exactly that. Its
      composite (formID, uniqueID) key genuinely does not fit the FormID-keyed
      index, which is why it was copied, but the visitor half could be adopted.
      Both grow in value if the apparel expansion above lands, since it
      multiplies the effect types being classified (S each)

## Doc-migration findings (2026-08-29)
Surfaced by the one-agent-per-doc migration pass. Every one is a code or config
defect the docs exposed, not a documentation problem. Ordered by what a player
would notice.

- [ ] **`sUncastableSpellPolicy = Penalize` behaves identically to `Allow`.**
      Split out of the `[Candidates]` wiring fix (0.19.13), which got the setting
      to `CandidateGenerator` but could not make `Penalize` mean anything: both
      `RunVisitorFilters` and `PassesAffordabilityFilter` branch only on
      `Disallow`, and there is no penalty mechanism to reconnect. The docs
      described one — a shortfall ratio and a `penaltyFloor` — but it was never
      built, and `fUncastablePenaltyFloor` has now been removed from the shipped
      INI rather than left implying it works.
      So this is a scoring FEATURE, not a settings bug: decide whether a partial
      relevance penalty for an unaffordable spell is wanted at all, and if so
      what the curve is. Until then the option is honest but has only two
      distinct behaviours (M)
- [ ] **Three tag values have no writer.** `ItemTagExt::Ravage*` and
      `Damage*Regen` are read by `HasHarmfulSideEffects()` but `PopulateItemTags`
      never sets them; `WeaponTag::EnchantSilence` has no writer anywhere in
      `src/`. Either wire them or delete them — as they stand,
      `HasHarmfulSideEffects()` cannot fire on those grounds (S)
- [ ] **Dead work in Release builds:** `damageRate`, `healingRate`,
      `damageIncreasing` and `damageDecreasing` are computed unconditionally
      every tick (`StateManager_HealthTracking.cpp:283-330`, no `_DEBUG` guard)
      and their only consumer is an ImGui debug widget. Either guard it or wire
      it — it is also exactly the substrate a future trend feature would need.
      Adjacent dead code: `SlotAllocator::AllocateSlots` (both overloads), kept
      as a legacy/test entry point; `IntuitionMenu::SetUrgent` / `setUrgent` /
      `_urgentSlots`, vestigial with no caller;
      `maxCandidatesPerCycle` on `ScorerConfig`, still parsed by
      `ScorerSettings.cpp:47` and read by nothing (0.19.13 removed the INI key
      only, not the code); `weightWeaponChargeModerate/Low/Critical` on
      `ContextWeightSettings`, loaded but absent from `ContextWeightConfig` so
      they reach no consumer — the weapon-charge weight became a continuous
      `pow()` curve and these three tiers stayed behind (their INI keys were
      removed in 0.19.13); `FilterStats::filteredByRelevance`, never incremented
      but still summed;
      `StateEvaluator::EvaluateCurrentState()`, which takes a
      `const WorldState&` it never reads (S)
- [ ] **Code comments that actively mislead**, all found because a doc repeated
      them. Fixing these is what stops the docs rotting again.
      `StateManager_Targets.cpp:498` says "Scan ALL process levels (high,
      middleHigh, middleLow)" above a loop reading `highActorHandles` only — the
      middle lists were removed for cost, and the doc's headline ally-scanning
      claim came straight from this line. `ContextRuleEngine.cpp` says "Stage 1a
      (skeleton): Returns all zeros — no rules implemented yet" above a fully
      implemented method. `StateManager.h` says "3 locks" and "7 float
      accumulators"; it is 4 and 11. `StateFeatures.h:17` cites the stale
      36,288-state figure (it is 72,576 — and the file is `src/learning/`, not
      `src/state/`). `SettingsReloader.cpp:94` says the dMenu INI holds "Widget,
      Keybindings, Debug" — keybindings moved to the main INI in the 0.19.0
      split. `FeatureBanditLearner.h` says "~90% confidence at 15 trains"; the
      sigmoid gives 95.3%. Each verified still present 2026-09-07.
      Two are already fixed and dropped from this list: the "Semi-gradient
      TD(0)" claim (0.20.0, with the identifier rename) and
      `ContextWeightConfig.h`, whose field breakdown now correctly reads 35
      against 38 (XS each)
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
- [ ] **Suppress-mode favorites are fully scored every tick to be discarded.**
      Favorites bypass the `minimumContextWeight` early filter
      (`UtilityScorer.cpp:77`), then `GetFavoritesMultiplier` returns 0.0, then
      `minimumUtility` drops them. Low severity, but note that with
      `fMinimumUtility = 0` suppressed favorites would reappear at utility 0 (XS)

## Architecture Critique — Backlog
See `reviews/architecture-critique.md` — **the file is missing from the repo**; it was never committed and is not in the recovered docs snapshot.
**Landed:** Tier 1 (all); Tier 2 #8 registry consolidation (PR #55), #9 display
abstraction (PR #56), #10 safe pieces — GetContextWeight move + ComputeRelevanceTags
dedup (PR #57), #10 leftover — relevance-tag encoding unified on ContextRuleEngine
(PR #58, merged; verified in-game across 5 Debug sessions — all 26 reason labels
observed, threshold parity exact on both smoothing exponents). Critique #10 is
now closed; #59–#65 are follow-ups it surfaced, not remaining critique work.

### Tier 2 — COMPLETE
All Tier 2 critique items have landed (PRs #55–#58); the detail was in the deleted
archive and is now only in git history.

### Tier 3 — hot-path perf (trace-prioritized; see docs/profiling/tracy-traces.md)
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
- [ ] `ValidateWheelState` emits ~11 desync warns during a Wheeler edit-mode
      session — stale by construction, since Huginn has no signal that indices
      moved until edit mode exits, and the exit re-resolve corrects everything
      1.4 s later. Observed 2026-08-29 13:03:18 against a 13:03:20 re-resolve.
      Skip the check while `IsInEditMode()` — the diagnostic cannot say anything
      true there (XS)
- [ ] Unit tests for Context::WeightForCandidate (Tests.cpp:2656/3374 currently
      hand-reimplement the weight mapping — call the real one). DominantReason /
      ReasonLabel are covered by unit test 17.
- [ ] Delete the merged remote branches: `docs-pass`, `override-ini-namespacing`,
      `rename-bandit-learner`, and now `weapon-stale-recs`, `slot-stability`,
      `slot-seating` and `inventory-count` — all merged or superseded. Keep
      `widget-hide-while-wheel-open`, which is 1 ahead and still holds work.
      Re-verify with `git ls-remote` before deleting anything; the first three
      were last checked 2026-09-07, the rest merged 2026-09-21. Nothing depends
      on this; it is tidying (XS)
- [ ] Soak protocol needs deliberate MANUAL equips — accept% is fed only by
      equips made outside Huginn, so a burst played through the wheel/hotkeys
      produces no recommendation-quality data at all. Confirmed 2026-08-26: a
      44-min session reported accept=n/a in every window while 21
      external-equip events fired and were all filtered as wheel-open (each
      coinciding with a src=Wheeler reward in the same second). The filter is
      CORRECT — grading a wheel pick asks whether Huginn predicted the item the
      player chose off Huginn's own list. v0.19.1 adds `skipped=N (wheel=…)` to
      the heartbeat so n/a is self-explaining (branch `soak-skip-telemetry`
      @067397b), and docs/playtest/LongPlaySoak.md now lists manual equips as a
      coverage requirement and a void-run signal. Remaining: decide whether
      accept% is the right headline metric for a wheel-driven player at all,
      and whether to fold non-wheel consumption into it (5 of 50 events on that
      session, not 50 — wheel/hotkey rewards must stay out)
- [ ] Positive log line when the text-entry input gate engages — a
      transition-only `[InputHandler] Input suppressed — text entry active`.
      Today the gate is only verifiable by the ABSENCE of `KEY PRESS` lines,
      which is indistinguishable from any other reason input stopped, and
      unbound letters never log at all. Verified once by watching the widget
      instead; a log line makes it checkable and catches a regression (XS)
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
- [ ] Addendum #15/#16 (Kalman learner / learnable context weights) — **parked**: needs a v3
      cosave bump, NOT landable during an active soak run
