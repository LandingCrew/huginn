"""Mutation probes for r8_check.py (R8.4): each one plants a way back for the pruned ranking
layer, in memory, and the check must FAIL on the file it planted it in. The unmutated tree must
PASS. Standard library only.

usage:  python -I tools/prune/test_r8_check.py

A probe is (name, path, anchor, line, mode): `line` goes after (mode 'after') the first line
containing `anchor`, or replaces that line (mode 'replace'). The probes cover the verifier's
dodges of the first version of the check (utility ranking again through a member read, the
facade names, a changed allowed form) and the earlier set (old-engine names in the live scorer,
the pipeline, the console, the Debug suite, the display).
"""
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import r8_check as rc  # noqa: E402

PC = 'src/pipeline/PipelineCoordinator.cpp'
US = 'src/learning/UtilityScorer.cpp'
IM = 'src/ui/IntuitionMenu.cpp'
FALLBACK = 'ctx.scoredCandidates = ctx.legacy.FallbackRanking();'

PROBES = [
    # --- the R8.4 verifier's dodges: none renames anything ---
    ('a: utility ranks again in the live pipeline', PC, FALLBACK,
     '        for (auto& sc : ctx.scoredCandidates) sc.score = std::log(sc.utility);', 'after'),
    ('b: the ln_util feature back in RankOnChoice', PC, 'C::SetLegacy(x.feat,',
     '        x.feat[0] = std::log(std::max(sc.utility, 1e-6f));', 'after'),
    ('c: the Verbose context breakdown back', IM, 'const double s = scored.SlotScore();',
     '            const float old = scored.breakdown.contextWeight + scored.breakdown.learningScore;', 'after'),
    ('d: utility x context weight in the live scorer', US, 'const auto& p = scored.choice;',
     '            const float u = scored.utility * bd.contextWeight;', 'after'),
    ('e: a second old-learner read in SoakMetrics', 'src/telemetry/SoakMetrics.cpp',
     'const auto oldLearner = Scoring::Legacy::Counts();',
     '        const auto again = Scoring::Legacy::Counts();', 'after'),
    ('f: the old learner clock called from a display backend', 'src/display/IntuitionBackend.cpp',
     '#include', '#include "learning/legacy/LegacyRun.h"', 'after'),
    ('g: using namespace Legacy, then bare facade calls', IM, 'namespace Huginn::UI',
     'using namespace Huginn::Scoring::Legacy;', 'after'),
    ('h: an allowed pipeline form changed (utility scaled)', PC, 'sc.utility = L->utility;',
     '            sc.utility = L->utility * 2.0f;', 'replace'),
    ('i: a breakdown part read in the slot locker', 'src/slot/SlotLocker.cpp', '#include',
     'static float Probe(const Huginn::Scoring::ScoredCandidate& c) { return c.breakdown.contextWeight; }', 'after'),
    ('j: a second fallback ranking call in the pipeline', PC, FALLBACK,
     '        ctx.scoredCandidates = ctx.legacy.FallbackRanking();', 'after'),
    ('k: a kept row read off the pipeline handle elsewhere', 'src/learning/ReachReporter.cpp', '#include',
     'static auto Probe(const Huginn::Pipeline::PipelineContext& ctx) { return ctx.legacy.KeptFor(0); }', 'after'),
    # --- the earlier set ---
    ('the old learner named in the live scorer', US, 'namespace Huginn::Scoring',
     '    static Learning::FeatureBanditLearner* s_probe = nullptr;', 'after'),
    ('topNCandidates read in the pipeline', PC, FALLBACK,
     '        const auto n = g_utilityScorer->GetConfig().topNCandidates;', 'after'),
    ('lambdaMax read in the pipeline', PC, FALLBACK,
     '        const auto l = g_utilityScorer->GetConfig().lambdaMax;', 'after'),
    ('RankOnUtility called in the pipeline', PC, FALLBACK,
     '        for (auto& sc : ctx.scoredCandidates) sc.RankOnUtility(1.0f);', 'after'),
    ('a dead accessor back in the pipeline', PC, FALLBACK,
     '        auto* fb = GetFeatureBanditLearner();', 'after'),
    ('a legacy global in a console command', 'src/console/ConsoleCommands.cpp', 'namespace Huginn::Console',
     '    static auto* s_probe = g_featureBanditLearner.get();', 'after'),
    ('a legacy global in the Debug suite', 'src/Tests.cpp', 'namespace',
     'static auto* s_probe = g_usageMemory.get();', 'after'),
    ('DisplayUtility in the HUD menu', IM, 'namespace Huginn::UI',
     '    static float Probe(float s) { return Huginn::Core::DisplayUtility(s); }', 'after'),
    ('g_legacyScorer in a display backend', 'src/display/WheelerBackend.cpp', '#include',
     'static bool Probe() { return g_legacyScorer != nullptr; }', 'after'),
    # --- PRUNED symbols coming back ---
    ('the potion tier preference back', US, 'namespace Huginn::Scoring',
     '    void ApplyPotionTierPreference();', 'after'),
    ('a Suppress branch back in the live scorer', US, 'const auto& p = scored.choice;',
     '            if (m_config.favoritesMode == FavoritesMode::Suppress) continue;', 'after'),
    ('the potion multiplier\'s own timer back', 'src/learning/PotionDiscriminator.h', '#pragma once',
     'inline float m_combatDuration = 0.0f;', 'after'),
    ('the elemental window back', 'src/pipeline/PipelineCoordinator.h', '#pragma once',
     'inline bool m_elementalWindowActive = false;', 'after'),
]
INI_PROBE = ('a pruned INI key back', 'configs/Huginn.ini', 'sPotionTierPreference = Lowest')


def mutate(text, anchor, line, mode):
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        if anchor in ln:
            if mode == 'after':
                lines.insert(i + 1, line)
            else:
                lines[i] = line
            return '\n'.join(lines) + '\n'
    raise AssertionError(f'anchor not found: {anchor!r}')


def run(tree, ini):
    sink = io.StringIO()
    bad, paths = rc.check(tree, ini, out=lambda s: sink.write(s + '\n'))
    return bad, paths, sink.getvalue()


def main():
    tree, ini = rc.load_tree(), rc.load_ini()
    failures = 0
    bad, _, report = run(tree, ini)
    if bad:
        print('FAIL baseline: the unmutated tree does not pass\n' + report)
        return 1
    print('ok   baseline: the unmutated tree passes')
    for name, path, anchor, line, mode in PROBES:
        if path not in tree:
            print(f'FAIL {name}: {path} is not in the scanned tree')
            failures += 1
            continue
        t2 = dict(tree)
        t2[path] = mutate(tree[path], anchor, line, mode)
        bad, paths, report = run(t2, ini)
        if bad and path in paths:
            print(f'ok   {name}: caught in {path}')
        else:
            print(f'FAIL {name}: r8_check passed it ({path})\n{report}')
            failures += 1
    name, path, line = INI_PROBE
    i2 = dict(ini)
    i2[path] = (ini.get(path) or '') + '\n' + line + '\n'
    bad, paths, _ = run(tree, i2)
    if bad and path in paths:
        print(f'ok   {name}: caught in {path}')
    else:
        print(f'FAIL {name}: r8_check passed it')
        failures += 1
    total = len(PROBES) + 1
    print(f'{total - failures}/{total} probes caught' + ('' if not failures else f'; {failures} FAILED'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
