#!/usr/bin/env bash
# migrate_instances.sh — ONE-TIME migration to centralized, per-game INSTANCES (see src/instancestore.*).
#
# For every package in GlobalConfig.JSON's LIBRARY it moves:
#   • the package's in-bundle  <pkgdir>/USERDATA/*      → <root>/USERDATA/<uid>/DefaultInstance/USERDATA/   and
#   • its GlobalConfig LIBRARY[i].USERSETTINGS blob     → <root>/USERDATA/<uid>/DefaultInstance/instance.json
# then STRIPS USERSETTINGS from that package's LIBRARY entry — but ONLY once that package's move + config write both
# succeed. instance.json is written LAST, as the commit marker, so a crash/disk-full mid-run is resumable (re-run
# moves the rest and merges). Idempotent: a package with nothing left to migrate is skipped WITHOUT stripping. Never
# touches a "Vidya Backup" tree (golden backup) — neither its files NOR its config USERSETTINGS.
#
# <root> = Settings.Paths.UserDataRoot, else <data-dir>/USERDATA (data-dir = the dir holding GlobalConfig.JSON).
# <uid> is SANITIZED (matches InstanceStore::SanitizeUid): each char not in [A-Za-z0-9._-] → '_'.
# DRY-RUN by default (prints the plan); pass --apply to execute. The config is backed up before it is rewritten.
set -uo pipefail

DATA_DIR="${VG_DATA_DIR:-$HOME/.VidyaGod}"
CONFIG="$DATA_DIR/GlobalConfig.JSON"
APPLY=0
[ "${1:-}" = "--apply" ] && APPLY=1

[ -f "$CONFIG" ] || { echo "!! no config at $CONFIG (set VG_DATA_DIR)"; exit 1; }
command -v python3 >/dev/null || { echo "!! python3 required"; exit 1; }

sanitize() { printf '%s' "$1" | sed 's#[^A-Za-z0-9._-]#_#g; s#^\.\{1,2\}$#_#'; }

ROOT="$(python3 - "$CONFIG" "$DATA_DIR" <<'PY'
import json,sys,os
cfg=json.load(open(sys.argv[1])); data=sys.argv[2]
r=(((cfg.get("Settings") or {}).get("Paths") or {}).get("UserDataRoot") or "").strip()
print(r if r else os.path.join(data,"USERDATA"))
PY
)"
echo "== data-dir: $DATA_DIR"
echo "== USERDATA root: $ROOT"
[ "$APPLY" = 1 ] && echo "== MODE: APPLY" || echo "== MODE: DRY-RUN (pass --apply to execute)"
NOW="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

mapfile -t ROWS < <(python3 - "$CONFIG" <<'PY'
import json,sys
cfg=json.load(open(sys.argv[1]))
for e in cfg.get("LIBRARY",[]):
    if not isinstance(e,dict): continue
    uid=str(e.get("PACKAGEUID","")).strip()
    if not uid: continue
    us=e.get("USERSETTINGS")
    has=1 if isinstance(us,dict) and us else 0
    print("\t".join([uid, str(e.get("PATH","")).strip(), str(has), json.dumps(us if has else {})]))
PY
)

MIGRATED=0; SKIPPED=0; FAILED=0
STRIP_LIST="$(mktemp)"; trap 'rm -f "$STRIP_LIST"' EXIT
for row in "${ROWS[@]}"; do
    IFS=$'\t' read -r PUID PKGDIR HAS US <<<"$row"
    case "$PKGDIR" in *"Vidya Backup"*) echo "  SKIP (golden backup, untouched): $PUID"; continue;; esac
    SAN="$(sanitize "$PUID")"
    INST="$ROOT/$SAN/DefaultInstance"; UD_DIR="$INST/USERDATA"; CFG_PATH="$INST/instance.json"
    SRC_UD="$PKGDIR/USERDATA"
    HAVE_UD=0
    if [ -d "$SRC_UD" ]; then
        if ! ls -A "$SRC_UD" >/dev/null 2>&1; then echo "  !! $PUID: $SRC_UD unreadable — skipping (no data touched)"; ((FAILED++)); continue; fi
        [ -n "$(ls -A "$SRC_UD" 2>/dev/null)" ] && HAVE_UD=1
    fi
    # Nothing to migrate (no settings, no in-bundle userdata) → leave it; the app makes DefaultInstance on first run.
    if [ "$HAS" = 0 ] && [ "$HAVE_UD" = 0 ]; then echo "  skip (nothing to migrate): $PUID"; ((SKIPPED++)); continue; fi
    echo "  MIGRATE $PUID → $INST   (usersettings=$HAS, in-pkg USERDATA=$HAVE_UD)"
    [ "$APPLY" = 1 ] || { ((MIGRATED++)); continue; }

    mkdir -p "$UD_DIR" || { echo "  !! mkdir failed for $PUID"; ((FAILED++)); continue; }
    # MOVE FIRST (merge/resume: leave anything already at the destination — a prior partial run).
    if [ "$HAVE_UD" = 1 ]; then
        ok=1
        ( shopt -s dotglob nullglob
          for f in "$SRC_UD"/*; do
              b="$(basename "$f")"
              if [ -e "$UD_DIR/$b" ]; then echo "    (already migrated, left in place: $b)"; else mv "$f" "$UD_DIR/" || exit 1; fi
          done ) || ok=0
        [ "$ok" = 1 ] || { echo "  !! move failed for $PUID — instance.json NOT written, USERSETTINGS NOT stripped"; ((FAILED++)); continue; }
        rmdir "$SRC_UD" 2>/dev/null || true
    fi
    # instance.json LAST = the commit marker; atomic (temp + os.replace). Any failure ⇒ do NOT strip USERSETTINGS.
    if ! python3 - "$US" "$NOW" "$CFG_PATH" <<'PY'
import json,sys,os,tempfile
us=json.loads(sys.argv[1]); us["LASTRUN"]=sys.argv[2]; dst=sys.argv[3]
d=os.path.dirname(dst); os.makedirs(d, exist_ok=True)
fd,tmp=tempfile.mkstemp(dir=d, suffix=".tmp")
with os.fdopen(fd,"w") as f: json.dump(us,f,indent=2)
os.replace(tmp,dst)
PY
    then echo "  !! config write failed for $PUID — USERSETTINGS NOT stripped"; ((FAILED++)); continue; fi
    printf '%s\n' "$PUID" >> "$STRIP_LIST"
    ((MIGRATED++))
done

echo "== packages: $MIGRATED migrated, $SKIPPED nothing-to-do, $FAILED failed"
if [ "$APPLY" = 1 ]; then
    cp -a "$CONFIG" "$CONFIG.pre-instances.$(date -u +%Y%m%d%H%M%S).bak"
    # Strip USERSETTINGS ONLY for packages that fully migrated this run (atomic rewrite).
    if ! python3 - "$CONFIG" "$STRIP_LIST" <<'PY'
import json,sys,os,tempfile
cfg_path, strip_path = sys.argv[1], sys.argv[2]
strip=set(l.strip() for l in open(strip_path) if l.strip())
cfg=json.load(open(cfg_path))
for e in cfg.get("LIBRARY",[]):
    if isinstance(e,dict) and str(e.get("PACKAGEUID","")).strip() in strip: e.pop("USERSETTINGS",None)
d=os.path.dirname(cfg_path) or "."
fd,tmp=tempfile.mkstemp(dir=d, suffix=".tmp")
with os.fdopen(fd,"w") as f: json.dump(cfg,f,indent=2)
os.replace(tmp,cfg_path)
PY
    then echo "!! config rewrite FAILED — the .bak holds your data; nothing was stripped from the live file's on-disk copy safely"; exit 1; fi
    echo "== stripped USERSETTINGS for $MIGRATED migrated package(s); config backed up to $CONFIG.pre-instances.*.bak"
    echo "== VERIFY a game launches (--node <id>) + reads its VARIABLES before deleting the .bak or any old backups."
fi
[ "$FAILED" = 0 ] || { echo "!! $FAILED package(s) FAILED — re-run to resume (safe/idempotent)"; exit 1; }
