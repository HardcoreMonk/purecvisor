# PureCVisor Single Edge 운영 가이드

> **Single Edge Edition** | 공개 배포용 독립 노드 가이드 | 2026-05-04 | `purecvisorsd` | 공개 도메인/edge nginx 기준 현행화
>
> **현재 범위**: 이 문서는 `purecvisor-single` 기준으로 정리되며, Single Edge에서 실제로 제공하는 기능과 운영 절차만 안내합니다.
>
> **공개 운영 기준**: 2026-05-04 KST부터 표준 공개 URL은 `https://purecvisor.example.com`이며, `pcv-prod-node-1`(`192.0.2.10`)의 edge nginx가 TLS 종료와 reverse proxy를 담당합니다. `https://purecvisor-compat.example.com`는 호환 공개 엔드포인트로 유지하며, `pcv-prod-node-2`(`192.0.2.20`)와 같은 릴리스 산출물인지 함께 검증합니다.
>
> **단축키**: `/` 검색 · `n` 새 VM · `?` 단축키 도움말 · `g` 대시보드 · `m` 모니터링

---

## 목차

1. [시작하기](#1-시작하기)
2. [설치 및 환경 구성](#2-설치-및-환경-구성)
3. [VM 관리](#3-vm-관리)
4. [컨테이너 관리](#4-컨테이너-관리)
5. [스토리지](#5-스토리지)
6. [네트워크](#6-네트워크)
7. [멀티 제어면 참고 기록](#7-멀티-제어면-참고-기록)
8. [모니터링 & 알림](#8-모니터링--알림)
9. [백업 & 복원](#9-백업--복원)
10. [보안](#10-보안)
11. [클라우드 마이그레이션](#11-클라우드-마이그레이션)
12. [AI & 자가치유](#12-ai--자가치유)
13. [Web UI](#13-web-ui)
14. [REST API](#14-rest-api)
15. [CLI 레퍼런스](#15-cli-레퍼런스)
16. [설정 레퍼런스](#16-설정-레퍼런스)
17. [트러블슈팅](#17-트러블슈팅)
18. [부록](#18-부록)
19. [개발자 & 엔지니어 가이드](#19-개발자-엔지니어-가이드)
20. [영업 & 마케팅 가이드](#20-영업-마케팅-가이드)
21. [아키텍처 리팩토링 가이드](#21-아키텍처-리팩토링-가이드)
22. [품질 게이트 가이드](#22-품질-게이트-가이드)

---

## 1. 시작하기

> **코어 개발자 문서 지도**: 아키텍처, 개발, 검증, 배포, 운영 문서를 작업 단계 기준으로 찾으려면 [DEVELOPER_INDEX.md](DEVELOPER_INDEX.md)를 먼저 보세요.
> **검증 운영 문서**: 개발 단계별 검증 기준은 [DEVELOPMENT_VERIFICATION_POLICY.md](DEVELOPMENT_VERIFICATION_POLICY.md)를 참조하세요. 이 문서는 `Level 1 로컬 코드 검증`부터 `Level 4 출시 게이트`까지의 공식 규칙을 정의합니다.

### 1.1 PureCVisor란?

PureCVisor Single Edge는 C23 기반 KVM 하이퍼바이저 오케스트레이터입니다. 단일 프로세스 데몬 `purecvisorsd`가 fork 없이 GMainLoop 이벤트 루프로 동작하며, VM, 컨테이너, 스토리지, 네트워크를 독립 노드 기준으로 통합 관리합니다.

**핵심 특징:**

| 특징 | 설명 |
|------|------|
| 단일 프로세스 아키텍처 | fork 없이 GMainLoop + GTask 스레드 풀로 동작 |
| 3계층 인터페이스 | CLI + TUI + Web UI |
| 에디션 전용 RPC 집합 | GHashTable O(1) 디스패치, 플러그인 동적 확장 |
| REST API | JWT HS256 + RBAC + VM owner-scope + CORS + gzip 압축 |
| Prometheus 메트릭 | 내장 exporter 기반 상태 노출 |
| 배포 형태 | Single Edge (독립 노드) |
| C23 코드베이스 | Single Edge 데몬, CLI/TUI, Web UI, 테스트, 운영 스크립트 |

### 1.2 아키텍처 개요

```
클라이언트 (pcvctl / pcvtui / Web UI / REST API)
        |
        v
+-------+-------+--------------------+
| UDS 서버              | REST 서버           |
| (JSON-RPC 2.0)        | (HTTP :80 / HTTPS :443) |
| io_uring 비동기 I/O   | libsoup3, JWT, CORS  |
+-------+-------+--------------------+
        |
        v
  디스패처 (Single Edge RPC, O(1) GHashTable 라우팅)
  method policy / RBAC pre-route / VM owner-scope
        |
        v
  핸들러 계층 (src/modules/dispatcher/ + src/modules/network/)
        |
        v
  코어 모듈
  +-- vm_manager (libvirt)     +-- lxc_driver (LXC)
  +-- network_manager (OVS/OVN) +-- zfs_driver (ZFS)
  +-- auth_manager (SQLite)     +-- backup_scheduler
        |
        v
  시스템 (libvirt, nftables, dnsmasq, ZFS, LXC, OVS, OVN)
```

**요청 처리 흐름 상세:**

1. 클라이언트가 UDS 소켓 또는 REST API로 요청 전송
2. REST 서버는 JWT로 인증 주체를 검증하고, RBAC DB의 현재 role을 JSON-RPC params에 주입해 디스패처에 전달
3. 디스패처가 GHashTable에서 O(1)로 핸들러 함수 조회 후 method policy, RBAC, VM owner-scope를 검사
4. 핸들러가 코어 모듈을 호출하여 작업 수행
5. 장시간 작업은 fire-and-forget 패턴으로 즉시 응답 후 GTask 비동기 실행

### 1.2.1 검증 문서 맵

문서 역할은 다음처럼 나눠서 봐야 합니다.

| 문서 | 역할 |
|------|------|
| [DEVELOPMENT_VERIFICATION_POLICY.md](DEVELOPMENT_VERIFICATION_POLICY.md) | 개발 단계별 검증 규칙, Level 1~4 운영 기준 |
| [DEVELOPER_INDEX.md](DEVELOPER_INDEX.md) | 현재 추출 리포 기준 개발 문서 진입점 |
| [ADR_INDEX.md](ADR_INDEX.md) | ADR별 현재 Single Edge 적용 상태 |
| `docs/adr/` | 설계 결정과 예외 규칙의 단일 진실 |
| `docs/superpowers/specs/` | 추출 설계와 세션 산출물 |

### 1.2.2 설계 결정 빠른 보기

운영 가이드 본문에서 `ADR-0023`처럼 표시되는 항목은 단순 참고 문구가 아니라 기능의 허용 조건과 예외 규칙이다. `ui/guide.html`에서는 ADR 번호를 클릭하면 요약과 원문 경로가 열린다.

| 설계 결정 | 먼저 봐야 하는 상황 | 현재 Single Edge 결론 |
|-----------|--------------------|-----------------------|
| [ADR-0023: VM clone 오픈 베타 안전장치](adr/0023-vm-clone-beta-safety-guard.md) | VM clone, Guest reset, Prepared template, power on 거부 조건 | source VM은 `shut off` 상태여야 하며, prepared template 또는 libguestfs 기반 Guest reset 중 하나가 필요하다 |
| [ADR-0022: VM 생성 저장 위치 계약](adr/0022-vm-create-storage-location-contract.md) | VM 생성 저장소, zvol/qcow2/raw 위치 정책 변경 | `storage_type`과 `storage_pool` 또는 `image_dir` 조합을 명시 계약으로 유지한다 |
| [ADR-0019: RBAC UDS 우회 정책](adr/0019-rbac-uds-bypass-policy.md) | 권한, operator owner-scope, UDS/REST 보안 변경 | REST 인증과 dispatcher method policy를 모두 유지하고 `make check-rbac`로 검증한다 |
| [ADR-0018: fire-and-forget audit 기록 정책](adr/0018-fire-and-forget-audit-policy.md) | 장시간 RPC, worker callback, audit/WS completion 변경 | accepted 응답은 완료가 아니며 worker callback에서 실제 결과 audit를 남긴다 |
| [ADR-0014: JWT Bearer 전용 인증](adr/0014-remove-csrf-jwt-bearer.md) | REST 인증, CSRF, 브라우저 호출 모델 설명 | 쿠키 세션 대신 `Authorization: Bearer <JWT>`를 사용하고 별도 CSRF 토큰을 운영하지 않는다 |
| [ADR-0013: 프론트엔드 IIFE 모듈 스코프](adr/0013-frontend-iife-module-scope.md) | Web UI 모듈, endpoint registry, sanitizer 변경 | Vanilla JS를 유지하고 `PCV.*` 네임스페이스와 `EP` 레지스트리를 사용한다 |

### 1.3 Single Edge 공개 리포지토리

> 📦 **공개 배포 저장소** — LinkedIn/GitHub 공개 시에는 민감정보 정리가 끝난 HEAD를 `git archive`로 추출해 새 공개 저장소의 첫 커밋으로 올립니다. 기존 개발 저장소의 `.git` 이력은 공개 저장소에 가져가지 않습니다. 공개 URL은 생성 후 `<public-repo-url>`로 확정합니다.
>
> - 공개 배포 기준 바이너리: `bin/purecvisorsd`
> - systemd 서비스: `purecvisorsd.service`
> - `/api/v1/health` 기준 런타임 모드: `cluster=false`, `node_name=standalone`

이 가이드는 싱글 노드 운영과 출시 범위만 다룹니다. 클러스터 제어면, 라이브 마이그레이션, 페더레이션, 노드 드레인/리밸런싱 같은 멀티 노드 기능은 이 에디션의 지원 범위에 포함되지 않습니다.

#### 공개 범위 핵심

1. **독립 노드 운영**: `purecvisorsd` 하나로 VM, 컨테이너, 스토리지, 네트워크를 통합 관리합니다.
2. **수동/로컬 네트워크**: OVS 오버레이와 OVN 로컬 SDN 코어는 포함되지만, 클러스터 자동화는 포함되지 않습니다.
3. **출시 게이트 재인증 완료**: VM lifecycle, Storage, Network, Backup/Restore, Auth/RBAC, Single Edge OVN/OVS, Longrun 축이 다시 통과한 상태를 기준으로 정리합니다.

### 1.4 5분 퀵스타트

```bash
# 1. 서비스 시작
sudo systemctl start purecvisorsd   # Single Edge

# 2. 상태 확인
curl -s http://localhost:80/api/v1/health | python3 -m json.tool
```

정상 응답 예시:

```json
{
    "capabilities": {
        "ovn": true,
        "dpdk": false,
        "cluster": false
    },
    "status": "ok",
    "service": "purecvisorsd",
    "version": "1.1.1",
    "node_name": "standalone"
}
```

제품 버전의 현재 공개 표기는 `1.1`이다(태그 `v1.1.1`). 소스 기준 단일 값은 `include/purecvisor/version.h`의 `PCV_PRODUCT_VERSION`이며, `/api/v1/health`, `/api/v1/version`, `pcvctl --version`, Prometheus `purecvisor_info`, Web UI config와 HTML 정적 자산 query string은 같은 릴리스 단위로 맞춘다. `/api/v1` 같은 API path, OpenAPI spec version, Prometheus text format version, 라이브러리 ABI symbol은 제품 버전이 아니므로 별도 계약으로 유지한다.

```bash
# 3. bootstrap admin으로 첫 인증 토큰 발급
# 첫 설치에는 내장 기본 비밀번호가 없습니다.
# daemon.conf 또는 PURECVISOR_ADMIN_PASSWORD로 bootstrap 비밀번호를 먼저 설정합니다.
TOKEN=$(curl -s -X POST http://localhost:80/api/v1/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<configured-admin-password>"}' | python3 -c "import sys,json;print(json.load(sys.stdin)['access_token'])")

echo "Token: $TOKEN"

# 4. VM 생성
pcvctl vm create web-prod --vcpu 2 --memory_mb 2048 --disk_size_gb 20

# 5. VM 시작
pcvctl vm start web-prod

# 6. VM 목록 확인
pcvctl vm list

# 7. Web UI 접속
echo "http://localhost:80/ui/ (admin / configured password)"
```

### 1.5 접속 정보 요약

| 인터페이스 | 주소 | 인증 |
|-----------|------|------|
| UDS 소켓 | `/var/run/purecvisor/daemon.sock` | 없음 (로컬) |
| REST API | `http://localhost:80/api/v1/` | JWT HS256 |
| HTTPS | `https://localhost:443/api/v1/` | JWT HS256 + TLS |
| Web UI | `http://localhost:80/ui/` | bootstrap admin `admin / configured password` |
| WebSocket (이벤트) | `ws://localhost:80/api/v1/ws/events` | JWT |
| WebSocket (VNC) | `ws://localhost:80/api/v1/ws/vnc` | JWT |
| Prometheus | `http://localhost:80/api/v1/metrics` | 없음 |
| Health | `http://localhost:80/api/v1/health` | 없음 |

### 1.6 수동 RPC 테스트

UDS 소켓을 통해 직접 JSON-RPC 요청을 보낼 수 있습니다.

```bash
# VM 목록 조회
echo '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 호스트 메트릭 조회
echo '{"jsonrpc":"2.0","method":"telemetry.host","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

---

## 2. 설치 및 환경 구성

### 2.1 시스템 요구사항

#### 하드웨어

| 항목 | 최소 | 권장 | 비고 |
|------|------|------|------|
| CPU | 4코어 | 8코어+ | VT-x/AMD-V 필수 |
| RAM | 8GB | 32GB+ | VM 할당량에 따라 |
| 디스크 | 100GB SSD | ZFS SSD 풀 1TB+ | ZFS는 ECC RAM 권장 |
| 네트워크 | 1GbE | 10GbE | 오버레이/마이그레이션용 |

#### 소프트웨어

| 항목 | 최소 | 권장 |
|------|------|------|
| OS | Ubuntu 22.04 LTS | Ubuntu 24.04 LTS |
| 커널 | 5.15+ | 6.x HWE (io_uring 최적) |
| libvirt | 8.0+ | 10.0+ |
| ZFS | 2.1+ | 2.2+ (HWE 커널 동기화 필수) |
| GLib | 2.72+ | 2.78+ |

> **ZFS 버전 동기화**: HWE 커널 업그레이드 시 ZFS 유저랜드 버전도 함께 확인해야 합니다. 현재 추출 리포에서는 설치 장, `Makefile`, 관련 ADR을 기준으로 판단합니다.

### 2.2 패키지 설치

#### 방법 A — `.deb` 바이너리 패키지 (권장, Ubuntu 24.04)

릴리스 `.deb`(`purecvisor-single_<version>_amd64.deb`)로 데몬·CLI·UI·systemd 유닛을 한 번에 설치한다. 런타임 의존(libvirt/qemu/dnsmasq/nftables 등)은 apt가 자동 해결한다.

```bash
# 의존성 자동 해결 포함 설치 (dpkg -i 대신 apt 사용 권장)
sudo apt install -y ./purecvisor-single_1.1.0_amd64.deb

# 최초 설치 시 /etc/purecvisor/daemon.conf 가 sample 기반으로 생성된다.
# admin_password 등 필수 항목 편집 후 시작:
sudo nano /etc/purecvisor/daemon.conf
sudo systemctl start purecvisorsd
systemctl status purecvisorsd
pcvctl --version          # → pcvctl 1.1 (Single Edge)
```

- 설치 위치: 바이너리 `/usr/local/bin/{purecvisorsd,pcvctl,pcvtui}`, UI `/usr/local/share/purecvisor/ui/`, 유닛 `/etc/systemd/system/purecvisorsd.service`, 설정 `/etc/purecvisor/`.
- REST(:8080)를 외부에 노출하려면 nginx 리버스 프록시(:80/:443 → 127.0.0.1:8080)를 **별도 구성**한다(패키지 미포함).
- 업그레이드: 새 `.deb`로 같은 명령 재실행. 기존 `daemon.conf`는 보존된다(conffile prompt 시 `--force-confold`로 유지 또는 `--force-confnew`로 새 유닛 채택).
- 제거: `sudo apt remove purecvisor-single` (설정 보존) / `sudo apt purge purecvisor-single` (설정 포함 제거).

#### 방법 B — 소스 빌드

##### 빌드 의존성

```bash
sudo apt update && sudo apt install -y \
    build-essential gcc-14 make pkg-config ccache \
    libglib2.0-dev libncurses-dev libncursesw5-dev \
    libvirt-dev libvirt-clients libvirt-daemon-system qemu-kvm \
    libguestfs-tools \
    libsoup-3.0-dev libjson-glib-dev \
    libvirt-glib-1.0-dev liblxc-dev \
    zfsutils-linux lxc lxc-utils \
    libsqlite3-dev libssl-dev \
    libcap-dev libseccomp-dev libreadline-dev liburing-dev \
    protobuf-c-compiler libprotobuf-c-dev
```

> **참고**: `libvirt-glib-1.0-dev`가 `libvirt-gobject-1.0.pc`도 함께 제공합니다. 별도의 `libvirt-gobject-1.0-dev` 패키지는 존재하지 않습니다.

> **운영 필수**: `libguestfs-tools`는 일반 VM 복제의 Guest reset 경로에 필요하다. 이 패키지가 없으면 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish`를 실행할 수 없어 `guest_reset=true` clone이 preflight에서 거부된다.

##### 런타임 의존성 (Single Edge/오버레이)

```bash
# OVS 오버레이 네트워크
sudo apt install -y openvswitch-switch

# iSCSI 스토리지
sudo apt install -y tgt open-iscsi
```

#### 검증용 도구 (선택)

```bash
# 정적 분석
sudo apt install -y cppcheck

# 메모리 검사
sudo apt install -y valgrind

# 커버리지 리포트
sudo apt install -y lcov
```

### 2.3 빌드

```bash
git clone <public-repo-url> purecvisor-single
cd purecvisor-single
```

#### 빌드 타겟

| 명령 | 설명 | 출력 |
|------|------|------|
| `make single` | Single Edge 빌드 | `bin/purecvisorsd` |
| `make cli` | CLI 빌드 | `bin/pcvctl` |
| `make tui` | TUI 빌드 | `bin/pcvtui` |
| `make all` | 전체 빌드 | 데몬 + CLI + TUI |
| `make release` | 릴리스 빌드 (-O2, NDEBUG, 하드닝) | 최적화된 바이너리 |
| `make clean` | 빌드 산출물 정리 | - |

```bash
# 클린 빌드 (경고 0 확인)
make clean && make single

# 빌드 결과 확인
ls -lh bin/
# bin/purecvisorsd  (~1.9MB, Single Edge)
# bin/pcvctl        (CLI)
# bin/pcvtui        (TUI)
```

#### 품질 검증

```bash
# 유닛 테스트 (167 케이스, sudo 필요)
make test

# 정적 분석
make cppcheck

# 메모리 누수 검사
make memcheck

# 코드 커버리지 HTML 리포트
make coverage-html
# 결과: coverage_report/html/index.html

# CI용 TAP 형식 테스트 출력
make test-tap
```

### 2.4 디렉터리 구조 생성

```bash
# 런타임 디렉터리
sudo mkdir -p /etc/purecvisor/tls
sudo mkdir -p /etc/purecvisor/plugins.d
sudo mkdir -p /etc/purecvisor/seccomp
sudo mkdir -p /var/lib/purecvisor
sudo mkdir -p /var/run/purecvisor
sudo mkdir -p /var/log/purecvisor
```

### 2.5 systemd 서비스 설치

```bash
sudo cp systemd/purecvisorsd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now purecvisorsd
```

서비스 상태 확인:

```bash
sudo systemctl status purecvisorsd

# 로그 확인 (실시간)
journalctl -u purecvisorsd -f
```

기동 완료 시 journalctl에 다음과 같은 배너가 출력됩니다:

```
PureCVisor daemon started
  UDS:  /var/run/purecvisor/daemon.sock
  REST: http://0.0.0.0:80/api/v1/
  Web:  http://0.0.0.0:80/ui/
  WS:   ws://0.0.0.0:80/api/v1/ws/events
  RPC methods: 265 | REST endpoints: ~190
```

### 2.6 daemon.conf 설정

설정 파일 경로: `/etc/purecvisor/daemon.conf`

설정 우선순위: **환경 변수 > daemon.conf > 컴파일 기본값**

```ini
# /etc/purecvisor/daemon.conf
# 전체 설정 레퍼런스

# ─────────────────────────────────────────────
# [server] 서버 기본 설정
# ─────────────────────────────────────────────
[server]
# UDS 소켓 경로
socket_path = /var/run/purecvisor/daemon.sock

# REST API 포트 (1-65535, CAP_NET_BIND_SERVICE 필요)
rest_port = 80

# 그레이스풀 드레인 타임아웃 (초, 최소 5)
drain_timeout = 30

# 워커 스레드 풀 크기 (1-64)
pool_max_conn = 16

# ─────────────────────────────────────────────
# [storage] 스토리지 설정
# ─────────────────────────────────────────────
[storage]
# ZFS zvol 풀 경로
zvol_pool = pcvpool/vms

# qcow2/raw 이미지 폴백 디렉터리
image_dir = /var/lib/libvirt/images

# ISO 스캔 디렉터리 (콤마 구분, .iso/.img 파일)
iso_dirs = /pcvpool/iso,/var/lib/libvirt/images,/iso

# VM당 최대 스냅샷 수
max_snapshots_per_vm = 100

# ─────────────────────────────────────────────
# [tls] HTTPS 설정
# ─────────────────────────────────────────────
[tls]
# true 시 HTTP(80) + HTTPS(443) 동시 리스닝
enabled = false

# 인증서 경로
cert_path = /etc/purecvisor/tls/server.crt
key_path = /etc/purecvisor/tls/server.key

# mTLS CA (클러스터 간 통신)
ca_path = /etc/purecvisor/tls/ca.crt

# ─────────────────────────────────────────────
# [auth] 인증 설정
# ─────────────────────────────────────────────
[auth]
# JWT 서명 비밀키 (HS256)
# 환경변수 PCV_SECRET_AUTH_JWT_SECRET 우선
jwt_secret = change-me-in-production

# JWT 토큰 만료 시간 (초)
jwt_expiry = 3600

# 브루트포스 방어: 최대 실패 횟수
max_login_failures = 5

# 잠금 시간 (초)
lockout_duration = 300

# 셀프 회원가입 활성화 여부
# true이면 로그인 랜딩의 회원가입으로 VIEWER 계정을 생성할 수 있습니다.
allow_self_register = false

# ─────────────────────────────────────────────
# [alert] 알림 엔진 설정
# ─────────────────────────────────────────────
[alert]
enabled = true

# CPU 임계값 (%)
cpu_warn = 80
cpu_crit = 95

# 메모리 임계값 (%)
mem_warn = 85
mem_crit = 95

# 디스크 임계값 (%)
disk_warn = 80
disk_crit = 90

# 데이터 풀 임계값 (%)
data_pool_warn = 80
data_pool_crit = 90

# 지속 평가 기간 (초, 임계값 초과 지속 시간)
eval_period = 30

# Webhook URL
webhook_url = https://hooks.slack.com/services/T00/B00/xxx

# Webhook 포맷: slack | telegram | generic
webhook_format = slack

# ─────────────────────────────────────────────
# [cluster] 클러스터 설정 (cluster build)
# ─────────────────────────────────────────────
[cluster]
# etcd 엔드포인트 (콤마 구분)
etcd_endpoints = http://192.0.2.19:2379

# etcd 타임아웃 (2-120초)
etcd_timeout = 15

# 노드 식별자
node_name = PureCVisor-Node1
node_ip = 192.0.2.19

# 피어 노드 SSH IP (콤마 구분)
peer_ssh_ip = 192.0.2.20,192.0.2.21

# ZFS 복제 풀
repl_pool = pcvpool/vms

# 복제 RPO (초)
repl_rpo = 300

# IPMI 펜싱 호스트
fence_ipmi_host = 192.0.2.10

# 자동 리밸런싱
auto_rebalance = true
rebalance_threshold = 3

# ─────────────────────────────────────────────
# [network] 관리형 기본 네트워크 (VP-1/VP-6, 2026-07)
# ─────────────────────────────────────────────
[network]
# vm create 시 network_bridge 미지정이면 부착되는 기본 NAT 네트워크.
# 데몬이 기동 시 브릿지+NAT(nftables)+DHCP/DNS(dnsmasq)를 멱등 보장한다.
# 마이그레이션: 구 [vm] default_bridge 키는 폐기됨(설정돼 있어도 무시) —
# 이 섹션으로 이관할 것.
default_bridge = pcvnat0

# 기본 네트워크 게이트웨이 CIDR (호스트 주소 포함 표기)
default_subnet = 10.78.0.1/24

# 0이면 기동 시 기본 네트워크 보장을 건너뜀
default_ensure = 1

# 호스트 방화벽(UFW/iptables FORWARD DROP) 자동 공존. "auto"면 관리형
# NAT 설정 시 필요한 allow 룰을 자동 삽입(AUDIT 로그 기록), "off"면
# 데몬이 호스트 방화벽을 건드리지 않음 (공존 실패 시 게스트 네트워크
# 불통은 운영자 책임).
firewall_integration = auto

# ─────────────────────────────────────────────
# [security_group] 보안 그룹
# ─────────────────────────────────────────────
[security_group]
# vnet 캐시 주기 재동기화 간격(초). 0 또는 음수면 타이머 비활성.
resync_interval_sec = 300

# ─────────────────────────────────────────────
# [overlay] OVS VXLAN 오버레이 설정
# ─────────────────────────────────────────────
[overlay]
name = pcvoverlay0
vni = 100
cidr = 10.100.0.1/24
local_ip = 192.0.2.19
peers = 192.0.2.20,192.0.2.21

# ─────────────────────────────────────────────
# [cpu] CPU 할당 설정
# ─────────────────────────────────────────────
[cpu]
# 오버커밋 허용 여부
allow_overcommit = false

# 격리 코어 목록 (예: 4-7,12-15)
# isolated_cores = 4-7
```

#### 설정 검증

데몬 시작 시 다음 항목이 자동 검증됩니다:

- `rest_port`: 1-65535 범위
- `drain_timeout`: 5초 이상
- `pool_max_conn`: 1-64 범위
- `etcd_timeout`: 2-120초 범위
- 경로 존재 여부 (`socket_path`, `cert_path` 등)

범위를 벗어나면 `PCV_LOG_WARN`으로 경고 후 기본값을 사용합니다.

#### 런타임 설정 변경 (SIGHUP)

데몬 재시작 없이 설정을 다시 로드할 수 있습니다:

```bash
sudo kill -SIGHUP $(pidof purecvisorsd)
```

### 2.7 CLI 자동완성 설치

```bash
# 시스템 전체 설치 (bash + zsh)
make install-completion

# 현재 사용자만 설치
make install-completion-user

# 적용 확인 (새 셸에서)
pcvctl <TAB><TAB>
```

### 2.8 멀티노드 배포

`scripts/deploy.sh`를 사용하면 빌드부터 3노드 배포까지 자동화됩니다.

```bash
# 릴리스 빌드 + 3노드 전체 배포 (바이너리 + UI 파일)
scripts/deploy.sh

# 디버그 빌드 배포
scripts/deploy.sh --debug

# 특정 노드만 배포 (빌드 생략)
scripts/deploy.sh --nodes 2,3 --skip-build
```

#### Web UI 정적 자산 배포 체크

Web UI 배포는 HTML/JS/CSS만 복사하면 끝나지 않습니다. 운영 브라우저는 CSP, PWA manifest, Service Worker 캐시를 함께 검증하므로 다음 자산이 같은 릴리스 단위로 배포되어야 합니다.

- `ui/index.html`, `ui/guide.html`, `ui/guide-content.md`
- `ui/samples/design-system-preview.html`, `ui/samples/design-borrowing-mockup.html`과 관련 `ui/samples/*.html`
- `ui/app.js`, `ui/app.bundle.js`, `ui/i18n.js`, `ui/sw.js`
- `ui/modules/*.js`
- `ui/manifest.json`, `ui/icon-192.png`, `ui/icon-512.png`
- `ui/vendor/chart.umd.min.js`
- `ui/vendor/novnc/novnc.esm.js`
- `ui/vendor/pretendard/pretendard.css`, `ui/vendor/pretendard/woff2/*.woff2`
- `ui/vendor/coolicons/coolicons.svg`

`scripts/bundle-ui.sh`는 `ui/modules/*.js`를 `ui/app.bundle.js`로 다시 묶고 `ui/sw.js`의 캐시 이름을 번들 해시로 갱신합니다. UI 모듈, WebSocket, metrics, 정적 자산 목록을 바꾸면 배포 전에 다음 순서로 확인합니다.

```bash
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
for f in ui/app.js ui/modules/*.js ui/vendor/chart.umd.min.js ui/vendor/novnc/novnc.esm.js; do node -c "$f"; done
git diff --check -- ui scripts src/api/rest_server.c
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" ui/index.html ui/guide.html ui/app.bundle.js ui/sw.js ui/vendor
rg -n "customConfirm\([^\\n]*<[^\\n]*>|<br><b>|idx \|\| selectedVmIndex" ui/modules ui/app.bundle.js
```

두 `rg` 명령은 결과가 없어야 정상입니다. 운영 CSP를 넓혀 외부 아이콘 API나 CDN sourcemap을 허용하지 말고, 필요한 런타임 자산은 `ui/vendor/` 또는 inline SVG처럼 로컬 자산으로 고정합니다. `customConfirm()` 호출부는 HTML 조각을 넘기지 않고 plain text와 `\n`만 사용합니다.

운영 서버에 반영할 때는 로컬 파일 검증과 공개 URL 검증을 분리한다. `127.0.0.1` 또는 개발 호스트의 `/usr/local/share/purecvisor/ui`가 통과해도, NAT와 도메인이 실제로 가리키는 공개 서버가 다른 호스트일 수 있다. UI 배포 완료 판정 전에는 공개 URL의 번들 해시와 외부 런타임 참조를 직접 확인한다.

```bash
curl -sk -o /tmp/pcv-live-app.bundle.js 'https://purecvisor.example.com/ui/app.bundle.js?v=<ui-version>'
curl -sk -o /tmp/pcv-compat-app.bundle.js 'https://purecvisor-compat.example.com/ui/app.bundle.js?v=<ui-version>'
curl -sk -o /tmp/pcv-live-guide-content.md 'https://purecvisor.example.com/ui/guide-content.md?v=<ui-version>'
curl -sk -o /tmp/pcv-compat-guide-content.md 'https://purecvisor-compat.example.com/ui/guide-content.md?v=<ui-version>'
curl -sk -o /tmp/pcv-live-sw.js 'https://purecvisor.example.com/ui/sw.js?v=<ui-version>'
curl -sk -o /tmp/pcv-compat-sw.js 'https://purecvisor-compat.example.com/ui/sw.js?v=<ui-version>'
curl -skL 'https://purecvisor.example.com/ui' | rg -n '<base href="/ui/">'
curl -skL 'https://purecvisor.example.com/ui/app.bundle.js?v=<ui-version>' | rg -n 'renderOpsTriage|replace\(/\^#'
sha256sum ui/app.bundle.js /tmp/pcv-live-app.bundle.js /tmp/pcv-compat-app.bundle.js
sha256sum ui/guide-content.md /tmp/pcv-live-guide-content.md /tmp/pcv-compat-guide-content.md
sha256sum ui/sw.js /tmp/pcv-live-sw.js /tmp/pcv-compat-sw.js
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" /tmp/pcv-live-app.bundle.js
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" /tmp/pcv-compat-app.bundle.js
```

외부 런타임 참조를 찾는 두 `rg` 명령은 결과가 없어야 한다. `/ui` no-slash 진입점은 `<base href="/ui/">`를 포함해야 하며, 번들은 `renderOpsTriage`와 `#page`/`#/page` 해시 라우팅 호환 코드를 포함해야 한다. 표준 도메인과 호환 도메인의 해시가 다르면 각 공개 서버의 `/usr/local/share/purecvisor/ui/app.bundle.js`와 `sw.js`를 같은 릴리스 산출물로 다시 배포한다.

현재 `1.0` 릴리스의 공개 UI 자산은 `?v=1.0` query string을 사용한다. 제품 버전을 바꿀 때는 API/CLI 버전과 HTML query string, `app.bundle.js`, `sw.js`를 함께 갱신하고 공개 URL에서 `/api/v1/health`, `/api/v1/version`, UI bundle hash를 다시 확인한다.

### 2.9 logrotate 설정

```bash
# 로그 로테이션 설치
sudo cp systemd/purecvisor.logrotate /etc/logrotate.d/purecvisor
```

기본 설정:
- 일일 회전
- 30일 보존
- 압축 활성화
- `/var/log/purecvisor/*.log`
- `/var/log/ovn/ovn-controller.log` 별도 보호
  - 일일 회전
  - `size 50M`, `maxsize 200M`
  - `copytruncate` 사용

Single Edge OVN local controller 준비 경로는 `ovn-controller` 재기동 직후
파일 로그 레벨을 `ERR`로 낮춥니다. `OVNSB commit failed` 같은 INFO 폭주가
재발하더라도 운영 로그가 무제한으로 불어나는 것을 막기 위한 안전장치입니다.

---

## 3. VM 관리

### 3.1 VM 생성

#### 기본 생성

```bash
pcvctl vm create \
  web-prod \
  --vcpu 2 \
  --memory_mb 2048 \
  --disk_size_gb 20
```

#### 스토리지 타입 선택

VM 디스크의 스토리지 백엔드를 선택할 수 있습니다.
저장 위치 계약의 설계 기준은 [ADR-0022: VM 생성 저장 위치 계약](adr/0022-vm-create-storage-location-contract.md)을 따른다.

| 타입 | 설명 | 적합한 용도 |
|------|------|-----------|
| `zvol` | ZFS 볼륨 (블록 디바이스) | 고성능 I/O, 스냅샷/복제 |
| `qcow2` | QEMU Copy-on-Write | 범용, 씬 프로비저닝 |
| `raw` | RAW 이미지 | 최대 I/O 성능 |

```bash
# ZFS zvol (기본, ZFS 풀이 있을 때)
pcvctl vm create db-prod --vcpu 4 --memory_mb 8192 --disk_size_gb 100 --storage_type zvol

# ZFS zvol 저장 위치 지정
pcvctl vm create db-prod --vcpu 4 --memory_mb 8192 --disk_size_gb 100 --storage_type zvol --storage_pool tank/vms

# qcow2 (ZFS 풀 없는 localhost 또는 파일 디스크가 필요한 경우)
pcvctl vm create dev-vm --vcpu 2 --memory_mb 4096 --disk_size_gb 50 --storage_type qcow2

# qcow2 저장 디렉터리 지정
pcvctl vm create dev-vm --vcpu 2 --memory_mb 4096 --disk_size_gb 50 --storage_type qcow2 --image_dir /var/lib/libvirt/images

# RAW (최대 성능)
pcvctl vm create bench-vm --vcpu 8 --memory_mb 16384 --disk_size_gb 200 --storage_type raw
```

> **자동 감지**: CLI/REST/RPC에서 `storage_type`을 지정하지 않으면 dispatcher inline `vm.create`가 `vm_manager.c`로 위임하고, `vm_manager.c`가 ZFS 풀 존재 여부를 감지합니다. ZFS 풀이 있으면 `zvol`, 없으면 `daemon.conf [storage] image_dir`에 `qcow2`로 폴백합니다. `storage_pool` 또는 `image_dir`를 함께 주면 자동 감지 대상 위치도 해당 값으로 바뀝니다. `storage_type=zvol`을 명시하면 지정한 ZFS 부모 데이터셋이 없을 때 실패합니다.

#### 고급 옵션

```bash
pcvctl vm create \
  db-prod \
  --vcpu 4 \
  --memory_mb 8192 \
  --disk_size_gb 100 \
  --storage_type zvol \
  --storage_pool tank/vms \
  --network_bridge pcvnat0 \
  --iso_path /pcvpool/iso/ubuntu-24.04-live-server-amd64.iso
```

| 옵션 | 설명 |
|------|------|
| `--network_bridge` | 연결할 브릿지. 미지정 시 관리형 기본 NAT 네트워크(`[network] default_bridge`, 기본 `pcvnat0`)에 부착. `none`이면 NIC 미부착 |
| `--iso_path` | 설치용 ISO 또는 seed ISO 경로 |
| `--storage_type` | 디스크 타입 (`zvol`, `qcow2`, `raw`) |
| `--storage_pool` | zvol 부모 데이터셋. 예: `tank/vms` |
| `--image_dir` | qcow2/raw 파일 디스크 저장 디렉터리. 예: `/var/lib/libvirt/images` |
| `--template` | VM 템플릿 이름 |

고급 VM 속성(`firmware`, `boot_mode`, `tpm`, `cpu_mode`, `hugepages`, `vlan_id`, `base_image`, `ovn_switch`)은 현재 REST/RPC body로 지정한다. `pcvctl vm create` CLI는 위 표의 기본 옵션과 저장 위치 옵션을 직접 파싱한다.

#### RPC 직접 호출

```bash
echo '{"jsonrpc":"2.0","method":"vm.create","params":{
  "name": "web-prod",
  "vcpu": 2,
  "memory_mb": 2048,
  "disk_size_gb": 20,
  "os_variant": "ubuntu24.04",
  "storage_type": "zvol",
  "storage_pool": "tank/vms",
  "network_bridge": "pcvnat0",
  "iso_path": "/pcvpool/iso/ubuntu-24.04-live-server-amd64.iso"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### REST API

```bash
curl -X POST http://localhost:80/api/v1/vms \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "web-prod",
    "vcpu": 2,
    "memory_mb": 2048,
    "disk_size_gb": 20,
    "storage_type": "zvol",
    "network_bridge": "pcvnat0"
  }'
```

응답:

```json
{
  "result": {
    "accepted": true,
    "message": "VM creation started in background"
  }
}
```

> **fire-and-forget 패턴**: VM 생성은 즉시 `accepted: true`를 반환하고 백그라운드에서 비동기 실행됩니다. `virDomainDefineXML` 실패 시 zvol/디스크가 자동 롤백됩니다.

#### OVS 자동 감지

`network_bridge`에 OVS 브릿지를 지정하면, 데몬이 `ovs-vsctl br-exists`로 OVS 여부를 자동 확인하고 VM XML에 `<virtualport type='openvswitch'/>`를 자동 추가합니다.

### 3.2 VM 라이프사이클

```bash
# 목록 조회
pcvctl vm list

# 시작
pcvctl vm start web-prod

# 정상 중지 (graceful 30초 → force destroy)
pcvctl vm stop web-prod

# 일시 정지 / 재개
pcvctl vm pause web-prod
pcvctl vm resume web-prod

# 삭제 (비동기, 스토리지 포함)
pcvctl vm delete web-prod

# 삭제 진행 상태 확인
pcvctl vm delete-status web-prod
```

**중지 동작 상세:**

1. `virDomainShutdown`으로 ACPI 셧다운 신호 전송
2. 1초 간격으로 최대 30초 동안 정상 종료 폴링
3. 타임아웃 시 `virDomainDestroy`로 강제 종료
4. 반환값 검증 후 결과 보고

**삭제 진행 상태:**

```bash
# RPC로 삭제 상태 확인
echo '{"jsonrpc":"2.0","method":"vm.delete.status","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

응답 상태값: `pending` | `deleting` | `done` | `failed`

**이름 변경:**

VM 이름 변경은 정지된 VM에서만 허용된다. libvirt domain 이름, 표준 ZFS zvol 또는 표준 qcow2/raw/img 파일 디스크 경로, UEFI NVRAM 경로를 함께 변경한다.

지원 조건:

- VM 상태가 `shut off`여야 한다.
- 새 이름은 `pcv_validate_vm_name()` 규칙을 따른다. 허용 문자는 영문, 숫자, 하이픈, 언더스코어이며 최대 64자다.
- 기본 디스크는 `/dev/zvol/<parent>/<vm>` 또는 `<vm>.qcow2`, `<vm>.raw`, `<vm>.img` 형식이어야 한다.
- libvirt snapshot metadata가 남아 있으면 이름 변경을 거부한다.

```bash
# CLI
pcvctl vm rename web-prod web-prod-01

# RPC
echo '{"jsonrpc":"2.0","method":"vm.rename","params":{
  "name":"web-prod",
  "new_name":"web-prod-01"
},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### REST API 라이프사이클

```bash
# VM 목록
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/vms | python3 -m json.tool

# VM 시작
curl -X POST -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/vms/web-prod/start

# VM 중지
curl -X POST -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/vms/web-prod/stop

# VM 일시 정지
curl -X POST -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/vms/web-prod/suspend

# VM 재개
curl -X POST -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/vms/web-prod/resume

# VM 삭제
curl -X DELETE -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/vms/web-prod

# VM 이름 변경
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"new_name":"web-prod-01"}' \
  http://localhost:80/api/v1/vms/web-prod/rename

# 삭제 상태 확인
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/vms/web-prod/delete-status
```

#### TUI VM 조작

`pcvtui`의 F1 VM 화면에서도 주요 운영 기능을 직접 실행할 수 있다.

| 키 | 기능 |
|----|------|
| `c` | VM 생성 |
| `s` / `x` / `p` / `d` | 시작, 중지, 일시정지/재개, 삭제 |
| `r` | 정지된 VM 이름 변경 |
| `k` | VM 복제. source VM이 `shut off` 상태여야 하며 `cow/full`, `guest-reset/prepared`를 선택한다 |
| `O` | OVA 내보내기. 출력 디렉터리를 입력하지 않으면 `/tmp`를 사용한다 |
| `U` | OVA 가져오기. OVA 경로, target VM 이름, 선택적 ZFS pool을 입력한다 |
| `G` | Guest Agent ping, exec, shutdown, 상태 진단, channel 보정 |
| `I` / `Z` | 블록 I/O IOPS 제한, 라이브 디스크 리사이즈 |
| `W` | VM 네트워크 대역폭 제한 |
| `Y` | USB 패스스루 목록, 연결, 분리 |
| `N` / `+` / `-` | NIC 목록, 추가, 제거 |

### 3.3 핫플러그

실행 중인 VM에 리소스를 동적으로 추가하거나 변경할 수 있습니다.

#### vCPU / 메모리

```bash
# vCPU 조정 (라이브)
pcvctl vm set-vcpu web-prod 4

# 메모리 조정 (balloon, MB 단위)
pcvctl vm set-memory web-prod 4096

# CPU 피닝 (특정 물리 코어에 고정)
pcvctl vm pin-vcpu web-prod --vcpu 0 --cpuset 2,3
pcvctl vm pin-vcpu web-prod --vcpu 1 --cpuset 4,5

# CPU 통계 확인
echo '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 메모리 balloon 통계 확인
echo '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

메모리 통계 응답 예시:

```json
{
  "actual": 4194304,
  "rss": 3145728,
  "unused": 1048576,
  "available": 4194304,
  "swap_in": 0,
  "swap_out": 0
}
```

#### NIC 관리

```bash
# NIC 목록
pcvctl nic list web-prod

# NIC 추가
pcvctl nic add web-prod --bridge pcvnat0 --model virtio

# NIC 제거 (MAC 주소 지정)
pcvctl nic remove web-prod --mac 52:54:00:ab:cd:ef
```

#### ISO 관리

```bash
# ISO 목록 (설정된 iso_dirs 전체 스캔)
pcvctl iso list

# ISO 마운트
pcvctl iso mount web-prod /pcvpool/iso/ubuntu-24.04-live-server-amd64.iso

# ISO 꺼내기
pcvctl iso eject web-prod
```

#### USB 핫플러그

```bash
# USB 장치 목록
pcvctl vm usb-list web-prod

# USB 장치 연결
pcvctl vm usb-attach web-prod 0x1234 0x5678

# USB 장치 분리
pcvctl vm usb-detach web-prod 0x1234 0x5678
```

#### 디스크 리사이즈

```bash
# 라이브 디스크 확장 (축소 불가, GB 단위)
pcvctl vm disk-resize web-prod vda 50

# REST API
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/disk-resize \
  -H "Content-Type: application/json" \
  -d '{"new_size_gb": 50}'
```

> **주의**: ZFS zvol의 경우 `zfs set volsize`와 `virDomainBlockResize`가 동시에 수행됩니다. 게스트 OS 내에서 파티션 확장이 별도로 필요할 수 있습니다.

#### Block I/O 스로틀

```bash
# I/O 제한 설정 (IOPS, bytes/sec)
pcvctl vm disk-throttle web-prod --read-iops 1000 --write-iops 500
pcvctl vm blkio-set web-prod --read_iops 1000 --write_iops 500

# 현재 I/O 제한 조회
pcvctl vm blkio-get web-prod
```

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"vm.blkio.set","params":{
  "name": "web-prod",
  "device": "vda",
  "read_bytes_sec": 104857600,
  "write_bytes_sec": 52428800,
  "read_iops_sec": 1000,
  "write_iops_sec": 500
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 네트워크 QoS (대역폭 제한)

```bash
# VM 네트워크 대역폭 제한 (KB/s)
pcvctl vm bandwidth web-prod --inbound-kbps 100000 --outbound-kbps 100000
```

### 3.4 스냅샷

#### 생성 및 관리

```bash
# 스냅샷 생성
pcvctl snapshot create web-prod before-upgrade

# 스냅샷 목록
pcvctl snapshot list web-prod

# 스냅샷 롤백 (fire-and-forget: VM 중지 -> 롤백 -> 재시작)
pcvctl snapshot rollback web-prod before-upgrade

# 스냅샷 삭제
pcvctl snapshot delete web-prod before-upgrade
```

#### 일괄 삭제

prefix 패턴 필터와 최근 N개 보존 옵션을 지원합니다.

```bash
# "auto-" 프리픽스 스냅샷 중 최근 5개만 보존, 나머지 삭제
pcvctl vm snapshot-delete-all web-prod --prefix auto- --keep 5

# 모든 스냅샷 삭제 (보존 없음)
pcvctl vm snapshot-delete-all web-prod
```

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"vm.snapshot.delete_all","params":{
  "name": "web-prod",
  "prefix": "auto-",
  "keep_recent": 5
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 스냅샷 스케줄

```bash
# 스케줄 상태 조회
pcvctl snapshot schedule-status
```

### 3.5 VM 복제

```bash
# 준비된 템플릿: guest reset 생략
pcvctl vm clone web-template web-staging --mode cow --template-prepared

# 일반 VM: target disk 생성 후 libguestfs guest reset 수행
pcvctl vm clone web-prod web-prod-copy --mode full --guest-reset
```

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"vm.clone","params":{
  "source": "web-template",
  "clone_name": "web-staging",
  "mode": "cow",
  "template_prepared": true
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **안전 제한**: `vm.clone`은 source VM이 `shut off` 상태일 때만 허용하며, 단일 data disk VM만 지원한다. `template_prepared=true`가 없으면 `guest_reset=true` 경로로 `libguestfs-tools`의 `virt-sysprep`, `virt-filesystems`, `guestfish`, `virt-customize`를 실행해야 한다. qcow2/raw는 추가로 `mode=full`만 허용한다. 자세한 기준은 [ADR-0023](adr/0023-vm-clone-beta-safety-guard.md)을 따른다.

> **fire-and-forget**: 클론은 즉시 `accepted: true`를 반환하고 백그라운드에서 disk clone + guest reset + XML 패치를 수행합니다.

accepted 응답에는 실제 XML에서 확인한 source/target disk가 포함된다.

```json
{
  "status": "accepted",
  "source": "web-template",
  "clone_name": "web-staging",
  "mode": "cow",
  "job_id": "vm.clone:web-template:web-staging",
  "guest_reset": false,
  "storage_type": "zvol",
  "source_disk": "/dev/zvol/rpool/web-template",
  "target_disk": "/dev/zvol/rpool/web-staging"
}
```

동작 기준:

- clone plan은 `daemon.conf [storage] zvol_pool`이나 `image_dir`을 추측하지 않고 libvirt XML의 실제 disk source를 기준으로 계산한다.
- source VM이 power on 상태이면 storage type과 무관하게 preflight에서 거부한다.
- `--mode cow`는 source snapshot을 origin으로 유지하는 ZFS CoW clone이다.
- `--mode full`은 `pcv_spawn_pipe_sync()`로 `zfs send`와 `zfs recv`를 직접 연결해 독립 zvol을 만든다. 셸 파이프, 리다이렉션, `/tmp` 대용량 임시 파일은 사용하지 않으며, 성공 후 source 임시 snapshot은 정리한다.
- snapshot 생성 이후 worker가 실패하면 target dataset을 먼저 best-effort 정리하고, source 임시 snapshot이 origin으로 필요하지 않으면 함께 best-effort 정리한다.
- qcow2/raw 파일 디스크는 `mode=full`에서 `qemu-img convert`로 별도 target 파일을 만든다.
- `guest_reset=true`는 `virt-sysprep`으로 machine-id, DHCP/SSH/cloud-init 상태, hostname, LVM PV/VG UUID를 분리하고, `guestfish`로 ext filesystem UUID를 보정하며, `virt-customize`로 `/etc/fstab` UUID 참조와 Ubuntu/Rocky 계열 boot artifact 재생성 command를 실행한다.
- 현재 실환경 완료 범위는 준비된 zvol clone, Ubuntu 24.04 non-LVM qcow2/raw full clone + guest reset, Ubuntu 24.04 LVM qcow2/raw/ZFS zvol full clone + guest reset이다. Rocky/RHEL LVM, SELinux enforcing boot smoke는 문서상 후속 검증 항목으로만 유지한다.
- 이름, UUID, MAC, disk source를 각각 패치한다. MAC 재생성은 원본 XML을 한 번만 스캔해 새 MAC이 다시 매칭되는 무한 루프를 방지한다.
- clone 생성 결과는 자동 기동하지 않는다. 운영자가 검증 후 수동으로 시작한다.
- worker 결과는 accepted 응답의 `job_id`, `/var/log/purecvisor/audit.log`의 `vm.clone` 결과, WebSocket job completion으로 확인한다.

Guest reset과 Prepared template 선택 기준:

- `Guest reset`은 일반 VM 복제 기본값이다. 운영 서버에 `libguestfs-tools`가 설치되어 있어야 하며, target disk 생성 후 machine-id, hostname, SSH/DHCP/cloud-init 상태, filesystem/LVM UUID, `/etc/fstab` 참조를 새 VM 기준으로 분리한다.
- `Prepared template`은 운영자가 이미 guest identity를 정리한 템플릿 VM에만 사용한다. 이 선택은 guest reset을 건너뛰므로 source VM에 중복 식별자가 남아 있지 않다는 책임은 운영자에게 있다.
- 두 선택을 동시에 사용할 필요는 없다. 준비된 템플릿이면 `--template-prepared`, 일반 VM이면 `--guest-reset` 또는 기본 Guest reset 경로를 사용한다.

Guest reset 후 로그인 정보:

- Guest reset은 guest OS의 기존 사용자 계정과 `/etc/shadow` 비밀번호 해시를 새 값으로 바꾸지 않는다. 현재 구현은 `virt-sysprep`의 `user-account`나 비밀번호 재설정 operation을 사용하지 않는다.
- 콘솔 로그인은 원본 VM의 기존 계정/비밀번호가 유지되는 것이 기본 동작이다.
- `virt-sysprep defaults`는 SSH host key와 사용자 홈의 `.ssh` 디렉터리를 정리할 수 있다. 따라서 SSH 키 로그인은 실패할 수 있고, SSH 비밀번호 로그인 가능 여부는 guest OS의 sshd 설정에 따른다.
- cloud-init으로 생성한 `webadmin` 계정도 평문 비밀번호를 복구할 수 있다고 가정하면 안 된다. Ubuntu autoinstall의 `identity.password`는 보통 `$6$...` 형태의 SHA-512 crypt 해시로 들어가며, 운영 문서에는 해시 원문도 저장하지 않는다.
- autoinstall seed에 `chage -d 0 webadmin`이 포함된 VM은 첫 로그인 시 비밀번호 변경을 요구할 수 있다.
- `webadmin` 접속 복구가 필요하면 source/target VM을 종료한 뒤 `sudo virt-customize -d <vm_name> --password webadmin:password:'새비밀번호'`로 새 비밀번호를 주입하고 수동 boot smoke를 수행한다.

실환경 확인 예시:

```bash
sudo pcvctl vm clone example-vm-source example-vm-clone --mode cow --template-prepared
virsh -c qemu:///system list --all | grep example-vm-clone
sudo zfs list -H -o name,origin rpool/example-vm-clone
```

예상 결과:

```text
example-vm-clone   shut off
rpool/example-vm-clone   rpool/example-vm-source@clone-example-vm-clone
```

full clone 확인 예시:

```bash
sudo pcvctl --format=json vm clone example-vm-source example-vm-full-clone --mode full --template-prepared
virsh -c qemu:///system dominfo example-vm-full-clone
sudo zfs list -H -o name,origin rpool/example-vm-full-clone
sudo zfs list -t snapshot -H -o name rpool/example-vm-source@clone-example-vm-full-clone || echo "not found"
```

예상 결과:

```text
example-vm-full-clone   shut off / Persistent: yes
rpool/example-vm-full-clone   -
not found
```

2026-04-29 KST 운영 서버 검증에서는 위 full clone 경로가 `accepted`, target zvol `origin=-`, source 임시 snapshot 없음, audit `result=ok`로 확인됐다. 이어 Ubuntu 24.04 non-LVM qcow2/raw와 Ubuntu 24.04 LVM qcow2/raw/ZFS zvol clone은 guest reset, hostname/machine-id 재생성, filesystem UUID 분리, LVM PV/VG UUID 분리, `/etc/fstab` UUID 참조 갱신, 수동 boot smoke까지 성공했다.

클론 후 독립 데이터셋으로 전환하려면:

```bash
# ZFS 프로모트 (원본 스냅샷 의존 해소)
pcvctl storage promote rpool/web-staging
```

### 3.6 OVA 내보내기 / 가져오기

```bash
# OVA 내보내기 (qemu-img VMDK 변환 + OVF + tar)
pcvctl vm export-ova web-prod --output-dir /tmp

# OVA 가져오기
pcvctl vm import-ova /tmp/web-prod.ova web-imported
```

### 3.7 Guest Agent 연동

QEMU Guest Agent는 VM 내부 패키지와 libvirt channel이 함께 있어야 사용할 수 있다. **v1.1부터 `vm create`가 생성하는 VM에는 guest-agent channel이 기본 포함**되므로 신규 VM은 별도 보정이 필요 없다(패키지만 VM 내부에서 설치). `guest-agent-ensure-channel`은 채널이 없는 **기존/레거시 VM 보정**용이다. PureCVisor는 상태 진단과 channel 보정을 함께 제공한다.

Web UI에서는 `대시보드 > 요약`에서 VM을 선택한 뒤 Storage 카드의 `디스크 사용량` 버튼으로 게스트 파일시스템 사용량을 확인한다.
이 조회는 qemu-guest-agent의 고정 `guest-get-fsinfo` 명령만 사용하며, 임의 guest-exec 명령을 실행하지 않는다.

```bash
# Guest Agent 상태 진단
pcvctl vm guest-agent-status web-prod

echo '{"jsonrpc":"2.0","method":"vm.guest.agent.status","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 기존 VM XML에 Guest Agent channel 보정
pcvctl vm guest-agent-ensure-channel web-prod

echo '{"jsonrpc":"2.0","method":"vm.guest.agent.ensure_channel","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# Guest Agent 연결 확인 (5초 타임아웃)
pcvctl vm guest-ping web-prod

echo '{"jsonrpc":"2.0","method":"vm.guest.ping","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# Guest Agent를 통한 안전한 셧다운 (ACPI 폴백)
pcvctl vm guest-shutdown web-prod

echo '{"jsonrpc":"2.0","method":"vm.guest.shutdown","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# Guest Agent를 통한 명령 실행 (base64 stdout/stderr)
pcvctl vm guest-exec web-prod "hostname -I"

echo '{"jsonrpc":"2.0","method":"vm.guest.exec","params":{
  "name":"web-prod",
  "command":"hostname -I"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 게스트 파일시스템 디스크 사용량 조회
echo '{"jsonrpc":"2.0","method":"vm.guest.fsinfo","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

REST API:

```bash
# Guest Agent 상태 진단
curl -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/guest-agent

# Guest Agent channel 보정
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/guest-agent-channel

# Guest Agent ping
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/guest-ping

# Guest Agent 명령 실행
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  http://localhost:80/api/v1/vms/web-prod/guest-exec \
  -d '{"command": "cat /etc/hostname"}'

# 게스트 파일시스템 디스크 사용량 조회
curl -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/disk-usage
```

VM 내부 설치 명령:

```bash
# Debian / Ubuntu
sudo apt update && sudo apt install -y qemu-guest-agent
sudo systemctl enable --now qemu-guest-agent

# RHEL / Rocky / Fedora
sudo dnf install -y qemu-guest-agent
sudo systemctl enable --now qemu-guest-agent
```

상태 해석:

| 상태 | 의미 | 다음 조치 |
|------|------|-----------|
| `ok` | channel이 있고 agent ping 성공 | 정상 |
| `channel_missing` | VM XML에 guest agent channel이 없음 | `guest-agent-channel` 보정 |
| `reboot_required` | 영구 XML에는 channel이 있으나 실행 중 VM에는 아직 없음 | VM 재시작 또는 live attach 재시도 |
| `agent_unavailable` | channel은 있으나 VM 내부 agent가 응답하지 않음 | VM 내부 `qemu-guest-agent` 설치/시작 |
| `vm_stopped` | channel은 있으나 VM이 꺼져 있어 ping 불가 | VM 시작 후 재확인 |

### 3.8 VM 템플릿

템플릿을 사용하면 사전 정의된 설정으로 빠르게 VM을 생성할 수 있습니다.

```bash
# 프리셋 템플릿 목록
pcvctl template list

# 템플릿 생성
pcvctl template create web \
  --vcpu 2 \
  --memory_mb 2048 \
  --disk_gb 20 \
  --os_variant ubuntu24.04

# 템플릿으로 VM 생성
pcvctl template apply web --name web-prod-01

# 템플릿 삭제
pcvctl template delete web
```

> **이름 검증**: 템플릿 이름은 `pcv_validate_vm_name()` 규칙을 따릅니다 (영문소문자, 숫자, 하이픈, 최대 64자).

### 3.9 VM 메트릭

```bash
# 개별 VM 메트릭
pcvctl monitor vm web-prod
```

출력 예시:

```
VM: web-prod (running)
  CPU:    15.2%
  Memory: 1024/2048 MB (50.0%)
  Disk:   8.5 GB / 20 GB
  Net RX: 1.2 MB/s  TX: 0.3 MB/s
```

```bash
# 전체 VM 일괄 메트릭 (fleet 뷰)
pcvctl monitor fleet
```

REST API:

```bash
# 개별 VM 메트릭
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/metrics | python3 -m json.tool
```

---

## 4. 컨테이너 관리

PureCVisor는 LXC 컨테이너를 ZFS 백엔드와 통합하여 관리합니다.

### 4.1 컨테이너 생성

```bash
# 기본 생성 (LXC + ZFS rootfs)
pcvctl container create --name app-ctr --dist ubuntu --release noble

# 생성 후 바로 시작
pcvctl container create --name web-ctr --dist ubuntu --release jammy
pcvctl container start web-ctr
```

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"container.create","params":{
  "name": "app-ctr",
  "dist": "ubuntu",
  "release": "noble"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **fire-and-forget**: 컨테이너 생성/시작/중지/삭제는 모두 fire-and-forget 패턴으로 즉시 응답합니다.

### 4.2 라이프사이클

```bash
# 시작 (cgroup v2 리소스 제한 자동 적용)
pcvctl container start app-ctr

# 중지 (오퍼레이션 잠금으로 동시 실행 방지)
pcvctl container stop app-ctr

# 삭제
pcvctl container destroy app-ctr

# 목록 조회
pcvctl container list
```

REST API:

```bash
# 컨테이너 목록
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/containers | python3 -m json.tool

# 컨테이너 시작
curl -X POST -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/containers/app-ctr/start

# 컨테이너 중지
curl -X POST -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/containers/app-ctr/stop
```

### 4.3 명령 실행

```bash
# 단일 명령
pcvctl container exec app-ctr "hostname -I"

# 패키지 업데이트
pcvctl container exec app-ctr "apt update && apt upgrade -y"

# 서비스 상태 확인
pcvctl container exec app-ctr "systemctl status nginx"
```

### 4.4 리소스 제한 (cgroup v2)

```bash
# CPU/메모리 제한 설정
pcvctl container set-limits app-ctr --cpu_quota 50 --memory_mb 1024
# --cpu_quota: CPU quota
# --memory_mb: 메모리 제한 (MB)
```

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"container.set_limits","params":{
  "name": "app-ctr",
  "cpu_percent": 50,
  "memory_mb": 1024
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 네트워크 대역폭 QoS

```bash
# 대역폭 제한 (tc qdisc 기반, KB/s)
pcvctl container set-bandwidth app-ctr --inbound 100000 --outbound 100000
```

### 4.5 컨테이너 메트릭

```bash
# 개별 컨테이너 메트릭
pcvctl container metrics app-ctr
```

출력 예시:

```
Container: app-ctr (RUNNING)
  CPU:    8.3%
  Memory: 256/1024 MB
  PIDs:   42
  Net:    eth0 RX 15.2MB TX 3.1MB
```

### 4.6 컨테이너 스냅샷

```bash
# 스냅샷 생성
pcvctl container snap create app-ctr --name v1

# 스냅샷 목록
pcvctl container snap list app-ctr

# 롤백
pcvctl container snap rollback app-ctr v1

# 삭제
pcvctl container snap delete app-ctr v1
```

### 4.7 컨테이너 복제

```bash
# CoW 클론 (lxc-copy, ZFS 기반)
pcvctl container clone app-ctr --name app-ctr-clone
```

### 4.8 볼륨 마운트

```bash
# 호스트 디렉터리를 컨테이너에 바인드 마운트
echo '{"jsonrpc":"2.0","method":"container.volume.attach","params":{
  "name": "app-ctr",
  "host_path": "/data/shared",
  "container_path": "/mnt/shared",
  "readonly": false
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 볼륨 목록
echo '{"jsonrpc":"2.0","method":"container.volume.list","params":{
  "name": "app-ctr"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 볼륨 분리
echo '{"jsonrpc":"2.0","method":"container.volume.detach","params":{
  "name": "app-ctr",
  "container_path": "/mnt/shared"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **경로 순회 방어**: `realpath` 검증으로 심볼릭 링크를 통한 경로 순회 공격을 차단합니다.

### 4.9 환경 변수

```bash
# 환경변수 설정 (멱등 upsert)
echo '{"jsonrpc":"2.0","method":"container.env.set","params":{
  "name": "app-ctr",
  "key": "DATABASE_URL",
  "value": "postgresql://localhost:5432/app"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 환경변수 목록
echo '{"jsonrpc":"2.0","method":"container.env.list","params":{
  "name": "app-ctr"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 환경변수 삭제
echo '{"jsonrpc":"2.0","method":"container.env.delete","params":{
  "name": "app-ctr",
  "key": "DATABASE_URL"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

### 4.10 헬스체크

컨테이너에 주기적 헬스체크를 등록할 수 있습니다.

```bash
# 헬스체크 등록
echo '{"jsonrpc":"2.0","method":"container.health.set","params":{
  "name": "app-ctr",
  "cmd": "curl -sf http://localhost:8080/health || exit 1",
  "interval": 30,
  "timeout": 5,
  "retries": 3
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 헬스 상태 확인
echo '{"jsonrpc":"2.0","method":"container.health.get","params":{
  "name": "app-ctr"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

상태값: `healthy` | `unhealthy` | `starting`

```bash
# 헬스체크 해제
echo '{"jsonrpc":"2.0","method":"container.health.delete","params":{
  "name": "app-ctr"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

### 4.11 컨테이너 로그

```bash
# 최근 100줄 로그 조회
echo '{"jsonrpc":"2.0","method":"container.logs","params":{
  "name": "app-ctr",
  "lines": 100
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

### 4.12 CRIU 체크포인트/복원

실행 중인 컨테이너의 상태를 저장하고 나중에 복원할 수 있습니다.

```bash
# 체크포인트 (실행 상태 저장)
pcvctl container checkpoint app-ctr --dir /tmp/criu-app

# 복원 (체크포인트에서 재개)
pcvctl container restore app-ctr --dir /tmp/criu-app
```

### 4.13 Seccomp 프로파일

```bash
# 프로파일 설정
pcvctl container seccomp-set app-ctr --profile default
# 프로파일 파일: /etc/purecvisor/seccomp/default.seccomp

# 현재 프로파일 조회
pcvctl container seccomp-get app-ctr
```

### 4.14 NIC 관리 (VM 동등 기능)

컨테이너도 VM과 동일한 NIC 관리 기능을 제공합니다.

```bash
# NIC 목록
echo '{"jsonrpc":"2.0","method":"container.nic.list","params":{
  "name": "app-ctr"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# NIC 추가
echo '{"jsonrpc":"2.0","method":"container.nic.attach","params":{
  "name": "app-ctr",
  "bridge": "pcvbr0"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# NIC 분리
echo '{"jsonrpc":"2.0","method":"container.nic.detach","params":{
  "name": "app-ctr",
  "interface": "eth1"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

---

## 5. 스토리지

### 5.1 ZFS 풀 관리

#### 풀 목록

```bash
pcvctl storage pool list
```

출력 예시:

```
NAME       SIZE   USED   AVAIL  CAP  HEALTH
pcvpool    8.0T   2.1T   5.9T   26%  ONLINE
datapool   4.0T   1.5T   2.5T   38%  ONLINE
```

#### 풀 생성

```bash
# mirror 풀 (2-디스크 미러)
pcvctl storage pool create --name datapool --vdev /dev/sdb /dev/sdc --type mirror

# raidz1 풀 (3-디스크 RAIDZ)
pcvctl storage pool create --name raidpool --vdev /dev/sdb /dev/sdc /dev/sdd --type raidz1
```

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"storage.pool.create","params":{
  "name": "datapool",
  "vdevs": ["/dev/sdb", "/dev/sdc"],
  "type": "mirror"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 풀 상태 / 스크럽

```bash
# 풀 헬스 (health, scrub 진행률, 용량)
pcvctl storage pool health pcvpool

# 스크럽 시작
pcvctl storage pool scrub pcvpool

# 풀 삭제
pcvctl storage pool destroy datapool
```

#### 용량 예측

```bash
# 현재 사용량 추세 기반 용량 고갈 예측
pcvctl storage pool forecast pcvpool
```

### 5.2 ZFS Zvol 관리

#### Zvol 목록

```bash
pcvctl storage zvol list
```

출력 예시:

```
NAME                      VOLSIZE   USED   REFER
pcvpool/vms/web-prod      20.0G     8.5G   8.5G
pcvpool/vms/db-prod       100.0G    45.2G  45.2G
pcvpool/vms/web-staging   20.0G     8.5G   8.5G   (clone)
```

#### Zvol CRUD

```bash
# zvol 생성
pcvctl storage zvol create pcvpool/vms/new-disk --size 50G

# zvol 삭제
pcvctl storage zvol delete pcvpool/vms/old-disk
```

RPC 직접 호출:

```bash
# 목록
echo '{"jsonrpc":"2.0","method":"storage.zvol.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 생성
echo '{"jsonrpc":"2.0","method":"storage.zvol.create","params":{
  "name": "pcvpool/vms/test-disk",
  "size": "50G"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 삭제
echo '{"jsonrpc":"2.0","method":"storage.zvol.delete","params":{
  "name": "pcvpool/vms/test-disk"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### ZFS 프로모트

클론 데이터셋을 독립 데이터셋으로 전환합니다 (원본 스냅샷 의존 해소).

```bash
pcvctl storage promote pcvpool/vms/web-staging
```

### 5.3 iSCSI 관리

#### iSCSI 타겟

```bash
# 타겟 목록
pcvctl iscsi list

# 타겟 생성
pcvctl iscsi create --name iqn.2026-04.purecvisor:disk1 \
  --path /dev/zvol/pcvpool/iscsi/disk1

# 타겟 삭제
pcvctl iscsi delete --name iqn.2026-04.purecvisor:disk1
```

RPC 직접 호출:

```bash
# 타겟 목록
echo '{"jsonrpc":"2.0","method":"iscsi.target.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 타겟 생성
echo '{"jsonrpc":"2.0","method":"iscsi.target.create","params":{
  "iqn": "iqn.2026-04.purecvisor:disk1",
  "backing_store": "/dev/zvol/pcvpool/iscsi/disk1"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **CHAP 인증**: iSCSI 타겟에 CHAP 인증을 설정할 수 있습니다 (보안 고도화 포함).

### 5.4 스토리지 티어링

ZFS 풀의 디스크를 NVMe/SSD/HDD 계층으로 자동 분류합니다.

```bash
# 티어 목록
pcvctl storage tier list
```

출력 예시:

```
TIER     DEVICES         IOPS      LATENCY
NVMe     nvme0n1         500K      0.1ms
SSD      sda, sdb        50K       0.5ms
HDD      sdc, sdd        200       5.0ms
```

### 5.5 ISO 관리

```bash
# 설정된 모든 iso_dirs에서 ISO/IMG 파일 스캔
pcvctl iso list
```

출력 예시:

```
PATH                                                    SIZE
/pcvpool/iso/ubuntu-24.04-live-server-amd64.iso         2.6G
/pcvpool/iso/debian-12.0-amd64-netinst.iso              628M
/var/lib/libvirt/images/cloud-init.img                  384K
```

> **다중 ISO 경로**: `daemon.conf [storage] iso_dirs`에 콤마로 구분하여 여러 디렉터리를 지정할 수 있습니다.

### 5.6 Prometheus 스토리지 메트릭

`/api/v1/metrics`에서 다음 스토리지 메트릭을 수집합니다:

```
# ZFS 풀 메트릭 (6개)
purecvisor_zpool_size_bytes{pool="pcvpool"}
purecvisor_zpool_used_bytes{pool="pcvpool"}
purecvisor_zpool_free_bytes{pool="pcvpool"}
purecvisor_zpool_capacity_percent{pool="pcvpool"}
purecvisor_zpool_fragmentation_percent{pool="pcvpool"}
purecvisor_zpool_health{pool="pcvpool"}

# 파일시스템 메트릭
node_filesystem_size_bytes{mountpoint="/",fstype="ext4"}
node_filesystem_avail_bytes{mountpoint="/",fstype="ext4"}
node_filesystem_files{mountpoint="/"}

# Disk I/O 메트릭
node_disk_reads_completed_total{device="sda"}
node_disk_writes_completed_total{device="sda"}
node_disk_read_bytes_total{device="sda"}
node_disk_written_bytes_total{device="sda"}
node_disk_io_time_seconds_total{device="sda"}
```

---

## 6. 네트워크

### 6.1 브릿지 네트워크

#### 네트워크 생성

```bash
# NAT 모드 (기본, 인터넷 접근 가능)
pcvctl network create --name mgmt-net --mode nat --cidr 192.0.2.10/24

# 브릿지 모드 (물리 NIC 직접 연결)
pcvctl network create --name prod-net --mode bridge --bridge pcvbr0

# 격리 모드 (외부 접근 불가)
pcvctl network create --name test-net --mode isolated --cidr 10.0.0.0/24

# 라우티드 모드 (NAT 없이 라우팅)
pcvctl network create --name routed-net --mode routed --cidr 172.16.0.0/24
```

| 모드 | 설명 | 외부 접근 | DHCP |
|------|------|----------|------|
| `nat` | iptables/nftables MASQUERADE | O (NAT) | 자동 |
| `bridge` | 물리 NIC 브릿지 | O (직접) | 선택 |
| `isolated` | 외부 격리, VM 간만 통신 | X | 자동 |
| `routed` | 정적 라우팅 | O (라우팅) | 선택 |

#### 네트워크 관리

```bash
# 네트워크 목록
pcvctl network list

# 네트워크 상세 정보
pcvctl network info mgmt-net

# DHCP 토글
pcvctl network dhcp mgmt-net --enable
pcvctl network dhcp mgmt-net --disable

# 물리 NIC 바인딩
pcvctl network bind mgmt-net eno1

# 삭제 (멱등: 존재하지 않아도 성공)
pcvctl network delete mgmt-net
```

RPC 직접 호출:

```bash
# 네트워크 생성
echo '{"jsonrpc":"2.0","method":"network.create","params":{
  "name": "mgmt-net",
  "mode": "nat",
  "cidr": "192.0.2.10/24"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 네트워크 목록
echo '{"jsonrpc":"2.0","method":"network.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **메타데이터 영속화**: 브릿지 모드/CIDR 정보는 `/var/run/purecvisor/dnsmasq-<bridge>.meta` JSON 파일에 저장됩니다.

> **멱등 삭제**: `network.delete`는 대상이 없어도 성공을 반환합니다 (재시도 안전).

### 6.2 방화벽 (nftables)

PureCVisor는 nftables 기반 방화벽을 사용합니다.

```bash
# 현재 규칙 조회
sudo nft list table inet purecvisor

# 방화벽 규칙 추가 (RPC)
echo '{"jsonrpc":"2.0","method":"firewall.rule.add","params":{
  "chain": "input",
  "protocol": "tcp",
  "dport": 8080,
  "action": "accept"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **Command Injection 방어**: 모든 방화벽 조작은 `pcv_spawn_sync()` argv 배열로 실행됩니다. `system()`/`popen()`은 전면 제거되었습니다.

### 6.3 VLAN 필터링

OVS 브릿지에서 VLAN 태깅을 사용할 수 있습니다.

```bash
# VLAN 추가
echo '{"jsonrpc":"2.0","method":"network.vlan.add","params":{
  "bridge": "pcvbr0",
  "interface": "vnet0",
  "vlan_id": 100
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# VM 생성 시 VLAN 지정
pcvctl vm create web-vlan --vcpu 2 --memory_mb 2048 --disk_size_gb 20 \
  --network_bridge pcvbr0
```

### 6.4 QoS (트래픽 제어)

tc (traffic control) 기반 네트워크 QoS를 지원합니다.

```bash
# 인터페이스 기반 QoS 설정
pcvctl network qos-set vnet0 --rate 100000 --burst 64000

# QoS 조회
pcvctl network qos-get vnet0

# QoS 제거
pcvctl network qos-remove vnet0
```

> **영속화**: QoS 규칙은 `/var/run/purecvisor/qos_rules.json`에 저장되어 데몬 재시작 시 자동 복원됩니다.

### 6.5 OVS VXLAN 오버레이

> **에디션 경계**: 오버레이 코어(`create/delete/list/info/add_peer/remove_peer`)는 Single Edge 공개 범위에 포함됩니다. 다만 자동 풀메시와 피어 기반 자동화는 비공개 멀티 제어면 참고 범위이며, 이 리포의 출시 표면에는 포함되지 않습니다.

Open vSwitch 기반 VXLAN 오버레이 네트워크를 구성합니다.

#### 수동 구성

```bash
# 오버레이 생성
pcvctl overlay create --name pcvoverlay0 --vni 100 --cidr 10.100.0.1/24

# 피어 추가 (VXLAN 터널 자동 생성)
pcvctl overlay add-peer pcvoverlay0 192.0.2.20
pcvctl overlay add-peer pcvoverlay0 192.0.2.21

# 오버레이 목록
pcvctl overlay list

# 오버레이 상세 정보
pcvctl overlay info pcvoverlay0

# 피어 제거
pcvctl overlay remove-peer pcvoverlay0 192.0.2.21

# 오버레이 삭제
pcvctl overlay delete pcvoverlay0
```

RPC 직접 호출:

```bash
# 오버레이 목록
echo '{"jsonrpc":"2.0","method":"overlay.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 오버레이 상세
echo '{"jsonrpc":"2.0","method":"overlay.info","params":{
  "name": "pcvoverlay0"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 자동 프로비저닝

`daemon.conf [overlay]` 섹션을 설정하면 부팅 시 기본 브리지가 자동 생성됩니다. Single Edge에서는 로컬 브리지 생성까지만 자동화되며, 피어 기반 자동 풀메시는 공개 범위 밖 멀티 제어면 참고 기능으로 남겨 둡니다.

Single Edge는 overlay 코어 기능을 지원하지만, `tunnel_ip`는 자동 추론하지 않습니다. 따라서 Single Edge에서 overlay를 활성화하려면 운영자가 `[overlay]` 섹션에 `tunnel_ip`를 명시해야 합니다. 이 값은 로컬 호스트가 VXLAN 터널 소스로 사용할 IP이며, Single Edge에서는 운영 가이드에 따라 명시적으로 관리합니다.

```ini
[overlay]
name = pcvoverlay0
vni = 100
cidr = 10.100.0.1/24
tunnel_ip = 192.0.2.19
peers = 192.0.2.20,192.0.2.21
```

OVS 상태 확인:

```bash
sudo ovs-vsctl show
```

### 6.6 OVN SDN

> **에디션 경계**: OVN 코어(`status`, switch/router/ACL/NAT/DHCP/tenant/vm_port`)는 Single Edge 공개 범위에 포함됩니다. Single Edge는 서비스 기동 시 local OVN controller를 자동 준비해 로컬 SDN 데이터면을 구성하고, encap 설정과 auto-provision 자동화는 공개 범위 밖 멀티 제어면 참고 기능으로 남겨 둡니다.

OVN (Open Virtual Network) 기반 소프트웨어 정의 네트워크를 지원합니다.

#### OVN 상태

```bash
pcvctl ovn status
```

#### 논리 스위치

```bash
# 스위치 생성
pcvctl ovn switch create --name ls-web

# 스위치 목록
pcvctl ovn switch list

# 스위치 상세
pcvctl ovn switch detail ls-web

# 스위치 삭제
pcvctl ovn switch delete ls-web
```

#### 논리 라우터

```bash
# 라우터 생성
pcvctl ovn router create --name lr-main

# 라우터에 스위치 포트 연결
pcvctl ovn router add-port lr-main ls-web --cidr 10.0.1.1/24

# 라우터 목록
pcvctl ovn router list

# 라우터 삭제
pcvctl ovn router delete lr-main
```

#### ACL (접근 제어)

```bash
# 인그레스 규칙 추가
pcvctl ovn acl add ls-web --direction ingress --priority 100 \
  --match "tcp.dst==80" --action allow

# 이그레스 규칙 추가
pcvctl ovn acl add ls-web --direction egress --priority 50 \
  --match "tcp.dst==443" --action allow

# ACL 목록
pcvctl ovn acl list ls-web

# ACL 삭제
pcvctl ovn acl remove ls-web --priority 100 --direction ingress
```

#### NAT

```bash
# SNAT 규칙 추가
pcvctl ovn nat add lr-main --type snat \
  --external 203.0.113.1 --logical 10.0.1.0/24

# DNAT 규칙 추가
pcvctl ovn nat add lr-main --type dnat \
  --external 203.0.113.1 --logical 10.0.1.10

# NAT 목록
pcvctl ovn nat list lr-main

# NAT 삭제
pcvctl ovn nat remove lr-main --type snat --external 203.0.113.1
```

#### DHCP

```bash
# OVN DHCP 옵션 설정
pcvctl ovn dhcp set ls-web --cidr 10.0.1.0/24 \
  --router 10.0.1.1 --dns 8.8.8.8

# DHCP 옵션 조회
pcvctl ovn dhcp list ls-web
```

### 6.7 보안 그룹

NFV 스타일의 보안 그룹으로 VM/컨테이너에 네트워크 정책을 적용합니다.

```bash
# 보안 그룹 생성
pcvctl sg create --name web-sg --description "Web server security group"

# 인그레스 규칙 추가 (HTTP/HTTPS 허용)
pcvctl sg rule add web-sg --direction ingress --protocol tcp \
  --port 80-443 --source 0.0.0.0/0

# SSH 허용 (특정 소스에서만)
pcvctl sg rule add web-sg --direction ingress --protocol tcp \
  --port 22 --source 192.0.2.10/24

# 이그레스 전체 허용
pcvctl sg rule add web-sg --direction egress --protocol tcp \
  --port 0 --source 0.0.0.0/0

# VM에 보안 그룹 적용
pcvctl sg apply web-sg --vm web-prod

# 보안 그룹 목록
pcvctl sg list

# 규칙 목록
pcvctl sg rule list web-sg

# 규칙 삭제
pcvctl sg rule remove web-sg --rule-id 3
```

> **영속화**: 보안 그룹은 SQLite에 저장되며, 데몬 재시작 시 nftables 규칙이 자동 복원됩니다. default-deny 정책이 기본 적용됩니다.

### 6.8 DPDK

고성능 데이터 플레인을 위한 DPDK (Data Plane Development Kit) 통합을 지원합니다.

```bash
# DPDK 상태
pcvctl dpdk status

# NIC 바인딩 (vfio-pci 드라이버)
pcvctl dpdk bind 0000:03:00.0 --driver vfio-pci

# NIC 언바인딩 (원래 드라이버로 복원)
pcvctl dpdk unbind 0000:03:00.0

# DPDK 브릿지 생성
pcvctl dpdk bridge create --name dpdk-br0

# HugePages 정보
pcvctl dpdk hugepage info
```

### 6.9 SR-IOV

SR-IOV를 사용하여 물리 NIC의 가상 기능(VF)을 VM에 직접 할당합니다.

```bash
# SR-IOV 지원 NIC 상태
pcvctl sriov status eno2

# VF 활성화 (4개)
pcvctl sriov enable eno2 --num-vfs 4

# VF 목록
pcvctl sriov list eno2

# VM에 VF 할당
pcvctl sriov attach web-prod --vf 0000:03:10.0

# VM에서 VF 분리
pcvctl sriov detach web-prod --vf 0000:03:10.0
```

### 6.10 네트워크 디버깅

```bash
# 브릿지 상태
ip link show type bridge
brctl show

# OVS 상태
sudo ovs-vsctl show

# nftables 규칙
sudo nft list table inet purecvisor

# VXLAN 터널 상태
ip -d link show type vxlan
```

### 6.11 네트워크 Prometheus 메트릭

```
# 네트워크 인터페이스 메트릭
node_network_receive_bytes_total{device="eno1"}
node_network_transmit_bytes_total{device="eno1"}
node_network_receive_errors_total{device="eno1"}
node_network_transmit_errors_total{device="eno1"}
node_network_receive_drop_total{device="eno1"}
node_network_transmit_drop_total{device="eno1"}

# 소켓 통계
node_sockstat_TCP_inuse
node_sockstat_TCP_tw
node_sockstat_UDP_inuse

# conntrack
node_nf_conntrack_entries
```

## 7. 멀티 제어면 참고 기록

Single Edge 공개판은 클러스터 HA, 라이브 마이그레이션, 페더레이션, 노드 드레인/리밸런싱을 운영 기능으로 제공하지 않는다. 이 리포에서 사용자가 그대로 따라 할 수 있는 절차는 단일 노드 운영으로 제한한다.

현재 판단 기준:

- `purecvisor-single`의 지원 표면은 단일 노드 `purecvisorsd`와 Single Edge UI/API/CLI다.
- 클러스터/페더레이션/라이브 마이그레이션 절차는 공개판 기능 장으로 작성하지 않는다.
- 역사적 설계 판단은 [ADR_INDEX.md](ADR_INDEX.md)와 `docs/adr/`에서 확인한다.
- 공개판 경계 검증은 [PUBLIC_RELEASE_BOUNDARY.md](PUBLIC_RELEASE_BOUNDARY.md)를 따른다.

멀티 제어면이 필요한 배포는 이 공개 리포가 아니라 별도 Multi Edge 범위에서 설계, 검증, 출시 판정을 수행해야 한다.

---

## 8. 모니터링 & 알림

PureCVisor는 외부 에이전트 없이 자체 메트릭 수집, 알림 엔진, Prometheus 호환 엔드포인트를 내장한다.

### 8.1 자체 node_exporter

`ebpf_telemetry.c`가 10개 콜렉터로 ~178개 Prometheus 메트릭을 직접 수집한다 (node_* 126 + purecvisor_* 52). 별도의 node_exporter 설치가 불필요하다.

| 콜렉터 | 메트릭 수 | 수집 내용 |
|--------|----------|----------|
| CPU | ~18 | per-core 9모드 (user/system/idle/iowait/irq/softirq/steal/nice/guest) |
| Memory | ~60+ | meminfo 전 필드 (Slab/SReclaimable/Swap/PageFault) |
| Diskstats | ~12 | IOPS, read/write bytes, IO time |
| Filesystem | ~6 | 마운트별 total/free/used |
| Netdev | ~8 | rx/tx bytes/packets, Error, Drop |
| VMstat | ~4 | pgfault, pgmajfault, pswpin, pswpout |
| Sockstat | ~4 | TCP/UDP 소켓 수 |
| Pressure (PSI) | ~6 | CPU/memory/io some/full |
| Hwmon | 가변 | 온도, 팬 속도 |
| Misc | ~8 | boot_time, entropy, filefd, conntrack, ARP, NIC meta |

추가 `purecvisor_*` 메트릭 (~44개):
- `purecvisor_cb_state` / `purecvisor_cb_failures` (서킷 브레이커)
- `purecvisor_vm_locks_held` (VM 상태 잠금)
- `purecvisor_tls_cert_expiry_days` (인증서 만료)
- `purecvisor_zpool_*` (6개: size/alloc/free/frag/cap/health)
- `purecvisor_worker_pool_pending` (GTask 큐 깊이)
- `purecvisor_audit_queue_depth` (감사 큐)
- `purecvisor_connpool_idle/active/max` (커넥션 풀)

```bash
# Prometheus 메트릭 엔드포인트 (인증 불필요)
curl -s http://localhost:80/api/v1/metrics

# 특정 메트릭 필터
curl -s http://localhost:80/api/v1/metrics | grep purecvisor_cb_state
```

### 8.2 프로세스 모니터

`process_monitor.c`가 `/proc/[pid]/stat+io`를 20초 주기로 스캔하여 프로세스별 CPU%, 메모리, I/O를 추적한다.

| 항목 | 값 |
|------|-----|
| 스캔 주기 | 20초 |
| 최대 추적 수 | 512 프로세스 |
| CPU% 계산 | delta(utime+stime) / delta(total) |
| 정렬 | CPU% 내림차순 Top N |

```bash
# CLI
pcvctl monitor processes --top 10

# REST
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/processes | python3 -m json.tool

# RPC
echo '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":10},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

### 8.3 알림 엔진

WhaTap 지속 조건(sustained condition) 모델을 기반으로 한 알림 시스템.

| 파라미터 | 기본값 | daemon.conf 키 |
|---------|--------|----------------|
| 평가 주기 | 5초 | 하드코딩 |
| eval_period | 30초 | `[alert] eval_period` |
| dedup_window | 300초 | `[alert] dedup_window` |
| CPU 경고/위험 | 80% / 95% | `[alert] cpu_warn` / `cpu_crit` |
| Memory 경고/위험 | 85% / 95% | `[alert] mem_warn` / `mem_crit` |
| Disk 경고/위험 | 80% / 90% | `[alert] disk_warn` / `disk_crit` |
| 히스토리 | 1000건 링버퍼 | 하드코딩 |

알림 평가 흐름:
```
5초 주기 → 메트릭 샘플링 → 임계값 비교
  → eval_period(30초) 연속 초과 확인
    → dedup_window(300초) 내 중복 제거
      → 웹훅 발송 + 히스토리 기록
```

#### DataPool 디스크 모니터링 (v1.0)

ZFS pool 사용량을 `purecvisor_zpool_*` 메트릭으로 수집하고, disk_warn/disk_crit 임계값 초과 시 알림을 발생시킨다.

### 8.4 비동기 웹훅

GTask 스레드에서 비동기로 웹훅을 발송하여 메인 루프를 블로킹하지 않는다.

| 형식 | 설정값 | 페이로드 |
|------|--------|---------|
| Slack | `webhook_format=slack` | `{"text": "..."}` |
| Telegram | `webhook_format=telegram` | `{"chat_id": "...", "text": "..."}` |
| Generic | `webhook_format=generic` | PureCVisor JSON 전문 |

모든 웹훅에 HMAC-SHA256 서명 헤더(`X-PCV-Signature`)를 포함한다.

```ini
# daemon.conf
[alert]
enabled=true
cpu_warn=80
cpu_crit=95
mem_warn=85
mem_crit=95
disk_warn=80
disk_crit=90
eval_period=30
dedup_window=300
webhook_url=https://hooks.slack.com/services/T.../B.../xxx
webhook_format=slack
```

### 8.5 알림 ACK & 에스컬레이션 (v1.0)

미확인(unacknowledged) 알림은 10분 후 자동 재전송된다.

```bash
# 알림 히스토리 조회
pcvctl alert list

# 알림 확인(ACK)
pcvctl alert ack --id <alert-id>

# REST
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/alerts | python3 -m json.tool

# RPC
echo '{"jsonrpc":"2.0","method":"alert.history","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

### 8.6 SLA 추적 (v1.0)

VM별 가동 시간을 추적하여 `uptime_percent`를 계산한다.

```bash
# RPC
echo '{"jsonrpc":"2.0","method":"vm.sla","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

### 8.7 per-VM 알림 라우팅 (v1.0)

VM별로 별도의 웹훅 대상을 지정할 수 있다.

```bash
# RPC
echo '{"jsonrpc":"2.0","method":"alert.set_config","params":{
  "vm_name":"web-prod",
  "webhook_url":"https://hooks.slack.com/services/...",
  "webhook_format":"slack"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

### 8.8 복합 알림 규칙

AND/OR 조건으로 최대 8개 조건을 조합한 복합 알림 규칙을 생성할 수 있다.

```json
{
  "jsonrpc": "2.0",
  "method": "alert.set_config",
  "params": {
    "compound_rules": [
      {
        "operator": "AND",
        "conditions": [
          {"metric": "cpu_percent", "op": ">", "value": 90},
          {"metric": "mem_percent", "op": ">", "value": 85}
        ]
      }
    ]
  },
  "id": "1"
}
```

### 8.9 DLQ (Dead Letter Queue)

웹훅 발송 실패 시 DLQ에 최대 1000건까지 보관하며, 재시도 가능하다.

### 8.10 Prometheus 연동

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'purecvisor'
    scrape_interval: 15s
    metrics_path: '/api/v1/metrics'
    static_configs:
      - targets:
        - '192.0.2.19:80'
        - '192.0.2.20:80'
        - '192.0.2.21:80'
```

Grafana 대시보드: `192.0.2.10:3000` (NEON CORE 통합 대시보드)

### 8.11 WebSocket 메트릭 Push (v1.0)

`ws://localhost:80/api/v1/ws/events`로 10초 주기 실시간 메트릭을 push한다.

```javascript
const ws = new WebSocket('ws://localhost:80/api/v1/ws/events');
ws.onmessage = (e) => {
    const data = JSON.parse(e.data);
    // { type: "metrics", cpu: 45.2, mem: 62.1, ... }
};
```

---

## 9. 백업 & 복원

### 9.1 정책 기반 자동 백업

`backup_scheduler.c`가 ZFS 스냅샷 기반으로 자동 백업을 수행한다.

| 파라미터 | 설명 | 기본값 |
|---------|------|--------|
| interval_hours | 백업 주기 (시간) | 24 |
| retention_count | 보존할 스냅샷 수 | 7 |
| 대상 | VM 이름 또는 `*` (전체) | - |

```bash
# 정책 설정 — 전체 VM 일일 백업, 7일 보존
pcvctl backup set '*' --interval 24 --retention 7

# 특정 VM 정책
pcvctl backup set web-prod --interval 12 --retention 14

# 정책 목록
pcvctl backup list

# 히스토리
pcvctl backup history web-prod
```

### 9.2 fsfreeze/thaw (v1.0)

Guest Agent가 설치된 VM에서 `fsfreeze --freeze` / `--thaw`를 통해 파일시스템을 정지시킨 후 스냅샷을 생성한다. 애플리케이션 정합성(application-consistent) 스냅샷을 보장한다.

### 9.3 무결성 검증 (v1.0)

백업 완료 후 `zfs list` 명령으로 스냅샷 존재 여부와 크기를 검증한다.

### 9.4 GTask 비동기 (v1.0)

백업 작업은 GTask 스레드에서 비동기 실행되어 메인 GMainLoop를 블로킹하지 않는다. fire-and-forget 패턴:

```
클라이언트 요청 → 즉시 "accepted" 응답 → GTask 백그라운드 실행
```

### 9.5 원격 ZFS 복제 (범위 밖)

원격 노드 증분 복제는 Multi 제어면과 연결되는 범위이므로 Single Edge 공개판의 운영 절차로 제공하지 않는다. Single Edge에서는 로컬 ZFS snapshot, 로컬 백업 정책, S3 업로드처럼 단일 노드에서 검증 가능한 백업 경로를 우선 사용한다.

### 9.6 원격 보존 정책 (v1.0)

원격 노드 보존 정책은 Multi 제어면 참고 범위다. Single Edge 공개판에서는 로컬 snapshot 보존 정책을 기준으로 운영한다.

### 9.7 S3 업로드 (v1.0)

100MB 초과 시 멀티파트 업로드를 사용하여 AWS S3에 백업을 업로드한다.

```bash
# RPC
echo '{"jsonrpc":"2.0","method":"backup.s3_upload","params":{
  "name":"web-prod",
  "snapshot":"auto-20260401",
  "bucket":"purecvisor-backups",
  "region":"ap-northeast-2"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

### 9.8 히스토리 페이지네이션 (v1.0)

```bash
# REST — 페이지네이션
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://localhost:80/api/v1/backups/web-prod/history?offset=0&limit=20"
```

### 9.9 스냅샷 일괄 삭제

prefix 필터와 keep_recent 보존 옵션을 지원하는 일괄 삭제:

```bash
# auto- 접두사 스냅샷 중 최근 3개 보존, 나머지 삭제
echo '{"jsonrpc":"2.0","method":"vm.snapshot.delete_all","params":{
  "name":"web-prod",
  "prefix":"auto-",
  "keep_recent":3
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

---

## 10. 보안

PureCVisor는 엔터프라이즈급 다계층 보안 아키텍처를 구현한다. 총 63건의 보안 고도화가 완료되었다.

### 10.1 인증

#### JWT HS256

| 항목 | 값 |
|------|-----|
| 알고리즘 | HS256 (HMAC-SHA256) |
| Access Token 만료 | 15분 |
| Refresh Token 만료 | 7일 |
| IP 바인딩 (v1.0) | Access Token에 클라이언트 IP 포함, 불일치 시 거부 |
| 비밀 키 | `daemon.conf [auth] jwt_secret` (빈 값 = 랜덤 생성) |

```bash
# 토큰 발급
TOKEN=$(curl -s -X POST http://localhost:80/api/v1/auth/token \
  -d '{"username":"admin","password":"<configured-admin-password>"}' \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['access_token'])")

# 토큰 갱신
curl -s -X POST http://localhost:80/api/v1/auth/refresh \
  -d "{\"refresh_token\":\"$REFRESH_TOKEN\"}"

# API 호출
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms
```

#### PBKDF2-SHA256 패스워드 해싱 (v1.0)

100,000 iterations + 32바이트 랜덤 salt. 기존 평문 패스워드는 첫 로그인 시 자동 마이그레이션된다.

### 10.2 RBAC 3-Tier

| 역할 | 레벨 | 권한 |
|------|------|------|
| VIEWER | 0 | 읽기 전용 (list, status, metrics) |
| OPERATOR | 1 | VIEWER + VM/컨테이너 lifecycle. VM 단일 대상 action은 자신이 생성한 VM에 한정 |
| ADMIN | 2 | 전체 권한 (사용자 관리, 설정 변경, 클러스터 조작) |

#### VM owner-scope

operator의 VM 단일 대상 action은 libvirt domain metadata의 `pcv:owner`와 호출자 subject가 일치해야 허용된다. `admin`은 전역 관리 권한을 유지한다.

- `vm.create`는 생성자 subject를 `pcv:owner` metadata에 기록한다.
- `vm.start`, `vm.stop`, `vm.delete`, snapshot, ISO, VM NIC hotplug, VNC 등 단일 VM action은 operator 호출 시 owner 일치 여부를 검사한다.
- VM NIC 추가/제거(`device.nic.attach`, `device.nic.detach`)는 operator에게 열려 있지만, 자기 VM이 아니면 owner-scope에서 403으로 거부된다.
- `vm.clone`은 [ADR-0023](adr/0023-vm-clone-beta-safety-guard.md)에 따라 source VM owner-scope를 통과한 operator와 admin에게 열려 있으며, source VM `shut off` 상태와 준비된 템플릿 확인 또는 libguestfs 기반 guest reset 경로를 요구한다.
- `vm.batch`는 요청에 포함된 모든 VM owner가 호출자와 일치할 때만 실행한다.
- owner metadata가 없는 기존 VM은 operator action이 거부된다. 소유권 정리와 복구는 admin 경로에서 처리한다.

#### Bootstrap admin과 전용 admin

Single Edge는 관리자 계정을 두 층으로 운영합니다.

- `bootstrap admin`: `daemon.conf` 또는 `PURECVISOR_ADMIN_PASSWORD`에 명시된 경우에만 활성화되는 초기/비상 진입 계정입니다. 내장 기본 비밀번호는 없으며, 비밀번호 변경은 설정 파일 수정과 서비스 재기동으로 관리합니다.
- `전용 admin`: RBAC DB에 별도로 생성한 운영용 관리자 계정입니다. 평소 운영, 감사 추적, 계정 회전은 이 계정을 기준으로 수행합니다.

권장 순서:
1. `admin_password`를 명시 설정한 뒤 bootstrap admin으로 첫 로그인
2. `pcvctl auth create <name> <password> admin`으로 전용 admin 생성 (또는 `POST /api/v1/auth/users`)
3. `daemon.conf`의 bootstrap admin 비밀번호를 강한 값으로 교체
4. 이후 일상 운영과 API 사용은 전용 admin으로 수행

> **평문 완전 제거 변형(선택)**: 3단계에서 강한 값으로 교체하는 대신 `admin_password=`를 **비우고** 데몬을 재기동하면 bootstrap admin이 비활성화되어 `daemon.conf`에 평문 자격증명이 전혀 남지 않는다. 전용 admin(RBAC DB 해시)만으로 운영. **트레이드오프**: 비상 진입 계정이 사라지므로, 전용 admin 자격증명 분실 시 다시 `admin_password`를 설정·재기동해야 재진입할 수 있다.
>
> **⚠️ 파일 권한**: `daemon.conf`는 `admin_password` 등 자격증명을 담으므로 **반드시 `chmod 600 root:root`** 여야 한다(world-readable 금지). `.deb` 설치는 postinst가 자동으로 `0600`을 강제한다. 소스/수동 설치 시 직접 확인할 것.

per-user 쿼터 (v1.0): 사용자별 VM 수, CPU 코어, 메모리 상한 설정 가능.

```bash
# 전용 admin 포함 사용자 관리
pcvctl auth list
pcvctl auth create ops-admin strongpass admin
pcvctl auth create dev pass123 operator
pcvctl auth role dev admin
pcvctl auth delete dev

# RPC
echo '{"jsonrpc":"2.0","method":"auth.create_user","params":{
  "username":"dev","password":"pass123","role":"operator"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

### 10.3 API Key

해시 저장, 만료일 설정, 해지(revoke) 지원.

```bash
# API Key 발급
pcvctl auth apikey create --name "ci-pipeline" --expires 90d

# API Key 해지
pcvctl auth apikey revoke --key-id "pk_abc123"

# API Key로 인증
curl -s -H "X-API-Key: pk_abc123.secret" \
  http://localhost:80/api/v1/vms
```

### 10.4 JWT Bearer와 CSRF 정책

PureCVisor REST API는 쿠키 기반 세션을 쓰지 않고 `Authorization: Bearer <JWT>` 헤더만 사용한다. 브라우저가 Authorization 헤더를 자동 첨부하지 않으므로 별도 `X-CSRF-Token`은 사용하지 않는다. 이 결정은 [ADR-0014](adr/0014-remove-csrf-jwt-bearer.md)를 따른다.

### 10.5 Rate Limiting

| 대상 | 제한 | 단위 |
|------|------|------|
| IP 기본 | 600 | 요청/분 |
| User 기본 | 1,200 | 요청/분 |
| Auth 엔드포인트 | 60 | 요청/분 |
| Metrics 엔드포인트 | 3,600 | 요청/시간 |
| per-endpoint (v1.0) | 설정 가능 | 엔드포인트별 |

429 응답 시 `Retry-After` 헤더 포함.

### 10.6 보안 그룹

SQLite 기반 보안 그룹 CRUD + nftables 규칙 자동 적용.

```bash
# 보안 그룹 생성
pcvctl security-group create web-sg

# 규칙 추가
pcvctl security-group add-rule web-sg --direction ingress \
  --protocol tcp --port 80 --source 0.0.0.0/0

pcvctl security-group add-rule web-sg --direction ingress \
  --protocol tcp --port 443 --source 0.0.0.0/0

# VM에 적용
pcvctl security-group attach web-sg --vm web-prod
```

### 10.7 Native Host HIDS/HIPS

Security Guard는 Single Edge 호스트 노드를 보호하는 탐지 우선 HIDS/HIPS 기능이다.
기본값은 비활성이며, v1은 자동 차단보다 감사, 알림, 운영자 승인 대응을 우선한다.
Web UI에서는 `관제 > 보안 이벤트`에서 이벤트 큐, 감사 상관키, 권고 대응, 승인 대기열을 확인한다.
CLI는 `pcvctl security ...` 명령군을 제공하고, TUI는 `F5 HOST` 화면에서 Security Guard 운영 단축키를 제공한다.

#### 기능 범위

| 구분 | 역할 | v1 동작 |
|------|------|---------|
| HIDS | 호스트 파일 무결성과 런타임 보안 이벤트 탐지 | admin이 명시한 파일 baseline을 저장하고 변경 상태를 `trusted`, `stale`, `unknown`으로 노출한다. |
| HIPS | 탐지 이벤트에 대한 대응 후보 생성 | `pending` action을 만들고, 운영자가 승인한 경우에만 실행한다. |
| 감사 상관 | 보안 이벤트와 감사 로그 연결 | WARN/CRIT 이벤트는 `security.event` audit을 남기며 audit `target`은 같은 `event_id`를 사용한다. |
| 운영 인터페이스 | Web UI, CLI, TUI, UDS | 같은 `security.*` RPC를 사용하므로 이벤트와 pending action 상태가 일관된다. |

HIDS baseline은 처음에는 `unknown`이다. 운영자가 신뢰할 수 있는 시점에 핵심 파일을 지정해 refresh해야 `trusted`가 된다.
HIPS는 자동 차단 제품처럼 동작하지 않는다. v1에서 승인 후 실제 실행 가능한 대응은 `block_ip`와 `revoke_api_key`뿐이고,
`lock_user`, `restart_service`, `quarantine_process`, `restore_config` 계열은 `manual_runbook`으로만 남긴다.

#### 운영 흐름

1. `pcvctl security status`로 Security Guard 활성 여부, baseline 상태, open risk, pending action 수를 확인한다.
2. 신규 서버나 배포 직후에는 핵심 설정 파일을 명시해 HIDS baseline을 refresh한다.
3. Security Guard를 활성화한 뒤 Web UI, CLI, TUI 중 하나로 보안 이벤트 큐를 확인한다.
4. 이벤트 상세의 evidence, target, recommended action을 확인하고 승인 또는 거부한다.
5. 승인 결과는 job 완료 상태와 `security.action.approve` audit으로 확인한다.

#### 실용 예제

신규 서버에서 처음 켤 때는 baseline을 먼저 확정하고 guard를 활성화한다.

```bash
pcvctl security status
pcvctl security baseline-refresh \
  --path /etc/purecvisor/daemon.conf \
  --path /var/lib/purecvisor/rbac.db
pcvctl security baseline-status
pcvctl security enable
pcvctl security status
```

일일 점검은 open 이벤트와 pending action을 먼저 본다.

```bash
pcvctl security events --limit 20 --status open
pcvctl security pending
pcvctl security event <event_id>
```

무차별 인증 시도처럼 IP 차단 권고가 나온 이벤트는 상세를 확인한 뒤 승인한다.

```bash
pcvctl security event <event_id>
pcvctl security approve <event_id>
pcvctl job list
pcvctl audit search --method security.action.approve --limit 10
```

정상 작업 창에서 발생한 설정 변경이나 수동 조치가 필요한 이벤트는 사유를 남기고 닫는다.

```bash
pcvctl security dismiss <event_id> --reason "approved maintenance window"
pcvctl audit search --method security.action.dismiss --limit 10
```

API key 유출 의심 이벤트는 target이 client name인지 확인한 뒤 승인한다.
승인하면 `revoke_api_key` action이 비동기 job으로 처리되고 결과는 job completion과 audit에 남는다.

```bash
pcvctl security event <event_id>
pcvctl security approve <event_id>
pcvctl job list
```

#### 인터페이스별 사용

Web UI에서는 `관제 > 보안 이벤트`에서 Security Guard 토글, 이벤트 상세, evidence, 감사 상관키, 승인/거부 버튼을 사용한다.

`pcvtui`에서는 `F5 HOST` 화면에서 다음 단축키를 사용한다.

| 키 | 기능 |
|----|------|
| `S` | Security Guard 상태 조회 |
| `T` | Security Guard 활성/비활성 토글 |
| `E` | 최근 보안 이벤트 조회 |
| `P` | 승인 대기 HIPS action 조회 |
| `A` | event_id 입력 후 HIPS action 승인 |
| `D` | event_id 입력 후 HIPS action 거부 |
| `B` | 파일 경로 입력 후 HIDS baseline refresh |

CLI 기본 명령은 다음과 같다.

```bash
# CLI 상태와 이벤트 조회
pcvctl security status
pcvctl security events --limit 20 --status open
pcvctl security event <event_id>
pcvctl security pending

# HIPS 승인/거부
pcvctl security approve <event_id>
pcvctl security dismiss <event_id> --reason "operator dismissed"

# baseline과 guard 토글
pcvctl security baseline-status
pcvctl security baseline-refresh --path /etc/purecvisor/daemon.conf --path /var/lib/purecvisor/rbac.db
pcvctl security enable
pcvctl security disable
```

#### UDS 직접 점검

REST/API 계층이 의심될 때는 daemon UDS에 직접 RPC를 보내 같은 상태를 확인한다.

```bash
# 상태 조회
echo '{"jsonrpc":"2.0","method":"security.config.get","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# baseline 상태 조회
echo '{"jsonrpc":"2.0","method":"security.baseline.status","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# admin 명시 baseline refresh
echo '{"jsonrpc":"2.0","method":"security.baseline.refresh","params":{"admin_user":"ops-admin","paths":["/etc/purecvisor/daemon.conf","/var/lib/purecvisor/rbac.db"]},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# Security Guard 활성화
echo '{"jsonrpc":"2.0","method":"security.config.set","params":{"admin_user":"ops-admin","enabled":true},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

CLI/TUI 표면 회귀는 다음 스크립트로 확인한다.

```bash
bash tests/integration/test_security_cli_tui_surface.sh
```

#### 운영 주의

- baseline이 `unknown`인 상태에서 guard만 켜면 파일 무결성 판단 기준이 없다. 신규 설치, 설정 변경, RBAC DB 교체 후에는 baseline을 먼저 갱신한다.
- HIPS action은 `pending` 상태에서만 승인한다. 이미 닫힌 이벤트를 다시 실행 대상으로 쓰지 않는다.
- `manual_runbook` 이벤트는 자동 실행하지 않는다. 운영자가 이벤트 evidence를 보고 별도 절차로 처리한 뒤 dismiss한다.
- `security.config.set`은 runtime security DB의 `enabled` 값을 바꾼다. `/etc/purecvisor/daemon.conf`를 자동 수정하지 않는다.

### 10.8 시스템 보안

| 기능 | 구현 |
|------|------|
| Seccomp | 불필요 syscall 차단 |
| CAP_NET_BIND_SERVICE | 포트 80/443 바인딩 (루트 불필요) |
| Command Injection 방어 | `system()`/`popen()` 전면 제거 → `pcv_spawn_sync()` argv 배열 |
| XSS 방어 | `esc()` HTML 이스케이프 |
| SQL Injection 방어 | SQLite prepared statement |
| Path Traversal 방어 | `realpath()` 정규화 |
| CORS | 화이트리스트 기반 |
| WebSocket 인증 | JWT 검증 |
| 플러그인 검증 | 심링크 탐지 차단 |
| Webhook SSRF 차단 | 내부 IP 대역 필터링 |
| 시크릿 관리 | `PCV_SECRET_*` 환경변수 / `ENC:base64` daemon.conf 암호화 (v1.0) |

### 10.9 STONITH IPMI 펜싱

클러스터 분할(split-brain) 방지를 위해 장애 노드의 전원을 IPMI를 통해 강제 차단한다.

### 10.10 감사 로그

| 항목 | 값 |
|------|-----|
| 저장소 | `/var/lib/purecvisor/pcv_audit.db` (SQLite WAL) |
| 텍스트 로그 | `/var/log/purecvisor/audit.log` |
| 보존 기간 | 30일 (1시간 주기 정리) |
| 최대 크기 | 1GB (`PRAGMA max_page_count`) |
| Rate Limit | 1,000건/초 (토큰 버킷) |
| 큐 | GAsyncQueue 비동기 |

```bash
# 감사 로그 조회
pcvctl audit list --limit 50

# REST
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/audit?limit=50
```

---

## 11. 클라우드 마이그레이션

AWS EC2와 PureCVisor 간 양방향 VM 이전을 지원한다. `cloud_migration.c` + `aws_client.c` + `disk_converter.c`로 구성.

### 11.1 AWS EC2 Import

#### 표준 Import (6단계)

```
1. AMI → Snapshot → S3 Export (aws ec2 create-store-image-task)
2. S3 다운로드 (멀티파트)
3. qemu-img convert (VMDK/VHD → qcow2/raw)
4. virt-customize (cloud-init 제거, 네트워크 재설정)
5. virDomainDefineXML (VM 정의)
6. virDomainCreate (VM 시작)
```

#### Near-Live Import 2-Phase (v1.0)

다운타임을 최소화하는 2단계 이전:

```
Phase 1: 사전 동기화 (다운타임 0)
  ├── 소스 EC2 실행 중 상태로 디스크 스냅샷 생성
  ├── 스냅샷을 qcow2로 변환 + 전송
  └── PureCVisor에 VM 정의 (시작하지 않음)

Phase 2: 최종 전환 (다운타임 2~5분)
  ├── aws ec2 stop-instances (소스 중지)
  ├── aws ec2 create-snapshot (최종 스냅샷)
  ├── aws ec2 wait snapshot-completed
  ├── pcv_disk_apply_delta (증분 적용)
  ├── virt-customize (최종 커스터마이즈)
  ├── virDomainCreate (VM 시작)
  └── 소스 EC2 종료 확인
```

```bash
# CLI — 표준 Import
pcvctl cloud import --ami ami-0abcdef1234567890 \
  --vm-name web-prod --vcpu 4 --memory 8192

# CLI — Near-Live Import
pcvctl cloud import --ami ami-0abcdef1234567890 \
  --vm-name web-prod --near-live

# 최종 전환
pcvctl cloud finalize --name web-prod

# RPC
echo '{"jsonrpc":"2.0","method":"cloud.import","params":{
  "ami_id":"ami-0abcdef1234567890",
  "vm_name":"web-prod",
  "vcpu":4,
  "memory_mb":8192,
  "near_live": true
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

### 11.2 AWS EC2 Export (5단계)

```
1. VM 중지 + ZFS/qcow2 스냅샷
2. qemu-img convert (qcow2 → VMDK/VHD)
3. S3 업로드 (멀티파트)
4. aws ec2 import-image (AMI 등록)
5. aws ec2 run-instances (EC2 시작)
```

```bash
# CLI
pcvctl cloud export --name web-prod --region ap-northeast-2 \
  --instance-type t3.large --bucket purecvisor-exports
```

### 11.3 Job 관리

#### SQLite Job 영속화 (v1.0)

마이그레이션 작업 상태를 SQLite에 영속 저장하여 데몬 재시작 후에도 복구 가능.

#### AWAITING_CUTOVER 타임아웃 (v1.0)

Near-Live Import의 Phase 1 완료 후 `AWAITING_CUTOVER` 상태에서 2시간 내에 finalize하지 않으면 자동 타임아웃.

```bash
# 작업 목록
pcvctl cloud jobs

# 영속 저장소 조회 (v1.0)
echo '{"jsonrpc":"2.0","method":"cloud.jobs.persist.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock

# 작업 취소
pcvctl cloud cancel --name web-prod

# RPC
echo '{"jsonrpc":"2.0","method":"cloud.jobs.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock

echo '{"jsonrpc":"2.0","method":"cloud.job.cancel","params":{
  "vm_name":"web-prod"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

### 11.4 GCancellable 취소

진행 중인 작업은 `GCancellable`을 통해 안전하게 취소된다. 다운로드/변환 중 즉시 중단하며, 중간 파일을 정리한다.

---

## 12. AI & 자가치유

### 12.1 4-프로바이더 합의

PureCVisor AI Agent는 복수의 AI 프로바이더에 동시 질의하여 합의(consensus) 기반으로 의사결정한다.

| 프로바이더 | 용도 | 설정 |
|-----------|------|------|
| Claude (Anthropic) | 범용 분석 | API Key + 모델 고정 |
| OpenAI | 범용 분석 | API Key + 모델 고정 |
| Gemini (Google) | 범용 분석 | API Key |
| Ollama | 로컬 추론 | 엔드포인트 URL |

#### 가중 쿼럼 (v1.0)

프로바이더별 가중치를 부여하여, 가중 합산이 60% 초과 시 합의 성립.

```bash
# AI Agent 설정
pcvctl agent config

# 프로바이더 설정
pcvctl agent set --provider claude --api-key "sk-ant-..." --weight 0.3
pcvctl agent set --provider openai --api-key "sk-..." --weight 0.3
pcvctl agent set --provider ollama --endpoint "http://localhost:11434" --weight 0.2
pcvctl agent set --provider gemini --api-key "AI..." --weight 0.2

# 합의 이력
pcvctl agent history
```

#### per-policy dry-run (v1.0)

정책별로 dry-run 모드를 설정하여 실제 실행 없이 의사결정 결과만 확인.

### 12.2 Z-Score 이상탐지

`anomaly_detector.c`에서 Z-Score 기반 이상 탐지를 수행한다.

```
Z = (현재값 - 평균) / 표준편차
|Z| > 3.0 이면 이상(anomaly) 판정
```

기준선 리셋 (v1.0): 배포/유지보수 후 기준선을 수동 리셋하여 오탐을 방지.

```bash
# RPC — 기준선 리셋
echo '{"jsonrpc":"2.0","method":"ai.baseline.reset","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

### 12.3 자가치유 5계층 안전 스택

```
Layer 5: 승인 큐 (v1.0) — 위험 액션은 관리자 승인 대기
Layer 4: Rate Limit — 5분 내 최대 3개 자동 액션
Layer 3: 쿨다운 — 동일 대상 재치유 최소 간격
Layer 2: 합의 검증 — 4-프로바이더 가중 쿼럼 60%+
Layer 1: 정책 매칭 — 사전 정의된 치유 정책
```

치유 액션 예시:
- VM CPU 과부하 → vCPU hot-add
- VM 메모리 부족 → memory balloon 조정
- 프로세스 OOM → VM 재시작
- 디스크 부족 → 오래된 스냅샷 정리

### 12.4 승인 큐 (v1.0)

위험도가 높은 자가치유 액션은 자동 실행하지 않고 승인 큐에 등록한다. 1시간 이내에 승인/거부하지 않으면 자동 만료.

```bash
# 승인 대기 목록
echo '{"jsonrpc":"2.0","method":"ai.approval.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock

# 승인
echo '{"jsonrpc":"2.0","method":"ai.approval.approve","params":{
  "action_id":"heal-001"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock

# 거부
echo '{"jsonrpc":"2.0","method":"ai.approval.reject","params":{
  "action_id":"heal-001"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

Web UI에서도 approve/reject 가능 (v1.0).

---

## 13. Web UI

### 13.1 개요

| 항목 | 값 |
|------|-----|
| URL | `http://localhost:80/ui/` |
| 이벤트 센터 | `http://localhost:80/ui#/ops-triage` |
| bootstrap admin | `admin / configured password` |
| 전용 admin | 설치 직후 RBAC `admin` 역할 사용자 추가 권장 |
| 앱 셸 | 운영 콘솔 + 활동 바 + 운영 그룹 사이드바 + 브레드크럼 |
| JS 모듈 | `app.js` + `app.bundle.js` + `modules/*.js` 19개 + `i18n.js` + `sw.js` |
| 프레임워크 | Vanilla JS (단일 번들 + 임베디드 UI) |

### 13.2 페이지 구조

운영 콘솔은 상단 메뉴바와 툴바, 활동 바, 운영 그룹 사이드바, 브레드크럼, 메인 콘텐츠, 상태바로 구성된다.

```
┌──────────────────────────────────────────────────────────────┐
│ 메뉴바 · 툴바 · 세션 상태                                     │
├──────┬───────────────────────────────────────────────────────┤
│활동바│ 운영 그룹 사이드바         │ 메인 콘텐츠 · 브레드크럼       │
│      │ VM / Containers / Infra    │ 대시보드 · 이벤트 센터 · Host │
│      │                            │ Storage · Network · OVN · Help│
├──────┴───────────────────────────────────────────────────────┤
│ 상태바                                                       │
└──────────────────────────────────────────────────────────────┘
```

운영 그룹별 내용:

| 영역 | 패널 |
|------|------|
| VM | 요약, 콘솔, 스냅샷, 성능, 운영 타임라인 |
| Containers | 컨테이너 목록, 생성, 실행 |
| 운영 개요와 모니터링 | 대시보드, 이벤트 센터, 호스트 상태, 알림, 감사/활동 로그 |
| 네트워크와 스토리지 | 네트워크, OVN, 오버레이, 스토리지, 백업, iSCSI |
| 고급 기능과 지원 | 계정/RBAC, API 관리, 설정 관리, 템플릿, 가이드 |

### 13.3 테마 시스템

3-테마 시스템 — 2026-04-11 Supanova Taste Layer 도입 후 Supanova 변형만
남기고, 2026-04-26 접근성 후속으로 고대비 변형을 추가했다. 2026-05-08에는
대시보드 선택지를 단순화하기 위해 emerald와 light 변형을 제거했다.
[ADR-0016](adr/0016-supanova-theme-reduction.md)에 따라 localStorage에 영속 저장하고, 새로고침 없이 즉시 전환한다.

| 테마 id | accent | 설명 |
|---|---|---|
| `supanova` (기본) | Teal-500 `#14b8a6` | 차분한 청록, 운영 콘솔 중성 톤 |
| `supanova-cyan` | Cyan-600 `#0891b2` | 기존 brand cyan 연속감, 채도 경계 |
| `supanova-hicontrast` | Yellow `#facc15` | 고대비 접근성 변형 |

공통 스택: self-hosted Pretendard + local Outfit fallback, Double-Bezel 카드, spring motion
(`cubic-bezier(0.16,1,0.3,1)`), `@supports not (color-mix)` 폴백.
Supanova 금칙(neon glow, clip-path, cyan→magenta 그라디언트) 전량 제거.
레거시/삭제 테마 id(`pure-light`, `midnight-blue`, `supanova-emerald`,
`supanova-light` 등)는 inline head script가
자동으로 `supanova`로 마이그레이션.

### 13.4 DESIGN.md 시각 규격

UI 시각 규격은 상위 제품/운영 가이드인 `docs/GUIDE.md`와 분리해
루트 [`DESIGN.md`](../DESIGN.md)에서 관리한다. `DESIGN.md`는 색상 token,
typography, component state, dashboard density, table/card/button/modal 규칙을
정의하고, [`ui/samples/design-system-preview.html`](../ui/samples/design-system-preview.html)은
같은 `ui/style.css` 위에서 그 규칙을 확인하는 preview HTML이다.

UI visual 변경, `ui/samples/` 변경, `ui/guide.html`/`ui/guide-content.md`의 시각
규격 연결을 바꾸면 다음 검증을 Level 1에 포함한다.

```bash
python3 scripts/check_design_md.py
bash tests/integration/test_design_md_surface.sh
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
node --check ui/app.bundle.js
git diff --check
```

`DESIGN.md`의 Reference Pattern Borrowing 규칙은 외부 제품의 브랜드를 복제하지 않고 운영 콘솔에 필요한 패턴만 차용하는 기준이다. 현재 샘플은 [`ui/samples/design-borrowing-mockup.html`](../ui/samples/design-borrowing-mockup.html)이며, 실제 적용 화면은 `운영 > 이벤트 센터`의 `ops-triage` 라우트다.

### 13.5 i18n 국제화

`i18n.js`에서 ko/en 2개 언어, 280+키를 관리한다.

| 항목 | 값 |
|------|-----|
| 지원 언어 | 한국어 (ko), English (en) |
| 키 수 | 280+ |
| HTML data-i18n | 86개 요소 |
| 전환 함수 | `t(key)` / `applyI18n()` |
| 즉시 전환 | 새로고침 불필요 |

### 13.6 Service Worker 오프라인 캐싱

`sw.js`는 정적 파일을 Network-First 전략으로 갱신하고 실패 시 캐시로 폴백한다. API/WebSocket 요청에는 개입하지 않으며, 네트워크 단절 시에도 마지막으로 캐시된 UI 접근을 지원한다.

### 13.7 WebSocket 실시간 이벤트

- `ws://localhost:80/api/v1/ws/events` — 메트릭 push (10초)
- `ws://localhost:80/api/v1/ws/vnc` — noVNC WebSocket 프록시

유휴 타임아웃: 300초 미활동 시 자동 종료. 최대 동시 연결: 1,000.

### 13.8 커맨드 팔레트

`Ctrl+K`로 커맨드 팔레트를 열어 빠른 네비게이션 및 액션 실행.

기타 키보드 단축키:
- `?` — 키보드 도움말
- `Esc` — 모달 닫기

### 13.9 반응형 디자인

| 브레이크포인트 | 대상 | 적용 |
|--------------|------|------|
| <= 1024px | iPad | 사이드바 축소, 그리드 조정 |
| <= 768px | 모바일 | 햄버거 메뉴, 단일 컬럼 |
| <= 480px | 소형 기기 | 최소 레이아웃, 터치 스와이프 |

### 13.10 접근성

- ARIA 레이블 전체 적용
- 포커스 트랩 (모달 내 Tab 순환)
- 키보드 네비게이션 활성화
- WCAG 2.1 AA 수준 대비율

### 13.11 모듈 구조

```
ui/
├── index.html
├── login.html
├── style.css
├── app.js              # 메인 엔트리포인트
├── app.bundle.js       # modules/*.js 단일 번들
├── sw.js               # Service Worker
├── i18n.js             # 국제화
└── modules/
    ├── endpoints.js    # EP 레지스트리 (하드코딩 금지)
    ├── api.js          # unwrapData/unwrapList, fetch 래퍼
    ├── ui.js           # customConfirm, 토스트, 기본 UI helper
    ├── uxlib.js        # escape, sanitizer, UI 유틸리티
    ├── modal.js        # 모달 helper
    ├── charts.js       # 차트 렌더링
    ├── vm.js           # VM CRUD, 스냅샷, 디스크
    ├── container.js    # 컨테이너 관리
    ├── network.js      # 네트워크 관리
    ├── storage.js      # 스토리지 관리
    ├── monitor.js      # 모니터링 대시보드, 운영 이벤트 센터
    ├── security.js     # 보안 이벤트 UI
    ├── cloud.js        # Cloud Migration
    ├── help.js         # 도움말, Service Guide
    ├── nav.js          # 네비게이션, 라우팅, 이벤트 센터 route
    ├── theme.js        # 테마 관리
    ├── accounts.js     # 계정/RBAC UI
    ├── advanced.js     # 고급 운영 UI
    └── selfhealing.js  # Self-healing UI
```

`customConfirm()`은 확인 메시지를 escape한 뒤 줄바꿈만 안전하게 렌더링한다. 확인창에 강조가 필요해도 호출부에서 `<br>`, `<b>` 같은 HTML 조각을 넘기지 않는다.

### 13.12 보안 헤더와 로컬 정적 자산

운영 Web UI는 강한 CSP를 기본으로 둔다. 원칙은 외부 런타임 호출을 허용하지 않고, 브라우저가 필요한 자산을 같은 origin의 `/ui/` 아래에서 받도록 고정하는 것이다.

| 항목 | 기준 |
|------|------|
| 진입점 | `/ui`와 `/ui/` 모두 자산 기준 경로가 `/ui/`가 되도록 `index.html`에 `<base href="/ui/">` 유지 |
| 아이콘 | 외부 런타임 API를 사용하지 않고 inline SVG symbol 또는 `ui/vendor/coolicons/coolicons.svg` 같은 로컬 파일 사용 |
| Chart.js | `ui/vendor/chart.umd.min.js`로 self-host, `sourceMappingURL` 제거 |
| noVNC | `ui/vendor/novnc/novnc.esm.js`로 self-host, 외부 ESM import 금지 |
| 폰트 | `ui/vendor/pretendard/pretendard.css`와 woff2 파일로 self-host |
| PWA manifest | `icon-192.png`, `icon-512.png`를 반드시 배포 |
| PNG MIME | `Content-Type: image/png` + `X-Content-Type-Options: nosniff` |
| Service Worker | API/WebSocket은 개입하지 않고 `/ui/` 정적 자산만 캐시 |
| WebSocket | URL에 토큰을 넣지 않고 연결 후 첫 메시지로 인증 |
| Metrics | UI fetch는 `Authorization: Bearer ...` 헤더를 붙여 호출 |

권장 CSP/Permissions-Policy 예시는 다음과 같다.

```nginx
add_header Permissions-Policy "accelerometer=(), autoplay=(), camera=(), display-capture=(), encrypted-media=(), fullscreen=(self), geolocation=(), gyroscope=(), microphone=(), midi=(), payment=(), usb=()" always;
add_header Content-Security-Policy "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; font-src 'self'; connect-src 'self' wss://purecvisor.example.com https://purecvisor.example.com; img-src 'self' data: blob:; frame-src 'none'; object-src 'none'; base-uri 'self'; form-action 'self'" always;
```

호환 엔드포인트 `purecvisor-compat.example.com`를 별도 server block으로 운영하면 `connect-src`의 host도 해당 도메인으로 맞춘다. 정적 파일 배포 검증은 두 도메인의 `app.bundle.js`, `guide-content.md`, `sw.js` 해시가 같은지 확인한 뒤 완료 처리한다.

nginx가 앞단 reverse proxy로 동작하고 `nosniff`를 켠 경우, PWA 아이콘은 데몬 프록시보다 nginx 정적 location으로 직접 내려주는 편이 안전하다.

```nginx
location ~ ^/ui/.+\.(png|ico|svg|webp|woff|woff2)$ {
    root /usr/local/share/purecvisor;
    types {
        image/png png;
        image/x-icon ico;
        image/svg+xml svg;
        image/webp webp;
        font/woff woff;
        font/woff2 woff2;
    }
    try_files $uri =404;
}
```

데몬 직접 서빙 경로도 `.png`, `.ico`, `.svg`, `.webp` MIME을 제공해야 한다. 이 경로는 nginx 없는 로컬 설치나 장애 분석 시 동일한 manifest 동작을 보장하기 위한 폴백이다.

noVNC는 반드시 `/ui/vendor/novnc/novnc.esm.js`에서 로드한다. `app.bundle.js` 안에 `https://cdn.jsdelivr.net/npm/@novnc/novnc` 같은 외부 ESM import가 남아 있으면 운영 CSP에서 차단된다. 정적 파일 교체 후에는 `/ui/vendor/novnc/novnc.esm.js`가 `200 application/javascript`로 응답하고, 공개 `app.bundle.js`에서 외부 CDN 문자열이 검색되지 않는지 확인한다.

해시 라우팅은 `#/page`를 표준으로 사용한다. 공개 안내나 외부 링크가 `#page` 형식으로 들어와도 `ui/modules/uxlib.js`의 parser가 같은 page로 정규화해야 한다. 예: `/ui#ops-triage`와 `/ui#/ops-triage`는 모두 `운영 이벤트 센터`를 렌더링해야 한다.

---

## 14. REST API

### 14.1 기본 정보

| 항목 | 값 |
|------|-----|
| Base URL | `http://localhost:80/api/v1` |
| 인증 | `Authorization: Bearer {JWT}` |
| CSRF | 쿠키 세션 미사용. `X-CSRF-Token` 헤더 없음 |
| Content-Type | `application/json` |
| 압축 | gzip (`Accept-Encoding: gzip`) |
| CORS | 화이트리스트 기반 |
| ETag/304 (v1.0) | 조건부 캐싱 (`If-None-Match`) |
| 페이지네이션 (v1.0) | `X-Total-Count` + `Link: <url>; rel="next"` |
| Correlation ID (v1.0) | 요청/응답 UUID (`X-Correlation-Id`) |

### 14.2 per-endpoint Rate Limit (v1.0)

엔드포인트별로 Rate Limit을 차등 적용:

| 엔드포인트 | 제한 |
|-----------|------|
| `POST /auth/token` | 60/분 |
| `GET /metrics` | 3,600/시간 |
| `GET /vms` | 600/분 |
| `POST /vms` | 60/분 |
| 기타 | 600/분 (IP 기본) |

### 14.3 per-method 타임아웃 (v1.0)

| 유형 | 타임아웃 |
|------|---------|
| 읽기 (GET) | 8초 |
| 쓰기 (POST/PUT) | 30초 |
| 장기 작업 (migrate, backup) | 60초 |

### 14.4 주요 엔드포인트

#### 인증

```bash
# 토큰 발급
curl -s -X POST http://localhost:80/api/v1/auth/token \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"<configured-admin-password>"}'
# → {"access_token":"eyJ...", "refresh_token":"eyJ..."}

# 토큰 갱신
curl -s -X POST http://localhost:80/api/v1/auth/refresh \
  -d '{"refresh_token":"eyJ..."}'
```

#### VM 관리

```bash
# VM 목록
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms

# VM 생성
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "name":"web-prod",
    "vcpu":4,
    "memory_mb":8192,
    "disk_size_gb":50,
    "os_variant":"ubuntu24.04",
    "storage_type":"zvol"
  }' http://localhost:80/api/v1/vms

# VM 시작/중지/삭제
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/start

curl -s -X POST ... http://localhost:80/api/v1/vms/web-prod/stop
curl -s -X DELETE ... http://localhost:80/api/v1/vms/web-prod

# VM 삭제 상태 확인 (비동기)
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/vms/web-prod/delete-status
```

#### 컨테이너

```bash
# 목록
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/containers

# 생성
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"nginx-ct","image":"ubuntu:24.04"}' \
  http://localhost:80/api/v1/containers
```

#### 클러스터

Single Edge 공개판은 클러스터 상태, 클러스터 전체 VM, 라이브 마이그레이션 REST 절차를 운영 표면으로 제공하지 않는다. 관련 역사 기록은 `docs/adr/`와 Multi 범위 문서에서만 확인한다.

#### 모니터링

```bash
# 호스트 메트릭
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/host/metrics

# 프로세스 목록
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/processes

# 알림 히스토리
curl -s -H "Authorization: Bearer $TOKEN" \
  http://localhost:80/api/v1/alerts

# Prometheus 메트릭 (인증 불필요)
curl -s http://localhost:80/api/v1/metrics
```

#### Health

```bash
# 심층 프로브 (인증 불필요)
curl -s http://localhost:80/api/v1/health
# → {"status":"ok","subsystems":{
#     "libvirt":"ok","etcd":"ok","disk":"ok",
#     "audit_db":"ok","tls":"ok","capabilities":{...}
#   }}
```

---

## 15. CLI 레퍼런스

### 15.1 개요

| 항목 | 값 |
|------|-----|
| 바이너리 | `bin/pcvctl` |
| 커맨드 수 | 168 |
| 모드 | readline REPL + 배치 |
| 자동완성 | bash/zsh (`make install-completion`) |
| 소켓 | `/var/run/purecvisor/daemon.sock` |

```bash
# REPL 모드
pcvctl
pcvctl> vm list
pcvctl> exit

# 배치 모드
pcvctl vm list
pcvctl vm create web-prod --vcpu 4 --memory_mb 8192 --disk_size_gb 50
```

### 15.2 커맨드 분류

#### VM 관리 (30+)

| 커맨드 | 설명 |
|--------|------|
| `vm list` | VM 목록 |
| `vm create <name> --vcpu N --memory_mb N --disk_size_gb N` | VM 생성 |
| `vm rename <old_name> <new_name>` | 정지된 VM 이름과 표준 디스크/NVRAM 경로 변경 |
| `vm start <name>` | VM 시작 |
| `vm stop <name>` | VM 중지 (graceful) |
| `vm delete <name>` | VM 삭제 |
| `vm delete-status <name>` | 비동기 삭제 상태 |
| `vm clone <source> <clone_name> --mode cow\|full --guest-reset\|--template-prepared` | VM 복제 |
| `vm vnc <name>` | VNC 포트 조회 |
| `snapshot create <name> <snap>` | 스냅샷 생성 |
| `snapshot list <name>` | 스냅샷 목록 |
| `snapshot rollback <name> <snap>` | 스냅샷 롤백 |
| `snapshot delete <name> <snap>` | 스냅샷 삭제 |
| `vm snapshot-delete-all <name>` | 스냅샷 일괄 삭제 |
| `vm disk-resize <name> <target> <new_size_gb>` | 디스크 크기 조정 |
| `vm disk-throttle <name> [--device vda] --read-iops N --write-iops N` | 디스크 IOPS 제한 |
| `vm blkio-set <name> [--read_bps N] [--write_bps N] [--read_iops N] [--write_iops N]` | 블록 I/O 제한 |
| `vm bandwidth <name> --inbound-kbps N --outbound-kbps N` | VM 네트워크 대역폭 제한 |
| `vm pin-vcpu <name> --vcpu N --cpuset 0-3` | vCPU 피닝 |
| `vm guest-agent-status <name>` | Guest Agent channel/package 상태 진단 |
| `vm guest-agent-ensure-channel <name>` | Guest Agent libvirt channel 보정 |
| `vm guest-ping <name>` | Guest Agent 연결 확인 |
| `vm guest-exec <name> <command>` | Guest Agent 명령 실행 |
| `vm guest-shutdown <name>` | Guest Agent 안전 종료 |
| `vm export-ova <name> --output-dir /tmp` | OVA 내보내기 |
| `vm import-ova <ova_path> <name> [--pool pcvpool/vms]` | OVA 가져오기 |
| `vm usb-list <name>` | USB hostdev 목록 |
| `vm usb-attach <name> <vendor_id> <product_id>` | USB hostdev 연결 |
| `vm usb-detach <name> <vendor_id> <product_id>` | USB hostdev 분리 |

#### NIC 관리

| 커맨드 | 설명 |
|--------|------|
| `nic list <name>` | NIC 목록 |
| `nic add <name> <bridge>` | NIC 추가 |
| `nic remove <name> <mac>` | NIC 제거 |

#### ISO 관리

| 커맨드 | 설명 |
|--------|------|
| `iso list` | ISO 이미지 목록 |
| `iso mount <name> <iso_path>` | ISO 마운트 |
| `iso eject <name>` | ISO 추출 |

#### 컨테이너 (10+)

| 커맨드 | 설명 |
|--------|------|
| `container list` | 목록 |
| `container create <name> --image <img>` | 생성 |
| `container start <name>` | 시작 |
| `container stop <name>` | 중지 |
| `container destroy <name>` | 삭제 |
| `container exec <name> -- <cmd>` | 명령 실행 |
| `container metrics <name>` | 메트릭 |
| `container snapshot <name> <snap>` | 스냅샷 |

#### 네트워크 (20+)

| 커맨드 | 설명 |
|--------|------|
| `network list` | 브릿지 목록 |
| `network create <name> --cidr <cidr>` | 브릿지 생성 |
| `network delete <name>` | 브릿지 삭제 |
| `network edit <name> --cidr <cidr>` | 브릿지 수정 |
| `network dhcp <name> --start <ip> --end <ip>` | DHCP 설정 |
| `firewall list` | 방화벽 규칙 |
| `firewall add --vm <name> --port <port> --action accept` | 규칙 추가 |
| `overlay list` | OVS 오버레이 |
| `ovn status` | OVN 상태 |
| `ovn switch list` | 논리 스위치 |
| `ovn router list` | 논리 라우터 |

#### 스토리지 (10+)

| 커맨드 | 설명 |
|--------|------|
| `storage pool list` | ZFS 풀 목록 |
| `storage zvol list` | zvol 목록 |
| `iscsi target list` | iSCSI 타겟 |
| `iscsi target create --pool <p> --name <n> --size <s>` | iSCSI 생성 |

#### 클러스터 (범위 밖 참고)

다음 항목은 역사 기록 또는 Multi 제어면 참고용이다. Single Edge 공개판의 운영 명령으로 안내하지 않는다.

| 커맨드 | 설명 |
|--------|------|
| `cluster status` | 클러스터 상태 |
| `cluster vms` | 전체 노드 VM |
| `cluster migrate <name> <dest>` | 라이브 마이그레이션 |
| `cluster replicate <name>` | ZFS 복제 |
| `cluster replicate-status` | 복제 상태 |
| `cluster failover-test` | 페일오버 테스트 |
| `node drain` | 노드 드레인 |
| `node resume` | 노드 복귀 |
| `node version` | 노드 버전 |

#### 인증 & 보안 (10+)

| 커맨드 | 설명 |
|--------|------|
| `auth list` | 사용자 목록 |
| `auth create <user> <pass> <role>` | 사용자 생성 |
| `auth role <user> <role>` | 역할 변경 |
| `auth delete <user>` | 사용자 삭제 |
| `auth apikey create --name <n>` | API Key 생성 |
| `security-group create <name>` | 보안 그룹 |
| `security-group add-rule <sg> ...` | 규칙 추가 |

#### 모니터링 & 알림 (10+)

| 커맨드 | 설명 |
|--------|------|
| `monitor processes --top N` | 프로세스 Top N |
| `alert list` | 알림 히스토리 |
| `alert config` | 알림 설정 조회 |
| `alert set --cpu-warn 80 --cpu-crit 95` | 알림 임계값 |

#### 백업 (5+)

| 커맨드 | 설명 |
|--------|------|
| `backup list` | 정책 목록 |
| `backup set <name> --interval N --retention N` | 정책 설정 |
| `backup history <name>` | 히스토리 |

#### 클라우드 (5+)

| 커맨드 | 설명 |
|--------|------|
| `cloud import --ami <id> --vm-name <n>` | EC2 Import |
| `cloud export --name <n> --region <r>` | EC2 Export |
| `cloud jobs` | 작업 목록 |
| `cloud cancel --name <n>` | 작업 취소 |
| `cloud finalize --name <n>` | Near-Live 최종 전환 |

#### AI Agent (5+)

| 커맨드 | 설명 |
|--------|------|
| `agent config` | Agent 설정 |
| `agent set --provider <p> --api-key <k>` | 프로바이더 설정 |
| `agent history` | 합의 이력 |

#### 템플릿 (5+)

| 커맨드 | 설명 |
|--------|------|
| `template list` | 프리셋 목록 |
| `template create <name> --vcpu N --memory_mb N --disk_gb N` | 생성 |
| `template delete <name>` | 삭제 |

#### 기타

| 커맨드 | 설명 |
|--------|------|
| `dpdk bind <pci>` | DPDK 바인드 |
| `sriov enable <pci> --numvfs N` | SR-IOV 활성화 |
| `gpu list` | GPU 목록 |
| `audit list --limit N` | 감사 로그 |
| `federation site join <url>` | 페더레이션 가입 |

### 15.3 자동완성 설치

```bash
# 시스템 설치 (root)
sudo make install-completion

# 사용자 로컬 설치
make install-completion-user

# 수동 bash
source completion/pcvctl-completion.bash

# 수동 zsh
source completion/_pcvctl
```

---

## 16. 설정 레퍼런스

### 16.1 daemon.conf 위치

```
/etc/purecvisor/daemon.conf
```

설정 우선순위: **환경변수** > **daemon.conf** > **컴파일 기본값**

SIGHUP 시 비파괴적 재로드: `[alert]` 임계값, `rate_limit`, `etcd_timeout`, `log_level`, `pool_max_conn`, `drain_timeout`.

### 16.2 전체 설정 키

#### [daemon]

| 키 | 기본값 | 범위 | 설명 |
|----|--------|------|------|
| `socket_path` | `/var/run/purecvisor/daemon.sock` | 경로 | UDS 소켓 경로 |
| `libvirt_uri` | `qemu:///system` | URI | libvirt 접속 URI |
| `db_path` | `/var/lib/purecvisor/vm_state.db` | 경로 | SQLite 상태 DB |
| `log_level` | `info` | debug/info/warn/error | 로그 레벨 |
| `pool_max_conn` | 8 | 1-64 | libvirt 커넥션 풀 |
| `drain_timeout` | 30 | >= 5 | 드레인 타임아웃 (초) |

#### [rest]

| 키 | 기본값 | 범위 | 설명 |
|----|--------|------|------|
| `port` | 80 | 1-65535 | REST API 포트 |

#### [auth]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `admin_user` | `admin` | bootstrap 관리자 사용자명 (초기/비상 진입용) |
| `admin_password` | (미설정) | bootstrap 관리자 비밀번호. 첫 로그인 전 명시 설정 필요 |
| `jwt_secret` | (빈 문자열 = 랜덤) | JWT 서명 비밀 키 |
| `allow_self_register` | `false` | 로그인 랜딩의 셀프 회원가입 허용 여부. 생성 계정의 기본 역할은 `VIEWER` |

운영용 관리자 계정은 `pcvctl auth create <name> <password> admin` 또는 `POST /api/v1/auth/users`로 별도 생성해 사용합니다.

#### [tls]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `enabled` | `false` | HTTPS 활성화 |
| `cert_file` | - | TLS 인증서 경로 |
| `key_file` | - | TLS 개인 키 경로 |
| `ca_file` | - | CA 인증서 (mTLS) |

#### [storage]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `zvol_pool` | `pcvpool/vms` | ZFS zvol 풀 경로 |
| `container_pool` | `pcvpool/containers` | 컨테이너 ZFS 데이터셋 |
| `image_dir` | `/var/lib/libvirt/images` | qcow2 저장 경로 |
| `iso_dirs` | `/pcvpool/iso,/var/lib/libvirt/images,/iso` | ISO 검색 경로 (CSV) |

#### [container]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `lxc_path` | `/var/lib/purecvisor/lxc` | LXC 루트 경로 |

#### [cluster]

| 키 | 기본값 | 범위 | 설명 |
|----|--------|------|------|
| `etcd_endpoints` | (빈 문자열) | URL CSV | etcd 엔드포인트 |
| `etcd_timeout` | 15 | 2-120 | etcd 타임아웃 (초) |
| `peer_ssh_ip` | (빈 문자열) | IP CSV | 피어 SSH 주소 |
| `ssh_user` | `pcvdev` | 문자열 | SSH 사용자명 |
| `auto_rebalance` | 0 | 0/1 | 자동 리밸런싱 |
| `rebalance_threshold` | 3 | >= 1 | 리밸런싱 VM 수 차이 |
| `replication_interval` | 300 | >= 60 | 복제 주기 (초) |
| `repl_bw_limit` | 0 | >= 0 | 복제 대역폭 (MB/s, 0=무제한) |

#### [migration]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `timeout_sec` | 600 | 마이그레이션 타임아웃 (초) |
| `bandwidth_mbps` | 0 | 대역폭 제한 (0=무제한) |

#### [alert]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `enabled` | `true` | 알림 활성화 |
| `cpu_warn` | 80 | CPU 경고 (%) |
| `cpu_crit` | 95 | CPU 위험 (%) |
| `mem_warn` | 85 | Memory 경고 (%) |
| `mem_crit` | 95 | Memory 위험 (%) |
| `disk_warn` | 80 | Disk 경고 (%) |
| `disk_crit` | 90 | Disk 위험 (%) |
| `eval_period` | 30 | 지속 조건 (초) |
| `dedup_window` | 300 | 중복 제거 윈도우 (초) |
| `webhook_url` | (빈 문자열) | 웹훅 URL |
| `webhook_format` | `generic` | slack/telegram/generic |

---

## 17. 트러블슈팅

### 17.1 자주 발생하는 문제

| 증상 | 원인 | 해결 |
|------|------|------|
| 데몬 시작 실패 | 소켓 경로 디렉터리 없음 | `sudo mkdir -p /var/run/purecvisor` |
| REST API 503 | 서킷 브레이커 OPEN | `journalctl -u purecvisorsd -f`로 libvirt 상태 확인 |
| VM 생성 실패 | 스토리지 풀 미존재 | `zfs list` 확인, `daemon.conf [storage]` 검증 |
| 인증 토큰 만료 | 15분 초과 | refresh token으로 갱신 |
| Rate Limit 429 | 요청 빈도 초과 | `Retry-After` 헤더 참조 |
| WebSocket 끊김 | 300초 유휴 | 클라이언트 ping 구현 |
| VM 삭제 지연 | ZFS 스냅샷 의존성 | `vm delete-status` 확인 후 스냅샷 정리 |
| VM 삭제 후 ISO가 사라짐 | `vm.delete`가 CD-ROM ISO `<source file>`을 파일 디스크로 오인한 회귀 | [ADR-0017](adr/0017-vm-delete-atomicity-rollback.md) 기준으로 `device='disk'` source만 삭제 대상인지 확인하고 최신 데몬 배포 |
| 메모리 누수 의심 | 알 수 없음 | `make memcheck-daemon` (Valgrind 5초) |
| 빌드 경고 | 헤더 누락/타입 불일치 | `make clean && make single` 후 경고 확인 |
| /health 느림 | libvirt 장애 (degraded) | `virsh list` 확인, libvirt 재시작 |
| 컨테이너 IP 없음 | lxc-info 지연 | 시작 후 10초 대기 (IP 할당 폴백) |
| `Permissions-Policy`의 `ambient-light-sensor` 경고 | 브라우저가 더 이상 인식하지 않는 feature 토큰 | nginx/데몬 보안 헤더에서 해당 토큰 제거 |
| CSP가 Iconify API 접속을 차단 | 외부 아이콘 런타임이 JSON API를 fetch | Iconify 런타임 제거, inline SVG 또는 로컬 아이콘 사용 |
| CSP가 Chart.js sourcemap 접속을 차단 | CDN JS 끝의 `sourceMappingURL`을 devtools가 요청 | Chart.js를 `ui/vendor/`에 self-host하고 sourcemap 주석 제거 |
| VNC 화면 진입 시 CSP가 noVNC import를 차단 | noVNC ESM을 jsDelivr에서 동적 import | noVNC를 `ui/vendor/novnc/`에 self-host하고 로컬 import 사용 |
| `https://도메인/ui` 접근 시 CSS/JS가 `401 application/json` MIME 오류로 차단됨 | trailing slash 없는 `/ui`에서 상대 자산이 `/style.css`, `/app.bundle.js`처럼 루트 경로로 해석됨 | `index.html`의 `<base href="/ui/">`가 배포됐는지 확인하고 `/ui/style.css`, `/ui/app.bundle.js`, `/ui/manifest.json` MIME을 확인 |
| `#ops-triage` 직접 링크가 빈 화면을 보임 | 오래된 라우터가 `#/ops-triage`만 처리함 | 최신 `ui/modules/uxlib.js`, `app.bundle.js`, `sw.js`를 배포하고 `/ui#ops-triage`와 `/ui#/ops-triage`를 모두 확인 |
| 로컬 UI 검사는 통과했지만 공개 사이트 번들에 CDN import가 남음 | 개발 호스트와 공개 운영 서버가 다르거나 공개 서버의 정적 파일이 stale 상태 | 공개 URL의 `app.bundle.js` 해시를 로컬 산출물과 비교하고 운영 서버 `/usr/local/share/purecvisor/ui/app.bundle.js`, `sw.js`를 같은 릴리스로 갱신 |
| Pretendard CSS sourcemap JSON이 표시됨 | jsDelivr가 생성한 외부 CSS sourcemap을 devtools가 요청 | Pretendard CSS/woff2를 `ui/vendor/pretendard/`로 self-host |
| manifest 아이콘 404 또는 invalid image | `icon-192.png` 미배포 또는 `text/html` MIME 응답 | 아이콘 파일 배포, nginx/static MIME을 `image/png`로 고정 |
| WebSocket `send` on `CONNECTING` | 전역 소켓 교체 중 이전 `onopen`이 새 소켓에 send | local WebSocket 인스턴스에만 인증 메시지 전송 |
| `/api/v1/metrics` 401 반복 | UI의 raw metrics fetch에 JWT 누락 | `Authorization: Bearer` 헤더를 붙이고 non-OK 응답은 조용히 무시 |
| noVNC 새 창의 Reconnect 버튼 무응답 | `about:blank` 팝업에 `document.write()`로 만든 문서를 `location.reload()`로 재연결하려는 회귀 | 팝업 내부 `RFB` 객체를 `disconnect()` 후 새 `RFB(container, wsUrl)`로 재생성 |
| CD/DVD ISO PATH가 비어 보이거나 빈 Mount가 성공 표시됨 | 설정 UI가 `cdrom_path`를 input에 반영하지 않고 `vm.mount_iso`가 빈 경로를 거부하지 않음 | `cdrom_path`를 현재 ISO로 표시하고 빈/상대/없는 ISO path는 RPC에서 거부 |
| VM `Export OVA` 확인창에 `<br>` 또는 `<b>` 태그가 그대로 보임 | 오래된 UI 번들 또는 `customConfirm()` 호출부가 HTML 조각을 메시지로 전달한 회귀 | 최신 UI 번들을 배포하고 공개 URL의 `app.bundle.js` 해시를 확인한다. 호출부는 plain text와 `\n`만 넘긴다 |
| `대시보드 > 요약 > VM`에서 `디스크 사용량` 버튼이 보이지 않음 | 공개 도메인이 오래된 UI 번들을 서빙하거나 표준/호환 도메인이 서로 다른 서버를 바라봄 | `purecvisor.example.com`와 `purecvisor-compat.example.com`의 `app.bundle.js`, `sw.js` 해시를 로컬 산출물과 비교하고 두 운영 서버를 같은 릴리스로 재배포한다 |
| `vm.clone` accepted 후 clone VM이 생성되지 않음 | 오래된 배포본이 실제 libvirt disk source 대신 `daemon.conf [storage].zvol_pool` 기본값으로 target zvol을 추정 | 최신 데몬으로 재배포하고 accepted 응답의 `source_disk`/`target_disk`가 실제 XML disk source와 일치하는지 확인 |
| `vm.clone` 후 worker CPU가 높게 유지됨 | 오래된 MAC 치환 루프가 새로 생성한 `<mac address='...'>`를 다시 매칭해 무한 반복 | one-pass `g_regex_replace_eval()` 기반 수정본을 배포하고 orphan zvol/snapshot을 정리한 뒤 재시도 |
| 일반 VM `vm.clone`이 libguestfs 도구 없음으로 거부됨 | host에 `libguestfs-tools`가 설치되지 않아 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish`를 실행할 수 없음 | `libguestfs-tools` 설치 후 재시도하거나, 운영자가 이미 정리한 템플릿이면 `template_prepared=true`로 호출 |
| `vm.clone`이 source power on으로 거부됨 | 실행 중인 VM의 디스크와 guest identity를 복제하면 일관성을 보장할 수 없음 | source VM을 종료한 뒤 재시도. qcow2/raw는 `mode=full` 사용 |

### 17.2 디버그 명령어

```bash
# 데몬 상태
sudo systemctl status purecvisorsd

# 실시간 로그
journalctl -u purecvisorsd -f

# 디버그 로그 레벨
# daemon.conf에서 log_level=debug 후 SIGHUP
sudo kill -HUP $(pidof purecvisorsd)

# libvirt 로그
journalctl -u libvirtd -n 50

# 수동 RPC 테스트
echo '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# Health 프로브
curl -s http://localhost:80/api/v1/health | python3 -m json.tool

# 브릿지/nftables
ip link show type bridge && brctl show
sudo nft list table inet purecvisor

# OVS 상태
sudo ovs-vsctl show

# Valgrind 메모리 검사
make memcheck-daemon

# 정적 분석
make cppcheck
```

### 17.3 로그 위치

| 로그 | 경로 |
|------|------|
| 데몬 로그 | `journalctl -u purecvisorsd` |
| 감사 로그 | `/var/log/purecvisor/audit.log` |
| 감사 DB | `/var/lib/purecvisor/pcv_audit.db` |
| logrotate | `/etc/logrotate.d/purecvisor` (일일 회전, 30일 보존) |

---

## 18. 부록

### 18.1 용어집

| 용어 | 설명 |
|------|------|
| CAS | Compare-and-Swap. etcd의 원자적 키 갱신 메커니즘 |
| CB | Circuit Breaker. 장애 전파 차단 패턴 (CLOSED/OPEN/HALF_OPEN) |
| CSI | Container Storage Interface |
| DPDK | Data Plane Development Kit. 커널 바이패스 패킷 처리 |
| etcd | 분산 키-값 저장소. 클러스터 메타데이터 관리 |
| fire-and-forget | 응답 즉시 반환 후 비동기 실행하는 패턴 |
| GMainLoop | GLib 이벤트 루프. PureCVisor의 핵심 실행 모델 |
| GTask | GLib 비동기 태스크. 스레드 풀에서 실행 |
| io_uring | Linux 비동기 I/O 인터페이스 |
| JSON-RPC | JSON Remote Procedure Call. UDS 통신 프로토콜 |
| keepalived | VRRP 기반 가상 IP 관리 데몬 |
| Lease TTL | etcd 키의 생존 시간 (Time-To-Live) |
| mTLS | Mutual TLS. 양방향 인증서 인증 |
| NFV | Network Functions Virtualization |
| NUMA | Non-Uniform Memory Access. CPU-메모리 토폴로지 |
| OVN | Open Virtual Network. SDN 제어 평면 |
| OVS | Open vSwitch. 가상 스위치 |
| PBKDF2 | Password-Based Key Derivation Function 2 |
| RBAC | Role-Based Access Control |
| RPO | Recovery Point Objective. 데이터 손실 허용 시간 |
| SR-IOV | Single Root I/O Virtualization. 하드웨어 NIC 가상화 |
| STONITH | Shoot The Other Node In The Head. 펜싱 메커니즘 |
| UDS | Unix Domain Socket |
| VIP | Virtual IP. keepalived VRRP로 관리 |
| VRRP | Virtual Router Redundancy Protocol |
| WAL | Write-Ahead Logging. SQLite 동시성 모드 |
| VXLAN | Virtual Extensible LAN. 오버레이 터널 프로토콜 |
| zvol | ZFS Volume. 블록 디바이스 형태의 ZFS 데이터셋 |

### 18.2 에러 코드

#### GError 도메인: PCV_VM_ERROR

| 코드 | 상수 | 설명 |
|------|------|------|
| 1 | `PCV_VM_ERR_NOT_FOUND` | 지정한 VM이 존재하지 않음 |
| 2 | `PCV_VM_ERR_ALREADY_EXISTS` | 동일 이름의 VM이 이미 존재 |
| 3 | `PCV_VM_ERR_BUSY` | VM이 다른 작업 중 (lock 점유) |
| 4 | `PCV_VM_ERR_INVALID_STATE` | 현재 상태에서 해당 작업 불가 |
| 5 | `PCV_VM_ERR_LIBVIRT_FAILED` | libvirt API 호출 실패 |
| 6 | `PCV_VM_ERR_XML_FAILED` | VM XML 구성/파싱 실패 |
| 7 | `PCV_VM_ERR_INTERNAL` | 내부 오류 |

#### GError 도메인: PCV_LXC_ERROR

| 코드 | 상수 | 설명 |
|------|------|------|
| 1 | `PCV_LXC_ERR_NOT_FOUND` | 컨테이너 미존재 |
| 2 | `PCV_LXC_ERR_ALREADY_EXISTS` | 동일 이름 컨테이너 존재 |
| 3 | `PCV_LXC_ERR_NOT_RUNNING` | 실행 중 상태 필요 |
| 4 | `PCV_LXC_ERR_CMD_FAILED` | 외부 명령 실행 실패 |
| 5 | `PCV_LXC_ERR_CONFIG_FAILED` | liblxc config 적용 실패 |
| 6 | `PCV_LXC_ERR_INTERNAL` | 내부 오류 |

#### GError 도메인: PCV_VALIDATE_ERROR

| 코드 | 상수 | 설명 |
|------|------|------|
| 1 | `PCV_VALIDATE_ERR_NAME` | VM/컨테이너 이름 형식 오류 |
| 2 | `PCV_VALIDATE_ERR_SNAP_NAME` | 스냅샷 이름 형식 오류 |
| 3 | `PCV_VALIDATE_ERR_PATH` | 경로 형식/순회 오류 |
| 4 | `PCV_VALIDATE_ERR_BRIDGE` | 브릿지 이름 형식 오류 |
| 5 | `PCV_VALIDATE_ERR_RANGE` | 수치 범위 초과 |
| 6 | `PCV_VALIDATE_ERR_IMAGE` | 컨테이너 이미지 형식 오류 |
| 7 | `PCV_VALIDATE_ERR_CMD` | exec 명령어 형식 오류 |

#### JSON-RPC 에러 코드

| 코드 | 설명 |
|------|------|
| -32700 | Parse error (JSON 파싱 실패) |
| -32600 | Invalid Request (필수 필드 누락) |
| -32601 | Method not found (미등록 RPC) |
| -32602 | Invalid params (파라미터 검증 실패) |
| -32000 | Server error (내부 오류) |
| -32001 | Not implemented (placeholder) |

### 18.3 외부 의존성

#### 빌드 시 의존성

| 패키지 | 용도 |
|--------|------|
| gcc (14.2+) | C23 컴파일러 |
| make | 빌드 시스템 |
| libglib2.0-dev | GLib/GIO/GObject |
| libsoup-3.0-dev | HTTP 서버 (REST API) |
| libjson-glib-dev | JSON 파싱 |
| libvirt-dev | VM 관리 |
| libvirt-glib-1.0-dev | libvirt GObject 바인딩 |
| liblxc-dev | LXC 컨테이너 |
| libsqlite3-dev | SQLite (상태 DB, 감사 로그) |
| libncursesw5-dev | TUI (ncurses) |
| libssl-dev | OpenSSL crypto/JWT 서명 |
| libcap-dev | Linux capabilities |
| libseccomp-dev | Seccomp 시스템 콜 필터 |
| libreadline-dev | CLI REPL |
| liburing-dev | io_uring fast path |
| protobuf-c-compiler / libprotobuf-c-dev | protobuf-c 코드 생성과 링크 |

#### 런타임 의존성

| 패키지 | 용도 |
|--------|------|
| libvirt-daemon-system | libvirtd |
| qemu-kvm | KVM 하이퍼바이저 |
| libguestfs-tools | 일반 VM 복제 Guest reset 도구 묶음 |
| zfsutils-linux | ZFS 파일시스템 |
| lxc / lxc-utils | LXC 컨테이너 런타임 |
| openvswitch-switch | OVS 가상 스위치 |
| tgt | iSCSI 타겟 |
| open-iscsi | iSCSI 이니시에이터 |
| etcd | 분산 키-값 저장소 |
| keepalived | VRRP VIP 관리 |
| dnsmasq | DHCP/DNS |
| nftables | 방화벽 |

### 18.4 포트 목록

| 포트 | 프로토콜 | 용도 |
|------|---------|------|
| 80 | TCP/HTTP | REST API + Web UI |
| 443 | TCP/HTTPS | TLS REST API + Web UI |
| 2379 | TCP | etcd 클라이언트 |
| 2380 | TCP | etcd 피어 |
| 4789 | UDP | VXLAN 오버레이 (VNI=100) |
| 9090 | TCP | Prometheus (외부 컨테이너) |
| 3000 | TCP | Grafana (외부 컨테이너) |

### 18.5 파일 경로

| 경로 | 용도 |
|------|------|
| `/etc/purecvisor/daemon.conf` | 설정 파일 |
| `/etc/purecvisor/plugins.d/*.so` | 플러그인 디렉터리 |
| `/var/run/purecvisor/daemon.sock` | UDS 소켓 |
| `/var/run/purecvisor/dnsmasq-<br>.meta` | 브릿지 메타데이터 |
| `/var/lib/purecvisor/vm_state.db` | VM 상태 DB (SQLite WAL) |
| `/var/lib/purecvisor/pcv_audit.db` | 감사 로그 DB |
| `/var/lib/purecvisor/lxc/` | LXC 컨테이너 루트 |
| `/var/lib/libvirt/images/` | qcow2 디스크 이미지 |
| `/var/log/purecvisor/audit.log` | 감사 텍스트 로그 |
| `/etc/logrotate.d/purecvisor` | logrotate 설정 |

### 18.6 프로젝트 통계

다음 수치는 2026-04-29 로컬 작업트리 기준 스냅샷이다. 고정 계약이 아니라 현행 확인용이며, 릴리스 판단은 `git`, `Makefile`, 정적 게이트 출력이 우선한다.

| 항목 | 2026-05-08 기준 |
|------|-----------------|
| C 표준 | `-std=gnu23` |
| 에디션 | Single Edge 공개 범위 |
| RPC 등록/정책 | `make check-rbac` 기준 RPC 238건, 정책 매핑 157건 |
| C/H 소스 | C 파일 104개, 헤더 100개, C/H 약 107,387 LOC |
| Web UI | 모듈 19개, core UI 파일 41개, vendor 포함 UI 파일 55개 |
| 문서 | `docs/` 파일 60개, Markdown 약 15,551 LOC |
| 테스트 | C 테스트 파일 50개, `make test` 기준 실행 514건, `tests/integration` 파일 64개 |
| 커밋 | `git rev-list --count HEAD` 기준 608개 |
| 배포 구성 | Single Edge 단일 노드 검증 환경 |
| 주요 정적 게이트 | `make check-rbac`, `scripts/check_audit_placement.py`, UI CSP/vendor 자산 검사 |

---

## 19. 개발자 & 엔지니어 가이드

이 장은 PureCVisor Single Edge를 **수정, 검증, 배포, 장애 분석**하는 사람을 위한 작업 기준이다. 앞 장들이 기능 사용법을 설명한다면, 이 장은 변경을 안전하게 넣고 운영 상태를 해석하는 순서를 정의한다.

### 19.1 독자와 목표

| 독자 | 이 장에서 얻어야 하는 것 |
|------|--------------------------|
| 신규 개발자 | 저장소 경계, 핵심 모듈, 먼저 읽을 문서, 최소 빌드/테스트 루틴 |
| 백엔드 엔지니어 | RPC 추가/변경, RBAC, fire-and-forget, audit, libvirt/ZFS/LXC 연동 규칙 |
| 프론트엔드 엔지니어 | Vanilla JS 모듈 구조, `PCV.*` 네임스페이스, `EP` 엔드포인트 레지스트리, sanitizer 규칙 |
| 운영/SRE | systemd, 로그, 헬스 체크, libvirt/ZFS/OVS 상태 확인, 장애 증거 수집 |
| 릴리스 담당 | Single Edge 공개 범위, 검증 명령, 문서/배포 산출물 동기화 |

### 19.2 빠른 독해 경로

| 상황 | 먼저 볼 장 | 다음 확인 |
|------|------------|-----------|
| 처음 빌드한다 | 2장 설치 및 환경 구성 | 21장 아키텍처 리팩토링, 22장 품질 게이트 |
| VM 기능을 바꾼다 | 3장 VM 관리 | [ADR-0022](adr/0022-vm-create-storage-location-contract.md), [ADR-0023](adr/0023-vm-clone-beta-safety-guard.md), `tests/test_vm_clone_plan.c` |
| REST/API를 바꾼다 | 14장 REST API | `src/api/rest_server.c`, `src/api/dispatcher.c`, `scripts/verify_api_consistency.sh` |
| 권한을 바꾼다 | 10장 보안 | `make check-rbac`, `docs/adr/0019-rbac-uds-bypass-policy.md` |
| Web UI를 바꾼다 | 13장 Web UI | `ui/modules/endpoints.js`, `scripts/bundle-ui.sh`, `node --check ui/app.bundle.js`, 공개 URL route smoke |
| 배포 전 검증한다 | 22장 품질 게이트 | `make single`, `make test`, `make check-rbac`, `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh` |

### 19.3 개발 불변 규칙

- 현재 저장소는 Linux/KVM 기반 **Single Edge** 범위만 다룬다.
- 설계 결정은 `docs/ADR_INDEX.md`와 `docs/adr/`를 우선한다. 기존 구현을 바꾸면 관련 ADR 적용 상태를 먼저 확인한다.
- 장시간 RPC는 먼저 `accepted` 응답을 반환하고 `GTask` worker에서 실행한다. 닫힌 소켓으로 후속 응답을 보내는 패턴은 금지한다.
- fire-and-forget RPC는 dispatcher 자동 audit에 의존하지 않는다. worker callback에서 실제 성공/실패 기준으로 `pcv_audit_log()`와 WebSocket job completion을 남긴다.
- destructive RPC는 가능하면 멱등성을 유지한다. 실패 경로는 생성된 dataset/file/domain을 best-effort로 정리해야 한다.
- `system()`과 `popen()`은 사용하지 않는다. 외부 명령은 `pcv_spawn_sync()` 또는 `pcv_spawn_pipe_sync()`에 argv 배열로 넘긴다.
- VM/템플릿 이름은 핸들러 진입점에서 검증 함수를 거친다.
- UI 모듈은 `PCV.*` 네임스페이스와 `ui/modules/endpoints.js`의 `EP` 레지스트리를 사용한다. `innerHTML` 대입은 sanitizer 또는 escape helper를 거친다.

### 19.4 변경 작업 표준 흐름

1. 관련 문서와 ADR을 먼저 읽는다.
2. 변경 대상 모듈의 기존 호출 흐름을 `rg`로 추적한다.
3. API/RPC/권한/비동기 여부를 먼저 결정한다.
4. 구현은 기존 모듈 경계 안에서 최소 범위로 넣는다.
5. 실패 경로와 audit/job completion을 성공 경로와 같은 수준으로 구현한다.
6. UI가 필요하면 `EP`에 엔드포인트를 등록하고 `unwrapData()` / `unwrapList()` 패턴으로 응답을 처리한다.
7. 영향 범위에 맞는 테스트와 정적 게이트를 실행한다.
8. `docs/GUIDE.md`, `ui/guide-content.md`, ADR, 검증 정책 중 바뀐 계약을 반영한다.

### 19.5 변경 유형별 최소 검증

| 변경 유형 | 최소 검증 |
|-----------|-----------|
| C 코어/dispatcher | `make single`, `make test`, `make check-rbac` |
| fire-and-forget RPC | `scripts/check_audit_placement.py`, 관련 worker 성공/실패 audit 확인 |
| VM clone | `./test_runner -p /vm_clone_plan`, `scripts/check_vm_clone_cleanup.py`, [ADR-0023](adr/0023-vm-clone-beta-safety-guard.md) 실환경 기준 확인 |
| Web UI | `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`, `node --check ui/app.bundle.js`, 공개 URL 해시와 `/ui#ops-triage` 확인 |
| REST surface | `scripts/verify_api_consistency.sh`, 인증/권한/에러 응답 확인 |
| ZFS inflight/metric | ZFS inflight 정적 검사와 Web UI 모니터링 노출 검사 |
| 문서만 변경 | `git diff --check`, 공개 가이드 배포 시 `/ui/guide-content.md` 해시 확인 |

### 19.6 운영 디버깅 플레이북

| 증상 | 확인 순서 |
|------|-----------|
| API가 실패한다 | `/api/v1/health`, journal, REST status/error body, dispatcher method 등록 |
| VM action이 거부된다 | JWT subject, RBAC role, VM owner metadata, `make check-rbac` 계약 |
| VM clone이 실패한다 | source VM `shut off`, disk 개수/type, `libguestfs-tools`, accepted 응답의 `source_disk`/`target_disk`, audit result |
| UI만 최신이 아니다 | 로컬 번들 해시, `/usr/local/share/purecvisor/ui`, 공개 URL 해시, Service Worker cache name, `/ui` base href, `#/page` 해시 라우팅 |
| 네트워크가 이상하다 | `ip -brief addr`, `ovs-vsctl show`, bridge metadata, nftables, dnsmasq 상태 |
| 스토리지 오류가 난다 | `zpool status`, `zfs list -o name,origin`, dataset lock, target cleanup 여부 |

### 19.7 PR/배포 증거 기준

변경 완료 보고에는 다음을 남긴다.

- 무엇을 바꿨는지: 사용자 관점의 동작 변화
- 어디를 바꿨는지: 주요 파일과 모듈
- 어떻게 검증했는지: 명령, 결과, 실환경 여부
- 남은 리스크: 실행하지 못한 테스트, 운영 권한/환경 제약
- 배포 여부: 로컬만 변경인지, 운영 서버와 공개 URL까지 반영했는지

### 19.8 엔지니어 핸드오프 템플릿

```text
변경 요약:
- ...

영향 범위:
- Backend:
- Frontend:
- Docs:
- Ops:

검증:
- ...

운영 반영:
- ...

남은 리스크/후속:
- ...
```

---

## 20. 영업 & 마케팅 가이드

이 장은 PureCVisor Single Edge를 외부에 설명하거나 데모하는 사람이 **무엇을 약속할 수 있고, 무엇을 약속하면 안 되는지**를 정리한다. 메시지는 제품 사실과 공개 범위에 맞춰야 한다.

### 20.1 포지셔닝

PureCVisor Single Edge는 단일 Linux 서버에서 KVM VM, LXC 컨테이너, ZFS 스토리지, OVS/OVN 네트워크, REST API, Web UI를 한 콘솔로 운영하기 위한 **self-hosted Single Edge 가상화 운영면**이다.

짧은 설명:

```text
PureCVisor Single Edge는 단일 서버 운영에 필요한 VM, 컨테이너, ZFS, 네트워크, 백업, REST API를 한 화면과 한 데몬으로 묶은 KVM 기반 운영 콘솔입니다.
```

### 20.2 핵심 메시지

| 메시지 | 설명 |
|--------|------|
| 단일 노드 집중 | 클러스터 복잡도 없이 한 서버의 VM/컨테이너/스토리지를 운영한다. |
| Linux/KVM 기반 | libvirt, KVM, LXC, ZFS, OVS처럼 검증된 Linux 구성요소 위에 얹는다. |
| Web UI + REST API | 브라우저 운영과 자동화를 같은 상태 모델로 제공한다. |
| 운영 안전장치 | RBAC, owner-scope, audit, fire-and-forget job 결과, 실패 cleanup을 제품 계약으로 둔다. |
| 스토리지 중심 운영 | ZFS zvol, snapshot, CoW/full clone, backup/restore 흐름을 운영 작업으로 노출한다. |

### 20.3 적합 고객

| 적합한 경우 | 이유 |
|-------------|------|
| 소규모 온프레미스 서버 운영 | 단일 서버에서 VM, 컨테이너, 스토리지를 한 번에 관리해야 한다. |
| 엣지/지점 서버 | 중앙 클러스터보다 현장 독립 운영과 빠른 복구가 중요하다. |
| 개발/검증 랩 | VM clone, snapshot, REST API로 반복 환경을 빠르게 만든다. |
| 내부 플랫폼 팀 | Web UI와 API를 함께 제공해 운영자와 자동화 스크립트가 같은 제어면을 쓴다. |

부적합하거나 별도 설명이 필요한 경우:

- 멀티 노드 HA 클러스터를 즉시 요구하는 고객
- 완전 관리형 SaaS를 원하는 고객
- 여러 data disk, 암호화 guest disk, 비-Linux guest identity 초기화까지 clone 자동화를 기대하는 고객

### 20.4 데모 흐름

1. 대시보드에서 단일 노드 상태, VM/컨테이너 수, 스토리지 상태를 보여준다.
2. VM 목록에서 NIC, IP, DNS, 상태 정보를 확인한다.
3. VM 생성 또는 준비된 템플릿 clone을 실행한다.
4. snapshot 생성과 rollback/delete 흐름을 보여준다.
5. ZFS pool/zvol과 origin 관계를 설명한다.
6. 모니터링, audit, job completion으로 작업 결과를 확인한다.
7. REST API 예시를 보여주고 자동화 가능성을 설명한다.

데모에서 피해야 할 것:

- 아직 Single Edge 범위가 아닌 클러스터 자동 복구를 확정 기능처럼 말하지 않는다.
- Guest reset clone을 실행 중인 VM에서 시도하지 않는다. source VM은 `shut off` 상태여야 한다.
- prepared template과 일반 VM Guest reset의 차이를 생략하지 않는다.

### 20.5 구매자별 가치 제안

| 대상 | 관심사 | 말할 내용 |
|------|--------|-----------|
| 인프라 운영자 | 반복 작업과 장애 대응 | VM lifecycle, snapshot, clone, audit, Web UI를 한 콘솔에서 처리한다. |
| 개발팀 리더 | 테스트 환경 생성 속도 | REST API와 clone/snapshot으로 검증 환경을 반복 생성할 수 있다. |
| 보안 담당 | 권한과 추적성 | JWT, RBAC, owner-scope, audit log, SSH allow list 같은 운영 통제가 있다. |
| 의사결정자 | 비용과 복잡도 | 단일 서버 운영에 맞춘 경량 제어면으로 과도한 클러스터 도입을 피한다. |

### 20.6 반론 대응

| 질문/반론 | 답변 기준 |
|-----------|-----------|
| Proxmox와 뭐가 다른가? | PureCVisor Single Edge는 공개 범위를 단일 노드 운영과 API/Web UI 자동화에 맞춘 경량 제어면으로 설명한다. 기능 비교표를 만들 때는 검증된 항목만 사용한다. |
| 클러스터 HA가 되는가? | 이 공개판의 현재 범위는 Single Edge다. 멀티 노드 기록은 문서에 남아 있지만 현재 운영 약속으로 말하지 않는다. |
| VM clone은 안전한가? | source VM `shut off`, 단일 data disk, prepared template 또는 libguestfs Guest reset 조건을 만족할 때만 허용한다. |
| 비밀번호나 키가 자동 정리되는가? | Guest reset은 identity와 SSH/cloud-init 상태를 정리하지만, OS 사용자 비밀번호를 새 값으로 만들지는 않는다. 필요하면 별도 주입 절차를 사용한다. |
| API 자동화가 가능한가? | REST API와 JSON-RPC 기반이며, Web UI도 같은 backend 상태를 사용한다. |

### 20.7 메시지 경계

사용 가능한 표현:

- "Single Edge 단일 노드 운영"
- "KVM/libvirt 기반 VM 관리"
- "LXC 컨테이너와 ZFS 스토리지 통합"
- "REST API와 Web UI를 같은 운영면에서 제공"
- "RBAC, audit, job completion 기반 작업 추적"

피해야 할 표현:

- "완전한 클러스터 HA 지원"
- "모든 guest OS의 clone identity 자동 보장"
- "운영 중 VM의 무중단 clone 보장"
- "백업/복구 RPO/RTO 수치 보장" 단, 실제 검증 수치가 없는 경우

### 20.8 영업 자료 체크리스트

- 제품 범위: Single Edge 공개판 기준인지 명시
- 기능 목록: VM, container, ZFS, network, monitoring, backup, REST, Web UI
- 검증 근거: 현재 가이드와 운영 기록의 날짜 기준
- 데모 준비: source VM 상태, template prepared 여부, `libguestfs-tools` 설치 여부
- 보안 설명: RBAC, audit, SSH allow list, 기본 admin 교체 권장
- 후속 질문 대응: 클러스터, guest reset 경계

### 20.9 고객 질문 스크립트

```text
현재 운영 대상은 단일 서버입니까, 여러 노드 클러스터입니까?
VM과 컨테이너를 같은 운영 화면에서 관리해야 합니까?
ZFS snapshot/clone을 운영 절차로 사용하고 있습니까?
브라우저 콘솔과 REST API 중 어느 쪽이 더 중요합니까?
권한 분리와 audit log가 구매 조건에 포함됩니까?
테스트/개발 환경을 반복 생성하는 시간이 병목입니까?
```

### 20.10 데모 후 후속 메일 구조

```text
제목: PureCVisor Single Edge 데모 후속 정리

확인한 요구:
- ...

데모에서 확인한 기능:
- VM lifecycle / snapshot / clone
- ZFS storage
- Web UI / REST API
- RBAC / audit

적합한 사용 시나리오:
- ...

확인이 더 필요한 항목:
- ...

다음 액션:
- ...
```

---

## 21. 아키텍처 리팩토링 가이드

> 이 장은 v1.0 이후 수행된 아키텍처 리팩토링으로 생성된 신규 파일을 설명합니다.

### 21.1 리팩토링 개요

대형 파일을 책임별로 분리하여 유지보수성과 증분 빌드 속도를 개선했습니다.

```
리팩토링 전:                          리팩토링 후:
rest_server.c (3,942 LOC)     →     rest_server.c (3,100 LOC)
                                    + rest_middleware.c (287 LOC)
                                    + rest_middleware.h (75 LOC)

purecvisortui.c (8,006 LOC)   →     purecvisortui.c (7,300 LOC)
                                    + tui_widgets.c (726 LOC)
                                    + tui_widgets.h (212 LOC)
                                    + tui_rpc.c (156 LOC)
                                    + tui_rpc.h (29 LOC)

purecvisorctl.c (6,996 LOC)   →     purecvisorctl.c (6,700 LOC)
                                    + cli_rpc.c (190 LOC)
                                    + cli_rpc.h (65 LOC)
                                    + cli_output.c (280 LOC)
                                    + cli_output.h (34 LOC)
```

### 21.2 REST 미들웨어 (`src/api/rest_middleware.c`)

HTTP 요청 처리 파이프라인에서 **횡단 관심사(cross-cutting concerns)**를 분리한 모듈입니다.

```
HTTP 요청 도착
    ↓
[rest_server.c]  요청 수신 + 라우팅
    ↓
[rest_middleware.c]  ETag / Rate Limit / 타임아웃 / 검증
    ↓
[dispatcher.c]  RPC 핸들러 호출
```

#### 제공 함수

| 함수 | 역할 | 사용 예시 |
|------|------|----------|
| `pcv_compute_etag(body, len)` | 응답 본문의 MD5 ETag 생성 | GET 응답 캐싱 (304 Not Modified) |
| `pcv_validate_required(params, keys)` | 필수 파라미터 검증 | RPC 파라미터 체크 |
| `pcv_get_endpoint_rate_limit(path)` | 엔드포인트별 Rate Limit 티어 | `/auth/*` 60, `/metrics` 3600 |
| `pcv_get_rpc_timeout(method)` | RPC 메서드별 타임아웃 | `vm.create` 30초, `vm.list` 8초 |
| `pcv_rest_error(msg, code, status, body)` | REST 에러 응답 생성 | 인증 실패, 파라미터 오류 |

#### Rate Limit 티어

```c
constexpr int 기본      = 600;   // req/min (일반 API)
constexpr int 인증      = 60;    // req/min (브루트포스 방지)
constexpr int 모니터링  = 3600;  // req/min (에이전트 폴링)
constexpr int VM생성    = 120;   // req/min (리소스 보호)
```

### 21.3 TUI 위젯 (`src/tui/tui_widgets.c`)

ncurses TUI의 **재사용 가능한 렌더링 컴포넌트**를 분리한 모듈입니다.

```
purecvisortui.c (메인 루프 + 탭 전환)
    ↓ 호출
tui_widgets.c (공통 위젯 렌더링)
    ├── draw_panel()        — 제목 패널
    ├── draw_bar()          — 퍼센트 게이지 바
    ├── draw_sparkline()    — 시계열 스파크라인
    ├── draw_table()        — 정렬 가능 테이블
    ├── bgrid_*()           — Braille 차트 엔진
    ├── spinner_*()         — 로딩 스피너
    ├── create_popup()      — 팝업 윈도우
    └── prompt_input()      — 입력 다이얼로그
```

#### Braille 차트 엔진

터미널 셀 1개로 **2×4 = 8 픽셀**을 표현하는 고해상도 차트입니다.

```
터미널 셀 하나의 Braille 점 배치:
┌───┐
│ 1 4 │    비트: 0x01 0x08
│ 2 5 │          0x02 0x10
│ 3 6 │          0x04 0x20
│ 7 8 │          0x40 0x80
└───┘
유니코드 범위: U+2800 ~ U+28FF (256 조합)
```

#### 색상 시스템

```c
// 7단계 그라데이션 (C_GRAD_BASE ~ C_GRAD_BASE+6)
// 초록 → 노랑 → 주황 → 빨강
pcv_color_for_pct(35.0)  → C_GREEN      // 안전
pcv_color_for_pct(75.0)  → C_YELLOW     // 주의
pcv_color_for_pct(95.0)  → C_RED        // 위험
```

### 21.4 TUI RPC 통신 (`src/tui/tui_rpc.c`)

TUI에서 데몬과 통신하는 **UDS JSON-RPC 클라이언트**입니다.

```c
// 사용 예시: VM 목록 조회
GError *err = nullptr;
gchar *resp = tui_send_request("vm.list", nullptr, &err);
if (resp) {
    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, resp, -1, nullptr);
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    // ... 데이터 처리 ...
    g_object_unref(parser);
    g_free(resp);
}
```

#### JSON 안전 접근 헬퍼

```c
// 위험: 키가 없으면 크래시
const gchar *name = json_object_get_string_member(obj, "name");

// 안전: 키가 없으면 기본값 반환
const gchar *name = safe_str(obj, "name", "(unknown)");
double cpu = safe_double(obj, "cpu_percent");     // 없으면 0.0
gint64 mem = safe_int(obj, "memory_bytes");       // 없으면 0
```

### 21.5 CLI RPC 통신 (`src/cli/cli_rpc.c`)

CLI(pcvctl)에서 데몬과 통신하는 **UDS JSON-RPC 클라이언트**입니다.

```
pcvctl vm list
    ↓
cli_rpc.c: purectl_send_request("vm.list", params)
    ↓
UDS 소켓: /var/run/purecvisor/daemon.sock
    ↓
dispatcher.c → handler_vm_lifecycle.c → vm_manager.c
```

#### PcvCtx 전역 컨텍스트

```c
typedef struct {
    gchar    *sock_path;    // UDS 소켓 경로
    gboolean  color;        // 터미널 색상 출력 여부 (isatty 검사)
    gboolean  json_mode;    // --json 플래그
    gboolean  csv_mode;     // --csv 플래그
} PcvCtx;

// 글로벌: 모든 CLI 커맨드에서 공유
extern PcvCtx g_ctx;
```

#### 색상 조건부 출력

```c
// cc()/ce(): 터미널이 아니면 (파이프 출력) 색상 코드를 비활성화
printf("%sVM 시작됨%s\n", cc(GREEN), ce());  // 터미널: 초록색 / 파이프: 무색
```

### 21.6 CLI 출력 포맷터 (`src/cli/cli_output.c`)

CLI의 **구조화된 출력 시스템**입니다.

#### PcvTable 시스템

```c
// 사용 예시: VM 목록 테이블 출력
PcvTable *t = ptbl_new(4, "NAME", "STATE", "CPU", "MEM");
ptbl_row(t, "web-prod", "running", "2.5%", "1.2G");
ptbl_row(t, "db-prod",  "stopped", "-",    "4.0G");

if (g_ctx.csv_mode)
    ptbl_print_csv(t);    // NAME,STATE,CPU,MEM\nweb-prod,running,...
else
    ptbl_print_plain(t);  // 정렬된 컬럼 출력

ptbl_free(t);
```

#### 출력 모드

| 모드 | 플래그 | 형식 |
|------|--------|------|
| 테이블 | (기본) | 정렬된 컬럼 + 색상 |
| JSON | `--json` | raw JSON 응답 |
| CSV | `--csv` | RFC 4180 CSV |
| Plain | 파이프 시 | 색상 없는 테이블 |

### 21.7 디스패처 핸들러 헤더 (`src/api/dispatcher_handlers.h`)

디스패처에서 사용하는 **핸들러 함수 선언 모음**입니다.

```c
// 핸들러 함수 시그니처 (모든 RPC 핸들러의 공통 패턴)
typedef void (*PcvDispatchHandler)(
    JsonObject      *params,      // RPC 파라미터
    const gchar     *rpc_id,      // 요청 ID (응답 매칭용)
    UdsServer       *server,      // UDS 서버 인스턴스
    GSocketConnection *connection  // 클라이언트 연결
);
```

### 21.8 커밋 메시지 Hook (`scripts/commit-msg`)

**Conventional Commits** 포맷을 강제하는 Git hook입니다.

```
허용 접두사: feat | fix | refactor | perf | docs | chore | test | ci | style

형식:
  <type>: <description>
  <type>(scope): <description>

예시:
  feat: VM 라이브 마이그레이션 대역폭 제한
  fix(rest_server): rate limiter 1024 IP 우회 수정
  refactor: C11 → C23 전환
  docs: CHANGELOG.md v1.0 업데이트
```

## 22. 품질 게이트 가이드

### 22.1 개요

PureCVisor는 **25단계** 품질 게이트로 코드 품질을 자동 검증합니다.

```
커밋 시:
  pre-commit hook (12단계) → commit-msg hook (1단계) → 커밋 완료

PR/Push 시:
  GitHub Actions CI (13단계) → 머지 허용
```

### 22.2 pre-commit hook (12단계)

```bash
# 설치
make install-hooks
# 또는
cp scripts/pre-commit .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
cp scripts/commit-msg .git/hooks/commit-msg && chmod +x .git/hooks/commit-msg
```

| 단계 | 게이트 | 트리거 | 차단 |
|------|--------|--------|------|
| 1 | Single Edge 릴리즈 빌드 경고 0 | C 변경 | 차단 |
| 2 | Single Edge 디버그 빌드 경고 0 | C 변경 | 차단 |
| 3 | 유닛 테스트 218건 (200건 최소) | C 변경 | 차단 |
| 4 | REST↔RPC 정합성 | API 변경 | 차단 |
| 5 | 프론트엔드 패턴 (10건 임계값) | UI 변경 | 차단 |
| 6 | cppcheck 정적 분석 에러 | C 변경 | 차단 |
| 7 | 바이너리 크기 3MB 상한 | C 변경 | 차단 |
| 8 | SAFE 통합 테스트 | API/UI + 데몬 | 차단 |
| 9 | 문서 정합성 (RPC/테스트 수) | C 변경 | 경고 |
| 10 | 신규 TODO/FIXME | C 변경 | 경고 |
| 11 | 커밋 메시지 포맷 | 매 커밋 | 차단 |
| 12 | Valgrind definite leak | C 변경 | 차단 |

### 22.3 commit-msg hook

```
허용 접두사: feat | fix | refactor | perf | docs | chore | test | ci | style

형식:
  <type>: <설명>
  <type>(scope): <설명>

예시:
  feat: VM 라이브 마이그레이션 대역폭 제한
  fix(rest_server): rate limiter 우회 수정
  perf: json_generator → json_to_string 전환
```

### 22.4 GitHub Actions CI (13단계)

| 단계 | 게이트 | 차단 |
|------|--------|------|
| 1-2 | Multi + Single + Release 빌드 경고 0 | 차단 |
| 3 | 유닛 테스트 200건+ | 차단 |
| 4 | cppcheck strict (exitcode=1) | 차단 |
| 5 | REST↔RPC 정합성 | 차단 |
| 6 | 프론트엔드 패턴 (10건 임계값) | 차단 |
| 7 | Valgrind definite leak | 차단 |
| 8 | 코드 커버리지 (gcov) | 수집 |
| 9 | 바이너리 크기 3MB 상한 | 차단 |
| 10 | 헤더 순환 의존 검사 | 차단 |
| 11 | 문서 정합성 | 차단 |
| 12 | TODO/FIXME 총량 (5건 임계값) | 경고 |
| 13 | 커밋 메시지 포맷 (PR only) | 차단 |

### 22.5 품질 게이트 건너뛰기

```bash
# 긴급 커밋 (pre-commit + commit-msg 모두 건너뜀)
git commit --no-verify -m "hotfix: 긴급 수정"

# 주의: CI는 건너뛸 수 없음 (GitHub Actions는 항상 실행)
```

### 22.6 신규 RPC 추가 시 체크리스트

1. `handler_xxx.h`에 함수 선언
2. `handler_xxx.c`에 핸들러 구현
3. `dispatcher.c` 라우트 등록 (`g_hash_table_insert`)
4. `Makefile` DAEMON_SRCS에 소스 등록
5. `make clean && make all` — 경고 0 확인
6. `nc -U` 수동 RPC 테스트
7. REST 필요 시 `rest_server.c` 라우팅 추가
8. `scripts/verify_api_consistency.sh` — FAIL 0 확인
9. CLAUDE.md RPC 수 갱신 (문서 정합성 게이트)
10. `git commit` — 12단계 게이트 통과

---

> PureCVisor v1.0 Complete Guide — 22장 끝.
