#!/usr/bin/env bash
set -euo pipefail

test -f docs/PUBLIC_SOURCE_POLICY.md
test -x scripts/strip_source_comments.py
python3 scripts/strip_source_comments.py --check
node scripts/check_javascript_comments.mjs
test ! -e ui/app.bundle.js.map
rg -q "PCV_UI_SOURCE_SHA1" scripts/check_ui_bundle_fresh.py
printf '%s\n' "PASS: public source comment policy"
