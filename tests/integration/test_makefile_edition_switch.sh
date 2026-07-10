#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

make clean >/dev/null
make single >/dev/null
if make multi >/dev/null 2>&1; then
  printf 'FAIL: purecvisor-single must reject make multi\n' >&2
  exit 1
fi

printf 'PASS: single build succeeds and multi build is rejected\n'
