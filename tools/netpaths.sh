#!/usr/bin/env bash
# netpaths.sh — the NETWORK-PATH ISOLATION MATRIX. For each way content can reach the node, EXTERNALLY block every
# other path (connection gater by IP for libp2p peers, DoH-resolver refusal by hostname for anything DNS-named) and
# prove a real fetch still completes through the one path left — or, with every path blocked, that the node keeps
# retrying instead of hanging or crashing. The oracle is the node's own trace (VG_FETCH_DEBUG + [finder] lines): a
# row passes only when the bytes landed AND the trace shows the intended path did the work and the blocked ones did not.
#
# Why this exists: a path can be silently DEAD while everything looks online. The HTTPS-gateway fallback was exactly
# that — it ran under a context the libp2p attempt had already exhausted, so on the hostile networks it was built for
# it failed instantly, every time, and the app sat at "will sync when online" with 88 peers connected. And the first
# run of THIS matrix caught its own harness hole (the DoH transport re-dialing a "blocked" host via the OS resolver).
# Nothing short of isolating each path and watching the trace catches that class.
#
# What the matrix established (run-2, 2026-09-15, Pinata-only content, PC seeder OFF):
#   * Pinata does NOT announce to the Amino DHT: the DHT's only provider record was our own dead seeder (its LAN,
#     WG, ZeroTier and eight Docker-bridge addrs). The DELEGATED INDEXER is the libp2p path to Pinata content, so it
#     is raced across two instances (delegated-ipfs.dev + cid.contact) and each is validated alone below.
#   * dht-only against Pinata content is therefore a DOCUMENTED NEGATIVE here (clean isolation + orderly retrying);
#     the real DHT-only validation needs a DHT-announcing provider — the two-node set (PC seeder ON, laptop fetching).
#
# Run-4 (2026-09-15, hardened binary — hedged gateways, indexer race, DoH race, dial-backoff clear): 6/8.
#   * gateway-any exposed a SECOND hole: the hedge winner (w3s.link) delivered a first block, then stalled; the
#     watchdog cut the CAR, but the ROOT was now local — so every later attempt skipped the gateway and looped a
#     peerless bitswap session ("incomplete" ×7). Fixed: a stalled session resumes its MISSING LEAVES over the gateway
#     (entity-bytes CAR from the first missing offset; Pinata honours it). gw-public failed the same way on the one
#     flaky public backend (16-byte bodies from curl at the time) — that row reads the backend's health at run time.
#   * gw-pinata 19.4 s · indexer-a-only 2 attempts · indexer-b-only 2 attempts · dht-only-neg 4 clean attempts
#     (4 stale providers = our own seeder) · nothing 10 clean attempts · baseline 145 s / 5 attempts (Pinata's
#     bitswap peer intermittent again).
# Run-5 (same day, with the leaf resume + budget split): 7/8 — gateway-any PASS (15 s, 1 attempt), gw-pinata 13.6 s,
#   indexer-a-only 2 attempts, indexer-b-only 5 attempts, dht-only-neg / nothing clean. gw-public FAIL again, and this
#   time the node was RIGHT: node-free curl at the same minute got HTTP 520 (16-byte body, 28 s) from two routes and a
#   TRUNCATED 1.5 MB of a 9.8 MB CAR from the third; the node hedged all three, failed cleanly, retried (3 attempts).
#   The one public backend is not a path to count on; the row stays because it reads that backend's health honestly.
# Run-6 (leaf resume, budget split, losers cancelled at commit; header wait 45 s): 7/8 again —
#   gateway-any 9.8 s, gw-pinata 11.4 s, indexer-a-only 2 attempts / 13 s, indexer-b-only 10 attempts / 72 s (cid.contact
#   is the flakier indexer), negatives clean; baseline 34.7 s THROUGH THE GATEWAY (Pinata's bitswap peer was not serving
#   again — the fallback did its job on the positive control). gw-public: the public backend still down (4 attempts).
#   The public backend, when it DOES answer, needs the full header wait: ipfs.io's first block landed 20.3 s after the
#   request in this run's gw-public row (then died) — the reason gatewayHeaderTimeout stays at 45 s (doh.go). Header
#   latency for present / absent content per route is in tools/gwprobe.sh + its dated output (2026-09-15): Pinata
#   headers 6.0 s (10 MB) / 7.1 s (1 GB CAR, streamed at ~9.7 MB/s) / 404 for an absent CID at 61.7 s; public backend
#   7.3 s (ipfs.io) and 1.1 s (w3s.link) to FIRST BYTE for the 10 MB CID — but both those 200s were CUT short of the
#   full CAR (7.9 and 6.3 of 9.8 MB) — and 520 at 28.2 s for the 1 GB CID and for an absent one; only-if-cached is
#   INCONSISTENT (Pinata: 412 for present in one manual probe, 200 in the committed one; 412 at 34 s for absent) —
#   so there is no cheap definitive negative, and the 45 s header wait is the probe's whole cost.
# Run-7 (the SHIPPED binary, 2026-09-15 17:36): 7/8 — baseline 38 s (Pinata bitswap peer down again → gateway),
#   gateway-any 43 s / 2 attempts (the leaf resume completed what a cut stream left), gw-pinata 11 s, indexer-a-only
#   6.3 s / 1 attempt, indexer-b-only 5.7 s / 1 attempt, dht-only-neg + nothing clean. gw-public: backend down (3 attempts).
#   ⚠ run-7's gateway-any "2 attempts" was the 5 s hedge delay sitting BELOW Pinata's 6.0–7.1 s header floor: the race
#   included the flaky backend, w3s.link WON it, died mid-stream, and the leaf resume cleaned up (43 s vs gw-pinata's
#   11 s). gatewayHedgeDelay is 10 s since — the primary now wins alone whenever it is healthy. Full run-7 results:
#   tools/netpaths.run7.2026-09-15.txt.
#
# Knobs (all outside the fetch logic):
#   VG_BENCH_NO_TUNNEL=1        install the libp2p gater (blocks the WG/ZT overlay; REQUIRED for BLOCK_CIDRS to apply)
#   VG_BENCH_BLOCK_CIDRS=...    libp2p-ONLY IP block (0.0.0.0/0,::/0 = no peer at all; HTTPS stays reachable)
#   VG_BENCH_BLOCK_HOSTS=...    DNS-layer block, hard-refused by resolver AND transport: gateways / indexers / bootstrap
#
# Usage:  tools/netpaths.sh [CID]     (results + per-row logs in $BASE; VG_BIN / VG_NETPATHS_DIR override)
set -uo pipefail

BIN="${VG_BIN:-$HOME/Code/VidyaGod/build/VidyaGod}"
CID="${1:-QmU1v8RnPEx8REbvkzaaVaUv9pawysPPCNyNhovSjhcRkm}"   # ~10 MB multi-block Minecraft delta, pinned on Pinata
BASE="${VG_NETPATHS_DIR:-$HOME/.cache/vgnetpaths}"           # btrfs, not tmpfs
PREV="$BASE.prev"                                            # last run's evidence survives ONE generation
OUT="$BASE/results.txt"
GW_PINATA="gateway.pinata.cloud"
# The public "trustless gateway" aliases are ONE backend (trustless-gateway.net) behind several CDN routes; ipfs.io,
# gateway.ipfs.io, w3s.link, dweb.link all REDIRECT into trustless-gateway.link/.net — so a per-alias row must allow the
# whole redirect chain, and a "backend" row blocks the chain as a unit.
GW_PUBLIC="ipfs.io,gateway.ipfs.io,w3s.link,dweb.link,trustless-gateway.link,trustless-gateway.net"
GATEWAYS="$GW_PINATA,$GW_PUBLIC"
IDX_A="delegated-ipfs.dev"; IDX_B="cid.contact"; INDEXERS="$IDX_A,$IDX_B"
BOOTSTRAP_HOST="bootstrap.libp2p.io"; BOOTSTRAP_LITERAL="104.131.131.82/32"
ALL_PEERS="0.0.0.0/0,::/0"

# Only ever wipe/rotate a directory THIS SCRIPT made. A results.txt filename is not proof of ownership (any dir can
# contain one) — the marker is a magic first line only we write. Anything else at $BASE is refused untouched.
OWNED=0
[ -f "$OUT" ] && head -1 "$OUT" | grep -q '^NETWORK-PATH MATRIX ' && OWNED=1
if [ -e "$BASE" ] && [ "$OWNED" != 1 ]; then echo "refusing to touch $BASE: not a directory this script created" >&2; exit 2; fi
if [ "$OWNED" = 1 ]; then rm -rf "$PREV"; mv "$BASE" "$PREV"; fi   # previous run's evidence survives one generation
mkdir -p "$BASE"; : > "$OUT"
say() { printf '%s\n' "$*" | tee -a "$OUT"; }
say "NETWORK-PATH MATRIX  $(date -Is)  cid=$CID  bin=$BIN"
say "(seeder must be OFF: $(pgrep -af 'VidyaGod' | grep -vcE 'konsole|claude|netpaths|pgrep') other VidyaGod proc(s) running)"
say "======================================================================="

# routerMax LOG N -> the largest provider count router N reported across all lookups in the log
routerMax() { grep -oE "\[finder\] router $2: [0-9]+ providers total" "$1" | awk '{print $4}' | sort -n | tail -1; }
# fetchSecs LOG -> the node's own fetch duration (the --fetch CLI first waits up to 30s for peers; with libp2p gated that
# warm-up is the full 30s every time — a probe artifact the GUI never pays, so wall time alone overstates the path)
fetchSecs() { grep -oE "Fetched [0-9]+ bytes in [0-9.]+s" "$1" | tail -1 | grep -oE "in [0-9.]+s" | tr -d 'in s'; }
# gwServed LOG -> the gateway host that served the DAG (if any)
gwServed() { grep -oE "gateway https://[^ ]+ served" "$1" | head -1 | sed -E 's|gateway https://([^ ]+) served|\1|'; }

pass=0; fail=0
# run NAME TIMEOUT ENV... — one row; the verdict rule is keyed by NAME below.
run() {
  local name="$1" tmo="$2"; shift 2
  local dd="$BASE/$name" log="$BASE/$name.log"
  mkdir -p "$dd"
  say ""; say "--- $name  (env: $*) ---"
  local t0=$(date +%s)
  env "$@" VG_FETCH_DEBUG=1 timeout "$tmo" "$BIN" --data-dir "$dd/repo" --bypass-single-instance-lock \
      --fetch "$CID" "$dd/out.bin" > "$log" 2>&1
  local rc=$?; local dt=$(( $(date +%s) - t0 ))
  local ok=0; grep -q "Materialized CID $CID" "$log" && ok=1
  local gw=$(gwServed "$log"); local viaGW=0; [ -n "$gw" ] && viaGW=1
  local r0=$(routerMax "$log" 0); local r1=$(routerMax "$log" 1); local r2=$(routerMax "$log" 2); local r3=$(routerMax "$log" 3)
  local idx=$(( ${r2:-0} > ${r3:-0} ? ${r2:-0} : ${r3:-0} ))
  local attempts=$(grep -c "FetchOnce ENTER" "$log")   # each queue dispatch = one attempt (rolling retries)
  local size=$(stat -c%s "$dd/out.bin" 2>/dev/null || echo 0); local fs=$(fetchSecs "$log")
  say "  rc=$rc wall=${dt}s fetch=${fs:-n/a}s ok=$ok size=$size gateway=${gw:-none} attempts=$attempts | providers: friend=${r0:-0} dht=${r1:-0} idxA=${r2:-0} idxB=${r3:-0}"
  local v="FAIL"
  case "$name" in
    baseline)         [ $ok = 1 ] && v=PASS ;;
    gateway-any)      [ $ok = 1 ] && [ $viaGW = 1 ] && v=PASS ;;
    gw-pinata)        [ $ok = 1 ] && [ "$gw" = "$GW_PINATA" ] && v=PASS ;;
    gw-public)        [ $ok = 1 ] && [ -n "$gw" ] && [ "$gw" != "$GW_PINATA" ] && v=PASS ;;
    indexer-a-only)   [ $ok = 1 ] && [ $viaGW = 0 ] && [ "${r2:-0}" -gt 0 ] && [ "${r3:-0}" -eq 0 ] && [ "${r1:-0}" -eq 0 ] && v=PASS ;;
    indexer-b-only)   [ $ok = 1 ] && [ $viaGW = 0 ] && [ "${r3:-0}" -gt 0 ] && [ "${r2:-0}" -eq 0 ] && [ "${r1:-0}" -eq 0 ] && v=PASS ;;
    # documented negative: Pinata is not in the DHT → must NOT fetch, isolation must be clean, node must keep retrying
    dht-only-neg)     [ $ok = 0 ] && [ $rc = 124 ] && [ "$attempts" -ge 2 ] && [ $viaGW = 0 ] && [ $idx -eq 0 ] && v=PASS
                      [ "${r1:-0}" -gt 0 ] && say "  note: the DHT did return ${r1} provider(s) — a stale record (our own seeder), not Pinata" ;;
    nothing)          [ $ok = 0 ] && [ $rc = 124 ] && [ "$attempts" -ge 2 ] && v=PASS ;;
  esac
  say "  => $name: $v"
  [ "$v" = PASS ] && pass=$((pass+1)) || fail=$((fail+1))
  rm -rf "$dd/repo" "$dd/out.bin"
}

# --- positive control
run baseline       180 VG_NOOP=1
# --- the HTTPS gateway path: any, then EACH BACKEND alone (all libp2p blocked; the other backend blocked as a unit)
run gateway-any    180 VG_BENCH_NO_TUNNEL=1 VG_BENCH_BLOCK_CIDRS="$ALL_PEERS"
run gw-pinata      180 VG_BENCH_NO_TUNNEL=1 VG_BENCH_BLOCK_CIDRS="$ALL_PEERS" VG_BENCH_BLOCK_HOSTS="$GW_PUBLIC"
run gw-public      180 VG_BENCH_NO_TUNNEL=1 VG_BENCH_BLOCK_CIDRS="$ALL_PEERS" VG_BENCH_BLOCK_HOSTS="$GW_PINATA"
# --- the libp2p path via EACH delegated indexer alone (dead DHT, gateways blocked, the other indexer blocked)
run indexer-a-only 180 VG_BENCH_NO_TUNNEL=1 VG_BENCH_BLOCK_HOSTS="$BOOTSTRAP_HOST,$GATEWAYS,$IDX_B" VG_BENCH_BLOCK_CIDRS="$BOOTSTRAP_LITERAL"
run indexer-b-only 180 VG_BENCH_NO_TUNNEL=1 VG_BENCH_BLOCK_HOSTS="$BOOTSTRAP_HOST,$GATEWAYS,$IDX_A" VG_BENCH_BLOCK_CIDRS="$BOOTSTRAP_LITERAL"
# --- the DHT alone against Pinata content: documented negative (see header)
run dht-only-neg   100 VG_BENCH_NO_TUNNEL=1 VG_BENCH_BLOCK_HOSTS="$INDEXERS,$GATEWAYS"
# --- negative control: no path at all → must keep retrying, never hang or crash
run nothing        100 VG_BENCH_NO_TUNNEL=1 VG_BENCH_BLOCK_CIDRS="$ALL_PEERS" VG_BENCH_BLOCK_HOSTS="$BOOTSTRAP_HOST,$INDEXERS,$GATEWAYS"

say ""; say "======================================================================="
say "MATRIX: $pass passed, $fail failed   (logs: $BASE/*.log)"
[ "$fail" -eq 0 ]
