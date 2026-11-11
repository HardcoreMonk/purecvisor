# PureCVisor Single Edge 운영 가이드
> **2026-11-11 공개 소스 기준 (2.2.0):** 2.1.0 이후의 Single Edge 기능을 담은 기능 릴리스입니다. 호스트 장치 화면과 조회(`host.device.list`), USB 장치 영구 할당, Windows 게스트 명령 실행이 포함됩니다.
> DB 스키마와 설정 파일은 그대로지만 USB 할당과 Windows 게스트 명령 실행의 동작이 바뀌었습니다. 변경 목록과 업그레이드·원복 안내는 저장소의 `CHANGELOG.md` 2.2.0 항목을 따릅니다.
> 업그레이드 전에 붙인 USB 장치는 live 정의에만 있으므로 같은 `usb-attach`를 한 번 더 보내 영구 정의에 저장합니다.

> **2026-11-08 공개 소스 기준 (2.0.4):** 읽기 전용으로 붙인 컨테이너 볼륨이 컨테이너 안에서 읽기 전용으로 유지되도록 고친 보안 수정 릴리스입니다.
> 새 기능은 없으며 데이터·설정 형식과 API는 그대로입니다. 변경 목록과 업그레이드 안내는 저장소의 `CHANGELOG.md` 2.0.4 항목을 따릅니다.
> 볼륨을 쓰는 컨테이너는 업그레이드 뒤 한 번 정지·시작해야 새 동작이 적용됩니다.

> **2026-10-25 공개 소스 기준 (2.0.3):** 2.0.2의 컨테이너 명령 실행 결함을 고친 보안 수정 릴리스입니다.
> 새 기능은 없으며 데이터·설정 형식은 그대로입니다. 변경 목록과 업그레이드 안내는 저장소의 `CHANGELOG.md` 2.0.3 항목을 따릅니다.
> 2.0.2 이하 설치본의 업그레이드는 2장 `.deb` 설치 방법의 업그레이드 안내를 따릅니다.

> **2026-10-16 공개 소스 기준 (2.0.2):** 2.0.1의 REST 요청 처리 결함 두 건을 고친 보안 수정 릴리스입니다.
> 새 기능은 없으며 데이터·설정 형식은 그대로입니다. 변경 목록은 저장소의 `CHANGELOG.md` 2.0.2 항목을 따릅니다.
> 2.0.1 이하 설치본의 업그레이드는 2장 `.deb` 설치 방법의 업그레이드 안내를 따릅니다.

> **2026-10-11 공개 소스 기준 (2.0.1):** 2.0.0의 권한 판정·VPC 서비스 게시·세션 처리 결함을 고친 보안 수정 릴리스입니다.
> 새 기능은 없으며 데이터·설정 형식은 그대로입니다. 변경 목록은 저장소의 `CHANGELOG.md` 2.0.1 항목을 따릅니다.
> 2.0.0 설치본의 업그레이드는 2장 `.deb` 설치 방법의 업그레이드 안내를 따릅니다.

> **2026-10-08 공개 소스 기준:** 버전 2.0.0을 유지하며 AWS 수동 이관·S3 백업을 제거했습니다.
> OVA·로컬 백업·공통 Job/WS·QGA를 유지하고 VM 시작·Trace·VNC·OVA·자가 치유를 보강했습니다.
> 기존 v2.0.0 태그/바이너리와 현재 main은 다를 수 있으므로 source commit을 함께 확인합니다.
> 전체 감사와 실환경 인증의 잔여는 유지합니다.


> **2026-10-02 소스 기준:** 공개 제품 로직을 개발 main에 맞췄다. NVRAM·첫 알림 쿨다운 시정은 유지하며 LXC는 ZFS 전용이다. 공개 전용 Btrfs와 개발 main 미병합 후속 브랜치는 포함하지 않는다. [정합화 인계](../docs/operations/2026-10-02-public-main-sync-handoff.md)에서 이번 검증을 구분한다.


> **Single Edge Edition** | 공개 배포용 독립 노드 가이드 | `purecvisorsd` | v2.2.0
>
> **현재 범위**: 이 문서는 `purecvisor-single` 기준으로 정리되며, Single Edge에서 실제로 제공하는 기능과 운영 절차만 안내합니다.
>
> **배포 기준**: 기본 `purecvisorsd` 자체 HTTPS와 선택형 NGINX 외부 TLS 종료를 모두
> 지원합니다. NGINX는 외부 종료를 선택한 노드에서만 필요합니다. 공개 도메인·인증서·노드
> 주소는 배포 환경에 맞춰 설정하고, 선택한 모드의 health·version·BPF 상태 검사를
> 통과해야 합니다.
>
> **기능 기준(2026-08-31)**: 네트워크 작업은 읽기 전용 호스트 기준선을 먼저 확인합니다.
> generic OVN 공개 표면은 등록된 18개 RPC, switch-owned DHCP 자동 정리와 인증 REST
> ACL/NAT 필터 전달까지입니다. 완결되지 않은 OVN/NFV Load Balancer와 VM 자동 포트 내부
> helper는 공개 기능이 아니며, Local VPC의 선택형 OVN backend는 별도 지원 gate를 따릅니다.
>
> **문서 현행화(2026-11-11)**: 2.2.0 공개 소스 기준으로 최신 릴리스 기준(1.0), 프로젝트 통계 스냅샷(18.6)과 공개 소스·문서 현황(22.8)을 갱신했습니다. 공개 소스 검증 결과와 전체 감사·지원 환경 인증은 22.8절에서 구분합니다.
>
> **단축키**: `Ctrl+K`(또는 `/`, `Ctrl+Shift+F`) 통합 검색 팔레트 · `Ctrl+N` 또는 `n` 새 VM · `Ctrl+D` VM 설정 · `Ctrl+P` 환경설정 · `Ctrl+B` 사이드바 접기 · `F11` 전체 화면 · `?` 단축키 도움말 · `g` 대시보드 · `m` 운영 개요 · `Esc` 대화상자 닫기

---

## 목차

1. [시작하기](#1-시작하기)
2. [설치 및 환경 구성](#2-설치-및-환경-구성)
3. [VM 관리](#3-vm-관리)
4. [컨테이너 관리](#4-컨테이너-관리)
5. [스토리지](#5-스토리지)
6. [네트워크](#6-네트워크)
8. [모니터링 & 알림](#8-모니터링--알림)
9. [백업 & 복원](#9-백업--복원)
10. [보안](#10-보안)
11. [비동기 작업 관리](#11-비동기-작업-관리)
12. [AI & 자가치유](#12-ai--자가치유)
13. [Web UI](#13-web-ui)
14. [REST API](#14-rest-api)
15. [CLI 레퍼런스](#15-cli-레퍼런스)
16. [설정 레퍼런스](#16-설정-레퍼런스)
17. [트러블슈팅](#17-트러블슈팅)
18. [부록](#18-부록)
19. [개발자 & 엔지니어 가이드](#19-개발자--엔지니어-가이드)
20. [영업 & 마케팅 가이드](#20-영업--마케팅-가이드)
21. [아키텍처 리팩토링 가이드](#21-아키텍처-리팩토링-가이드)
22. [품질 게이트 가이드](#22-품질-게이트-가이드)

---

## 1. 시작하기

### 1.0 최신 릴리스 기준

현재 공개 제품 버전은 `2.2.0`입니다.<br>
단일 노드 배포 뒤 `purecvisorsd`는 항상 active여야 하고, NGINX는 선택형 외부 TLS 종료 모드에서만 active 조건입니다.<br>
선택한 모드의 `/api/v1/health`, `/api/v1/version`과 BPF 상태 검사가 통과해야 합니다.

**2026-11-11 공개 현황:** 공개 `2.2.0` 릴리스는 2.1.0 위에 호스트 장치 화면과 조회, USB 장치 영구 할당, Windows 게스트 명령 실행과 GPU IOMMU 그룹 판정 수정을 더한 기능 릴리스이며 Single Edge 범위를 유지합니다.

게시 전 검증에서 공개판 계약 게이트(`make check-all`) 47개, release·debug 빌드 각각의 C 단위 시험 2,514개(환경 의존 22개 생략)와 브라우저 UI 시험 613개가 통과했습니다. 패키지 검사, 격리된 Ubuntu 26.04 컨테이너의 설치·업그레이드·제거 lifecycle과 개발 호스트의 실제 업그레이드·되돌림도 확인했습니다.

지정 공개 소스 검증은 통과했지만 전체 소스 감사와 지원 환경 인증은 미완료이며, 전체 감사 판정은 `FAIL(미완료)`입니다. 회차별 결과와 GPU Passthrough 영상의 검증 범위는 [품질 게이트의 공개 현황](/ko/development/quality-gates/#228-2026-09-15-공개-소스문서-현황)을 따릅니다.

> **검증 운영 문서**: 개발 단계별 검증 기준은 [DEVELOPMENT_VERIFICATION_POLICY.md](../docs/DEVELOPMENT_VERIFICATION_POLICY.md)를 참조하세요.<br>
이 문서는 `Level 1 로컬 코드 검증`부터 `Level 4 출시 게이트`까지의 공식 규칙을 정의합니다.

### 1.1 PureCVisor란?

PureCVisor Single Edge는 C23 기반 KVM 하이퍼바이저 오케스트레이터입니다.<br>
단일 프로세스 데몬 `purecvisorsd`가 fork 없이 GMainLoop 이벤트 루프로 동작하며, VM, 컨테이너, 스토리지, 네트워크를 통합 관리합니다.

**핵심 특징:**

| 특징 | 설명 |
|------|------|
| 단일 프로세스 아키텍처 | fork 없이 GMainLoop + GTask 스레드 풀로 동작 |
| 클라이언트 인터페이스 | CLI + Web UI |
| 에디션 전용 RPC 집합 | GHashTable O(1) 디스패치, 플러그인 동적 확장 |
| REST API | JWT HS256 + RBAC + VM owner-scope + CORS + gzip 압축 |
| Prometheus 메트릭 | 내장 exporter 기반 상태 노출 |
| 배포 형태 | Single Edge (독립 노드) |
| C23 코드베이스 | Single Edge 데몬, CLI, Web UI, 테스트, 운영 스크립트 |

### 1.2 아키텍처 개요

`purecvisorsd`는 API transport, dispatcher, 도메인 핸들러와 서비스 모듈을 한 프로세스에 두고 `GMainLoop`가 전체 수명주기를 소유합니다.<br>
짧은 작업은 이벤트 루프에서 응답을 끝내고, 긴 작업만 제한된 `GTask` 워커 풀로 보냅니다.<br>
아래 탭에서 실제 TLS 배포 모드를 선택하면 클라이언트와 부팅 입력부터 로컬 영속 상태와 Linux/KVM 호스트까지 이어지는 Single Edge 전체 구조를 해당 진입 경계로 확인할 수 있습니다.<br>
기본 선택은 NGINX가 없는 `purecvisorsd` 직접 HTTPS 모드입니다.<br>
마우스를 사용하는 환경에서는 SVG의 서비스 레이어 또는 컴포넌트에 포인터를 올려 직접 연결된 화살표의 흐름을 강조할 수 있습니다.

<section class="pcv-overview-architecture-tabs pcv-architecture-wide" data-pcv-architecture-tabs aria-label="TLS 배포 모드별 PureCVisor Single Edge 아키텍처">
  <div class="pcv-architecture-tablist" role="tablist" aria-label="TLS 배포 모드">
    <button class="pcv-architecture-tab" id="pcv-architecture-tab-direct" type="button" role="tab" aria-selected="true" aria-controls="pcv-architecture-panel-direct" tabindex="0" data-pcv-architecture-tab>purecvisorsd 직접 HTTPS</button>
    <button class="pcv-architecture-tab" id="pcv-architecture-tab-nginx" type="button" role="tab" aria-selected="false" aria-controls="pcv-architecture-panel-nginx" tabindex="-1" data-pcv-architecture-tab>NGINX 외부 TLS 종료</button>
  </div>

  <div class="pcv-architecture-panel" id="pcv-architecture-panel-direct" role="tabpanel" aria-labelledby="pcv-architecture-tab-direct" data-pcv-architecture-panel>
    <figure class="pcv-overview-architecture pcv-control-map pcv-architecture-source" aria-labelledby="pcv-overview-architecture-direct-title">
      <div class="pcv-map-bar">
        <strong id="pcv-overview-architecture-direct-title">Single Edge · purecvisorsd 직접 HTTPS</strong>
        <div class="pcv-map-meta">
          <span class="pcv-status"><i aria-hidden="true"></i>기본 · NGINX 없음</span>
          <a class="pcv-architecture-source-open" href="/assets/diagrams/purecvisor-single-direct-https-architecture.svg" target="_blank" rel="noopener">확대해서 보기 <span aria-hidden="true">↗</span></a>
        </div>
      </div>
      <a class="pcv-architecture-source-canvas" href="/assets/diagrams/purecvisor-single-direct-https-architecture.svg" target="_blank" rel="noopener" aria-label="NGINX 없이 클라이언트가 purecvisorsd의 HTTPS와 WebSocket TLS에 직접 접근하고, GMainLoop 제어면, 6개 서비스 도메인, 로컬 SQLite 데이터베이스 9개와 Linux/KVM 호스트로 이어지는 전체 아키텍처 SVG를 새 탭에서 확대해서 보기" data-pcv-architecture-interactive="direct">
        <img class="pcv-overview-architecture-image pcv-architecture-source-image" src="/assets/diagrams/purecvisor-single-direct-https-architecture.svg" width="1935.1484375" height="2523.699951171875" loading="lazy" decoding="async" alt="Web UI와 REST client가 NGINX 없이 purecvisorsd의 HTTPS REST와 WebSocket TLS transport에 직접 접근하고, GMainLoop 제어면, 동기·비동기 완료 경로, 6개 서비스 도메인, 로컬 SQLite 데이터베이스 9개와 desired state를 거쳐 Linux/KVM 호스트로 이어지는 PureCVisor Single Edge 아키텍처">
      </a>
      <figcaption class="pcv-architecture-source-note"><code>purecvisorsd</code>가 외부 <code>:443</code>의 HTTPS와 WebSocket TLS를 직접 종료하는 기본 모드입니다.<br>별도 NGINX 프로세스가 없습니다.</figcaption>
    </figure>
  </div>

  <div class="pcv-architecture-panel" id="pcv-architecture-panel-nginx" role="tabpanel" aria-labelledby="pcv-architecture-tab-nginx" data-pcv-architecture-panel hidden>
    <figure class="pcv-overview-architecture pcv-control-map pcv-architecture-source" aria-labelledby="pcv-overview-architecture-nginx-title">
      <div class="pcv-map-bar">
        <strong id="pcv-overview-architecture-nginx-title">Single Edge · NGINX 외부 TLS 종료</strong>
        <div class="pcv-map-meta">
          <span class="pcv-status"><i aria-hidden="true"></i>선택형 · host-loopback</span>
          <a class="pcv-architecture-source-open" href="/assets/diagrams/purecvisor-single-full-architecture.svg" target="_blank" rel="noopener">확대해서 보기 <span aria-hidden="true">↗</span></a>
        </div>
      </div>
      <a class="pcv-architecture-source-canvas" href="/assets/diagrams/purecvisor-single-full-architecture.svg" target="_blank" rel="noopener" aria-label="NGINX가 외부 TLS를 종료하고 purecvisorsd의 loopback REST와 WebSocket으로 전달한 뒤 GMainLoop 제어면, 6개 서비스 도메인, 로컬 SQLite 데이터베이스 9개와 Linux/KVM 호스트로 이어지는 전체 아키텍처 SVG를 새 탭에서 확대해서 보기" data-pcv-architecture-interactive="nginx">
        <img class="pcv-overview-architecture-image pcv-architecture-source-image" src="/assets/diagrams/purecvisor-single-full-architecture.svg" width="2009.1953125" height="2653" loading="lazy" decoding="async" alt="Web UI와 REST client가 NGINX 외부 TLS 종료를 거쳐 purecvisorsd의 loopback REST와 WebSocket transport에 접근하고, GMainLoop 제어면, 동기·비동기 완료 경로, 6개 서비스 도메인, 로컬 SQLite 데이터베이스 9개와 desired state를 거쳐 Linux/KVM 호스트로 이어지는 PureCVisor Single Edge 아키텍처">
      </a>
      <figcaption class="pcv-architecture-source-note">NGINX가 외부 <code>:443</code>을 소유하고 <code>purecvisorsd</code>의 loopback REST·WebSocket으로 전달하는 선택형 모드입니다.<br>ADR-0029의 host-loopback 신뢰 경계가 성립하는 전용 호스트에서만 사용합니다.</figcaption>
    </figure>
  </div>
</section>

#### 1.2.1 런타임·접근 경계

- **사용자와 외부 소비자**: Web UI, `pcvctl`, REST API client, 선택형 gRPC client와 Prometheus scraper가 단일 노드에 접근합니다.
- **기본 HTTPS 모드**: `purecvisorsd`가 외부 `:443`의 REST·WebSocket TLS를 직접 종료합니다.<br>
HTTP listener는 loopback 복구 경로로 제한합니다.
- **선택형 NGINX 외부 종료 모드**: NGINX가 외부 `:443`을 소유하고 `127.0.0.1`의 daemon REST·WebSocket으로 전달합니다.<br>
두 모드가 같은 주소의 `:443`을 동시에 소유하지 않습니다.
- **부팅 입력**: `daemon.conf`, secret source와 Kernel LSM 목록이 bootstrap 정책, transport, BPF 준비 상태를 결정합니다.
- **프로세스 내부 transport**: root 전용 UDS JSON-RPC, libsoup3 REST, 기본 비활성인 선택형 gRPC와 WebSocket event channel은 별도 서비스가 아니라 `purecvisorsd` 내부 진입점입니다.<br>
선택형 외부 종료 모드의 NGINX만 별도 프로세스입니다.

#### 1.2.2 요청·권한·완료 흐름

1. `pcvctl`은 로컬 UDS로, Web UI와 REST client는 선택한 HTTPS 모드로 요청합니다.<br>
선택형 gRPC는 token과 고정 caller role을 검증한 뒤 같은 RPC 정책으로 수렴합니다.
2. transport가 인증 주체를 확정하면 dispatcher가 method policy, RBAC와 VM·컨테이너 owner-scope를 검사합니다.
3. dispatcher는 `GHashTable`에서 O(1)로 도메인 핸들러를 찾아 검증된 요청만 전달합니다.
4. 짧은 작업은 canonical JSON-RPC 응답으로 즉시 완료합니다.
5. 긴 작업은 Job ID가 포함된 accepted 응답을 먼저 보내고 제한된 `GTask` 워커에서 실행합니다.<br>
상태 registry를 사용하는 경로만 `pcv_jobs.db` 행을 생성하며, 일부 경로는 합성 Job ID를 사용합니다.
6. worker callback은 실제 결과를 경로별 상태 저장소, `pcv_audit.db`, daemon log와 WebSocket 완료 이벤트에 남깁니다.<br>
클라이언트는 WebSocket 또는 polling으로 최종 상태를 확인하며, **accepted 응답은 실제 성공을 뜻하지 않습니다.**

#### 1.2.3 서비스 도메인

- **Workload**: VM, LXC 컨테이너, template과 GPU 연결
- **Network**: Linux bridge, Local VPC, OVS·OVN, Security Group과 QoS
- **Storage**: ZFS, snapshot, backup·restore, iSCSI와 로컬 백업 작업
- **Security**: JWT, TOTP, RBAC, audit, HIDS·HIPS와 BPF LSM audit
- **Monitoring**: host telemetry, process status와 Prometheus metrics
- **Operations**: telemetry, alert, Web Push, AI healing과 plugin

Monitoring 경로는 `handler_monitor`, telemetry, process monitor와 eBPF telemetry가 제공하는
host·process 관측값을 조회합니다.<br>
공개 소스에는 systemd D-Bus availability writer나 별도 Monitoring SQLite DB가 없습니다.

#### 1.2.4 영속 상태와 호스트 통합

PureCVisor는 외부 DBMS 없이 로컬 SQLite 데이터베이스 9개(대부분 WAL 모드)와 파일 기반 desired state를
사용합니다.

- **Core·identity·security·network DB 7개**: `vm_state.db`, `pcv_audit.db`, `pcv_jobs.db`, `rbac.db`, `pcv_security.db`, `security_groups.db`, `vpc.db`
- **Operations DB 1개**: `pcv_webpush.db`
- **호스트 자원 경계 DB 1개**: `resource_registry.db`(호스트 자원 등록·테넌트 허용·VPC 주소 풀, 2.1.0)
- **Desired state**: network, overlay, QoS, BPF와 backup 설정
- **Virtualization**: libvirt, QEMU, KVM과 LXC
- **Storage**: qcow2/raw, 선택형 ZFS, LIO와 open-iscsi
- **Network host**: Linux bridge, nftables, dnsmasq, WireGuard, tc, eBPF, OVS와 OVN
- **Host security**: Kernel LSM, bpffs, Suricata, systemd, journald, cgroups와 PSI
- **Acceleration**: GPU, SR-IOV와 선택형 DPDK

SQLite는 의도, 작업 상태와 증거를 보존하지만 libvirt domain, ZFS dataset, bridge, nftables, OVS·OVN과 bpffs의 actual state를 대신하지 않습니다.<br>
DB 사이의 분산 트랜잭션이나 노드 간 복제도 제공하지 않으므로, 재시작과 복원 뒤에는 각 도메인의 reconcile과 실제 시스템 상태를 함께 확인해야 합니다.<br>
DPDK는 선택형 가속 경로이며 현재 BPF LSM hook은 기존 LSM 결정을 바꾸지 않는 audit-only 경계입니다.

#### 1.2.5 Single Edge 경계와 상세 문서

이 구조는 독립 Linux/KVM 노드 하나와 `purecvisorsd` 제어면 하나만 설명합니다.

- SQLite 파일별 책임, schema, 일관성·백업·복구 경계: [데이터베이스 아키텍처](/ko/development/database-architecture/)
- TLS 모드와 설치 절차: [설치 및 환경 구성](/ko/getting-started/installation/#tls-배포-모드-선택-purecvisorsd-자체-https-또는-선택형-nginx-외부-tls-종료)
- REST 인증과 endpoint: [REST API](/ko/interfaces/rest-api/)
- 현재 공개판 포함·제외 기준: [PUBLIC_RELEASE_BOUNDARY.md](../docs/PUBLIC_RELEASE_BOUNDARY.md)

<span id="121-검증-문서-맵" aria-hidden="true"></span>

#### 1.2.6 검증 문서 맵

문서 역할은 다음처럼 나눠서 봐야 합니다.

| 문서 | 역할 |
|------|------|
| [DEVELOPMENT_VERIFICATION_POLICY.md](../docs/DEVELOPMENT_VERIFICATION_POLICY.md) | 개발 단계별 검증 규칙, Level 1~4 운영 기준 |
| [ADR_INDEX.md](../docs/ADR_INDEX.md) | ADR별 현재 Single Edge 적용 상태 |
| `docs/adr/` | 설계 결정과 예외 규칙의 단일 진실 |
| [DATABASE_STRUCTURE.md](../docs/DATABASE_STRUCTURE.md) | SQLite DB 9개와 영구 테이블 28개의 책임, schema와 복구 경계 |
| [PUBLIC_SOURCE_POLICY.md](../docs/PUBLIC_SOURCE_POLICY.md) | 공개 소스 주석 제거와 소스맵 제외 정책 |

<span id="122-설계-결정-빠른-보기" aria-hidden="true"></span>

#### 1.2.7 설계 결정 빠른 보기

운영 가이드 본문에서 `ADR-0023`처럼 표시되는 항목은 단순 참고 문구가 아니라 기능의 허용 조건과 예외 규칙이다.<br>
통합 `ui/docs.html` reader는 같은 릴리스의 본문과 ADR 참조를 빠짐없이 표시하며, 설계 결정의 정본은 `docs/ADR_INDEX.md`와 `docs/adr/`에서 확인한다.

| 설계 결정 | 먼저 봐야 하는 상황 | 현재 Single Edge 결론 |
|-----------|--------------------|-----------------------|
| [fork 금지 단일 데몬](../docs/adr/0001-no-fork-single-daemon.md) | 프로세스, event loop, worker lifecycle 변경 | `purecvisorsd` 단일 프로세스와 GMainLoop 소유권을 유지하고 긴 작업만 bounded worker로 보낸다 |
| [비동기 결과 채널](../docs/adr/0012-async-result-channel.md) | Job ID, polling, WebSocket 완료 경로 변경 | accepted 응답과 실제 완료 상태를 분리하고 Job ID 기반 결과 채널을 유지한다 |
| [REST/WS TLS 기본 활성](../docs/adr/0029-rest-ws-tls-always-on.md) | daemon 직접 HTTPS, 선택형 NGINX 외부 종료 변경 | 기본은 daemon 자체 TLS이며 외부 종료는 host-loopback 신뢰 경계의 명시적 opt-in으로만 허용한다 |
| [VM clone 오픈 베타 안전장치](../docs/adr/0023-vm-clone-beta-safety-guard.md) | VM clone, Guest reset, Prepared template, power on 거부 조건 | source VM은 `shut off` 상태여야 하며, prepared template 또는 libguestfs 기반 Guest reset 중 하나가 필요하다 |
| [VM 생성 저장 위치 계약](../docs/adr/0022-vm-create-storage-location-contract.md) | VM 생성 저장소, zvol/qcow2/raw 위치 정책 변경 | `storage_type`과 `storage_pool` 또는 `image_dir` 조합을 명시 계약으로 유지한다 |
| [RBAC UDS 우회 정책](../docs/adr/0019-rbac-uds-bypass-policy.md) | 권한, operator owner-scope, UDS/REST 보안 변경 | REST 인증과 dispatcher method policy를 모두 유지하고 `make check-rbac`로 검증한다 |
| [fire-and-forget audit 기록 정책](../docs/adr/0018-fire-and-forget-audit-policy.md) | 장시간 RPC, worker callback, audit/WS completion 변경 | accepted 응답은 완료가 아니며 worker callback에서 실제 결과 audit를 남긴다 |
| [JWT Bearer 전용 인증](../docs/adr/0014-remove-csrf-jwt-bearer.md) | REST 인증, CSRF, 브라우저 호출 모델 설명 | 쿠키 세션 대신 `Authorization: Bearer <JWT>`를 사용하고 별도 CSRF 토큰을 운영하지 않는다 |
| [프론트엔드 IIFE 모듈 스코프](../docs/adr/0013-frontend-iife-module-scope.md) | Web UI 모듈, endpoint registry, sanitizer 변경 | Vanilla JS를 유지하고 `PCV.*` 네임스페이스와 `EP` 레지스트리를 사용한다 |

### 1.3 Single Edge 공개 리포지토리

> 📦 **공개 배포 저장소** — LinkedIn/GitHub 공개 시에는 민감정보 정리가 끝난 HEAD를 `git archive`로 추출해 새 공개 저장소의 첫 커밋으로 올립니다. 기존 개발 저장소의 `.git` 이력은 공개 저장소에 가져가지 않습니다. 공개 저장소 주소는 `https://github.com/HardcoreMonk/purecvisor`입니다.
>
> - 공개 배포 기준 바이너리: `bin/purecvisorsd`
> - systemd 서비스: `purecvisorsd.service`
> - `/api/v1/health` 기준 런타임 모드: `capabilities`의 `cluster`는 `false`, `checks`의 `cluster` 항목은 `mode: single_edge`, `node_name`은 호스트 이름

이 가이드는 싱글 노드 운영과 출시 범위만 다룹니다.

#### 공개 범위 핵심

1. **독립 노드 운영**: `purecvisorsd` 하나로 VM, 컨테이너, 스토리지, 네트워크를 통합 관리합니다.
2. **수동/로컬 네트워크**: OVS 오버레이와 OVN 로컬 SDN 코어는 포함되지만, 클러스터 자동화는 포함되지 않습니다.
3. **출시 게이트 재인증 완료**: VM lifecycle, Storage, Network, Backup/Restore, Auth/RBAC, Single Edge OVN/OVS, Longrun 축이 다시 통과한 상태를 기준으로 정리합니다.

### 1.4 5분 퀵스타트

> **명령 표기**: 데몬에 요청을 보내는 `pcvctl` 명령과 데몬 소켓(`nc -U /var/run/purecvisor/daemon.sock`)을 쓰는 명령은 root 권한이 필요합니다. 데몬은 root(uid 0) 연결만 받고 소켓 디렉터리 권한도 `0700`입니다. 이 가이드의 예시는 대부분 `sudo`를 생략하므로 일반 사용자 계정에서는 명령 앞에 `sudo`를 붙입니다.

```bash
# 1. 서비스 시작
sudo systemctl start purecvisorsd   # Single Edge

# 2. 상태 확인
curl -s http://127.0.0.1:8080/api/v1/health | python3 -m json.tool
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
    "version": "2.2.0",
    "node_name": "purecvisor-edge-01"
}
```

제품 버전은 `2.2.0`, canonical Git/GitHub 릴리스 태그는 `v2.2.0`이다.<br>
소스 기준 단일 값은 `include/purecvisor/version.h`의 `PCV_PRODUCT_VERSION`이며, `/api/v1/health`, `/api/v1/version`, `pcvctl --version`, Prometheus `purecvisor_info`,
Web UI config와 HTML 정적 자산 query string은 같은 릴리스 단위로 맞춘다.<br>
`/api/v1` 같은 API path, OpenAPI spec version, Prometheus text format version, 라이브러리 ABI symbol은 제품 버전이 아니므로 별도 계약으로 유지한다.

```bash
# 3. bootstrap admin으로 첫 인증 토큰 발급
# 첫 설치에는 내장 기본 비밀번호가 없습니다.
# daemon.conf 또는 PURECVISOR_ADMIN_PASSWORD로 bootstrap 비밀번호를 먼저 설정합니다.
TOKEN=$(curl -s -X POST http://127.0.0.1:8080/api/v1/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<configured-admin-password>"}' | python3 -c "import sys,json;print(json.load(sys.stdin)['access_token'])")

echo "Token: $TOKEN"

# 4. VM 생성 (기본 NIC는 --qos_min_mbps/--qos_max_mbps 필수)
sudo pcvctl vm create web-prod --vcpu 2 --memory_mb 2048 --disk_size_gb 20 --qos_min_mbps 0 --qos_max_mbps 1000

# 5. VM 시작
sudo pcvctl vm start web-prod

# 6. VM 목록 확인
sudo pcvctl vm list

# 7. Web UI 접속
echo "http://127.0.0.1:8080/ui/ (admin / configured password)"
```

### 1.5 접속 정보 요약

| 인터페이스 | 주소 | 인증 |
|-----------|------|------|
| UDS 소켓 | `/var/run/purecvisor/daemon.sock` | root 전용 (SO_PEERCRED로 uid 0 확인, 비-root 연결 거부) |
| REST API | `http://127.0.0.1:8080/api/v1/` | JWT HS256 |
| HTTPS | `https://localhost:443/api/v1/` | JWT HS256 + TLS |
| Web UI | `http://127.0.0.1:8080/ui/` | bootstrap admin `admin / configured password` |
| WebSocket (이벤트) | `ws://127.0.0.1:8080/api/v1/ws/events` | JWT |
| WebSocket (VNC) | `ws://127.0.0.1:8080/api/v1/ws/vnc` | JWT |
| Prometheus | `http://127.0.0.1:8080/api/v1/metrics` | 없음 |
| Health | `http://127.0.0.1:8080/api/v1/health` | 없음 |

이벤트 WebSocket에서 데몬은 30초마다 `{"type":"ping"}`을 보내고, 60초 안에 `pong`이 없으면 연결을 닫습니다. Web UI는 `{"type":"pong"}`으로 답해 연결을 유지합니다. 직접 만든 클라이언트도 같이 답해야 합니다.

### 1.6 수동 RPC 테스트

UDS 소켓을 통해 직접 JSON-RPC 요청을 보낼 수 있습니다.

```bash
# VM 목록 조회
echo '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}' \
  | sudo nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 호스트·VM 메트릭 조회
echo '{"jsonrpc":"2.0","method":"monitor.fleet","params":{},"id":"1"}' \
  | sudo nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

---

## 2. 설치 및 환경 구성

### 2.1 솔루션 권장사항 — Ubuntu 26.04.1 LTS

PureCVisor 2.2.0 Single Edge의 공개 설치 기준은 Ubuntu Server 26.04.1 LTS(Resolute Raccoon) `amd64`입니다.
Ubuntu 26.04 LTS는 2031년 4월까지 표준 보안 업데이트와 중요 수정이 제공되며, 26.04.1 설치 이미지는 26.04 출시 이후의 설치·기본 플랫폼 수정 사항을 포함합니다.

> **설치 기준**
>
> - 새 노드는 [Ubuntu 26.04.1 Server 이미지](https://releases.ubuntu.com/26.04.1/)로 설치합니다.
> - 설치 전에 [Ubuntu 26.04 LTS 릴리스 노트](https://documentation.ubuntu.com/release-notes/26.04/)와 26.04.1 변경 사항을 확인합니다.

#### 권장 배포 프로파일

| 항목 | 기능 확인용 최소 구성 | 운영 권장 구성 | 검증 기준 노드 |
|---|---|---|---|
| CPU | 8코어, VT-x/AMD-V | 16코어 이상, IOMMU | Intel Xeon E5-2697 v4 2소켓, 36코어/72스레드, VT-x·IOMMU |
| RAM | 32GiB | 64GiB 이상, ZFS 사용 시 ECC 권장 | 128GB급, OS 인식 121GiB |
| 시스템 디스크 | 256GB SSD | OS 전용 SSD 또는 mirror | 2TB NVMe의 ZFS root pool |
| VM 데이터 | 512GB SSD | 별도 1TB 이상 NVMe mirror와 외부 백업 | 단일 2TB NVMe 공유, 검증용이며 디스크 장애 이중화 없음 |
| 관리 네트워크 | 1GbE | 고정 주소 1GbE 이상 | 1GbE 관리 NIC, 설치자가 확인한 미사용 IPv4 |
| 게스트·스토리지 네트워크 | 관리 NIC와 공유 | 별도 10GbE 이상 | 10GbE 전용 NIC, 호스트 L3 주소 없음 |

최소 구성은 제품 기능 확인용이며 동시에 실행할 VM의 vCPU·메모리·디스크를 포함하지 않습니다.
운영 노드는 호스트와 `purecvisorsd`용으로 최소 4 vCPU와 8GiB RAM을 남기고 나머지를 VM 할당 한도로 잡습니다.
VM 데이터 사용률은 80% 아래로 유지하고, snapshot과 같은 pool에만 있는 복사본을 백업으로 간주하지 않습니다.
검증 기준 노드의 단일 NVMe 구성은 재현·검증용이며 운영 장애 허용 구성이 아니므로, 운영 배포에서는 mirror와 별도 백업 대상을 사용합니다.

#### Ubuntu 26.04 소프트웨어 기준

| 구성 요소 | 26.04 기준 | 검증 기준 노드 확인값 |
|---|---|---|
| OS | Ubuntu Server 26.04.1 LTS `amd64` | Ubuntu 26.04.1 LTS |
| 커널 | Ubuntu generic 커널, cgroup v2 | `7.0.0-30-generic`, `cgroup2fs` |
| C compiler | GCC 14 이상, C23 `-std=gnu23` | GCC 14.3 |
| GLib | 2.88 계열 | 2.88.0 |
| libvirt | 12.0 계열 | 12.0.0 |
| QEMU/KVM | QEMU 10.2 계열, `/dev/kvm` | 10.2.1, KVM 검증 통과 |
| ZFS | 2.4 계열, 설치 커널과 같은 Ubuntu archive | 2.4.1 |
| OVS/OVN | 기능 사용 시 OVS 3.7·OVN 26.03 계열 | OVS 3.7.1·OVN 26.03.0 |

> **패키지 적용 원칙**
>
> - 표의 patch version은 2026-08-30 기준 검증값이며 고정 핀이 아닙니다.
> - `resolute-updates`와 `resolute-security`의 최신 호환 package를 함께 적용합니다.
> - Ubuntu 26.04의 HWE virtualization stack은 커널, libvirt, QEMU와 ZFS를 함께 업데이트한 뒤 재검증합니다.
> - Ubuntu 24.04 guest clone 검증 이력은 guest 호환 범위이며 host OS 기준을 24.04로 낮추지 않습니다.

#### 관리 IPv4 선정 기준

| 역할 | 값 | 운영 원칙 |
|---|---|---|
| 관리 NIC | `<management-interface>` | `ip -br link`에서 확인한 실제 이름, host L3와 default route를 유지하며 `bridge/dedicated`에 넣지 않음 |
| 관리 주소 | `<unused-management-ipv4>/<prefix-length>` | 관리망 대역 내 미사용 IPv4를 선택하고 router DHCP reservation 또는 정적 Netplan으로 고정 |
| gateway | `<default-gateway-ipv4>` | 기존 default route에서 확인하고 관리 주소와 같은 대역인지 검증 |
| DNS | `<dns-ipv4>` | 현재 resolver 정책을 유지하고 설치 전후 DNS 응답 확인 |
| 제품 HTTPS | `https://<management-ipv4>:443` | 기본 `purecvisorsd` 자체 HTTPS, NGINX 불필요 |
| 로컬 복구 HTTP | `http://127.0.0.1:8080` | loopback에서만 접근, 외부 공개 금지 |
| 기본 VM NAT | `pcvnat0`, `10.78.0.1/24` | 관리망·기존 libvirt/LXC/VPC 대역과 중복 금지 |
| 선택형 guest uplink | `<guest-uplink-interface>` | 주소·default route·master가 없음을 확인한 뒤에만 `bridge/dedicated` 후보로 사용 |

> **주소 선택 원칙**
>
> - 문서의 길이 표시는 그대로 복사할 예시 IP가 아닙니다.
> - 설치자는 router·DHCP 할당표와 현재 lease를 확인해 동적 pool 외부이거나 reservation으로 보호된 미사용 IPv4를 선택해야 합니다.

권장 방식은 현재 Netplan renderer와 DHCP를 유지하고 router에서 `<management-interface>`의 lease를 선택한 IPv4로 예약하는 것입니다.
이 방식은 host에서 주소를 중복 선언하지 않으면서 서비스 주소를 고정합니다.
router reservation을 사용할 수 없을 때만 아래 정적 Netplan 절차를 사용합니다.

### 2.2 솔루션 설치

#### 26.04 host 기본 준비

> **인증서 생성 전 확정**
>
> - hostname과 시간대를 확정합니다.
> - 관리 IPv4 또는 운영 DNS 이름을 확정합니다.
> - 이미 인증서를 운영 중인 노드의 hostname이나 관리 IP를 바꾸면 SAN과 접속 URL도 함께 갱신합니다.

```bash
sudo hostnamectl set-hostname purecvisor-edge-01
sudo timedatectl set-timezone Asia/Seoul

sudo apt update
sudo apt full-upgrade -y
sudo apt install -y ca-certificates curl git jq cpu-checker libvirt-clients libvirt-daemon-common

test "$(stat -fc %T /sys/fs/cgroup)" = cgroup2fs
kvm-ok
sudo virt-host-validate qemu
timedatectl status
```

커널, ZFS 또는 QEMU가 갱신됐다면 이 단계에서 한 번 재부팅하고 같은 검사를 다시 실행합니다.
`virt-host-validate`의 `/dev/kvm`, `/dev/vhost-net`, `/dev/net/tun`, IOMMU 항목은 `PASS`여야 합니다.
confidential guest 기능을 사용하지 않는 Single Edge 노드의 SEV/TDX 경고는 기능 미사용 상태로 기록할 수 있지만 KVM 또는 IOMMU 실패는 설치 전에 해결합니다.

#### 관리 주소 고정

권장 방식은 router에서 DHCP reservation을 설정한 뒤 host 설정을 그대로 유지하는 것입니다.
MAC 주소는 router 관리 화면에서 직접 확인하고 공개 문서나 이력에 기록하지 않습니다.

```bash
MGMT_NIC="<management-interface>"
NODE_IPV4="<unused-management-ipv4>"
PREFIX_LENGTH="<prefix-length>"
GATEWAY_IPV4="<default-gateway-ipv4>"
DNS_IPV4="<dns-ipv4>"

sudo netplan get
ip -4 address show dev "${MGMT_NIC}"
ip route show default
resolvectl status "${MGMT_NIC}"

# 재부팅 후에도 아래 세 값이 유지되는지 확인
# address: ${NODE_IPV4}/${PREFIX_LENGTH}
# default route: ${GATEWAY_IPV4} via ${MGMT_NIC}
# DNS server: ${DNS_IPV4}
```

DHCP reservation을 사용할 수 없는 환경에서는 console 또는 BMC 접속을 확보한 뒤 `/etc/netplan/60-purecvisor-mgmt.yaml`을 다음과 같이 만듭니다.
먼저 모든 `<...>` 값을 해당 노드의 실제 값으로 바꾸고, 기존 `sudo netplan get`에서 확인한 renderer 정책을 유지합니다.
다른 Netplan 파일이 `<management-interface>`를 함께 정의하면 먼저 중복을 제거합니다.

```yaml
network:
  version: 2
  ethernets:
    <management-interface>:
      dhcp4: false
      dhcp6: false
      addresses:
        - <unused-management-ipv4>/<prefix-length>
      routes:
        - to: default
          via: <default-gateway-ipv4>
      nameservers:
        addresses:
          - <dns-ipv4>
```

```bash
sudo chown root:root /etc/netplan/60-purecvisor-mgmt.yaml
sudo chmod 600 /etc/netplan/60-purecvisor-mgmt.yaml
sudo netplan generate
sudo netplan try --timeout 120

MGMT_NIC="<management-interface>"
GATEWAY_IPV4="<default-gateway-ipv4>"
ip -4 address show dev "${MGMT_NIC}"
ip route show default
resolvectl status "${MGMT_NIC}"
ping -c 3 "${GATEWAY_IPV4}"
```

SSH 연결만 있는 상태에서 `netplan apply`를 바로 실행하지 않습니다.
`netplan try` 확인에 실패하거나 시간이 끝나면 이전 설정으로 돌아가므로 console에서 원인을 수정합니다.

#### 방법 A — `.deb` 바이너리 패키지 (권장, Ubuntu 26.04)

릴리스 `.deb`(`purecvisor-single_<version>_amd64.deb`)로 데몬·CLI·UI·systemd 유닛을 한 번에 설치합니다.
런타임 의존성인 libvirt, QEMU, dnsmasq, nftables와 iproute2는 apt가 자동 해결하며 OVS, OVN, ZFS는 package `Recommends`로 설치됩니다.

```bash
# release 디렉터리에서 서명 또는 전달받은 SHA256SUMS를 먼저 검증
sha256sum -c SHA256SUMS --ignore-missing

# 의존성 자동 해결 포함 설치
sudo apt install -y ./purecvisor-single_2.2.0_amd64.deb

# 선택: 같은 릴리스의 BPF 객체(BPF LSM 감사)
sudo install -d -m 0755 /usr/lib/purecvisor/bpf
sudo tar -xzf purecvisor-single-bpf_2.2.0_amd64.tar.gz -C /usr/lib/purecvisor/bpf

# 최초 설치는 sample 기반 daemon.conf를 만들고 서비스는 enable만 한다.
sudoedit /etc/purecvisor/daemon.conf
sudo chown root:root /etc/purecvisor/daemon.conf
sudo chmod 600 /etc/purecvisor/daemon.conf

sudo systemctl enable --now libvirtd
sudo systemctl start purecvisorsd
systemctl --no-pager --full status purecvisorsd
pcvctl --version
```

- 설치 위치: 바이너리 `/usr/local/bin/{purecvisorsd,pcvctl}`, UI `/usr/local/share/purecvisor/ui/`, 유닛 `/etc/systemd/system/purecvisorsd.service`, 설정 `/etc/purecvisor/`.
- 기본 모드는 데몬 자체 HTTPS `:443`(모든 IPv4 주소에서 수신 — 관리 IPv4로 접속)과 loopback HTTP `127.0.0.1:8080`입니다.
- NGINX는 외부 TLS 종료가 필요한 경우에만 별도 설치하며 기본 의존성이 아닙니다.
- 업그레이드: 새 `.deb`로 같은 명령 재실행.<br>
기존 `daemon.conf`는 보존된다.<br>
(conffile을 묻지 않고 처리하려면 `-o Dpkg::Options::=--force-confold`(기존 유지) 또는 `-o Dpkg::Options::=--force-confnew`(새 파일 채택)를 붙인다).<br>
2.1.0에서 2.2.0으로 올릴 때는 같은 명령 뒤 `sudo systemctl restart purecvisorsd`로 실행 중인 데몬을 새 바이너리로 바꾼다(DB 스키마 이관 없음).<br>
2.0.4 이하에서 올릴 때는 먼저 `sudo systemctl stop purecvisorsd`로 정지하고 `/var/lib/purecvisor/*.db*`(WAL 포함)를 백업한 뒤 설치·시작하며, `CHANGELOG.md` 2.1.0의 '업그레이드 시 확인할 점'(DB 자동 이관·모든 사용자 한 번 재로그인·새 systemd 파일 두 개)도 함께 적용한다.<br>
패키지 설치는 실행 중인 데몬을 재시작하지 않는다.<br>
같은 버전을 다시 설치할 때는 `sudo apt install --reinstall ./purecvisor-single_<버전>_amd64.deb`를 사용한다.
- 제거: `sudo apt remove purecvisor-single`은 설정을 보존한다. `sudo apt purge purecvisor-single`은 자동 생성한 `/etc/purecvisor/daemon.conf`도 제거한다. DB·로그·PKI는 보존하므로 운영 데이터 정리는 별도로 판단한다. 이 동작은 이 수정이 들어간 패키지의 제거 스크립트 기준이며, 그 이전의 구형 패키지가 설치된 상태에서 제거하면 적용되지 않는다.

#### 방법 B — 소스 빌드

##### 빌드 의존성

```bash
sudo apt update && sudo apt install -y \
    build-essential gcc-14 make pkg-config ccache \
    libglib2.0-dev \
    libvirt-dev libvirt-clients libvirt-daemon-system qemu-system-x86 ovmf \
    libguestfs-tools \
    libsoup-3.0-dev libjson-glib-dev \
    libvirt-glib-1.0-dev lxc-dev \
    zfsutils-linux lxc \
    libsqlite3-dev libssl-dev \
    libcap-dev libseccomp-dev libreadline-dev liburing-dev \
    libbpf-dev libxml2-dev \
    protobuf-c-compiler libprotobuf-c-dev
```

> **참고**: `libvirt-glib-1.0-dev`가 `libvirt-gobject-1.0.pc`도 함께 제공합니다.<br>
별도의 `libvirt-gobject-1.0-dev` 패키지는 존재하지 않습니다.
> `libbpf-dev` 부재 시 eBPF 경로가 stub 으로 빌드되고(런타임 DEGRADED_NO_BTF 폴백),
> `libxml2-dev` 는 `libvirt-gconfig-1.0.pc` 의 전이 의존이라 없으면 pkg-config 해석 전체가
> 비어 `glib.h` 부터 못 찾는 것처럼 보일 수 있다.<br>
> 테스트 스위트 실행에는 추가로 `wireguard-tools`·`sqlite3`·`openvswitch-switch`·
> `python3-pytest`(게이트 자체테스트)가 필요하다.

> **운영 필수**: `libguestfs-tools`는 일반 VM 복제의 Guest reset 경로에 필요하다.<br>
이 패키지가 없으면 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish`를 실행할 수 없어 `guest_reset=true` clone이 preflight에서 거부된다.

#### 공통 런타임 의존성 (사용 기능별)

이 절의 런타임 구성은 `.deb` 패키지와 소스 빌드에 공통으로 적용합니다.
`.deb` 설치에서는 libvirt, QEMU, dnsmasq-base, nftables와 iproute2를 package `Depends`로 자동 설치합니다.
OVS, OVN과 ZFS는 package `Recommends`로 설치되며, `--no-install-recommends`를 사용했다면 필요한 기능의 패키지를 직접 설치합니다.
일반 VM 복제, LXC와 iSCSI initiator는 해당 기능을 사용할 때 명시적으로 런타임 패키지를 설치합니다.

> **ZFS는 선택형 런타임입니다 — LXC 컨테이너 생성에는 필수**
>
> PureCVisor 데몬과 Web UI, REST API, CLI는 `zvol_pool`이나 ZFS volume이 없어도 시작하고 동작합니다.
> VM 생성 요청에서 `storage_type`을 생략하면 설정된 ZFS dataset을 먼저 확인하고, 사용할 수 없으면 `[storage] image_dir`에 qcow2 파일 디스크를 생성합니다.
> ZFS를 사용하지 않는 노드는 `zfsutils-linux`를 생략할 수 있지만 `image_dir`이 존재하고 쓰기 가능해야 하며 `qemu-img`를 사용할 수 있어야 합니다.
> `storage_type=zvol`을 명시한 VM 생성, ZFS snapshot·rollback·send/receive 기반 backup과 현재 ZFS dataset을 사용하는 LXC 경로에는 ZFS가 필요합니다.

```bash
# 일반 VM 복제의 Guest reset
sudo apt install -y libguestfs-tools

# LXC 컨테이너
sudo apt install -y lxc

# 선택: ZFS zvol·snapshot·backup 또는 LXC를 사용하는 경우
sudo apt install -y zfsutils-linux

# OVS 오버레이 네트워크
sudo apt install -y openvswitch-switch
```

##### OVN Single Edge 구성

OVN을 사용하지 않고 OVS bridge만 사용하는 노드는 이 절을 건너뜁니다.
OVN Single Edge 구성은 NB DB, SB DB, `ovn-northd`와 `ovn-controller`를 같은 노드에 설치하고 로컬 OVS를 해당 Southbound DB에 연결합니다.
이 명령은 Multi Edge나 클러스터 제어면을 구성하지 않습니다.

`purecvisor-ovn-single`과 `scripts/install-ovn-single.sh`는 동일한 스크립트의 설치본과 저장소 원본입니다.
두 명령을 모두 실행하지 말고 PureCVisor를 설치한 방식에 맞는 한 경로만 선택합니다.

| PureCVisor 설치 방식 | 선택할 명령 | 실행 위치 |
|---|---|---|
| 릴리스 `.deb` 패키지 | `purecvisor-ovn-single` | 패키지가 `/usr/local/sbin/`에 설치하므로 어느 디렉터리에서나 실행 |
| Git 저장소 소스 빌드 | `scripts/install-ovn-single.sh` | PureCVisor 저장소 최상위 디렉터리에서 실행 |

일반 실행은 `openvswitch-switch`, `ovn-central`, `ovn-host`를 설치하고 관련 서비스를 활성화합니다.
이어서 OVS의 system ID, 로컬 Southbound DB socket, Geneve encap IPv4를 설정하고 NB/SB 동기화와 local chassis 등록이 끝날 때까지 최대 30초 동안 확인합니다.
성공 메시지가 출력되면 별도의 즉시 검증은 필요하지 않습니다.

`--encap-ip`에는 Geneve endpoint로 사용할, 이미 이 host에 할당된 IPv4를 지정합니다.
옵션을 생략하면 default route 조회 결과의 source IPv4를 자동 선택하므로 관리망과 overlay transport망이 분리된 환경에서는 명시적으로 지정합니다.

`.deb`로 설치한 노드는 다음 명령을 사용합니다.

```bash
sudo purecvisor-ovn-single --encap-ip <ovn-encap-ipv4>
```

소스 빌드 노드는 저장소 최상위 디렉터리에서 다음 명령을 사용합니다.

```bash
sudo scripts/install-ovn-single.sh --encap-ip <ovn-encap-ipv4>
```

`--verify-only`는 패키지 설치나 OVS 설정을 다시 수행하지 않습니다.
재부팅 후 또는 장애 점검 시 처음 선택한 경로에만 붙여 OVS·OVN 서비스, NB/SB 동기화, local chassis 등록 상태를 재검사합니다.

```bash
# .deb 설치 노드
sudo purecvisor-ovn-single --verify-only

# 소스 빌드 노드
sudo scripts/install-ovn-single.sh --verify-only
```

##### iSCSI 런타임

```bash
# iSCSI 스토리지 — 이니시에이터(클라이언트)만 패키지가 필요하다.
# 타겟(서버) 측은 커널 LIO(configfs)를 직접 쓰므로 `tgt` 패키지가 **불요**하다.
sudo apt install -y open-iscsi
```

##### 소스 배포의 LIO 모듈 설정

`.deb` 패키지는 `/etc/modules-load.d/purecvisor-lio.conf`를 conffile로 설치합니다.
소스 빌드 결과를 직접 배포하는 경우에만 다음 파일을 수동으로 배치합니다.

```bash
# iSCSI 타겟용 커널 모듈 로드 — deb 를 설치했다면 이미 깔려 있다(conffile).
# 아래 블록은 deb 를 쓰지 않는 배포(소스 빌드 + scp)에서만 필요하다.
# ⚠ 모듈명 행에 주석을 붙이지 말 것 — modules-load.d(5) 는 행 전체를 모듈명으로
#   읽어 4개 전부 'Failed to find module' 이 되는데도 유닛은 성공으로 끝난다.
sudo tee /etc/modules-load.d/purecvisor-lio.conf >/dev/null <<'EOF'
target_core_mod
iscsi_target_mod
target_core_iblock
nf_conntrack_bridge
EOF
sudo systemctl restart systemd-modules-load
```

#### 검증용 도구

```bash
# 런타임 전제 무접근 계약 게이트 필수
sudo apt install -y strace

# 정적 분석
sudo apt install -y cppcheck

# 메모리 검사
sudo apt install -y valgrind

# 커버리지 리포트
sudo apt install -y lcov
```

`make check-runtime-prereqs`는 `strace -f -e trace=%file`로 BPF
`--verify-only` 검증이 설치 root, config, PKI, JWT, 기존 BPF 목적지에 접근하지
않음을 확인한다. 이 보안 게이트에서는 `strace` 미설치를 skip으로 처리하지 않으며,
Ubuntu에서는 `sudo apt install strace`로 설치한 뒤 다시 실행한다.

### 2.3 빌드

```bash
git clone https://github.com/HardcoreMonk/purecvisor.git purecvisor-single
cd purecvisor-single
```

#### 빌드 타겟

| 명령 | 설명 | 출력 |
|------|------|------|
| `make single` | Single Edge 빌드 | `bin/purecvisorsd` |
| `make cli` | CLI 빌드 | `bin/pcvctl` |
| `make all` | 전체 빌드 | 데몬 + CLI |
| `make release` | 릴리스 빌드 (-O2, NDEBUG, 하드닝) | 최적화된 바이너리 |
| `make clean` | 빌드 산출물 정리 | - |

```bash
# 클린 빌드 (경고 0 확인)
make clean && make single

# 빌드 결과 확인
ls -lh bin/
# bin/purecvisorsd  (~1.9MB, Single Edge)
# bin/pcvctl        (CLI)
```

#### 품질 검증

```bash
# 전체 unit·integration test (root 단계는 운영 상태 경로를 빈 tmpfs로 가린 사설 mount 네임스페이스 안에서만 돈다)
make test

# 공개 경계와 계약 게이트 전체
make check-all

# 정적 분석
make cppcheck

# 메모리 누수 검사
make memcheck
# 같은 검사를 최상위 suite 단위로 나눠 동시에 실행(운영 호스트가 아닌 개발 환경용, 로그는 valgrind_shards/)
make memcheck-sharded MEMCHECK_JOBS=16

# 코드 커버리지 HTML 리포트
make coverage-html
# 결과: coverage_report/html/index.html

# CI용 TAP 형식 테스트 출력
make test-tap
```

### 2.4 디렉터리 구조 생성과 배포 런타임 전제

```bash
# 런타임 디렉터리
sudo install -d -m 0700 /etc/purecvisor/pki
sudo install -d -m 0755 /etc/purecvisor/plugins.d
sudo install -d -m 0755 /etc/purecvisor/seccomp
sudo install -d -m 0755 /var/lib/purecvisor
sudo install -d -m 0700 /var/run/purecvisor
sudo install -d -m 0750 /var/log/purecvisor
```

#### 스토리지 경로 확정

##### ZFS를 사용하지 않는 구성

ZFS volume은 서비스 시작의 필수 조건이 아닙니다.
ZFS를 사용하지 않는 노드는 `image_dir`을 준비하고 VM 생성 시 `storage_type=qcow2` 또는 `raw`를 지정하거나 자동 감지를 사용합니다.

```bash
sudo install -d -m 0711 /var/lib/libvirt/images
sudo test -d /var/lib/libvirt/images
sudo test -w /var/lib/libvirt/images
command -v qemu-img
```

```ini
[storage]
image_dir = /var/lib/libvirt/images
iso_dirs = /data/iso,/var/lib/libvirt/images
```

자동 감지에서 `zvol_pool`을 찾지 못하면 `image_dir`에 qcow2 디스크를 생성합니다.
`storage_type=zvol`을 명시하면 폴백하지 않고 지정한 ZFS 부모 dataset이 없다는 오류로 요청을 종료합니다.
따라서 ZFS 미사용 노드에서는 VM file disk를 사용할 수 있지만 ZFS snapshot·rollback·send/receive backup과 현재 LXC 생성 경로는 사용할 수 없습니다.

##### ZFS 기능을 사용하는 구성

ZFS 기능을 사용하는 경우에만 `zvol_pool`에 실제로 존재하는 ZFS dataset을 지정합니다.
설치 전에 ZFS zvol 또는 file image 중 주 저장 방식을 결정하고 설정과 실제 경로를 일치시킵니다.

Ubuntu가 `rpool` 단일 NVMe에 설치된 검증 노드는 새 설치일 때만 다음 전용 dataset을 만들 수 있습니다.
기존 VM이 있는 노드에서는 이 명령으로 경로를 바꾸지 말고 별도 migration과 rollback 계획을 사용합니다.

```bash
sudo zpool status
sudo zfs list

# 새 설치 전용 예시
sudo zfs create -o mountpoint=none -o canmount=off rpool/data/purecvisor
sudo zfs create -o mountpoint=none -o canmount=off -o compression=lz4 \
  rpool/data/purecvisor/vms
sudo install -d -m 0711 /var/lib/libvirt/images
sudo install -d -m 0755 /data/iso
```

```ini
[storage]
zvol_pool = rpool/data/purecvisor/vms
image_dir = /var/lib/libvirt/images
iso_dirs = /data/iso,/var/lib/libvirt/images
```

```bash
sudo zfs list rpool/data/purecvisor/vms
sudo test -d /var/lib/libvirt/images
sudo test -d /data/iso
```

운영 권장 구성은 OS `rpool`과 분리된 mirror data pool을 만들고 `zvol_pool=pcvpool/vms`를 사용하는 방식입니다.
단일 디스크 `rpool` 예시는 검증 환경 재현용이며 디스크 장애에 대한 중복성을 제공하지 않습니다.

#### 배포 런타임 전제와 보존 정책

기본 `scripts/deploy.sh`는 `make release` 뒤 `make bpf`를 실행한다. `--skip-build`
에서도 daemon/CLI, BPF object, manifest 존재 여부와 helper의 `--verify-only` 검사를
SSH/SCP보다 앞선 로컬 preflight에서 수행하며, 실패하면 원격 상태를 바꾸기 전에
배포를 중단한다. `--verify-only`는 BPF staging만 검증하는 전용 경로이며 설치 대상
root, `daemon.conf`, PKI, JWT, 기존 BPF 목적지에는 접근하지 않는다.

원격 설치 helper의 계약은 다음과 같다.

- 최초 배포는 `/etc/purecvisor/pki`를 mode `0700`으로 준비한다. helper는 TLS
  인증서 내용을 만들지 않으며, 기존 cert/key를 덮어쓰지 않고 보존한다. 인증서와
  키가 모두 없을 때의 자가서명 생성은 데몬의 TLS 초기화가 담당한다.
- 배포 helper가 관리하는 값은 `[daemon] jwt_secret`이다. 키가 누락됐거나 빈 값이면
  Python CSPRNG `secrets.token_hex(32)`로 64자리 소문자 hex를 만들어
  `/etc/purecvisor/daemon.conf`에 영속화하고 파일 mode를 `0600`으로 고정한다.
  충분한 길이인 32바이트 이상의 기존 JWT와 인증서는 내용 그대로 보존한다.
- 비어 있지 않은 기존 JWT가 32바이트 미만이거나 `placeholder`류 또는 반복 문자처럼
  명백히 weak하면 자동 회전으로 예고 없이 기존 토큰을 무효화하지 않는다. 배포를
  fail-closed로 거부하고 운영자가 값을 명시적으로 교체하도록 요구한다.
- BPF 배포는 `manifest.json`의 schema와 SHA-256을 검증하고 성공한 object와
  manifest만 `/usr/lib/purecvisor/bpf`에 설치한다. 디렉터리는 `0755`, 파일은
  `0644`이며 manifest를 마지막 commit marker로 교체한다.
- 원자적 교체의 임시 파일은 대상과 같은 디렉터리 안의 고정 이름 `.pcv-install-<대상 이름>`이다
  (예: `.pcv-install-manifest.json`, `.pcv-install-daemon.conf`). 이름이 반영마다 같으므로 LSM 감시
  경로 아래의 보안 사건 키가 반영 횟수만큼 늘지 않는다. helper는 SIGTERM·SIGINT에서 임시 파일을
  지우지만, SIGKILL·연결 끊김(SIGHUP)처럼 비정상으로 끝나 임시 파일이 남으면 다음 설치는 그 파일을
  지우지 않고 멈춘다. BPF 쪽은 `error: BPF destination contains an unsafe entry`(이름 없음 —
  `/usr/lib/purecvisor/bpf/.pcv-*`를 확인)로, 설정 쪽(JWT를 만드는 설치에서만 임시 파일을 쓴다)은
  `error: stale atomic runtime temp file exists: .pcv-install-daemon.conf`로 멈춘다. 남은 파일이
  앞선 설치의 잔재인지 확인한 뒤 지우고 다시 실행한다.
- 원격 staging은 추측 불가능한 이름과 mode `0700`으로 만들고, helper 실패 시
  서비스를 시작하지 않는다. 성공과 실패 모두 EXIT 경로에서 helper와 BPF staging을
  cleanup하여 정리한다.
- 이 배포는 커널 `lsm=bpf` 활성화나 커널 명령행 변경, 재부팅을 자동으로 수행하지
  않는다. BPF LSM이 필요하면 별도 승인된 운영 절차로 변경하고 재부팅해야 한다.

JWT 로드에는 두 경로가 공존한다. 데몬 시작 시
`PCV_SECRET_AUTH_JWT_SECRET`과 정확한 `[auth] jwt_secret`을 먼저 읽는다. 둘 다
없으면 `PURECVISOR_JWT_SECRET`, `[daemon] jwt_secret`을 호환 fallback으로 찾고,
마지막으로 다른 섹션을 검색한다. 따라서 deploy helper의 `[daemon] jwt_secret` 영속화가
모든 runtime override의 단일 정본이라는 뜻은 아니다. `[auth]` 또는 환경변수 override를
운영자가 사용한다면 32바이트 이상 값을 별도로 보장해야 한다.

<!-- PCV-NGINX-OPERATIONS:BEGIN -->
#### TLS 배포 모드 선택: `purecvisorsd` 자체 HTTPS 또는 선택형 NGINX 외부 TLS 종료

이 절은 제품 노드의 REST·Web UI·WebSocket 진입 경계를 설명한다. 공개 문서 도메인
`purecvisor.site`는 GitHub Pages가 정적 파일과 HTTPS를 제공하므로 아래 제품 노드 NGINX
구성과 무관하다. 제품 노드는 다음 두 모드 중 하나를 명시적으로 사용한다. NGINX는 필수
의존성이 아니라 외부 TLS 종료를 선택한 노드에서만 필수인 구성 요소다.

| 항목 | `purecvisorsd` 자체 HTTPS | 선택형 NGINX 외부 TLS 종료 |
|---|---|---|
| 외부 TLS 소유자 | `purecvisorsd` | NGINX |
| NGINX 필요 여부 | 불필요 | 필수 |
| 외부 진입 | `https://<node>:443` → 데몬 | `https://<node>:443` → NGINX → 데몬 |
| 데몬 평문 리스너 | `127.0.0.1:<rest_port>` 복구·로컬 점검용 | `127.0.0.1:8080` 프록시 upstream 전용 |
| 핵심 설정 | `https_enabled=true`, `https_port=443` | `https_enabled=false`, `bind_plaintext=loopback` |
| 권장 용도 | 별도 proxy 운영이 필요 없는 단순한 단일 노드 | 공인 인증서, 중앙 보안 헤더·rate limit, WebSocket proxy, maintenance fallback이 필요한 환경 |
| 정상 TLS health | `mode=internal`, `enabled=true`, `degraded=false`, `status=ok` | `mode=external_termination`, `enabled=false`, `degraded=false`, `status=disabled_by_config` |

```text
# 모드 A — purecvisorsd 자체 HTTPS
클라이언트 ── HTTPS :443 ──> purecvisorsd
                              └─ HTTP 127.0.0.1:<rest_port> (로컬 복구·점검)

# 모드 B — 선택형 NGINX 외부 TLS 종료
클라이언트 ── HTTPS :443 ──> NGINX ── HTTP 127.0.0.1:8080 ──> purecvisorsd
```

##### 모드 A — `purecvisorsd` 자체 HTTPS

자체 HTTPS는 기본 전송 모드다. 데몬이 인증서를 로드하고 외부 HTTPS listener를 직접
소유한다. `[server] bind_plaintext=loopback`을 유지하면 평문 REST는 로컬 복구와 점검에만
사용되고 원격 자격증명·JWT·API key가 HTTP로 노출되지 않는다.

```ini
[tls]
https_enabled = true
https_port = 443
cert = /etc/purecvisor/pki/node.crt
key = /etc/purecvisor/pki/node.key

[server]
bind_plaintext = loopback
```

- cert와 key가 모두 없으면 데몬이 자체서명 쌍을 원자적으로 생성한다. 둘 중 하나만 있으면
  사용자 자산을 덮어쓰지 않고 TLS 초기화를 실패 처리한다.
- 운영자가 제공한 cert/key 쌍은 자동 생성보다 우선하며 배포 helper가 내용을 교체하지 않는다.
- TLS 초기화나 HTTPS bind가 실패하면 외부 평문으로 확장하지 않는다. 데몬은 루프백 HTTP와
  UDS만 남긴 `degraded` 상태로 기동해 복구 경로는 보존하고 외부 표면은 닫는다.
- 이 모드에서는 NGINX service, NGINX vhost와 `PCV_NGINX_BIND_IP`가 필요하지 않다.

```bash
NODE_IPV4="<configured-management-ipv4>"
systemctl is-active purecvisorsd
curl -ksS "https://${NODE_IPV4}/api/v1/health" | jq '{status, tls: .checks.tls}'
sudo ss -lntp | grep -E '127\.0\.0\.1:8080|0\.0\.0\.0:443'
# 기대: mode=internal, enabled=true, degraded=false, status=ok
```

##### 모드 B — 선택형 NGINX 외부 TLS 종료

외부 종료 모드에서는 NGINX가 인증서와 `:443`을 소유하고 REST·Web UI·WebSocket 요청을
데몬의 루프백 HTTP로 전달한다. 데몬 자체 HTTPS는 의도적으로 끄되 평문 upstream은 반드시
loopback으로 제한한다.

```ini
[daemon]
rest_port = 8080

[tls]
https_enabled = false

[server]
bind_plaintext = loopback
```

NGINX vhost는 최소한 TLS 인증서, HTTP→HTTPS 정책, `proxy_pass http://127.0.0.1:8080`,
WebSocket Upgrade, 보안 헤더와 forwarded header 덮어쓰기, server 블록 수준의 공유 비밀 조각
`include /etc/nginx/snippets/purecvisor-proxy-auth.conf;`를 포함해야 한다. 2.1.0 전에 만든
vhost는 템플릿에서 왔든 손으로 만들었든 이 include가 없으므로 배포 전에 더한다. vhost와 인증서를
먼저 프로비저닝한 다음 opt-in 배포를 실행한다. 배포는 `--commit` 전에 `nginx -T`에서 이
include(또는 `proxy_set_header X-PCV-Proxy-Auth`)를 찾지 못하면 실패하고 nginx 트랜잭션을 롤백한다.

```bash
PCV_NODES=<ip> \
PCV_NGINX_BIND_IP=<ip> \
scripts/deploy.sh --no-local

sudo nginx -t
systemctl is-active nginx purecvisorsd
curl -ksS https://<ip>/api/v1/health | jq '.checks.tls'
# 기대: mode=external_termination, enabled=false, degraded=false,
#       status=disabled_by_config
```

`enabled=false`는 외부 통신이 평문이라는 뜻이 아니라 데몬 대신 NGINX가 TLS를 정상 종료한다는
뜻이다. `X-Forwarded-For`와 `X-Forwarded-Proto`는 연결 peer가 `[server] trusted_proxies`(기본
`127.0.0.1`)에 있고, `proxy_auth_secret`이 설정됐으면 `X-PCV-Proxy-Auth`가 맞을 때만 신뢰한다.

##### 선택·전환 시 금지 조합

- `https_enabled=false`인데 정상 NGINX listener가 없으면 외부 HTTPS 진입점이 사라진다.
- 외부 종료 모드에서 `bind_plaintext=all`을 쓰거나 값을 누락하면 데몬이 시작을 거부한다.
- 같은 IP의 `:443`을 NGINX와 `purecvisorsd`가 동시에 소유하도록 구성하지 않는다.
- `PCV_NGINX_BIND_IP`를 다음 배포에서 생략해도 기존 외부 종료 설정이 자체 HTTPS로 자동 복귀하지
  않는다. 이 환경변수가 없으면 배포 스크립트는 기존 NGINX·daemon 전송 설정을 보존한다.
- 두 모드 전환은 `:443` 소유자가 바뀌는 maintenance 작업이다. 기존 설정·인증서를 백업하고
  새 listener와 health를 검증할 rollback 가능한 순서로 수행한다.
- `PCV-NGINX-TRUST-BOUNDARY: host-loopback`을 만족하지 못해 신뢰하지 않는 로컬 프로세스가
  loopback upstream에 접근할 수 있는 호스트에서는 NGINX 외부 종료 모드를 활성화하지 않는다.
  공유 비밀이 설정된 호스트의 위조 범위는 아래 신뢰 경계 문단을 본다.

**선택형 NGINX 외부 TLS 종료 설치와 복구**

`PCV_NGINX_BIND_IP`는 명시적으로 설정한 경우에만 활성화되는 opt-in이다. 값이
없으면 nginx 설정, systemd drop-in, `daemon.conf`의 전송 모드를 바꾸지 않는다.
활성화하면 nginx가 지정한 LAN 주소의 외부 TLS를 종료하고, `purecvisorsd`는
`127.0.0.1` 루프백 HTTP만 수신한다.

로컬 노드 배포:

```bash
NODE_IPV4="<configured-management-ipv4>"
PCV_NODES="" \
PCV_NGINX_BIND_IP="${NODE_IPV4}" \
scripts/deploy.sh --nodes local
```

원격 노드 배포(ssh 경유, `<ip>` 를 대상 노드로):

```bash
PCV_NODES=<ip> \
PCV_NGINX_BIND_IP=<ip> \
scripts/deploy.sh --no-local
```

> nginx vhost 자체는 이 트랜잭션의 관리 대상이 아니다 — 도메인 노드는
> `ops/nginx/purecvisor.example.com`, LAN IP 노드는
> `ops/nginx/purecvisor-lan-ip.conf.template`(+ `purecvisor-common.conf` http 전제)를
> 먼저 프로비저닝한다. 2.1.0 전에 프로비저닝한
> vhost에는 공유 비밀 조각 include가 없다 — server 블록에 `include /etc/nginx/snippets/purecvisor-proxy-auth.conf;`를
> 먼저 더한다(위 점검 목록).

설치 트랜잭션은 기존 인증서(cert)와 개인 키(key)를 내용 그대로 보존하고, 네 대상(nginx
systemd drop-in·`wait-for-local-ip` helper·daemon 설정·공유 비밀 조각)의 이전 상태를
백업한다. 설치 후 LAN health 검증이나 include 확인이 실패하면 `rollback`으로 이 네 대상을
함께 복원하며 보존된 인증서와 키를 삭제하거나 재생성하지 않는다. rollback 자체가 실패하면 서비스를 추측 상태로
재시작하지 않고 복구 자료를 남긴다.

부팅 시 `wait-for-local-ip`가 주소 준비를 기다린다. 제한 시간 안에 IP가 없으면
`exit 75`로 끝나 systemd가 재시도한다. 반면 `nginx -t` 문법 오류는 `exit 1`이며
`RestartPreventExitStatus=1`로 재시작 방지한다. 패키지 업데이트 뒤에는 vendor
유닛의 `ExecStartPre`가 정확히
`/usr/sbin/nginx -t -q -g 'daemon on; master_process on;'`인지 확인하고,
drop-in이 IP 대기와 이 검사를 모두 유지하는지 재검증한다.

Vendor 명령이 불일치하면 설치기는 변경 전에 실패한다.
이 실패는 기존 상태를 변경하지 않고 보존한다.
Vendor 계약은 자동 동기화하지 않는다.
재시도 전에 installer 계약과 테스트를 갱신하고 재검토한다.

정상 health의 전체 서비스 `status=ok`와 TLS check는 정확히
`mode=external_termination`, `enabled=false`, `degraded=false`,
`status=disabled_by_config`여야 한다. 이 값은 TLS가 꺼져 노출됐다는 뜻이 아니라
외부 TLS 종료가 정상 선택됐다는 뜻이다. `X-Forwarded-For`와
`X-Forwarded-Proto` 같은 프록시 헤더는 연결 peer가 `[server] trusted_proxies`(기본
`127.0.0.1`)에 있고, 공유 비밀(`proxy_auth_secret`)이 설정된 경우 `X-PCV-Proxy-Auth`가
일치할 때만 신뢰하며, 그 밖의 클라이언트가 보낸 같은 헤더는 신원·scheme 판정에
사용하지 않는다.

`PCV-NGINX-TRUST-BOUNDARY: host-loopback`은 이 모드가 호스트의 모든 루프백
프로세스를 privileged/trusted host boundary로 신뢰한다는 뜻이다.
`PCV-NGINX-COUNTERFACTUAL: untrusted-local-process`처럼 신뢰하지 않는 로컬
사용자나 프로세스가 daemon의 루프백 HTTP에 직접 접속할 수 있으면 전달 헤더를
위조할 수 있으므로 이 모드를 활성화하지 않는다. 전용 호스트·최소 사용자,
서비스 sandbox/MAC, 로컬 방화벽 또는 network namespace로 직접 루프백 접근을
제한한다. 원격 클라이언트는 loopback peer가 아니고 nginx가 전달 헤더를
덮어쓰므로 이 신뢰 경계를 직접 위조할 수 없다.
`proxy_auth_secret`이 설정되면(설치기 재실행 뒤) 데몬은 `X-PCV-Proxy-Auth`가 맞는 요청에서만 전달 헤더를 믿으므로, 위조할 수 있는 로컬 주체는 root와 nginx 워커 uid로 좁혀진다.

**관리면 노출 — 공유 비밀·신뢰 프록시·업그레이드 순서**

- nginx 외부 종료 설치기(`scripts/install-nginx-termination.sh`)는 `/etc/nginx/snippets/purecvisor-proxy-auth.conf`와 `[server] proxy_auth_secret`을 같은 비밀로 함께 쓴다. 데몬은 이 비밀이 맞는 `X-PCV-Proxy-Auth` 헤더가 붙은 요청에서만 전달 헤더(`X-Real-IP` 등)를 믿는다. 2.1.0 전에 만든 vhost는 템플릿에서 왔든 손으로 만들었든 모두 include가 없으므로 server 블록에 `include /etc/nginx/snippets/purecvisor-proxy-auth.conf;` 한 줄을 더한다(include가 있는데 조각이 없으면 `nginx -t`가 실패해 드러나지만, include가 없으면 `nginx -t`는 통과하고 nginx 뒤 모든 클라이언트가 `127.0.0.1` 하나로 합쳐진다).
- 믿을 프록시는 `[server] trusted_proxies`(쉼표, IP 또는 CIDR, 기본 `127.0.0.1`)로 정한다. `127.0.0.2`·`::1`에서 오는 프록시는 목록에 적어야 한다. 대역은 `127.0.0.1/8`이 아니라 `127.0.0.0/8`로 쓴다. 환경 변수 `PCV_SECRET_SERVER_PROXY_AUTH_SECRET`로 비밀을 줄 때는 nginx 조각과 같은 값이어야 한다.
- 업그레이드 순서는 두 경우다. (a) 이미 nginx 종단인 설치: 기존 vhost 전부에 include 추가(이 릴리스 전에 만든 vhost는 템플릿에서 왔든 손으로 만들었든 모두 없다) → 설치기 재실행 → `nginx -t && systemctl reload nginx` → 데몬 재시작. 조각은 설치기가 만들므로 include를 더한 뒤 설치기를 다시 실행할 때까지 nginx를 reload·restart하지 않는다(`nginx -t` 실패). (b) 자체 HTTPS(INTERNAL)에서 처음 nginx로 옮길 때: 설치기 → 데몬 재시작(`:443` 해제) → nginx reload, 끝에 `ss -ltn`으로 `<BIND_IP>:443` 리스너를 확인한다. 데몬이 `:443`을 쥔 채 reload하면 `systemctl reload`는 성공(rc 0)을 돌려주지만 nginx는 bind에 실패하고 `/var/log/nginx/error.log`에만 남는다.
- 설치기 `--rollback`과, 새 설치가 남은 pending 트랜잭션을 자동 복원할 때 모두 드롭인을 되돌린 뒤 `systemctl daemon-reload`를 한다.
- `scripts/deploy.sh`(`PCV_NGINX_BIND_IP`, 원격·로컬)는 health 확인 뒤 `--commit` 전에 `nginx -T` 출력에서 `include /etc/nginx/snippets/purecvisor-proxy-auth.conf;` 줄이나 `proxy_set_header X-PCV-Proxy-Auth` 줄을 찾고, 없으면 commit하지 않고 실패해 nginx 트랜잭션을 롤백한다(조각이 지워지고 include 없는 vhost는 그대로라 롤백은 깨끗하다). 확인은 설정 어딘가에 있는지까지다 — vhost마다 server 블록에 있는지는 보지 않는다.
- systemd 샌드박스 드롭인 `97-sandbox.conf`(`NotifyAccess=main`, `SystemCallArchitectures=native`, `LockPersonality=yes`, `RestrictRealtime=yes`, `SystemCallFilter=~@swap @obsolete @cpu-emulation @module @raw-io`)는 2.1.0 패키지가 `/etc/systemd/system/purecvisorsd.service.d/`에 설치한다. 컨테이너는 전용 유닛(4.2)으로 시작해 이 필터를 물려받지 않지만, `container.exec`와 헬스 `exec` 프로브는 `lxc-attach`로 실행되어 물려받으므로 그 안의 32비트 바이너리는 SIGSYS로, `setarch i686`은 EPERM으로 실패한다. 필요하면 `/etc/systemd/system/purecvisorsd.service.d/97-sandbox.conf`를 지우고 `systemctl daemon-reload` 뒤 데몬을 재시작해 원복한다.
<!-- PCV-NGINX-OPERATIONS:END -->

### 2.5 systemd 서비스 설치

```bash
sudo cp packaging/deb/purecvisorsd.service /etc/systemd/system/
# 컨테이너 시작 유닛과 서비스 샌드박스 드롭인 (deb 설치는 자동)
sudo install -m 0644 packaging/systemd/purecvisor-lxc@.service /etc/systemd/system/
sudo install -D -m 0644 packaging/systemd/97-sandbox.conf /etc/systemd/system/purecvisorsd.service.d/97-sandbox.conf
sudo systemctl daemon-reload
sudo systemctl enable --now purecvisorsd
```

서비스 상태 확인:

```bash
sudo systemctl status purecvisorsd

# 로그 확인 (실시간)
journalctl -u purecvisorsd -f
```

설정한 관리 IPv4에서 기본 모드가 정상 기동하면 journal에 다음 listener가 기록됩니다.

```
HTTPS listening on https://0.0.0.0:443 (HSTS enabled)
HTTP/2 support enabled (TLS via ALPN negotiation)
REST API listening on http://127.0.0.1:8080 + https://0.0.0.0:443/api/v1
```

외부 평문 `0.0.0.0:8080`이 보이면 정상 기준이 아닙니다.
`ss -lntp`와 `[server] bind_plaintext=loopback`을 확인한 뒤 서비스를 다시 시작합니다.

### 2.6 daemon.conf 설정

설정 파일 경로: `/etc/purecvisor/daemon.conf`

설정 우선순위: **환경 변수 > daemon.conf > 컴파일 기본값**

REST `rest_port`의 컴파일 기본값은 `80`입니다.
아래 Single Edge 권장 예시는 `8080`을 loopback 복구 listener로 두고 `purecvisorsd`가 설정한 관리 IPv4의 `443` HTTPS를 직접 소유합니다.
NGINX 외부 TLS 종료를 선택한 경우에만 같은 `127.0.0.1:8080`을 proxy upstream으로 사용합니다.

```ini
# /etc/purecvisor/daemon.conf
# Single Edge 권장 baseline

# ─────────────────────────────────────────────
# [daemon] 데몬 기본 설정
# ─────────────────────────────────────────────
[daemon]
# UDS 소켓 경로
socket_path = /var/run/purecvisor/daemon.sock

# REST API 포트 (1-65535, CAP_NET_BIND_SERVICE 필요)
rest_port = 8080
# 자체 HTTPS에서는 loopback 복구 listener, 선택형 NGINX에서는 proxy upstream

# 첫 로그인 전에 12자 이상의 임시 bootstrap 비밀번호를 설정
# 전용 admin 생성 뒤에는 빈 값으로 바꾸고 재기동해 bootstrap 계정을 비활성화할 수 있음
admin_user = admin
admin_password = <운영자가 sudoedit로 설정>

# 그레이스풀 드레인 타임아웃 (초, 최소 5)
drain_timeout = 30

# 워커 스레드 풀 크기 (1-64)
pool_max_conn = 16

# 배포 helper가 누락/빈 값만 64자리 hex로 영속화
jwt_secret =

# ─────────────────────────────────────────────
# [storage] 스토리지 설정
# ─────────────────────────────────────────────
[storage]
# ZFS zvol 풀 경로
zvol_pool = rpool/data/purecvisor/vms

# qcow2/raw 이미지 폴백 디렉터리
image_dir = /var/lib/libvirt/images

# ISO 스캔 디렉터리 (콤마 구분, .iso/.img 파일)
iso_dirs = /data/iso,/var/lib/libvirt/images

# ─────────────────────────────────────────────
# [tls] HTTPS 설정
# ─────────────────────────────────────────────
[tls]
# 기본 true. false는 nginx 외부 TLS 종료 + server.bind_plaintext=loopback에서만 허용
https_enabled = true
# 인증서와 키가 모두 없으면 이 기본 경로에 자가서명 쌍을 생성
cert = /etc/purecvisor/pki/node.crt
key = /etc/purecvisor/pki/node.key

# 선택적 client mTLS 신뢰 CA
ca = /etc/purecvisor/pki/ca.crt

# https_enabled=false일 때 반드시 loopback을 명시
[server]
bind_plaintext = loopback

# ─────────────────────────────────────────────
# [auth] 인증 설정
# ─────────────────────────────────────────────
[auth]
# 선택적 JWT runtime override. PCV_SECRET_AUTH_JWT_SECRET이 이 값보다 우선한다.
# 미설정 시 위 [daemon] jwt_secret 호환 fallback을 사용한다.
# jwt_secret = <32바이트 이상의 운영자 관리 값>

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

# 동일 경보 중복 억제 기간 (초)
dedup_window = 300

# Webhook URL. Slack·Telegram 형식은 URL 자체가 자격 증명이다. ADMIN이 아닌 조회와 로그에는 scheme+host만 나간다
webhook_url = https://hooks.slack.com/services/T00/B00/xxx
webhook_crit_url = https://events.example.com/purecvisor/critical

# HMAC 서명 비밀키. 평문 대신 ENC: 값 또는 PCV_SECRET_ALERT_WEBHOOK_SECRET 사용을 권장
webhook_secret = ENC:...

# Webhook 포맷: slack | telegram | generic
webhook_format = slack
telegram_chat_id =

# ─────────────────────────────────────────────
# [network] 관리형 기본 네트워크
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

# 호스트 방화벽(UFW/iptables FORWARD DROP) 자동 공존. "auto"면 UFW/iptables FORWARD DROP
# 호스트에 모드별 최소 구멍(nat: 게스트 출발 포워딩 + DHCP·DNS·DHCPv6 포트,
# routed: + 들어오는 포워딩, isolated: 포트만)을 맞추고 데몬이 예전에 만든 전 포트
# 규칙은 지움(AUDIT 기록). "off"면 UFW·iptables를 건드리지 않음 — 데몬 nft 차단은
# off와 무관하게 적용됨.
firewall_integration = auto

# ─────────────────────────────────────────────
# [security_group] 보안 그룹
# ─────────────────────────────────────────────
[security_group]
# vnet 캐시 주기 재동기화 간격(초). 0 또는 음수면 타이머 비활성.
resync_interval_sec = 300

# ─────────────────────────────────────────────
# CPU 격리 코어 (daemon.conf 키 없음)
# ─────────────────────────────────────────────
# CPU 할당과 격리 코어는 daemon.conf로 정하지 않는다. 격리 코어는 커널 부팅 인자
# isolcpus로 정하고, 데몬은 기동 시 /sys/devices/system/cpu/isolated를 읽는다.
# vm.start는 vcpu_count가 없으면 도메인의 현재 vCPU 수만큼 격리 코어를 잡고,
# 모자라면 핀 없이 시작해 WARN을 남긴다.
# 명시 vcpu_count가 도메인의 현재 vCPU 수보다 크면 −32602로 거절한다

# ─────────────────────────────────────────────
# [update] 버전 알림 설정
# ─────────────────────────────────────────────
[update]
# 버전 알림 기능 on/off. true면 데몬이 GitHub 공개 repo 최신 릴리스를 조회해
# 대시보드 툴바에 상태 배지(최신 / 업데이트 가능 여부, 예: `✓ vX.Y.Z` / `↑ vX.Y.Z`)로
# 표시한다 — 읽기 전용 정보 표시만이며 자동 다운로드/설치는 하지 않는다.
# air-gapped/외부망 차단 환경은 false로 꺼서 아웃바운드 호출 자체를 막을 것.
check_enabled = true

# 조회 대상 GitHub Releases API URL (기본값: purecvisor 공개 repo의 latest release)
check_url = https://api.github.com/repos/HardcoreMonk/purecvisor/releases/latest

# 조회 주기(시간 단위). 이 시간 이내에는 재조회하지 않는다(캐시 재사용).
# 1 미만 값은 기본값 24로 보정된다.
check_interval_hours = 24
```

> `[update]` 설정 변경은 SIGHUP 핫리로드 대상이 아닙니다 — 데몬 재시작
> (`systemctl restart purecvisorsd`) 후에만 반영됩니다.

#### 설정 검증

데몬 시작 시 다음 항목이 자동 검증됩니다:

- `rest_port`: 1-65535 범위
- `drain_timeout`: 5초 이상
- `pool_max_conn`: 1-64 범위
- `admin_password`: 미설정 시 bootstrap admin 비활성화, 12자 미만이면 보안 경고
- `zvol_pool`: ZFS 기능 사용 시 실제 dataset 존재 확인
- 경로 존재 여부 (`socket_path`, `[tls] cert`/`key` 등)

범위를 벗어나면 `PCV_LOG_WARN`으로 경고 후 기본값을 사용합니다.

#### 런타임 설정 변경 (SIGHUP)

데몬 재시작 없이 설정을 다시 로드할 수 있습니다:

```bash
sudo kill -SIGHUP $(pidof purecvisorsd)
```

### 2.7 CLI 자동완성

공개 저장소에는 bash/zsh 자동완성 파일(`completion/`)이 없어 `make install-completion`·`make install-completion-user`는 실패합니다.<br>
명령 자동완성은 `pcvctl --interactive`(`-i`) 대화형 셸에서 Tab으로 씁니다(`pcvctl --version` 출력이 `readline: enabled`인 빌드).

```bash
# 대화형 셸 진입 (UDS는 root 전용)
sudo pcvctl --interactive
```

#### CLI 종료상태 계약

`pcvctl`은 화면 출력 형식과 무관하게 다음 프로세스 종료상태를 사용한다. JSON-RPC 오류의
정확한 `code`가 필요하면 `--format=json` 응답을 함께 파싱한다.

| 종료코드 | 의미 |
|---:|---|
| `0` | 명령 성공 또는 `help`/`version` 같은 정상 로컬 동작 |
| `1` | 실행·UDS 전송·응답 프로토콜/파싱·JSON-RPC 거절 실패 |
| `2` | 알 수 없는 명령, 필수 인자 누락, 잘못된 사용법 |

```bash
if pcvctl vm start web-prod; then
    echo "started"
else
    rc=$?
    echo "pcvctl failed: exit=$rc" >&2
fi
```

2026-08-09 이전에는 화면에 RPC 오류가 표시돼도 exit 0이 반환될 수 있었다. 현재 계약은
옵트인 없이 기본 동작이므로, 기존 자동화는 `set -e`, `&&`, `if` 분기가 달라질 수 있다.

### 2.8 로컬 Single Edge 배포

배포 대상 노드 자체에서 실행할 때는 deploy target을 `local`로 명시합니다.
빈 `PCV_NODES`는 저장된 원격 node 목록이 섞이는 것을 막고 `--nodes local`은 현재 host만 변경합니다.

```bash
# 릴리스 빌드 + BPF + 현재 노드 배포
PCV_NODES="" scripts/deploy.sh --nodes local

# 디버그 빌드도 현재 노드로 제한
PCV_NODES="" scripts/deploy.sh --nodes local --debug

# 이미 검증한 산출물 재배포
PCV_NODES="" scripts/deploy.sh --nodes local --skip-build
```

#### Web UI 정적 자산 배포 체크

Web UI 배포는 HTML/JS/CSS만 복사하면 끝나지 않습니다. 운영 브라우저는 CSP, PWA manifest, Service Worker 캐시를 함께 검증하므로 다음 자산이 같은 릴리스 단위로 배포되어야 합니다.

- `ui/index.html`, `ui/docs.html`, `ui/guide.html`, `ui/guide-content.md`, `ui/offline.html`
- `ui/maintenance.html`, `ui/maintenance-status.json`
- `ui/samples/design-system-preview.html`, `ui/samples/design-borrowing-mockup.html`과 관련 `ui/samples/*.html`
- `ui/app.js`, `ui/app.bundle.js`, `ui/i18n.js`, `ui/sw.js`, `ui/vnc-flow.mjs`
- `ui/style.css`
- `ui/modules/*.js`
- `ui/manifest.json`, `ui/icon-192.png`, `ui/icon-512.png`
- `ui/assets/diagrams/purecvisor-single-full-architecture.svg`
- `ui/vendor/chart.umd.min.js`
- `ui/vendor/novnc/novnc.esm.js`
- `ui/vendor/qrcode/qrcode.js`
- `ui/vendor/pretendard/pretendard.css`, `ui/vendor/pretendard/woff2/*.woff2`
- `ui/vendor/coolicons/coolicons.svg`

번들 파이프라인의 정본은 Makefile의 `ui-bundle` 타깃입니다 — `ui/modules/*.js`를 `ui/app.bundle.js`로 다시 묶고(UI_MODULES 등록 누락 가드 + esbuild 민파이/소스맵) `ui/sw.js`의 `CACHE_NAME`을 `UI_CACHE_INPUTS`에 등록된 전체 선캐시 자산 해시로 bump 합니다. 따라서 `docs.html`, `style.css`, 아이콘처럼 bundle 밖의 정적 자산만 바뀌어도 기존 precache가 무효화됩니다. `scripts/bundle-ui.sh`는 그 타깃에 위임하는 하위호환 진입점이며, 추가로 로컬 데몬 서빙 경로(`/usr/local/share/purecvisor/ui`)에 산출물을 복사합니다(`PCV_NO_DEPLOY=1`로 생략). UI 모듈, WebSocket, metrics, 정적 자산 목록을 바꾸면 배포 전에 다음 순서로 확인합니다.

```bash
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py
for f in ui/app.js ui/modules/*.js ui/vendor/chart.umd.min.js ui/vendor/novnc/novnc.esm.js; do node -c "$f"; done
git diff --check -- ui scripts src/api/rest_server.c
rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" ui/index.html ui/docs.html ui/guide.html ui/app.bundle.js ui/sw.js ui/vendor
rg -n "customConfirm\([^\\n]*<[^\\n]*>|<br><b>|idx \|\| selectedVmIndex" ui/modules ui/app.bundle.js
```

두 `rg` 명령은 결과가 없어야 정상입니다. 운영 CSP를 넓혀 외부 아이콘 API나 CDN sourcemap을 허용하지 말고, 필요한 런타임 자산은 `ui/vendor/` 또는 inline SVG처럼 로컬 자산으로 고정합니다. `customConfirm()` 호출부는 HTML 조각을 넘기지 않고 plain text와 `\n`만 사용합니다.

배포 완료 판정은 저장소 산출물, 설치 파일과 설정한 관리 IPv4에서 실제 제공하는 파일을 함께 비교합니다.
초기 자체서명 인증서를 사용하는 동안만 `curl -k`를 사용하며 운영 CA 인증서를 설치한 뒤에는 `-k` 없이 검증합니다.

```bash
NODE_IPV4="<configured-management-ipv4>"
curl -ksS "https://${NODE_IPV4}/api/v1/health" \
  | jq '{status,service,version,node_name,tls:.checks.tls}'
curl -ksS -o /tmp/pcv-live-app.bundle.js \
  "https://${NODE_IPV4}/ui/app.bundle.js?v=2.2.0"

sha256sum ui/app.bundle.js \
  /usr/local/share/purecvisor/ui/app.bundle.js \
  /tmp/pcv-live-app.bundle.js

rg -n "iconify|code\.iconify|api\.iconify|api\.unisvg|api\.simplesvg|cdn\.jsdelivr|fonts\.googleapis|fonts\.gstatic|sourceMappingURL" \
  /tmp/pcv-live-app.bundle.js
```

세 SHA-256 값은 같아야 하며 외부 런타임 참조를 찾는 `rg` 명령은 결과가 없어야 합니다.
health의 `status=ok`, `version=2.2.0`, TLS `mode=internal`, `enabled=true`, `degraded=false`, `status=ok`를 확인합니다.

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
  - `maxsize 200M`, 14일 보존
  - `copytruncate` 사용

Single Edge OVN local controller 준비 경로는 `ovn-controller` 재기동 직후
파일 로그 레벨을 `ERR`로 낮춥니다. `OVNSB commit failed` 같은 INFO 폭주가
재발하더라도 운영 로그가 무제한으로 불어나는 것을 막기 위한 안전장치입니다.
OVN 구축 성공은 `ovn-nbctl` 설치 여부가 아니라 Northbound/Southbound DB 조회,
`ovn-northd` 동기화, `ovn-controller` active, 로컬 Chassis 등록까지 모두 확인한
상태를 뜻합니다. 단일 노드는 DB를 TCP로 공개하지 않고 로컬 Unix socket을 사용합니다.

---

### 2.10 Arch 계열 Omarchy 소스 컴파일 설치

Omarchy에서는 대상 호스트에서 PureCVisor를 직접 컴파일해 설치합니다.
아래 절차는 **새 설치**를 위한 것으로, Bash 셸에서 순서대로 실행합니다.
각 단계에서 오류가 발생하면 해결한 뒤 다음 단계로 진행하세요.
기존 설치를 갱신할 때는 바이너리·UI·설정·PKI를 먼저 백업하고 서비스 중지·교체·복구를
별도 계획해야 합니다. 아래 최초 설치 명령으로 기존 설정을 덮어쓰지 않습니다.

#### 검증한 환경과 소스 선택

| 항목 | 확인한 기준 |
|---|---|
| 호스트 | Omarchy 4.0.3, Arch 계열 x86_64 |
| 커널 | `7.2.3-arch1-3`, cgroup v2, KVM |
| 컨테이너 | LXC `7.0.0-2`, btrfs-progs `7.1-1`, ZFS 미설치 |
| 빌드 | 대상 호스트의 GCC로 최적화·LTO 릴리스 전체 빌드, 최종 컴파일 경고 0 |
| 제품 버전 | `2.0.0`, 당시 검증 소스 `e028ef2` (Btrfs는 현재 main에서 제외) |

현재 공개 `main`은 개발 main과 같은 ZFS 전용 LXC를 사용합니다. NVRAM 수정은
유지하지만 과거 `e028ef2`의 Btrfs 기능은 포함하지 않습니다. 아래 절차는 현재 main의
소스 빌드·file disk VM 설치에 맞췄으며, 지정 Arch 실기 결과는 당시 소스의 역사 근거입니다.
현재 main의 Arch 전체 기능을 다시 인증한 결과는 아닙니다. `git rev-parse HEAD`를
기록하고 LXC를 사용할 때는 별도로 ZFS 환경을 준비합니다.

#### 호스트 준비와 Arch 패키지

관리 IPv4는 DHCP reservation 등 현재 Omarchy 네트워크 관리 방식으로 고정합니다.
2.2절의 Ubuntu용 `apt`·Netplan 명령을 이 호스트에 적용하지 않습니다.
관리 NIC와 default route를 유지하고, 관리 NIC를 VM 브리지에 편입하지 않습니다.

Arch 저장소 전체를 일관된 버전으로 갱신한 뒤 의존성을 설치합니다. `pacman -Sy`만
실행한 상태에서 일부 패키지만 갱신하지 않습니다. 커널이 바뀌면 새 커널로 재부팅한 뒤
KVM과 BTF를 다시 확인하고 컴파일을 시작합니다.

```bash
sudo pacman -Syu
sudo pacman -S --needed \
  base-devel git pkgconf ccache python nodejs npm \
  glib2 glib2-devel json-glib libsoup3 libvirt libvirt-glib lxc \
  sqlite openssl libcap libseccomp readline liburing libbpf libxml2 protobuf-c \
  clang llvm bpf \
  qemu-base edk2-ovmf swtpm \
  qemu-hw-display-virtio-gpu qemu-hw-display-virtio-gpu-pci qemu-hw-display-virtio-vga \
  dnsmasq nftables iproute2 wireguard-tools ca-certificates curl jq

uname -r
gcc --version
node --version
test "$(stat -fc %T /sys/fs/cgroup)" = cgroup2fs
test -c /dev/kvm
test -r /sys/kernel/btf/vmlinux
command -v bpftool
pkg-config --exists glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 \
  libvirt-glib-1.0 libvirt-gobject-1.0 libvirt lxc libsoup-3.0 libcrypto
```

GCC는 C23 `-std=gnu23`을 지원해야 하며 UI 도구 검증에는 Node.js 24를 사용했습니다.
Arch의 `bpf` 패키지가 BPF 도구를 제공하고, GLib 개발 파일은 `glib2-devel`로 준비합니다.
[Arch BPF 패키지](https://archlinux.org/packages/extra/x86_64/bpf/),
[GLib 개발 패키지](https://archlinux.org/packages/core/x86_64/glib2-devel/)에서 현재 구성을 확인할 수 있습니다.

QEMU의 virtio 그래픽 장치는 별도 패키지입니다. 지정 실기에서는 기본
`qemu-hw-display-virtio-gpu` 누락 시 QEMU 장치 조회가 실패했고 위 세 패키지를 갖춘 뒤
정상화됐습니다. [Arch QEMU 패키지 구성](https://archlinux.org/packages/extra/x86_64/qemu-base/)을 함께 확인하세요.

새 libvirt 설치에서는 다음을 실행합니다. 기존에 modular libvirt를 운영한다면 현재
서비스 구성을 유지하고 `qemu:///system` 접속을 확인합니다.

```bash
sudo systemctl enable --now libvirtd.service virtlogd.socket virtlockd.socket
sudo virt-host-validate qemu
sudo virsh -c qemu:///system list --all
```

`/dev/kvm`·TUN·cgroup 실패는 먼저 해결합니다. GPU passthrough 등 IOMMU가 필요한
기능은 해당 항목도 확인합니다. confidential guest 미사용 시 SEV/TDX 관련 경고는
사용 기능과 구분해 기록합니다.

#### 공개 소스와 릴리스 빌드

```bash
git clone https://github.com/HardcoreMonk/purecvisor.git
cd purecvisor
git rev-parse HEAD
git merge-base --is-ancestor e028ef2 HEAD

npm ci
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
python3 scripts/check_ui_bundle_fresh.py

make clean
make -j"$(nproc)" CC=gcc BUILD=release all
make bpf
scripts/install-runtime-prereqs.sh --verify-only --bpf-stage build/bpf

ldd bin/purecvisorsd
ldd bin/pcvctl
bin/pcvctl --version
sha256sum bin/purecvisorsd bin/pcvctl build/bpf/*.o build/bpf/manifest.json
```

`CC=gcc`로 Arch 호스트의 compiler를 명시합니다. `make release`와 같은
`BUILD=release` 설정으로 데몬·CLI를 만들며, debug 산출물을 섞지 않도록 먼저 정리합니다.
빌드 로그의 경고·오류가 없어야 하고 `ldd`에 `not found`가 없어야 합니다.
임시 sysroot나 `LD_LIBRARY_PATH`에 의존하는 시험용 바이너리를 운영에 설치하지 않습니다.
BPF는 객체 2개와 SHA-256 manifest를 함께 배포합니다.

개발 검증까지 수행하려면 `strace`, `python-pytest`, `openvswitch`를 추가로 준비하고
2.3절과 검증 정책을 따릅니다. 전체 계약 게이트는 `make -j1 check-all`로 순차 실행하며,
선택형 기능의 도구·커널 의존성과 환경에 따른 skip을 별도로 기록합니다.

#### 일반 UEFI 펌웨어 경로 준비

현재 소스는 Ubuntu 계열 OVMF 파일명을 탐색합니다. Arch의
[edk2-ovmf 파일 목록](https://archlinux.org/packages/extra/any/edk2-ovmf/files/)에서
`/usr/share/edk2/x64/OVMF_CODE.4m.fd`와 `OVMF_VARS.4m.fd`를 확인한 뒤 호환 링크를 준비합니다.
지정 실기에서는 `/usr/share/OVMF`가 `/usr/share/edk2`를 가리켰습니다.

```bash
test -r /usr/share/edk2/x64/OVMF_CODE.4m.fd
test -r /usr/share/edk2/x64/OVMF_VARS.4m.fd
test "$(readlink -f /usr/share/OVMF)" = /usr/share/edk2

sudo tee /etc/tmpfiles.d/purecvisor-ovmf-compat.conf >/dev/null <<'EOF'
L /usr/share/edk2/OVMF_CODE_4M.fd - - - - /usr/share/edk2/x64/OVMF_CODE.4m.fd
L /usr/share/edk2/OVMF_VARS_4M.fd - - - - /usr/share/edk2/x64/OVMF_VARS.4m.fd
EOF
sudo systemd-tmpfiles --create /etc/tmpfiles.d/purecvisor-ovmf-compat.conf
readlink -f /usr/share/OVMF/OVMF_CODE_4M.fd
readlink -f /usr/share/OVMF/OVMF_VARS_4M.fd
```

마지막 두 경로는 각각 위 Arch CODE·VARS 파일이어야 합니다. `L`은 기존 경로를 강제로
교체하지 않으므로, 다른 파일이 있거나 패키지 경로가 바뀌면 먼저 원인을 확인합니다.
이 설정은 **일반 UEFI**용입니다. Secure Boot 서명·키 등록까지 검증한 설정은 아닙니다.

#### 최초 설치와 설정

다음 명령은 저장소 루트에서 실행합니다. 기존 설정 또는 설치 바이너리가 있으면 중단합니다.

```bash
if sudo test -e /etc/purecvisor/daemon.conf || sudo test -e /usr/local/bin/purecvisorsd; then
  printf '%s\n' '기존 설치가 있습니다. 백업 후 업그레이드 절차를 사용하세요.' >&2
  exit 1
fi

sudo install -d -m 0755 /usr/local/bin /usr/local/share/purecvisor/ui
sudo install -m 0755 bin/purecvisorsd bin/pcvctl /usr/local/bin/
sudo cp -R ui/. /usr/local/share/purecvisor/ui/
sudo install -d -m 0755 /usr/local/share/purecvisor/fallback
sudo install -m 0644 ui/maintenance.html ui/maintenance-status.json /usr/local/share/purecvisor/fallback/
sudo install -d -m 0700 /etc/purecvisor/pki /var/run/purecvisor
sudo install -d -m 0755 /etc/purecvisor/plugins.d /etc/purecvisor/seccomp /var/lib/purecvisor
sudo install -d -m 0750 /var/log/purecvisor
sudo install -d -m 0711 /var/lib/libvirt/images
sudo install -m 0600 packaging/deb/daemon.conf.sample /etc/purecvisor/daemon.conf
sudoedit /etc/purecvisor/daemon.conf
```

sample의 **기존 섹션을 편집**해 다음 값을 맞춥니다. 같은 이름의 섹션을 끝에 중복해서
붙이지 않습니다. 관리자 비밀번호는 `sudoedit`에서 직접 12자 이상으로 설정하고
아래 설명용 값을 그대로 사용하지 않습니다. NAT 대역은 관리망·기존 VM망과 겹치지 않게 선택합니다.

```ini
[daemon]
admin_user = admin
admin_password = <운영자가 설정할 12자 이상의 비밀번호>

[server]
rest_port = 8080
bind_plaintext = loopback

[tls]
https_enabled = true
cert = /etc/purecvisor/pki/node.crt
key = /etc/purecvisor/pki/node.key

[storage]
image_dir = /var/lib/libvirt/images
iso_dirs = /var/lib/libvirt/images

[network]
default_bridge = pcvnat0
default_subnet = 10.78.0.1/24
default_ensure = 1
firewall_integration = auto
```

LXC를 사용할 때는 2.2절과 4.1절의 ZFS 커널·도구·풀 전제를 먼저 충족합니다.
호스트가 Btrfs 파일시스템이라는 사실만으로 현재 제품의 LXC를 사용할 수는 없습니다.
file disk VM만 사용하는 최초 설치는 다음 런타임 준비를 계속합니다.

```bash
sudo chown root:root /etc/purecvisor/daemon.conf
sudo chmod 0600 /etc/purecvisor/daemon.conf
sudo scripts/install-runtime-prereqs.sh --bpf-stage build/bpf
```

runtime helper는 BPF manifest를 검증·설치하고 누락된 JWT 비밀값을 안전하게 생성합니다.
VM은 `storage_type=qcow2` 또는 `raw`로 생성할 수 있습니다. LXC·zvol·ZFS backup을
사용할 때는 별도로 ZFS 커널·도구·풀을 준비합니다. 게스트 reset에는 별도의 `libguestfs`
도구도 필요합니다.

#### systemd 등록과 설치 확인

```bash
sudo install -m 0644 packaging/deb/purecvisorsd.service /etc/systemd/system/purecvisorsd.service
sudo install -d -m 0755 /etc/systemd/system/purecvisorsd.service.d
sudo install -m 0644 packaging/systemd/90-ovl3-netns.conf packaging/systemd/95-coredump-hardening.conf \
  packaging/systemd/97-sandbox.conf /etc/systemd/system/purecvisorsd.service.d/
sudo install -m 0644 packaging/systemd/purecvisor-lxc@.service /etc/systemd/system/purecvisor-lxc@.service
sudo install -m 0644 packaging/deb/purecvisor-lio.conf /etc/modules-load.d/purecvisor-lio.conf
for module in target_core_mod iscsi_target_mod target_core_iblock nf_conntrack_bridge; do
  sudo modprobe "$module"
done
sudo systemctl daemon-reload
sudo systemctl enable --now purecvisorsd.service
systemctl --no-pager --full status purecvisorsd.service
sudo journalctl -u purecvisorsd.service -b --no-pager -n 100

NODE_IPV4="<확정한-관리-IPv4>"
sudo curl --fail --silent --show-error --cacert /etc/purecvisor/pki/node.crt \
  "https://${NODE_IPV4}/api/v1/health" | jq .
sudo pcvctl vm list
```

서비스는 `active/running`, health는 `status=ok`·`version=2.2.0`·`cluster=false`인지
확인합니다. libvirt·TLS·BPF 세부 상태도 확인하고 `https://<관리-IPv4>/ui/`에서 로그인합니다.
최초 자체서명 인증서는 hostname·관리 주소를 확정한 뒤 생성하며, HTTPS 검사에서 신뢰와
SAN 주소가 일치해야 합니다. 외부 접속은 관리망에서 443/tcp를 허용하고 평문 8080은
loopback으로 유지합니다. NGINX는 이 직접 HTTPS 구성에 필요하지 않습니다.

이 문서는 기존 지정 호스트의 설치·빌드 및 후속 NVRAM·Btrfs 실기 증거를 정리한 것입니다.
OVS/OVN·GPU passthrough·Secure Boot·ZFS 전체 기능이나 모든 Arch 환경의 인증,
호스트 재부팅·정전·장시간 안정성 검증을 뜻하지 않습니다.

---

## 3. VM 관리

**2.0.0 시작 계약:** 정지 VM의 NUMA·2MiB hugepage 요구를 실제 XML로 확인하고 DPDK 준비
뒤 다시 검사합니다. 용량 snapshot은 자원 예약 보장이 아닙니다. PMSUSPENDED VM은
깨우기 요청 뒤 RUNNING을 확인하며 기존 active CPU 예약을 다시 할당하지 않습니다.


### 3.1 VM 생성

#### 기본 생성

```bash
pcvctl vm create \
  web-prod \
  --vcpu 2 \
  --memory_mb 2048 \
  --disk_size_gb 20 \
  --qos_min_mbps 0 \
  --qos_max_mbps 1000
```

> **QoS SLA 필수**: 커널 netdev(tc 셰이핑 가능) 조합 — `network_bridge` 미지정/bridge, 또는
> `tenant-overlay` — 은 `--qos_min_mbps`/`--qos_max_mbps`가 필수다(`nic_type`이 `dpdk`/`sriov`인
> 경우만 면제). 생략하면 서버가 `vm.create` 자체를 `-32602`로 거부한다. 아래 스토리지 타입
> 예시들은 지면상 이 두 플래그를 생략했지만 실제 호출에는 포함해야 한다.

> **새 VM 이름**: UUID 표기(16진수 32자리, 하이픈 유무 무관) 이름은 `-32602`로 거부한다.

#### 스토리지 타입 선택

VM 디스크의 스토리지 백엔드를 선택할 수 있습니다.
저장 위치 계약의 설계 기준은 [ADR-0022: VM 생성 저장 위치 계약](../docs/adr/0022-vm-create-storage-location-contract.md)을 따른다.

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

# qcow2 (ZFS 풀이 없는 호스트 또는 파일 디스크가 필요한 경우)
pcvctl vm create dev-vm --vcpu 2 --memory_mb 4096 --disk_size_gb 50 --storage_type qcow2

# qcow2 저장 디렉터리 지정
pcvctl vm create dev-vm --vcpu 2 --memory_mb 4096 --disk_size_gb 50 --storage_type qcow2 --image_dir /var/lib/libvirt/images

# RAW (최대 성능)
pcvctl vm create bench-vm --vcpu 8 --memory_mb 16384 --disk_size_gb 200 --storage_type raw
```

> **자동 감지**: CLI/REST/RPC에서 `storage_type`을 지정하지 않으면 dispatcher inline `vm.create`가 `vm_create.c`로 위임하고, `vm_create.c`가 ZFS 풀 존재 여부를 감지합니다. ZFS 풀이 있으면 `zvol`, 없으면 `daemon.conf [storage] image_dir`에 `qcow2`로 폴백합니다. `storage_pool` 또는 `image_dir`를 함께 주면 자동 감지 대상 위치도 해당 값으로 바뀝니다. `storage_type=zvol`을 명시하면 지정한 ZFS 부모 데이터셋이 없을 때 실패합니다.

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
  --iso_path /pcvpool/iso/ubuntu-24.04-live-server-amd64.iso \
  --qos_min_mbps 100 \
  --qos_max_mbps 500
```

| 옵션 | 설명 |
|------|------|
| `--network_bridge` | 연결할 브릿지. 미지정 시 관리형 기본 NAT 네트워크(`[network] default_bridge`, 기본 `pcvnat0`)에 부착. `none`이면 NIC 미부착 |
| `--iso_path` | 설치용 ISO 또는 seed ISO 경로 |
| `--storage_type` | 디스크 타입 (`zvol`, `qcow2`, `raw`) |
| `--storage_pool` | zvol 부모 데이터셋. 예: `tank/vms` |
| `--image_dir` | qcow2/raw 파일 디스크 저장 디렉터리. 예: `/var/lib/libvirt/images` |
| `--qos_min_mbps` | QoS 보장 대역(Mbit/s). 커널 netdev 셰이핑 대상 VM 은 필수(위 안내 참고) |
| `--qos_max_mbps` | QoS 상한 대역(Mbit/s). 0 이면 서버가 거부(`> 0` 필수) |

고급 VM 속성(`firmware`, `boot_mode`, `tpm`, `cpu_mode`, `hugepages`, `vlan_id`, `base_image`, `ovn_switch`)은 현재 REST/RPC body로 지정한다. `pcvctl vm create` CLI는 위 표의 기본 옵션과 저장 위치 옵션을 직접 파싱한다.

`base_image`를 지정하면 제품은 저장 방식(zvol, qcow2, raw)과 상관없이 요청한 크기의 새 target을
먼저 생성하고 base content를 기록한다. source의 format은 파일 내용으로 판별하므로 확장자 `.qcow2`·`.qcow`·`.img`·`.raw`인
raw·qcow2·qcow 파일을 쓸 수 있다. source가 없거나 변환이 실패하면(target보다 큰 이미지 포함) VM 생성을
성공으로 보고하지 않고 이 요청이 새로 만든 target만 정리한다. Job과 감사에는 실패로 남는다.
이미 존재하는 target과 base source는 덮어쓰거나 삭제하지 않는다.

변환은 root로 돌기 때문에 이미지 안에 적힌 경로를 따라가면 호스트 파일이 VM 디스크로 복사된다. 그래서 target을 만들기 전에
`qemu-img info`로 검사해 다음은 거부한다: backing file이 있는 이미지, external data file이 있는 qcow2, raw·qcow2·qcow가 아닌
형식, 30초 안에 조회되지 않거나 조회에 실패한 파일. 이름이 `.raw`여도 내용으로 판별한다. backing file이 있는 이미지는
`qemu-img convert`로 합쳐 올린다. `vm.import.ova`의 디스크는 `create-type`이 `monolithicSparse`·`streamOptimized`이고 모든 extent가
자기 파일인 vmdk만 받는다. OVF의 `ovf:href`는 경로 구분자가 없는 파일 이름이어야 하고, `.ovf`와 vmdk는 심볼릭 링크가 아닌 일반 파일이어야 한다.

Web UI는 생성 Job이 실패하면 서버가 기록한 사유를 보인다. 기다리는 시간 안에 끝나지 않으면 `완료 미확인 + job_id`로 남는다.

#### RPC 직접 호출

```bash
echo '{"jsonrpc":"2.0","method":"vm.create","params":{
  "name": "web-prod",
  "vcpu": 2,
  "memory_mb": 2048,
  "disk_size_gb": 20,
  "storage_type": "zvol",
  "storage_pool": "tank/vms",
  "network_bridge": "pcvnat0",
  "iso_path": "/pcvpool/iso/ubuntu-24.04-live-server-amd64.iso",
  "qos_min_mbps": 100,
  "qos_max_mbps": 500
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### REST API

```bash
curl -X POST http://127.0.0.1:8080/api/v1/vms \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "web-prod",
    "vcpu": 2,
    "memory_mb": 2048,
    "disk_size_gb": 20,
    "storage_type": "zvol",
    "network_bridge": "pcvnat0",
    "qos_min_mbps": 100,
    "qos_max_mbps": 500
  }'
```

응답:

```json
{
  "result": {
    "accepted": true,
    "name": "web-prod",
    "job_id": "job-…",
    "status": "accepted"
  }
}
```

> **fire-and-forget 패턴**: VM 생성은 즉시 `accepted: true`와 `job_id`를 반환하고 백그라운드에서 비동기 실행됩니다. 결과는 `jobs.get`·WebSocket `job.complete`로 확인합니다. `virDomainDefineXML` 실패 시 zvol/디스크가 자동 롤백됩니다.

#### OVS 자동 감지

`network_bridge`에 OVS 브릿지를 지정하면, 데몬이 `ovs-vsctl br-exists`로 OVS 여부를 자동 확인하고 VM XML에 `<virtualport type='openvswitch'/>`를 자동 추가합니다.

#### 부팅 디스크 I/O 정책과 메모리 격리 (2.0.0)

`2.0.0`부터 `vm.create`가 만드는 도메인 XML에 다음 정책이 항상 적용됩니다. 켜고 끄는 옵션은 없습니다.

| 항목 | 값 | 이유 |
|------|-----|------|
| 부팅 디스크 캐시 | `cache=none` | 호스트 페이지캐시 우회 — zvol/데이터셋 위에서 ZFS ARC와의 이중 캐싱 제거 |
| 부팅 디스크 AIO | `io=native` | `cache=none`(O_DIRECT)과 짝. 버퍼 I/O에서 native AIO는 블로킹될 수 있음 |
| 부팅 디스크 discard | `discard=unmap` | 게스트 `fstrim`이 zvol/qcow2 블록을 실제 해제 → 스냅샷 델타·복제 전송량 감소 |
| 메모리 병합 | `<nosharepages/>` | 호스트 KSM의 page-dedup 측면 채널 차단 |

핫플러그로 붙는 보조 디스크는 이전부터 같은 I/O 정책을 썼습니다. 2.0.0 시정은 부팅 디스크만 QEMU 기본값
(`cache=writeback`, `io=threads`, discard 무시)으로 남아 있던 **두 경로의 정책 비대칭**을 없앤 것입니다.
전제는 백엔드가 O_DIRECT를 지원한다는 점이며, 지원하지 않는 배치는 비지원입니다.

`<nosharepages/>`(QEMU `mem-merge=off`)는 호스트 KSM이 서로 다른 테넌트 VM의 동일 페이지를 병합할 때
생기는 측면 채널을 막습니다 — 병합된 페이지에 쓸 때의 copy 지연을 재는 것만으로 "옆 VM이 이 페이지를
갖고 있는가"를 알아낼 수 있기 때문입니다. 대가는 동종 게스트를 대량 배치할 때의 메모리 절감 포기이며,
도메인별 설정이라 호스트 전역 `ksm/run=0`보다 되돌리기 쉽습니다.

**hugepage 프리플라이트**: `hugepages=true`로 생성하는데 호스트의 2MB 여유 페이지가 부족하면
**디스크 프로비저닝 직전에** 즉시 실패합니다(필요 개수와 여유 개수를 오류 메시지에 담습니다).
이 검사가 없으면 정의는 성공하고 `vm.start`가 libvirt 내부 오류로 죽어 "만들 때는 됐는데 켜지지 않는"
불투명한 상태가 됩니다. 판정은 total이 아니라 free 기준이고, sysfs를 읽지 못하면 막지 않습니다
(프리플라이트가 새 실패 지점이 되면 안 되므로). 프리플라이트를 디스크 생성보다 앞에 두는 이유는
뒤에 두면 실패해도 zvol/qcow2가 남기 때문입니다.

**동시 시작 제어**: 지원 hugepage 구성의 정지 VM `vm.start`는 같은 프로세스에서 첫 검사~DPDK 준비~시작 반환을
직렬화합니다. 다른 지원 시작이 진행 중이면 대기 없이 `hugepage start admission busy`로 실패하므로 실제 VM 상태를
확인한 뒤 재시도합니다. 일반 메모리·DEFERRED·실행 중 VM은 이 제한에서 제외합니다. 다른 NUMA node도 공통 잠금을
쓰며 외부 OVS/virsh와 create/restore 등 다른 시작 경로의 경쟁은 통제하지 않습니다. 실제 page 예약을 보장하지는
않습니다.

**vhost-user(DPDK) 공유 메모리**: `nic_type="dpdk"`는 libvirt가 vhost-user 인터페이스에 공유 메모리
backend를 요구하기 때문에 해당 시정 전에는 **도메인 정의 자체가 거부되던 죽은 경로**였습니다
(`unsupported configuration: 'interface' requires shared memory`). 2.0.0은 memoryBacking을 조합으로
조립해 해소했습니다.

| 조합 | memoryBacking |
|------|---------------|
| `dpdk` + `hugepages` | `<hugepages…/>` + `<access mode='shared'/>` |
| `dpdk`만 | `<source type='memfd'/>` + `<access mode='shared'/>` |
| `hugepages`만 | 종전과 동일 |

> OVS-DPDK 소켓이 없으면 정의가 통과해도 `vm.start`는 여전히 실패합니다. 2.0.0의 시정 범위는
> "정의조차 되지 않던" 결함까지입니다.

### 3.2 VM 라이프사이클

#### Web UI에서 VM 삭제

가상 머신 화면에서 VM을 선택한 뒤 상단 **VM 삭제**를 누릅니다. 여러 VM은 목록 또는
카드의 체크박스를 선택하고 아래 **일괄 삭제 (N)** 또는 **일괄 작업 → 선택한 VM 삭제**를
사용합니다. 전화 폭(600px 이하)은 하단 **전원** 탭의 VM 카드와 **일괄 삭제**를 사용합니다.
ADMIN/OPERATOR에게 표시되며 실제 요청은 서버의 소유권·권한 검사를 받습니다.

확인창은 대상 이름 전체와 영구 디스크 삭제 경고를 표시합니다. 단일 삭제는 VM 이름,
일괄 삭제는 표시된 수량 문구(예: `삭제 2`)를 정확히 입력해야 진행할 수 있습니다.
실행 중인 VM도 강제 종료됩니다. 취소하면 삭제 요청을 보내지 않습니다.

요청은 하나씩 전송하고 대상별 결과를 남깁니다. 서버는 삭제 요청을 Job으로 접수하고(`job_id`),
화면은 그 Job이 끝날 때까지 기다려 **삭제 완료**(남긴 디스크나 부속 정리 실패가 있으면 경고 문구를 모두 덧붙임) 또는
거절·실패 사유를 표시합니다. 기다리는 시간 안에 끝나지 않으면 **처리 중 · 완료 미확인**으로 남으며, 이때는
`vm delete-status <이름>` 또는 `jobs.get`으로 최종 결과를 확인합니다. `job_id` 없이 접수만 돌려주는 이전 서버에서는
**요청 접수 · 완료 미확인**으로 남으며 삭제 완료가 아닙니다. 목록에서 VM이 사라져도 디스크 정리가 끝났다고 단정하지 않습니다.
결과 창을 닫으면 아직 보내지 않은 요청만 중단하며, 접수된 삭제는 계속됩니다.
통신 오류 시 자동 재전송하지 않습니다. 대상이 없어지거나 동명 VM의 UUID가 바뀌면
삭제 요청을 보내지 않으므로 목록을 새로고침하고 다시 선택합니다.


**삭제 범위.** 함께 지우는 저장소는 VM 정의의 **첫 `device='disk'` 블록 하나**이고, 그 이름이 VM과 맞을 때뿐입니다.
zvol은 `/dev/zvol/<풀>/<VM 이름>`, 파일은 `<VM 이름>.qcow2`·`.raw`·`.img`입니다. rename·clone·OVA로 만든 VM도 이 규칙에 맞습니다.

- **남는 것**: 따로 붙인 zvol·파일, 업그레이드 전에 `device.disk.detach`로 live에서만 뗀 디스크(영구 정의에 남아 있습니다), 이름이 다른 첫 디스크, `/dev/sdX` 같은 물리 장치.
  VM은 지워지고 Job이 `completed`로 끝나며 `warnings`에 `Disk not deleted (not owned by this VM): <데이터셋·경로>`가 적힙니다. 지우려면 직접 지웁니다.
- **거절되는 것**: 스냅샷에 클론이 있는 zvol(`vm.clone --mode cow`의 원본 등), 다른 VM(켜짐·꺼짐 모두, 정의 기준)이 참조하는 저장소, 다른 VM 이미지의 backing 파일.
  Job은 `failed`이고 사유에 의존 대상과 먼저 할 일이 적히며, VM 상태·정의·DPDK 포트·저장소는 그대로입니다. 클론을 먼저 지우거나 `zfs promote`로 독립시킨 뒤 다시 삭제합니다.
  다른 VM의 정의에 남은 참조는 그 VM에서 `pcvctl device disk detach <VM> --target <dev>`로 live와 영구 정의에서 뺍니다.
- **libvirt에 닿지 못하면 삭제하지 않습니다.** 연결이 없거나 조회가 `no such domain` 말고 다른 이유로 실패하면 Job은 `failed`이고 아무것도 바뀌지 않습니다. 정의가 정말 없을 때만 기본 경로 `<zvol_pool>/<이름>`을 치우고 성공합니다(멱등).
- **다른 VM 이미지를 읽지 못해도 삭제하지 않습니다.** backing 검사는 다른 VM의 `raw`가 아닌 디스크를 `qemu-img info`로 읽습니다. 이미지 머리가 깨진 VM이 하나라도 있으면 그 VM이 있는 동안 다른 VM 삭제가 Job `failed`(`could not read the backing chain …`)로 끝나고 아무것도 바뀌지 않습니다.
  그 이미지를 고치거나 그 VM을 먼저 지웁니다. 그 VM 자신의 삭제는 자기 디스크를 검사하지 않으므로 막히지 않습니다.

파일형 UEFI VM 삭제는 NVRAM을 보존한 채 domain을 해제한 뒤 주 디스크를 삭제하고,
성공 후 해당 VM XML의 NVRAM을 정리합니다. 디스크 삭제 실패는 domain을 복원하고 NVRAM을
보존합니다. 주 디스크 삭제 뒤 NVRAM 정리만 실패하면 삭제는 완료(감사 `ok`)이고 Job 결과에
`NVRAM cleanup failed: <경로>` 경고가 붙어 수동 정리 대상을 알려 주며, 디스크가 복구됐다고 안내하지 않습니다. firmware loader/template·설치 ISO는 삭제 대상이
아닙니다.

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

# 삭제 (비동기, VM 소유 디스크만 함께 삭제)
pcvctl vm delete web-prod

# 삭제 결과 확인 (최근 삭제 Job 상태, 거절 사유·남긴 디스크)
pcvctl vm delete-status web-prod
```

**중지 동작 상세:**

1. `virDomainShutdown`으로 ACPI 셧다운 신호 전송
2. 1초 간격으로 최대 30초 동안 정상 종료 폴링
3. 타임아웃 시 `virDomainDestroy`로 강제 종료
4. 반환값 검증 후 결과 보고

**삭제 결과(Job):**

```bash
# RPC로 삭제 결과 확인
echo '{"jsonrpc":"2.0","method":"vm.delete.status","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

`vm.delete` 응답은 `{"accepted": true, "name": "<VM 이름>", "job_id": "job-…", "status": "accepted"}`이다. 결과는 `jobs.get`, WebSocket `job.complete`, `vm.delete.status {name}`으로 본다.
`vm.delete.status` 응답 `status`는 Job 상태(`pending` | `running` | `completed` | `failed` | `cancelled` | `interrupted`, 기록 없음 `not_found`)이고, 정의·디스크를 지운 뒤 부속 정리만 실패했거나 VM 소유가 아닌 디스크를 남겼으면 `completed`에 `warnings`가 붙는다. 거절·실패 사유는 `error`다.

**이름 변경:**

VM 이름 변경은 정지된 VM에서만 허용된다. libvirt domain 이름, 표준 ZFS zvol 또는 표준 qcow2/raw/img 파일 디스크 경로, UEFI NVRAM 경로를 함께 변경한다.

지원 조건:

- VM 상태가 `shut off`여야 한다.
- 새 이름은 `pcv_validate_vm_name()` 규칙을 따른다. 허용 문자는 영문, 숫자, 하이픈, 언더스코어이며 최대 64자다.
- 새 이름이 UUID 표기(16진수 32자리, 하이픈 유무 무관)이면 `-32602`로 거부한다.
- 기본 디스크는 `/dev/zvol/<parent>/<vm>` 또는 `<vm>.qcow2`, `<vm>.raw`, `<vm>.img` 형식이어야 한다.
- libvirt snapshot metadata가 남아 있으면 이름 변경을 거부한다.
- 그 VM(옛 이름)이나 새 이름에 다른 작업(스냅샷·백업·시작 등)이 진행 중이면 `vm.rename`은 `-32004`(BUSY)로 거절하고 아무것도 바꾸지 않는다.
- 보안 그룹이 바인딩된 VM은 `vm.rename`을 `-32002`(CONFLICT, REST 409)으로 거절한다. 바인딩이 VM 이름에 묶여 있어 이름이 바뀌면 필터가 풀리기 때문이다. `security_group.detach`로 해제한 뒤 이름을 바꾸고 다시 붙인다.

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
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/vms | python3 -m json.tool

# VM 시작
curl -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/vms/web-prod/start

# VM 중지
curl -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/vms/web-prod/stop

# VM 일시 정지
curl -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/vms/web-prod/suspend

# VM 재개
curl -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/vms/web-prod/resume

# VM 삭제
curl -X DELETE -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/vms/web-prod

# VM 이름 변경
curl -X PUT -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"new_name":"web-prod-01"}' \
  http://127.0.0.1:8080/api/v1/vms/web-prod/rename

# 최근 삭제 Job 상태 확인
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/vms/web-prod/delete-status
```

#### ADMIN 전용 구성 관측 (`vm.inspect`)

2.1.0부터 ADMIN이 정규 소문자 UUID로 VM 구성을 조회합니다.
2.0.4 이하 공개 패키지에는 이 명령이 없습니다.

```bash
sudo pcvctl --format=json vm inspect <canonical-lowercase-uuid>
# 기존 ADMIN 토큰 또는 API key 인증 사용; UUID를 실제 값으로 지정
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://127.0.0.1:8080/api/v1/vms/$VM_UUID/inspection"
```

이름 fallback 없이 UUID 하나만 받습니다. GET 전용 경로의 추가 path/query/body는 거절합니다.
기존 ADMIN 인증·root UDS 정책을 유지하며 VIEWER/OPERATOR 권한은 넓히지 않습니다.
응답 contract는 `pcv-vm-inspection-v1`, 구성 범위는 `configuration_view=inactive-redacted`입니다.
`outcome`은 `observed`·`not_observed`·`unavailable`이고 모르는 사실은 null/unknown으로 남깁니다.
실행 중 VM의 용량·OS·Secure Boot·backing/encryption을 추정하지 않으며 원시 XML·호스트
disk 경로·credential을 응답하지 않습니다. 구성 hash는 디스크 내용 revision이나 source 보호의 증거가 아닙니다.
XML 1 MiB·disk 64개·응답 256 KiB 상한과 30초 협력적 deadline을 적용합니다. blocking libvirt
호출을 강제 중단하는 한도는 아닙니다. REST bridge/CLI 대기는 35초이며 다른 CLI의 기본 10초와 구분합니다.
VM을 변경하거나 Job/WS 완료 이벤트를 만들지 않고 실제 완료·timeout·입력 거절을 audit에 기록합니다.

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

Web UI CPU 핀 화면도 같습니다. vCPU 번호와 물리 CPU 목록(예: `0-3,6`)을 넣어 한 번에 vCPU 하나씩 고정합니다.

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

# NIC 추가 (CLI는 브리지 이름만 받는다)
pcvctl nic add web-prod pcvnat0

# NIC 제거 (MAC 주소 지정)
pcvctl nic remove web-prod 52:54:00:ab:cd:ef
```

> **model 허용목록**: RPC `device.nic.attach`·REST의 `model`은 `virtio` / `e1000` / `e1000e` / `rtl8139`만 허용한다(생략하면 `virtio`,
> CLI `nic add`는 model을 받지 않는다). 범위 밖 값은 `-32602 INVALID_PARAMS`로 거부된다.

> **MTU 명시**: NIC XML의 `<mtu>`는 attach 시점 브리지의 실측 MTU(`/sys/class/net/<bridge>/mtu`)를
> 1500 포함 항상 명시한다. 자세한 계약은 6.1 절 참조.

#### ISO 관리

```bash
# ISO 목록 (설정된 iso_dirs 전체 스캔)
pcvctl iso list

# ISO 마운트
pcvctl iso mount web-prod /pcvpool/iso/ubuntu-24.04-live-server-amd64.iso

# ISO 꺼내기
pcvctl iso eject web-prod
```

#### 디스크 핫플러그

```bash
# 블록 장치 연결 (--bus 생략 시 virtio, scsi·sata·ide 선택 가능)
pcvctl device disk attach web-prod --source /dev/zvol/pcvpool/vms/new-disk --target vdb

# 분리
pcvctl device disk detach web-prod --target vdb
```

- `attach`는 실행 중이면 live와 영구 정의 모두에, 꺼진 VM이면 영구 정의에만 붙입니다. `detach`도 같아서 live와 영구 정의에서 함께 뺍니다(꺼진 VM은 영구 정의만). 응답에 바뀐 쪽이 `{"live": <bool>, "config": <bool>}`로 담깁니다. `live: true`는 탈착 요청이 받아들여졌다는 뜻이라 게스트 협조가 늦으면 잠시 남을 수 있습니다.
- 한쪽에만 있는 디스크는 있는 쪽만 바꿉니다. 실행 중 VM의 live와 영구 정의에서 같은 target의 source가 다르면 −32000 `Disk '<target>' differs between the running VM and its persistent definition; nothing was changed`로 거절하고 아무것도 바꾸지 않습니다. 어느 쪽에도 없으면 −32000 `Device not found in live or persistent XML`입니다.
- 같은 VM에 다른 작업이 진행 중이면 VM 존재 여부와 무관하게 −32004가 먼저 옵니다. VM을 UUID로 지정했는데 바로 쓸 libvirt 연결이 없으면(libvirtd 무응답·연결 풀 고갈 등) 잠금 전에 −32004 `VM '<uuid>' identifier could not be resolved (libvirt unavailable)`이고, 이름으로 지정하면 연결을 기다렸다가 끝내 얻지 못할 때 −32000 `Hypervisor connection pool exhausted`입니다.
- 업그레이드 전에 "임시 탈착"한 디스크는 영구 정의에 남아 있어 다음 부팅에 다시 붙습니다. 빼려면 같은 detach를 한 번 더 보냅니다(live에 없으면 영구 정의만 바뀝니다).

#### USB 핫플러그

```bash
# USB 장치 목록 — 항목마다 live(실행 중 정의)·persistent(영구 정의)·startup_policy·missing
pcvctl vm usb-list web-prod

# USB 장치 연결 — 실행 중이면 live와 영구 정의 둘 다, 꺼져 있으면 영구 정의만(기본 startup_policy optional)
pcvctl vm usb-attach web-prod 0x1234 0x5678
pcvctl vm usb-attach web-prod 0x1234 0x5678 --startup-policy mandatory

# 실행 중 VM에 지금만 연결 — live 정의에만 붙고 영구 정의는 그대로라 VM을 끄면 빠진다(RPC·REST persistent: false)
pcvctl vm usb-attach web-prod 0x1234 0x5678 --live-only

# USB 장치 분리 — 장치가 있는 쪽(live·영구)에서만
pcvctl vm usb-detach web-prod 0x1234 0x5678
```

- `vendor_id`·`product_id`는 `0x`+16진 4자리다(`0X1234`도 같은 장치로 본다). 실행 중 VM에는 live와 영구 정의 모두에, 꺼진 VM에는 영구 정의에만 붙는다. 영구 정의가 없는(transient) 실행 중 VM은 live에만 붙는다. 응답의 `live`·`config`는 이번 호출이 바꾼 쪽이고, attach 응답 `startup_policy`는 호출 뒤 영구 정의에 적힌 값이다(속성이 없으면 `mandatory`, 영구 정의에 없으면 null).
- `--startup-policy`(REST·RPC `startup_policy`)는 `mandatory`·`requisite`·`optional` 중 하나이고 기본은 `optional`이다 — 장치가 빠져 있어도 VM이 시작하고 목록에 `missing: true`가 보인다. libvirt 기본(`mandatory`)과 일부러 다르다: 빠지기 쉬운 장치 하나 때문에 VM이 시작하지 못하면 안 된다. 장치가 없으면 시작하지 않아야 하는 VM은 `mandatory`로 붙인다. 다른 값은 −32602다.
- `--live-only`(RPC·REST `persistent: false`, 생략하면 `true`)는 실행 중 VM의 live 정의에만 붙이고 영구 정의는 쓰지 않는다 — VM을 끄면 빠진다. live에 이미 있으면 아무것도 바꾸지 않는다(`live: false`·`config: false`). 영구 정의를 쓰지 않으므로 그 정책과 비교하지 않고, 응답 `startup_policy`는 영구 정의에 적힌 값이다(영구 정의에 없으면 null). 영구 정의가 없는(transient) VM에서는 생략과 같다. 꺼진 VM에는 붙일 곳이 없어 −32002(REST 409) `USB device <v>:<p> cannot be attached to VM '<vm>' without saving it: the VM is not running`이다. `persistent`가 JSON boolean이 아니면 −32602 `persistent must be a boolean`, `persistent: false`와 `startup_policy`를 함께 주면 −32602 `startup_policy applies only when persistent is true`다(둘 다 잠금 전 판정). 플래그가 없으면 CLI는 `persistent`를 보내지 않는다.
- 실행 중 VM에 호스트에 없는 장치를 붙이면 −32000 `USB device <v>:<p> could not be attached to the running VM '<vm>'; nothing was changed: …`로 끝나고 영구 정의도 그대로다. live에 붙인 뒤 영구 정의 저장이 실패하면 이번 호출이 붙인 live만 되돌린다.
- 같은 장치를 다시 붙여도 오류가 아니다 — 이미 같은 상태면 성공(`live: false`·`config: false`)이고 모자란 쪽만 채운다. 영구 정의의 정책이 다르면 −32002(REST 409)이므로 떼고 다시 붙여 정책을 바꾼다. 자리표시자(`missing`)로 남은 장치, 같은 장치가 여러 항목인 정의도 −32002다.
- detach는 장치가 있는 쪽에서만 뗀다. 어디에도 없으면 −32000 `USB device <v>:<p> is not attached to VM '<vm>' (live or persistent)`이다 — vid:pid 오타가 성공으로 보이지 않는다.
- 업그레이드 전에 붙인 장치는 live에만 있다(목록 `persistent: false`). 같은 `usb-attach`를 한 번 더 보내면 영구 정의에 저장된다. 그대로 두면 다음 정지 때 사라진다.
- `usb-list` 표는 `VENDOR_ID PRODUCT_ID LIVE PERSISTENT POLICY`(자리표시자는 LIVE 칸 `missing`)이고, plain·CSV는 앞 두 칸 뒤에 `live,persistent,startup_policy,missing`을 덧붙인다.
- `usb-attach`·`usb-detach`의 표 형식 성공 요약 `changed: live=<yes|no>, persistent=<yes|no> · startup_policy: <값>`은 이번 호출이 바꾼 쪽을 보인다(이미 같은 상태라 바꾼 것이 없으면 둘 다 `no`). 장치가 지금 live·영구 정의에 있는지는 `usb-list`가 보인다.
- 두 변경 RPC는 메인 루프가 아니라 `VM 장치 변경` 줄(1칸·대기 8)에서 돈다(아래 "libvirtd·외부 명령이 응답하지 않을 때" 표). 같은 VM에 다른 작업이 진행 중이면 −32004가 먼저 온다.
- Web UI `인프라 > 호스트 장치` 화면(13.2 '호스트 장치 화면')이 장치마다 할당된 VM과 실행 중·영구 상태를 보이고 이 RPC들로 할당·해제한다. 할당 창의 '영구로 저장' 체크를 끄면 `persistent: false`로 보낸다. CLI 쪽 같은 표는 `pcvctl host devices`다.
- 한계: 케이블 재연결·펌웨어 업데이트로 장치가 다시 열거되면 실행 중 VM은 장치를 잃는다. 펌웨어가 제품 ID를 바꾸면 영구 정의도 새 ID로 다시 붙여야 한다. VM을 다시 시작하거나 떼고 다시 붙인다. 호스트 장치 화면·`pcvctl host devices`는 live 항목이 가리키는 장치 번호가 호스트에 없으면 '호스트에서 빠짐'(같은 ID가 다른 번호로 있으면 '옛 장치 번호', CLI 토큰 `stale-address`)으로 보이고 '실행 중'으로 보이지 않는다.

#### GPU 할당

```bash
# GPU 목록
pcvctl gpu list

# 꺼진 VM에 GPU(IOMMU 그룹) 할당 / 제거
pcvctl gpu attach web-prod 0000:01:00.0
pcvctl gpu detach web-prod 0000:01:00.0
```

- GPU 할당·해제(`device.gpu.attach`·`detach`)는 ADMIN만 하고, 꺼진 VM에만 한다. 실행 중이면 `GPU assignment requires a shut off VM`(해제는 `GPU removal requires a shut off VM`)으로 거절한다.
- GPU의 IOMMU 그룹 전체를 미리 `vfio-pci`에 묶어 둔다. 그룹 안에 다른 드라이버를 쓰는 장치가 있으면 `IOMMU group member <주소> uses '<드라이버>'; pre-bind the whole GPU group to vfio-pci`로 거절한다. 2.2.0부터 IOMMU 그룹은 장치 깊이와 무관하게 그룹 번호로 찾는다.
- Web UI는 `인프라 > 호스트 장치` 화면(13.2 '호스트 장치 화면')의 GPU 행에서 할당·해제한다. 'GPU 장치' 화면의 '호스트 장치에서 할당' 버튼은 이 화면으로 옮긴다. 장치별 할당 상태 표는 CLI `pcvctl host devices`다.

#### 디스크 리사이즈

```bash
# 라이브 디스크 확장 (축소 불가, GB 단위)
pcvctl vm disk-resize web-prod vda 50

# REST API
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/vms/web-prod/disk-resize \
  -H "Content-Type: application/json" \
  -d '{"target": "vda", "new_size_gb": 50}'
```

파일 디스크는 VM 정의의 `driver type`을 형식으로 고정해 키웁니다. `driver type`이 없거나 raw·qcow2가 아니면 아무것도 바꾸지 않고 실패합니다.

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

Web UI 대역폭 화면도 같은 값을 KB/s로 받습니다(125000 KB/s는 약 1 Gbps). 빈 칸은 바꾸지 않으며 둘 중 하나는 넣어야 합니다. Burst 입력은 서버가 쓰지 않아 없앴습니다.

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

> **새 VM 이름**: UUID 표기(16진수 32자리, 하이픈 유무 무관) 이름은 `-32602`로 거부한다.

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"vm.clone","params":{
  "source": "web-template",
  "clone_name": "web-staging",
  "mode": "cow",
  "template_prepared": true
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **안전 제한**: `vm.clone`은 source VM이 `shut off` 상태일 때만 허용하며, 단일 data disk VM만 지원한다. `template_prepared=true`가 없으면 `guest_reset=true` 경로로 `libguestfs-tools`의 `virt-sysprep`, `virt-filesystems`, `guestfish`, `virt-customize`를 실행해야 한다. qcow2/raw는 추가로 `mode=full`만 허용한다. 자세한 기준은 [ADR-0023](../docs/adr/0023-vm-clone-beta-safety-guard.md)을 따른다.

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

클론 후 원본 스냅샷 의존을 풀려면(제품 CLI·RPC는 없고 ZFS 명령을 직접 쓴다):

```bash
# 원본 snapshot 의존 조회 (origin이 '-'가 아니면 클론)
sudo zfs get origin rpool/web-staging
# 의존 방향 뒤집기
sudo zfs promote rpool/web-staging
```

promote 뒤에는 원본이 클론 쪽 스냅샷에 의존한다.

### 3.6 OVA 내보내기 / 가져오기

```bash
# OVA 내보내기 (qemu-img VMDK 변환 + OVF + tar)
pcvctl vm export-ova web-prod --output-dir /tmp

# OVA 가져오기
pcvctl vm import-ova /tmp/web-prod.ova web-imported
```

> **새 VM 이름**: UUID 표기(16진수 32자리, 하이픈 유무 무관) 이름은 `-32602`로 거부한다.

### 3.7 Guest Agent 연동

QEMU Guest Agent는 VM 내부 패키지와 libvirt channel이 함께 있어야 사용할 수 있다. **v1.1부터 `vm create`가 생성하는 VM에는 guest-agent channel이 기본 포함**되므로 신규 VM은 별도 보정이 필요 없다(패키지만 VM 내부에서 설치). `guest-agent-ensure-channel`은 채널이 없는 **기존/레거시 VM 보정**용이다. PureCVisor는 상태 진단과 channel 보정을 함께 제공한다.

Web UI에서는 `워크로드 > 가상 머신`에서 실행 중인 VM을 선택한 뒤 요약 탭 머리의 `디스크 사용량` 버튼(스토리지 카드 '게스트 사용량' 줄이 이 버튼을 가리킨다)으로 게스트 파일시스템 사용량을 확인한다.
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

# Guest Agent를 통한 명령 실행 (Linux /bin/sh, Windows PowerShell — 응답 stdout/stderr는 유효 UTF-8)
pcvctl vm guest-exec web-prod "hostname -I"
pcvctl vm guest-exec win-guest 'cmd /c dir "C:\Program Files" /b'

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
  http://127.0.0.1:8080/api/v1/vms/web-prod/guest-agent

# Guest Agent channel 보정
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/vms/web-prod/guest-agent-channel

# Guest Agent ping
curl -X POST -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/vms/web-prod/guest-ping

# Guest Agent 명령 실행
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/vms/web-prod/guest-exec \
  -d '{"command": "cat /etc/hostname"}'

# 게스트 파일시스템 디스크 사용량 조회
curl -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/vms/web-prod/disk-usage
```

게스트 명령 실행(`vm.guest.exec`) 계약:

- 셸은 게스트 OS로 정한다. 데몬이 먼저 `guest-get-osinfo`(5초)를 묻고 `id`가 `mswindows`면 PowerShell(`powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand`), 그 밖이나 조회 실패면 `/bin/sh -c`로 실행한다. 응답 `shell`(`sh`·`powershell`)과 `os_id`(게스트가 알려 줄 때)로 어느 쪽인지 보인다.
- **Windows 게스트의 `command`는 PowerShell 언어다.** cmd 문법·배치 파일은 `cmd /c "…"`로 감싼다(예: `cmd /c dir "C:\Program Files" /b`). 따옴표·한글·여러 줄이 그대로 간다. 래퍼가 콘솔 출력 코드 페이지를 UTF-8로 바꾸므로 그 안에서 실행한 `cmd /c echo 한글`도 UTF-8로 나오지만, `ipconfig`처럼 지역화된 도구는 영어로 출력할 수 있다. PowerShell 시작에 1–1.5초가 더 든다. 명령은 게스트 agent 계정(Windows LocalSystem, Linux root)으로 돈다.
- 종료 코드: Linux는 셸 종료 코드 그대로다. Windows는 외부 명령의 종료 코드를 그대로 넘기고 다른 오류보다 먼저 본다(`cmd /c exit 3` → 3). 외부 명령이 stderr에 쓴 줄은 `stderr`에 텍스트로 오지만 그것만으로 종료 코드가 1이 되지 않는다(`cmd /c "echo warn 1>&2"` → 0). cmdlet 오류(예: `Get-Item C:\nope`)와 명령을 멈추는 오류(`throw`, `-ErrorAction Stop`)는 1이고 `stderr`에 텍스트로 남는다 — cmdlet 오류 뒤의 출력은 이어지고, 멈추는 오류는 그 자리에서 끝난다. 명령 안의 `exit N`은 그 자리에서 N으로 끝난다(그 앞 출력은 남는다). 그 밖은 0이다.
- 시간: 서버는 처리를 시작한 때부터 15초 안에 답한다. 그 안에 끝나지 않은 명령은 `exited: false`(아직 실행 중, `exitcode` −1)로 돌아오고 게스트에서는 계속 돌 수 있다. REST·CLI는 20초, Web UI는 25초까지 기다린다 — 서버 예산보다 먼저 끊지 않는다.
- 출력: `stdout`·`stderr`는 늘 유효 UTF-8이다. 원문이 UTF-8이 아니면(예: Linux 게스트의 비UTF-8 출력이나 NUL이 섞인 출력) 잘못된 바이트는 U+FFFD로 보이고 원문 바이트는 `stdout_b64`·`stderr_b64`(base64)로 함께 온다. Windows는 PowerShell이 출력을 UTF-8로 다시 써서 보내므로 `*_b64`가 보통 나타나지 않는다 — 코드 페이지를 따르지 않는 도구가 낸 바이트는 PowerShell 단계에서 이미 U+FFFD가 되어 b64로도 되살릴 수 없다. 출력 잘림 표시(`out-truncated`)는 전달하지 않는다.
- 게스트 agent가 응답하지 않으면(연결 끊김·멈춤) 명령을 보내지 않고 −32000 `guest agent on '<vm>' is not responding: …`로 끝난다.
- 한계 — agent 작업 겹침: 같은 VM에 다른 agent 작업(예: 응답하지 않는 agent에 다른 도구가 보낸 명령)이 걸려 있으면 데몬의 호출은 libvirt가 그 작업을 끝낼 때까지 기다린다. 이 대기는 15초 예산이 다스리지 못한다 — 실측에서 약 29초 뒤 −32000이었고, 그 사이 REST·CLI는 20초에서 시간 초과로 먼저 끊는다. 이전 판도 같다. 살아 있는 agent에서 명령 실행이 겹치는 것은 기다리지 않는다.

Web UI 명령 실행 칸은 위 셸(Linux `/bin/sh -c`, Windows PowerShell)이 그대로 실행하는 한 줄이라 인자도 같은 칸에 적습니다. 서버가 읽지 않던 Args 입력은 없앴습니다.

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

#### 에이전트 IP 조회와 `ip_source`

`monitor.fleet`(모니터 화면·`/api/v1/metrics`)과 `device.nic.list`는 메인 루프에서 게스트 에이전트를 직접 기다리지 않는다. 에이전트가 알려 주는 IP는 데몬이 메인 루프 밖에서 5초 상한으로 조회해 VM별로 캐시하고(60초 동안 신선), 응답하지 않는 에이전트는 60초부터 두 배씩(최대 600초) 간격을 두고 다시 묻는다. 그래서:

- libvirt lease가 없고 에이전트만 IP를 아는 VM은 **시작 직후 첫 조회에서 IP가 비거나 ARP 값이 나오고, 한 폴링 주기(약 30초) 늦게** 에이전트 IP가 보인다.
- `monitor.fleet`의 VM 항목에 `ip_source`가 더해졌다: `lease`(libvirt DHCP lease), `guest-agent`(에이전트 캐시), `lease-file`(dnsmasq lease 파일), `arp`(호스트 ARP 표), 못 찾으면 빈 문자열(이때 `ip`는 `N/A`). `device.nic.list`의 `ip_source`는 값 이름은 같고 `guest-agent`가 캐시에서 온 결과를 뜻한다.
- 에이전트가 응답하지 않는 VM이 있어도 다른 요청이 느려지지 않는다. 데몬 로그에 `guest agent of VM '<이름>' did not answer guest-network-get-interfaces (…); next try in <초> s` WARN이 나오면 그 VM 안의 `qemu-guest-agent`를 확인한다(회복하면 `answered again` INFO).
- `vm.guest.agent.status`도 같은 실행기에서 3초 `guest-ping`으로 돌고, 같은 VM에 대한 겹친 호출은 결과를 함께 받는다. 비 ZFS VM의 `vm.snapshot.rollback`은 같은 VM에서 겹치면 BUSY로 거절된다.

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

# 템플릿 삭제
pcvctl template delete web
```

템플릿을 적용해 VM을 만드는 CLI·RPC는 없습니다(`template`은 list·get·create·delete·history). Web UI `워크로드 > 템플릿`의 'Use' 버튼이 템플릿 값을 VM 생성 마법사에 채웁니다. CLI는 `pcvctl template get web`으로 값을 읽어 `pcvctl vm create`에 넣습니다.

> **이름 검증**: 템플릿 이름은 `pcv_validate_vm_name()` 규칙을 따릅니다 (영문 대소문자, 숫자, 하이픈, 언더스코어, 최대 64자).

### 3.9 VM 메트릭

```bash
# 개별 VM 메트릭 (CPU·메모리 사용률 %)
pcvctl monitor metrics web-prod
```

출력 예시(`--format=plain`, 탭 구분. 표 형식은 `REALTIME TELEMETRY` 배너와 CPU·MEMORY 막대):

```
vm	cpu	mem
web-prod	15	50
```

```bash
# 전체 VM 일괄 메트릭 (fleet 뷰)
pcvctl monitor fleet
```

출력 예시(사이버 테마):

```
 ENTITY_ID            │ LIFELINE   │ CPU_TIME │   MEM%
──────────────────────┼────────────┼──────────┼────────
 example-vm-source    │ RUNNING    │    63.0s │  26.4%
 example-vm-app       │ RUNNING    │    50.7s │  17.2%
```

PLAIN/CSV(`--format=plain`/`--format=csv`)는 헤더가 `NAME,STATE,CPU_TIME,MEM%`로 바뀐다. CPU_TIME은 누적
CPU 시간(초, 예 `63.0s`, 값 없으면 `-`)이고 MEM%는 `mem_used_mb/mem_max_mb*100`이다(정지 VM 등
계산 불가 시 `N/A`).

REST API:

```bash
# 개별 VM 메트릭
curl -s -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/vms/web-prod/metrics | python3 -m json.tool
```

---

## 4. 컨테이너 관리

PureCVisor는 LXC 컨테이너를 ZFS 백엔드와 통합하여 관리합니다.

### 4.1 컨테이너 생성

> **ZFS 필수 — 컨테이너 생성 전 확인**
>
> PureCVisor의 LXC 컨테이너 생성에는 **사용 가능한 ZFS 풀과 컨테이너용 파일시스템 데이터셋**이 필요합니다.
> ZFS 커널 모듈과 `zfs` 명령을 준비하고, `daemon.conf`의 `[storage] container_pool`을 해당 풀 아래의 컨테이너 부모 데이터셋 경로로 설정하세요. 기본값은 `pcvpool/containers`이며 부모 데이터셋이 없으면 생성 과정에서 만들기를 시도합니다.
> 이 저장소는 파일시스템 데이터셋입니다. VM용 블록 볼륨 `zvol`을 별도로 만드는 절차는 필요하지 않습니다.
> **LXC 패키지만 설치하거나 Btrfs·ext4 디렉터리만 준비한 상태에서는 현재 PureCVisor의 컨테이너 생성이 실패합니다.** 현재 Btrfs·일반 디렉터리 백엔드와 자동 폴백은 지원하지 않습니다.
> ZFS 없이 사용할 수 있는 qcow2/raw VM 생성과 컨테이너 생성의 전제조건을 구분하세요.

```bash
# 기본 생성 (LXC + ZFS rootfs)
pcvctl container create app-ctr --image ubuntu:24.04

# 생성 후 바로 시작
pcvctl container create web-ctr --image ubuntu:22.04
pcvctl container start web-ctr
```

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"container.create","params":{
  "name": "app-ctr",
  "image": "ubuntu:24.04"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **fire-and-forget**: 컨테이너 생성/시작/중지/삭제는 모두 fire-and-forget 패턴으로 즉시 응답합니다.

새 컨테이너 이름은 `-`로 시작할 수 없습니다. `pcvctl`은 사용법 오류(exit 2)로, RPC `container.create`·`container.clone`(`dest`)은 −32602로 끝납니다. `--image`를 생략하면 `ubuntu:22.04`입니다. 예전 가이드대로 만들어 이름이 `--name`이 된 컨테이너는 `pcvctl container destroy --name`으로 지울 수 있습니다.

`container.create`는 브리지를 `network_bridge`로 받으며 `bridge`는 같은 뜻의 별칭입니다(웹 UI가 `bridge`를 보냅니다). 둘 다 보내면 값이 같아야 하고 다르면 −32602입니다. 별칭도 `network_bridge`와 같은 이름 검증·OPERATOR 자원 경계 판정을 받습니다 — 허용되지 않은 브리지는 −32006입니다(예전에는 UI가 고른 브리지가 조용히 무시되고 기본 브리지에 붙었습니다).

Web UI 생성 폼에는 정적 IP·게이트웨이·DNS 입력이 없습니다. 서버가 읽지 않아 조용히 버려지던 값이기 때문입니다. 상세 요약 탭의 Bridge는 실제 NIC 설정을 보여 주고, 옵션 탭의 권한 값은 서버가 준 값입니다. AppArmor처럼 서버가 주지 않는 값은 "알 수 없음"으로 표시합니다.

### 4.2 라이프사이클

```bash
# 시작 (cgroup v2 리소스 제한 자동 적용)
pcvctl container start app-ctr

# 중지 (오퍼레이션 잠금으로 동시 실행 방지)
pcvctl container stop app-ctr

# 삭제 (ZFS 데이터셋 정리가 끝나야 성공)
pcvctl container destroy app-ctr

# 목록 조회
pcvctl container list
```

REST API:

```bash
# 컨테이너 목록
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/containers | python3 -m json.tool

# 컨테이너 시작
curl -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/containers/app-ctr/start

# 컨테이너 중지
curl -X POST -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/containers/app-ctr/stop
```

같은 컨테이너에 다른 작업이 진행 중이면 작업 잠금 충돌로 거절됩니다. `container.create`, `container.destroy`, `container.start`, `container.stop`과 nic 워커(`nic.attach`, `nic.detach`, `set_bandwidth`)의 잠금 충돌은 모두 JSON-RPC `-32004`, REST **409**로 통일되어(볼륨 attach/detach, clone, env.set/delete, set_limits, snapshot.rollback과 같은 "지금 사용 중, 잠시 뒤 다시" 의미), 자동화가 한 코드로 재시도를 가릅니다. 응답 문구는 `Container is busy` 또는 작업 잠금 사유 문자열입니다. HTTP 500을 서버 오류로 처리하던 자동화는 409 재시도 분기로 옮깁니다.

컨테이너는 데몬이 직접 띄우지 않고 전용 템플릿 유닛 `purecvisor-lxc@<이름>.service`로 시작합니다(2.1.0). 그래서 컨테이너 안의 systemd 서비스(networkd·resolved·journald 등)가 데몬 서비스의 보안 필터를 물려받지 않고 정상 동작합니다. 시작 상태는 `systemctl status purecvisor-lxc@app-ctr`로도 볼 수 있습니다.

- **유닛 파일이 없으면 시작하지 않습니다.** 패키지(deb)는 유닛을 자동으로 설치합니다. deb 밖 설치는 `packaging/systemd/purecvisor-lxc@.service`를 `/etc/systemd/system/`에 두고 `systemctl daemon-reload`를 한 번 실행해야 합니다. 없으면 오류 문구가 이 안내를 줍니다.
- 시작 실패의 원인은 컨테이너 로그 파일에 남습니다(4.11). 시작 전체는 최악 약 31초 안에 끝납니다.
- 유닛 표시의 `Tasks`·메모리는 컨테이너를 세지 않습니다 — root가 띄운 LXC는 컨테이너를 유닛 cgroup 밖에 둡니다. 자원은 `pcvctl container list`·메트릭(4.5)으로 봅니다.
- 실행 중인 컨테이너가 있을 때 `daemon.conf`의 `lxc_path`를 바꾸는 것은 지원하지 않습니다(정상 정지가 실패합니다). 바꾼 경로는 다음 시작부터 적용됩니다.
- **업그레이드 직후에는 컨테이너를 한 번 stop/start 하세요.** 업그레이드 전 데몬이 띄워 실행 중이던 컨테이너는 다음 정지·시작 전까지 전용 유닛 밖에서 돕니다.
- 삭제 도중 ZFS 데이터셋이 사용 중이면 데몬이 1초 간격으로 최대 10번 다시 시도합니다. 그래도 남으면 작업이 **실패**로 끝나며 `container '<이름>' removed but ZFS dataset '<데이터셋>' remains: <원인>; run container.destroy again to finish cleanup` 문구를 줍니다. 같은 이름으로 `pcvctl container destroy`를 다시 실행하면 남은 데이터셋을 정리합니다.

### 4.3 명령 실행

```bash
# 단일 명령
pcvctl container exec app-ctr "hostname -I"

# 패키지 업데이트
pcvctl container exec app-ctr "apt update && apt upgrade -y"

# 서비스 상태 확인
pcvctl container exec app-ctr "systemctl status nginx"
```

`container.exec`의 명령은 컨테이너 안 `/bin/sh -c`로 실행됩니다. 실행은 **60초**, 출력은 **4 MiB**까지이며, 넘으면 데몬이 명령 프로세스 그룹을 종료하고 `exec failed (timeout or error): …`로 답합니다(REST·CLI 응답 대기는 65초, UI 터미널은 70초; UI 요약·자원 탭의 자동 호출은 기본 12초). 컨테이너 안에서 실행되는 명령은 데몬 환경을 받지 않고 `PATH=/usr/sbin:/usr/bin:/sbin:/bin`으로 시작합니다.

> **한계**: `container.exec`와 헬스 `exec` 프로브는 `lxc-attach`로 실행되므로 그 프로세스는 데몬의 보안 필터를 그대로 상속합니다(예: 32비트 바이너리는 SIGSYS, `setarch`는 EPERM). 컨테이너 안에서 서비스로 도는 프로세스(init 하위)는 영향을 받지 않습니다.

### 4.4 리소스 제한 (cgroup v2)

컨테이너의 vCPU 수 N은 **CPU를 최대 N코어까지 쓸 수 있다**는 상한입니다(`cpu.max`가 `N×100000 100000`). `set_limits`로 CPU를 정하면 그 값이 우선하고 재시작 뒤에도 유지됩니다. 실행 중 컨테이너에도 `set_limits`가 cgroup에 바로 반영되며, 반영에 실패하면 오류로 답합니다.

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

`name`이 컨테이너 이름 규칙을 어기면 −32602 `Invalid container name`입니다. 같은 컨테이너에 시작·정지·스냅샷·복제·헬스 재시작이 도는 동안에는 −32004이므로 끝난 뒤 다시 합니다(설정 파일을 롤백과 겹쳐 쓰지 않게 컨테이너별 작업 잠금을 잡습니다).

Web UI 자원 제한 화면은 CPU 상한(%), CPU Weight(1-10000), 메모리 상한(MB)을 받습니다. 빈 칸은 바꾸지 않습니다. 서버가 쓰지 않는 CPU Shares·Quota·Swap 입력은 없앴습니다. 꺼진 컨테이너는 다음 시작 때 적용된다고 알려 줍니다.

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
 Container Metrics: app-ctr
  State     : RUNNING
  CPU %     : 8.3%
  Mem Used  : 256.0 MB / 1024.0 MB
  Net RX    : 15.20 MB
  Net TX    : 3.10 MB
  IP        : 192.0.2.15
  Init PID  : 12345
```

CPU %는 같은 컨테이너의 직전 `container.metrics` 조회(어느 클라이언트든)와의 차이 — 누적 CPU 시간 증가 ÷ 흐른 시간 × 100(100 = 한 코어를 다 씀, 여러 코어면 100을 넘는다) — 로 계산한다. 그래서 첫 조회와 컨테이너를 다시 켠 뒤 첫 조회는 0이다(직전 조회 때와 컨테이너 init 프로세스가 다르면 직전 값과 견주지 않는다). 응답(`--format=json`·REST)에는 누적 CPU 시간 `cpu_time_ns`(정수, 나노초)도 있어 직접 차이를 낼 수 있다.

### 4.6 컨테이너 스냅샷

```bash
# 스냅샷 생성
pcvctl container snapshot create app-ctr v1

# 스냅샷 목록
pcvctl container snapshot list app-ctr

# 롤백
pcvctl container snapshot rollback app-ctr v1

# 삭제
pcvctl container snapshot delete app-ctr v1
```

**롤백은 실행 중 컨테이너를 멈췄다가 다시 켭니다.** 실행 중(또는 일시 정지)이면 정상 정지(최대 30초) → 강제 정지 → 정지 확인 → 되돌림 순으로 하고, 응답·감사 뒤 정식 `container.start`로 다시 켭니다(재시작 감사 행위자는 롤백 요청자). 응답은 되돌림 결과이고 재시작 결과는 `container.start` 감사와 `job.complete` 이벤트로 봅니다. 실패 응답 끝에는 다시 켜기를 요청했으면 ` (container restart requested; see container.start result)`가 붙습니다. 정상 정지를 기다리므로 응답이 늦습니다 — 응답 상한은 REST·CLI 75초, Web UI 80초입니다. 되돌림은 스냅샷 뒤의 rootfs·config 변경을 버리지만 붙어 있던 rw 호스트 볼륨의 데이터는 되돌리지 않습니다.

- 스냅샷이 없으면 컨테이너는 그대로이고 −32603 `Snapshot '<s>' not found for container '<n>' (container left untouched)`입니다.
- 시작·정지 중이거나 상태를 읽지 못하면 아무것도 하지 않고 −32603 `container '<n>' is <상태>; nothing was rolled back (retry when it is running or stopped)`입니다. 끝난 뒤 다시 합니다.
- 정지에 실패하면 되돌리지 않고 컨테이너는 실행 중 그대로입니다(`… could not be stopped for the rollback: …; nothing was rolled back`).
- 롤백 중에는 같은 컨테이너의 `env.set`·`env.delete`·`set_limits`·볼륨 attach·detach가 −32004입니다.

### 4.7 컨테이너 복제

복제는 원본 데이터셋을 **ZFS send/recv로 통째로 복사**합니다(전체 복사 — 원본 크기만큼 시간·공간을 씁니다). 복제본은 원본이나 원본 스냅샷에 의존하지 않으므로 복제 뒤 원본을 지워도 됩니다. 원본은 정지해 있어야 합니다(실행 중이면 `stop the source container before cloning`).

```bash
# 전체 복사 (ZFS send/recv) — 원본은 정지 상태
echo '{"jsonrpc":"2.0","method":"container.clone","params":{"source":"app-ctr","dest":"app-ctr-clone"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

- **볼륨 줄 상속**: 복제본 config는 원본의 호스트 볼륨 줄(`lxc.mount.entry`)을 그대로 가집니다 — 복제본도 같은 호스트 폴더를 붙입니다. 접수 응답의 `volume_entries`가 그 줄 수입니다(원본 config를 읽었을 때만 있습니다). 원치 않으면 복제 뒤 `container.volume.detach`합니다.
- **권한**: OPERATOR도 자기 컨테이너를 복제할 수 있지만, privileged 원본과 **호스트 볼륨 줄이 있는 원본**(원본 config를 읽지 못한 경우 포함)의 복제는 ADMIN만 합니다. 아니면 −32006 `cloning a container with host volumes requires admin role`입니다(privileged는 `cloning a privileged container requires admin role`). NIC가 테넌트 허용 밖 브리지(ADMIN이 `container.nic.attach`로 붙인 것 등)에 붙은 원본도 OPERATOR는 복제할 수 없습니다 — −32006 `cloning a container connected to bridge '<브리지>' requires admin role: the bridge is not allowed for this account`. 규칙은 `container.create`의 `network_bridge` 판정과 같고, 생략 기본 `virbr0`은 허용됩니다.
- **소유자**: 복제본의 소유자는 요청자입니다. UDS 직결 ADMIN 요청처럼 요청자가 없으면 소유자를 기록하지 않습니다(생성과 같습니다 — ADMIN 전용 컨테이너).
- **임시 스냅샷**: 복사하는 동안 원본에 `@pcv-clone-<dest>` 스냅샷이 잠시 보입니다(`container.snapshot.list`). 끝나면 지웁니다. 지우지 못해도 복제는 성공이고 남은 스냅샷은 `container.snapshot.delete`로 지웁니다.
- **바뀌는 것**: `lxc.rootfs.path`, `lxc.uts.name`, NIC `hwaddr`(새 `00:16:3e:XX:XX:XX` — 사용자가 지정한 값도 새 값), 컨테이너 디렉터리 안 훅·`lxc.mount.fstab` 경로, rootfs `/etc/hostname`. 원본 config에 `lxc.hook.clone` 줄이 있으면 거절합니다(PureCVisor는 복제 훅을 실행하지 않습니다 — 그 줄을 지운 뒤 다시 합니다). `zfs send`는 원본에 손으로 건 ZFS 속성(quota 등)을 옮기지 않습니다.
- **실패하면**: 만든 데이터셋·대상 디렉터리·임시 스냅샷을 지우고 `clone '<dest>' failed at <단계>: <원인>; clone removed`로 끝납니다. 같은 이름으로 다시 복제할 수 있습니다. 못 지운 것이 있으면 `…; could not remove <이름>; remove it manually`이므로 그것을 손으로 지웁니다.
- 원본·대상에 다른 작업이 돌면 접수 전 −32004입니다.

### 4.8 볼륨 마운트

`readonly`를 적지 않으면 읽기 전용으로 붙습니다(2.1.0부터, 2.0.4 이하는 읽기·쓰기). 아래 예시는 쓰기를 허용하려고 `"readonly": false`를 명시한 것입니다.

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

#### 컨테이너 볼륨 원본 규칙

`host_path`(원본)는 데몬이 루트부터 한 성분씩 **심볼릭 링크를 따르지 않고** 열어 확인하고, 확인한 그 디렉터리를 붙인다(실행 중 attach는 확인한 fd 그대로, 정지 attach는 정규 경로를 config에 적는다). 그래서 **링크를 지나는 경로는 쓸 수 없다 — 실제 경로를 쓴다**(예: `/var/run/…` 대신 `/run/…`, merged-usr 배포판의 `/lib/…` 대신 `/usr/lib/…`). 데몬이 켤 때마다(`container.start`, 헬스 자동 재시작, 롤백 재시작) config에 남은 원본을 같은 방식으로 다시 확인하고, **확인한 그 원본을 root 전용 staging 자리(`/run/purecvisor-volstage/<이름>/`)에 붙인 뒤 liblxc에는 staging 경로만 넘긴다**(데몬이 시작마다 쓰는 런타임 config `/run/purecvisor/lxc-runtime/<이름>/config`, 전용 유닛의 `lxc-start -f`). 그래서 읽기 전용 볼륨은 컨테이너 안에서 쓰기 가능으로 바꿀 수 없고, 확인과 마운트 사이에 원본이 바뀌는 창도 없다. 영속 config(`<lxc_path>/<이름>/config`)는 실제 원본을 그대로 적는다. 컨테이너 안 `reboot`는 같은 staging을 다시 붙인다. config에 들어간 시작 검사 훅(`lxc.hook.pre-start = /usr/local/bin/pcvctl container volume-guard-hook`)은 볼륨 원본이 그 컨테이너 staging 밖이면(데몬 밖 `lxc-start`) 또는 상대 경로이면 시작을 거절하고, staging 경로에 링크가 끼었는지 다시 본다. 시작은 볼륨마다 컨테이너 안 대상 자리를 실행 중 attach와 같은 규칙으로 링크 없이 걷고 없으면 만든다(대상 경로가 컨테이너 안 링크를 지나면 시작 실패).

**detach 계약**: `container.volume.detach` 성공은 **컨테이너 프로세스의 마운트 NS 어디에도 그 볼륨이 보이지 않는다**는 뜻이다. 실행 중이면 호스트 쪽 마운트와 컨테이너 안쪽 사본(시작 때 붙은 볼륨 포함)을 떼고, 컨테이너의 모든 프로세스·스레드를 확인한 뒤에만 그 줄의 staging 마운트를 떼고 config 줄을 지운다. 남았거나 확인하지 못하면 −32002이고 config 줄은 그대로다 — 컨테이너를 정지한 뒤 같은 detach를 한 번 더 한다. 열어 둔 파일 등 프로세스 없이 쥔 참조까지 회수하는 완전한 떼기는 정지 뒤 detach다. 실행 중 확인은 컨테이너 root가 일부러 숨기거나 피한 사본까지 보장하지 않으므로, 신뢰하지 않는 컨테이너의 볼륨은 정지한 뒤 detach한다. 한 번의 확인이 오래 걸리면(5초 넘게) −32002로 끝나니 큰 컨테이너도 정지 뒤 detach한다. 정지 상태 detach는 호스트에 남은 마운트·staging을 정리한 뒤 config 줄을 지운다.

**볼륨 자리 규칙**: `container_path`는 빈 성분·`.`를 지운 한 철자로 저장한다(`/mnt//./data/` → `mnt/data`). 이미 붙은 볼륨을 가리는 자리(그 볼륨 자리의 조상)에는 붙이지 않는다. 실행 중에는 이미 볼륨이 있는 자리나 다른 볼륨 안쪽 자리에 붙이지 않는다 — 정지한 뒤 attach한다. 정지 상태에서 다른 볼륨 안쪽에 붙이려면 그 바깥 볼륨의 호스트 원본에 같은 종류의 자리를 먼저 만든다(데몬은 사용자 데이터에 자리를 만들지 않는다).

| 오류 | 뜻 | 조치 |
|---|---|---|
| −32602 `host_path must not traverse symbolic links` | 원본 경로에 링크·`..`가 있다 | 링크를 푼 실제 경로로 다시 attach한다 |
| −32602 `host_path must be an absolute path` | 상대 경로다 | `/`로 시작하는 절대 경로를 쓴다 |
| −32602 `host_path/container_path must not contain whitespace, control characters or backslashes` | 공백·제어 문자·`\`가 든 경로다 | 그런 글자가 없는 경로로 바꾼다 |
| −32002 `volume source changed since attach: <container_path>` | 시작 거절 — attach 뒤 그 볼륨 원본의 어느 성분이 링크로 바뀌었다 | 누가 링크를 만들었는지(특히 다른 컨테이너의 쓰기 볼륨 아래인지) 확인하고, `container.volume.detach`로 그 항목을 뗀 뒤 실제 경로로 다시 attach하고 `container.start`한다 |
| −32002 `volume source changed since attach: (volume guard fingerprint could not be reset)` | 시작 거절 — 데몬이 컨테이너 디렉터리에 남은 옛 시작 기록 파일(`purecvisor.volgen`)을 지우지 못했다(볼륨이 없는 컨테이너에도 해당. 데몬은 이 파일을 쓰지 않고 남은 것만 지운다) | 컨테이너 디렉터리에 쓸 수 있는지(디스크·권한) 확인한 뒤 다시 `container.start`한다 |
| −32602 `host_path is already attached to this container at <container_path>` | 같은 호스트 경로가 이미 이 컨테이너에 붙어 있다 | 이미 붙은 자리를 그대로 쓰거나, 그 자리를 detach한 뒤 새 자리로 attach한다 |
| −32602 `container_path would cover another attached volume: <자리>; detach it first` | 새 자리가 이미 붙은 볼륨을 가린다(그 볼륨 자리의 조상) | 안쪽 볼륨을 먼저 detach하거나 다른 자리를 쓴다 |
| −32002 `container_path already has a volume attached while the container is running: …` · `container_path is inside another attached volume while the container is running: …` | 실행 중 attach 자리에 이미 볼륨이 있거나 다른 볼륨 안쪽이다(붙여도 컨테이너에 보이지 않는다) | 정지한 뒤 attach한다(다른 볼륨 안쪽이면 그 원본에 자리를 먼저 만든다) |
| −32602 `container_path is inside the attached volume at <바깥>, but <나머지> does not exist in that volume's source; create it there first` | 정지 attach 자리가 다른 볼륨 안쪽인데 바깥 볼륨 원본에 그 자리가 없다 | 바깥 볼륨의 호스트 원본에 `<나머지>`를 새 원본과 같은 종류로 만든 뒤 다시 attach한다 |
| −32602 `container_path must name an entry below the root` | 자리가 컨테이너 루트(`/`)다 | 루트 아래 경로를 쓴다 |
| −32002 `volume is still mounted inside the container: <container_path>; stop the container, then detach again` | detach 뒤에도 컨테이너 안 어딘가(다른 마운트 NS 포함)에 볼륨이 보인다. config 줄은 그대로다 | 컨테이너를 정지한 뒤 같은 detach를 한 번 더 한다 |
| −32002 `cannot verify that the volume is gone from the container: <사유>; …` · `cannot identify the volume source on the host; …` · `the same host path is attached twice to this container; …` · `another attached volume is mounted under this path: …` · `cannot identify the host source of another attached volume: …` | 실행 중 detach가 떼거나 확인할 수 없다(확인 상한 — 스레드 4,096·하위 cgroup 1,024·5초 초과, 원본을 식별하지 못함, 옛 config의 같은 원본 중복, 대상 아래 다른 볼륨) | 정지한 뒤 detach한다(대상 아래 다른 볼륨이면 안쪽부터 떼도 된다) |
| −32603 `volume detached from the running container, but its staging mount was not removed: …` · `…, but its config line was not removed: …; run detach again` | 떼기는 끝났지만 뒤 단계가 실패했다 | 같은 detach를 다시 한다 |
| 시작 실패 `container '<n>' has leftover volume mounts under its rootfs that could not be removed: …` | 호스트에 남은 볼륨 마운트를 떼지 못해 켜지 않았다(fail-closed) | 문구 끝이 `follow the GUIDE procedure "pre-upgrade stacked volume mounts"`면 아래 절차를 따른다. 아니면 데몬 로그의 `leftover host mount` 줄을 보고 원인을 없앤 뒤 다시 시작한다 |
| 시작 실패 `cannot prepare the volume staging for <container_path>: …` · `staging path <경로> has unexpected type, owner or mode; refusing to start` · `cannot write the runtime config: …` | staging·컨테이너 안 대상·런타임 config를 준비하지 못했다(대상 종류 불일치·링크, 중첩 볼륨의 바깥 원본에 자리 없음 포함). 그 시작에서 만든 staging은 지운다 | 사유대로 고친 뒤 다시 시작한다. staging 성분 불일치는 데몬 밖에서 만든 자리라 그 경로를 지운 뒤 시작한다 |
| −32002 `volume guard hook not installed: /usr/local/bin/pcvctl` | 시작 검사 훅 실행체가 root 소유로 설치돼 있지 않다 | 데몬과 같은 판의 `pcvctl`을 `/usr/local/bin/pcvctl`(root 소유)에 설치한다 |
| −32002 `host_path check needs openat2 (kernel 5.6+)` | 커널이 `openat2`를 모른다 | 커널 5.6 이상을 쓴다 |

컨테이너 안 `reboot`나 데몬 밖 `lxc-start`가 정지로 끝나면(liblxc 로그 `pcv volume guard: …`) 볼륨 원본이 staging 밖이거나(데몬 밖 `lxc-start`), 상대 경로 원본 bind 줄이 있거나, staging 경로에 링크가 끼었다. 데몬 `container.start`로 켠다(그때 다시 확인하고 staging을 새로 만든다). 전용 유닛을 `systemctl start purecvisor-lxc@<이름>`으로 직접 켜는 것은 지원 경로가 아니다 — 데몬이 정지를 처리하지 않은 컨테이너(컨테이너 안 poweroff)는 이전 시작의 staging으로 켜져 그 사이 config 변경이 반영되지 않는다.

**업그레이드 노트(2.0.4 이하 → 2.1.0, 볼륨 마운트 생명주기·롤백·복제)** — 시작 볼륨 staging, 시작 검사 훅, 데몬 밖 `lxc-start` 거절은 2.0.4와 같다. 그 밖의 바뀐 동작은 다음과 같다:

1. **데몬·`pcvctl`·컨테이너 시작 유닛(`purecvisor-lxc@.service`)은 함께 반영하고 함께 되돌린다.** deb는 셋을 함께 설치한다. 유닛 파일이 없으면 2.1.0 데몬은 컨테이너를 시작하지 않는다. **되돌리기 전에 볼륨 컨테이너를 정지한다** — 2.1.0 데몬이 정지 때 staging을 정리한다.
2. **detach는 정말 떨어졌을 때만 성공한다.** 성공은 컨테이너 프로세스의 마운트 NS 어디에도 그 볼륨이 보이지 않는다는 뜻이다. 시작 때 붙은 볼륨도 실행 중 detach로 뗀다. 컨테이너가 다른 마운트 NS에 사본을 쥐고 있거나 열어 둔 파일 등으로 쥔 참조까지 끊으려면 정지한 뒤 detach한다. −32002면 컨테이너를 정지한 뒤 같은 detach를 한 번 더 한다.
3. 업그레이드 전 detach가 남긴 마운트: 컨테이너 안 마운트는 정지로 사라진다. 호스트 쪽 잔여는 다음 시작·삭제·복제·정지 상태 detach가 정리하고, 정리하지 못하면 시작이 실패한다(fail-closed). 반영 때 실행 중 컨테이너는 멈추지 않는다.
4. 실행 중 스냅샷 롤백은 컨테이너를 멈췄다가(정상 정지에 최대 30초) 되돌린 뒤 다시 켠다. 응답이 늦어진다. 재시작 결과는 `container.start` 감사·이벤트로 본다.
5. 같은 컨테이너에 다른 작업(시작·정지·스냅샷·복제·헬스 재시작)이 도는 동안 `env.set`·`env.delete`·`set_limits`·볼륨 attach·detach는 −32004다. 끝난 뒤 다시 한다.
6. 같은 호스트 경로를 한 컨테이너에 두 번 붙일 수 없다. 기존 config의 중복은 시작에는 영향이 없고, detach가 어느 쪽인지 가릴 수 없으면 −32002 → 정지 뒤 detach.
7. 복제는 전체 복사(ZFS send/recv)이고 원본의 볼륨 줄을 그대로 갖는다. **호스트 볼륨이 붙은 컨테이너와 테넌트 허용 밖 브리지에 NIC가 붙은 컨테이너의 복제는 ADMIN만 할 수 있다**(OPERATOR −32006). 복제본의 소유자는 요청자다. 복제 중 원본에 임시 스냅샷이 잠시 보인다.
8. 헬스 exec target에 공백을 넣으면 −32602다(인자는 지원하지 않는다).
9. 컨테이너 메트릭의 메모리·CPU가 실제 값으로 바뀐다. CPU %는 직전 조회와의 차이라 첫 조회와 재시작 뒤 첫 조회는 0이고, 응답에 누적 CPU 시간 `cpu_time_ns`(정수, ns)가 더해졌다.
10. **볼륨 자리 규칙이 엄격해진다.** 다른 볼륨을 가리는 자리는 상태와 무관하게 −32602다. 실행 중에는 이미 볼륨이 있는 자리와 다른 볼륨 안쪽 자리를 −32002로 거절한다 — 정지한 뒤 attach한다. 정지 attach로 다른 볼륨 안쪽에 붙이려면 바깥 볼륨 원본에 자리를 먼저 만든다(없으면 −32602). 시작도 기존 config의 중첩 줄을 같은 규칙으로 보고, 바깥 원본에 자리가 없으면 liblxc 앞에서 실패한다. attach가 config에 적는 `container_path`는 빈 성분·`.`를 지운 한 철자다.
11. **실행 중 detach가 떼기 전에 거절하는 경우가 있다.** 대상 아래에 다른 볼륨이 있거나 남는 다른 볼륨의 원본을 식별하지 못하면 −32002다. 안쪽 볼륨부터 떼거나 정지한 뒤 detach한다.
12. **업그레이드 전 겹친 볼륨 마운트.** 업그레이드 전 판이 한 자리에 실행 중 attach를 두 번 해 생긴 겹침은 데몬이 떼지 않는다 — 시작·삭제·복제·정지 상태 detach가 아래 절차를 가리키며 실패한다. 손으로 경로 umount하지 말고 그 절차를 따른다.
13. **스냅샷 롤백이 상태를 확인한다.** 시작·정지 중이거나 상태를 읽지 못하면 아무것도 하지 않고 실패한다 — 끝난 뒤 다시 한다. 응답 상한은 REST·CLI 75초, Web UI 80초다.
14. **잠금·검증 응답.** `container.clone`도 원본·대상에 다른 작업이 있으면 접수 전 −32004다. `container.set_limits`는 이름이 컨테이너 이름 규칙을 어기면 −32602 `Invalid container name`이다.
15. **보안 사건 분류.** 데몬이 `lxc-copy`를 실행하면 이제 WARN 사건이다(PureCVisor는 더 부르지 않는다). 데몬이 띄우는 `/usr/local/bin/pcvctl`(실행 중 detach의 정리 자식)은 INFO다.
16. **데몬 밖 유닛 시작은 지원 경로가 아니다.** 컨테이너 안 poweroff처럼 데몬이 정지를 처리하지 않은 뒤 `systemctl start purecvisor-lxc@<이름>`을 직접 부르면 이전 시작의 staging으로 켜진다 — 그 사이 config 변경은 반영되지 않는다. 시작은 `container.start`로 한다.

#### 업그레이드 전 겹친 볼륨 마운트 (pre-upgrade stacked volume mounts)

업그레이드 전 판은 한 `container_path`에 실행 중 attach를 두 번 하면 두 마운트를 겹쳐 걸었고, 그 마운트는 호스트 원본과 같은 전파 묶음(shared)에 들어 있었다. 이런 겹침이 남은 컨테이너는 시작·삭제·복제·정지 상태 detach가 `… stacked volume mounts at <호스트 경로> include a shared mount from before the upgrade and were left in place; unmounting them directly can detach host mounts — follow the GUIDE procedure "pre-upgrade stacked volume mounts"`로 실패한다(실행 중 detach는 −32002 같은 문구). 데몬은 이 겹침을 떼지 않는다 — 덮인 shared 마운트는 경로로 열 수 없고, 위의 것을 그대로 떼면 해제가 호스트 원본 쪽 마운트로 번진다. **`<호스트 경로>`를 바로 `umount`하지 않는다.** 실 LXC 시험에서 잰 절차는 다음과 같다(`<lxc_path>` 기본값 `/var/lib/purecvisor/lxc`, `<이름>` = 컨테이너 이름).

1. 컨테이너를 정지한다(정지 상태에서 잰 절차다).
2. 컨테이너 디렉터리 아래 마운트 전체를 private로 바꾼다 — 이제 떼어도 호스트 쪽으로 번지지 않는다.
   ```bash
   sudo mount --make-rprivate <lxc_path>/<이름>
   ```
3. 문구의 `<호스트 경로>`를 위에서부터 하나씩 뗀다. 마운트가 남지 않을 때까지(`findmnt <호스트 경로>`가 빈 출력) 되풀이한다.
   ```bash
   sudo umount <호스트 경로>
   ```
   `target is busy`가 나오면 그 경로 아래에 호스트 마운트가 번져 들어온 자식이 있는 것이다(예: 원본 아래 호스트에서 건 마운트의 사본 `<호스트 경로>/sub`). 그 자식 경로를 먼저 `sudo umount <호스트 경로>/sub`로 뗀 뒤 3을 계속한다 — 2에서 private로 바꿨으므로 자식을 떼도 호스트 원본 쪽 마운트는 그대로다.
4. 전파를 원래대로 되돌린다 — 이것을 빼면 뒤의 실행 중 attach가 컨테이너 안으로 번지지 않는다.
   ```bash
   sudo mount --make-rshared <lxc_path>/<이름>
   ```
5. `container.start`로 켠다. 같은 자리 볼륨 줄들은 staging으로 config 순서대로 겹쳐 붙고 뒤 줄이 보인다.
6. 업그레이드 전 판은 둘째 볼륨을 **첫 볼륨의 호스트 원본 경로 위에도** 걸어 두었을 수 있다. `findmnt <첫 볼륨 host_path>`에 볼륨 마운트가 보이면 `sudo umount <첫 볼륨 host_path>`로 뗀다(측정에서 원본 아래 호스트 마운트는 그대로였다. 측정은 5 뒤에 뗐다).

이 절차는 kernel 7.0·LXC 6.0.6 실 LXC 시험에서 확인했다 — 호스트 원본 아래 마운트가 어느 단계에서도 떨어지지 않았고, 다음 시작은 RUNNING, 뒤 실행 중 attach는 컨테이너 안에서 보였다.

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

같은 컨테이너에 시작·정지·스냅샷·복제·헬스 재시작이 도는 동안 `env.set`·`env.delete`는 −32004입니다. 끝난 뒤 다시 합니다.

### 4.10 헬스체크

컨테이너에 주기적 헬스체크를 등록할 수 있습니다.

```bash
# 헬스체크 등록
echo '{"jsonrpc":"2.0","method":"container.health.set","params":{
  "name": "app-ctr",
  "type": "http",
  "target": "http://localhost:8080/health",
  "interval_sec": 30,
  "timeout_sec": 5,
  "failure_threshold": 3,
  "auto_restart": false
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 헬스 상태 확인
echo '{"jsonrpc":"2.0","method":"container.health.get","params":{
  "name": "app-ctr"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

`name`은 컨테이너 이름 규칙(1–64자 `[a-zA-Z0-9_-]`)을 지켜야 하고, `target`은 type별로 검증해 아니면 −32602입니다 — `tcp`는 1–65535 십진 포트, `http`는 `http://` 또는 `https://`로 시작하고 공백·제어 문자가 없는 URL, `exec`는 비어 있지 않고 `-`로 시작하지 않으며 제어 문자·공백이 없는 프로그램 경로 하나(255바이트까지)입니다 — 인자는 지원하지 않으므로 공백이 있으면 −32602 `Invalid exec target: a single program path without spaces (arguments are not supported)`입니다(인자가 필요하면 컨테이너 안에 스크립트를 두고 그 경로를 씁니다). `type`은 `tcp`(`target`은 포트), `http`(`target`은 URL), `exec`(`target`은 컨테이너 안에서 실행할 명령) 중 하나입니다. `interval_sec`는 5초 이상(기본 30), `timeout_sec`는 1~30초로 제한됩니다(기본 5). `exec` 프로브도 이 시간을 넘기면 실패로 셉니다(이전 판은 상한이 없었습니다). `failure_threshold`만큼 연속 실패하면 `auto_restart`가 켜진 컨테이너를 정지 후 다시 시작합니다. 프로브는 워커에서 동시에 최대 4개까지 실행하며 느린 프로브가 데몬을 막지 않습니다. 상태값은 `container.health.get` 응답의 `healthy`(true/false)와 `consecutive_failures`·`restart_count`입니다. `failure_threshold`가 1보다 작으면 1로 맞춥니다. **같은 이름으로 다시 등록(갱신)하면 새 등록으로 보고 `restart_count`·연속 실패 수가 0으로 돌아갑니다.** CLI는 `pcvctl container health-set … --timeout <초>`로 `timeout_sec`을 지정합니다. 자동 재시작도 컨테이너 시작 유닛을 거칩니다.

> **자동 재시작 도중 데몬이 종료 중이면** 정지(stop) 뒤 시작(start)을 건너뛰어 컨테이너가 **정지 상태로 남습니다**. 데몬을 다시 올린 뒤 운영자가 직접 시작합니다(`pcvctl container start <name>`).

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

# CLI는 줄을 그대로 출력한다 (없으면 "(no log lines yet)")
pcvctl container logs app-ctr
```

OPERATOR의 `container.logs`·`container.memory.stats`·`container.health.check`는 자기가 만든 컨테이너만 통과한다(남의 것은 403, ADMIN은 전부). `container.metrics`는 `vm.metrics`처럼 VIEWER 전체 조회다.

`container.logs`는 **LXC의 기동·정지 기록**(`<lxc_path>/<이름>/<이름>.log`, 시작 실패의 원인 포함)입니다. 컨테이너 안 프로그램의 콘솔 출력은 여기에 없습니다 — `lxc-console`이나 `container.exec`로 봅니다. 아직 한 번도 유닛으로 시작하지 않아 로그 파일이 없으면 오류가 아니라 빈 결과(`lines: []`, `total: 0`)입니다. 로그는 시작 전에 1 MiB를 넘었으면 한 판만 남기고 새로 시작하며, 1 MiB를 넘는 로그의 `total`은 끝 1 MiB 기준입니다. OPERATOR는 자기 컨테이너의 로그만 읽을 수 있습니다(−32006).

### 4.12 CRIU 체크포인트/복원

공개판에는 컨테이너 CRIU 체크포인트·복원 명령이 없습니다(CLI·RPC·REST 미제공).

### 4.13 Seccomp 프로파일

공개판에는 컨테이너별 Seccomp 프로파일을 설정·조회하는 명령이 없습니다(CLI·RPC·REST 미제공).

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
  "nic_name": "eth1"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

실행 중 컨테이너의 NIC 조회·추가·분리는 컨테이너 안에서 짧은 명령을 실행하므로 각각 시간 상한이 있습니다(조회 3초, 분리 5초, 실행 중 배선 전체 6초).

- `container.nic.list` 원소의 `ipv4_unavailable`이 `true`면 컨테이너는 실행 중인데 IP를 시간 안에 조회하지 못한 것이며 `ipv4`는 빈 문자열입니다. 컨테이너가 꺼져 있으면 `false`입니다.
- `container.nic.detach` 응답에 `warning`이 있으면 설정에서는 제거됐지만 실행 중 인터페이스 제거 명령이 0이 아닌 값으로 끝난 것입니다.
- 실행 중 `nic.attach`가 실패하면 `config saved; live wiring not confirmed: <단계>: <원인>`(−32000)으로 답합니다 — 설정에는 저장됐으나 실행 중 컨테이너에는 연결을 확인하지 못했다는 뜻이며, 컨테이너를 다시 시작하면 설정대로 연결됩니다. 분리의 같은 상황은 `config removed; live interface removal not confirmed: <원인>`입니다.
- `hwaddr`는 `xx:xx:xx:xx:xx:xx` 형식(16진 두 자리 6개)만 받고 아니면 −32602이며, 브리지 이름 등 설정 줄에 들어가는 값의 개행·제어 문자는 거절됩니다. 실행 중 NIC의 호스트 쪽 인터페이스 이름은 컨테이너마다 다릅니다(같은 번호의 NIC를 두 컨테이너에 붙여도 충돌하지 않습니다). 업그레이드 전에 붙인 옛 이름(`ethN-host`)의 NIC는 분리하면 컨테이너 안 제거만 하고, 호스트에 남은 옛 이름은 다른 컨테이너 것일 수 있어 자동으로 지우지 않습니다.
- 같은 컨테이너에 다른 변경 작업(시작 등)이 진행 중이면 −32004 잠금 오류(REST 409, `VM '<이름>' is already locked (op: <작업>, pid: <PID>)` 또는 `Container is busy`)입니다.

### 4.15 격리 방식과 호스트 준비

컨테이너는 2.1.0부터 기본이 **unprivileged**입니다. 컨테이너 안의 root는 호스트에서 일반 uid(`rootless_uid_start`부터, 기본 100000)로 보이므로 컨테이너가 침해되어도 호스트 root가 아닙니다. 이전 판에서 만든 컨테이너는 그대로이며, `container.list`·CLI `pcvctl container list`의 `PRIV` 열·UI 목록 배지로 privileged와 구별합니다.

**호스트 준비** — 기본 생성은 root에게 위임된 범위가 있어야 하고 다른 계정의 범위와 겹치지 않아야 합니다. `/etc/subuid`와 `/etc/subgid` 두 파일에 같은 줄을 둡니다.

```text
root:100000:65536
```

겹침 확인: `awk -F: '$1!="root" && $1!="0"' /etc/subuid /etc/subgid` 결과의 범위가 `rootless_uid_start`부터 65536개와 한 uid라도 겹치면 안 됩니다. 겹치면 둘 중 하나로 풉니다.

1. 다른 계정의 범위를 비어 있는 곳으로 옮깁니다: `sudo usermod --del-subuids 100000-165535 --del-subgids 100000-165535 <계정>; sudo usermod --add-subuids 200000-265535 --add-subgids 200000-265535 <계정>`. 그 계정에 rootless podman·docker 저장소가 있으면 `podman system migrate` 등으로 다시 정리해야 합니다. 그 뒤 `root:100000:65536`을 두 파일에 더합니다.
2. 또는 `daemon.conf`의 `[container]`에서 `rootless_uid_start = 1000000` 같은 비어 있는 범위로 바꾸고 `root:1000000:65536`을 두 파일에 더합니다.

준비가 안 되면 `container.create`는 privileged로 조용히 떨어지지 않고 **즉시 −32002**로 거절합니다(메시지에 다른 계정 이름은 없습니다).

| 거절 원인 | 메시지 요지 | 해결 |
|---|---|---|
| `NO_ROOT_RANGE` | `… must delegate <시작>:<개수> to root. Add 'root:<시작>:<개수>' to both files.` | 두 파일에 root 줄을 더합니다 |
| `OVERLAP` | `another account's subordinate id range overlaps <시작>-<끝>. …` | 위 1번(다른 계정 범위 이동) 또는 2번(`rootless_uid_start` 변경) 뒤 root 줄 추가 |
| `BAD_RANGE` | `[container] rootless_uid_start=… rootless_uid_count=… is not a valid range …` | `count ≥ 65536`, `start ≥ count`, `start + count ≤ 4294967295`가 되게 `daemon.conf`를 고칩니다 |

원인이 둘 이상이면 `BAD_RANGE` → `OVERLAP` → `NO_ROOT_RANGE` 순서로 하나만 알립니다.

**privileged 요청**(ADMIN 전용): `pcvctl container create <name> --privileged`, 또는 RPC `container.create`의 `"rootless": false`. ADMIN이 아니면 −32006입니다. 호스트 단위로 기본을 바꾸려면 `daemon.conf`의 `[container] rootless = false`(ADMIN 선택)입니다. privileged 컨테이너를 복제하는 것도 ADMIN 전용입니다.

**바인드 마운트** — `container.volume.attach`는 `readonly`를 적지 않으면 읽기 전용입니다. 쓰려면 `"readonly": false`(CLI는 `volume-attach <name> <host_path> <container_path> --rw`)를 명시합니다. unprivileged 컨테이너에서는 호스트 파일이 매핑 밖 uid라 컨테이너 안에서 `nobody`로 보이고, 쓰기를 허용해도 실제 쓰기는 호스트 파일 권한에 달려 있습니다. 실행 중 컨테이너에 붙일 때는 `container_path`에 심볼릭 링크 성분이 있으면 −32602로 거절하므로(`/var/run` 대신 `/run`처럼) 실제 경로를 쓰거나 정지 상태에서 붙입니다. 실행 중 attach는 커널 5.12 이상이 필요합니다.

> **이전 판에서 `rootless:true`로 만든 컨테이너**는 목록에서 `privileged:false`로 보이지만 rootfs가 호스트 uid 0 소유로 풀려 있어 정상 동작하지 않을 수 있습니다. 재생성을 권장합니다.

---

## 5. 스토리지

### 5.1 ZFS 풀 관리

#### 풀 목록

```bash
pcvctl storage pool list
```

출력 예시:

```
 POOL_NAME       │ TOTAL      │ ALLOC      │ FREE       │ HEALTH
─────────────────┼────────────┼────────────┼────────────┼────────────
 pcvpool         │ 7.27T      │ 2.10T      │ 5.17T      │ ONLINE
 datapool        │ 3.62T      │ 1.50T      │ 2.12T      │ ONLINE
─────────────────┴────────────┴────────────┴────────────┴────────────
```

`--format=plain`·`--format=csv`의 머리 칸은 `POOL`·`TOTAL`·`ALLOC`·`FREE`·`HEALTH`입니다.

#### 풀 생성

> **`pcvctl storage pool`에는 `list`만 있습니다.** 풀 생성·스크럽·삭제는 RPC(또는 REST·웹 UI)로만 합니다.

Web UI 풀 생성 화면은 디스크를 공백으로 구분해 받고, 구성(Stripe·Mirror·RAIDZ·RAIDZ2)을 반드시 고르게 합니다. Stripe는 중복이 없어 디스크 하나가 고장 나면 풀 전체를 잃으므로 기본값으로 두지 않습니다. RPC의 `disks`는 문자열 배열이며, 배열이 아니거나 문자열이 아닌 원소가 있으면 `-32602`입니다. `vdev_type`은 `mirror`, `raidz`, `raidz1`, `raidz2`, `raidz3`이고, 빈 문자열이나 생략은 stripe입니다. 그 밖의 값은 `zpool`을 부르기 전에 `-32602`입니다.

RPC 직접 호출:

```bash
echo '{"jsonrpc":"2.0","method":"storage.pool.create","params":{
  "name": "datapool",
  "disks": ["/dev/sdb", "/dev/sdc"],
  "vdev_type": "mirror"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 풀 상태 / 스크럽

```bash
# 풀 헬스 (health, scrub 진행률, 용량)
pcvctl storage health pcvpool

# 스크럽 시작
echo '{"jsonrpc":"2.0","method":"storage.pool.scrub","params":{"name":"pcvpool"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 풀 삭제 (풀 안의 데이터셋·스냅샷이 즉시 영구 삭제됩니다)
echo '{"jsonrpc":"2.0","method":"storage.pool.destroy","params":{"name":"datapool"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 용량 예측

```bash
# 현재 사용량 추세 기반 용량 고갈 예측
pcvctl storage pool-forecast pcvpool
```

### 5.2 ZFS Zvol 관리

#### Zvol 목록

```bash
pcvctl storage zvol list
```

출력 예시:

```
 ZVOL_PATH                                │ VOL_SIZE   │ USED
──────────────────────────────────────────┼────────────┼────────────
 pcvpool/vms/web-prod                     │ 20G        │ 8.50G
 pcvpool/vms/db-prod                      │ 100G       │ 45.2G
 pcvpool/vms/web-staging                  │ 20G        │ 1.20G
──────────────────────────────────────────┴────────────┴────────────
```

`--format=plain`·`--format=csv`의 머리 칸은 `ZVOL_PATH`·`VOL_SIZE`·`USED`입니다.

#### Zvol CRUD

```bash
# zvol 생성
pcvctl storage zvol create pcvpool/vms/new-disk --size 50G

# zvol 삭제
pcvctl storage zvol delete pcvpool/vms/old-disk
```

같은 풀의 `zfs create`·`zfs destroy`는 VM 생성·삭제, 컨테이너, 스냅샷 삭제를 통틀어 한 번에 하나씩 돈다. 긴 삭제가 끝날 때까지 VM·컨테이너 생성·삭제 같은 Job은 최대 5분 기다린다. 응답을 바로 돌려주는 RPC(`storage.zvol.create`·`delete`, `vm.snapshot.delete`·`delete_all`, `container.snapshot.delete`)는 5초만 기다리고 `-32004`("풀 사용 중, 다시 시도")로 끝난다. 어느 쪽이든 시간을 넘기면 그 `zfs` 명령은 돌지 않은 채 실패하므로 다시 시도하면 된다. 여러 단계로 된 Job은 앞 단계가 이미 끝났을 수 있으니 Job 결과를 확인한다. 대기는 지표 `purecvisor_zfs_pool_lock_wait_ms`·`purecvisor_zfs_pool_lock_contentions_total`에서 볼 수 있다.

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

클론 데이터셋을 원본 스냅샷 의존에서 풀어 줍니다. 제품 CLI·RPC는 없고 ZFS 명령을 직접 씁니다. promote 뒤에는 원본이 클론 쪽 스냅샷에 의존합니다.

```bash
# 원본 snapshot 의존 조회 (origin이 '-'가 아니면 클론)
sudo zfs get origin pcvpool/vms/web-staging
# 의존 방향 뒤집기
sudo zfs promote pcvpool/vms/web-staging
```

### 5.3 iSCSI 관리

#### iSCSI 타겟

> **`pcvctl` 에는 `iscsi` 서브커맨드가 없습니다.** iSCSI 타겟 관리는 RPC(또는 REST·웹 UI)
> 로만 합니다 — 아래 예시를 쓰세요. IQN 접두사는 `iqn.2026-03.purecvisor` 이며, 타겟은
> IQN 이 아니라 **`vm_name` 으로 지정**합니다 — IQN(`iqn.2026-03.purecvisor:<vm_name>`)은
> 데몬이 조립합니다.

RPC 직접 호출:

```bash
# 타겟 목록 — 응답 요소는 {vm_name, tid, iqn}.
#   ★ 정본 식별자는 iqn 이다. tid 는 커널 LIO 와 무관한 인메모리 순번(하위 호환 잔존 필드)이므로
#     tid 로 커널 자원을 찾으려 하지 말 것.
echo '{"jsonrpc":"2.0","method":"iscsi.target.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 타겟 생성 — params 는 vm_name·zvol_path 두 개다(IQN 은 데몬이 조립한다).
#   vm_name 은 1~64자 [a-zA-Z0-9_-] 로 제한되며, IQN 은 iqn.2026-03.purecvisor:<vm_name> 이 된다.
echo '{"jsonrpc":"2.0","method":"iscsi.target.create","params":{
  "vm_name": "disk1",
  "zvol_path": "/dev/zvol/pcvpool/iscsi/disk1"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 타겟 삭제 (멱등)
echo '{"jsonrpc":"2.0","method":"iscsi.target.delete","params":{"vm_name":"disk1"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 이니시에이터: 원격 타겟 로그인 / 로그아웃
echo '{"jsonrpc":"2.0","method":"iscsi.connect","params":{
  "target_ip": "192.0.2.50", "vm_name": "disk1"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
echo '{"jsonrpc":"2.0","method":"iscsi.disconnect","params":{
  "target_ip": "192.0.2.50", "vm_name": "disk1"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **타겟 백엔드는 커널 LIO다.** 데몬은 `tgtadm` 같은 외부 명령을 실행하지 않고
> `/sys/kernel/config/target` 아래 configfs 트리를 직접 만든다. 따라서 `tgt` 패키지·데몬이
> 필요 없고, 대신 `target_core_mod`·`iscsi_target_mod`·`target_core_iblock` 커널 모듈이
> 올라와 있어야 한다(`/etc/modules-load.d/purecvisor-lio.conf` — **deb 가 conffile 로
> 설치하며**, deb 를 쓰지 않는 배포에서만 운영자가 직접 만든다). 모듈이 없으면 데몬은
> **degraded 경고만 남기고 기동**하며, 타겟 생성 시 실패 경로를 명확히 알려 준다.

> **CHAP 인증**: `daemon.conf` 의 `[iscsi] chap_user`/`chap_password` 를 설정하면 타겟에
> CHAP 이 적용됩니다. **타겟 측 비밀번호는 프로세스 인자로 나가지 않습니다** — 데몬이
> configfs `auth/password` 파일에 직접 씁니다(LIO 전환의 목적).
>
> 데몬은 타겟을 만들기 **전에** 자격을 검증하고, 아래 중 하나라도 걸리면 생성을
> **거부**합니다(조용히 넘어가지 않습니다):
>
> - **한쪽만 설정** — `chap_user` 만 있고 `chap_password` 가 없거나 그 반대. 그대로 두면
>   커널에 "인증 불필요"가 기록되어 **누구나 인증 없이 그 LUN 에 접속**할 수 있게 됩니다.
>   `chap_password` 가 암호화 저장돼 있는데 복호에 실패한 경우(머신 ID 변경, 다른 노드에서
>   복원한 설정 파일 등)도 같은 경로이므로, 살아 있는 타겟의 인증이 재생성 중에 꺼지는 일이
>   없습니다.
> - **`"NULL"` 로 시작하는 값** — 커널이 그 자격을 **미설정으로 되돌립니다**.
> - **255자 초과** — 커널이 **조용히 잘라** 저장하므로 로그인이 영영 실패합니다.
> - **개행이 섞인 값** — configfs 는 개행을 제거하지 않아 자격의 일부가 됩니다.
>
> 거부 시 오류 메시지는 어느 항목이 문제인지만 알려 주고 **값 자체는 절대 싣지 않습니다**.
>
> 이니시에이터(클라이언트) 측 CHAP도 비밀번호를 `iscsiadm` 인자로 전달하지 않습니다.
> discovery 뒤 open-iscsi 호환 lock을 잡고 같은 target/address/port의 모든 TPGT·iface node
> record를 0600 임시 파일과 atomic rename으로 갱신합니다. 일부 파일 갱신 중 오류가 나면
> 이미 교체한 record를 역순 원복하며, 무매치·symlink·중복 auth key·반쪽 CHAP 설정은 login
> 전에 fail-closed합니다. discovery는 `discoverydb --discover`로 record를 영속하며, 최종 활성
> `/var/lib/iscsi/nodes/...` record(legacy 설치는 `/etc/iscsi/nodes/...`)에는 open-iscsi가
> 로그인에 사용할 비밀번호가 저장되므로 해당 트리와 root 권한은 계속 비밀 경계입니다.
> 같은 target이 두 root에 모두 있으면 정본을 추측하지 않고 거부합니다(ADR-0036).

### 5.4 스토리지 티어링

스토리지 티어 manager의 내부 초기화와 실제 공개 관리 표면을 구분합니다. 현재 tier 목록
CLI·RPC는 등록돼 있지 않으며 NVMe/SSD/HDD 자동 분류·자동 이동과 고정 IOPS/latency를
제품 지원으로 보장하지 않습니다. 현재 pool 상태는 다음 지원 명령으로 확인합니다.

```bash
pcvctl storage health
```

### 5.5 ISO 관리

```bash
# 설정된 모든 iso_dirs에서 ISO/IMG 파일 스캔 (항목 필드는 JSON 출력으로 확인)
pcvctl --format=json iso list | jq '.result'
```

출력 예시:

```json
[
  {
    "name": "ubuntu-24.04-live-server-amd64.iso",
    "path": "/pcvpool/iso/ubuntu-24.04-live-server-amd64.iso",
    "dir": "/pcvpool/iso",
    "size_mb": 2662
  },
  {
    "name": "debian-12.0-amd64-netinst.iso",
    "path": "/pcvpool/iso/debian-12.0-amd64-netinst.iso",
    "dir": "/pcvpool/iso",
    "size_mb": 628
  },
  {
    "name": "cloud-init.img",
    "path": "/var/lib/libvirt/images/cloud-init.img",
    "dir": "/var/lib/libvirt/images",
    "size_mb": 0
  }
]
```

항목마다 `name`(파일 이름), `path`(전체 경로), `dir`(파일을 찾은 `iso_dirs` 디렉터리), `size_mb`(MiB 단위, 소수점 아래 버림)가 있습니다. 파일 정보를 읽지 못한 항목에는 `size_mb`가 없습니다.

> **다중 ISO 경로**: `daemon.conf [storage] iso_dirs`에 콤마로 구분하여 여러 디렉터리를 지정할 수 있습니다.

### 5.6 Prometheus 스토리지 메트릭

`/api/v1/metrics`에서 다음 스토리지 메트릭을 수집합니다:

```
# ZFS 풀 메트릭 (6개, daemon.conf [storage] pool 하나, 60초 주기, 라벨 없음)
purecvisor_zpool_state
purecvisor_zpool_errors_read
purecvisor_zpool_errors_write
purecvisor_zpool_errors_cksum
purecvisor_zpool_scrub_age_seconds   # 스크럽 기록이 있을 때
purecvisor_zpool_capacity_percent

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

### 네트워크 서비스 개요

PureCVisor 네트워크 서비스는 한 Single Edge 호스트에서 VM의 연결 방식, 주소 할당, 외부 통신, tenant 격리와 트래픽 정책을 함께 관리하는 로컬 네트워크 제어면입니다.
`purecvisorsd`가 CLI, Web UI와 REST/RPC 요청을 받아 Linux bridge, `dnsmasq`, nftables, TC, OVS/OVN과 VM persistent XML에 필요한 상태를 적용합니다.
패킷은 데몬을 경유하지 않고 선택한 Linux/KVM 데이터 경로로 흐르며, 데몬은 구성의 생성·조회·복구와 권한·감사 경계를 담당합니다.

| 구성 영역 | 네트워크 서비스가 제공하는 기능 | 주요 구현과 확인 지점 |
|---|---|---|
| 기본 VM 연결 | NAT, 격리, 라우팅, 물리 LAN 연결 | `pcvctl network`, Linux bridge, `dnsmasq`, nftables |
| tenant 네트워크 | VPC, IPv4 subnet, 정지 VM attachment, 제한형 Service Publish | `pcvctl vpc`, Linux Local VPC, Web UI `인프라 > Local VPC` |
| 트래픽 정책 | 방화벽, Security Group, QoS, IDS/IPS | nftables, TC, Suricata |
| 오버레이·가속 경로 | VLAN, VXLAN, OVN, DPDK, SR-IOV | 기능별 상태 명령과 이 장의 전용 제약 |
| 관측과 복구 | host 기준선, desired/actual 상태, 네트워크 메트릭 | `/api/v1/networks/host-baseline`, `network list`, `vpc status`, Prometheus |

기본 네트워크와 Local VPC는 목적이 다릅니다.
기본 네트워크는 VM을 하나의 bridge 또는 물리 LAN에 직접 연결하는 단순한 호스트 네트워크이고, Local VPC는 tenant, subnet, attachment와 게시 서비스를 하나의 desired state로 관리하는 격리 경계입니다.
Local VPC를 사용하는 VM NIC는 VPC attachment 절차로 연결해야 하며, VPC가 관리하는 bridge를 일반 `network.*` 명령이나 `vm.create network_bridge`로 수정하면 안 됩니다.

| 요구 사항 | 권장 방식 | 선택 이유 |
|---|---|---|
| 사설 주소를 사용하는 VM의 외부 접속 | `nat` | 내부 DHCP와 outbound NAT를 함께 구성합니다. |
| 외부와 단절된 VM 간 통신 | `isolated` | host 내부 bridge에만 트래픽을 유지합니다. |
| upstream 장비가 VM subnet을 정적으로 라우팅 | `routed` | 주소 변환 없이 host를 다음 홉으로 사용합니다. |
| VM 전용 물리 NIC를 upstream LAN에 연결 | `bridge/dedicated` | host L3가 없는 전용 NIC를 bridge port로 편입합니다. |
| host 관리 연결을 유지하면서 같은 물리 LAN 사용 | `bridge/shared` | host IP·route·DNS를 이동하지 않고 VM L2를 중계합니다. |
| tenant별 subnet·정책·선택적 inbound 게시 | `Local VPC`의 `linux` backend | 현재 공개 지원 경계 안에서 수명주기와 격리를 함께 관리합니다. |

구성 전에는 `ip -br link`, `ip -4 route`와 Web UI의 `호스트 네트워크 기준선`을 함께 확인합니다.
새 CIDR은 host connected CIDR과 다른 VPC subnet에 겹치지 않아야 합니다.
물리 NIC를 사용할 때는 관리 경로인지 먼저 판별하고, `dedicated`와 `shared` 중 하나를 명시적으로 선택해야 합니다.
특히 원격 접속에 사용하는 NIC를 `bridge/dedicated` 대상으로 선택하면 안 됩니다.

일반 네트워크 변경은 결과를 다시 조회해 bridge, DHCP와 정책 상태를 확인합니다.
Local VPC 변경은 `accepted` 응답만으로 성공으로 판단하지 않고 CLI의 terminal 결과 또는 `jobs.get`의 `completed`를 확인한 뒤 `vpc status`로 actual state를 검증합니다.
현재 공개 지원 backend는 `linux`이며, `ovn`은 이 장에 명시된 추가 검증을 모두 통과하기 전까지 구현 후보로 취급합니다.

<figure class="pcv-network-architecture pcv-control-map pcv-architecture-wide" aria-labelledby="pcv-network-architecture-title">
  <div class="pcv-map-bar">
    <strong id="pcv-network-architecture-title">Single Edge · 네트워크 서비스와 실제 패킷 경로</strong>
    <div class="pcv-map-meta">
      <span class="pcv-status"><i aria-hidden="true"></i>Linux/KVM actual state</span>
      <a class="pcv-architecture-source-open" href="/assets/diagrams/purecvisor-single-network-services.svg" target="_blank" rel="noopener">확대해서 보기 <span aria-hidden="true">↗</span></a>
    </div>
  </div>
  <a class="pcv-architecture-source-canvas pcv-network-architecture-canvas" href="/assets/diagrams/purecvisor-single-network-services.svg" target="_blank" rel="noopener" aria-label="사용자 진입점과 purecvisorsd 네트워크 제어면에서 기본 연결, 가상 네트워크, 정책, 가속과 관측 서비스를 거쳐 Linux KVM 실제 패킷 경로로 이어지는 구성도를 새 탭에서 확대해서 보기" data-pcv-architecture-interactive="network">
    <img class="pcv-architecture-source-image pcv-network-architecture-image" src="/assets/diagrams/purecvisor-single-network-services.svg" width="1440" height="1080" loading="lazy" decoding="async" alt="Web UI와 pcvctl 요청이 purecvisorsd의 네트워크 제어면으로 들어와 기본 연결, 가상 네트워크, 정책, 가속과 관측 서비스로 나뉘고 Linux bridge, nftables, OVS, OVN, WireGuard, VFIO, IOMMU와 libvirt VM NIC에 적용되는 Single Edge 네트워크 구성도">
  </a>
  <figcaption class="pcv-architecture-source-note">위쪽 청록색 화살표는 구성·조회·복구 제어 흐름이고, 아래쪽 회색 화살표는 VM 패킷이 흐르는 Linux/KVM 데이터 경로입니다. 주황색 점선은 DPDK·SR-IOV 가속 우회 경로이며, Local VPC의 OVN backend는 추가 검증 전 후보로 구분했습니다. 마우스 환경에서는 서비스 또는 컴포넌트에 포인터를 올리면 직접 연결된 흐름이 움직이며 강조됩니다.</figcaption>
</figure>

### 네트워크 서비스 활용 예제

#### 예제 1 — NAT 네트워크에 VM 연결

이 예제는 별도의 upstream VLAN이나 물리 NIC 변경 없이 VM에 사설 주소와 외부 방향 통신을 제공하는 가장 단순한 구성입니다.
`10.44.0.0/24`가 host route와 기존 VM/VPC 대역에 사용되지 않는지 먼저 확인합니다.

```bash
# 1. host와 기존 관리형 네트워크의 충돌 여부를 확인한다.
ip -br link
ip -4 route show table main
pcvctl network list

# 2. gateway 10.44.0.1을 사용하는 NAT 네트워크를 만든다.
pcvctl network create app-nat --mode nat --cidr 10.44.0.1/24

# 3. 새 VM NIC를 app-nat에 연결한다.
pcvctl vm create web-demo \
  --vcpu 2 \
  --memory_mb 2048 \
  --disk_size_gb 20 \
  --storage_type qcow2 \
  --network_bridge app-nat \
  --qos_min_mbps 0 \
  --qos_max_mbps 1000

# 4. VM을 시작하고 제어면과 host actual state를 확인한다.
pcvctl vm start web-demo
pcvctl network list
pcvctl vm list
ip -br address show app-nat
sudo nft list ruleset
```

게스트 OS의 NIC가 DHCP를 사용하면 `10.44.0.0/24`에서 주소를 받고 host의 NAT 경계를 통해 외부로 나갑니다.
NAT 네트워크 생성만으로 외부에서 게스트로 들어오는 포트가 자동 공개되지는 않습니다.
외부 inbound가 필요하고 tenant 단위 수명주기까지 관리하려면 다음 Local VPC 예제처럼 Service Publish를 사용합니다.

#### 예제 2 — Local VPC의 웹 서비스를 허용된 네트워크에 게시

이 예제는 `acme` tenant의 웹 subnet을 만들고, 정지 상태의 `web-prod` VM을 연결한 뒤 host TCP `8443`을 게스트 TCP `443`으로 제한 게시합니다.
`web-prod`는 미리 생성되어 정지 상태여야 하고, 연결할 Security Group은 게스트 TCP `443`을 허용해야 합니다.
아래 `198.51.100.10`과 `192.0.2.0/24`는 RFC 문서용 주소이므로 실행 전 실제 node IPv4와 허용할 클라이언트 CIDR로 반드시 바꿉니다.

```bash
# 1. 공개 지원 backend와 현재 capacity를 확인한다.
pcvctl vpc backends

# 2. Linux NAT VPC와 첫 subnet을 한 Job으로 생성한다.
pcvctl vpc create app-vpc --tenant acme --egress nat \
  --backend linux --subnet-name web --cidr 10.60.10.0/24 --mtu 1500

# 3. terminal 완료 뒤 aggregate에서 VPC와 subnet UUID를 확인한다.
pcvctl vpc list --tenant acme
VPC_ID="replace-with-vpc-uuid"
SUBNET_ID="replace-with-subnet-uuid"
pcvctl vpc get "$VPC_ID" --tenant acme

# 4. 정지 VM을 연결하고 완료 응답의 attachment UUID를 확인한다.
pcvctl vpc attachment-create "$SUBNET_ID" web-prod --tenant acme
ATTACHMENT_ID="replace-with-attachment-uuid"
pcvctl vpc get "$VPC_ID" --tenant acme

# 5. 문서용 주소를 실제 운영 값으로 교체한 뒤 서비스 하나만 게시한다.
NODE_IPV4="198.51.100.10"
ALLOWED_CLIENT_CIDR="192.0.2.0/24"
pcvctl vpc service-publish "$ATTACHMENT_ID" --tenant acme \
  --protocol tcp --listen-address "$NODE_IPV4" --listen-port 8443 \
  --target-port 443 --allowed-source "$ALLOWED_CLIENT_CIDR"

# 6. VM을 시작하고 게시 상태와 전체 수렴 상태를 확인한다.
pcvctl vm start web-prod
pcvctl vpc service-list "$VPC_ID" --tenant acme
pcvctl vpc status
```

`listen_address`를 특정 node IPv4로 제한하면 다른 host 주소에는 같은 포트가 열리지 않습니다.
`0.0.0.0`은 모든 host IPv4에 게시한다는 의미이므로 명확한 운영 사유가 있을 때만 사용합니다.
Service Publish는 게스트 서비스 시작, Security Group 허용 또는 인증서를 대신하지 않으므로 허용된 클라이언트에서 실제 응답까지 별도로 확인합니다.
명령이 실패하면 `vpc status`의 `reconcile_required`, resource의 `last_error`와 해당 Job의 terminal 오류를 먼저 확인합니다.

#### 서비스별 예제 지도

아래 표는 공개 네트워크 서비스 전체를 실제 작업 예제와 연결합니다.
한 예제에서 여러 기능을 함께 쓰더라도 각 서비스의 생성 또는 적용 명령과 actual state 확인 지점을 해당 절에 따로 제시합니다.

| 서비스 | 활용 시나리오 | 적용 후 확인 |
|---|---|---|
| [기본 브릿지 네트워크](#61-브릿지-네트워크) | NAT, 내부 격리, upstream 정적 route, 전용·공유 물리 LAN | `pcvctl network list`, `ip -br link`, upstream DHCP 또는 route |
| [관리형 방화벽](#62-방화벽-nftables) | NAT와 isolated 네트워크 경계를 자동 생성 | `sudo nft list table inet purecvisor` |
| [VLAN](#63-vlan-필터링) | VM NIC를 upstream VLAN 100에 연결 | `virsh dumpxml`의 `<vlan>`과 switch port |
| [QoS](#64-qos-트래픽-제어) | VM·tenant별 최소/최대 대역 SLA 적용 | `qos.vm.get`, `qos.tenant.get`, `qos.stats` |
| [OVS VXLAN·tenant overlay](#65-ovs-vxlan-오버레이) | 수동 VXLAN peer와 tenant 암호화 격리 | `overlay.info`, `tenant_overlay.get`, `ovs-vsctl show` |
| [generic OVN](#66-ovn-sdn) | 논리 스위치·라우터·DHCP·ACL·SNAT 구성 | `pcvctl ovn status`, 리소스별 list, 인증 REST 필터 |
| [Security Group](#67-보안-그룹) | 웹 VM의 80·443과 관리 CIDR의 22만 허용 | 그룹 목록, VM binding, `nft` actual rule |
| [DPDK](#68-dpdk) | 전용 NIC를 `vfio-pci`에 바인딩해 OVS-DPDK bridge 구성 | `pcvctl dpdk list`, `ovs-vsctl show` |
| [SR-IOV](#69-sr-iov) | VF에 VLAN·spoof check를 설정하고 VM에 직접 할당 | `pcvctl sriov list`, VM hostdev XML |
| [네트워크 디버깅](#610-네트워크-디버깅) | VM 통신 장애를 link→bridge→policy→overlay 순서로 격리 | 계층별 actual 명령의 일치 여부 |
| [Prometheus 메트릭](#611-네트워크-prometheus-메트릭) | NIC error·drop과 conntrack 포화를 관측 | `/api/v1/metrics` 필터 결과 |
| [Suricata IDS/IPS](https://purecvisor.site/ko/security/security/#1012-suricata-dpiidsips-20-d13) | 보안 10.12에서 탐지 상태 확인 후 선택 SID만 인라인 차단 | IPS status, drop list, 보안 이벤트 |
| [Local VPC](#612-local-vpc) | tenant subnet의 VM 서비스를 허용 CIDR에 제한 게시 | Job terminal, `vpc get`, `vpc service-list`, `vpc status` |

### 6.1 브릿지 네트워크

#### 활용 예제 — 연결 목적에 맞는 기본 네트워크 생성

아래 다섯 모드는 같은 기능의 단계가 아니라 서로 다른 연결 목적입니다.
호스트의 connected CIDR과 겹치지 않는 대역을 고르고, 물리 LAN을 사용하는 두 모드는 NIC 역할과 복구 경로를 먼저 확인합니다.
`dedicated`에는 원격 관리 NIC를 사용하지 않고, `shared`는 host L3 보존 조건과 upstream의 다중 source MAC 허용 여부를 확인합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-bridge-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/bridge-network.svg" target="_blank" rel="noopener" aria-label="브릿지 네트워크 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/bridge-network.svg" width="960" height="280" loading="lazy" decoding="async" alt="연결 mode와 CIDR 또는 uplink 선택이 PureCVisor desired state와 Linux bridge, dnsmasq, nftables 또는 물리 uplink를 거쳐 VM NIC actual state로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-bridge-caption">입력한 연결 목적이 host 데이터 경로와 VM NIC에 어떻게 반영되는지 먼저 확인합니다.</figcaption>
</figure>

```bash
# NAT 모드: 사설 DHCP 주소와 outbound NAT
pcvctl network create app-nat --mode nat --cidr 10.44.0.1/24

# 전용 업링크: 호스트 IP/기본 경로가 없는 VM 전용 유선 NIC
pcvctl network create prod-net --mode bridge --iface enp5s0 \
  --uplink-mode dedicated --confirm-dedicated-uplink

# 공유 업링크: 호스트가 사용 중인 유선 NIC의 IP/route/DNS를 유지
pcvctl network create shared-lan --mode bridge --iface enp4s0 \
  --uplink-mode shared --confirm-shared-uplink

# 격리 모드: host 내부 VM 간 통신만 허용
pcvctl network create lab-isolated --mode isolated --cidr 10.45.0.1/24 --mtu 1500

# 라우티드 모드: upstream에 10.46.0.0/24 경로를 별도로 선언
pcvctl network create routed-net --mode routed --cidr 10.46.0.1/24

# desired state와 host actual bridge를 함께 확인
pcvctl network list
ip -br link
```

| 모드 | 설명 | 외부 접근 | DHCP |
|------|------|----------|------|
| `nat` | iptables/nftables MASQUERADE | O (NAT) | 자동 |
| `bridge/dedicated` | 전용 물리 NIC를 Linux bridge port로 편입(호스트 IP 없음) | O (upstream 직접) | upstream |
| `bridge/shared` | 물리 NIC의 호스트 L3를 유지하고 TC-BPF portal로 VM L2만 중계 | O (동일 LAN 직접) | upstream |
| `isolated` | 외부 격리, VM 간만 통신 | X | 자동 |
| `routed` | 정적 라우팅 | O (라우팅) | 선택 |

> **게스트 격리**: nat 게스트는 외부와 다른 legacy 망에서 새로 들어오는 연결을 받지 않습니다(운영자가 호스트에 직접 둔 DNAT만 통과). UFW(기본 FORWARD 정책 DROP) 호스트에서 DNAT를 쓰려면 포워드마다 좁힌 규칙도 둡니다 — 예: `ufw route allow in on eno1 out on pcvnat0 to 10.78.0.5 port 22 proto tcp`. `ufw route allow out on <브릿지>`처럼 넓은 규칙은 데몬이 다음 적용에서 지우고, 좁힌 규칙은 그대로 둡니다. iptables FORWARD DROP 호스트도 같은 꼴의 좁힌 FORWARD 규칙이 필요합니다. legacy 망 사이(nat·isolated·routed 어느 조합이든) 새 연결은 막힙니다. 게스트는 호스트의 DHCP(67)·자기 게이트웨이 DNS(53)·DHCPv6(547)·이웃 탐색·게이트웨이 ping만 쓸 수 있고 SSH·UI·NFS·iSCSI 등 호스트 서비스에는 닿지 않습니다. routed 게스트로 들어오는 외부 연결은 그대로 허용되며 보안 그룹으로 제한합니다. 이 규칙은 UFW·iptables·firewalld 유무와 관계없이 데몬이 집행합니다.

`bridge/dedicated`는 관리 NIC를 자동 변환하지 않는다. 서버는 IPv4 주소, link-local 외 IPv6
주소, IPv4/IPv6 기본 경로, 기존 master가 있거나 bond/VLAN이거나 상태를 확정할 수 없는
NIC를 거부한다.
호스트 관리 IP·route·DNS를 Linux bridge로 옮기려면 Web UI/RPC가 아니라 사용 중인
Netplan/NetworkManager/systemd-networkd의 선언적 설정과 out-of-band 복구 수단을 사용한다.
`network bind`는 원자 rollback을 제공할 수 없어 deprecated·차단되며, 물리 NIC 연결은
위 `network create --mode bridge` 트랜잭션으로만 수행한다.
`bridge/shared`는 물리 NIC를 bridge port로 만들거나 host IP·route·DNS·renderer profile을
이동하지 않는다. 내부 unnumbered bridge와 veth portal, 물리 NIC의 PureCVisor 소유 TC-BPF
filter만 사용한다. 최초 지원 범위는 untagged 유선 Ethernet 한 개이며 Wi-Fi, bond, VLAN,
trunk는 거부한다. 상위 스위치 포트가 여러 source MAC을 허용해야 VM이 upstream DHCP에서
호스트와 같은 LAN 대역의 독립 주소를 받을 수 있다. port-security가 VM MAC을 막아도 호스트
경로는 그대로 유지되며 NAT로 자동 fallback하지 않는다.

shared bridge는 고정 guest MAC이나 IP를 미리 만들지 않는다. 각 KVM VM NIC는 생성·연결
시점의 독립 MAC을 사용하고 upstream DHCP 또는 정적 설정으로 주소를 얻는다. 2026-08-14
검증 예제의 `02:16:3e:44:55:66`과 `192.0.2.50`은 예약값이 아니다. 실제 배포에서는
충돌하지 않는 게스트 MAC과 upstream 네트워크의 주소 정책을 사용한다.

두 physical bridge 모드는 `network mode`로 live 전환할 수 없다. 먼저 관리형 삭제 경로로
NIC를 분리·원상복구한 뒤 원하는 모드로 다시 생성한다. 일반 삭제 경로도 desired state가
없는 host uplink를 발견하면 네트워크를 건드리지 않고 거부한다. physical bridge의
DHCP 활성화도 차단되며 게스트 주소 할당은 upstream 네트워크가 담당한다.

#### 네트워크 관리

```bash
# 네트워크 목록
pcvctl network list

# uplink mode·dataplane·host L3 보존 필드를 포함한 상세 목록
sudo pcvctl --format=json network list

# DHCP 토글
pcvctl network dhcp mgmt-net --enable
pcvctl network dhcp mgmt-net --disable

# standalone 물리 NIC 바인딩은 차단됨 — 업링크 방식은 생성 시 함께 지정
pcvctl network create shared-lan --mode bridge --iface enp4s0 \
  --uplink-mode shared --confirm-shared-uplink

# 삭제 (멱등: 존재하지 않아도 성공)
pcvctl network delete mgmt-net
```

RPC 직접 호출:

```bash
# 네트워크 생성
echo '{"jsonrpc":"2.0","method":"network.create","params":{
  "bridge_name": "mgmt-net",
  "mode": "nat",
  "cidr": "192.168.100.0/24"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 네트워크 목록
echo '{"jsonrpc":"2.0","method":"network.list","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

> **메타데이터와 desired state**: `/var/run/purecvisor/network/dnsmasq-<bridge>.meta`는
> 현재 부팅의 조회 캐시다. dedicated/shared physical bridge의 재부팅 복구 정본은 mode `0600`의
> `/var/lib/purecvisor/networks/<bridge>.json`이다. NAT/isolated/routed의 구성 정본은
> 기존 설정·운영 계약을 따른다.

> **멱등 삭제**: `network.delete`는 대상이 없어도 성공을 반환합니다 (재시도 안전).

> **게스트 MTU 계약**: bridge NIC XML(정의 XML·`vm.start` 라이브 attach·핫플러그, 3경로 모두)은
> attach/정의 시점 브리지 실측 MTU(`/sys/class/net/<bridge>/mtu` 단일 소스)로 `<mtu size='N'/>`를
> 1500 포함 항상 명시한다. 유효 대역은 68–9216이며, 읽기 실패나 대역 밖 값은 생략 + 경고로
> fail-open 한다(attach 자체는 계속 진행). dpdk(vhostuser)·SR-IOV(hostdev) 경로는 범위 밖이다.
> 설계 근거: [ADR-0033](../docs/adr/0033-bridge-nic-mtu-contract.md).

### 6.2 방화벽 (nftables)

PureCVisor는 nftables를 기본 네트워크, Security Group, Local VPC와 Suricata IPS의 공통 host 정책 경계로 사용합니다.
사용자가 임의 chain에 규칙을 넣는 독립 `firewall.rule.*` 공개 RPC는 없으며, 서비스의 desired state를 변경하면 데몬이 소유한 규칙을 함께 생성·복구·정리합니다.

#### 활용 예제 — NAT와 내부 격리 경계 비교

아래 예제는 외부 통신이 필요한 workload와 외부에서 분리할 workload를 서로 다른 관리형 네트워크에 둡니다.
NAT 쪽에는 outbound masquerade가 생기고, isolated 쪽은 같은 bridge의 VM 간 통신만 남습니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-firewall-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/managed-firewall.svg" target="_blank" rel="noopener" aria-label="관리형 방화벽 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/managed-firewall.svg" width="960" height="280" loading="lazy" decoding="async" alt="NAT 또는 isolated network mode가 purecvisorsd의 관리형 정책을 거쳐 nftables inet purecvisor 테이블과 VM 허용 범위에 반영되는 구성도">
  </a>
  <figcaption id="pcv-network-example-firewall-caption">서비스 desired state와 nftables actual state를 함께 비교해야 정책 적용을 확인할 수 있습니다.</figcaption>
</figure>

```bash
# 서로 겹치지 않는 두 네트워크를 생성한다.
pcvctl network create policy-nat --mode nat --cidr 10.47.0.1/24
pcvctl network create policy-isolated --mode isolated --cidr 10.48.0.1/24

# desired state와 데몬 소유 nft actual state를 함께 확인한다.
pcvctl network list
sudo nft list table inet purecvisor
```

VM 단위 포트 허용은 §6.7 Security Group을 사용하고, tenant subnet과 inbound 게시 수명주기는 §6.12 Local VPC를 사용합니다.
데몬이 소유한 `inet purecvisor` table을 운영자가 직접 수정하면 desired state와 actual state가 어긋나므로 영구 설정 절차로 사용하지 않습니다.

`inet purecvisor`의 `postrouting`·`forward`·`guest_input` 체인과 `guest_*_ifaces` 집합 넷은 데몬 소유로 망 생성·전환·삭제·기동 때 통째로 다시 쓰입니다 — 여기에 손으로 넣은 규칙은 지워지므로 운영자 규칙은 별도 테이블에 둡니다. `nft list set inet purecvisor guest_nat_ifaces`로 망별 소속을 봅니다.

> **명령 실행 안전성**: 모든 방화벽 조작은 `pcv_spawn_sync()` argv 배열로 실행됩니다. `system()`과 `popen()`은 사용하지 않습니다.

### 6.3 VLAN 필터링

VM NIC의 persistent libvirt XML에 802.1Q VLAN tag를 지정할 수 있습니다.
`vlan_id`는 고급 VM 속성이므로 현재 `pcvctl vm create` 옵션이 아니라 REST 또는 JSON-RPC body로 전달하며, 공개 `network.vlan.add` RPC는 없습니다.

#### 활용 예제 — VM을 upstream VLAN 100에 연결

이 예제의 `pcvbr0`는 VLAN 100을 전달할 수 있는 Linux bridge 또는 OVS bridge여야 합니다.
upstream switch port도 VLAN 100을 허용해야 하며, native VLAN과 guest tag 정책은 호스트 밖의 switch 설정과 일치해야 합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-vlan-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/vlan-filtering.svg" target="_blank" rel="noopener" aria-label="VLAN 필터링 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/vlan-filtering.svg" width="960" height="280" loading="lazy" decoding="async" alt="VLAN 100 입력이 PureCVisor 검증과 libvirt persistent XML의 VM NIC tag를 거쳐 bridge와 upstream trunk로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-vlan-caption">VM NIC의 VLAN tag와 upstream trunk 허용 목록이 같은 값으로 이어져야 합니다.</figcaption>
</figure>

```bash
# VM 생성 body에 VLAN 100을 지정한다.
echo '{"jsonrpc":"2.0","method":"vm.create","params":{
  "name":"web-vlan","vcpu":2,"memory_mb":2048,"disk_size_gb":20,
  "storage_type":"qcow2","network_bridge":"pcvbr0","vlan_id":100,
  "qos_min_mbps":0,"qos_max_mbps":1000
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# persistent XML에 VLAN tag가 남았는지 확인한다.
sudo virsh dumpxml web-vlan
sudo virsh domiflist web-vlan
```

`dumpxml`의 VM interface 아래에 `<vlan><tag id="100"/></vlan>`이 있어야 합니다.
게스트에서 주소를 받지 못하면 VM XML만 반복 수정하지 말고 bridge의 VLAN filtering과 upstream trunk 허용 목록을 함께 확인합니다.

### 6.4 QoS (트래픽 제어)

TC 기반 네트워크 QoS를 지원합니다.
인터페이스 단위 `network.qos.*`는 기존 자동화 호환용 deprecated 표면이며, 새 구성은 VM·tenant SLA를 지속적으로 식별하고 reconcile하는 `qos.vm.*`와 `qos.tenant.*`를 우선합니다.

#### 활용 예제 — 기존 vnet 제한과 VM·tenant SLA 적용

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-qos-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/qos.svg" target="_blank" rel="noopener" aria-label="QoS 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/qos.svg" width="960" height="280" loading="lazy" decoding="async" alt="VM 또는 tenant의 대역폭 SLA가 PureCVisor QoS handler와 Linux TC qdisc, filter를 거쳐 실제 rate와 drop 통계로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-qos-caption">desired SLA와 실제 vnet의 TC 통계를 함께 확인해 대역폭 제한의 수렴 여부를 판단합니다.</figcaption>
</figure>

```bash
# 기존 vnet 자동화와의 호환이 필요할 때만 인터페이스 상한을 설정한다.
pcvctl network qos-set vnet0 --rate-mbps 100 --burst-kb 64

# 실제 tc 적용 상태를 조회한다.
pcvctl network qos-get vnet0

# 호환 규칙이 더 필요하지 않으면 제거한다.
pcvctl network qos-remove vnet0
```

> **호환 표면 영속화**: 물리 NIC·브리지의 `network.qos.*` 규칙은 `/var/lib/purecvisor/qos_rules.json`에 저장되어 데몬 재시작과 재부팅 뒤 자동 복원됩니다. `vnet` 뒤가 숫자인 이름(`vnet[0-9]+`)의 규칙은 재부팅 뒤 같은 이름이 다른 VM에 붙을 수 있어 **현재 세션에만** 적용하고 저장하지 않습니다(`vm_name`으로 호출해도 같으며 응답의 `persisted`가 `false`입니다). 파일 쓰기가 실패해도 `persisted`는 `false`입니다. 이전 버전이 저장한 해당 키는 다음 기동 때 WARN을 남기고 파일에서 지워집니다. VM 대역폭을 재시작 뒤에도 유지하려면 `qos.vm.set`을 쓰세요. 고급 QoS 계층(`qos.vm.*`·`qos.tenant.*`)이 관리하는 인터페이스에는 `network.qos.set`·`network.qos.remove`의 ingress(`all` 포함)가 거부되고 복원도 건너뜁니다. 새 workload SLA의 정본으로 사용하지 않습니다.

#### 고급 QoS — per-VM/tenant SLA (2.0)

2.0은 위 인터페이스별 tc 셰이핑(`network qos-*`) 위에, VM·테넌트 단위 대역폭 SLA를 강제하는 고급 QoS 계층을 추가했습니다. 단일 IFB 디바이스 `pcvqos0` 위에 단일 HFSC 트리(단일 major `1:`, VM·테넌트별 minor 파생)를 세우고, 각 VM leaf는 Cake(besteffort) qdisc로 성형합니다. class 배정은 이름 기반 결정적 계산이라 데몬을 재시작해도 같은 VM은 같은 class를 다시 씁니다(reconcile). 테넌트 SLA는 `/var/lib/purecvisor/qos_tenants.json`(재부팅 생존)에, tc id 매핑은 `/var/run/purecvisor/qos_ids.json`(tmpfs, 커널 tc 트리와 동일 수명)에 영속화됩니다.

> **RPC 전용(2.0)**: `qos.*` 네임스페이스는 전용 REST 경로·CLI 서브커맨드·UI 페이지가 없습니다. `POST /api/v1/rpc` JSON-RPC 패스스루(또는 UDS 직접 `nc -U`)로만 호출합니다.

| 메서드 | RBAC | 파라미터 | 용도 |
|--------|------|----------|------|
| `qos.vm.set` | ADMIN | `{vm, qos_min_mbps, qos_max_mbps, [qos_burst_kb]}` | per-VM 대역폭 SLA 설정 |
| `qos.vm.get` | VIEWER | `{vm}` | per-VM SLA 조회 |
| `qos.tenant.set` | ADMIN | `{tenant, min_mbps, max_mbps}` | per-tenant 대역폭 SLA 설정 |
| `qos.tenant.get` | VIEWER | `{tenant}` | per-tenant SLA 조회 |
| `qos.stats` | ADMIN | `{[tenant], [vm]}` | QoS 강제 통계(테넌트·VM 필터 선택) |

```bash
# per-VM SLA 설정 (POST /api/v1/rpc 패스스루)
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"qos.vm.set","params":{
        "vm":"web-prod","qos_min_mbps":100,"qos_max_mbps":500},"id":"1"}'

# per-tenant SLA 설정
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"qos.tenant.set","params":{
        "tenant":"acme","min_mbps":200,"max_mbps":1000},"id":"1"}'

# VM 설정과 tenant별 강제 통계를 다시 조회한다.
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"qos.vm.get","params":{"vm":"web-prod"},"id":"1"}'
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"qos.stats","params":{"tenant":"acme"},"id":"1"}'
```

> **VM 생성 시 SLA 필수**: 커널 netdev(tc 셰이핑 가능) NIC — `network_bridge` 미지정/bridge 또는 `tenant-overlay` — 을 쓰는 VM은 `vm.create` 시 `--qos_min_mbps`/`--qos_max_mbps`가 필수입니다(§3.1 「기본 생성」의 QoS SLA 안내와 상호 참조). `dpdk`/`sriov` NIC만 면제되며, 생략 시 서버가 `vm.create`를 `-32602`로 거부합니다.

#### QoS 카오스 주입 (netem, 테스트용)

VM leaf class에 netem을 삽입해 지연·손실을 인위적으로 주입하는 카오스 하네스를 제공합니다(장애 주입 테스트 용도). 타임박스·`dry_run`·감사·부팅 시 자동 정리(잔여 netem을 Cake besteffort로 복원) 가드가 걸려 있으며, 타임박스 만료 시 자동으로 정상 성형으로 되돌립니다.

| 메서드 | RBAC | 파라미터 | 용도 |
|--------|------|----------|------|
| `qos.chaos.start` | ADMIN | `{vm, profile, timebox_sec, [dry_run]}` | netem 카오스(지연/손실) 시작 |
| `qos.chaos.stop` | ADMIN | `{vm}` | 카오스 중지 + Cake besteffort 복원 |
| `qos.chaos.status` | ADMIN | `{}` | 카오스 상태 조회 |

### 6.5 OVS VXLAN 오버레이

Web UI 화면은 `인프라 > 오버레이 네트워크`입니다.

> **에디션 경계**: 오버레이 코어(`create/delete/list/info/add_peer/remove_peer`)는 Single Edge 공개 범위에 포함됩니다. 자동 풀메시와 peer discovery는 공개 출시 표면에 포함되지 않습니다.

Open vSwitch 기반 VXLAN 오버레이 네트워크를 구성합니다.

#### 활용 예제 — 외부 VXLAN endpoint와 수동 peer 구성

이 예제는 PureCVisor 노드가 `192.0.2.19`를 tunnel source로 사용하고, 운영자가 관리하는 VXLAN 호환 endpoint `192.0.2.20`과 VNI 100을 연결하는 구성입니다.
두 주소는 RFC 문서용 주소이므로 실제 tunnel endpoint로 바꾸고, underlay에서 UDP 4789와 MTU를 먼저 확인합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-vxlan-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/ovs-vxlan.svg" target="_blank" rel="noopener" aria-label="OVS VXLAN 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/ovs-vxlan.svg" width="960" height="280" loading="lazy" decoding="async" alt="tunnel source와 remote endpoint 및 VNI 100이 PureCVisor peer desired state, OVS VXLAN port와 UDP 4789 underlay를 거쳐 원격 endpoint로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-vxlan-caption">수동 peer 구성은 OVS actual port와 두 endpoint 사이의 underlay 도달성을 모두 확인합니다.</figcaption>
</figure>

오버레이 전용 CLI 명령은 없으므로 JSON-RPC(UDS 직접 `nc -U` 또는 `POST /api/v1/rpc` 패스스루)로 호출합니다.
오버레이 목록은 REST `GET /api/v1/overlay`와 Web UI `인프라 > 오버레이 네트워크`에서도 확인할 수 있습니다.

```bash
# 오버레이 생성
echo '{"jsonrpc":"2.0","method":"overlay.create","params":{
  "name":"pcvoverlay0","vni":100,"cidr":"10.100.0.1/24"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 명시적으로 관리하는 endpoint만 peer로 추가
echo '{"jsonrpc":"2.0","method":"overlay.add_peer","params":{
  "name":"pcvoverlay0","peer_ip":"192.0.2.20"
},"id":"2"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# desired state와 OVS actual port 확인
echo '{"jsonrpc":"2.0","method":"overlay.list","params":{},"id":"3"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
echo '{"jsonrpc":"2.0","method":"overlay.info","params":{
  "name":"pcvoverlay0"
},"id":"4"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
sudo ovs-vsctl show

# 사용을 마치면 peer와 overlay를 역순으로 정리
echo '{"jsonrpc":"2.0","method":"overlay.remove_peer","params":{
  "name":"pcvoverlay0","peer_ip":"192.0.2.20"
},"id":"5"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
echo '{"jsonrpc":"2.0","method":"overlay.delete","params":{
  "name":"pcvoverlay0"
},"id":"6"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

#### 자동 프로비저닝

`daemon.conf [overlay]` 섹션에 `default_bridge`를 설정하면 부팅 시 그 이름의 기본 오버레이 브리지가 `default_vni`(기본 100)·`default_cidr`로 자동 생성됩니다. 설정 파일로 peer를 넣는 키는 없으므로 peer는 `overlay.add_peer` RPC로 명시해 추가하며, 자동 peer discovery나 자동 풀메시는 제공하지 않습니다.

Single Edge는 overlay 코어 기능을 지원하지만, `tunnel_ip`는 자동 추론하지 않습니다. 따라서 Single Edge에서 overlay를 활성화하려면 운영자가 `[overlay]` 섹션에 `tunnel_ip`를 명시해야 합니다. 이 값은 로컬 호스트가 VXLAN 터널 소스로 사용할 IP이며, Single Edge에서는 운영 가이드에 따라 명시적으로 관리합니다.

```ini
[overlay]
default_bridge = pcvoverlay0
default_vni = 100
default_cidr = 10.100.0.1/24
tunnel_ip = 192.0.2.19
```

OVS 상태 확인:

```bash
sudo ovs-vsctl show
```

#### 테넌트 암호화 오버레이 (tenant_overlay, 2.0)

> **에디션 경계**: 테넌트 오버레이 코어(`create/delete/get/list/attach_vm/detach_vm`)는 Single Edge 공개 범위에 포함됩니다. 자동 peer 구성과 키 교환 자동화는 공개 출시 표면에 포함되지 않습니다.

2.0은 멀티테넌트 VM 트래픽을 per-VM 암호화 오버레이로 격리하는 `tenant_overlay.*` 제어평면을 추가했습니다. 각 VM은 자체 network namespace 안의 per-VM WireGuard 엔드포인트로 배선되며, 공유 브리지 위를 지나는 오버레이 트래픽은 암호문입니다. 오버레이 레지스트리는 영속화되어 데몬 재시작 시 재수화되고, `vm.create`/`vm.start` 오케스트레이션은 fail-closed로 동작합니다(암호 오버레이 준비 실패 시 VM 기동을 막음).

VM을 테넌트 오버레이에 붙이려면 `vm.create` 시 `network_mode`를 `tenant-overlay`로, `tenant`를 오버레이 테넌트 이름으로 지정하며, 이 조합은 커널 netdev 셰이핑 대상이라 §3.1의 QoS SLA(`--qos_min_mbps`/`--qos_max_mbps`)가 필수입니다. 서브넷은 오버레이 생성 시 자동 배정되어 응답으로 반환되고, VM attach 시 내부 오버레이 IP가 배정됩니다.

> **RPC 전용(2.0)**: `tenant_overlay.*` 네임스페이스는 전용 REST 경로·CLI 서브커맨드·UI 페이지가 없습니다. `POST /api/v1/rpc` JSON-RPC 패스스루(또는 UDS 직접 `nc -U`)로만 호출하며, 모든 메서드는 ADMIN 권한이 필요합니다.

| 메서드 | 파라미터 | 용도 |
|--------|----------|------|
| `tenant_overlay.create` | `{tenant}` | 테넌트 암호화 오버레이 생성(서브넷 자동 배정 → 응답 `subnet`) |
| `tenant_overlay.delete` | `{tenant}` | 테넌트 오버레이 삭제 |
| `tenant_overlay.list` | `{}` | 테넌트 오버레이 목록(`[{tenant, subnet}, …]`) |
| `tenant_overlay.get` | `{tenant}` | 단일 테넌트 오버레이 상세(`{tenant, subnet}`) |
| `tenant_overlay.attach_vm` | `{tenant, vm}` | VM을 오버레이에 참여(내부 IP 배정 → 응답 `overlay_ip`) |
| `tenant_overlay.detach_vm` | `{tenant, vm}` | VM을 오버레이에서 분리 |

```bash
# 테넌트 오버레이 생성 (POST /api/v1/rpc 패스스루)
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"tenant_overlay.create","params":{
        "tenant":"acme"},"id":"1"}'

# VM을 오버레이에 참여
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"tenant_overlay.attach_vm","params":{
        "tenant":"acme","vm":"web-prod"},"id":"1"}'

# tenant subnet과 VM overlay IP가 등록됐는지 다시 조회
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://127.0.0.1:8080/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"tenant_overlay.get","params":{
        "tenant":"acme"},"id":"1"}'
```

### 6.6 OVN SDN

Web UI 화면은 `인프라 > OVN SDN`입니다.

> **에디션 경계**: generic OVN 코어(`status`, switch/port/ACL/router/DHCP/NAT/tenant)는
> Single Edge 공개 범위에 포함됩니다. 등록된 RPC inventory는 정확히 18개이며 완전한
> CRUD를 뜻하지 않습니다. 완결되지 않은 OVN/NFV Load Balancer와 production caller가 없는
> VM 자동 포트 내부 helper는 공개 기능이 아닙니다. encap 설정과 auto-provision 자동화는
> 공개 범위 밖 멀티 제어면 참고 기능으로 남겨 둡니다.

OVN (Open Virtual Network) 기반 소프트웨어 정의 네트워크를 지원합니다.

#### 활용 예제 — 웹 논리 네트워크에 DHCP·ACL·SNAT 적용

아래 절을 순서대로 실행하면 `ls-web` 논리 스위치와 `lr-main` 논리 라우터를 만들고, `10.0.1.0/24` DHCP와 웹 ACL, SNAT를 연결합니다.
`203.0.113.1`은 RFC 문서용 주소이므로 실제 external IP로 교체하고, 실행 전 host 기준선과 `ovn status`가 모두 준비 상태인지 확인합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-ovn-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/ovn-sdn.svg" target="_blank" rel="noopener" aria-label="OVN SDN 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/ovn-sdn.svg" width="960" height="280" loading="lazy" decoding="async" alt="웹 논리 네트워크 모델이 PureCVisor generic OVN RPC를 거쳐 logical switch, router, DHCP, ACL, SNAT과 OVS br-int actual state로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-ovn-caption">논리 객체의 REST 조회와 OVN·OVS actual state를 함께 비교해 단계별 수렴을 확인합니다.</figcaption>
</figure>

#### 작업 전 호스트 네트워크 기준선

Web UI의 `인프라 > 네트워크`에서 `호스트 네트워크 기준선`을 먼저 확인합니다.
관리 interface/IP, IPv4 main route와 connected CIDR, Linux bridge/port, OVS bridge/port,
현재 tenant의 Local VPC CIDR과 OVN readiness를 한 화면에서 비교할 수 있습니다.

```bash
curl -ksS -H "Authorization: Bearer $TOKEN" \
  "https://${NODE_IPV4}/api/v1/networks/host-baseline" | python3 -m json.tool
```

이 endpoint는 host interface·route·OVS actual을 변경하지 않는 읽기 전용 조회입니다.
응답 일부가 `partial` 또는 `unavailable`이면 빈 상태로 해석하지 말고 해당 수집 실패를
해결한 뒤 OVN이나 Local VPC 리소스를 생성합니다.

#### 등록된 generic OVN RPC — 정확히 18개

| 영역 | 개수 | 등록 RPC |
|---|---:|---|
| 상태 | 1 | `ovn.status` |
| 스위치 | 4 | `ovn.switch.create`, `ovn.switch.delete`, `ovn.switch.list`, `ovn.switch.detail` |
| 포트 | 2 | `ovn.port.add`, `ovn.port.remove` |
| ACL | 2 | `ovn.acl.add`, `ovn.acl.list` |
| 라우터 | 5 | `ovn.router.create`, `ovn.router.delete`, `ovn.router.list`, `ovn.router.detail`, `ovn.router.add_port` |
| DHCP | 1 | `ovn.dhcp.enable` |
| NAT | 2 | `ovn.nat.add`, `ovn.nat.list` |
| 테넌트 | 1 | `ovn.tenant.create` |
| **총계** | **18** | dispatcher에 등록된 generic `ovn.*` 공개 inventory |

#### 등록되지 않은 역동작과 공개 제외 기능

현재 `ACL 삭제`, `NAT 삭제`, `DHCP 목록·삭제`, `tenant 삭제`, `router port 제거` RPC는
등록돼 있지 않습니다. manager 내부에 일부 역동작 함수가 있더라도 사용자 RPC로 간주하지
않습니다. cleanup은 등록된 port/switch/router 삭제와 부모 리소스의 안전한 cascade를
기준으로 합니다. OVN/NFV Load Balancer를 생성·연결·조회·삭제하는 사용자 절차도
제공하지 않습니다. Local VPC의 제한형 Service Publish는 이 제외 기능과 별개입니다.

#### OVN 상태

```bash
pcvctl ovn status
```

#### 논리 스위치

```bash
# 스위치 생성
pcvctl ovn switch create ls-web

# 스위치 목록
pcvctl ovn switch list

# 스위치 삭제
pcvctl ovn switch delete ls-web
```

논리 스위치 생성은 L2 switch만 만들며 subnet 입력을 받지 않습니다. 주소 대역과 DHCP는
아래 `ovn dhcp enable` 단계에서 별도로 구성합니다.

#### 논리 라우터

```bash
# 라우터 생성
pcvctl ovn router create lr-main

# 라우터 목록
pcvctl ovn router list

# 라우터 삭제
pcvctl ovn router delete lr-main
```

#### ACL (접근 제어)

```bash
# 인그레스 규칙 추가
pcvctl ovn acl add ls-web to-lport 100 'tcp.dst == 80' allow

# 이그레스 규칙 추가
pcvctl ovn acl add ls-web from-lport 50 'tcp.dst == 443' allow

# ACL 목록
pcvctl ovn acl list ls-web
```

#### NAT

```bash
# NAT 목록
pcvctl ovn nat list lr-main
```

NAT 추가와 router↔switch 포트 연결은 현재 JSON-RPC 표면을 사용한다.

```bash
echo '{"jsonrpc":"2.0","method":"ovn.router.add_port","params":{
  "router":"lr-main","switch":"ls-web","mac":"02:00:00:00:01:01",
  "cidr":"10.0.1.1/24"},"id":"1"}' \
  | socat - UNIX-CONNECT:/var/run/purecvisor/daemon.sock

echo '{"jsonrpc":"2.0","method":"ovn.nat.add","params":{
  "router":"lr-main","type":"snat","external_ip":"203.0.113.1",
  "logical_ip":"10.0.1.0/24"},"id":"1"}' \
  | socat - UNIX-CONNECT:/var/run/purecvisor/daemon.sock
```

#### DHCP

```bash
# OVN DHCP 옵션 설정
pcvctl ovn dhcp enable 10.0.1.0/24 10.0.1.1 --switch ls-web
```

`--switch`를 주면 DHCP option record를 스위치에 귀속시키고, 이미 있는
일반 logical switch port와 이후 추가할 포트 모두에 같은 옵션을 연결한다.
OVN 포트, DHCP, router link 생성은 하나의 `ovn-nbctl` transaction으로
실패하며 부분 구성을 성공으로 보고하지 않는다.

스위치를 삭제하면 같은 ownership marker를 가진 DHCP option record도 동일 transaction에서
자동 정리합니다. 다른 스위치가 소유한 행과 foreign OVN 행은 보존하며, 소유권 조회가
모호하거나 UUID가 비정상이거나 안전 상한을 넘으면 실제 변경 전에 실패합니다.

#### 인증 REST 조회 필터

ACL과 NAT 목록 REST는 대상 식별자를 query parameter로 받아 canonical RPC parameter로
전달합니다.

```bash
curl -ksS -H "Authorization: Bearer $TOKEN" \
  "https://${NODE_IPV4}/api/v1/ovn/acl?switch=ls-web"

curl -ksS -H "Authorization: Bearer $TOKEN" \
  "https://${NODE_IPV4}/api/v1/ovn/nat?router=lr-main"
```

`switch` 또는 `router`가 없거나 비어 있으면 필터 없는 전체 목록으로 넓히지 않고 canonical
JSON-RPC `-32602`(`Invalid params`)로 거부합니다. 대상이 지정된 성공 응답에는 다른
switch/router의 표식이 섞이면 안 됩니다.

generic OVN 18개 RPC의 `NET-OVN-01~07` 검증과 Local VPC OVN backend의 공개 지원 판정은
서로 다른 gate입니다. generic OVN 검증이 통과해도 Local VPC OVN의 부팅 KVM,
Linux/OVN 공존, controller/host reboot와 전 단계 fault injection을 대신하지 않습니다.

### 6.7 보안 그룹

Web UI 화면은 `인프라 > 보안 그룹`입니다.

Security Group으로 VM의 ingress와 egress 허용 범위를 선언하고, nftables actual rule로 적용합니다.
CLI는 기본 생성·단일 포트 규칙·VM 연결을 제공하고, source CIDR이나 rule 제거처럼 세부 속성이 필요한 작업은 JSON-RPC를 사용합니다.

#### 활용 예제 — 웹 포트와 관리 CIDR의 SSH만 허용

아래 예제는 `web-prod` VM에 HTTP·HTTPS를 허용하고, SSH는 RFC 문서용 관리 대역 `192.0.2.0/24`에서만 허용합니다.
실행 전 해당 대역을 실제 관리 CIDR로 바꾸며, VM에 기존 그룹이 있다면 교체 영향을 먼저 확인합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-sg-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/security-group.svg" target="_blank" rel="noopener" aria-label="보안 그룹 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/security-group.svg" width="960" height="280" loading="lazy" decoding="async" alt="HTTP, HTTPS와 관리 CIDR SSH 규칙이 PureCVisor 보안 그룹 desired state와 nftables VM NIC 경계를 거쳐 web-prod의 허용 및 차단 결과로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-sg-caption">허용 규칙뿐 아니라 default-deny가 유지되는 차단 경로까지 함께 시험합니다.</figcaption>
</figure>

```bash
# 보안 그룹 생성
pcvctl security-group create web-sg

# 공개 웹 포트는 CLI로 각각 추가
pcvctl security-group rule add web-sg --direction ingress --proto tcp --port 80
pcvctl security-group rule add web-sg --direction ingress --proto tcp --port 443

# SSH 규칙의 source CIDR은 JSON-RPC로 제한
echo '{"jsonrpc":"2.0","method":"security_group.rule.add","params":{
  "name":"web-sg","direction":"ingress","protocol":"tcp",
  "port":22,"source":"192.0.2.0/24"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 애플리케이션의 HTTPS egress 허용
pcvctl security-group rule add web-sg --direction egress --proto tcp --port 443

# VM에 보안 그룹 적용
pcvctl vm security-group web-prod web-sg

# desired state와 nft actual state 확인
pcvctl security-group list
sudo nft list table inet purecvisor
```

이 예제는 DNS, NTP, package mirror 등 운영체제에 필요한 다른 egress를 자동으로 허용하지 않습니다.
실제 workload 의존성을 확인한 뒤 최소 규칙을 추가하고, 잘못 연결했으면 `pcvctl security-group detach web-prod web-sg`로 분리합니다.

> **영속화**: 보안 그룹은 SQLite에 저장되며, 데몬 재시작 시 nftables 규칙이 자동 복원됩니다. default-deny 정책이 기본 적용됩니다.

### 6.8 DPDK

Web UI 화면은 `인프라 > DPDK`입니다.

고성능 데이터 플레인을 위한 DPDK (Data Plane Development Kit) 통합을 지원합니다.
이 경로는 NIC를 host kernel network stack에서 분리하므로 out-of-band 복구 수단과 전용 NIC가 준비된 경우에만 사용합니다.

#### 활용 예제 — 전용 PCI NIC로 OVS-DPDK bridge 구성

`0000:03:00.0`은 예시 PCI 주소입니다.
`dpdk status`의 `available`과 hugepage 준비 상태를 먼저 확인하고, host 관리 경로 또는 기본 route가 연결된 NIC는 대상으로 선택하지 않습니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-dpdk-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/dpdk.svg" target="_blank" rel="noopener" aria-label="DPDK 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/dpdk.svg" width="960" height="280" loading="lazy" decoding="async" alt="전용 PCI NIC와 hugepage 사전 조건이 PureCVisor vfio-pci bind와 OVS-DPDK bridge를 거쳐 VM vhost-user 가속 경로로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-dpdk-caption">주황색 점선은 host kernel network stack을 우회하는 가속 경로와 별도 복구 책임을 나타냅니다.</figcaption>
</figure>

```bash
# 사전 조건 확인
pcvctl dpdk status
pcvctl dpdk hugepage

# 전용 NIC를 vfio-pci에 바인딩하고 같은 PCI 장치를 bridge port로 구성
# 호스트가 쓰는 NIC(브리지·본드 소속, 위에 VLAN이 있음, IPv4·IPv6 주소, 기본 경로 보유)는 거부됩니다.
# 아래 bridge create 의 PCI 인자도 같은 기준으로 거부됩니다.
pcvctl dpdk bind 0000:03:00.0 vfio-pci
pcvctl dpdk bridge create dpdk-br0 0000:03:00.0

# desired state와 OVS actual state 확인
pcvctl dpdk list
sudo ovs-vsctl show

# 사용 종료 시 bridge를 먼저 삭제한 뒤 kernel driver를 복원
pcvctl dpdk bridge delete dpdk-br0
pcvctl dpdk unbind 0000:03:00.0
```

> **언바인딩 완료 판정:** `dpdk unbind`는 복원 명령의 종료코드만 믿지 않습니다.
> `driver_override`를 해제하고 커널 probe를 요청한 뒤 실제 non-DPDK 드라이버가
> 다시 보일 때만 성공합니다. 장치가 이미 사라졌거나 커널 드라이버로 복원된 상태는
> 멱등 성공이며, 도구 부재·권한 오류·상태 미변경은 실패로 보고됩니다.

### 6.9 SR-IOV

Web UI 화면은 `인프라 > SR-IOV`입니다.

SR-IOV를 사용하여 물리 NIC의 가상 기능(VF)을 VM에 직접 할당합니다.
VF 트래픽은 일반 Linux bridge와 host TC QoS를 우회하므로 switch 정책, IOMMU 격리와 guest driver를 함께 검증해야 합니다.

#### 활용 예제 — VLAN 100 VF를 웹 VM에 직접 할당

`eno2`는 예시 PF이며 host 관리 경로에 사용되지 않는 SR-IOV 지원 NIC여야 합니다.
아래 MAC은 문서용 locally administered 주소이므로 실제 배포에서는 중복되지 않는 값으로 바꿉니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-sriov-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/sriov.svg" target="_blank" rel="noopener" aria-label="SR-IOV 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/sriov.svg" width="960" height="280" loading="lazy" decoding="async" alt="PF eno2의 VF 0과 VLAN 100, spoof check 설정이 PureCVisor 검증과 IOMMU hostdev 직접 할당을 거쳐 web-prod guest VF로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-sriov-caption">직접 할당은 Linux bridge와 host TC를 우회하므로 switch와 guest까지 검증 범위를 확장합니다.</figcaption>
</figure>

```bash
# SR-IOV 지원 NIC 상태
pcvctl sriov status

# VF 활성화 (4개)
pcvctl sriov enable eno2 4

# VF 목록
pcvctl sriov list eno2

# VF 0에 MAC, VLAN과 spoof check를 적용
pcvctl sriov set eno2 0 --mac 02:00:00:00:01:10 --vlan 100 --spoofchk on

# VM에 VF 할당
pcvctl sriov attach web-prod eno2 0

# VM persistent XML과 VF actual state 확인
sudo virsh dumpxml web-prod
pcvctl sriov list eno2

# VM에서 VF 분리
pcvctl sriov detach web-prod 0000:03:10.0
```

detach의 PCI 주소는 `pcvctl sriov list eno2`가 VF 0에 반환한 실제 값을 사용합니다.
사용을 모두 마친 뒤에만 `pcvctl sriov disable eno2`로 VF 수를 0으로 되돌립니다.

`vm.create`의 `nic_type=sriov`는 `pci_addr`(`DDDD:BB:SS.F` 또는 `BB:SS.F`)가 필수이고, 없거나 형식이 틀리면 −32602로 거절합니다.
브리지가 없으면(`network_bridge: "none"` 또는 빈 기본 브리지) −32602 `nic_type=sriov cannot be used without a network bridge (…)`로 거절하고, `network_mode: "tenant-overlay"`와 함께 쓰면 −32602 `nic_type=sriov cannot be combined with network_mode "tenant-overlay" (…)`로 거절합니다. 두 거절 모두 접수 전에 일어나므로 VM·디스크가 만들어지지 않습니다. 이 거절은 tenant-overlay 테넌트 판정(−32006)·QoS SLA(−32602)보다 먼저 오지만, 비ADMIN의 호스트 자원 참조 판정(10.2.1, −32006)보다는 뒤입니다. NIC 없는 VM이 목적이면 `nic_type`을 빼고 `network_bridge: "none"`을 씁니다.

> **직접 할당 안전 경계:** `sriov attach`는 PF와 VF의 IOMMU group을 서버에서 모두
> 확인한 뒤에만 첫 드라이버 변경을 시작합니다. `sriov detach`는 libvirt의 정확한
> “이미 연결되지 않음” 응답만 멱등 성공으로 처리하며, VM 부재·권한·libvirt 장애는
> 실패로 전파합니다.

### 6.10 네트워크 디버깅

#### 활용 예제 — link에서 VM NIC까지 계층별 장애 격리

한 번에 설정을 바꾸기보다 desired state, host link, bridge·OVS, 정책, VM NIC 순서로 실제 상태를 좁힙니다.
VLAN 또는 VXLAN을 쓰지 않는 구성이라면 해당 단계의 빈 결과는 정상입니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-debug-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/debugging.svg" target="_blank" rel="noopener" aria-label="네트워크 디버깅 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/debugging.svg" width="960" height="280" loading="lazy" decoding="async" alt="PureCVisor desired state에서 host link와 route, bridge와 OVS, nftables와 Security Group, libvirt VM NIC 순서로 실제 상태를 좁히는 구성도">
  </a>
  <figcaption id="pcv-network-example-debug-caption">기대 상태와 실제 상태가 처음 달라지는 계층을 장애 경계로 좁힙니다.</figcaption>
</figure>

```bash
# 1. PureCVisor desired state와 host 주소·route
pcvctl network list
ip -br address
ip -4 route show table main

# 2. Linux bridge와 연결된 port
ip -d link show type bridge
bridge link

# 3. OVS와 VXLAN actual state
sudo ovs-vsctl show
ip -d link show type vxlan

# 4. 데몬이 소유한 정책
sudo nft list table inet purecvisor

# 5. 대상 VM NIC와 persistent XML
sudo virsh domiflist web-prod
sudo virsh dumpxml web-prod
```

기본 네트워크가 목록에 있지만 bridge가 없으면 생성 또는 재수화 오류를 확인합니다.
bridge와 VM NIC가 정상인데 통신이 실패하면 nftables·Security Group을 보고, VXLAN 구성에서만 실패하면 underlay endpoint, UDP 4789와 MTU를 확인합니다.

### 6.11 네트워크 Prometheus 메트릭

#### 활용 예제 — NIC drop과 conntrack 포화 징후 확인

먼저 `ip -br link`로 실제 interface 이름을 확인한 뒤 내장 `/api/v1/metrics`에서 같은 device label을 조회합니다.
counter는 누적값이므로 한 번의 숫자보다 일정 구간의 증가율을 기준으로 판단합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-metrics-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/prometheus-metrics.svg" target="_blank" rel="noopener" aria-label="네트워크 Prometheus 메트릭 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/prometheus-metrics.svg" width="960" height="280" loading="lazy" decoding="async" alt="Linux NIC, socket과 conntrack 통계가 purecvisorsd collector와 metrics API를 거쳐 Prometheus의 증가율 및 포화 신호로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-metrics-caption">device label을 실제 interface와 맞춘 뒤 counter 증가율과 conntrack 사용 비율을 판정합니다.</figcaption>
</figure>

```bash
# eno1을 실제 uplink 이름으로 교체한다.
curl -s http://127.0.0.1:8080/api/v1/metrics | \
  grep -E '^(node_network_(receive|transmit)_(bytes|errs|drop)_total\{device="eno1"\}|node_nf_conntrack_(entries|entries_limit))'
```

관련 메트릭 이름은 다음과 같습니다.

```text
# 네트워크 인터페이스 메트릭
node_network_receive_bytes_total{device="eno1"}
node_network_transmit_bytes_total{device="eno1"}
node_network_receive_errs_total{device="eno1"}
node_network_transmit_errs_total{device="eno1"}
node_network_receive_drop_total{device="eno1"}
node_network_transmit_drop_total{device="eno1"}

# 소켓 통계
node_sockstat_TCP_inuse
node_sockstat_TCP_tw
node_sockstat_UDP_inuse

# conntrack
node_nf_conntrack_entries
node_nf_conntrack_entries_limit
```

`errs` 또는 `drop` counter가 지속적으로 증가하면 NIC·switch·qdisc를 함께 확인합니다.
`node_nf_conntrack_entries`가 limit에 가까우면 NAT·Security Group workload의 연결 수와 timeout 정책을 점검합니다.

### 6.12 Local VPC

Local VPC는 한 Single Edge 호스트에서 tenant별 VPC, 여러 IPv4 subnet과 정지 VM
attachment를 하나의 desired state로 관리합니다. 생성할 때 `linux` 또는 `ovn` backend를
고정하며, 생략하면 기존 호환값인 `linux`입니다. Linux는 subnet별 bridge/dnsmasq,
OVN은 공유 OVS `br-int` 위의 LS/LR/DHCP/LSP/Port Group/ACL을 사용합니다. 두 backend 모두
공통 nft 경계에서 `nat`와 `isolated` egress를 제공하고, 서로 다른 VPC와 VM에서 host 관리
서비스로 향하는 트래픽을 기본 차단합니다.

외부 inbound에는 Floating IP pool이나 범용 port forwarding 대신 제한형 Service
Publish를 사용합니다. 운영자는 host의 `listen_address:listen_port`를 `ACTIVE`
attachment의 `target_port`에 연결하고, 허용 source CIDR을 반드시 명시합니다. 대상 VM에는
Security Group이 연결돼 있어야 합니다. 즉 VM에 직접 공인 IP를 주지 않아도 host IP와
게시 포트를 통해 선택한 서비스만 외부에 노출할 수 있습니다.

> **Web UI + REST + CLI + RPC:** Web UI의 `인프라 > Local VPC`, 전용 `/api/v1/vpcs`
> REST resource, `pcvctl vpc` CLI와 `POST /api/v1/rpc` JSON-RPC 패스스루/UDS를 사용할 수
> 있습니다. Web UI와 CLI 변경 명령은 `accepted`의 Job ID를 `jobs.get` terminal까지
> 조회해 completed만 성공으로 확정합니다.
> `--no-wait`를 지정하면 접수와 Job ID만 즉시 반환하므로 별도 결과 확인이 필요합니다.

Local VPC 작업 전에는 Web UI의 `인프라 > 네트워크`에서 `호스트 네트워크 기준선`을 먼저
확인합니다. `GET /api/v1/networks/host-baseline`은 읽기 전용 RPC
`network.host.info`에 매핑되어 host interface·route·OVS actual을,
`GET /api/v1/vpcs/status`의 `subnet_cidrs`는 현재 tenant 범위의 VPC 주소 대역을 반환합니다.
어느 영역이 `partial` 또는 `unavailable`이면 빈 상태로 간주하지 말고 원인을 해결한 뒤
생성합니다.

Web UI의 `Local VPC 생성`은 VPC 이름·tenant·backend·egress와 첫 subnet 이름·IPv4 CIDR·MTU를
한 모달에서 받아 하나의 Job으로 생성합니다. CIDR 입력 중 gateway와 VM 할당 범위를
미리 표시하며, 첫 subnet 적용 실패 시 이번 요청에서 만든 VPC도 역순 rollback합니다.
backend 준비 상태, 현재 VPC 수와 실제 주소 pool 잔량을 제출 전에 표시하며, 생성 실패는
모달 내부의 읽을 수 있는 오류로 남겨 입력을 보존합니다.
목록에서 VPC를 선택하면 같은 화면 아래에 subnet, VM attachment와 Service Publish를
순서대로 표시합니다. egress와 두 번째 이후 subnet 변경은 상세에서 읽은 현재 revision을
자동 전달하며, 삭제·연결 해제·게시 해제는 영향 확인을 거칩니다. VIEWER는 조회,
OPERATOR는 일반 변경, ADMIN은 VPC 삭제와 전체 reconcile을 수행할 수 있으며 실제 인가는
서버가 판정합니다.

전용 REST 경로:

| HTTP | 경로 | 기능 |
|---|---|---|
| `GET` | `/api/v1/networks/host-baseline` | 읽기 전용 host interface·route·Linux bridge·OVS·VPC 기준선 |
| `GET`, `POST` | `/api/v1/vpcs` | 목록, 생성 |
| `GET` | `/api/v1/vpcs/backends` | backend readiness, 현재 수와 주소 기반 capacity |
| `GET` | `/api/v1/vpcs/status` | controller/reconcile 상태 |
| `POST` | `/api/v1/vpcs/reconcile` | 전체 desired state 수렴 |
| `GET`, `DELETE` | `/api/v1/vpcs/{vpc_id}` | aggregate 상세, 삭제 |
| `POST` | `/api/v1/vpcs/{vpc_id}/egress` | egress 변경 |
| `GET`, `POST` | `/api/v1/vpcs/{vpc_id}/subnets` | subnet 목록, 생성 |
| `DELETE` | `/api/v1/vpc-subnets/{subnet_id}` | subnet 삭제 |
| `GET` | `/api/v1/vpcs/{vpc_id}/attachments` | VM 연결 목록 |
| `POST`, `DELETE` | `/api/v1/vpc-attachments[/{attachment_id}]` | VM 연결, 해제 |
| `GET` | `/api/v1/vpcs/{vpc_id}/services` | 게시 서비스 목록 |
| `POST`, `DELETE` | `/api/v1/vpc-services[/{publish_id}]` | 서비스 게시, 해제 |

#### 활용 예제 — Linux Local VPC의 VM 서비스를 제한 게시

이 절의 전체 예제는 공개 지원 backend인 `linux`를 사용합니다.
`198.51.100.10`과 `192.0.2.0/24`는 RFC 문서용 주소이므로 실행 전 실제 node IPv4와 허용할 client CIDR로 교체합니다.

REST로 VPC와 첫 subnet을 한 Job에 생성합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-vpc-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/local-vpc.svg" target="_blank" rel="noopener" aria-label="Local VPC 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/local-vpc.svg" width="960" height="280" loading="lazy" decoding="async" alt="허용된 client CIDR이 Single Edge host의 8443 listener와 Local VPC Service Publish, Linux bridge attachment를 거쳐 web-prod VM의 443 포트로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-vpc-caption">허용 source CIDR과 host listener를 단일 VPC attachment의 target port에 제한적으로 연결합니다.</figcaption>
</figure>

```bash
curl -sS -X POST http://127.0.0.1:8080/api/v1/vpcs \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"tenant":"acme","name":"prod","backend":"linux","egress_mode":"nat","subnet_name":"web","subnet_cidr":"10.60.10.0/24","subnet_mtu":1500}'

# 응답의 job_id를 terminal까지 확인한다.
curl -sS http://127.0.0.1:8080/api/v1/jobs/<job-id> \
  -H "Authorization: Bearer $TOKEN"
```

| 메서드 | 최소 역할 | 주요 파라미터 |
|---|---|---|
| `vpc.list`, `vpc.status`, `vpc.backend.list` | VIEWER | admin은 선택적 `tenant` |
| `vpc.get`, `vpc.subnet.list`, `vpc.attachment.list`, `vpc.service.list` | VIEWER | `vpc_id` |
| `vpc.create` | OPERATOR | `name`, `egress_mode`; 선택적 `backend=linux\|ovn`과 all-or-none `subnet_name`, `subnet_cidr`, `subnet_mtu`; admin UDS 호출은 `tenant` 명시 |
| `vpc.egress.set` | OPERATOR | `vpc_id`, `egress_mode`, `expected_revision` |
| `vpc.subnet.create` | OPERATOR | `vpc_id`, `name`, `cidr`, `expected_revision`, 선택적 `mtu` |
| `vpc.subnet.delete` | OPERATOR | `subnet_id` |
| `vpc.attachment.create` | OPERATOR | `subnet_id`, `vm`, 선택적 `ip_address` |
| `vpc.attachment.delete` | OPERATOR | `attachment_id` |
| `vpc.service.publish` | OPERATOR | `attachment_id`, `protocol`, `listen_address`, `listen_port`, `target_port`, `allowed_sources` |
| `vpc.service.unpublish` | OPERATOR | `publish_id` |
| `vpc.delete`, `vpc.reconcile` | ADMIN | `vpc_id` 또는 전체 수렴 |

tenant가 없는 비ADMIN 사용자는 VPC 조회도 변경과 같은 오류(`이 작업에는 tenant가 필요합니다`, `-32002`, REST 409)로 거부된다. 빈 목록을 돌려주지 않아 "tenant 없음"과 "VPC 없음"이 구분된다. `auth.user.tenant.set`으로 tenant를 지정하면 풀린다. ADMIN의 전역 조회는 그대로다.

비ADMIN API 키의 주체는 키 이름(`client_name`)이고 tenant는 같은 이름의 사용자에서 읽는다. 그런 사용자가 없는 키는 tenant가 없어 VPC 조회가 같은 오류로 거부되고, `auth.user.tenant.set`도 `-32005`(`User '<이름>' not found`, REST 404)로 실패해 풀리지 않는다. 키 이름과 같은 사용자를 만들고 그 사용자에게 tenant를 지정하면 풀린다.

CLI로 같은 수명주기를 실행할 수 있습니다.

```bash
# backend 준비 상태와 주소 기반 capacity를 먼저 확인한다.
pcvctl vpc backends

# Linux NAT VPC와 첫 subnet 일괄 생성 — 기본값은 worker terminal 완료까지 기다린다.
pcvctl vpc create prod --tenant acme --egress nat \
  --backend linux --subnet-name web --cidr 10.60.10.0/24 --mtu 1500

# 목록과 상세 조회
pcvctl vpc list --tenant acme
pcvctl vpc get <vpc-uuid> --tenant acme

# 두 번째 이후 subnet 생성: vpc.get에서 확인한 현재 revision을 사용한다.
pcvctl vpc subnet-create <vpc-uuid> db --tenant acme \
  --cidr 10.60.20.0/24 --mtu 1500 --revision 2

# 정지 VM 연결과 제한형 Service Publish
pcvctl vpc attachment-create <subnet-uuid> web-prod --tenant acme
pcvctl vpc service-publish <attachment-uuid> --tenant acme \
  --protocol tcp --listen-address 198.51.100.10 --listen-port 8443 \
  --target-port 443 --allowed-source 192.0.2.0/24

# attachment, publish와 controller 수렴 상태 확인
pcvctl vpc get <vpc-uuid> --tenant acme
pcvctl vpc service-list <vpc-uuid> --tenant acme
pcvctl vpc status
```

같은 기능의 raw UDS RPC 예시:

```bash
# NAT VPC와 첫 subnet 일괄 생성 — 응답 Job 완료 뒤 aggregate를 확인한다.
echo '{"jsonrpc":"2.0","method":"vpc.create","params":{
  "tenant":"acme","name":"prod","egress_mode":"nat",
  "backend":"linux",
  "subnet_name":"web","subnet_cidr":"10.60.10.0/24","subnet_mtu":1500
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 두 번째 이후 subnet 생성
echo '{"jsonrpc":"2.0","method":"vpc.subnet.create","params":{
  "tenant":"acme","vpc_id":"<vpc-uuid>","name":"db",
  "cidr":"10.60.20.0/24","mtu":1500,"expected_revision":2
},"id":"2"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 정지 VM 연결
echo '{"jsonrpc":"2.0","method":"vpc.attachment.create","params":{
  "tenant":"acme","subnet_id":"<subnet-uuid>","vm":"web-prod"
},"id":"3"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# 지정한 host IPv4의 TCP 8443을 VM TCP 443으로 제한 게시
echo '{"jsonrpc":"2.0","method":"vpc.service.publish","params":{
  "tenant":"acme","attachment_id":"<attachment-uuid>","protocol":"tcp",
  "listen_address":"198.51.100.10","listen_port":8443,"target_port":443,
  "allowed_sources":["192.0.2.0/24"]
},"id":"4"}' | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

제약과 안전 경계:

- VPC subnet은 다른 VPC와 기존 host connected CIDR을 포함해 호스트 전체에서 겹칠 수
  없습니다.
- backend는 VPC 생성 뒤 바꿀 수 없고 한 VPC 안에서 혼합할 수 없습니다. OVN 장애 시 Linux로
  자동 fallback하지 않으며 generic `ovn.*`는 Local VPC 소유 row 변경을 거부합니다.
- OVN readiness는 NB/SB/northd/controller/chassis/`br-int`, 기능 parity와 `[ovn]`
  `edge_transit_pool` 충돌을 함께 검사합니다. 기본 `100.64.0.0/16`을 VPC당 `/30`으로 나누면
  이론상 16,384개지만, `allocatable_vpcs`는 이미 예약한 주소를 뺀 현재값이고
  `product_limit=null`은 무제한 보증이 아닙니다.
- attachment는 정지 VM persistent XML만 변경합니다. 실행 중 VM live attach는 아직
  지원하지 않습니다.
- 한 VM의 cross-VPC multi-homing과 tenant-overlay 동시 연결은 거부합니다.
- VPC managed bridge는 `network.*`, raw NIC attach와 `vm.create network_bridge`로
  수정할 수 없습니다.
- `isolated` VPC에는 Service Publish를 만들 수 없고, non-admin은 `0.0.0.0/0` 또는
  동등한 분할 CIDR 합집합으로 전체 공개할 수 없습니다.
- `0.0.0.0` listen은 모든 host IPv4 주소를 의미합니다. 특정 NIC만 공개하려면 해당 host
  local IPv4를 명시합니다.
- 데이터면 actual state가 불일치하면 데몬은 listener를 열기 전에 quarantine을 적용하며,
  `vpc.status`의 `reconcile_required`와 각 resource의 `last_error`를 확인해야 합니다.
- OVN 쪽 실패의 범위는 그 VPC입니다. NB 장애나 준비 미달이면 해당 OVN VPC만 `DEGRADED`가 되고 host edge가
  내려가며, Linux VPC는 계속 수렴하고 데몬도 기동합니다. 다만 NB 장애 동안에는 OVN VPC가 있는 호스트의 VPC 변경
  API가 Linux VPC 대상이어도 실패합니다. 이 실패는 Linux 방화벽을 건드리기 전에 나므로 Linux VPC 트래픽은 끊기지 않으며,
  먼저 ACL을 건 다른 OVN VPC만 되돌릴 때까지 잠깐 끊길 수 있습니다. 외부에서 Port_Group이 지워졌는데 `ACTIVE` attachment가 있는 VPC는 다른 VPC 변경을 막지 않고 그 VPC만 `DEGRADED`가 되며 host edge가 내려갑니다. 그 VPC 자신을 대상으로 한 변경은 같이 `DEGRADED`가 되고 실패로 돌아옵니다. `vpc reconcile`이 복구합니다. 생성에 실패해 `DEGRADED`·`CREATING`으로 남은 VPC는
  NB 복구 뒤 `vpc delete`로 지우거나 `vpc reconcile`로 복구합니다. Linux 방화벽 적용 실패는 지금도 전체 bridge DOWN입니다.

Linux Local VPC backend와 데이터면은 실제 VM·패킷 효과 검증을 완료했습니다. 전용 CLI는
당시 17개 action(지금은 `subnet-confirm`·`pool-set`·`pool-list`를 더해 20개)과 create/list/get/delete terminal 왕복을 검증했습니다. 선택형 OVN backend
후보는 제품 API 생성, inactive VM XML, LSP/chassis packet, NAT/isolated, SG, Service
Publish, 관리면 차단, daemon restart, schema migration과 cleanup을 검증했습니다. 다만
부팅 KVM·Linux/OVN 공존·host/controller reboot·
전 단계 fault injection 전에는 `Implemented` 후보이며 공개 지원은 Linux backend만입니다.
Network Flow/IPFIX
collector, VPC peering, Floating IP pool, live attachment와 다중 노드 router는 이번 Local
VPC 범위에 포함되지 않습니다.

#### 6.12.1 관리 대역 보호와 콘솔 복구

VPC subnet을 만들 때 호스트가 응답하는 **관리 PC 대역**을 가리면 그 PC와의 연결이 끊깁니다.
호스트의 그 대역행 응답이 VPC 브리지로 가기 때문입니다. 이를 막기 위해 데몬은 요청마다
**예약 대역**을 계산합니다 — `[vpc] reserved_cidrs`에 적은 대역과, 지금 관리 포트(`[vpc] mgmt_ports`,
기본 `22,80,443`과 REST 포트)에 ESTABLISHED로 붙어 있는 사설(RFC1918·100.64.0.0/10) 관리 피어의 `/32`입니다.

- OPERATOR가 예약 대역과 겹치는 subnet을 만들면 접수 전에 −32006으로 거절합니다(Job 없음).
- ADMIN은 만들 수 있지만 경고가 나오고, `confirm_timeout_sec`(기본 120초) 안에
  `pcvctl vpc subnet-confirm <subnet_id>`를 보내지 않으면 **자동 원복**(subnet 삭제, 첫 subnet이면
  VPC까지)됩니다. 확인 대기 중인 subnet에는 VM을 연결할 수 없습니다.
- 확인 대기 항목은 `/var/lib/purecvisor/vpc_confirm_pending.json`(0600)에 남아, 데몬을 재시작하거나
  재부팅해도 기동 때 원복됩니다. UI로 만든 ADMIN은 확인 화면이 없어 대기 시간 뒤 subnet이 사라집니다.

| 오류 | 뜻 | 조치 |
|---|---|---|
| −32006 `Permission denied: subnet CIDR overlaps a reserved host management range` | OPERATOR 요청이 예약 대역과 겹침 | 다른 대역을 쓰거나 ADMIN에게 요청. 예약 대역 계산 실패도 같은 문구로 거절(fail-closed) |
| −32005 `no pending confirmation for this subnet` | 확인할 대기 항목 없음 | 이미 확인·원복됨. `pcvctl vpc subnet-list <vpc>`로 상태 확인 |
| −32002 `subnet auto-revert in progress` | 자동 원복이 진행 중이라 확인할 수 없음 | 원복이 끝난 뒤 `pcvctl vpc subnet-list <vpc>`로 상태 확인. 원복이 실패하면 다시 확인 대기로 돌아가 확인할 수 있음 |
| −32002 `subnet is awaiting confirmation — run vpc.subnet.confirm first` | 확인 대기 subnet에 VM 연결 시도 | `pcvctl vpc subnet-confirm <subnet_id>` 뒤 연결 |
| −32602 (cidr) | 정규 표기가 아닌 cidr | 이제 접수 전에 동기로 거절(이전에는 접수 뒤 실패 Job) |

설정(`/etc/purecvisor/daemon.conf`의 `[vpc]`, 모두 선택):

| 키 | 기본 | 뜻 |
|---|---|---|
| `reserved_cidrs` | 빈 값 | 항상 보호할 대역(쉼표). **지금 붙어 있지 않은 관리 PC 대역은 여기에 적어야 합니다.** 정규 CIDR이 아닌 항목은 버리고 경고 |
| `mgmt_ports` | `22,80,443` | 관리 피어를 찾을 지역 포트(`[server] rest_port`는 자동 포함) |
| `confirm_timeout_sec` | 120 | ADMIN 확인 대기 시간, 허용 5–3600(밖이면 120) |

**콘솔 복구 절차** — 특정 대역과만 연결이 안 되고 그 대역의 게이트웨이에는 닿을 때, 호스트 콘솔에서:

1. 증상을 확인합니다. 특정 대역(관리 PC 쪽)과만 불통이고 같은 대역의 게이트웨이는 닿습니다.
2. `ip route get <관리 PC 주소>`가 `dev pcvs…`(`pcvs` 뒤 16진 11자리)처럼 **VPC 브리지**를 가리키는지 봅니다.
3. `ip -br addr show type bridge`로 그 브리지에 걸린 게이트웨이 주소(관리 대역과 겹치는 주소)를 확인합니다.
4. 지웁니다.
   - (가) 데몬이 살아 있으면: `pcvctl vpc subnet-list <vpc-uuid>` → `pcvctl vpc subnet-delete <subnet-uuid> --tenant <tenant>`
     (VPC째 지우려면 `pcvctl vpc delete <vpc-uuid> --tenant <tenant>`).
   - (나) 데몬이 기동 때 다시 세우면: `touch /etc/purecvisor/vpc-reconcile.skip`(root 소유 일반 파일이어야 함)
     뒤 재부팅하거나 `systemctl restart purecvisorsd`합니다. 기동 로그에
     `VPC reconcile skipped by /etc/purecvisor/vpc-reconcile.skip — remove it after console recovery`가
     나오고 VPC 재적용이 건너뛰어집니다. 기동 뒤 (가)로 subnet을 지우고, 플래그를 지운 다음
     `pcvctl vpc reconcile`로 나머지를 수렴시킵니다. 플래그는 데몬이 지우지 않습니다.
5. 임시로 관리 경로만 살리려면 `ip route add <관리 PC 주소>/32 via <기본 게이트웨이> dev <장치>`를 추가합니다
   (복구 뒤 `ip route del`로 지웁니다).
6. 예방: `[vpc] reserved_cidrs`에 관리 PC 대역을 적습니다. 확인 대기 경고가 오면 의도한 subnet일 때만
   120초 안에 `pcvctl vpc subnet-confirm <subnet_id>`를 보내고, 아니면 그대로 두어 원복시킵니다.

**VPC 재적용 건너뛰기 조건** — 다음 넷 중 하나면 데몬은 그 기동에서 VPC를 재적용(reconcile)하지 않고
경고와 감사 `vpc.reconcile.skip`을 남깁니다(`vpc.status`의 `boot_reconcile_skipped`가 true). 건너뛴 기동에서는
`pcvctl vpc reconcile`(ADMIN)을 실행하기 전까지 VPC가 내려가 있습니다.

| 기동 로그(WARN) | 원인 | 조치 |
|---|---|---|
| `VPC reconcile skipped by /etc/purecvisor/vpc-reconcile.skip — remove it after console recovery` | 유효한 플래그 | 복구 뒤 플래그 삭제 → `pcvctl vpc reconcile` |
| `VPC reconcile skipped: confirm ledger unreadable — fix access to <원장 경로> (do not delete it), …` | 원장 파일을 읽지 못함(권한·I/O) | **원장을 지우지 말고** 접근을 고친 뒤 데몬 재시작 또는 `pcvctl vpc reconcile` |
| `VPC reconcile skipped: confirm ledger was unreadable — inspect and remove <….corrupt-…>, …` | 깨진 원장이 `.corrupt-*`로 옮겨져 남아 있음(**지울 때까지 매 기동**) | 아래 `.corrupt-*` 순서 |
| `VPC reconcile skipped: auto-revert failed for <n> pending subnet(s) — fix the cause and run vpc.reconcile` | 기동 원복 실패(원복은 30초 간격 3회까지 재시도) | 원인(예: OVN NB 미준비)을 고친 뒤 `pcvctl vpc reconcile` |

`.corrupt-*` 처리 순서: ① 파일 내용을 확인합니다(어떤 subnet이 확인 대기였는지). ② 위험 subnet이 남았는지
`pcvctl vpc subnet-list <vpc>`로 보고 필요하면 지웁니다. ③ `.corrupt-*` 파일을 삭제합니다.
④ `pcvctl vpc reconcile`을 실행합니다.

#### 6.12.2 테넌트 주소 풀

OPERATOR가 만드는 VPC subnet은 **ADMIN이 그 테넌트에 배정한 주소 풀 안**에서만 만들어집니다. 풀은 테넌트마다
CIDR 목록이고, subnet 대역 하나가 풀의 CIDR 하나에 **완전히** 들어가야 합니다. ADMIN의 요청에는 풀 판정이 없습니다.
풀로 쓸 수 있는 대역은 subnet 생성이 받는 IPv4 공간(RFC1918과 `100.64.0.0/10`)에서 정규 표기이면서 그 블록 하나에
들어가는 대역입니다(`10.0.0.0/7`처럼 블록 밖으로 걸치거나 정규 표기가 아닌 값은 −32602). `pool-set` 목록
안에서 같은 값이 반복되거나 서로 포함·걸치면 거절되고(−32602), `vpc_pool` 일괄 등록에서는 이미 풀에 있는 같은 값이 한 항목으로
합쳐지며(멱등) 기존 풀 대역을 포함하거나 걸치는 값은 거절됩니다.

**ADMIN 절차**

- 배정: `pcvctl vpc pool-set <tenant> <cidr>[,<cidr>…]` — 목록을 **통째로 교체**합니다. 빈 값 `""`이면 풀을 비웁니다.
- 조회: `pcvctl vpc pool-list [--tenant <tenant>]` — 테넌트와 CIDR 목록. `--tenant` 없이는 풀이 있는 모든 테넌트를 보입니다.
- 결과의 `warnings`: 다른 테넌트의 풀(`overlaps pool of tenant <t>: <cidr>`)이나 예약 대역(6.12.1,
  `overlaps reserved management range: <cidr>`)과 겹치면 **막지 않고 알립니다**. 의도한 배정인지 확인하세요.
- 지금 쓰는 대역 확인: `pcvctl resource unregistered --tenant <tenant> --json`의 `vpc_subnets`는 그 테넌트 풀 밖에 있는
  기존 subnet(`tenant`·`vpc_id`·`vpc_name`·`cidr`)을 보입니다(`--tenant`로 거르지 않으므로 항목의 `tenant`를 보고 검토합니다). 그 항목을 `{"kind":"vpc_pool","tenant":"<t>","cidr":"<c>"}`로
  바꿔 `resource.import`하면 풀에 **합쳐집니다**(교체가 아니며 되풀이해도 같습니다). 지금 `vpc_pool` import는 UDS RPC로만
  됩니다 — `pcvctl resource import` 파일은 `vpc_pool` 항목의 `tenant`·`cidr`를 나르지 못합니다.

| 오류 | 뜻 | 조치 |
|---|---|---|
| −32006 `Permission denied: subnet CIDR is outside this tenant's address pool` | OPERATOR 요청 대역이 자기 테넌트 풀 밖이거나 풀이 비어 있음(풀을 읽지 못해도 같은 문구, fail-closed). 풀·요청 값은 알려 주지 않음 | ADMIN에게 풀 배정을 요청하거나 풀 안 대역을 쓴다 |
| −32006 `Permission denied: subnet CIDR overlaps a reserved host management range` | 풀 안이지만 예약 대역(6.12.1)과 겹침. 풀 판정이 **먼저**이고 그다음에 예약 판정입니다 | 다른 대역을 쓰거나 ADMIN에게 요청 |
| −32002 `이 작업에는 tenant가 필요합니다` | 테넌트가 없는 비ADMIN의 VPC 변경·조회 | ADMIN이 `pcvctl auth tenant <user> <tenant>` |
| −32002 `pool change would leave subnets outside the pool: <cidr>[, …]` | `pool-set`이 그 테넌트가 이미 쓰는 subnet을 풀 밖으로 밀어냄(**축소 거부**, 저장되지 않음). cidr를 읽을 수 없는 subnet도 밖으로 셈 | 풀에 그 대역을 포함하거나, 먼저 그 subnet을 지운다 |
| −32602 `cidrs[N] must be a canonical private IPv4 network (RFC1918 or 100.64.0.0/10): <값>` 등 | 형식·허용 공간 위반, 같은 목록 안 중복·겹침(`cidrs must not repeat or overlap each other`) | 값을 고친다. 아무것도 바뀌지 않음 |

테넌트는 서버가 정합니다 — RBAC 사용자 테넌트이고, gRPC는 설정 `[grpc] tenant`(없으면 테넌트 없음)입니다. REST 본문의
`_pcv_caller_tenant`는 서버가 지웁니다. 테넌트 없는 비ADMIN과 테넌트 이름 규칙을 어긴 옛 계정(테넌트 없음으로 봅니다)은
VPC 목록·조회도 −32002 `이 작업에는 tenant가 필요합니다`(REST 409)로 거절됩니다(6.12). 두 RPC(`vpc.pool.set`·`vpc.pool.list`)는
ADMIN 전용입니다.

**업그레이드(2.0.4 이하 → 2.1.0 이상)** — VPC를 쓰는 테넌트의 OPERATOR는 업그레이드 직후 풀이 비어 있어 **새 subnet을 만들 수 없습니다**.
기존 subnet과 VPC는 그대로 동작합니다. 위 절차로 `vpc_subnets`를 확인하고 풀을 배정하세요(10.2.1 단계 6).
풀과 자원 등록은 2.1.0에서 새로 생기는 `resource_registry.db`(`user_version` 2)에 저장됩니다. 2.0.4 이하는 이 파일을 쓰지 않으므로
되돌리면 테넌트 주소 풀과 호스트 자원 경계가 적용되지 않습니다. 파일은 지우지 말고 보존합니다 — 다시 올리면 그대로 열립니다.

## 8. 모니터링 & 알림

PureCVisor는 외부 에이전트 없이 자체 메트릭 수집, 알림 엔진, Prometheus 호환 엔드포인트를 내장한다.

Web UI `관제` 메뉴의 화면과 이 가이드에서 그 화면을 다루는 절은 다음과 같다. `감사 로그`는 ADMIN에게만 보인다.

| 관제 화면 | 관련 절 |
|-----------|---------|
| 운영 개요 | 8.1 자체 node_exporter, 12.4 승인 큐(자가치유 대기 액션) |
| 이벤트 센터 | 8.1 자체 node_exporter(호스트·VM 요약), 알림·감사 로그 화면 바로 가기 |
| 알림 | 8.3 알림 엔진, 8.5 알림 ACK & 에스컬레이션, 8.9 DLQ |
| 보안 이벤트 | 10.7 Native Host HIDS/HIPS, 10.12 Suricata DPI/IDS/IPS |
| 감사 로그 (ADMIN) | 10.10 감사 로그 |
| 활동 로그 | 따로 다루는 절 없음 — 이 브라우저가 보낸 API 요청 기록 |
| VM 모니터 | 8.1 자체 node_exporter |
| 스토리지 모니터 | 8.1 자체 node_exporter, 5.6 Prometheus 스토리지 메트릭 |
| 호스트 모니터 | 8.1 자체 node_exporter |
| 히트맵 | 따로 다루는 절 없음 — VM 목록의 현재 CPU 조회값 |
| API 성능 | 따로 다루는 절 없음 — 주요 REST 조회의 응답 시간을 브라우저에서 잰 값 |
| 자가치유 | 12.3 자가치유 5계층 안전 스택, 12.4 승인 큐 |

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
- `purecvisor_zpool_*` (6개: state, errors_read, errors_write, errors_cksum, scrub_age_seconds, capacity_percent)
- `purecvisor_worker_pool_pending` (GTask 큐 깊이)
- `purecvisor_audit_queue_depth` (감사 큐)
- `purecvisor_connpool_idle/active/max` (커넥션 풀)

```bash
# Prometheus 메트릭 엔드포인트 (인증 불필요)
curl -s http://127.0.0.1:8080/api/v1/metrics

# 특정 메트릭 필터
curl -s http://127.0.0.1:8080/api/v1/metrics | grep purecvisor_cb_state
```

#### 모니터 화면의 IP 출처

`monitor.fleet`은 VM IP를 다음 순서로 찾고 찾은 곳을 `ip_source`로 알린다: libvirt DHCP lease(`lease`) → 게스트 에이전트 캐시(`guest-agent`) → dnsmasq lease 파일(`lease-file`) → 호스트 ARP 표(`arp`, 완성된 항목만). 에이전트 IP는 메인 루프 밖에서 조회하므로 처음에는 보이지 않다가 한 폴링 주기 늦게 나타날 수 있다(§3.7). 첫 NIC가 direct 방식인 멀티 NIC VM은 lease 파일 단계에서 놓칠 수 있다.

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
  http://127.0.0.1:8080/api/v1/processes | python3 -m json.tool

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

#### 런타임 설정 변경과 revision

알림 설정은 process-local `config_revision`으로 낙관적 동시성을 제어한다.
`alert.config.set` 요청은 `alert.config.get`에서 읽은 양의 정수
`expected_revision`을 반드시 포함해야 한다. 서버는 revision 비교, 부분 patch 적용,
최종 설정 전체 검증, commit, revision 증가를 하나의 원자 구간에서 수행한다.

```bash
# CLI는 내부에서 GET→SET을 한 번씩 수행한다.
pcvctl alert set --cpu_warn 82

# raw JSON-RPC를 사용할 때는 조회한 revision을 직접 전달한다.
echo '{"jsonrpc":"2.0","method":"alert.config.set","params":{"expected_revision":7,"cpu_warn":82},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

- 성공한 SET과 유효한 reload만 revision을 정확히 1 증가시킨다.
- 오래된 revision은 `-32002`로 거절되며 CLI는 자동 재시도하지 않는다.
- CPU/Memory/Disk/DataPool 임계값은 각각 `0..100` 범위이며
  `Warning < Critical`이어야 한다. `eval_period`는 `5..600`초다.
- 잘못된 타입, 알 수 없는 키, 임계값 쌍/범위 위반, 잘못된 URL은
  `-32602`로 요청 전체가 거절된다. 실패 시 설정과 revision은 변하지 않는다.
- `alert.config.reload`의 source가 잘못되면 현재 런타임 설정을 보존하고
  `daemon_config_valid=false`, `daemon_config_error=invalid_alert_config`로 경고한다.
- `webhook_secret` 원문은 GET/SET 성공 응답, 로그, 오류에 반환하지 않는다.
  응답의 `webhook_secret_configured` boolean으로 설정 여부만 확인한다.
- 시작 source가 잘못되면 자동 metric 평가가 꺼진 안전 기본값으로 시작한다.
- `enabled=false`는 CPU/메모리/디스크/DataPool 자동 평가만 멈춘다. 보안 이벤트, 직접 운영
  이벤트, 히스토리, ACK, 에스컬레이션 처리는 계속 동작한다.
- `alert.config.set`과 `pcvctl alert set` 변경은 현재 프로세스에만 적용된다.
  daemon.conf를 영속 수정하지 않으며, 프로세스 재시작 시 daemon.conf 값으로 복원된다.

라이브 JSON-RPC 계약은 응답 가능한 실제 daemon에서 strict 모드로 검증한다.
daemon socket이 없거나 응답하지 않으면 SKIP이 아니라 실패한다.

```bash
sudo env PCV_R7_ALERT_LIVE=1 \
  bash tests/integration/test_alert_config_live.sh
```

이 전용 gate는 `daemon.conf`를 root 전용 백업으로 보존하고 모든 종료 경로에서
원본 파일과 서비스를 복구한다. VM·스토리지·호스트 네트워크는 변경하지 않는다.

#### DataPool 디스크 모니터링

`[storage] image_dir`가 놓인 파일시스템의 사용률을 `DataPool` 지표로 평가해 `data_pool_warn`/`data_pool_crit`(기본 80/90)를 넘으면 알림을 발생시킨다. ZFS 풀 상태·용량은 따로 `purecvisor_zpool_*` 메트릭으로 나간다.

### 8.4 비동기 웹훅

GTask 스레드에서 비동기로 웹훅을 발송하여 메인 루프를 블로킹하지 않는다.

| 형식 | 설정값 | 페이로드 |
|------|--------|---------|
| Slack | `webhook_format=slack` | `{"text": "..."}` |
| Telegram | `webhook_format=telegram` | `{"chat_id": "...", "text": "..."}` |
| Generic | `webhook_format=generic` | PureCVisor JSON 전문 |

`webhook_secret`이 설정된 웹훅에는 HMAC-SHA256 서명 헤더
`X-PureCVisor-Signature`를 포함한다.

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
data_pool_warn=80
data_pool_crit=90
eval_period=30
dedup_window=300
webhook_url=https://hooks.slack.com/services/T.../B.../xxx
webhook_crit_url=https://events.example.com/purecvisor/critical
webhook_secret=ENC:...
webhook_format=slack
telegram_chat_id=
```

### 8.5 알림 ACK & 에스컬레이션

확인되지 않은 CRIT 알림은 발생 10분 뒤 한 번, `[ESCALATION] Unacknowledged CRIT …` 메시지로 `webhook_crit_url`(없으면 `webhook_url`)과 Web Push에 다시 보낸다. WARN은 다시 보내지 않는다.

```bash
# 알림 히스토리 조회
pcvctl alert list

# 알림 확인(ACK) — CLI에는 ACK 명령이 없다. 아래 REST·RPC나 웹 UI를 쓴다

# REST
curl -s -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/alerts | python3 -m json.tool

# REST — per-alert ACK (OPERATOR 이상)
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/alerts/<alert-id>/ack

# RPC
echo '{"jsonrpc":"2.0","method":"alert.history","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock

# RPC — per-alert ACK
echo '{"jsonrpc":"2.0","method":"alert.ack","params":{"alert_id":501},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

웹 UI에서도 같은 동작을 한다. **관제 → 알림**의 알림 이력 표 마지막 열이 `확인` 열이며,
미확인 행에는 확인 버튼이, 확인된 행에는 `ACK` 배지가 놓인다. 버튼을 누르면 해당 행만
즉시 배지로 바뀌고(필터·페이지 상태 유지) 포커스는 같은 표의 다음 미확인 버튼으로 옮겨간다.
표 머리의 **전체 확인**은 확인 대화상자를 거친 뒤 미확인 항목을 20건씩 나눠 처리하고 실패
건수를 토스트로 알린다. ACK 권한은 **OPERATOR 이상**이라 VIEWER 세션에서는 두 버튼 모두
보이지 않는다. 글로벌 statusbar의 Critical 세그먼트는 `+N warn · M unack`으로 미확인 잔량을
같이 보여주고, 모바일 알림 탭의 각 행에도 같은 확인 버튼이 있다.

### 8.6 SLA 추적

VM별 가동 시간을 추적하여 `uptime_percent`를 계산한다.

```bash
# CLI
pcvctl vm sla web-prod

# RPC
echo '{"jsonrpc":"2.0","method":"vm.sla.report","params":{"name":"web-prod"},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

### 8.7 per-VM 알림 라우팅

VM별 웹훅 대상을 지정하는 RPC·CLI·화면은 없다. 심각도별로 나누려면 `webhook_url`(전체)과 `webhook_crit_url`(CRIT 전용)을 쓴다.

### 8.8 복합 알림 규칙

CPU·Memory·Disk 가운데 두 지표를 AND/OR로 묶은 복합 규칙을 최대 8개까지 `alert.config.set`의 `composite_rules`로 설정한다. `expected_revision`이 필요하고 배열 전체를 바꿔 쓴다. 임계값은 `0..100`, `level`은 `WARN`·`CRIT`이다.

```json
{
  "jsonrpc": "2.0",
  "method": "alert.config.set",
  "params": {
    "expected_revision": 7,
    "composite_rules": [
      {
        "metric_a": "CPU", "thresh_a": 90,
        "op": "AND",
        "metric_b": "Memory", "thresh_b": 85,
        "level": "CRIT"
      }
    ]
  },
  "id": "1"
}
```

### 8.9 DLQ (Dead Letter Queue)

웹훅 발송 실패 시 DLQ에 최대 1000건까지 보관하며, 재시도 가능하다.

웹훅 URL은 자격 증명으로 다룬다. DLQ 조회(`webhook.dlq.list`·`alert.dlq.list`)와 재전송(`webhook.dlq.retry`)은 ADMIN 전용이다. `alert.config.get`·`alert.action.list`·`vm.event.webhook.list`는 ADMIN에게만 원문을 주고 VIEWER·OPERATOR에게는 scheme+host만 준다(키 이름과 형식은 같다). 시작·재시도·실패 로그에도 scheme+host만 남고, URL이 든 REST 요청 본문은 로그에서 가린다.

`webhook.dlq.retry`는 한 번 호출에 항목 **100개, 총 60초**까지만 다시 보낸다(데몬이 종료 중이면 중단). 상한에 걸리면 남은 항목은 DLQ에 그대로 두고 결과에 `remaining`(남은 건수)과 `capped`(`true`면 상한에 걸림)를 담는다 — 기존 `retried`·`succeeded`·`failed`는 그대로다. `capped`이면 같은 호출을 다시 하면 이어서 보낸다. 오래 걸릴 수 있어 클라이언트 대기 시간이 늘었다: REST 70초, CLI 70초, 웹 UI 75초. ADMIN 호출이 겹치면 순서대로 처리된다.

### 8.10 Prometheus 연동

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'purecvisor'
    scrape_interval: 15s
    metrics_path: '/api/v1/metrics'
    authorization:
      type: Bearer
      credentials_file: /etc/prometheus/purecvisor.token
    static_configs:
      - targets:
        - '192.0.2.19:80'
        - '192.0.2.20:80'
        - '192.0.2.21:80'
```

Grafana는 위 Prometheus를 데이터 소스로 추가해 대시보드를 직접 만듭니다(공개판은 대시보드 정의 파일을 제공하지 않습니다).

`[metrics] auth`는 `/metrics`·`/internal/vms`·`/internal/telemetry`·`/health/recent-errors` 넷을 함께 다룹니다. 기본 `loopback`은 호스트에서 데몬 평문 리스너(`127.0.0.1:8080`)로 곧장 온 요청(전달 헤더 없음)만 무인증으로 받고, 원격·nginx 경유 요청에는 토큰을 요구합니다. `required`는 언제나 토큰, `none`은 언제나 무인증(기동 경고)이며 그 밖의 값은 `required`로 봅니다. 원격 Prometheus는 가장 낮은 권한의 API 키(RPC/REST `auth.apikey.create`에 `role: 0`을 명시 — 생략하면 role 1(OPERATOR)이고 `pcvctl`에는 이 명령이 없습니다)를 `authorization: { type: Bearer, credentials_file: … }`로 보냅니다. 전달 헤더를 붙이지 않는 사용자 정의 프록시 뒤 요청은 직접 로컬로 보이므로 그런 프록시는 쓰지 않습니다.

`purecvisor_mainloop_stalls_total{level="warn"}`·`{level="crit"}`는 메인 루프 정지 감지 횟수(counter)입니다. 평소 0이 정상이고, 늘면 17.1절의 정지 로그와 `current RPC`를 봅니다.

### 8.11 WebSocket 메트릭 Push

`ws://127.0.0.1:8080/api/v1/ws/events`에 연결한 뒤 5초 안에 `{"type":"auth","token":"<access_token>"}`를 보내야 한다. 성공하면 `{"type":"auth_ok"}`가 오고, 5초 안에 인증하지 않으면 연결을 닫는다(URL의 `?token=`은 무시한다). 이벤트는 `{"type":…,"ts":…,"payload":{…}}` 형식이며 `metric`(텔레메트리 갱신마다, `payload.vm_count`)·`job.complete`·`healing`·`anomaly`·`forecast`가 온다.

```javascript
const ws = new WebSocket('ws://127.0.0.1:8080/api/v1/ws/events');
ws.onopen = () => ws.send(JSON.stringify({ type: 'auth', token: TOKEN }));
ws.onmessage = (e) => {
    const ev = JSON.parse(e.data);
    // { type: "metric", ts: 1762819200, payload: { vm_count: 3 } }
};
```

### 8.12 관측성 트레이싱 (retis, 2.0)

2.0은 실 네트워크 경로를 커널 레벨에서 추적·프로파일링하는 `debug.trace.*` 제어면을 추가했습니다. 백엔드는 retis이며, 5-tuple 필터로 특정 VM/테넌트/프로토콜/IP의 패킷 경로와 drop 지점을 잡습니다. 진단(디버그) 용도이므로 강한 안전 가드가 걸립니다.

- **타임박스 필수**: `timebox_sec`(1..3600초, 무기한 캡처 금지)을 반드시 지정해야 하며, 만료 시 수집이 자동 종료됩니다.
- **동시성 1**: 동시에 하나의 추적만 실행되도록 가드합니다.
- **오버레이 netns**: 대상이 테넌트 오버레이 VM이면 해당 엔드포인트 netns에 진입해 추적합니다.
- **순환 보존 + 부팅 fail-safe purge**: 산출물은 순환 보존되며, 부팅 시 잔여 추적 상태를 정리합니다.
- **report**: `debug.trace.report`는 수집 결과에서 병목·drop 지점을 분석해 돌려줍니다.

retis가 설치되어 있지 않거나 self-check에 실패하면 시작 요청은 명시 오류를 반환합니다.
overlay의 `nsenter` 내부는 중앙 launcher의 고정 PATH(`/usr/sbin:/usr/bin:/sbin:/bin`)를 사용합니다.
같은 Retis binary가 이 경로에서도 실행 가능해야 합니다. usr/local-only 설치의 native self-check
성공은 overlay 실행 가능성을 증명하지 않으며 해당 exit 127 실패를 별도로 확인해야 합니다.

**종료 요청과 완료(ADR-0064, 2026-10-08 공개 소스 반영)**: `debug.trace.stop`의
`stopped=true`는 요청 접수 의미이며 `stop_requested=true`도 반환합니다. 회수 대기 중
status는 `state=running, stop_requested=true`이고 동시 1개 가드와 `.running` 마커를
유지합니다. 동일 자식의 wait 성공 뒤에만 종료 audit·마커 제거·idle 전이·가드 해제가
완료됩니다. RPC 접수 감사 자체는 캡처 완료 증거로 사용하지 않습니다.
대기 중 같은 id의 stop은 멱등 접수, 완료 뒤에는 NOT_FOUND입니다.
wait 확인 실패는 완료로 추정하지 않고 running·마커·가드와 warning을 유지합니다.
**수집 권한**: 수집 호출만 keep-5+BPF+SYSLOG capability를 사용하며 일반 child와 daemon Effective
capability는 그대로 유지합니다.

> **RPC 전용(2.0)**: `debug.trace.*` 네임스페이스는 전용 REST 경로·CLI 서브커맨드·UI 페이지가 없습니다. `POST /api/v1/rpc` JSON-RPC 패스스루(또는 UDS 직접 `nc -U`)로만 호출하며, 5개 메서드 모두 ADMIN 권한이 필요합니다(전 테넌트 트래픽 관찰 표면).

| 메서드 | 파라미터 | 용도 |
|--------|----------|------|
| `debug.trace.start` | `{timebox_sec, [vm], [tenant], [proto], [src_ip], [dst_ip]}` | 추적 시작(추적 ID·산출 폴더 반환) |
| `debug.trace.stop` | `{trace_id}` | 종료 요청 접수(완료는 status idle로 확인) |
| `debug.trace.status` | `{[trace_id]}` | running/idle 상태·종료 요청 여부 |
| `debug.trace.list` | `{}` | 추적 목록 |
| `debug.trace.report` | 추적 ID 지정 | 추적 리포트(병목·drop 분석) |

```bash
# 특정 VM 트래픽 30초 추적 (POST /api/v1/rpc 패스스루)
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  http://localhost:80/api/v1/rpc \
  -d '{"jsonrpc":"2.0","method":"debug.trace.start","params":{
        "timebox_sec":30,"vm":"web-prod"},"id":"1"}'
```

### 8.13 브라우저 Web Push 알림 (2.0.0)

`2.0.0`부터 데몬이 WARN/CRIT 알림을 **브라우저 구독으로 직접 발송**합니다. 중계 서비스를 두지 않고
RFC 8030(Web Push 프로토콜)·RFC 8291(aes128gcm 메시지 암호화)·RFC 8292(VAPID)를 데몬이 직접
구현하므로 신규 런타임 의존이 없습니다(이미 링크된 libcrypto·libsoup3·sqlite3만 사용). 브라우저
사업자의 push 서비스는 암호문만 중계하며 알림 본문을 읽지 못합니다.

**구독 켜기**

| 화면 | 위치 |
|------|------|
| 데스크톱 | 상단 우측 톱니(⚙) → 설정(Preferences) 모달의 **알림** 섹션 → `알림 구독` 버튼 |
| 모바일 | **알림** 탭 상단 `푸시 알림` 카드의 같은 버튼 |

두 곳은 같은 토글 컴포넌트라 버튼 라벨과 상태 문구가 동일합니다. 구독된 상태에서는 라벨이
`구독 해제`로 바뀝니다. 소유자를 서버가 호출자 신원으로 고정하므로 **VIEWER도 자기 구독은
등록·해지·조회할 수 있습니다**.

**구독의 소유자와 상한**

구독은 계정별로 귀속됩니다. 클라이언트가 소유자를 지정할 수 없고, 조회(`push.mine`)도 인자 없이
본인 것만 돌아옵니다(endpoint 요약 + SHA-256 지문이라 원문 왕복 없이 대조합니다). 한 사용자가
가질 수 있는 구독 행은 **최대 10개**이며(브라우저·기기마다 1행), 초과하면 `400`으로 거부합니다.
이미 본인이 가진 endpoint를 다시 등록하는 것(자가치유 재등록)은 행이 늘지 않으므로 상한과
무관합니다.

| 메서드 | REST | 권한 | 용도 |
|--------|------|------|------|
| `push.vapid.get` | `GET /api/v1/push/vapid` | VIEWER | 구독에 필요한 VAPID 공개키 조회 |
| `push.subscribe` | `POST /api/v1/push/subscribe` | VIEWER | 브라우저 구독 등록(endpoint 기준 upsert) |
| `push.unsubscribe` | `POST /api/v1/push/unsubscribe` | VIEWER | 구독 해지(비-ADMIN은 본인 소유 행만) |
| `push.mine` | `GET /api/v1/push/mine` | VIEWER | 본인 구독만 조회(인자 없음) |
| `push.list` | `GET /api/v1/push/subscriptions` | ADMIN | 전체 구독 목록(브라우저 키 `p256dh`/`auth`는 미포함) |
| `push.vapid.rotate` | `POST /api/v1/push/vapid/rotate` | ADMIN | VAPID 키 재생성 + 전 구독 폐기 |
| `push.test` | `POST /api/v1/push/test` | ADMIN | 테스트 알림 큐잉(운영 진단) |

**설정 (`daemon.conf`)**

```ini
[webpush]
# 기본 true. false면 발송과 구독 등록이 모두 멈춘다
enabled = true
# "warn" | "crit" — 이 심각도 이상만 발송 (기본 warn)
min_severity = warn
# RFC 8292 VAPID sub. mailto: 또는 https: 스킴 필수, 미설정 시 생략
contact = mailto:ops@purecvisor.example.com
```

`min_severity`와 `contact`는 허용 목록·스킴 검증을 거치며, 위반하면 기동을 막지 않고 기본값으로
되돌린 뒤 경고를 남깁니다.

**secure context 요건 (자주 걸리는 함정)**

브라우저의 Service Worker와 Push API는 **secure context**에서만 동작합니다 — `https://`이거나
`http://127.0.0.1` 계열이어야 합니다. 자체서명 인증서로 HTTPS를 올린 노드에서는 브라우저가
Service Worker 등록 자체를 거부하고, 그러면 구독은 어떤 방법으로도 되지 않습니다. 이 경우 토글은
**누르기 전에 잠기고** 사유가 버튼 옆에 그대로 표시됩니다.

| 사유 | 표시되는 안내 |
|------|---------------|
| 인증서 미신뢰로 SW 등록 차단 | 인증서를 신뢰 목록에 추가한 뒤 재시도 안내(로컬 노드면 `http://127.0.0.1` 계열 주소는 인증서 없이 동작) |
| 그 외 SW 등록 실패 | 브라우저가 준 실패 사유 원문 |
| 브라우저 알림 권한이 `denied` | 사이트 권한을 허용으로 바꾸라는 안내 |
| Push API 미지원 브라우저 | 지원하지 않는다는 안내 |

"페이지를 새로고침하세요"는 인증서 문제에서는 영원히 틀린 안내이므로, 등록 실패가 인증서
계열로 판정되면 그 문구를 쓰지 않습니다.

> **iOS(Safari)**: 이 화면을 홈 화면에 PWA로 설치한 경우에만 푸시가 도착합니다. 일반 브라우저 탭
> 상태에서는 구독을 켜도 알림이 오지 않습니다.

**구독 인수인계와 자가치유**

- 브라우저가 endpoint를 교체하면(`pushsubscriptionchange`) Service Worker가 자동으로 재구독하고, 열려 있는 페이지가 그 결과를 서버에 반영합니다 — 운영자가 다시 켤 필요가 없습니다.
- ADMIN이 `push.vapid.rotate`를 돌리면 기존 구독은 전부 무효가 됩니다. 열려 있는 탭은 최대 1분 안에 스스로 재구독해 수렴합니다(옛 키로 만든 구독은 폐기하고 새로 만듭니다).
- 구독 등록 시 endpoint는 서버의 SSRF 가드를 통과해야 합니다. 거부되면 사람이 읽는 사유가 토글 옆에 그대로 노출됩니다.

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

### 9.2 fsfreeze/thaw

스냅샷 직전에 `virsh domfsfreeze`(Guest Agent)로 VM 파일시스템을 멈추고, 직후 `virsh domfsthaw`로 푼다. Guest Agent가 없거나 동결에 실패하면 경고만 남기고 동결 없이 스냅샷을 만든다.

### 9.3 무결성 검증

백업 완료 후 `zfs list` 명령으로 스냅샷 존재 여부와 크기를 검증한다.

### 9.3a 복원(`backup.restore`) 잠금과 재기동

다른 작업 중인 VM은 `-32004`(BUSY)로 바로 거절한다. 원래 켜져 있던 VM은 정식 `vm.start`로 다시 켜고 그 결과로 작업이 끝난다.

### 9.4 GTask 비동기

백업 작업은 GTask 스레드에서 비동기 실행되어 메인 GMainLoop를 블로킹하지 않는다. fire-and-forget 패턴:

```
클라이언트 요청 → 즉시 "accepted" 응답 → GTask 백그라운드 실행
```

### 9.5 원격 ZFS 복제 (범위 밖)

원격 노드 증분 복제는 Multi 제어면과 연결되는 범위이므로 Single Edge 공개판의 운영 절차로 제공하지 않는다. Single Edge에서는 로컬 ZFS snapshot, 로컬 백업 정책처럼 단일 노드에서 검증 가능한 백업 경로를 우선 사용한다.

RPC(`backup.replicate`)를 직접 쓰는 경우 대상 노드의 SSH 호스트 키를 먼저 등록해야 한다(2.1.0). 복제 ssh는
`StrictHostKeyChecking=yes`와 복제 전용 `/var/lib/purecvisor/replication_known_hosts`만 쓰며 `/root/.ssh/*`,
`/etc/ssh/ssh_config`, 전역 known_hosts는 읽지 않는다. 등록·조회·삭제는 ADMIN 전용이다.

```bash
pcvctl backup hostkey scan node-b                 # 키 형식·지문·상태(untrusted|trusted|mismatch)
# 원격 콘솔에서: ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub  → 지문을 눈으로 대조한다
pcvctl backup hostkey trust node-b SHA256:<지문>   # 서버가 다시 스캔한 지문과 같을 때만 등록
pcvctl backup hostkey list
pcvctl backup hostkey remove node-b               # 원격 재설치로 키가 바뀌면 remove → 대조 → trust
```

| 코드 | 이름 | 의미 |
|------|------|------|
| -32012 | HOSTKEY_UNTRUSTED | 대상에 등록된 키가 없다 — `backup.replicate`는 Job 없이 거절된다 |
| -32013 | HOSTKEY_MISMATCH | 대상이 보인 키가 기대한 키와 다르다 — trust에서는 RPC 오류, replicate에서는 `accepted` 뒤 Job 실패(`jobs.get` 결과의 `error_code`) |

- 키가 바뀌었으면 Job은 어떤 원격 명령·전송보다 먼저 실패한다(전송 0바이트). 연결 불가는 지금처럼 일반 Job 실패다.
- 업그레이드 직후에는 등록 파일이 비어 있어 모든 복제가 −32012로 거절된다(유예 스위치 없음). 대상마다 위 절차를 한 번 한다.
  `/root/.ssh/known_hosts`의 기존 키는 가져오지 않는다.
- IP와 호스트명은 서로 다른 대상이다(대소문자는 무시). 스캔은 인증되지 않은 채널이므로 대조 없이 trust하지 않는다.
- `ssh-keyscan`이 없으면 scan·trust가 −32000(`ssh-keyscan not found`)으로 실패한다. replicate는 쓰지 않는다.
- 등록 파일(`replication_known_hosts`)은 데몬이 쓰고 데몬이 읽는 단일 정본이다. 손으로 편집하는 것은 지원하지 않는다 —
  ssh는 모듈이 무시하는 줄(같은 대상의 둘째 줄, 주석이 붙은 4필드 줄, 대소문자가 다른 항목)도 일치로 볼 수 있어 모듈의 판정과 어긋난다.
  키를 바꿀 때는 `remove` → `trust`를 쓴다.

### 9.6 원격 보존 정책

원격 노드 보존 정책은 Multi 제어면 참고 범위다. Single Edge 공개판에서는 로컬 snapshot 보존 정책을 기준으로 운영한다.

### 9.7 외부 오브젝트 백업 제거

S3·S3 호환 오브젝트 스토리지로 백업을 올리는 기능은 제거되었습니다.
로컬 백업 정책·증분·검증·복구·보존 기능은 유지합니다. 기존 외부 객체·스냅샷과 설정은
자동으로 삭제하지 않습니다.

<a id="98-히스토리-페이지네이션-v10"></a>

### 9.8 백업 히스토리

`backup.history`(OPERATOR 이상, `vm_name` 필수)는 그 VM의 자동 백업 스냅샷 이름 목록을 한 번에 돌려준다. 페이지 인자는 없다.

```bash
pcvctl backup history web-prod
```

### 9.9 스냅샷 일괄 삭제

prefix 필터와 keep_recent 보존 옵션을 지원하는 일괄 삭제:

```bash
# 자동 백업 스냅샷(pcv-auto- 접두사) 중 최근 3개 보존, 나머지 삭제 (ADMIN)
# CLI: pcvctl vm snapshot-delete-all web-prod --prefix pcv-auto- --keep 3
echo '{"jsonrpc":"2.0","method":"vm.snapshot.delete_all","params":{
  "name":"web-prod",
  "prefix":"pcv-auto-",
  "keep_recent":3
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

---

## 10. 보안

PureCVisor는 엔터프라이즈급 다계층 보안 아키텍처를 구현한다.

### 10.1 인증

#### JWT HS256

| 항목 | 값 |
|------|-----|
| 알고리즘 | HS256 (HMAC-SHA256) |
| Access Token 만료 | 15분 |
| Refresh Token 만료 | 7일 |
| IP 바인딩 | Access Token에 클라이언트 IP 포함, 불일치 시 거부 |
| 비밀 키 | `PCV_SECRET_AUTH_JWT_SECRET` / `[auth] jwt_secret` 우선, 없으면 배포 helper가 영속화한 `[daemon] jwt_secret` 호환 fallback |

```bash
# 토큰 발급
TOKEN=$(curl -s -X POST http://127.0.0.1:8080/api/v1/auth/token \
  -d '{"username":"admin","password":"<configured-admin-password>"}' \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['access_token'])")

# 토큰 갱신
curl -s -X POST http://127.0.0.1:8080/api/v1/auth/refresh \
  -d "{\"refresh_token\":\"$REFRESH_TOKEN\"}"

# API 호출
curl -s -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/vms
```

#### 로그아웃 (세션 단위)

`POST /api/v1/auth/logout`은 본문 `refresh_token`이 가리키는 세션 하나만 폐기한다. 같은 계정의 다른 브라우저·탭과
직후 재로그인 세션은 살아남는다. Bearer access를 함께 보내면 그 access도 즉시 무효화한다(그 사용자의 세션일 때만 폐기).

| Bearer | 본문 `refresh_token` | 결과 |
|------|------|------|
| 유효 | 그 사용자의 활성 refresh | 200 `scope:"session"`, `session_revoked:true` |
| 유효 | 회전·폐기·만료됐거나 다른 사용자의 refresh | 200 `scope:"session"`, `session_revoked:false`(access만 무효화) |
| 유효 | 없음 | 200 `scope:"access"` — 세션은 남는다(전체 폐기는 `/auth/logout/all`) |
| 없음·무효 | 활성 refresh | 200 `scope:"session"`, `session_revoked:true` |
| 없음·무효 | 없음 또는 비활성 | 401 |
| 무관 | 문자열이 아니거나 빈 값, 깨진 JSON | 400 |

로그아웃 두 경로는 Bearer `pcv_` API 키·`X-API-Key`를 세션 판정에 쓰지 않는다(키 단독 401).

`POST /api/v1/auth/logout/all`은 그 사용자의 refresh 세션을 전부 폐기한다(유효 Bearer 또는 활성 refresh로 신원 확인,
`scope:"all"`). 다른 곳에 이미 발급된 access는 만료(최대 15분)까지 유효하다. Web UI 상단의 '모든 기기' 버튼이 이 경로를
부른다. refresh는 탭의 sessionStorage에 있으므로 '로그아웃'은 이 탭의 로그인만 끝낸다.

```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/auth/logout \
  -H "Authorization: Bearer $TOKEN" -d "{\"refresh_token\":\"$REFRESH_TOKEN\"}"
curl -s -X POST http://127.0.0.1:8080/api/v1/auth/logout/all -H "Authorization: Bearer $TOKEN"
```

Bearer만 보내던 스크립트는 이제 access만 무효화된다(`scope:"access"`). 전체 폐기가 필요하면 `/auth/logout/all`을 쓴다.

#### PBKDF2-SHA256 패스워드 해싱

기본 600,000회 반복(`[auth] pbkdf2_iterations`로 높일 수 있고, 600,000보다 작은 값은 600,000으로 올린다) + 16바이트 랜덤 salt(16진 32자). 옛 형식(이전 해시, 반복수가 낮은 PBKDF2)으로 저장된 비밀번호는 로그인에 성공할 때 현재 반복수로 다시 저장된다.

#### 로그인 실패와 잠금

`POST /auth/token`의 실패 응답은 계정이 있든 없든 같습니다. 없는 계정도 비밀번호 지문을 현재 반복수만큼 계산한 뒤 같은 문구(`401 Invalid credentials`)로 답하고, 저장 형식이 옛것인 계정의 실패도 같은 비용으로 맞춥니다. 실제 사유(`user not found`)는 감사 행과 운영 로그에만 남습니다.

| 축 | 규칙 | 응답 |
|----|------|------|
| 같은 이름 연속 실패 5회 | 다섯 번째 실패부터 잠금. 잠금 시간은 30초 → 60초 → 5분 → 1시간으로 늘고, 성공하면 처음으로 돌아갑니다. **계정이 없는 이름도 똑같이 잠깁니다** | `429`, `Retry-After` |
| 같은 IP 실패 20회 | 300초 동안 그 IP의 로그인을 비밀번호 확인 전에 거절합니다 | `429`, `Retry-After` |

- 잠금 기록은 메모리에만 있어 데몬을 다시 시작하면 사라집니다. 이름 기록과 IP 기록은 각각 4,096개까지입니다. 가득 차면 지금 잠기지 않은 기록 중 가장 오래된 것부터 지우고, 모두 잠겨 있으면 새 항목을 기록하지 않으며 `Login attempt table (name|IP) is full of locked entries` WARN을 1분에 한 번 남깁니다. 잠긴 기록은 지우지 않습니다.
- 없는 이름이 잠긴 동안 그 이름으로 계정을 만들면 남은 잠금 시간만큼 로그인이 429입니다.
- 관리자 행이 없는 부트스트랩 이름도 같은 잠금을 거칩니다. 잠겨도 복구 안내 로그(`bootstrap fallback refused`)와 감사 `auth.bootstrap.fallback`은 시도마다 남고, 응답만 401에서 429로 바뀝니다.
- 비밀번호 계산은 REST 스레드 밖의 전용 스레드 2개에서 돕니다. 로그인이 몰려도 화면 파일과 다른 API는 늦어지지 않고, 데몬이 로그인 계산에 쓰는 CPU는 약 2코어로 묶입니다. 계산 중이거나 줄 선 로그인이 12건이면 그다음 요청은 비밀번호를 확인하지 않고 바로 `429 Login service is busy`와 `Retry-After: 2`로 답합니다. 이 응답은 이름과 무관하고 잠금 카운터에 세지 않습니다. 줄이 빠지면 정상 로그인은 그대로 됩니다. 넘친 동안 `login worker pool is full` WARN이 1분에 한 번 남습니다.

#### TOTP 2FA (2단계 인증)

RFC 6238 TOTP(HMAC-SHA1, 30초 스텝, 6자리, ±1 스텝 윈도)를 로그인에 추가할 수 있다.
계정별 opt-in이 기본이며, `[auth] require_totp_roles`(콤마 구분 role 목록)로 특정
role에 등록을 강제할 수 있다 — 예: `require_totp_roles = admin`이면 admin 로그인마다
TOTP 미등록 시 강제 등록 흐름으로 진입한다. 설정값에 인식되지 않는 role 이 있으면
데몬이 기동을 거부한다(fail-start).

**2단계 로그인 흐름**

1. `POST /auth/token`에 username/password 전송.
2. TOTP가 걸린 계정이면 액세스 토큰 대신 `{totp_required: true, pending_token}`
   (미등록이면 `{totp_enroll_required: true, pending_token}`)을 받는다.
   `pending_token`은 `scope=totp` 클레임을 가진 JWT로 **TTL 180초**이며,
   `/auth/totp/verify`·`/auth/totp/enroll` 외 어떤 보호 엔드포인트도 통과하지 못한다
   (scope 클레임이 있으면 일반 `pcv_jwt_verify()` 경로는 fail-closed 거부).
3. `POST /auth/totp/verify`에 `{pending_token, code}`(또는 등록 미완료 시
   `/auth/totp/enroll`)로 6자리 코드를 제출하면, 성공 시 `/auth/token` 성공 응답과
   같은 형태로 정식 `access_token`/`refresh_token`을 받는다.

```bash
# 1. 로그인 — TOTP 등록 계정이면 pending_token 반환
resp=$(curl -s -X POST http://127.0.0.1:8080/api/v1/auth/token \
  -d '{"username":"admin","password":"<password>"}')
PENDING=$(echo "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin)['pending_token'])")

# 2. TOTP 코드로 완주
curl -s -X POST http://127.0.0.1:8080/api/v1/auth/totp/verify \
  -d "{\"pending_token\":\"$PENDING\",\"code\":\"123456\"}"
```

이미 로그인한 사용자가 스스로 TOTP를 켜는 opt-in 경로도 같은 두 엔드포인트를
공유한다 — `pending_token` 없이 `Authorization: Bearer <정식 access_token>` 헤더로
호출하면 등록/재검증 경로로 분기한다(이 경로는 새 토큰을 발급하지 않는다).

**RPC / REST 요약**

| 인터페이스 | 대상 | 설명 |
|------|------|------|
| `POST /auth/token` | 전체 | 로그인 1단계. TOTP 대상이면 `pending_token` 반환(액세스 토큰 미발급) |
| `POST /auth/totp/verify` | pending 또는 정식 세션 | 코드 검증 — 로그인 완주 또는 정식 세션 재검증(opt-in 확정) |
| `POST /auth/totp/enroll` | pending 또는 정식 세션 | 등록 시작 — `otpauth://` URI 발급(QR 렌더용) |
| `auth.totp.status` | VIEWER (self-scope) | 본인 TOTP 상태 조회. 타 사용자 지정 시 403 |
| `auth.totp.disable` | VIEWER (self-scope) | 본인 TOTP 해제(코드 재검증 필요) |
| `auth.totp.recovery.regenerate` | VIEWER (self-scope) | 복구코드 10개 재발급(기존 복구코드 전량 폐기) |
| `auth.totp.reset` | ADMIN | 대상 사용자 TOTP 강제 해제(분실 대응) |

**복구코드**: 등록 완료 시 10개(`xxxxx-xxxxx` 형식)가 **1회만** 화면에 표시되며
(모달은 버튼으로만 닫힘 — 백드롭/ESC로 실수로 잃지 않도록), 서버에는 SHA-256 해시로만
저장한다. 각 코드는 1회용 — 로그인 시 TOTP 코드 대신 복구코드를 제출하면 그 코드는
즉시 소비된다. 단말 분실 등으로 전량 소진·분실했다면 ADMIN이 `auth.totp.reset`으로
강제 해제한 뒤 재등록해야 한다.

**오류코드**

| 코드 | 이름 | 의미 |
|------|------|------|
| -32007 | TOTP_REQUIRED | TOTP 등록 계정 — pending_token으로 `/auth/totp/verify` 필요 |
| -32008 | TOTP_ENROLL_REQUIRED | 강제 role인데 미등록 — `/auth/totp/enroll`로 등록 먼저 |
| -32009 | TOTP_INVALID_CODE | 코드 불일치 |
| -32010 | TOTP_PENDING_EXPIRED | pending_token TTL(180초) 만료 — 로그인부터 재시작 |
| -32011 | TOTP_LOCKED | TOTP 전용 브루트 카운터 잠금(로그인 브루트포스 잠금과 독립) |

### 10.2 RBAC 3-Tier

| 역할 | 레벨 | 권한 |
|------|------|------|
| VIEWER | 0 | 읽기 전용 (list, status, metrics) |
| OPERATOR | 1 | VIEWER + VM/컨테이너 lifecycle. VM 단일 대상 action은 자신이 생성한 VM에 한정 |
| ADMIN | 2 | 전체 권한 (사용자 관리, 설정 변경, 호스트 자원 등록) |

#### VM owner-scope

operator의 VM 단일 대상 action은 libvirt domain metadata의 `pcv:owner`와 호출자 subject가 일치해야 허용된다. `admin`은 전역 관리 권한을 유지한다.

- `vm.create`는 생성자 subject를 `pcv:owner` metadata에 기록한다.
- `vm.start`, `vm.stop`, `vm.delete`, snapshot, ISO, VM NIC hotplug, VNC 등 단일 VM action은 operator 호출 시 owner 일치 여부를 검사한다.
- VM NIC 추가/제거(`device.nic.attach`, `device.nic.detach`)는 operator에게 열려 있지만, 자기 VM이 아니면 owner-scope에서 403으로 거부된다.
- `vm.clone`은 [ADR-0023](../docs/adr/0023-vm-clone-beta-safety-guard.md)에 따라 source VM owner-scope를 통과한 operator와 admin에게 열려 있으며, source VM `shut off` 상태와 준비된 템플릿 확인 또는 libguestfs 기반 guest reset 경로를 요구한다.
- `vm.batch`는 요청에 포함된 모든 VM owner가 호출자와 일치할 때만 실행한다.
- `vm.batch start`는 VM을 한 대씩 차례로(앞 VM의 시작이 끝난 뒤 다음) 정식 `vm.start` 경로로 시작하며 같은 이름은 `duplicate`로 거절한다. 대수만큼 시간이 더 걸린다. `vm.batch stop`은 정상 종료(최대 30초 뒤 강제)를 풀 크기에 맞춘 상한(최대 4대)씩 진행하며, 이 상한 K(`MIN(4, pool_max_conn−1)`)는 요청마다가 아니라 **프로세스 전역**이다. `vm.batch stop` 요청이 여러 개 겹쳐도 합산 K대 이하로 정지하고, 겹친 요청은 거절되지 않고 교대로 진행되어 늦게 끝난다(응답은 즉시). 또 VM별 작업 잠금은 차례가 올 때 얻는다. 그때 다른 작업 중인 VM은 응답 `rejected`가 아니라 `vm.stop` 감사 기록(fail)로 남는다(2.1.0).
- 같은 VM을 이름으로 요청하든 UUID로 요청하든 같은 작업 잠금으로 서로 배제되며, 거절 메시지는 VM 이름을 쓴다(`VM '<이름>' is already locked (op: …, pid: …)`). 잠금 거절 오류 코드는 RPC마다 이전과 같다(2.1.0).
- owner metadata가 없는 기존 VM은 operator action이 거부된다. 소유권 정리와 복구는 admin 경로에서 처리한다.

#### Bootstrap admin과 전용 admin

Single Edge는 관리자 계정을 두 층으로 운영합니다.

- `bootstrap admin`: 첫 설치에서 `daemon.conf`의 `[daemon] admin_password` 또는 `PURECVISOR_ADMIN_PASSWORD`로 **DB에 처음 생성하는** 관리자입니다. 내장 기본 비밀번호는 없습니다. 생성 이후의 인증 기준은 RBAC DB이며, 설정 파일 수정·서비스 재시작은 기존 비밀번호를 변경하지 않습니다.
- `전용 admin`: RBAC DB에 별도로 생성한 운영용 관리자 계정입니다. 일상 운영과 감사 추적은 이 계정을 기준으로 수행합니다.

권장 순서:
1. `admin_password`를 명시 설정한 뒤 bootstrap admin으로 첫 로그인
2. **시스템 > 계정과 권한** 화면에서 전용 admin을 생성하고 해당 계정의 로그인을 확인
3. bootstrap admin 본인으로 로그인해 상단 오른쪽 사용자 이름 옆 **PW** 버튼(비밀번호 변경)을 사용. API는 `POST /api/v1/auth/password`이며 Bearer 토큰과 `old_password`·`new_password`가 필요
4. 이후 일상 운영과 API 사용은 전용 admin으로 수행

비밀번호 변경은 새 해시·salt 저장과 기존 **refresh 세션 폐기**를 하나의 DB 트랜잭션으로 처리합니다. 어느 단계든 실패하면 전체 변경을 취소합니다. 이미 발급된 access 토큰은 원래 만료까지 최대 15분 유효하며, 현재 브라우저는 변경 후 다시 로그인합니다. 등록된 bootstrap admin에도 TOTP 확인·역할별 등록 의무가 동일하게 적용됩니다.

> **초기 설정값 정리(선택)**: 전용 admin의 정상 로그인을 확인한 뒤 `admin_password=`를 비우면 설정에 평문 비밀번호를 남기지 않을 수 있습니다. 이는 새 bootstrap 생성(기동 때 `admin_password`로 관리자 행을 시딩)을 중지하며, **이미 생성된 계정을 삭제하거나 비활성화하지 않습니다**. 예전에 있던 "DB에 계정이 없을 때 설정값으로 비상 토큰을 발급하는 fallback"은 없어졌습니다(아래 "사용자 삭제와 인증 수단"). 비밀번호를 분실한 기존 계정은 설정값을 다시 넣고 재시작해도 초기화되지 않습니다. DB 직접 UPDATE 대신 정상 로그인 상태에서 위 변경 API를 사용합니다.
>
> **⚠️ 파일 권한**: `daemon.conf`는 `admin_password` 등 자격증명을 담으므로 **반드시 소유자 `root:root`, 권한 `0600`**(`chown root:root` + `chmod 600`)이어야 한다(world-readable 금지). `.deb` 설치는 postinst가 자동으로 `0600`을 강제한다. 소스/수동 설치 시 직접 확인할 것.

> **사용자 삭제와 인증 수단**: 사용자를 삭제하면 그 사용자의 access 토큰·refresh 세션·대기 중 로그인(pending)·WebSocket·VNC 연결이 즉시 무효가 되고, 열려 있던 WebSocket·VNC 연결은 닫힙니다. 같은 이름으로 다시 만든 사용자는 **다른 계정**이며(사용자마다 신원 세대 값이 따로 있음) 옛 토큰·refresh 세션·TOTP 등록을 물려받지 않습니다. 인증된 요청은 매번 주체가 DB에 있는지와 신원 세대가 맞는지 확인하며, 조회 자체가 실패하면 허용하지 않고 503으로 답합니다(예전의 "없는 사용자는 VIEWER" 폴백은 없습니다). API 키는 사용자 역할이 아니라 **키에 저장된 역할**로 판정하고, 업그레이드 때 저장 역할이 사용자 역할보다 높은 키는 한 번 낮춥니다(올리지는 않으며, 낮춘 키 수는 기동 로그에 남습니다). 부트스트랩 비상 토큰 발급은 중지되었습니다: 관리자 계정이 없으면 `pcvctl auth create`(UDS)로 만들거나 데몬을 재기동해 `admin_password`로 다시 시딩합니다. 이 변경을 반영한 뒤에는 모든 사용자가 **한 번 다시 로그인**해야 합니다(옛 refresh 세션은 이관 때 폐기됨).

```bash
# 전용 admin 포함 사용자 관리
pcvctl auth list
pcvctl auth create ops-admin "${PCV_NEW_ADMIN_PASSWORD:?설정한 강한 비밀번호 필요}" admin
pcvctl auth create dev "${PCV_NEW_USER_PASSWORD:?설정한 강한 비밀번호 필요}" operator
pcvctl auth role dev admin
pcvctl auth delete dev

# RPC
echo '{"jsonrpc":"2.0","method":"auth.user.create","params":{
  "username":"dev","password":"<configured-user-password>","role":"operator"
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

사용자 이름은 자원 경계 이름 규칙 `[A-Za-z0-9][A-Za-z0-9._-]{0,62}`(첫 글자 영숫자, 이후 영숫자·`.`·`_`·`-`, 63자 이하)를 따라야 합니다. `auth.user.create`는 이 규칙을 어긴 이름을 `-32602`로 거절합니다(`auth.user.tenant.set`의 테넌트 이름 규칙과 같은 집합). 자가 등록 `auth.register`는 자체 규칙(3–32자 소문자 영숫자·`_`)을 그대로 쓰고, 부트스트랩 `admin_user`는 영향받지 않습니다. 이미 만들어진 비규칙 이름 계정은 계속 동작하지만, 규칙에 맞게 고치려면 삭제 후 재생성해야 합니다(덮어쓰기 경로는 없습니다). 재생성 계정은 새 신원 세대를 받아 옛 토큰·세션·TOTP 등록을 물려받지 않고 테넌트·소유 범위도 초기화됩니다.

`auth create`의 비밀번호, `container env-set`의 값, `alert set --webhook`의 URL은 입력을 되풀이하는 출력(`--verbose`의 요청·응답 줄, `--batch`의 되풀이·실패 줄, 오류 안내)과 REPL 이력에 남지 않습니다. 예외로 `--format=json`은 사용자가 고른 원문 출력이라 `alert set`의 응답을 그대로 찍어 URL이 보이고, `alert config`도 ADMIN에게 URL 원문을 보여 줍니다. 또 명령줄 인자이므로 실행 중에는 같은 호스트의 `ps`에, `sudo`로 실행하면 sudo 로그에, 값을 직접 치면 셸 이력에 보일 수 있습니다. 위처럼 환경변수로 넘기고 공유 호스트에서는 REST(`POST /api/v1/auth/users`)를 씁니다.

REST에서 자기 역할을 바꾸는 `auth.role.set`은 거부된다(`-32602`, HTTP 400, 감사 `denied` 1행). 자기 권한을 스스로 올리지 못하게 하기 위해서다. 다른 ADMIN 계정이나 UDS(`pcvctl auth role`)로 바꾼다.

#### 10.2.1 호스트 자원 등록과 테넌트 허용

ADMIN이 아닌 계정(OPERATOR·VIEWER)은 요청에 넣는 **호스트 자원 참조**를 자기 **테넌트**에 허용된 등록 항목으로만 쓸 수 있다. 경로·장치 주소는 지금처럼 넘기고, 데몬이 그 값이 허용 항목과 맞을 때만 받아들인다. ADMIN은 그대로다(값을 판정하지 않고 원문을 쓴다). 설정 기본 저장 위치(`[storage] image_dir`·`zvol_pool`), 기본 브리지(`[network] default_bridge`, 생략 기본 `pcvnat0`), 컨테이너 기본 브리지 `virbr0`은 등록 없이 모든 테넌트에 허용된다.

| 종류 | 판정하는 요청 값 | 맞는 기준 |
|---|---|---|
| `image` | `vm.create` `base_image`, `vm.import.ova` `ova_path` | 링크를 푼 실제 경로가 같고, 등록할 때의 그 파일(파일시스템 id·inode·생성 시각)이다 |
| `iso` | `vm.create`·`vm.mount_iso` `iso_path` | 위와 같다 |
| `storage` | `vm.create` `image_dir`·`storage_pool`, `vm.import.ova` `pool` | 등록한 디렉터리·ZFS 데이터셋 아래다(디렉터리는 등록할 때 있어야 한다) |
| `pci` | `vm.create` `pci_addr` | 정규 표기(`0000:3b:00.1`)가 같다 |
| `usb` | `vm.usb.attach` `vendor_id`/`product_id` | `vvvv:pppp`가 같다 |
| `bridge` | `vm.create` `network_bridge`, `device.nic.attach` 브리지, `container.create` `network_bridge` | 이름이 같다 |

- 계정의 테넌트는 `auth.user.create`의 `tenant`다. 테넌트가 비어 있는 계정은 `pcvctl auth tenant <user> <tenant>`로 한 번 정한다(이미 있으면 바꿀 수 없다). 테넌트 이름은 `[A-Za-z0-9][A-Za-z0-9._-]{0,62}`다.
- 테넌트 없는 OPERATOR는 위 참조를 전혀 쓸 수 없다. 비ADMIN의 `vm.create` `tenant`(tenant-overlay)는 자기 테넌트 이름일 때만 통과한다.
- gRPC 요청의 테넌트는 `[grpc] tenant`다(설정이 없으면 테넌트 없음, `[grpc] role = admin`이면 ADMIN). REST 본문에 넣은 `_pcv_caller_tenant`는 서버가 지운다.
- 비ADMIN의 `iso.list`·`sriov.list`·`gpu.list`·`vm.usb.list`·`template.list`·`template.get`은 자기 테넌트에 허용된 항목만 보인다(템플릿은 `iso_path`가 빠진다). 테넌트가 없으면 빈 목록이다.
- 등록 항목을 쓰는 VM이 있으면 해제(`revoke` — 그 테넌트 소유 VM 기준)·삭제(`unregister` — 모든 VM 기준)가 거절되고 VM 이름이 나온다. ISO를 빼거나 장치·NIC를 떼거나 VM을 지운 뒤 해제한다.
- 판정을 통과한 비ADMIN의 `base_image`·`ova_path`는 워커가 읽기 전에 등록 때의 그 파일인지 다시 대조한다.

```bash
# 등록과 허용 (ADMIN)
pcvctl resource register iso iso-ubuntu /pcvpool/iso/ubuntu-24.04.iso
pcvctl resource grant iso-ubuntu team-a
pcvctl resource register storage st-team-a /data/team-a
pcvctl resource grant st-team-a team-a
pcvctl resource list --kind iso
pcvctl resource revoke iso-ubuntu team-a      # 그 테넌트 VM이 쓰면 −32002
pcvctl resource unregister iso-ubuntu         # 어떤 VM이든 쓰면 −32002

# 테넌트 지정 (비어 있을 때만)
pcvctl auth tenant alice team-a

# 기존 VM의 미등록 참조 → 검토·편집 → 일괄 등록
pcvctl resource unregistered --tenant team-a --json > /tmp/resource-import.json
pcvctl resource import /tmp/resource-import.json
```

| 오류 | 뜻 | 조치 |
|---|---|---|
| −32006 `Permission denied: this account has no tenant — host resource references are disabled until an admin assigns one` | 계정에 테넌트가 없다 | ADMIN이 `pcvctl auth tenant <user> <tenant>` |
| −32006 `Permission denied: <파라미터> is not a resource allowed for this tenant` | 등록·허용되지 않은 값이다(없는 경로도 같은 응답) | ADMIN이 등록·허용하거나 허용된 값으로 바꾼다 |
| −32002 `resource '<name>' is in use by VM(s): …` | 해제·삭제할 항목을 VM이 쓰는 중이다 | 그 VM에서 떼거나 VM을 지운 뒤 다시 한다 |
| −32002 `tenant already set — moving a user between tenants is not supported` | 이미 테넌트가 있는 계정이다 | 테넌트 이동은 지원하지 않는다 |
| −32002 `resource name '<name>' is already registered for a different kind or reference` 등 | 같은 이름이나 같은 (종류, 참조값)의 다른 항목이 있다 | 다른 이름을 쓰거나 기존 항목을 쓴다. 같은 이름·값의 재등록은 정체성만 갱신한다(`refreshed:true`) |
| −32602·−32002·−32005 `item N: …` (`resource.import`) | N번째 항목이 잘못됐다 — 아무것도 바뀌지 않았다 | 그 항목을 고쳐 다시 import한다 |

**업그레이드 절차(호스트 자원 참조 경계, ADMIN)**:

1. 업그레이드 전 `pcvctl --format=json auth list`로 OPERATOR·VIEWER 계정과 테넌트(`tenant`)를 기록한다(표 출력에는 테넌트 열이 없다). 테넌트 없는 OPERATOR는 업그레이드 직후 이미지·ISO·OVA·저장 위치·PCI·USB·브리지 참조가 전부 −32006이다. `vm.create`의 저장 위치·브리지·PCI는 요청에 준 `storage_pool`·`image_dir`, 해석한 브리지(생략하면 기본 브리지 `pcvnat0`, `none`이면 판정 없음), 요청에 키가 있는 `pci_addr`(빈 값 포함)를 판정하고, 생략한 저장 위치(설정 기본값)는 판정하지 않는다. 그래서 참조가 없는 `vm.create`도 브리지를 생략하면 −32006이지만, `network_bridge:"none"`이면 판정할 참조가 없어 테넌트 없는 OPERATOR도 통과한다. 단, `network_bridge`를 생략한 `container.create`는 판정하지 않는다.
2. 업그레이드 직후 `pcvctl auth tenant <user> <tenant>`로 테넌트를 지정한다. 이름 규칙(`[A-Za-z0-9][A-Za-z0-9._-]{0,62}`)을 어긴 테넌트로 만든 기존 계정은 게이트에서 테넌트 없음이 되고 `auth.user.tenant.set`도 already로 거절되므로, 그런 계정은 다시 만든다.
3. `pcvctl resource unregistered --tenant <tenant> --json > /tmp/resource-import.json`으로 기존 VM의 미등록 참조를 받아 확인·편집한 뒤 `pcvctl resource import /tmp/resource-import.json`한다. `--tenant`는 import 입력의 허용 대상 표지일 뿐 필터가 아니다 — 다른 테넌트 VM의 참조도 들어 있으니 검토한 뒤에 import한다. 사라진 경로를 가리키는 VM이 있거나 같은 경로의 파일이 바뀌어 이름이 `-2`로 제안되면, 그대로 import할 때 일괄 등록 전체가 거절된다(그 항목을 빼거나 고친다).
4. ISO·이미지 라이브러리를 `pcvctl resource register`·`grant`로 등록한다. 기본 저장 위치·기본 브리지는 등록 없이 모두 허용된다.
5. gRPC를 OPERATOR로 쓰면 `[grpc] tenant`를 설정하고 재시작한다.
6. VPC를 쓰는 테넌트의 OPERATOR는 업그레이드 직후 풀이 비어 있어 새 subnet을 만들 수 없다 — `pcvctl resource unregistered --tenant <t> --json`의 `vpc_subnets`로 지금 쓰는 대역을 확인하고 `pcvctl vpc pool-set <t> <cidr,…>` 또는 `vpc_pool` 항목 import로 풀을 배정한다(6.12.2). 기존 subnet은 그대로 동작한다.

기존 VM의 기동·정지·재시작은 등록 여부와 상관없이 그대로다.

### 10.3 API Key

해시 저장, 만료일 설정, 해지(revoke) 지원.

```bash
# API Key 발급 (ADMIN, REST·RPC 전용 — pcvctl에는 apikey 명령이 없다)
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/auth/apikeys \
  -d '{"name":"ci-pipeline","role":0,"expires_at":"2027-02-09T00:00:00Z"}'

# API Key 해지 (이름으로)
curl -s -X POST -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/auth/apikeys/ci-pipeline/revoke

# API Key로 인증 (키는 pcv_ + 16진 64자)
curl -s -H "X-API-Key: pcv_<16진 64자>" \
  http://127.0.0.1:8080/api/v1/vms
```

`auth.apikey.list`의 `last_used_at`은 그 키로 인증에 성공한 마지막 시각이다. 쓰기 부하를 막기 위해 같은 키는 60초에 한 번만 갱신한다.

### 10.4 JWT Bearer와 CSRF 정책

PureCVisor REST API는 쿠키 기반 세션을 쓰지 않고 `Authorization: Bearer <JWT>` 헤더만 사용한다. 브라우저가 Authorization 헤더를 자동 첨부하지 않으므로 별도 `X-CSRF-Token`은 사용하지 않는다. 이 결정은 [ADR-0014](../docs/adr/0014-remove-csrf-jwt-bearer.md)를 따른다.

### 10.5 Rate Limiting

| 대상 | 제한 | 단위 |
|------|------|------|
| IP 기본 | 600 | 요청/분 |
| User 기본 | 1,200 | 요청/분 |
| Auth 엔드포인트 | 60 | 요청/분 |
| Metrics·Health 엔드포인트 | 3,600 | 요청/분 |
| VM 생성(`POST /vms`) | 120 | 요청/분 |

엔드포인트별 한도는 고정값이며 설정 키가 없다. 429 응답 시 `Retry-After` 헤더 포함.

### 10.6 보안 그룹

SQLite 기반 보안 그룹 CRUD + nftables 규칙 자동 적용.

```bash
# 보안 그룹 생성
pcvctl security-group create web-sg

# 규칙 추가 (CLI의 출발지는 0.0.0.0/0 — 다른 출발지는 RPC security_group.rule.add의 source)
pcvctl security-group rule add web-sg --direction ingress --proto tcp --port 80
pcvctl security-group rule add web-sg --direction ingress --proto tcp --port 443

# VM에 적용
pcvctl vm security-group web-prod web-sg
```

### 10.7 Native Host HIDS/HIPS

Security Guard는 Single Edge 호스트 노드를 보호하는 탐지 우선 HIDS/HIPS 기능이다.
기본값은 비활성이며, v1은 자동 차단보다 감사, 알림, 운영자 승인 대응을 우선한다.
Web UI에서는 `관제 > 보안 이벤트`에서 이벤트 큐, 감사 상관키, 권고 대응, 승인 대기열을 확인한다.
CLI는 `pcvctl security ...` 명령군을 제공한다.

#### 기능 범위

| 구분 | 역할 | v1 동작 |
|------|------|---------|
| HIDS | 호스트 파일 무결성과 런타임 보안 이벤트 탐지 | admin이 명시한 파일 baseline을 저장하고 변경 상태를 `trusted`, `stale`, `unknown`으로 노출한다. |
| HIPS | 탐지 이벤트에 대한 대응 후보 생성 | `pending` action을 만들고, 운영자가 승인한 경우에만 실행한다. |
| 감사 상관 | 보안 이벤트와 감사 로그 연결 | WARN/CRIT 새 이벤트는 `security.event` audit을 남기며 audit `target`은 사건 행 id(`event_id`)다. 같은 키로 합쳐지는 반복은 매번 감사하지 않고 `occurrence_count`만 올리며, 키마다 시간당 한 번 `security.event.repeat`(target `<event_id>#n=<반복 수>`)로 요약한다. |
| 감사 증가 상한 | 반복·폭주 이벤트가 감사 DB와 사건 DB를 채우지 못하게 함 | 출처별 시간당 감사 예산 600건을 새 사건과 반복 요약이 함께 쓴다(WARN 몫 500, CRIT 몫 보장). 넘은 새 사건은 감사하지 않고 창이 끝나면 `security.event.flood`(target `<source>#n=<건수>,since=<창 시작>`) 1행으로 요약한다. 접을 수 있는 열린 사건(CRIT·대기 액션 제외)이 5,000건을 넘으면 `last_seen`이 가장 오래된 500행을 `suppressed`로 접고(`closed_reason=overflow`, 감사 `security.event.overflow`), 14일 넘게 갱신 없는 열린 사건도 `suppressed`로 접는다(`closed_reason=expired`, 감사 `security.event.expire`). CRIT·대기 액션 사건은 접지 않으며, 그 수가 상한을 넘으면 `security.config.get`의 `protected_open_over_cap`·`protected_open_count`로 경고한다. |
| 알려진 도구 실행 | 데몬이 직접 실행하는 도구의 LSM 실행 사건을 일반 의심 실행과 구분 | 출처 `lsm`, 종류 `process_suspicious`인 사건의 target이 신뢰 디렉터리의 알려진 도구 경로와 정확히 같으면 `info`로 낮춘다. `info`는 감사·알림 대상이 아니다. 규칙은 아래 "알려진 도구 INFO 규칙"에 있다. |
| 운영자 종결 | 확인이 끝난 열린 사건을 운영자가 한 건씩 닫음 | `security.event.close`(ADMIN)가 열린 사건을 `resolved`로 바꾸고 사유를 사건 행에 남긴다. `closed_reason`은 `operator`다. 종결 보관은 최근에 닫힌 행을 남긴다. 자세한 규칙은 아래 "운영자 단건 종결"에 있다. |
| 운영 인터페이스 | Web UI, CLI, UDS | 같은 `security.*` RPC를 사용하므로 이벤트와 pending action 상태가 일관된다. |

HIDS baseline은 처음에는 `unknown`이다. 운영자가 신뢰할 수 있는 시점에 핵심 파일을 지정해 refresh해야 `trusted`가 된다.
HIPS는 자동 차단 제품처럼 동작하지 않는다. v1에서 승인 후 실제 실행 가능한 대응은 `block_ip`와 `revoke_api_key`뿐이고,
`lock_user`, `restart_service`, `quarantine_process`, `restore_config` 계열은 `manual_runbook`으로만 남긴다.

#### 알려진 도구 INFO 규칙

데몬은 `ip`·`virsh`·`zfs` 같은 도구를 직접 실행하고, LSM은 그 실행마다 `process_suspicious` 사건을 남긴다. 이 중 데몬이 직접 쓰는 도구는 `info`로 구분한다.

- 신뢰 디렉터리는 `/usr/sbin`·`/usr/bin`·`/sbin`·`/bin` 4곳이다. target이 `<신뢰 디렉터리>/<알려진 이름>`과 정확히 같을 때만 `info`가 된다. 접두사나 basename만 같은 경로(`/usr/sbin//ip`, `/usr/sbin/../sbin/ip`)는 해당하지 않는다.
- 정확 경로 `/usr/local/bin/purecvisorsd`(핫 리로드 재실행)와 `/usr/local/bin/pcvctl`(LXC 볼륨 시작 검사 훅 `container volume-guard-hook`)도 `info`다.
- `/usr/local/sbin`·`/usr/local/bin`의 같은 이름 도구는 `warn`으로 남는다. 데몬 PATH는 `/usr/local`이 먼저라 그쪽에 놓인 같은 이름은 가로채기일 수 있다.
- 이름 표는 데몬이 argv[0]으로 직접 실행하는 도구 이름이다(`security_policy.c`).
- 절대 낮추지 않는 이름 16개는 데몬이 쓰더라도 표에 넣지 않는다: `sh`·`bash`·`dash`·`zsh`·`busybox`·`curl`·`wget`·`nc`·`ncat`·`socat`·`ssh`·`scp`·`openssl`·`python3`·`perl`·`nsenter`. BPF 사건에는 argv가 없어 데몬의 고정 명령과 다른 주체의 같은 도구를 구분할 수 없기 때문이다.
- 이미 열린 `warn` 행은 다음 발생 때 같은 키로 합쳐지면서 `info`로 바뀐다. 새 행은 생기지 않는다.

#### 다른 파일시스템 열기

LSM은 열린 경로 문자열만 보므로, 운영 서비스가 아닌 곳(격리 시험 데몬의 샌드박스 등)이 감시 경로와 같은 이름의 파일을 열어도 사건이 만들어졌습니다. 이제 열린 파일이 tmpfs에 있으면 `file_open` 사건에 그 파일시스템의 superblock 장치를 함께 담고, 데몬이 `/proc/self/mountinfo`로 감시 경로가 놓인 장치 집합을 유지합니다. tmpfs 파일의 장치가 그 집합에 없으면(감시 경로와 무관한 tmpfs에 있는 같은 이름 파일) 보안 사건으로 올리지 않고 카운터로만 셉니다. tmpfs는 자기 내용만 담아 호스트 실물을 드러낼 수 없기 때문입니다.

- tmpfs가 아닌 파일시스템(overlay 포함)은 장치를 담지 않고 **그대로 사건**이 됩니다. overlay처럼 아래층의 실물을 드러낼 수 있는 파일시스템을 다른 파일로 치지 않기 위해서입니다.
- 이 구분에는 2.1.0 이후 릴리스(2.2.0 포함)의 선택형 BPF tarball에 든 객체가 필요합니다. 사건 레코드 크기(304바이트)와 경로 위치는 그대로이므로 2.0.4 객체도 그대로 적재되며, 그 객체는 장치 필드를 0으로 채워 모든 열기가 이전처럼 사건으로 제출됩니다.

- 카운터는 지표 `pcv_bpf_lsm_foreign_fs_total`로 노출됩니다. 첫 건에만 INFO 로그가 한 번 남습니다.
- 감시 경로 아래에 새 마운트가 생기거나 사라지면(`mountinfo` 변경 통지) 집합을 다시 만듭니다. 장치를 알 수 없는 열기와 `bprm_check_security` 사건은 지금까지처럼 사건으로 제출됩니다.

#### 운영자 단건 종결

확인이 끝난 열린 사건은 `security.event.close`로 닫는다.

- 권한은 ADMIN이다. REST 앞단도 ADMIN이다.
- 입력은 `event_id`와 `reason`이다. 사유는 필수이고 제어 문자(C0·DEL·C1·양방향 제어)를 공백으로 바꾸고 앞뒤 공백을 지운 뒤 1~200자여야 한다. 200자를 넘으면 자르지 않고 거절한다.
- 열린(`open`) 사건만 닫는다. 없는 사건, 이미 닫힌 사건(`resolved`·`suppressed`), 살아 있는 대기 액션이 있는 사건은 거절한다.
- 성공하면 `status=resolved`, `closed_reason=operator`가 되고 응답은 바뀐 사건 객체다. 같은 키가 다시 오면 새 열린 행이 생긴다.
- 감사는 dispatcher 자동 감사 1행(op `security.event.close`, target `event_id`)이다. 행위자는 인증된 사용자이고 요청 본문의 이름은 쓰지 않는다. 사유는 감사가 아니라 사건 행에 남는다.
- 사건 JSON에 필드 3개가 더해졌다: `closed_by`(종결자), `closed_at`(종결 시각), `close_note`(사유). 열린 사건과 접기·만료로 닫힌 사건은 빈 값(`closed_at`은 0)이다. `closed_reason`에는 `operator`가 더해졌다.
- 종결 보관은 `MAX(timestamp, closed_at)` 순으로 최근에 닫힌 행을 남긴다. 방금 닫은 오래된 사건이 곧바로 지워지지 않는다.
- approve/dismiss는 사건이 `open`·`action_pending`일 때만 사건 상태를 바꾼다. 이미 닫힌 사건의 상태는 바꾸지 않는다.
- REST 역할이 dispatcher와 맞춰졌다. 이제 VIEWER는 `security.action.pending`·`security.baseline.status`·`capacity.forecast`·`daemon.update_check`·`vm.billing.report`·`vm.guest.fsinfo`·`vm.sla.report`를, OPERATOR는 `security.action.dismiss`를 REST로도 쓸 수 있다(8개 메서드). REST가 dispatcher보다 엄격한 메서드는 없다.

```bash
pcvctl security close <event_id> --reason "reviewed: known maintenance job"
pcvctl audit search --method security.event.close --limit 10
```

Web UI에서는 열린 사건의 상세 패널에서 ADMIN에게만 '종결' 버튼이 보인다. 사유를 입력하면 닫히고, 닫힌 사건의 상세 패널에는 종결 구분·종결자·종결 시각·종결 사유가 보인다.

#### 운영 흐름

1. `pcvctl security status`로 Security Guard 활성 여부, baseline 상태, open risk, pending action 수를 확인한다.
2. 신규 서버나 배포 직후에는 핵심 설정 파일을 명시해 HIDS baseline을 refresh한다.
3. Security Guard를 활성화한 뒤 Web UI, CLI 중 하나로 보안 이벤트 큐를 확인한다.
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

Web UI에서는 `관제 > 보안 이벤트`에서 Security Guard 토글, 이벤트 상세, evidence, 감사 상관키, 승인/거부/종결 버튼을 사용한다.

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

# 열린 사건 종결 (ADMIN, 사유 필수)
pcvctl security close <event_id> --reason "operator closed"

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

# baseline refresh (감사 주체는 서버가 확인한 호출자다. UDS 직결은 system으로 남는다)
echo '{"jsonrpc":"2.0","method":"security.baseline.refresh","params":{"paths":["/etc/purecvisor/daemon.conf","/var/lib/purecvisor/rbac.db"]},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool

# Security Guard 활성화
echo '{"jsonrpc":"2.0","method":"security.config.set","params":{"enabled":true},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock | python3 -m json.tool
```

CLI 표면 회귀는 다음 스크립트로 확인한다.

```bash
bash tests/integration/test_security_cli_surface.sh
```

#### 운영 주의

- baseline이 `unknown`인 상태에서 guard만 켜면 파일 무결성 판단 기준이 없다. 신규 설치, 설정 변경, RBAC DB 교체 후에는 baseline을 먼저 갱신한다.
- HIPS action은 `pending` 상태에서만 승인한다. 이미 닫힌 이벤트를 다시 실행 대상으로 쓰지 않는다.
- `security.event.close`는 대기 액션이 있는 사건을 닫지 않는다. 먼저 승인하거나 거부한다.
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
| 시크릿 관리 | `PCV_SECRET_*` 환경변수 / `ENC:base64` daemon.conf 암호화 |

### 10.9 노드 펜싱(공개판 범위 밖)

STONITH·IPMI 펜싱은 여러 노드 클러스터용 기능이라 Single Edge 공개판에는 없습니다.

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
# 감사 로그 조회 (ADMIN)
pcvctl audit search --limit 50

# REST
curl -s -H "Authorization: Bearer $TOKEN" \
  "http://127.0.0.1:8080/api/v1/audit/search?limit=50"
```

REST 검색 조건은 쿼리로 줍니다. 허용 키는 `from_ts`, `to_ts`, `username`, `method`, `limit`, `offset`입니다. `from_ts`·`to_ts`는 감사 시각과 같은 UTC `YYYY-MM-DD HH:MM:SS` 문자열이고, `method`는 SQL LIKE 패턴입니다. `limit`은 1 이상, `offset`은 0 이상 정수입니다. 모르는 키·중복 키·형식이 틀린 값(시각의 숫자 자리에 공백·부호가 든 값 포함)은 필터 없는 결과 대신 400입니다. 응답의 `total`은 전체 일치 수가 아니라 이번에 읽은 행 수(최대 `offset+limit+1`)입니다. 다음 페이지가 있는지 알려고 한 행을 더 읽고, 그 결과가 `has_more`입니다. `items`는 `limit`만큼만 돌려줍니다.

```bash
curl -s -G -H "Authorization: Bearer $TOKEN" \
  --data-urlencode 'username=bob' --data-urlencode 'method=vm.delete' \
  --data-urlencode 'from_ts=2026-11-01 00:00:00' --data-urlencode 'limit=50' \
  http://127.0.0.1:8080/api/v1/audit/search
```

`audit.search`는 ADMIN 전용이라 Web UI 감사 로그 메뉴도 ADMIN에게만 보입니다. 화면의 날짜는 브라우저 시간대의 하루이며 UTC로 바꿔 보내고, 결과 시각은 UTC로 표시합니다. 권한 거부 같은 오류는 "검색 결과 없음"이 아니라 오류로 표시합니다.

감사 `username`은 REST 인증 주체, gRPC는 `grpc`, 서버에서 직접 연 pcvctl(UDS)은 `local-admin`, 요청 없이 데몬이 한 일은 `system`이다. `-`는 행위자를 알 수 없는 기록이다.

### 10.11 /proc hidepid 하드닝 (2.0)

2.0 고유 호스트 하드닝으로, `/proc`를 `hidepid=2` + `gid=pcvmon`으로 마운트해 프로세스 argv·환경의 무분별한 노출을 차단한다. 이는 iSCSI CHAP 비밀번호가 world-readable `/proc/*/cmdline`으로 새던 노출(target·initiator 양쪽)을 소거하기 위한 조치이며, 1.0 라인에는 없는 2.0 전용 태세다. 메트릭 수집·프로세스 모니터 등 `/proc`를 읽어야 하는 구성요소는 `pcvmon` 그룹 멤버십으로 접근을 유지한다.

> **AppArmor 데몬-confinement 미배포 결정([ADR-0028](../docs/adr/0028-2.0-host-hardening-hidepid-over-apparmor-confinement.md))**: 이 환경(hidepid=2)에서 데몬을 AppArmor 프로필로 confine 하면 libvirt 연결이 파손된다 — `hidepid=2`가 타 프로세스 `/proc/<pid>` 접근을 커널 `ptrace_may_access()`로 게이트하고, 그 경로가 AppArmor ptrace 매개를 발동해 confined libvirtd가 데몬의 `/proc/stat`을 identity로 읽지 못하기 때문이다. 2.0은 실증 소거가 확인된 **hidepid를 유지**하고, **AppArmor 데몬-self-confinement은 배포·enforce 하지 않는다**. AppArmor 프로필 파일 자체는 1.0 라인·참조용으로 패키지에 존치되며, VM(libvirt per-domain)·컨테이너(lxc per-container) 런타임 격리는 그대로 유지된다. 즉 2.0 호스트 통제는 capabilities 축소 + hidepid + 게스트/컨테이너 런타임 격리로 구성된다.

### 10.12 Suricata DPI/IDS/IPS (2.0, D13)

2.0은 Suricata 엔진을 연동해 L7 심층 패킷 검사(DPI) 기반 침입 탐지(IDS)와 인라인 침입 방지(IPS) 제어면을 추가했습니다.
Suricata IDS 엔진은 `purecvisorsd`와 별도 systemd 유닛(`suricata.service`, `Restart=no`)으로 기동하며, 재기동 권위는 데몬의 보안 헬스 감시가 갖습니다.

> **ADMIN 전용 제어면**: `suricata.*` 10종은 `/api/v1/suricata/*` 전용 REST와 Web UI **관제 → 보안 이벤트**의 `Suricata Runtime`에서 사용합니다.
> CLI 서브커맨드는 없으며, 정책·상태도 전 테넌트 관찰 표면이므로 조회와 변경 모두 ADMIN 권한이 필요합니다.

#### IDS (탐지)

Suricata가 `/var/log/suricata/eve.json`에 남기는 alert 라인(`event_type=="alert"`만 대상)을 실시간으로 tail 해 PureCVisor 표준 보안 이벤트(`security_event`)로 정규화합니다.
폭주 방지를 위해 coalesce(동종 이벤트 병합)와 rate-limit이 걸립니다.
정규화된 이벤트는 §10.7 Native Host HIDS/HIPS와 같은 `security.event.*` 큐로 흘러 Web UI `관제 > 보안 이벤트`에서 함께 확인됩니다.

| 메서드 | 용도 |
|--------|------|
| `suricata.status` | Suricata 엔진/유닛 상태 |
| `suricata.rules.update` | 룰 시그니처 갱신(원자 교체·롤백) |
| `suricata.policy.get` / `suricata.policy.set` | 탐지 정책 조회/설정 |

#### IPS (인라인)

인라인 방지 경로는 nftables `inet purecvisor ips` 체인(forward hook, priority -10)과 `suricata-ips` nfqueue 유닛으로 구성됩니다.
forward 트래픽을 Suricata로 보내는 규칙 하나가 fail-open/fail-closed 축의 **유일한 표현**입니다 — `queue flags bypass to N`이면 fail-open(큐 리스너가 없을 때 커널이 그대로 ACCEPT), `queue num N`(bypass 없음)이면 fail-closed(리스너가 없으면 드롭)입니다.
어느 쪽을 걸지는 `daemon.conf`의 `[ips] fail_open`이 정하며, 기본값은 `true`(fail-open)입니다.
base 체인의 accept는 비종결이라 기존 forward 필터·격리 규칙과 간섭하지 않습니다.

IPS 엔진은 IDS 원본이 아니라 데몬이 만드는 **파생 룰셋**(`/var/lib/suricata/suricata-ips.rules`)을 읽습니다.
파생본은 원본을 복사하면서 `suricata.ips.drop.*`로 지정된 SID의 선두 `alert`만 `drop`으로 치환한 것이고, 교체 전에 `suricata -T`로 검증합니다(실패 시 기존 파생본 무변경 — fail-safe).
**차단 대상 SID의 기본값은 빈 집합**이므로 초기 파생본은 원본과 동일하며, 실제 차단은 운영자가 `drop.add`로 SID를 넣은 뒤부터 시작됩니다.

fail-closed에서 엔진이 죽으면 헬스 tick이 규칙을 먼저 걷어 경로를 열고, 유닛 재기동 → 큐 바인딩 확인(readiness) → 규칙 복원 순으로 자동 복구합니다.
재기동이 연속 3회 실패하면 **fail-open으로 강등**하고(bypass 규칙 재삽입) CRIT 알림을 발화합니다.
이때 `status`의 `fail_open`은 설정값 그대로 `false`인 채 `degraded`만 `true`가 되므로, 두 필드의 불일치가 "설정은 fail-closed인데 실효 fail-open"을 나타내는 신호입니다.

파생 룰셋 구조의 설계 전제는 **IDS와 룰셋 원본을 공유한다**는 점입니다.
ET Open 원본(`/var/lib/suricata/rules/suricata.rules`)은 파생 과정에서 **읽기만 하고 절대 수정하지 않으며**, 새로 쓰는 것은 IPS 전용 파생본뿐입니다.
그래서 IPS 쪽 조작 실수가 IDS 탐지 범위를 훼손할 수 없고, 원본 갱신(`suricata.rules.update`)과 파생본 재생성이 서로를 덮어쓰지 않습니다.
원본이 파생본보다 나중에 바뀐 상태(staleness)는 별도로 감지해 부팅 시 WARN으로 드러냅니다.

fail-closed는 **opt-in**입니다 — `[ips] fail_open` 기본값이 `true`라 아무것도 설정하지 않으면 엔진이 죽어도 트래픽은 통과합니다.
`false`로 바꾸는 것은 "검사할 수 없으면 끊는다"는 운영 결정이며, 위의 readiness 게이팅(큐 바인딩을 확인한 뒤에만 규칙 복원)과 3연속 실패 시 fail-open 자동 강등이 그 결정에 딸린 안전망입니다.

> **혼동 주의 — 축이 둘입니다**: 엔진 yaml(`suricata-ips.yaml`)의 `nfq.fail-open`은 위의 fail-open/fail-closed 축이 **아닙니다**.
> 그 값은 큐가 포화됐을 때 커널이 미판정 패킷을 어떻게 할지를 정하며, 현재 `no`(검사 우회 대신 안전 실패)입니다.
> 엔진 사망 축의 단일 표현은 어디까지나 nft 규칙의 `bypass` 토큰이고, 두 값은 서로 간섭 없이 공존합니다(`nfq.fail-open: no` + `queue flags bypass to 0`).

| 메서드 | 용도 |
|--------|------|
| `suricata.ips.enable` | nfqueue 인라인 경로 활성화(`suricata-ips` 유닛 기동 + 큐 규칙 add) |
| `suricata.ips.disable` | 인라인 경로 비활성화(체인 flush + 유닛 stop) |
| `suricata.ips.status` | IPS(인라인) 상태(`{engine, enabled, queue_num, fail_open, mode, degraded, last_toggle}`) |
| `suricata.ips.drop.list` | 차단 대상 SID 목록(오름차순) |
| `suricata.ips.drop.add` | 차단 대상 SID 추가 — 비동기 단일-비행, 즉시 `{"status":"started"}` 반환 후 파생 룰셋 재생성 |
| `suricata.ips.drop.remove` | 차단 대상 SID 제거 — `add`와 동일한 비동기 계약 |

`drop.add`/`drop.remove`는 파생 룰셋 재생성 + `suricata -T` 검증 + 원자 교체 + 엔진 reload를 수행하므로 응답이 즉시 오더라도 **반영까지는 시간이 걸립니다**(최악 약 65초).
완료 여부는 `status`의 `last_toggle` 또는 `drop.list` 재조회로 확인하세요.
세 메서드 모두 role 2(ADMIN) 이상입니다.

> **SID 선정 주의**: 잘못 고른 SID 하나가 정상 트래픽을 끊습니다.
> 차단 후보는 발화 이력·`noalert`·`flowbits` 의존·confidence 기준으로 걸러 고르고, 투입 후에는 IPS eve-log의 `drop` 레코드로 무엇이 실제로 끊겼는지 사후 확인합니다.

#### 활용 예제 — IDS 상태 확인 후 선택 SID만 IPS 차단

먼저 IDS engine과 현재 IPS 모드를 읽고, 운영자가 검토한 SID만 drop 목록에 추가합니다.
IPS 활성화는 host forward 경로에 영향을 주므로 유지보수 시간에 수행하고, 응답 직후가 아니라 status와 drop 목록이 수렴한 뒤 성공으로 판정합니다.

<figure class="pcv-network-example-diagram pcv-technical-wide" aria-labelledby="pcv-network-example-suricata-caption">
  <a class="pcv-network-example-canvas" href="/assets/diagrams/network-examples/suricata.svg" target="_blank" rel="noopener" aria-label="Suricata IDS IPS 활용 예제 구성도를 새 탭에서 확대해서 보기">
    <img src="/assets/diagrams/network-examples/suricata.svg" width="960" height="280" loading="lazy" decoding="async" alt="VM forward packet이 nftables NFQUEUE와 Suricata IDS IPS engine을 거쳐 allow 또는 drop 결과와 PureCVisor 보안 이벤트로 이어지는 구성도">
  </a>
  <figcaption id="pcv-network-example-suricata-caption">queue readiness, 파생 drop 룰셋과 status가 수렴한 뒤 인라인 적용을 완료로 판단합니다.</figcaption>
</figure>

```bash
# 엔진 상태 조회
curl -s -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/suricata/status

# 현재 inline 모드와 fail-open 설정 확인
curl -s -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/suricata/ips/status

# IPS 인라인 경로 활성화 (queue_num/fail_open은 daemon.conf 값 사용)
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -X POST http://127.0.0.1:8080/api/v1/suricata/ips/enable

# 차단 대상 SID 추가 (비동기 — 즉시 {"status":"started"} 반환)
curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -X POST http://127.0.0.1:8080/api/v1/suricata/ips/drop \
  -d '{"sids":[2034647,2034648]}'

# 반영 확인 (파생 룰셋 재생성 완료까지 최악 ~65초)
curl -s -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8080/api/v1/suricata/ips/drop
```

> **헬스 감시**: `suricata.service`·`suricata-ips` 유닛은 데몬의 보안 헬스 tick이 같은 주기로 probe 하며, FAILED 감지 시 bounded `systemctl restart`(30초 상한) + 감사 로그로 자동 복구합니다.


---

## 11. 비동기 작업 관리

VM·OVA·백업 등 장시간 작업은 접수와 실제 결과를 구분합니다.
AWS 수동 Import·Export·Near-Live와 전용 작업 관리, S3 백업은 2026-10-08 2.0.0 소스 갱신에서
제거했습니다. 일반 VM·OVA·로컬 백업은 유지합니다.
2.1.0부터 Job 결과를 별도 journal에 먼저 내구화한 뒤 DB를 갱신합니다(11.1). 2.0.4 이하 공개 패키지에는 이 journal이 없습니다.

### 11.1 접수·결과 저장

VM 생성·OVA·백업·Local VPC·DPDK 등 통합 Job 큐를 쓰는 요청은 SQLite에 접수 row를
저장한 뒤에만 Job ID·accepted를 반환합니다.
DB 비활성·쓰기 잠금/오류에는 신규 접수를 거부하며 실제 worker를 시작하지 않습니다.
작업 ID 충돌은 기존 작업을 덮지 않고
최대 32회 새 번호로 재발급합니다.

실제 작업 결과와 결과 기록의 저장 여부는 별개입니다. 2.1.0부터는 별도 결과 journal을 내구화한 뒤 DB를 갱신합니다. 완료 WS의 `result_persisted: false`,
`recovery_pending: true`는 실제 결과는 기록됐지만 조회 반영 재시도가 남았다는 뜻입니다.
WS가 끊겨도 journal이 남았다면 재시도/재시작 뒤 `jobs.get`으로 결과를 확인할 수 있습니다.
같은 작업을 바로 재실행하지 말고 Job ID, 실제 리소스 상태와 audit를 대조합니다.

결과 파일이 없거나 손상된 이전 실행은 `interrupted`, `outcome_known: false`로
표시하며 자동 재실행하지 않습니다. 취소 요청도 worker의 실제 중단을 보장하지 않습니다.
시작 복구 중 DB가 잠기면 최대 32회 재시도하며 정리가 완료되기 전에는 신규 접수를 거부합니다.
DB/journal 양쪽 저장 실패나 효과 직후 기록 전 crash는 자동 복구 보장 밖입니다.
이 구현은 일반 `pcv_jobs.db`의 schema version 2(`PRAGMA user_version=2`)를 쓰며 데몬 시작 때 자동 이관합니다.
2.0.4 이하 binary로 되돌릴 때의 주의는 [DATABASE_STRUCTURE.md](../docs/DATABASE_STRUCTURE.md)의 Job 상태 DB 절을 따릅니다.
[ADR-0060](../docs/adr/0060-job-admission-and-result-persistence.md)의 접수 계약은 그대로 유지됩니다.

### 11.2 상태 확인과 기존 이력

일반 작업은 `jobs.list`·`jobs.get`·`jobs.status`로 확인하고 해당 작업의 지원 범위에서
`jobs.cancel`을 사용합니다. 완료 알림과 실제 리소스 상태·감사 로그를 함께 확인합니다.
기존 AWS 작업 DB·감사 기록·VM·디스크·외부 자원은 삭제하지 않습니다.
기능 제거로 이미 접수된 외부 작업이 취소되거나 자원이 정리되지는 않습니다.
제거된 API/CLI는 미지원이며 Cloud 화면의 이전 북마크는 도움말로 이동합니다.

종료된 Job 기록은 보존 기간이 지나면 하루 한 번 자동으로 정리됩니다. 완료·실패·취소로 결과가 확정된(`outcome_known`) Job만 대상이며, 진행 중·대기·결과 미확정 Job은 남깁니다. 기본 보존 기간은 30일이고 `[daemon]` 설정의 `job_retention_days`로 조정합니다 — 0 이하(0·음수)는 정리를 끄고(보존 기간을 잘못 0으로 두어 전량 삭제되는 것을 막는 안전값), 양수 N은 N일을 뜻합니다. 보존 기간을 넘긴 Job은 기록에서 지워지므로 `jobs.get`·`jobs.status`가 `not_found`를 돌려줍니다. 장기 보관이 필요하면 완료 WebSocket·감사 로그로 결과를 따로 남기거나 보존 기간을 늘립니다.


## 12. AI & 자가치유

### 12.1 4-프로바이더 합의

PureCVisor AI Agent는 복수의 AI 프로바이더에 동시 질의하여 합의(consensus) 기반으로 의사결정한다.

| 프로바이더 | 용도 | 설정 |
|-----------|------|------|
| Claude (Anthropic) | 범용 분석 | API Key + 모델 고정 |
| OpenAI | 범용 분석 | API Key + 모델 고정 |
| Gemini (Google) | 범용 분석 | API Key |
| Ollama | 로컬 추론 | 엔드포인트 URL |

#### 가중 쿼럼

프로바이더별 가중치를 부여하여, 가중 합산이 60% 이상이면 합의 성립. 가중치는 데몬에 고정된 값(Claude·OpenAI 1.0, Gemini 0.9, Ollama 0.7)이라 설정으로 바꾸지 않습니다.

```bash
# AI Agent 설정
pcvctl agent config

# 프로바이더 설정 (키를 넣으면 켜집니다)
pcvctl agent set --provider claude --api_key "$ANTHROPIC_API_KEY"
pcvctl agent set --provider openai --api_key "$OPENAI_API_KEY" --model gpt-4o-2024-08-06
pcvctl agent set --provider gemini --api_key "$GEMINI_API_KEY"

# Ollama는 키를 쓰지 않지만, 활성 여부를 키 유무로 정하므로 아무 값이나 넣습니다
pcvctl agent set --provider ollama --endpoint "http://localhost:11434" --api_key local

# 프로바이더 끄기 (빈 키)
pcvctl agent set --provider openai --api_key ""

# 합의 이력
pcvctl agent history
```

`agent set`은 `--provider` 외에 `--api_key`·`--model`·`--endpoint` 중 하나 이상을 받습니다. `--enabled`와 `--weight`, 모르는 프로바이더 이름, `...`이 든 키와 `***`(가려진 값)는 RPC를 보내지 않고 사용법 오류(exit 2)로 끝납니다. 이 값은 실행 중인 데몬에만 적용되므로, 재시작 뒤에도 유지하려면 `daemon.conf`의 `[ai]`에 적습니다. 키는 출력(`--batch`의 되풀이·실패 줄 포함)과 REPL 이력(`~/.pcvctl_history`)에 남지 않습니다. 명령줄 인자는 같은 호스트의 `ps`에 보일 수 있으니 위처럼 환경변수로 넘기고, 셸 이력에 키를 직접 남기지 않습니다. RPC `agent.config.set`의 `providers`는 `[{name,api_key,model,endpoint}]` 배열이며, 배열이 아니면 −32602입니다.

Web UI AI Agent 화면에서 공급자는 API 키가 있으면 켜지고, 키 칸을 비우고 저장하면 꺼집니다. 켜고 끄는 체크박스는 서버에 닿지 않아 없앴습니다. 호출 간격(300초)과 요청 시간 제한(10초)은 데몬 고정값이라 설정 탭에서 읽기만 합니다.

#### per-policy dry-run

정책별로 dry-run 모드를 설정하여 실제 실행 없이 의사결정 결과만 확인.

### 12.2 Z-Score 이상탐지

`anomaly_detector.c`에서 Z-Score 기반 이상 탐지를 수행한다.

```
Z = (현재값 - 평균) / 표준편차
|Z| > 3.0 이면 이상(anomaly) 판정
```

이름이 `_total`로 끝나는 누적 카운터(`node_disk_io_time_seconds_total`, `node_network_receive_errs_total`,
`node_pressure_*_total` 등)와 `/proc/vmstat` 누적값인 `node_vmstat_*`(`swap-storm`이 보는 `node_vmstat_pswpout` 포함)는 원값이 아니라 주기 증가량으로 평가한다. 첫 표본과 합이 줄어든 주기(카운터 리셋, VM 삭제 뒤
장치 번호 재사용)는 건너뛴다. 누적 지표는 평균보다 큰 증가량만 이상으로 보므로 부하가 끝나 증가량이 줄어들 때는 경보가 나지 않는다. 게이지 지표는 양방향이다. Z-Score는 평상시와 달라지는 전환을 잡는다. 부하가 일정하게 계속되면 증가량도 일정해 Z가
0에 가까워지므로 **지속 포화 자체로는 `disk-saturated`가 발동하지 않는다.** 포화가 계속되는지는 지표 값을 직접 확인한다.

기준선 리셋: 배포/유지보수 후 기준선을 수동 리셋하여 오탐을 방지.

```bash
# RPC — 기준선 리셋 (ADMIN)
echo '{"jsonrpc":"2.0","method":"anomaly.reset_baseline","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

### 12.3 자가치유 5계층 안전 스택

```text
Layer 5: 승인 큐 — 승인이 필요한 액션은 관리자 승인 대기
Layer 4: Rate Limit — 5분 내 최대 3개 자동 액션 (alert_only 제외)
Layer 3: 쿨다운 — VM은 정책·대상별, 호스트 알림은 정책별 최소 간격
Layer 2: 합의 검증 — 4-프로바이더 가중 쿼럼 60%+
Layer 1: 정책 매칭 — 사전 정의된 치유 정책
```

**self-healing CPU 알림**은 호스트 CPU 이상 또는 예측 부하에 대한 `cpu-overload`
정책의 `alert_only` 기록입니다. CPU를 강제로 제한하거나 VM을 재시작하는 지시가 아닙니다.
기본 `[ai].mode`는 `dry_run`이므로 판단을 로그·감사·치유 이력에 기록합니다.
`active`에서도 CPU·메모리 정책의 액션은 `alert_only`입니다.

| 정책 | 이상 이벤트 입력의 Z-score 하한 | 5분 뒤 사용률 예측 조건 | 액션 | 쿨다운 |
|---|---|---|---|---|
| `cpu-overload` | 3.0 | CPU 예측값 > 85% | `alert_only` | 600초 |
| `mem-pressure` | 2.5 | 메모리 예측값 > 90% | `alert_only` | 600초 |

이상 이벤트와 예측은 별도 진입 경로입니다. 85%·90%는 현재 사용률의 고정 알림 임계값이
아니라 예측 경로의 조건이며, 이상 이벤트는 수집·학습과 탐지 조건을 먼저 통과해야 합니다.

2026-09-15 공개 수정부터 호스트 정책은 실제 첫 발동 여부를 따로 저장합니다. 조건에 맞는
첫 이벤트는 부팅 후 600초 이내에도 기록하며, 이후 같은 정책의 반복은 600초 동안 억제합니다.
CPU와 메모리는 서로의 쿨다운을 소비하지 않습니다. 이 수정이 이상탐지의 초기 학습 시간을
없애지는 않습니다. VM 대상 정책의 쿨다운은 기존처럼 정책·VM 조합별로 유지합니다.

실제 VM 재시작은 대상이 지정된 `vm-unresponsive`의 별도 `restart` 정책과 실행 모드·안전
가드를 따릅니다. CPU 알림을 자동 vCPU 증설, 메모리 balloon 조정이나 스냅샷 정리 완료로
해석하지 않습니다.

### 12.4 승인 큐

위험도가 높은 자가치유 액션은 자동 실행하지 않고 승인 큐에 등록한다. 승인 대기 칸은 8개이고 시간 만료는 없다. 칸이 차면 가장 오래된 미처리 건이 새 건에 밀려난다.

```bash
# 승인 대기 목록
echo '{"jsonrpc":"2.0","method":"healing.pending","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock

# 승인
echo '{"jsonrpc":"2.0","method":"ai.healing.approve","params":{
  "action_id":1
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock

# 거부
echo '{"jsonrpc":"2.0","method":"ai.healing.reject","params":{
  "action_id":1
},"id":"1"}' | nc -U /var/run/purecvisor/daemon.sock
```

Web UI에서도 approve/reject 가능.

승인 예제의 `action_id`는 `healing.pending`에서 읽은 정수로 바꿉니다. 승인/거부 응답의 `true`만으로 실행 성공·실제 대상 변경을 단정하지 않고 목록·이력·actual state를 재조회합니다.

현재 모드와 대기 건수는 읽기 전용 RPC `healing.status`(VIEWER)로 봅니다. 응답은 `{"mode":"active"|"dry_run","pending":정수}`입니다.

```bash
echo '{"jsonrpc":"2.0","method":"healing.status","params":{},"id":"1"}' \
  | nc -U /var/run/purecvisor/daemon.sock
```

Web UI의 운영 개요, 자가치유 화면, 모바일은 이 값을 그대로 표시합니다. `ACTIVE`이면 자동 조치가 실제로 실행되고, 조회에 실패하면 `DRY RUN`으로 짐작하지 않고 "알 수 없음"으로 표시합니다. 운영 개요의 대기 목록은 `healing.pending`을 읽습니다.

### 12.5 per-VM 압력(PSI) 연결 (2.0.0)

per-VM PSI 연결 전의 이상탐지는 `/proc/pressure/*`의 **노드 전역 PSI**만 봤다. 그래서 VM 한 대가 메모리에
굶어도 호스트 평균이 낮으면 놓쳤고(미탐), 무관한 부하가 평균을 밀어 올리면 멀쩡한 VM이 범인이
됐다(오탐). `2.0.0`부터는 cgroup 단위로 PSI를 수집해 이 사각을 없앤다.

- **수집 범위**: `machine.slice`(qemu/libvirt-lxc/nspawn)와 `lxc` 두 레이아웃을 깊이 1 고정으로 스캔해 비용을 상수화한다. 항목 상한은 64이며, 초과분은 조용히 자르지 않고 truncated 게이지 + WARN(300초 스로틀)으로 드러낸다.
- **라벨 방어**: 게스트 이름은 사용자 입력이다. systemd 이스케이프를 디코드한 뒤 제어문자·DEL·`"`·`\`가 섞여 있으면 그 항목을 **거부한다**(잘라 쓰지 않는다 — 잘린 이름은 다른 게스트와 충돌한다).
- **정리**: 사라진 게스트의 라벨은 기존 TTL 5분 sweep이 걷어간다.
- **노드 전역 수집은 그대로**다. 기존 대시보드 의존은 변하지 않는다.

여기서 붙는 `vm_name` 라벨을 이상탐지가 읽어 치유 액션에 `target_vm`으로 넘긴다. 그 결과
자가치유가 "호스트 어딘가"가 아니라 **대상 VM을 지목해** 판단할 수 있게 됐다.

- **자동 watch 등록/회수**: 매 주기 mark → discover → sweep. 새 시계열이 보이면 등록하고, **3주기 연속** 보이지 않을 때만 회수한다(1주기 결손으로 지우면 5분치 학습분이 날아가고 워밍업으로 되돌아간다). 렌더 결과를 한 번도 못 읽은 주기는 동기화를 아예 건너뛴다 — "못 봤다"를 "사라졌다"로 읽으면 레지스트리 일시 장애 한 번에 전 VM 감시가 회수되기 때문이다.
- **슬롯**: 자동 64 + 고정 예약 16 = 80. 고정 감시 10건이 자동 등록에 밀려나지 않음이 컴파일 시점 불변식(`static_assert`)으로 고정돼 있다.
- **정책**: `vm-mem-pressure`가 추가됐고 **`alert_only`뿐**이다. restart·migrate처럼 게스트에 영향을 주는 액션은 넣지 않았다 — 자동 조치로의 승격은 운영 결정이며 별도 선행 조건이 있다.

> 알림 라벨은 WS 브로드캐스트와 최근 이벤트 JSON에 실릴 때 이스케이프된다. 라벨 값에 원시 `"`가
> 들어가 수신측 `JSON.parse`가 실패하던 알림 유실은 같은 `2.0.0` 시정 묶음에서 해소됐다.


---

## 13. Web UI

### 13.1 개요

아래 loopback URL은 제품 노드 안에서의 로컬 점검 예시입니다. 원격 브라우저는 설치 시 설정한 관리 주소의 `https://<관리-주소>/ui/`와 `wss://`를 사용합니다.

| 항목 | 값 |
|------|-----|
| URL | `http://127.0.0.1:8080/ui/` |
| 이벤트 센터 | `http://127.0.0.1:8080/ui#/ops-triage` |
| bootstrap admin | `admin / configured password` |
| 전용 admin | 설치 직후 RBAC `admin` 역할 사용자 추가 권장 |
| 앱 셸 | 고정 사이드바 트리(236px) + topbar(브레드크럼·통합 검색·세션) + 글로벌 statusbar |
| JS 모듈 | `app.js` + `app.bundle.js` + `modules/*.js` 30개 + `i18n.js` + `sw.js` |
| 프레임워크 | Vanilla JS (단일 번들 + 임베디드 UI) |

### 13.2 페이지 구조

로그인 화면(`#login-page`)은 좌측에 무인증 `GET /health` 기반 실데이터 노드 상태 패널(상태
dot·NODE/VERSION/UPTIME·KVM/DISK 점검 2종), 우측에 인증 카드를 배치한 2열 구성이다. 좌측
패널은 부트 1회 + 30초 간격으로 갱신되고(`_loginHealthTick`), 화면이 960px 이하로 좁아지면
1열로 접힌다. 활성 테마(13.3)를 로그인 화면부터 그대로 따른다 — 로그인 전용 배색은 없다.

운영 콘솔은 고정 사이드바 트리(왼쪽 236px), topbar(브레드크럼·통합 검색·세션 상태), 메인 콘텐츠, 글로벌 statusbar(6세그먼트)로 구성된다. 사이드바는 항상 펼쳐진 트리이며(구셸의 3탭 전환식 사이드바 폐지), `Ctrl+B`로 접고 펼 수 있다.

```
┌──────┬───────────────────────────────────────────────────────┐
│      │ topbar: 브레드크럼 · 통합 검색(Ctrl+K) · 세션 상태        │
│ 사이드│───────────────────────────────────────────────────────┤
│ 바 트리│ 메인 콘텐츠                                            │
│(236px)│ 대시보드 · VM · 컨테이너 · 인프라 · 관제 · 시스템 · 도움말 │
│      │───────────────────────────────────────────────────────┤
│      │ statusbar: VM · 컨테이너 · Critical · 보안 · 스토리지 · 자가치유 │
└──────┴───────────────────────────────────────────────────────┘
```

사이드바 섹션별 내용:

| 섹션 | 항목 |
|------|------|
| (무제) | 운영 대시보드 |
| 워크로드 | 가상 머신(요약·콘솔·스냅샷·성능·타임라인), 컨테이너, 템플릿 |
| 인프라 | 네트워크, Local VPC, OVN SDN, 오버레이 네트워크, 보안 그룹, 스토리지, 백업, iSCSI 타깃, DPDK, SR-IOV, 커넥션 풀, 호스트 장치, GPU 장치, 토폴로지, 호스트 상태 |
| 관제 | 운영 개요, 이벤트 센터, 알림, 보안 이벤트, 감사 로그(ADMIN), 활동 로그, VM/스토리지/호스트 모니터, 히트맵, API 성능, 자가치유 |
| 시스템 | 계정과 권한(ADMIN), API 관리(ADMIN), 설정 관리 |
| 도움말 | 도움말, Swagger API |

**운영 대시보드**는 호스트 상태·워크로드·최근 경고를 한 화면에 모은 진입 화면이다. 위에서
아래로 네 블록으로 고정되며, 어떤 블록도 사용자가 숨기거나 순서를 바꾸지 않는다 — 구버전의
표시 항목 on/off 위젯 토글(localStorage 영속)·빠른 작업 그리드·화면 안 상태바·Chart.js 2차트는
전역 statusbar와 아래 표로 흡수돼 폐지됐다.

| 블록 | 내용 |
|------|------|
| 게이지 4타일 | 호스트 CPU · 메모리 · 최악 ZFS 풀 · 호스트 디스크. 각 타일은 현재값·선택 창의 peak·warn/crit 눈금을 함께 표시한다 |
| 워크로드 통합 표 | VM과 LXC 컨테이너를 한 표에서 본다. 상태(실행·중지·오류)·유형(VM·LXC) 2축 필터 칩, 상태 dot/pill, 인라인 CPU/MEM 게이지, 선택 창 CPU 스파크라인, 행 hover 시 나타나는 퀵액션(시작·중지·콘솔) — 행을 클릭하면 해당 VM 요약 또는 컨테이너 화면으로 이동한다 |
| 알림 · 보안 이벤트 피드 | 알림과 Suricata 보안 이벤트를 최신순으로 합쳐 상위 8건. 행을 클릭하면 알림 또는 보안 이벤트 화면으로 이동한다 |
| 자가치유 승인 대기 | 승인 대기 중인 자가치유 액션 상위 3건을 정책·사유·경과 시간과 함께 표시하고, 카드에서 바로 승인·거부한다(ADMIN 전용) |

우측 상단 **타임세그**(`Live` · `15m` · `1h`)가 게이지의 peak 계산 창과 워크로드 표 스파크라인의
표시 구간을 함께 바꾼다. 창 전환·필터 칩 변경·폴링 갱신은 해당 블록 내부만 다시 그리므로
화면 전체가 스켈레톤으로 되돌아가거나 스크롤이 초기화되지 않는다. 필터 칩 상태는 URL 쿼리
(`?status=running&type=vm`)에 반영돼 링크 공유·새로고침 후에도 복원된다.

#### 호스트 장치 화면

`인프라 > 호스트 장치`(라우트 `host-devices`, `GPU 장치` 바로 위, `Ctrl+K` 팔레트에도 있다)는 호스트의 GPU·USB·블루투스 장치를 장치 기준 표 한 장으로 보이고, 장치마다 어느 VM에 할당됐는지와 그 상태를 함께 보인다. 데이터는 읽기 RPC `host.device.list`(VIEWER, REST `GET /api/v1/host/devices`) 한 번이고, 화면을 열 때·새로고침·동작 뒤에만 읽는다(자동 갱신 없음). 이 RPC는 `libvirt 읽기` 줄에서 돌고 외부 명령 없이 sysfs와 `pci.ids`·`usb.ids`만 읽는다.

| 요소 | 내용 |
|------|-----|
| 머리줄 | 필터 칩 전체·할당됨·미할당(URL `?hdassign=`에 남는다), 허브·저장 장치 숨김 안내와 '보기'(다시 열면 숨김), 새로고침 |
| 표 | 묶음 GPU → USB → 블루투스, 열 장치·ID·할당·상태·동작. GPU 보조 줄은 함께 옮겨 가는 IOMMU 그룹 동반 function, USB 보조 줄은 종류(키보드·마우스·오디오·저장 장치·블루투스·허브). 할당이 없으면 점선 '호스트' 배지, 볼 수 없는 VM의 할당은 '다른 VM' |
| 상태 | 실행 쪽 말(실행 중 · 호스트에서 빠짐 · 옛 장치 번호 · 자리만 있음 · 같은 ID 장치 여럿)과 영구 쪽 말(영구 · 지금만(VM을 끄면 빠짐) · 영구만 · 영구(VM 꺼짐))을 " · "로 잇는다. '실행 중'은 live 항목이 가리키는 장치가 지금 호스트에 있을 때만 나온다. 경고 말은 노란 글이다 |
| 동작 | 할당이 없으면 '할당…', 있으면 '해제'(할당이 둘 이상이면 '해제…'에서 VM을 고른다). 실행 중 VM의 영구 정의에만 있고 지금 붙어 있지 않은 USB는 '지금 붙이기'가 함께 있다. 쓸 수 없는 동작은 이유를 글로 쓴다 |
| 할당 창 | VM 선택, '영구로 저장 — VM을 다시 켜도 붙어 있음'(끄면 `vm.usb.attach` `persistent: false` — 꺼진 VM·GPU는 켠 채 잠김), 호스트가 키보드·마우스·저장 장치로 쓰는 장치는 확인 체크 전 '할당' 잠김, 블루투스는 경고만 |

- 권한: VIEWER는 보기만 한다. USB 할당·해제는 OPERATOR 이상(`vm.usb.attach`·`detach` — OPERATOR는 자기 VM만), GPU 할당·해제는 ADMIN(`device.gpu.attach`·`detach`)이다. ADMIN이 아니면 자기 테넌트에 허용된 장치 행만 보이고 VM 이름은 자기 VM 것만 보인다.
- 호스트에서 빠짐·옛 장치 번호·자리만 있음의 복구는 '해제' 뒤 다시 '할당…' 두 단계다. 장치를 다시 꽂아도 자동으로 붙지 않는다. 호스트에 없는 PCI 정의(`managed='no'`)는 '해제 불가'이고 정의는 운영자가 정리한다.
- 목록을 읽지 못하면 '장치 없음'이 아니라 오류 문구와 다시 시도가 보인다. VM 정의 하나라도 읽지 못하면 표 일부를 보내지 않고 −32000이다. 장치가 512개를 넘으면 앞부분만 보이고 안내 줄이 전체를 보는 명령을 알려 준다.
- 화면의 할당·해제는 기존 RPC를 그대로 보내므로 권한·감사·VM 잠금·레인이 CLI와 같다(USB 변경은 `VM 장치 변경` 줄).
- GPU 행의 할당 창은 실행 중 VM을 고를 수 없게 두고, 실행 중 VM이 쓰는 GPU의 '해제'는 'VM을 끈 뒤 가능'으로 잠근다. 호스트 화면을 쓰는 GPU는 '할당 불가'이고, 그룹 전체가 `vfio-pci`가 아니거나 IOMMU 그룹에 다른 장치가 섞이면 이유 글과 함께 잠긴다. 'GPU 장치' 화면의 '호스트 장치에서 할당' 버튼은 요청 없이 이 화면으로 옮긴다 — 할당 동작은 이 화면 한 곳에만 있다. CLI는 정지 VM에 `pcvctl gpu attach <vm> <pci>`·`gpu detach`를 쓴다.

CLI는 같은 응답을 표로 보인다.

```bash
# 장치 표(허브 등 숨김 행 제외) — 할당마다 한 줄, 할당이 없으면 ASSIGNED host
pcvctl host devices

# 숨김 행까지, 응답 JSON 원문
pcvctl --format=json host devices --all
```

표 머리는 `GROUP ID NAME ASSIGNED STATE`이고 볼 수 없는 VM은 `(other VM)`이다. STATE는 영어 토큰을 `;`로 잇는다 — `live`, `persistent` 또는 `live-only`, `placeholder`·`stale-address`·`ambiguous`·`attach-now`(USB), `not-on-host`·`host-display`·`driver=<이름>`(GPU), `vm-off`·`partial`·`managed=yes`·`blocked=<이유>` 순서이고 낱말이 없으면 `-`다. 예를 들어 VM이 잡고 있던 장치를 뽑아 호스트에 없는 USB는 `live;persistent;stale-address;not-on-host`다. 표 형식은 GPU 동반 function을 `  + <pci_addr> <이름>` 줄로, 잘림·sources 안내를 표 아래 줄로 보인다. plain은 탭 구분, CSV는 RFC 4180 인용이다. 인자는 `--all`만 받고 그 밖은 사용법(종료 2)이다.

### 13.3 테마 시스템

4-테마 시스템 — 2026-04-11 Supanova Taste Layer 도입 후 Supanova 변형만
남기고, 2026-04-26 접근성 후속으로 고대비 변형을 추가했다. 2026-05-08에는
대시보드 선택지를 단순화하기 위해 emerald와 light 변형을 제거했으며, 데스크톱
리뉴얼에서 승인된 `supanova-mockup` 운영 콘솔 팔레트를 네 번째 변형으로 고정했다.
[ADR-0016](../docs/adr/0016-supanova-theme-reduction.md)에 따라 localStorage에 영속 저장하고, 새로고침 없이 즉시 전환한다.

| 테마 id | accent | 설명 |
|---|---|---|
| `supanova` (기본) | Teal-500 `#14b8a6` | 차분한 청록, 운영 콘솔 중성 톤 |
| `supanova-cyan` | Cyan-600 `#0891b2` | 기존 brand cyan 연속감, 채도 경계 |
| `supanova-hicontrast` | Yellow `#facc15` | 고대비 접근성 변형 |
| `supanova-mockup` | Cyan `#00e0ff` | 데스크톱 리뉴얼 목업 팔레트(`--bg #0a0e14`) |

테마 id allowlist는 `index.html`의 프리부트 스크립트에도 같이 박혀 있어, 선택한 테마는
새로고침 직후 첫 페인트부터(로그인 화면 포함) 그대로 적용된다. 테마 편집기로 만든
커스텀 테마가 활성인 동안 선택기에는 `CUSTOM` 항목이 표시되지만 **선택은 불가**하다 —
고를 수 있게 두면 커스텀 값이 기본 테마로 강등되기 때문이다.

공통 스택: self-hosted Pretendard + local Outfit fallback, Double-Bezel 카드, spring motion
(`cubic-bezier(0.16,1,0.3,1)`), `@supports not (color-mix)` 폴백.
Supanova 금칙(neon glow, clip-path, cyan→magenta 그라디언트) 전량 제거.
레거시/삭제 테마 id(`pure-light`, `midnight-blue`, `supanova-emerald`,
`supanova-light` 등)는 inline head script가
자동으로 `supanova`로 마이그레이션.

테마 선택기는 상단 바가 아니라 **상단 우측 톱니(⚙) 버튼 → 설정(Preferences) 모달** 안에 있다
(2026-07-28 셸 정리로 이동 — 상단 바는 크럼·검색·상태 표시만 남겼다). 같은 모달에서 언어도
바꾼다.

### 13.4 DESIGN.md 시각 규격

UI 시각 규격은 상위 제품/운영 가이드인 `docs/GUIDE.md`와 분리해
루트 [`DESIGN.md`](../DESIGN.md)에서 관리한다. `DESIGN.md`는 색상 token,
typography, component state, dashboard density, table/card/button/modal 규칙을
정의하고, [`ui/samples/design-system-preview.html`](../ui/samples/design-system-preview.html)은
같은 `ui/style.css` 위에서 그 규칙을 확인하는 preview HTML이다.

UI visual 변경, `ui/samples/` 변경, `ui/docs.html`/`ui/guide.html`/`ui/guide-content.md`의 시각
규격 연결을 바꾸면 다음 검증을 Level 1에 포함한다.

```bash
python3 scripts/check_design_md.py
bash tests/integration/test_design_md_surface.sh
PCV_NO_DEPLOY=1 scripts/bundle-ui.sh
node --check ui/app.bundle.js
git diff --check
```

`DESIGN.md`의 Reference Pattern Borrowing 규칙은 외부 제품의 브랜드를 복제하지 않고 운영 콘솔에 필요한 패턴만 차용하는 기준이다. 현재 샘플은 [`ui/samples/design-borrowing-mockup.html`](../ui/samples/design-borrowing-mockup.html)이며, 실제 적용 화면은 `관제 > 이벤트 센터`의 `ops-triage` 라우트다.

### 13.5 i18n 국제화

`i18n.js`에서 ko/en 2개 언어, 280+키를 관리한다.

| 항목 | 값 |
|------|-----|
| 지원 언어 | 한국어 (ko), English (en) |
| 키 수 | 280+ |
| HTML data-i18n | 6개 요소 |
| 전환 함수 | `t(key)` / `applyI18n()` |
| 즉시 전환 | 새로고침 불필요 |
| 전환 위치 | 상단 우측 톱니(⚙) → 설정(Preferences) 모달의 Language |

### 13.6 Service Worker 오프라인 캐싱

`sw.js`는 정적 파일을 Network-First 전략으로 갱신하고 실패 시 캐시로 폴백한다. API/WebSocket 요청에는 개입하지 않으며, 네트워크 단절 시에도 마지막으로 캐시된 UI 접근을 지원한다.

### 13.7 WebSocket 실시간 이벤트

- `ws://127.0.0.1:8080/api/v1/ws/events` — 메트릭 push (10초)
- `ws://127.0.0.1:8080/api/v1/ws/vnc` — noVNC WebSocket 프록시

유휴 타임아웃: 300초 미활동 시 자동 종료. 최대 동시 연결: 1,000.

### 13.8 커맨드 팔레트 (통합 검색)

`Ctrl+K`로 커맨드 팔레트를 열어 빠른 네비게이션 및 액션 실행. 팔레트가 곧 **통합 검색**이다 —
질의를 입력하면 결과가 **명령 · 가상 머신 · 컨테이너 · 페이지** 그룹으로 나뉘어 나오고,
컨테이너는 이름뿐 아니라 **IP로도** 찾을 수 있다. VM 항목을 고르면 해당 VM을 선택한 채
VM 화면으로, 컨테이너 항목을 고르면 그 컨테이너를 선택한 채 컨테이너 화면으로 이동한다.
빈 질의일 때는 예전처럼 명령 목록만 보여준다.

별도의 전역 검색 오버레이는 없다. `Ctrl+Shift+F`, `/`, topbar의 검색 상자는 모두 같은
팔레트를 연다(이미 열려 있으면 새 창을 겹치지 않고 입력란으로 포커스만 되돌린다).

기타 키보드 단축키(`?` 오버레이가 보여주는 목록과 동일):
- `Ctrl+N` — 새 VM · `Ctrl+D` — VM 설정 · `Ctrl+P` — 환경설정
- `Ctrl+B` — 사이드바 접기/펼치기 · `F11` — 전체 화면
- `?` — 키보드 도움말 · `Esc` — 대화상자 닫기

수식키 없는 단일 키도 등록돼 있다(입력 필드에 포커스가 있으면 무시된다). `/` 통합 검색 · `n` 새 VM · `g` 대시보드 · `m` 운영 개요.
VM 목록 화면에서는 `j`/`k`(또는 ↑/↓)로 행을 옮기고 `Enter`로 요약 화면에 들어간다.

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
    ├── host-devices.js # 호스트 장치(GPU·USB) 표와 할당·해제 창
    ├── security.js     # 보안 이벤트 UI
    ├── help.js         # 도움말, Swagger API
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

호환 엔드포인트 `purecvisor-compat.example.com`를 별도 server block으로 운영하면 `connect-src`의 host도 해당 도메인으로 맞춘다. 정적 파일 배포 검증은 두 도메인의 `docs.html`, `app.bundle.js`, `guide-content.md`, `sw.js` 해시가 같은지 확인한 뒤 완료 처리한다.

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

### 13.13 VM 메모리·I/O 화면 시정과 확인 범위

2026-09-07 시정은 다음 계약을 다룹니다. 목록의 순서와 현재 열린 창이 바뀌어도 작업 대상과 결과 표시가 일치하도록 보완했습니다.

- I/O 조회·적용은 창을 열 때 정한 VM 이름/UUID를 확인합니다. 목록 재정렬, 대상 삭제와 동명 VM 교체를 구분합니다.
- 메모리 통계·I/O의 늦은 응답은 요청한 창과 최신 요청에만 표시합니다. 이미 닫힌 창의 응답이 새 창을 바꾸지 않습니다.
- RPC 오류는 정상 빈 결과와 구분합니다. 자동 닫기 timer도 원래 창에 속합니다.
- 검색 팔레트가 겹쳐 열려 있어도 아래에서 열린 원래 창은 완료 상태를 받습니다.

이 시정의 시험 결과를 공개 소스의 시험 집계에 합산하거나 모든 실제 VM 변경 효과의 인증으로 사용하지 않습니다. 공개 소스의 시정 포함 여부는 해당 릴리스 diff와 회귀 결과로 별도 확인해야 합니다.

공개 스냅샷의 모니터링 차트에도 누적 RX/TX를 속도 단위로 표시하는 경로와 표본 간격을 5초로 고정하는 경로가 남아 있습니다. 해당 차트의 시간·속도 값만으로 운영 임계값을 판단하지 말고 원본 counter의 증가량과 실제 수집 간격을 함께 확인하세요. 관련 시정·독립 재검토는 진행 중입니다.

---

## 14. REST API

JWT Bearer 인증과 RBAC를 사용해 Single Edge의 VM, 컨테이너, 스토리지, 네트워크를 자동화한다. 먼저 인증 계약을 확인한 뒤 필요한 예제를 복사해 시작한다.

### 14.1 개요와 기본 계약

| 항목 | 값 |
|------|-----|
| Base URL | `https://HOST/api/v1` |
| 포트 | HTTPS `443`(기본). 평문 HTTP `rest_port`는 기본 `127.0.0.1`에서만 열림(`[server] bind_plaintext=all`일 때만 외부) |
| REST 엔드포인트 | 225 (도움말 표시 기준) |
| JSON-RPC 메서드 | 318 (`POST /api/v1/rpc`, 도움말 표시 317) |
| WebSocket | `/api/v1/ws/events` |
| 인증 | `Authorization: Bearer {JWT}` · JWT HS256 |
| CSRF | 쿠키 세션 미사용. `X-CSRF-Token` 헤더 없음 |
| Content-Type | `application/json` |
| 압축 | gzip (`Accept-Encoding: gzip`) |
| CORS | 화이트리스트 기반 |
| 성공 응답 | `{"data": ...}` |
| 오류 응답 | `{"error": {code, message}}` |
| 캐시 | ETag + `max-age=5` (GET) / `no-store` (POST) |
| 페이지네이션 | `X-Total-Count` + `Link rel="next/prev"` |
| Correlation ID | 요청/응답 UUID (`X-Correlation-Id`) |

REST와 JSON-RPC passthrough, WebSocket은 같은 인증 경계를 사용한다. `HOST`는 현재 Single Edge 노드 주소로 바꿔 사용한다.

### 14.2 인증과 토큰

PureCVisor REST API는 쿠키 세션 대신 `Authorization` header의 Bearer token을 사용한다.

#### 14.2.1 로그인

설정된 관리자 계정으로 access token을 발급한다.

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"<configured-admin-password>"}' \
  https://HOST/api/v1/auth/token
```

#### 14.2.2 토큰 사용

모든 보호된 요청에 Bearer token을 전달한다.

```bash
curl -H "Authorization: Bearer eyJ..." \
  https://HOST/api/v1/vms
```

#### 14.2.3 쓰기 작업

POST 작업도 같은 Authorization header와 역할 검사를 사용한다.

```bash
curl -X POST \
  -H "Authorization: Bearer eyJ..." \
  https://HOST/api/v1/vms/web-prod/start
```

access token 만료 후에는 `POST /api/v1/auth/refresh`에 `{"refresh_token":"eyJ..."}`를 전송해 갱신한다.

### 14.3 RBAC 역할

| 역할 | 레벨 | 권한 |
|------|------|------|
| `VIEWER` | 0 | 읽기 전용(GET). 자신의 브라우저 Push 구독 등록·해지·조회 허용 |
| `OPERATOR` | 1 | VM/컨테이너 운영. VM action은 생성자 범위 |
| `ADMIN` | 2 | 전체 관리자 |

> 내장 기본 비밀번호가 없다. 첫 로그인 전 `daemon.conf` 또는 `PURECVISOR_ADMIN_PASSWORD`로 bootstrap 비밀번호를 설정한다.
>
> `OPERATOR`는 libvirt domain metadata의 owner가 본인인 VM에만 시작, 중지, 삭제, 스냅샷, VNC, 일괄 작업을 수행할 수 있다.

### 14.4 보안

| 항목 | 계약 |
|------|------|
| 속도 제한 | 600 IP / 1200 유저 / 60 인증 |
| JWT 알고리즘 | HS256 |
| JWT 만료 | 900s (15min) + refresh 7d |
| CORS | 화이트리스트 |
| ETag | GET 응답 조건부 캐싱(304) |
| JWT IP 바인딩 | 선택적 클라이언트 IP 검증 |
| 테넌트 오버레이 암호화 | per-VM WireGuard netns (fail-closed) |
| hidepid | `/proc hidepid=2` 하드닝(ADR-0028) |
| Suricata | IDS/IPS 심층 패킷 검사 |

#### 14.4.1 엔드포인트별 Rate Limit

| 엔드포인트 | 제한 |
|-----------|------|
| `POST /auth/token` | 60/분 |
| `GET /metrics`, `GET /health` | 3,600/분 |
| `GET /vms` | 600/분 |
| `POST /vms` | 120/분 |
| 기타 | 600/분(IP 기본) |

#### 14.4.2 메서드별 타임아웃

| 유형 | 타임아웃 |
|------|---------|
| 기본(읽기·쓰기 공통) | 8초 |
| `vm.create` | 30초 |
| `vm.inspect` | 35초 bridge 대기·30초 협력적 수집 deadline |
| `vm.guest.exec` | 20초 |
| `debug.trace.report` | 40초 |
| `zfs.*`, `backup.restore` | 60초 |
| `container.exec` | 65초 |
| `webhook.dlq.retry` | 70초 |
| `container.snapshot.rollback` | 75초 |

### 14.5 엔드포인트와 응답

| 영역 | 경로 | 용도·응답 |
|------|------|-----------|
| 인증 | `POST /auth/token`, `POST /auth/refresh` | access/refresh token 발급과 갱신 |
| VM | `GET/POST /vms`, `/vms/{name}/start`, `/stop`, `DELETE /vms/{name}` | 목록·생성·수명주기. 삭제 상태는 `GET /vms/{name}/delete-status` |
| 컨테이너 | `GET/POST /containers`, `/containers/{name}/exec` | 목록·생성·명령 실행 |
| 모니터링 | `GET /monitor/metrics`, `/monitor/fleet`, `/processes`, `/alerts` | VM 메트릭·플릿 통계·호스트 프로세스·알림 히스토리 |
| Prometheus | `GET /metrics` | Prometheus 메트릭. 기본(`[metrics] auth=loopback`)은 같은 호스트의 직접 접속만 무인증, 원격은 Bearer 토큰 필요(`required`·`none`으로 조정) |
| Health | `GET /health` | 인증 없는 심층 프로브. `status`와 `libvirt`, `etcd`, `disk`, `audit_db`, `tls`, `capabilities` 하위 상태 반환 |
| 호스트 장치 | `GET /host/devices` | GPU·USB·블루투스 장치와 VM 할당(`host.device.list`, VIEWER). USB 할당·해제는 `POST`·`DELETE /vms/{name}/usb`(`startup_policy`·`persistent`) |

Single Edge 공개판은 클러스터 상태, 클러스터 전체 VM, 라이브 마이그레이션 REST 절차를 운영 표면으로 제공하지 않는다. 관련 역사 기록은 `docs/adr/`에서 확인한다.

### 14.6 curl 예제

`HOST`와 `TOKEN`을 현재 Single Edge 환경의 값으로 바꿔 실행한다.

#### 14.6.1 VM 목록

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
  https://HOST/api/v1/vms | jq
```

#### 14.6.2 VM 생성

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"web","vcpu":2,"memory_mb":2048,"disk_size_gb":20}' \
  https://HOST/api/v1/vms
```

#### 14.6.3 스냅샷 생성

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -d '{"snap_name":"backup-1"}' \
  https://HOST/api/v1/vms/web/snapshot/create
```

#### 14.6.4 컨테이너 실행

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -d '{"command":"hostname -I"}' \
  https://HOST/api/v1/containers/app-ctr/exec
```

#### 14.6.5 비동기 작업 조회

```bash
curl -H "Authorization: Bearer $TOKEN" https://HOST/api/v1/jobs
```

#### 14.6.6 WebSocket 이벤트

```bash
wscat -c "wss://HOST/api/v1/ws/events"
# 연결 뒤 첫 메시지로 {"type":"auth","token":"<JWT>"}를 보내 인증한다
```

### 14.7 브라우저 푸시

> 구독은 3단계다. `GET /push/vapid`로 서버 공개키를 받고, 브라우저가 만든 `PushSubscription.toJSON()`을 `POST /push/subscribe`로 올리고, `POST /push/unsubscribe`로 해지한다. 내 구독은 `GET /push/mine`으로 확인한다.
>
> 소유자는 호출자 신원으로 고정된다. `VIEWER`도 자기 구독은 등록·해지·조회할 수 있지만 전체 명단, VAPID 회전, 테스트 발송은 `ADMIN` 전용이다. 발송은 `daemon.conf` `[webpush]`의 `enabled`, `min_severity(warn|crit)`, `contact`가 결정한다.
>
> iOS Safari는 이 화면을 홈 화면에 PWA로 설치한 경우에만 푸시를 받는다.

#### 14.7.1 VAPID 공개키 조회

요청:

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
  https://HOST/api/v1/push/vapid
```

응답:

```json
{"data":{"key":"BN1nZq...(base64url)"}}
```

#### 14.7.2 구독 등록

요청:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"endpoint":"https://fcm.googleapis.com/fcm/send/cXY..",
       "keys":{"p256dh":"BN1n..","auth":"k9Xz.."}}' \
  https://HOST/api/v1/push/subscribe
```

응답:

```json
{"data":{"status":"subscribed"}}
```

#### 14.7.3 구독 해지

요청:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -d '{"endpoint":"https://fcm.googleapis.com/fcm/send/cXY.."}' \
  https://HOST/api/v1/push/unsubscribe
```

응답:

```json
{"data":{"status":"unsubscribed"}}
```

#### 14.7.4 구독 명단 (ADMIN)

요청:

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
  https://HOST/api/v1/push/subscriptions
```

응답:

```json
{"data":{"subscriptions":[
  {"username":"admin","endpoint":"https://fcm.googleapis.com/…",
   "created_at":1785000000,"last_ok_at":1785003600,"fail_count":0}]}}
```

#### 14.7.5 VAPID 회전 (ADMIN — 전 구독 폐기)

요청:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  https://HOST/api/v1/push/vapid/rotate
```

응답:

```json
{"data":{"revoked":3,"key":"BJ7pQw...(new)"}}
```

#### 14.7.6 테스트 발송 (ADMIN)

요청:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -d '{"username":"admin"}' \
  https://HOST/api/v1/push/test
```

응답:

```json
{"data":{"status":"queued"}}
```

### 14.8 2.0 RPC 예제 (RPC 전용)

> 테넌트 오버레이, QoS, Suricata, 트레이스 네임스페이스는 RPC 전용이다. 전용 REST 경로, CLI, UI 페이지가 없으며 `POST /api/v1/rpc` JSON-RPC passthrough로만 호출한다.

#### 14.8.1 테넌트 암호화 오버레이 생성

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"tenant_overlay.create","params":{"tenant":"acme"},"id":"1"}' \
  https://HOST/api/v1/rpc
```

#### 14.8.2 테넌트 QoS 설정

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"qos.tenant.set","params":{"tenant":"acme","min_mbps":100,"max_mbps":1000},"id":"1"}' \
  https://HOST/api/v1/rpc
```

#### 14.8.3 Suricata IPS 활성화

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"suricata.ips.enable","params":{},"id":"1"}' \
  https://HOST/api/v1/rpc
```

#### 14.8.4 트래픽 트레이스 시작

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"debug.trace.start","params":{"timebox_sec":30,"tenant":"acme"},"id":"1"}' \
  https://HOST/api/v1/rpc
```

---

## 15. CLI 레퍼런스

### 15.1 개요

| 항목 | 값 |
|------|-----|
| 바이너리 | `bin/pcvctl` |
| 커맨드 수 | 207 (`routes[]` 기준) |
| 모드 | readline REPL + 배치 |
| 자동완성 | 대화형 셸(`pcvctl -i`)의 Tab 완성. 공개 소스에는 bash/zsh 자동완성 스크립트가 없음 |
| 소켓 | `/var/run/purecvisor/daemon.sock` |

```bash
# 단계적 도움말
pcvctl help             # 33개 명령 그룹 요약
pcvctl help vpc         # VPC 그룹 상세
pcvctl vpc --help       # 같은 VPC 그룹 상세
pcvctl help guest-agent # object·action·설명 검색
pcvctl help all         # 전체 207개 명령

# REPL 모드
pcvctl
pcvctl> vm list
pcvctl> exit

# 배치 모드
pcvctl vm list
pcvctl vm create web-prod --vcpu 4 --memory_mb 8192 --disk_size_gb 50 \
  --qos_min_mbps 0 --qos_max_mbps 1000
```

기본 도움말은 전체 명령을 한 번에 펼치지 않고 그룹별 개수와 용도를 보여 준다. 그룹 상세는
복사 가능한 `pcvctl <object> <action>` 형태이며 긴 설명은 같은 설명 열에서 자동 줄바꿈된다.
기존 전체 목록과 부분 검색은 각각 `help all`, `help <검색어>`로 유지한다.

### 15.2 커맨드 분류

#### VM 관리 (30+)

| 커맨드 | 설명 |
|--------|------|
| `vm list` | VM 목록 |
| `vm inspect <canonical-lowercase-uuid>` | ADMIN 구성 관측·JSON 출력(2.1.0) |
| `vm create <name> --vcpu N --memory_mb N --disk_size_gb N` | VM 생성 |
| `vm rename <old_name> <new_name>` | 정지된 VM 이름과 표준 디스크/NVRAM 경로 변경 |
| `vm start <name>` | VM 시작 |
| `vm stop <name>` | VM 중지 (graceful) |
| `vm delete <name>` | VM 삭제 |
| `vm delete-status <name>` | 최근 삭제 Job 상태(경고·오류 포함) |
| `vm clone <source> <clone_name> --mode cow\|full --guest-reset\|--template-prepared` | VM 복제 |
| `vm vnc <name>` | VNC 포트 조회 |
| `snapshot create <name> <snap>` | 스냅샷 생성 |
| `snapshot list <name>` | 스냅샷 목록 |
| `snapshot rollback <name> <snap>` | 스냅샷 롤백 |
| `snapshot delete <name> <snap>` | 스냅샷 삭제 |
| `vm snapshot-delete-all <name>` | 스냅샷 일괄 삭제 |
| `vm disk-resize <name> <target> <new_size_gb>` | 디스크 크기 조정 |
| `vm limit <name> [--cpu <percent>] [--mem <mb>]` | 실행 중 VM의 CPU·메모리 상한. `-1`은 상한 해제 |
| `vm disk-throttle <name> [--device vda] --read-iops N --write-iops N` | 디스크 IOPS 제한 |
| `vm blkio-set <name> [--read_bps N] [--write_bps N] [--read_iops N] [--write_iops N]` | 블록 I/O 제한 |
| `vm bandwidth <name> --inbound-kbps N --outbound-kbps N` | VM 네트워크 대역폭 제한 |
| `vm pin-vcpu <name> --vcpu N --cpuset 0-3` | vCPU 피닝 |
| `vm guest-agent-status <name>` | Guest Agent channel/package 상태 진단 |
| `vm guest-agent-ensure-channel <name>` | Guest Agent libvirt channel 보정 |
| `vm guest-ping <name>` | Guest Agent 연결 확인 |
| `vm guest-exec <name> <command>` | Guest Agent 명령 실행 (Linux `/bin/sh -c`, Windows PowerShell — 15초 예산) |
| `vm guest-shutdown <name>` | Guest Agent 안전 종료 |
| `vm export-ova <name> --output-dir /tmp` | OVA 내보내기 |
| `vm import-ova <ova_path> <name> [--pool pcvpool/vms]` | OVA 가져오기 |
| `vm usb-list <name>` | USB hostdev 목록 (live·영구·startup_policy·missing) |
| `vm usb-attach <name> <vendor_id> <product_id> [--startup-policy mandatory\|requisite\|optional] [--live-only]` | USB hostdev 연결 (실행 중 live+영구, 꺼짐 영구, 기본 optional, `--live-only`면 실행 중 VM live만) |
| `vm usb-detach <name> <vendor_id> <product_id>` | USB hostdev 분리 (있는 쪽 live·영구에서) |

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
| `container exec <name> "<cmd>"` | 명령 실행 |
| `container metrics <name>` | 메트릭 |
| `container snapshot create <name> <snap>` | 스냅샷 생성(`list`·`rollback`·`delete`도 같은 형식) |

#### 네트워크 (20+)

| 커맨드 | 설명 |
|--------|------|
| `network list` | 브릿지 목록 |
| `network create <name> --mode <mode> ... [--mtu 68..9216]` | 관리형 네트워크 생성 |
| `network create <name> --mode bridge --iface <nic> --uplink-mode dedicated --confirm-dedicated-uplink` | 전용 L2 물리 브릿지 생성 |
| `network create <name> --mode bridge --iface <nic> --uplink-mode shared --confirm-shared-uplink` | host L3를 보존하는 공유 물리 브릿지 생성 |
| `network bind <name> <nic>` | deprecated·차단 — 위 bridge create 사용 |
| `network delete <name>` | 브릿지 삭제 |
| `network mode <name> <nat\|isolated\|routed> <cidr>` | 브릿지 모드·대역 수정(canonical) |
| `network dhcp <name> --enable\|--disable` | 브릿지 DHCP 토글 |
| `ovn status` | OVN 상태 |
| `ovn switch list` | 논리 스위치 |
| `ovn router list` | 논리 라우터 |

`network edit <name> --mode <mode> --cidr <cidr>`는 이전 자동화를 위한 deprecated
호환 별칭이다. 새 호출과 문서는 `network mode`를 사용한다. 두 표면은 동일한
`network.mode_set {name, mode, cidr}` 요청을 전송한다. 단, host uplink가 붙은 physical
bridge는 live 모드 변경을 지원하지 않으며 안전한 삭제·재생성이 필요하다.

#### 스토리지 (10+)

| 커맨드 | 설명 |
|--------|------|
| `storage pool list` | ZFS 풀 목록 |
| `storage zvol list` | zvol 목록 |

#### 인증 & 보안 (10+)

| 커맨드 | 설명 |
|--------|------|
| `auth list` | 사용자 목록 |
| `auth create <user> <pass> <role>` | 사용자 생성 |
| `auth role <user> <role>` | 역할 변경 |
| `auth delete <user>` | 사용자 삭제 |
| (CLI 명령 없음) RPC/REST `auth.apikey.create` | API Key 생성. `role`을 생략하면 1(OPERATOR)이므로 원격 Prometheus용은 `role: 0`(viewer)을 명시해 `Authorization: Bearer`로 사용 |
| `security-group create <name>` | 보안 그룹 |
| `security-group rule add <name> --direction ingress\|egress --proto tcp --port 80` | 규칙 추가 |

#### 모니터링 & 알림 (10+)

| 커맨드 | 설명 |
|--------|------|
| `monitor processes --top N` | 프로세스 Top N |
| `alert list` | 알림 히스토리 |
| `alert config` | 알림 설정 조회 |
| `alert set --cpu_warn 80 --cpu_crit 95` | 알림 임계값 |

#### 백업 (5+)

| 커맨드 | 설명 |
|--------|------|
| `backup list` | 정책 목록 |
| `backup set <name> --interval N --retention N` | 정책 설정 |
| `backup history <name>` | 히스토리 |

#### 비동기 작업

CLI는 `pcvctl job list`로 목록을 보고, 조회·취소는 RPC `jobs.get`·`jobs.cancel`을 씁니다. AWS 수동 이관 명령은 제거했습니다.

#### AI Agent (5+)

| 커맨드 | 설명 |
|--------|------|
| `agent config` | Agent 설정 |
| `agent set --provider <p> --api_key <k>` | 프로바이더 설정 |
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
| `node drain` | 노드 드레인(새 RPC 접수 중단) |
| `node resume` | 드레인 뒤 다시 접수 |
| `node version` | 데몬 버전 |
| `dpdk bind <pci>` | DPDK 바인드 |
| `sriov enable <pf> [num_vfs]` | SR-IOV 활성화 |
| `gpu list` | GPU 목록 |
| `host devices [--all]` | 호스트 GPU·USB·블루투스 장치와 VM 할당 표 (`--all` 숨김 행 포함, JSON은 `--format=json`) |
| `gpu attach <vm> <pci>` | 정지 VM에 사전 VFIO GPU group 할당 |
| `gpu detach <vm> <pci>` | 정지 VM에서 GPU group 제거 |
| `audit search [--user U] [--from T] [--to T] [--method M] [--limit N]` | 감사 로그 검색 |

### 15.3 자동완성

공개 소스에는 자동완성 스크립트가 포함돼 있지 않다. 명령 자동완성은 `sudo pcvctl --interactive`(`-i`) 대화형 셸에서 Tab으로 쓴다.

---

## 16. 설정 레퍼런스

### 16.1 daemon.conf 위치

```
/etc/purecvisor/daemon.conf
```

설정 우선순위: **환경변수** > **daemon.conf** > **컴파일 기본값**

SIGHUP 시 비파괴적 재로드: `[alert]` 임계값, `log_level`, `pool_max_conn`, `drain_timeout`.

### 16.2 주요 설정 키

이 절은 주요 키만 싣고, `[metrics] auth`는 8.10, `[webpush]`는 8.13·14.7, `[ai]`는 12.1·12.3, `[tls] https_port`는 2.4의 TLS 배포 모드 선택, `[network]`·`[update]`는 2.6에서 설명합니다.

#### [daemon]

| 키 | 기본값 | 범위 | 설명 |
|----|--------|------|------|
| `socket_path` | `/var/run/purecvisor/daemon.sock` | 경로 | UDS 소켓 경로 |
| `libvirt_uri` | `qemu:///system` | URI | libvirt 접속 URI |
| `db_path` | `/var/lib/purecvisor/vm_state.db` | 경로 | SQLite 상태 DB |
| `log_level` | `info` | debug/info/warn/error | 로그 레벨 |
| `pool_max_conn` | 8 | 1-64 | libvirt 커넥션 풀 |
| `drain_timeout` | 30 | >= 5 | 드레인 타임아웃 (초). **정지 신호 인수 기한도 정한다** — SIGTERM/SIGINT를 받고 이 시간 안에 메인 루프가 정지 처리를 시작하지 않으면 데몬이 실패 코드로 스스로 종료한다. systemd `TimeoutStopSec`(180)보다 작게 둔다(같거나 크면 이 보호가 사라진다) |
| `rest_port` | 80 | 1-65535 | REST API 포트 컴파일 기본값 |
| `jwt_secret` | (최초 배포 시 생성) | 32바이트 이상 | deploy helper가 관리하는 영속 JWT 호환 fallback |
| `job_retention_days` | 30 | 정수 | 종료 Job 기록 보존 일수. 0 이하(0·음수)는 자동 정리를 끈다(11.2) |

#### [auth]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `admin_user` | `admin` | bootstrap 관리자 사용자명 (초기/비상 진입용) |
| `admin_password` | (미설정) | bootstrap 관리자 비밀번호. 첫 로그인 전 명시 설정 필요 |
| `jwt_secret` | (미설정) | 선택적 runtime 우선 JWT. `PCV_SECRET_AUTH_JWT_SECRET`이 더 우선하며, 둘 다 없으면 `[daemon]` 호환 fallback 사용 |
| `allow_self_register` | `false` | 로그인 랜딩의 셀프 회원가입 허용 여부. 생성 계정의 기본 역할은 `VIEWER` |

운영용 관리자 계정은 `pcvctl auth create <name> <password> admin` 또는 `POST /api/v1/auth/users`로 별도 생성해 사용합니다.

#### [tls]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `https_enabled` | `true` | daemon 자체 HTTPS. `false`는 nginx 외부 종료와 `[server] bind_plaintext=loopback`을 함께 쓸 때만 허용 |
| `cert` | `/etc/purecvisor/pki/node.crt` | TLS 인증서 경로 |
| `key` | `/etc/purecvisor/pki/node.key` | TLS 개인 키 경로 |
| `ca` | `/etc/purecvisor/pki/ca.crt` | CA 인증서 (mTLS, 선택) |

#### [server]

| 키 | 기본값 | 설명 |
|----|--------|------|
| `bind_plaintext` | (미설정) | `https_enabled=false`일 때 반드시 `loopback`을 명시. `all`이나 누락은 시작 거부 |

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
| `rootless` | `true` | 기본 격리 방식. `false`/`0`/`no`만 거짓(새 컨테이너 기본이 privileged가 되며 ADMIN 선택)이고 알 수 없는 값은 참으로 보고 경고를 남깁니다 |
| `rootless_uid_start` | `100000` | unprivileged 컨테이너의 호스트 시작 uid. `/etc/subuid`·`/etc/subgid`에 `root:<start>:<count>` 위임이 필요합니다 |
| `rootless_uid_count` | `65536` | 매핑 uid 개수. `start ≥ count ≥ 65536`이어야 합니다 |

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
| `data_pool_warn` | 80 | DataPool 경고 (%) |
| `data_pool_crit` | 90 | DataPool 위험 (%) |
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
| VM 삭제 실패 | ZFS 스냅샷 의존성(hold·clone) | `vm delete-status`의 `error` 확인 후 스냅샷 정리, 정의는 롤백돼 있어 재시도 가능 |
| VM 삭제 후 ISO가 사라짐 | `vm.delete`가 CD-ROM ISO `<source file>`을 파일 디스크로 오인한 회귀 | [ADR-0017](../docs/adr/0017-vm-delete-atomicity-rollback.md) 기준으로 `device='disk'` source만 삭제 대상인지 확인하고 최신 데몬 배포 |
| 메모리 누수 의심 | 알 수 없음 | `make memcheck-daemon` (Valgrind 5초) |
| 빌드 경고 | 헤더 누락/타입 불일치 | `make clean && make single` 후 경고 확인 |
| /health 느림 | libvirt 장애 (degraded) | `virsh list` 확인, libvirt 재시작 |
| 정지 때 `stop signal not handled by main loop within <N> s … exiting unsuccessfully` CRIT가 나오고 데몬이 실패 코드로 끝남 | 메인 루프가 `drain_timeout`초 동안 정지 신호를 받아 가지 못함(로그의 `current RPC`가 그때 멈춰 있던 요청) | 설계된 동작이다 — 데몬이 실패 코드로 끝나므로 `systemctl stop` 뒤 유닛이 `failed`로 남을 수 있다(`systemctl reset-failed purecvisorsd` 또는 다음 `start`로 정리). 직전 `main loop stalled` 로그의 RPC를 보고 그 요청의 외부 의존(libvirt·ZFS 등)을 확인한다 |
| `main loop stalled for <N> s (current RPC: <메서드>, running <M> s)` WARN(10초) / `main loop unresponsive for <N> s …` CRIT(60초) | 메인 루프가 멈춰 있음. `current RPC`가 원인 후보(`none`이면 요청 밖) | 해당 메서드가 기다리는 libvirt·ZFS·외부 명령 상태를 확인한다. 회복하면 `main loop recovered after <N> s stall` INFO가 남는다. 로그만 남기고 데몬을 죽이지는 않는다. 횟수는 `purecvisor_mainloop_stalls_total` 지표로 본다 |
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
| `워크로드 > 가상 머신 > 요약`에서 `디스크 사용량` 버튼이 보이지 않음 | 공개 도메인이 오래된 UI 번들을 서빙하거나 표준/호환 도메인이 서로 다른 서버를 바라봄 | `purecvisor.example.com`와 `purecvisor-compat.example.com`의 `app.bundle.js`, `sw.js` 해시를 로컬 산출물과 비교하고 두 운영 서버를 같은 릴리스로 재배포한다 |
| `vm.clone` accepted 후 clone VM이 생성되지 않음 | 오래된 배포본이 실제 libvirt disk source 대신 `daemon.conf [storage].zvol_pool` 기본값으로 target zvol을 추정 | 최신 데몬으로 재배포하고 accepted 응답의 `source_disk`/`target_disk`가 실제 XML disk source와 일치하는지 확인 |
| `vm.clone` 후 worker CPU가 높게 유지됨 | 오래된 MAC 치환 루프가 새로 생성한 `<mac address='...'>`를 다시 매칭해 무한 반복 | one-pass `g_regex_replace_eval()` 기반 수정본을 배포하고 orphan zvol/snapshot을 정리한 뒤 재시도 |
| 일반 VM `vm.clone`이 libguestfs 도구 없음으로 거부됨 | host에 `libguestfs-tools`가 설치되지 않아 `virt-sysprep`, `virt-customize`, `virt-filesystems`, `guestfish`를 실행할 수 없음 | `libguestfs-tools` 설치 후 재시도하거나, 운영자가 이미 정리한 템플릿이면 `template_prepared=true`로 호출 |
| `vm.clone`이 source power on으로 거부됨 | 실행 중인 VM의 디스크와 guest identity를 복제하면 일관성을 보장할 수 없음 | source VM을 종료한 뒤 재시도. qcow2/raw는 `mode=full` 사용 |

#### libvirtd·외부 명령이 응답하지 않을 때

오래 걸릴 수 있는 일부 RPC는 자원별 줄(레인)에서 돈다. 창구인 메인 루프는 그동안 다른 요청을 받으므로 `daemon.version`은 바로 답하고, 막힌 줄의 RPC만 느려지거나 오류가 된다.

| 줄 | 동시 실행 | 대기 칸 | RPC |
|---|---:|---:|---|
| libvirt 읽기 | 2 | 32 | `monitor.fleet`, `quota.get`, `vm.cpu.stats`, `vm.memory.stats`, `vm.blkio.get`, `vm.usb.list`, `device.nic.list`, `vm.vnc`, `get_vnc_info`, `qos.vm.get`, `resource.unregistered.list`, `host.device.list` |
| 외부 명령 읽기 | 2 | 32 | `dpdk.list`, `dpdk.status`, `gpu.list`, `gpu.metrics`, `sriov.list`, `backup.history`, `container.env.list`, `container.volume.list`, `network.host.info`, `network.info`, `network.qos.get`, `storage.pool.health`, `vm.snapshot.list`, `push.subscribe` |
| 망 변경 | 1 | 8 | `security_group.create`·`delete`·`detach`·`rule.add`·`rule.remove`, `vm.security_group.set`, `dpdk.bind`·`unbind` |
| 저장소 변경 | 1 | 8 | `storage.pool.create`, `vm.snapshot.create`·`delete`·`delete_all` |
| VM 장치 변경 | 1 | 8 | `vm.usb.attach`·`detach` |
| 컨테이너 볼륨 변경 | 1 | 8 | `container.volume.attach`·`detach` |

| 보이는 것 | 뜻 | 할 일 |
|---|---|---|
| −32004 `<method>: server busy` (REST 409) | **BUSY.** 그 줄의 대기 칸이 찼다. 요청은 실행되지 않았다 | 잠시 뒤 다시 보낸다. 계속되면 그 줄이 기다리는 libvirtd·ZFS·외부 명령을 확인한다 |
| −32004 `<method>: queued too long` (REST 409) | 변경 줄에서 8초 넘게 기다려 **실행하지 않았다.** | 앞 작업이 끝난 뒤 다시 보낸다 |
| −32004 `VM '<이름>' is already locked (op: SNAPSHOT, …)` (REST 409) | 비 ZFS VM의 스냅샷 생성·삭제와 겹쳤다 | 스냅샷이 끝난 뒤 다시 보낸다 |
| −32000 `<method>: host command timed out`(`vm.snapshot.list`는 zfs 확인이 멈출 때 폴백 없이 이 응답) | 외부 명령 읽기가 명령당 10초(호출당 20초)를 넘겼다. 감사는 `fail`이다. REST는 8초 504, CLI는 10초 소켓 시간 초과가 먼저 보일 수 있다 | `zfs`·`zpool`·`tc` 등 그 명령과 호스트 상태를 확인한다 |
| −32000 `server is shutting down` | 데몬이 내려가는 중이라 실행하지 않았거나, 진행 중 명령이 끊겼다 | 재시작 뒤 다시 보낸다 |
| −32000 `Hypervisor connection pool exhausted`, 로그 `libvirtd not answering for <N> s; refusing libvirt calls on the main loop` | libvirtd가 10초 넘게 답하지 않아 메인 루프의 libvirt 호출을 하지 않는다. 이름 지정 `vm.start`도 핀닝 없이 뜨는 대신 같은 오류로 거절된다 | libvirtd를 확인한다. 회복하면 `libvirtd answering again` INFO가 남고 다시 보내면 된다 |
| OPERATOR가 자기 VM에 −32006 `Permission denied: operators can access only VMs they created` | libvirtd 무응답이면 소유자를 확인할 수 없어 거부한다. 권한 문제가 아니다 | libvirtd 회복 뒤 다시 보낸다 |
| `vm.list` −32000 `Failed to list domains.` (REST 500) | 목록을 읽는 중 연결이 끊겼다. VM이 빠진 목록을 내지 않는다. UI는 직전 목록을 유지한다 | libvirtd 회복 뒤 다음 주기에 정상으로 돌아온다 |
| `/health` 503 `critical`이 약 3초 뒤 | libvirt 확인이 3초 안에 끝나지 않았다. 5초 동안 캐시한다 | libvirtd를 확인한다 |

정지 감지 횟수는 `purecvisor_mainloop_stalls_total` 지표(`level` = `warn`, `crit`, counter)로 본다. `monitor.metrics`, `snapshot.schedule.status`, `health.deep`과 변경 RPC 대부분은 아직 메인 루프에서 돈다. 무응답이 시작되고 10초 안에 들어온 그런 호출은 libvirtd가 회복할 때까지 묶일 수 있다.

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
curl -s http://127.0.0.1:8080/api/v1/health | python3 -m json.tool

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
| CB | Circuit Breaker. 장애 전파 차단 패턴 (CLOSED/OPEN/HALF_OPEN) |
| CSI | Container Storage Interface |
| DPDK | Data Plane Development Kit. 커널 바이패스 패킷 처리 |
| etcd | 분산 키-값 저장소. 클러스터 메타데이터 관리 |
| fire-and-forget | 응답 즉시 반환 후 비동기 실행하는 패턴 |
| GMainLoop | GLib 이벤트 루프. PureCVisor의 핵심 실행 모델 |
| GTask | GLib 비동기 태스크. 스레드 풀에서 실행 |
| io_uring | Linux 비동기 I/O 인터페이스 |
| JSON-RPC | JSON Remote Procedure Call. UDS 통신 프로토콜 |
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
| -32001 | 대상 없음(VM·Job·보안 사건 등, REST 404) |
| -32002 | 상태 충돌(설정 revision 불일치, 같은 USB 장치를 다른 `startup_policy`로 다시 붙임 등, REST 409) |
| -32003 | 시간 초과(REST 504) |
| -32004 | BUSY (큐 포화·대기 초과·VM 잠금 겹침, 다시 시도, REST 409) |
| -32005 | 대상 없음(VM 외 일부 자원, REST 404) |
| -32006 | Permission denied (REST 403) |

`-32602`는 `PureRpcErrorCode`의 canonical invalid-params 값입니다. 예를 들어 인증 REST의
OVN ACL `switch` 또는 NAT `router` 필터가 누락되면 이 값을 사용하며, handler나 adapter가
raw 숫자 리터럴로 별도 의미를 만들지 않습니다.

### 18.3 외부 의존성

#### 빌드 시 의존성

| 패키지 | 용도 |
|--------|------|
| gcc-14 | C23 컴파일러, 검증 기준 노드 확인값 14.3 |
| make | 빌드 시스템 |
| libglib2.0-dev | GLib/GIO/GObject |
| libsoup-3.0-dev | HTTP 서버 (REST API) |
| libjson-glib-dev | JSON 파싱 |
| libvirt-dev | VM 관리 |
| libvirt-glib-1.0-dev | libvirt GObject 바인딩 |
| lxc-dev | LXC 컨테이너 (Ubuntu 26.04 패키지 이름) |
| libsqlite3-dev | SQLite (상태 DB, 감사 로그) |
| libssl-dev | OpenSSL crypto/JWT 서명 |
| libcap-dev | Linux capabilities |
| libseccomp-dev | Seccomp 시스템 콜 필터 |
| libreadline-dev | CLI REPL |
| liburing-dev | io_uring fast path |
| libbpf-dev | eBPF loader와 BTF 경로 |
| libxml2-dev | libvirt-gconfig pkg-config 전이 의존 |
| protobuf-c-compiler / libprotobuf-c-dev | protobuf-c 코드 생성과 링크 |

#### 런타임 의존성

| 패키지 | 용도 |
|--------|------|
| libvirt-daemon-system | libvirtd |
| qemu-system-x86 / ovmf | QEMU/KVM 하이퍼바이저와 UEFI firmware |
| libguestfs-tools | 일반 VM 복제 Guest reset 도구 묶음 |
| zfsutils-linux | ZFS 파일시스템 |
| lxc | LXC 컨테이너 런타임(`lxc-attach` 등 도구 포함) |
| openvswitch-switch | OVS 가상 스위치 |
| ovn-central / ovn-host | 선택형 Local OVN control plane과 host controller |
| (패키지 없음 — 커널 LIO) | iSCSI 타겟. 커널 LIO를 쓰므로 `tgt`에 의존하지 않는다. `/etc/modules-load.d/purecvisor-lio.conf` 로 모듈만 확보한다 — **deb 가 conffile 로 설치한다**. deb 를 쓰지 않는 배포에서만 운영자가 직접 만든다 |
| open-iscsi | iSCSI 이니시에이터 |
| dnsmasq | DHCP/DNS |
| nftables | 방화벽 |

### 18.4 포트 목록

| 포트 | 프로토콜 | 용도 |
|------|---------|------|
| 443 | TCP/HTTPS | `purecvisorsd` 기본 REST API + Web UI + WebSocket |
| 8080 | TCP/HTTP | `127.0.0.1` 복구 listener 또는 선택형 NGINX upstream, 외부 공개 금지 |
| 80 | TCP/HTTP | 선택형 NGINX HTTP→HTTPS redirect를 사용할 때만 외부 수신 |
| 4789 | UDP | 선택형 VXLAN overlay를 구성한 경우 |
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

2026-11-11 공개 `2.2.0` 소스의 현재 도움말·필수 gate는 다음과 같습니다. 소스 게시와
전체 실환경 인증은 구분합니다.

| 현재 항목 | 값·범위 |
|---|---|
| 도움말 표시 | RPC 317·REST 225 (`make check-help-counts`) |
| 지원 범위 | Single Edge·OVA·로컬 백업, AWS/S3 제거 |
| 활성 DB | 9개·영구 테이블 28개; 과거 Cloud DB는 자동 삭제하지 않음 |
| Web UI | `tests/ui/*.test.mjs` 77개 |
| 주요 정적 게이트 | `make -j1 check-all` 47개 게이트 |
| 운영 기능 아티팩트 | 공개 소스 검증과 별도이며 실제 설치본의 source commit으로 확인 |


다음 수치는 2026-11-11 공개 `2.2.0` 소스의 추적 파일과 `Makefile`,
`make check-rbac`를 대조한 스냅샷입니다. 시험 통과 수는 같은 소스의 게시 전 검증 기록이며,
이번 문서 현행화에서 전체 제품 시험을 다시 실행했다는 뜻은 아닙니다.

| 항목 | 2026-11-11 기준 |
|------|----------------|
| C 표준 | `-std=gnu23` |
| 에디션·버전 | Single Edge · `2.2.0` |
| RPC 등록/정책 | RPC 318건, 정책 매핑 333건, 정책표 밖 조회성 VIEWER 기본 0건 — 모든 라우트가 정책표에 매핑됨 (`make check-rbac`) |
| C/H 소스 | `src/` 아래 C 265개, 헤더 239개, 총 229,701행 (공백 포함) |
| Web UI | `ui/modules/*.js` 30개, 추적 UI 파일 65개 |
| 테스트 파일 | `tests/` C 182개, `tests/integration/` 116개, `tests/ui/*.test.mjs` 69개 |
| 공개 문서 사이트 | 21개 운영 가이드 장 + DB 아키텍처, 8개 분류·22개 문서 |
| 공개 소스 검증 기록 | release·debug 빌드 각각 C 2,514 PASS·22 SKIP, audit startup 5 PASS, UI 613 PASS |
| 계약 게이트 | `make check-all` 47개 — 정확한 목록은 Makefile 의존성과 22.4절 |
| 운영 인증 경계 | 지정 시험 통과와 전체 감사·지원 환경 인증은 별도이며 22.8절 참조 |

파일 수는 `git ls-files`의 해당 경로·확장자로 집계합니다. 등록 테스트 수·실행 통과 수와
소스 파일 수는 서로 다른 값입니다. RPC 등록 수는 라우트 표 기준이며, 도움말 표시 수는 여기서
레거시 별칭 1건을 뺀 값입니다. 과거 2026-09-15 `2.0.0` 스냅샷(`22d6912`, C 1,479 PASS·
UI 512 PASS·게이트 40)은 22.8절, 2026-08-31 C 1,375/1,375·게이트 38/38 기록은 22.7절의
과거 회차와 함께 읽습니다.

---

## 19. 개발자 & 엔지니어 가이드

이 장은 PureCVisor Single Edge를 **수정, 검증, 배포, 장애 분석**하는 사람을 위한 작업 기준이다. 앞 장들이 기능 사용법을 설명한다면, 이 장은 변경을 안전하게 넣고 운영 상태를 해석하는 순서를 정의한다.

### 19.1 독자와 목표

| 독자 | 이 장에서 얻어야 하는 것 |
|------|--------------------------|
| 신규 개발자 | 저장소 경계, 핵심 모듈, 먼저 읽을 문서, 최소 빌드/테스트 루틴 |
| 백엔드 엔지니어 | RPC 추가/변경, RBAC, fire-and-forget, audit, libvirt/ZFS/LXC 연동 규칙 |
| 프론트엔드 엔지니어 | Vanilla JS 모듈 구조, `PCV.*` 네임스페이스, `EP` 엔드포인트 레지스트리, 안전한 DOM 노드 빌더(`PCV.uxlib.el`·`HN`) 규칙 |
| 운영/SRE | systemd, 로그, 헬스 체크, libvirt/ZFS/OVS 상태 확인, 장애 증거 수집 |
| 릴리스 담당 | Single Edge 공개 범위, 검증 명령, 문서/배포 산출물 동기화 |

### 19.2 빠른 독해 경로

| 상황 | 먼저 볼 장 | 다음 확인 |
|------|------------|-----------|
| 처음 빌드한다 | 2장 설치 및 환경 구성 | 21장 아키텍처 리팩토링, 22장 품질 게이트 |
| VM 기능을 바꾼다 | 3장 VM 관리 | [ADR-0022](../docs/adr/0022-vm-create-storage-location-contract.md), [ADR-0023](../docs/adr/0023-vm-clone-beta-safety-guard.md), `tests/test_vm_clone_plan.c` |
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
- UI 모듈은 `PCV.*` 네임스페이스와 `ui/modules/endpoints.js`의 `EP` 레지스트리를 사용한다. DOM은 `innerHTML` 없이 `PCV.uxlib.el`·`HN` 노드 빌더와 `textContent`로 조립한다.

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
| VM clone | `./test_runner -p /vm_clone_plan`, `scripts/check_vm_clone_cleanup.py`, [ADR-0023](../docs/adr/0023-vm-clone-beta-safety-guard.md) 실환경 기준 확인 |
| Web UI | `PCV_NO_DEPLOY=1 scripts/bundle-ui.sh`, `python3 scripts/check_ui_bundle_fresh.py`, `node --check ui/app.bundle.js`, 공개 URL의 bundle/guide/SW 해시 확인 |
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
| 메인 루프가 멈춘 것 같다(UDS 응답 없음) | `journalctl -u purecvisorsd`에서 `main loop stalled`(WARN 10초)·`main loop unresponsive`(CRIT 60초)·`main loop recovered`(INFO)와 `current RPC`, 그 메서드가 기다릴 libvirt(`virsh list`)·ZFS(`zpool status`) 상태. 정지가 `exiting unsuccessfully` CRIT로 끝났다면 같은 줄의 RPC 이름이 정지 원인 후보 |
| VM IP가 비어 있다 | `monitor.fleet`의 `ip_source`, 데몬 로그의 `guest agent of VM … did not answer`(WARN, 백오프 초), VM 안 `qemu-guest-agent` 상태. 에이전트 IP는 한 폴링 주기 늦게 나올 수 있다 |

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
| 보안 담당 | 권한과 추적성 | JWT, RBAC, owner-scope, audit log, 보안 그룹 같은 운영 통제가 있다. |
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
- 보안 설명: RBAC, audit, 보안 그룹, 기본 admin 교체 권장
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

아래 줄 수는 리팩토링 당시 값이며 지금 파일 크기와 다릅니다.

```
리팩토링 전:                          리팩토링 후:
rest_server.c (3,942 LOC)     →     rest_server.c (3,100 LOC)
                                    + rest_middleware.c (287 LOC)
                                    + rest_middleware.h (75 LOC)

purecvisorctl.c (6,996 LOC)   →     purecvisorctl.c (6,700 LOC)
                                    + cli_rpc.c (190 LOC)
                                    + cli_rpc.h (65 LOC)
                                    + cli_output.c (280 LOC)
                                    + cli_output.h (34 LOC)
```

### 21.2 REST 미들웨어 (`src/api/rest_middleware.c`)

HTTP 요청 처리 파이프라인에서 **횡단 관심사**(cross-cutting concerns)를 분리한 모듈입니다.

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
| `pcv_validate_required(msg, body, fields, count)` | 필수 파라미터 검증 | REST 본문 필드 체크 |
| `pcv_get_endpoint_rate_limit(path, http_method)` | 엔드포인트별 Rate Limit 티어 | `/auth/*` 60, `/metrics` 3600 |
| `pcv_get_rpc_timeout(method)` | RPC 메서드별 타임아웃 | `vm.create` 30초, `vm.list` 8초 |
| `pcv_rest_error(msg, status, code, detail)` | REST 에러 응답 생성 | 인증 실패, 파라미터 오류 |

#### Rate Limit 티어

```c
constexpr int 기본      = 600;   // req/min (일반 API)
constexpr int 인증      = 60;    // req/min (브루트포스 방지)
constexpr int 모니터링  = 3600;  // req/min (에이전트 폴링)
constexpr int VM생성    = 120;   // req/min (리소스 보호)
```

### 21.3 CLI RPC 통신 (`src/cli/cli_rpc.c`)

CLI(pcvctl)에서 데몬과 통신하는 **UDS JSON-RPC 클라이언트**입니다.

```
pcvctl vm list
    ↓
cli_rpc.c: purectl_send_request("vm.list", params)
    ↓
UDS 소켓: /var/run/purecvisor/daemon.sock
    ↓
dispatcher.c → handler_vm_lifecycle.c → vm_ops.c
```

#### PcvCtx 전역 컨텍스트

```c
typedef struct {
    OutputFormat  fmt;              // --format=table|json|plain|csv
    bool          interactive;      // --interactive, -i
    bool          batch;            // --batch
    bool          no_color;         // --no-color
    bool          verbose;          // --verbose, -v
    const char   *socket_path;      // --socket=<path>
    PcvCliExitStatus command_status; // 명령 종료 상태(0/1/2)
} PcvCtx;

// 글로벌: 모든 CLI 커맨드에서 공유
extern PcvCtx g_ctx;
```

#### 색상 조건부 출력

```c
// cc()는 stdout, ce()는 stderr용: --no-color이거나 터미널이 아니면 (파이프 출력) 빈 문자열
printf("%sVM 시작됨%s\n", cc(CYBER_GREEN), cc(CYBER_RESET));  // 터미널: 초록색 / 파이프: 무색
```

### 21.4 CLI 출력 포맷터 (`src/cli/cli_output.c`)

CLI의 **구조화된 출력 시스템**입니다.

#### PcvTable 시스템

```c
// 사용 예시: --format=plain|csv 일 때 VM 목록 출력
const char *hdrs[] = {"NAME", "STATE", "CPU", "MEM"};
PcvTable *t = ptbl_new(hdrs, 4);
ptbl_row(t, "web-prod", "running", "2.5%", "1.2G");
ptbl_row(t, "db-prod",  "stopped", "-",    "4.0G");

if (g_ctx.fmt == FMT_CSV)
    ptbl_print_csv(t);    // NAME,STATE,CPU,MEM\nweb-prod,running,...
else
    ptbl_print_plain(t);  // 헤더 없는 탭 구분 행

ptbl_free(t);
```

#### 출력 모드

| 모드 | 플래그 | 형식 |
|------|--------|------|
| 테이블 | (기본) `--format=table` | 정렬된 컬럼 + 색상(터미널이 아니거나 `--no-color`면 색상 없음) |
| JSON | `--format=json` | raw JSON 응답 |
| CSV | `--format=csv` | RFC 4180 CSV |
| Plain | `--format=plain` | 색상·테두리 없는 탭 구분 출력 |

### 21.5 디스패처 핸들러 시그니처 (`src/api/dispatcher.c`, `src/modules/dispatcher/handler_*.c`)

RPC 핸들러는 모두 **같은 시그니처**를 가지며, `g_rpc_routes` 해시테이블이
메서드 이름(`"vm.clone"` 등)을 그 함수 포인터로 매핑합니다.

**정의 위치는 한 곳이 아닙니다** — 핸들러 본문은 네임스페이스별
`src/modules/dispatcher/handler_*.c`(일부는 `src/modules/network/network_manager.c`)로 나뉘어 외부 링크를 갖고,
`src/api/dispatcher.c`에는 디스패치 흐름과 라우트 등록이 남습니다. 정책표(`src/api/dispatcher_policy.c`)·owner-scope
실행부(`src/api/dispatcher_owner_scope.c`)·영역 공용 보조(`src/modules/dispatcher/dispatcher_shared.c`)도 별도 파일입니다. 한 곳에 모여 있는 것은
정의가 아니라 **등록**입니다 — `dispatcher_init_routes()` 의 `g_rpc_routes` 삽입 목록이
"어떤 메서드가 존재하는가"의 진리원이고, 계약 게이트들도 그 표를 읽습니다.

```c
// 핸들러 함수 시그니처 (모든 RPC 핸들러의 공통 패턴)
typedef void (*PcvDispatchHandler)(
    JsonObject      *params,      // RPC 파라미터
    const gchar     *rpc_id,      // 요청 ID (응답 매칭용)
    UdsServer       *server,      // UDS 서버 인스턴스
    GSocketConnection *connection  // 클라이언트 연결
);
```

### 21.6 커밋 메시지 Hook (`scripts/commit-msg`)

**Conventional Commits** 포맷을 강제하는 Git hook입니다.

```
허용 접두사: feat | fix | refactor | perf | docs | chore | test | ci | style

형식:
  <type>: <description>
  <type>(scope): <description>

예시:
  feat: VM 스냅샷 목록 필터 추가
  fix(rest_server): rate limiter 1024 IP 우회 수정
  refactor: C11 → C23 전환
  docs: CHANGELOG.md v1.0 업데이트
```

### 21.7 감사 후속의 결과·자원 수명 계약

2026-09-07 개발선 감사는 기존 단일 프로세스·GMainLoop·GTask 구조 안에서 결과와 자원의 소유권을 재검토했습니다. 새로운 프로세스 구조나 프레임워크 도입을 완료한 것으로 해석하지 않습니다. 이 가운데 공통 Job 접수·결과 영속화와 Trace 자식 회수는 2026-10-08 공개 소스 갱신에서 보강됐습니다(ADR-0060, `CHANGELOG.md`의 2.0.0 소스 갱신).

| 검토 영역 | 확인해야 할 계약 | 2026-09-07 당시 상태 |
|---|---|---|
| Job 결과 | accepted 응답·Job ID·영속 row·실제 작업 결과가 일치하고 SQL 실패와 ID 충돌을 식별해야 함 | 시정·독립 검증 잔여 |
| Trace 자식 프로세스 | stop 요청, 실제 wait 회수, 실행 guard 해제가 순서대로 확인돼야 함 | 실제 도구·환경 검증 잔여 |
| 비동기 I/O | 실패 반환한 제출의 후속 실행 방지, pending 접근 동기화, 미전달 결과 FD와 ring 소유권 정리 | 지정 경로의 발견을 후속 처리 중 |
| UI 요청과 창 | 대상 VM 식별과 요청 순번, 원래 창의 완료·timer 수명이 일치해야 함 | 개발선 지정 시정·독립 리뷰·UI 배포 확인 완료 |
| 모니터링 표시 | 누적 counter, 실제 경과 시간과 차트 단위·시간축이 일치해야 함 | 시정·독립 검증 잔여 |

본문 읽기·격리 시험·실환경 확인·독립 리뷰는 서로 다른 증거입니다. 지정 경로의 통과를 해당 모듈 전체나 공개 릴리스의 완료로 확대하지 않습니다.

---

## 22. 품질 게이트 가이드

### 22.1 개요

PureCVisor는 커밋 훅과 계약 게이트 타깃으로 코드 품질을 자동 검증합니다.

```
커밋 시:
  설치된 pre-commit hook (변경 영역별 조건부 검사) → 선택 설치한 commit-msg hook → 커밋 완료

릴리스 전:
  make check-all (계약 게이트 전량) + make test (등록된 전체 테스트) → 검증 정책의 릴리스 조건 확인
```

pre-commit은 staged 변경 종류(C 소스 / API / UI / 전체)를 보고 실행할 게이트를 고르므로 실제 실행
수는 커밋마다 다릅니다. 문서만 바꾼 커밋은 빌드·테스트 게이트를 전부 건너뜁니다. 반면
`make check-all`은 조건 분기 없이 **의존 목록 전량**을 매번 돌립니다. 게이트 수의 정본은 항상
Makefile 의 `check-all:` 의존 목록입니다 — 문서의 숫자가 어긋나면 Makefile 이 옳습니다.

### 22.2 pre-commit hook (변경 영역별 조건부 검사)

```bash
# pre-commit 설치
make install-hooks
# 커밋 메시지 형식 검사도 사용할 경우 별도 설치
cp scripts/commit-msg .git/hooks/commit-msg
chmod +x .git/hooks/commit-msg
```

| 단계 | 게이트 | 트리거 | 차단 |
|------|--------|--------|------|
| 1 | 기본 빌드(`make all`, Single Edge) 경고 0 | C 변경 | 차단 |
| 2 | Single Edge 빌드 경고 0 | C 변경 | 차단 |
| 3 | 등록된 유닛 테스트 (hook 최소 200건) | C 변경 | 차단 |
| 4 | REST↔RPC 정합성 | API 변경 | 차단 |
| 4 계열 | 변경 영역별 계약 게이트와 도움말 수 검사 (`scripts/pre-commit`의 조건이 정본) | 해당 영역 변경 | 차단 |
| 5 | 프론트엔드 패턴 (10건 임계값) · ESLint · OpenAPI 동기화 · UI 번들 최신성 · XSS 패턴 · i18n 키 정합성 | UI 변경 | 차단 |
| 6 | cppcheck 정적 분석 에러 | C 변경 + cppcheck 설치 | 차단 |
| 7 | 바이너리 크기 3MB 상한 | C 변경 | 차단 |
| 8 | SAFE 통합 테스트 + 보안 스캔 (OWASP Top 10) | API/UI + 데몬 실행 | 차단 |
| 9 | 문서에 RPC/테스트 수가 있을 때 정합성 보조 검사 | C 변경 | 경고 |
| 10 | 신규 TODO/FIXME | C 변경 | 경고 |
| 11 | ASan/UBSan test_runner — 스테이징된 트리를 임시 디렉터리로 꺼낸 사본(`git checkout-index`)에서 훅 시작과 함께 백그라운드로 돌며 작업 트리는 make clean·재빌드하지 않는다. ASan/UBSan 오류 문구가 있으면 차단하고, 오류 문구 없는 5분 타임아웃은 경고 후 생략하며, 그 밖의 비정상 종료 코드(빌드·링크 실패 포함)는 차단한다. 사본은 버리는 트리라 `PCV_SANITIZE_DISPOSABLE_TREE=1`로 계측 실패·중단 때 복원을 건너뛴다 | C 변경 + sanitize 가능 | 차단 |
| 12 | Valgrind 표적 검사 — 스테이징된 `tests/*.c`가 등록한 GLib 시험 경로만(`check_test_runner_selectors.py --paths-for`, `test_runner -p 경로…`) 300초 상한으로 돈다. 메모리 오류·definite leak과 경로 추출 실패는 차단하고, 그 밖의 시간 초과·valgrind 아래 시험 실패는 경고한다. 시험 파일이 스테이징되지 않으면 생략한다 — 전체 memcheck는 `make memcheck` 또는 최상위 suite 단위를 `MEMCHECK_JOBS`개씩 동시에 돌리는 `make memcheck-sharded`(로그·합본 요약은 `valgrind_shards/`) | C 변경 + valgrind 설치 + `tests/*.c` staged | 차단 |
| 13 | gitleaks 시크릿 스캔 | staged diff | 차단 |

커밋 메시지 형식은 pre-commit이 아니라 commit-msg hook이 판정합니다(22.3 참조).

설치된 훅(`.git/hooks/pre-commit`)은 `scripts/pre-commit`의 **복사본**입니다. `scripts/pre-commit`을 고치면 `make install-hooks`를 다시 실행해야 반영됩니다.

### 22.3 commit-msg hook

```
허용 접두사: feat | fix | refactor | perf | docs | chore | test | ci | style

형식:
  <type>: <설명>
  <type>(scope): <설명>

예시:
  feat: VM 스냅샷 목록 필터 추가
  fix(rest_server): rate limiter 우회 수정
  perf: json_generator → json_to_string 전환
```

### 22.4 계약 게이트 일괄 — `make check-all` (47게이트)

```bash
make -j1 check-all
```

| 게이트 | 검사 범위 |
|---|---|
| `check-aws-manual-removed` | AWS 수동 이관·S3 실행/API/UI 부재·북마크와 VM/OVA/로컬 백업/일반 Job 보존 |
| `check-help-contract` | help.js RPC 역할 표 = `dispatcher_policy.c`, REST 예시의 경로·본문 키 = 라우팅·핸들러, 예시 이름은 실재하기 어려운 값 |
| `check-rbac` | ADR-0019 RBAC 정책 계약 |
| `check-rpc-consumers` | RPC 소비⊆등록 계약 |
| `check-dead-exports` | 미소비 공개 심볼(dead export) 차단 |
| `check-rpc-param-contract` | RPC param-key 계약 |
| `check-json-ingress` | JSON 파싱 초크포인트 |
| `check-safety-controls` | 안전통제 효과 테스트 레지스트리 |
| `check-error-codes` | raw 에러코드 리터럴 방지 |
| `check-cli-exit-status` | `pcvctl` 성공·실패·사용법 오류 0/1/2와 VPC terminal Job 효과 |
| `check-audit-placement` | audit 배치 계약 (ADR-0018). Job 스키마 복원 계약 시험 포함(이전 판 Job 코드는 `tests/fixtures/job_queue_previous/`의 고정 사본으로 빌드해 대조하며, 사본이 없으면 실패) |
| `check-cors-anchor` | CORS 오리진 앵커 (A05·V3·V13). REST 요청 경로 계약 시험 2종(cleanup 초기화·segs 경계) 포함 |
| `check-secret-logging` | 감사 로그 자격증명 마스킹 (A09·V14·V16) |
| `check-ssrf-guard` | 아웃바운드 리다이렉트 금지 (A10·V4) |
| `check-grpc-authz` | gRPC 인증/RBAC (A01·V8) |
| `check-ssrf-target-guard` | 아웃바운드 대상 SSRF allowlist (A10·V4) |
| `check-audit-hashchain` | 감사 로그 해시체인 (A09) |
| `check-rng-safe` | 보안 RNG/PBKDF2 하드닝 (A02·V11) |
| `check-uds-authz` | UDS root-only 접근 (A01·V8) |
| `check-transport-bind` | 평문 전송 루프백 바인딩 (A02·V12) |
| `check-proxy-identity` | 프록시 클라이언트 신원 단일 경계 |
| `check-container-owner-scope` | 컨테이너 operator owner-scope (A01) |
| `check-mtls-wiring` | mTLS 클라이언트 인증서 검증 배선 (A02·V12) |
| `check-tls-min-version` | TLS 최소 버전 고정 (A02·V11·V12) |
| `check-secret-wipe` | 시크릿 free-without-wipe 방지 |
| `check-security-headers` | `/ui` 정적 응답 보안 헤더 (A05) |
| `check-password-policy` | user-create 비밀번호 복잡도 정책 (A07) |
| `check-ws-token-url` | WS URL-query 토큰 인증 제거 (A07) |
| `check-zpool-suspend-recover` | ZFS 풀 SUSPENDED 탐지 + 가드된 자동복구 |
| `check-deb-apparmor` | 2.0 deb AppArmor 미부착 (ADR-0028) |
| `check-public-comments` | 설명 주석·소스맵 제외, 변환 회귀와 공개 실행 계약 |
| `check-vendor-integrity` | 벤더링 자산 SHA-256 핀 — 전수 등재·유령·심링크 우회 차단 (A03) |
| `check-npm-lockfile` | npm 의존 SRI 핀·레지스트리 단일 출처·lock 드리프트 0 (A03) |
| `check-deb-supply-chain` | deb 의존 버전 하한 배선·md5sums 전수·벤더 핀 전이 (A03) |
| `check-fe-rpc-params` | UI 요청 파라미터 키와 backend 소비 계약 |
| `check-network-mode-contract` | UI/backend network mode enum 양방향 정합성 |
| `check-iscsi-chap-argv` | initiator CHAP 비밀번호 argv 재도입 차단 |
| `check-rpc-route-unique` | `g_rpc_routes` 중복 등록 금지 |
| `check-rerror-guard` | Web UI JSON-RPC `r.error` 미검사 호출부 래칫 |
| `check-dpdk-owned-lifecycle` | DPDK OVS ownership·MTU·VM 수명주기와 accepted Job 결과 채널 |
| `check-single-ui-surface` | 이벤트·경보·감사·명령 연결, VM 디스크 사용량 표면과 개발판 소스맵 배포 계약 |
| `check-runtime-prereqs` | 원격 daemon/CLI ABI stop-before-mutation · 배포 런타임 전제 · nginx 종단 · LIO 모듈 패키징 |
| `check-vnc-proxy-lifetime` | 실제 binary relay·REST context·WS signal/source/FD 종료 수명 (ADR-0065) |
| `check-vnc-flow-control` | 인증·소유 VM·ACK·bounded queue·inline 경쟁·REST 종료 효과 |
| `check-vm-start-single-entry` | VM 시작 단일 진입점: start/stop async 직접 참조·virDomainCreate* 허용 목록 밖 호출 차단(래칫). 새 VM 이름 UUID 거부·`vm.rename` 잠금 계약 시험 포함 |
| `check-file-size-ratchet` | 파일 크기 래칫: 추적 소스 파일 상한 1,500줄, 기준선 파일은 줄이는 방향만 허용 |
| `check-mainloop-reach` | 메인 루프에서 도달 가능한 차단 호출의 래칫: 기준선 밖의 새 도달을 막고, 비어 있어야 하는 범주(`g_mutation_mu`·동기 DNS)는 0을 지킨다 |

### 22.5 품질 게이트 건너뛰기

```bash
# 긴급 커밋 (pre-commit + commit-msg 모두 건너뜀)
git commit --no-verify -m "fix: 긴급 수정"

# 주의: 훅을 건너뛴 커밋도 릴리스 전 make check-all + make test는 그대로 통과해야 함
```

### 22.6 신규 RPC 추가 시 체크리스트

1. `handler_xxx.h`에 함수 선언
2. `handler_xxx.c`에 핸들러 구현
3. `dispatcher.c` 라우트 등록(`g_hash_table_insert`)과 `src/api/dispatcher_policy.c` 정책표(`g_method_policies`)에 역할 행 추가 — 정책 행이 없으면 ADMIN 전용으로 잠긴다(ADR-0079)
4. `Makefile`의 `DAEMON_COMMON_SRCS`에 소스 등록(`DAEMON_SRCS`가 이를 포함)
5. `make clean && make all` — 경고 0 확인
6. `nc -U` 수동 RPC 테스트
7. REST 필요 시 매핑 사슬(`rest_map_compute.c`·`rest_map_observe.c`·`rest_map_platform.c` 중 그 리소스 묶음)에 라우팅 추가
8. `scripts/verify_api_consistency.sh` — FAIL 0 확인
9. 공개 가이드·RPC 계약을 갱신하고 `make check-rbac`로 등록·정책 매핑 검증
10. `git commit` — pre-commit 조건부 게이트 통과

---

### 22.7 2026-09-07 검토·시정 현황

아래는 **개발선의 감사·시정 회차**입니다. 공개 저장소의 제품 코드와 시험 수치, 지원 환경 인증은 별도로 판정합니다. 파일 목록을 만들거나 시험이 통과했다는 이유만으로 전체 감사를 완료 처리하지 않습니다.

| 항목 | 확인 결과 | 판정 경계 |
|---|---|---|
| 전수 대상 | 현행 1,600파일 목록·해시 고정 | 목록화는 본문 검토가 아님 |
| 이번 본문 검토 | 소스·시험 22파일, 4,626행과 직접 호출부 | 나머지 파일·계약은 후속 검토 대상 |
| 기존 발견 처분 | 36건 중 34건 시정 검증·2건 후속 | 기존 회차의 누적 처분 |
| 새 회차 발견 | I/O·모니터링 7건 미완료(P2 5·P3 2) | 기존 36건 처분과 구분 |
| 지정 UI 시정 | 전체 60파일 505 PASS·독립 대조 44 PASS | 지정 시정의 로컬 검증 결과 |
| UI 운영 확인 | 독립 운영 노드 2대의 자산 21/21·브라우저 cache 17/17·기본 로드·health 정상 | 실제 VM 변경 효과·모든 환경 인증은 별도 |
| 전체 감사 | **FAIL(미완료)** | 미검토 소스, 결과·회수 계약과 지원 환경·전체 메모리 검사 잔여 |

독립 리뷰는 변경 작성자와 별도 검토자가 같은 소스·입력·원본 결과를 대조하는 절차입니다. 시정 후 재검토와 실제 설치 확인은 완료 범위를 구체적으로 기록하며, 개발선 결과를 공개 소스의 시정 완료로 전용하지 않습니다.

공개 스냅샷의 C 1,375/1,375·audit startup 5/5와 계약 gate 38/38은 2026-08-31 기록입니다. 2026-09-07 문서 배포에서 다시 실행한 전체 C·제품 UI 시험 수치가 아니며, 후속 공개 소스 검증은 22.8절을 따릅니다. Pages 자체는 콘텐츠 build·route·링크·공개 자산 검사와 게시 후 HTTP·브라우저 확인으로 검증합니다.

---

<a id="228-2026-09-15-공개-소스문서-현황"></a>

### 22.8 공개 소스·문서 현황

**2026-11-11 이후 공개 main:** 새 릴리스 없이 2.2.0 뒤의 후속 변경을 담습니다. ZFS 풀 잠금 자가 초기화의 스레드 안전성 수정,
도움말 모니터링 카드의 메트릭 경로 수정, C·브라우저 UI 시험 보강(`tests/ui/*.test.mjs` 69개 → 77개)이 들어 있습니다.
변경 목록은 `CHANGELOG.md` 맨 위 '2026-11-11 2.2.0 이후 main 변경' 항목을 따릅니다. RPC·REST·DB 스키마·`daemon.conf`
형식과 제품 버전 문자열 `2.2.0`은 바뀌지 않았습니다.

**2026-11-11 현재 소스 (2.2.0):** 2.1.0 이후의 Single Edge 기능을 담은 기능 릴리스입니다.
변경 목록은 `CHANGELOG.md`의 2.2.0 항목을 따릅니다.
게시 전 검증에서 공개판 `make check-all` 47개 게이트, release·debug 빌드 각각의 C 단위 시험
2,514 PASS·22 SKIP과 audit startup 5 PASS, 브라우저 UI 시험 613 PASS, 패키지 검사, 격리된
Ubuntu 26.04 컨테이너의 설치·업그레이드·제거 lifecycle과 개발 호스트의 실제 업그레이드·되돌림을
확인했습니다. 컨테이너 검증에는 systemd 부팅, 실제 KVM 작업과 커널 BPF 적재가 포함되지 않습니다.

**2026-11-09 현재 소스 (2.1.0):** 2.0.4 이후의 Single Edge 기능을 담은 기능 릴리스입니다.
변경 목록은 `CHANGELOG.md`의 2.1.0 항목을 따릅니다.
게시 전 검증에서 공개판 `make check-all` 47개 게이트, release·debug 빌드 각각의 C 단위 시험
2,495 PASS·22 SKIP과 audit startup 5 PASS, 브라우저 UI 시험 573 PASS, 패키지 검사, 격리된
Ubuntu 26.04 컨테이너의 설치·업그레이드·제거 lifecycle과 개발 호스트의 실제 업그레이드·되돌림을
확인했습니다. 컨테이너 검증에는 systemd 부팅, 실제 KVM 작업과 커널 BPF 적재가 포함되지 않습니다.

**2026-11-08 현재 소스 (2.0.4):** 2.0.3 공개 소스에 시작 볼륨 읽기 전용 보안 시정만 반영했습니다.
변경 목록은 `CHANGELOG.md`의 2.0.4 항목을 따릅니다.

**2026-10-25 현재 소스 (2.0.3):** 2.0.2 공개 소스에 컨테이너 명령 실행 보안 시정만 반영했습니다.
변경 목록은 `CHANGELOG.md`의 2.0.3 항목을 따릅니다.

**2026-10-16 현재 소스 (2.0.2):** 2.0.1 공개 소스에 REST 요청 처리 보안 시정 두 건만 반영했습니다.
변경 목록은 `CHANGELOG.md`의 2.0.2 항목을 따릅니다.

**2026-10-11 현재 소스 (2.0.1):** 2.0.0 공개 소스에 보안 시정만 반영했습니다.
변경 목록은 `CHANGELOG.md`의 2.0.1 항목, 결정은 ADR-0079를 따릅니다.

**2026-10-08 현재 소스:** AWS 수동 이관·S3 백업 제거와 공통 Job/WS·QGA·VM 시작·
Trace·VNC·OVA·자가 치유 보강을 포함합니다. 버전 2.0.0을 유지하며 기존 태그·패키지는
재발행하지 않았습니다. [당시 공개 인계](https://github.com/HardcoreMonk/purecvisor/blob/main/docs/operations/2026-10-08-public-source-refresh-handoff.md)의
검증 범위를 따르며, 아래 표는 과거 회차의 검증 기록입니다.

> **2026-10-02 소스 기준:** 공개 제품 로직을 개발 main에 맞췄다. NVRAM·첫 알림 쿨다운 시정은 유지하며 LXC는 ZFS 전용이다. 공개 전용 Btrfs와 개발 main 미병합 후속 브랜치는 포함하지 않는다. [정합화 인계](../docs/operations/2026-10-02-public-main-sync-handoff.md)에서 이번 검증을 구분한다.

#### 2026-09-15 검증 기록 (과거 회차, 2.0.0)

아래 표는 2.0.0 공개 소스 `22d6912` 회차의 기록입니다. 현재 `2.2.0`의 검증 결과가 아니며,
2.2.0 게시 전 검증은 위 2.2.0 항목을 따릅니다.

| 항목 | 공개 근거와 확인 범위 |
|---|---|
| 제품 소스 | [`22d6912`](https://github.com/HardcoreMonk/purecvisor/commit/22d6912fe5ee951cbc6c46e0da23e8f7971427a8): 호스트 첫 알림 쿨다운 수정, 공개 UI 표면·소스맵 부재 검사를 `check-all`·`dev-check`에 연결 |
| C·UI 시험 | 해당 소스 검증 회차에서 C 1,479 PASS·14 SKIP, audit startup 5 PASS, UI 512 PASS·0 SKIP |
| 계약·빌드 | 공개 `check-all` 40 PASS, UI 표면 반사실 11 PASS, debug·clean release 경고 0 |
| 메모리 검사 | ASan/UBSan 전체 통과(기본 leak detection 비활성), 지정 self-healing Valgrind 4 PASS·오류/definite/indirect/possible leak 0 |
| GPU 테스트 영상 | [`52587a5`](https://github.com/HardcoreMonk/purecvisor/commit/52587a5f4cc637d34a6a55a48c0cd76e93c800c9), [Pages 게시 성공](https://github.com/HardcoreMonk/purecvisor/actions/runs/34974574546): 4개 기능·16편, RTX 3070 Ti·Windows 11의 124초 영상 추가 |
| 영상 검증 범위 | 30초 Vulkan 렌더링과 종료·원복 확인. 장시간 안정성·모든 GPU·게임 성능 인증은 별도 |
| 문서 현행화 | 현재 소스·설정·게이트와 문서를 대조하고 문서 링크·사이트 build·공개 자산을 검증. 문서 변경은 제품 시험의 재실행이나 전체 감사 완료를 의미하지 않음 |

지정 Valgrind 통과는 이전 전체 검사 잔여를 닫는 근거가 아닙니다. 미검토 소스,
결과·자원 회수 계약, 전체 메모리 검사와 지원 환경 인증이 남아 있어 이 회차의 전체 감사는
`FAIL(미완료)`로 남겼습니다. 개발선의 22.7절 이력과 이 공개 소스 회차의 결과를 합산하지
않습니다. 지금 공개 제품 버전은 `2.2.0`이며 이 회차는 2.0.0 기준입니다.

---

> PureCVisor v2.2.0 Complete Guide — 22장 끝.
