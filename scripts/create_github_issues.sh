#!/usr/bin/env bash
# =============================================================================
# create_github_issues.sh
# PureCVisor 버그 수정 이력 및 기능 이슈를 GitHub Issues로 생성하는 스크립트
#
# 사전 요구사항:
#   - gh CLI 설치: https://cli.github.com/
#   - gh auth login 완료
#
# 사용법:
#   chmod +x scripts/create_github_issues.sh
#   ./scripts/create_github_issues.sh
#
# 멱등성: 동일 제목의 이슈가 이미 존재하면 건너뜁니다.
# =============================================================================

set -euo pipefail

REPO="HardcoreMonk/purecvisor"
MILESTONE_NAME="Sprint-Bug-Fixes-2026-03"

# ---------------------------------------------------------------------------
# 색상 출력 헬퍼
# ---------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[SKIP]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ---------------------------------------------------------------------------
# gh CLI 및 인증 확인
# ---------------------------------------------------------------------------
check_prerequisites() {
    if ! command -v gh &>/dev/null; then
        error "gh CLI가 설치되어 있지 않습니다."
        error "설치: https://cli.github.com/"
        exit 1
    fi

    if ! gh auth status &>/dev/null; then
        error "gh 인증이 필요합니다. 'gh auth login'을 실행하세요."
        exit 1
    fi

    info "gh CLI 인증 확인 완료"
}

# ---------------------------------------------------------------------------
# 라벨 생성 (이미 존재하면 건너뜀)
# ---------------------------------------------------------------------------
ensure_label() {
    local name="$1"
    local color="$2"
    local description="$3"

    if gh label list --repo "$REPO" --limit 200 | grep -qw "$name"; then
        warn "라벨 '$name' 이미 존재"
    else
        gh label create "$name" --repo "$REPO" --color "$color" --description "$description"
        info "라벨 '$name' 생성 완료"
    fi
}

create_labels() {
    info "=== 라벨 생성 ==="
    ensure_label "bug"         "d73a4a" "버그 수정"
    ensure_label "feature"     "0075ca" "신규 기능"
    ensure_label "docs"        "0e8a16" "문서화"
    ensure_label "P1-critical" "b60205" "긴급: 서비스 장애 유발"
    ensure_label "P2-major"    "e4e669" "중요: 기능 동작 불완전"
    ensure_label "P3-minor"    "fbca04" "경미: 사용성/표시 이슈"
    ensure_label "resolved"    "0e8a16" "해결 완료"
}

# ---------------------------------------------------------------------------
# 마일스톤 생성 (이미 존재하면 건너뜀)
# ---------------------------------------------------------------------------
create_milestone() {
    info "=== 마일스톤 생성 ==="
    if gh api "repos/$REPO/milestones" --paginate -q '.[].title' | grep -qx "$MILESTONE_NAME"; then
        warn "마일스톤 '$MILESTONE_NAME' 이미 존재"
    else
        gh api "repos/$REPO/milestones" \
            -f title="$MILESTONE_NAME" \
            -f state="open" \
            -f due_on="2026-03-31T23:59:59Z" \
            -f description="2026년 3월 스프린트 — 실전 배포 버그 11건 수정 + 기능 6건 구현"
        info "마일스톤 '$MILESTONE_NAME' 생성 완료"
    fi
}

# ---------------------------------------------------------------------------
# 이슈 생성 헬퍼 (멱등: 동일 제목 이슈 존재 시 건너뜀)
# ---------------------------------------------------------------------------
create_issue() {
    local title="$1"
    local body="$2"
    shift 2
    local labels=("$@")

    # 제목으로 기존 이슈 검색
    local existing
    existing=$(gh issue list --repo "$REPO" --search "\"$title\" in:title" --state all --limit 5 -q '.[] | .title' --json title 2>/dev/null || true)

    if echo "$existing" | grep -qxF "$title"; then
        warn "이슈 이미 존재: $title"
        return
    fi

    local label_args=""
    for lbl in "${labels[@]}"; do
        label_args="$label_args --label $lbl"
    done

    gh issue create \
        --repo "$REPO" \
        --title "$title" \
        --body "$body" \
        --milestone "$MILESTONE_NAME" \
        $label_args

    info "이슈 생성 완료: $title"
}

# ---------------------------------------------------------------------------
# 버그 이슈 #1 ~ #14
# ---------------------------------------------------------------------------
create_bug_issues() {
    info "=== 버그 이슈 생성 (11건) ==="

    # --- #1 ---
    create_issue \
        "[Bug] Zvol 삭제 시 (null) 에러" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
`storage.zvol.delete` RPC 호출 시 응답에 `(null)` 문자열이 포함되어 오류로 표시됨.
정상적으로 zvol이 삭제되었음에도 클라이언트에서 실패로 인식.

## 원인 (Root Cause)
`zfs_driver.c`에서 삭제 성공 시 결과 메시지 포맷 문자열에 NULL 포인터가 전달됨.
`g_strdup_printf()` 호출 시 `%s` 포맷에 NULL이 들어가 `(null)` 출력.

## 수정 (Fix)
성공 응답 메시지에 zvol 이름을 명시적으로 전달하도록 수정.
NULL 체크 가드 추가.

## 관련 파일 (Files)
- `src/modules/storage/zfs_driver.c`
- `src/modules/dispatcher/handler_storage.c`
BODY
)" \
        "bug" "P2-major" "resolved"

    # --- #2 ---
    create_issue \
        "[Bug] 컨테이너 START 실패 — seccomp BPF 상속 차단" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
`container.start` RPC 호출 시 LXC 컨테이너가 시작되지 않고 실패.
`lxc-start`가 AppArmor 전환 및 clone syscall에서 차단됨.

## 원인 (Root Cause)
데몬의 `PR_SET_NO_NEW_PRIVS` + seccomp BPF 필터가 자식 프로세스(lxc-start)에 상속됨.
LXC는 컨테이너 초기화 시 AppArmor 프로필 전환과 clone(CLONE_NEWNS 등)이 필요한데,
NNP가 설정되면 AppArmor 전환이 불가하고 seccomp가 관련 syscall을 차단.

## 수정 (Fix)
1. NNP(No New Privileges) 비활성화
2. seccomp 화이트리스트에 LXC 필수 syscall 추가 (clone, mount, pivot_root 등)
3. 이후 NNP + seccomp 재활성화하되 LXC 호환성 확보

## 관련 파일 (Files)
- `src/utils/pcv_privdrop.c`
- `src/modules/lxc/lxc_driver.c`
BODY
)" \
        "bug" "P1-critical" "resolved"

    # --- #3 ---
    create_issue \
        "[Bug] 컨테이너 STOP 시 SEGV 크래시" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
`container.stop` RPC 호출 시 데몬이 SIGSEGV로 크래시.
`journalctl`에 segfault 로그 확인.

## 원인 (Root Cause)
LXC 컨테이너 중지 후 상태 조회 시 이미 해제된 lxc_container 포인터를 역참조.
컨테이너 객체의 ref count 관리 미흡.

## 수정 (Fix)
lxc_container 객체의 참조 카운트를 정확히 관리하고,
중지 후 상태 조회 전에 NULL 체크 가드 추가.

## 관련 파일 (Files)
- `src/modules/lxc/lxc_driver.c`
- `src/modules/dispatcher/handler_container.c`
BODY
)" \
        "bug" "P1-critical" "resolved"

    # --- #4 ---
    create_issue \
        "[Bug] 컨테이너 exec 출력 미표시" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
`container.exec` RPC 호출 시 명령이 실행되지만 stdout/stderr 출력이 클라이언트에 전달되지 않음.
빈 문자열 또는 NULL 응답.

## 원인 (Root Cause)
`GSubprocess`가 데몬의 seccomp 환경을 상속하여 `lxc-attach`가 제한됨.
출력 캡처 파이프가 정상 동작하지 않음.

## 수정 (Fix)
`pcv_spawn_sync` 폴백 방식(`/bin/sh -c`)으로 전환하여
seccomp 상속 문제를 우회하고 출력을 정상 캡처.

## 관련 파일 (Files)
- `src/modules/lxc/lxc_driver.c`
- `src/utils/pcv_spawn.c`
BODY
)" \
        "bug" "P2-major" "resolved"

    # --- #5 ---
    create_issue \
        "[Bug] 컨테이너 IP 미노출 (N/A 표시)" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
`container.list` 및 `container.metrics`에서 컨테이너 IP가 항상 "N/A"로 표시.
실제로는 컨테이너에 IP가 할당되어 있음.

## 원인 (Root Cause)
liblxc의 `get_ips()` API가 seccomp 환경에서 실패하여 NULL 반환.
네트워크 네임스페이스 접근이 seccomp에 의해 차단됨.

## 수정 (Fix)
`lxc-info -iH` CLI 명령어를 폴백으로 추가.
liblxc API 실패 시 CLI 출력을 파싱하여 IP 주소 획득.

## 관련 파일 (Files)
- `src/modules/lxc/lxc_driver.c`
BODY
)" \
        "bug" "P3-minor" "resolved"

    # --- #7 ---
    create_issue \
        "[Bug] REST exec 필드 매핑 오류" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
REST API `POST /containers/{name}/exec` 호출 시 명령어가 전달되지 않음.
JSON 필드명 불일치로 인해 exec_cmd가 NULL로 처리.

## 원인 (Root Cause)
REST 서버에서 JSON 바디의 필드명을 `command`로 기대하지만,
디스패처/핸들러는 `exec_cmd` 필드명을 사용.
필드 매핑이 일치하지 않음.

## 수정 (Fix)
REST 서버의 JSON→RPC 변환 로직에서 필드명을 `exec_cmd`로 통일.

## 관련 파일 (Files)
- `src/api/rest_server.c`
- `src/modules/dispatcher/handler_container.c`
BODY
)" \
        "bug" "P2-major" "resolved"

    # --- #8 ---
    create_issue \
        "[Bug] Zvol 스냅샷 포함 삭제 실패" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
스냅샷이 존재하는 zvol을 `storage.zvol.delete`로 삭제 시 실패.
"dataset is busy" 또는 "has children" 오류 반환.

## 원인 (Root Cause)
ZFS는 자식 스냅샷이 있는 데이터셋을 기본적으로 삭제 불가.
`zfs destroy` 호출 시 `-r` (재귀) 플래그가 누락됨.

## 수정 (Fix)
zvol 삭제 시 `-r` 플래그를 추가하여 하위 스냅샷도 함께 삭제.
사용자에게 스냅샷 포함 삭제임을 응답에 명시.

## 관련 파일 (Files)
- `src/modules/storage/zfs_driver.c`
BODY
)" \
        "bug" "P2-major" "resolved"

    # --- #10 ---
    create_issue \
        "[Bug] VM 시작 실패 — CPU 코어 부족 오류" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
`vm.start` 호출 시 "insufficient CPU cores" 오류로 VM 시작 실패.
호스트에 유휴 CPU 코어가 충분한데도 시작 불가.

## 원인 (Root Cause)
`cpu_allocator.c`의 NUMA 인식 배타적 할당기가 이미 할당된 코어를
해제하지 않은 상태에서 중복 추적. VM 종료 시 코어 해제가 누락되어
가용 코어 수가 점진적으로 감소.

## 수정 (Fix)
VM 종료/삭제 시 할당된 CPU 코어를 정확히 반환하도록 해제 로직 추가.
할당 상태 추적 자료구조의 일관성 검증 코드 추가.

## 관련 파일 (Files)
- `src/modules/core/cpu_allocator.c`
- `src/modules/dispatcher/handler_vm_start.c`
- `src/modules/dispatcher/handler_vm_lifecycle.c`
BODY
)" \
        "bug" "P1-critical" "resolved"

    # --- #11 ---
    create_issue \
        "[Bug] noVNC 'RFB is not a constructor' 오류" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
Web UI에서 VM VNC 콘솔 접속 시 JavaScript 콘솔에
"RFB is not a constructor" 오류 발생. VNC 화면 미표시.

## 원인 (Root Cause)
noVNC 라이브러리 버전 업데이트 후 ES6 모듈 import 방식 변경.
기존 `new RFB(...)` 호출이 모듈 default export와 불일치.

## 수정 (Fix)
noVNC import 방식을 ES6 모듈 형식으로 수정.
`import RFB from './core/rfb.js'` 패턴 적용.

## 관련 파일 (Files)
- `ui/index.html`
BODY
)" \
        "bug" "P2-major" "resolved"

    # --- #12 ---
    create_issue \
        "[Bug] Node2/3 모니터링 메트릭 미수집" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
클러스터 모니터링에서 Node1의 메트릭만 표시되고
Node2, Node3의 CPU/메모리/VM 수가 수집되지 않음.

## 원인 (Root Cause)
`/internal/telemetry` 엔드포인트 호출 시 SoupSession의
GMainContext 충돌로 인해 피어 노드 요청이 데드락.
매 호출마다 새 Session을 생성하지 않아 발생.

## 수정 (Fix)
피어 텔레메트리 수집 시 매 호출마다 새 SoupSession 생성.
타임아웃(3초) 추가하여 응답 없는 노드는 건너뜀.

## 관련 파일 (Files)
- `src/modules/cluster/cluster_manager.c`
- `src/modules/daemons/telemetry.c`
BODY
)" \
        "bug" "P2-major" "resolved"

    # --- #13 ---
    create_issue \
        "[Bug] Grafana 데이터소스 URL 오류" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
Grafana 대시보드에서 Prometheus 데이터소스 연결 실패.
"Bad Gateway" 또는 "connection refused" 오류.

## 원인 (Root Cause)
Grafana 프로비저닝 설정에서 Prometheus URL이
`http://localhost:9090`으로 하드코딩되어 있으나,
LXC 컨테이너 환경에서는 호스트 IP 사용 필요.

## 수정 (Fix)
데이터소스 URL을 호스트 브릿지 IP(`10.0.3.1:9090`)로 수정.
프로비저닝 YAML에서 환경 변수로 오버라이드 가능하도록 변경.

## 관련 파일 (Files)
- Grafana 프로비저닝 설정 (외부 설정 파일)
- `docs/` 배포 가이드 문서
BODY
)" \
        "bug" "P3-minor" "resolved"

}

# ---------------------------------------------------------------------------
# 기능 이슈 #15 ~ #20
# ---------------------------------------------------------------------------
create_feature_issues() {
    info "=== 기능 이슈 생성 (6건) ==="

    # --- #15 ---
    create_issue \
        "[Feature] Proxmox 9.1 스타일 LXC 컨테이너 UI" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
Web UI에 컨테이너 관리 탭이 없어 CLI로만 컨테이너 조작 가능.

## 원인 (Root Cause)
컨테이너 UI가 미구현 상태. Web UI에 컨테이너 탭이 없음.

## 수정 (Fix)
Proxmox 9.1 스타일의 컨테이너 관리 UI 구현:
- 컨테이너 목록 (상태, IP, CPU, 메모리)
- 생성/시작/중지/삭제 액션
- exec 터미널
- 스냅샷 관리
- 메트릭 차트

## 관련 파일 (Files)
- `ui/index.html` (Web UI 컨테이너 탭)
BODY
)" \
        "feature" "resolved"

    # --- #16 ---
    create_issue \
        "[Feature] Swagger API Explorer (82개 엔드포인트)" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
REST API 문서가 CLAUDE.md에만 텍스트로 존재.
인터랙티브 API 탐색/테스트 도구 없음.

## 원인 (Root Cause)
OpenAPI/Swagger 스펙 및 UI 미구현.

## 수정 (Fix)
82개 REST 엔드포인트에 대한 Swagger UI 구현:
- OpenAPI 3.0 스펙 자동 생성
- Swagger UI 내장 (`/api/docs`)
- JWT 인증 지원
- 요청/응답 예시 포함

## 관련 파일 (Files)
- `ui/index.html` (Swagger 탭)
- `src/api/rest_server.c` (스펙 서빙 엔드포인트)
BODY
)" \
        "feature" "resolved"

    # --- #17 ---
    create_issue \
        "[Feature] VNC WebSocket 프록시 (noVNC 내장)" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
VM VNC 접속 시 외부 VNC 클라이언트 필요.
Web UI에서 직접 VNC 콘솔 접속 불가.

## 원인 (Root Cause)
libvirt VNC는 TCP raw 소켓 프로토콜이므로
웹 브라우저에서 직접 접속 불가. WebSocket 프록시 필요.

## 수정 (Fix)
WebSocket→TCP VNC 프록시 구현:
- libsoup3 WebSocket 핸들러
- noVNC 클라이언트 내장
- VM별 VNC 포트 자동 감지
- 토큰 기반 접근 제어

## 관련 파일 (Files)
- `src/api/rest_server.c` (WebSocket 프록시 핸들러)
- `ui/index.html` (noVNC 통합)
- `src/modules/dispatcher/handler_vnc.c`
BODY
)" \
        "feature" "resolved"

    # --- #18 ---
    create_issue \
        "[Feature] Prometheus + Grafana LXC 모니터링 스택" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
호스트/VM/컨테이너의 장기 메트릭 저장 및 시각화 도구 부재.
실시간 메트릭만 있고 히스토리컬 데이터 분석 불가.

## 원인 (Root Cause)
Prometheus 스크래핑 대상 설정 및 Grafana 대시보드 미구현.

## 수정 (Fix)
LXC 기반 모니터링 스택 구축:
- Prometheus LXC 컨테이너 (pcvpool/containers)
- Grafana LXC 컨테이너
- PureCVisor /metrics 엔드포인트 스크래핑
- 프리빌트 대시보드 (호스트/VM/컨테이너/클러스터)

## 관련 파일 (Files)
- `src/modules/daemons/prometheus_exporter.c`
- `src/api/rest_server.c` (`/metrics` 엔드포인트)
BODY
)" \
        "feature" "resolved"

    # --- #19 ---
    create_issue \
        "[Feature] VM pause/resume 구현" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
VM의 일시 정지(pause) 및 재개(resume) 기능 미지원.
start/stop만 가능하여 유지보수 시 VM을 완전히 중지해야 함.

## 원인 (Root Cause)
libvirt의 `virDomainSuspend`/`virDomainResume` API 호출이 미구현.

## 수정 (Fix)
VM pause/resume RPC 메서드 추가:
- `vm.pause` — virDomainSuspend 호출
- `vm.resume` — virDomainResume 호출
- CLI/REST/Web UI 전체 경로 구현
- 상태 표시에 "paused" 상태 추가

## 관련 파일 (Files)
- `src/modules/virt/vm_manager.c`
- `src/modules/dispatcher/handler_vm_lifecycle.c`
- `src/api/rest_server.c`
- `src/cli/pcvctl.c`
- `ui/index.html`
BODY
)" \
        "feature" "resolved"

    # --- #20 ---
    create_issue \
        "[Feature] 네이티브 모니터링 대시보드 (5페이지)" \
        "$(cat <<'BODY'
## 증상 (Symptoms)
Web UI에 기본 VM 목록만 존재하고 종합 모니터링 대시보드 부재.
호스트 리소스, 클러스터 상태, 네트워크 토폴로지를 한눈에 파악 불가.

## 원인 (Root Cause)
모니터링 전용 대시보드 페이지 미구현.

## 수정 (Fix)
5페이지 네이티브 모니터링 대시보드 구현:
1. **Overview** — 클러스터 요약 (노드 수, VM 수, 총 리소스)
2. **Host Metrics** — CPU/메모리/디스크/네트워크 실시간 차트
3. **VM Dashboard** — VM별 리소스 사용량 히트맵
4. **Network Topology** — OVS/OVN 네트워크 토폴로지 시각화
5. **Alerts & Events** — WebSocket 실시간 이벤트 로그

WebSocket 기반 실시간 업데이트, Chart.js 차트 라이브러리 사용.

## 관련 파일 (Files)
- `ui/index.html` (대시보드 탭들)
- `src/api/rest_server.c` (WebSocket 이벤트 핸들러)
- `src/modules/daemons/telemetry.c`
BODY
)" \
        "feature" "resolved"
}

# ---------------------------------------------------------------------------
# 메인 실행
# ---------------------------------------------------------------------------
main() {
    echo "=============================================="
    echo "  PureCVisor GitHub Issues 생성 스크립트"
    echo "  대상 저장소: $REPO"
    echo "=============================================="
    echo ""

    check_prerequisites
    echo ""

    create_labels
    echo ""

    create_milestone
    echo ""

    create_bug_issues
    echo ""

    create_feature_issues
    echo ""

    info "=============================================="
    info "  완료! 총 17개 이슈 처리됨"
    info "  확인: https://github.com/$REPO/issues"
    info "=============================================="
}

main "$@"
