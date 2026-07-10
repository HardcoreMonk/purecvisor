#!/usr/bin/env bash
set -euo pipefail

# Shell companion to scripts/check_design_md.py.
# This test verifies the docs/files that should point at DESIGN.md exist before
# invoking the Python checker for section/token-level validation.

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

require_file() {
  local file="$1"
  [[ -f "$ROOT/$file" ]] || fail "$file must exist"
}

require_literal() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if ! rg -Fq -- "$needle" "$ROOT/$file"; then
    printf 'FAIL: %s\n' "$message" >&2
    printf '  missing: %s in %s\n' "$needle" "$file" >&2
    exit 1
  fi
}

require_file "DESIGN.md"
require_file "scripts/check_design_md.py"
require_file "ui/samples/design-system-preview.html"

require_literal "DESIGN.md" "AGENTS.md" "AGENTS.md must require DESIGN.md before UI work"
require_literal "scripts/check_design_md.py" "AGENTS.md" "AGENTS.md must name the DESIGN.md checker"
require_literal "DESIGN.md" "docs/GUIDE.md" "GUIDE.md must link to the separated visual contract"
require_literal "ui/samples/design-system-preview.html" "docs/GUIDE.md" "GUIDE.md must link to the design preview"
require_literal "DESIGN.md" "ui/guide-content.md" "UI guide content must mention the separated visual contract"
require_literal "ui/samples/design-system-preview.html" "ui/guide-content.md" "UI guide content must link to the preview"
require_literal "ui/samples" "scripts/deploy.sh" "deploy script must publish linked UI samples"
require_literal "pcv_ui_samples" "scripts/deploy.sh" "deploy script must use a UI samples deploy staging directory"

require_literal "Color Tokens & Roles" "DESIGN.md" "DESIGN.md must define color token roles"
require_literal "Typography Rules" "DESIGN.md" "DESIGN.md must define typography rules"
require_literal "Component States" "DESIGN.md" "DESIGN.md must define component states"
require_literal "Dashboard Density" "DESIGN.md" "DESIGN.md must define dashboard density"
require_literal "Table Rules" "DESIGN.md" "DESIGN.md must define table rules"
require_literal "Card Rules" "DESIGN.md" "DESIGN.md must define card rules"
require_literal "Button Rules" "DESIGN.md" "DESIGN.md must define button rules"
require_literal "Modal Rules" "DESIGN.md" "DESIGN.md must define modal rules"
require_literal "Reference Pattern Borrowing" "DESIGN.md" "DESIGN.md must define selective reference borrowing"
require_literal "Linear" "DESIGN.md" "DESIGN.md must record Linear-inspired density guidance"
require_literal "Sentry" "DESIGN.md" "DESIGN.md must record Sentry-inspired triage guidance"
require_literal "IBM Carbon" "DESIGN.md" "DESIGN.md must record IBM Carbon-inspired table/form guidance"
require_literal "Raycast" "DESIGN.md" "DESIGN.md must record Raycast-inspired command guidance"
require_literal "--font-sans" "DESIGN.md" "DESIGN.md must define the sans typography token"
require_literal "Coolicons" "DESIGN.md" "DESIGN.md must define the local icon baseline"
require_literal "--accent" "DESIGN.md" "DESIGN.md must reference the runtime accent token"
require_literal ".hc" "DESIGN.md" "DESIGN.md must name the card shell class"
require_literal ".btn" "DESIGN.md" "DESIGN.md must name the button class"
require_literal ".modal" "DESIGN.md" "DESIGN.md must name the modal class"
require_literal "라벨 전체" "DESIGN.md" "DESIGN.md must require unclipped progress labels"
require_literal "ui/samples/design-system-preview.html" "DESIGN.md" "DESIGN.md must link to its preview"
require_literal 'class="pb-t">2.0%' "ui/samples/design-system-preview.html" "preview must include a low progress label"
require_file "ui/samples/design-borrowing-mockup.html"
require_literal "PureCVisor Operations Triage Mockup" "ui/samples/design-borrowing-mockup.html" "borrowed-pattern mockup must exist"
require_literal "../vendor/pretendard/pretendard.css" "ui/samples/design-borrowing-mockup.html" "borrowed-pattern mockup must load self-hosted Pretendard"
require_literal "../vendor/coolicons/coolicons.svg#ci-" "ui/samples/design-borrowing-mockup.html" "borrowed-pattern mockup must use local Coolicons"
require_literal 'href="../vendor/pretendard/pretendard.css"' "ui/samples/design-system-preview.html" "preview must load self-hosted Pretendard"
require_literal "../vendor/coolicons/coolicons.svg#ci-" "ui/samples/design-system-preview.html" "preview must demonstrate local Coolicons"
require_literal "ui/vendor/coolicons/coolicons.svg" "docs/GUIDE.md" "GUIDE.md must mention the local Coolicons asset"
require_literal "ui/vendor/coolicons/coolicons.svg" "ui/guide-content.md" "UI guide content must mention the local Coolicons asset"
require_literal "height: 18px" "ui/style.css" "progress track must fit label inside its border"
require_literal "white-space: nowrap" "ui/style.css" "progress label must not wrap or clip horizontally"
require_literal "line-height: 16px" "ui/style.css" "progress label line-height must fit the track"
require_literal "font-family: var(--font-sans)" "ui/style.css" "body must use the sans typography token"
require_literal "letter-spacing: 0" "ui/style.css" "body typography must keep default tracking at zero"

python3 "$ROOT/scripts/check_design_md.py"

printf 'DESIGN.md surface OK\n'
