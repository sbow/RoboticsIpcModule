#!/usr/bin/env bash
# Phase D3 — idle CPU regression gate.
#
# Re-baselines the Phase C2 measurement on the current revision:
# start `router_server --config jetson_prod.toml`, let it bind, then
# sample its CPU usage with pidstat for `SAMPLES * INTERVAL` seconds.
# Asserts the average idle CPU is <= 5% of one core.
#
# This is the regression gate for ADR 0007 (router idle wake). The
# Phase C baseline before the fix was **100% of one core** — using
# yield() to "idle" actually spun. The post-fix measurement was 1.6%.
# A 5% ceiling leaves enough margin for kernel scheduler jitter and
# noisy hosts while still catching a real regression.
#
# Usage:
#   bash robotics-ipc-module/scripts/idle_cpu_check.sh
#
# Environment knobs:
#   IDLE_CPU_SAMPLES   default 6   — number of pidstat samples
#   IDLE_CPU_INTERVAL  default 10  — seconds per sample (60s total default)
#   IDLE_CPU_THRESHOLD default 5.0 — pass ceiling (percent of one core)
#
# Exit code: 0 if avg CPU <= threshold; 1 otherwise (or on setup error).

set -euo pipefail
IFS=$'\n\t'

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

readonly SAMPLES="${IDLE_CPU_SAMPLES:-6}"
readonly INTERVAL="${IDLE_CPU_INTERVAL:-10}"
readonly THRESHOLD="${IDLE_CPU_THRESHOLD:-5.0}"

if [[ -t 1 ]]; then
    readonly C_RESET=$'\033[0m'
    readonly C_GREEN=$'\033[32m'
    readonly C_RED=$'\033[31m'
    readonly C_YELLOW=$'\033[33m'
    readonly C_DIM=$'\033[2m'
    readonly C_BOLD=$'\033[1m'
else
    readonly C_RESET= C_GREEN= C_RED= C_YELLOW= C_DIM= C_BOLD=
fi

if ! command -v pidstat >/dev/null 2>&1; then
    echo "${C_RED}[idle-cpu] pidstat not found (apt: sysstat). Install or skip this check.${C_RESET}" >&2
    exit 1
fi

readonly ROUTER_BIN="build/ipc/test/router_server"
readonly PROFILE="config/profiles/jetson_prod.toml"

if [[ ! -x "$ROUTER_BIN" ]]; then
    echo "${C_DIM}[idle-cpu] $ROUTER_BIN missing; building...${C_RESET}"
    make -s ipc-router-server >/dev/null
fi

cleanup_shm() {
    rm -f /dev/shm/rim_* 2>/dev/null || true
}
cleanup_shm

readonly LOG=/tmp/rim_idle_cpu.log
: >"$LOG"

# Launch the router in the background. ROUTER_TEST must NOT be set —
# we want the router to stay alive for the full sample window.
unset ROUTER_TEST
"$ROUTER_BIN" --config "$PROFILE" >>"$LOG" 2>&1 &
ROUTER_PID=$!

terminate_router() {
    if [[ -n "${ROUTER_PID:-}" ]] && kill -0 "$ROUTER_PID" 2>/dev/null; then
        kill -TERM "$ROUTER_PID" 2>/dev/null || true
        # Give the destructor a chance to shm_unlink.
        local i
        for i in 1 2 3 4 5; do
            if ! kill -0 "$ROUTER_PID" 2>/dev/null; then break; fi
            sleep 0.1
        done
        kill -KILL "$ROUTER_PID" 2>/dev/null || true
        wait "$ROUTER_PID" 2>/dev/null || true
    fi
    cleanup_shm
}
trap terminate_router EXIT

# Wait until the router has actually bound (SHM region appears) before
# starting the measurement window. Bound by a 3 s deadline.
deadline=$(( $(date +%s) + 3 ))
until [[ -e /dev/shm/rim_router_sensor ]]; do
    if (( $(date +%s) >= deadline )); then
        echo "${C_RED}[idle-cpu] router did not bind within 3 s; aborting${C_RESET}" >&2
        cat "$LOG" >&2 || true
        exit 1
    fi
    sleep 0.05
done

printf '%s[idle-cpu]%s sampling pid=%d for %ds × %d samples (~%ds total)\n' \
    "$C_BOLD" "$C_RESET" "$ROUTER_PID" "$INTERVAL" "$SAMPLES" "$(( INTERVAL * SAMPLES ))"

# pidstat -u prints %usr %system %guest %wait %CPU. We want %CPU
# (the last column before "Command"). Column layout differs slightly
# between sysstat versions, so we anchor on the "Average:" line and
# pick the field labelled "%CPU" using awk header introspection.
pidstat_output=$(LC_ALL=C pidstat -u -p "$ROUTER_PID" "$INTERVAL" "$SAMPLES" 2>&1 || true)

if [[ -z "$pidstat_output" ]] || ! grep -q '^Average' <<<"$pidstat_output"; then
    echo "${C_RED}[idle-cpu] pidstat returned no usable data:${C_RESET}" >&2
    printf '%s\n' "$pidstat_output" >&2
    exit 1
fi

# Extract %CPU from the Average line by header position.
avg_cpu=$(awk '
    /^#/             { next }
    /%CPU/ && /^[# ]*Time/ {
        for (i = 1; i <= NF; ++i) {
            if ($i == "%CPU") cpu_col = i
        }
    }
    /^Average:/ {
        if (cpu_col) {
            print $cpu_col
        } else {
            # Fallback for older sysstat: %CPU is the 8th field on the
            # Average line when started with "Average:  UID  PID  %usr ..."
            print $8
        }
    }
' <<<"$pidstat_output")

if [[ -z "$avg_cpu" ]]; then
    echo "${C_RED}[idle-cpu] could not parse %CPU from pidstat output:${C_RESET}" >&2
    printf '%s\n' "$pidstat_output" >&2
    exit 1
fi

# bc -l for float comparison; 0/1 result.
within=$(printf '%s\n' "$avg_cpu <= $THRESHOLD" | bc -l)

printf '%s[idle-cpu]%s avg=%s%%  threshold=%s%%  samples=%d  interval=%ss\n' \
    "$C_BOLD" "$C_RESET" "$avg_cpu" "$THRESHOLD" "$SAMPLES" "$INTERVAL"

# Compact dump of every sample for trend-spotting.
echo "${C_DIM}[idle-cpu] per-sample %CPU:${C_RESET}"
echo "$pidstat_output" \
    | awk '/^[0-9]/ { for (i = 1; i <= NF; ++i) if ($i == "%CPU") cpu_col = i }
           NR == FNR && cpu_col {
               # nothing; just figuring out the column
           }
           { print }' \
    | grep -E '^[0-9]|^Average' | sed 's/^/    /'

if [[ "$within" == "1" ]]; then
    echo "${C_GREEN}[idle-cpu] PASS — idle CPU within ${THRESHOLD}% bar${C_RESET}"
    exit 0
fi

echo "${C_RED}[idle-cpu] FAIL — idle CPU ${avg_cpu}% > ${THRESHOLD}% threshold${C_RESET}" >&2
echo "${C_YELLOW}[idle-cpu] regression candidate: RouterRunOptions::idle_sleep_us (ADR 0007)${C_RESET}" >&2
exit 1
