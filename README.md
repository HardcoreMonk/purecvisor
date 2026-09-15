# PureCVisor Single Edge

> 단일 Linux/KVM 노드에서 VM, LXC 컨테이너, ZFS 스토리지, OVS/OVN 네트워크, 인증, 감사, 관측성,<br> Web UI를 한 프로세스로 관리하는 C23 기반 하이퍼바이저 오케스트레이터입니다.

[![Edition: Single Edge](https://img.shields.io/badge/Edition-Single%20Edge-blue.svg)](docs/PUBLIC_RELEASE_BOUNDARY.md)
[![Runtime: Linux/KVM](https://img.shields.io/badge/Runtime-Linux%2FKVM-2f855a.svg)](docs/GUIDE.md)
[![Language: C23](https://img.shields.io/badge/Language-C23-555.svg)](docs/PUBLIC_SOURCE_POLICY.md)
[![Version: 2.0.0](https://img.shields.io/badge/Version-2.0.0-6b46c1.svg)](include/purecvisor/version.h)

[공개 문서](https://purecvisor.site) · [전체 운영 가이드](https://purecvisor.site/ko/getting-started/installation/) · [네트워크·OVN](https://purecvisor.site/ko/infrastructure/networking/) · [데이터베이스 아키텍처](https://purecvisor.site/ko/development/database-architecture/)

PureCVisor Single Edge는 `purecvisorsd` 하나로 독립 노드의 가상화 운영 표면을 묶습니다.<br> CLI, REST API, UDS JSON-RPC, Vanilla JS Web UI가 같은 dispatcher와 RBAC 정책을 통과하며, 긴 작업은 Job ID, WebSocket 완료 알림, polling, audit log로 추적됩니다.

이 저장소는 Linux/KVM 기반 `purecvisor-single` 공개 스냅샷입니다.<br> 공개 범위는 Single Edge 기능과 그 실행에 필요한 공통 코어로 제한합니다.<br> 전체 운영 매뉴얼은 [공개 문서 사이트](https://purecvisor.site)와 [docs/GUIDE.md](docs/GUIDE.md), 개발 규칙은 [AGENTS.md](AGENTS.md), 공개판 경계는 [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md)를 기준으로 봅니다.

공개 네트워크 계약은 작업 전 읽기 전용 host baseline 확인, 등록된 generic OVN
18개 RPC, switch-owned DHCP 자동 정리와 인증 REST ACL/NAT filter까지입니다.<br> 미완성
OVN/NFV Load Balancer와 VM 자동 포트 내부 helper는 공개 기능이 아니며, Local VPC의
선택형 OVN backend는 별도 실환경 gate가 남아 있습니다.<br> 공개 데이터베이스 설명은 이
저장소 소스에 실제 포함된 로컬 SQLite 9개를 기준으로 합니다.

공개 소스에는 내부 전용 Monitoring 확장과 비공개 운영 자료를 포함하지 않습니다.<br>
기존 공개 host·VM·process 지표, Prometheus와 일반 알림은 유지합니다. <br> 로컬 검증은
출시 인증과 별개이며, 전체 감사 **FAIL(미완료)**와 지원 환경·실노드 인증 잔여는
[품질 게이트 현황](docs/GUIDE.md#227-2026-09-07-검토시정-현황)을 따릅니다.

## 빠른 시작

Host 설치 기준은 Ubuntu Server 26.04.1 LTS `amd64`입니다. <br> 전체 권장 사양과 설치 환경별 관리 IPv4 선정·단일 노드 구성 절차는 [docs/GUIDE.md](docs/GUIDE.md)의 설치 장을 따릅니다.

공개 소스를 내려받고 저장소 디렉터리로 이동합니다.

```bash
git clone https://github.com/HardcoreMonk/purecvisor.git
cd purecvisor
```

<details>
<summary>Ubuntu 의존성 설치 예시</summary>

```bash
sudo apt update
sudo apt install -y \
  ca-certificates curl git jq cpu-checker netcat-openbsd \
  build-essential gcc-14 make pkg-config ccache fakeroot \
  libglib2.0-dev libjson-glib-dev libsoup-3.0-dev \
  libvirt-dev libvirt-clients libvirt-daemon-system qemu-system-x86 ovmf \
  libguestfs-tools \
  libvirt-glib-1.0-dev liblxc-dev lxc lxc-utils \
  zfsutils-linux libsqlite3-dev libssl-dev \
  libcap-dev libseccomp-dev libreadline-dev liburing-dev \
  libbpf-dev libxml2-dev \
  protobuf-c-compiler libprotobuf-c-dev
```

</details>

UI 번들·검증 도구의 의존성은 저장소 루트에서 설치합니다. 검증 환경은 Node.js 24와
npm을 사용했습니다. 전체 C·계약 검증에는 `wireguard-tools`, `sqlite3`,
`openvswitch-switch`, `python3-pytest`, `strace`도 준비합니다.

```bash
npm ci
```

.deb 패키지를 직접 만들어 설치할 수도 있습니다(Ubuntu 26.04.1, 데몬·CLI·UI·systemd 유닛 일괄).

```bash
make deb                                          # dist/purecvisor-single_<ver>_amd64.deb 생성
sudo apt install -y ./dist/purecvisor-single_*.deb
sudoedit /etc/purecvisor/daemon.conf               # admin_password·storage·TLS 편집
sudo chmod 600 /etc/purecvisor/daemon.conf
sudo systemctl start purecvisorsd
```

소스에서 직접 빌드·검증:

```bash
make single
make test
make check-all
```

현재 로컬 단일 노드에만 배포한 뒤, 설치 중 확정한 관리 IPv4로 기본 자체 HTTPS health를 확인합니다.

```bash
NODE_IPV4="<configured-management-ipv4>"
PCV_NODES="" scripts/deploy.sh --nodes local
curl -ksS "https://${NODE_IPV4}/api/v1/health" | jq '{status,version,tls:.checks.tls}'
```

정상적인 Single Edge health 응답은 다음 성격을 유지해야 합니다.

```json
{
  "service": "purecvisorsd",
  "status": "ok",
  "version": "2.0.0",
  "capabilities": {
    "cluster": false
  }
}
```

Release 빌드는 다음 명령으로 확인합니다.

```bash
make release
```

---

## 기본 접속 정보

| 인터페이스 | 기본 경로 |
|------------|-----------|
| Web UI | `https://<management-ipv4>/ui/` |
| 이벤트 센터 | `https://<management-ipv4>/ui/#/ops-triage` |
| REST API | `https://<management-ipv4>/api/v1/` |
| Health | `https://<management-ipv4>/api/v1/health` |
| Metrics | `https://<management-ipv4>/api/v1/metrics` |
| 로컬 복구 HTTP | `http://127.0.0.1:8080/` |
| UDS socket | `/var/run/purecvisor/daemon.sock` |

예시의 `-k`는 최초 자체서명 인증서 확인용입니다. 운영 CA 인증서를 배치한 뒤에는 `-k`를 제거하고 인증서 체인과 설정한 관리 IPv4 또는 DNS 이름의 SAN을 검증합니다.

첫 설치 bootstrap 계정은 운영 전 반드시 전용 관리자 계정으로 교체해야 합니다. 셀프 회원가입은 `[auth] allow_self_register` 설정으로 제어하며, 기본값은 비활성화입니다.

---

## 사용 예시

토큰을 발급합니다.

```bash
NODE_IPV4="<configured-management-ipv4>"
TOKEN=$(curl -ksS -X POST "https://${NODE_IPV4}/api/v1/auth/token" \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<configured-admin-password>"}' \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['access_token'])")
```

VM을 만들고 시작합니다.

```bash
pcvctl vm create web01 --vcpu 2 --memory_mb 2048 --disk_size_gb 20 \
  --qos_min_mbps 0 --qos_max_mbps 1000
pcvctl vm start web01
pcvctl vm list
```

REST API로 VM 목록을 조회합니다.

```bash
curl -ksS -H "Authorization: Bearer $TOKEN" \
  "https://${NODE_IPV4}/api/v1/vms" | python3 -m json.tool
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
make sanitize
make memcheck
make release
```

`make check-all`은 RBAC 정책, RPC 소비⊆등록 계약, 공개 주석 정책, UI 표면과 소스맵
부재를 포함한 40개 게이트를 실행합니다. 정확한 목록은 [Makefile](Makefile)의
`check-all` 의존성을 따릅니다. UI 표면과 반사실 회귀만 확인하려면
`make check-single-ui-surface`를 실행합니다.

로컬 커밋 시 변경 유형에 맞는 검사를 자동 실행하려면 pre-commit 훅을 설치합니다.

```bash
make install-hooks
```

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
make check-single-ui-surface
bash tests/integration/test_single_backend_build_boundaries.sh
bash tests/integration/test_single_ovn_ovs_layout.sh
```

공개 소스의 주석 제거 정책을 확인할 때:

```bash
make check-public-comments
git diff --check
```

Web UI 시각 규격은 루트 [DESIGN.md](DESIGN.md)를 기준으로 관리합니다. <br> UI 모듈, Service Worker, vendor 자산, `ui/samples/` 프리뷰를 바꾼 경우 공개 배포 전 외부 런타임 참조가 남지 않았는지 확인합니다.

```bash
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" ui/index.html ui/guide.html ui/app.bundle.js ui/sw.js ui/vendor
```

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
| [docs/GUIDE.md](docs/GUIDE.md) | 제품, 설치, 운영 통합 가이드 |
| [docs/DATABASE_STRUCTURE.md](docs/DATABASE_STRUCTURE.md) | 공개 SQLite 9개 저장소의 책임과 복구 경계 |
| [DESIGN.md](DESIGN.md) | Web UI 시각 규격, token, typography, component state |
| [docs/DEVELOPMENT_VERIFICATION_POLICY.md](docs/DEVELOPMENT_VERIFICATION_POLICY.md) | 단계별 검증 기준 |
| [docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md](docs/SERVICE_FUNCTIONAL_TEST_SCENARIOS.md) | 서비스 기능 테스트 시나리오 기준 |
| [docs/PUBLIC_SOURCE_POLICY.md](docs/PUBLIC_SOURCE_POLICY.md) | 공개 소스 주석 제거·소스맵 제외 정책 |
| [docs/PUBLIC_RELEASE_BOUNDARY.md](docs/PUBLIC_RELEASE_BOUNDARY.md) | Single Edge 공개판 경계 |
| [docs/PUBLIC_DOCUMENTATION_SITE.md](docs/PUBLIC_DOCUMENTATION_SITE.md) | Astro/Starlight GitHub Pages 운영·검증 기준 |
| [docs/ADR_INDEX.md](docs/ADR_INDEX.md) | ADR별 현재 적용 상태 |
| [docs/adr/](docs/adr/) | 설계 결정 기록 |

---

## 개발 규칙 요약

- C 표준은 `gnu23`입니다.
- 목표는 빌드 경고 0건입니다.
- 단일 프로세스 + `GMainLoop` 실행 모델을 유지합니다.
- 장시간 RPC는 accepted 응답과 worker callback의 실제 결과 audit를 분리합니다.
- JSON-RPC 응답은 `pure_rpc_build_success_response`와 `pure_rpc_build_error_response`를 사용합니다.
- 프론트엔드는 Vanilla JS와 `PCV.*` 네임스페이스를 유지합니다.
- API endpoint는 `ui/modules/endpoints.js`의 `EP` registry를 사용합니다.
- 신규 UI 코드는 `innerHTML` 계열 대입을 금지하고 `PCV.uxlib`/`HN` 노드 빌더와 `textContent`로 조립합니다(zone ADR-013). 레거시 접점은 래칫으로 점진 축소합니다(`npm run lint:domsafe`).
- VM/템플릿 이름은 핸들러 진입점에서 검증 함수를 경유합니다.
- `system()` / `popen()`은 금지하며 `pcv_spawn_sync()` argv 배열을 사용합니다.

---

## 라이선스

PureCVisor는 [Apache License 2.0](LICENSE)에 따라 제공되는 오픈소스 소프트웨어입니다.<br> 사용·수정·배포 시 해당 라이선스의 조건을 따라야 합니다.

포함된 외부 자산과 개발 의존성의 라이선스는 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)를 확인하십시오.
