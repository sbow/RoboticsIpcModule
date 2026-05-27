#!/usr/bin/env bash
# Phase D3 — SHM / UDS leak detector.
#
# Counts cpp_tricks_* resources in /dev/shm/ and /tmp/ before and after
# a full unit + integration test pass. Asserts the delta is exactly
# zero — any non-zero delta indicates a destructor was skipped, an
# exception path didn't unlink, or a test exited via SIGKILL without
# cleanup.
#
# Test scope (matches the Phase D acceptance gate):
#   make test-ipc-unit
#   make test-ipc-integration
#   make test-router
#
# Files counted:
#   /dev/shm/cpp_tricks_*           — SHM SPSC regions
#   /tmp/cpp_tricks_*.sock          — UDS sockets
# Files excluded (test outputs, not leaks):
#   /tmp/cpp_tricks_*.log
#   /tmp/cpp_tricks_soak_*.log
#
# Usage:
#   bash robotics-ipc-module/scripts/shm_leak_check.sh
#
# Exit code: 0 if delta == 0; 1 on any leak.

set -euo pipefail
IFS=$'\n\t'

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

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

list_resources() {
    # Glob into a sorted list of paths. Suppress the "no match" output
    # by piping into ls -d with nullglob semantics emulated manually.
    {
        compgen -G '/dev/shm/cpp_tricks_*' || true
        compgen -G '/tmp/cpp_tricks_*.sock' || true
    } | sort
}

count_resources() {
    list_resources | wc -l
}

print_resources_diff() {
    local label="$1"
    local list="$2"
    if [[ -n "$list" ]]; then
        echo "${C_DIM}[leak-check] $label:${C_RESET}"
        printf '  %s\n' $list
    fi
}

# -----------------------------------------------------------------------
# 1. Snapshot pre-run state. If there are leftover resources from a prior
#    interrupted run, warn and clean — leak detection only works against
#    a known-clean baseline.
# -----------------------------------------------------------------------
pre_list=$(list_resources)
if [[ -n "$pre_list" ]]; then
    echo "${C_YELLOW}[leak-check] WARNING: pre-existing cpp_tricks_* resources detected; cleaning before run${C_RESET}"
    print_resources_diff "pre-existing" "$pre_list"
    rm -f /dev/shm/cpp_tricks_* 2>/dev/null || true
    rm -f /tmp/cpp_tricks_*.sock 2>/dev/null || true
fi

before=$(count_resources)
echo "${C_BOLD}[leak-check]${C_RESET} baseline cpp_tricks_* count: $before"

# -----------------------------------------------------------------------
# 2. Run the full test surface. test-router needs network namespace (UDP
#    sockets); some sandboxes block AF_INET on the host. We honor a
#    LEAK_CHECK_SKIP_ROUTER=1 escape hatch for those environments.
# -----------------------------------------------------------------------
echo "${C_DIM}[leak-check] running make test-ipc-unit...${C_RESET}"
if ! make -s test-ipc-unit >/tmp/cpp_tricks_leak_check.log 2>&1; then
    cat /tmp/cpp_tricks_leak_check.log >&2
    rm -f /tmp/cpp_tricks_leak_check.log
    echo "${C_RED}[leak-check] make test-ipc-unit failed${C_RESET}" >&2
    exit 1
fi

echo "${C_DIM}[leak-check] running make test-ipc-integration...${C_RESET}"
if ! make -s test-ipc-integration >>/tmp/cpp_tricks_leak_check.log 2>&1; then
    cat /tmp/cpp_tricks_leak_check.log >&2
    rm -f /tmp/cpp_tricks_leak_check.log
    echo "${C_RED}[leak-check] make test-ipc-integration failed${C_RESET}" >&2
    exit 1
fi

if [[ "${LEAK_CHECK_SKIP_ROUTER:-0}" != "1" ]]; then
    echo "${C_DIM}[leak-check] running make test-router...${C_RESET}"
    if ! make -s test-router >>/tmp/cpp_tricks_leak_check.log 2>&1; then
        cat /tmp/cpp_tricks_leak_check.log >&2
        rm -f /tmp/cpp_tricks_leak_check.log
        echo "${C_RED}[leak-check] make test-router failed${C_RESET}" >&2
        exit 1
    fi
else
    echo "${C_YELLOW}[leak-check] LEAK_CHECK_SKIP_ROUTER=1; skipping test-router${C_RESET}"
fi

rm -f /tmp/cpp_tricks_leak_check.log

# -----------------------------------------------------------------------
# 3. Post-run snapshot. Anything left over is a leak.
# -----------------------------------------------------------------------
after_list=$(list_resources)
after=$(printf '%s\n' "$after_list" | grep -c . || true)
echo "${C_BOLD}[leak-check]${C_RESET} post-run cpp_tricks_* count: $after"

delta=$(( after - before ))

if (( delta == 0 )); then
    echo "${C_GREEN}[leak-check] PASS — no leaked resources${C_RESET}"
    exit 0
fi

echo "${C_RED}[leak-check] FAIL — delta=$delta resources leaked${C_RESET}" >&2
print_resources_diff "leaked" "$after_list"
exit 1
