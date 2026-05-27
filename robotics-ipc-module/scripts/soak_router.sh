#!/usr/bin/env bash
# Phase D3 — router soak.
#
# Loop `make test-router` N times. Abort on first failure. Emit a per-
# iteration timing line and a final summary (mean / min / max / total).
# Cleans /dev/shm/rim_* and /tmp/rim_*.sock between
# iterations so a flake in run K can't cascade into run K+1.
#
# Usage:
#   bash robotics-ipc-module/scripts/soak_router.sh        # 10 iterations
#   bash robotics-ipc-module/scripts/soak_router.sh 50
#
# Exit code: 0 if every iteration passed; 1 on the first failure.

set -euo pipefail
IFS=$'\n\t'

readonly N="${1:-10}"
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# Color helpers — only emit escape codes on a TTY so CI logs stay clean.
if [[ -t 1 ]]; then
    readonly C_RESET=$'\033[0m'
    readonly C_GREEN=$'\033[32m'
    readonly C_RED=$'\033[31m'
    readonly C_DIM=$'\033[2m'
    readonly C_BOLD=$'\033[1m'
else
    readonly C_RESET= C_GREEN= C_RED= C_DIM= C_BOLD=
fi

cleanup_paths() {
    # SHM regions live under /dev/shm/. UDS sockets and test logs live
    # under /tmp/. We intentionally leave the build/ tree alone — soak
    # is about runtime artefacts, not rebuilds.
    rm -f /dev/shm/rim_* 2>/dev/null || true
    rm -f /tmp/rim_*.sock /tmp/rim_*.log 2>/dev/null || true
}

trap cleanup_paths EXIT

# Pre-flight: build once. test-router rebuilds when sources change but
# we want the build cost outside the per-iteration timing.
if [[ ! -x build/ipc/test/router_test ]]; then
    echo "${C_DIM}[soak] building router_test...${C_RESET}"
    make -s test-router >/dev/null || {
        echo "${C_RED}[soak] initial build failed${C_RESET}" >&2
        exit 1
    }
fi

declare -i passed=0
declare -a per_iter_ms=()
declare -i min_ms=2147483647 max_ms=0 total_ms=0

echo "${C_BOLD}[soak] router soak: $N iterations starting${C_RESET}"

for i in $(seq 1 "$N"); do
    cleanup_paths

    # Time the iteration with monotonic-ish millis (date +%s%3N is GNU
    # date; safe on Linux).
    start_ms=$(date +%s%3N)
    if ./build/ipc/test/router_test >/tmp/rim_soak_iter.log 2>&1; then
        end_ms=$(date +%s%3N)
        iter_ms=$((end_ms - start_ms))
        passed+=1
        total_ms+=$iter_ms
        (( iter_ms < min_ms )) && min_ms=$iter_ms
        (( iter_ms > max_ms )) && max_ms=$iter_ms
        per_iter_ms+=("$iter_ms")
        printf '[soak] iter %2d/%d  %selapsed=%sms PASS%s\n' \
            "$i" "$N" "$C_GREEN" "$iter_ms" "$C_RESET"
    else
        end_ms=$(date +%s%3N)
        iter_ms=$((end_ms - start_ms))
        printf '[soak] iter %2d/%d  %selapsed=%sms FAIL%s\n' \
            "$i" "$N" "$C_RED" "$iter_ms" "$C_RESET" >&2
        echo "${C_DIM}--- router_test output ---${C_RESET}" >&2
        cat /tmp/rim_soak_iter.log >&2 || true
        echo "${C_DIM}--- end output ---${C_RESET}" >&2
        echo "${C_RED}[soak] aborting after $(( i - 1 )) successful iterations${C_RESET}" >&2
        exit 1
    fi
done

mean_ms=$(( total_ms / passed ))

printf '%s[soak] summary:%s passed=%d/%d  total=%dms  mean=%dms  min=%dms  max=%dms\n' \
    "$C_BOLD" "$C_RESET" "$passed" "$N" "$total_ms" "$mean_ms" "$min_ms" "$max_ms"

# Print the per-iteration distribution as a compact CSV line for graphing.
printf '%s[soak] iterations_ms:%s ' "$C_DIM" "$C_RESET"
printf '%s ' "${per_iter_ms[@]}"
printf '\n'
