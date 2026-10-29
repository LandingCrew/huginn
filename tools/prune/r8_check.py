"""R8's 'Done when', mechanically (R8.4, 0.24.1): the prune grep and the D2 allowlist.

R8 prunes the old ranking layer, but R6 chose D2, whose `ln learn mult` feature needs the old
learner's learning score on the rows the old floors pass. So two lists:

  PRUNED  symbols that are gone. `git grep -w` over the code under src/, tools/ and tests/
          (comments included; Markdown docs excluded, they follow in R11) must find nothing.
          The shipped INI and the dMenu JSON must not carry the pruned INI keys.
  LEGACY  the old engine R8 keeps for D2 only. These may appear only in the allowlisted files
          below, each with its reason, or in a BUDGET file: a live file allowed a fixed number of
          LEGACY lines (optionally each matching a pattern), so a new use anywhere in it fails.
          PipelineCoordinator.cpp, the live pipeline, may name the old engine on ONE line: its
          call into the legacy pass. UtilityScorer.{h,cpp}, the live scorer that ranks, on none.
          The list covers the old engine's types and globals, its config fields (C++ only: they
          are common words in the Python fit tools), and the utility -> rank-score bridge
          (RankOnUtility, BridgeScore, DisplayUtility), so utility cannot rank again outside the
          fallback in the legacy module and the slot harness that states scores as utilities.

Exits 1 and prints every hit on a violation; 0 otherwise. Standard library only.

usage:  python -I tools/prune/r8_check.py [--verbose]
        (--verbose also lists the allowlisted LEGACY hits per file)

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
SELF = 'tools/prune/r8_check.py'

# Code files only: comments count, Markdown does not.
CODE_GLOBS = ['*.h', '*.hpp', '*.cpp', '*.inl', '*.py', '*.as']
ROOTS = ['src', 'tools', 'tests']

# ---------------------------------------------------------------------------------------------
# PRUNED: must appear nowhere in code (map Phase 6 item 6 as R8.4 built it).
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
    # Dead accessors and helpers off the D2 path (R8.4 review)
    'GetFeatureBanditLearner': 'Globals accessor with no caller (slipped past -w FeatureBanditLearner)',
    'GetUsageMemory': 'Globals accessor with no caller',
    'GetContextWeight': 'ScoredCandidate::GetContextWeight, no caller',
    # tools/replay's old learner (map Phase 4)
    'ChoiceTarget': 'tools/replay old learner policy',
    'UsefulLife': 'tools/replay old learner policy',
    'pseudo_confidence': 'tools/replay old learner helper',
}
# Words too common to grep repo-wide: pruned only where they were defined.
PRUNED_SCOPED = {
    'Learner': ('tools/replay/*.py', 'tools/replay old learner (class Learner)'),
    'Current': ('tools/replay/*.py', 'tools/replay old learner policy (class Current)'),
}
PRUNED_INI_KEYS = ['sPotionTierPreference', 'fFavoritesBoostMin']
# The shipped INI, its templates and the dMenu JSON.
INI_FILES = sorted(p.relative_to(REPO).as_posix() for p in (REPO / 'configs').rglob('*.ini')) + [
    'Data/SKSE/Plugins/dmenu/customSettings/Huginn.json']

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
    'minimumUtility', 'minimumContextWeight', 'coldStartUCBBoost',
    'fMinimumUtility', 'fMinimumContextWeight', 'fColdStartUCBBoost',
    # the context rules (the floors' input; a binary gate on which rows get the feature)
    'ContextRuleEngine', 'EvaluateRules', 'ContextWeightMap', 'WeightForCandidate', 'IsHardContextGated',
    # the legacy scorer split out of UtilityScorer (R8.4) and its global
    'LegacyScorer', 'g_legacyScorer',
    # the utility -> rank-score bridge: only the fallback (legacy module) and the slot harness
    'RankOnUtility', 'BridgeScore', 'DisplayUtility',
]
# The old score's config fields (ScorerConfig.h): C++ only.
LEGACY_CPP = [
    'lambdaMin', 'lambdaMax', 'explorationWeight', 'favoritesBoostMax', 'FavoritesMultiplier',
    'topNCandidates',
    'bowArrowBonus', 'crossbowBoltBonus', 'meleeDefensiveBonus', 'silverUndeadBonus', 'fortifySchoolBonus',
    'staffLowMagickaBonus', 'twoHandedDefensiveBonus',
    'combatStartWindow', 'regenPotionCombatStartMult', 'flatRestoreLowResourceMult',
]
CPP_GLOBS = ['*.h', '*.hpp', '*.cpp', '*.inl']

ALLOW = [
    ('src/learning/legacy/*', 'the legacy module: the D2 feature, the pass handle, the v3 OldScore capture, '
                              'the old learner clock'),
    ('src/learning/FeatureBanditLearner.*', 'the old learner'),
    ('src/learning/StateFeatures.h', 'the old learner: its 18-float features'),
    ('src/learning/UsageMemory.*', 'the old learner: recency'),
    ('src/learning/PriorCalculator.*', 'the old score: prior'),
    ('src/learning/CorrelationBooster.*', 'the old score: correlations'),
    ('src/learning/PotionDiscriminator.*', 'the old score: potion multiplier'),
    ('src/context/ContextRuleEngine.*', 'the context rules: the floors\' input'),
    ('src/context/ContextWeightForCandidate.*', 'the context rules: weight per candidate, the hard-gate check'),
    ('src/context/ContextWeightConfig.h', 'the context rules: config'),
    ('src/learning/EquipSubscribers.h', 'the old learner\'s subscribers (BanditSubscriber, UsageMemorySubscriber)'),
    ('src/learning/EquipEvent.h', 'the old learner\'s subscriber input: press-time features on the event'),
    ('src/learning/EquipEventBus.cpp', 'the old learner\'s subscriber input: the bus computes those features'),
    ('src/persist/BanditSerializer.*', 'the BNDW cosave record (D2 reads the old learner\'s per-item state)'),
    ('src/learning/SelectionLog.cpp', 'the v2 selection log (the audit trail for refits)'),
    ('src/learning/SelectionLogV3.cpp', 'the v3 OldScore columns (the audit trail for refits)'),
    ('src/core/OldScore.h', 'the v3 OldScore columns\' record'),
    ('src/core/DecisionLog.cpp', 'the v3 format: the OldRun head writes the floors\' lambdaMin / lambdaMax'),
    ('tests/core/DecisionLogTests.cpp', 'the v3 format\'s host test (the OldRun head)'),
    ('src/learning/ScorerSettings.*', 'settings loader: [Scoring] / [Favorites]'),
    ('src/learning/ScorerConfig.h', 'settings loader: the config it builds'),
    ('src/learning/LearningSettings.h', 'settings loader: [Learning] (the old learner\'s memory life)'),
    ('src/context/ContextWeightSettings.*', 'settings loader: [ContextWeights]'),
    ('tests/core/LegacyFloorPassTests.cpp', 'the host test of the moved floor loop (bit for bit)'),
    # The R7 slot math states slot scores as utilities through the bridge: the definitions, the
    # golden snapshot format (utility tokens), the Debug capture harness and the host slot tests.
    ('src/learning/ScoredCandidate.h', 'the bridge: RankOnUtility\'s definition'),
    ('src/core/SlotScoreMath.h', 'the bridge: BridgeScore / DisplayUtility\'s definitions'),
    ('src/core/SlotSnapshotIO.*', 'the R7 golden snapshot format: utility tokens bridged on load'),
    ('src/slot/SlotCapture.cpp', 'the R7 capture harness (Debug, test mode): synthetic scores as utilities'),
    ('tests/core/SlotScoreMathTests.cpp', 'the bridge\'s host test'),
    ('tests/core/SlotAllocAdversarialTests.cpp', 'host slot tests: synthetic scores as utilities'),
    ('tests/core/SlotAllocTestSupport.h', 'host slot tests: synthetic scores as utilities'),
]
# Files allowed a fixed number of LEGACY lines (and, with a pattern, each must match it): a new
# use anywhere in the file fails, so a whole-file allowlist cannot hide one. A budget is the
# file's footprint at R8.4; shrinking is fine, growing needs this table edited, with a reason.
BUDGET = {
    # The live pipeline: its one call into the legacy pass.
    'src/pipeline/PipelineCoordinator.cpp': (1, re.compile(r'Scoring::Legacy::LegacyPass\('),
                                             'the one call into the legacy pass'),
    # The live scorer that ranks (sort, wildcards, envelopes, [Recs]): none.
    'src/learning/UtilityScorer.h': (0, None, 'the live scorer: no old engine'),
    'src/learning/UtilityScorer.cpp': (0, None, 'the live scorer: no old engine'),
    # Construction, settings and the legacy commands and tests (counted, not whole-file).
    'src/console/ConsoleCommands.cpp': (32, None, 'hg weights, hg dump weights, the dumps\' train-count '
                                                   'columns, hg status counts, hg reset'),
    'src/Main.cpp': (23, None, 'construction and the equip-bus registration'),
    'src/Globals.h': (8, None, 'construction'),
    'src/Globals.cpp': (6, None, 'construction and the reset'),
    'src/settings/SettingsReloader.cpp': (8, None, 'settings loader: hot reload and the dMenu reset'),
    'src/settings/SettingsReloader.h': (2, None, 'settings loader: the reset-weights doc comment'),
    'src/Tests.cpp': (293, None, 'the in-game Debug suite\'s legacy and slot-harness tests'),
}


def git_grep(words, roots, globs):
    pathspecs = [f'{r}/{g}' if '/' not in g else g for r in roots for g in globs]
    cmd = ['git', 'grep', '-n', '-w', '-I', '--untracked', '--no-color']
    for w in words:
        cmd += ['-e', w]
    cmd += ['--'] + pathspecs + [f':(exclude){SELF}']
    res = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if res.returncode not in (0, 1):
        sys.exit(f'git grep failed: {res.stderr.strip()}')
    hits = []
    for line in res.stdout.splitlines():
        path, num, text = line.split(':', 2)
        if '__pycache__' in path:
            continue
        hits.append((path, int(num), text))
    return hits


def words_in(text, words):
    return [w for w in words if re.search(rf'(?<![A-Za-z0-9_]){re.escape(w)}(?![A-Za-z0-9_])', text)]


def allowed(path):
    for pat, why in ALLOW:
        if fnmatch.fnmatch(path, pat):
            return why
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # code comments carry arrows and dashes
    bad = 0

    # --- PRUNED ---
    hits = git_grep(list(PRUNED), ROOTS, CODE_GLOBS)
    for word, (glob, _) in PRUNED_SCOPED.items():
        hits += [h for h in git_grep([word], ['tools'], [glob]) if fnmatch.fnmatch(h[0], glob)]
    pruned_words = list(PRUNED) + list(PRUNED_SCOPED)
    if hits:
        print(f'PRUNED: {len(hits)} hit(s) of symbols R8 removed:')
        for path, num, text in sorted(set(hits)):
            print(f'  {path}:{num}: [{", ".join(words_in(text, pruned_words))}] {text.strip()[:160]}')
        bad += len(hits)
    else:
        print(f'PRUNED: 0 hits ({len(PRUNED) + len(PRUNED_SCOPED)} symbols over {", ".join(ROOTS)}; code and comments)')

    ini_hits = []
    for f in INI_FILES:
        p = REPO / f
        if not p.exists():
            print(f'  (missing {f})')
            continue
        for i, line in enumerate(p.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            for k in PRUNED_INI_KEYS:
                if re.search(rf'\b{k}\b', line):
                    ini_hits.append((f, i, k, line.strip()))
    if ini_hits:
        print(f'PRUNED INI keys: {len(ini_hits)} hit(s):')
        for f, i, k, line in ini_hits:
            print(f'  {f}:{i}: [{k}] {line[:160]}')
        bad += len(ini_hits)
    else:
        print(f'PRUNED INI keys: 0 hits ({", ".join(PRUNED_INI_KEYS)} in {", ".join(INI_FILES)})')

    # --- LEGACY ---
    hits = set(git_grep(LEGACY, ROOTS, CODE_GLOBS)) | set(git_grep(LEGACY_CPP, ['src', 'tests'], CPP_GLOBS))
    legacy_words = LEGACY + LEGACY_CPP
    per_file = defaultdict(list)
    for h in sorted(hits):
        per_file[h[0]].append(h)
    violations = []
    allowed_counts = {}
    for path, fh in sorted(per_file.items()):
        why = allowed(path)
        if why:
            allowed_counts[path] = (len(fh), why)
            continue
        if path in BUDGET:
            n, pat, why = BUDGET[path]
            ok = [h for h in fh if pat is None or pat.search(h[2])]
            rest = [h for h in fh if pat is not None and not pat.search(h[2])]
            if len(ok) <= n and not rest:
                allowed_counts[path] = (len(fh), f'budget {n}{": " + pat.pattern if pat else ""}; {why}')
                continue
            if len(ok) > n:
                print(f'LEGACY budget: {path} has {len(ok)} LEGACY line(s), budget {n} ({why}); all listed below')
                violations += ok
            violations += rest
            continue
        violations += fh
    for path, (n, pat, why) in BUDGET.items():   # a budgeted file with no LEGACY line at all
        if path not in per_file and path not in allowed_counts:
            allowed_counts[path] = (0, f'budget {n}; {why}')
    if violations:
        print(f'LEGACY: {len(violations)} hit(s) outside the D2 allowlist:')
        for path, num, text in violations:
            print(f'  {path}:{num}: [{", ".join(words_in(text, legacy_words))}] {text.strip()[:160]}')
        bad += len(violations)
    else:
        print(f'LEGACY: 0 hits outside the allowlist ({len(legacy_words)} symbols; '
              f'{sum(c for c, _ in allowed_counts.values())} allowed lines in {len(allowed_counts)} files, '
              f'{len(BUDGET)} of them budgeted)')
    if args.verbose:
        for path, (n, why) in sorted(allowed_counts.items()):
            print(f'  {n:4d}  {path}  ({why})')

    print('r8_check: FAIL' if bad else 'r8_check: PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
