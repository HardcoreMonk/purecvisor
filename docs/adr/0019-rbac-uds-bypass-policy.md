# ADR-0019 — RBAC UDS 우회 정책 (디스패처 method policy)

- **상태**: Accepted / Approved (Option C 구현 완료, 2026-04-12; 사용자 승인, 2026-04-26)
- **일자**: 2026-04-12
- **관련**: ADR-0001 (단일 데몬), ADR-0014 (CSRF 제거), ADR-0018 (audit 무결성)
- **트리거**: B2 cloud 감사 WARN5 + B5 스냅샷 감사 CRIT3 — 동일 이슈가 2건의
  수직 슬라이스에서 독립적으로 발견됨. 디스패처 핸들러가 caller의 role을
  받지 않아 UDS 직접 연결 시 RBAC가 우회됨.

## 컨텍스트

PureCVisor의 RBAC는 두 계층에 분산되어 있다:

1. **REST 계층** (`src/api/rest_server.c` + `rest_middleware.c`):
   - JWT Bearer 토큰 검증 → subject(username) 추출
   - 핸들러 라우팅 전에 RBAC DB에서 현재 role 조회 후 검증
   - viewer가 vm.delete 호출 시 403 거부

2. **UDS 계층** (`src/api/uds_server.c` → `dispatcher.c::pure_dispatcher_dispatch`):
   - JSON-RPC 메시지 직접 파싱
   - dispatcher pre-route RBAC 미들웨어에서 method policy 검사
   - UDS direct 호출은 소켓 권한으로 격리되는 admin으로 간주

과거에는 UDS 소켓에 접근할 수 있는 모든 프로세스가 `vm.delete`,
`vm.snapshot.delete_all`, `cloud.import` 같은 destructive RPC를 RBAC 없이
실행할 수 있었다. 현재는 `dispatcher.c::g_method_policies`와
`pcv_dispatcher_check_rbac()`가 REST/UDS 공통 2차 방어선이다.

### 현재 가정: "UDS = admin"

기본 systemd unit은 UDS를 다음 권한으로 생성한다:
```
/var/run/purecvisor/daemon.sock  srw-rw---- root root
```

즉, **UDS 접근 = root = admin** 가정. 이 가정에 의존해 dispatcher 핸들러는
RBAC를 생략한다.

### 가정의 한계

1. **legacy socket activation 시 권한 변형 가능** — 과거 `purecvisord.socket`의
   `SocketMode=` 값이 0660보다 느슨해질 수 있음
2. **plugin/grpc/IPC 확장 시** — gRPC 핸들러나 in-process 플러그인이
   dispatcher를 직접 호출할 때 caller context가 없음
3. **컨테이너 마운트** — 컨테이너에서 호스트 UDS를 마운트하면 비특권 사용자가
   admin 권한으로 dispatcher 호출 가능
4. **forensics 추적성** — audit 로그의 username 필드가 항상 "system"이라
   "누가 호출했는지" 구분 불가
5. **REST 우회 의존성** — REST 미들웨어 버그로 RBAC를 우회하는 방법이 발견되면,
   2차 방어선이 없음

## 결정 (Accepted — Option C 구현 완료)

### Option A: 현재 모델 유지 (status quo)

- **UDS = admin** 가정을 ADR로 명시
- systemd unit의 `SocketMode=0600` + `SocketUser=root` 강제
- daemon.conf에 `[security] uds_admin_only = true` 검증
- 컨테이너 마운트 사용 시 보안 경고 문서화
- **장점**: 코드 변경 없음
- **단점**: 다층 방어 없음, plugin/gRPC 확장 시 재검토 필요

### Option B: 디스패처 핸들러 시그니처에 caller 컨텍스트 추가

- 핸들러 시그니처를 `(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection, PcvCallContext *call_ctx)`로 변경
- `PcvCallContext` 구조체:
  ```c
  typedef struct {
      const gchar *username;     /* JWT에서 추출, UDS=시 "uds-local" */
      PcvRole      role;         /* ADMIN/OPERATOR/VIEWER */
      const gchar *src_ip;       /* REST=원격IP, UDS="local" */
      gboolean     is_uds;       /* TRUE면 UDS 직접 연결 */
  } PcvCallContext;
  ```
- 200+ 핸들러 시그니처 수정 필요 (광범위)
- dispatcher가 caller_ctx를 빌드하여 전달
- 각 핸들러는 원하는 RBAC 정책을 명시 (`require_admin(call_ctx)`)
- **장점**: 다층 방어, plugin/gRPC 확장 가능
- **단점**: 200+ 핸들러 수정, 1~2주 작업

### Option C: 메서드명 기반 RBAC 미들웨어 (절충)

- 디스패처 라우팅 직전에 메서드명 → 필요 role 매핑 테이블 검사
- 매핑 테이블:
  ```c
  static const PcvMethodPolicy g_method_policies[] = {
      { "vm.delete",                PCV_ROLE_OPERATOR }, // VM action은 owner metadata 일치 시만 허용
      { "vm.snapshot.delete_all",   PCV_ROLE_ADMIN },
      { "vm.create",                PCV_ROLE_OPERATOR },
      { "vm.list",                  PCV_ROLE_VIEWER },
      ...
  };
  ```
- caller role은 connection metadata 또는 REST가 주입한 내부 params로 전달
- VM 단일 대상 operator action은 base role을 `OPERATOR`로 열고, 디스패처가 libvirt metadata의 VM 생성자와 인증 주체를 비교해 최종 허용한다. owner metadata가 없거나 다른 사용자가 만든 VM이면 operator는 거부된다.
- `vm.create`는 생성 시 owner metadata를 기록한다. `vm.clone`은 source VM owner가 호출자와 일치해야 하며, 복제 XML metadata를 통해 owner가 유지된다. 준비된 템플릿 확인 또는 libguestfs 기반 guest reset 요구사항은 ADR-0023이 우선한다.
- 핸들러 시그니처 변경 없음
- **장점**: 200+ 핸들러 수정 없음, 다층 방어
- **단점**: 정책이 핸들러 코드와 분리되어 동기화 어려움 (RPC 추가 시 잊을 수 있음)

## 구현 상태

**Option C** (메서드 매핑 미들웨어)를 채택했다. 이유:
1. 핸들러 코드 비침해성
2. 200+ 핸들러 수정 회피
3. 정책이 한 곳에 집중되어 audit 가능
4. `make check-rbac`와 pre-commit hook으로 "신규 RPC 등록 시 매핑 누락"과 "operator VM owner-scope 정책 계약 회귀" 검증 가능
   (`scripts/check_rbac_policies.py` — `g_rpc_routes` ↔ `g_method_policies` diff + 정책 계약 검사)

구현 위치:

- `src/api/dispatcher.c::g_method_policies`
- `src/api/dispatcher.c::pcv_dispatcher_check_rbac()`
- `src/api/dispatcher.c::dispatcher_init_routes()`의 eager policy map 초기화
- `Makefile::check-rbac`
- `scripts/check_rbac_policies.py` 정적 검증

정적 검증은 다음 두 축을 모두 검사한다.

- destructive RPC가 `g_method_policies`에 명시되어 있는지 확인한다.
- `device.nic.attach`/`device.nic.detach`처럼 operator에게 허용되지만 VM owner-scope를 반드시 통과해야 하는 메서드가 `OPERATOR` 권한과 owner-scope 대상성을 유지하는지 확인한다.

## 승인 이력

- 2026-04-12: Option C를 구현 기준으로 채택했다.
- 2026-04-26: 사용자 승인 완료. 승인 범위는 현재 구현된 메서드명 기반 RBAC pre-route 미들웨어, UDS direct admin fallback 운영 가정, destructive RPC 정책 매핑 정적 검증 유지 규칙이다.
- 2026-04-28: VM NIC hotplug 정책 회귀 방지를 위해 operator VM owner-scope 정책 계약 검사를 정적 게이트와 pre-commit에 추가했다.

## 영향

### 긍정 (Option C)
- vm.snapshot.delete_all UDS 우회 차단 (B5-C3)
- cloud.* RBAC 강제 (B2-WARN5)
- 다층 방어 — REST + UDS 모두 RBAC 적용
- destructive RPC가 REST/UDS 양쪽에서 동일 role policy를 통과해야 함

### 비용
- `connection_metadata` GHashTable 도입
- 매 RPC마다 정책 lookup (O(1) 해시)
- 신규 RPC 추가 시 정책 등록 의무화 (자동 검증)

### 리스크
- 기존 UDS-only 사용자 (예: pcvctl) 호환성 — 기본 role 정의 필요
- pcvctl이 어떻게 인증하는지 — 현재 인증 없음. UDS = admin 가정.
- 마이그레이션: 기존 pcvctl 사용자를 위한 임시 fallback (e.g. `daemon.conf [security] uds_default_role = admin`)

## 후속 작업

1. **완료**: Option C 채택 및 구현.
2. **완료**: B5-C3 + B2-WARN5 임시 경고를 dispatcher RBAC 정책으로 영구화.
3. **완료**: 신규 RPC 추가 시 RBAC 정책 매핑 필수 규칙을 에이전트 문서에 반영.
4. **완료**: `make check-rbac`가 destructive RPC 매핑 누락과 operator VM owner-scope 정책 계약 회귀를 검출.
5. **유지**: UDS socket 권한이 root/admin 가정에서 벗어나면 Option B 수준의 caller context 전달을 재검토한다.
