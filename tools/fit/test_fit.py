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


class NarcoticColumn(TempWork):
    """0.24.9 added a column after kind_light (named alcohol; 0.24.16 renamed it narcotic):
    every later column's index moved by one. The C++ encoder's golden session as 0.24.8
    wrote it (243 columns) and as this build writes it (244) hold the same caps; v3load maps
    both by the head's names, alone and in one load (a rotated file across the update)."""
    DEC = HERE.parents[1] / 'tests' / 'core' / 'fixtures' / 'decisions'

    @staticmethod
    def named_caps(D):
        out = []
        for key in sorted(D['ctxs']):
            for row in D['ctxs'][key]['rows']:
                c = row[common.CAP]
                out.append(None if c is None or c < 0 else sorted((D['cols'][j], v) for j, v in D['caps'][c]))
        return out

    def test_old_and_new_golden_read_the_same(self):
        old = v3load.load([self.DEC / 'synthetic_v3_0.24.8.jsonl'])
        new = v3load.load([self.DEC / 'synthetic_v3.jsonl'])
        self.assertEqual(old['heads'][0]['ncols'], 243)
        self.assertEqual(new['heads'][0]['ncols'], 244)
        self.assertNotIn('narcotic', old['cols'])
        self.assertNotIn('alcohol', new['cols'])
        self.assertEqual(new['cols'].index('narcotic'), new['cols'].index('kind_light') + 1)
        caps = self.named_caps(old)
        self.assertEqual(caps, self.named_caps(new))
        # the session's caps reach past the inserted column (school_*, weapon and armour stats)
        moved = {n for c in caps if c for n, _ in c} & set(new['cols'][new['cols'].index('narcotic') + 1:])
        self.assertTrue(moved)

    def test_one_load_across_the_update(self):
        old = (self.DEC / 'synthetic_v3_0.24.8.jsonl').read_text(encoding='utf-8')

        def relaunch(r):             # the older launch, so the two are not one launch twice
            if 'launch' in r:
                r['launch'] = '20261009-110000'
        D = v3load.load([self.write('old.jsonl', edit_lines(old, relaunch)), self.DEC / 'synthetic_v3.jsonl'])
        self.assertEqual(len(D['cols']), 244)          # the union by name: narcotic appended once
        self.assertEqual(D['stats']['dup_ctx'], 2)      # each file's own second segment, nothing across
        by_launch = {}
        for (launch, cid), ctx in D['ctxs'].items():
            for row in ctx['rows']:
                c = row[common.CAP]
                by_launch.setdefault(launch, []).append(
                    None if c is None or c < 0 else sorted((D['cols'][j], v) for j, v in D['caps'][c]))
        self.assertEqual(by_launch['20261009-110000'], by_launch['20261009-120000'])

    def test_rename_is_a_new_name(self):
        """0.24.16 renamed alcohol to narcotic at the same index. v3load keys columns by name
        and has no alias (no backward-compatibility path): a 0.24.9 file reads its column as
        alcohol, this build's as narcotic, every other column the same."""
        old = v3load.load([self.DEC / 'synthetic_v3_0.24.9.jsonl'])
        new = v3load.load([self.DEC / 'synthetic_v3.jsonl'])
        self.assertEqual(old['heads'][0]['ncols'], 244)
        at = old['cols'].index('alcohol')
        self.assertNotIn('narcotic', old['cols'])
        self.assertEqual(new['cols'].index('narcotic'), at)
        self.assertEqual(old['cols'][:at] + old['cols'][at + 1:], new['cols'][:at] + new['cols'][at + 1:])
        self.assertEqual(self.named_caps(old), self.named_caps(new))   # the session sets neither column


class OldEngineColumns(TempWork):
    """0.23.53: the old engine's score parts and the favourites hotkey (row columns est..hk),
    the favorite bit, ctx.old and dec.rid. A row that carries none of the columns -- every row
    of an older head -- reads OLD None; a decision without rid reads None."""

    def text(self):
        def older(r):                # a 0.23.52 writer: twelve row columns, no floors, no rid
            if r['t'] == 'head':
                r['row'] = r['row'][:12]
                r.pop('floors', None)
                r['flags'].pop('favorite', None)
            elif r['t'] == 'ctx':
                r['rows'] = [row[:12] for row in r['rows']]
            elif r['t'] == 'dec':
                r['add'] = [row[:12] for row in r['add']]
        old = edit_lines(session('20261028-100000', CHAR_A, '0.23.52 (abc1234)', minute=1), older)

        def with_rid(r):             # the key press carries its v2 record's id
            if r['t'] == 'dec':
                r['rid'] = 7 if r['out'] == 'key' else None
            elif r['t'] == 'ctx' and r['id'] == 10:
                r['old'] = {'minCtx': 0.05, 'minUtil': 0.1, 'topN': 10, 'scored': 1, 'phi': [1] + [0] * 17}
        new = edit_lines(session('20261029-100000', CHAR_A, '0.23.53 (abc1234)', minute=2), with_rid)
        return old + new

    def test_v3load(self):
        D = v3load.load([self.write('v3.jsonl', self.text())])
        self.assertEqual(len(D['decs']), 8)
        new = D['ctxs'][('20261029-100000', 10)]
        old = D['ctxs'][('20261028-100000', 10)]
        rows = new['rows']
        self.assertEqual(rows[0][common.OLD], (0.31, 0.75, 6, 0.25, 0, 0.5, 0.9, 'pass', None, None))
        self.assertEqual(rows[1][common.OLD][common.O_FL], 'ctx')
        self.assertEqual(rows[1][common.OLD][common.O_HK], 0)
        self.assertEqual(rows[1][common.OLD][common.O_WU], 0.004)
        self.assertTrue(rows[1][common.FLAGS] & common.F['favorite'])
        self.assertIsNone(rows[2][common.OLD])        # a row with none of the columns
        self.assertEqual(rows[3][common.FB], 'cooldown')   # the earlier columns unchanged
        self.assertTrue(all(r[common.OLD] is None for r in old['rows']))
        self.assertEqual(len(rows[0]), 15)          # 0.23.56: SHADOW appended; 0.24.17: SHADOW_A
        self.assertEqual(len(old['rows'][0]), 15)
        self.assertEqual(new['old']['topN'], 10)       # kept as written
        self.assertIsNone(old['old'])
        rids = {(d['launch'], d['out']): d['rid'] for d in D['decs']}
        self.assertEqual(rids[('20261029-100000', 'key')], 7)
        self.assertIsNone(rids[('20261029-100000', 'nothing')])
        self.assertIsNone(rids[('20261028-100000', 'key')])   # older: the default
        self.assertEqual(D['heads'][-1]['floors'], ['pass', 'cold', 'ctx', 'util', 'suppressed'])
        self.assertNotIn('floors', D['heads'][0])

    def test_build_still_runs(self):
        self.quiet(v3load.main, [str(self.write('v3.jsonl', self.text()))])
        S = fitlib.build(common.load(), include_nothing=True)
        launches = [m['launch'] for m in S['meta']]
        self.assertGreater(len(launches), 0)
        # The same session in both launches: the new columns change nothing the fit builds.
        self.assertEqual(launches.count('20261028-100000'), launches.count('20261029-100000'))
        new = np.array([m['launch'] == '20261029-100000' for m in S['meta']])
        for key in ('N', 'Y'):
            if key in S and isinstance(S[key], np.ndarray) and len(S[key]) == len(new):
                self.assertTrue(np.array_equal(S[key][new], S[key][~new]), key)

    def test_replay_reader(self):
        decs = replay.load_v3([self.write('v3.jsonl', self.text())])
        new = [d for d in decs if d['launch'] == '20261029-100000']
        old = [d for d in decs if d['launch'] == '20261028-100000']
        self.assertEqual(new[0]['ctx']['rows'][0]['fl'], 'pass')
        self.assertEqual(new[0]['ctx']['rows'][1]['hk'], 0)
        self.assertEqual(new[0]['rid'], 7)
        self.assertTrue(all(r['est'] is None and r['hk'] is None for d in old for r in d['rows']))


class ShadowAColumns(TempWork):
    """0.24.17: the D2 engine in shadow (row columns amu, ar, br, ak, bk). v3load keeps them as
    the row tuple's SHADOW_A element (None on older rows and on rows with none of them); the fit
    builds the same thing with or without them."""

    def text(self):
        def older(r):                # a 0.24.16 writer: no D2 shadow columns
            if r['t'] == 'head':
                r['row'] = r['row'][:27]
            elif r['t'] == 'ctx':
                r['rows'] = [row[:27] for row in r['rows']]
            elif r['t'] == 'dec':
                r['add'] = [row[:27] for row in r['add']]
        old = edit_lines(session('20261028-100000', CHAR_A, '0.24.16 (abc1234)', minute=1), older)
        new = session('20261029-100000', CHAR_A, '0.24.17 (abc1234)', minute=2)
        return old + new

    def test_v3load(self):
        D = v3load.load([self.write('v3.jsonl', self.text())])
        new = D['ctxs'][('20261029-100000', 10)]['rows']
        old = D['ctxs'][('20261028-100000', 10)]['rows']
        self.assertEqual(new[0][common.SHADOW_A], (1.125, 1, 3, 0, -1))
        self.assertEqual(new[0][common.SHADOW_A][common.A_AK], 0)
        self.assertIsNone(new[1][common.SHADOW_A])
        self.assertTrue(all(r[common.SHADOW_A] is None for r in old))
        self.assertEqual(new[0][common.SHADOW][:4], (0.875, 0.25, 0.125, 0))   # the columns before, unchanged

    def test_build_ignores_them(self):
        self.quiet(v3load.main, [str(self.write('v3.jsonl', self.text()))])
        S = fitlib.build(common.load(), include_nothing=True)
        new = np.array([m['launch'] == '20261029-100000' for m in S['meta']])
        self.assertGreater(new.sum(), 0)
        for key in ('N', 'Y'):
            if key in S and isinstance(S[key], np.ndarray) and len(S[key]) == len(new):
                self.assertTrue(np.array_equal(S[key][new], S[key][~new]), key)


class ShadowColumns(TempWork):
    """0.23.56 (R8.2): the choice model's columns (mu, sd, lm, um, ex; the head's bootstrap;
    in shadow at 0.23.56, the live ranking from R8.3). v3load keeps them as the row tuple's
    SHADOW element (None on older rows), ex as (need id, column id) by the loader's global names
    or "learned"; of them the fit reads only lm / um, and only in the 'learn_live' feature
    (LiveLearnFeature), so the other specs build the same thing with or without them."""

    def text(self):
        def older(r):                # a 0.23.55 writer: no shadow columns, no bootstrap
            if r['t'] == 'head':
                r['row'] = r['row'][:22]
                r.pop('bootstrap', None)
            elif r['t'] == 'ctx':
                r['rows'] = [row[:22] for row in r['rows']]
            elif r['t'] == 'dec':
                r['add'] = [row[:22] for row in r['add']]
        old = edit_lines(session('20261028-100000', CHAR_A, '0.23.55 (abc1234)', minute=1), older)
        new = session('20261029-100000', CHAR_A, '0.23.56 (abc1234)', minute=2)
        return old + new

    def test_v3load(self):
        D = v3load.load([self.write('v3.jsonl', self.text())])
        new = D['ctxs'][('20261029-100000', 10)]['rows']
        old = D['ctxs'][('20261028-100000', 10)]['rows']
        sh = new[0][common.SHADOW]
        self.assertEqual(sh[:4], (0.875, 0.25, 0.125, 0))
        k, j = sh[common.S_EX]
        self.assertEqual((D['needs'][k], D['cols'][j]), ('magicka_deficit', 'restore_magicka'))
        self.assertEqual(new[2][common.SHADOW][common.S_EX], 'learned')
        self.assertIsNone(new[1][common.SHADOW])
        self.assertTrue(all(r[common.SHADOW] is None for r in old))
        self.assertEqual(D['heads'][-1]['bootstrap']['created_utc'], '2026-10-29 12:29:59')
        self.assertNotIn('bootstrap', D['heads'][0])

    def test_build_ignores_them(self):
        self.quiet(v3load.main, [str(self.write('v3.jsonl', self.text()))])
        S = fitlib.build(common.load(), include_nothing=True)
        new = np.array([m['launch'] == '20261029-100000' for m in S['meta']])
        for key in ('N', 'Y', 'CAPID', 'INHA', 'UTIL'):
            a, b = S[key][new], S[key][~new]
            self.assertTrue(np.array_equal(a, b, equal_nan=a.dtype.kind == 'f'), key)


class ReplayEquivalence(unittest.TestCase):
    """R8.2: export_scores.py writes the fixture the C++ replay test reads (fitlib's logits under
    the shipped bootstrap). The checked-in fixture is what --synthetic --write-fixture writes now:
    a change to fitlib's model, the synthetic log or the bootstrap must regenerate it."""

    def test_fixture_is_current(self):
        import export_scores
        with tempfile.TemporaryDirectory() as tmp:
            env = os.environ.get('HUGINN_FIT_WORK')
            os.environ['HUGINN_FIT_WORK'] = tmp
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    path = export_scores.main(['--synthetic', '--out', 'replay.jsonl'])
            finally:
                if env is None:
                    os.environ.pop('HUGINN_FIT_WORK', None)
                else:
                    os.environ['HUGINN_FIT_WORK'] = env
            got = Path(path).read_text(encoding='utf-8')
        want = export_scores.FIXTURE.read_text(encoding='utf-8')
        self.assertEqual(got, want, 'regenerate: python tools/fit/export_scores.py --synthetic --write-fixture')

    def test_fixture_exercises_the_terms(self):
        rows = [json.loads(l) for l in open(common.REPO / 'tests' / 'core' / 'fixtures' / 'choice' / 'replay_synthetic.jsonl',
                                            encoding='utf-8')]
        decs = [r for r in rows if r['t'] == 'dec']
        self.assertGreater(len(decs), 40)
        self.assertTrue(any(d['rec'] for d in decs))                       # recent_<family>
        self.assertTrue(any(d['in'] for d in decs))                        # a curve need
        self.assertTrue(any(r[4] != 0 for d in decs for r in d['rows']))   # ln learn mult
        self.assertTrue(any(r[3] in rows[0]['items'] for d in decs for r in d['rows']))   # b
        self.assertTrue(any(r[2] == 1 for d in decs for r in d['rows']))   # kappa


class HardGateDrops(TempWork):
    """0.23.55 (R8.1): a row dropped by each of the four hard gates reads its reason by the
    head's own filter list, in v3load and in tools/replay; the older reasons read the same.
    0.24.29: lock_out_of_reach, appended after them, the same way."""
    NEW = ('no_follower', 'no_workstation', 'not_dark', 'favorite_suppressed', 'lock_out_of_reach')
    SWORD = {"weapon_damage": 0.75, "weapon_type_sword": 1}

    def text(self):
        head, seg1, seg2 = trv3.make_session()
        self.assertEqual(head['filters'][15:], list(self.NEW))     # the golden head (0.23.55, 0.24.29)
        r = seg1[0]['ctx']['rows'][0]
        extra = [dict(r, form='000A000' + str(i), uid=0, kind=kind, src=None, cap=cap, flags=4, slot=-1,
                      util=None, x={}, wp=None, fb=fb, cr=None, **{k: None for k in trv3.OLD_KEYS})
                 for i, (fb, kind, cap) in enumerate([
                     ('no_follower', 'Spell', {"restore": 0.5, "restore_health_other": 0.5}),
                     ('no_workstation', 'Armor', None),
                     ('not_dark', 'Light', None),
                     ('favorite_suppressed', 'Weapon', self.SWORD),
                     ('lock_out_of_reach', 'Spell', {"utility": 0.5, "utility_unlock": 0.5})])]
        seg1[0]['ctx']['rows'] = seg1[0]['ctx']['rows'] + extra
        head = dict(head, launch='20261029-120000')
        for d in seg1 + seg2:
            d['launch'], d['char'] = '20261029-120000', CHAR_A
        return trv3.encode([(head, seg1), (head, seg2)])

    def test_v3load(self):
        D = v3load.load([self.write('v3.jsonl', self.text())])
        rows = D['ctxs'][('20261029-120000', 10)]['rows']
        self.assertEqual([row[common.FB] for row in rows[-len(self.NEW):]], list(self.NEW))
        self.assertEqual([row[common.FORM] for row in rows[-len(self.NEW):]], [f'000A000{i}' for i in range(len(self.NEW))])
        self.assertTrue(all(not row[common.FLAGS] & common.F['eligible'] for row in rows[-len(self.NEW):]))
        self.assertEqual(rows[3][common.FB], 'cooldown')          # an older reason, unchanged
        self.assertIsNone(rows[0][common.FB])                      # an eligible row: none
        self.assertEqual(len(D['heads'][0]['filters']), 20)

    def test_replay_reader(self):
        decs = replay.load_v3([self.write('v3.jsonl', self.text())])
        rows = decs[0]['ctx']['rows']
        self.assertEqual([r['fb'] for r in rows[-len(self.NEW):]], list(self.NEW))
        self.assertEqual(rows[3]['fb'], 'cooldown')

    def test_older_head_reads_the_same(self):
        # The same rows under a 0.23.54 head (15 names): the older reasons keep their
        # indices, so a file written before 0.23.55 decodes exactly as before.
        def older(r):
            if r['t'] == 'head':
                r['filters'] = r['filters'][:15]
            elif r['t'] == 'ctx':
                r['rows'] = [row for row in r['rows'] if not str(row[0]).startswith('000A000')]
        text = edit_lines(self.text(), older)
        D = v3load.load([self.write('v3.jsonl', text)])
        rows = D['ctxs'][('20261029-120000', 10)]['rows']
        self.assertEqual(rows[3][common.FB], 'cooldown')
        self.assertEqual(len(D['heads'][0]['filters']), 15)


class FloorCheck(TempWork):
    """v3load.floor_check: each row's old-engine floor (fl) against the tests the floor pass
    applies. The C++ encoder's golden session (whose floors the legacy module's capture
    gives) passes; rows edited to break each rule are caught, and a value logged equal to its
    floor is not judged."""
    GOLDEN = HERE.parents[1] / 'tests' / 'core' / 'fixtures' / 'decisions' / 'synthetic_v3.jsonl'
    # row columns of the 0.23.53 head (DecisionLog.cpp): flags 5, cw 18, fl 19, wu 20
    FLAGS, CW, FL, WU = 5, 18, 19, 20
    FLOORS = ['pass', 'cold', 'ctx', 'util', 'suppressed']

    def golden(self):
        return self.GOLDEN.read_text(encoding='utf-8')

    def test_golden_passes(self):
        bad, checked, _ = v3load.floor_check(v3load.load([self.write('v3.jsonl', self.golden())]))
        self.assertGreater(checked, 0)
        self.assertEqual(dict(bad), {})

    def test_older_rows_skipped(self):
        p = self.GOLDEN.parent / 'synthetic_v3_0.23.52.jsonl'
        bad, checked, _ = v3load.floor_check(v3load.load([p]))
        self.assertEqual((dict(bad), checked), ({}, 0))

    def test_violations(self):
        def breaks(r):               # context 1 (the second segment writes it again)
            if r['t'] != 'ctx' or r['id'] != 1:
                return
            rows = {row[0]: row for row in r['rows']}
            fl = self.FLOORS.index
            a = rows['0003EADE']     # scored, pass, cw 0.8: call it ctx
            a[self.FL] = fl('ctx')
            b = rows['00012EB7']     # unscored util: a wu above minUtil 0.1
            b[self.WU] = 0.25
            c = rows['000640B6']     # a filter drop: give it a floor
            c[self.FL] = fl('util')
            c[self.WU] = 0.0001
            r['rows'].append(['00099999', 0, 0, 0, -1, 1, -1, None, [], None, None, None,
                              0, 0, 0, 1, 0, 0.3, 0.9, fl('suppressed'), 0])   # fav is boost
            r['rows'].append(['00099998', 0, 0, 0, -1, 1, -1, None, [], None, None, None,
                              0, 0, 0, 1, 0, 0.3, 0.01, fl('util'), 0.01])    # non-favourite, cw < minCtx
            r['rows'].append(['00099997', 0, 0, 0, -1, 3, -1, 0.5, [], None, None, None,
                              0, 0, 0, 1, 0, 0.3, 0.01, fl('pass')])          # non-favourite pass, cw < minCtx
            r['rows'].append(['00099996', 0, 0, 0, -1, 1, -1, None, [], None, None, None,
                              0, 0, 0, 1, 0, 0.3, 0.05, fl('ctx'), 0.1])      # both exactly at a floor
            r['rows'].append(['00099995', 0, 0, 0, -1, 1 | 512, -1, None, [], None, None, None,
                              0, 0, 0, 1, 0, 0.3, 0.01, fl('util'), 0.05])    # a favourite under minCtx
        D = v3load.load([self.write('v3.jsonl', edit_lines(self.golden(), breaks))])
        bad, checked, ex = v3load.floor_check(D)
        self.assertEqual(dict(bad), {'ctx_above_min': 1, 'scored_mismatch': 1, 'util_above_min': 1,
                                     'not_eligible': 1, 'not_suppress': 1, 'util_below_ctx': 1,
                                     'pass_below_ctx': 1})
        self.assertEqual(ex['ctx_above_min'][0][2], '0003EADE')
        self.assertEqual(ex['util_above_min'][0][2], '00012EB7')
        self.assertEqual(ex['not_eligible'][0][2], '000640B6')
        self.assertEqual(ex['util_below_ctx'][0][2], '00099998')
        self.assertEqual(ex['pass_below_ctx'][0][2], '00099997')
        _, out = self.quiet(v3load.main, [str(self.write('v3b.jsonl', edit_lines(self.golden(), lambda r: None)))])
        self.assertIn('no violations', out)


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
# honest yardstick, curve fit, bootstrap export (0.23.52)
# ---------------------------------------------------------------------------
import bootstrap   # noqa: E402
import curve       # noqa: E402
import export      # noqa: E402
import honest      # noqa: E402
import cv          # noqa: E402

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

    def test_astar_temperature_is_the_past(self):
        """A*'s page temperature for a chunk is fitted on the decisions before it only: each tested
        page_logp is softmax(beta * z) with beta from the chunk's training set, and changing one pick
        inside a chunk changes neither the earlier chunks nor the other decisions of its own chunk
        (test_no_future_leak changes only the last launch, so it misses a leak inside a chunk)."""
        S = synth_S(260, per_launch=40, seed=3)
        every, mt = 10, 60
        a = fitlib.astar_online(S, every, mt)
        _, z = fitlib.astar_scores(S)
        chunks = list(fitlib.online_chunks(S, every, mt))
        checked = 0
        for tr, te in chunks:
            beta = fitlib.page_temperature(S, z, tr)
            for i in te:
                if a[i] is None or 'page_logp' not in a[i]:
                    continue
                on = S['MASK'][i] & (S['INHA'][i] == 0)
                sp = beta * z[i][on]
                y = np.where(on)[0].tolist().index(int(S['Y'][i]))
                m = sp.max()
                self.assertAlmostEqual(a[i]['page_logp'], float(sp[y] - m - np.log(np.exp(sp - m).sum())), places=7)
                checked += 1
        self.assertGreater(checked, 20)
        # one key pick of a middle chunk moved to another on-page row: a pick page_temperature reads
        tr, te = chunks[len(chunks) // 2]
        keys = [i for i in te if S['OUT'][i] == 'key']
        self.assertGreaterEqual(len(keys), 2)
        d = keys[-1]
        on = np.where(S['INHA'][d] == 0)[0]
        new = next(int(r) for r in on if r != S['Y'][d] and abs(z[d, r] - z[d, S['Y'][d]]) > 1e-3)
        S2 = dict(S)
        S2['Y'] = S['Y'].copy()
        S2['Y'][d] = new
        b = fitlib.astar_online(S2, every, mt)
        for i in range(d):                        # every earlier decision, those of d's chunk included
            if a[i] is not None and 'page_logp' in a[i]:
                self.assertAlmostEqual(a[i]['page_logp'], b[i]['page_logp'], places=9, msg=f'decision {i}')
        # the change is one a temperature fitted on the chunk itself would see
        self.assertNotAlmostEqual(fitlib.page_temperature(S, z, np.concatenate([tr, te])),
                                  fitlib.page_temperature(S2, z, np.concatenate([tr, te])), places=6)

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


class DataKey(unittest.TestCase):
    def test_changes_with_what_the_fit_reads(self):
        S = synth_S(80, rec=True)
        k = fitlib.data_key(S, SYN_OBV)
        self.assertEqual(k, fitlib.data_key(dict(S), dict(SYN_OBV)))
        for name in ('CapMat', 'N', 'IN', 'REC', 'INHA', 'UTIL', 'Y'):
            S2 = dict(S)
            S2[name] = S[name].copy()
            S2[name].flat[1] = 7                           # any change to an array the fit reads
            self.assertNotEqual(fitlib.data_key(S2, SYN_OBV), k, name)
        self.assertNotEqual(fitlib.data_key(S, {(0, 0): 1.0}), k)                # the obvious pairs
        self.assertNotEqual(fitlib.data_key(dict(S, needs=['a', 'b', 'c']), SYN_OBV), k)
        self.assertNotEqual(fitlib.data_key(dict(S, V2=np.zeros_like(S['V2'])), SYN_OBV), k)  # D5 reads V2 (0.23.54)
        self.assertEqual(fitlib.data_key(dict(S, CLS=S['CLS'] + 1), SYN_OBV), k)  # only 'cheap' reads it


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
        self.assertTrue(broken(lambda d: d['recent'].update(window_s=20.0)))   # window = max(taus_s)
        self.assertTrue(broken(lambda d: d['features'].append({'name': 'ln ctx', 'w': 1})))
        self.assertTrue(broken(lambda d: d['curves'].update(encumbrance=['logistic', 0.9, 0])))
        self.assertTrue(broken(lambda d: d['needs'].append('hunger')))
        self.assertTrue(broken(lambda d: d.update(theta_bias=[0, 0, 0, 0])))      # no bias with presence+strength
        # a precision is above 0 (sigma^2 = 1 / precision)
        self.assertTrue(broken(lambda d: d['theta_precision'][0].__setitem__(1, 0)))
        self.assertTrue(broken(lambda d: d['theta_strength_precision'][2].__setitem__(0, -1.0)))
        self.assertTrue(broken(lambda d: d['recent']['features'][1]['precision'].__setitem__(3, 0.0)))
        self.assertTrue(broken(lambda d: d['items'][1].update(precision=0)))
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

    def test_item_key_names_the_index_owner(self):
        """An injected record (the real dump's FE97B053: defined by 'BOOB - Requiem.esp' at BOOB.esp's
        index FE97B; the patch owns FE97C) is keyed by the index owner, so LookupForm(local, plugin)
        rebuilds its FormID; an index the dump cannot attribute is not keyed."""
        forms = {0xFE97B001: ('BOOB.esp', 'A', 'Spell'), 0xFE97B002: ('BOOB.esp', 'B', 'Spell'),
                 0xFE97B053: ('BOOB - Requiem.esp', 'Drunkness Monitor', 'Spell'),
                 0xFE97C800: ('BOOB - Requiem.esp', 'C', 'Spell'),
                 0x05000801: ('Master.esm', 'M', 'Potion'), 0x06000802: ('Patch.esp', 'P', 'Potion'),
                 0x05000900: ('patch.ESP', 'Injected', 'Potion'),         # plugin names compare without case
                 0x07000001: ('One.esp', 'X', 'Armor'), 0x07000002: ('Two.esp', 'Y', 'Armor')}
        own = bootstrap.index_owners(forms)
        self.assertEqual(own[0xFE97B], 'BOOB.esp')
        self.assertEqual(own[0xFE97C], 'BOOB - Requiem.esp')
        self.assertEqual(own[0x05], 'Master.esm')
        self.assertEqual(own[0x06], 'Patch.esp')
        self.assertIsNone(own[0x07])                     # two plugins seen only there: which owns it is unknown
        self.assertEqual(bootstrap.item_key(0xFE97B053, forms, own), (('BOOB.esp', '000053'), None))
        self.assertEqual(bootstrap.item_key(0xFE97C800, forms), (('BOOB - Requiem.esp', '000800'), None))
        self.assertEqual(bootstrap.item_key(0x05000900, forms, own), (('Master.esm', '000900'), None))
        self.assertEqual(bootstrap.item_key(0x07000001, forms, own), (None, 'index owner unknown'))

    def test_read_forms(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'Huginn_All.csv'
            p.write_text('kind,formID,plugin,winningPlugin,name,playable\n'
                         'Potion,0003EAE3,"Skyrim.esm","Requiem.esp","Potion of X",1\n'
                         'Potion,0003EAE3,"Skyrim.esm","Requiem.esp","Potion of X",1\n'
                         'Spell,FE012ABC,"Light, Mod.esp","Light, Mod.esp","Spell, Y",1\n', encoding='utf-8')
            f = bootstrap.read_forms(p)
        self.assertEqual(f, {0x3EAE3: ('Skyrim.esm', 'Potion of X', 'Potion'), 0xFE012ABC: ('Light, Mod.esp', 'Spell, Y', 'Spell')})


# FE012ABC is an injected record (as the real dump's FE97B053): defined by 'Light - Patch.esp' inside
# Light.esp's index FE012 ('Light - Patch.esp' owns FE013), so the file keys it by Light.esp, the
# index owner (bootstrap.index_owners), and doc_scores resolves that key back to FE012ABC.
SYN_FORMS = {0x0003EAE3: ('Skyrim.esm', 'Feather', 'Spell'), 0x00012EB7: ('Skyrim.esm', 'Bread', 'Food'),
             0xFE012ABC: ('Light - Patch.esp', 'Potion', 'Potion'), 0x0001397D: ('Skyrim.esm', 'Sword', 'Weapon'),
             0x00099999: ('Skyrim.esm', 'Not the Elixir', 'Potion'),
             0xFE012001: ('Light.esp', 'Light item', 'Armor'), 0xFE013800: ('Light - Patch.esp', 'Patch item', 'Armor')}


def with_cross_and_slots(S, seed=11):
    """S with two runtime cross-features (they name kind_food and restore_health, as an
    xcol entry names an effect column) on some rows, and page slots 0-7 on the rows held on
    the page (INHA 0), so the doc's x_j(i) and slot N terms are exercised."""
    rng = np.random.default_rng(seed)
    S = dict(S)
    Dn, R = S['CAPID'].shape
    S['xcol'] = np.array([1, 2])
    S['X'] = np.where(rng.random((Dn, R, 2)) < 0.4, rng.uniform(0.1, 1.0, (Dn, R, 2)), 0.0)
    slot = np.full((Dn, R), -1, dtype=np.int32)
    for d in range(Dn):
        on = np.where(S['INHA'][d] == 0)[0]
        slot[d, on] = rng.permutation(8)[:len(on)]
    S['SLOT'] = slot
    return S


def doc_scores(doc, S, forms):
    """An independent reader of a bootstrap document, as docs/architecture/9-bootstrap-format.md
    writes the score: (logit per row, u0 per decision) for every decision of S (the base dataset:
    S['needs'] and S['cols'] in the build's order; recent_<family> values from S['REC']).
    Names map by name; an item is found by resolving its (plugin, local) key back through the
    form map, as LookupForm would."""
    NI = {n: i for i, n in enumerate(S['needs'])}
    CI = {c: j for j, c in enumerate(S['cols'])}
    K, J = len(S['needs']), len(S['cols'])
    fcol = [CI.get(c) for c in doc['cols']]

    def row(vals):
        out = np.zeros(J)
        for b, j in enumerate(fcol):
            if j is not None:
                out[j] = vals[b]
        return out

    TH, THS = np.zeros((K, J)), np.zeros((K, J))
    for a, n in enumerate(doc['needs']):
        if n in NI:
            TH[NI[n]] = row(doc['theta'][a])
            if doc['theta_strength'] is not None:
                THS[NI[n]] = row(doc['theta_strength'][a])
    strength = doc['model']['grading'] == 'presence+strength'
    presence = doc['model']['grading'] != 'graded'
    rname = {n: i for i, n in enumerate(S['recency_names'])}
    rec = []
    for fo in (doc['recent'] or {}).get('features', []):
        rec.append((rname[f"recent_{fo['family']}@{fo['tau_s']:g}s"], row(fo['theta']),
                    row(fo['theta_strength']) if strength else np.zeros(J), fo.get('u0', 0.0)))
    tbias = row(doc['theta_bias']) if doc['theta_bias'] is not None else None
    loc_of = {}
    owners = bootstrap.index_owners(forms)
    for fid in forms:
        key, _ = bootstrap.item_key(fid, forms, owners)
        if key:
            loc_of[(key[0].lower(), key[1])] = f'{fid:08X}'
    b_of = {loc_of[(it['plugin'].lower(), it['local'])]: it['b'] for it in doc['items']}
    form_of = {i: f for f, i in S['items'].items()}
    W = {f['name']: f['w'] for f in doc['features']}
    three = doc['model']['outcomes'] == 'three'
    Dn, R = S['CAPID'].shape
    logit, u0 = np.full((Dn, R), -np.inf), np.full(Dn, -np.inf)
    for d in range(Dn):
        n = S['N'][d]
        r = S['REC'][d]
        if three:
            u0[d] = doc['u0']['bias'] + sum(w * n[NI[k]] for k, w in doc['u0']['need'].items() if k in NI) \
                + sum(ru * r[ri] for ri, _, _, ru in rec)
        for i in range(R):
            if not S['MASK'][d, i]:
                continue
            cap = S['CapMat'][S['CAPID'][d, i]]
            x = np.zeros(J)
            for xi, j in enumerate(S['xcol']):
                x[j] += S['X'][d, i, xi]
            g = ((cap > 0).astype(float) if presence else cap) + x
            st = (cap - 0.5) * (cap > 0)
            s = n @ TH @ g + (n @ THS @ st if strength else 0.0)
            for ri, th, ths, _ in rec:
                s += r[ri] * (th @ g + (ths @ st if strength else 0.0))
            if tbias is not None:
                s += tbias @ g
            s += b_of.get(form_of[int(S['ITEM'][d, i])], 0.0)
            u = S['UTIL'][d, i]
            if 'ln util' in W:
                s += W['ln util'] * (0.0 if np.isnan(u) else math.log(max(u, 1e-6)))
            s += W.get('util missing', 0.0) * (1.0 if np.isnan(u) else 0.0)
            # the old score's components: the old engine's own values for the row (the format doc)
            s += W.get('ln learn mult', 0.0) * S['LRN'][d, i, 0] + W.get('ln learner', 0.0) * S['LRN'][d, i, 1]
            s += W.get('eligible', 0.0) * S['ELIG'][d, i]
            v = S['V2'][d, i]
            if not np.isnan(v[0]) and not np.isnan(u):
                s += sum(W.get('ln ' + n, 0.0) * math.log(max(v[k], 1e-6)) for k, n in ((3, 'fav'), (1, 'corr'), (2, 'potion')))
            sl = int(S['SLOT'][d, i])
            if 0 <= sl < 8:
                s += W.get(f'slot {sl}', 0.0)
            logit[d, i] = s - doc['kappa'] * S['INHA'][d, i]
    return logit, u0


def with_learner(S, seed=13):
    """S with the v2-derived arrays an old engine would give (fitlib.build's LRN, ELIG, V2): a
    learner multiplier on the scored rows, the carried-forward learner state, eligibility and
    the fav / corr / potion multipliers."""
    rng = np.random.default_rng(seed)
    S = dict(S)
    Dn, R = S['CAPID'].shape
    scored = rng.random((Dn, R)) < 0.7
    S['UTIL'] = np.where(scored, S['UTIL'], np.nan)
    S['LRN'] = np.stack([np.where(scored, rng.uniform(0, 1.5, (Dn, R)), 0.0),
                         np.where(rng.random((Dn, R)) < 0.5, rng.uniform(0, 1.5, (Dn, R)), 0.0)], axis=-1)
    S['ELIG'] = (rng.random((Dn, R)) < 0.8).astype(float)
    V2 = np.full((Dn, R, 4), np.nan)
    V2[scored] = rng.uniform(0.5, 2.5, (int(scored.sum()), 4))
    S['V2'] = V2
    return S


class Export(unittest.TestCase):
    def doc(self, spec, three=False, rec=False, log_kinds=None, cross=False, hold=()):
        S = with_learner(synth_S(600, seed=7, nothing=0.3 if three else 0.0, rec=rec))
        if cross:
            S = with_cross_and_slots(S)
        Sv, M, p, f, it = export.fit_all(S, SYN_OBV, spec, maxiter=150, hold=hold)
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
        # the injected record is written under its index owner, never its defining plugin
        self.assertEqual(by[('Light.esp', '000ABC')]['name'], 'Potion')
        self.assertNotIn('light - patch.esp', {it['plugin'].lower() for it in d['items']})
        self.assertTrue(any('injected' in m and 'FE012ABC (Light - Patch.esp in Light.esp)' in m for m in msgs))
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
        for ok in (['learn'], ['learn', 'mults'], ['learn_live'], ['learner'], ['astar', 'pos']):
            export.check_spec({'outside': False, 'feat_spec': ok})
        for bad in ({'feat_spec': ['ctx']}, {'feat_spec': ['cheap']}, {'_with': ['split']}, {'_latest': True},
                    {'bias_need': True, '_with': ['presence+strength']},
                    {'theta': False, 'item': True, 'outside': False}):        # cv.py 'b only': no theta to carry
            with self.assertRaises(SystemExit):
                export.check_spec(bad)

    def assert_scores_round_trip(self, spec, three=False, rec=False):
        """The written file, read back as JSON and scored with the format doc's formula by an
        independent reader, gives the fitted model's logits (and u0) on every decision."""
        S, Sv, M, p, d, _ = self.doc(spec, three=three, rec=rec, cross=True)
        self.assertEqual(bootstrap.validate(d), [])
        doc = json.loads(bootstrap.dumps(d))
        Fm, _ = fitlib.row_features(Sv, M.feat_spec)
        _, logit_m, u0_m = M.scores(Sv, p, Fm)
        logit_r, u0_r = doc_scores(doc, S, SYN_FORMS)
        # the injected Potion (FE012ABC) is carried and scored with its b (the reader finds it via Light.esp)
        if M.item:
            self.assertIn(('Light.esp', '000ABC'), {(it['plugin'], it['local']) for it in doc['items']})
        # an item the file does not carry reads b = 0 in the reader
        kept = {(it['plugin'], it['local']) for it in doc['items']}
        q = M.unpack(p)
        if M.item:
            for f, i in S['items'].items():
                key, _ = bootstrap.item_key(f, SYN_FORMS)
                if key not in kept:
                    logit_m = np.where(S['ITEM'] == i, logit_m - q['b'][i], logit_m)
        fin = np.isfinite(logit_m)
        np.testing.assert_array_equal(fin, np.isfinite(logit_r))
        np.testing.assert_allclose(logit_r[fin], logit_m[fin], rtol=0, atol=2e-4 * max(1.0, np.abs(logit_m[fin]).max()))
        if three:
            np.testing.assert_allclose(u0_r, u0_m, rtol=0, atol=2e-4 * max(1.0, np.abs(u0_m).max()))
        return d

    def test_scores_round_trip_graded_recent_features(self):
        d = self.assert_scores_round_trip({'outside': False, 'feat_spec': ['astar', 'pos'], '_with': ['recent']}, rec=True)
        self.assertEqual([f['name'] for f in d['features']][:3], ['ln util', 'util missing', 'slot 0'])

    def test_scores_round_trip_old_score_components(self):
        """D2 + D5's multipliers, and L1: the feature names the format lists, scored by the reader."""
        d = self.assert_scores_round_trip({'outside': False, 'feat_spec': ['learn', 'mults'], '_with': ['recent']}, rec=True)
        self.assertEqual([f['name'] for f in d['features']],
                         ['ln learn mult', 'util missing', 'ln fav', 'ln corr', 'ln potion'])
        d = self.assert_scores_round_trip({'outside': False, 'feat_spec': ['learner']})
        self.assertEqual([f['name'] for f in d['features']], ['ln learner', 'eligible'])

    def test_hold_needs_keeps_theta_rows_at_the_prior(self):
        for spec in ({'outside': False, '_with': ['recent']}, {'outside': False, '_with': ['presence+strength']}):
            S, Sv, M, p, d, _ = self.doc(spec, rec=True, hold=['hunger'])
            q = M.unpack(p)
            k = SYN_NEEDS.index('hunger')
            np.testing.assert_array_equal(q['theta'][k], M.theta0[k])
            np.testing.assert_allclose(d['theta_precision'][k], 1.0 / M.sig[k, :len(SYN_COLS)] ** 2, rtol=1e-5)
            self.assertFalse(np.allclose(q['theta'][0], M.theta0[0]))        # the others are fitted
            if d['theta_strength'] is not None:
                self.assertEqual(d['theta_strength'][k], [0.0] * len(SYN_COLS))
        with self.assertRaises(SystemExit):
            export.hold_mask(synth_S(30), ['not_a_need'])
        self.assertIsNone(export.hold_mask(synth_S(30), []))

    def test_provenance_is_written_last_and_unchecked(self):
        *_, d, _ = self.doc({'outside': False})
        doc = dict(d)
        doc.pop('created_utc')
        prov = {'chosen_by': 'R6 gate', 'metrics': {'menu': [0.5, 0.4, 0.6]}}
        d2 = bootstrap.make_document(source=doc['source'], spec='x', outcomes='picks', grading='graded',
                                     needs=doc['needs'], cols=doc['cols'], theta=doc['theta'],
                                     theta_precision=doc['theta_precision'], kappa=doc['kappa'], provenance=prov)
        self.assertEqual(list(d2)[-1], 'provenance')
        self.assertEqual(bootstrap.validate(json.loads(bootstrap.dumps(d2))), [])
        self.assertEqual(json.loads(bootstrap.dumps(d2))['provenance'], prov)

    def test_scores_round_trip_presence_strength_three(self):
        self.assert_scores_round_trip({'_with': ['presence+strength', 'recent']}, three=True, rec=True)

    def test_scores_round_trip_bias_need_three(self):
        """A bias need's u0 weight is folded into u0.bias (the bias need is a constant 1)."""
        d = self.assert_scores_round_trip({'bias_need': True, '_with': ['presence']}, three=True)
        self.assertIsNotNone(d['theta_bias'])

    def test_build_range(self):
        meta = [{'build': b} for b in ('0.23.44 (bbbbbbb)', '0.23.41 (fff)', '0.23.44 (aaaaaaa)', 'dev', '0.23.44 (bbbbbbb)')]
        # the same version twice: ordered by when each was logged, not by the hash
        self.assertEqual(export.build_range(meta), ['dev', '0.23.44 (bbbbbbb)'])
        self.assertEqual(export.build_range(meta[:3]), ['0.23.41 (fff)', '0.23.44 (aaaaaaa)'])


def v2_record(utc, form, char, cands, launch='20261011-100000', gen=2):
    """One v2 record (Huginn_Selections.jsonl) with the candidate columns load_v2 reads; cands:
    {form: dict of the learner's values} (the rest of the columns get neutral values)."""
    cols = ['form', 'type', 'need', 'rank', 'util', 'ctx', 'est', 'prior', 'ucb', 'alpha', 'learn', 'lambda', 'rec',
            'corr', 'potion', 'fav', 'wildcard', 'coldStart', 'pred']
    rows = []
    for k, (f, c) in enumerate(cands.items()):
        v = dict(form=f, type='Potion', need='HealthAny', rank=k, util=1.0, ctx=0.5, est=0.0, prior=0.0, ucb=0.0,
                 alpha=0.0, learn=0.0, **{'lambda': 0.0}, rec=0.0, corr=1.0, potion=1.0, fav=1.0, wildcard=0,
                 coldStart=0, pred=0.0)
        v.update(c)
        rows.append([v[x] for x in cols])
    return json.dumps({'v': 2, 'utc': utc, 'launch': launch, 'list': 'py', 'char': char, 'gen': gen, 'form': form,
                       'cols': cols, 'cands': rows})


# A row the old floors dropped, as a 0.24.0..0.24.18 writer logged it: ScoreBreakdown's defaults
# (src/learning/ScoredCandidate.h), util 0 (fitlib.v2_unscored skips it).
V2_DROPPED = dict(util=0, ctx=0, est=0, prior=0, ucb=0, alpha=0, learn=0, rec=0, corr=0, potion=1, fav=1,
                  **{'lambda': 0})


class LearnerFeatures(TempWork):
    """The old score's components (fitlib.row_features 'learn', 'mults', 'learner'; the R8
    component test): read from the v2 log, joined per pick, carried forward per item."""

    def setUp(self):
        super().setUp()
        self._v2env = os.environ.pop('HUGINN_FIT_V2', None)

    def tearDown(self):
        if self._v2env is not None:
            os.environ['HUGINN_FIT_V2'] = self._v2env
        super().tearDown()

    def dataset(self, launches=1, edit=None, dropped=False):
        """edit: applied to every v3 record (edit_lines); dropped: the key pick's v2 record also
        holds the sword as a 0.24.0..0.24.18 writer logged a row the old floors dropped."""
        # launch 1: key 0003EAE3 at 15:01:01 (its v2 record joins), menu 00099999, wheel 00012EB7 at 15:01:04
        text = ''.join(session(f'2026101{i}-100000', CHAR_A, '0.23.50 (abc)', minute=i) for i in range(1, launches + 1))
        if edit:
            text = edit_lines(text, edit)
        self.quiet(v3load.main, [str(self.write('v3.jsonl', text))])
        key_cands = {'0003EAE3': {'lambda': 2.0, 'learn': 0.5, 'alpha': 0.5, 'est': 0.6, 'rec': 0.1, 'fav': 2.5,
                                  'corr': 1.3, 'potion': 0.8}}
        if dropped:
            key_cands['00012EB7'] = V2_DROPPED
        lines = [
            # an earlier record of this character holds the sword (not a candidate of the key pick's record)
            v2_record('2026-10-09 14:59:00.000', 'AAAAAAAA', CHAR_A,
                      {'00012EB7': {'lambda': 1.0, 'alpha': 0.5, 'est': 0.4, 'rec': 0.3}}),
            # a later one of ANOTHER character: never carried forward to this one
            v2_record('2026-10-09 15:00:59.000', 'BBBBBBBB', CHAR_B,
                      {'00012EB7': {'lambda': 9.0, 'alpha': 1.0, 'est': 5.0}}),
            # the key pick's own record, 0.5 s after it
            v2_record('2026-10-09 15:01:01.500', '0003EAE3', CHAR_A, key_cands),
        ]
        self.write('Huginn_Selections.jsonl', '\n'.join(lines) + '\n')     # beside the v3 file: v2_path finds it
        D = common.load()
        v2 = fitlib.load_v2()
        return fitlib.build(D, include_nothing=False, v2=v2), v2

    def row(self, S, d, form):
        i = S['items'][form]
        return int(np.where(S['MASK'][d] & (S['ITEM'][d] == i))[0][0])

    def test_join_carry_forward_and_char(self):
        S, _ = self.dataset()
        key = [d for d in range(len(S['Y'])) if S['OUT'][d] == 'key'][0]
        wheel = [d for d in range(len(S['Y'])) if S['OUT'][d] == 'wheel'][0]
        pot, sword = self.row(S, key, '0003EAE3'), self.row(S, key, '00012EB7')
        # D2 on the scored, joined row: ln(1 + lambda learn)
        self.assertAlmostEqual(S['LRN'][key, pot, 0], math.log(1 + 2.0 * 0.5))
        # L1 on it: ln(1 + lambda (alpha est + rec)), rec from the pick's own record
        self.assertAlmostEqual(S['LRN'][key, pot, 1], math.log(1 + 2.0 * (0.5 * 0.6 + 0.1)))
        # the sword: no utility (not scored) -> D2 0; L1 from the earlier record of the same
        # character (not the other character's later one), recency 0 (not in this pick's record)
        self.assertEqual(S['LRN'][key, sword, 0], 0.0)
        self.assertAlmostEqual(S['LRN'][key, sword, 1], math.log(1 + 1.0 * (0.5 * 0.4)))
        # the wheel pick (the sword) has no v2 record: its rows are carried forward, no D2 term
        self.assertFalse(S['v2ok'][wheel])
        ws = self.row(S, wheel, '00012EB7')
        self.assertEqual(S['Y'][wheel], ws)
        self.assertEqual(S['LRN'][wheel, ws, 0], 0.0)
        self.assertAlmostEqual(S['LRN'][wheel, ws, 1], math.log(1 + 1.0 * (0.5 * 0.4)))
        # a form no record holds reads 0; the eligible flag is the row's
        self.assertEqual(S['LRN'][key, self.row(S, key, '0001397D')].tolist(), [0.0, 0.0])
        self.assertEqual(S['ELIG'][key, pot], 1.0)
        self.assertEqual(S['ELIG'][key, self.row(S, key, '0003EAE5')], 0.0)

    def test_row_features(self):
        S, _ = self.dataset()
        key = [d for d in range(len(S['Y'])) if S['OUT'][d] == 'key'][0]
        pot, sword = self.row(S, key, '0003EAE3'), self.row(S, key, '00012EB7')
        Fm, names = fitlib.row_features(S, ['learn', 'mults'])
        self.assertEqual(names, ['ln learn mult', 'util missing', 'ln fav', 'ln corr', 'ln potion'])
        np.testing.assert_allclose(Fm[key, pot], [math.log(2.0), 0.0, math.log(2.5), math.log(1.3), math.log(0.8)])
        np.testing.assert_allclose(Fm[key, sword], [0.0, 1.0, 0.0, 0.0, 0.0])
        Fm, names = fitlib.row_features(S, ['learner'])
        self.assertEqual(names, ['ln learner', 'eligible'])
        np.testing.assert_allclose(Fm[key, sword], [math.log(1.2), 1.0])

    def test_learner_before_dynamic_forms_stay_in_their_load(self):
        v2 = fitlib.V2Index()
        c = lambda est: (0.5, 1, 1, 1, 'x', 1.0, 1.0, 0.0, est, 1.0, 0.0, 0.0)
        v2.appear['FF000801'] = [(common.T('2026-10-09 15:00:00.000'), 'L1', 1, CHAR_A, c(0.1)),
                                 (common.T('2026-10-09 15:00:10.000'), 'L2', 1, CHAR_A, c(0.2))]
        t = common.T('2026-10-09 15:00:20.000')
        self.assertEqual(fitlib.learner_before(v2, 'FF000801', t, 'L1', 1, CHAR_A)[8], 0.1)   # skips L2's
        self.assertIsNone(fitlib.learner_before(v2, 'FF000801', t, 'L3', 1, CHAR_A))
        self.assertIsNone(fitlib.learner_before(v2, 'FF000801', t, 'L1', 2, CHAR_A))          # another load
        self.assertIsNone(fitlib.learner_before(v2, 'FF000801', common.T('2026-10-09 15:00:00.000'), 'L1', 1, CHAR_A))
        self.assertIsNone(fitlib.learner_before(v2, '00012EB7', t, 'L1', 1, CHAR_A))
        self.assertIsNone(fitlib.learner_before({}, '00012EB7', t, 'L1', 1, CHAR_A))           # no v2 log

    def test_no_v2_log_reads_zero(self):
        self.quiet(v3load.main, [str(self.write('v3.jsonl', session('20261011-100000', CHAR_A, '0.23.50 (abc)')))])
        with contextlib.redirect_stderr(io.StringIO()):
            S = fitlib.build(common.load(), include_nothing=False, v2=fitlib.load_v2())
        self.assertFalse(S['LRN'].any())
        self.assertTrue(S['ELIG'].any())

    def test_data_key_covers_the_learner_features(self):
        S, _ = self.dataset()
        k = fitlib.data_key(S)
        self.assertNotEqual(fitlib.data_key(dict(S, LRN=S['LRN'] + 1)), k)
        self.assertNotEqual(fitlib.data_key(dict(S, ELIG=1 - S['ELIG'])), k)

    def test_dropped_rows_of_a_0_24_v2_record_are_not_scored(self):
        """0.24.0..0.24.18 wrote every ranked candidate into the v2 record, the ones the old floors
        dropped with an all-zero breakdown: load_v2 skips them, so the dataset is the one a record
        holding only the scored rows gives (no context weight, the learner carried forward)."""
        S, v2 = self.dataset(dropped=True)
        self.assertTrue(fitlib.v2_unscored(V2_DROPPED))
        self.assertFalse(fitlib.v2_unscored(dict(V2_DROPPED, corr=1.0)))    # a scored row never has corr 0
        self.assertEqual([list(c) for _, c in v2['0003EAE3']], [['0003EAE3']])
        self.assertEqual(len(v2.appear['00012EB7']), 2)                     # the two earlier records only
        S0, _ = self.dataset()
        for k in ('V2', 'LRN', 'CLS', 'v2ok'):
            np.testing.assert_array_equal(S[k], S0[k], err_msg=k)
        key = [d for d in range(len(S['Y'])) if S['OUT'][d] == 'key'][0]
        sword = self.row(S, key, '00012EB7')
        self.assertTrue(np.isnan(S['V2'][key, sword]).all())
        self.assertAlmostEqual(S['LRN'][key, sword, 1], math.log(1 + 1.0 * (0.5 * 0.4)))

    def test_learn_live_reads_what_the_live_score_read(self):
        """'learn_live' reads the v3 row's lm / um (what the live score read), 'learn' derives the
        same two columns from util and the v2 record; a row with no lm / um reads as 'learn'."""
        S, _ = self.dataset()
        key = [d for d in range(len(S['Y'])) if S['OUT'][d] == 'key'][0]
        pot, sword, ammo = (self.row(S, key, f) for f in ('0003EAE3', '00012EB7', '0001397D'))
        L, names = fitlib.row_features(S, ['learn'])
        Fm, live_names = fitlib.row_features(S, ['learn_live'])
        self.assertEqual(live_names, names)          # the same bootstrap weights (export.py)
        np.testing.assert_allclose(L[key, pot], [math.log(2.0), 0.0])
        np.testing.assert_allclose(Fm[key, pot], [0.125, 0.0])        # the synthetic row's lm, um
        np.testing.assert_allclose(Fm[key, ammo], [0.0, 1.0])
        np.testing.assert_allclose(Fm[key, sword], L[key, sword])     # no lm / um logged

    def test_learn_live_under_engine_b(self):
        """Engine B live with the D2 shadow on (0.24.17): the old floors kept the potion (util
        set) but the live score read 0 / 1. 'learn' reads A's values, 'learn_live' B's."""
        head = {}

        def b_live(r):
            if r['t'] == 'head':
                head['lm'], head['um'] = r['row'].index('lm'), r['row'].index('um')
            for row in r.get('rows', []) + r.get('add', []):
                if row[0] == '0003EAE3' and len(row) > head['um']:
                    row[head['lm']], row[head['um']] = 0, 1
        S, _ = self.dataset(edit=b_live)
        key = [d for d in range(len(S['Y'])) if S['OUT'][d] == 'key'][0]
        pot = self.row(S, key, '0003EAE3')
        self.assertFalse(np.isnan(S['UTIL'][key, pot]))
        np.testing.assert_allclose(fitlib.row_features(S, ['learn'])[0][key, pot], [math.log(2.0), 0.0])
        np.testing.assert_allclose(fitlib.row_features(S, ['learn_live'])[0][key, pot], [0.0, 1.0])

    def test_learn_live_before_0_23_56_reads_as_learn(self):
        def older(r):                # a 0.23.55 writer: no lm / um
            if r['t'] == 'head':
                r['row'] = r['row'][:22]
            elif r['t'] == 'ctx':
                r['rows'] = [row[:22] for row in r['rows']]
            elif r['t'] == 'dec':
                r['add'] = [row[:22] for row in r['add']]
        S, _ = self.dataset(edit=older)
        self.assertTrue(np.isnan(S['LMUM']).all())
        np.testing.assert_array_equal(fitlib.row_features(S, ['learn_live'])[0], fitlib.row_features(S, ['learn'])[0])

    def test_component_specs_fit(self):
        S, _ = self.dataset(launches=3)
        obv = fitlib.obvious_pairs(S['needs'], S['cols'])
        FOLD = np.zeros(len(S['Y']), dtype=int)
        for name in ('theta + b + recent + learn mult (D2)', 'theta + b + recent + learn mult + fav/corr/potion (D5)',
                     'theta + b + recent + old learner state (L1)',
                     'theta + b + recent + learn mult as the live score read it (lm/um, B)'):
            spec = cv.PICKS[name]
            res, fits = fitlib.online_results(S, obv, spec, every=1, min_train=1, maxiter=10)
            self.assertGreater(fits, 0, name)
            self.assertEqual(len(res), len(S['Y']))
            self.assertEqual(len(fitlib.cv_results(S, obv, spec, FOLD, maxiter=10)), len(S['Y']))


class ExportMain(TempWork):
    def test_end_to_end_on_a_log(self):
        text = ''.join(session(f'2026101{i}-100000', CHAR_A, '0.23.50 (abc)', minute=i) for i in range(1, 4))
        self.quiet(v3load.main, [str(self.write('v3.jsonl', text))])
        forms = self.write('forms.csv', 'kind,formID,plugin,name\n'
                                        'Potion,0003EAE3,Skyrim.esm,Potion A\nWeapon,00012EB7,Skyrim.esm,Sword\n'
                                        'Ammo,0001397D,Skyrim.esm,Bolt\nAmmo,0201397D,Dawnguard.esm,Bolt 2\n'
                                        'Potion,0003EAE5,Skyrim.esm,Potion B\n'
                                        'Armor,00099999,Skyrim.esm,Robe\n')
        cj = self.write('curve.json', json.dumps({'need': 'magicka_deficit', 'kind': 'logistic', 'centre': 0.5, 'slope': 10}))
        doc, out = self.quiet(export.main, ['--forms', str(forms), '--curve', str(cj), '--maxiter', '30'])
        self.assertIn('wrote', out)
        written = json.loads((self.dir / 'bootstrap.json').read_text(encoding='utf-8'))
        self.assertEqual(bootstrap.validate(written), [])
        self.assertEqual(written['model']['spec'], 'theta + b + recent_<family>')
        self.assertEqual(written['source']['char'], CHAR_A)
        self.assertEqual({(i['plugin'], i['local']) for i in written['items']},
                         {('Skyrim.esm', '03EAE3'), ('Skyrim.esm', '012EB7'), ('Skyrim.esm', '01397D'),
                          ('Skyrim.esm', '03EAE5'), ('Skyrim.esm', '099999')})
        self.assertEqual(written['curves'], {'magicka_deficit': ['logistic', 0.5, 10.0]})
        self.assertTrue(written['recent']['features'])
        with self.assertRaises(SystemExit):
            self.quiet(export.main, ['--forms', str(forms), '--out', '../x.json'])


class HonestMain(TempWork):
    """honest.main on a tiny log: the report, the caches (reused on a second run, refitted when
    the data or --maxiter changes) and cv.py's pickle (reused only for the same data_key, folds,
    seed and maxiter)."""

    def log(self, n):
        text = ''.join(session(f'2026101{i}-100000', CHAR_A, '0.23.50 (abc)', minute=i) for i in range(1, n + 1))
        self.quiet(v3load.main, [str(self.write('v3.jsonl', text))])

    ARGS = ['picks', '--spec', 'theta + b (doc 9)', '--every', '2', '--min-train', '1', '--folds', '2', '--nboot', '20']

    def test_end_to_end(self):
        self.log(4)
        r, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '20'])
        self.assertIn('theta + b (doc 9): CV fitted, online fitted', out)
        self.assertIn('=== R6 gate', out)
        self.assertTrue((self.dir / 'honest_picks.txt').exists() and (self.dir / 'honest_picks.pkl').exists())
        self.assertEqual(len(list(self.dir.glob('honest_cache_*.pkl'))), 2)
        self.assertTrue(r['tested'].any())
        for meth in honest.METHODS:
            self.assertIn(('theta + b (doc 9)', meth), r['verdicts'])
        # the same run again: both from the cache, the same verdicts
        r2, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '20'])
        self.assertIn('theta + b (doc 9): CV cache, online cache', out)
        self.assertEqual(r2['verdicts'], r['verdicts'])
        # another --maxiter: refitted
        _, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '21'])
        self.assertIn('CV fitted, online fitted', out)
        # cv.py's pickle: reused at its maxiter (300), not at another
        self.quiet(cv.main, ['picks', '--folds', '2', '--spec', 'theta + b (doc 9)', '--nboot', '20'])
        _, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '300'])
        self.assertIn('CV cv pickle', out)
        _, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '20'])
        self.assertIn('CV cache', out)
        # a rebuilt v3.pkl (one more launch, other data): the caches and the cv pickle are not reused
        self.log(5)
        _, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '300'])
        self.assertIn('CV fitted, online fitted', out)

    def test_stale_cv_pickle_is_not_reused(self):
        self.log(3)
        self.quiet(cv.main, ['picks', '--folds', '2', '--spec', 'theta + b (doc 9)', '--nboot', '20'])
        with open(self.dir / 'cv_picks.pkl', 'rb') as f:
            C = pickle.load(f)
        C['data_key'] = 'something else'                  # as a pickle of other data, or of older fit code
        with open(self.dir / 'cv_picks.pkl', 'wb') as f:
            pickle.dump(C, f)
        _, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '300', '--no-cache'])
        self.assertIn('CV fitted', out)
        self.assertEqual(list(self.dir.glob('honest_cache_*.pkl')), [])

    def test_redefined_spec_is_not_reused(self):
        """cv_<group>.pkl's results are reused by the spec's definition, not its name: a spec
        changed in cv.py under the same name, or a pickle without the definitions, is refitted."""
        self.log(3)
        self.quiet(cv.main, ['picks', '--folds', '2', '--spec', 'theta + b (doc 9)', '--nboot', '20'])
        with open(self.dir / 'cv_picks.pkl', 'rb') as f:
            C = pickle.load(f)
        self.assertEqual(C['specs']['theta + b (doc 9)'], cv.spec_key(cv.PICKS['theta + b (doc 9)']))
        _, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '300', '--no-cache'])
        self.assertIn('CV cv pickle', out)
        for defs in ({'theta + b (doc 9)': cv.spec_key({'outside': False, 'feat_spec': ['pos']})}, None):
            C2 = dict(C)
            if defs is None:
                del C2['specs']                           # written before the definitions were stored
            else:
                C2['specs'] = defs                        # the spec was something else when cv.py ran
            with open(self.dir / 'cv_picks.pkl', 'wb') as f:
                pickle.dump(C2, f)
            _, out = self.quiet(honest.main, self.ARGS + ['--maxiter', '300', '--no-cache'])
            self.assertIn('CV fitted', out)


class CurveMain(TempWork):
    def test_end_to_end_then_export(self):
        text = ''.join(session(f'2026101{i}-100000', CHAR_A, '0.23.50 (abc)', minute=i) for i in range(1, 4))
        self.quiet(v3load.main, [str(self.write('v3.jsonl', text))])
        fr, out = self.quiet(curve.main, ['--need', 'magicka_deficit', '--col', 'restore_magicka', '--spec',
                                          'theta + b (doc 9)', '--rounds', '1', '--quick-profile'])
        self.assertIn('[Needs] line: magicka_deficit = logistic', out)
        self.assertIn('profile over the centre', out)
        js = json.loads((self.dir / 'curve_magicka_deficit.json').read_text(encoding='utf-8'))
        self.assertEqual((js['need'], js['kind']), ('magicka_deficit', 'logistic'))
        self.assertAlmostEqual(js['centre'], fr['centre'], places=6)
        self.assertTrue((self.dir / 'curve_magicka_deficit.txt').exists())
        forms = self.write('forms.csv', 'kind,formID,plugin,name\nPotion,0003EAE3,Skyrim.esm,Potion A\n')
        doc, _ = self.quiet(export.main, ['--forms', str(forms), '--curve', str(self.dir / 'curve_magicka_deficit.json'),
                                          '--spec', 'theta + b (doc 9)', '--maxiter', '20'])
        self.assertEqual(doc['curves']['magicka_deficit'][0], 'logistic')
        self.assertAlmostEqual(doc['curves']['magicka_deficit'][1], js['centre'], places=4)
        with self.assertRaises(SystemExit):
            self.quiet(curve.main, ['--need', 'no_such_need', '--rounds', '1'])


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
