"""Compare a `hg dump races` file's huginnReading column with the race map.

    python -I tools/races/check_race_reading.py <Huginn_Races.csv> [race_map.csv]

The dump comes from the game (Debug build, `hg dump races`, written next to the
SKSE log). Rows are joined on plugin + editorID: a runtime formID moves when the
load order changes (an ESL record's FE-prefixed ID shifts with its slot). The
formID is used only for a map row with no editorID; a map row WITH an editorID
that the dump lacks is reported missing, not looked up by formID. No dump row
is matched twice. A race the map lists but the dump lacks is reported; races
the dump has and the map does not are ignored.

Expected reading per map row (the same rule as the host tests in
tests/core/ActorTypeClassifierTests.cpp): a row flagged in today_mismatch reads
its primary family folded onto today's six types; every other row reads the
map's `today`.

Exit code: 0 when every row matches; 1 on a mismatch, a missing row, or a
duplicated (plugin, editorID) key in either file; 2 when an input lacks a
column this needs.
"""
import csv
import os
import sys

FOLD = {
    'humanoid': 'Humanoid', 'none': 'Humanoid', 'undead': 'Undead', 'daedra': 'Daedra',
    'dragon': 'Dragon', 'construct': 'Construct', 'animal': 'Beast', 'arthropod': 'Beast',
    'troll': 'Beast', 'giant': 'Beast', 'werebeast': 'Beast', 'monster': 'Beast',
}
DUMP_COLUMNS = ('formID', 'editorID', 'plugin', 'huginnReading')
MAP_COLUMNS = ('formID', 'editorID', 'plugin', 'family', 'today', 'today_mismatch')


def read_csv(path, encoding):
    with open(path, encoding=encoding, errors='replace', newline='') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        return reader.fieldnames or [], rows


def missing_columns(name, fields, needed):
    gone = [c for c in needed if c not in fields]
    if gone:
        hint = ' (made by a build before 0.23.8?)' if name == 'dump' and 'huginnReading' in gone else ''
        print(f"{name} lacks column(s) {', '.join(gone)}{hint}")
    return bool(gone)


def index(rows, name):
    """(plugin, editorID) -> row, and formID -> rows for rows without an
    editorID. A key on more than one row is reported, never last-wins."""
    by_key, by_form, dupes = {}, {}, 0
    for r in rows:
        if r['editorID']:
            key = (r['plugin'], r['editorID'])
            if key in by_key:
                dupes += 1
                print(f"DUPLICATE {name} key plugin={key[0]} editorID={key[1]} "
                      f"(formIDs {by_key[key]['formID']}, {r['formID']})")
                continue
            by_key[key] = r
        else:
            by_form.setdefault(r['formID'], []).append(r)
    return by_key, by_form, dupes


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    map_path = argv[2] if len(argv) > 2 else os.path.join(
        here, '..', '..', 'docs', 'architecture', '9-data', 'race_map.csv')
    dump_fields, dump_rows = read_csv(argv[1], 'utf-8-sig')
    map_fields, rows = read_csv(map_path, 'utf-8-sig')
    if missing_columns('dump', dump_fields, DUMP_COLUMNS) | missing_columns('map', map_fields, MAP_COLUMNS):
        return 2

    dump_by_key, _, dump_dupes = index(dump_rows, 'dump')
    _, _, map_dupes = index(rows, 'map')
    # Every dump row a formID could reach, so a no-editorID map row can still
    # find a dump row that has an editorID.
    dump_any_form = {}
    for d in dump_rows:
        dump_any_form.setdefault(d['formID'], []).append(d)

    used = set()   # id() of dump rows already matched
    bad = dump_dupes + map_dupes
    for r in rows:
        if r['editorID']:
            d = dump_by_key.get((r['plugin'], r['editorID']))
        else:
            candidates = [c for c in dump_any_form.get(r['formID'], []) if id(c) not in used]
            if len(candidates) > 1:
                print(f"AMBIGUOUS {r['formID']} (no editorID): {len(candidates)} dump rows share the formID")
                bad += 1
                continue
            d = candidates[0] if candidates else None
        if d is None:
            print(f"MISSING  {r['formID']} {r['plugin']} {r['editorID'] or '(no editorID)'}")
            bad += 1
            continue
        if id(d) in used:
            print(f"REUSED   {r['formID']} {r['editorID']}: its dump row already matched another map row")
            bad += 1
            continue
        used.add(id(d))
        family = r['family']
        if r['today_mismatch'] and family not in FOLD:
            print(f"UNKNOWN family '{family}' on {r['formID']} {r['editorID']}")
            bad += 1
            continue
        expected = FOLD[family] if r['today_mismatch'] else r['today']
        if d['huginnReading'] != expected:
            bad += 1
            print(f"MISMATCH {r['formID']} {r['editorID']}: read {d['huginnReading']}, expected {expected} "
                  f"(family {family}, today {r['today']}, flag '{r['today_mismatch']}')")
    print(f"rows {len(rows)}  problems {bad}  (duplicate keys: dump {dump_dupes}, map {map_dupes})")
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
