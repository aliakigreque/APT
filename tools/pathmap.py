#!/usr/bin/env python3
# Draws a whole path as png pages from the output of apsim pathmap, the level cut in strips.
# Usage: pathmap.py map.jsonl prefix [--seg 3000] [--rows 4] [--from X] [--to X] [--ypad 450]

import json, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
from matplotlib.collections import PatchCollection

args = sys.argv[1:]
src, outp = args[0], args[1]
# value of a --name option, or dflt
def opt(name, dflt):
    if name in args:
        return float(args[args.index(name) + 1])
    return dflt
SEG = opt("--seg", 3000.0); ROWS = int(opt("--rows", 4)); X0 = opt("--from", -1e30); X1 = opt("--to", 1e30); YPAD = opt("--ypad", 450.0)
P, P2, O, end = [], [], [], None
for line in open(src):
    d = json.loads(line)
    if "p" in d: P.append(d["p"])
    elif "p2" in d: P2.append(d["p2"])
    elif "o" in d: O.append(d["o"])
    elif "end" in d: end = d["end"]
if not P:
    print("empty path"); sys.exit(1)
xmin = max(X0, min(p[1] for p in P)); xmax = min(X1, max(p[1] for p in P))
RINGS = (11, 12, 13, 29, 32, 35, 36, 37, 38, 43, 46); PADS = (8, 9, 10, 34, 44)
PORTALS = (3, 4, 5, 6, 14, 15, 16, 17, 18, 19, 23, 24, 26, 27, 28, 33, 41, 45)
# color and opacity of an object type
def style(typ, cls):
    if typ in RINGS: return "#e6b800", 0.9
    if typ in PADS: return "#d000d0", 0.9
    if typ in (2, 47): return "#d62728", 0.75
    if typ in (0, 21, 25): return "#333333", 0.65
    if typ in PORTALS: return "#2ca02c", 0.5
    return "#17becf", 0.5
segs = []
x = xmin
while x < xmax:
    segs.append((x, min(x + SEG, xmax + 1)))
    x += SEG
page = 0
for k in range(0, len(segs), ROWS):
    rows = segs[k:k + ROWS]
    fig, axes = plt.subplots(len(rows), 1, figsize=(18, 4.2 * len(rows)))
    if len(rows) == 1: axes = [axes]
    for ax, (a, b) in zip(axes, rows):
        pts = [p for p in P if a <= p[1] < b]
        if not pts: ax.set_axis_off(); continue
        ylo = min(p[2] for p in pts) - YPAD; yhi = max(p[2] for p in pts) + YPAD
        patches = {}
        for uid, oid, typ, cls, ox, oy, w, h, dis, t in O:
            if ox + w < a - 50 or ox > b + 50 or oy + h < ylo - 50 or oy > yhi + 50: continue
            c, al = style(typ, cls)
            if dis: al *= 0.25
            patches.setdefault((c, al), []).append(Rectangle((ox, oy), max(w, 2), max(h, 2)))
        for (c, al), ps in patches.items():
            ax.add_collection(PatchCollection(ps, fc=c, ec="none", alpha=al))
        ax.plot([p[1] for p in pts], [p[2] for p in pts], "-", lw=1.0, color="#ff7f0e")
        held = [p for p in pts if p[3]]
        if held: ax.plot([p[1] for p in held], [p[2] for p in held], ".", ms=1.2, color="#9467bd")
        q = [p for p in P2 if a <= p[1] < b]
        if q: ax.plot([p[1] for p in q], [p[2] for p in q], "-", lw=0.8, color="#1f77b4")
        last = None
        for p in pts:
            if p[5] != last:
                ax.text(p[1], p[2] + 20, f"{p[5]} t{p[0]}", fontsize=6, color="#000080")
                last = p[5]
        ax.set_xlim(a, b); ax.set_ylim(ylo, yhi)
        ax.set_aspect("equal", adjustable="box")
        ax.set_title(f"x {a:.0f} .. {b:.0f}  (ticks {pts[0][0]}..{pts[-1][0]})", fontsize=8)
        ax.tick_params(labelsize=6)
    fig.tight_layout()
    fn = f"{outp}_{page:02d}.png"
    fig.savefig(fn, dpi=72)
    plt.close(fig)
    print(fn)
    page += 1
if end: print("end :", end)
