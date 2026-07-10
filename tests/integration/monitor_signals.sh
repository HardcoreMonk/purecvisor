#!/usr/bin/env bash
# tests/integration/monitor_signals.sh
#
# 실행 중인 daemon 의 signal_probe 이벤트를 실시간으로 모니터링합니다.
#
# 사용법:
#   # 터미널 A — daemon 실행
#   sudo ./purecvisor_daemon 2>&1 | tee /tmp/pcv.log
#
#   # 터미널 B — 신호 모니터링
#   sudo bash tests/integration/monitor_signals.sh /tmp/pcv.log
#
#   # 터미널 C — VM 조작
#   sudo ./purecvisorctl vm start myvm

LOG_FILE="${1:-/tmp/purecvisor_integration_test.log}"
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}== PureCVisor Signal Monitor (Ctrl+C 로 종료) ==${NC}"
echo -e "   로그 파일: ${LOG_FILE}"
echo ""

tail -f "$LOG_FILE" 2>/dev/null | while IFS= read -r line; do
    if echo "$line" | grep -q "vm-started RECEIVED"; then
        echo -e "${GREEN}[SIGNAL] vm-started  ${NC}$(echo "$line" | grep -o "vm_name='[^']*'")"
    elif echo "$line" | grep -q "vm-stopped RECEIVED"; then
        echo -e "${YELLOW}[SIGNAL] vm-stopped  ${NC}$(echo "$line" | grep -o "vm_name='[^']*'")"
    elif echo "$line" | grep -q "vm-metrics-updated RECEIVED"; then
        echo -e "${CYAN}[SIGNAL] metrics     ${NC}$(echo "$line" | grep -o "vm_count=[0-9]* first_uuid=[^ ]*")"
    elif echo "$line" | grep -q "GSubprocessLauncher"; then
        echo -e "${CYAN}[P7]     ${NC}${line##*] }"
    fi
done
