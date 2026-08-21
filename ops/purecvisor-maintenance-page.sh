#!/usr/bin/env bash
                          
                                                                
                                                            
                                                                  
                                                               
                                                           
                                                        
                                                         
 
                      
                                                
                                                      
                                                 
set -euo pipefail

FLAG="/usr/local/share/purecvisor/fallback/maintenance.enabled"
STATUS="/usr/local/share/purecvisor/fallback/maintenance-status.json"

usage() {
  echo "usage: $0 on|off|status [eta] [impact]" >&2
}

cmd="${1:-}"
case "$cmd" in
  status)
    if [ -f "$FLAG" ]; then
      echo "maintenance=on"
    else
      echo "maintenance=off"
    fi
    [ -f "$STATUS" ] && cat "$STATUS"
    exit 0
    ;;
  on)
    eta="${2:-확인 중}"
    impact="${3:-웹 콘솔 및 API 접속이 일시적으로 제한됩니다.}"
    updated_at="$(date '+%Y-%m-%d %H:%M:%S %Z')"
    ;;
  off)
    ;;
  *)
    usage
    exit 2
    ;;
esac

                                                     
                                                          
nginx -t >/dev/null

if [[ "$cmd" == "on" ]]; then
  command -v python3 >/dev/null 2>&1 \
    || { echo "python3 is required to encode maintenance JSON" >&2; exit 1; }
  status_dir="$(dirname -- "$STATUS")"
  flag_dir="$(dirname -- "$FLAG")"
  install -d -m 0755 -- "$status_dir" "$flag_dir"

  status_tmp="$(mktemp "$status_dir/.maintenance-status.XXXXXX")"
  flag_tmp=""
  cleanup() {
    [[ -z "$status_tmp" ]] || rm -f -- "$status_tmp"
    [[ -z "$flag_tmp" ]] || rm -f -- "$flag_tmp"
  }
  trap cleanup EXIT

  python3 - "$eta" "$impact" "$updated_at" >"$status_tmp" <<'PY'
import json
import sys

eta, impact, updated_at = sys.argv[1:]
json.dump(
    {
        "state": "maintenance",
        "title": "서비스 점검 중",
        "impact": impact,
        "data_status": "고객 VM과 스토리지 데이터 보호를 우선으로 확인 중입니다.",
        "eta": eta,
        "updated_at": updated_at,
        "support": "support@purecvisor.example.com",
    },
    sys.stdout,
    ensure_ascii=False,
    indent=2,
)
sys.stdout.write("\n")
PY
  chmod 0644 "$status_tmp"
  mv -fT -- "$status_tmp" "$STATUS"
  status_tmp=""

                                                          
                                               
  flag_tmp="$(mktemp "$flag_dir/.maintenance-enabled.XXXXXX")"
  chmod 0644 "$flag_tmp"
  mv -fT -- "$flag_tmp" "$FLAG"
  flag_tmp=""
else
  rm -f -- "$FLAG"
fi

systemctl reload nginx
