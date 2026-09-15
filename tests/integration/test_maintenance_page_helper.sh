#!/usr/bin/env bash






set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HELPER="$ROOT_DIR/ops/purecvisor-maintenance-page.sh"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-maintenance-helper.XXXXXX")"
trap 'rm -rf -- "$STATE"' EXIT

FALLBACK="$STATE/fallback"
FAKE_BIN="$STATE/bin"
LOG="$STATE/systemctl.log"
mkdir -p "$FALLBACK" "$FAKE_BIN"
: >"$LOG"



sed \
  -e "s#^FLAG=.*#FLAG=\"$FALLBACK/maintenance.enabled\"#" \
  -e "s#^STATUS=.*#STATUS=\"$FALLBACK/maintenance-status.json\"#" \
  "$HELPER" >"$STATE/helper.sh"
chmod 0700 "$STATE/helper.sh"

cat >"$FAKE_BIN/nginx" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
[[ "${FAKE_NGINX_OK:-1}" == "1" ]]
SH
cat >"$FAKE_BIN/systemctl" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FAKE_SYSTEMCTL_LOG"
SH
chmod 0700 "$FAKE_BIN/nginx" "$FAKE_BIN/systemctl"

run_helper() {
  PATH="$FAKE_BIN:$PATH" FAKE_SYSTEMCTL_LOG="$LOG" "$STATE/helper.sh" "$@"
}


if FAKE_NGINX_OK=0 run_helper on "30분" "API 제한" >/dev/null 2>&1; then
  echo "FAIL: nginx -t 실패가 maintenance on을 차단하지 않았다" >&2
  exit 1
fi
[[ ! -e "$FALLBACK/maintenance.enabled" && ! -e "$FALLBACK/maintenance-status.json" ]]
[[ ! -s "$LOG" ]]

ETA='45 "분" 후 재확인'
IMPACT=$'API는 읽기 전용입니다.\nVM 데이터는 보호됩니다.'
FAKE_NGINX_OK=1 run_helper on "$ETA" "$IMPACT" >/dev/null
[[ -f "$FALLBACK/maintenance.enabled" && -f "$FALLBACK/maintenance-status.json" ]]
[[ "$(stat -c '%a' "$FALLBACK/maintenance.enabled")" == "644" ]]
[[ "$(stat -c '%a' "$FALLBACK/maintenance-status.json")" == "644" ]]
python3 - "$FALLBACK/maintenance-status.json" "$ETA" "$IMPACT" <<'PY'
import json
import pathlib
import sys

payload = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
assert payload["state"] == "maintenance", payload
assert payload["eta"] == sys.argv[2], payload
assert payload["impact"] == sys.argv[3], payload
assert payload["updated_at"], payload
PY
grep -Fxq 'reload nginx' "$LOG"


before_sha="$(sha256sum "$FALLBACK/maintenance-status.json" | awk '{print $1}')"
if FAKE_NGINX_OK=0 run_helper off >/dev/null 2>&1; then
  echo "FAIL: nginx -t 실패가 maintenance off를 허용했다" >&2
  exit 1
fi
[[ -f "$FALLBACK/maintenance.enabled" ]]
[[ "$(sha256sum "$FALLBACK/maintenance-status.json" | awk '{print $1}')" == "$before_sha" ]]

FAKE_NGINX_OK=1 run_helper off >/dev/null
[[ ! -e "$FALLBACK/maintenance.enabled" ]]
[[ -f "$FALLBACK/maintenance-status.json" ]]
reload_count="$(grep -Fxc 'reload nginx' "$LOG")"
status_output="$(run_helper status)"
[[ "$status_output" == maintenance=off$'\n'* ]]
[[ "$(grep -Fxc 'reload nginx' "$LOG")" == "$reload_count" ]]

echo "PASS: maintenance page helper contract"
