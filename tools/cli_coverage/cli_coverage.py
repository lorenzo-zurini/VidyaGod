#!/usr/bin/env python3
"""Every function of every module, held against the CLI (the standing rule: everything is drivable from the CLI).

A function is
  covered    — a command names it in its Covers (VidyaGod commands --json),
  excluded   — exclusions.txt lists it (or a pattern matching it) WITH ITS REASON, or
  debt       — baseline.txt lists it: known, not yet covered. The baseline only ever shrinks.

The check fails when a function is none of the three (new code without its CLI path), when a baseline entry is
covered/excluded/gone (shrink the baseline: --update-baseline), and when an exclusion matches nothing.

What is counted: every function and every public or protected method declared in a project header (src/, VidyaGodFS/
src/), qualified by namespace and class, and every function the Go node exports (VidyaGodIPFS //export). Private
methods and file-static helpers are not: they are reached only through the counted functions that call them.

  tools/cli_coverage/cli_coverage.py --binary build/VidyaGod [--build build] [--report] [--update-baseline] [--init]
"""
import argparse, fnmatch, json, os, re, shlex, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KINDS = {}   # qualified name -> "{constructor}" / "{destructor}" / "{signal}": a whole kind, as a command or an exclusion names it
SECTION = [""]   # the Qt section the class walk is in (qt_signal / qt_slot / "")
ROOT = os.path.dirname(os.path.dirname(HERE))


def headers():
    out = []
    for base in ("src", "VidyaGodFS/src"):
        for d, _, fs in os.walk(os.path.join(ROOT, base)):
            for f in fs:
                if f.endswith((".h", ".hpp")):
                    out.append(os.path.join(d, f))
    return sorted(out)


def flags(build):
    db = json.load(open(os.path.join(build, "compile_commands.json")))
    probe = next(e for e in db if e["file"].endswith("src/cli/cliapp.cpp"))
    args = probe.get("arguments") or shlex.split(probe["command"])
    keep, skip = [], False
    for a in args[1:]:
        if skip: skip = False; continue
        if a in ("-o", "-c"): skip = a == "-o"; continue
        if a.endswith((".cpp", ".cc")) or a.startswith("-W") or a in ("-Werror", "-mno-direct-extern-access"): continue
        keep.append(a)
    # libclang needs its own builtin headers (stddef.h & co.): the compiler that built the project is GCC
    res = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
    return keep + (["-resource-dir", res] if res else []) + ["-D_FILE_OFFSET_BITS=64", "-DQT_ANNOTATE_ACCESS_SPECIFIER(x)=__attribute__((annotate(#x)))",
                                                    "-Wno-everything", "-ferror-limit=0"]


def cpp_functions(build):
    import clang.cindex as ci
    hs = headers()
    with tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False) as f:
        for h in hs:
            f.write('#include "%s"\n' % h)
        tu_path = f.name
    tu = ci.Index.create().parse(tu_path, args=flags(build),
                                 options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES | ci.TranslationUnit.PARSE_INCOMPLETE)
    os.unlink(tu_path)
    fatal = [d for d in tu.diagnostics if d.severity >= 4]
    if fatal: sys.exit("cli_coverage: the headers did not parse — the count would be short:\n  " + "\n  ".join(str(d) for d in fatal[:5]))
    hset = set(os.path.realpath(h) for h in hs)
    K = ci.CursorKind
    found = {}

    def qual(c):
        parts = []
        p = c
        while p is not None and p.kind != K.TRANSLATION_UNIT:
            if p.kind in (K.NAMESPACE, K.CLASS_DECL, K.STRUCT_DECL, K.CLASS_TEMPLATE, K.FUNCTION_DECL, K.CXX_METHOD,
                          K.FUNCTION_TEMPLATE, K.CONSTRUCTOR, K.DESTRUCTOR):
                if not p.spelling: return None        # an anonymous namespace / struct: internal
                parts.append(p.spelling)
            p = p.semantic_parent
        return "::".join(reversed(parts))

    def walk(c, depth=0):
        for ch in c.get_children():
            loc = ch.location.file
            if loc is None or os.path.realpath(loc.name) not in hset:
                if ch.kind == K.NAMESPACE: walk(ch, depth + 1)
                continue
            if ch.kind in (K.NAMESPACE, K.CLASS_DECL, K.STRUCT_DECL, K.CLASS_TEMPLATE):
                saved = SECTION[0]; SECTION[0] = ""
                walk(ch, depth + 1)
                SECTION[0] = saved
                continue
            if ch.kind == K.CXX_ACCESS_SPEC_DECL:
                # Qt's `signals:` / `slots:` carry an annotation (QT_ANNOTATE_ACCESS_SPECIFIER): what follows is one
                SECTION[0] = next((a.spelling for a in ch.get_children() if a.kind == K.ANNOTATE_ATTR), "")
                continue
            if ch.kind not in (K.FUNCTION_DECL, K.CXX_METHOD, K.FUNCTION_TEMPLATE, K.CONSTRUCTOR, K.DESTRUCTOR):
                continue
            if ch.kind in (K.CXX_METHOD, K.CONSTRUCTOR, K.DESTRUCTOR) and ch.access_specifier == ci.AccessSpecifier.PRIVATE:
                continue
            if ch.kind == K.FUNCTION_DECL and ch.linkage == ci.LinkageKind.INTERNAL:
                continue
            if ch.is_deleted_method() if hasattr(ch, "is_deleted_method") else False:
                continue
            q = qual(ch)
            if not q: continue
            rel = os.path.relpath(os.path.realpath(loc.name), ROOT)
            found.setdefault(q, "%s:%d" % (rel, ch.location.line))
            if SECTION[0] == "qt_signal": KINDS[q] = "{signal}"
            if ch.kind == K.CONSTRUCTOR: KINDS[q] = "{constructor}"
            elif ch.kind == K.DESTRUCTOR: KINDS[q] = "{destructor}"
    walk(tu.cursor)
    return found


def go_exports():
    out = {}
    d = os.path.join(ROOT, "VidyaGodIPFS")
    for f in sorted(os.listdir(d)):
        if not f.endswith(".go") or f.endswith("_test.go"): continue
        for n, line in enumerate(open(os.path.join(d, f)), 1):
            m = re.match(r"//export (\w+)", line)
            if m: out["VidyaGodIPFS::" + m.group(1)] = "VidyaGodIPFS/%s:%d" % (f, n)
    return out


def go_callers(build):
    """Which C++ functions call each Go export: an export is driven by whatever command drives its caller."""
    import clang.cindex as ci
    K = ci.CursorKind
    out = {}
    for src in ("src/ipfswrapper.cpp", "src/main.cpp"):
        tu = ci.Index.create().parse(os.path.join(ROOT, src), args=flags(build), options=ci.TranslationUnit.PARSE_INCOMPLETE)
        def walk(c, fn):
            for ch in c.get_children():
                f = fn
                if ch.kind in (K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR) and ch.is_definition() and ch.location.file \
                        and os.path.realpath(ch.location.file.name) == os.path.realpath(os.path.join(ROOT, src)):
                    parts, p = [], ch
                    while p is not None and p.kind != K.TRANSLATION_UNIT:
                        if p.spelling: parts.append(p.spelling)
                        p = p.semantic_parent
                    f = "::".join(reversed(parts))
                if ch.kind == K.CALL_EXPR and f and re.match(r"Vg[A-Z]", ch.spelling or ""):
                    out.setdefault("VidyaGodIPFS::" + ch.spelling, set()).add(f)
                walk(ch, f)
        walk(tu.cursor, None)
    return out


def covers(binary):
    r = subprocess.run([binary, "commands", "--json"], capture_output=True, text=True, timeout=120)
    if r.returncode != 0: sys.exit("cli_coverage: `%s commands --json` failed: %s" % (binary, r.stderr.strip()))
    out = {}
    for c in json.loads(r.stdout):
        for f in c["covers"]: out.setdefault(f, []).append(c["name"])
    return out


def exclusions():
    out = []
    for n, line in enumerate(open(os.path.join(HERE, "exclusions.txt")), 1):
        line = line.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"): continue
        if "#" not in line: sys.exit("cli_coverage: exclusions.txt:%d has no reason (pattern  # why it cannot be a command)" % n)
        pat, why = line.split("#", 1)
        if not why.strip(): sys.exit("cli_coverage: exclusions.txt:%d: an empty reason" % n)
        out.append((pat.strip(), why.strip(), n))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "VidyaGod"))
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--report", action="store_true", help="list every function and how it is held")
    ap.add_argument("--update-baseline", action="store_true", help="drop baseline entries that are no longer debt")
    ap.add_argument("--init", action="store_true", help="write the baseline from scratch (once, when adopting the rule)")
    a = ap.parse_args()

    funcs = cpp_functions(a.build)
    funcs.update(go_exports())
    cov = covers(a.binary)
    exc = exclusions()
    base_path = os.path.join(HERE, "baseline.txt")
    baseline = set()
    if os.path.exists(base_path):
        baseline = {l.split("#")[0].strip() for l in open(base_path) if l.strip() and not l.startswith("#")}

    callers = go_callers(a.build)
    held, uncovered, used_exc = {}, [], set()
    for q in sorted(funcs):
        if q in cov: held[q] = "covered by " + ", ".join(cov[q]); continue
        if KINDS.get(q) in cov: held[q] = "covered by " + ", ".join(cov[KINDS[q]]); continue
        by = sorted(f for f in callers.get(q, ()) if f in cov)
        if by: held[q] = "covered through " + ", ".join(by); continue
        hit = next(((p, w, n) for p, w, n in exc if fnmatch.fnmatchcase(q, p) or KINDS.get(q) == p), None)
        if hit: held[q] = "excluded: " + hit[1]; used_exc.add(hit[2]); continue
        uncovered.append(q)

    if a.init:
        with open(base_path, "w") as f:
            f.write("# CLI coverage debt: functions with no command yet (tools/cli_coverage/cli_coverage.py). This list only\n"
                    "# shrinks: cover a function (a command's Covers) or exclude it with its reason, then --update-baseline.\n")
            for q in uncovered: f.write("%s  # %s\n" % (q, funcs[q]))
        print("cli_coverage: baseline written: %d function(s) of debt" % len(uncovered))
        return 0

    new = [q for q in uncovered if q not in baseline]
    paid = sorted(baseline - set(uncovered))
    stale_exc = [(p, n) for p, w, n in exc if n not in used_exc]
    unknown_cov = sorted(set(cov) - set(funcs))
    n = len(funcs)
    print("cli_coverage: %d functions — %d covered, %d excluded, %d debt (baseline), %d NEW" %
          (n, sum(1 for v in held.values() if v.startswith("covered")), sum(1 for v in held.values() if v.startswith("excluded")),
           len(uncovered) - len(new), len(new)))
    if a.report:
        for q in sorted(funcs): print("  %-70s %s" % (q, held.get(q, "DEBT" if q in baseline else "NEW — uncovered")))
    if a.update_baseline:
        if new: print("cli_coverage: --update-baseline only drops paid debt; cover or exclude the NEW function(s) first")
        else:
            with open(base_path, "w") as f:
                f.write("# CLI coverage debt: functions with no command yet (tools/cli_coverage/cli_coverage.py). This list only\n"
                        "# shrinks: cover a function (a command's Covers) or exclude it with its reason, then --update-baseline.\n")
                for q in uncovered: f.write("%s  # %s\n" % (q, funcs[q]))
            print("cli_coverage: baseline now %d (was %d)" % (len(uncovered), len(baseline)))
            paid = []
    fail = False
    for q in new:
        print("NEW  %s  (%s) — give it a command (its Covers), or exclude it with its reason" % (q, funcs[q])); fail = True
    for q in paid:
        print("PAID %s — no longer debt: run --update-baseline" % q); fail = True
    for p, ln in stale_exc:
        print("STALE exclusions.txt:%d  %s matches no function" % (ln, p)); fail = True
    for q in unknown_cov:
        print("note: a command covers '%s', which is no function of the codebase (Qt's, or a typo)" % q)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
