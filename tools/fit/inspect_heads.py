"""Print every head of a v3 file and what changed between consecutive heads
(columns, needs, cross-features, kinds, sources, row columns).

usage: python inspect_heads.py <path/to/Huginn_Selections_v3.jsonl>"""
import json,sys,collections
if len(sys.argv) != 2:
    sys.exit(__doc__)
heads=[];types=collections.Counter()
prev=None
for line in open(sys.argv[1],'rb'):
    try: r=json.loads(line)
    except Exception: types['BAD']+=1; continue
    types[r.get('t')]+=1
    if r.get('t')=='head':
        key=(len(r['cols']),len(r['needs']),len(r['cross']),tuple(r['row']))
        print(r['launch'],r['list'],r['build'],key[:3],len(r['row']), r.get('episode'))
        if prev:
            for k in ('cols','needs','cross','kinds','src','row'):
                if prev[k]!=r[k]:
                    a,b=prev[k],r[k]
                    print('   CHANGED',k,'added',[x for x in b if x not in a][:20],'removed',[x for x in a if x not in b][:20], 'reorder' if [x for x in a if x in b]!=[x for x in b if x in a] else '')
        prev=r
print(types)
