# IPFS Transfer — Optimization Findings & Final Refactor Plan

Status: **exploration phase** (live two-machine experiments). This document is the plan for the
**final refactor** to run *after* exploration concludes. Do not start the refactor tasks until the
open investigations below are closed.

Experiment setup: PC (home NAT, seeder) + laptop (hostile hospital NAT, client, driven by a separate
Opus Claude Code session). Goal: maximise open-internet transfer bandwidth between two hostile NATs
(explicitly NOT over the WireGuard/ZeroTier tunnels the two machines share).

---

## 1. Verified findings (the exploration)

- **Steady-state throughput is uplink-bound, not protocol-bound.** Direct open-internet transfer
  sustained **11.5 MB/s = the PC's measured home upload ceiling** (Cloudflare `__up` = 11.52 MB/s).
  Single-source content cannot go faster in software.
- **Multi-source scales bandwidth** — boxo bitswap `Session` (`fetch.go` NewSession/GetBlocks) fans one
  wantlist across ALL providers in parallel via `combinedFinder`; N seeders aggregate toward the
  *client* downlink. **Confirmed in code only — never measured with 2 seeders (open gap).**
- **Path:** discovery ~0.3s → relay circuit up ~8s (AutoRelay, clean single handshake) → **DCUtR
  holepunch → direct QUIC on the seeder's home public IP** → 11.5 MB/s.
- **circuit-relay-v2 is data-capped by design** (control-plane only). Bitswap CANNOT pull bulk over the
  relay; it waits for the direct path. "Fetch root over relay to cut warmup" is a **dead end**.
- **Holepunch warmup ~8-16s is inherent NAT-traversal latency** (variable, pure DCUtR noise).
  **Amortised to ~0 in the real long-lived GUI node** (paid once per peer per session; the CLI `--fetch`
  pays it every run only because each invocation is a fresh process).

## 2. Changes shipped during exploration — keep / refactor decisions

| Change | File | Verdict for final refactor |
|---|---|---|
| **dropRef batched** (15.838s → 102ms) | `VidyaGodIPFS/fetch.go` | **KEEP as-is.** Real fix for the missing-files/heal refetch path. Production-ready. |
| **Provider warmer** (live DHT addr refresh, non-blocking, 8s-bounded FindPeer) | `VidyaGodIPFS/warm.go` | **KEEP, minor cleanup.** Fixes cold-cache hard failure. Review: is the `warmProviderCount=6` fan-out + 8s FindPeer bound right for the common (non-hostile) net? Consider gating the FindPeer refresh to only fire when the record-addr dial fails. |
| **Bench gater + observer** (`VG_BENCH_NO_TUNNEL`, `VG_BENCH_OBSERVE`) | `VidyaGodIPFS/bench.go` | **DECIDE:** keep as a permanently-available diagnostic (env-gated, zero cost when off) OR strip before release. Leaning KEEP — the observer is invaluable for field debugging; the gater is niche but harmless. If kept, fix the per-PEER-vs-per-CONNECTION bandwidth attribution caveat (it can mislead). |
| **Warmup phase timestamps** (fdbg in warm.go/fetch.go) | both | **KEEP** — all under `VG_FETCH_DEBUG`, no cost when off. |

## 3. Final refactor tasks (after exploration)

1. Land the keep/cleanup decisions in §2.
2. **Decide bench.go's fate** (permanent diagnostic vs strip). If permanent, document the env vars in the
   IPFS tab or a `--help` and fix the observer's per-peer bandwidth attribution.
3. **Warmer polish:** only run the bounded FindPeer refresh when the record-addr dial fails/stalls, to
   avoid redundant DHT walks on healthy networks.
4. Update `project_ipfs_transfer_optimization` memory + this file with final numbers.
5. Run the full both-machines build + ctest gate before merge.

## 4. Open robustness work (bandwidth is DONE; these are reliability, prioritised)

1. **[STALL — SEE §5, TOP PRIORITY]** GUI large-game download pulses (bursts then stalls, repeats).
2. **Un-holepunchable pair has no bulk fallback.** Symmetric-NAT ↔ symmetric-NAT: DCUtR fails, relay-v2
   can't carry data → download FAILS. Decide: accept (rely on multi-source finding one reachable seeder)
   or add a data-carrying relay/TURN (needs public infra the user lacks). Compounded on SSL-MITM nets
   where the HTTPS-gateway fallback is also dead and content is private.
3. **Prove multi-seeder aggregation** — 2nd seeder, different uplink, confirm aggregate > one uplink.
4. **Confirm warm-node amortisation** empirically — one long-lived node, second transfer, ~0 warmup.

## 5. ACTIVE INVESTIGATION — the "pulsing" stall (top priority)

Symptom (user, live GUI, large games): connects → downloads a couple seconds at ~10 MB/s → **stalls** →
repeats every few seconds. Wants a stable, non-flaky link. **Also load-bearing for multiplayer**
(L3 tunnel over libp2p streams — a pulsing underlying connection = a flaky tunnel).

Note: our `--fetch` benchmark ran CLEAN to completion with no pulsing. The difference is the GUI's
long-lived, many-connection, multi-GB, multi-layer, concurrent-queue download. Leading hypotheses to
test (see investigation notes appended below as findings land):
- **connmgr pruning** the seeder/relay connection under connection-count pressure (400/900 high-water,
  20s grace — a long transfer's connection ages past grace and gets trimmed mid-stream).
- **DCUtR direct-path instability** — holepunched UDP NAT binding churn → periodic drop back to
  (data-incapable) relay → stall → re-punch.
- **Concurrent download queue** contention (multiple layers hitting one seeder-peer, the 128MB per-peer
  window / 16 workers interacting with stalls).
- **Seeder-side upload contention** — reprovide sweep / DHT churn starving the upload.

Fix target: a stable long-lived direct connection that does not drop mid-transfer.
