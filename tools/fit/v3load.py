"""Lean loader for Huginn_Selections_v3.jsonl -> compact pickle (v3.pkl in the work
directory, common.py). The first step of every other script here.

usage: python v3load.py <v3 file> [<v3 file> ...] [--char <id>]
       python v3load.py <v3 file> [...] --list

The log rotates: give the rotated file(s) and the current one, oldest first
(Huginn_Selections_v3-20261015-180003-1.jsonl Huginn_Selections_v3.jsonl).
Each file is read on its own (a file starts with a head; a launch cut by the
rotation starts again with a head of the same launch in the next file), and a
torn line -- the last one of a copy taken while the game writes -- is skipped
and counted (stats: bad, before_head, bad_ctx, bad_dec, other_launch -- a
decision of another launch than the head in force, behind a torn head --
and dup_ctx: a context id the launch already has, which behind a torn head
is the next launch's; the first one read is kept).

--char <id>  keeps one character's decisions (dec.char, 16 hex digits) and the
             contexts of that character's launches. The others are dropped
             before v3.pkl is written, so every later script sees only them.
--list       prints the characters and the launches in the files, with their
             decision counts, and writes nothing. Run it first to find the id.

Columns/needs are mapped by NAME to a global index (union over heads), so the
0.23.16 (+4 cols), 0.23.19 (+deep_water_ahead), 0.23.35 (+combat_regen_ok)
schema additions -- and any need a later build adds -- are handled: a need a
head does not name reads 0 in its contexts. Caps are deduped globally by
content. Record fields this loader does not know are kept as written
(contexts, decisions) or ignored (heads); decision fields an older build did
not write take the defaults in DEC_DEFAULTS.

0.23.46 added two row columns, read by the head's `row` names and None for
older rows: FB (the candidate filter that dropped a row that is not eligible,
by name) and CR (the remaining seconds behind the `covered` cross-feature,
-1 = no timer); the `covered` cross-feature itself (mapped by name like the
others) and the context's `summon` object (None before 0.23.46).

0.23.51 cut an open need episode into stretches on a 60 s lap grid
(core/NeedEpisodes.h "lap"): a nothing record's `ep` gained `why` ("end": the
episode expired, "lap": the stretch reached the grid with the need still on),
`lap` (the stretch's index) and `epSec` (the episode's onset to the stretch's
end). An older record is a whole episode and reads why "end", lap 0, epSec =
durSec (EP_DEFAULTS); the heads keep `episode.lapSec` (absent before).

0.23.53 logged the old engine's score parts on every held or eligible row and
the favourites: row columns est, a, n, ucb, rec, prior, cw, fl, wu, hk (by the
head's `row` names; a row may end before the last of them, a missing one reads
None), kept as the row tuple's OLD element -- a tuple indexed by O_EST..O_HK
(common.py), fl by floor name from the head's `floors` -- or None when the row
carries none of them (every row before 0.23.53); the `favorite` flag bit; the
context's `old` object (the run's scorer parameters, kept as written; None
before); and the decision's `rid` (the id its v2 record carries; None when it
has none, and before 0.23.53). main() also checks each row's floor `fl`
against the context's `old` floors (floor_check) and prints the violations: the
floors are derived by hand from UtilityScorer::ScoreCandidates' tests, and a
real log is where the two would be seen to drift apart."""
import argparse
import json
import pickle
from collections import Counter, defaultdict
from pathlib import Path

from common import F, FLAGS, FORM, O_CW, O_FL, O_WU, OLD, work

# Decision fields the fit scripts read, with the value a record that lacks
# one gets (an older build, or a field a later one drops).
DEC_DEFAULTS = {'char': None, 'list': '', 'gen': 1, 'name': None, 'form': None, 'kind': '', 'src': '',
                'via': '', 'case': '', 'how': '', 'skip': '', 'confirmMs': 0, 'ctxAgeMs': 0, 'repeat': 0,
                'learned': 1, 'preEquipped': 0, 'ep': None, 'rid': None}
# The row columns 0.23.53 appended, in the order of the row tuple's OLD element.
OLD_COLS = ('est', 'a', 'n', 'ucb', 'rec', 'prior', 'cw', 'fl', 'wu', 'hk')
# 0.23.56 (R8.2): the new model in shadow, by name (common.SHADOW, S_MU..S_EX):
# mu, sd, lm (ln learn mult), um (util missing), ex ((need id, column id) by
# the loader's global indices, or "learned"). None on older rows.
SHADOW_COLS = ('mu', 'sd', 'lm', 'um', 'ex')
# A nothing record's episode fields 0.23.51 added (see the module docstring);
# epSec defaults to the record's own durSec.
EP_DEFAULTS = {'why': 'end', 'lap': 0}
# Context fields the loader rebuilds; every other field of a ctx record is kept as written.
CTX_REBUILT = ('t', 'id', 'rows', 'need', 'in')
OUTCOMES = ('key', 'wheel', 'menu', 'nothing')


def load(paths):
    """Read the v3 file(s), in the order given, into one dict (see the module docstring)."""
    if isinstance(paths, (str, Path)):
        paths = [paths]
    cols, needs, cross = [], [], []
    colix, needix, crossix = {}, {}, {}
    capstore, capkey = [], {}          # global cap id -> tuple((col,val)...)
    heads, ctxs, decs = [], {}, []
    stats = Counter()

    def gix(names, table, lst):
        out = []
        for n in names:
            if n not in table:
                table[n] = len(lst); lst.append(n)
            out.append(table[n])
        return out

    for path in paths:
        head = None                    # every file starts with a head, a rotated one too
        with open(path, 'rb') as fh:
            lines = fh.readlines()
        for raw in lines:
            try:
                r = json.loads(raw)
            except ValueError:
                stats['bad'] += 1; continue
            if not isinstance(r, dict):
                stats['bad'] += 1; continue
            t = r.get('t')
            if t != 'head' and head is None:
                stats['before_head'] += 1; continue
            if t == 'head':
                head = r
                hcol = gix(r['cols'], colix, cols)
                hneed = gix(r['needs'], needix, needs)
                hcross = gix(r['cross'], crossix, cross)
                segcaps = {}
                seg = len(heads)
                heads.append({k: r[k] for k in ('launch', 'list', 'build', 'kinds', 'src', 'row', 'flags', 'episode', 'filters', 'floors',
                                                'bootstrap') if k in r}
                             | {'ncols': len(r['cols']), 'nneeds': len(r['needs']), 'file': str(path)})
                kinds, srcs = r['kinds'], r['src']
                hfilters = r.get('filters', [])
                rnames = r.get('row') or []
                ifb = rnames.index('fb') if 'fb' in rnames else None
                icr = rnames.index('cr') if 'cr' in rnames else None
                hfloors = r.get('floors', [])
                iold = [rnames.index(n) if n in rnames else None for n in OLD_COLS]   # 0.23.53
                ifl = OLD_COLS.index('fl')
                ishadow = [rnames.index(n) if n in rnames else None for n in SHADOW_COLS]   # 0.23.56
                iex = SHADOW_COLS.index('ex')

                def row_tuple(f):
                    form, uid, kind, src, cap, flags, slot, util, x, wp = f[:10]
                    fb = f[ifb] if ifb is not None and ifb < len(f) else None
                    cr = f[icr] if icr is not None and icr < len(f) else None
                    old = [f[i] if i is not None and i < len(f) else None for i in iold]
                    if all(v is None for v in old):
                        old = None
                    else:
                        if old[ifl] is not None:
                            old[ifl] = hfloors[old[ifl]]
                        old = tuple(old)
                    sh = [f[i] if i is not None and i < len(f) else None for i in ishadow]
                    if all(v is None for v in sh):
                        sh = None
                    else:
                        ex = sh[iex]
                        if isinstance(ex, list) and len(ex) == 2:
                            sh[iex] = (hneed[ex[0]], hcol[ex[1]])
                        elif ex != 'learned':
                            sh[iex] = None
                        sh = tuple(sh)
                    return (form, uid, None if kind is None else kinds[kind], None if src is None else srcs[src],
                            -1 if cap == -1 else segcaps[cap], flags, slot, util,
                            tuple((hcross[i], v) for i, v in x), wp,
                            None if fb is None else hfilters[fb], cr, old, sh)
            elif t == 'cap':
                c = tuple(sorted((hcol[i], v) for i, v in r['c']))
                if c not in capkey:
                    capkey[c] = len(capstore); capstore.append(c)
                segcaps[r['id']] = capkey[c]
            elif t == 'ctx':
                try:
                    rows = [row_tuple(f) for f in r['rows']]
                except (KeyError, IndexError, TypeError, ValueError):
                    stats['bad_ctx'] += 1; continue      # a cap a torn line took, a torn row
                ctx = {k: v for k, v in r.items() if k not in CTX_REBUILT}
                for k in ('utc', 'why', 'menu', 'pipe', 'race', 'heldFull', 'wc', 'summon', 'env', 'old'):
                    ctx.setdefault(k, None)
                ctx.update({'seg': seg, 'id': r['id'], 'rows': rows,
                            'need': {hneed[i]: v for i, v in r.get('need', [])},
                            'in': {hneed[i]: v for i, v in r.get('in', [])}})
                key = (head['launch'], r['id'])
                if key in ctxs:
                    stats['dup_ctx'] += 1; continue      # behind a torn head: another launch's ids
                ctxs[key] = ctx
            elif t == 'dec':
                if r.get('launch', head['launch']) != head['launch']:
                    stats['other_launch'] += 1; continue      # behind a torn head of another launch
                d = {k: v for k, v in r.items() if k not in ('t', 'add')}
                for k, v in DEC_DEFAULTS.items():
                    d.setdefault(k, v)
                d['launch'] = head['launch']
                d['seg'] = seg
                d['build'] = head['build']
                try:
                    d['add'] = [row_tuple(f) for f in r.get('add', [])]
                except (KeyError, IndexError, TypeError, ValueError):
                    stats['bad_dec'] += 1; continue
                d['ctxkey'] = (head['launch'], r['ctx']) if r.get('ctx') is not None else None
                ep = d['ep']
                if ep:
                    for k, v in EP_DEFAULTS.items():
                        ep.setdefault(k, v)
                    ep.setdefault('epSec', ep.get('durSec'))
                    i = ep.get('i')
                    ep['gi'] = hneed[i] if isinstance(i, int) and 0 <= i < len(hneed) else needix.get(ep.get('need'))
                d['open'] = [hneed[i] for i in r.get('open', []) if 0 <= i < len(hneed)]
                decs.append(d)
            else:
                stats['unknown_type'] += 1; continue
            stats[t] += 1
    return dict(cols=cols, needs=needs, cross=cross, caps=capstore, heads=heads, ctxs=ctxs, decs=decs, stats=stats,
                source=str(Path(paths[-1]).resolve()), sources=[str(Path(p).resolve()) for p in paths])


def keep_char(D, char):
    """D with one character's decisions, and the contexts of that character's
    launches only (a context names no character; a launch plays one)."""
    decs = [d for d in D['decs'] if d['char'] == char]
    launches = {d['launch'] for d in decs}
    out = dict(D)
    out['decs'] = decs
    out['ctxs'] = {k: v for k, v in D['ctxs'].items() if k[0] in launches}
    out['char'] = char
    return out


def listing(D):
    """The characters, then the launches, with their decision counts by outcome (lines of text);
    "of them laps" counts the nothing records that are lapped stretches (0.23.51)."""
    by_char, by_launch = defaultdict(Counter), defaultdict(Counter)
    laps_char, laps_launch = Counter(), Counter()
    lists, builds = defaultdict(set), defaultdict(set)
    for d in D['decs']:
        c, l = str(d['char']), (d['launch'], str(d['char']))
        by_char[c][d['out']] += 1
        by_launch[l][d['out']] += 1
        if is_lap(d):
            laps_char[c] += 1
            laps_launch[l] += 1
        lists[c].add(d['list']); lists[l].add(d['list'])
        builds[l].add(d['build'])
    nlaunch = Counter(c for _, c in by_launch)

    def counts(c, laps):
        return ('  '.join(f'{o} {c[o]:5}' for o in OUTCOMES) + f' (of them laps {laps:5})'
                + f'  total {sum(c.values()):6}')

    lines = [f'{len(D["decs"])} decisions, {len(by_launch)} launches, {len(by_char)} character(s)', '', 'characters:']
    for c in sorted(by_char, key=lambda c: -sum(by_char[c].values())):
        lines.append(f'  {c}  launches {nlaunch[c]:4}  {counts(by_char[c], laps_char[c])}  lists {", ".join(sorted(lists[c]))}')
    lines += ['', 'launches (launch, character, list, build):']
    for l in sorted(by_launch):
        lines.append(f'  {l[0]}  {l[1]}  {", ".join(sorted(lists[l])):20} {", ".join(sorted(builds[l])):22} '
                     f'{counts(by_launch[l], laps_launch[l])}')
    return lines


# Relative slack for comparing two logged numbers: the encoder writes 4
# significant digits, so each side may be off by half a unit in the 4th.
FLOOR_TOL = 1e-3


def floor_check(D):
    """Check each row's old-engine floor (`fl`, 0.23.53) against the tests
    UtilityScorer::ScoreCandidates applies, with the context's `old` floors.
    PipelineCoordinator::CaptureEligible derives `fl` by copying those tests by hand;
    this catches the two drifting apart, on a real log. Returns (Counter of violations
    by kind, the rows checked, up to 5 examples per kind as (ctx key, row index, form)).
    Kinds:
      not_eligible    fl set on a row that is not eligible
      scored_mismatch fl pass/cold on an unscored row, or ctx/util/suppressed on a scored one
      ctx_above_min   fl ctx with cw > minCtx (the context floor would not have dropped it)
      util_above_min  fl util with wu > minUtil (the utility floor would not have dropped it)
      util_below_ctx  fl util on a non-favourite with cw < minCtx (that is ctx)
      pass_below_ctx  fl pass on a non-favourite with cw < minCtx (only the cold start keeps it)
      not_suppress    fl suppressed when the run's favourites mode is not suppress
    A comparison counts only beyond the 4-digit rounding of both sides (FLOOR_TOL), so a
    value logged equal to its floor is not judged. "Favourite" is the row's `favorite`
    bit, which every candidate the old engine read as favourited carries; a bit set by
    the held scan alone only skips a check."""
    def above(a, b):             # a > b beyond the rounding of either
        return a - b > FLOOR_TOL * max(abs(a), abs(b), 1e-6)

    bad, examples, checked = Counter(), defaultdict(list), 0
    for key, c in D['ctxs'].items():
        o = c.get('old')
        if not o:
            continue
        min_ctx, min_util = o.get('minCtx'), o.get('minUtil')
        for i, r in enumerate(c['rows']):
            old = r[OLD]
            if old is None or old[O_FL] is None:
                continue
            checked += 1
            fl, cw, wu, flags = old[O_FL], old[O_CW], old[O_WU], r[FLAGS]
            fav = flags & F['favorite']
            kinds = []
            if not flags & F['eligible']:
                kinds.append('not_eligible')
            if bool(flags & F['scored']) != (fl in ('pass', 'cold')):
                kinds.append('scored_mismatch')
            if fl == 'ctx' and None not in (cw, min_ctx) and above(cw, min_ctx):
                kinds.append('ctx_above_min')
            if fl == 'util' and None not in (wu, min_util) and above(wu, min_util):
                kinds.append('util_above_min')
            if fl in ('util', 'pass') and not fav and None not in (cw, min_ctx) and above(min_ctx, cw):
                kinds.append(f'{fl}_below_ctx')
            if fl == 'suppressed' and o.get('fav') not in (None, 'suppress'):
                kinds.append('not_suppress')
            for k in kinds:
                bad[k] += 1
                if len(examples[k]) < 5:
                    examples[k].append((key, i, r[FORM]))
    return bad, checked, dict(examples)


def is_lap(d):
    """A nothing record that stands for a lapped stretch of a still-open episode (0.23.51)."""
    return d['out'] == 'nothing' and bool(d.get('ep')) and d['ep'].get('why') == 'lap'


def main(argv=None):
    ap = argparse.ArgumentParser(description='Read selection log v3 file(s) into v3.pkl in the work directory.')
    ap.add_argument('paths', nargs='+', help='v3 file(s), oldest first: the rotated file(s), then the current one')
    ap.add_argument('--char', help='keep one character (dec.char, 16 hex digits; --list shows them)')
    ap.add_argument('--list', action='store_true', help='print the characters and launches with their decision counts; write nothing')
    args = ap.parse_args(argv)
    D = load(args.paths)
    if args.list:
        print('\n'.join(listing(D)))
        return D
    if args.char:
        n = len(D['decs'])
        D = keep_char(D, args.char)
        if not D['decs']:
            raise SystemExit(f'no decisions of character {args.char} (--list shows the characters)')
        print(f'--char {args.char}: kept {len(D["decs"])} of {n} decisions, '
              f'{len({d["launch"] for d in D["decs"]})} launches, {len(D["ctxs"])} contexts')
    print(dict(D['stats']), len(D['cols']), 'cols', len(D['needs']), 'needs', len(D['cross']), 'cross',
          len(D['caps']), 'caps', len(D['ctxs']), 'ctxs')
    bad, checked, examples = floor_check(D)
    if checked:                  # 0.23.53 rows: the old floors as the scorer applies them
        print(f'old-engine floors: {checked} rows checked, '
              + (f'VIOLATIONS {dict(bad)} e.g. {examples}' if bad else 'no violations'))
    with open(work('v3.pkl'), 'wb') as f:
        pickle.dump(D, f, protocol=5)
    print('wrote', work('v3.pkl'))
    return D


if __name__ == '__main__':
    main()
