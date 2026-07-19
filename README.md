# PureCVisor

> 단일 Linux/KVM 노드에서 VM, LXC 컨테이너, ZFS 스토리지, OVS/OVN 네트워크, 인증, 감사, 관측성, Web UI를 한 프로세스로 관리하는 C23 기반 하이퍼바이저 컨트롤 플레인입니다.

[![Edition: Single Edge](https://img.shields.io/badge/Edition-Single%20Edge-blue.svg)](docs/PUBLIC_RELEASE_BOUNDARY.md)
[![Runtime: Linux/KVM](https://img.shields.io/badge/Runtime-Linux%2FKVM-2f855a.svg)](docs/GUIDE.md)
[![Language: C23](https://img.shields.io/badge/Language-C23-555.svg)](AGENTS.md)
[![License: Non-Commercial Source](https://img.shields.io/badge/License-Non--Commercial%20Source-6b46c1.svg)](LICENSE)
[![Version: 1.4.1](https://img.shields.io/badge/Version-1.4.1-2f855a.svg)](include/purecvisor/version.h)

PureCVisor는 한 대의 Linux 서버를 작은 가상화 플랫폼처럼 다루기 위한 Single Edge 공개 소스 트리입니다. `purecvisorsd` 하나가 VM, 컨테이너, 스토리지, 네트워크, 권한, audit, metrics, Web UI를 같은 제어 흐름으로 묶습니다.

이 저장소는 복잡한 클러스터부터 시작하지 않습니다. 먼저 단일 노드에서 실제로 VM과 컨테이너를 만들고 운영하는 데 필요한 기능을 제공합니다. 전체 운영 매뉴얼은 [docs/GUIDE.md](docs/GUIDE.md), 공개판 경계는 [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md), 설계 결정은 [docs/ADR_INDEX.md](docs/ADR_INDEX.md)와 [docs/adr/](docs/adr/)를 기준으로 봅니다.

라이선스는 비상업용 무료 사용을 허용하는 source-available 라이선스입니다. 사용, 수정, 빌드, 배포 조건은 [LICENSE](LICENSE)를 확인하세요.

---

## 왜 필요한가요?

단일 노드 운영에서도 가상화 표면은 쉽게 흩어집니다. VM은 libvirt, 컨테이너는 LXC, 스토리지는 ZFS, 네트워크는 bridge/OVS/OVN, 권한과 audit는 별도 도구로 나뉘기 쉽습니다. PureCVisor는 이 표면을 하나의 운영 제어면으로 묶는 데 초점을 둡니다.

- VM lifecycle, LXC, ZFS, OVS/OVN, backup, auth를 한 제어면에서 다룹니다.
- REST, UDS JSON-RPC, gRPC, CLI, Web UI가 같은 dispatcher와 RBAC 정책을 통과합니다.
- 장시간 작업은 accepted 응답과 실제 완료 결과를 분리해 거짓 성공을 줄입니다.
- Job ID, WebSocket 완료 알림, polling, audit log로 작업 결과를 추적합니다.
- 공개판은 Single Edge 범위만 책임지고, 클러스터 자동화나 라이브 마이그레이션은 제외합니다.

---

## 빠른 시작

Ubuntu 22.04 LTS 또는 24.04 LTS 계열을 기준으로 합니다. 전체 설치 전제와 운영 절차는 [docs/GUIDE.md](docs/GUIDE.md)의 설치 장을 따릅니다.

<details>
<summary>Ubuntu 의존성 설치 예시</summary>

```bash
sudo apt update
sudo apt install -y \
  build-essential gcc-14 make pkg-config ccache \
  libglib2.0-dev libjson-glib-dev libsoup-3.0-dev \
  libvirt-dev libvirt-clients libvirt-daemon-system qemu-kvm \
  libguestfs-tools \
  libvirt-glib-1.0-dev liblxc-dev lxc lxc-utils \
  zfsutils-linux libsqlite3-dev libssl-dev \
  libcap-dev libseccomp-dev libreadline-dev liburing-dev \
  protobuf-c-compiler libprotobuf-c-dev \
  libncurses-dev libncursesw5-dev
```

</details>

```bash
make single
make test
make check-all
```

로컬 단일 노드에 배포한 뒤 health를 확인합니다.

```bash
scripts/deploy.sh --nodes local
curl -s http://localhost:80/api/v1/health | python3 -m json.tool
```

정상적인 Single Edge health 응답은 다음 성격을 유지해야 합니다.

```json
{
  "service": "purecvisorsd",
  "status": "ok",
  "version": "1.4.1",
  "capabilities": {
    "cluster": false
  }
}
```

릴리즈 빌드는 다음 명령으로 확인합니다.

```bash
make release
```

---

## 포함 범위

| 영역 | 제공 기능 |
|------|-----------|
| 데몬 | 단일 프로세스 `purecvisorsd`, `GMainLoop`, `GTask` 비동기 작업 |
| 인터페이스 | UDS JSON-RPC, REST API, gRPC(토큰 인증·RBAC), WebSocket, CLI, Web UI |
| VM | 생성, 시작, 중지, 삭제, 스냅샷, 리소스 조정, 안전 조건부 VM clone |
| 컨테이너 | LXC 생성, 실행, 명령 실행, 리소스 제한 |
| 스토리지 | ZFS pool, zvol, snapshot, scrub, quota, 백업(증분·S3 export)과 스냅샷 리텐션 |
| 네트워크 | bridge, NAT, isolated, routed, OVS/OVN local SDN, QoS·오버레이 재부팅 재수화 |
| 보안 | JWT, RBAC(PBKDF2 해시), operator VM/컨테이너 owner-scope, bootstrap admin fallback, API key 만료 집행, **audit 해시체인 무결성**, **AppArmor MAC 프로필(호스트 데몬 심층방어)**, **SSRF/CORS 하드닝**, **mTLS·전송 강제(opt-in)**, audit log |
| 관측성 | health check, Prometheus metrics, WebSocket event stream, 알림 에스컬레이션·음소거·DLQ |
| AI Ops | 이상탐지 메트릭 트리거, 합의 최소 정족수, VM 자동 재시작 self-healing(기본 `dry_run`)과 재시작 서킷브레이커 |
| 운영 | systemd 배포, release build, Single Edge 공개판 검증 스크립트, **AppArmor MAC 프로필 토글**(complain↔enforce) |

Single Edge 공개판에 포함하지 않는 기능은 다음과 같습니다.

- 상용 Multi Edge 제어면
- 멀티 노드 클러스터 자동화
- 라이브 마이그레이션
- 페더레이션
- 노드 드레인, 리밸런싱, 분산 스케줄링

현재 공개판 경계의 단일 진실은 [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md)입니다.

---

## 어떻게 동작하나요?

```text
CLI / Web UI / REST Client
        |
        v
UDS JSON-RPC Server + REST Server + WebSocket
        |
        v
Dispatcher
  method policy / RBAC pre-route / VM owner-scope
        |
        v
Handlers
        |
        v
Core modules: VM, LXC, ZFS, Network, Auth, Audit, Metrics
        |
        v
System: libvirt, qemu-kvm, LXC, ZFS, OVS/OVN, nftables, dnsmasq
```

요청은 dispatcher에서 메서드 정책, RBAC, VM owner-scope를 먼저 통과합니다. 짧은 작업은 즉시 성공/오류 응답을 반환하고, 긴 작업은 먼저 accepted 응답을 보낸 뒤 worker callback에서 실제 결과를 기록합니다.

fire-and-forget 작업의 완료 경로는 다음 패턴을 따릅니다.

```text
client request
  -> accepted response with job id
  -> GTask worker
  -> pcv_audit_log(actual result)
  -> pcv_ws_broadcast_job_complete
  -> polling endpoint remains available
```

이 규칙은 [ADR-0018](docs/adr/0018-fire-and-forget-audit-policy.md)과 [docs/ADR_INDEX.md](docs/ADR_INDEX.md)의 현재 적용 상태를 기준으로 유지합니다.

---

## 기본 접속 정보

| 인터페이스 | 기본 경로 |
|------------|-----------|
| Web UI | `http://localhost:80/ui/` |
| 이벤트 센터 | `http://localhost:80/ui#/ops-triage` |
| REST API | `http://localhost:80/api/v1/` |
| Health | `http://localhost:80/api/v1/health` |
| Metrics | `http://localhost:80/api/v1/metrics` |
| UDS socket | `/var/run/purecvisor/daemon.sock` |

첫 설치 bootstrap 계정은 운영 전 반드시 전용 관리자 계정으로 교체해야 합니다. 셀프 회원가입은 `[auth] allow_self_register` 설정으로 제어하며, 기본값은 비활성화입니다.

---

## 사용 예시

토큰을 발급합니다.

```bash
TOKEN=$(curl -s -X POST http://localhost:80/api/v1/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<configured-admin-password>"}' \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['access_token'])")
```

VM을 만들고 시작합니다.

```bash
pcvctl vm create web01 --vcpu 2 --memory_mb 2048 --disk_size_gb 20
pcvctl vm start web01
pcvctl vm list
```

REST API로 VM 목록을 조회합니다.

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms | python3 -m json.tool
```

UDS JSON-RPC를 직접 호출할 수도 있습니다.

```bash
echo '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

---

## 검증 명령

변경 유형별 검증 깊이는 [docs/DEVELOPMENT_VERIFICATION_POLICY.md](docs/DEVELOPMENT_VERIFICATION_POLICY.md)를 따릅니다. 자주 쓰는 기준 명령은 다음과 같습니다.

```bash
make single
make test
make check-all
make release
```

`make check-all`은 RBAC 정책 정합과 RPC 소비⊆등록 계약(FE/CLI가 호출하는 모든 RPC의 등록 여부)을 함께 검사합니다.

Web UI 번들, 디자인 표면, XSS 경계를 바꾼 경우:

```bash
python3 scripts/check_design_md.py
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
node --check ui/app.bundle.js
python3 scripts/check_xss.py
```

Single Edge 공개판 경계를 바꾼 경우:

```bash
tests/integration/test_single_ui_surface.sh
tests/integration/test_single_backend_build_boundaries.sh
tests/integration/test_single_ovn_ovs_layout.sh
```

공개 소스 기본 점검:

```bash
rg -n "\b(TODO|FIXME|HACK|XXX)\b" src include tests ui/modules scripts
git diff --check
```

첫 번째 명령은 일반 소스 영역에서 남은 작업 표식이 없는지 확인합니다. 출력이 없어야 정상입니다.

Web UI 시각 규격은 루트 [DESIGN.md](DESIGN.md)를 기준으로 관리합니다. UI 모듈, Service Worker, vendor 자산, `ui/samples/` 프리뷰를 바꾼 경우 공개 배포 전 외부 런타임 참조가 남지 않았는지 확인합니다.

```bash
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" ui/index.html ui/guide.html ui/app.bundle.js ui/sw.js ui/vendor
```

---

## 저작 기록

PureCVisor의 공개 소스 코드와 문서는 AI 코딩 에이전트(CODEX, Claude)가 작성하고 유지보수합니다. `Hardcoremonk`는 요구사항, 제품 방향, UX 판단, 사용자 테스트, 검수를 담당합니다. 코드는 직접 수정하지 않고, 도구를 통해 생성된 결과물을 검토하고 결정합니다.

| 역할 | 담당 |
|------|------|
| 코드 작성 / 리팩토링 / 유지보수 | AI 코딩 에이전트 (CODEX, Claude) |
| 모든 문서 (`README.md`, `DESIGN.md`, `docs/**`) | AI 코딩 에이전트 (CODEX, Claude) |
| 사양 결정 / 요구사항 / UX 판단 / 검수 | `Hardcoremonk` |
| 라이선스/저작권 보유 | `Hardcoremonk` |

작업 세션마다 추측 금지, 자동 재시도 금지, 보안 우선 같은 내부 원칙을 적용합니다. 새 기능 추가와 버그 수정도 같은 방식으로 진행하며, 관련 커밋에는 작성 에이전트의 `Co-Authored-By` 트레일러를 남깁니다.

---

## 저장소 구조

| 경로 | 설명 |
|------|------|
| `src/api/` | UDS, REST, WebSocket, middleware |
| `src/modules/dispatcher/` | JSON-RPC handler 계층 |
| `src/modules/virt/` | libvirt 기반 VM 관리 |
| `src/modules/storage/` | ZFS driver와 스토리지 기능 |
| `src/modules/network/` | bridge, firewall, DHCP, OVS/OVN local networking |
| `src/modules/auth/` | RBAC, 사용자, API key, JWT 관련 로직 |
| `src/modules/audit/` | audit log |
| `src/bootstrap/` | Single Edge bootstrap과 공개판 stub |
| `ui/` | Vanilla JS Web UI |
| `tests/` | 단위, 통합, 경계 검증 |
| `docs/` | 운영, 개발, 공개판 경계 문서 |
| `scripts/` | 빌드, 번들, 배포, 검증 보조 스크립트 |

---

## 문서 지도

| 문서 | 용도 |
|------|------|
| [AGENTS.md](AGENTS.md) | Codex 작업 규칙, 명령, 불변 조건 |
| [DESIGN.md](DESIGN.md) | Web UI 시각 규격, token, typography, component state |
| [docs/GUIDE.md](docs/GUIDE.md) | 제품, 설치, 운영 통합 가이드 |
| [docs/DEVELOPMENT_VERIFICATION_POLICY.md](docs/DEVELOPMENT_VERIFICATION_POLICY.md) | 단계별 검증 기준 |
| [docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md](docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md) | 서비스 기능 테스트 시나리오 기준 |
| [docs/DATABASE_STRUCTURE.md](docs/DATABASE_STRUCTURE.md) | SQLite DB 구조와 영속 상태 계약 |
| [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md) | Single Edge 공개판 경계 |
| [docs/ADR_INDEX.md](docs/ADR_INDEX.md) | ADR별 현재 적용 상태 |
| [docs/adr/](docs/adr/) | 설계 결정 기록 |
| [docs/agents/](docs/agents/) | agent workflow, issue tracker, triage label 규칙 |

---

## 개발 규칙 요약

- C 표준은 `gnu23`입니다.
- 목표는 빌드 경고 0건입니다.
- 공개본 소스에는 설명 주석을 남기지 않습니다.
- 단일 프로세스 + `GMainLoop` 실행 모델을 유지합니다.
- 장시간 RPC는 accepted 응답과 worker callback의 실제 결과 audit를 분리합니다.
- 프론트엔드는 Vanilla JS와 `PCV.*` 네임스페이스를 유지합니다.
- API endpoint는 `ui/modules/endpoints.js`의 registry를 사용합니다.
- 신규 UI 코드는 `innerHTML` 계열 대입을 금지하고 DOM API로 조립합니다. 남은 레거시 접점은 sanitizer를 경유하며 래칫으로 점진 축소합니다.
- VM/템플릿 이름은 handler 진입점에서 검증 함수를 경유합니다.
- secrets, 토큰, 비밀번호는 commit하지 않습니다.

---

## 공개 저장소 기준

문서 예시의 표준 운영 URL은 `https://purecvisor.example.com`입니다. 호환 공개 엔드포인트 예시는 `https://purecvisor-compat.example.com`입니다.

공개용 저장소는 기존 개발 저장소의 `.git` 이력을 그대로 공개하지 않습니다. 공개 기준은 릴리스 태그별로 민감정보 검색과 공개 범위 검증을 통과한 sanitized 스냅샷을 추출해, 이 저장소에 릴리스 단위 커밋과 동일 버전 태그로 순차 반영하는 방식입니다. 버전별 변경 내역은 [CHANGELOG.md](CHANGELOG.md)와 [Releases](https://github.com/HardcoreMonk/purecvisor/releases)를 참고하세요.

현재 문서의 공개용 예시는 다음 placeholder를 사용합니다.

| 항목 | 공개용 예시 |
|------|-------------|
| 표준 도메인 | `purecvisor.example.com` |
| 호환 도메인 | `purecvisor-compat.example.com` |
| 운영 노드 | `pcv-prod-node-1`, `pcv-prod-node-2` |
| 내부 IP | `192.0.2.10`, `192.0.2.20` |
| bootstrap 비밀번호 | `<configured-admin-password>` |

공개판 기능과 제외 범위는 [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md)를 기준으로 판정합니다.
