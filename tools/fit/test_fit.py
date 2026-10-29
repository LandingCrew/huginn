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
    """A later build adds need ids (time_to_edge, edge_approach: branch edge-approach) and
    record fields; an older build's records lack fields the loader reads. Both load."""

    def text(self):
        old = session('20261010-100000', CHAR_A, '0.23.38 (c265bab)', minute=1)

        def drop_fields(r):          # an older writer: no char/gen/open/ep/preEquipped
            if r['t'] == 'dec':
                for k in ('char', 'gen', 'open', 'ep', 'preEquipped', 'confirmMs'):
                    r.pop(k, None)
        old = edit_lines(old, drop_fields)
        new = session('20261029-100000', CHAR_A, '0.23.48 (abc1234)', minute=2,
                      new_needs=('time_to_edge', 'edge_approach'))

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
        self.assertEqual(D['needs'][-2:], ['time_to_edge', 'edge_approach'])
        k = D['needs'].index('time_to_edge')
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
        k = S['needs'].index('time_to_edge')
        new = np.array([m['launch'] == '20261029-100000' for m in S['meta']])
        self.assertTrue((S['N'][~new, k] == 0).all())
        self.assertTrue((S['N'][new, k] > 0).any())

    def test_replay_reader(self):
        decs = replay.load_v3([self.write('v3.jsonl', self.text())])
        self.assertEqual(len(decs), 8)
        self.assertEqual(decs[4]['ctx']['need']['time_to_edge'], 0.7)
        self.assertNotIn('time_to_edge', decs[0]['ctx']['need'])
        self.assertNotIn('char', decs[0])


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


if __name__ == '__main__':
    result = unittest.main(exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() else 1)
