#!/usr/bin/env bash
# Run router integration test N times (Phase D3).
set -euo pipefail

N="${1:-10}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ ! -x build/ipc/test/router_test ]]; then
  make test-router
fi

for i in $(seq 1 "$N"); do
  echo "soak iteration $i/$N"
  ./build/ipc/test/router_test || exit 1
done

echo "soak_router: $N iterations passed"
