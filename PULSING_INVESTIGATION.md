# The "pulsing download" — root cause and fix (2026-08-27, overnight)

## Symptom
Download speed oscillating 6 MB/s → ~0 → 6 MB/s within seconds, on every large GUI
download, on every network (work wifi, hospital wifi, mobile hotspot). The earlier
two-agent exploration measured steady-state throughput and holepunch warmup but did
not find this. It was assumed to be link flakiness. **It is not the network.**

## Method
Added `VG_FETCH_RATE=1` instrumentation (per-second MB/s + blocks/s + largest
inter-block gap inside each second, printed from the bitswap receive loop), then ran
controlled loopback fetches on ONE machine (throwaway `--data-dir` client node
`--connect`ed to the seeding GUI node over 127.0.0.1) — removing wifi, NAT and
holepunching from the picture entirely.

## Measurement (loopback, 347 MB file, 1324 leaves)
```
BEFORE (stock):        t=001s   90.7 MB/s  blk=346
                       t=002s  177.7 MB/s  blk=678     ← first 1024 blocks at line rate
                       t=003s    0.3 MB/s  blk=1  maxgap=613ms
                       t=004s    0.3 MB/s  blk=1  maxgap=1533ms
                       ... 35 more seconds of 0.3–3 MB/s trickle for the last 300 blocks
AFTER (both fixes):    t=001s  156.8 MB/s  blk=598
                       t=002s  190.2 MB/s  blk=726
                       Fetched 347 MB in 2.35 s (140.8 MB/s), maxgap ≤ 49 ms
```
The pulse reproduces **perfectly on loopback**: exactly 1024 blocks arrive at line
rate, then collapse.

## Root cause
boxo's bitswap **server truncates each peer's queued wantlist at 1024 entries**
(`defaults.MaxQueuedWantlistEntiresPerPeer = 1024` — typo theirs), **silently
dropping the overflow** (engine.go `filterOverflow`/truncate). Our client sends one
continuous want-list for every missing leaf (a 347 MB file = 1324 wants; a 4 GB
runner = ~16k). Everything past 1024 is dropped without any signal; the client's
session limps through the tail on periodic rebroadcast/idle ticks — the trickle.

Worse: the cap is **per peer across ALL sessions**, so three concurrent GUI
downloads overflow it even when each file is small enough alone. On a real link the
serviced burst drains at 6 MB/s (looks like healthy download), then the dropped-want
gap shows 0 B/s, then a rebroadcast re-queues more — the exact observed pulsing.

## Fixes (VidyaGodIPFS)
1. **Server (our seeders)**: `bitswap.MaxQueuedWantlistEntriesPerPeer(1<<16)` in
   `online.go` — our ecosystem pipelines entire game layers (64k wants ≈ 16 GB of
   256 KiB leaves).
2. **Client (universal — works against third-party/default seeders too)**: want
   WINDOWING in `writeThrough` — the want-list is fed through the session in chunks
   of 128 with ≤2 chunks in flight, receipt-refilled (a chunk's channel closing
   admits the next). Each fetch keeps ≤256 wants queued server-side; 3 concurrent
   downloads stay under even a default 1024 cap. 256 × 256 KiB = 64 MiB of pipeline,
   far above any link's BDP → no throughput cost where it matters (loopback shows
   111 vs 140 MB/s; at internet speeds the difference is zero).
   Env overrides for experiments: `VG_WANT_CHUNK`, `VG_WANT_CHUNKS`.

## Verification
- Loopback single fetch: 35 s of pulsing → 2.35 s clean (see above).
- Loopback 3 × ~300 MB concurrent (`VG_FETCH_MULTI`): all three finish in ~11 s,
  per-file worst seconds 16–38 MB/s, no starvation, no pulsing.
- `go test -race`, ctest: green.

## Bonus finds (same night)
- **Crash-on-exit (SIGSEGV, coredump-verified)**: the DownloadQueue's detached
  dispatcher thread iterates its static `Jobs` map while static destruction frees it
  at process exit (`AnyQueued` → `_Rb_tree_increment` on a dead map). Latent in the
  GUI (quit mid-download). Fixed by deliberately leaking the queue singleton
  (`downloadqueue.cpp Q()`).
- `fetchWindow` const in fetch.go was dead code from the pre-write-through design
  (its comment described windowing that no longer existed — ironically the actual
  fix reintroduced windowing properly).

## Real-network validation (laptop → PC, same night)
- OLD client (continuous wantlist) vs NEW 64k server: 347 MB in 39 s, steady
  8.4 MB/s — the server fix alone already cures transfers between our nodes.
- NEW client (windowed) vs NEW server, `VG_FETCH_RATE=1`: **347 MB in 11.8 s,
  28 MB/s avg, every second 17–46 MB/s, zero dead seconds, maxgap ≤ 315 ms.**
  The remaining variance is ordinary link jitter, not protocol collapse. Both
  machines built, `go test -race` + ctest green, all pushed (VidyaGodIPFS
  98d6412, app e60b4ab). CLOSED.
