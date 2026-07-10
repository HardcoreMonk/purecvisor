#!/usr/bin/env bash
set -euo pipefail

# Source-surface gate for the LinkedIn public OVS/OVN demo.
# The runtime demo is built on the production node, but this test pins the local
# API/UI contract so the read-only health surface does not silently drift.

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

require_literal 'PCV_OVN_DEMO_HEALTH_PATH "/var/lib/purecvisor/demo/ovn-ovs-health.json"' "src/api/rest_server.c" "backend must pin demo health state file path"
require_literal "PCV_OVN_DEMO_HEALTH_STALE_SEC 300" "src/api/rest_server.c" "backend must pin five minute stale policy"
require_literal "_send_ovn_demo_health" "src/api/rest_server.c" "backend must expose a dedicated read-only demo helper"
require_literal 'g_strcmp0(resource, "demo") == 0' "src/api/rest_server.c" "backend must route /demo resources explicitly"
require_literal 'g_strcmp0(name, "ovn-ovs") == 0' "src/api/rest_server.c" "backend must route the ovn-ovs demo explicitly"
require_literal 'g_strcmp0(action, "health") == 0' "src/api/rest_server.c" "backend must route the health action explicitly"
require_literal "DEMO_OVN_HEALTH" "ui/modules/endpoints.js" "frontend endpoint registry must expose demo health"
require_literal "/demo/ovn-ovs/health" "ui/modules/endpoints.js" "frontend endpoint must match backend route"
require_literal "공개 OVN 데모 헬스" "ui/modules/network.js" "OVN screen must show public demo health card"
require_literal "VM 간 통신" "ui/modules/network.js" "OVN screen must show VM reachability"
require_literal "OVN 데모 서비스 구성" "ui/modules/network.js" "OVN screen must show dynamic demo service composition"
require_literal "OVN 내부 시각 서비스" "ui/modules/network.js" "OVN screen must show internal visual service evidence"
require_literal "visual_service" "ui/modules/network.js" "OVN screen must read visual service health payload"
require_literal "node_descriptions" "ui/modules/network.js" "OVN screen must read visual service node descriptions"
require_literal "요청을 시작하는 클라이언트 VM" "ui/modules/network.js" "OVN screen must explain ovn-demo-a"
require_literal "OVN Logical Switch입니다" "ui/modules/network.js" "OVN screen must explain pcv-demo-ls"
require_literal "OVN Logical Router입니다" "ui/modules/network.js" "OVN screen must explain pcv-demo-lr"
require_literal "시각 서비스가 실행되는 서버 VM" "ui/modules/network.js" "OVN screen must explain ovn-demo-b"
require_literal "service_flows" "ui/modules/network.js" "OVN screen must render visual service flow lanes"
require_literal "외부 공개 시각 서비스 흐름" "ui/modules/network.js" "OVN screen must show public visual service flow"
require_literal "내부 VM 점검 흐름" "ui/modules/network.js" "OVN screen must show internal VM check flow"
require_literal "pcv-demo-lr 라우팅 경계" "ui/modules/network.js" "OVN screen must show logical router boundary flow"
require_literal "host collector" "ui/modules/network.js" "OVN screen must show collector-to-VM validation flow"
require_literal "nginx /ovn-visual/" "ui/modules/network.js" "OVN screen must show public visual proxy flow"
require_literal "10.77.0.12:8080" "ui/modules/network.js" "OVN visual service must show the internal-only endpoint"
require_literal "https://' + publicDomain + '/ovn-visual/" "ui/modules/network.js" "OVN visual service must show the public reverse proxy endpoint"
require_literal "외부 inbound 없음" "ui/modules/network.js" "OVN visual service must show the no-external-inbound boundary"
require_literal "demo.purecvisor.example.com" "ui/modules/network.js" "OVN demo composition must show public demo domain"
require_literal "viewer 읽기 전용 경계" "ui/modules/network.js" "OVN demo composition must show viewer permission boundary"
require_literal "PureCVisor API" "ui/modules/network.js" "OVN demo composition must show API layer"
require_literal "VM A → VM B" "ui/modules/network.js" "OVN demo composition must show VM-to-VM path"
require_literal "PureCVisor OVN demo architecture" "docs/purecvisor_ovn_demo_architecture.svg" "OVN demo architecture SVG must identify the diagram"
require_literal "pcv-demo-host" "docs/purecvisor_ovn_demo_architecture.svg" "OVN demo architecture SVG must show the host access interface"
require_literal "host collector" "docs/purecvisor_ovn_demo_architecture.svg" "OVN demo architecture SVG must show the internal collector flow"
require_literal "VM public inbound: false" "docs/superpowers/specs/2026-05-08-ovn-demo-webpage-redesign-design.md" "OVN visual redesign must highlight VM public inbound boundary"
require_literal "Public Entry" "docs/superpowers/specs/2026-05-08-ovn-demo-webpage-redesign-design.md" "OVN visual redesign must group public entry nodes"
require_literal "Host Boundary" "docs/superpowers/specs/2026-05-08-ovn-demo-webpage-redesign-design.md" "OVN visual redesign must group host boundary nodes"
require_literal "OVN Fabric" "docs/superpowers/specs/2026-05-08-ovn-demo-webpage-redesign-design.md" "OVN visual redesign must group OVN fabric nodes"
require_literal "Workloads" "docs/superpowers/specs/2026-05-08-ovn-demo-webpage-redesign-design.md" "OVN visual redesign must group workload nodes"
require_literal 'data-node="browser"' "docs/purecvisor_ovn_demo_architecture.svg" "OVN architecture SVG must expose clickable browser node metadata"
require_literal 'data-node="ovn-demo-b"' "docs/purecvisor_ovn_demo_architecture.svg" "OVN architecture SVG must expose clickable VM B node metadata"
require_literal 'data-flow="external-visual-service"' "docs/purecvisor_ovn_demo_architecture.svg" "OVN architecture SVG must expose external flow metadata"
require_literal 'data-flow="internal-health-check"' "docs/purecvisor_ovn_demo_architecture.svg" "OVN architecture SVG must expose internal flow metadata"
require_literal 'data-flow="logical-router-boundary"' "docs/purecvisor_ovn_demo_architecture.svg" "OVN architecture SVG must expose router boundary flow metadata"
require_literal "addEventListener" "docs/operations/2026-05-07-ovn-ovs-public-demo-handoff.md" "OVN visual page must use JS event listeners instead of inline handlers"
require_literal "is-flow-active" "docs/operations/2026-05-07-ovn-ovs-public-demo-handoff.md" "OVN visual page must document animated flow CSS state"
require_literal "SVG 중심 단일 화면" "docs/superpowers/specs/2026-05-08-ovn-demo-simplified-page-design.md" "OVN visual page must be simplified around the architecture SVG"
require_literal "selected-node" "docs/superpowers/specs/2026-05-08-ovn-demo-simplified-page-design.md" "OVN visual page must keep a compact selected-node summary"
require_literal "flow-tab" "docs/superpowers/specs/2026-05-08-ovn-demo-simplified-page-design.md" "OVN visual page must reduce flow cards into compact tabs"
require_literal "details-panel" "docs/superpowers/specs/2026-05-08-ovn-demo-simplified-page-design.md" "OVN visual page must move node detail copy behind a collapsible details panel"
require_literal "버튼은 화면에서 제거" "docs/superpowers/specs/2026-05-08-ovn-demo-simplified-page-design.md" "OVN visual page must document visual-state button removal"
reject_literal "sendPrompt" "docs/purecvisor_ovn_demo_architecture.svg" "OVN demo architecture SVG must not ship editor prompt handlers"
reject_literal "onclick=" "docs/purecvisor_ovn_demo_architecture.svg" "OVN demo architecture SVG must not ship inline event handlers"
reject_literal "javascript:" "docs/purecvisor_ovn_demo_architecture.svg" "OVN demo architecture SVG must not ship javascript URLs"

printf 'PASS: OVS/OVN demo source surface found\n'
