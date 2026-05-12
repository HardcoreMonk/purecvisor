#!/usr/bin/env bash
set -euo pipefail

FLAG="/usr/local/share/purecvisor/fallback/maintenance.enabled"
STATUS="/usr/local/share/purecvisor/fallback/maintenance-status.json"

usage() {
  echo "usage: $0 on|off|status [eta] [impact]" >&2
}

cmd="${1:-}"
case "$cmd" in
  on)
    eta="${2:-확인 중}"
    impact="${3:-웹 콘솔 및 API 접속이 일시적으로 제한됩니다.}"
    updated_at="$(date '+%Y-%m-%d %H:%M:%S %Z')"
    cat >"$STATUS" <<JSON
{
  "state": "maintenance",
  "title": "서비스 점검 중",
  "impact": "$impact",
  "data_status": "고객 VM과 스토리지 데이터 보호를 우선으로 확인 중입니다.",
  "eta": "$eta",
  "updated_at": "$updated_at",
  "support": "support@purecvisor.example.com"
}
JSON
    touch "$FLAG"
    ;;
  off)
    rm -f "$FLAG"
    ;;
  status)
    if [ -f "$FLAG" ]; then
      echo "maintenance=on"
    else
      echo "maintenance=off"
    fi
    [ -f "$STATUS" ] && cat "$STATUS"
    ;;
  *)
    usage
    exit 2
    ;;
esac

nginx -t >/dev/null
systemctl reload nginx
