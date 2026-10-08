"""Trim a slot capture (Huginn_SlotSnapshots.txt) into a golden-test fixture.

A capture session (tools/ingame/run_tests.py --capture-slots SEC) records
every allocation, and the pipeline re-runs the same page many times a second
while nothing changes. This keeps, per tag and page, only a snapshot whose
block differs from the last one kept apart from its clock reading (`now=`),
so the fixture holds each distinct situation once.

    python -I tools/slots/trim_snapshots.py CAPTURE.txt OUT.txt [--max N]

--max N then keeps at most N per tag (play, campaign), evenly spaced, so a
fixture stays small enough to check in. The item dictionary (ITEM lines) is
kept whole, ahead of the blocks.

The format is src/core/SlotSnapshotIO.h; the golden test is
tests/core/SlotAllocGoldenTests.cpp.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

RE_NOW = re.compile(r" now=-?\d+")


def split(text: str) -> tuple[list[str], list[list[str]]]:
    """The item dictionary (ITEM lines, between blocks) and the snapshot blocks."""
    items: list[str] = []
    blocks: list[list[str]] = []
    cur: list[str] = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        if line.startswith("ITEM ") and not cur:
            items.append(line)
            continue
        cur.append(line)
        if line == "END":
            blocks.append(cur)
            cur = []
    if cur:
        raise SystemExit("capture ends inside a snapshot (no END)")
    return items, blocks


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--max", type=int, default=0,
                    help="keep at most N snapshots per tag, evenly spaced (0 = every distinct one)")
    args = ap.parse_args()

    text = args.capture.read_text(encoding="utf-8")
    header = next((l for l in text.splitlines() if l.startswith("#")), "")
    items, blocks = split(text)
    last: dict[str, str] = {}
    kept: list[str] = []
    total = 0
    for b in blocks:
        total += 1
        key = " ".join(b[0].split(" ")[1:3])   # tag and page
        # A snapshot whose inputs and result differ only in `now` is the same
        # situation (departure ages move with it, but nothing else does).
        sig = "\n".join([RE_NOW.sub("", b[0])] + b[1:])
        if last.get(key) == sig:
            continue
        last[key] = sig
        kept.append("\n".join(b) + "\n")
    if args.max:
        # Evenly spaced within each tag (play and campaign kept apart).
        by_tag: dict[str, list[str]] = {}
        for k in kept:
            by_tag.setdefault(k.split(" ", 2)[1], []).append(k)
        kept = []
        for group in by_tag.values():
            if len(group) > args.max:
                step = len(group) / args.max
                group = [group[int(i * step)] for i in range(args.max)]
            kept.extend(group)
    # The whole dictionary first: ids stay valid whichever blocks are dropped.
    out = (header + " (trimmed by tools/slots/trim_snapshots.py)\n" if header else "") \
        + "".join(line + "\n" for line in items) + "".join(kept)
    args.out.write_text(out, encoding="utf-8", newline="\n")
    print(f"{total} snapshot(s) in, {len(kept)} kept, {len(out.encode('utf-8'))} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
