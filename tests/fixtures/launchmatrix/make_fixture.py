#!/usr/bin/env python3
"""Builds the launch-matrix fixture: a synthetic library exercising every launch-engine feature.

The point is COVERAGE, not realism. Every node type, every mode of every type, and the combinations that have
actually broken before (a delta over several bases, a WHEN that reads inert and fires anyway, a runner chain
two hops long) exist here so that touching the launch engine has something to fail against.

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
echo "  combined/from_b.txt: $(cat combined/combined/from_b.txt 2>/dev/null || echo MISSING)"
echo "  deltatarget/masked.txt (opaque delta must hide it): $(cat deltatarget/masked.txt 2>/dev/null || echo MASKED)"
echo "=== submount"
echo "  unpacked/relocated.txt: $(cat unpacked/relocated.txt 2>/dev/null || echo MISSING)"
echo "  carrier/nested/ignored.txt (must be absent): $(cat carrier/nested/ignored.txt 2>/dev/null || echo ABSENT)"
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
    # A zip whose interesting content is NESTED, mounted through SUBMOUNTS.
    store_zip(f"{B}/inner.zip", {"payload/inner.txt": "inner\n"})
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
    store_zip(f"{B}/combined_target.zip", {
        "combined/from_a.txt": "a\n",
        "combined/from_b.txt": "b\n",
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
    # over_base: base = the zip directly below at the delta's own target (base.zip).
    subprocess.run([tool, f"{B}/over_base.vgdelta", f"{B}/delta_target.zip", f"{B}/base.zip"], check=True)
    # over_concat: base = the CONCATENATION of the two declared BASE_TARGETS, in order — the reconstructed
    # view at the chain target (delta_target.zip) followed by the zip at the inner target.
    subprocess.run([tool, f"{B}/over_concat.vgdelta", f"{B}/combined_target.zip",
                    f"{B}/delta_target.zip", f"{B}/inner.zip"], check=True)
    return True

# ---------------------------------------------------------------------------------------------------------
# the node matrix
# ---------------------------------------------------------------------------------------------------------

def nodes():
    N = []
    def add(**kw):
        # Model C: a node's wiring handle is its stored "CID". For this fixture we use the readable LABEL string as the
        # handle (unique here) so PARENTS — which reference the returned handle — stay legible. GatherWorkingTree keys
        # on "CID"; FreezeToIndex then derives the real CID and indexes by it. LABEL rides along as the cosmetic name.
        kw.setdefault("CID", kw["LABEL"])
        # ONE-EDGE schema: no TYPE. Each add() declares ONE item of one kind in the old flat vocabulary (kept here
        # because it reads well); collapse it into the node's SECTION — LAYERS/VARS/PERSISTS/REGEDITS/FILEEDITS/
        # PATCHES/DLLOVERRIDES/ENTRYPOINTS/TILE — and PARENTS into OVER. NodeLower expands each section entry into the
        # same flat layer the resolvers consume, so the resolved plan (and thus the golden) is unchanged by the shape.
        SECTION = {"Content":        ("LAYERS",   ("FORM", "PATH", "TARGET", "SOURCE", "WHEN", "SUBMOUNTS", "BASE_TARGETS", "COMMENT")),
                   "CustomVar":      ("VARS",     ("KEY", "DEFAULT", "COMMENT", "UI", "WHEN")),
                   "DeclarePersist": ("PERSISTS", ("SCOPE", "PATH", "TARGET", "CLOUD", "WHEN")),
                   "DeclareExec":    ("ENTRYPOINTS", ("HOST", "GUEST", "PATH", "ARGS", "ENV", "ENV_REMOVE", "WORKDIR",
                                                      "RUNNER", "CONTENT_ROOT", "PREFIX_GENERATE", "UNIFIED_RUNTIME"))}
        t = kw.pop("TYPE", "Group")
        if t in SECTION:
            key, fields = SECTION[t]
            item = {f: kw.pop(f) for f in fields if f in kw}
            if t == "DeclareExec":
                item["LABEL"] = kw["LABEL"]                        # the entry's label = the node's name
                if "GUEST" not in item: kw["VARIANT"] = kw["LABEL"]   # a game entry ⇒ on the shelf; RECOMMENDED stays a NODE facet
            kw[key] = [item]
        elif t == "RegEdit":      kw["REGEDITS"] = kw.pop("EDITS")
        elif t == "FileEdit":     kw["FILEEDITS"] = [{k: kw.pop(k) for k in ("FILE", "EDITS", "OVERRIDE") if k in kw}]
        elif t == "BinaryPatch":  kw["PATCHES"] = [{k: kw.pop(k) for k in ("FILE", "EDITS") if k in kw}]
        elif t == "DllOverride":  kw["DLLOVERRIDES"] = kw.pop("OVERRIDES")
        elif t == "DeclareLibraryItem": kw["TILE"] = {k: kw.pop(k) for k in ("UID", "TITLE", "COVER", "META") if k in kw}
        elif t == "Group": pass
        else: raise SystemExit(f"make_fixture: unknown kind {t}")
        parents = kw.pop("PARENTS", [])
        if parents: kw["OVER"] = parents
        N.append(kw); return kw["CID"]

    # ---- identity -------------------------------------------------------------------------------------
    add(LABEL="lm_tile", TYPE="DeclareLibraryItem", PARENTS=[], UID="90000000000001",
        TITLE="Launch Matrix", META={"SERIES": "Fixtures", "YEAR": "2026"})

    # ---- runners: a native terminal, and a two-hop chain through a synthetic "prefix" runner ------------
    # Runs the content through an explicit interpreter rather than exec'ing it directly, so the fixture does
    # not depend on the FUSE mount preserving an executable bit.
    add(LABEL="lm_runner_native", TYPE="DeclareExec", PARENTS=[], HOST="linux64",
        GUEST=["linux64"], PATH="/bin/sh", ARGS=["%Content%"],
        # The runner sets both: one the launchable overrides, one it REMOVES. Removal has to beat the runner,
        # or a launchable can never get rid of something its runner insists on.
        ENV={"LM_RUNNER_ONLY": "from-runner", "LM_SHOULD_BE_GONE": "runner-set-this",
             "LM_EXEC_ENV": "runner-loses"},
        # LOAD-BEARING — do not delete as dead weight. As an OUTER link of lm_run_chained's chain this runner
        # asks for LM_GAME_KEEPS_THIS to be removed, and the game sets it, so the golden's
        # "LM_GAME_KEEPS_THIS=survived-the-outer-remove" is a real assertion that an outer wrapper cannot
        # strip a key the GAME declared. Verified: the run logs "Chain wrap: lm_runner_native (/bin/sh)" (it is
        # wrapped precisely because its PATH is a real program rather than %Content%), and restoring the old
        # exec-time environment order makes that line vanish from the golden.
        ENV_REMOVE=["LM_GAME_KEEPS_THIS"])
    add(LABEL="lm_runner_content", TYPE="Content", PARENTS=[], FORM="file", PATH="fakerunner.sh",
        TARGET="runner/fakerunner.sh")
    # HOST linux64 / GUEST fixture32 ⇒ running fixture32 content takes two hops: this, then the native one.
    add(LABEL="lm_runner_prefix", TYPE="DeclareExec", PARENTS=["lm_runner_content"], HOST="linux64",
        GUEST=["fixture32"], PATH="%RunnerMount%/runner/fakerunner.sh", ARGS=["--run"],
        ENV={"LM_RUNNER_ENV": "set", "LM_FROM_VAR": "%lm_text%"}, ENV_REMOVE=["LM_UNWANTED"],
        CONTENT_ROOT="%PrefixRoot%/drive_c/%PackageUID%", PREFIX_GENERATE=False)

    # ---- Content: every FORM, every TARGET shape, submounts, deltas -------------------------------------
    base   = add(LABEL="lm_c_zip_base", TYPE="Content", PARENTS=[], FORM="zip", PATH="base.zip",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%", COMMENT="the base layer")
    patch  = add(LABEL="lm_c_zip_patch", TYPE="Content", PARENTS=[base], FORM="zip", PATH="patch.zip",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%")
    subm   = add(LABEL="lm_c_submount", TYPE="Content", PARENTS=[patch], FORM="zip", PATH="carrier.zip",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/carrier",
                 # SUBMOUNTS is a list of "source/path:dest/path" strings, not objects.
                 SUBMOUNTS=["nested/relocated.txt:%PrefixRoot%/drive_c/%PackageUID%/unpacked/relocated.txt"])
    dirl   = add(LABEL="lm_c_dir", TYPE="Content", PARENTS=[subm], FORM="dir", PATH="loosedir",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/loose")
    # A FORM "file" layer's TARGET is the CONTAINING DIRECTORY — the file appears as TARGET/<basename of
    # PATH>. Naming the file itself in TARGET creates a directory of that name with the file inside it.
    filel  = add(LABEL="lm_c_file", TYPE="Content", PARENTS=[dirl], FORM="file", PATH="single.txt",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/loosefile")
    # A delta reconstructs a COMPLETE archive and MASKS everything below it at its target, so each one gets a
    # target whose byte view is exactly one known zip. dbase mounts base.zip at the chain target; the delta
    # above it rebuilds delta_target.zip from those bytes.
    dbase  = add(LABEL="lm_c_delta_base", TYPE="Content", PARENTS=[filel], FORM="zip", PATH="base.zip",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/deltatarget")
    masked = add(LABEL="lm_c_masked_dir", TYPE="Content", PARENTS=[dbase], FORM="dir", PATH="maskeddir",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/deltatarget")
    d1     = add(LABEL="lm_c_delta_implicit", TYPE="Content", PARENTS=[masked], FORM="delta",
                 PATH="over_base.vgdelta", TARGET="%PrefixRoot%/drive_c/%PackageUID%/deltatarget")
    # A second byte view for the multi-base delta to concatenate with.
    innerz = add(LABEL="lm_c_inner_zip", TYPE="Content", PARENTS=[d1], FORM="zip", PATH="inner.zip",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/innerzip")
    # BASE_TARGETS order is load-bearing: it must match the order the delta was generated against.
    d2     = add(LABEL="lm_c_delta_multibase", TYPE="Content", PARENTS=[innerz], FORM="delta",
                 PATH="over_concat.vgdelta", TARGET="%PrefixRoot%/drive_c/%PackageUID%/combined",
                 BASE_TARGETS=["%PrefixRoot%/drive_c/%PackageUID%/deltatarget",
                               "%PrefixRoot%/drive_c/%PackageUID%/innerzip"])
    # The probe sits at its own sub-target so the opaque delta cannot mask it.
    probe  = add(LABEL="lm_c_probe", TYPE="Content", PARENTS=[d2], FORM="file", PATH="probe.sh",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/bin")
    # Content that is only present as a CID — un-hydrated, so the plan must report it rather than mount silence.
    # Un-hydrated content: present only as a CID, never fetched. The engine REFUSES to launch a closure with
    # missing sources, which is correct and is why this hangs off a SIDE BRANCH — only the plan-level
    # launchables pull it in. Putting it in the runnable closure would test nothing but the refusal.
    # A REAL, well-formed CIDv1 (dag-raw sha2-256) whose blocks are simply never in the local store — the accurate
    # shape of un-hydrated remote content in the gigagraph, where a SOURCE.CID is a live IPLD link the freeze must be
    # able to decode. (A bogus non-CID string used to sit here; it parsed fine when a CID was an opaque field, but the
    # content-addressed freeze now decodes every link, so it made this node — and its whole downstream — unindexable.)
    remote = add(LABEL="lm_c_remote", TYPE="Content", PARENTS=[probe], FORM="zip",
                 TARGET="%PrefixRoot%/drive_c/%PackageUID%/remote",
                 SOURCE={"PATH": "never_fetched.zip",
                         "CID": "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e"})
    add(LABEL="lm_unhydrated", TYPE="Group", PARENTS=[remote])

    # ---- CustomVar: defaults, UI kinds, cross-reference, WHEN, format spec ------------------------------
    v1 = add(LABEL="lm_v_text", TYPE="CustomVar", PARENTS=[probe], KEY="lm_text", DEFAULT="hello",
             UI={"LABEL": "Text", "CONTROL": "text", "GROUP": "Matrix"})
    v2 = add(LABEL="lm_v_enum", TYPE="CustomVar", PARENTS=[v1], KEY="lm_mode", DEFAULT="beta",
             UI={"LABEL": "Mode", "CONTROL": "enum", "GROUP": "Matrix",
                 "CHOICES": [{"LABEL": "Alpha", "VALUE": "alpha"}, {"LABEL": "Beta", "VALUE": "beta"}]})
    v3 = add(LABEL="lm_v_bool", TYPE="CustomVar", PARENTS=[v2], KEY="lm_flag", DEFAULT="1",
             UI={"LABEL": "Flag", "CONTROL": "bool", "GROUP": "Matrix"})
    # A var whose DEFAULT references another var — resolution is a fixpoint, so forward order must not matter.
    v4 = add(LABEL="lm_v_derived", TYPE="CustomVar", PARENTS=[v3], KEY="lm_derived",
             DEFAULT="%lm_text%-%lm_mode%")
    # A var that only exists when another var says so.
    v5 = add(LABEL="lm_v_conditional", TYPE="CustomVar", PARENTS=[v4], KEY="lm_conditional",
             DEFAULT="on-because-beta", WHEN="%lm_mode% == beta")
    vars_tip = v5

    # NOTE, and the reason these say drive_c/... rather than %PrefixRoot%/drive_c/...: a FileEdit/BinaryPatch
    # FILE must be RELATIVE to its pass base. "%PrefixRoot%/x" is relative only while PrefixRoot is non-empty
    # (a wine prefix: "pfx"); on a native runner PrefixRoot is "" and the same string becomes the ABSOLUTE
    # "/drive_c/x", which silently escapes the pass. Layer TARGETs are normalised and survive it; edit FILEs
    # are not. The fixture found this by running.
    # ---- FileEdit: all three modes, both passes --------------------------------------------------------
    fe1 = add(LABEL="lm_fe_config", TYPE="FileEdit", PARENTS=[vars_tip], OVERRIDE=True,
              FILE="%PrefixRoot%/drive_c/%PackageUID%/game/config.ini",
              EDITS=[{"MODE": "ConfigWrite", "KEY": "Setting=", "VALUE": "1"},
                     {"MODE": "ConfigWrite", "KEY": "FromVar=", "VALUE": "%lm_derived%"}])
    fe2 = add(LABEL="lm_fe_overwrite", TYPE="FileEdit", PARENTS=[fe1], OVERRIDE=True,
              FILE="%PrefixRoot%/drive_c/%PackageUID%/game/written.txt",
              EDITS=[{"MODE": "Overwrite", "VALUE": "overwritten by the matrix\n"}])
    fe3 = add(LABEL="lm_fe_append", TYPE="FileEdit", PARENTS=[fe2], OVERRIDE=True,
              FILE="%PrefixRoot%/drive_c/%PackageUID%/game/config.ini",
              EDITS=[{"MODE": "AppendLine", "VALUE": "; appended once", "COMMENT": "idempotent by contract"}])
    # A BASE-pass edit (OVERRIDE absent) — it runs against DEFAULTDATA, before anything is mounted.
    fe4 = add(LABEL="lm_fe_basepass", TYPE="FileEdit", PARENTS=[fe3],
              FILE="basepass.txt", EDITS=[{"MODE": "Overwrite", "VALUE": "base pass ran\n"}])
    # A conditional edit: inert unless the flag is on. The class of bug this guards is a WHEN that reads
    # inert in the file and fires at launch anyway.
    fe5 = add(LABEL="lm_fe_when", TYPE="FileEdit", PARENTS=[fe4], OVERRIDE=True, WHEN="%lm_flag% == 1",
              FILE="%PrefixRoot%/drive_c/%PackageUID%/game/conditional.txt",
              EDITS=[{"MODE": "Overwrite", "VALUE": "flag was on\n"}])

    # ---- BinaryPatch: every MODE ----------------------------------------------------------------------
    bp = add(LABEL="lm_bp", TYPE="BinaryPatch", PARENTS=[fe5],
             FILE="%PrefixRoot%/drive_c/%PackageUID%/game/patch.exe",
             # Each MODE names its bytes differently: Replace->REPLACE, Poke->VALUE, Cave->PAYLOAD.
             EDITS=[{"MODE": "Replace", "OFFSET": "0x401000", "EXPECT": "000102", "REPLACE": "aabbcc",
                     "COMMENT": "VA replace behind an EXPECT guard"},
                    {"MODE": "Poke", "OFFSET": "0x401010", "VALUE": "ff"},
                    {"MODE": "Cave", "OFFSET": "0x401020", "EXPECT": "2021222324", "PAYLOAD": "9090",
                     # EXPECT must be >= 5 bytes: the patcher replaces it with a jmp rel32 to the cave, and
                     # the displaced original runs there before jumping back.
                     "CAVE": "auto", "COMMENT": "cave into section slack the patcher picks itself"}])

    # ---- RegEdit: both views, default value, key-only, conditional entry -------------------------------
    reg = add(LABEL="lm_reg", TYPE="RegEdit", PARENTS=[bp], EDITS=[
        {"ARCHITECTURE": ["32", "64"], "COMMENT": "written into both views",
         "HKLM": {"Software": {"LaunchMatrix": {"Value": "plain", "Number": "dword:0000002a",
                                                "FromVar": "%lm_mode%"}}}},
        {"ARCHITECTURE": ["64"],
         "HKCU": {"Software": {"LaunchMatrix": {"": "this is the key's DEFAULT value",
                                                "EmptyKey": {}}}}},
        {"WHEN": "%lm_flag% == 1",
         "HKLM": {"Software": {"LaunchMatrixConditional": {"Only": "when the flag is on"}}}},
    ])

    # ---- DllOverride ----------------------------------------------------------------------------------
    dll = add(LABEL="lm_dll", TYPE="DllOverride", PARENTS=[reg],
              OVERRIDES={"ddraw": "n,b", "dinput8": "n,b", "winmm": "b,n", "broken": ""})

    # ---- DeclarePersist: one node = one persist, exercising every classification the parser produces -----
    #   file dir  → KeepDirs ;  file single-file → KeepFiles ;  registry key → KeepRegKeys (x2).
    #   TARGET names the durable subdir under the instance; CLOUD=false marks machine-specific data (shader cache).
    per_dir = add(LABEL="lm_persist_saves", TYPE="DeclarePersist", PARENTS=[dll],
                  SCOPE="file", PATH="%PrefixRoot%/drive_c/%PackageUID%/game/saves/", TARGET="Saves")
    per_file = add(LABEL="lm_persist_config", TYPE="DeclarePersist", PARENTS=[per_dir],
                   SCOPE="file", PATH="%PrefixRoot%/drive_c/%PackageUID%/game/config.ini", TARGET="Config")
    per_cache = add(LABEL="lm_persist_cache", TYPE="DeclarePersist", PARENTS=[per_file],
                    SCOPE="file", PATH="%PrefixRoot%/drive_c/%PackageUID%/game/shadercache/",
                    TARGET="ShaderCache", CLOUD=False)
    per_reg1 = add(LABEL="lm_persist_hkcu", TYPE="DeclarePersist", PARENTS=[per_cache],
                   SCOPE="registry", PATH="HKCU")
    per = add(LABEL="lm_persist_hklm", TYPE="DeclarePersist", PARENTS=[per_reg1],
              SCOPE="registry", PATH="HKLM\\Software\\LaunchMatrix")

    # ---- Group: payload-less composition --------------------------------------------------------------
    grp = add(LABEL="lm_group", TYPE="Group", PARENTS=[per])

    # ---- launchables ----------------------------------------------------------------------------------
    # (1) the whole matrix, on the native runner.
    add(LABEL="lm_all", TYPE="DeclareExec", PARENTS=[grp, "lm_unhydrated", "lm_tile"], HOST="linux64",
        PATH="%PrefixRoot%/drive_c/%PackageUID%/probe.sh", ARGS=["--matrix", "%lm_derived%"],
        RECOMMENDED=True,
        # ENV on a LAUNCHABLE reaches the process (it used to be dropped by the lowering, and had no consumer
        # either). lm_run proves the whole path at runtime; this one pins it in the plan.
        ENV={"LM_EXEC_ENV": "does-this-arrive"})
    # (2) THE RUNTIME CASE: mounts for real and runs the probe, whose stdout is its own golden. Same closure
    # as lm_all except it points at a program instead of a data file.
    add(LABEL="lm_run", TYPE="DeclareExec", PARENTS=[grp, "lm_tile"], HOST="linux64",
        PATH="%PrefixRoot%/drive_c/%PackageUID%/bin/probe.sh", ARGS=["--matrix", "%lm_derived%"],
        # WORKDIR: without it the working directory is the exe's own folder (bin/), and everything the probe
        # inspects is one level up. Setting it here exercises the field AND anchors the report.
        WORKDIR="%PrefixRoot%/drive_c/%PackageUID%",
        # A launchable's own ENV, including a %var% reference — the probe prints every LM_* it was given, so
        # this is the end-to-end proof that a game's environment reaches its process.
        ENV={"LM_EXEC_ENV": "arrived", "LM_FROM_VAR": "%lm_derived%"}, ENV_REMOVE=["LM_SHOULD_BE_GONE"])
    # (3) THE RUNTIME TWO-HOP CASE. Same runnable closure as lm_run, reached through the chain, and its ENV
    # collides with the OUTER link's on purpose: the game's value must survive, which is a property of the
    # order the environment is assembled in at exec time and is invisible in a plan.
    add(LABEL="lm_run_chained", TYPE="DeclareExec", PARENTS=[grp, "lm_tile"], HOST="fixture32",
        PATH="%PrefixRoot%/drive_c/%PackageUID%/bin/probe.sh", ARGS=["--chained"],
        WORKDIR="%PrefixRoot%/drive_c/%PackageUID%",
        ENV={"LM_EXEC_ENV": "game-beats-the-outer-link",
             #The outer link asks for this key to be REMOVED. The game sets it, so it must survive.
             "LM_GAME_KEEPS_THIS": "survived-the-outer-remove"})
    # (4) the same content routed through the two-hop chain, plus the un-hydrated branch (plan only).
    add(LABEL="lm_chained", TYPE="DeclareExec", PARENTS=[grp, "lm_unhydrated", "lm_tile"], HOST="fixture32",
        PATH="%PrefixRoot%/drive_c/%PackageUID%/probe.sh", ARGS=[],
        # The game's own ENV against a CHAIN: the outer native link sets LM_EXEC_ENV too, and the game must
        # still win. Before the ordering fix the outer wrapper was applied last and silently overrode it.
        ENV={"LM_EXEC_ENV": "game-wins-over-the-chain"})
    # (5) the minimum that can launch at all — the control case a regression shows up against first.
    # ---- grafts: selection ≠ closure ---------------------------------------------------------------------
    # A graft is a node in nobody's list that is OVER something of this title. It mounts ABOVE lm_run only when
    # SELECTED — here by the author's default (TOGGLE on). The probe's "mounted tree" is the proof: grafted.txt
    # and grafted_hd.txt (a graft on a graft, applicable through the fixpoint) and lib.txt (substance pulled in
    # beneath) appear; never.txt (no TOGGLE ⇒ not selected) and wrong.txt (OVER lm_v_text, an ANCESTOR of lm_run
    # that is never SELECTED — a sibling branch, not a mod for lm_run) do not. None of these touch lm_all /
    # lm_chained / lm_minimal / lm_run_chained: lm_run is not selected there, so nothing is applicable.
    add(LABEL="lm_graft_on", TYPE="Content", PARENTS=["lm_run"], FORM="dir", PATH="graftdir",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%", TOGGLE="on")
    add(LABEL="lm_graft_hd", TYPE="Content", PARENTS=["lm_graft_on"], FORM="dir", PATH="grafthd",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%", TOGGLE="on")
    add(LABEL="lm_graft_off", TYPE="Content", PARENTS=["lm_run"], FORM="dir", PATH="graftoff",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%")
    add(LABEL="lm_graft_wrong_branch", TYPE="Content", PARENTS=["lm_v_text"], FORM="dir", PATH="graftwrong",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%", TOGGLE="on")
    add(LABEL="lm_graft_lib", TYPE="Content", PARENTS=[], FORM="dir", PATH="graftlib",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%")
    add(LABEL="lm_graft_uses_lib", TYPE="Group", PARENTS=["lm_run", "lm_graft_lib"], TOGGLE="on")
    # ---- the final chain: facts fold, choices don't ---------------------------------------------------------
    # lm_inherits is a VARIANT over lm_run that declares no entry: it runs lm_run's entry (inherited — the
    # nearest beneath), mounts lm_run's whole closure plus its own dir, and NONE of lm_run's grafts: picking
    # lm_inherits selects exactly lm_inherits, and a graft OVER lm_run needs lm_run — a variant — selected.
    add(LABEL="lm_inherits", TYPE="Content", PARENTS=["lm_run"], FORM="dir", PATH="inheritdir",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%", VARIANT="lm_inherits")
    # lm_graft_entry is a graft that CARRIES an entry (a mod loader): ticked on lm_run it mounts above lm_run
    # like any graft, and it is also a way to RUN that mount — `--entry-node lm_graft_entry` runs its entry
    # over lm_run's closure + grafts. It is not a variant, so it is never on the shelf by itself.
    add(LABEL="lm_graft_entry", TYPE="Content", PARENTS=["lm_run"], FORM="dir", PATH="graftentry",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%", TOGGLE="on",
        ENTRYPOINTS=[{"LABEL": "lm_graft_entry", "HOST": "linux64",
                      "PATH": "%PrefixRoot%/drive_c/%PackageUID%/bin/probe.sh", "ARGS": ["--via-graft-entry"],
                      "WORKDIR": "%PrefixRoot%/drive_c/%PackageUID%", "ENV": {"LM_EXEC_ENV": "from-the-graft-entry"}}])

    add(LABEL="lm_minimal_content", TYPE="Content", PARENTS=[], FORM="zip", PATH="base.zip",
        TARGET="%PrefixRoot%/drive_c/%PackageUID%")
    add(LABEL="lm_minimal", TYPE="DeclareExec", PARENTS=["lm_minimal_content", "lm_tile"], HOST="linux64",
        PATH="%PrefixRoot%/drive_c/%PackageUID%/game/data.txt", ARGS=[])
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
    with open(f"{Bundle}/launchmatrix.json", "w") as F:
        json.dump(nodes(), F, indent=2)
        F.write("\n")

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
