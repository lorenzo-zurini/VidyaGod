#!/usr/bin/env bash
# e2e_regression.sh — the POST-BUILD 2-machine regression battery. Every flow a user actually performs is
# exercised END TO END between this machine (PC) and the laptop, over the OPEN INTERNET (VG_BENCH_NO_TUNNEL
# gates the WireGuard subnet out of libp2p, so WG stays only the ssh control channel):
#
#   phase 1  IDENTITY      fresh nodes + peer IDs on both machines (every run starts from zero)
#   phase 2  FRIENDING     DHT discovery → friend request → auto-accept → accepted BOTH sides,
#                          nickname propagation, presence online (retries absorb DHT cold-start)
#   phase 3  VIRTUAL LAN   the game-traffic battery: UDP broadcast discovery (server-browser moment),
#                          TCP lobby echo, 5MB bulk throughput, bidirectional pings
#   phase 4  RECOVERY      kill the laptop node mid-traffic, restart it, assert the link maintainer
#                          reconnects and traffic resumes
#   phase 5  IPFS          catalog fetch (games meta-CID, ~2000 JSONs), a CONTENT layer fetch chosen from
#                          the catalog, byte-verify by re-hash (--cid), pin present after fetch (--pin-ls)
#   phase 6  HEALTH        the VgHealth service report on BOTH machines (tools/health_probe.py drives
#                          libvgipfs.so directly): a live node must report ZERO "down" services
#
# ── STANDING RULE (user, 2026-09-04): EVERY change tests EVERY flow it adds — not just user flows — in the
#    same change that ships it. Cross-machine/user behavior lands HERE as a phase; internal mechanisms get Go
#    unit/two-host tests (mutation-verified); C++ wiring gets Qt Test; GUI flows get AT-SPI (tools/a11ydrive.py)
#    + shot.sh; game behavior gets the gamedrive/realinput harnesses. NOTHING is exempt: if no harness can
#    observe a behavior, BUILDING one is part of the feature's cost (precedents: tools/health_probe.py drives
#    libvgipfs.so via ctypes; /proc/mem runtime differentials; uinput synthetic input). Run at least the new
#    checks standalone as acceptance before pushing. A flow that isn't gated somewhere can silently break later
#    — which is exactly what this file exists to prevent.
#
# Harness traps (each bit us once — do not reintroduce):
#   * overlay routes are derived from ACCEPTED friends at TUN-attach time → befriend FIRST, then start --lan nodes
#   * pgrep/pkill self-match: use the [d] bracket idiom; never put a plain binary-name pattern in the command text
#   * --log captures fd 2 only → overlay-exec payloads need `exec 1>&2` or their output is lost
#   * catalog JSONs use nested "CID": keys; Qm CIDs may be DIRECTORY metas → pick bafkrei leaves for --fetch tests
#
# Invoked automatically at the end of tools/provision_laptop.sh (VG_SKIP_E2E=1 to skip). Runs ~11-14 min.
# THIS CODEBASE'S FAILURE MODE IS SILENCE: every phase prints a loud PASS/FAIL and the script exits non-zero
# if anything failed. Full per-node logs (--log, both machines) are archived under the run dir it prints.
set -u

HOST="${VG_LAPTOP_HOST:-lorenzo-zurini@10.10.0.5}"
REPO_PC="${VG_PC_REPO:-$HOME/Code/VidyaGod}"
REPO_LAP="${VG_LAPTOP_REPO:-~/Code/VidyaGod}"
GAMES_CID="${VG_GAMES_CID:-QmXaGMNvBrfp6Y4opTRzQ5uPwGSnRg65t5v8ALej4qGPxs}"
NOTUN="${VG_E2E_NO_TUNNEL:-1}"      # 1 = force open-internet (recommended); 0 = allow any path
TS=$(date +%Y%m%d-%H%M%S)
RUN="$HOME/.vidyagod-e2e/$TS"
LAPRUN="/tmp/vg-e2e-$TS"
BIN="$REPO_PC/build/VidyaGod"
mkdir -p "$RUN"

FAILS=0; RESULTS=()
say()  { printf '\n\033[1m===== %s =====\033[0m\n' "$*"; }
pass() { RESULTS+=("PASS  $*"); printf '\033[32mPASS\033[0m  %s\n' "$*"; }
fail() { RESULTS+=("FAIL  $*"); printf '\033[31mFAIL\033[0m  %s\n' "$*"; FAILS=$((FAILS+1)); }
SSH()  { timeout "${2:-25}" ssh -o ConnectTimeout=8 -o BatchMode=yes "$HOST" "$1"; }
# Kill only OUR test nodes: the [d] bracket defeats pgrep -f self-matching, the run-dir scopes it.
killpc()  { for P in $(pgrep -f "VidyaGo[d] .*vidyagod-e2e/$TS"); do kill "$P" 2>/dev/null; done; }
killlap() { SSH "pkill -f 'VidyaGo[d] .*vg-e2e-$TS' 2>/dev/null; true" 15; }
ENVPC=(); ENVLAP=""
if [ "$NOTUN" = "1" ]; then ENVPC=(env VG_BENCH_NO_TUNNEL=1); ENVLAP="VG_BENCH_NO_TUNNEL=1"; fi

say "e2e regression $TS  laptop=$HOST  no-tunnel=$NOTUN  logs=$RUN"

# ---------- phase 0: prerequisites ----------
say "phase 0: prerequisites"
if ! SSH "echo ok" 10 >/dev/null 2>&1; then fail "laptop unreachable over ssh — cannot run the battery"; else pass "laptop reachable"; fi
[ -x "$BIN" ] || { fail "PC binary missing: $BIN"; }
SSH "test -x $REPO_LAP/build/VidyaGod" 10 && pass "laptop binary present" || fail "laptop binary missing"
if [ "$FAILS" -gt 0 ]; then say "prerequisites failed — aborting"; exit 1; fi

# ---------- phase 1: fresh identities ----------
say "phase 1: fresh identities"
timeout 45 "${ENVPC[@]}" "$BIN" --peer-id --data-dir "$RUN/pc-data" --log "$RUN/pc-id.log" >/dev/null 2>&1
PCID=$(grep -aoE 'peerID=12D3[A-Za-z0-9]+' "$RUN/pc-id.log" | head -1 | cut -d= -f2)
SSH "cd $REPO_LAP && $ENVLAP timeout 40 ./build/VidyaGod --peer-id --data-dir $LAPRUN/data >$LAPRUN.id.log 2>&1; grep -aoE 'peerID=12D3[A-Za-z0-9]+' $LAPRUN.id.log | head -1" 60 > "$RUN/lap-id.txt"
LAPID=$(cut -d= -f2 < "$RUN/lap-id.txt")
if [ -n "$PCID" ] && [ -n "$LAPID" ] && [ "$PCID" != "$LAPID" ]; then pass "fresh peer ids (pc=${PCID:0:16}… lap=${LAPID:0:16}…)"; else fail "identity generation (pc='$PCID' lap='$LAPID')"; exit 1; fi
PCVIP=$(python3 -c "import hashlib,sys;h=hashlib.sha256(sys.argv[1].encode()).digest();hi,lo=h[0],h[1];lo=1 if (hi==0 and lo==0) else (254 if (hi==255 and lo==255) else lo);print(f'10.66.{hi}.{lo}')" "$PCID")
LAPVIP=$(python3 -c "import hashlib,sys;h=hashlib.sha256(sys.argv[1].encode()).digest();hi,lo=h[0],h[1];lo=1 if (hi==0 and lo==0) else (254 if (hi==255 and lo==255) else lo);print(f'10.66.{hi}.{lo}')" "$LAPID")
echo "vIPs: pc=$PCVIP lap=$LAPVIP"

# ---------- phase 2: friending flow ----------
say "phase 2: friending (DHT discovery → request → accept → nick + presence)"
# Laptop: long-lived responder — serves friend requests AND (later) the LAN battery. It warms into the DHT.
SSH "cd $REPO_LAP && cat > $LAPRUN-host.py" 15 <<'LAPPY'
import socket, sys, threading, time
def log(*a): print("[LAPHOST]", *a, file=sys.stderr, flush=True)
def tcp_srv():
    s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("0.0.0.0",47778)); s.listen(4)
    log("TCP host on 47778")
    while True:
        c,_=s.accept()
        def h(c):
            try:
                c.settimeout(30); first=c.recv(65536)
                if first.startswith(b"ECHO:"): c.sendall(first); c.close(); return
                tot=len(first); t0=time.time()
                while True:
                    d=c.recv(65536)
                    if not d: break
                    tot+=len(d)
                log(f"TCP sink {tot}B in {time.time()-t0:.2f}s")
            except Exception as e: log("tcp err",e)
            finally: c.close()
        threading.Thread(target=h,args=(c,),daemon=True).start()
def udp_srv():
    u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); u.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); u.bind(("0.0.0.0",47777))
    log("UDP host on 47777")
    while True:
        d,a=u.recvfrom(2048); log(f"UDP-RX {a} {d[:40]!r}"); u.sendto(b"GAME-HOST-REPLY "+d[:32],a)
threading.Thread(target=tcp_srv,daemon=True).start(); threading.Thread(target=udp_srv,daemon=True).start()
time.sleep(900)
LAPPY
# Friend-serve WITHOUT the overlay: routes are derived from ACCEPTED friends at overlay-attach time, so the
# LAN responder must start AFTER the friendship exists (phase 3) — starting it here would bake in empty routes.
SSH "cd $REPO_LAP && $ENVLAP nohup ./build/VidyaGod --friend-add $PCID --friend-nick Laptop --friend-serve --friend-secs 400 --data-dir $LAPRUN/data --log $LAPRUN-node.log >/dev/null 2>&1 & echo started" 20
echo "warming laptop into the DHT (90s)…"
sleep 90
# PC: friend-add with up to 3 attempts (cold-start FindPeer may need the announce to settle)
ACCEPTED=0; T0=$(date +%s)
for ATT in 1 2 3; do
  timeout 75 "${ENVPC[@]}" "$BIN" --friend-add "$LAPID" --friend-nick PC --friend-serve --friend-secs 45 \
      --data-dir "$RUN/pc-data" --log "$RUN/pc-friend-$ATT.log" >/dev/null 2>&1
  if grep -qaE 'accepted' "$RUN/pc-friend-$ATT.log"; then ACCEPTED=1; break; fi
  echo "attempt $ATT: not yet accepted — retrying"
done
if [ "$ACCEPTED" = "1" ]; then pass "friend handshake accepted (attempt $ATT, $(( $(date +%s) - T0 ))s incl. windows)"; else fail "friend handshake never reached accepted in 3 attempts"; fi
# nickname + presence propagation (the address book the user sees)
timeout 60 "${ENVPC[@]}" "$BIN" --friend-ls --data-dir "$RUN/pc-data" --log "$RUN/pc-ls.log" >/dev/null 2>&1
grep -qaE "accepted.*$LAPID.*nick='Laptop'" "$RUN/pc-ls.log" && pass "nickname propagated (sees 'Laptop')" || fail "nickname not propagated (friend-ls)"
SSH "grep -qaE \"(auto-accepted.*$PCID|$PCID -> accepted)\" $LAPRUN-node.log" 15 && pass "laptop side accepted too" || fail "laptop never recorded the friendship"

# ---------- phase 3: virtual LAN game battery ----------
say "phase 3: virtual LAN (broadcast discovery, lobby TCP, throughput, bidirectional pings)"
# NOW start the laptop's LAN game-host responder — the friendship exists, so its overlay routes include the PC.
killlap; sleep 2
SSH "cd $REPO_LAP && $ENVLAP nohup ./build/VidyaGod --lan --overlay-exec 'python3 $LAPRUN-host.py' --friend-nick Laptop --friend-secs 600 --data-dir $LAPRUN/data --log $LAPRUN-lan.log >/dev/null 2>&1 & echo lan-host up" 20
sleep 20   # node up + TUN attached (peers already known — no full DHT re-warm needed)
cat > "$RUN/pc-battery.py" <<PCPY
import socket, subprocess, sys, time
LAPVIP="$LAPVIP"
def log(*a): print("[PCBAT]", *a, file=sys.stderr, flush=True)
time.sleep(30)
log("PHASE-PING")
subprocess.run(["ping","-c","15","-W","2",LAPVIP],stdout=sys.stderr,stderr=sys.stderr)
log("PHASE-BCAST")
u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); u.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1)
u.bind(("0.0.0.0",0)); u.settimeout(3); got=0
for i in range(5):
    u.sendto(f"DISCOVER-{i}".encode(),("10.66.255.255",47777))
    try:
        d,a=u.recvfrom(2048); got+=1; log(f"BCAST reply {i} from {a}")
    except socket.timeout: log(f"BCAST {i}: timeout")
    time.sleep(1)
log(f"BCAST-RESULT {got}/5")
log("PHASE-TCP-ECHO")
try:
    c=socket.create_connection((LAPVIP,47778),timeout=8); t0=time.time()
    c.sendall(b"ECHO:x"); c.recv(65536); log(f"TCP-ECHO-OK rtt={1000*(time.time()-t0):.1f}ms"); c.close()
except Exception as e: log("TCP-ECHO-FAIL",e)
log("PHASE-THROUGHPUT")
try:
    c=socket.create_connection((LAPVIP,47778),timeout=8); c.settimeout(60); t0=time.time()
    b=b"x"*65536
    for _ in range(80): c.sendall(b)
    c.shutdown(socket.SHUT_WR)
    try: c.recv(1)
    except Exception: pass
    log(f"THROUGHPUT-OK {5.24/max(time.time()-t0,0.001):.2f} MB/s")
except Exception as e: log("THROUGHPUT-FAIL",e)
log("PC-BATTERY-DONE"); time.sleep(30)
PCPY
"${ENVPC[@]}" nohup "$BIN" --lan --overlay-exec "python3 $RUN/pc-battery.py" --friend-nick PC --friend-secs 240 \
    --data-dir "$RUN/pc-data" --log "$RUN/pc-lan.log" >/dev/null 2>&1 &
for i in $(seq 1 30); do sleep 6; grep -qa 'PC-BATTERY-DONE' "$RUN/pc-lan.log" 2>/dev/null && break; done
PINGRX=$(grep -aoE '[0-9]+ received' "$RUN/pc-lan.log" | head -1 | cut -d' ' -f1); PINGRX=${PINGRX:-0}
[ "$PINGRX" -ge 13 ] && pass "LAN ping pc→lap ($PINGRX/15)" || fail "LAN ping pc→lap ($PINGRX/15 received)"
BC=$(grep -aoE 'BCAST-RESULT [0-9]+' "$RUN/pc-lan.log" | grep -oE '[0-9]+$'); BC=${BC:-0}
[ "$BC" -ge 3 ] && pass "UDP broadcast discovery ($BC/5 host replies)" || fail "UDP broadcast discovery ($BC/5)"
grep -qa 'TCP-ECHO-OK' "$RUN/pc-lan.log" && pass "TCP lobby echo" || fail "TCP lobby echo"
grep -qa 'THROUGHPUT-OK' "$RUN/pc-lan.log" && pass "bulk throughput ($(grep -aoE 'THROUGHPUT-OK [0-9.]+ MB/s' "$RUN/pc-lan.log" | head -1 | cut -d' ' -f2-))" || fail "bulk throughput"
# Reverse-direction proof: the laptop's host log counts the broadcasts it RECEIVED and replied to — its
# inbound datapath — while the echo/throughput replies prove its outbound. Both directions are covered.
UDPRX=$(SSH "grep -acE 'UDP-RX' $LAPRUN-lan.log || true" 15)
[ "${UDPRX:-0}" -ge 3 ] && pass "laptop-inbound datapath (received UDP-RX×$UDPRX + replied)" || fail "laptop-inbound datapath (UDP-RX=${UDPRX:-0})"

# ---------- phase 4: link recovery ----------
say "phase 4: link recovery (kill laptop node mid-hold, restart, must reconnect)"
killlap; sleep 2
SSH "cd $REPO_LAP && $ENVLAP nohup ./build/VidyaGod --lan --overlay-exec 'sleep 300' --friend-nick Laptop --friend-secs 360 --data-dir $LAPRUN/data --log $LAPRUN-rec.log >/dev/null 2>&1 & echo up" 20
killpc; sleep 2
"${ENVPC[@]}" nohup "$BIN" --lan --overlay-exec "sh -c 'exec 1>&2; sleep 15; ping -i 2 -c 90 -W 1 $LAPVIP; echo REC-END'" \
    --friend-nick PC --friend-secs 260 --data-dir "$RUN/pc-data" --log "$RUN/pc-rec.log" >/dev/null 2>&1 &
sleep 55                       # ping flowing
SSH "pkill -f 'VidyaGo[d] .*vg-e2e-$TS' 2>/dev/null; true" 15
sleep 25                       # outage window
SSH "cd $REPO_LAP && $ENVLAP nohup ./build/VidyaGod --lan --overlay-exec 'sleep 240' --friend-nick Laptop --friend-secs 300 --data-dir $LAPRUN/data --log $LAPRUN-rec2.log >/dev/null 2>&1 & echo restarted" 20
for i in $(seq 1 30); do sleep 6; grep -qa 'REC-END' "$RUN/pc-rec.log" 2>/dev/null && break; done
grep -qaE '\[lan\] Laptop: .*→ down' "$RUN/pc-rec.log" && pass "outage detected (link → down)" || fail "outage never detected"
grep -qaE 'dial Laptop ok' "$RUN/pc-rec.log" && pass "reconnect dial succeeded" || fail "no successful reconnect dial"
RECRX=$(grep -aoE '[0-9]+ received' "$RUN/pc-rec.log" | tail -1 | cut -d' ' -f1); RECRX=${RECRX:-0}
[ "$RECRX" -ge 60 ] && pass "traffic resumed after restart ($RECRX/90 replies incl. outage)" || fail "traffic did not resume ($RECRX/90)"

# ---------- phase 5: IPFS flows ----------
say "phase 5: IPFS (catalog fetch → content fetch → byte verify → pin present)"
SSH "cd $REPO_LAP && rm -rf $LAPRUN/cat && $ENVLAP timeout 150 ./build/VidyaGod --fetch-dir $GAMES_CID $LAPRUN/cat --data-dir $LAPRUN/fdata --log $LAPRUN-fetch.log >/dev/null 2>&1; find $LAPRUN/cat -type f | wc -l" 170 > "$RUN/catcount.txt"
CATN=$(tr -dc 0-9 < "$RUN/catcount.txt"); CATN=${CATN:-0}
[ "$CATN" -ge 100 ] && pass "catalog meta-CID fetched ($CATN files)" || fail "catalog fetch ($CATN files)"
# pick one CONTENT CID from the catalog and fetch + re-hash it (the download-a-game flow, byte-verified)
CCID=$(SSH "grep -rhoE '\"CID\": *\"bafkrei[a-z2-7]+\"' '$LAPRUN/cat' 2>/dev/null | grep -oE 'bafkrei[a-z2-7]+' | head -1" 20)
if [ -z "$CCID" ]; then fail "no content CID found in catalog"; else
  echo "content CID: $CCID"
  SSH "cd $REPO_LAP && $ENVLAP timeout 240 ./build/VidyaGod --fetch $CCID $LAPRUN/content.bin --data-dir $LAPRUN/fdata --log $LAPRUN-cfetch.log >/dev/null 2>&1; ls -la $LAPRUN/content.bin 2>/dev/null | awk '{print \$5}'" 260 > "$RUN/csize.txt"
  CSZ=$(tr -dc 0-9 < "$RUN/csize.txt"); CSZ=${CSZ:-0}
  [ "$CSZ" -gt 0 ] && pass "content layer fetched ($CSZ bytes)" || fail "content fetch produced nothing"
  RECID=$(SSH "cd $REPO_LAP && timeout 60 ./build/VidyaGod --cid $LAPRUN/content.bin 2>/dev/null | grep -aoE '[A-Za-z0-9]{40,}' | tail -1" 70)
  [ "$RECID" = "$CCID" ] && pass "fetched bytes re-hash to the same CID (integrity)" || fail "re-hash mismatch (got '$RECID')"
  SSH "cd $REPO_LAP && timeout 60 ./build/VidyaGod --pin-ls --data-dir $LAPRUN/fdata 2>&1 | grep -qa '$CCID'" 70 && pass "fetched content is pinned (will re-seed)" || fail "fetched content not pinned"
fi

# ---------- phase 6: service health (both machines) ----------
say "phase 6: service health — a live node must report ZERO down services"
PROBE="$REPO_PC/tools/health_probe.py"
timeout 60 "${ENVPC[@]}" python3 "$PROBE" "$REPO_PC/build/libvgipfs.so" "$RUN/pc-health-repo" 15 > "$RUN/pc-health.txt" 2>&1
PCROWS=$(grep -cE '^(ok|warn|down|off) ' "$RUN/pc-health.txt" || true)
PCDOWN=$(grep -cE '^down ' "$RUN/pc-health.txt" || true)
if [ "${PCROWS:-0}" -ge 10 ] && [ "${PCDOWN:-0}" -eq 0 ]; then
  pass "PC service health ($PCROWS services, 0 down)"
else
  fail "PC service health (rows=$PCROWS down=$PCDOWN — see pc-health.txt)"
  grep -E '^down ' "$RUN/pc-health.txt" | head -5
fi
SSH "cd $REPO_LAP && $ENVLAP timeout 55 python3 tools/health_probe.py build/libvgipfs.so $LAPRUN/health-repo 15 2>&1" 70 > "$RUN/lap-health.txt"
LROWS=$(grep -cE '^(ok|warn|down|off) ' "$RUN/lap-health.txt" || true)
LDOWN=$(grep -cE '^down ' "$RUN/lap-health.txt" || true)
if [ "${LROWS:-0}" -ge 10 ] && [ "${LDOWN:-0}" -eq 0 ]; then
  pass "laptop service health ($LROWS services, 0 down)"
else
  fail "laptop service health (rows=$LROWS down=$LDOWN — see lap-health.txt)"
  grep -E '^down ' "$RUN/lap-health.txt" | head -5
fi

# ---------- teardown + verdict ----------
say "teardown"
killpc; killlap
SSH "cd /tmp && tar cz vg-e2e-$TS*.log vg-e2e-$TS-host.py 2>/dev/null || true" 30 > "$RUN/laptop-logs.tgz" 2>/dev/null
SSH "rm -rf $LAPRUN $LAPRUN-host.py ${LAPRUN}.id.log $LAPRUN-node.log $LAPRUN-rec.log $LAPRUN-rec2.log $LAPRUN-fetch.log $LAPRUN-cfetch.log 2>/dev/null; true" 20

say "VERDICT"
for R in "${RESULTS[@]}"; do echo "  $R"; done
echo ""
if [ "$FAILS" -eq 0 ]; then
  printf '\033[32m=== E2E REGRESSION: ALL %d CHECKS PASSED ===\033[0m\n' "${#RESULTS[@]}"
else
  printf '\033[31m=== E2E REGRESSION: %d FAILURE(S) of %d checks — logs in %s ===\033[0m\n' "$FAILS" "${#RESULTS[@]}" "$RUN"
fi
exit $(( FAILS > 0 ? 1 : 0 ))
