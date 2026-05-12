#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAIN_SRC="$ROOT_DIR/src/main.c"
VIRT_EVENTS_SRC="$ROOT_DIR/src/modules/daemons/virt_events.c"

if grep -n 'PCV_LOG_INFO(SIG_PROBE_DOM' "$MAIN_SRC" >/tmp/pcv-signal-probe-main-hit.$$; then
  printf 'FAIL: signal_probe logs in src/main.c still use INFO level\n' >&2
  cat /tmp/pcv-signal-probe-main-hit.$$ >&2
  rm -f /tmp/pcv-signal-probe-main-hit.$$
  exit 1
fi
rm -f /tmp/pcv-signal-probe-main-hit.$$

if grep -n 'g_log("signal_probe", G_LOG_LEVEL_INFO' "$VIRT_EVENTS_SRC" >/tmp/pcv-signal-probe-virt-hit.$$; then
  printf 'FAIL: virt_events signal_probe logs still use INFO level\n' >&2
  cat /tmp/pcv-signal-probe-virt-hit.$$ >&2
  rm -f /tmp/pcv-signal-probe-virt-hit.$$
  exit 1
fi
rm -f /tmp/pcv-signal-probe-virt-hit.$$

printf 'PASS: signal_probe logging no longer uses INFO level in runtime paths\n'
