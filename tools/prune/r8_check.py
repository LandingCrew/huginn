"""R8's and R11's 'Done when', mechanically: the prune grep, the D2 allowlist, the retired classifiers.

R8 prunes the old ranking layer, but R6 chose D2, whose `ln learn mult` feature needs the old
learner's learning score on the rows the old floors pass. So two lists:

  PRUNED  symbols that are gone. A whole-word search over the code under src/, tools/ and
          tests/ (comments included; Markdown docs excluded, they follow in R11) must find
          nothing. A few common words are pruned only in the files that had them
          (PRUNED_SCOPED). The shipped INI and the dMenu JSON must not carry the pruned INI keys.
  LEGACY  the old engine R8 keeps for D2 only. A LEGACY line may appear only in the allowlisted
          files below, each with its reason, or in a BUDGET file: a live file allowed a fixed
          number of LEGACY lines, each matching one of the file's patterns (the exact form each
          kept use takes), so a new use anywhere in it fails, and so does a changed one.
          A LEGACY line is any line that names
            - the old engine's types, globals and config fields (LEGACY words; the config
              fields C++ only: they are common words in the Python fit tools);
            - the legacy module's facade: the namespace (`Legacy::`, `Scoring::Legacy`), its
              include (`#include "learning/legacy/...`), the pipeline's handle on it
              (`ctx.legacy`, `legacy.<member>`) and its distinctive names (FallbackRanking,
              KeptFor, RowFor, OldScoreCapture, ...). Names the module shares with the live
              ChoiceLearner (AdvancePlayTime, WeightsChanged, ...) are caught by the namespace;
            - a member read of the old utility or its parts: `.utility`, `.breakdown`, and the
              breakdown's fields (`.contextWeight`, `.learningScore`, ...), C++ only. Reading
              them is how utility would rank or display again, whatever the symbols are called.
              The v3 row's copy of it (`.util`, DecisionLog::Row::util) too: that is how the
              live learner sees the old utility;
            - the old floors' keep flag: `legacyScored` (ScoredCandidate), `.Scored()` (the
              legacy module's Row) and `Flag::Scored` (the v3 row's util-is-finite bit, under any
              qualifier: `F::Scored`), C++ only for the last two. Reading any is how the floors
              would filter or rank again;
            - a member read of the v3 row's OldScore columns or the decision's OldRun head
              (`r.old.floor`, `r.old.wouldUtil`, `d.ctx->old.valid`, ...), C++ only: the old
              engine's parts as the log carries them. On the live learner's path (ChoiceLearner.cpp,
              ChoiceSubscriber.h, core/ChoiceUpdate.h's ChoiceSet) any `.old` read is LEGACY, so
              an alias (`const auto& o = r.old;`) fails there too.
          A PRUNED or LEGACY word is also matched in its house spellings: `m_` / `g_` / `s_` before
          it (either case on its first letter: m_featureBanditLearner, s_UsageMemory) and a `Get`
          accessor named for it (GetDominantReason, GetUsageMemoryReader). A bare name with its
          first letter's case flipped (a local `dominantReason`) is not.
          The utility -> rank-score bridge (RankOnUtility, BridgeScore) is LEGACY too, so
          utility cannot rank outside the fallback in the legacy module and the slot harness
          that states scores as utilities. DisplayUtility, its inverse, was deleted in 0.24.5;
          it stays a LEGACY word so a revival fails. A few words are LEGACY in one file only
          (FILE_PATTERNS): a bare `utility` / `breakdown` in ScoredCandidate.h's code, and
          `Suppress` in PipelineCoordinator.cpp. A file that ranks or does slot math live
          (ScoredCandidate.h, SlotScoreMath.h, UtilityScorer, PipelineCoordinator) is never
          allowed whole: it is a patterned budget. Nor is a file on the live learner's path
          (ChoiceSubscriber.h and ChoiceLearner.cpp: no LEGACY line, the learner reading none of
          the v3 row's old-engine columns since 0.24.17, util missing and ln learn mult coming off
          um / lm; SelectionLogV3.cpp, which builds the v3 rows the learner trains on: patterned).
          The old utility's row feature (`ln_util`) is LEGACY too: the bootstrap refuses a weight
          on it, and nothing but that refusal, its enum line and its host test may name it.
          0.24.17, the D2 engine in shadow (engine A beside the live engine B): every name with
          `legacyShadow` / `LegacyShadow` (or `legacy_shadow`, any case) anywhere in it is LEGACY,
          C++ only. A's values are computed in
          pure core (core/LegacyShadowPass.h, allowlisted: no game access, const inputs) and
          travel outside it only under such names -- the run (ctx.legacyShadow), the feature
          vector A reads, the eligible rows' and the v3 rows' fields, the [Soak] tally's locals --
          so an alias or a copy is itself a LEGACY line. They appear in the pipeline's calls and
          copies and the log's copies (PipelineStateCache, SelectionLogV3, DecisionLog), each in
          its exact form; a read anywhere else -- a live score, an allocation, a lock, the display,
          the learner -- fails. ChoiceLearner may name none, and REQUIRED keeps the two guards that
          keep engine B's learner on B's features (CaptureEligible's ctx.legacyLive, the learner's
          um / lm read).
  STALE   phrases that describe a pruned behaviour as live (the display reason derived from the
          context weights), in code and comments, two lines at a time.
  REQUIRED code that must stay, verbatim up to whitespace: the bootstrap's ln util refusal.

R11 (0.24.4) adds, for the retired classifiers (map Phase 10):
  PRUNED   the per-type dumps and the live helpers that read the classifiers (Cmd_DumpSpells ...,
           ItemRegistry::GetBestPotion, the two resist-redundancy checks, SlotClassifier's
           Matches* arms, SpellClassifier::RetryDecidedType), each with an `R11:` reason.
  CLASSIFIER the old spell / item / scroll classifiers' judgements -- their classes, the tag and
           type enums (SpellTag, ItemTag, WeaponTag, SpellType, ItemType, ...), HasTag, the
           override loaders, a member read of a candidate's `.tags` / `.tagsExt` / `.element` --
           may appear only in the classifiers' own files, the candidate types that copy them, the
           D2 legacy feature's inputs (the context rules, correlations, potion multiplier, prior,
           the legacy module) and a few allowlisted readers, or in a budgeted file in its exact
           form. CODE ONLY: comments are stripped first (they name the classifiers to say what
           R11 replaced), unlike LEGACY. The live engine (slot classes, filters, overrides, label,
           display) reads the effect catalog's facets instead (core/CapFacets.h).
  REQUIRED the facets' readers stay wired: SlotClassifier asks core/CapFacets.h, the filters
           take the candidate's facets from effect/Facets.h, the override finders read them.
The D2 legacy feature (`ln learn mult` behind [Scoring] bLegacyLearnFeature) is kept exactly:
it needs the old floors, which need the old context weight, which reads the classifiers' tags.
So the classifiers stay, confined to it; they go when D2 goes (the LEGACY retirement below).

What the check is: a guard against ACCIDENTAL reintroduction, by name and by plain member read.
It does not try to defeat a deliberate evasion (a structured binding, a macro, a new accessor
written outside ScoredCandidate.h to hide a read).

Exits 1 and prints every hit on a violation; 0 otherwise. Standard library only.

usage:  python -I tools/prune/r8_check.py [--verbose]
        (--verbose also lists the allowlisted LEGACY hits per file)
        python -I tools/prune/test_r8_check.py   (the mutation probes: each must FAIL the check)

Retiring the LEGACY list (roadmap, R8): a refit on 0.23.53+ logs where a variant without the
old scorer passes R6's gate, or the user's call. Then the allowlist shrinks to nothing.
"""
import argparse
import fnmatch
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXCLUDE = ['tools/prune/*']   # this script and its probes name every symbol on purpose

# Code files only: comments count, Markdown does not.
CODE_GLOBS = ['*.h', '*.hpp', '*.cpp', '*.inl', '*.py', '*.as']
CPP_GLOBS = ['*.h', '*.hpp', '*.cpp', '*.inl']
ROOTS = ['src', 'tools', 'tests']
CPP_ROOTS = ['src', 'tests']

# ---------------------------------------------------------------------------------------------
# PRUNED: must appear nowhere in code (map Phase 6 item 6 as R8.4 built it, plus what R8.1 and
# R8.3 pruned that has a name of its own).
# ---------------------------------------------------------------------------------------------
PRUNED = {
    # Favourites rank scaling (the uniform fFavoritesBoostMax multiplier stays in the floors)
    'ApplyFavoritesRankScaling': 'favourites rank scaling',
    'fFavoritesBoostMin': 'favourites rank scaling: INI key',
    'favoritesBoostMin': 'favourites rank scaling: config field',
    'FAVORITES_BOOST_MIN': 'favourites rank scaling: default',
    'm_favoriteRankScratch': 'favourites rank scaling: scratch',
    # The potion tier preference
    'ApplyPotionTierPreference': 'potion tier preference',
    'PotionTierPreference': 'potion tier preference: enum',
    'potionTierPreference': 'potion tier preference: config field',
    'ParsePotionTierPreference': 'potion tier preference: parser',
    'sPotionTierPreference': 'potion tier preference: INI key',
    'POTION_TIER_PREFERENCE': 'potion tier preference: default',
    'POTION_TIER_STEP': 'potion tier preference: the 1.5 step (ScorerConfig.h:28)',
    # The old weights' reason and its attribution
    'DominantReason': 'the dominant context reason',
    'DominantContextReason': 'the dominant context reason: UtilityScorer forward',
    'ReasonAppliesTo': 'the per-slot reason attribution (ExplanationLabel.h:108)',
    'ContextReasonAttribution': 'ContextReasonAttribution.{h,cpp}',
    'ContextRuleEngine_Reason': 'ContextRuleEngine_Reason.cpp',
    'ContextReasonSignals': 'the label-only signals only DominantReason read',
    'WeightFieldFor': 'the reason -> weight field table',
    'ReasonWeightField': 'the reason -> weight field table: type',
    # The skip gate's old inputs (R8.3 replaced them with the need signature)
    'ambientSignature': 'the skip gate ambient signature',
    'm_lastAmbientSignature': 'the skip gate ambient signature: latch',
    'IsElementalWindowActive': 'the skip gate elemental window',
    'm_elementalWindowActive': 'the skip gate elemental window: flag',
    'elementalDamageActive': 'the skip gate elemental window: context field',
    'm_wasElementalDamageActive': 'the skip gate elemental window: latch',
    # The potion multiplier's own combat timer (R8.1: it reads the combat_onset clock)
    'OnCombatStart': 'the potion multiplier\'s own combat timer (and UtilityScorer\'s pass-through)',
    'OnCombatEnd': 'the potion multiplier\'s own combat timer (and UtilityScorer\'s pass-through)',
    'ConsumeCombatTransition': 'the StateManager latch that fed that timer',
    # Dead accessors and helpers off the D2 path (R8.4 review)
    'GetFeatureBanditLearner': 'Globals accessor with no caller (slipped past -w FeatureBanditLearner)',
    'GetUsageMemory': 'Globals accessor with no caller',
    'GetContextWeight': 'ScoredCandidate::GetContextWeight, no caller',
    # tools/replay's old learner (map Phase 4)
    'ChoiceTarget': 'tools/replay old learner policy',
    'UsefulLife': 'tools/replay old learner policy',
    'pseudo_confidence': 'tools/replay old learner helper',
    # R11 (map Phase 10): the per-type dumps, gone with the classifiers they printed
    # (`hg dump all` is the one load-order dump, from the effect catalog)
    'Cmd_DumpSpells': 'R11: hg dump spells', 'Cmd_DumpFood': 'R11: hg dump food',
    'Cmd_DumpPotions': 'R11: hg dump potions', 'Cmd_DumpScrolls': 'R11: hg dump scrolls',
    'Cmd_DumpWeapons': 'R11: hg dump weapons', 'Cmd_DumpApparel': 'R11: hg dump apparel',
    'Cmd_DumpDiseases': 'R11: hg dump diseases', 'ItemSubclass': 'R11: the potion dump\'s classifier subclass',
    'RetryDecidedType': 'R11: SpellClassifier\'s dump-only probe',
    # R11: live readers of the classifiers, replaced by the effect catalog's facets
    'GetBestPotion': 'R11: ItemRegistry\'s potion pick by classifier type (OverrideManager::BestRestorePotion)',
    'IsResistPotionRedundant': 'R11: the resist filter by classifier tags (CandidateFilters::IsResistRedundant)',
    'IsResistSpellRedundant': 'R11: the resist filter by classifier type and element',
    'MatchesSpell': 'R11: SlotClassifier\'s tag arms (core/CapFacets.h Matches)',
    'MatchesItem': 'R11: SlotClassifier\'s tag arms', 'MatchesScroll': 'R11: SlotClassifier\'s tag arms',
    'MatchesWeapon': 'R11: SlotClassifier\'s tag arms',
    # R11 (the verifier's round): the classifier readers the override finders still reached
    'HasHarmfulSideEffects': 'R11: ItemData\'s side-effect test by tags (core/CapFacets.h HasSideEffect)',
    'GetBestCurePoisonPotion': 'R11: ItemRegistry\'s cure pick by tag (OverrideManager::FindCurePoisonItem)',
    'GetBestWaterbreathingPotion': 'R11: ItemRegistry\'s waterbreathing pick by tag (FindWaterbreathingItem)',
    'BestPotionPick': 'R11: ItemRegistry\'s pick struct (OverrideManager\'s own PotionPick)',
}
# Words too common to search repo-wide: pruned only in the files that had them.
PRUNED_SCOPED = [
    ('Learner', 'tools/replay/*.py', 'tools/replay old learner (class Learner)'),
    ('Current', 'tools/replay/*.py', 'tools/replay old learner policy (class Current)'),
    ('m_combatDuration', 'src/learning/PotionDiscriminator.*', 'the potion multiplier\'s own combat timer'),
    ('m_inCombat', 'src/learning/PotionDiscriminator.*', 'the potion multiplier\'s own combat timer'),
    # R8.1: the scorer's two Suppress skips became the favorite_suppressed filter. The live
    # scorer must not test the mode again. FavoritesMode::Suppress itself stays: the INI's mode,
    # the filter's input, and the legacy scorer's fav = 0 arm, which keeps the v3 OldScore
    # columns of a filter drop as they were.
    ('Suppress', 'src/learning/UtilityScorer.*', 'the scorer\'s Suppress branches (now a filter)'),
    # 0.24.17 (review round 1): the untagged names engine A's values once had in the pipeline,
    # before the pure pass (core/LegacyShadowPass.h) took them; a revival would carry A's values
    # past the legacyShadow tag.
    ('shadowExtras', 'src/pipeline/PipelineCoordinator.*', '0.24.17: A\'s extras now live in the pure pass'),
    ('shadowFeat', 'src/pipeline/PipelineCoordinator.*', '0.24.17: A\'s features are legacyShadowFeat'),
    ('scoresA', 'src/pipeline/PipelineCoordinator.*', '0.24.17: A\'s page scores now live in the pure pass'),
    ('scoresANow', 'src/pipeline/PipelineCoordinator.*', '0.24.17: A\'s page scores now live in the pure pass'),
    # 0.24.17 (review round 2): the allocation input the pass leaves holding A's order and mu, under
    # its old untagged names (it is legacyShadowAllocInput, legacyShadowInput, legacyShadowPageInput)
    ('allocInput', 'src/pipeline/PipelineCoordinator.*', '0.24.17: the shadow\'s input is legacyShadowAllocInput'),
    ('shadowInput', 'src/pipeline/PipelineCoordinator.*', '0.24.17: it is legacyShadowInput'),
    ('pageInput', 'src/pipeline/PipelineCoordinator.*', '0.24.17: it is legacyShadowPageInput'),
]
PRUNED_INI_KEYS = ['sPotionTierPreference', 'fFavoritesBoostMin']
# Phrases that describe a pruned behaviour as live (comments included): (name, regex, why).
STALE = [
    # Present tense only ("is derived / comes from"): the history notes that say what R8.3 and
    # R8.4 replaced ("the reason derived off the weight map ... is gone") stay.
    ('reason from weights', r'\breason\b[^.;]{0,80}?\b(?:is|are)\s+(?:derived|computed|taken)\b[^.;]{0,80}?'
                            r'\bfrom\s+the\s+(?:scorer\'?s\s+|old\s+)?context\s+weights?\b'
                            r'|\breason\b[^.;]{0,40}?\bcomes\s+from\s+the\s+(?:scorer\'?s\s+|old\s+)?context\s+weights?\b',
     'the display reason came from the old context weights (DominantReason); since R8.3 it comes '
     'from the page\'s explanations (PipelineCoordinator::UpdateContextReason)'),
    # Present tense only: the notes that say what a dump found on a date stay.
    ('per-type dump as live', r'`?hg dump (?:spells|food|potions|scrolls|weapons|apparel|diseases)`?\s+'
                              r'(?:classifies|walks|prints|writes|lists|is\s+the)\b',
     'R11 deleted the per-type dumps; `hg dump all` is the one load-order dump'),
]
# Code that must stay (path, regex over the whole file, why): deleting it lets the old utility back
# in without naming anything new. Whitespace-tolerant.
REQUIRED = [
    ('src/core/Bootstrap.cpp',
     r'if \(static_cast<Choice::Feature>\(i\) == Choice::Feature::ln_util\) \{\s*'
     r'if \(w != 0\.0\) \+\+rep\.featuresRefused;\s*continue;\s*\}',
     'the bootstrap refuses a weight on `ln util` (R8.4): without it a refit weighing the old '
     'utility would rank on it'),
]
# 0.24.17: what keeps the D2 shadow out of the live learner. CaptureEligible writes lm / um as the
# live score read them (0 / 1 under engine B, also while the legacy pass runs for the shadow), and
# ChoiceLearner trains on um / lm, not on whether the old floors set `util`.
REQUIRED += [
    ('src/pipeline/PipelineCoordinator.cpp',
     r'row\.lnLearnMult = 0\.0f;\s*row\.utilMissing = 1;\s*if \(haveLegacy\) \{\s*'
     r'const auto lg = ctx\.legacy\.RowFor\(ci\);\s*if \(ctx\.legacyLive\) \{\s*'
     r'row\.lnLearnMult = lg\.Scored\(\) \? lg\.lnLearnMult : 0\.0f;\s*'
     r'row\.utilMissing = lg\.utilMissing \? 1 : 0;\s*\}\s*'
     r'if \(static_cast<uint8_t>\(lg\.floor\) != row\.old\.floor\) \+\+floorMismatch;\s*\}',
     '0.24.17: the v3 rows\' lm / um are what the live score read: the floors\' values only under '
     'ctx.legacyLive (bLegacyLearnFeature on), never under engine B with the shadow running'),
    ('src/learning/ChoiceLearner.cpp',
     r'const bool scored = r\.um == 0;\s*f\[static_cast<std::size_t>\(C::Feature::util_missing\)\] = '
     r'scored \? 0\.0 : 1\.0;\s*if \(scored\) \{\s*if \(std::isfinite\(r\.lm\)\) '
     r'f\[static_cast<std::size_t>\(C::Feature::ln_learn_mult\)\] = r\.lm;\s*\}',
     '0.24.17: the live learner reads util missing / ln learn mult off the row\'s um / lm (what the live '
     'score read), not off whether the old floors set util'),
    ('src/pipeline/PipelineCoordinator.cpp',
     r'const bool legacyOn = Scoring::Legacy::Enabled\(\);\s*'
     r'const bool shadowOn = !legacyOn && ctx\.choiceLive && Scoring::Legacy::ShadowEnabled\(\);\s*'
     r'const bool runLegacy = legacyOn \|\| shadowOn \|\| !ctx\.choiceLive;[^\n]*\n\s*'
     r'ctx\.legacyLive = legacyOn;',
     '0.24.17: the run\'s legacyLive is the switch itself, never the legacy pass running (the shadow runs it too)'),
    ('src/pipeline/PipelineCoordinator.cpp', r'\n\s*const bool legacyOn = ctx\.legacyLive;\s*\n\s*const bool shadowA = !legacyOn && ctx\.legacy\.ran;',
     '0.24.17: RankOnChoice reads the D2 feature live only under the run\'s legacyLive'),
    ('src/pipeline/PipelineCoordinator.h', r'\n\s*bool legacyLive = false;',
     '0.24.17: legacyLive defaults off (engine B) in the run context'),
    ('src/pipeline/PipelineCoordinator.h', r'\n\s*legacy\.Clear\(\);\s*legacyLive = false;\s*legacyShadow\.Clear\(\);\s*'
     r'legacyShadowAllocInput = \{\};',
     '0.24.17: Reset clears the switch, the shadow run and the input that held A\'s order'),
    ('src/pipeline/PipelineCoordinator.cpp',
     r'Core::LegacyShadow::RanksAndPages\([^\n]*\);\s*(?://[^\n]*\n\s*)*ctx\.legacyShadowAllocInput = \{\};',
     '0.24.17: the input the pass left holding A\'s order and mu is cleared right after the pass'),
    ('src/learning/SelectionLogV3.cpp', r'r\.lm = e\.lnLearnMult;\s*r\.um = e\.utilMissing;',
     '0.24.17: the v3 row carries the lm / um the live score read (the hop between CaptureEligible and the learner)'),
]
# R11: the facets' readers stay wired (a deleted call would let the classifiers back by default).
REQUIRED += [
    ('src/slot/SlotClassifier.cpp', r'return\s+Core::Facets::Matches\(\s*SubjectOf\(candidate\),\s*classification\);',
     'R11: SlotClassifier answers from core/CapFacets.h over the effect catalog'),
    ('src/slot/SlotClassifier.cpp', r'return\s+Core::Facets::Classify\(\s*SubjectOf\(candidate\)\);',
     'R11: the class (class cap, v2 log, dump) from core/CapFacets.h'),
    ('src/candidate/CandidateFilters.cpp', r'const auto subject = Effect::SubjectOf\(candidate\);\s*const auto f = subject\.facets;',
     'R11: the filters read the candidate\'s facets'),
    ('src/override/OverrideManager.cpp', r'Effect::FacetsOfForm\(sd\.formID\)\.facets\.Has\(restore\)',
     'R11: the restore-spell fallback reads the restore facet'),
    ('src/override/OverrideManager.cpp', r'return\s+Core::Facets::HasSideEffect\(ff\.facets\);',
     'R11: the override finders\' pure-before-impure split reads the self_harm / intoxication / hostile facets'),
    ('src/override/OverrideManager.cpp',
     r'const float restored = PotionRestoreWithin\(item\.data\.formID, target\.av, target\.vital, windowSec\);',
     'R11: a restore potion ranks on its restore row, not the costliest effect\'s magnitude'),
    ('src/candidate/CandidateFilters.cpp', r'if \(!Core::Facets::ResistJudgeable\(facets\)\) return false;',
     'R11: a hostile or damaging spell (an elemental cloak\'s resist rider) is never resist-filtered'),
    ('src/pipeline/PipelineCoordinator.cpp', r'sc\.subject = Effect::SubjectOf\(sc\.candidate\);\s*sc\.hasSubject = true;',
     'R11: every ranked row carries its facets for the slot code'),
]
# Searched for those keys by name: a missing one is a violation (a move would hide a key).
REQUIRED_INI = ['configs/Huginn.ini', 'Data/SKSE/Plugins/dmenu/customSettings/Huginn.json']

# ---------------------------------------------------------------------------------------------
# LEGACY: kept for D2, allowed only in these files.
# ---------------------------------------------------------------------------------------------
LEGACY = [
    # the old learner and its inputs
    'FeatureBanditLearner', 'g_featureBanditLearner', 'StateFeatures', 'BanditSubscriber',
    'UsageMemory', 'UsageMemorySubscriber', 'g_usageMemory',
    'PriorCalculator', 'CorrelationBooster', 'PotionDiscriminator',
    # the old utility, its floors and the cold start
    'ComputeAdaptiveLambda', 'ComputeUtility', 'ScoreCandidateInternal', 'ExplainCandidate',
    'LegacyPass', 'FloorPass',
    'minimumUtility', 'minimumContextWeight', 'coldStartUCBBoost', 'isColdStartBoosted',
    'fMinimumUtility', 'fMinimumContextWeight', 'fColdStartUCBBoost',
    # the context rules (the floors' input; a binary gate on which rows get the feature)
    'ContextRuleEngine', 'EvaluateRules', 'ContextWeightMap', 'WeightForCandidate', 'IsHardContextGated',
    # the legacy scorer split out of UtilityScorer (R8.4) and its global
    'LegacyScorer', 'g_legacyScorer',
    # the legacy module's facade names (the shared ones are caught by the namespace pattern)
    'FallbackRanking', 'KeptFor', 'KeptCount', 'RowFor', 'OldScoreCapture', 'LearnerCounts',
    # the utility -> rank-score bridge: only the fallback (legacy module) and the slot harness
    'RankOnUtility', 'BridgeScore', 'DisplayUtility',
    # the old utility's row feature (format v1 `ln util`): the bootstrap refuses a weight on it
    # (core/Bootstrap.cpp); no row, live or logged, may set it
    'ln_util',
    # the old floors' keep flag on ScoredCandidate: D2 reads it through um (the legacy module's
    # Row); a live read elsewhere is the floors filtering or ranking again
    'legacyScored',
]
# The old score's config fields (ScorerConfig.h): C++ only.
LEGACY_CPP = [
    'lambdaMin', 'lambdaMax', 'explorationWeight', 'favoritesBoostMax', 'FavoritesMultiplier',
    'topNCandidates',
    'bowArrowBonus', 'crossbowBoltBonus', 'meleeDefensiveBonus', 'silverUndeadBonus', 'fortifySchoolBonus',
    'staffLowMagickaBonus', 'twoHandedDefensiveBonus',
    'combatStartWindow', 'regenPotionCombatStartMult', 'flatRestoreLowResourceMult',
    # 0.24.17: whether the live score reads the D2 feature this run (PipelineContext::legacyLive),
    # set once from the switch, read by RankOnChoice and CaptureEligible. Another form of either
    # makes engine A live (or B's learner train on A) while the log still says engine B.
    'legacyLive',
]
# Patterns, C++ only: the facade and the member reads of the old utility (see the docstring).
LEGACY_CPP_PATTERNS = {
    'Legacy::': r'\bLegacy::|\bScoring::Legacy\b',
    'legacy include': r'#\s*include\s+"learning/legacy/',
    'ctx.legacy': r'\bctx\.legacy\b',
    'legacy.<member>': r'\blegacy\.(?:ran|rows|rowOf|kept|weights|Skip|Clear|ResolveRows)\b',
    # `.`, `->` and `::` (a qualified name, e.g. a pointer to member); `Col::utility` is the
    # effect column enum (core/EffectColumns.h), not the old utility.
    '.utility': r'(?:\.|->|(?<!\bCol)::)\s*utility\b',
    '.breakdown': r'(?:\.|->|::)\s*breakdown\b',
    # the v3 row's copy of the old utility (DecisionLog::Row::util): the live learner's view of it
    '.util': r'(?:\.|->)\s*util\b',
    # the old floors' keep test on a legacy module Row (Legacy::Row::Scored)
    '.Scored()': r'(?:\.|->)\s*Scored\s*\(',
    # the same keep as the v3 row's flag: Flag::Scored is the util-is-finite bit EligibleToRow sets,
    # on engine B's rows too while the D2 shadow runs. Any qualifier: ChoiceSet names the flags
    # through `namespace F = DecisionLog::Flag;`
    '::Scored': r'::\s*Scored\b',
    # the v3 row's OldScore columns (floor, wouldUtil, est, ...) and the decision's OldRun head,
    # read through a member: the old engine's parts as the log carries them
    '.old.<member>': r'(?:\.|->)\s*old\s*(?:\.|->)',
    # 0.24.17: the D2 engine in shadow -- engine A's mu, ranks and page keys (the pipeline's
    # LegacyShadowRun, ctx.legacyShadow, the eligible rows' and the v3 rows' legacyShadow*
    # fields). A line that names one is the shadow's writer or its log copy; anywhere else it
    # is A's ranking read live (a score, an allocation, the learner). No left boundary: a
    # prefixed name (m_legacyShadowKeyA, haveLegacyShadow) carries A's values too.
    'legacy shadow': r'(?i:legacy_?shadow)\w*',
    'breakdown part': r'(?:\.|->|::)\s*(?:contextWeight|rewardEstimate|learningScore|recencyBoost|correlationBonus'
                      r'|potionMultiplier|favoritesMultiplier|ToCompactString|ToDetailString)\b',
}

# Patterns that make a LEGACY line in one file only: words too common to tag everywhere.
FILE_PATTERNS = {
    # Inside ScoredCandidate a bare `utility` (or `breakdown`) is a read of the old utility: an
    # accessor or comparator written on it. Code only (before any `//`): the comments name it.
    'src/learning/ScoredCandidate.h': {
        'bare utility': r'^(?:(?!//).)*\b(?:utility|breakdown)\b',
    },
    # R8.1 made the scorer's Suppress skips the favorite_suppressed filter. The pipeline reads
    # FavoritesMode::Suppress once, for that filter's input (HardGateInputs); a second read
    # would be a skip put back.
    'src/pipeline/PipelineCoordinator.cpp': {
        'Suppress': r'\bSuppress\b',
        # 0.24.17: the legacy row's util missing, which only CaptureEligible's guarded block may copy
        # into the v3 row (a second copy re-feeds D2's feature to engine B's learner)
        'lg.utilMissing': r'\blg\.utilMissing\b',
        # 0.24.17: the legacy row's feature and floor (the old floors' values; with the shadow on
        # they are filled under engine B too): only D2's exact live line and the floor check read them
        'lg.<member>': r'\blg\.(?:lnLearnMult|floor)\b',
    },
}
# The live learner's path (the v3 rows it trains on: RowFeatures, OnPick, ChoiceSet): any read of
# a row's OldScore or the decision's OldRun, an alias included (`const auto& o = r.old;`). Not
# repo-wide: `.old` is a common member name (MiniRegex's backtrack entries).
for _p in ('src/learning/ChoiceLearner.cpp', 'src/learning/ChoiceSubscriber.h', 'src/core/ChoiceUpdate.h'):
    FILE_PATTERNS.setdefault(_p, {})['.old'] = r'(?:\.|->)\s*old\b'

ALLOW = [
    ('src/learning/legacy/*', 'the legacy module: the D2 feature, the pass handle, the v3 OldScore capture, '
                              'the old learner clock, the fallback ranking'),
    ('src/learning/FeatureBanditLearner.*', 'the old learner'),
    ('src/learning/StateFeatures.h', 'the old learner: its 18-float features'),
    ('src/learning/UsageMemory.*', 'the old learner: recency'),
    ('src/learning/PriorCalculator.*', 'the old score: prior'),
    ('src/learning/CorrelationBooster.*', 'the old score: correlations'),
    ('src/learning/PotionDiscriminator.*', 'the old score: potion multiplier'),
    ('src/context/ContextRuleEngine.*', 'the context rules: the floors\' input'),
    ('src/context/ContextWeightForCandidate.*', 'the context rules: weight per candidate, the hard-gate check'),
    ('src/context/ContextWeightConfig.h', 'the context rules: config'),
    ('src/learning/EquipSubscribers.h', 'the old learner\'s subscribers (BanditSubscriber, UsageMemorySubscriber); '
                                        'the live learner\'s ChoiceSubscriber is in ChoiceSubscriber.h (budget 0)'),
    ('src/learning/EquipEvent.h', 'the old learner\'s subscriber input: press-time features on the event'),
    ('src/learning/EquipEventBus.cpp', 'the old learner\'s subscriber input: the bus computes those features'),
    ('src/persist/BanditSerializer.*', 'the BNDW cosave record (D2 reads the old learner\'s per-item state)'),
    ('src/learning/SelectionLog.cpp', 'the v2 selection log (the audit trail for refits)'),
    ('src/core/OldScore.h', 'the v3 OldScore columns\' record'),
    ('src/core/DecisionLog.cpp', 'the v3 format: the OldScore columns, and the OldRun head writes the floors\' '
                                 'lambdaMin / lambdaMax'),
    ('tests/core/DecisionLogTests.cpp', 'the v3 format\'s host test (the OldRun head, the OldScore columns, '
                                        'the Scored flag)'),
    ('src/learning/ScorerSettings.*', 'settings loader: [Scoring] / [Favorites]'),
    ('src/learning/ScorerConfig.h', 'settings loader: the config it builds'),
    ('src/learning/LearningSettings.h', 'settings loader: [Learning] (the memory life: ChoiceLearner\'s b battery and the old learner)'),
    ('src/context/ContextWeightSettings.*', 'settings loader: [ContextWeights]'),
    ('tests/core/LegacyFloorPassTests.cpp', 'the host test of the moved floor loop (bit for bit)'),
    ('tests/core/LegacyRunTests.cpp', 'the host test of the legacy module\'s row resolution'),
    # 0.24.17: the D2 shadow's pass computes engine A's values in pure core (no game, no slot
    # state, no learner) from const views of the live run; its host test names them too.
    ('src/core/LegacyShadowPass.h', 'the D2 shadow pass: engine A\'s mu, ranks and stateless pages, pure core, const inputs'),
    ('tests/core/ShadowCompareTests.cpp', 'the D2 shadow pass\'s host test'),
    # The R7 slot math states slot scores as utilities through the bridge: the golden snapshot
    # format (utility tokens), the Debug capture harness and the host slot tests. The bridge's
    # definitions (core/SlotScoreMath.h) and ScoredCandidate.h hold live ranking code (RanksBefore,
    # operator<, SlotScore()), so they are patterned budgets below, not whole-file allows.
    ('src/core/SlotSnapshotIO.*', 'the R7 golden snapshot format: utility tokens bridged on load'),
    ('src/slot/SlotCapture.cpp', 'the R7 capture harness (Debug, test mode): synthetic scores as utilities'),
    ('tests/core/SlotScoreMathTests.cpp', 'the bridge\'s host test'),
    ('tests/core/SlotAlloc*', 'host slot tests: synthetic scores as utilities'),
    ('tests/core/LegacySlotPolicy.h', 'host slot tests: the R7 legacy slot policy (utility order)'),
]

# Files allowed a fixed number of LEGACY lines. With patterns, EVERY LEGACY line in the file
# must match one of them: the exact form each kept use takes. A new use anywhere in the file
# fails on the count, a changed one on the pattern. A budget is the file's footprint at R8.4;
# shrinking is fine, growing needs this table edited, with a reason. Comment lines (COMMENT)
# count against the budget like code.
COMMENT = r'^\s*//'
BUDGET = {
    # The live pipeline (PipelineCoordinator): the legacy pass and everything it reads off it.
    # 0.24.20: 60: the favourites flag written onto OldScore is a LEGACY line (`.old.<member>`).
    'src/pipeline/PipelineCoordinator.cpp': (60, [
        COMMENT,
        # CaptureEligible's floor cross-check (the one read of lg.floor)
        r'^\s*if \(static_cast<uint8_t>\(lg\.floor\) != row\.old\.floor\) \+\+floorMismatch;$',
        # the pass (or its skip with the switch off), and the switch
        r'^\s*ctx\.legacy = Scoring::Legacy::LegacyPass\(',
        r'^\s*ctx\.legacy\.Skip\(candidates\.size\(\)\);$',
        r'^\s*const bool legacyOn = Scoring::Legacy::Enabled\(\);$',
        # 0.24.17: the D2 shadow's switch (B live, the choice score ranking)
        r'^\s*const bool shadowOn = !legacyOn && ctx\.choiceLive && Scoring::Legacy::ShadowEnabled\(\);$',
        # the fallback ranking (no bootstrap): built in the legacy module
        r'^\s*ctx\.scoredCandidates = ctx\.legacy\.FallbackRanking\(\);$',
        # the old learner's weights-changed latch: a trained pick moves lm, so the run re-scores
        r'^\s*Scoring::Legacy::ClearWeightsChanged\(\);$',
        r'^\s*return Scoring::Legacy::WeightsChanged\(\) \|\|$',
        # RankOnChoice: the D2 feature per candidate, and the kept row's old parts copied for
        # the v2 log (they rank nothing)
        # 0.24.17: the D2 shadow. Engine A's values exist in this file only under legacyShadow*
        # names, every line naming one in an exact form here: the pass's include, the feature
        # vector A reads (the live call with the switch on), the pure pass's two calls, the
        # allocation-input hand-back, and the copy onto the eligible rows (the v3 log).
        r'^#include "core/LegacyShadowPass\.h"',
        r'^\s*const bool shadowA = !legacyOn && ctx\.legacy\.ran;$',
        r'^\s*std::vector<C::LegacyValues> legacyShadowFeat\(shadowA \? rows : 0\);$',
        r'^\s*if \(shadowA\) legacyShadowFeat\[i\] = C::LegacyFeature\(true, lg\.Scored\(\), '
        r'static_cast<double>\(lg\.lnLearnMult\)\);$',
        r'^\s*Core::LegacyShadow::ScoreMu\(model, tick, attrs, extras, gateRows, atLimit \? outIdx : SIZE_MAX, '
        r'leftOut, rows,$',
        r'^\s*legacyShadowFeat, ctx\.legacyShadow\);$',
        r'^\s*Core::SlotAlloc::Input\* const legacyShadowInput = ctx\.legacyShadow\.ran \? '
        r'&ctx\.legacyShadowAllocInput : nullptr;$',
        r'^\s*if \(legacyShadowInput\) \{$',
        r'^\s*\*legacyShadowInput = \{\};\s+// ',
        r'^\s*&wildcardRequest, &wildcardResult, legacyShadowInput\);$',
        r'^\s*ctx\.haveAllocInput = legacyShadowInput != nullptr && !ctx\.legacyShadowAllocInput\.slots\.empty\(\) &&$',
        r'^\s*ctx\.legacyShadowAllocInput\.candidates\.size\(\) == ctx\.pageRunRows\.size\(\);$',
        r'^\s*Core::SlotAlloc::Input\* const legacyShadowPageInput = ctx\.haveAllocInput \? '
        r'&ctx\.legacyShadowAllocInput : nullptr;$',
        r'^\s*ctx\.legacyShadowAllocInput = \{\};$',
        # 0.24.17: the switch as this run read it, and its two readers (REQUIRED below pins them too)
        r'^\s*ctx\.legacyLive = legacyOn;$',
        r'^\s*const bool legacyOn = ctx\.legacyLive;$',
        r'^\s*if \(ctx\.legacyLive\) \{$',
        r'^\s*row\.utilMissing = lg\.utilMissing \? 1 : 0;$',
        r'^\s*if \(!ctx\.legacyShadow\.ran\) return;$',
        r'^\s*Core::LegacyShadow::RanksAndPages\(liveOrder, liveMu, tie, legacyShadowPageInput, ctx\.pageRunRows, '
        r'ctx\.legacyShadow\);$',
        r'^\s*if \(ctx\.eligible\.size\(\) == ctx\.legacyShadow\.mu\.size\(\) && '
        r'ctx\.legacyShadow\.keyA\.size\(\) == ctx\.eligible\.size\(\)\) \{$',
        r'^\s*e\.legacyShadowMu = static_cast<float>\(ctx\.legacyShadow\.mu\[r\]\);$',
        r'^\s*e\.legacyShadow(?:RankA = ctx\.legacyShadow\.rankA|RankB = ctx\.legacyShadow\.rankB'
        r'|KeyA = ctx\.legacyShadow\.keyA|KeyB0 = ctx\.legacyShadow\.keyB0)\[r\];$',
        r'^\s*const Scoring::Legacy::Row lg = ctx\.legacy\.RowFor\(i\);$',
        r'^\s*if \(const auto\* L = ctx\.legacy\.KeptFor\(i\)\) \{$',
        r'^\s*sc\.utility = L->utility;$',
        r'^\s*sc\.breakdown = L->breakdown;$',
        r'^\s*sc\.isColdStartBoosted = L->isColdStartBoosted;$',
        r'^\s*sc\.legacyScored = true;$',
        r'^\s*C::SetLegacy\(x\.feat, C::LegacyFeature\(legacyOn, lg\.Scored\(\), '
        r'static_cast<double>\(lg\.lnLearnMult\)\)\);$',
        # the favourites mode for the favorite_suppressed filter's input (HardGateInputs): the
        # one read of Suppress (R8.1's skips are that filter now)
        r'^\s*g_utilityScorer->GetConfig\(\)\.favoritesMode == Scoring::FavoritesMode::Suppress\);$',
        # the Debug widget's cold-start count
        r'^\s*if \(sc\.isColdStartBoosted\) \+\+coldStartCount;$',
        # the v3 log: the OldScore capture, the shadow rows' D2 feature, the v2 row's utility
        r'^\s*const Scoring::Legacy::OldScoreCapture capture\(ctx\.legacy, ',
        r'^\s*const bool haveLegacy = ctx\.legacy\.ran && ctx\.legacy\.rows\.size\(\) == candidates\.size\(\) &&$',
        r'^\s*ctx\.legacy\.rowOf\.size\(\) == candidates\.size\(\);$',
        r'^\s*const auto lg = ctx\.legacy\.RowFor\(ci\);$',
        r'^\s*row\.utility = it->second->utility;$',
        # CaptureEligible: the v3 rows' `scored` keeps its old meaning (the floors kept the row),
        # the OldRun head's kept count, and the v3 row's lm off a kept row
        r'^\s*if \(sc\.legacyScored\) scoredByKey\.try_emplace\(key, &sc\);$',
        r'^\s*\[\]\(const Scoring::ScoredCandidate& sc\) \{ return sc\.legacyScored && !sc\.isRememberedOnly; \}\)\)\);$',
        r'^\s*row\.lnLearnMult = lg\.Scored\(\) \? lg\.lnLearnMult : 0\.0f;$',
        # 0.24.20: CaptureEligible's favourites flag for a row with no OldScore capture (the v3
        # Favorite flag reads it; IsFavorited, not an old-engine judgement)
        r'^\s*row\.old\.favorite = Candidate::IsFavorited\(c\);\s+// the v3 Favorite flag reads it$',
    ], 'the legacy pass, the switch, the fallback, the weights latch, the D2 feature per candidate, '
       'the v2 / v3 log copies, the Suppress filter input, the Debug widget cold-start count; 0.24.17: the '
       'D2 shadow (its switch, engine A\'s feature and mu, the allocation input hand-back, the ranks and '
       'page keys onto the eligible rows)'),
    'src/pipeline/PipelineCoordinator.h': (16, [
        COMMENT,
        r'^\s*#include "learning/legacy/LegacyRun\.h"',
        r'^\s*Scoring::Legacy::Run legacy;$',
        r'^\s*legacy\.Clear\(\);$',
        # 0.24.17: the D2 shadow's run (engine A's mu, ranks, page keys; core/LegacyShadowPass.h)
        r'^#include "core/LegacyShadowPass\.h"',
        r'^\s*Core::LegacyShadow::Run legacyShadow;$',
        r'^\s*legacyShadow\.Clear\(\);$',
        r'^\s*Core::SlotAlloc::Input legacyShadowAllocInput;$',
        r'^\s*legacyShadowAllocInput = \{\};$',
        # 0.24.17: the switch as the run read it: its default and its Reset (REQUIRED too)
        r'^\s*bool legacyLive = false;$',
        r'^\s*legacyLive = false;$',
    ], 'the run context\'s handle on the legacy pass, and (0.24.17) the D2 shadow\'s run'),
    # The old learner's clock and forget sweep: they run whatever bLegacyLearnFeature says, so
    # BNDW keeps ageing and stays bounded.
    'src/UpdateLoop.cpp': (4, [
        r'^\s*#include "learning/legacy/LegacyRun\.h"',
        r'^\s*if \(Scoring::Legacy::HasLearner\(\)\) \{$',
        r'^\s*Scoring::Legacy::AdvancePlayTime\(deltaSeconds\);$',
        r'^\s*Scoring::Legacy::MaybeForgetFaded\(\);$',
    ], 'the old learner\'s play clock and forget sweep (BNDW ages while the switch is off)'),
    # The [Soak] heartbeat's old-learner counts: the soak's check that the forget sweep keeps the
    # BNDW record bounded (items, trains, forgot). Off the lm / um path; reads only.
    'src/telemetry/SoakMetrics.cpp': (10, [
        r'^\s*#include "learning/legacy/LegacyRun\.h"',
        r'^\s*const auto oldLearner = Scoring::Legacy::Counts\(\);$',
        # 0.24.17: the shadowA field says moot / off from the two switches
        r'^\s*const bool shadowMoot = Scoring::Legacy::Enabled\(\);$',
        r'^\s*const bool shadowOn = Scoring::Legacy::ShadowEnabled\(\);$',
        # 0.24.17: the window's D2 shadow tally, taken and printed (A-derived counts, prefixed)
        r'^\s*Core::ShadowCompare::Tally legacyShadowTally;$',
        r'^\s*legacyShadowTally = legacyShadowWindowTally;$',
        r'^\s*legacyShadowWindowTally = \{\};$',
        r'^\s*legacyShadowTally\.(?:keys|reach|reachAOnly), legacyShadowTally\.\w+, legacyShadowTally\.\w+(?:,|\);)$',
    ], 'the [Soak] heartbeat\'s old-learner counts (BNDW bounded by the forget sweep), and (0.24.17) the '
       'shadowA field\'s two switches'),
    # 0.24.17: the [Soak] D2 shadow tally (A-derived counts) is legacyShadow-prefixed: its record
    # call and member, in their exact forms; nothing else in the header may name it.
    'src/telemetry/SoakMetrics.h': (4, [
        r'^\s*void RecordShadowPick\(const Core::ShadowCompare::Pick& legacyShadowPick\)$',
        r'^\s*if \(legacyShadowPick\.kind == Core::ShadowCompare::PickKind::None\) return;$',
        r'^\s*legacyShadowWindowTally\.Add\(legacyShadowPick\);$',
        r'^\s*Core::ShadowCompare::Tally legacyShadowWindowTally;$',
    ], 'the [Soak] D2 shadow tally (0.24.17)'),
    # The live scorer that ranks (sort, wildcards, envelopes): no old engine, but its [Recs]
    # lines log lm's inputs (hg recs detail) and the fallback's line.
    'src/learning/UtilityScorer.h': (0, None, 'the live scorer: no old engine'),
    'src/learning/UtilityScorer.cpp': (7, [
        r'^\s*const std::string legacy = !scored\.legacyScored \? std::string\(" \| lm: under the old floors"\)$',
        r'^\s*const auto& bd = scored\.breakdown;$',
        r'^\s*: detail\s+\? fmt::format\(" \| \{\} from \{\}", p\.utilMissing != 0\.0 \? "shadow A\'s lm" : "lm", '
        r'bd\.ToDetailString\(\)\)$',
        r'^\s*scored\.score, scored\.utility,$',
        r'^\s*detail \? bd\.ToDetailString\(\) : bd\.ToCompactString\(\),$',
        r'^\s*scored\.isColdStartBoosted \? " \[COLD\]" : ""\);$',
    ], 'the live scorer: only [Recs] lines log the old parts (lm\'s inputs; the fallback line)'),
    # The v2 selection log's cached rows (PipelineStateCache feeds SelectionLog.cpp).
    'src/learning/PipelineStateCache.h': (9, [
        r'^\s*\.utility = sc\.utility,$',
        r'^\s*\.breakdown = sc\.breakdown,$',
        r'^\s*bool isColdStartBoosted = false;$',
        r'^\s*\.isColdStartBoosted = sc\.isColdStartBoosted,$',
        # 0.24.17: the eligible row's copy of the D2 shadow, for the v3 log
        r'^\s*float legacyShadowMu = std::numeric_limits<float>::quiet_NaN\(\);$',
        r'^\s*int16_t legacyShadowRank[AB] = 0;$',
        r'^\s*int8_t legacyShadowKey(?:A|B0) = -2;$',
    ], 'the v2 selection log\'s cached rows; the eligible rows\' D2 shadow fields (0.24.17)'),
    # 0.24.17: the v3 Row's D2 shadow fields (logged only; ChoiceLearner, which reads the Row,
    # names none: its budget is 0).
    'src/core/DecisionLog.h': (5, [
        r'^\s*float legacyShadowMu = kNone;$',
        r'^\s*std::int16_t legacyShadowRank[AB] = 0;$',
        r'^\s*std::int8_t legacyShadowKey(?:A|B0) = kNoShadowKey;$',
    ], 'the v3 row\'s D2 shadow columns (amu, ar, br, ak, bk)'),
    # The Debug widget's cold-start badge (reads the legacy pass's flag; ranks nothing).
    'src/ui/UtilityScorerDebugWidget.cpp': (1, [r'^\s*if \(candidate\.isColdStartBoosted\) \{$'],
                                            'the Debug widget cold-start badge'),
    # ScoredCandidate holds live ranking code (operator<, SlotScore()), so it is budgeted, not
    # allowed: only the old utility's fields, the cold-start flag and RankOnUtility (the bridge
    # for the fallback and the slot harness). A bare `utility` in its code is tagged (FILE_PATTERNS).
    'src/learning/ScoredCandidate.h': (11, [
        COMMENT,
        r'^\s*float utility = 0\.0f;$',
        r'^\s*ScoreBreakdown breakdown;\s+// ',
        r'^\s*bool isColdStartBoosted = false;\s+// ',
        r'^\s*bool legacyScored = false;$',
        r'^\s*void RankOnUtility\(float u\) noexcept \{$',
        r'^\s*utility = u;$',
        r'^\s*score = Core::BridgeScore\(u\);$',
    ], 'the old utility fields, the cold-start flag, the floors\' keep flag and RankOnUtility (the bridge of the fallback)'),
    # The R7 slot math (RanksBefore, BucketLogRatio) is live: only the bridge's two definitions.
    'src/core/SlotScoreMath.h': (1, [
        r'^\s*\[\[nodiscard\]\] inline SlotScore BridgeScore\(float utility\) noexcept$',
    ], 'the bridge: the BridgeScore definition'),
    # BuildAllocInput, the live allocation's input (SlotAllocator calls it on every allocation):
    # it carries the old utility as the CandidateRec's token for the R7 capture and the host legacy
    # slot policy. No live core reads CandidateRec::utility; the slots rank on c.score.
    'src/slot/SlotSnapshot.cpp': (1, [r'^\s*c\.utility = sc\.utility;$'],
                                  'the live alloc input: the utility token for the R7 capture and the '
                                  'host legacy slot policy (no live core reads it)'),
    # The live learner's path. ChoiceSubscriber feeds ChoiceLearner::OnPick: no old engine.
    'src/learning/ChoiceSubscriber.h': (0, None, 'the live learner\'s subscriber: no old engine'),
    # ChoiceLearner trains on the v3 rows. 0.24.17: no old-engine line at all -- util missing and
    # ln learn mult come from the row's um / lm (what the live score read), no longer from whether
    # the old floors set `util` (they do on B's rows too while the D2 shadow runs), and it names
    # none of the D2 shadow's columns.
    'src/learning/ChoiceLearner.cpp': (0, None, 'the live learner: no old engine, no D2 shadow'),
    # SelectionLogV3 builds the v3 Decision rows the live learner trains on (util, lm): only the
    # row's old-utility copy and the OldScore columns' learner fill, each in its exact form.
    # 0.24.20: 31: the OldScore member reads (`.old.<member>`) are LEGACY lines now.
    'src/learning/SelectionLogV3.cpp': (31, [
        r'^#include "FeatureBanditLearner\.h"$',
        r'^#include "UsageMemory\.h"$',
        # EligibleToRow: the row's utility and Scored flag, copied as the run left them
        r'^\s*r\.flags = Flag::Eligible \| \(std::isfinite\(e\.utility\) \? Flag::Scored : 0\);$',
        r'^\s*r\.util = e\.utility;$',
        # FillLearner: the OldScore columns (est, alpha, trains, ucb, rec) of rows the run missed
        r'^\s*if \(!old\.run\.valid \|\| old\.run\.phi\.size\(\) != Learning::StateFeatures::NUM_FEATURES\) return;$',
        r'^\s*if \(!g_featureBanditLearner \|\| !g_usageMemory\) return;$',
        r'^\s*std::array<float, Learning::StateFeatures::NUM_FEATURES> phi\{\};$',
        r'^\s*const auto learner = g_featureBanditLearner->AcquireReader\(\);$',
        r'^\s*const auto usage = g_usageMemory->AcquireReader\(old\.stateHash\);$',
        r'^\s*r\.old\.est = m\.rewardEstimate;$',
        r'^\s*r\.old\.(?:alpha = m\.confidence|trains = m\.trains|ucb = m\.ucb|rec = usage\.GetRecencyBoost\(r\.form\));$',
        r'^\s*return \(r\.flags & \(Flag::Held \| Flag::Eligible\)\) && !r\.old\.HasLearner\(\);$',
        r'^\s*if \(!\(r\.flags & \(Flag::Held \| Flag::Eligible\)\) \|\| r\.old\.HasLearner\(\)\) continue;$',
        # the added row's FillLearner reads the decision's OldRun head (the state features)
        r'^\s*if \(d\.ctx->old\.valid\) \{$',
        # Annotate: a filter drop's OldScore columns, with no floor (it never reached the floors)
        r'^\s*r\.old\.floor = Core::DecisionLog::kNoFloor;\s+// not eligible: no floor$',
        # EligibleToRow and Annotate: the favourites flag rides on OldScore (IsFavorited, not an
        # old-engine judgement) into the v3 Favorite flag
        r'^\s*if \(e\.old\.favorite\) r\.flags \|= Flag::Favorite;$',
        r'^\s*// eligible row\'s bit is set from e\.old\.favorite\.$',
        # 0.24.17: EligibleToRow copies the D2 shadow's columns; the [Soak] tally reads A's page key
        r'^\s*r\.legacyShadow(Mu|RankA|RankB|KeyA|KeyB0) = e\.legacyShadow\1;$',
        r'^\s*if \(r\.legacyShadowKeyA != Core::DecisionLog::kNoShadowKey\) \{$',
        r'^\s*const std::int8_t legacyShadowChosen(A|B0) = chosen \? chosen->legacyShadowKey\1 : '
        r'Core::DecisionLog::kNoShadowKey;$',
        r'^\s*chosen != nullptr, legacyShadowChosenA, legacyShadowChosenB0\);$',
        r'^\s*const auto legacyShadowPick = Core::ShadowCompare::ClassifyPick\(d\.outcome == Outcome::Nothing, d\.repeat,$',
        r'^\s*Telemetry::SoakMetrics::GetSingleton\(\)\.RecordShadowPick\(legacyShadowPick\);$',
    ], 'the v3 rows the live learner trains on: the utility copy and the OldScore learner fill; the D2 '
       'shadow\'s columns and the [Soak] tally\'s read of them (0.24.17); the OldScore columns of a filter '
       'drop and the favourites flag they carry (0.24.20)'),
    # `ln_util`: its enum line, the bootstrap's refusal (REQUIRED below keeps it whole) and the
    # refusal's host test.
    'src/core/ChoiceScore.h': (1, [r'^\s*ln_util,$'], 'the format v1 feature enum: `ln util`\'s slot'),
    'src/core/Bootstrap.cpp': (1, [r'^\s*if \(static_cast<Choice::Feature>\(i\) == Choice::Feature::ln_util\) \{$'],
                               'the ln util refusal'),
    'tests/core/BootstrapTests.cpp': (1, [r'^\s*CHECK\(m\.model\.w\[static_cast<std::size_t>\(C::Feature::ln_util\)\] == 0\.0\);$'],
                                      'the ln util refusal\'s host test'),
    # Construction, settings and the legacy commands and tests (counted, not whole-file).
    # R11: 31, down from 34: the per-type dumps and their train-count columns went.
    # 0.24.17: 32: hg status names the D2 shadow's switch beside the feature's.
    'src/console/ConsoleCommands.cpp': (32, None, 'hg weights, hg dump weights, hg status counts and the '
                                                   'switches, hg reset'),
    # 0.24.17: 25: the D2 shadow's switch set at load.
    'src/Main.cpp': (25, None, 'construction, the equip-bus registration and the switches'),
    'src/Globals.h': (9, None, 'construction'),
    'src/Globals.cpp': (6, None, 'construction and the reset'),
    # 0.24.17: 10: the D2 shadow's switch hot-reloaded.
    'src/settings/SettingsReloader.cpp': (10, None, 'settings loader: hot reload, the switches and the dMenu reset'),
    'src/settings/SettingsReloader.h': (2, None, 'settings loader: the reset-weights doc comment'),
    'src/Tests.cpp': (282, None, 'the in-game Debug suite\'s legacy and slot-harness tests'),
}


# ---------------------------------------------------------------------------------------------
# CLASSIFIER (R11): the old classifiers' judgements, confined to their own files and D2's inputs.
# ---------------------------------------------------------------------------------------------
CLASSIFIER = [
    # the classifiers and their override loaders (Huginn_Overrides.ini: type/tag corrections)
    'SpellClassifier', 'ItemClassifier', 'ScrollClassifier', 'ClassifySpell', 'ClassifyItem', 'ClassifyScroll',
    'ClassifySoulGem', 'SpellOverrides', 'ItemOverrides',
    # the tag and type vocabulary they produce
    'SpellTag', 'SpellTagExt', 'ItemTag', 'ItemTagExt', 'WeaponTag', 'ScrollTag', 'ScrollTagExt',
    'HasTag', 'HasTagExt', 'ItemType', 'ScrollType', 'ElementType',
    'SpellTypeToString', 'ItemTypeToString', 'ElementTypeToString',
    # the registries' views by classifier type
    'GetItemsByType', 'GetSpellCountByType',
]
# C++ patterns (code only): the spell type enum (not the game's RE::MagicSystem::SpellType), and a
# member read of a candidate's or a registry entry's classifier fields. `.element` is not looked
# for in src/core (the effect mapper's own `raw.element`; core cannot see the candidate types).
CLASSIFIER_CPP_PATTERNS = {
    'SpellType': r'(?<!MagicSystem::)(?<![A-Za-z0-9_])SpellType(?![A-Za-z0-9_])',
    '.tags': r'(?:\.|->)\s*(?:tags|tagsExt)\b',
    '.element': r'(?:\.|->)\s*element\b',
    # The registries' and ItemData's own views by classifier type or tag: a call from a live file
    # reads the classifiers through an allowlisted file's member function (how the override
    # finders kept reading tags through ItemData::HasHarmfulSideEffects).
    'potion views': r'\bGet(?:Best)?(?:Health|Magicka|Stamina|Resist(?:Fire|Frost|Shock|Poison|Magic)|'
                    r'Cure(?:Disease|Poison)|Fortify(?:School|Combat|Utility)|Invisibility|Waterbreathing)'
                    r'Potions?(?:ByMagnitude)?\b|\bGetItemsWithTag\b|\bIsFortify(?:Destruction|Smithing)\b',
    # ScrollRegistry's views by ScrollType or element (today only the Debug registry widget calls
    # them): the same way round through src/scroll/*.
    'scroll views': r'\bGet(?:Best)?(?:Damage|Healing|Defensive|Utility|Summon|Fire|Frost|Shock)Scrolls?\b'
                    r'|\bGetScrollsByType\b',
}
CLASSIFIER_ALLOW = [
    ('src/spell/*', 'the spell classifier, its data, overrides and registry (the D2 inputs\' producer)'),
    ('src/learning/item/*', 'the item classifier, its data, overrides and registry'),
    ('src/scroll/*', 'the scroll classifier, its data and registry'),
    ('src/weapon/*', 'the weapon classifier, its data and registry (WeaponTag)'),
    ('src/candidate/CandidateTypes.*', 'the candidates\' copies of the classifier fields (the legacy pass reads them)'),
    ('src/context/ContextWeightForCandidate.*', 'D2: the old context weight per candidate (the floors\' input)'),
    ('src/context/ContextRuleEngine.*', 'D2: the old context rules'),
    ('src/learning/CorrelationBooster.*', 'D2: the old correlations'),
    ('src/learning/PotionDiscriminator.*', 'D2: the old potion multiplier'),
    ('src/learning/PriorCalculator.*', 'D2: the old prior'),
    ('src/learning/legacy/*', 'D2: the legacy module'),
    ('src/IniLoad.h', 'the Huginn_Overrides.ini section parser the classifiers\' override loaders share'),
    ('src/ui/RegistryDebugWidget.cpp', 'the Debug registry widget: shows the registries\' classifier fields, places nothing'),
]
# Files allowed a fixed number of CLASSIFIER lines (code only), each in one of its exact forms.
CLASSIFIER_BUDGET = {
    # The soul gem split: the registry's own form-kind decision (TESSoulGem, or a mod's soul-gem
    # alchemy item), which routes gems to their own candidate path; not an effect judgement.
    'src/candidate/CandidateGenerator.cpp': (2, [
        r'^\s*if \(invItem\.data\.type == Item::ItemType::SoulGem\) \{$',
        r'^\s*if \(item\.data\.type != Item::ItemType::SoulGem\) return;$',
    ], 'the soul gem split (the registry\'s form kind)'),
    # The in-game Debug suite: the classifier and D2 tests, the synthetic candidates stated in the
    # old vocabulary, and their facet resolver (InstallSyntheticFacetResolver). Counted. 177:
    # 153 plus the 25 lines of the item registry's own tests that call its potion views, less
    # the healingSpell SpellTag local that went out of RunMultiplicativeScoringTests' Test 1.
    'src/Tests.cpp': (177, None, 'the Debug suite: classifier and D2 tests, synthetic candidates and their resolver'),
}


def strip_comments(text):
    """C++ text without // and /* */ comments (line count kept; string literals not parsed)."""
    out = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    return '\n'.join(re.sub(r'//.*$', '', ln) for ln in out.split('\n'))


def classifier_allowed(path):
    for pat, why in CLASSIFIER_ALLOW:
        if fnmatch.fnmatch(path, pat):
            return why
    return None


def check_classifier(tree, out, verbose=False):
    """R11: (violations, paths, allowed counts)."""
    rx_words = word_rx(CLASSIFIER)
    rx_pats = [(name, re.compile(p)) for name, p in CLASSIFIER_CPP_PATTERNS.items()]
    per_file = defaultdict(list)
    for path, text in tree.items():
        cpp = matches(path, CPP_ROOTS, CPP_GLOBS)
        code = strip_comments(text) if cpp else re.sub(r'#.*$', '', text, flags=re.M)
        for i, line in enumerate(code.splitlines(), 1):
            tags = [m.group(0) for m in rx_words.finditer(line)]
            if cpp:
                for name, prx in rx_pats:
                    if name == '.element' and path.startswith('src/core/'):
                        continue
                    if prx.search(line):
                        tags.append(name)
            if tags:
                per_file[path].append((path, i, sorted(set(tags)), line))
    violations, allowed_counts = [], {}
    for path, fh in sorted(per_file.items()):
        why = classifier_allowed(path)
        if why:
            allowed_counts[path] = (len(fh), why)
            continue
        if path in CLASSIFIER_BUDGET:
            n, pats, why = CLASSIFIER_BUDGET[path]
            cps = [re.compile(p) for p in pats] if pats else None
            off = [h for h in fh if cps is not None and not any(c.search(h[3]) for c in cps)]
            if len(fh) <= n and not off:
                allowed_counts[path] = (len(fh), f'budget {n}{", patterned" if cps else ""}; {why}')
                continue
            if len(fh) > n:
                out(f'CLASSIFIER budget: {path} has {len(fh)} line(s), budget {n} ({why}); all listed below')
                violations += fh
            else:
                out(f'CLASSIFIER budget: {path}: {len(off)} line(s) match none of its allowed forms ({why})')
                violations += off
            continue
        violations += fh
    if violations:
        out(f'CLASSIFIER: {len(violations)} hit(s) of the retired classifiers outside their files and D2\'s inputs:')
        for path, i, tags, line in violations:
            out(f'  {path}:{i}: [{", ".join(tags)}] {line.strip()[:160]}')
    else:
        out(f'CLASSIFIER: 0 hits outside the allowlist ({len(CLASSIFIER)} symbols and {len(CLASSIFIER_CPP_PATTERNS)} '
            f'patterns, code only; {sum(c for c, _ in allowed_counts.values())} allowed lines in '
            f'{len(allowed_counts)} files, {len(CLASSIFIER_BUDGET)} budgeted)')
    if verbose:
        for path, (n, why) in sorted(allowed_counts.items()):
            out(f'  {n:4d}  {path}  ({why})')
    return violations, sorted({v[0] for v in violations})


# ---------------------------------------------------------------------------------------------
# The scan: over a {path: text} map, so the probes (test_r8_check.py) can mutate it in memory.
# ---------------------------------------------------------------------------------------------
def word_rx(words, prefixed=False):
    """Whole-word match. prefixed: a PRUNED / LEGACY word in its house spellings too -- `m_` / `g_` /
    `s_` before it (either case on its first letter) and a `Get` accessor named for it, with or
    without a camel suffix (GetUsageMemoryReader). A word that already carries a prefix
    (g_usageMemory) matches as itself only. One alternation per prefix, so a line without one
    costs one character test more than the plain match."""
    alt = '|'.join(re.escape(w) for w in words)
    free = [w for w in words if w[:1].isalpha() and not re.match(r'[mgs]_', w)] if prefixed else []
    if free:
        cased = sorted({c + w[1:] for w in free for c in (w[0].lower(), w[0].upper())})
        caps = sorted({w[0].upper() + w[1:] for w in free})
        alt = (f'(?:{alt})|[mgs]_(?:{"|".join(re.escape(w) for w in cased)})'
               f'|Get(?:{"|".join(re.escape(w) for w in caps)})(?:[A-Z0-9]\\w*)?')
    return re.compile(r'(?<![A-Za-z0-9_])(?:' + alt + r')(?![A-Za-z0-9_])')


def matches(path, roots, globs):
    return any(path.startswith(r + '/') for r in roots) and any(fnmatch.fnmatch(path, g) for g in globs)


def load_tree():
    """Every code file under ROOTS, tracked or untracked-not-ignored (so it works before a commit)."""
    res = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '--', *ROOTS], cwd=REPO,
                         capture_output=True, text=True, encoding='utf-8', errors='replace')
    if res.returncode != 0:
        sys.exit(f'git ls-files failed: {res.stderr.strip()}')
    tree = {}
    for path in sorted(set(res.stdout.splitlines())):
        if '__pycache__' in path or any(fnmatch.fnmatch(path, e) for e in EXCLUDE):
            continue
        if not matches(path, ROOTS, CODE_GLOBS):
            continue
        p = REPO / path
        if p.is_file():
            tree[path] = p.read_text(encoding='utf-8', errors='replace')
    return tree


def load_ini():
    """The shipped INI, its templates and the dMenu JSON (None: missing)."""
    # The shipped INI and the dMenu JSON by name, so a move or rename fails the check; the
    # templates and the other INIs by glob.
    files = sorted({p.relative_to(REPO).as_posix() for p in (REPO / 'configs').rglob('*.ini')}
                   | set(REQUIRED_INI))
    out = {}
    for f in files:
        p = REPO / f
        out[f] = p.read_text(encoding='utf-8', errors='replace') if p.exists() else None
    return out


def allowed(path):
    for pat, why in ALLOW:
        if fnmatch.fnmatch(path, pat):
            return why
    return None


def check(tree, ini, verbose=False, out=print):
    """Returns (number of violations, the paths that had them)."""
    bad = 0
    bad_paths = []

    # --- PRUNED ---
    rx = word_rx(list(PRUNED), prefixed=True)
    hits = []
    for path, text in tree.items():
        scoped = [w for w, g, _ in PRUNED_SCOPED if fnmatch.fnmatch(path, g)]
        rx_s = word_rx(scoped, prefixed=True) if scoped else None
        for i, line in enumerate(text.splitlines(), 1):
            words = [m.group(0) for m in rx.finditer(line)]
            if rx_s:
                words += [m.group(0) for m in rx_s.finditer(line)]
            if words:
                hits.append((path, i, sorted(set(words)), line))
    if hits:
        out(f'PRUNED: {len(hits)} hit(s) of symbols R8 removed:')
        for path, i, words, line in sorted(hits):
            out(f'  {path}:{i}: [{", ".join(words)}] {line.strip()[:160]}')
        bad += len(hits)
        bad_paths += [h[0] for h in hits]
    else:
        out(f'PRUNED: 0 hits ({len(PRUNED) + len(PRUNED_SCOPED)} symbols: {len(PRUNED)} over '
            f'{", ".join(ROOTS)}, {len(PRUNED_SCOPED)} scoped to their files; code and comments)')

    ini_hits = []
    for f, text in ini.items():
        if text is None:
            # A moved or renamed file would let a pruned key back in unseen: a violation.
            out(f'PRUNED INI keys: missing {f} (the check cannot read it)')
            bad += 1
            bad_paths.append(f)
            continue
        for i, line in enumerate(text.splitlines(), 1):
            for k in PRUNED_INI_KEYS:
                if re.search(rf'\b{k}\b', line):
                    ini_hits.append((f, i, k, line.strip()))
    if ini_hits:
        out(f'PRUNED INI keys: {len(ini_hits)} hit(s):')
        for f, i, k, line in ini_hits:
            out(f'  {f}:{i}: [{k}] {line[:160]}')
        bad += len(ini_hits)
        bad_paths += [h[0] for h in ini_hits]
    else:
        out(f'PRUNED INI keys: 0 hits ({", ".join(PRUNED_INI_KEYS)} in {len(ini)} files: '
            f'configs/**/*.ini and the dMenu JSON)')

    # --- STALE: comments that describe a pruned behaviour (two lines at a time, so a sentence
    # wrapped over a line break is still seen) ---
    stale = []
    stale_rx = [(name, re.compile(p, re.IGNORECASE), why) for name, p, why in STALE]
    for path, text in tree.items():
        lines = text.splitlines()
        seen = set()
        for i in range(len(lines)):
            pair = lines[i] + ' ' + re.sub(r'^\s*(?://+|#+|\*+)?\s*', '', lines[i + 1]) if i + 1 < len(lines) \
                else lines[i]
            for name, prx, why in stale_rx:
                if prx.search(pair) and (name, i) not in seen and (name, i - 1) not in seen:
                    seen.add((name, i))
                    stale.append((path, i + 1, name, why, lines[i]))
    if stale:
        out(f'STALE: {len(stale)} comment(s) describe a pruned behaviour:')
        for path, i, name, why, line in stale:
            out(f'  {path}:{i}: [{name}: {why}] {line.strip()[:160]}')
        bad += len(stale)
        bad_paths += [s[0] for s in stale]
    else:
        out(f'STALE: 0 hits ({len(STALE)} phrase(s))')

    # --- REQUIRED ---
    missing = [(p, why) for p, rx_req, why in REQUIRED
               if p not in tree or not re.search(rx_req, tree[p])]
    if missing:
        out(f'REQUIRED: {len(missing)} piece(s) of code gone:')
        for p, why in missing:
            out(f'  {p}: {why}')
        bad += len(missing)
        bad_paths += [p for p, _ in missing]
    else:
        out(f'REQUIRED: all {len(REQUIRED)} present')

    # --- LEGACY ---
    rx_words = word_rx(LEGACY, prefixed=True)
    rx_cpp_words = word_rx(LEGACY_CPP, prefixed=True)
    rx_pats = [(name, re.compile(p)) for name, p in LEGACY_CPP_PATTERNS.items()]
    per_file = defaultdict(list)
    for path, text in tree.items():
        cpp = matches(path, CPP_ROOTS, CPP_GLOBS)
        file_pats = [(name, re.compile(p)) for name, p in FILE_PATTERNS.get(path, {}).items()]
        for i, line in enumerate(text.splitlines(), 1):
            tags = [m.group(0) for m in rx_words.finditer(line)]
            if cpp:
                tags += [m.group(0) for m in rx_cpp_words.finditer(line)]
                tags += [name for name, prx in rx_pats if prx.search(line)]
            tags += [name for name, prx in file_pats if prx.search(line)]
            if tags:
                per_file[path].append((path, i, sorted(set(tags)), line))
    violations = []
    allowed_counts = {}
    for path, fh in sorted(per_file.items()):
        why = allowed(path)
        if why:
            allowed_counts[path] = (len(fh), why)
            continue
        if path in BUDGET:
            n, pats, why = BUDGET[path]
            cps = [re.compile(p) for p in pats] if pats else None
            off = [h for h in fh if cps is not None and not any(c.search(h[3]) for c in cps)]
            if len(fh) <= n and not off:
                allowed_counts[path] = (len(fh), f'budget {n}{", patterned" if cps else ""}; {why}')
                continue
            if len(fh) > n:
                out(f'LEGACY budget: {path} has {len(fh)} LEGACY line(s), budget {n} ({why}); all listed below')
                violations += fh
            else:
                out(f'LEGACY budget: {path}: {len(off)} line(s) match none of its allowed forms ({why})')
                violations += off
            continue
        violations += fh
    for path, (n, _, why) in BUDGET.items():   # a budgeted file with no LEGACY line at all
        if path not in per_file:
            allowed_counts[path] = (0, f'budget {n}; {why}')
    if violations:
        out(f'LEGACY: {len(violations)} hit(s) outside the D2 allowlist:')
        for path, i, tags, line in violations:
            out(f'  {path}:{i}: [{", ".join(tags)}] {line.strip()[:160]}')
        bad += len(violations)
        bad_paths += [v[0] for v in violations]
    else:
        out(f'LEGACY: 0 hits outside the allowlist ({len(LEGACY) + len(LEGACY_CPP)} symbols and '
            f'{len(LEGACY_CPP_PATTERNS)} patterns; {sum(c for c, _ in allowed_counts.values())} allowed lines '
            f'in {len(allowed_counts)} files, {len(BUDGET)} of them budgeted)')
    if verbose:
        for path, (n, why) in sorted(allowed_counts.items()):
            out(f'  {n:4d}  {path}  ({why})')

    # --- CLASSIFIER (R11) ---
    cv, cpaths = check_classifier(tree, out, verbose)
    bad += len(cv)
    bad_paths += cpaths
    return bad, sorted(set(bad_paths))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # code comments carry arrows and dashes
    bad, _ = check(load_tree(), load_ini(), args.verbose)
    print('r8_check: FAIL' if bad else 'r8_check: PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
