import json, glob, os, collections
ids = collections.defaultdict(list)
for f in glob.glob("/home/lorenzo-zurini/.VidyaGod/LIBRARY/*/*/*.json"):
    try: j = json.load(open(f))
    except Exception: continue
    nid = j.get("NODE_ID")
    if nid: ids[nid].append(f)
dups = {k: v for k, v in ids.items() if len(v) > 1}
print("total unique node ids:", len(ids))
if dups:
    print("GLOBAL COLLISIONS:", len(dups))
    for k, v in sorted(dups.items()):
        print("  ", k, "->", [os.path.basename(os.path.dirname(x)) for x in v])
else:
    print("NO COLLISIONS")
