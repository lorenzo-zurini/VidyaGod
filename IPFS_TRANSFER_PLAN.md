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

### 5.1 DIAGNOSIS (done, 2026-08-26) — it's QUIC/CUBIC cwnd collapse on packet loss

Reproduced with a 1 GB fetch over the hostile wifi. The stall is a **transport-layer throughput
collapse on a HEALTHY, never-dropped direct QUIC connection** — affirmatively NOT churn, connmgr
pruning, relay-cap cycling, or re-holepunch:
- Same direct QUIC conn (`86.120.154.71:udp/45275`) present before/during/after the stall; no new
  ephemeral port, no relay flip.
- Client download AND seeder upload decay to zero **in lockstep**, gradually (~6s down, ~4s up).
- Seeder had ZERO internal events (no reprovide/compaction/provide) during the transfer → not a seeder
  pause. One continuous bitswap session, `stalled=false`, no teardown/backoff/resume → invisible above
  the transport.
- Signature = **CUBIC congestion-control collapse-and-recover after a hostile-wifi packet-loss burst**
  (loss → cwnd drains, possibly an RTO to true zero → slow-start rebuild).

Constraints found:
- **No BBR anywhere in quic-go (checked through v0.61, latest) — only CUBIC.** No config swap or version
  upgrade gives loss-tolerant congestion control. A quic-go fork with BBR is the only CC route (heavy).
- go-libp2p receive windows already large (10 MB stream / 15 MB conn) — NOT the limiter.
- Laptop `net.core.rmem_max` = 4 MB (quic-go wants ~7 MB). Possibly contributory (buffer overflow on
  10 MB/s bursts = real loss) but likely marginal (bitswap reads the socket promptly). **Raising it needs
  root and there is NO userspace way past the kernel `rmem_max` ceiling** — a deployment problem for an
  AppImage/Flatpak (sysctl.d drop-in via installer, or documented dep check), not a code fix. A/B test
  blocked on passwordless sudo on the laptop.

### 5.2 FIX OPTIONS (scope decisions for the user)

**Downloads** (pulse lowers average but completes byte-correct — resilient):
- (a) Ship an 8 MB `rmem_max` sysctl.d drop-in via packaging + document it. Cheap, partial, needs
  root-once. Confirm impact with the A/B first.
- (b) Fork quic-go for BBR. Big win on lossy links, heavy maintenance. Probably not worth it for
  downloads alone.
- (c) Accept it — downloads resume and complete; the pulse is a throughput annoyance, not a failure.

**Multiplayer (the crucial one) — a real, actionable code fix:**
The overlay datapath (`overlay.go`) tunnels raw IP packets over a **reliable, ordered libp2p STREAM**
(`/vidyagod/overlay/1.0.0`). For real-time game traffic (UDP-native, loss-tolerant, latency-sensitive)
this is the WRONG transport: a single reliable ordered stream converts wifi packet loss into
head-of-line-blocking + CUBIC cwnd collapse → tunnel lag/freezes exactly like the download pulse.
**FIX: move the overlay datapath to QUIC unreliable DATAGRAMS** (the quicreuse config already sets
`EnableDatagrams: true`). Game packets are small (< ~1200 B path-MTU) and loss-tolerant, so datagrams
give no retransmit, no HoL-blocking, no cwnd-collapse stall. Needs: a feasibility check on whether
go-libp2p v0.48 exposes datagram send/recv through its Conn abstraction (it's a QUIC-specific feature,
may need reaching the underlying `quic.Conn` or a newer go-libp2p); fall back to the stream path for
oversized packets and non-QUIC transports. If datagrams aren't reachable, a weaker mitigation is
per-flow streams (independent QUIC streams remove HoL-blocking, though shared connection CC still paces
them). **This is the highest-value follow-up and directly serves "crucial for multiplayer".**
