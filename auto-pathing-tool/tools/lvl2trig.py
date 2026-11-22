#!/usr/bin/env python3
# Adds the trigger fields to a level export made before v0.5, read from the level string.
# Usage: lvl2trig.py levels/X.objects.jsonl levels/X.levelstring.txt out.objects.jsonl

import base64, gzip, json, sys, zlib

INT = {51: 'tgt', 71: 'ctr', 30: 'ease', 101: 'mtm', 395: 'tmc', 69: 't360', 401: 'rtid', 92: 'fyo', 82: 'ttm', 198: 'tpm',
       80: 'item', 95: 'item2', 534: 'ctl', 77: 'cnt', 88: 'cmp', 580: 'stop', 115: 'ord', 170: 'ch'}
FLT = {10: 'dur', 85: 'rate', 143: 'mmx', 144: 'mmy', 396: 'dmd', 68: 'rdeg', 402: 'roff', 72: 'fxm', 73: 'fym', 90: 'fys', 91: 'fyd',
       105: 'fymax', 371: 'zoom', 120: 'tw', 63: 'sdel', 556: 'sdr', 449: 'cmul', 148: 'grav'}
BITS = {58: 0, 59: 1, 141: 2, 142: 3, 100: 4, 394: 5, 397: 6, 544: 7, 70: 8, 56: 9, 81: 10, 89: 11, 441: 12, 93: 13, 535: 14, 94: 15,
        138: 16, 200: 17, 78: 18, 381: 19, 382: 20, 118: 21, 581: 22, 104: 23, 139: 24, 34: 26}
BOOLS = {11: 'touch', 62: 'spawn', 87: 'multi', 284: 'single', 117: 'rev', 504: 'rso'}

# decodes a level string (plain, or base64 url + gzip or zlib)
def decode(ls: str) -> str:
    ls = ls.strip()
    if ls.startswith('kS') or ls.startswith('kA'):
        return ls
    raw = base64.urlsafe_b64decode(ls + '=' * (-len(ls) % 4))
    try:
        return gzip.decompress(raw).decode('utf-8', 'replace')
    except Exception:
        return zlib.decompress(raw, 15 + 32).decode('utf-8', 'replace')

# splits a level string into objects, each a dict of numeric keys
def parse_objects(text: str):
    parts = text.split(';')
    objs = []
    for seg in parts[1:]:
        if not seg:
            continue
        kv = seg.split(',')
        d = {}
        for i in range(0, len(kv) - 1, 2):
            try:
                d[int(kv[i])] = kv[i + 1]
            except ValueError:
                pass
        if 1 in d:
            objs.append(d)
    return objs

# float of a string, or default
def num(s, default=0.0):
    try:
        return float(s)
    except Exception:
        return default

# matches every export object with its level string object and copies the trigger fields
def main():
    if len(sys.argv) < 4:
        print('usage: lvl2trig.py levels/X.objects.jsonl levels/X.levelstring.txt out.objects.jsonl')
        return 2
    exp_path, ls_path, out_path = sys.argv[1:4]
    lines = open(exp_path, encoding='utf-8').read().splitlines()
    text = decode(open(ls_path, encoding='utf-8').read())
    lobjs = parse_objects(text)
    by_key = {}
    for i, d in enumerate(lobjs):
        key = (int(num(d.get(1), 0)), round(num(d.get(2)), 2), round(num(d.get(3)) + 90.0, 2))
        by_key.setdefault(key, []).append(i)
    used = set()
    out = []
    matched = 0
    parents = []
    n_obj = 0
    for line in lines:
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get('ev') == 'level':
            rec['trig'] = 1
            out.append(json.dumps(rec, separators=(',', ':')))
            continue
        if 'uid' not in rec:
            out.append(line)
            continue
        n_obj += 1
        key = (rec['id'], round(rec['x'], 2), round(rec['y'], 2))
        cands = [i for i in by_key.get(key, []) if i not in used]
        if not cands:
            key2 = (rec['id'], round(rec.get('px', rec['x']), 2), round(rec.get('py', rec['y']), 2))
            cands = [i for i in by_key.get(key2, []) if i not in used]
        if cands:
            i = cands[0]
            used.add(i)
            d = lobjs[i]
            matched += 1
            if rec.get('cls') == 1 or 57 in d or 34 in d:
                for k, name in INT.items():
                    if k in d:
                        rec[name] = int(num(d[k]))
                for k, name in FLT.items():
                    if k in d:
                        rec[name] = num(d[k])
                for k, name in BOOLS.items():
                    if k in d:
                        rec[name] = num(d[k]) != 0
                tf = 0
                for k, bit in BITS.items():
                    if k in d and num(d[k]) != 0:
                        tf |= 1 << bit
                if 442 in d and d[442]:
                    tf |= 1 << 25
                if tf:
                    rec['tf'] = tf
                if 28 in d or 29 in d:
                    rec['mv'] = [num(d.get(28)), num(d.get(29))]
                if 109 in d and 'zoom' not in rec:
                    rec['zoom'] = 300.0 / (num(d[109]) * 30.0 + 300.0)
                if int(num(d.get(1), 0)) == 3022:
                    for k, name in ((351, 'tpsave'), (352, 'tpix'), (353, 'tpiy'), (345, 'tpsf'), (347, 'tprf'), (510, 'tpsg'), (591, 'tprd')):
                        if k in d and num(d[k]) != 0:
                            rec[name] = True
                    if 354 in d:
                        rec['tpgrav'] = int(num(d[354]))
                    if 346 in d:
                        rec['tpsfv'] = num(d[346])
                if int(num(d.get(1), 0)) == 2899:
                    opt = 0
                    for k, bit in ((160, 1), (165, 2), (199, 4), (593, 8)):
                        if k in d and num(d[k]) != 0:
                            opt |= bit
                    if opt:
                        rec['opt'] = opt
                if 34 in d and num(d[34]) != 0:
                    for g in rec.get('g', []):
                        parents.append((g, rec['uid']))
        out.append(json.dumps(rec, separators=(',', ':')))
    if parents:
        seen = set()
        pl = []
        for g, uid in parents:
            if g in seen:
                continue
            seen.add(g)
            pl.append([g, uid])
        out.insert(1, json.dumps({'ev': 'parents', 'p': pl}, separators=(',', ':')))
    open(out_path, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print(f'{n_obj} objects exported, {len(lobjs)} in the level string, {matched} matched ; {len(parents)} group parents')
    return 0

if __name__ == '__main__':
    sys.exit(main())
