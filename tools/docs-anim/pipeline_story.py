#!/usr/bin/env python3
"""Generate the animated "pipeline story" SVG for the docs.

One scripted encounter, compressed from ~3.5 minutes of play into a short
loop, showing how a change in game state travels through the pipeline:

    StateManager -> ContextRuleEngine -> FeatureBanditLearner
                 -> UtilityScorer -> SlotAllocator / SlotLocker

The SVG uses CSS @keyframes only (no JavaScript, which GitHub strips), so it
plays when embedded with ![](...) in Markdown. Dark mode follows
prefers-color-scheme; prefers-reduced-motion shows a single still beat.

Usage:
    python3 tools/docs-anim/pipeline_story.py                 # animated
    python3 tools/docs-anim/pipeline_story.py --frame 3       # still of beat 3
    python3 tools/docs-anim/pipeline_story.py -o out.svg

Values are illustrative but follow the shipped defaults: lambda 0.5..3.0
([Scoring] fLambdaMin/fLambdaMax), on-fire weight 8.0 -> 0.8 normalized
([ContextWeights] fWeightOnFire), in-combat 0.3, buff-in-combat 0.35,
critical-health override at 35% ([Overrides] fCriticalHealthThreshold) and
the 3000 ms slot lock ([Slots] fLockDurationMs). Edit BEATS, not the SVG.
"""

import argparse
from html import escape
from pathlib import Path

# ---------------------------------------------------------------------------
# Scenario data
# ---------------------------------------------------------------------------

LAMBDA_MIN, LAMBDA_MAX = 0.5, 3.0
BEAT_SEC = 4.0      # how long each beat holds
FADE_SEC = 0.5      # transition time at the start of each beat

CANDIDATES = [
    # key, label, class
    ("candle", "Candlelight", "Spell · Utility"),
    ("fireball", "Fireball", "Spell · Damage"),
    ("icespike", "Ice Spike", "Spell · Damage"),
    ("resist", "Resist Fire", "Potion · Buff"),
    ("health", "Restore Health", "Potion · Restore"),
]

SLOTS = ["Damage", "Any", "Any"]

# Per beat: what the player sees, the sensor state, each candidate's
# (contextWeight, learningScore, confidence), and the slot contents.
# Slot entries are (candidate key, badge) where badge is None, "OVERRIDE"
# or "LOCKED".
BEATS = [
    dict(
        clock="0:00", title="Exploring",
        caption="Out of combat. Context favours utility; nothing is urgent.",
        stage="context",
        hp=1.00, combat=False, fire=False,
        cand=dict(candle=(0.60, 0.10, 0.0), fireball=(0.05, 0.30, 0.2),
                  icespike=(0.05, 0.35, 0.2), resist=(0.15, 0.10, 0.0),
                  health=(0.05, 0.20, 0.2)),
        slots=[("icespike", None), ("candle", None), ("resist", None)],
    ),
    dict(
        clock="0:40", title="Combat starts",
        caption="In-combat weight (0.3) wakes the damage spells; Candlelight falls away.",
        stage="context",
        hp=1.00, combat=True, fire=False,
        cand=dict(candle=(0.05, 0.10, 0.0), fireball=(0.30, 0.30, 0.2),
                  icespike=(0.30, 0.35, 0.2), resist=(0.35, 0.10, 0.0),
                  health=(0.05, 0.20, 0.2)),
        slots=[("icespike", None), ("fireball", None), ("resist", None)],
    ),
    dict(
        clock="1:10", title="Taking fire damage",
        caption="Fire damage sets the resist-fire weight to 0.8. The potion jumps to the top.",
        stage="context",
        hp=0.70, combat=True, fire=True,
        cand=dict(candle=(0.05, 0.10, 0.0), fireball=(0.30, 0.30, 0.2),
                  icespike=(0.30, 0.35, 0.2), resist=(0.80, 0.10, 0.0),
                  health=(0.09, 0.20, 0.2)),
        slots=[("icespike", None), ("resist", None), ("fireball", None)],
    ),
    dict(
        clock="1:45", title="Health critical",
        caption="Health under 35%: the override skips scoring and pins the best restore potion.",
        stage="override",
        hp=0.30, combat=True, fire=True,
        cand=dict(candle=(0.05, 0.10, 0.0), fireball=(0.30, 0.30, 0.2),
                  icespike=(0.30, 0.35, 0.2), resist=(0.80, 0.10, 0.0),
                  health=(0.70, 0.20, 0.2)),
        slots=[("icespike", None), ("health", "OVERRIDE"), ("resist", None)],
    ),
    dict(
        clock="2:30", title="The player keeps choosing Fireball",
        caption="Each pick trains Fireball and nudges the passed-over Ice Spike down. Confidence raises λ.",
        stage="learn",
        hp=0.80, combat=True, fire=False,
        cand=dict(candle=(0.05, 0.10, 0.0), fireball=(0.30, 0.75, 0.6),
                  icespike=(0.30, 0.25, 0.2), resist=(0.35, 0.10, 0.0),
                  health=(0.05, 0.20, 0.2)),
        slots=[("fireball", None), ("icespike", None), ("resist", None)],
    ),
    dict(
        clock="3:10", title="Slot lock holds",
        caption="Restore Health edges past Resist Fire (0.38 vs 0.37), but the 3 s lock keeps the slot still.",
        stage="lock",
        hp=0.62, combat=True, fire=False,
        cand=dict(candle=(0.05, 0.10, 0.0), fireball=(0.30, 0.75, 0.6),
                  icespike=(0.30, 0.25, 0.2), resist=(0.35, 0.10, 0.0),
                  health=(0.32, 0.20, 0.2)),
        slots=[("fireball", None), ("icespike", None), ("resist", "LOCKED")],
    ),
]

def lam(conf):
    return LAMBDA_MIN + conf * (LAMBDA_MAX - LAMBDA_MIN)


def utility(ctx, learned, conf):
    return ctx * (1 + lam(conf) * learned)


# ---------------------------------------------------------------------------
# Animation helpers
# ---------------------------------------------------------------------------

class Anim:
    """Collects per-element keyframes; each value holds for one beat."""

    def __init__(self, frame):
        self.frame = frame          # None = animated, else index of still
        self.rules = []
        self.n = 0

    def _pct(self, t):
        return f"{100 * t / (BEAT_SEC * len(BEATS)):.3f}%"

    def prop(self, prop, values, fmt):
        """Return a style string animating `prop` through per-beat values."""
        if self.frame is not None:
            return f"{prop}:{fmt(values[self.frame])}"
        self.n += 1
        name = f"k{self.n}"
        frames = [f"0%{{{prop}:{fmt(values[-1])}}}"]
        for i, v in enumerate(values):
            start = i * BEAT_SEC
            frames.append(f"{self._pct(start + FADE_SEC)}{{{prop}:{fmt(v)}}}")
            frames.append(f"{self._pct(start + BEAT_SEC)}{{{prop}:{fmt(v)}}}")
        self.rules.append(f"@keyframes {name}{{{''.join(frames)}}}")
        # Base value = beat 0, used when reduced-motion disables animation.
        return f"{prop}:{fmt(values[0])};animation-name:{name}"

    def show(self, mask):
        """Opacity animation: visible on beats where mask[i] is true."""
        return self.prop("opacity", [1 if m else 0 for m in mask], str)

    def scale_x(self, values):
        return self.prop("transform", values, lambda v: f"scaleX({max(v, 0.001):.4f})")


def text(x, y, s, cls="", anchor="start", style=""):
    st = f' style="{style}"' if style else ""
    c = f' class="{cls}"' if cls else ""
    return (f'<text x="{x}" y="{y}" text-anchor="{anchor}"{c}{st}>'
            f"{escape(s)}</text>")


def per_beat_text(a, x, y, strings, cls="", anchor="start"):
    """One <text> per distinct string, cross-faded by beat."""
    out = []
    for s in dict.fromkeys(strings):
        out.append(text(x, y, s, cls, anchor, a.show([t == s for t in strings])))
    return "".join(out)


def bar(a, x, y, w, h, values, scale, cls):
    """A horizontal bar whose length animates through per-beat values."""
    track = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" class="track"/>'
    fill = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" class="bar {cls}" '
            f'style="{a.scale_x([v / scale for v in values])}"/>')
    return track + fill


# ---------------------------------------------------------------------------
# SVG
# ---------------------------------------------------------------------------

W, H = 1200, 690

CSS = """
:root{--bg:#ffffff;--fg:#1f2328;--muted:#59636e;--track:#eaeef2;--panel:#f6f8fa;
--line:#d1d9e0;--ctx:#0969da;--learn:#8250df;--util:#1a7f37;--warn:#cf222e;
--lock:#9a6700;--accent:#bf3989}
@media (prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;--muted:#9198a1;
--track:#21262d;--panel:#161b22;--line:#3d444d;--ctx:#4493f8;--learn:#ab7df8;
--util:#3fb950;--warn:#f85149;--lock:#d29922;--accent:#db61a2}}
svg{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Noto Sans",Helvetica,Arial,sans-serif}
text{fill:var(--fg);font-size:16px}
.bg{fill:var(--bg)} .panel{fill:var(--panel);stroke:var(--line)}
.h1{font-size:26px;font-weight:600} .h2{font-size:18px;font-weight:600}
.cap{font-size:18px;fill:var(--muted)} .muted{fill:var(--muted)} .small{font-size:14px} .tiny{font-size:12px}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.num{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:15px}
.clock{font-size:30px;font-weight:600;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.track{fill:var(--track)} .bar{transform-box:fill-box;transform-origin:0 50%}
.ctx{fill:var(--ctx)} .learn{fill:var(--learn)} .util{fill:var(--util)} .hp{fill:var(--util)}
.pill{stroke:var(--line);fill:var(--track)} .on-warn{fill:var(--warn)} .on-ctx{fill:var(--ctx)}
.slot{fill:var(--bg);stroke:var(--line);stroke-width:1.5}
.badge-o{fill:var(--warn)} .badge-l{fill:var(--lock)} .badge-t{fill:#fff;font-size:12px;font-weight:700}
.hl{fill:none;stroke:var(--accent);stroke-width:3}
.flow{stroke:var(--muted);stroke-width:2;fill:none;stroke-dasharray:6 6}
.arrow{fill:var(--muted)}
.anim *,.anim{animation-duration:%DUR%s;animation-iteration-count:infinite;animation-timing-function:ease-in-out}
.flow{animation:dash 1s linear infinite}
@keyframes dash{to{stroke-dashoffset:-12}}
@media (prefers-reduced-motion:reduce){.anim *,.anim,.flow{animation:none!important}}
"""


def build(frame=None):
    a = Anim(frame)
    o = []
    n = len(BEATS)

    # --- Header: title, beat title, caption, clock ---------------------------
    o.append(text(30, 44, "Huginn, one fight, compressed", "h1"))
    o.append(per_beat_text(a, 30, 80, [f"{i + 1}. {b['title']}" for i, b in enumerate(BEATS)], "h2"))
    o.append(per_beat_text(a, 30, 108, [b["caption"] for b in BEATS], "cap"))
    o.append(text(W - 30, 44, "game time", "small muted", "end"))
    o.append(per_beat_text(a, W - 30, 80, [b["clock"] for b in BEATS], "clock", "end"))

    # Beat progress dots
    for i in range(n):
        cx = W - 30 - (n - 1 - i) * 18
        o.append(f'<circle cx="{cx}" cy="104" r="5" class="track"/>')
        o.append(f'<circle cx="{cx}" cy="104" r="5" class="util" '
                 f'style="{a.show([j == i for j in range(n)])}"/>')

    top = 140

    # --- Panel 1: game state (StateManager) ---------------------------------
    px, pw = 30, 270
    o.append(f'<rect x="{px}" y="{top}" width="{pw}" height="460" rx="10" class="panel"/>')
    o.append(text(px + 16, top + 30, "Game state", "h2"))
    o.append(text(px + 16, top + 50, "StateManager, polled ~100 ms", "small muted mono"))

    y = top + 90
    o.append(text(px + 16, y, "Health"))
    o.append(per_beat_text(a, px + pw - 16, y, [f"{round(b['hp'] * 100)}%" for b in BEATS], "num", "end"))
    o.append(bar(a, px + 16, y + 10, pw - 32, 14, [b["hp"] for b in BEATS], 1.0, "hp"))
    # 35% override threshold marker
    tx = px + 16 + (pw - 32) * 0.35
    o.append(f'<line x1="{tx}" y1="{y + 4}" x2="{tx}" y2="{y + 30}" stroke="var(--warn)" stroke-width="2"/>')
    o.append(text(tx, y + 46, "override 35%", "small muted", "middle"))

    def pill(y, label, values, oncls):
        s = f'<rect x="{px + 16}" y="{y - 20}" width="{pw - 32}" height="30" rx="15" class="pill"/>'
        s += (f'<rect x="{px + 16}" y="{y - 20}" width="{pw - 32}" height="30" rx="15" class="{oncls}" '
              f'style="{a.show(values)}"/>')
        s += text(px + 32, y, label)
        s += per_beat_text(a, px + pw - 32, y, ["yes" if v else "no" for v in values], "small", "end")
        return s

    o.append(pill(top + 190, "In combat", [b["combat"] for b in BEATS], "on-ctx"))
    o.append(pill(top + 240, "Taking fire damage", [b["fire"] for b in BEATS], "on-warn"))

    o.append(text(px + 16, top + 300, "Only what the player can", "small muted"))
    o.append(text(px + 16, top + 320, "see: own vitals, own effects,", "small muted"))
    o.append(text(px + 16, top + 340, "combat state. Never enemy", "small muted"))
    o.append(text(px + 16, top + 360, "spell lists or inventories.", "small muted"))

    # --- Panel 2: scoring ----------------------------------------------------
    sx, sw = 330, 610
    o.append(f'<rect x="{sx}" y="{top}" width="{sw}" height="460" rx="10" class="panel"/>')
    o.append(text(sx + 16, top + 30, "Scoring", "h2"))
    o.append(text(sx + 16, top + 54, "utility = context × (1 + λ(confidence) × learned)", "mono small"))

    c1, c2, c3 = sx + 180, sx + 340, sx + 500     # column x for the three bars
    cw = 100
    hy = top + 90
    o.append(text(c1, hy, "context", "small", style="fill:var(--ctx);font-weight:600"))
    o.append(text(c2, hy, "1 + λ·learned", "small", style="fill:var(--learn);font-weight:600"))
    o.append(text(c3, hy, "utility", "small", style="fill:var(--util);font-weight:600"))
    o.append(text(c1, hy + 18, "ContextRuleEngine", "tiny muted mono"))
    o.append(text(c2, hy + 18, "FeatureBanditLearner", "tiny muted mono"))
    o.append(text(c3, hy + 18, "UtilityScorer", "tiny muted mono"))

    row_y0, row_h = top + 140, 62
    for r, (key, label, cls) in enumerate(CANDIDATES):
        ry = row_y0 + r * row_h
        vals = [b["cand"][key] for b in BEATS]
        ctx = [v[0] for v in vals]
        mult = [1 + lam(v[2]) * v[1] for v in vals]
        util = [utility(*v) for v in vals]
        o.append(text(sx + 16, ry + 4, label))
        o.append(text(sx + 16, ry + 24, cls, "small muted"))
        for cx, series, scale, bcls, fmt in (
            (c1, ctx, 1.0, "ctx", "{:.2f}"),
            (c2, mult, 1 + LAMBDA_MAX, "learn", "×{:.2f}"),
            (c3, util, 1.0, "util", "{:.2f}"),
        ):
            o.append(bar(a, cx, ry - 8, cw, 12, series, scale, bcls))
            o.append(per_beat_text(a, cx, ry + 24, [fmt.format(v) for v in series], "num muted"))

    # Stage highlight: outline the column that drives this beat
    def stage_mask(name):
        return [b["stage"] == name for b in BEATS]

    for name, x0 in (("context", c1), ("learn", c2)):
        o.append(f'<rect x="{x0 - 10}" y="{hy - 24}" width="{cw + 20}" height="{row_h * 5 + 80}" '
                 f'rx="8" class="hl" style="{a.show(stage_mask(name))}"/>')

    # --- Panel 3: slots -------------------------------------------------------
    lx, lw = 970, 200
    o.append(f'<rect x="{lx}" y="{top}" width="{lw}" height="460" rx="10" class="panel"/>')
    o.append(text(lx + 16, top + 30, "Slots", "h2"))
    o.append(text(lx + 16, top + 50, "SlotAllocator", "small muted mono"))

    labels = {k: lbl for k, lbl, _ in CANDIDATES}
    for s, slot_cls in enumerate(SLOTS):
        y = top + 80 + s * 120
        o.append(f'<rect x="{lx + 16}" y="{y}" width="{lw - 32}" height="96" rx="8" class="slot"/>')
        o.append(text(lx + 28, y + 22, f"Slot {s + 1} · {slot_cls}", "small muted"))
        names = [labels[b["slots"][s][0]] for b in BEATS]
        # Long names wrap onto two lines
        for nm in dict.fromkeys(names):
            first, _, rest = nm.partition(" ")
            l1, l2 = (first, rest) if len(nm) > 12 else (nm, "")
            st = a.show([x == nm for x in names])
            o.append(f'<g style="{st}">{text(lx + 28, y + 50, l1, "h2")}'
                     f'{text(lx + 28, y + 72, l2, "h2") if l2 else ""}</g>')
        badges = [b["slots"][s][1] for b in BEATS]
        for badge, bcls in (("OVERRIDE", "badge-o"), ("LOCKED", "badge-l")):
            mask = [x == badge for x in badges]
            if any(mask):
                bw = 84 if badge == "OVERRIDE" else 70
                o.append(f'<g style="{a.show(mask)}"><rect x="{lx + lw - 24 - bw}" y="{y + 70}" '
                         f'width="{bw}" height="20" rx="10" class="{bcls}"/>'
                         f'{text(lx + lw - 24 - bw / 2, y + 84, badge, "badge-t", "middle")}</g>')
        # Outline the slot when override/lock is the story of this beat
        hl = [x is not None for x in badges]
        if any(hl):
            o.append(f'<rect x="{lx + 16}" y="{y}" width="{lw - 32}" height="96" rx="8" '
                     f'class="hl" style="{a.show(hl)}"/>')

    # --- Flow arrows between panels -----------------------------------------
    for x1, x2 in ((px + pw, sx), (sx + sw, lx)):
        ym = top + 230
        o.append(f'<path d="M{x1 + 2} {ym} H{x2 - 8}" class="flow"/>')
        o.append(f'<path d="M{x2 - 10} {ym - 6} L{x2 - 2} {ym} L{x2 - 10} {ym + 6} Z" class="arrow"/>')

    # --- Footer ---------------------------------------------------------------
    fy = top + 500
    o.append(text(30, fy, "Each beat is one moment of a ~3.5 minute fight. Every ~100 ms Huginn re-reads game state, "
                          "re-scores every candidate and refills the slots.", "small muted"))
    o.append(text(30, fy + 22, "Override bypasses scoring for urgent needs; the slot lock (3 s) stops near-ties "
                               "from flickering. Values are illustrative, parameters are the shipped defaults.",
                  "small muted"))

    dur = BEAT_SEC * n
    css = CSS.replace("%DUR%", f"{dur:g}") + "\n".join(a.rules)
    title = "Huginn pipeline: one fight, compressed"
    desc = ("Animated diagram. Game state (health, combat, fire damage) feeds context weights; "
            "the learner's preference multiplies them into a utility score; the top items fill three slots. "
            "Beats: exploring, combat starts, fire damage raises Resist Fire, critical health triggers an "
            "override, repeated Fireball picks are learned, and a slot lock holds a near-tie.")
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
            f'role="img" aria-labelledby="t d">'
            f'<title id="t">{escape(title)}</title><desc id="d">{escape(desc)}</desc>'
            f"<style>{css}</style>"
            f'<rect width="{W}" height="{H}" class="bg"/>'
            f'<g class="anim">{"".join(o)}</g></svg>\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-o", "--out", type=Path,
                    default=Path(__file__).resolve().parents[2] / "docs/assets/pipeline-story.svg")
    ap.add_argument("--frame", type=int, help="emit a still of beat N (1-based) instead of the animation")
    args = ap.parse_args()
    frame = None
    if args.frame is not None:
        if not 1 <= args.frame <= len(BEATS):
            ap.error(f"--frame must be 1..{len(BEATS)}")
        frame = args.frame - 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(build(frame), encoding="utf-8")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
