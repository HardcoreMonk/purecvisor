#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INDEX="$ROOT/ui/index.html"
STYLE="$ROOT/ui/style.css"
API_JS="$ROOT/ui/modules/api.js"
APP_JS="$ROOT/ui/app.js"

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

reject_literal() {
  local needle="$1"
  local file="$2"
  local label="$3"
  if rg -Fq "$needle" "$file"; then
    fail "$label"
  fi
}

# The unauthenticated homepage must sell the operational surface, not only the
# login form. These strings/classes are intentionally static because they are
# public pre-login copy.
require_literal "login-console-preview" "$INDEX" "login page must include an operational console preview"
require_literal "login-mobile-proof" "$INDEX" "mobile login page must keep a compact operational proof near the hero"
require_literal "Node pcv-edge-1" "$INDEX" "console preview must show the single edge node context"
require_literal "VM 관리" "$INDEX" "login surface cards must include VM management"
require_literal "LXC 컨테이너" "$INDEX" "login surface cards must include LXC containers"
require_literal "ZFS 스토리지" "$INDEX" "login surface cards must include ZFS storage"
require_literal "Jobs · Audit" "$INDEX" "login surface cards must include jobs and audit"
require_literal "REST API" "$INDEX" "login surface cards must include REST API"
require_literal "로그인 실패와 세션 변경은 audit 기록에 남습니다." "$INDEX" "login form must state audit behavior"
reject_literal 'placeholder="admin"' "$INDEX" "login form must not suggest a default admin username"
require_literal 'id="login-user" class="login-input" placeholder="계정 이름" autocomplete="username" autocapitalize="none" autocorrect="off" spellcheck="false"' "$INDEX" "username input must disable mobile autocorrect/autocapitalize"
require_literal 'id="login-pass" type="password" class="login-input" placeholder="••••••••" autocomplete="current-password" autocapitalize="none" autocorrect="off" spellcheck="false"' "$INDEX" "password input must disable mobile autocorrect/autocapitalize"
require_literal "document.getElementById('login-user')?.value.trim()" "$API_JS" "login code must trim username copy/paste whitespace"

# Mobile must not expose authenticated navigation while the login overlay is
# visible.
require_literal "body.login-active .mobile-nav" "$STYLE" "CSS must hide mobile nav while login is active"
require_literal ".login-mobile-proof" "$STYLE" "CSS must style the compact mobile proof strip"
require_literal "pcvSetLoginVisible" "$API_JS" "auth module must toggle login-active state"
require_literal "login-tls-compact" "$APP_JS" "TLS status must use compact mobile-friendly markup"

printf 'PASS: login homepage surface contract found\n'
