# Phase D3 — stress / soak scripts

Shell wrappers around the existing test binaries for soak, leak
detection, and CPU regression. Each script is self-contained, exits
non-zero on the first failure, and cleans `/dev/shm/rim_*` +
`/tmp/rim_*.sock` via a `trap` so it never leaves the host in a
worse state than it found it.

All scripts honor a NO-TTY mode (colour escapes are suppressed when
stdout isn't a terminal) and prefer `/usr/bin/env bash` so they work
under both the repo's bash and any custom toolchain.

## Inventory

| Script | Wrapper target | What it does |
|---|---|---|
| [`soak_router.sh`](soak_router.sh) | `make test-soak` (override with `SOAK_ITERATIONS=N`) | Loop `./build/ipc/test/router_test` N times (default 10), cleaning SHM/UDS leftovers between iterations, abort on first failure, print a per-iteration timing line, end with `summary: passed=N/N total=Xms mean=Yms min=A max=B` and a CSV-ish `iterations_ms:` line for graphing. |
| [`shm_leak_check.sh`](shm_leak_check.sh) | `make test-leak-check` | Count `rim_*` resources in `/dev/shm/` and `/tmp/*.sock` before and after `make test-ipc-unit && make test-ipc-integration && make test-router`; assert delta == 0. Auto-cleans pre-existing leftovers from interrupted prior runs so the baseline is honest. Set `LEAK_CHECK_SKIP_ROUTER=1` for hosts where AF_INET is sandboxed. |
| [`idle_cpu_check.sh`](idle_cpu_check.sh) | `make test-idle-cpu` | Boot `router_server --config config/profiles/jetson_prod.toml`, wait for `/dev/shm/rim_router_sensor` to appear (bind readiness), sample `pidstat -u -p $PID INTERVAL SAMPLES`, assert avg `%CPU` ≤ threshold via `bc -l`. Knobs: `IDLE_CPU_SAMPLES=6`, `IDLE_CPU_INTERVAL=10`, `IDLE_CPU_THRESHOLD=5.0` (≈ 60 s window). Regression gate for [ADR 0007](../../docs/adr/0007-router-idle-wake.md). |
| [`latency_histogram.sh`](latency_histogram.sh) | `make test-latency-histogram` | (Optional) Re-run `echo_tests` N times (default 5), parse `<TRANSPORT> round trips in 5s: <N>` lines, print `min / p25 / p50 / p75 / max / max-over-min` per transport. **Throughput** variance — not per-trip latency. Per-trip would require a `--per-trip-csv` flag on `echo_client_benchmark`, deferred to Phase E. |

## Acceptance gate

The Phase D3 deliverable is verified by running:

```bash
make test-leak-check                    # no leaks across full unit+integration+router pass
make test-idle-cpu                      # idle CPU <= 5% (post-fix baseline ~1.5–1.7%)
make test-soak SOAK_ITERATIONS=10       # 10 × router_test, clean
```

`make test-latency-histogram` is optional (informational; not a
pass/fail gate on absolute numbers).

## Why scripts, not C++ tests

The Phase A "no external test framework" rule applies to the module
library itself. Stress / soak / CPU gates are operational checks that:

  * **wall-clock loop** the existing binaries — duplicating that in
    a C++ test would just add another fork() layer;
  * **observe external state** (`/dev/shm/`, `/tmp/`, `pidstat`) that
    bash + standard Linux tools handle cleanly;
  * **run optionally** in CI / on a babysit hook, never on every
    `make test-ipc-unit`.

If a future check needs to be precise enough to live in C++ (e.g. a
per-trip latency histogram with kernel-side `clock_gettime` deltas), it
will graduate to a `*_test.cpp` under `ipc/test/` with an ADR.

## Manual cleanup / recovery

When a hung test (typically the D4 `fault_injection_test` SIGKILL
mid-traffic scenario, or any subprocess that didn't reach
`reap_bounded()`) leaves an orphan `router_server` alive, the normal
`kill -9 <pid>` from inside the cursor agent's shell will fail with
**EPERM** even though the orphan is owned by the same UID — the cursor
sandbox installs a session / cgroup boundary that Linux's UID-level
permission check sits *under*, and signal delivery is blocked at that
boundary even with the harness's `required_permissions: ["all"]`.

The recipe that works:

```bash
sudo pkill -KILL -f "build/ipc/test/router_server"
rm -f /dev/shm/rim_* /tmp/rim_*.sock
```

Anchor `-f` on the **binary path** (`build/ipc/test/router_server`),
not on the bare `router_server` substring — otherwise you'll also sweep
up unrelated `cursorsandbox` host-wrapper processes whose argv tail
contains the same string (see the `pgrep -f` lesson in
`robotics-ipc-module/LESSONS-LEARNED.md`).

Sanity-check afterwards:

```bash
pgrep -ax router_server          # should print nothing
ls /dev/shm/rim_* 2>&1    # should say "No such file or directory"
ls /tmp/rim_*.sock 2>&1   # same
```

After cleanup, `make test-leak-check` should pass with a zero baseline
and zero delta. If it doesn't, you have a new leak somewhere — start
by reading the post-run resource list the leak-check script prints.

## Adding a new script

  1. Start `set -euo pipefail` and decide on `IFS` early — if you take
     any space-separated env-var input, read it into an array
     **before** narrowing `IFS` (see `latency_histogram.sh`).
  2. Honour `[[ -t 1 ]]` for colour output.
  3. Install a `trap` that cleans `/dev/shm/rim_*` and
     `/tmp/rim_*.{sock,log}`.
  4. Use the SHM region (`/dev/shm/rim_router_sensor`) as the
     router-bind readiness signal, not a `sleep`.
  5. Print a single-line PASS / FAIL summary with the threshold and
     measured value visible. Exit non-zero on failure.
  6. Add a matching `test-<name>` phony target in the top-level
     `Makefile` so CI hooks can call it by `make` name, and link the
     script from this README.
