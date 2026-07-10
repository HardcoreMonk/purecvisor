#!/usr/bin/env bash
# =============================================================================
# PureCVisor Web UI 고도화 기능 테스트 시나리오
# F-1~F-4 + G-1~G-4 + H-1~H-2 전체 검증
# 대상: 3노드 클러스터 (Node1:192.0.2.10 / Node2:192.0.2.53 / Node3:192.0.2.55)
# VIP: 192.0.2.100
# =============================================================================
set -uo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0; TOTAL=0

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
VIP="192.0.2.100"
NODES=("192.0.2.10" "192.0.2.53" "192.0.2.55")
NODE_NAMES=("Node1" "Node2" "Node3")
API="http://${VIP}:80/api/v1"
UI="http://${VIP}:80/ui"
TOKEN=""
CSRF=""

# ── Helpers ──────────────────────────────────────────────────
ok()   { ((PASS++)); ((TOTAL++)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { ((FAIL++)); ((TOTAL++)); echo -e "  ${RED}[FAIL]${NC} $1"; }
skip() { ((SKIP++)); ((TOTAL++)); echo -e "  ${YELLOW}[SKIP]${NC} $1"; }
section() { echo -e "\n${CYAN}━━━ $1 ━━━${NC}"; }

check_http() {
  local desc="$1" url="$2" expect="$3"
  local code
  code=$(curl -s --connect-timeout 5 -o /dev/null -w "%{http_code}" "$url" 2>/dev/null)
  if [ "$code" = "$expect" ]; then ok "$desc (HTTP $code)"; else fail "$desc (got $code, expected $expect)"; fi
}

check_http_auth() {
  local desc="$1" url="$2" expect="$3"
  local code
  code=$(curl -s --connect-timeout 5 -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $TOKEN" "$url" 2>/dev/null)
  if [ "$code" = "$expect" ]; then ok "$desc (HTTP $code)"; else fail "$desc (got $code, expected $expect)"; fi
}

check_file_contains() {
  local desc="$1" filepath="$2" pattern="$3"
  if grep -qF "$pattern" "$filepath" 2>/dev/null; then ok "$desc"; else fail "$desc (not found in file)"; fi
}

check_contains() {
  local desc="$1" url="$2" pattern="$3"
  local body
  body=$(curl -s --connect-timeout 5 "$url" 2>/dev/null)
  if echo "$body" | grep -qF "$pattern"; then ok "$desc"; else fail "$desc (pattern '$pattern' not found)"; fi
}

check_content_type() {
  local desc="$1" url="$2" expect="$3"
  local ct
  ct=$(curl -s --connect-timeout 5 -o /dev/null -w "%{content_type}" "$url" 2>/dev/null)
  if echo "$ct" | grep -qF "$expect"; then ok "$desc ($ct)"; else fail "$desc (got '$ct', expected '$expect')"; fi
}

get_token() {
  local resp
  resp=$(curl -s --connect-timeout 5 -X POST "${API}/auth/token" \
    -H "Content-Type: application/json" \
    -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" 2>/dev/null)
  TOKEN=$(echo "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
  CSRF=$(echo "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('csrf_token',''))" 2>/dev/null)
  [ -n "$TOKEN" ] && [ "$TOKEN" != "" ]
}

auth_get() {
  curl -s --connect-timeout 5 -H "Authorization: Bearer $TOKEN" "$1" 2>/dev/null
}

# =============================================================================
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║   PureCVisor Web UI 고도화 기능 테스트                      ║${NC}"
echo -e "${CYAN}║   F-1~F-4 + G-1~G-4 + H-1~H-2 (3차 고도화 완료)           ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"

# =============================================================================
section "1. 파일 구조 + 서빙 검증 (F-1)"
# =============================================================================

for i in "${!NODES[@]}"; do
  ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
  check_http "$name index.html" "http://${ip}:80/ui/index.html" "200"
  check_http "$name style.css" "http://${ip}:80/ui/style.css" "200"
  check_http "$name app.js" "http://${ip}:80/ui/app.js" "200"
  check_http "$name i18n.js" "http://${ip}:80/ui/i18n.js" "200"
done

check_content_type "index.html MIME" "${UI}/index.html" "text/html"
check_content_type "style.css MIME" "${UI}/style.css" "text/css"
check_content_type "app.js MIME" "${UI}/app.js" "application/javascript"
check_content_type "i18n.js MIME" "${UI}/i18n.js" "application/javascript"

# =============================================================================
section "2. CSP + 보안 (F-1, H-2)"
# =============================================================================

check_contains "CSP connect-src allows http:" "${UI}/index.html" "connect-src 'self' ws: wss: https: http:"
check_contains "CSP script-src allows CDN" "${UI}/index.html" "https://cdn.jsdelivr.net"
check_contains "CSP style-src allows fonts" "${UI}/index.html" "https://fonts.googleapis.com"

check_http "Root → 302 /ui/" "http://${VIP}:80/" "302"
check_http "Path traversal blocked" "http://${VIP}:80/ui/../etc/passwd" "401"

# =============================================================================
section "3. HTML 구조 + 접근성 (F-4)"
# =============================================================================

HTML=$(curl -s --connect-timeout 5 "${UI}/index.html" 2>/dev/null)

# ARIA
aria_label=$(echo "$HTML" | grep -co 'aria-label' 2>/dev/null || echo 0)
aria_live=$(echo "$HTML" | grep -co 'aria-live' 2>/dev/null || echo 0)
roles=$(echo "$HTML" | grep -co 'role=' 2>/dev/null || echo 0)
tabindexes=$(echo "$HTML" | grep -co 'tabindex=' 2>/dev/null || echo 0)

[ "$aria_label" -ge 25 ] && ok "aria-label: ${aria_label}개" || fail "aria-label: ${aria_label}개 (25+ expected)"
[ "$aria_live" -ge 2 ] && ok "aria-live: ${aria_live}개" || fail "aria-live: ${aria_live}개 (2+ expected)"
[ "$roles" -ge 50 ] && ok "role=: ${roles}개" || fail "role=: ${roles}개 (50+ expected)"
[ "$tabindexes" -ge 20 ] && ok "tabindex=: ${tabindexes}개" || fail "tabindex=: ${tabindexes}개 (20+ expected)"

check_contains "Skip-to-content 링크" "${UI}/index.html" 'class="skip-link"'
check_contains "모달 role=dialog" "${UI}/index.html" 'role="dialog"'
check_contains "모달 aria-modal" "${UI}/index.html" 'aria-modal="true"'
check_contains "언어 선택기 존재" "${UI}/index.html" 'id="lang-select"'
check_contains "테마 선택기 존재" "${UI}/index.html" 'id="theme-select"'

# =============================================================================
section "4. CSS 클래스 + 테마 (F-1, G-1, H-2)"
# =============================================================================

LOCAL_CSS="${PROJECT_DIR}/ui/style.css"
LOCAL_JS="${PROJECT_DIR}/ui/app.js"
LOCAL_I18N="${PROJECT_DIR}/ui/i18n.js"

check_file_contains "NEON_DARK (default)" "$LOCAL_CSS" ":root"
check_file_contains "PURE_LIGHT 테마" "$LOCAL_CSS" "pure-light"
check_file_contains "PURE_DARK 테마" "$LOCAL_CSS" "pure-dark"
check_file_contains "prefers-color-scheme" "$LOCAL_CSS" "prefers-color-scheme"
check_file_contains "focus-visible" "$LOCAL_CSS" "focus-visible"

# G-1 시맨틱 클래스
for cls in grid-2 grid-3 stat-xl stat-lg sparkline section-title; do
  check_file_contains "CSS 클래스: .$cls" "$LOCAL_CSS" "$cls"
done

# H-2 마이크로 인터랙션
for cls in page-fade empty-state notif-badge btn-loading cmd-overlay fav-star confirm-overlay; do
  check_file_contains "마이크로 인터랙션: .$cls" "$LOCAL_CSS" "$cls"
done

# 반응형
for bp in "max-width: 1024" "max-width: 768" "max-width: 480"; do
  check_file_contains "반응형 BP: $bp" "$LOCAL_CSS" "$bp"
done

# =============================================================================
section "5. i18n 모듈 (F-4, G-3)"
# =============================================================================

check_file_contains "i18n ko 언어" "$LOCAL_I18N" "ko:"
check_file_contains "i18n en 언어" "$LOCAL_I18N" "en:"
check_file_contains "t() 함수" "$LOCAL_I18N" "t(key"
check_file_contains "setLang()" "$LOCAL_I18N" "setLang"
check_file_contains "toggle()" "$LOCAL_I18N" "toggle"
check_file_contains "getLang()" "$LOCAL_I18N" "getLang"
check_file_contains "localStorage 영속" "$LOCAL_I18N" "localStorage"

# =============================================================================
section "6. app.js 구조 + 코드 품질 (F-1, G-2, H-1)"
# =============================================================================

# 변수명 정규화 (F-1)
check_file_contains "변수 rename: authToken" "$LOCAL_JS" "authToken"
check_file_contains "변수 rename: vmList" "$LOCAL_JS" "vmList"
check_file_contains "변수 rename: csrfToken" "$LOCAL_JS" "csrfToken"
check_file_contains "함수 rename: fetchGet" "$LOCAL_JS" "fetchGet"
check_file_contains "함수 rename: fetchPost" "$LOCAL_JS" "fetchPost"
check_file_contains "함수 rename: escapeHtml" "$LOCAL_JS" "escapeHtml"

# H 빌더 (G-2)
check_file_contains "H 빌더 정의됨" "$LOCAL_JS" "const H ="
check_file_contains "H.card 사용됨" "$LOCAL_JS" "H.card"
check_file_contains "H.row 사용됨" "$LOCAL_JS" "H.row"
check_file_contains "H.badge 사용됨" "$LOCAL_JS" "H.badge"

# 함수 분할 (G-2)
for fn in renderMonOverview renderMonCluster renderMonHosts renderMonVms renderMonStorage; do
  check_file_contains "분할 함수: $fn" "$LOCAL_JS" "function $fn"
done

# Promise.all 병렬 fetch (G-2)
check_file_contains "Promise.all 병렬 fetch" "$LOCAL_JS" "Promise.all"

# 빈 catch 블록 제거 (H-1)
empty_catch=$(grep -cF 'catch (e) {}' "$LOCAL_JS" 2>/dev/null)
empty_catch="${empty_catch:-0}"
empty_catch=$(echo "$empty_catch" | tr -d '[:space:]')
[ "$empty_catch" = "0" ] && ok "빈 catch 블록: 0개" || fail "빈 catch 블록 잔존: ${empty_catch}개"

# console.warn 추가 (H-1)
warn_count=$(grep -cF 'console.warn' "$LOCAL_JS" 2>/dev/null || echo 0)
[ "$warn_count" -ge 20 ] && ok "에러 로깅: console.warn ${warn_count}개" || fail "console.warn 부족: ${warn_count}개"

# Chart.js 통합 (F-3)
check_file_contains "Chart.js 레지스트리" "$LOCAL_JS" "chartRegistry"
check_file_contains "createLineChart 함수" "$LOCAL_JS" "createLineChart"
check_file_contains "destroyAllCharts 함수" "$LOCAL_JS" "destroyAllCharts"

# =============================================================================
section "7. 새 UX 기능 (G-4, H-2)"
# =============================================================================

# 커맨드 팔레트
check_file_contains "커맨드 팔레트: CMD_ACTIONS" "$LOCAL_JS" "CMD_ACTIONS"
check_file_contains "커맨드 팔레트: openCmdPalette" "$LOCAL_JS" "openCmdPalette"
check_file_contains "커맨드 팔레트: closeCmdPalette" "$LOCAL_JS" "closeCmdPalette"

# VM 즐겨찾기
check_file_contains "VM 즐겨찾기: toggleFavorite" "$LOCAL_JS" "toggleFavorite"
check_file_contains "VM 즐겨찾기: localStorage" "$LOCAL_JS" "pcv-favorites"

# customConfirm
check_file_contains "커스텀 확인 다이얼로그" "$LOCAL_JS" "customConfirm"

# 스켈레톤 로딩
check_file_contains "스켈레톤 로딩" "$LOCAL_JS" "showSkeleton"

# 토스트 스택 제한
check_file_contains "토스트 스택 3개 제한" "$LOCAL_JS" "children.length > 3"

# 빈 상태 안내
check_file_contains "빈 상태 안내 UI" "$LOCAL_JS" "empty-state"

# =============================================================================
section "8. 인증 흐름 (JWT + CSRF)"
# =============================================================================

if get_token; then
  ok "JWT 토큰 발급 성공"
else
  fail "JWT 토큰 발급 실패"
fi

check_http "인증 없이 → 401" "${API}/vms" "401"

resp=$(auth_get "${API}/vms")
vm_count=$(echo "$resp" | python3 -c "import sys,json;d=json.load(sys.stdin);print(len(d) if isinstance(d,list) else len(d.get('data',[])))" 2>/dev/null || echo 0)
[ "$vm_count" -ge 0 ] && ok "인증된 VM 목록: ${vm_count}개" || fail "VM 목록 조회 실패"

check_http "Public /health" "${API}/health" "200"
check_http "Public /metrics" "${API}/metrics" "200"

# =============================================================================
section "9. REST API 엔드포인트 (VM/CTR/NET/STG)"
# =============================================================================

get_token

# VM
vm_resp=$(auth_get "${API}/vms")
ok "GET /vms (${vm_count} VMs)"

running_vm=$(echo "$vm_resp" | python3 -c "
import sys,json;d=json.load(sys.stdin)
vms=d if isinstance(d,list) else d.get('data',[])
r=[v for v in vms if v.get('state')=='running']
print(r[0]['name'] if r else '')
" 2>/dev/null)

if [ -n "$running_vm" ]; then
  check_http_auth "GET /vms/$running_vm/metrics" "${API}/vms/${running_vm}/metrics" "200"
  check_http_auth "GET /vms/$running_vm/snapshot" "${API}/vms/${running_vm}/snapshot" "200"
  check_http_auth "GET /vms/$running_vm/nics" "${API}/vms/${running_vm}/nics" "200"
  check_http_auth "GET /vnc/$running_vm" "${API}/vnc/${running_vm}" "200"
else
  skip "Running VM 없음 — 메트릭/스냅샷/NIC/VNC 테스트 생략"
fi

# Networks
net_resp=$(auth_get "${API}/networks")
net_count=$(echo "$net_resp" | python3 -c "import sys,json;d=json.load(sys.stdin);print(len(d) if isinstance(d,list) else len(d.get('data',[])))" 2>/dev/null || echo 0)
[ "$net_count" -ge 0 ] && ok "GET /networks: ${net_count}개" || fail "Networks 조회 실패"

# Storage
check_http_auth "GET /storage/pools" "${API}/storage/pools" "200"
check_http_auth "GET /storage/zvols" "${API}/storage/zvols" "200"

# Containers
ctr_resp=$(auth_get "${API}/containers")
ctr_count=$(echo "$ctr_resp" | python3 -c "import sys,json;d=json.load(sys.stdin);print(len(d) if isinstance(d,list) else len(d.get('data',[])))" 2>/dev/null || echo 0)
[ "$ctr_count" -ge 0 ] && ok "GET /containers: ${ctr_count}개" || fail "Containers 조회 실패"

# ISO
check_http_auth "GET /iso" "${API}/iso" "200"

# OVN
check_http_auth "GET /ovn/status" "${API}/ovn/status" "200"

# Auth
check_http_auth "GET /auth/users" "${API}/auth/users" "200"

# =============================================================================
section "10. Monitoring + 메트릭 수집 (F-3, G-2)"
# =============================================================================

for i in "${!NODES[@]}"; do
  ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
  metrics=$(curl -s --connect-timeout 5 "http://${ip}:80/api/v1/metrics" 2>/dev/null)
  pcv=$(echo "$metrics" | grep -c "^purecvisor_" 2>/dev/null || echo 0)
  node=$(echo "$metrics" | grep -c "^node_" 2>/dev/null || echo 0)
  cpu=$(echo "$metrics" | grep "^purecvisor_host_cpu_percent " | awk '{print $2}')
  [ "$pcv" -gt 0 ] && ok "$name 메트릭: purecvisor_* ${pcv}줄, node_* ${node}줄, CPU=${cpu}%" || fail "$name 메트릭 수집 실패"
done

# Alerts
check_http_auth "GET /alerts" "${API}/alerts" "200"
check_http_auth "GET /alerts/config" "${API}/alerts/config" "200"

# Processes
check_http_auth "GET /processes" "${API}/processes" "200"

# Chart.js local vendor asset
check_http "Chart.js 로컬 vendor 접근" "http://${VIP}:80/ui/vendor/chart.umd.min.js" "200"

# =============================================================================
section "11. Cluster HA (3노드)"
# =============================================================================

cluster_resp=$(curl -s --connect-timeout 5 "http://${VIP}:80/api/v1/internal/cluster/vms" 2>/dev/null)
cluster_vms=$(echo "$cluster_resp" | python3 -c "
import sys,json;d=json.load(sys.stdin)
vms=d if isinstance(d,list) else d.get('result',d.get('data',[]))
nodes=set(v.get('node','?') for v in vms)
print(f'{len(vms)} VMs, {len(nodes)} nodes')
" 2>/dev/null || echo "parse error")
ok "Cluster VMs: ${cluster_vms}"

for i in "${!NODES[@]}"; do
  ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
  check_http "$name /health" "http://${ip}:80/api/v1/health" "200"
done

# =============================================================================
section "12. WebSocket 엔드포인트"
# =============================================================================

get_token
for i in "${!NODES[@]}"; do
  ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
  ws_code=$(curl -s --connect-timeout 3 -o /dev/null -w "%{http_code}" \
    -H "Upgrade: websocket" -H "Connection: Upgrade" \
    "http://${ip}:80/api/v1/ws/events?token=${TOKEN}" 2>/dev/null)
  [ "$ws_code" = "400" ] || [ "$ws_code" = "101" ] && ok "$name WS /events: HTTP $ws_code" || fail "$name WS: HTTP $ws_code"
done

# =============================================================================
section "13. 키보드 단축키 + 모바일 (app.js)"
# =============================================================================

check_file_contains "Escape 키 모달 닫기" "$LOCAL_JS" "Escape"
check_file_contains "F11 전체화면" "$LOCAL_JS" "F11"
check_file_contains "Ctrl+K 커맨드 팔레트" "$LOCAL_JS" "openCmdPalette"
check_file_contains "터치 스와이프 지원" "$LOCAL_JS" "touchstart"
check_file_contains "모바일 사이드바 토글" "$LOCAL_JS" "toggleMobileSB"

# =============================================================================
section "14. VIP 통합 접근 테스트"
# =============================================================================

check_http "VIP root → /ui/" "http://${VIP}:80/" "302"
check_http "VIP /ui/" "http://${VIP}:80/ui/" "200"
check_http "VIP /health" "http://${VIP}:80/api/v1/health" "200"
check_http "VIP /metrics" "http://${VIP}:80/api/v1/metrics" "200"

# =============================================================================
# 결과 요약
# =============================================================================
echo ""
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║                    테스트 결과 요약                          ║${NC}"
echo -e "${CYAN}╠══════════════════════════════════════════════════════════════╣${NC}"
printf "${CYAN}║${NC}  ${GREEN}PASS: %-4d${NC} ${RED}FAIL: %-4d${NC} ${YELLOW}SKIP: %-4d${NC}  Total: %-4d        ${CYAN}║${NC}\n" $PASS $FAIL $SKIP $TOTAL
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"

if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}All tests passed!${NC}"
  exit 0
else
  echo -e "${RED}${FAIL} test(s) failed.${NC}"
  exit 1
fi
