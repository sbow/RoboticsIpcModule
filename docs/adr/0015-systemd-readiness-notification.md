# ADR 0015: systemd readiness notification (`Type=notify`, no libsystemd)

- **Status:** Accepted (C6 closure, production-hardening cluster)
- **Date:** 2026-06-03
- **Builds on:** [ADR 0004](0004-robotics-module-boundaries.md) (dependency-light / header-only module boundary), [ADR 0007](0007-router-idle-wake.md) (the forward-loop structure this hooks into)
- **Closes:** [post-phases review C6 — systemd readiness signaling](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c6--systemd-readiness-signaling-sd_notify--typenotify)
- **Scope:** How the router signals "I have finished binding my endpoints" to systemd so dependent peer units start only when the router is actually ready; why we send the `sd_notify` datagram ourselves instead of linking `libsystemd`.

## Context

The Phase E E2 units shipped `rim-router.service` as `Type=simple`. With
`Type=simple`, systemd considers the unit "started" the instant it
`fork()`/`exec()`s the binary — **not** when the router has bound its SHM
regions / UDS socket / UDP port. A peer unit gated on the router with:

```ini
After=rim-router.service
Requires=rim-router.service
```

is therefore released the moment `router_server` is `exec()`'d, while the
router is still mapping `/dev/shm/rim_router_*` and calling `bind()`. The peer
races the router's first `shm_open` / `connect` and fails intermittently —
exactly the kind of boot-time flake that is painful to reproduce on a Jetson
in the field. The C6 backlog entry flagged this as a "real Jetson-production
footgun" and the shipped units even carried inline comments telling operators
that peers must retry-with-backoff "until `sd_notify("READY=1")` integration
lands."

The canonical fix is the systemd readiness protocol: the daemon declares
`Type=notify`, and after it is genuinely ready it calls
`sd_notify(3)` with `READY=1`. systemd then treats the unit as `active` only
after that signal, so `After=`/`Requires=` ordering becomes readiness ordering.

The obvious implementation is to link `libsystemd` and call `sd_notify()`. But
[ADR 0004](0004-robotics-module-boundaries.md) deliberately keeps this module
**header-only and dependency-light** — the build links only `-lrt -pthread`
today. Pulling `libsystemd` (and a `pkg-config` probe, and a `HAVE_LIBSYSTEMD`
compile path, and a non-Linux fallback) onto the link line for a one-line
datagram is a poor trade.

## Decision

### Send the `sd_notify` datagram ourselves — no `libsystemd`

The `sd_notify(3)` wire protocol is trivial and stable: the daemon sends a
newline-separated `KEY=VALUE` datagram to the `AF_UNIX` socket whose path is in
the `$NOTIFY_SOCKET` environment variable. We implement exactly that in
`ipc/src/router_app.h` (the app-only convenience header — library headers under
`src/router/` never include it):

```cpp
inline bool router_sd_notify(const char* state);   // sends one status line
inline bool router_notify_ready();                 // "READY=1"
inline bool router_notify_stopping();              // "STOPPING=1"
```

`router_sd_notify`:

1. Reads `$NOTIFY_SOCKET`. If unset or empty, returns `false` immediately — a
   **silent no-op** when not running under `Type=notify` (a shell, a test
   harness, or a deployment still on `Type=simple`). Callers invoke it
   unconditionally.
2. Builds a `sockaddr_un`. A leading `@` is systemd's spelling of an
   abstract-namespace socket (first byte `NUL`); a leading `/` is a filesystem
   socket. Both forms are handled, with the abstract form's address length
   excluding the trailing `NUL`.
3. Rejects a path that does not fit in `sun_path` (returns `false`, no
   overflow).
4. Opens a transient `AF_UNIX`/`SOCK_DGRAM`/`SOCK_CLOEXEC` socket, `sendto`s
   the status line with `MSG_NOSIGNAL`, closes it, and reports whether the full
   payload was sent.

No new link dependency, no new compile flag, ~40 lines, header-only — it stays
inside the ADR 0004 boundary.

### Where readiness is signalled

`run_forward_loop()` in `ipc/test/router_server.cpp` is the single chokepoint
every transport path reaches **after** binding (the SHM / UDS / UDP / mixed
runners each bind, then call it). It calls `router_notify_ready()` once, just
before entering `server.run()`:

```cpp
if (router_notify_ready()) {
    router_log(ROUTER_LOG_INFO, "sd_notify READY=1 (endpoints bound)");
}
server.run(/* ... */);
router_notify_stopping();   // clean-shutdown window after the loop unwinds
```

`STOPPING=1` is sent after the loop returns (stop requested or idle-exit) so
systemd can distinguish a clean stop from a crash during shutdown.

### Unit change

`rim-router.service` flips `Type=simple` → `Type=notify` and adds
`NotifyAccess=main` (the default, stated explicitly — the router is
single-process). `rim-peer@.service` keeps its `After=`/`Requires=` ordering;
its comments now note that ordering gates on real readiness, with retry-with-
backoff retained as a documented backstop.

## Consequences

### Positive

- Peers gated on the router start only after the router's endpoints are bound;
  the boot-time `shm_open`/`connect` race is closed at the source.
- Zero new build dependencies — the binary still links `-lrt -pthread`.
- The notify path is exercised by `sd_notify_test` (filesystem + abstract
  sockets, no-op contract, oversized-path rejection) with no SHM, fork, or
  systemd required, so it runs in plain CI.
- Defence in depth: peer-side retry stays documented, so a deployment that
  reverts to `Type=simple` is still safe.

### Negative / Neutral

- We hand-roll a slice of the `sd_notify` protocol. We implement only the
  subset the router needs (`READY=1`, `STOPPING=1`); `WATCHDOG=1`,
  `RELOADING=1`, `FDSTORE=1`, and `MAINPID=` are not implemented. If a future
  deployment wants the systemd watchdog, that is a small additive change
  (periodic `router_sd_notify("WATCHDOG=1")` plus `WatchdogSec=` in the unit).
- `unset_environment` semantics from `sd_notify(3)` (clearing `$NOTIFY_SOCKET`
  so children don't inherit it) are not replicated. The router does not spawn
  children, so inheritance is moot; documented here so it is a conscious
  omission rather than an oversight.

## Alternatives considered

### A. Link `libsystemd` and call `sd_notify()`

Rejected for this module: adds a `pkg-config`/link dependency, a
`HAVE_LIBSYSTEMD` compile path, and a non-Linux fallback, all for a one-line
datagram. Violates the ADR 0004 dependency-light posture. The inline datagram
is functionally identical for the `READY=1`/`STOPPING=1` subset we use.

### B. Stay `Type=simple`, rely on peer-side retry only

Rejected as the primary fix: it pushes a boot-ordering race onto every peer
author (including non-C++ bridges) and was already the unsatisfactory
status-quo workaround the C6 entry called out. Retry stays as a *backstop*, not
the mechanism.

### C. `Type=forking` / readiness-by-pidfile

Rejected: `Type=forking` doesn't fit a single foreground process, and a
pidfile/socket-probe handshake is more moving parts than the standard notify
protocol systemd already supports natively.

## Verification

```bash
# Unit test — wire protocol, no systemd needed:
make test-sd-notify        # sd_notify_test: 15/15 assertions passed

# On a systemd host, confirm readiness ordering end to end:
systemctl show -p Type,NotifyAccess rim-router.service     # Type=notify
journalctl -u rim-router.service | grep 'sd_notify READY=1'
# rim-peer@*.service instances stay queued until READY=1 arrives.
```
