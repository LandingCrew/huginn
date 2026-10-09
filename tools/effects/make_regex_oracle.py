"""tests/core/fixtures/regex_oracle.csv: Python `re` results for every rule-table pattern
(huginn_effect_report --patterns-out) on a corpus of real effect names, normalised
descriptions and keyword lists taken from the effect fixtures, non-ASCII ones included.

MiniRegex is byte-based with ASCII classes, so the oracle is Python's `re` on BYTES (UTF-8
text, ASCII-only lower-casing): the same semantics, which `re` on str is not (its \\w is
Unicode). usage: python -I tools/effects/make_regex_oracle.py <repo> <patterns.txt>"""
import sys, csv, re, random
repo, pat_file = sys.argv[1], sys.argv[2]
random.seed(7)
patterns = [l.rstrip('\n') for l in open(pat_file, encoding='utf-8') if l.strip()]


def lower_ascii(b):
    return b.lower()  # bytes.lower() lower-cases ASCII only, like the C++ Lower()


def norm(b):
    t = lower_ascii(b)
    t = re.sub(rb'<[^>]*>', b'N', t)
    t = re.sub(rb'\b\d+(\.\d+)?', b'N', t)
    return t


corpus = set()
for lst in ('vanilla', 'simonrim', 'lorerim'):
    for r in csv.DictReader(open(f'{repo}/tests/core/fixtures/effects_{lst}.csv', encoding='utf-8')):
        corpus.add(lower_ascii(r['effectName'].encode('utf-8')))
        if r.get('effectDescription'):
            corpus.add(norm(r['effectDescription'].encode('utf-8')))
        corpus.add(r['effectKeywords'].encode('utf-8'))
        corpus.add(lower_ascii(r['name'].encode('utf-8')))
corpus = sorted(t for t in corpus if len(t) < 400 and b'\n' not in t)
# The dumps hold almost no non-ASCII text: add some (accented letters next to word
# characters, a dash, a CJK name) so word classes, boundaries and spans are checked on
# multi-byte input.
_base = [t for t in corpus if b'e' in t][:60]
corpus += sorted({t.replace(b'e', 'é'.encode('utf-8'), 1) for t in _base} |
                 {'café damage — fortify destruction'.encode('utf-8'), 'restore health ñ 25'.encode('utf-8'),
                  '火の魔法 fire damage'.encode('utf-8'), 'résist fire'.encode('utf-8')})
non_ascii = [t for t in corpus if any(c > 127 for c in t)]
rows = []
for p in patterns:
    rx = re.compile(p.encode('utf-8'))
    hits, misses = [], []
    for t in corpus:
        (hits if rx.search(t) else misses).append(t)
    random.shuffle(hits); random.shuffle(misses)
    random.shuffle(non_ascii)
    for t in hits[:12] + misses[:4] + non_ascii[:3]:
        m = rx.search(t)
        g1 = b''
        if m and m.re.groups >= 1 and m.group(1) is not None:
            g1 = m.group(1)
        try:
            g1s = g1.decode('utf-8')
        except UnicodeDecodeError:
            continue  # a group cutting a multi-byte character cannot be written as text
        rows.append({'pattern': p, 'text': t.decode('utf-8'), 'matched': 1 if m else 0,
                     'start': m.start() if m else -1, 'end': m.end() if m else -1, 'group1': g1s})
with open(f'{repo}/tests/core/fixtures/regex_oracle.csv', 'w', newline='', encoding='utf-8') as f:
    w = csv.DictWriter(f, fieldnames=['pattern', 'text', 'matched', 'start', 'end', 'group1'])
    w.writeheader()
    w.writerows(rows)
print(len(patterns), 'patterns', len(corpus), 'texts', len(non_ascii), 'non-ASCII', len(rows), 'rows',
      sum(r['matched'] for r in rows), 'matches')
