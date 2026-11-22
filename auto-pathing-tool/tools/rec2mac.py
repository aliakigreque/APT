#!/usr/bin/env python3
# Turns an F6 recording into an apsim text macro: one digit per tick, 0 none, 1 p1, 2 p2, 3 both.
# Usage: rec2mac.py recording.jsonl macro.txt

import json, sys
held = {}
pre = []
with open(sys.argv[1]) as fh:
    for line in fh:
        if '"ev"' in line[:8]:
            d = json.loads(line)
            if d.get('ev') == 'start' and d.get('pre'): p = d['pre']; pre = [(p[i], p[i + 1]) for i in range(0, len(p) - 1, 2)]
            continue
        d = json.loads(line)
        if 't' not in d or 'p' not in d: continue
        t = d['t']; v = held.get(t, 0)
        if d.get('h'): v |= 1 if d['p'] == 1 else 2
        held[t] = v
first = min(held) if held else 0
for t in range(first):
    v = 0
    for (pt, pv) in pre:
        if pt <= t: v = pv
    held[t] = v
n = max(held) if held else 0
s = ''.join(str(held.get(t + 1, 0)) for t in range(n))
open(sys.argv[2], 'w').write(s)
print(f"{n} ticks, {s.count('1')} ticks pressed")
