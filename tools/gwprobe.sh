#!/usr/bin/env bash
# gwprobe.sh — node-free HEADER-LATENCY probe of the trustless gateways: how long each route takes to START answering
# a CAR request (curl's time_starttransfer), for content of two sizes and for a CID nobody has, plain and with
# `Cache-Control: only-if-cached`. This is the artifact behind the gateway header-timeout comments (doh.go) and the
# matrix header (netpaths.sh): a probe for content no gateway has costs whatever the routes take to say no.
# Usage: tools/gwprobe.sh [MAXTIME=70] > tools/gwprobe.<date>.txt
set -u
MAXTIME="${1:-70}"
SMALL=QmU1v8RnPEx8REbvkzaaVaUv9pawysPPCNyNhovSjhcRkm      # ~10 MB (Minecraft delta), pinned on Pinata
LARGE=QmQcJQwMPiwfTL4y6VWnV6AYjs6bqiwuV5ucXFpPh89T9M      # 1009 MB (base_GE-Proton7-55.zip), pinned on Pinata
ABSENT=Qmb1Cfh3rQuT4i7pVHnKswpcUNFhjd8T91fZeZVcyknjTi     # valid CIDv0 of random bytes: nobody has it
GWS="gateway.pinata.cloud ipfs.io w3s.link"
echo "GATEWAY HEADER-LATENCY PROBE $(date -Is)  max-time=${MAXTIME}s  (code / bytes-before-abort / seconds-to-first-byte / total)"
probe() { # GW CID LABEL [extra curl args...]
  local gw="$1" cid="$2" label="$3"; shift 3
  local out; out=$(curl -sL --max-time "$MAXTIME" -o /dev/null -w '%{http_code} %{size_download} %{time_starttransfer} %{time_total}' \
        -H 'Accept: application/vnd.ipld.car' "$@" "https://$gw/ipfs/$cid?format=car&car-order=dfs&dag-scope=all" 2>&1)
  printf '%-22s %-7s %-16s %s\n' "$gw" "$label" "${1:+only-if-cached}" "$out"   # $1 = the first extra curl arg, if any
}
for gw in $GWS; do probe "$gw" "$SMALL" small; probe "$gw" "$LARGE" 1GB; probe "$gw" "$ABSENT" absent; done
for gw in gateway.pinata.cloud ipfs.io; do probe "$gw" "$SMALL" small -H 'Cache-Control: only-if-cached'; probe "$gw" "$ABSENT" absent -H 'Cache-Control: only-if-cached'; done
echo PROBE_DONE
