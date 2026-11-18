"""The D2 engine in shadow (0.24.17): would engine A's page have had what the player picked?

From 0.24.17 the live engine is B (the choice score without the D2 legacy feature, [Scoring]
bLegacyLearnFeature = 0) and D2 ("A") runs in shadow beside it ([Scoring] bShadowLegacy = 1):
every pipeline run also ranks the candidates under A and builds A's page and B's page WITHOUT
memory (no slot hold, seating, Remembrance hold, wildcard or lock) from the live allocation's
input. The selection log v3 carries, on each eligible row, A's mu (`amu`), the row's rank under
A and under B (`ar`, `br`, 1 = first) and its key on A's and on B's stateless page (`ak`, `bk`,
-1 = off); docs/architecture/9-selection-log-v3.md, "The D2 engine in shadow".

The fair comparison is the menu reach-in: the player opened the inventory, magic or favourites
menu because the page lacked the item. "Would A's page have had it" is `ak >= 0` on the chosen
row. B's stateless page (`bk`) is the control: the two stateless pages differ only by the
ranking, while the live page (the `shown` flag) also carries the memory (holds, Remembrance,
the wildcard, the locks), so A-vs-live mixes D2 with statelessness. Key and wheel picks are the
other side: an item the live page had -- did A's page have it too?

Per pick (key, wheel, menu; repeats left out) whose context carries the shadow (some row of it
has `ak`): on A's page, on B's stateless page, on A's only, on B's only, on the live page; the
chosen item's rank under A against under B. Split by context: in combat or not (the context's
need in_combat >= 0.5, as the need vector logs it), and by the chosen item's kind. The user
judges by feel; these numbers are context, not a gate.

usage:  python -I tools/shadow/shadow_compare.py <v3 file> [<v3 file> ...] [--char ID]
        (oldest first, as tools/replay/replay.py reads them; .jsonl or .jsonl.gz)

Standard library only (tools/replay/replay.py reads the log).
"""
import argparse
import importlib.util
import statistics
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True


def _load_replay():
    # python -I leaves the script's folder off sys.path: load replay.py by path.
    if 'replay' in sys.modules:
        return sys.modules['replay']
    spec = importlib.util.spec_from_file_location('replay', HERE.parent / 'replay' / 'replay.py')
    mod = importlib.util.module_from_spec(spec)
    sys.modules['replay'] = mod
    spec.loader.exec_module(mod)
    return mod


# The vias that mean a game menu (core/ReachLog.h IsMenuVia).
MENU_VIAS = ('inventory menu', 'favorites menu', 'magic menu', 'menu (just closed)')
SHOWN = 16
IN_COMBAT = 'in_combat'


def pick_class(d):
    """'key' (a Huginn key or the Huginn wheel), 'reach' (a menu reach-in) or None (nothing, a
    repeat, a vanilla hotkey or the player's own wheel: neither side of the comparison)."""
    if d.get('out') == 'nothing' or d.get('repeat') or d.get('chosen') is None:
        return None
    if d.get('out') in ('key', 'wheel'):
        return 'key'
    if d.get('out') == 'menu' and d.get('via') in MENU_VIAS:
        return 'reach'
    return None


def shadow_ran(d):
    """The run behind the pick's context ran the shadow: some row carries A's page key."""
    ctx = d.get('ctx') or {}
    return any(r.get('ak') is not None for r in ctx.get('rows', []))


class Cell:
    """One line of the table."""

    def __init__(self):
        self.n = self.on_a = self.on_b0 = self.a_only = self.b0_only = self.live = 0
        self.a_better = self.b_better = self.same = 0
        self.ar, self.br = [], []

    def add(self, c):
        # A row the run never ranked carries no key: off both pages.
        on_a = c.get('ak') is not None and c['ak'] >= 0
        on_b0 = c.get('bk') is not None and c['bk'] >= 0
        self.n += 1
        self.on_a += on_a
        self.on_b0 += on_b0
        self.a_only += on_a and not on_b0
        self.b0_only += on_b0 and not on_a
        self.live += bool(c.get('flags', 0) & SHOWN)
        ar, br = c.get('ar'), c.get('br')
        if ar is not None and br is not None:
            self.ar.append(ar)
            self.br.append(br)
            if ar < br:
                self.a_better += 1
            elif br < ar:
                self.b_better += 1
            else:
                self.same += 1

    def as_dict(self):
        return {'n': self.n, 'onA': self.on_a, 'onB0': self.on_b0, 'aOnly': self.a_only, 'b0Only': self.b0_only,
                'live': self.live, 'aBetter': self.a_better, 'bBetter': self.b_better, 'same': self.same,
                'medianAr': statistics.median(self.ar) if self.ar else None,
                'medianBr': statistics.median(self.br) if self.br else None}


def compare(decs):
    """{'cells': {(class, split): Cell.as_dict()}, 'noShadow': {class: n}}, where class is
    'key' or 'reach' and split 'all', 'combat', 'calm' or 'kind:<Kind>'."""
    cells = defaultdict(Cell)
    no_shadow = defaultdict(int)
    for d in decs:
        k = pick_class(d)
        if k is None:
            continue
        if not shadow_ran(d):
            no_shadow[k] += 1
            continue
        c = d['chosen']
        need = (d.get('ctx') or {}).get('need', {})
        combat = (need.get(IN_COMBAT) or 0) >= 0.5
        for split in ('all', 'combat' if combat else 'calm', f'kind:{c.get("kind") or "?"}'):
            cells[(k, split)].add(c)
    return {'cells': {key: cell.as_dict() for key, cell in cells.items()}, 'noShadow': dict(no_shadow)}


def _pct(a, n):
    return f'{a:4d} ({100.0 * a / n:3.0f}%)' if n else f'{a:4d}      '


def report(res, out=print):
    cells = res['cells']
    total = sum(c['n'] for (k, s), c in cells.items() if s == 'all')
    out(f'D2 shadow (0.24.17): {total} pick(s) with the shadow behind their context; without it: '
        + (', '.join(f'{k} {n}' for k, n in sorted(res['noShadow'].items())) or 'none'))
    if not total:
        out('nothing to compare (no 0.24.17+ picks with bShadowLegacy on and bLegacyLearnFeature off)')
        return
    out('')
    out(f'{"":28} {"picks":>5}  {"on A page":>11}  {"on B0 page":>11}  {"A only":>11}  {"B0 only":>11}  '
        f'{"on live":>11}  {"rank A<B / B<A / =":>18}  {"median ar br":>12}')
    titles = {'reach': 'menu reach-ins', 'key': 'key + wheel picks'}
    for k in ('reach', 'key'):
        splits = sorted((s for (kk, s) in cells if kk == k), key=lambda s: (s != 'all', s.startswith('kind:'), s))
        for s in splits:
            c = cells[(k, s)]
            label = titles[k] if s == 'all' else f'  {s}'
            med = (f'{c["medianAr"]:>5g} {c["medianBr"]:>5g}' if c['medianAr'] is not None else '-')
            out(f'{label:28} {c["n"]:5d}  {_pct(c["onA"], c["n"])}  {_pct(c["onB0"], c["n"])}  '
                f'{_pct(c["aOnly"], c["n"])}  {_pct(c["b0Only"], c["n"])}  {_pct(c["live"], c["n"])}  '
                f'{c["aBetter"]:5d} /{c["bBetter"]:5d} /{c["same"]:5d}  {med:>12}')
    out('')
    out('on A page / on B0 page: the chosen item sat on the stateless page of engine A (D2) / engine B (live')
    out('ranking, no memory). A reach-in caught by A only is one D2 would have saved; by B0 only, one it would')
    out('have lost. "on live": the page the player saw (a reach-in is off it by definition, unless the')
    out('menu pick was of an item the page showed). rank: the chosen item\'s place under each ranking.')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('paths', nargs='+', help='v3 file(s), oldest first')
    ap.add_argument('--char', help='keep one character (dec.char, 16 hex digits)')
    args = ap.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    replay = _load_replay()
    decs = replay.load_v3(args.paths)
    if args.char:
        decs = [d for d in decs if d.get('char') == args.char]
    res = compare(decs)
    report(res)
    return res


if __name__ == '__main__':
    main()
