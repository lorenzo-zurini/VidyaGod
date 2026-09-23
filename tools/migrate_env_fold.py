#!/usr/bin/env python3
"""Move the environment off the entrypoints and onto the node (generation 5: the environment folds).

`ENV` / `ENV_REMOVE` used to be fields of an ENTRYPOINTS entry. The environment is mutation and folds along the
chain like the registry, so it is a NODE section now: `ENV` (name → value) and `ENV_REMOVE` (names) at the top
level, and an entry that still carries them is refused by the lowerer.

  python3 tools/migrate_env_fold.py <LIBRARY dir> [--dry-run]
  python3 tools/migrate_env_fold.py --self-test

Every entry of one node must agree (the same ENV and ENV_REMOVE) — the tool refuses otherwise, since one node
has one environment. Backs up the library to <root>.pre-env-fold-backup-<ts>.tar before writing.
"""
import json, os, sys, tarfile, time, copy, glob


class Fail(Exception):
    pass


def load(path):
    with open(path) as F:
        return json.load(F)


def gather(root):
    nodes = {}
    for p in sorted(glob.glob(os.path.join(root, "**", "*.json"), recursive=True)):
        try:
            d = load(p)
        except ValueError:
            continue
        for n in (d if isinstance(d, list) else [d]):
            if isinstance(n, dict) and "CID" in n:
                nodes[n["CID"]] = (n, p)
    return nodes


def migrate(nodes):
    new, moved, refused = {}, 0, []
    for h, (n, p) in nodes.items():
        m = copy.deepcopy(n)
        envs, rms = [], []
        for e in m.get("ENTRYPOINTS", []) or []:
            if not isinstance(e, dict):
                continue
            if "ENV" in e:
                envs.append(e.pop("ENV"))
            if "ENV_REMOVE" in e:
                rms.append(e.pop("ENV_REMOVE"))
        if envs and any(x != envs[0] for x in envs):
            refused.append((n.get("LABEL"), "entries disagree on ENV")); new[h] = n; continue
        if rms and any(x != rms[0] for x in rms):
            refused.append((n.get("LABEL"), "entries disagree on ENV_REMOVE")); new[h] = n; continue
        if envs:
            if "ENV" in m and m["ENV"] != envs[0]:
                refused.append((n.get("LABEL"), "node already has a different ENV")); new[h] = n; continue
            m["ENV"] = envs[0]; moved += 1
        if rms:
            if "ENV_REMOVE" in m and m["ENV_REMOVE"] != rms[0]:
                refused.append((n.get("LABEL"), "node already has a different ENV_REMOVE")); new[h] = n; continue
            m["ENV_REMOVE"] = rms[0]; moved += 1
        # keep the key order stable and readable: the sections before ENTRYPOINTS
        if ("ENV" in m or "ENV_REMOVE" in m) and "ENTRYPOINTS" in m:
            items = [(k, v) for k, v in m.items() if k not in ("ENV", "ENV_REMOVE", "ENTRYPOINTS")]
            if "ENV" in m: items.append(("ENV", m["ENV"]))
            if "ENV_REMOVE" in m: items.append(("ENV_REMOVE", m["ENV_REMOVE"]))
            items.append(("ENTRYPOINTS", m["ENTRYPOINTS"]))
            m = dict(items)
        new[h] = m
    return new, moved, refused


def verify(nodes, new):
    for h, m in new.items():
        for e in m.get("ENTRYPOINTS", []) or []:
            if isinstance(e, dict) and ("ENV" in e or "ENV_REMOVE" in e):
                raise Fail(f"{h}: an entry still carries ENV")
        if "ENV" in m and (not isinstance(m["ENV"], dict) or any(not isinstance(v, str) for v in m["ENV"].values())):
            raise Fail(f"{h}: ENV must be an object of strings")
        if "ENV_REMOVE" in m and (not isinstance(m["ENV_REMOVE"], list) or any(not isinstance(v, str) for v in m["ENV_REMOVE"])):
            raise Fail(f"{h}: ENV_REMOVE must be a list of names")
        old = nodes[h][0]
        # nothing else changed
        a = {k: v for k, v in old.items() if k not in ("ENV", "ENV_REMOVE", "ENTRYPOINTS")}
        b = {k: v for k, v in m.items() if k not in ("ENV", "ENV_REMOVE", "ENTRYPOINTS")}
        if a != b:
            raise Fail(f"{h}: a field other than ENV/ENV_REMOVE/ENTRYPOINTS changed")
        # what the entries had is exactly what the node has now
        had_env = next((e["ENV"] for e in old.get("ENTRYPOINTS", []) or [] if isinstance(e, dict) and "ENV" in e), old.get("ENV"))
        had_rm = next((e["ENV_REMOVE"] for e in old.get("ENTRYPOINTS", []) or [] if isinstance(e, dict) and "ENV_REMOVE" in e), old.get("ENV_REMOVE"))
        if m.get("ENV") != had_env or m.get("ENV_REMOVE") != had_rm:
            raise Fail(f"{h}: the node's environment is not what its entries declared")


def self_test():
    nodes = {
        "a": ({"CID": "a", "LABEL": "runner", "ENTRYPOINTS": [{"HOST": "l", "GUEST": ["w"], "PATH": "p", "ENV": {"K": "V"}, "ENV_REMOVE": ["X"]}]}, "a.json"),
        "b": ({"CID": "b", "LABEL": "game", "LAYERS": [{"FORM": "zip", "PATH": "g.zip"}], "ENTRYPOINTS": [{"HOST": "w", "PATH": "g", "ENV": {"S": "0"}}, {"LABEL": "Editor", "HOST": "w", "PATH": "e", "ENV": {"S": "0"}}]}, "b.json"),
        "c": ({"CID": "c", "LABEL": "plain", "LAYERS": [{"FORM": "dir", "PATH": "d"}]}, "c.json"),
        "d": ({"CID": "d", "LABEL": "disagree", "ENTRYPOINTS": [{"HOST": "w", "PATH": "g", "ENV": {"S": "0"}}, {"HOST": "w", "PATH": "e", "ENV": {"S": "1"}}]}, "d.json"),
    }
    new, moved, refused = migrate(nodes)
    assert new["a"]["ENV"] == {"K": "V"} and new["a"]["ENV_REMOVE"] == ["X"] and "ENV" not in new["a"]["ENTRYPOINTS"][0]
    assert list(new["a"].keys()) == ["CID", "LABEL", "ENV", "ENV_REMOVE", "ENTRYPOINTS"]
    assert new["b"]["ENV"] == {"S": "0"} and all("ENV" not in e for e in new["b"]["ENTRYPOINTS"])
    assert new["c"] == nodes["c"][0]
    assert refused == [("disagree", "entries disagree on ENV")] and new["d"] == nodes["d"][0]
    verify({h: v for h, v in nodes.items() if h != "d"}, {h: v for h, v in new.items() if h != "d"})
    bad = copy.deepcopy(new); bad["a"]["ENV"]["K"] = "changed"
    try:
        verify(nodes, bad); raise SystemExit("verify missed a changed value")
    except Fail:
        pass
    print("self-test: ok")


def main():
    if "--self-test" in sys.argv:
        self_test(); return
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    root = args[0]; dry = "--dry-run" in sys.argv
    nodes = gather(root)
    new, moved, refused = migrate(nodes)
    for label, why in refused:
        print(f"   REFUSED '{label}': {why}")
    if refused:
        sys.exit("refusing: fix the nodes above first")
    verify(nodes, new)
    changed = [h for h in new if new[h] != nodes[h][0]]
    print(f"{len(nodes)} node(s): {moved} environment field(s) moved onto {len(changed)} node(s); verified")
    if dry:
        print(f"[dry-run] would rewrite {len(changed)} node file(s)"); return
    ts = time.strftime("%Y%m%d-%H%M%S")
    tar = os.path.normpath(root) + f".pre-env-fold-backup-{ts}.tar"
    with tarfile.open(tar, "w") as T:
        T.add(root, arcname=os.path.basename(os.path.normpath(root)))
    for h in changed:
        with open(nodes[h][1], "w") as F:
            json.dump(new[h], F, indent=2, ensure_ascii=False); F.write("\n")
    print(f"rewrote {len(changed)} node file(s); backup: {tar}")


if __name__ == "__main__":
    main()
