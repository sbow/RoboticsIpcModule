#!/usr/bin/env bash
# Install robotics IPC plan skills into .cursor/skills/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${1:-.cursor/skills}"

mkdir -p "$DEST"

for skill in "$ROOT"/.cursor/skills/*/; do
  name="$(basename "$skill")"
  rm -rf "$DEST/$name"
  cp -a "$skill" "$DEST/$name"
  echo "installed: $DEST/$name"
done

echo "Done."
echo "  Open: robotics-ipc-module/AGENTS.md"
echo "  Or invoke: @ipc-robotics-orchestrator"
