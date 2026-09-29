"""UserPatch 1.5 build 6268 as data: every SetupAoC.exe installer feature becomes an option of the "UserPatch 1.5"
graft. Derived from installs of the official SetupAoC.exe (-i -b -l -f:<flags>) over our 1.0e age2_x1.exe:
  base      = every feature off (-f:000…)      → the Base zip's age2_x1.exe
  feature k = its single-feature install's bytes over the base (runs), guarded by the base bytes (EXPECT)
  exceptions: 0x293241 is a bit per sync feature (Or); 0x293744 is water animation's rate unless lower quality.
The model (model.py) reproduced the installer's executable exactly for 30 random flag sets.

Used as an edits file for tools/gen6/remint.py: NEW = the content nodes, EDITS = the graft rewrite.
  UP_WORK=<dir> python3 tools/gen6/remint.py ~/.VidyaGod/LIBRARY tools/userpatch/gen_up.py [--apply]
"""
import json, os, sys
sys.path.insert(0, os.path.expanduser("~/Code/VidyaGod/tools/gen6"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from resolve import canonical, cid_of
import model

D = os.path.expanduser("~/.VidyaGod/LIBRARY/VidyaGod/[749][v1.0] Age of Empires II")
PKG = "VidyaGod/[749][v1.0] Age of Empires II"
EXE = "FILES/%GameDir%/age2_x1/age2_x1.exe"
cids = {os.path.basename(l.split("\t")[1].strip()): l.split("\t")[0] for l in open(os.path.join(os.environ["UP_WORK"], "cids.txt"))}   # `VidyaGod --cid` of the base zip and the two DLLs
BASE_ZIP = "Age of Empires II - The Conquerors - UserPatch v1.5 Build 6268 - Base.zip"

nodes = {f[:-5]: json.load(open(os.path.join(D, f))) for f in os.listdir(D) if f.endswith(".json")}
by = lambda l: [c for c, n in nodes.items() if n["LABEL"] == l]
(UP,) = by("UserPatch 1.5")
(P10E,) = by("The Conquerors - Patch 1.0e")

# ---- options: (var, label, group, default, installer position | None, description) ------------------------------
G_UI, G_GFX, G_PLAY, G_MP, G_DBG = ("UserPatch/Interface", "UserPatch/Graphics & sound", "UserPatch/Gameplay",
                                    "UserPatch/Multiplayer & spectating", "UserPatch/Debug")
EXE_OPTS = [  # executable features: (position, var, label, group, default)
    (9,  "up_water_anim",          "Water animation",                         G_GFX,  "1"),
    (10, "up_precision_scroll",    "Precision scrolling",                     G_UI,   "1"),
    (11, "up_shift_group",         "Shift+number appends to a group",         G_UI,   "1"),
    (12, "up_keydown_hotkeys",     "Hotkeys act on key down",                 G_UI,   "1"),
    (13, "up_new_save_names",      "Dated save file names (rec.yyyymmdd…)",   G_PLAY, "1"),
    (21, "up_touch",               "Touch screen control",                    G_UI,   "0"),
    (22, "up_store_sx_spec",       "Show Sx spectator connections",           G_MP,   "0"),
    (23, "up_custom_mouse",        "Custom normal mouse cursor",              G_UI,   "1"),
    (25, "up_delink_volume",       "Delink from system volume",               G_GFX,  "0"),
    (26, "up_alt_chat_wine",       "Alternate chat box for Wine",             G_UI,   "0"),
    (27, "up_lower_quality",       "Lower quality environment",               G_GFX,  "0"),
    (28, "up_20fps",               "Restore 20 fps for single player",        G_GFX,  "0"),
    (29, "up_no_ext_hotkeys",      "Disable F6/F7/F8 hotkeys",                G_UI,   "0"),
    (30, "up_force_new_gameplay",  "Force new gameplay features (sync)",      G_PLAY, "0"),
    (31, "up_ore_display",         "Ore resource display (sync)",             G_PLAY, "0"),
    (32, "up_no_anti_cheat",       "Disable multiplayer anti-cheat",          G_MP,   "0"),
    (33, "up_background_mode",     "Start in background mode",                G_UI,   "0"),
    (34, "up_windowed_fullscreen", "32-bit windowed fullscreen",              G_GFX,  "0"),
    (35, "up_mp_sp_speed",         "Multiplayer speed in single player (sync)", G_PLAY, "0"),
    (36, "up_rms_debug",           "RMS/SCX debug logging (sync)",            G_DBG,  "0"),
    (38, "up_background_audio",    "Keep audio playing in the background",    G_GFX,  "0"),
    (39, "up_no_civilian_attack",  "Disable civilian attack switch (sync)",   G_PLAY, "0"),
    (40, "up_small_farms",         "Handle small farm selections (sync)",     G_PLAY, "0"),
    (41, "up_rec_research",        "Show research events in rec/spec",        G_MP,   "0"),
    (42, "up_rec_market",          "Show market events in rec/spec",          G_MP,   "0"),
    (43, "up_rec_no_score",        "Disable rec/spec score stats",            G_MP,   "0"),
]
BOOL_OPTS = [  # registry / file features: (var, label, group, default)
    ("up_windowed",          "Windowed mode support (wndmode.dll)",     G_GFX,  "0"),
    ("up_port_forwarding",   "UPnP port forwarding (miniupnpc.dll)",    G_MP,   "1"),
    ("up_dark_red",          "Darken mini-map red",                     G_GFX,  "0"),
    ("up_dark_purple",       "Darken mini-map purple",                  G_GFX,  "0"),
    ("up_dark_grey",         "Darken mini-map grey",                    G_GFX,  "1"),
    ("up_extend_population", "Population caps to 1000",                 G_PLAY, "1"),
    ("up_snow_removal",      "Snow/ice terrain removal",                G_GFX,  "0"),
    ("up_multiple_queue",    "Multiple building queue",                 G_PLAY, "1"),
    ("up_patrol_delay",      "Original 10-second patrol default",       G_PLAY, "0"),
    ("up_no_water_movement", "Disable water movement",                  G_GFX,  "0"),
    ("up_no_weather",        "Disable weather system",                  G_GFX,  "0"),
    ("up_no_custom_terrains", "Disable custom terrains",                G_GFX,  "0"),
    ("up_no_underwater",     "Disable terrain underwater",              G_GFX,  "0"),
    ("up_numeric_age",       "Numeric age display",                     G_UI,   "0"),
    ("up_stats_font",        "Change statistics font style",            G_UI,   "0"),
    ("up_hidden_civs",       "Hidden civilization selection",           G_MP,   "0"),
    ("up_no_help_text",      "Disable event help text",                 G_UI,   "0"),
]

def bool_var(label, group, default):
    return {"DEFAULT": default, "UI": {"CONTROL": "bool", "SECTION": group, "LABEL": label}}

VARS = {
    "up_style": {"DEFAULT": "centered", "UI": {"CONTROL": "enum", "SECTION": G_UI, "LABEL": "Command bar style",
                 "CHOICES": [{"LABEL": "Widescreen", "VALUE": "widescreen"}, {"LABEL": "Classic centered", "VALUE": "centered"},
                             {"LABEL": "Classic left-aligned", "VALUE": "left"}]}},
}
for pos, var, label, group, default in EXE_OPTS:
    VARS[var] = bool_var(label, group, default)
for var, label, group, default in BOOL_OPTS:
    VARS[var] = bool_var(label, group, default)
VARS.update({
    "up_spec_default":    {"DEFAULT": "0", "UI": {"CONTROL": "int", "SECTION": G_MP, "LABEL": "Default spectator slots", "MIN": 0, "MAX": 5}},
    "up_spec_late_join":  {"DEFAULT": "19200", "UI": {"CONTROL": "int", "SECTION": G_MP, "LABEL": "Spectator late join (game turns)", "MIN": 0, "MAX": 1000000}},
    "up_spec_join_delay": {"DEFAULT": "5000", "UI": {"CONTROL": "int", "SECTION": G_MP, "LABEL": "Spectator join delay (ms)", "MIN": 0, "MAX": 600000}},
    "up_friend_foe_ids":  {"DEFAULT": "0x01000603", "UI": {"CONTROL": "text", "SECTION": G_MP, "LABEL": "Friend/foe colours (0xEESSNNAA)",
                           "PATTERN": "0[xX][0-9a-fA-F]{8}"}},
    # registry bitfields composed from independent options
    "up_minimap_colors":  {"DEFAULT": "%up_dark_red%*2 | %up_dark_purple%*32 | (1-%up_dark_grey%)*64", "EVAL": True},
    "up_setup_terrain":   {"DEFAULT": "%up_no_custom_terrains%*1 | %up_no_water_movement%*2 | %up_no_weather%*4 | %up_no_underwater%*8", "EVAL": True},
    "up_numeric_age_flags": {"DEFAULT": "%up_numeric_age%*1 | %up_stats_font%*2", "EVAL": True},
    "up_friend_foe_dword": {"DEFAULT": "%up_friend_foe_ids%", "EVAL": True},
})
REGVALS = {  # value name -> var (rendered as a dword)
    "Adjust Terrains": "up_snow_removal", "Extend Population": "up_extend_population", "Hidden Civs": "up_hidden_civs",
    "Mini-map Colors": "up_minimap_colors", "Multiple Queue": "up_multiple_queue", "Numeric Age": "up_numeric_age_flags",
    "Patrol Delay": "up_patrol_delay", "Setup Terrain": "up_setup_terrain", "Spec Default": "up_spec_default",
    "Spec Late Join": "up_spec_late_join", "Spec Join Delay": "up_spec_join_delay", "Friend Foe Ids": "up_friend_foe_dword",
    "Disable Help Text": "up_no_help_text",
}
TCKEY = ["Software", "Microsoft", "Microsoft Games", "Age of Empires II: The Conquerors Expansion", "1.0"]

# ---- byte patches -------------------------------------------------------------------------------------------------
Z = model.Z
def ops_for(diff, gap=8):
    """Replace ops over the base: runs of changed bytes, merged across small unchanged gaps."""
    offs = sorted(diff)
    ops, i = [], 0
    while i < len(offs):
        a = b = offs[i]
        while i + 1 < len(offs) and offs[i + 1] - b <= gap:
            i += 1; b = offs[i]
        new = bytes(diff.get(j, Z[j]) for j in range(a, b + 1))
        ops.append({"MODE": "Replace", "OFFSET": hex(a), "EXPECT": Z[a:b + 1].hex(), "REPLACE": new.hex()})
        i += 1
    return ops

LABEL_OF = {pos: label for pos, var, label, group, default in EXE_OPTS}
edit_layers = []
edit_layers.append({"EDIT": [dict(o, COMMENT="Widescreen command bar (installer: Command bar style = widescreen)") if n == 0 else o
                             for n, o in enumerate(ops_for(model.STYLE["widescreen"]))],
                    "TARGET": EXE, "WHEN": "%up_style%==widescreen"})
edit_layers.append({"EDIT": [dict(o, COMMENT="Classic left-aligned command bar") if n == 0 else o
                             for n, o in enumerate(ops_for(model.STYLE["left"]))],
                    "TARGET": EXE, "WHEN": "%up_style%==left"})
for pos, var, label, group, default in EXE_OPTS:
    ops = ops_for(model.F[pos])
    if pos in model.BITS:
        ops.append({"MODE": "Or", "OFFSET": hex(model.FLAGS), "VALUE": "%02x" % model.BITS[pos],
                    "COMMENT": "sync feature flags (one bit per sync feature)"})
    if pos == 9:
        ops.append({"MODE": "Replace", "OFFSET": hex(model.WATER), "EXPECT": "%02x" % Z[model.WATER], "REPLACE": "%02x" % model.WATER_ON,
                    "WHEN": "%up_lower_quality%!=1", "COMMENT": "water animation rate (lower quality environment keeps the base rate)"})
    ops[0] = dict(ops[0], COMMENT=f"{label} (installer feature {pos})" + (" — " + ops[0]["COMMENT"] if "COMMENT" in ops[0] else ""))
    edit_layers.append({"EDIT": ops, "TARGET": EXE, "WHEN": f"%{var}%==1"})

# split the patch layers into content nodes under the block limit
NEW, patch_nodes, cur = [], [], []
def flush():
    if cur:
        n = {"LABEL": f"UserPatch 1.5 - Features {len(patch_nodes) + 1}", "LAYERS": list(cur)}
        NEW.append((PKG, n)); patch_nodes.append(cid_of(canonical(n))); cur.clear()
for L in edit_layers:
    if cur and len(canonical({"LABEL": "x", "LAYERS": cur + [L]})) > 200 * 1024:
        flush()
    cur.append(L)
flush()

base = {"LABEL": "UserPatch 1.5 - Base",
        "LAYERS": [{"SOURCE": cids[BASE_ZIP], "TARGET": "FILES/%GameDir%/age2_x1", "ZIP": BASE_ZIP},
                   {"FILE": "wndmode.dll", "SOURCE": cids["wndmode.dll"], "TARGET": "FILES/%GameDir%/age2_x1", "WHEN": "%up_windowed%==1"},
                   {"FILE": "miniupnpc.dll", "SOURCE": cids["miniupnpc.dll"], "TARGET": "FILES/%GameDir%/age2_x1", "WHEN": "%up_port_forwarding%==1"}]}
NEW.append((PKG, base))
BASE = cid_of(canonical(base))

def graft(n):
    reg = {"HKCU": {}}
    k = reg["HKCU"]
    for p in TCKEY: k = k.setdefault(p, {})
    for name, var in REGVALS.items(): k[name] = f"%{var}:dword%"
    keep = {"REG/HKCU/" + "/".join(TCKEY) + "/" + name: False for name in REGVALS}   # the options', never the user's store
    n["LAYERS"] = [{"ANY": [P10E]}, {"VARS": VARS}, {"NODE": BASE}] + [{"NODE": c} for c in patch_nodes] + \
                  [{"REG": reg, "ARCH": ["32"]}, {"KEEP": keep}]
EDITS = {UP: graft}

if __name__ == "__main__":
    for p, n in NEW: print(len(canonical(n)), n["LABEL"])
    print(len(edit_layers), "patch layers,", sum(len(L["EDIT"]) for L in edit_layers), "ops,", len(VARS), "vars")
