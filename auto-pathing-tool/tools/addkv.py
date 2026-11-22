#!/usr/bin/env python3
# Adds the raw object properties of portals to a level export made before v0.5, read from the level string.
# Usage: addkv.py levels/X.objects.jsonl levels/X.levelstring.txt out.objects.jsonl

import sys,json,base64,zlib
PORT={12,13,47,111,660,745,1331,1933,286,287}
# decodes a base64 url + zlib level string
def dec(s):
    s=s.strip().replace('-','+').replace('_','/'); s+='='*(-len(s)%4)
    return zlib.decompress(base64.b64decode(s),15|32).decode('utf-8','replace')
src,ls,out=sys.argv[1:4]
cand={}
for o in dec(open(ls).read()).split(';')[1:]:
    p=o.split(',')
    if len(p)<6: continue
    d=dict(zip(p[::2],p[1::2]))
    try: i=int(d.get('1','0'))
    except: continue
    if i not in PORT: continue
    k=(i,round(float(d.get('2',0))*100),round((float(d.get('3',0))+90)*100))
    cand.setdefault(k,[]).append(o)
n=0
with open(out,'w') as f:
    for l in open(src):
        if '"kv"' not in l and '"id":' in l:
            o=json.loads(l)
            if o.get('id') in PORT and 'sp' in o:
                k=(o['id'],round(o['sp'][0]*100),round(o['sp'][1]*100))
                c=cand.get(k)
                if c:
                    o['kv']=c.pop(0); l=json.dumps(o,separators=(',',':'))+'\n'; n+=1
        f.write(l)
print(n, 'portals completed')
