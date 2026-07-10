#!/usr/bin/env bash
set -euo pipefail

# Source commenting standard gate.
# This test keeps the code-writing standard discoverable from the main developer
# entry points and pins the two-audience comment model requested for source work.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

require_literal() {
  local needle="$1"
  local file="$2"
  local label="$3"
  if ! grep -Fq "$needle" "$file"; then
    fail "$label"
  fi
}

STANDARD="docs/SOURCE_CODE_COMMENTING_STANDARD.md"

[[ -f "$STANDARD" ]] || fail "source commenting standard document must exist"

require_literal "주니어 개발자용 주석" "$STANDARD" "standard must define junior-developer comments"
require_literal "비개발자용 주석" "$STANDARD" "standard must define non-developer comments"
require_literal "Developer note" "$STANDARD" "standard must provide developer-note label"
require_literal "Operator note" "$STANDARD" "standard must provide operator-note label"
require_literal "신규 코드 완료 기준" "$STANDARD" "standard must define completion criteria"
require_literal "rg -n" "$STANDARD" "standard must include debt-marker scan guidance"

require_literal "SOURCE_CODE_COMMENTING_STANDARD.md" "AGENTS.md" "AGENTS must link source commenting standard"
require_literal "SOURCE_CODE_COMMENTING_STANDARD.md" "docs/DEVELOPER_INDEX.md" "developer index must link source commenting standard"
require_literal "SOURCE_CODE_COMMENTING_STANDARD.md" "docs/DEVELOPMENT_VERIFICATION_POLICY.md" "verification policy must link source commenting standard"
require_literal "SOURCE_CODE_COMMENTING_STANDARD.md" "docs/SOURCE_LOGIC_STEP_BY_STEP_GUIDE.md" "source logic guide must point to source commenting standard"

printf 'PASS: source commenting standard is linked and complete\n'
