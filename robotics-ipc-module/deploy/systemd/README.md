# systemd units — RoboticsIpcModule (Phase E E2)

Example systemd units for running the router and peer processes on Linux. Targeted at **Jetson on-robot** (SHM transport) but the shape works for any single-host deployment.

> **These files are examples**, not a turnkey deployment. Read the comments inline, then adapt the paths, transport, user/group, and hardening directives to your environment before enabling.

## Files in this directory

| File | Purpose |
|------|---------|
| [`rim-router.service`](rim-router.service) | Main router unit. Started first. Owns SHM region lifecycle. |
| [`rim-peer@.service`](rim-peer@.service) | Template unit for peer roles (`%i` = `sensor` / `controller` / `recorder`). `After=`+`Requires=` the router. |
| [`rim-router-cleanup.sh`](rim-router-cleanup.sh) | `ExecStopPost=` helper. Idempotent `rm -f` of leftover `/dev/shm/rim_*` and `/tmp/rim_*.sock`. |

## Install layout (example)

The unit files reference `/opt/rim/` as the canonical install prefix; adjust `ExecStart=` / `ExecStopPost=` paths to wherever you actually put the binaries:

```
/opt/rim/bin/router_server          # built from ipc/test/router_server.cpp
/opt/rim/bin/router_client          # built from ipc/test/router_client.cpp
/opt/rim/libexec/rim-router-cleanup.sh
/opt/rim/docs/robotics-reference-layout.md   # optional; for Documentation=
/etc/rim/router.toml                # your deployment profile
/etc/rim/peer-sensor.env            # optional per-peer overrides
/etc/rim/peer-controller.env
/etc/rim/peer-recorder.env
/etc/systemd/system/rim-router.service
/etc/systemd/system/rim-peer@.service
```

## Install steps

```sh
# 1. Build the binaries.
make all

# 2. Stage install tree (run as root or via your package manager).
sudo install -d /opt/rim/bin /opt/rim/libexec /opt/rim/docs /etc/rim
sudo install -m 0755 build/ipc/test/router_server    /opt/rim/bin/
sudo install -m 0755 build/ipc/test/router_client    /opt/rim/bin/
sudo install -m 0755 robotics-ipc-module/deploy/systemd/rim-router-cleanup.sh \
                                                      /opt/rim/libexec/
sudo install -m 0644 docs/robotics-reference-layout.md /opt/rim/docs/

# 3. Pick a deployment profile (see config/profiles/*.toml).
sudo install -m 0644 config/profiles/jetson_prod.toml /etc/rim/router.toml

# 4. Install the units.
sudo install -m 0644 robotics-ipc-module/deploy/systemd/rim-router.service \
                                                      /etc/systemd/system/
sudo install -m 0644 robotics-ipc-module/deploy/systemd/rim-peer@.service \
                                                      /etc/systemd/system/

# 5. Reload systemd, enable + start router, then peers.
sudo systemctl daemon-reload
sudo systemctl enable --now rim-router.service
sudo systemctl enable --now rim-peer@sensor.service
sudo systemctl enable --now rim-peer@controller.service
sudo systemctl enable --now rim-peer@recorder.service

# 6. Verify (peer logs land in journald + /tmp/rim_router_*.log).
journalctl -u rim-router.service -f
journalctl -u 'rim-peer@*.service' -f
```

## Verification (before deploying)

Syntax-check the units without enabling them:

```sh
systemd-analyze verify robotics-ipc-module/deploy/systemd/*.service
```

`systemd-analyze` will complain about paths that don't exist yet (e.g. `/opt/rim/bin/router_server`); those warnings are expected pre-install and resolve once you've completed the install steps above.

Dry-run a unit's environment without running it:

```sh
systemd-run --user --unit=rim-router-dryrun \
  --property=Environment=foo=bar -- /opt/rim/bin/router_server --help
```

## Customization

### Picking a deployment profile

Each `config/profiles/*.toml` corresponds to a transport story:

| Profile | Transport | Use case |
|---------|-----------|----------|
| `jetson_prod.toml` | SHM | On-robot Jetson; per-peer rings under `/dev/shm/rim_router_*` |
| `x86_dev.toml` | UDS | Developer laptop; sockets under `/tmp/rim_router_*.sock` |
| `hil.toml` | UDP loopback | HIL bench; ports 19100–19103 on `127.0.0.1` |
| `sim_cloud.toml` | UDP | Cloud / CI sim; per-host UDP across a container subnet |

Symlink (or `cp`) the chosen profile to `/etc/rim/router.toml`. The router auto-derives transport from the TOML's `[router] listen =` URI.

### Per-peer overrides via `EnvironmentFile`

`rim-peer@.service` reads `/etc/rim/peer-<role>.env` if present. Use this to override transport per instance without editing the unit file:

```sh
# /etc/rim/peer-sensor.env
RIM_TRANSPORT=uds
RIM_EXTRA_ARGS=/tmp/rim_router_a.sock
```

The unit's `ExecStart` substitutes these into `router_client %i ${RIM_TRANSPORT} ${RIM_EXTRA_ARGS}`.

### Per-deployment drop-ins

Prefer drop-in files over editing the shipped units directly:

```sh
sudo systemctl edit rim-router.service     # creates /etc/systemd/system/rim-router.service.d/override.conf
```

Common drop-ins:

```ini
# /etc/systemd/system/rim-router.service.d/hardening.conf
[Service]
MemoryLock=infinity
CPUAffinity=2
LimitRTPRIO=80
```

```ini
# /etc/systemd/system/rim-peer@.service.d/transport-uds.conf
[Service]
Environment=RIM_TRANSPORT=uds
```

### Running as non-root

For non-development deployments, create a dedicated user/group and pin the units to it:

```sh
sudo useradd --system --no-create-home --shell /sbin/nologin rim
sudo install -d -o rim -g rim -m 0755 /run/rim
```

Then add to a drop-in:

```ini
[Service]
User=rim
Group=rim
SupplementaryGroups=dialout   # only if mavlink_gateway needs serial access
```

The `rim` user needs write access to `/dev/shm` (default 1777 — works for any user) and `/run/rim` (created above).

## Logging

The router's `RouterLogFn` defaults to `demo_stderr_logger` (writes to stderr). systemd captures that into journald via `StandardError=journal`. View live:

```sh
journalctl -u rim-router.service -f
journalctl -u 'rim-peer@*.service' -f
```

Peer demos *additionally* write per-frame CSV records to `/tmp/rim_router_<role>.log` (see [router_client.cpp](../../../ipc/test/router_client.cpp) `append_record`). This is a demo artifact, not production logging — set up logrotate or systemd-tmpfiles for retention, or replace the peer binaries with your own that route logs through journald only.

## Cleanup behavior

### What `ExecStopPost=` removes

On clean stop (`systemctl stop rim-router.service`), [`rim-router-cleanup.sh`](rim-router-cleanup.sh) deletes:

- `/dev/shm/rim_router_*` — control-plane SHM regions
- `/dev/shm/rim_vision_*`, `/dev/shm/rim_ml_*` — sideband SHM regions (anticipated Phase F naming)
- `/tmp/rim_router_*.sock` — UDS sockets

### What it does NOT remove

- `/tmp/rim_router_*.log` — peer CSV logs (operator may want them post-crash)

### After a crash / kill -9

`ExecStopPost=` does not run on `SIGKILL`. To recover from an orphaned process, follow the recipe in [LESSONS-LEARNED.md](../../LESSONS-LEARNED.md):

```sh
sudo pkill -KILL -f "router_server"
sudo /opt/rim/libexec/rim-router-cleanup.sh
```

The shipped router unlinks-then-creates SHM regions on bind, so a fresh start after a hard kill still works without manual cleanup; the manual recipe is for "I want a clean slate before debugging."

## Known limitations

These are deliberately deferred from this phase — see the [post-phases robotics-integration review](../../plans/post-phases-robotics-review.md) for the full backlog.

| Limitation | Parked review item | Workaround today |
|---|---|---|
| `Type=simple`, not `Type=notify` — peers gated `After=` start before SHM regions are actually bound | [C6](../../plans/post-phases-robotics-review.md#c6--systemd-readiness-signaling-sd_notify--typenotify) | Peer code should retry first connect with backoff. `Restart=on-failure RestartSec=500ms` (in the peer unit) gives a coarse fallback. |
| Hardening directives (`MemoryLock=`, `CPUAffinity=`, `LimitRTPRIO=`) commented out by default | [C7](../../plans/post-phases-robotics-review.md#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo) | Uncomment + tune for your hardware. Router itself does not call `mlockall` / `sched_setscheduler` — these are user-side. |
| No CMake / `make install` — binaries must be `install -m 0755`'d by hand | [C10](../../plans/post-phases-robotics-review.md#c10--module-consumption-model) | Step 2 of the install recipe above. |
| `router_client` is a demo binary; real deployments substitute their own peer code | Phase F (F2–F5 sketches) | Treat the unit shape as the contract, replace `/opt/rim/bin/router_client` accordingly. |
| Sideband SHM region naming (`rim_vision_nv12`, `rim_ml_tensor_in`, etc.) is forward-declared but not produced by any peer in this tree | Phase F F5 | Cleanup script proactively `rm -f`'s the anticipated names so the future deployment is clean from day one. |

## Acceptance gates

Per [plans/E-robotics-integration.md](../../plans/E-robotics-integration.md):

- [ ] Fresh install: systemd units start router + 3 clients on bench hardware *(operator check, requires real hardware)*
- [ ] Shutdown leaves no stale `/dev/shm/rim_*` after clean stop *(verified by ExecStopPost + `shm_leak_check.sh`)*
- [x] `systemd-analyze verify deploy/systemd/*.service` passes
- [x] [`ipc/MODULE.md`](../../../ipc/MODULE.md) Related documents links the reference layout *(E1)*
