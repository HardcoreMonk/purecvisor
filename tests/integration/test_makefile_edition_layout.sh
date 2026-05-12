#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MAKEFILE="$ROOT/Makefile"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

require_literal() {
  local needle="$1"
  local label="$2"
  if ! rg -Fq "$needle" "$MAKEFILE"; then
    fail "$label"
  fi
}




require_literal "DAEMON_COMMON_SRCS =" "Makefile must declare DAEMON_COMMON_SRCS"
require_literal "COMMON_SINGLE_ALLOWED_NET_SRCS =" "Makefile must declare COMMON_SINGLE_ALLOWED_NET_SRCS"
require_literal "SINGLE_BOOTSTRAP_SRCS =" "Makefile must declare SINGLE_BOOTSTRAP_SRCS"
require_literal "TEST_COMMON_SRCS =" "Makefile must declare TEST_COMMON_SRCS"
require_literal "SINGLE_TEST_SRCS =" "Makefile must declare SINGLE_TEST_SRCS"
require_literal 'DAEMON_SRCS = src/main.c $(DAEMON_COMMON_SRCS) $(SINGLE_BOOTSTRAP_SRCS)' \
  "Makefile must compose the daemon from common and single-only sources"
require_literal '    $(SINGLE_TEST_SRCS)' "Makefile test runner must include single-only test sources"

if rg -q "^(MULTI_DAEMON_SRCS|MULTI_TEST_SRCS)\\s*=" "$MAKEFILE"; then
  fail "Single public repository must not declare Multi Edge source sets"
fi

printf 'PASS: Makefile Single Edge source layout variables found\n'
