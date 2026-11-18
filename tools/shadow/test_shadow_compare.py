"""Tests for tools/shadow/shadow_compare.py (0.24.17, the D2 engine in shadow).

A synthetic v3 log, written by the schema-doc encoder in tools/replay/test_replay_v3.py (not
the C++ one), with picks of every kind the comparison sorts: a key pick on both stateless
pages, menu reach-ins A's page held and B's did not (and the other way), a reach-in of an
item the run never ranked, outside picks that are not menus, a repeat, a nothing record, and
a reach-in whose context carries no shadow (an older build, or bShadowLegacy off).

Run:  python -I tools/shadow/test_shadow_compare.py     (exit 0 = pass)
"""
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True


def _load_by_path(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


trv3 = _load_by_path('test_replay_v3', HERE.parent / 'replay' / 'test_replay_v3.py')
sc = _load_by_path('shadow_compare', HERE / 'shadow_compare.py')

KEYS = trv3.OLD_KEYS + trv3.SHADOW_KEYS + trv3.SHADOW_A_KEYS


def row(form, flags, slot=-1, kind='Potion', **cols):
    r = {'form': form, 'uid': 0, 'kind': kind, 'src': None, 'cap': {'restore': 0.5}, 'flags': flags,
         'slot': slot, 'util': None, 'x': {}, 'wp': None, 'fb': None, 'cr': None}
    r.update({k: cols.get(k) for k in KEYS})
    return r


def dec(seq, out, ctx, chosen, form, via='', repeat=0):
    return {'seq': seq, 'utc': f'2026-10-30 12:00:{seq:02d}.000', 'launch': '20261030-120000', 'list': 'py',
            'char': '0000000000000001', 'gen': 1, 'out': out, 'form': form, 'name': None, 'row': chosen,
            'src': '', 'via': via, 'case': '', 'how': '', 'kind': '', 'confirmMs': 0, 'repeat': repeat,
            'learned': 1, 'skip': '', 'ctxAgeMs': 0, 'open': [], 'ep': None, 'ctx': ctx, 'add': []}


def session():
    head = trv3.make_head()
    E, S, H = 1, 16, 4   # eligible, shown, held
    fight = {'id': 1, 'utc': '2026-10-30 12:00:00.000', 'why': 'menu', 'need': {'in_combat': 1.0},
             'input': {'in_combat': 1.0}, 'pipe': {'ok': 1, 'page': 0, 'slots': 8, 'ageMs': 50}, 'race': None,
             'wc': {'base': 0.1, 'max': 0.3},
             'rows': [row('00000001', E | S | H, 0, amu=1.5, ar=1, br=1, ak=0, bk=0),          # on every page
                      row('00000002', E | H, amu=0.9, ar=3, br=9, ak=2, bk=-1),               # A's page only
                      row('00000003', E | H, kind='Spell', amu=-0.4, ar=12, br=5, ak=-1, bk=4),  # B's only
                      row('00000004', H)]}                                                     # never ranked
    calm = {'id': 2, 'utc': '2026-10-30 12:01:00.000', 'why': 'menu', 'need': {}, 'input': {},
            'pipe': {'ok': 1, 'page': 0, 'slots': 8, 'ageMs': 50}, 'race': None, 'wc': {'base': 0.1, 'max': 0.3},
            'rows': [row('00000005', E | H)]}   # no shadow columns at all
    decs = [dec(1, 'key', fight, 0, '00000001', 'key 1 (s0)'),
            dec(2, 'menu', fight, 1, '00000002', 'inventory menu'),
            dec(3, 'menu', fight, 2, '00000003', 'magic menu'),
            dec(4, 'menu', fight, 3, '00000004', 'favorites menu'),
            dec(5, 'menu', fight, 1, '00000002', 'vanilla hotkey'),     # not a menu: out
            dec(6, 'key', fight, 0, '00000001', 'key 1 (s0)', repeat=1),  # a repeat: out
            dec(7, 'menu', calm, 0, '00000005', 'inventory menu'),      # no shadow behind it
            dec(8, 'wheel', fight, 1, '00000002', 'Huginn wheel')]      # a wheel pick A's page had
    nothing = dec(9, 'nothing', fight, -1, None)
    nothing['ep'] = {'need': 'in_combat', 'i': head['needs'].index('in_combat'), 'durSec': 3.0, 'peak': 1.0,
                     'onset': '2026-10-30 12:00:00.000'}
    decs.append(nothing)
    return trv3.encode([(head, decs)])


class Compare(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'v3.jsonl'
            p.write_text(session(), encoding='utf-8')
            cls.decs = sc._load_replay().load_v3([p])
        cls.res = sc.compare(cls.decs)

    def test_classes(self):
        got = [sc.pick_class(d) for d in self.decs]
        self.assertEqual(got, ['key', 'reach', 'reach', 'reach', None, None, 'reach', 'key', None])

    def test_reach_ins(self):
        c = self.res['cells'][('reach', 'all')]
        self.assertEqual((c['n'], c['onA'], c['onB0'], c['aOnly'], c['b0Only'], c['live']), (3, 1, 1, 1, 1, 0))
        # ranks: item 2 (A 3 < B 9), item 3 (A 12 > B 5); item 4 has none
        self.assertEqual((c['aBetter'], c['bBetter'], c['same']), (1, 1, 0))
        self.assertEqual((c['medianAr'], c['medianBr']), (7.5, 7.0))
        self.assertEqual(self.res['cells'][('reach', 'combat')]['n'], 3)
        self.assertNotIn(('reach', 'calm'), self.res['cells'])   # the calm one has no shadow
        self.assertEqual(self.res['cells'][('reach', 'kind:Spell')]['b0Only'], 1)

    def test_key_picks(self):
        c = self.res['cells'][('key', 'all')]
        self.assertEqual((c['n'], c['onA'], c['onB0'], c['live']), (2, 2, 1, 1))
        self.assertEqual(c['aOnly'], 1)

    def test_no_shadow(self):
        self.assertEqual(self.res['noShadow'], {'reach': 1})

    def test_report_runs(self):
        buf = io.StringIO()
        sc.report(self.res, out=lambda s: buf.write(s + '\n'))
        text = buf.getvalue()
        self.assertIn('menu reach-ins', text)
        self.assertIn('key + wheel picks', text)
        buf = io.StringIO()
        sc.report(sc.compare([]), out=lambda s: buf.write(s + '\n'))
        self.assertIn('nothing to compare', buf.getvalue())

    def test_older_log_reads(self):
        # The 0.24.16 golden file: no shadow anywhere, every pick counted as without it.
        old = trv3.GOLDEN_0_24_16
        res = sc.compare(sc._load_replay().load_v3([old]))
        self.assertEqual(res['cells'], {})
        self.assertGreater(sum(res['noShadow'].values()), 0)


if __name__ == '__main__':
    unittest.main()
