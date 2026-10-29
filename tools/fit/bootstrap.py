"""The R8 bootstrap file: format version 1 (docs/architecture/9-bootstrap-format.md). Builds the
document from a fit, writes it deterministically, validates it, and keys items by plugin +
local FormID. No numpy here: the writer and the validator take plain lists, so the golden
examples (tests/core/fixtures/bootstrap/bootstrap_v1_full.json and bootstrap_v1_minimal.json)
are checked without a fit.

    make_document(...)    the document (a dict) from plain pieces; the field order is fixed
    dumps(doc)            deterministic text: objects one key per line, number arrays on one line,
                          floats at 6 significant digits, no NaN or infinity
    validate(doc)         a list of problems ([] = valid), the same rules a reader applies
    read_forms(path)      the `hg dump all` CSV (Huginn_All.csv) -> {FormID: (plugin, name, kind)}
    index_owners(forms)   {load-order index: the plugin that owns it, or None when the dump
                          cannot tell}
    item_key(form, forms, owners)
                          (index owner, local FormID hex) for a runtime FormID, or (None, reason)
"""
import csv
from collections import defaultdict
import json
import math

FORMAT = 'huginn.bootstrap'
VERSION = 1
GRADINGS = ('graded', 'presence', 'presence+strength')
OUTCOMES = ('picks', 'three')
FEATURES = (('ln util', 'util missing') + tuple(f'slot {s}' for s in range(8))
            # the old score's components (R6's real fit, 0.23.54; the format doc, features)
            + ('ln learn mult', 'ln fav', 'ln corr', 'ln potion', 'ln learner', 'eligible'))


def num(v):
    """A float at 6 significant digits (-0 reads 0)."""
    v = float(f'{float(v):.6g}')
    return 0.0 if v == 0 else v


def nums(vs):
    return [num(v) for v in vs]


def make_document(*, source, spec, outcomes, grading, needs, cols, theta, theta_precision, kappa,
                  theta_strength=None, theta_strength_precision=None, theta_bias=None, u0=None, recent=None,
                  features=(), items=(), items_dropped=None, curves=None, created_utc=None, provenance=None):
    """The bootstrap document. Matrices are lists of rows (need-major); recent is None or
    {'window_s', 'taus_s', 'features': [{'family', 'tau_s', 'theta', 'theta_strength'?,
    'precision', 'u0'?}]}; u0 is None or {'bias', 'need': {name: w}}; features [(name, w)];
    items [(plugin, local hex, name, b, precision, picks)]; curves {need: (kind, p1, p2)};
    provenance: None or a JSON object, written last as `provenance` (informational, unchecked)."""
    doc = {'format': FORMAT, 'version': VERSION}
    if created_utc is not None:
        doc['created_utc'] = created_utc
    doc['source'] = source
    doc['model'] = {'spec': spec, 'outcomes': outcomes, 'grading': grading}
    doc['needs'] = list(needs)
    doc['cols'] = list(cols)
    doc['theta'] = [nums(r) for r in theta]
    doc['theta_precision'] = [nums(r) for r in theta_precision]
    doc['theta_strength'] = None if theta_strength is None else [nums(r) for r in theta_strength]
    doc['theta_strength_precision'] = (None if theta_strength_precision is None
                                       else [nums(r) for r in theta_strength_precision])
    doc['theta_bias'] = None if theta_bias is None else nums(theta_bias)
    doc['kappa'] = num(kappa)
    doc['u0'] = None if u0 is None else {'bias': num(u0['bias']),
                                         'need': {n: num(w) for n, w in u0['need'].items()}}
    if recent is None:
        doc['recent'] = None
    else:
        feats = []
        for f in recent['features']:
            o = {'family': f['family'], 'tau_s': num(f['tau_s']), 'theta': nums(f['theta']),
                 'precision': nums(f['precision'])}
            if f.get('theta_strength') is not None:
                o['theta_strength'] = nums(f['theta_strength'])
            if f.get('u0') is not None:
                o['u0'] = num(f['u0'])
            feats.append(o)
        doc['recent'] = {'window_s': num(recent['window_s']), 'taus_s': nums(recent['taus_s']), 'features': feats}
    doc['features'] = [{'name': n, 'w': num(w)} for n, w in features]
    doc['items'] = [{'plugin': p, 'local': loc, 'name': nm, 'b': num(b), 'precision': num(pr), 'picks': int(k)}
                    for p, loc, nm, b, pr, k in sorted(items, key=lambda t: (t[0].lower(), t[1]))]
    doc['items_dropped'] = dict(sorted((items_dropped or {}).items()))
    doc['curves'] = {n: [c[0], num(c[1]), num(c[2])] for n, c in sorted((curves or {}).items())}
    if provenance is not None:
        doc['provenance'] = provenance
    return doc


def _dump(v, ind):
    pad = ' ' * ind
    if isinstance(v, dict):
        if not v:
            return '{}'
        inner = [f'{pad} {json.dumps(k)}: {_dump(x, ind + 1)}' for k, x in v.items()]
        return '{\n' + ',\n'.join(inner) + '\n' + pad + '}'
    if isinstance(v, list):
        if all(not isinstance(x, (dict, list)) for x in v):
            return '[' + ','.join(_scalar(x) for x in v) + ']'
        inner = [f'{pad} {_dump(x, ind + 1)}' for x in v]
        return '[\n' + ',\n'.join(inner) + '\n' + pad + ']'
    return _scalar(v)


def _scalar(x):
    if isinstance(x, float) and not math.isfinite(x):
        raise ValueError('bootstrap: a number is NaN or infinite')
    return json.dumps(x, ensure_ascii=False)


def dumps(doc):
    """Deterministic text for a document (one trailing newline)."""
    return _dump(doc, 0) + '\n'


def _matrix(doc, key, rows, cols, errs, allow_none=False, positive=False):
    m = doc.get(key)
    if m is None:
        if not allow_none:
            errs.append(f'{key}: missing')
        return
    if not isinstance(m, list) or len(m) != rows:
        errs.append(f'{key}: {rows} rows expected')
        return
    for i, r in enumerate(m):
        if not isinstance(r, list) or len(r) != cols:
            errs.append(f'{key}[{i}]: {cols} numbers expected')
            return
        if not all(_isnum(x) for x in r):
            errs.append(f'{key}[{i}]: not all finite numbers')
            return
        if positive and not all(x > 0 for x in r):
            errs.append(f'{key}[{i}]: a precision must be above 0 (sigma^2 = 1 / precision)')
            return


def _isnum(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)


def validate(doc):
    """The problems a version-1 reader rejects ([] = valid). Fields it does not know are
    allowed (a later writer may add them without a version bump: the format doc)."""
    errs = []
    if not isinstance(doc, dict):
        return ['not an object']
    if doc.get('format') != FORMAT:
        errs.append(f'format: {FORMAT!r} expected')
    if doc.get('version') != VERSION:
        errs.append(f'version: {VERSION} expected (a reader rejects any other)')
        return errs
    model = doc.get('model') or {}
    if model.get('grading') not in GRADINGS:
        errs.append(f'model.grading: one of {GRADINGS}')
    if model.get('outcomes') not in OUTCOMES:
        errs.append(f'model.outcomes: one of {OUTCOMES}')
    needs, cols = doc.get('needs'), doc.get('cols')
    for key, lst in (('needs', needs), ('cols', cols)):
        if not isinstance(lst, list) or not all(isinstance(x, str) and x for x in lst):
            errs.append(f'{key}: a list of names expected')
            return errs
        if len(set(lst)) != len(lst):
            errs.append(f'{key}: duplicate names')
    K, J = len(needs), len(cols)
    _matrix(doc, 'theta', K, J, errs)
    _matrix(doc, 'theta_precision', K, J, errs, positive=True)
    strength = model.get('grading') == 'presence+strength'
    _matrix(doc, 'theta_strength', K, J, errs, allow_none=not strength)
    _matrix(doc, 'theta_strength_precision', K, J, errs, allow_none=not strength, positive=True)
    if not strength and (doc.get('theta_strength') is not None or doc.get('theta_strength_precision') is not None):
        errs.append('theta_strength: only with grading presence+strength')
    tb = doc.get('theta_bias')
    if tb is not None and (not isinstance(tb, list) or len(tb) != J or not all(_isnum(x) for x in tb)):
        errs.append(f'theta_bias: null or {J} numbers')
    if tb is not None and strength:
        errs.append('theta_bias: not with grading presence+strength')
    if not _isnum(doc.get('kappa')):
        errs.append('kappa: a number')
    u0 = doc.get('u0')
    if model.get('outcomes') == 'three':
        if not isinstance(u0, dict) or not _isnum(u0.get('bias')) or not isinstance(u0.get('need'), dict):
            errs.append('u0: {bias, need} expected with outcomes three')
        elif any(n not in needs or not _isnum(w) for n, w in u0['need'].items()):
            errs.append('u0.need: names from needs, numbers')
    elif u0 is not None:
        errs.append('u0: null unless outcomes three')
    rec = doc.get('recent')
    if rec is not None:
        if not _isnum(rec.get('window_s')) or not isinstance(rec.get('taus_s'), list):
            errs.append('recent: window_s and taus_s expected')
        for i, f in enumerate(rec.get('features') or []):
            if not isinstance(f.get('family'), str) or f['family'] not in cols:
                errs.append(f'recent.features[{i}].family: a name from cols')
            if f.get('tau_s') not in rec.get('taus_s', []):
                errs.append(f'recent.features[{i}].tau_s: one of taus_s')
            for key in ('theta', 'precision') + (('theta_strength',) if strength else ()):
                v = f.get(key)
                if not isinstance(v, list) or len(v) != J or not all(_isnum(x) for x in v):
                    errs.append(f'recent.features[{i}].{key}: {J} numbers')
                elif key == 'precision' and not all(x > 0 for x in v):
                    errs.append(f'recent.features[{i}].precision: above 0')
            if (model.get('outcomes') == 'three') != ('u0' in f):
                errs.append(f'recent.features[{i}].u0: present exactly when outcomes three')
    for i, f in enumerate(doc.get('features') or []):
        if f.get('name') not in FEATURES or not _isnum(f.get('w')):
            errs.append(f'features[{i}]: name one of {FEATURES}, w a number')
    seen = set()
    for i, it in enumerate(doc.get('items') or []):
        p, loc = it.get('plugin'), it.get('local')
        if not isinstance(p, str) or not p:
            errs.append(f'items[{i}].plugin: a plugin file name')
        if not isinstance(loc, str) or len(loc) != 6 or any(ch not in '0123456789ABCDEF' for ch in loc):
            errs.append(f'items[{i}].local: 6 upper-case hex digits')
        if not _isnum(it.get('b')) or not _isnum(it.get('precision')):
            errs.append(f'items[{i}]: b and precision numbers')
        elif it['precision'] <= 0:
            errs.append(f'items[{i}].precision: above 0')
        key = (str(p).lower(), loc)
        if key in seen:
            errs.append(f'items[{i}]: duplicate plugin + local')
        seen.add(key)
    for n, c in (doc.get('curves') or {}).items():
        if n not in needs or not isinstance(c, list) or len(c) != 3 or c[0] != 'logistic' or not all(_isnum(x) for x in c[1:]) \
                or c[2] == 0:
            errs.append(f'curves.{n}: ["logistic", centre, slope != 0] for a need in needs')
    return errs


def read_forms(path):
    """{runtime FormID (int): (defining plugin, name, kind)} from `hg dump all`'s CSV
    (Huginn_All.csv, one row per effect; the first row of a form is kept)."""
    out = {}
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        for r in csv.DictReader(f):
            try:
                fid = int(r['formID'], 16)
            except (KeyError, TypeError, ValueError):
                continue
            if fid not in out and r.get('plugin'):
                out[fid] = (r['plugin'], r.get('name') or '', r.get('kind') or '')
    return out


def index_of(fid):
    """The load-order index a runtime FormID's high bits name: the top byte for a full plugin
    (0x00-0xFD), FE plus the 12-bit light index for a light one (0xFExxx)."""
    return fid >> 12 if fid >> 24 == 0xFE else fid >> 24


def index_owners(forms):
    """{load-order index: the plugin that owns it, or None} from a form map (read_forms).

    The map's plugin is the form's DEFINING plugin (TESForm::GetFile(0), the first file that has
    the record). That is the index's owner for almost every form, but not for a record a plugin
    INJECTS into a master's FormID space (its FormID carries the master's index; the real dump
    has one: FE97B053 'Drunkness Monitor', defined by 'BOOB - Requiem.esp' at BOOB.esp's index
    FE97B, while 'BOOB - Requiem.esp' owns FE97C). TESDataHandler::LookupForm(local, plugin)
    rebuilds the FormID from the named plugin's own index, so the key must name the owner.

    Each plugin owns one index and each index one plugin; a plugin appears at its own index
    and at the indices it injects into. An index is given to the one plugin seen there that
    can own no other index (every other index it appears at is already owned), repeated until
    nothing changes. An index with no such plugin, or with two, is None (unknown): its forms
    are not keyed. One case this cannot see: a plugin whose dumped forms are ALL injected into
    a master that has no dumped form of its own is taken as that index's owner."""
    at, of = defaultdict(set), defaultdict(set)
    name = {}
    for fid, hit in forms.items():
        if fid >> 24 == 0xFF or not hit[0]:
            continue
        pl = hit[0].lower()              # plugin names compare without case
        name.setdefault(pl, hit[0])
        x = index_of(fid)
        at[x].add(pl)
        of[pl].add(x)
    owners = {}
    changed = True
    while changed:
        changed = False
        taken = set(owners.values())
        for x in sorted(at):
            if x in owners:
                continue
            cands = [pl for pl in at[x] if pl not in taken and of[pl] - owners.keys() == {x}]
            if len(cands) == 1:
                owners[x] = cands[0]
                taken.add(cands[0])
                changed = True
    return {x: (name[owners[x]] if x in owners else None) for x in at}


def item_key(form, forms, owners=None):
    """(plugin, local) for a runtime FormID (8 hex digits or an int): plugin is the plugin that
    owns the FormID's load-order index (index_owners; owners is computed from forms when not
    given), local the FormID within it, 6 upper-case hex digits -- the low 24 bits for a full
    plugin, the low 12 bits for a light one (FE xxx yyy). LookupForm(local, plugin) gives the
    FormID back in the same load order. (None, reason) for a dynamic FF form (made in game: it
    has no plugin), a form the map does not hold, or an index whose owner the map cannot tell."""
    fid = int(form, 16) if isinstance(form, str) else int(form)
    if fid >> 24 == 0xFF:
        return None, 'dynamic (FF)'
    if forms.get(fid) is None:
        return None, 'not in the form map'
    if owners is None:
        owners = index_owners(forms)
    owner = owners.get(index_of(fid))
    if owner is None:
        return None, 'index owner unknown'
    local = fid & 0xFFF if fid >> 24 == 0xFE else fid & 0xFFFFFF
    return (owner, f'{local:06X}'), None
