#!/usr/bin/env bash
# Phase D3 — throughput variance probe (optional).
#
# Wraps `echo_client_benchmark` across N short runs per transport,
# computes the throughput distribution (round trips per 5 s window),
# and emits min / median / max / IQR. This is **throughput** variance,
# not per-trip latency — the benchmark binary today reports a
# count-over-duration, not per-trip timings. Per-trip histograms are a
# Phase E ask (would require adding a `--per-trip-csv` flag to the
# benchmark or wiring a separate timing driver); the variance probe is
# the closest signal we can get without that work.
#
# Reading the output:
#   * High variance across runs (max/min ratio > ~1.3) suggests CPU
#     contention or interrupt jitter — worth investigating.
#   * A regression in median throughput vs the values in MODULE.md
#     "Build & verify" indicates a real perf regression.
#
# Usage:
#   bash robotics-ipc-module/scripts/latency_histogram.sh        # 5 runs / transport
#   bash robotics-ipc-module/scripts/latency_histogram.sh 10
#
# Environment knobs:
#   TRANSPORTS  default "shm uds udp" — space-separated list to probe
#
# Exit code: 0 if every run succeeded; 1 if any benchmark invocation
# failed. This script is **not** a pass/fail gate on absolute numbers
# (perf depends on hardware); use it as an investigative tool.

set -euo pipefail
# Note: TRANSPORTS is intentionally space-separated, so we keep the
# default IFS for word-splitting on that variable. We re-tighten IFS
# only inside loops that read pidstat-style output.

readonly RUNS="${1:-5}"
readonly TRANSPORTS="${TRANSPORTS:-shm uds udp}"
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# Split TRANSPORTS into an array now so later loops are robust even
# under stricter IFS settings.
read -r -a TRANSPORT_LIST <<<"$TRANSPORTS"

if [[ -t 1 ]]; then
    readonly C_RESET=$'\033[0m'
    readonly C_GREEN=$'\033[32m'
    readonly C_RED=$'\033[31m'
    readonly C_DIM=$'\033[2m'
    readonly C_BOLD=$'\033[1m'
else
    readonly C_RESET= C_GREEN= C_RED= C_DIM= C_BOLD=
fi

readonly ECHO_BIN="build/ipc/test/echo_tests"

if [[ ! -x "$ECHO_BIN" ]]; then
    echo "${C_DIM}[latency] $ECHO_BIN missing; building...${C_RESET}"
    make -s test-ipc >/dev/null || true
fi

# echo_tests runs all three transports in a single invocation. To
# probe one at a time we'd need a per-transport binary; for the
# variance probe we just re-run the whole suite N times and split the
# count lines per transport.
#
# echo_tests output (one line per transport):
#   UDP round trips in 5s: 724357
#   UDS round trips in 5s: 801631
#   SHM round trips in 5s: 7728488

declare -A counts_by_transport
for t in "${TRANSPORT_LIST[@]}"; do
    counts_by_transport["$t"]=""
done

printf '%s[latency]%s variance probe — %d runs of echo_tests (~%ds each)\n' \
    "$C_BOLD" "$C_RESET" "$RUNS" "$(( 5 * 3 ))"

for run in $(seq 1 "$RUNS"); do
    echo "${C_DIM}[latency] run $run/$RUNS${C_RESET}"
    out=$("$ECHO_BIN" 2>&1) || {
        echo "${C_RED}[latency] echo_tests failed on run $run${C_RESET}" >&2
        printf '%s\n' "$out" >&2
        exit 1
    }
    while IFS= read -r line; do
        # Match e.g. "UDP round trips in 5s: 724357"
        if [[ "$line" =~ ^([A-Z]+)\ round\ trips\ in\ [0-9]+s:\ ([0-9]+) ]]; then
            transport="${BASH_REMATCH[1],,}"   # lowercase
            count="${BASH_REMATCH[2]}"
            if [[ -n "${counts_by_transport[$transport]+set}" ]]; then
                counts_by_transport["$transport"]+="$count "
            fi
        fi
    done <<<"$out"
done

quantile() {
    # quantile <pct> <sorted space-separated list>
    # Returns the value at percentile pct (0..100) using nearest-rank.
    local pct="$1"
    shift
    local -a v=("$@")
    local n=${#v[@]}
    if (( n == 0 )); then
        echo "0"
        return
    fi
    # idx = ceil(pct/100 * n) - 1, clamped to [0, n-1]
    local idx
    idx=$(awk -v p="$pct" -v n="$n" 'BEGIN{
        r = (p / 100.0) * n;
        i = int(r);
        if (r > i) i++;
        if (i > 0) i--;
        if (i < 0) i = 0;
        if (i >= n) i = n - 1;
        print i
    }')
    echo "${v[$idx]}"
}

format_row() {
    # format_row <transport> <space-separated counts>
    local transport="$1"
    shift
    local raw=("$@")
    if [[ ${#raw[@]} -eq 0 ]]; then
        printf '  %-4s | (no data)\n' "${transport^^}"
        return
    fi
    # Sort ascending.
    local -a sorted
    mapfile -t sorted < <(printf '%s\n' "${raw[@]}" | sort -n)
    local min="${sorted[0]}"
    local max="${sorted[-1]}"
    local p50 p25 p75
    p25=$(quantile 25 "${sorted[@]}")
    p50=$(quantile 50 "${sorted[@]}")
    p75=$(quantile 75 "${sorted[@]}")
    local ratio
    ratio=$(awk -v a="$max" -v b="$min" 'BEGIN { if (b > 0) printf "%.2f", a / b; else print "n/a" }')
    printf '  %-4s | min=%9d  p25=%9d  p50=%9d  p75=%9d  max=%9d  max/min=%s\n' \
        "${transport^^}" "$min" "$p25" "$p50" "$p75" "$max" "$ratio"
}

echo
printf '%s[latency] throughput distribution (round trips per 5 s)%s\n' "$C_BOLD" "$C_RESET"
printf '  %s\n' "(higher is better; max/min > ~1.3 suggests host jitter)"
for t in "${TRANSPORT_LIST[@]}"; do
    # shellcheck disable=SC2206
    runs_for_t=( ${counts_by_transport[$t]} )
    format_row "$t" "${runs_for_t[@]}"
done

echo "${C_GREEN}[latency] probe complete${C_RESET}"
