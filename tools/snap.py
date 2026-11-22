#!/usr/bin/env python3
# Draws a png of an apsim snap: the path and the objects around the player.
# Usage: snap.py snap.jsonl image.png [W] [CX CY], W half width of the view, CX CY its center (end by default)

import json, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
src, out = sys.argv[1], sys.argv[2]
P, P2, O, end = [], [], [], None
for line in open(src):
    d = json.loads(line)
    if "p" in d: P.append(d["p"])
    elif "p2" in d: P2.append(d["p2"])
    elif "o" in d: O.append(d["o"])
    elif "end" in d: end = d["end"]
col = {0: "#555555", 2: "#d62728", 8: "#d62728"}
fig, ax = plt.subplots(figsize=(16, 10))
cx, cy = end[3], end[4]
for uid, oid, typ, cls, x, y, w, h, dis in O:
    RINGS = (11, 12, 13, 29, 32, 35, 36, 37, 38, 43, 46); PADS = (8, 9, 10, 34, 44)
    if typ in RINGS: c = "#e6b800"; a = 0.9
    elif typ in PADS: c = "#d000d0"; a = 0.9
    elif typ in (2, 47): c = "#d62728"; a = 0.7
    elif typ in (0, 21, 25): c = "#444444"; a = 0.6
    elif typ == 7: continue
    elif cls == 1 and typ == 20: c = "#1f77b4"; a = 0.08
    else: c = "#17becf"; a = 0.6
    if dis: a *= 0.2
    ax.add_patch(Rectangle((x, y), max(w, 1), max(h, 1), fc=c, ec="none", alpha=a))
    if typ not in (0, 2, 20, 21, 25, 47) and w * h > 10: ax.text(x, y, str(oid), fontsize=5, clip_on=True)
if P:
    xs = [p[1] for p in P]; ys = [p[2] for p in P]
    ax.plot(xs, ys, "-", lw=0.8, color="#ff7f0e")
    px = [p[1] for p in P if p[3]]; py = [p[2] for p in P if p[3]]
    ax.plot(px, py, ".", ms=1.5, color="#9467bd")
if P2: ax.plot([p[1] for p in P2], [p[2] for p in P2], "-", lw=0.8, color="#17becf")
W = float(sys.argv[3]) if len(sys.argv) > 3 else 500
CX = float(sys.argv[4]) if len(sys.argv) > 4 else cx; CY = float(sys.argv[5]) if len(sys.argv) > 5 else cy
ax.set_xlim(CX - W, CX + W); ax.set_ylim(CY - W * 0.65, CY + W * 0.65)
ax.set_aspect("equal"); ax.set_title(f"tick {end[0]} dead={end[1]} killer={end[2]} ({cx:.0f},{cy:.0f})")
fig.savefig(out, dpi=110, bbox_inches="tight")
