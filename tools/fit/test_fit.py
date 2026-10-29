"""Tests for the R6 fit tools (tools/fit): the loader's multi-file read, character
filter and listing, tolerance of need ids and record fields it does not know (and
of older records without them), the build segmentation, the grouped folds by
launch, the A* page temperature and the yardstick's pooled metric.

The synthetic logs come from the encoder in tools/replay/test_replay_v3.py
(written from the schema doc, separate from the C++ one).

Run:  python tools/fit/test_fit.py     (exit 0 = pass; needs numpy)
"""
import contextlib
import importlib.util
import io
import json
import math
import os
import pickle
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))      # the fit scripts import their siblings by name

import common   # noqa: E402
import fitlib   # noqa: E402
import v3load   # noqa: E402


def _load_by_path(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


trv3 = _load_by_path('test_replay_v3', HERE.parent / 'replay' / 'test_replay_v3.py')
replay = trv3.replay

CHAR_A, CHAR_B = 'FEFD1B039AC78799', '3F3E2A8817962D59'


def session(launch, char, build, minute=1, new_needs=()):
    """One launch of the replay test's synthetic session (two segments, four decisions:
    key, nothing, menu, wheel), relabelled; new_needs are appended to the head's needs
    (with a curve) and set in the first context, as a later build would log them."""
    head, seg1, seg2 = trv3.make_session()
    head = dict(head, launch=launch, build=build)
    if new_needs:
        head['needs'] = head['needs'] + list(new_needs)
        if 'curves' in head:
            head['curves'] = head['curves'] + [['logistic', 1.5, -3]] * len(new_needs)
        for n in new_needs:
            seg1[0]['ctx']['need'][n] = 0.7
            seg1[0]['ctx']['input'][n] = 1.2
    for d in seg1 + seg2:
        d['launch'], d['char'] = launch, char
        d['utc'] = d['utc'].replace('15:01:', f'15:{minute:02d}:')
    return trv3.encode([(head, seg1), (head, seg2)])


def edit_lines(text, fn):
    """text with fn(record) applied to every JSON line (fn edits in place)."""
    out = []
    for line in text.splitlines():
        r = json.loads(line)
        fn(r)
        out.append(json.dumps(r))
    return '\n'.join(out) + '\n'


class TempWork(unittest.TestCase):
    """A temporary directory, also the fit work directory (HUGINN_FIT_WORK)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self._env = os.environ.get('HUGINN_FIT_WORK')
        os.environ['HUGINN_FIT_WORK'] = str(self.dir)

    def tearDown(self):
        if self._env is None:
            os.environ.pop('HUGINN_FIT_WORK', None)
        else:
            os.environ['HUGINN_FIT_WORK'] = self._env
        self._tmp.cleanup()

    def write(self, name, text, binary=False):
        p = self.dir / name
        if binary:
            p.write_bytes(text)
        else:
            p.write_text(text, encoding='utf-8')
        return p

    def quiet(self, fn, *a, **kw):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            r = fn(*a, **kw)
        return r, buf.getvalue()


class Loader(TempWork):
    def files(self):
        """A rotated file (launch 1 of A, launch 2 of B, the first segment of launch 3 of A
        and a torn last line) and the current one (launch 3 again from its head)."""
        l3 = session('20261015-180003', CHAR_A, '0.23.40 (f7a02d2)', minute=3)
        l3_lines = l3.splitlines(keepends=True)
        cut = next(i for i, ln in enumerate(l3_lines) if i > 0 and json.loads(ln)['t'] == 'head')
        rotated = (session('20261010-100000', CHAR_A, '0.23.38 (c265bab)', minute=1)
                   + session('20261011-100000', CHAR_B, '0.23.38 (c265bab)', minute=2)
                   + ''.join(l3_lines[:cut]))
        f1 = self.write('Huginn_Selections_v3-20261015-180003-1.jsonl',
                        rotated.encode('utf-8') + b'{"t":"dec","v":3,"seq":9', binary=True)
        f2 = self.write('Huginn_Selections_v3.jsonl', ''.join(l3_lines[cut:]))
        return f1, f2

    def test_reads_every_file_in_order(self):
        f1, f2 = self.files()
        D = v3load.load([f1, f2])
        self.assertEqual(len(D['decs']), 12)
        self.assertEqual(D['stats']['bad'], 1)                   # the torn last line of the copy
        self.assertEqual(sorted({d['launch'] for d in D['decs']}),
                         ['20261010-100000', '20261011-100000', '20261015-180003'])
        l3 = [d for d in D['decs'] if d['launch'] == '20261015-180003']
        self.assertEqual([d['out'] for d in l3], ['key', 'nothing', 'menu', 'wheel'])   # across the seam
        self.assertEqual([d['build'] for d in l3], ['0.23.40 (f7a02d2)'] * 4)
        self.assertEqual(D['source'], str(f2.resolve()))         # the v2 log is looked for beside it
        self.assertEqual(len(D['sources']), 2)
        self.assertEqual(len(D['ctxs']), 6)                       # two contexts per launch
        self.assertEqual(v3load.load(f2)['decs'][0]['out'], 'wheel')   # one path still works

    def test_torn_head_keeps_the_earlier_launch_contexts(self):
        # a crash mid-line, then the next launch's head appended to the torn line
        la = session('20261010-100000', CHAR_A, '0.23.38 (c265bab)', minute=1)
        lb = session('20261011-100000', CHAR_B, '0.23.40 (f7a02d2)', minute=2)
        lb = edit_lines(lb, lambda r: r.update(utc='2026-10-11 09:09:09.000') if r['t'] == 'ctx' else None)
        clean = v3load.load([self.write('a.jsonl', la)])
        D = v3load.load([self.write('torn.jsonl', la + '{"t":"ctx","v":3,"id":99,"rows":[[1,' + lb)])
        self.assertGreater(D['stats']['other_launch'], 0)
        self.assertGreater(D['stats']['dup_ctx'], 0)
        for k, c in clean['ctxs'].items():
            self.assertEqual(D['ctxs'][k]['utc'], c['utc'])
        self.assertEqual(len(v3load.keep_char(D, CHAR_A)['decs']), len(clean['decs']))

    def test_char_keeps_one_character(self):
        f1, f2 = self.files()
        D = v3load.keep_char(v3load.load([f1, f2]), CHAR_A)
        self.assertEqual(len(D['decs']), 8)
        self.assertTrue(all(d['char'] == CHAR_A for d in D['decs']))
        self.assertEqual({k[0] for k in D['ctxs']}, {'20261010-100000', '20261015-180003'})
        self.assertTrue(all(d['ctxkey'] in D['ctxs'] for d in D['decs']))

    def test_main_char_writes_only_that_character(self):
        f1, f2 = self.files()
        _, out = self.quiet(v3load.main, [str(f1), str(f2), '--char', CHAR_B])
        self.assertIn('kept 4 of 12 decisions, 1 launches', out)
        with open(self.dir / 'v3.pkl', 'rb') as f:   # written by this test just above
            D = pickle.load(f)
        self.assertEqual({d['char'] for d in D['decs']}, {CHAR_B})
        with self.assertRaises(SystemExit):
            self.quiet(v3load.main, [str(f1), str(f2), '--char', '0000000000000000'])

    def test_list_prints_characters_and_launches_and_writes_nothing(self):
        f1, f2 = self.files()
        _, out = self.quiet(v3load.main, [str(f1), str(f2), '--list'])
        self.assertFalse((self.dir / 'v3.pkl').exists())
        self.assertIn('12 decisions, 3 launches, 2 character(s)', out)
        line_a = next(ln for ln in out.splitlines() if ln.strip().startswith(CHAR_A))
        self.assertIn('launches    2', line_a)
        self.assertIn('total      8', line_a)
        self.assertIn('menu     2', line_a)
        launch_lines = [ln for ln in out.splitlines() if ln.strip().startswith('2026101')]
        self.assertEqual(len(launch_lines), 3)
        self.assertIn('0.23.40 (f7a02d2)', next(ln for ln in launch_lines if '20261015-180003' in ln))


class NewAndUnknownKeys(TempWork):
    """A later build adds need ids (placeholders: since 0.23.50 the synthetic session itself
    carries time_to_edge and edge_approach) and record fields; an older build's records lack
    fields the loader reads. Both load."""

    def text(self):
        old = session('20261010-100000', CHAR_A, '0.23.38 (c265bab)', minute=1)

        def drop_fields(r):          # an older writer: no char/gen/open/ep/preEquipped
            if r['t'] == 'dec':
                for k in ('char', 'gen', 'open', 'ep', 'preEquipped', 'confirmMs'):
                    r.pop(k, None)
        old = edit_lines(old, drop_fields)
        new = session('20261029-100000', CHAR_A, '0.23.50 (abc1234)', minute=2,
                      new_needs=('future_need_a', 'future_need_b'))

        def add_fields(r):           # fields no build writes yet
            if r['t'] == 'ctx':
                r['edge'] = {'dist': 175, 'speed': 300.0}
            elif r['t'] == 'dec':
                r['futureField'] = [1, 2]
            elif r['t'] == 'head':
                r['futureHead'] = {'x': 1}
        return old + edit_lines(new, add_fields)

    def test_v3load(self):
        D = v3load.load([self.write('v3.jsonl', self.text())])
        self.assertEqual(len(D['decs']), 8)
        self.assertEqual(D['needs'][-2:], ['future_need_a', 'future_need_b'])
        k = D['needs'].index('future_need_a')
        new_ctx = D['ctxs'][('20261029-100000', 10)]
        old_ctx = D['ctxs'][('20261010-100000', 10)]
        self.assertEqual(new_ctx['need'][k], 0.7)
        self.assertNotIn(k, old_ctx['need'])                    # reads 0 in the fit
        self.assertEqual(new_ctx['edge'], {'dist': 175, 'speed': 300.0})   # kept as written
        old = [d for d in D['decs'] if d['launch'] == '20261010-100000']
        self.assertEqual({(d['char'], d['gen'], d['ep'], d['preEquipped']) for d in old}, {(None, 1, None, 0)})
        self.assertEqual(old[0]['open'], [])
        self.assertEqual([d['futureField'] for d in D['decs'] if d['launch'] == '20261029-100000'], [[1, 2]] * 4)

    def test_build_reads_new_needs_as_columns(self):
        self.quiet(v3load.main, [str(self.write('v3.jsonl', self.text()))])
        S = fitlib.build(common.load(), include_nothing=True)
        k = S['needs'].index('future_need_a')
        new = np.array([m['launch'] == '20261029-100000' for m in S['meta']])
        self.assertTrue((S['N'][~new, k] == 0).all())
        self.assertTrue((S['N'][new, k] > 0).any())

    def test_replay_reader(self):
        decs = replay.load_v3([self.write('v3.jsonl', self.text())])
        self.assertEqual(len(decs), 8)
        self.assertEqual(decs[4]['ctx']['need']['future_need_a'], 0.7)
        self.assertNotIn('future_need_a', decs[0]['ctx']['need'])
        self.assertNotIn('char', decs[0])


def lap_session(launch, char, build):
    """The synthetic session plus the lapped stretches (0.23.51) of two lap ticks, none
    answered. Tick 1 (15:02:30, judged 15:02:34) cuts three stretches: hunger and diseased,
    begun by the previous lap (ep.lap 1, the lap context 12), and thirst's first stretch,
    begun at its onset (ep.lap 0, the onset context 13) -- one tick, two contexts. Tick 2
    (15:03:30) cuts hunger's next stretch (context 14). The records of one tick share one
    utc, as SelectionLogV3 writes them. The session's own nothing (darkness) is written as
    a 0.23.50 build wrote it: no why / lap / epSec."""
    head, seg1, seg2 = trv3.make_session()
    head = dict(head, launch=launch, build=build)

    def ctx(i, utc, why, need):
        return {'id': i, 'utc': utc, 'why': why, 'need': need, 'input': dict(need),
                'pipe': {'ok': 1, 'page': 0, 'slots': 8, 'ageMs': 50}, 'race': None,
                'wc': {'base': 0.165, 'max': 0.5}, 'rows': []}

    ctx_l = ctx(12, '2026-10-09 15:01:30.000', 'lap', {'hunger': 0.8, 'diseased': 1})
    ctx_o = ctx(13, '2026-10-09 15:01:10.000', 'onset', {'hunger': 0.8, 'diseased': 1, 'thirst': 0.6})
    ctx_l2 = ctx(14, '2026-10-09 15:02:30.000', 'lap', {'hunger': 0.8, 'diseased': 1, 'thirst': 0.6})
    laps = []
    for seq, need, i, c, lap, dur, ep_sec, utc in (
            (5, 'thirst', 27, ctx_o, 0, 80.0, 80.0, '15:02:34'),
            (6, 'hunger', 26, ctx_l, 1, 60.0, 90.0, '15:02:34'),
            (7, 'diseased', 18, ctx_l, 1, 60.0, 90.0, '15:02:34'),
            (8, 'hunger', 26, ctx_l2, 2, 60.0, 150.0, '15:03:34')):
        d = dict(seg1[1], seq=seq, utc=f'2026-10-09 {utc}.000', ctx=c, ctxAgeMs=64000,
                 ep={'need': need, 'i': i, 'durSec': dur, 'peak': 1, 'onset': '2026-10-09 15:00:00.000',
                     'why': 'lap', 'lap': lap, 'epSec': ep_sec})
        laps.append(d)
    for d in seg1 + seg2 + laps:
        d['launch'], d['char'] = launch, char
    return trv3.encode([(head, seg1), (head, seg2 + laps)])


class Laps(TempWork):
    """0.23.51: the lapped-stretch nothing records, read by v3load (old records default to a
    whole episode), listed, and kept once per lap tick, all, or none by fitlib.build."""

    def load(self):
        self.quiet(v3load.main, [str(self.write('v3.jsonl', lap_session('20261030-100000', CHAR_A, '0.23.51 (abc1234)')))])
        return common.load()

    def test_v3load_defaults_and_reads_laps(self):
        D = self.load()
        noth = [d for d in D['decs'] if d['out'] == 'nothing']
        self.assertEqual(len(noth), 5)
        old = next(d for d in noth if d['ep']['need'] == 'darkness')
        self.assertEqual((old['ep']['why'], old['ep']['lap'], old['ep']['epSec']), ('end', 0, 4.0))
        laps = [d for d in noth if d['ep']['why'] == 'lap']
        self.assertEqual(sorted(d['ep']['need'] for d in laps), ['diseased', 'hunger', 'hunger', 'thirst'])
        self.assertEqual({d['ctxkey'][1] for d in laps}, {12, 13, 14})
        self.assertEqual({D['needs'][d['ep']['gi']] for d in laps}, {'hunger', 'diseased', 'thirst'})
        self.assertEqual(len({fitlib.lap_tick(d) for d in laps}), 2)
        self.assertTrue(all(v3load.is_lap(d) for d in laps))
        self.assertFalse(v3load.is_lap(old))

    def test_listing_counts_laps(self):
        p = self.write('v3.jsonl', lap_session('20261030-100000', CHAR_A, '0.23.51 (abc1234)'))
        _, out = self.quiet(v3load.main, [str(p), '--list'])
        line = next(ln for ln in out.splitlines() if ln.strip().startswith(CHAR_A))
        self.assertIn('nothing     5 (of them laps     4)', line)
        self.assertIn('total      8', line)

    def test_build_laps_once_all_none(self):
        D = self.load()

        def whys(**kw):
            S = fitlib.build(D, include_nothing=True, **kw)
            return sorted(str(m['ep_why']) for m in S['meta']), S

        once, S = whys()
        self.assertEqual(once, ['None'] * 3 + ['end', 'lap', 'lap'])   # one per lap tick, not per context
        lap = [i for i, m in enumerate(S['meta']) if m['ep_why'] == 'lap']
        # Tick 1: the lowest-seq stretch begun by a lap (hunger, seq 6, context 12), not
        # thirst's first stretch (seq 5, its onset context 13). Tick 2: hunger, context 14.
        self.assertEqual([S['meta'][i]['ctxkey'][1] for i in lap], [12, 14])
        self.assertEqual([S['meta'][i]['need_ep'] for i in lap], [D['needs'].index('hunger')] * 2)
        self.assertEqual(S['Y'][lap[0]], -1)                                          # nothing chosen
        k = S['needs'].index('diseased')
        self.assertEqual(S['N'][lap[0], k], 1)
        self.assertEqual(whys(laps='all')[0], ['None'] * 3 + ['end'] + ['lap'] * 4)
        self.assertEqual(whys(laps='none')[0], ['None'] * 3 + ['end'])
        with self.assertRaises(ValueError):
            fitlib.build(D, laps='twice')
        # The picks group never sees them.
        S = fitlib.build(D, include_nothing=False)
        self.assertEqual({m['ep_why'] for m in S['meta']}, {None})

    def test_build_laps_once_keeps_a_lone_first_stretch(self):
        # A tick whose only stretch is an episode's first one keeps it.
        D = self.load()
        D = dict(D, decs=[d for d in D['decs'] if not (v3load.is_lap(d) and d['ep']['lap'] > 0)])
        S = fitlib.build(D, include_nothing=True)
        lap = [m for m in S['meta'] if m['ep_why'] == 'lap']
        self.assertEqual([m['ctxkey'][1] for m in lap], [13])

    def test_replay_reads_laps(self):
        decs = replay.load_v3([self.write('v3.jsonl', lap_session('20261030-100000', CHAR_A, '0.23.51 (abc1234)'))])
        self.assertEqual([replay.nothing_stretch(d) for d in decs if d['out'] == 'nothing'],
                         [('end', 0, 4.0), ('lap', 0, 80.0), ('lap', 1, 90.0), ('lap', 1, 90.0), ('lap', 2, 150.0)])


class ReplayChar(TempWork):
    def test_summarize_v3_keeps_one_character(self):
        p = self.write('v3.jsonl', session('20261010-100000', CHAR_A, '0.23.38 (x)', minute=1)
                       + session('20261011-100000', CHAR_B, '0.23.38 (x)', minute=2)
                       + session('20261012-100000', CHAR_B, '0.23.38 (x)', minute=3))
        _, every = self.quiet(replay.summarize_v3, [p])
        _, one = self.quiet(replay.summarize_v3, [p], CHAR_A)
        self.assertIn('12 decisions', every)
        self.assertIn('key=3', every)
        self.assertIn(f'4 decisions in 1 file(s)', one)
        self.assertIn(f'character {CHAR_A}: 4 of 12', one)
        self.assertIn('key=1', one)
        self.assertIn('menu=1', one)


class BuildSegments(TempWork):
    def S(self, **kw):
        text = (session('20261010-100000', CHAR_A, '0.23.38 (c265bab)', minute=1)
                + session('20261011-100000', CHAR_A, '0.23.40 (f7a02d2)', minute=2)
                + session('20261012-100000', CHAR_A, '0.23.44 (4bdfec4)', minute=3))
        self.quiet(v3load.main, [str(self.write('v3.jsonl', text))])
        return fitlib.build(common.load(), include_nothing=True, **kw)

    def test_version(self):
        self.assertEqual(fitlib.version('0.23.40 (f7a02d2)'), (0, 23, 40))
        self.assertEqual(fitlib.version('0.23.41'), (0, 23, 41))
        self.assertIsNone(fitlib.version('test'))
        self.assertIsNone(fitlib.version(None))
        self.assertEqual(fitlib.segment_names(['0.23.41']), ['<0.23.41', '>=0.23.41'])
        self.assertEqual(fitlib.segment_names(['0.23.44', '0.23.39']),
                         ['<0.23.39', '>=0.23.39,<0.23.44', '>=0.23.44'])

    def test_since_until(self):
        self.assertEqual(len(self.S()['Y']), 12)
        S = self.S(since='0.23.40')
        self.assertEqual({m['build'] for m in S['meta']}, {'0.23.40 (f7a02d2)', '0.23.44 (4bdfec4)'})
        S = self.S(since='0.23.39', until='0.23.40')
        self.assertEqual({m['build'] for m in S['meta']}, {'0.23.40 (f7a02d2)'})
        self.assertEqual(S['launches'], ['20261011-100000'])
        with self.assertRaises(ValueError):
            self.S(since='0.23.50')

    def test_split_at_and_split_needs(self):
        S = self.S(split_at=['0.23.40', '0.23.44'])
        self.assertEqual(S['segments'], ['<0.23.40', '>=0.23.40,<0.23.44', '>=0.23.44'])
        want = {'20261010-100000': 0, '20261011-100000': 1, '20261012-100000': 2}
        self.assertEqual(S['SEG'].tolist(), [want[m['launch']] for m in S['meta']])
        k = S['needs'].index('darkness')
        obv = {(k, 0): 1.0}
        S2, obv2 = fitlib.split_needs(S, obv)
        nsplit = len(fitlib.split_need_names(S))
        self.assertEqual(S2['K'], S['K'] + 2 * nsplit)
        k1 = S2['needs'].index('darkness@>=0.23.40,<0.23.44')
        k2 = S2['needs'].index('darkness@>=0.23.44')
        self.assertEqual((obv2[(k1, 0)], obv2[(k2, 0)]), (1.0, 1.0))
        # the darkness context (the nothing) reads in exactly one of the three columns
        for i, g in enumerate(S['SEG']):
            v = S['N'][i, k]
            self.assertEqual(S2['N'][i, [k, k1, k2]].tolist(), [v if g == j else 0.0 for j in range(3)])
        # a subset keeps SEG aligned
        sub = fitlib.subset(S, np.where(S['SEG'] == 2)[0])
        self.assertTrue((sub['SEG'] == 2).all())

    def test_recency(self):
        S = self.S()
        names = S['recency_names']
        self.assertIn('recent_restore@5s', names)
        j = names.index('recent_restore@5s')
        # the menu pick 2 s after the key press of the restore potion; the key press has none
        for i, m in enumerate(S['meta']):
            if m['out'] == 'menu':
                self.assertAlmostEqual(S['REC'][i, j], math.exp(-2 / 5))
            if m['out'] == 'key':
                self.assertEqual(S['REC'][i].sum(), 0.0)
        S2, _ = fitlib.with_recency(S, {})
        self.assertEqual(S2['N'].shape[1], S['K'] + len(names))


def fake_S(n_launch, seed=0):
    """Launches with random decision counts and outcomes (enough for the folds)."""
    rng = np.random.default_rng(seed)
    LA, OUT, Y = [], [], []
    for l in range(n_launch):
        for _ in range(int(rng.integers(1, 40))):
            o = rng.choice(['key', 'wheel', 'menu', 'nothing'], p=[0.3, 0.1, 0.1, 0.5])
            LA.append(l); OUT.append(o); Y.append(-1 if o == 'nothing' else 0)
    return {'launches': [f'L{l}' for l in range(n_launch)], 'LA': np.array(LA),
            'OUT': np.array(OUT, dtype=object), 'Y': np.array(Y)}


class Folds(unittest.TestCase):
    def test_each_launch_in_one_fold(self):
        S = fake_S(75)
        fold = fitlib.launch_folds(S, 10, seed=1)
        self.assertEqual(fold.shape, (75,))
        self.assertEqual(set(fold.tolist()), set(range(10)))      # every fold is used
        FOLD = fold[S['LA']]
        for l in range(75):
            self.assertEqual(len(set(FOLD[S['LA'] == l].tolist())), 1)
        # held-out sets partition the decisions
        parts = [set(np.where(FOLD == f)[0].tolist()) for f in range(10)]
        self.assertEqual(sum(map(len, parts)), len(S['LA']))
        self.assertEqual(set().union(*parts), set(range(len(S['LA']))))

    def test_deterministic(self):
        S = fake_S(60, seed=3)
        a = fitlib.launch_folds(S, 10, seed=7)
        self.assertEqual(a.tolist(), fitlib.launch_folds(S, 10, seed=7).tolist())
        self.assertEqual(a.tolist(), fitlib.launch_folds(fake_S(60, seed=3), 10, seed=7).tolist())
        seeds = {tuple(fitlib.launch_folds(S, 10, seed=s).tolist()) for s in range(5)}
        self.assertGreater(len(seeds), 1)                            # the seed matters (ties)

    def test_balanced_and_capped(self):
        S = fake_S(200, seed=5)
        fold = fitlib.launch_folds(S, 10)
        kw = np.bincount(fold[S['LA'][np.isin(S['OUT'], fitlib.KW)]], minlength=10)
        self.assertLessEqual(kw.max() - kw.min(), np.bincount(S['LA'][np.isin(S['OUT'], fitlib.KW)]).max())
        small = fake_S(4)
        self.assertEqual(sorted(fitlib.launch_folds(small, 10).tolist()), [0, 1, 2, 3])   # k = launches: LOO
        self.assertEqual(set(fitlib.launch_folds(small, 1).tolist()), {0})


class Yardstick(unittest.TestCase):
    @staticmethod
    def rec(launch, out, hit8, page_hit1=None, page_logp=None):
        r = {'launch': launch, 'out': out, 'logp': None}
        if out != 'nothing':
            r['full_hit8'] = hit8
        if page_hit1 is not None:
            r['page_hit1'] = page_hit1
            r['page_logp'] = page_logp
        return r

    def test_all_picks_pools_key_wheel_and_menu(self):
        # model: key 1/2, wheel 1/1, menu 2/2; A*: key 2/2, wheel 1/1, menu 0/2; a nothing is never counted
        model = [self.rec(0, 'key', True, True, -0.5), self.rec(0, 'key', False, False, -2.0),
                 self.rec(1, 'wheel', True, True, -0.1), self.rec(1, 'menu', True), self.rec(2, 'menu', True),
                 self.rec(2, 'nothing', None)]
        astar = [self.rec(0, 'key', True, False, -1.5), self.rec(0, 'key', True, True, -1.0),
                 self.rec(1, 'wheel', True, False, -0.7), self.rec(1, 'menu', False), self.rec(2, 'menu', False),
                 self.rec(2, 'nothing', None)]
        y = fitlib.yardstick(model, astar, nboot=500)
        np.testing.assert_allclose(y['key/wheel hit@8'][:3], (2 / 3, 1.0, -1 / 3))
        np.testing.assert_allclose(y['menu hit@8'][:3], (1.0, 0.0, 1.0))
        ma, mb, d, lo, hi, n = y['all picks hit@8']
        self.assertEqual(n, 5)
        self.assertAlmostEqual(ma, 4 / 5)
        self.assertAlmostEqual(mb, 3 / 5)
        self.assertAlmostEqual(d, 1 / 5)
        self.assertLessEqual(lo, d)
        self.assertGreaterEqual(hi, d)
        np.testing.assert_allclose(y['page hit@1'][:3], (2 / 3, 1 / 3, 1 / 3))
        self.assertAlmostEqual(y['page NLL'][2], (0.5 + 2.0 + 0.1) / 3 - (1.5 + 1.0 + 0.7) / 3)
        self.assertEqual(y, fitlib.yardstick(model, astar, nboot=500))   # deterministic
        # pooled = the pick-weighted mean of the parts
        kw, mn = y['key/wheel hit@8'], y['menu hit@8']
        self.assertAlmostEqual(ma, (kw[0] * kw[5] + mn[0] * mn[5]) / (kw[5] + mn[5]))

    def test_keep_mask_and_missing_metrics(self):
        model = [self.rec(0, 'key', True), self.rec(1, 'menu', False), {'launch': 2, 'out': 'key', 'logp': None}]
        astar = [self.rec(0, 'key', False), self.rec(1, 'menu', True), self.rec(2, 'key', True)]
        y = fitlib.yardstick(model, astar, nboot=200)
        self.assertEqual(y['all picks hit@8'][5], 2)               # the unfitted stub is skipped
        self.assertNotIn('page NLL', y)
        y = fitlib.yardstick(model, astar, nboot=200, keep=np.array([True, False, True]))
        self.assertEqual(list(y), ['key/wheel hit@8', 'all picks hit@8'])
        self.assertEqual(y['all picks hit@8'][:3], (1.0, 0.0, 1.0))

    def test_summarize_pools_by_launch(self):
        res = [self.rec(0, 'key', True), self.rec(0, 'menu', False), self.rec(1, 'wheel', False),
               self.rec(1, 'key', True), self.rec(2, 'menu', True), self.rec(2, 'nothing', None)]
        s = fitlib.summarize(res, nboot=300)
        self.assertEqual(s['key_full_hit8'][3], 3)
        self.assertAlmostEqual(s['key_full_hit8'][0], 2 / 3)
        self.assertAlmostEqual(s['menu_full_hit8'][0], 1 / 2)
        self.assertNotIn('pick_nll', s)                              # no logp

    def test_boot_mean(self):
        self.assertIsNone(fitlib.boot_mean({}))
        m, lo, hi, n = fitlib.boot_mean({0: [1.0, 1.0], 1: [0.0], 2: [1.0, 0.0, 1.0]}, nboot=1000)
        self.assertEqual(n, 6)
        self.assertAlmostEqual(m, 4 / 6)
        self.assertTrue(0.0 <= lo <= m <= hi <= 1.0)
        self.assertEqual(fitlib.boot_mean({0: [2.0], 1: [2.0]}, nboot=50)[:3], (2.0, 2.0, 2.0))


class PageTemperature(unittest.TestCase):
    def test_recovers_beta(self):
        rng = np.random.default_rng(0)
        n, R, beta = 4000, 8, 2.5
        z = rng.normal(size=(n, R))
        p = np.exp(beta * z); p /= p.sum(axis=1, keepdims=True)
        y = np.array([rng.choice(R, p=row) for row in p])
        S = {'OUT': np.array(['key'] * n, dtype=object), 'Y': y, 'MASK': np.ones((n, R), dtype=bool),
             'INHA': np.zeros((n, R))}
        b = fitlib.page_temperature(S, z, np.arange(n))
        self.assertAlmostEqual(b, beta, delta=0.15)
        S['OUT'][:] = 'menu'                                       # no key/wheel pages: the default
        self.assertEqual(fitlib.page_temperature(S, z, np.arange(n)), 1.0)


# ---------------------------------------------------------------------------
# honest yardstick, curve fit, bootstrap export (0.23.51)
# ---------------------------------------------------------------------------
import bootstrap   # noqa: E402
import curve       # noqa: E402
import export      # noqa: E402
import honest      # noqa: E402

REPO = HERE.parents[1]
GOLDEN_DIR = REPO / 'tests' / 'core' / 'fixtures' / 'bootstrap'
SYN_NEEDS = ['encumbrance', 'hunger', 'health_deficit']
SYN_COLS = ['utility_carry_weight', 'kind_food', 'restore_health']
SYN_OBV = {(0, 0): 1.0, (1, 1): 1.0, (2, 2): 1.0}
# item caps (rows of CapMat): Feather (carry only), Bread (food + a little carry), Potion
# (restore), Sword (nothing), Apple (food), Elixir (restore)
SYN_CAPS = np.array([[1.0, 0, 0], [0.2, 0.5, 0], [0, 0, 0.8], [0, 0, 0], [0, 0.3, 0], [0, 0, 0.4]])
SYN_B = np.array([-0.5, 0.2, 0.0, 0.3, -0.2, 0.1])


def synth_S(n=1500, seed=0, c_true=0.85, s_true=30.0, per_launch=25, w_carry=3.0, nothing=0.0, rec=False):
    """A dataset in build()'s shape whose choices follow doc 9's model with a known encumbrance
    curve: s_i = w_carry * logistic(r; c_true, s_true) * carry_i + 2 hunger food_i
    + 3 health restore_i + b_i, menu rows at kappa 1. The logged encumbrance need is the shipped
    curve (0.95, 40) of r, IN holds r. nothing: the share of decisions with no pick."""
    rng = np.random.default_rng(seed)
    R, K, J = len(SYN_CAPS), len(SYN_NEEDS), len(SYN_COLS)
    r = rng.uniform(0.3, 1.1, n)
    hunger, health = rng.uniform(0, 1, n), rng.uniform(0, 1, n) ** 2
    N = np.stack([curve.logistic(r, 0.95, 40.0), hunger, health], axis=1)
    IN = np.full((n, K), np.nan)
    IN[:, 0] = r
    INHA = np.ones((n, R))
    for d in range(n):
        INHA[d, rng.choice(R, 3, replace=False)] = 0.0
    carry = SYN_CAPS[:, 0]                                    # graded, as the default spec reads it
    s = (w_carry * curve.logistic(r, c_true, s_true)[:, None] * carry[None, :] + 2 * hunger[:, None] * SYN_CAPS[None, :, 1]
         + 3 * health[:, None] * SYN_CAPS[None, :, 2] + SYN_B[None, :])
    logit = s - INHA
    P = np.exp(logit - logit.max(1, keepdims=True))
    P /= P.sum(1, keepdims=True)
    Y = np.array([rng.choice(R, p=p) for p in P], dtype=np.int32)
    none = rng.random(n) < nothing
    Y[none] = -1
    OUT = np.array(['nothing' if y < 0 else ('key' if INHA[d, y] == 0 else 'menu') for d, y in enumerate(Y)], dtype=object)
    LA = (np.arange(n) // per_launch).astype(np.int32)
    launches = [f'2026101{l // 100}-{l % 100:02d}0000' for l in range(int(LA.max()) + 1)]
    forms = ['0003EAE3', '00012EB7', 'FE012ABC', '0001397D', 'FF000801', '00099999']
    names = ['Feather', 'Bread', 'Potion', 'Sword', 'Apple', 'Elixir']
    UTIL = np.exp(s + rng.normal(0, 1.0, s.shape))           # an "old engine": noisy, page-blind
    REC = np.zeros((n, 0))
    rec_names = []
    if rec:
        REC = np.where(rng.random((n, 2)) < 0.3, rng.uniform(0.2, 1.0, (n, 2)), 0.0)
        rec_names = ['recent_kind_food@5s', 'recent_kind_food@30s']
    meta = [{'launch': launches[LA[d]], 'out': OUT[d], 'name': names[Y[d]] if Y[d] >= 0 else None, 'kind': '',
             'ctxkey': (launches[LA[d]], d), 'build': '0.23.50 (abc)', 'case': '', 'need_ep': None} for d in range(n)]
    return dict(CapMat=np.vstack([SYN_CAPS, np.zeros((1, J))]), xcol=np.zeros(0, dtype=int),
                CAPID=np.tile(np.arange(R, dtype=np.int32), (n, 1)), X=np.zeros((n, R, 0)), COV=np.zeros((n, R)),
                MASK=np.ones((n, R), dtype=bool), INHA=INHA, ITEM=np.tile(np.arange(R, dtype=np.int32), (n, 1)),
                SLOT=np.full((n, R), -1, dtype=np.int32), UTIL=UTIL, V2=np.full((n, R, 4), np.nan),
                CLS=np.full((n, R), -1, dtype=np.int32), Y=Y, N=N, IN=IN, OUT=OUT, LA=LA, SEG=np.zeros(n, dtype=np.int32),
                REC=REC, v2ok=np.zeros(n, dtype=bool), launches=launches, items={f: i for i, f in enumerate(forms)},
                classes={}, meta=meta, K=K, J=J, needs=list(SYN_NEEDS), cols=list(SYN_COLS), R=R, segments=['all'],
                recency_names=rec_names)


class OnlineSchedule(unittest.TestCase):
    def test_train_is_exactly_the_past(self):
        S = synth_S(400, per_launch=37)
        seen = []
        for tr, te in fitlib.online_chunks(S, every=5, min_train=100):
            self.assertTrue(len(te) > 0)
            self.assertEqual(tr.tolist(), list(range(te[0])))            # every earlier decision, nothing else
            self.assertEqual(te.tolist(), list(range(te[0], te[-1] + 1)))  # consecutive
            self.assertEqual(len(set(S['LA'][te].tolist())), 1)            # never across a launch
            self.assertLessEqual((S['Y'][te] >= 0).sum(), 5)
            seen += te.tolist()
        self.assertEqual(len(seen), len(set(seen)))                         # each tested once
        first = seen[0]
        self.assertEqual(S['LA'][first], 3)        # 3 launches x 37 = 111 picks >= 100 precede launch 3
        self.assertEqual(seen, list(range(first, 400)))

    def test_chunks_count_picks_and_end_at_a_launch(self):
        S = synth_S(300, per_launch=50, nothing=0.4, seed=2)
        for tr, te in fitlib.online_chunks(S, every=4, min_train=20):
            npk = (S['Y'][te] >= 0).sum()
            last_of_launch = te[-1] == len(S['LA']) - 1 or S['LA'][te[-1] + 1] != S['LA'][te[-1]]
            self.assertTrue(npk == 4 or last_of_launch)
            if npk == 4:
                self.assertGreaterEqual(S['Y'][te[-1]], 0)                  # a chunk ends at its 4th pick

    def test_never_fits_on_no_picks_and_rejects_disorder(self):
        S = synth_S(100, per_launch=20)
        chunks = list(fitlib.online_chunks(S, every=5, min_train=0))
        self.assertEqual(S['LA'][chunks[0][1][0]], 1)                      # launch 0 has no past
        S['LA'] = S['LA'][::-1].copy()
        with self.assertRaises(ValueError):
            list(fitlib.online_chunks(S))

    def test_no_future_leak(self):
        """Online results before the last launch do not change when the last launch's choices do."""
        S = synth_S(260, per_launch=40, seed=3)
        spec = {'outside': False}
        a, fits = fitlib.online_results(S, SYN_OBV, spec, every=10, min_train=60, maxiter=60)
        S2 = dict(S)
        last = S['LA'] == S['LA'].max()
        S2['Y'] = S['Y'].copy()
        S2['Y'][last] = (S['Y'][last] + 1) % S['R']
        b, _ = fitlib.online_results(S2, SYN_OBV, spec, every=10, min_train=60, maxiter=60)
        self.assertGreater(fits, 5)
        for i in np.where(~last)[0]:
            self.assertEqual(a[i] is None, b[i] is None)
            if a[i] is not None:
                self.assertEqual(a[i]['full_rank'], b[i]['full_rank'])
                self.assertAlmostEqual(a[i]['logp'], b[i]['logp'], places=9)
        self.assertTrue(all(a[i] is None for i in np.where(S['LA'] == 0)[0]))
        astar_a = fitlib.astar_online(S, 10, 60)
        astar_b = fitlib.astar_online(S2, 10, 60)
        for i in np.where(~last)[0]:
            if astar_a[i] is not None and 'page_logp' in astar_a[i]:
                self.assertAlmostEqual(astar_a[i]['page_logp'], astar_b[i]['page_logp'], places=9)

    def test_cv_results_matches_the_old_loop(self):
        """cv_results is cv.py's former fold loop (fit from init on the other folds)."""
        S = synth_S(300, per_launch=30, seed=4)
        FOLD = fitlib.launch_folds(S, 3)[S['LA']]
        res = fitlib.cv_results(S, SYN_OBV, {'outside': False}, FOLD, maxiter=80)
        for f in range(3):
            te, tr = np.where(FOLD == f)[0], np.where(FOLD != f)[0]
            M = fitlib.Model(fitlib.subset(S, tr), obvious=SYN_OBV, nitems=S['R'], outside=False)
            p = fitlib.fit(M, fitlib.subset(S, tr), maxiter=80)
            M2 = fitlib.Model(fitlib.subset(S, te), obvious=SYN_OBV, nitems=S['R'], outside=False)
            for i, r in zip(te, fitlib.evaluate(M2, fitlib.subset(S, te), p)):
                self.assertAlmostEqual(res[i]['logp'], r['logp'], places=9)


class Gate(unittest.TestCase):
    @staticmethod
    def ys(menu, pooled):
        return {'menu hit@8': (0.6, 0.2, menu[0], menu[1], menu[2], 400),
                'all picks hit@8': (0.7, 0.7, pooled[0], pooled[1], pooled[2], 1800)}

    def test_verdicts(self):
        self.assertEqual(fitlib.gate(self.ys((0.43, 0.38, 0.5), (-0.011, -0.040, 0.022)))[0], True)  # the rehearsal
        self.assertEqual(fitlib.gate(self.ys((0.43, 0.0, 0.5), (0.0, -0.01, 0.02)))[0], False)       # lower end 0: not above
        self.assertEqual(fitlib.gate(self.ys((0.43, 0.01, 0.5), (-0.03, -0.06, 0.0)))[0], True)      # upper end 0: not worse
        v, why = fitlib.gate(self.ys((0.43, 0.38, 0.5), (-0.02, -0.05, -0.001)))
        self.assertEqual(v, False)
        self.assertIn('FAILS', why[1])
        self.assertEqual(fitlib.gate({'menu hit@8': (0.6, 0.2, 0.4, 0.3, 0.5, 9)})[0], None)
        self.assertEqual(fitlib.gate({'all picks hit@8': (0.6, 0.7, -0.1, -0.2, -0.05, 9)})[0], False)  # a fail stays a fail
        self.assertEqual(fitlib.verdict_word(None), 'no verdict')

    def test_honest_specs(self):
        self.assertEqual(list(honest.select_specs('picks', None)), list(honest.HONEST_DEFAULT))
        with self.assertRaises(SystemExit):
            honest.select_specs('picks', ['split'])        # needs --split-at: not in the table
        self.assertIn('theta + b + u0(n) + recent', honest.select_specs('three', ['recent']))


class Curve(unittest.TestCase):
    def test_logistic_is_the_plugin_curve(self):
        self.assertAlmostEqual(float(curve.logistic(0.9, 0.95, 40)), 1 / (1 + math.exp(2.0)))
        self.assertEqual(float(curve.logistic(float('nan'), 0.95, 40)), 0.0)
        self.assertEqual(curve.ini_line('encumbrance', 0.9033, 41.2), 'encumbrance = logistic 0.9033 41.2')

    def test_with_curve_rereads_logged_inputs_only(self):
        S = synth_S(50)
        S['IN'][:5, 0] = np.nan
        S2 = curve.with_curve(S, 0, 0.8, 20)
        np.testing.assert_allclose(S2['N'][5:, 0], curve.logistic(S['IN'][5:, 0], 0.8, 20))
        np.testing.assert_array_equal(S2['N'][:5, 0], S['N'][:5, 0])
        np.testing.assert_array_equal(S2['N'][:, 1:], S['N'][:, 1:])
        self.assertFalse(S2['N'] is S['N'])

    def test_pair_only_mask_and_fixed_theta(self):
        S = synth_S(400, seed=5)
        Sv, ov = fitlib.presence_grading(S, SYN_OBV, strength=True)
        m = curve.pair_only_mask(Sv, ov, 0)
        self.assertEqual(m.shape, (3, 6))
        self.assertEqual(np.where(~m[0])[0].tolist(), [0, 3])        # carry weight and its ~strength twin
        self.assertFalse(m[1:].any())
        M = fitlib.Model(Sv, obvious=ov, nitems=6, outside=False, theta_fixed=m)
        p = fitlib.fit(M, Sv, maxiter=100)
        th = M.unpack(p)['theta']
        np.testing.assert_array_equal(th[m], M.theta0[m])               # held exactly
        self.assertNotAlmostEqual(th[0, 0], M.theta0[0, 0], places=3)   # the pair moved

    def test_recovers_the_centre(self):
        S = synth_S(2500, seed=6, c_true=0.82, s_true=30.0, w_carry=3.0)
        fr = curve.fit_curve(S, SYN_OBV, {'outside': False}, prior={'centre': 0.9, 'centre_sd': 1.0, 'slope_sd_log': 2.0},
                             rounds=6, maxiter=150, start_grid=(0.7, 0.8, 0.9, 1.0))
        self.assertAlmostEqual(fr['centre'], 0.82, delta=0.04)
        self.assertTrue(fr['sd_centre'] > 0)
        # a tight prior holds the centre (the documented prior, scaled down)
        fr2 = curve.fit_curve(S, SYN_OBV, {'outside': False}, prior={'centre': 0.95, 'centre_sd': 0.002},
                              rounds=3, maxiter=150, start_grid=(0.9, 0.95))
        self.assertAlmostEqual(fr2['centre'], 0.95, delta=0.006)
        js = curve.curve_json(fr, 'x', False, 3)
        self.assertEqual(js['ini'], curve.ini_line('encumbrance', fr['centre'], fr['slope']))
        dn, dp = curve.profile(fr, grid=(0.7, 0.82, 0.95), refit=False)
        self.assertEqual(int(np.argmin(dn)), 1)
        self.assertEqual(curve.edge_warnings((0.7, 0.8, 0.9), np.array([0, 1, 2]), np.array([1, 0, 2]))[0][:8], 'WARNING:')


def golden_documents():
    """The two golden examples, from literal pieces (tests/core/fixtures/bootstrap/)."""
    needs = ['health_deficit', 'encumbrance', 'hunger']
    cols = ['restore_health', 'utility_carry_weight', 'kind_food', 'summon']
    source = {'char': '0000000000000001', 'launches': 3, 'first_launch': '20261009-141228',
              'last_launch': '20261028-171653', 'decisions': 120, 'picks': 100,
              'builds': ['0.23.41 (abc1234)', '0.23.50 (def5678)'], 'forms': 'Huginn_All.csv', 'tool': 'golden example'}
    full = bootstrap.make_document(
        source=source, spec='theta + b + u0(n) + recent', outcomes='three', grading='presence+strength',
        needs=needs, cols=cols,
        theta=[[2.5, 0, 0, -0.125], [0, 1.75, 0, 0], [0, 0, 1.5, 0]],
        theta_precision=[[40.5, 11.1111, 11.1111, 12], [11.1111, 3.25, 11.1111, 11.1111], [11.1111, 11.1111, 20, 11.1111]],
        theta_strength=[[0.5, 0, 0, 0], [0, -0.25, 0, 0], [0, 0, 0.375, 0]],
        theta_strength_precision=[[15, 11.1111, 11.1111, 11.1111]] * 3,
        kappa=3.125, u0={'bias': -1.5, 'need': {'health_deficit': 0.25, 'encumbrance': 0.0, 'hunger': -0.5}},
        recent={'window_s': 30, 'taus_s': [5, 30], 'features': [
            {'family': 'summon', 'tau_s': 5, 'theta': [0, 0, 0, -0.75], 'theta_strength': [0, 0, 0, 0.125],
             'precision': [11.1111, 11.1111, 11.1111, 14], 'u0': 0.0625},
            {'family': 'summon', 'tau_s': 30, 'theta': [0.25, 0, 0, -0.5], 'theta_strength': [0, 0, 0, 0],
             'precision': [12, 11.1111, 11.1111, 13], 'u0': 0.0}]},
        features=[('ln util', 0.875), ('util missing', -2.25)],
        items=[('Skyrim.esm', '03EAF3', 'Potion of Minor Healing', 0.375, 9.5, 12),
               ('Update.esm', '000801', 'Feather on Self', -0.0625, 4.25, 1),
               ('ccBGSSSE001-Fish.esm', '00087E', 'Roasted Carp', 0.125, 4.5, 2)],
        items_dropped={'dynamic (FF)': 2, 'not in the form map': 1},
        curves={'encumbrance': ('logistic', 0.9, 40)}, created_utc='2026-10-28 12:00:00')
    minimal = bootstrap.make_document(
        source=source, spec='theta + b (doc 9)', outcomes='picks', grading='graded', needs=needs[:2], cols=cols[:2],
        theta=[[1.0, 0], [0, 1.0]], theta_precision=[[1.5, 11.1111], [11.1111, 1.25]], kappa=2.5,
        created_utc='2026-10-28 12:00:00')
    return {'bootstrap_v1_full.json': full, 'bootstrap_v1_minimal.json': minimal}


class BootstrapFormat(unittest.TestCase):
    def test_golden_files(self):
        """The checked-in examples are what the writer produces, and they validate. To refresh them
        after a deliberate format change: python tools/fit/test_fit.py --write-golden."""
        for name, doc in golden_documents().items():
            self.assertEqual(bootstrap.validate(doc), [], name)
            text = (GOLDEN_DIR / name).read_text(encoding='utf-8')
            self.assertEqual(bootstrap.dumps(doc), text, name)
            self.assertEqual(json.loads(text), doc)
            self.assertEqual(bootstrap.validate(json.loads(text)), [])

    def test_validate_rejects(self):
        full = golden_documents()['bootstrap_v1_full.json']

        def broken(fn):
            d = json.loads(json.dumps(full))
            fn(d)
            return bootstrap.validate(d)

        self.assertTrue(broken(lambda d: d.update(version=2)))
        self.assertTrue(broken(lambda d: d.update(format='other')))
        self.assertTrue(broken(lambda d: d['theta'][1].pop()))
        self.assertTrue(broken(lambda d: d.update(theta_strength=None)))          # required by presence+strength
        self.assertTrue(broken(lambda d: d.update(u0=None)))                      # required by three
        self.assertTrue(broken(lambda d: d['items'][0].update(local='3EAF3')))
        self.assertTrue(broken(lambda d: d['items'][0].update(local='03eaf3')))
        self.assertTrue(broken(lambda d: d['items'].append(dict(next(i for i in d['items'] if i['plugin'] == 'Skyrim.esm'),
                                                                plugin='SKYRIM.ESM'))))       # plugin names ignore case
        self.assertTrue(broken(lambda d: d['recent']['features'][0].update(family='nope')))
        self.assertTrue(broken(lambda d: d['recent']['features'][0].pop('u0')))
        self.assertTrue(broken(lambda d: d['features'].append({'name': 'ln ctx', 'w': 1})))
        self.assertTrue(broken(lambda d: d['curves'].update(encumbrance=['logistic', 0.9, 0])))
        self.assertTrue(broken(lambda d: d['needs'].append('hunger')))
        self.assertTrue(broken(lambda d: d.update(theta_bias=[0, 0, 0, 0])))      # no bias with presence+strength
        self.assertEqual(broken(lambda d: d.update(future_field={'x': 1})), [])  # unknown fields are allowed
        minimal = golden_documents()['bootstrap_v1_minimal.json']
        self.assertTrue(bootstrap.validate(dict(minimal, theta_strength=[[0, 0], [0, 0]])))
        with self.assertRaises(ValueError):
            bootstrap.dumps(dict(minimal, kappa=float('nan')))

    def test_numbers(self):
        self.assertEqual(bootstrap.num(-0.0), 0.0)
        self.assertEqual(bootstrap.num(1.23456789), 1.23457)
        self.assertEqual(bootstrap.num(np.float64(2.5)), 2.5)

    def test_item_key(self):
        forms = {0x0003EAE3: ('Skyrim.esm', 'Potion', 'Potion'), 0xFE012ABC: ('Light.esp', 'L', 'Spell'),
                 0x2D001234: ('Mod.esp', 'M', 'Armor')}
        self.assertEqual(bootstrap.item_key('0003EAE3', forms), (('Skyrim.esm', '03EAE3'), None))
        self.assertEqual(bootstrap.item_key('FE012ABC', forms), (('Light.esp', '000ABC'), None))   # light: 12 bits
        self.assertEqual(bootstrap.item_key(0x2D001234, forms), (('Mod.esp', '001234'), None))
        self.assertEqual(bootstrap.item_key('FF000801', forms), (None, 'dynamic (FF)'))
        self.assertEqual(bootstrap.item_key('00012345', forms), (None, 'not in the form map'))

    def test_read_forms(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'Huginn_All.csv'
            p.write_text('kind,formID,plugin,winningPlugin,name,playable\n'
                         'Potion,0003EAE3,"Skyrim.esm","Requiem.esp","Potion of X",1\n'
                         'Potion,0003EAE3,"Skyrim.esm","Requiem.esp","Potion of X",1\n'
                         'Spell,FE012ABC,"Light, Mod.esp","Light, Mod.esp","Spell, Y",1\n', encoding='utf-8')
            f = bootstrap.read_forms(p)
        self.assertEqual(f, {0x3EAE3: ('Skyrim.esm', 'Potion of X', 'Potion'), 0xFE012ABC: ('Light, Mod.esp', 'Spell, Y', 'Spell')})


SYN_FORMS = {0x0003EAE3: ('Skyrim.esm', 'Feather', 'Spell'), 0x00012EB7: ('Skyrim.esm', 'Bread', 'Food'),
             0xFE012ABC: ('Light.esp', 'Potion', 'Potion'), 0x0001397D: ('Skyrim.esm', 'Sword', 'Weapon'),
             0x00099999: ('Skyrim.esm', 'Not the Elixir', 'Potion')}


class Export(unittest.TestCase):
    def doc(self, spec, three=False, rec=False, log_kinds=None):
        S = synth_S(600, seed=7, nothing=0.3 if three else 0.0, rec=rec)
        Sv, M, p, f, it = export.fit_all(S, SYN_OBV, spec, maxiter=150)
        _, _, grading = export.check_spec(spec)
        msgs = []
        d = export.document(S, Sv, M, p, 'spec', grading, three, SYN_FORMS, {'encumbrance': ('logistic', 0.9, 40)},
                            {'char': 'x'}, log=msgs.append, log_kinds=log_kinds)
        return S, Sv, M, p, d, msgs

    def test_theta_b_kappa_round_trip(self):
        S, Sv, M, p, d, msgs = self.doc({'outside': False})
        self.assertEqual(bootstrap.validate(d), [])
        q = M.unpack(p)
        np.testing.assert_allclose(np.array(d['theta']), q['theta'], rtol=1e-5, atol=1e-6)
        self.assertAlmostEqual(d['kappa'], q['kappa'], places=4)
        self.assertIsNone(d['u0'])
        self.assertIsNone(d['recent'])
        by = {(it['plugin'], it['local']): it for it in d['items']}
        self.assertAlmostEqual(by[('Skyrim.esm', '03EAE3')]['b'], q['b'][0], places=5)
        self.assertAlmostEqual(by[('Light.esp', '000ABC')]['b'], q['b'][2], places=5)
        self.assertEqual(by[('Skyrim.esm', '03EAE3')]['picks'], int((S['Y'] == 0).sum()))
        # the dynamic Apple is dropped; the "Elixir" is named otherwise in the map: dropped, warned
        self.assertEqual(d['items_dropped'], {'dynamic (FF)': 1, 'name differs from the form map': 1})
        self.assertTrue(any('disagree' in m for m in msgs))
        self.assertEqual(d['curves'], {'encumbrance': ['logistic', 0.9, 40.0]})
        self.assertTrue(all(x > 0 for row in d['theta_precision'] for x in row))

    def test_kind_mismatch_is_dropped(self):
        *_, d, _ = self.doc({'outside': False}, log_kinds={'0001397D': 'Ammo'})
        self.assertEqual(d['items_dropped'].get('kind differs from the form map'), 1)
        self.assertNotIn('Sword', [it['name'] for it in d['items']])

    def test_three_outcomes_recent_and_strength(self):
        S, Sv, M, p, d, _ = self.doc({'_with': ['presence+strength', 'recent']}, three=True, rec=True)
        self.assertEqual(bootstrap.validate(d), [])
        self.assertEqual(d['model'], {'spec': 'spec', 'outcomes': 'three', 'grading': 'presence+strength'})
        q = M.unpack(p)
        np.testing.assert_allclose(np.array(d['theta_strength']), q['theta'][:3, 3:], rtol=1e-5, atol=1e-6)
        self.assertEqual([(f['family'], f['tau_s']) for f in d['recent']['features']], [('kind_food', 5.0), ('kind_food', 30.0)])
        np.testing.assert_allclose(d['recent']['features'][1]['theta'], q['theta'][4, :3], rtol=1e-5, atol=1e-6)
        self.assertAlmostEqual(d['recent']['features'][0]['u0'], q['uout'][3], places=5)
        self.assertAlmostEqual(d['u0']['bias'], q['uout'][-1], places=5)
        self.assertEqual(list(d['u0']['need']), SYN_NEEDS)

    def test_features_and_refusals(self):
        *_, d, _ = self.doc({'outside': False, 'feat_spec': ['astar']})
        self.assertEqual([f['name'] for f in d['features']], ['ln util', 'util missing'])
        for bad in ({'feat_spec': ['ctx']}, {'_with': ['split']}, {'_latest': True},
                    {'bias_need': True, '_with': ['presence+strength']}):
            with self.assertRaises(SystemExit):
                export.check_spec(bad)


class ExportMain(TempWork):
    def test_end_to_end_on_a_log(self):
        text = ''.join(session(f'2026101{i}-100000', CHAR_A, '0.23.50 (abc)', minute=i) for i in range(1, 4))
        self.quiet(v3load.main, [str(self.write('v3.jsonl', text))])
        forms = self.write('forms.csv', 'kind,formID,plugin,name\n'
                                        'Potion,0003EAE3,Skyrim.esm,Potion A\nWeapon,00012EB7,Skyrim.esm,Sword\n'
                                        'Ammo,0001397D,Dawnguard.esm,Bolt\nPotion,0003EAE5,Skyrim.esm,Potion B\n'
                                        'Armor,00099999,Skyrim.esm,Robe\n')
        cj = self.write('curve.json', json.dumps({'need': 'magicka_deficit', 'kind': 'logistic', 'centre': 0.5, 'slope': 10}))
        doc, out = self.quiet(export.main, ['--forms', str(forms), '--curve', str(cj), '--maxiter', '30'])
        self.assertIn('wrote', out)
        written = json.loads((self.dir / 'bootstrap.json').read_text(encoding='utf-8'))
        self.assertEqual(bootstrap.validate(written), [])
        self.assertEqual(written['model']['spec'], 'theta + b + recent_<family>')
        self.assertEqual(written['source']['char'], CHAR_A)
        self.assertEqual({(i['plugin'], i['local']) for i in written['items']},
                         {('Skyrim.esm', '03EAE3'), ('Skyrim.esm', '012EB7'), ('Dawnguard.esm', '01397D'),
                          ('Skyrim.esm', '03EAE5'), ('Skyrim.esm', '099999')})
        self.assertEqual(written['curves'], {'magicka_deficit': ['logistic', 0.5, 10.0]})
        self.assertTrue(written['recent']['features'])
        with self.assertRaises(SystemExit):
            self.quiet(export.main, ['--forms', str(forms), '--out', '../x.json'])


def write_golden():
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    for name, doc in golden_documents().items():
        assert bootstrap.validate(doc) == [], name
        (GOLDEN_DIR / name).write_text(bootstrap.dumps(doc), encoding='utf-8', newline='\n')
        print('wrote', GOLDEN_DIR / name)


if __name__ == '__main__':
    if '--write-golden' in sys.argv:
        write_golden()
        sys.exit(0)
    result = unittest.main(exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() else 1)
