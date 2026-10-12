"""Shared by the fit scripts: where inputs and outputs live, the loaded log, the
row and flag constants of selection log v3 (docs/architecture/9-selection-log-v3.md).

Inputs and outputs (nothing is written into the repository):
  WORK      every script reads and writes in one work directory: the current
            directory, or $HUGINN_FIT_WORK. v3load.py writes v3.pkl there; the
            others read it and write their .txt / .pkl outputs beside it.
  v2 log    the old Huginn_Selections.jsonl, for the baselines that read the
            old engine's context weights: $HUGINN_FIT_V2, else the
            Huginn_Selections.jsonl beside the v3 file v3load.py read (its
            path is kept in v3.pkl), else none (those baselines then read NaN).
  csv       effects.csv comes from docs/architecture/9-data/.

v3.pkl is written by v3load.py from the player's own log; it is not
untrusted input, but do not load a v3.pkl from anywhere else.
"""
import datetime as dt
import os
import pickle
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DATA = REPO / 'docs' / 'architecture' / '9-data'
EFFECTS_CSV = DATA / 'effects.csv'

F = {'eligible': 1, 'scored': 2, 'held': 4, 'equipped': 8, 'shown': 16, 'wildcard': 32,
     'override': 64, 'remembered': 128, 'addedAtPick': 256}
# row tuple indices
FORM, UID, KIND, SRC, CAP, FLAGS, SLOT, UTIL, X, WP = range(10)


def work_dir():
    d = Path(os.environ.get('HUGINN_FIT_WORK', '.')).resolve()
    try:
        d.relative_to(REPO)
    except ValueError:
        return d
    sys.exit(f'work directory {d} is inside the repository; run from a scratch directory '
             f'(or set HUGINN_FIT_WORK) so no data or output is committed')


def work(name):
    """A file in the work directory."""
    return work_dir() / name


def out_file(name):
    """An output text file in the work directory."""
    return open(work(name), 'w', encoding='utf-8')


def T(s):
    return dt.datetime.strptime(s[:23], '%Y-%m-%d %H:%M:%S.%f')


def load():
    path = work('v3.pkl')
    if not path.exists():
        sys.exit(f'{path} not found: run v3load.py <Huginn_Selections_v3.jsonl> first (same work directory)')
    with open(path, 'rb') as f:
        D = pickle.load(f)
    for d in D['decs']:
        c = D['ctxs'].get(d['ctxkey']) if d['ctxkey'] else None
        d['C'] = c
        d['rows'] = (c['rows'] if c else []) + d['add']
        d['chosen'] = d['rows'][d['row']] if 0 <= d['row'] < len(d['rows']) else None
    return D


def v2_path(D=None):
    """The v2 log for the baselines, or None (see the module docstring)."""
    env = os.environ.get('HUGINN_FIT_V2')
    if env:
        return Path(env)
    if D is None:
        try:
            with open(work('v3.pkl'), 'rb') as f:
                D = pickle.load(f)
        except OSError:
            return None
    src = D.get('source')
    if src:
        p = Path(src).with_name('Huginn_Selections.jsonl')
        if p.exists():
            return p
    return None


def picks(D):
    return [d for d in D['decs'] if d['out'] != 'nothing']
