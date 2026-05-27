#!/bin/sh
# rim-router-cleanup.sh -- ExecStopPost helper for rim-router.service
#
# Removes leftover SHM regions and UDS sockets from a prior router run.
# Called by systemd AFTER the main process has exited. Idempotent and
# safe to invoke even when nothing is leftover. Failure to delete is
# non-fatal (rm -f ignores absent files; '|| true' swallows globbing
# misses on shells without nullglob).
#
# The router itself unlinks-then-creates SHM regions on every bind,
# so this script is not strictly required for restart correctness.
# Its job is to clean up after an operator-initiated stop (profile
# swap, upgrade, reboot) so the next start sees a tidy /dev/shm.
#
# Peer log files at /tmp/rim_router_*.log are intentionally NOT
# removed -- operator may want to inspect them post-crash. Set up
# logrotate or systemd-tmpfiles for retention policy.
#
# Install:  /opt/rim/libexec/rim-router-cleanup.sh   (chmod 0755)
# Caller:   rim-router.service  ExecStopPost=

set -u

# Shared-memory regions (router + per-peer rings, sideband descriptors).
rm -f /dev/shm/rim_router_*  2>/dev/null || true
rm -f /dev/shm/rim_vision_*  2>/dev/null || true
rm -f /dev/shm/rim_ml_*      2>/dev/null || true

# UDS sockets (x86_dev profile + any UDS sidebands an operator added).
rm -f /tmp/rim_router_*.sock 2>/dev/null || true

exit 0
