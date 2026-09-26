#!/usr/bin/env python3
"""Builds the launch-matrix fixture: a synthetic library exercising every launch-engine feature.

The point is COVERAGE, not realism. Every node type, every mode of every type, and the combinations that have
actually broken before (a delta over the zip beneath it, a WHEN that reads inert and fires anyway, a runner chain
two hops long, a graft on a graft) exist here so that touching the launch engine has something to fail against.

Everything is generated, deterministically, from this file: content zips are STORE (the format's own
requirement), file mtimes are pinned, and no bytes are committed to the repo. Run it, resolve the launchables,
diff against tests/fixtures/launchmatrix/golden/.

ADDING A FEATURE: add it to the matrix here, re-run the harness with --update, and commit the golden diff. The
diff IS the review — it shows exactly what the new feature changed about every plan it touches.
"""
import json, os, shutil, stat, sys, zipfile

FIXED_DATE = (2020, 1, 1, 0, 0, 0)     # pinned so a regenerated zip is byte-identical

def store_zip(path, entries):
    """A STORE (uncompressed) zip — the format refuses DEFLATE in mountable content."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as Z:
        for name, data in sorted(entries.items()):
            zi = zipfile.ZipInfo(name, date_time=FIXED_DATE)
            zi.external_attr = 0o644 << 16
            Z.writestr(zi, data)

def write(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as F:
        F.write(data if isinstance(data, bytes) else data.encode())
    os.chmod(path, mode)


# The synthetic "game": it reports what the launch engine actually handed it, in a fixed order, so its stdout
# can be a golden. Everything it prints is a claim about the ENGINE — the composed mount, the edits that were
# applied, the bytes that were patched — not about itself.
PROBE = r"""#!/bin/sh
set -u
echo "=== argv"
i=0; for a in "$@"; do echo "  [$i] $a"; i=$((i+1)); done
echo "=== cwd"
echo "  $(pwd)"
echo "=== env (matrix only)"
env | grep -E '^LM_' | sort | sed 's/^/  /' || true
echo "=== mounted tree"
find . -not -path '*/.*' | LC_ALL=C sort | sed 's/^/  /'
echo "=== layer order (a path contributed by two zips resolves to the upper one)"
echo "  shared.txt: $(cat game/shared.txt 2>/dev/null || echo MISSING)"
echo "  added.txt:  $(cat game/added.txt  2>/dev/null || echo MISSING)"
echo "=== FileEdit results"
echo "  --- config.ini"; sed 's/^/    /' game/config.ini 2>/dev/null || echo "    MISSING"
echo "  --- written.txt"; sed 's/^/    /' game/written.txt 2>/dev/null || echo "    MISSING"
echo "  --- conditional.txt (WHEN %lm_flag% == 1)"; sed 's/^/    /' game/conditional.txt 2>/dev/null || echo "    MISSING"
echo "=== BinaryPatch result (.text, where Replace/Poke landed)"
od -An -tx1 -j 1024 -N40 game/patch.exe 2>/dev/null | tr -s ' ' | sed 's/^ */  /' || echo "  MISSING"
echo "  cave region: $(od -An -tx1 -j 1088 -N8 game/patch.exe 2>/dev/null | tr -s ' ')"
echo "=== delta reconstruction"
echo "  chain/added.txt:     $(cat deltatarget/chain/added.txt 2>/dev/null || echo MISSING)"
echo "  deltatarget/masked.txt (opaque delta must hide it): $(cat deltatarget/masked.txt 2>/dev/null || echo MASKED)"
echo "=== submount"
echo "  unpacked/relocated.txt: $(cat unpacked/relocated.txt 2>/dev/null || echo MISSING)"
echo "  carrier/nested/ignored.txt (must be absent): $(cat carrier/nested/ignored.txt 2>/dev/null || echo ABSENT)"
echo "=== take (a library, contained selectively and placed at libs/)"
echo "  libs/kept.dll:    $(cat libs/kept.dll 2>/dev/null || echo MISSING)"
echo "  libs/readme.txt:  $(cat libs/readme.txt 2>/dev/null || echo MISSING)"
echo "  libs/renamed.txt: $(cat libs/renamed.txt 2>/dev/null || echo MISSING)"
echo "  drop.dll / keep.dll / libfile.txt (must be absent): $(ls libs/bin libs/drop.dll libs/keep.dll libs/libfile.txt 2>/dev/null || echo ABSENT)"
echo "  shared/diamond.txt: $(cat shared/diamond.txt 2>/dev/null || echo MISSING)"
echo "=== anchors (a file placed at %Documents% lands where the runner lays its drive out)"
echo "  Documents/lm/single.txt: $(cat ../users/player/Documents/lm/single.txt 2>/dev/null || echo MISSING)"
echo "=== writability (a KEEP dir must be writable, and survive)"
mkdir -p game/saves 2>/dev/null && echo "written by the probe" > game/saves/save.txt 2>/dev/null \
  && echo "  game/saves: writable" || echo "  game/saves: NOT writable"
echo "=== done"
"""

# ---------------------------------------------------------------------------------------------------------
# content
# ---------------------------------------------------------------------------------------------------------


def minimal_pe():
    """A hand-built, minimal PE32 — BinaryPatch refuses anything that is not a PE image.

    Built here rather than compiled so the fixture needs no cross-toolchain and is byte-identical everywhere.
    One .text section at RVA 0x1000 over ImageBase 0x400000: the first 64 bytes are a known ramp (00..3f) for
    Replace/Poke to hit with an EXPECT guard, and the rest is zero — the slack a Cave with CAVE "auto" has to
    find for itself.
    """
    import struct
    IMAGE_BASE, SEC_RVA, RAW_PTR, RAW_SIZE = 0x400000, 0x1000, 0x400, 0x400
    pe_off = 0x80
    dos = bytearray(pe_off)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, pe_off)

    coff = struct.pack("<4sHHIIIHH", b"PE\0\0", 0x014C, 1, 0, 0, 0, 0xE0, 0x0102)
    opt = bytearray(0xE0)
    struct.pack_into("<H", opt, 0, 0x10B)                       # PE32
    struct.pack_into("<I", opt, 16, SEC_RVA)                    # AddressOfEntryPoint
    struct.pack_into("<I", opt, 28, IMAGE_BASE)                 # ImageBase (PE32 reads it here)
    struct.pack_into("<I", opt, 32, 0x1000)                     # SectionAlignment
    struct.pack_into("<I", opt, 36, 0x200)                      # FileAlignment
    struct.pack_into("<I", opt, 56, SEC_RVA + RAW_SIZE)         # SizeOfImage
    struct.pack_into("<I", opt, 60, RAW_PTR)                    # SizeOfHeaders

    sec = struct.pack("<8sIIIIIIHHI", b".text\0\0\0", RAW_SIZE, SEC_RVA, RAW_SIZE, RAW_PTR,
                      0, 0, 0, 0, 0x60000020)                   # code | execute | read
    head = bytes(dos) + coff + bytes(opt) + sec
    assert len(head) <= RAW_PTR, len(head)
    body = bytearray(RAW_SIZE)
    body[0:64] = bytes(range(64))                               # the ramp Replace/Poke target
    return head + bytes(RAW_PTR - len(head)) + bytes(body)

def make_content(B):
    # A base zip and a patch zip that OVERLAPS it: layer order is the thing under test, so one path must be
    # contributed by both.
    store_zip(f"{B}/base.zip", {
        "game/data.txt":      "base\n",
        "game/shared.txt":    "from-base\n",
        "game/deep/leaf.txt": "leaf\n",
        "game/config.ini":    "[General]\nSetting=0\nFromVar=unset\nKeep=me\n",
        "game/patch.exe":     minimal_pe(),
    })
    store_zip(f"{B}/patch.zip", {"game/shared.txt": "from-patch\n", "game/added.txt": "added\n"})
    # A SUBMOUNT relocates ONE FILE out of the archive to a path of its own — it does not mount a nested
    # archive. Declaring submounts also REPLACES the layer's whole-archive mount: only the listed files appear.
    store_zip(f"{B}/carrier.zip", {"nested/relocated.txt": "relocated by a submount\n",
                                   "nested/ignored.txt":   "not submounted, so never visible\n"})
    # FORM dir and FORM file.
    write(f"{B}/loosedir/note.txt", "loose-dir\n")
    # Mounted UNDER the delta at the same target, to prove the delta is OPAQUE. A base zip cannot show this:
    # it is folded into the delta chain as a byte input and never becomes a layer at all, so opacity has
    # nothing to mask. A dir layer is not a byte view, is not folded, and must therefore disappear.
    write(f"{B}/maskeddir/masked.txt", "must be masked by the opaque delta above\n")
    write(f"{B}/single.txt", "single-file\n")
    # Grafts (see the node matrix): what a mod mounts above the launchable, what an unticked one must not, what a
    # branch off an ANCESTOR must never contribute, and a library pulled in as substance.
    write(f"{B}/graftdir/game/grafted.txt", "grafted above lm_run\n")
    write(f"{B}/grafthd/game/grafted_hd.txt", "a graft on a graft\n")
    write(f"{B}/graftoff/game/never.txt", "an unticked graft must not mount\n")
    write(f"{B}/graftwrong/game/wrong.txt", "a branch off an ancestor must not mount\n")
    write(f"{B}/graftlib/game/lib.txt", "substance pulled in beneath a graft\n")
    # TAKE: a library whose zip holds more than the matrix takes — only kept.dll (renamed), doc/'s contents and the
    # renamed loose file may appear, under libs/.
    store_zip(f"{B}/lib.zip", {"bin/keep.dll": "taken and renamed\n", "bin/drop.dll": "never taken\n",
                               "doc/readme.txt": "taken: doc/'s contents land in libs/\n"})
    write(f"{B}/libfile.txt", "a loose FILE, taken and renamed\n")
    # A diamond: two nodes contain the same one; it mounts once.
    write(f"{B}/shareddir/diamond.txt", "one occurrence, however many containers\n")
    # A graft whose NOT hits the row: pre-ticked, and still never mounted.
    write(f"{B}/graftconflict/game/conflict.txt", "a graft whose NOT hits must not mount\n")
    # The final chain: a variant that inherits its entry, and a graft that carries one.
    write(f"{B}/inheritdir/game/inherited.txt", "a variant over lm_run, running lm_run's inherited entry\n")
    write(f"{B}/graftentry/game/from_graft_entry.txt", "a graft that is also a way to run\n")
    # FORM delta — REAL ones, generated below by vg_make_delta.
    #
    # A delta reconstructs a COMPLETE archive and is OPAQUE: it masks everything below it at its own target.
    # So each delta here gets its own target with a predictable byte view underneath, and its target zip holds
    # the whole tree it is meant to produce. (Pointing a delta at a target that already stacks base+patch would
    # base it on the LAST zip there, which is a fine thing to test but a terrible thing to hand-compute.)
    store_zip(f"{B}/delta_target.zip", {
        "chain/data.txt":   "base\n",
        "chain/added.txt":  "added by the delta\n",
        "chain/deep/x.txt": "x\n",
    })
    # The synthetic "game" and the synthetic runner binary. probe.sh is mounted at its own sub-target so the
    # opaque delta above cannot mask it.
    write(f"{B}/probe.sh", PROBE, 0o755)
    #Drops its own flag then runs the content through an interpreter, so the chain does not depend on the
    #mount preserving an executable bit either.
    write(f"{B}/fakerunner.sh", '#!/bin/sh\n[ "$1" = "--run" ] && shift\nexec /bin/sh "$@"\n', 0o755)

def make_deltas(B, tool):
    """Real .vgdelta files, or None if the generator was not built (the fixture then has no delta coverage)."""
    if not tool or not os.path.isfile(tool):
        return False
    import subprocess
    # over_base: base = the nearest content beneath at the delta's own target (base.zip).
    subprocess.run([tool, f"{B}/over_base.vgdelta", f"{B}/delta_target.zip", f"{B}/base.zip"], check=True)
    return True

# ---------------------------------------------------------------------------------------------------------
# the node matrix (generation 6: a node is {LABEL, VARIANT?, RECOMMENDED?, LAYERS}; its parents are NODE layers)
# ---------------------------------------------------------------------------------------------------------

UID = "90000000000001"
G = "%GameDir%"                             # the game's folder, by anchor (the runner says where it is)
# Both fixture runners lay a guest out the way a wine prefix does: each anchor is a guest (Windows) path — what a
# value naming it reads — and the drive lives at %PrefixRoot%/drive_c in the runner's layout — where a file lands.
ROOTS = {"%GameDir%": "C:\\%PackageUID%", "%Documents%": "C:\\users\\player\\Documents"}
DRIVES = {"C:": "%PrefixRoot%/drive_c"}

def F(path=""):
    """A FILES address."""
    return "FILES/" + path if path else "FILES"

def nodes():
    N = []
    def add(label, layers, **fields):
        # The readable LABEL doubles as the working-tree handle (the stored "CID"), so NODE layers stay legible;
        # the freeze derives the real CID and remaps every reference to it.
        N.append(dict({"CID": label, "LABEL": label}, **fields, LAYERS=layers))
        return label
    def node(ref, **kw):
        return dict({"NODE": ref}, **kw)

    # ---- identity: the tile rides the "Play" entry — a partial entry every variant folds with its own -----
    add("lm_tile", [{"EXEC": [{"LABEL": "Play", "TILE": {"UID": UID, "TITLE": "Launch Matrix",
                                                           "META": {"SERIES": "Fixtures", "YEAR": "2026"}}}]}])

    # ---- runners: a native terminal, and a two-hop chain through a synthetic "prefix" runner ------------
    # Runs the content through an explicit interpreter rather than exec'ing it directly, so the fixture does
    # not depend on the FUSE mount preserving an executable bit.
    add("lm_runner_native", [
        # The runner sets both: one the launchable overrides, one it REMOVES. Removal has to beat the runner,
        # or a launchable can never get rid of something its runner insists on.
        # LM_GAME_KEEPS_THIS: null is LOAD-BEARING — as an OUTER link of lm_run_chained's chain this runner asks
        # for it to be removed, and the game sets it, so the golden's "LM_GAME_KEEPS_THIS=survived-the-outer-remove"
        # is a real assertion that an outer wrapper cannot strip a key the GAME declared.
        {"ENV": {"LM_RUNNER_ONLY": "from-runner", "LM_SHOULD_BE_GONE": "runner-set-this",
                 "LM_EXEC_ENV": "runner-loses", "LM_GAME_KEEPS_THIS": None}},
        {"EXEC": [{"LABEL": "lm_runner_native", "HOST": "linux64", "GUEST": ["linux64"], "EXE": "/bin/sh",
                   "ARGS": ["%Content%"], "GUEST_ROOTS": ROOTS, "DRIVES": DRIVES}]}])
    add("lm_runner_content", [{"FILE": "fakerunner.sh", "TARGET": F("runner/fakerunner.sh")}])
    # HOST linux64 / GUEST fixture32 ⇒ running fixture32 content takes two hops: this, then the native one.
    add("lm_runner_prefix", [
        node("lm_runner_content"),
        {"ENV": {"LM_RUNNER_ENV": "set", "LM_FROM_VAR": "%lm_text%", "LM_UNWANTED": None}},
        {"EXEC": [{"LABEL": "lm_runner_prefix", "HOST": "linux64", "GUEST": ["fixture32"],
                   "EXE": "%RunnerMount%/runner/fakerunner.sh", "ARGS": ["--run"],
                   "CONTENT_ROOT": "pfx/drive_c/%PackageUID%", "PREFIX_GENERATE": False,
                   "GUEST_ROOTS": ROOTS, "DRIVES": DRIVES}]}])

    # ---- content: every payload type, every TARGET shape, submounts, deltas -----------------------------
    base   = add("lm_c_zip_base", [{"ZIP": "base.zip", "TARGET": F(G), "COMMENT": "the base layer"}])
    patch  = add("lm_c_zip_patch", [node(base), {"ZIP": "patch.zip", "TARGET": F(G)}])
    # SUBMOUNTS: "source/path:dest" strings; declaring them REPLACES the layer's whole-archive mount.
    subm   = add("lm_c_submount", [node(patch), {"ZIP": "carrier.zip", "TARGET": F(G + "/carrier"),
                                                 "SUBMOUNTS": [f"nested/relocated.txt:{G}/unpacked/relocated.txt"]}])
    dirl   = add("lm_c_dir", [node(subm), {"DIR": "loosedir", "TARGET": F(G + "/loose")}])
    # A FILE layer's TARGET is the CONTAINING DIRECTORY — the file appears as TARGET/<its name>.
    filel  = add("lm_c_file", [node(dirl), {"FILE": "single.txt", "TARGET": F(G + "/loosefile")}])
    # A DELTA reconstructs a COMPLETE archive from the nearest content beneath it at its target (the zip it was
    # built on) and MASKS everything below: the dir under that zip must disappear (opacity), and a dir is not a
    # byte view, so it sits BENEATH the base (a dir between would be "the nearest content", a refused base).
    masked = add("lm_c_masked_dir", [node(filel), {"DIR": "maskeddir", "TARGET": F(G + "/deltatarget")}])
    dbase  = add("lm_c_delta_base", [node(masked), {"ZIP": "base.zip", "TARGET": F(G + "/deltatarget")}])
    d1     = add("lm_c_delta", [node(dbase), {"DELTA": "over_base.vgdelta", "TARGET": F(G + "/deltatarget")}])
    # The probe sits at its own sub-target so the opaque delta cannot mask it.
    probe  = add("lm_c_probe", [node(d1), {"FILE": "probe.sh", "TARGET": F(G + "/bin")}])
    # Un-hydrated content: present only as a CID (a real, well-formed one whose block is simply never in the local
    # store). The engine REFUSES to launch with missing sources, so this hangs off a SIDE BRANCH — only the
    # plan-level launchables contain it.
    remote = add("lm_c_remote", [node(probe), {"ZIP": "never_fetched.zip", "TARGET": F(G + "/remote"),
                                               "SOURCE": "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e"}])
    add("lm_unhydrated", [node(remote)])

    # ---- VARS: defaults, UI kinds, cross-reference, a gated value -----------------------------------------
    v1 = add("lm_v_text", [node(probe), {"VARS": {"lm_text": {"DEFAULT": "hello",
                                                              "UI": {"LABEL": "Text", "CONTROL": "text", "GROUP": "Matrix"}}}}])
    v2 = add("lm_v_enum", [node(v1), {"VARS": {"lm_mode": {"DEFAULT": "beta", "UI": {
        "LABEL": "Mode", "CONTROL": "enum", "GROUP": "Matrix",
        "CHOICES": [{"LABEL": "Alpha", "VALUE": "alpha"}, {"LABEL": "Beta", "VALUE": "beta"}]}}}}])
    v3 = add("lm_v_bool", [node(v2), {"VARS": {"lm_flag": {"DEFAULT": "1",
                                                           "UI": {"LABEL": "Flag", "CONTROL": "bool", "GROUP": "Matrix"}}}}])
    # A DEFAULT that references other variables — resolution is a fixpoint, so forward order must not matter.
    v4 = add("lm_v_derived", [node(v3), {"VARS": {"lm_derived": {"DEFAULT": "%lm_text%-%lm_mode%"}}}])
    # A declaration's WHEN gates its VALUE (to ""), inside the fixpoint.
    v5 = add("lm_v_conditional", [node(v4), {"VARS": {"lm_conditional": {"DEFAULT": "on-because-beta",
                                                                         "WHEN": "%lm_mode% == beta"}}}])

    # ---- EDIT: text modes and binary modes, applied at their position in the fold -------------------------
    fe1 = add("lm_fe_config", [node(v5), {"EDIT": [{"MODE": "ConfigWrite", "KEY": "Setting=", "VALUE": "1"},
                                                   {"MODE": "ConfigWrite", "KEY": "FromVar=", "VALUE": "%lm_derived%"}],
                                          "TARGET": F(G + "/game/config.ini")}])
    fe2 = add("lm_fe_overwrite", [node(fe1), {"EDIT": [{"MODE": "Overwrite", "VALUE": "overwritten by the matrix\n"}],
                                              "TARGET": F(G + "/game/written.txt")}])
    fe3 = add("lm_fe_append", [node(fe2), {"EDIT": [{"MODE": "AppendLine", "VALUE": "; appended once",
                                                     "COMMENT": "idempotent by contract"}],
                                           "TARGET": F(G + "/game/config.ini")}])
    # A relative TARGET: relative to where the node landed (here the mount root).
    fe4 = add("lm_fe_relative", [node(fe3), {"EDIT": [{"MODE": "Overwrite", "VALUE": "relative target\n"}],
                                             "TARGET": F("relative.txt")}])
    # A gated layer: inert unless the flag is on. A layer WHEN reads only phase-1 variables (ungated VARS reached
    # through ungated NODE layers) — lm_flag is one. The bug class: a WHEN that reads inert and fires anyway.
    fe5 = add("lm_fe_when", [node(fe4), {"EDIT": [{"MODE": "Overwrite", "VALUE": "flag was on\n"}],
                                         "TARGET": F(G + "/game/conditional.txt"), "WHEN": "%lm_flag% == 1"}])
    # Binary modes name their bytes differently: Replace->REPLACE, Poke->VALUE, Cave->PAYLOAD.
    bp = add("lm_bp", [node(fe5), {"EDIT": [
        {"MODE": "Replace", "OFFSET": "0x401000", "EXPECT": "000102", "REPLACE": "aabbcc",
         "COMMENT": "VA replace behind an EXPECT guard"},
        {"MODE": "Poke", "OFFSET": "0x401010", "VALUE": "ff"},
        # EXPECT must be >= 5 bytes: the patcher replaces it with a jmp rel32 to the cave.
        {"MODE": "Cave", "OFFSET": "0x401020", "EXPECT": "2021222324", "PAYLOAD": "9090",
         "CAVE": "auto", "COMMENT": "cave into section slack the patcher picks itself"}],
        "TARGET": F(G + "/game/patch.exe")}])

    # ---- REG: both views, a default value, a key-only entry, a gated layer ---------------------------------
    reg = add("lm_reg", [node(bp),
        {"REG": {"HKLM": {"Software": {"LaunchMatrix": {"Value": "plain", "Number": "dword:0000002a",
                                                        "FromVar": "%lm_mode%"}}}},
         "ARCH": ["32", "64"], "COMMENT": "written into both views"},
        {"REG": {"HKCU": {"Software": {"LaunchMatrix": {"": "this is the key's DEFAULT value", "EmptyKey": {}}}}},
         "ARCH": ["64"]},
        {"REG": {"HKLM": {"Software": {"LaunchMatrixConditional": {"Only": "when the flag is on"}}}},
         "WHEN": "%lm_flag% == 1"}])

    # ---- DLL ---------------------------------------------------------------------------------------------
    dll = add("lm_dll", [node(reg), {"DLL": {"ddraw": "n,b", "dinput8": "n,b", "winmm": "b,n", "broken": ""}}])

    # ---- KEEP: a dir, a single file, a machine-local dir (CLOUD false), registry keys -------------------
    per = add("lm_keep", [node(dll), {"KEEP": {
        F(G + "/game/saves/"): {"NAME": "Saves"},
        F(G + "/game/config.ini"): {"NAME": "Config"},
        F(G + "/game/shadercache/"): {"NAME": "ShaderCache", "CLOUD": False},
        "REG/HKCU": True,
        "REG/HKLM/Software/LaunchMatrix": True}}])

    # ---- TAKE: a library contained selectively and placed. Left-strip: a file lands under its own name, a
    # directory's CONTENTS ("FILES/doc/") land at the target; a pair renames. Facts are taken too: lm_lib_var
    # arrives, lm_lib_hidden does not; the DLL is not taken at all.
    add("lm_lib", [{"ZIP": "lib.zip"}, {"FILE": "libfile.txt"},
                   {"VARS": {"lm_lib_var": {"DEFAULT": "from-the-library"}, "lm_lib_hidden": {"DEFAULT": "not-taken"}}},
                   {"DLL": {"libonly": "n"}}])
    lib = add("lm_uses_lib", [node(per), node("lm_lib", TAKE=[["FILES/bin/keep.dll", "FILES/kept.dll"], "FILES/doc/",
                                                               ["FILES/libfile.txt", "FILES/renamed.txt"], "VARS/lm_lib_var"],
                                               TARGET=F(G + "/libs"))])
    # ---- a diamond: two containers, one occurrence (at its first position)
    add("lm_shared", [{"DIR": "shareddir", "TARGET": F(G + "/shared")}])
    add("lm_diamond_a", [node("lm_shared")])
    add("lm_diamond_b", [node("lm_shared")])
    dia = add("lm_diamond", [node(lib), node("lm_diamond_a"), node("lm_diamond_b")])

    # ---- ENV beneath the launchable: it folds along the resolution, so it reaches the process unless a later
    # layer overrides (LM_OVERRIDDEN) or removes (LM_REMOVED_ABOVE) it.
    # A value naming an anchor reads the guest path (LM_GAMEDIR = C:\\<UID>); a file placed at an anchor lands where
    # the runner's drive lives (%Documents% → <PrefixRoot>/drive_c/users/player/Documents).
    grp = add("lm_group", [node(dia), {"ENV": {"LM_FOLDED": "from-below", "LM_OVERRIDDEN": "below",
                                               "LM_REMOVED_ABOVE": "set-below", "LM_GAMEDIR": "%GameDir%"}},
                           {"FILE": "single.txt", "TARGET": F("%Documents%/lm")}])

    def play(host, exe, args, **kw):
        return {"EXEC": [dict({"LABEL": "Play", "HOST": host, "EXE": exe, "ARGS": args}, **kw)]}

    # ---- variants --------------------------------------------------------------------------------------
    # (1) the whole matrix, on the native runner (plan only: it contains the un-hydrated branch).
    add("lm_all", [node(grp), node("lm_unhydrated"), node("lm_tile"),
                   {"ENV": {"LM_EXEC_ENV": "does-this-arrive"}},
                   play("linux64", G + "/probe.sh", ["--matrix", "%lm_derived%"])],
        VARIANT="lm_all", RECOMMENDED=[UID])
    # (2) THE RUNTIME CASE: mounts for real and runs the probe, whose stdout is its own golden. WORKDIR anchors
    # the probe's report; its own ENV (with a %var%) is the end-to-end proof a game's environment reaches it.
    add("lm_run", [node(grp), node("lm_tile"),
                   {"ENV": {"LM_EXEC_ENV": "arrived", "LM_FROM_VAR": "%lm_derived%", "LM_OVERRIDDEN": "above",
                            "LM_SHOULD_BE_GONE": None, "LM_REMOVED_ABOVE": None}},
                   play("linux64", G + "/bin/probe.sh", ["--matrix", "%lm_derived%"], WORKDIR=G)],
        VARIANT="lm_run")
    # (3) THE RUNTIME TWO-HOP CASE: the same resolution through the chain; its ENV collides with the OUTER link's
    # on purpose — the game's value must survive (an exec-time property no plan can show).
    add("lm_run_chained", [node(grp), node("lm_tile"),
                           {"ENV": {"LM_EXEC_ENV": "game-beats-the-outer-link",
                                    "LM_GAME_KEEPS_THIS": "survived-the-outer-remove"}},
                           play("fixture32", G + "/bin/probe.sh", ["--chained"], WORKDIR=G)],
        VARIANT="lm_run_chained")
    # (4) the matrix through the two-hop chain, plus the un-hydrated branch (plan only).
    add("lm_chained", [node(grp), node("lm_unhydrated"), node("lm_tile"),
                       {"ENV": {"LM_EXEC_ENV": "game-wins-over-the-chain"}},
                       play("fixture32", G + "/probe.sh", [])],
        VARIANT="lm_chained")
    # (5) the minimum that can launch at all — the control case a regression shows up against first.
    add("lm_minimal_content", [{"ZIP": "base.zip", "TARGET": F(G)}])
    add("lm_minimal", [node("lm_minimal_content"), node("lm_tile"), play("linux64", G + "/game/data.txt", [])],
        VARIANT="lm_minimal")
    # (6) a BLOCKED variant: a NOT names something it contains — the launch is refused, naming the reason.
    add("lm_blocked", [node("lm_minimal_content"), {"NOT": "lm_minimal_content"}, node("lm_tile"),
                       play("linux64", G + "/game/data.txt", [])],
        VARIANT="lm_blocked")
    # (7) a variant over lm_run that declares no entry: it runs lm_run's (folded), mounts lm_run's whole
    # resolution plus its own dir — and, lm_run being in its resolution, is offered lm_run's grafts too.
    add("lm_inherits", [node("lm_run"), {"DIR": "inheritdir", "TARGET": F(G)}], VARIANT="lm_inherits")

    # ---- grafts: a node whose list begins with ANY (what it applies onto) ----------------------------------
    # Offered to a row whose resolution contains a member of its ANY, applied above the variant in the
    # instance's order; a fresh instance ticks the ones RECOMMENDED under the row's tile. The probe's "mounted
    # tree" is the proof: grafted.txt, grafted_hd.txt (a graft on a graft: offered once lm_graft_on is applied)
    # and lib.txt (a library the graft contains) appear; never.txt (offered, not recommended) does not, and
    # elsewhere.txt (its ANY names lm_minimal) is offered to lm_minimal only.
    add("lm_graft_on", [{"ANY": ["lm_run"]}, {"DIR": "graftdir", "TARGET": F(G)},
                        {"ENV": {"LM_GRAFT_ENV": "grafted"}}],          # a ticked graft's ENV folds above the variant's
        RECOMMENDED=[UID])
    add("lm_graft_hd", [{"ANY": ["lm_graft_on"]}, {"DIR": "grafthd", "TARGET": F(G)}], RECOMMENDED=[UID])
    add("lm_graft_off", [{"ANY": ["lm_run"]}, {"DIR": "graftoff", "TARGET": F(G)}])
    # Offered (its ANY holds) and pre-ticked — but its NOT names something lm_run contains, so it does not apply.
    add("lm_graft_conflict", [{"ANY": ["lm_run"]}, {"NOT": "lm_c_probe"}, {"DIR": "graftconflict", "TARGET": F(G)}],
        RECOMMENDED=[UID])
    add("lm_graft_elsewhere", [{"ANY": ["lm_minimal"]}, {"DIR": "graftwrong", "TARGET": F(G)}], RECOMMENDED=[UID])
    add("lm_graft_lib", [{"DIR": "graftlib", "TARGET": F(G)}])
    add("lm_graft_uses_lib", [{"ANY": ["lm_run"]}, node("lm_graft_lib")], RECOMMENDED=[UID])
    # A graft that CARRIES an entry (a mod loader): ticked, it mounts above the variant like any graft, and its
    # entry is a way to RUN that mount (--entrypoint lm_graft_entry). Its ENV folds above lm_run's.
    add("lm_graft_entry", [{"ANY": ["lm_run"]}, {"DIR": "graftentry", "TARGET": F(G)},
                           {"ENV": {"LM_EXEC_ENV": "from-the-graft-entry"}},
                           {"EXEC": [{"LABEL": "lm_graft_entry", "HOST": "linux64", "EXE": G + "/bin/probe.sh",
                                      "ARGS": ["--via-graft-entry"], "WORKDIR": G}]}],
        RECOMMENDED=[UID])
    return N

def main():
    if len(sys.argv) < 2:
        print("usage: make_fixture.py <data-dir>", file=sys.stderr); return 2
    Data = os.path.abspath(sys.argv[1])
    # Deliberately NOT under <data>/LIBRARY: that tree belongs to managed (CID) sources, and a bundle sitting
    # inside it is treated as one of their packages and indexed through the source rather than as a local path.
    Bundle = f"{Data}/fixture/[90000000000001][v1.0] Launch Matrix"
    if os.path.isdir(f"{Data}/fixture"): shutil.rmtree(f"{Data}/fixture")
    os.makedirs(Bundle, exist_ok=True)
    make_content(Bundle)
    tool = os.environ.get("VG_MAKE_DELTA") or os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
        "build", "vg_make_delta")
    if not make_deltas(Bundle, tool):
        print(f"make_fixture: {tool} missing — build vg_make_delta; the fixture needs REAL deltas",
              file=sys.stderr)
        return 3
    for n in nodes():                              # one node per file: the file IS the node's block
        with open(f"{Bundle}/{n['LABEL']}.json", "w") as Out:
            json.dump(n, Out, indent=2)
            Out.write("\n")

    # A config with exactly ONE package source: the fixture. The default sources are CIDs, and leaving them in
    # would make the harness depend on the network and on whatever the library happens to contain today — the
    # opposite of a fixture. A source with no CID is a plain local folder under <data>/LIBRARY/<NAME>.
    # The bundle is registered as a LOCAL package (GlobalConfig "LIBRARY"), not as a package SOURCE: a source
    # must carry a CID and is fetched over the network, which a fixture must never depend on. A local entry is
    # just a path, indexed directly.
    Cfg = {
        "Settings": {
            "SandboxByDefault": False,
            "LanEnabled": False,
            "IPFS": {"Enabled": False},
        },
        "LIBRARY": [{"PATH": Bundle}],
    }
    with open(f"{Data}/GlobalConfig.JSON", "w") as F:
        json.dump(Cfg, F, indent=4)
        F.write("\n")
    print(f"fixture written: {Bundle}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
