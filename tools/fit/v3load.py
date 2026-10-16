"""Lean loader for Huginn_Selections_v3.jsonl -> compact pickle (v3.pkl in the work
directory, common.py). The first step of every other script here.

usage: python v3load.py <path/to/Huginn_Selections_v3.jsonl>
Columns/needs are mapped by NAME to a global index (union over heads), so the
0.23.16 (+4 cols), 0.23.19 (+deep_water_ahead), 0.23.35 (+combat_regen_ok)
schema additions are handled. Caps are deduped globally by content."""
import json, pickle, sys
from collections import Counter
from pathlib import Path

from common import work

def load(path):
    cols, needs, cross = [], [], []
    colix, needix, crossix = {}, {}, {}
    capstore, capkey = [], {}          # global cap id -> tuple((col,val)...)
    heads, ctxs, decs = [], {}, []
    stats = Counter()
    head = None
    def gix(names, table, lst):
        out = []
        for n in names:
            if n not in table:
                table[n] = len(lst); lst.append(n)
            out.append(table[n])
        return out
    for raw in open(path, 'rb'):
        try:
            r = json.loads(raw)
        except Exception:
            stats['bad'] += 1; continue
        t = r.get('t')
        if t == 'head':
            head = r
            hcol = gix(r['cols'], colix, cols)
            hneed = gix(r['needs'], needix, needs)
            hcross = gix(r['cross'], crossix, cross)
            segcaps = {}
            seg = len(heads)
            heads.append({k: r[k] for k in ('launch', 'list', 'build', 'kinds', 'src', 'row', 'flags', 'episode') if k in r} | {'ncols': len(r['cols']), 'nneeds': len(r['needs'])})
            kinds, srcs = r['kinds'], r['src']
        elif t == 'cap':
            c = tuple(sorted((hcol[i], v) for i, v in r['c']))
            if c not in capkey:
                capkey[c] = len(capstore); capstore.append(c)
            segcaps[r['id']] = capkey[c]
        elif t == 'ctx':
            rows = []
            for f in r['rows']:
                form, uid, kind, src, cap, flags, slot, util, x, wp = f
                rows.append((form, uid, None if kind is None else kinds[kind], None if src is None else srcs[src],
                             -1 if cap == -1 else segcaps[cap], flags, slot, util,
                             tuple((hcross[i], v) for i, v in x), wp))
            key = (head['launch'], r['id'])
            ctxs[key] = {'seg': seg, 'id': r['id'], 'utc': r['utc'], 'why': r['why'], 'menu': r.get('menu'),
                         'need': {hneed[i]: v for i, v in r['need']}, 'in': {hneed[i]: v for i, v in r['in']},
                         'pipe': r.get('pipe'), 'race': r.get('race'), 'heldFull': r.get('heldFull'),
                         'wc': r.get('wc'), 'rows': rows}
        elif t == 'dec':
            d = {k: v for k, v in r.items() if k not in ('t', 'add')}
            d['seg'] = seg
            d['build'] = head['build']
            add = []
            for f in r['add']:
                form, uid, kind, src, cap, flags, slot, util, x, wp = f
                add.append((form, uid, None if kind is None else kinds[kind], None if src is None else srcs[src],
                            -1 if cap == -1 else segcaps[cap], flags, slot, util,
                            tuple((hcross[i], v) for i, v in x), wp))
            d['add'] = add
            d['ctxkey'] = (head['launch'], r['ctx']) if r.get('ctx') is not None else None
            if d['ep'] and 'i' in d['ep']:
                d['ep']['gi'] = hneed[d['ep']['i']]
            d['open'] = [hneed[i] for i in r.get('open', [])]
            decs.append(d)
        stats[t] += 1
    return dict(cols=cols, needs=needs, cross=cross, caps=capstore, heads=heads, ctxs=ctxs, decs=decs, stats=stats,
                source=str(Path(path).resolve()))

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    D = load(sys.argv[1])
    print(D['stats'], len(D['cols']), len(D['needs']), len(D['cross']), len(D['caps']), 'caps', len(D['ctxs']), 'ctxs')
    with open(work('v3.pkl'), 'wb') as f:
        pickle.dump(D, f, protocol=5)
    print('wrote', work('v3.pkl'))
