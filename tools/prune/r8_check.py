"""R8's 'Done when', mechanically (R8.4, 0.24.1): the prune grep and the D2 allowlist.

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
          The utility -> rank-score bridge (RankOnUtility, BridgeScore, DisplayUtility) is
          LEGACY too, so utility cannot rank outside the fallback in the legacy module and the
          slot harness that states scores as utilities. A few words are LEGACY in one file only
          (FILE_PATTERNS): a bare `utility` / `breakdown` in ScoredCandidate.h's code, and
          `Suppress` in PipelineCoordinator.cpp. A file that ranks or does slot math live
          (ScoredCandidate.h, SlotScoreMath.h, UtilityScorer, PipelineCoordinator) is never
          allowed whole: it is a patterned budget. Nor is a file on the live learner's path
          (ChoiceSubscriber.h: no LEGACY line; SelectionLogV3.cpp, which builds the v3 rows the
          learner trains on: patterned).
          The old utility's row feature (`ln_util`) is LEGACY too: the bootstrap refuses a weight
          on it, and nothing but that refusal, its enum line and its host test may name it.
  STALE   phrases that describe a pruned behaviour as live (the display reason derived from the
          context weights), in code and comments, two lines at a time.
  REQUIRED code that must stay, verbatim up to whitespace: the bootstrap's ln util refusal.

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
]
# The old score's config fields (ScorerConfig.h): C++ only.
LEGACY_CPP = [
    'lambdaMin', 'lambdaMax', 'explorationWeight', 'favoritesBoostMax', 'FavoritesMultiplier',
    'topNCandidates',
    'bowArrowBonus', 'crossbowBoltBonus', 'meleeDefensiveBonus', 'silverUndeadBonus', 'fortifySchoolBonus',
    'staffLowMagickaBonus', 'twoHandedDefensiveBonus',
    'combatStartWindow', 'regenPotionCombatStartMult', 'flatRestoreLowResourceMult',
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
    },
}

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
    ('src/core/DecisionLog.cpp', 'the v3 format: the OldRun head writes the floors\' lambdaMin / lambdaMax'),
    ('tests/core/DecisionLogTests.cpp', 'the v3 format\'s host test (the OldRun head)'),
    ('src/learning/ScorerSettings.*', 'settings loader: [Scoring] / [Favorites]'),
    ('src/learning/ScorerConfig.h', 'settings loader: the config it builds'),
    ('src/learning/LearningSettings.h', 'settings loader: [Learning] (the old learner\'s memory life)'),
    ('src/context/ContextWeightSettings.*', 'settings loader: [ContextWeights]'),
    ('tests/core/LegacyFloorPassTests.cpp', 'the host test of the moved floor loop (bit for bit)'),
    ('tests/core/LegacyRunTests.cpp', 'the host test of the legacy module\'s row resolution'),
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
    'src/pipeline/PipelineCoordinator.cpp': (24, [
        COMMENT,
        # the pass (or its skip with the switch off), and the switch
        r'^\s*ctx\.legacy = Scoring::Legacy::LegacyPass\(',
        r'^\s*ctx\.legacy\.Skip\(candidates\.size\(\)\);$',
        r'^\s*const bool legacyOn = Scoring::Legacy::Enabled\(\);$',
        # the fallback ranking (no bootstrap): built in the legacy module
        r'^\s*ctx\.scoredCandidates = ctx\.legacy\.FallbackRanking\(\);$',
        # the old learner's weights-changed latch: a trained pick moves lm, so the run re-scores
        r'^\s*Scoring::Legacy::ClearWeightsChanged\(\);$',
        r'^\s*return Scoring::Legacy::WeightsChanged\(\) \|\|$',
        # RankOnChoice: the D2 feature per candidate, and the kept row's old parts copied for
        # the v2 log (they rank nothing)
        r'^\s*const bool legacyOn = ctx\.legacy\.ran;',
        r'^\s*const Scoring::Legacy::Row lg = ctx\.legacy\.RowFor\(i\);$',
        r'^\s*if \(const auto\* L = ctx\.legacy\.KeptFor\(i\)\) \{$',
        r'^\s*sc\.utility = L->utility;$',
        r'^\s*sc\.breakdown = L->breakdown;$',
        r'^\s*sc\.isColdStartBoosted = L->isColdStartBoosted;$',
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
    ], 'the legacy pass, the switch, the fallback, the weights latch, the D2 feature per candidate, '
       'the v2 / v3 log copies, the Suppress filter input, the Debug widget cold-start count'),
    'src/pipeline/PipelineCoordinator.h': (6, [
        COMMENT,
        r'^\s*#include "learning/legacy/LegacyRun\.h"',
        r'^\s*Scoring::Legacy::Run legacy;$',
        r'^\s*legacy\.Clear\(\);$',
    ], 'the run context\'s handle on the legacy pass'),
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
    'src/telemetry/SoakMetrics.cpp': (2, [
        r'^\s*#include "learning/legacy/LegacyRun\.h"',
        r'^\s*const auto oldLearner = Scoring::Legacy::Counts\(\);$',
    ], 'the [Soak] heartbeat\'s old-learner counts (BNDW bounded by the forget sweep)'),
    # The live scorer that ranks (sort, wildcards, envelopes): no old engine, but its [Recs]
    # lines log lm's inputs (hg recs detail) and the fallback's line.
    'src/learning/UtilityScorer.h': (0, None, 'the live scorer: no old engine'),
    'src/learning/UtilityScorer.cpp': (6, [
        r'^\s*const auto& bd = scored\.breakdown;$',
        r'^\s*: detail\s+\? fmt::format\(" \| lm from \{\}", bd\.ToDetailString\(\)\)$',
        r'^\s*scored\.score, scored\.utility,$',
        r'^\s*detail \? bd\.ToDetailString\(\) : bd\.ToCompactString\(\),$',
        r'^\s*scored\.isColdStartBoosted \? " \[COLD\]" : ""\);$',
    ], 'the live scorer: only [Recs] lines log the old parts (lm\'s inputs; the fallback line)'),
    # The v2 selection log's cached rows (PipelineStateCache feeds SelectionLog.cpp).
    'src/learning/PipelineStateCache.h': (4, [
        r'^\s*\.utility = sc\.utility,$',
        r'^\s*\.breakdown = sc\.breakdown,$',
        r'^\s*bool isColdStartBoosted = false;$',
        r'^\s*\.isColdStartBoosted = sc\.isColdStartBoosted,$',
    ], 'the v2 selection log\'s cached rows'),
    # The Debug widget's cold-start badge (reads the legacy pass's flag; ranks nothing).
    'src/ui/UtilityScorerDebugWidget.cpp': (1, [r'^\s*if \(candidate\.isColdStartBoosted\) \{$'],
                                            'the Debug widget cold-start badge'),
    # ScoredCandidate holds live ranking code (operator<, SlotScore()), so it is budgeted, not
    # allowed: only the old utility's fields, the cold-start flag and RankOnUtility (the bridge
    # for the fallback and the slot harness). A bare `utility` in its code is tagged (FILE_PATTERNS).
    'src/learning/ScoredCandidate.h': (9, [
        COMMENT,
        r'^\s*float utility = 0\.0f;$',
        r'^\s*ScoreBreakdown breakdown;\s+// ',
        r'^\s*bool isColdStartBoosted = false;\s+// ',
        r'^\s*void RankOnUtility\(float u\) noexcept \{$',
        r'^\s*utility = u;$',
        r'^\s*score = Core::BridgeScore\(u\);$',
    ], 'the old utility fields, the cold-start flag and RankOnUtility (the bridge of the fallback)'),
    # The R7 slot math (RanksBefore, BucketLogRatio) is live: only the bridge's two definitions.
    'src/core/SlotScoreMath.h': (2, [
        r'^\s*\[\[nodiscard\]\] inline SlotScore BridgeScore\(float utility\) noexcept$',
        r'^\s*\[\[nodiscard\]\] inline float DisplayUtility\(SlotScore score\) noexcept$',
    ], 'the bridge: the BridgeScore / DisplayUtility definitions'),
    # BuildAllocInput, the live allocation's input (SlotAllocator calls it on every allocation):
    # it carries the old utility as the CandidateRec's token for the R7 capture and the host legacy
    # slot policy. No live core reads CandidateRec::utility; the slots rank on c.score.
    'src/slot/SlotSnapshot.cpp': (1, [r'^\s*c\.utility = sc\.utility;$'],
                                  'the live alloc input: the utility token for the R7 capture and the '
                                  'host legacy slot policy (no live core reads it)'),
    # The live learner's path. ChoiceSubscriber feeds ChoiceLearner::OnPick: no old engine.
    'src/learning/ChoiceSubscriber.h': (0, None, 'the live learner\'s subscriber: no old engine'),
    # SelectionLogV3 builds the v3 Decision rows the live learner trains on (util, lm): only the
    # row's old-utility copy and the OldScore columns' learner fill, each in its exact form.
    'src/learning/SelectionLogV3.cpp': (10, [
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
    ], 'the v3 rows the live learner trains on: the utility copy and the OldScore learner fill'),
    # `ln_util`: its enum line, the bootstrap's refusal (REQUIRED below keeps it whole) and the
    # refusal's host test.
    'src/core/ChoiceScore.h': (1, [r'^\s*ln_util,$'], 'the format v1 feature enum: `ln util`\'s slot'),
    'src/core/Bootstrap.cpp': (1, [r'^\s*if \(static_cast<Choice::Feature>\(i\) == Choice::Feature::ln_util\) \{$'],
                               'the ln util refusal'),
    'tests/core/BootstrapTests.cpp': (1, [r'^\s*CHECK\(m\.model\.w\[static_cast<std::size_t>\(C::Feature::ln_util\)\] == 0\.0\);$'],
                                      'the ln util refusal\'s host test'),
    # Construction, settings and the legacy commands and tests (counted, not whole-file).
    'src/console/ConsoleCommands.cpp': (34, None, 'hg weights, hg dump weights, the dumps\' train-count '
                                                   'columns, hg status counts and the switch, hg reset'),
    'src/Main.cpp': (24, None, 'construction, the equip-bus registration and the switch'),
    'src/Globals.h': (9, None, 'construction'),
    'src/Globals.cpp': (6, None, 'construction and the reset'),
    'src/settings/SettingsReloader.cpp': (9, None, 'settings loader: hot reload, the switch and the dMenu reset'),
    'src/settings/SettingsReloader.h': (2, None, 'settings loader: the reset-weights doc comment'),
    'src/Tests.cpp': (295, None, 'the in-game Debug suite\'s legacy and slot-harness tests'),
}


# ---------------------------------------------------------------------------------------------
# The scan: over a {path: text} map, so the probes (test_r8_check.py) can mutate it in memory.
# ---------------------------------------------------------------------------------------------
def word_rx(words):
    return re.compile(r'(?<![A-Za-z0-9_])(?:' + '|'.join(re.escape(w) for w in words) + r')(?![A-Za-z0-9_])')


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
    rx = word_rx(list(PRUNED))
    hits = []
    for path, text in tree.items():
        scoped = [w for w, g, _ in PRUNED_SCOPED if fnmatch.fnmatch(path, g)]
        rx_s = word_rx(scoped) if scoped else None
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
    rx_words = word_rx(LEGACY)
    rx_cpp_words = word_rx(LEGACY_CPP)
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
