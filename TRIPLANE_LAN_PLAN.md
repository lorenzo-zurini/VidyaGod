# Tri-plane LAN: host-LAN broadcast visibility from the sandbox (the reflector)

## Context

Bridge mode (pasta) gave sandboxed games internet + outbound real-LAN reach, but a game browsing for LAN sessions
still cannot SEE hosts on the physical LAN — inbound broadcasts don't traverse NAT, and true L2 bridging needs
root. The user wants all three at once: overlay vLAN (+ its internal broadcasts), real-LAN visibility INCLUDING
broadcasts, and internet — rootless.

**The insight that makes it possible**: (1) we already own the game's entire packet stream — `overlay.go
readLoop` sees every packet the game writes to the TUN, and `isFanout` already classifies broadcast/multicast
(that's how friend fan-out works); (2) an *unprivileged* host process CAN receive and send real-LAN UDP
broadcasts — binding `0.0.0.0:<port>` with `SO_BROADCAST` needs no capabilities. So a **userspace broadcast
reflector** in the node bridges the two broadcast domains that no kernel facility can join rootlessly.

**Result — one virtual NIC, three planes**:
- overlay plane: friends via libp2p fan-out (existing)
- host plane: real-LAN discovery via reflected broadcasts (NEW)
- internet plane: pasta NAT (existing)

## Design

### Reflector (`VidyaGodIPFS/hostrelay.go`, new ~250 lines)

**Outbound (game → real LAN)** — hook in `readLoop`'s fanout branch (`overlay.go:260`): in addition to the friend
fan-out, parse IPv4/UDP and hand `(srcPort S, dstPort D, payload)` to the reflector, which re-emits it as a REAL
host broadcast: `sendto(255.255.255.255:D)` from a host UDP socket bound to `:S` (multicast dst 224.0.0.0/4 →
`sendto(group:D)` likewise).

**Port learning (no whitelist)** — the game itself declares its discovery ports by broadcasting: on first sight of
`S→D`, bind host sockets `:S` (unicast replies from real peers arrive here) and `:D` (unsolicited announcements
from real-LAN hosts arrive here); dedupe when S==D (the common case, e.g. DirectPlay 6073). `SO_REUSEADDR` +
`SO_BROADCAST`; bind failure → log + skip (host service already owns the port).

**Inbound (real LAN → game)** — each bound socket's recv loop synthesizes an IPv4/UDP packet (src = the real
peer's IP:port; dst = `255.255.255.255:port` for broadcasts, `<gameVIP>:port` for unicast replies; checksums
computed) and injects it via the existing TUN link (`link.WritePacket`). The game's reply goes unicast to the
real peer's IP → routes out through pasta → arrives. Full LAN browsing + joining of real-LAN-hosted games.

**Loop prevention** — drop inbound datagrams whose source IP is one of the host's own addresses (snapshot host
addrs at attach, refresh lazily). Two VidyaGod machines on one physical LAN then see each other BOTH via the real
LAN and the overlay — duplicate announcements are normal for these protocols (they re-announce every second).

**Wiring** — `overlayService` gains a `relay *hostRelay` created in `attach()` (inject = `link.WritePacket`,
gameVIP = `o.localVIP`), closed in `detach()`. Gate: `VgOverlayServe` gains a `hostRelay` flag param
(api_overlay.go + vgipfsapi.h + ipfswrapper `OverlayServe`); C++ passes it from the per-game CustomVar
`VIDYAGOD_LAN_HOSTRELAY` (default on, "off" disables) — same pattern as `VIDYAGOD_LAN_BRIDGE`.

**Pure helpers, unit-tested**: `parseUDP4(pkt) (src, dst, sport, dport, payload, ok)` and
`buildUDP4(src, dst, sport, dport, payload)` with correct checksums; the learn table.

### sandbox-init (`src/sandboxinit.cpp`)

Append to the existing `ipcmd`: `sysctl -w net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.default.rp_filter=0` —
injected packets carry real-LAN source IPs whose return route points at pasta, and strict reverse-path filtering
would silently drop them (loose mode passes; pin 0 for determinism).

### Known, documented limitation

Protocols that EMBED their address in the payload (DirectPlay does): a real-LAN peer joining a game HOSTED in the
sandbox may fail (embedded 10.66.x.x is unreachable from the real LAN) — while the sandboxed player joining a
real-LAN-hosted game works fully (the embedded address is real, reachable via pasta). Symmetric fix would need
per-protocol payload rewriting — explicitly out of scope; the overlay plane (friends) has no such asymmetry.

## Files

- `VidyaGodIPFS/hostrelay.go` (new): reflector + packet helpers.
- `VidyaGodIPFS/overlay.go`: fanout-branch hook, attach/detach lifecycle, relay field.
- `VidyaGodIPFS/api_overlay.go`, `src/vgipfsapi.h`, `src/ipfswrapper.{h,cpp}`: `OverlayServe(sock, hostRelay)`.
- `src/containerwrapper.cpp`: read `VIDYAGOD_LAN_HOSTRELAY` var → pass to `OverlayServe`.
- `src/sandboxinit.cpp`: rp_filter sysctls.
- `VidyaGodIPFS/hostrelay_test.go` (new): parse/build round-trip incl. checksums; learn-table dedupe; loopback
  inject path with real UDP sockets on 127.0.0.1.

## Verification

1. `go test ./...` (new hostrelay tests) + `cmake --build` + `ctest` on BOTH machines.
2. Single-machine field test on the PC: a host-side python "fake LAN peer" broadcasting DirectPlay-style
   announcements on UDP :6073 every second; launch AoE2 → its multiplayer browser should list the fake session
   (inbound reflection), and the fake peer's socket should receive the game's own announcements (outbound
   reflection) — verifiable from the script's recv log.
3. Cross-plane regression: friend fan-out still works (laptop overlay peer sees the same announcements).
