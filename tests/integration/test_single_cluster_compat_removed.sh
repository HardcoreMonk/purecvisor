#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MAKEFILE="$ROOT/Makefile"

if rg -Fq "pcv_single_cluster_compat.c" "$MAKEFILE"; then
  printf 'FAIL: Makefile still links pcv_single_cluster_compat.c\n' >&2
  exit 1
fi

printf 'PASS: Makefile no longer links pcv_single_cluster_compat.c\n'
