# PureCVisor

PureCVisor는 한 대의 Linux 서버를 작은 가상화 플랫폼처럼 다루기 위한 C23 기반 컨트롤 플레인입니다.

KVM VM, LXC 컨테이너, ZFS 스토리지, OVS/OVN 네트워크, 백업, 인증, Web UI를 한 흐름에서 관리합니다.

이 공개 저장소는 Single Edge 구성에 초점을 맞춥니다.

복잡한 클러스터부터 시작하지 않고,
먼저 단일 노드에서 실제로 VM과 컨테이너를 만들고 운영하는 데 필요한 기능을 담았습니다.

라이선스는 비상업용 무료 사용을 허용하는 source-available 라이선스입니다.

자세한 조건은 [LICENSE](LICENSE)를 확인하세요.

## 저작 기록 (Authorship)

PureCVisor의 공개 소스 코드와 문서는 CODEX가 작성하고 유지보수합니다.

`Hardcoremonk`는 요구사항, 제품 방향, UX 판단, 사용자 테스트, 검수를 담당합니다.

코드는 직접 수정하지 않고, 도구를 통해 생성된 결과물을 검토하고 결정합니다.

| 역할 | 담당 |
|------|------|
| 코드 작성 / 리팩토링 / 유지보수 | CODEX |
| 모든 문서 (`README.md`, `DESIGN.md`, `docs/**`) | CODEX |
| 사양 결정 / 요구사항 / UX 판단 / 검수 | `Hardcoremonk` (인간) |
| 라이선스/저작권 보유 | `Hardcoremonk` (사용자가 도구를 통해 생성한 결과물의 권리) |

작업 세션마다 추측 금지, 자동 재시도 금지, 보안 우선 같은 내부 원칙을 적용합니다.

새 기능 추가와 버그 수정도 같은 방식으로 진행하며, 관련 커밋에는 `Co-Authored-By: CODEX` 트레일러를 남깁니다.

문서 예시의 표준 운영 URL은 `https://purecvisor.example.com`입니다.

## 무엇을 제공하나요

공개판에서 바로 다루는 영역은 다음과 같습니다.

- 단일 노드 데몬: `purecvisorsd`
- 인터페이스: UDS JSON-RPC, REST API, Web UI, CLI, TUI
- VM lifecycle: 생성, 시작, 중지, 삭제, 스냅샷, 리소스 조정, ADR-0023 기준의 안전 조건부 VM clone
- 컨테이너 관리: LXC 기반 생성, 실행, 명령 실행, 리소스 제한
- 스토리지: ZFS pool, zvol, snapshot, scrub, quota
- 네트워크: bridge, NAT, isolated, routed, OVS/OVN local SDN
- 보안: JWT, RBAC, bootstrap admin fallback, audit log
- 관측성: health check, Prometheus metrics, WebSocket event stream
- 운영: systemd 배포, release build, Single Edge 공개판 검증 스크립트

공개판 경계의 단일 진실은 [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md)입니다.

## 어떻게 동작하나요

```text
CLI / TUI / Web UI / REST Client
        |
        v
UDS JSON-RPC Server + REST Server
        |
        v
Dispatcher  method policy / RBAC pre-route / VM owner-scope
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

핵심은 단일 프로세스 데몬과 `GMainLoop` 이벤트 루프입니다.

요청은 디스패처를 지나면서 role 정책과 operator VM owner-scope 검사를 받습니다.

오래 걸리는 작업은 먼저 Job ID를 돌려주고 `GTask` 워커에서 실행합니다.

완료 결과는 WebSocket push와 polling으로 확인할 수 있습니다.

## 빠른 시작

### 1. 의존성 설치

Ubuntu 22.04 LTS 또는 24.04 LTS 계열을 기준으로 합니다.

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

### 2. 빌드와 테스트

```bash
make single
make test
make check-rbac
```

릴리즈 빌드:

```bash
make release
```

Web UI를 바꿨다면 번들과 기본 안전 검사를 함께 확인합니다.

```bash
python3 scripts/check_design_md.py
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
node --check ui/app.bundle.js
python3 scripts/check_xss.py
```

Web UI 시각 규격은 루트 [DESIGN.md](DESIGN.md)를 기준으로 관리합니다.

운영 이벤트를 한 화면에서 triage하는 `이벤트 센터`는 `운영 > 이벤트 센터` 또는 `/ui#/ops-triage`에서 확인합니다.

UI 모듈, Service Worker, vendor 자산, `ui/samples/` 프리뷰를 바꾼 경우 공개 배포 전 외부 런타임 참조가 남지 않았는지 확인합니다.

```bash
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" ui/index.html ui/guide.html ui/app.bundle.js ui/sw.js ui/vendor
```

Single Edge 공개판 경계를 건드렸다면 아래 검증을 우선 실행합니다.

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

변경 유형별 검증 깊이는 [docs/DEVELOPMENT_VERIFICATION_POLICY.md](docs/DEVELOPMENT_VERIFICATION_POLICY.md)를 따릅니다.

### 3. 로컬 배포

```bash
scripts/deploy.sh --nodes local
```

서비스 확인:

```bash
sudo systemctl status purecvisorsd
curl -s http://localhost:80/api/v1/health | python3 -m json.tool
```

정상적인 Single Edge health 응답은 `service=purecvisorsd`, `capabilities.cluster=false`를 유지해야 합니다.

## 기본 접속 정보

| 인터페이스 | 기본 경로 |
|------------|-----------|
| Web UI | `http://localhost:80/ui/` |
| Web UI 이벤트 센터 | `http://localhost:80/ui#/ops-triage` |
| REST API | `http://localhost:80/api/v1/` |
| Health | `http://localhost:80/api/v1/health` |
| Metrics | `http://localhost:80/api/v1/metrics` |
| UDS socket | `/var/run/purecvisor/daemon.sock` |

첫 설치 bootstrap 계정은 운영 전 반드시 전용 관리자 계정으로 교체해야 합니다.

셀프 회원가입은 `[auth] allow_self_register` 설정으로 제어하며, 기본값은 비활성화입니다.

## 예시

토큰 발급:

```bash
TOKEN=$(curl -s -X POST http://localhost:80/api/v1/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<configured-admin-password>"}' \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['access_token'])")
```

VM 생성:

```bash
pcvctl vm create web01 --vcpu 2 --memory_mb 2048 --disk_size_gb 20
pcvctl vm start web01
pcvctl vm list
```

REST API로 VM 목록 조회:

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms | python3 -m json.tool
```

UDS JSON-RPC 호출:

```bash
echo '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

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

## 개발 문서

- [DESIGN.md](DESIGN.md): Web UI 시각 규격, token, typography, component state, dashboard density
- [docs/GUIDE.md](docs/GUIDE.md): 제품, 설치, 운영 통합 가이드
- [docs/DEVELOPMENT_VERIFICATION_POLICY.md](docs/DEVELOPMENT_VERIFICATION_POLICY.md): 단계별 검증 기준
- [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md): Single Edge 공개판 경계
- [docs/ADR_INDEX.md](docs/ADR_INDEX.md): ADR별 현재 Single Edge 적용 상태
- [docs/adr/](docs/adr/): 설계 결정 기록

## 개발 규칙 요약

- C 표준은 `gnu23`입니다.
- 목표는 빌드 경고 0건입니다.
- 프론트엔드는 Vanilla JS와 `PCV.*` 네임스페이스를 유지합니다.
- API endpoint는 `ui/modules/endpoints.js`의 registry를 사용합니다.
- `innerHTML` 사용 시 sanitizer를 반드시 거치거나 DOM API를 사용합니다.
