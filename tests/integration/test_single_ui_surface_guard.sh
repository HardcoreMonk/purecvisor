#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP_JS="$ROOT/ui/app.js"
NAV_JS="$ROOT/ui/modules/nav.js"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

require_literal() {
  local needle="$1"
  local file="$2"
  local label="$3"
  if ! rg -Fq "$needle" "$file"; then
    fail "$label"
  fi
}

# Single Edge에서는 멀티 전용 화면이 메뉴/라우트 양쪽에서 차단돼야 한다.
require_literal "'cluster'" "$APP_JS" "PCV_CLUSTER_ONLY_NAV must include cluster"
require_literal "'mon-cluster'" "$APP_JS" "PCV_CLUSTER_ONLY_NAV must include mon-cluster"
require_literal "'federation'" "$APP_JS" "PCV_CLUSTER_ONLY_NAV must include federation"
require_literal "window.pcvClusterEnabled === false && window.PCV_CLUSTER_ONLY_NAV && window.PCV_CLUSTER_ONLY_NAV.includes(n)" "$NAV_JS" \
  "navigateTo must reject multi-only pages on single edge"

printf 'PASS: single-edge UI surface guard patterns found\n'
