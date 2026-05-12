#!/usr/bin/env bash
set -euo pipefail








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

reject_literal() {
  local needle="$1"
  local file="$2"
  local label="$3"
  if grep -Fq "$needle" "$file"; then
    fail "$label"
  fi
}

reject_literal_in_text() {
  local needle="$1"
  local text="$2"
  local label="$3"
  if printf '%s\n' "$text" | grep -Fq "$needle"; then
    fail "$label"
  fi
}

require_literal "var COMMON_ENDPOINTS = {" "ui/modules/endpoints.js" "common endpoint registry must be declared"
require_literal "return Object.assign({}, COMMON_ENDPOINTS);" "ui/modules/endpoints.js" "single endpoint surface must be common-only"
reject_literal "var MULTI_EDGE_ENDPOINTS = {" "ui/modules/endpoints.js" "single endpoint registry must not declare multi endpoint registry"
require_literal "PCV.applyEditionEndpointSurface" "ui/modules/endpoints.js" "edition endpoint surface rebuilder must exist"
require_literal "PCV.filterEditionItems" "ui/modules/uxlib.js" "edition item filter helper must exist"
require_literal "replace(/^#\\/?/, '')" "ui/modules/uxlib.js" "hash routing must accept both #page and #/page deep links"
require_literal "PCV.filterEditionItems" "ui/modules/nav.js" "navigation must use edition filter helper"
require_literal "buildHelpData" "ui/modules/help.js" "help data must be built through edition-aware helper"
reject_literal "federation.site" "ui/modules/advanced.js" "advanced module must not expose federation RPCs in Single Edge"
reject_literal "EP.VM_MIGRATE(" "ui/modules/vm.js" "vm module must not directly bind migration endpoint in single surface"
reject_literal "EP.CLUSTER_STATUS(" "ui/modules/accounts.js" "api management health probe must not depend on cluster endpoint"
reject_literal "EP.CLUSTER_VMS(" "ui/modules/monitor.js" "resource heatmap must not call removed cluster VM endpoint in single surface"
reject_literal "EP.CLUSTER_VMS(" "ui/app.bundle.js" "bundled resource heatmap must not call removed cluster VM endpoint in single surface"

single_landing_chips="$(sed -n "/^      single: {/,/^        meta:/p" ui/index.html)"
reject_literal_in_text "JWT · RBAC" "$single_landing_chips" "single landing page must not expose JWT · RBAC chip"
reject_literal_in_text "ZFS · Snapshot" "$single_landing_chips" "single landing page must not expose ZFS · Snapshot chip"
reject_literal_in_text "174 Metrics" "$single_landing_chips" "single landing page must not expose 174 Metrics chip"
reject_literal_in_text "io_uring · UDS" "$single_landing_chips" "single landing page must not expose io_uring · UDS chip"
reject_literal "BUILD · SINGLE EDGE · 2026-04-11" "ui/index.html" "single landing page must not expose single build footer text"

single_landing_markup="$(sed -n '/<section class="login-pitch">/,/<\/section>/p' ui/index.html)"
require_literal_in_text() {
  local needle="$1"
  local text="$2"
  local label="$3"
  if ! printf '%s\n' "$text" | grep -Fq "$needle"; then
    fail "$label"
  fi
}
reject_literal_in_text "클러스터 HA" "$single_landing_markup" "single landing page static hero must not mention cluster HA"
require_literal_in_text "단일 노드 운영" "$single_landing_markup" "single landing page static hero must describe single-node operations"
require_literal_in_text "웹 콘솔 · REST API" "$single_landing_markup" "single landing page static hero must expose concrete single operating surface"
require_literal "<meta name=\"description\"" "ui/index.html" "landing page must define description metadata"
require_literal "<base href=\"/ui/\">" "ui/index.html" "app shell must pin the asset base path for /ui entrypoint"
reject_literal "<title>PureCVisor — Dashboard</title>" "ui/index.html" "landing page title must not use dashboard default"
reject_literal "cdn.tailwindcss.com" "ui/index.html" "single UI must not load Tailwind CDN in production"
reject_literal "tailwind.config" "ui/index.html" "single UI must not define runtime Tailwind config in production"
require_literal "if (document.readyState === 'loading')" "ui/index.html" "splash init must handle already-parsed documents"
require_literal "window.addEventListener('load', _pcvSplashRemove" "ui/index.html" "splash removal must be reinforced on window load"
reject_literal "document.addEventListener('DOMContentLoaded', _pcvInitUI);" "ui/index.html" "splash init must not rely on bare DOMContentLoaded listener only"

reject_literal "class=\"toolbar-auth\"" "ui/index.html" "app toolbar must not expose inline credential fields"
reject_literal "id=\"iu\"" "ui/index.html" "app toolbar must not ship username input"
reject_literal "id=\"ip\"" "ui/index.html" "app toolbar must not ship password input"
require_literal "class=\"toolbar-session\"" "ui/index.html" "app toolbar must expose a compact session prompt"
require_literal "로그인 화면" "ui/index.html" "app toolbar guest prompt must route users to the login overlay"

breadcrumbs_count="$(grep -c 'id=\"breadcrumbs\"' ui/index.html)"
if [[ "$breadcrumbs_count" -ne 1 ]]; then
  fail "app shell must declare a single breadcrumbs container"
fi

reject_literal ">File</span>" "ui/index.html" "app shell must not ship File as the static menu label"
reject_literal ">VM Library</span>" "ui/index.html" "app shell must not ship VM Library as the static sidebar label"
reject_literal "placeholder=\"Filter...\"" "ui/index.html" "app shell search inputs must not use Filter placeholder"
reject_literal ">INFRASTRUCTURE</span>" "ui/index.html" "app shell must not ship INFRASTRUCTURE as the static infra heading"
reject_literal "<span>Home</span>" "ui/index.html" "mobile nav must not ship Home as the static single label"
reject_literal "<span>Monitor</span>" "ui/index.html" "mobile nav must not ship Monitor as the static single label"
require_literal "운영 콘솔" "ui/index.html" "app shell must identify the single console in Korean"
require_literal "이름으로 찾기" "ui/index.html" "app shell search inputs must use Korean helper text"
reject_literal ">Networks</div>" "ui/index.html" "top menu must not expose Networks in English"
reject_literal ">Storage</div>" "ui/index.html" "top menu must not expose Storage in English"

reject_literal "Cluster Overview" "ui/modules/monitor.js" "monitor overview must not use the cluster overview heading in single edition"
reject_literal "Cluster Timeline (rolling 5 min)" "ui/modules/monitor.js" "monitor overview must not use the cluster timeline heading in single edition"
reject_literal "Host Monitor" "ui/modules/monitor.js" "host screen must not use the host monitor heading in single edition"
require_literal "운영 개요" "ui/modules/monitor.js" "monitor overview must expose the new single operations heading"
require_literal "리소스 흐름" "ui/modules/monitor.js" "monitor overview must expose the new single resource timeline heading"
require_literal "호스트 상태" "ui/modules/monitor.js" "host screen must expose the new single host heading"
require_literal "운영 메모" "ui/modules/monitor.js" "host screen must expose the new operating note card"
require_literal "현재 조치" "ui/modules/monitor.js" "host screen must expose the new action guidance card"
require_literal "renderOpsTriage" "ui/modules/monitor.js" "monitor module must expose the operations triage renderer"
require_literal "운영 이벤트 센터" "ui/modules/monitor.js" "operations triage screen must expose the event center heading"
require_literal "이벤트 triage" "ui/modules/monitor.js" "operations triage screen must include a triage event lane"
require_literal "명령 팔레트" "ui/modules/monitor.js" "operations triage screen must include the command palette lane"
require_literal "'ops-triage': () => renderOpsTriage(b)" "ui/modules/nav.js" "navigation must route the operations triage page"
require_literal "{ id: 'ops-triage' }" "ui/modules/nav.js" "operations triage page must be included in the infra surface"
require_literal "data-nav=\"ops-triage\"" "ui/index.html" "sidebar must expose the operations triage page"
require_literal "이벤트 센터" "ui/index.html" "sidebar must label the operations triage page"
require_literal "renderOpsTriage" "ui/app.bundle.js" "bundled UI must contain the operations triage renderer"

reject_literal "No ZFS pools found" "ui/modules/storage.js" "storage screen must not ship the old empty-state copy"
reject_literal "Capacity Forecast" "ui/modules/storage.js" "storage screen must not ship the old capacity forecast heading"
require_literal "스토리지 운영 개요" "ui/modules/storage.js" "storage screen must expose the new single storage overview heading"
require_literal "용량 예측" "ui/modules/storage.js" "storage screen must expose the new forecast heading"
require_literal "아직 생성된 Zvol이 없습니다" "ui/modules/storage.js" "storage screen must expose the new zvol empty-state copy"

reject_literal "No networks configured" "ui/modules/network.js" "network screen must not ship the old empty-state copy"
reject_literal "OVN software-defined network access control. Manage via CLI:" "ui/modules/network.js" "network screen must not foreground the old OVN ACL CLI copy"
reject_literal "OVN Status" "ui/modules/network.js" "ovn screen must not ship the old OVN status heading"
reject_literal "Error loading OVN data" "ui/modules/network.js" "ovn screen must not ship the old generic error copy"
reject_literal "Use OVN status above to verify LB" "ui/modules/network.js" "ovn screen must not ship the old English load balancer helper"
require_literal "네트워크 인벤토리" "ui/modules/network.js" "network screen must expose the new inventory heading"
require_literal "방화벽 정책 편집" "ui/modules/network.js" "network screen must expose the new firewall policy editor heading"
require_literal "OVN ACL 운영 메모" "ui/modules/network.js" "network screen must expose the new OVN ACL operations note"
require_literal "OVN 가용성" "ui/modules/network.js" "ovn screen must expose the new availability heading"
require_literal "공개 OVN 데모 헬스" "ui/modules/network.js" "ovn screen must expose the public demo health heading"
require_literal "VM 간 통신" "ui/modules/network.js" "ovn screen must expose the public demo VM reachability card"
require_literal "DEMO_OVN_HEALTH" "ui/modules/endpoints.js" "endpoint registry must expose public OVN demo health"
require_literal "OVN 데모 서비스 구성" "ui/modules/network.js" "ovn screen must expose the dynamic demo service composition heading"
require_literal "OVN 내부 시각 서비스" "ui/modules/network.js" "ovn screen must expose the internal visual service heading"
require_literal "visual_service" "ui/modules/network.js" "ovn screen must render the visual service payload"
require_literal "node_descriptions" "ui/modules/network.js" "ovn screen must render OVN demo node descriptions"
require_literal "요청을 시작하는 클라이언트 VM" "ui/modules/network.js" "ovn screen must explain the demo source VM"
require_literal "service_flows" "ui/modules/network.js" "ovn screen must render OVN demo service flow lanes"
require_literal "외부 공개 시각 서비스 흐름" "ui/modules/network.js" "ovn screen must show the public visual service flow"
require_literal "내부 VM 점검 흐름" "ui/modules/network.js" "ovn screen must show the internal VM check flow"
require_literal "/ovn-visual/" "ui/modules/network.js" "ovn screen must expose the public visual service proxy path"
require_literal "외부 inbound 없음" "ui/modules/network.js" "ovn screen must state that the visual service has no external inbound path"
require_literal "demo.purecvisor.site" "ui/modules/network.js" "ovn demo composition must show the public demo domain"
require_literal "viewer 읽기 전용 경계" "ui/modules/network.js" "ovn demo composition must show the viewer read-only boundary"
require_literal "논리 토폴로지" "ui/modules/network.js" "ovn screen must expose the new topology heading"
require_literal "로드 밸런서 설정" "ui/modules/network.js" "ovn screen must expose the new load balancer heading"
require_literal "ACL 정책 추가" "ui/modules/network.js" "ovn screen must expose the new ACL policy heading"
require_literal "로드 밸런서 상태 메모" "ui/modules/network.js" "ovn screen must expose the new localized load balancer helper"

i18n_debt_marker='TO''DO: i18n'
reject_literal "$i18n_debt_marker" "ui/modules/vm.js" "vm console must not carry stale i18n debt comments"
reject_literal "VNC unavailable" "ui/modules/vm.js" "vm console unavailable copy must be localized"
reject_literal "Manual Check" "ui/modules/vm.js" "vm console manual check copy must be localized"
reject_literal ">Fit</button>" "ui/modules/vm.js" "vm console fit control must be localized"
reject_literal "Loading noVNC &amp; connecting to" "ui/modules/vm.js" "vm console loading status must be localized"
reject_literal "Remote: " "ui/modules/vm.js" "vm console remote-info label must be localized"
reject_literal "Security failure" "ui/modules/vm.js" "vm console security-failure copy must be localized"
require_literal "'vnc.fit'" "ui/i18n.js" "vnc fit label must be registered in i18n"
require_literal "'vnc.unavailable'" "ui/i18n.js" "vnc unavailable copy must be registered in i18n"
require_literal "'vnc.manual_check'" "ui/i18n.js" "vnc manual check copy must be registered in i18n"
require_literal "'vnc.loading_connecting'" "ui/i18n.js" "vnc loading status must be registered in i18n"
require_literal "'vnc.remote'" "ui/i18n.js" "vnc remote-info label must be registered in i18n"
require_literal "'vnc.error_suffix'" "ui/i18n.js" "vnc disconnect error suffix must be registered in i18n"
require_literal "'vnc.security_failure'" "ui/i18n.js" "vnc security-failure copy must be registered in i18n"

reject_literal "var label = n.replace('mon-', 'Mon: ')" "ui/app.js" "editor tabs must not derive English labels from raw route names"
reject_literal "var group = n.startsWith('mon-') ? 'Monitoring'" "ui/app.js" "breadcrumb grouping must not hardcode Monitoring in English"
reject_literal "['networks','storage','containers','host','cluster','ovn'].indexOf(n) >= 0 ? 'Infrastructure'" "ui/app.js" "breadcrumb grouping must not hardcode Infrastructure in English"
reject_literal "{ id: 'networks', label: 'Networks'" "ui/modules/nav.js" "global search pages must not expose English network label"
reject_literal "{ id: 'storage', label: 'Storage'" "ui/modules/nav.js" "global search pages must not expose English storage label"
require_literal "_L('운영', 'Operations')" "ui/app.js" "breadcrumb grouping must expose a localized operations label"
require_literal "_L('모니터링', 'Monitoring')" "ui/app.js" "breadcrumb grouping must expose a localized monitoring label"

printf 'PASS: single UI surface source boundaries found\n'
