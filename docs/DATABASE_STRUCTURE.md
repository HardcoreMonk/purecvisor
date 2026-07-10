# DB 구조 설명

> 기준 시점: 2026-05-05
> 대상: `purecvisor-single` Single Edge 저장소의 SQLite 기반 영속 상태

이 문서는 소스코드에 분산된 SQLite DB 구조를 한곳에서 설명한다. 실제 스키마의 단일 진실은 각 모듈의 `CREATE TABLE IF NOT EXISTS` 코드이며, 이 문서는 운영자, 아키텍트, 주니어 개발자가 같은 그림을 보고 대화할 수 있도록 만든 해설 문서다.

---

## 1. 전체 DB 지도

`purecvisor-single`은 외부 DBMS를 요구하지 않고 로컬 SQLite 파일 여러 개로 상태를 나눠 저장한다. 각 DB는 책임 영역이 다르며, 대부분 WAL 모드로 열린다.

| DB 파일 | 주 모듈 | 저장 내용 | 기본 경로 | 경로 오버라이드 |
|---|---|---|---|---|
| VM 상태 DB | `src/modules/core/vm_state.c` | VM별 진행 중 작업 락 | `/var/lib/purecvisor/vm_state.db` | `PCV_VM_STATE_DB_PATH`, `[daemon].db_path`, `PURECVISOR_DB_PATH` |
| Audit DB | `src/modules/audit/pcv_audit.c` | RPC/REST 감사 기록 | `/var/lib/purecvisor/pcv_audit.db` | `[audit].db_path` |
| Job Queue DB | `src/utils/pcv_job_queue.c` | fire-and-forget 작업 상태 | `/var/lib/purecvisor/pcv_jobs.db` | `PCV_JOBS_DB_PATH`, `[jobs].db_path` |
| RBAC DB | `src/modules/auth/pcv_rbac.c` | 사용자, 세션, API key, 쿼터 | `/var/lib/purecvisor/rbac.db` | 현재 `main.c`에서 고정 경로 전달 |
| Security DB | `src/modules/security/security_store.c`, `src/modules/security/hids_file_integrity.c` | Security Guard 이벤트, 설정, 승인 액션, HIDS 기준선 | `/var/lib/purecvisor/pcv_security.db` | `[security].db_path` |
| Security Group DB | `src/modules/network/security_group.c` | 방화벽 보안 그룹, 규칙, VM 바인딩 | `/var/lib/purecvisor/security_groups.db` | 현재 코드 상수 |
| Cloud Jobs DB | `src/modules/cloud/cloud_migration.c` | Cloud migration 작업 상태 | `/var/lib/purecvisor/cloud_jobs.db` | 현재 코드 상수 |

비개발자 관점에서 보면 이 구조는 하나의 거대한 DB가 아니라 “작은 장부 여러 권”에 가깝다. VM 작업 충돌 방지, 감사 기록, 사용자 인증, 보안 이벤트처럼 성격이 다른 기록을 분리해 장애 영향 범위와 백업 단위를 줄인다.

---

## 2. 공통 운영 원칙

### SQLite WAL 파일까지 함께 취급

대부분의 DB는 `PRAGMA journal_mode=WAL`로 열린다. WAL 모드에서는 기본 DB 파일 외에 다음 파일이 생길 수 있다.

```text
example.db
example.db-wal
example.db-shm
```

백업, 복사, 장애 분석 시에는 세 파일을 한 세트로 다뤄야 한다. 실행 중인 데몬의 DB 파일만 단독 복사하면 최근 쓰기가 빠질 수 있다.

### DB 파일 직접 수정 금지

운영 중 DB를 `sqlite3` CLI로 직접 수정하면 다음 문제가 생길 수 있다.

- 데몬의 메모리 상태와 DB 상태가 어긋난다.
- WAL에 남은 쓰기와 수동 변경이 충돌한다.
- audit, job completion, WebSocket 알림 같은 후속 처리 없이 상태만 바뀐다.

운영 변경은 RPC/API 또는 전용 복구 절차로 처리한다. 분석용 조회는 가능하지만, 변경 전에는 반드시 데몬 정지, 백업, 변경 계획, 복구 계획을 먼저 준비한다.

### 스키마 버전 테이블은 아직 없다

현재 DB들은 모듈 초기화 시 `CREATE TABLE IF NOT EXISTS`, `CREATE INDEX IF NOT EXISTS`, 일부 `ALTER TABLE ... ADD COLUMN`로 필요한 구조를 보장한다. 별도 `schema_version` 테이블 중심의 마이그레이션 프레임워크는 없다.

스키마를 바꿀 때는 다음을 함께 처리한다.

- 생성 SQL 또는 보정 SQL 수정
- 이 문서의 필드 설명 갱신
- 기존 DB를 가진 노드에서의 업그레이드 동작 확인
- 관련 단위/통합 테스트 실행
- 운영자가 백업할 DB 파일과 WAL 파일 명시

---

## 3. VM 상태 DB

### 목적

VM 상태 DB는 VM별로 동시에 실행되면 안 되는 작업을 막는 락 장부다. 예를 들어 같은 VM에 대해 `start`와 `delete`가 동시에 들어오면 데이터 손상이나 libvirt 상태 불일치가 생길 수 있으므로, 먼저 락을 얻은 작업만 진행한다.

### 위치와 초기화

| 항목 | 값 |
|---|---|
| 기본 경로 | `/var/lib/purecvisor/vm_state.db` |
| 코드 | `src/modules/core/vm_state.c`, `src/modules/core/vm_state.h` |
| 초기화 함수 | `init_pending_state_machine()` |
| 동시성 | `GMutex`, SQLite 트랜잭션, WAL |
| 복구 | 데몬 재시작 시 죽은 PID의 고아 락을 삭제 |

### 테이블: `vm_locks`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `vm_id` | `TEXT` | `PRIMARY KEY` | VM 이름 또는 UUID |
| `op_type` | `INTEGER` | `NOT NULL` | 진행 중인 작업 종류 |
| `pid` | `INTEGER` | `NOT NULL` | 락을 잡은 데몬 프로세스 PID |
| `locked_at` | `INTEGER` | `NOT NULL` | Unix timestamp 초 단위 |

`op_type` 값은 `VmPendingOp` 열거형을 따른다.

| 값 | 이름 | 의미 |
|---:|---|---|
| 0 | `VM_OP_NONE` | 작업 없음, DB에 저장하지 않는 상태 |
| 1 | `VM_OP_STARTING` | VM 시작 중 |
| 2 | `VM_OP_STOPPING` | VM 종료 중 |
| 3 | `VM_OP_DELETING` | VM 삭제 중 |
| 4 | `VM_OP_CREATING` | VM 생성 중 |
| 5 | `VM_OP_TUNING` | vCPU/메모리 핫플러그 중 |
| 6 | `VM_OP_SNAPSHOT` | 스냅샷 생성 또는 롤백 중 |
| 7 | `VM_OP_MIGRATING` | 라이브 마이그레이션 중 |

운영 의미는 단순하다. 이 DB에 행이 있다는 것은 “해당 VM은 지금 누군가 작업 중이므로 다른 위험 작업을 받으면 안 된다”는 뜻이다.

---

## 4. Audit DB

### 목적

Audit DB는 누가, 언제, 어떤 API/RPC를 호출했고 결과가 무엇이었는지 남기는 감사 장부다. 보안 분석, 장애 원인 분석, 운영 책임 추적에 사용된다.

### 위치와 초기화

| 항목 | 값 |
|---|---|
| 기본 경로 | `/var/lib/purecvisor/pcv_audit.db` |
| 코드 | `src/modules/audit/pcv_audit.c`, `src/modules/audit/pcv_audit.h` |
| 초기화 함수 | `pcv_audit_init()` |
| 호출 위치 | `src/main.c` |
| 동시성 | 비동기 큐와 worker thread, WAL |
| 보존 | 30일 초과 기록 삭제, 약 1GB DB 상한 |
| 장애 동작 | SQLite open 실패 시 file-only mode로 degrade |

### 테이블: `audit_log`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `id` | `INTEGER` | `PRIMARY KEY AUTOINCREMENT` | 감사 레코드 내부 ID |
| `ts` | `TEXT` | `NOT NULL` | 발생 시각 |
| `node` | `TEXT` | `NOT NULL` | 기록한 노드/호스트명 |
| `username` | `TEXT` | nullable | 인증된 사용자명 |
| `method` | `TEXT` | `NOT NULL` | RPC/API 메서드 |
| `target` | `TEXT` | nullable | 대상 VM, 리소스, 작업 ID 등 |
| `result` | `TEXT` | `NOT NULL` | 성공, 실패, accepted 등 결과 |
| `error_code` | `INTEGER` | nullable | 실패 시 표준 오류 코드 |
| `duration_ms` | `INTEGER` | nullable | 처리 시간 |
| `src_ip` | `TEXT` | nullable | REST 호출 출발 IP |

### 인덱스

| 인덱스 | 컬럼 | 용도 |
|---|---|---|
| `idx_audit_ts` | `ts` | 시간순 조회와 보존 기간 삭제 |
| `idx_audit_method` | `method` | 특정 메서드 호출 이력 조회 |

fire-and-forget RPC는 accepted 응답만으로 끝나지 않는다. ADR-0018에 따라 worker callback에서 실제 성공/실패 결과를 다시 `pcv_audit_log()`로 남겨야 한다.

---

## 5. Job Queue DB

### 목적

Job Queue DB는 오래 걸리는 비동기 작업의 현재 상태를 저장한다. 예를 들어 OVA export/import, VM 생성처럼 즉시 끝나지 않는 작업은 먼저 Job ID를 반환하고, 실제 진행률과 결과는 이 DB에 갱신한다.

### 위치와 초기화

| 항목 | 값 |
|---|---|
| 기본 경로 | `/var/lib/purecvisor/pcv_jobs.db` |
| 코드 | `src/utils/pcv_job_queue.c`, `src/utils/pcv_job_queue.h` |
| 초기화 함수 | `pcv_job_queue_init()` |
| 호출 위치 | `src/main.c` |
| 동시성 | `GMutex`, WAL |
| 장애 동작 | SQLite open 실패 시 job queue disabled 상태로 degrade |

### 테이블: `jobs`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `job_id` | `TEXT` | `PRIMARY KEY` | `job-...` 형태의 작업 ID |
| `type` | `TEXT` | `NOT NULL` | 작업 유형 |
| `target` | `TEXT` | nullable | 대상 VM 또는 리소스 |
| `status` | `INTEGER` | `DEFAULT 0` | 작업 상태 코드 |
| `progress` | `INTEGER` | `DEFAULT 0` | 0부터 100까지 진행률 |
| `detail` | `TEXT` | nullable | 현재 단계 설명 |
| `params` | `TEXT` | nullable | 요청 파라미터 JSON 문자열 |
| `result` | `TEXT` | nullable | 완료/실패 결과 JSON 문자열 |
| `created_at` | `INTEGER` | nullable | 생성 시각 Unix timestamp |
| `updated_at` | `INTEGER` | nullable | 마지막 갱신 시각 Unix timestamp |

`status` 값은 `PcvJobStatus` 열거형을 따른다.

| 값 | 이름 | 의미 |
|---:|---|---|
| 0 | `PCV_JOB_PENDING` | 생성 직후 대기 |
| 1 | `PCV_JOB_RUNNING` | 실행 중 |
| 2 | `PCV_JOB_COMPLETED` | 정상 완료 |
| 3 | `PCV_JOB_FAILED` | 실패 |
| 4 | `PCV_JOB_CANCELLED` | 취소 |

### 인덱스

| 인덱스 | 컬럼 | 용도 |
|---|---|---|
| `idx_jobs_status` | `status` | 실행 중/대기 중 작업 조회 |
| `idx_jobs_created` | `created_at DESC` | 최근 작업 목록 조회 |

비개발자 관점에서는 “진행 중 작업 현황판의 원천 데이터”다. UI나 API가 작업 진행률을 보여줄 때 이 DB를 기준으로 한다.

---

## 6. RBAC DB

### 목적

RBAC DB는 로그인 사용자, refresh session, API key, 사용자별 리소스 쿼터를 저장한다. 역할은 누적 모델이다. `ADMIN`은 `OPERATOR`와 `VIEWER` 권한을 포함하고, `OPERATOR`는 `VIEWER` 권한을 포함한다.

### 위치와 초기화

| 항목 | 값 |
|---|---|
| 기본 경로 | `/var/lib/purecvisor/rbac.db` |
| 코드 | `src/modules/auth/pcv_rbac.c`, `src/modules/auth/pcv_rbac.h` |
| 초기화 함수 | `pcv_rbac_init()` |
| 호출 위치 | `src/main.c` |
| 동시성 | `GMutex`, WAL |
| 초기 사용자 | 최소 1명의 admin 사용자 보장 |

### 테이블: `users`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `username` | `TEXT` | `PRIMARY KEY NOT NULL` | 로그인 ID |
| `password_hash` | `TEXT` | `NOT NULL` | salt를 반영한 비밀번호 hash |
| `salt` | `TEXT` | `NOT NULL` | 사용자별 salt |
| `role` | `INTEGER` | `NOT NULL DEFAULT 0` | `PcvRole` 값 |
| `tenant` | `TEXT` | nullable | 테넌트 격리 키 |
| `quota_vm_count` | `INTEGER` | `DEFAULT 0` | 생성 가능한 VM 수, 0은 무제한 |
| `quota_storage_gb` | `INTEGER` | `DEFAULT 0` | 스토리지 한도 GB, 0은 무제한 |

`quota_vm_count`, `quota_storage_gb`는 초기 `CREATE TABLE` 이후 `_ensure_quota_columns()`가 `ALTER TABLE`로 보장한다.

| 값 | 역할 | 의미 |
|---:|---|---|
| 0 | `PCV_ROLE_VIEWER` | 읽기 전용 |
| 1 | `PCV_ROLE_OPERATOR` | VM 운영 작업 가능 |
| 2 | `PCV_ROLE_ADMIN` | 사용자, 설정, 위험 작업 포함 전체 권한 |

### 테이블: `sessions`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `id` | `INTEGER` | `PRIMARY KEY AUTOINCREMENT` | 세션 내부 ID |
| `username` | `TEXT` | `NOT NULL` | 세션 소유 사용자 |
| `refresh_token_hash` | `TEXT` | `NOT NULL UNIQUE` | refresh token hash |
| `created_at` | `INTEGER` | `NOT NULL` | 생성 시각 Unix timestamp |
| `expires_at` | `INTEGER` | `NOT NULL` | 만료 시각 Unix timestamp |
| `revoked` | `INTEGER` | `NOT NULL DEFAULT 0` | 폐기 여부 |

### `sessions` 인덱스

| 인덱스 | 컬럼 | 용도 |
|---|---|---|
| `idx_sessions_hash` | `refresh_token_hash` | refresh token 검증 |
| `idx_sessions_user` | `username`, `revoked` | 사용자별 활성 세션 조회 |

### 테이블: `api_keys`

현재 코드에는 `api_keys` 테이블을 보장하는 경로가 두 개 있다.

첫 번째 경로는 사용자 소유 API key 구조를 만든다.

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `key_hash` | `TEXT` | `PRIMARY KEY` | API key hash |
| `username` | `TEXT` | `NOT NULL` | key 소유 사용자 |
| `description` | `TEXT` | `DEFAULT ''` | key 설명 |
| `created_at` | `INTEGER` | `NOT NULL` | 생성 시각 |
| `expires_at` | `INTEGER` | `NOT NULL` | 만료 시각 |
| `revoked` | `INTEGER` | `DEFAULT 0` | 폐기 여부 |

두 번째 경로는 머신 클라이언트 중심 구조를 보장한다.

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `key_hash` | `TEXT` | `PRIMARY KEY` | API key hash |
| `client_name` | `TEXT` | `NOT NULL` | 자동화 클라이언트 이름 |
| `role` | `INTEGER` | `NOT NULL DEFAULT 1` | key 권한 |
| `created_at` | `TEXT` | `NOT NULL DEFAULT datetime('now')` | 생성 시각 |
| `last_used_at` | `TEXT` | nullable | 마지막 사용 시각 |
| `revoked` | `INTEGER` | `NOT NULL DEFAULT 0` | 폐기 여부 |

주의: 같은 테이블명에 대해 두 구조가 코드에 공존하므로, API key 스키마를 변경할 때는 기존 운영 DB의 실제 컬럼, 호출 경로, 테스트를 먼저 확인해야 한다. 공개 문서나 운영 절차에서는 API key 평문을 저장한다고 표현하면 안 된다. 저장되는 값은 key hash다.

---

## 7. Security DB

### 목적

Security DB는 Security Guard의 이벤트, 설정, 승인 대기/처리된 보안 액션, HIDS 파일 무결성 기준선을 저장한다. 보안 화면과 보안 RPC가 읽는 핵심 상태 저장소다.

### 위치와 초기화

| 항목 | 값 |
|---|---|
| 기본 경로 | `/var/lib/purecvisor/pcv_security.db` |
| 코드 | `src/modules/security/security_store.c`, `src/modules/security/hids_file_integrity.c`, `src/modules/dispatcher/handler_security.c` |
| 초기화 방식 | security RPC 접근 시 lazy open |
| 경로 설정 | `[security].db_path` |
| 동시성 | `GMutex`, WAL |
| 장애 동작 | store open 실패 시 read path는 빈 JSON container 반환 |

### 테이블: `security_events`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `event_id` | `TEXT` | `PRIMARY KEY` | 보안 이벤트 ID |
| `timestamp` | `INTEGER` | `NOT NULL` | 발생 시각 Unix timestamp |
| `source` | `TEXT` | `NOT NULL` | 이벤트 생성 주체 |
| `type` | `TEXT` | `NOT NULL` | 이벤트 유형 |
| `severity` | `TEXT` | `NOT NULL` | 심각도 |
| `confidence` | `INTEGER` | `NOT NULL` | 신뢰도 점수 |
| `target_kind` | `TEXT` | `NOT NULL` | 대상 종류 |
| `target` | `TEXT` | `NOT NULL` | 대상 식별자 |
| `summary` | `TEXT` | `NOT NULL` | 요약 |
| `recommended_action` | `TEXT` | `NOT NULL` | 권장 조치 |
| `status` | `TEXT` | `NOT NULL` | open, action_pending 등 상태 |
| `evidence_json` | `TEXT` | `NOT NULL` | 증거 JSON 문자열 |
| `coalesce_key` | `TEXT` | `NOT NULL` | 중복 이벤트 묶음 키 |
| `occurrence_count` | `INTEGER` | `NOT NULL DEFAULT 1` | 같은 이벤트 반복 횟수 |
| `last_seen` | `INTEGER` | `NOT NULL` | 마지막 관측 시각 |
| `created_at` | `INTEGER` | `NOT NULL` | 최초 생성 시각 |

### `security_events` 인덱스

| 인덱스 | 컬럼 | 용도 |
|---|---|---|
| `idx_security_events_ts` | `timestamp DESC` | 최신 이벤트 조회 |
| `idx_security_events_sev` | `severity`, `status` | 심각도/상태별 필터 |
| `idx_security_events_coalesce_open` | `coalesce_key` | open/action_pending 이벤트 중복 억제 |

`idx_security_events_coalesce_open`은 부분 unique index다. 이미 열린 이벤트와 같은 `coalesce_key`가 들어오면 새 행을 무한히 만들지 않고 기존 이벤트의 반복 횟수와 마지막 관측 시각을 갱신하는 목적이다.

### 테이블: `security_config`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `key` | `TEXT` | `PRIMARY KEY` | 설정 키 |
| `value` | `TEXT` | `NOT NULL` | 설정 값 |
| `updated_at` | `INTEGER` | `NOT NULL` | 수정 시각 Unix timestamp |
| `updated_by` | `TEXT` | `NOT NULL` | 수정 주체 |

초기값으로 `enabled=false`가 `INSERT OR IGNORE`된다. 즉 최초 설치에서는 Security Guard가 명시적으로 켜지기 전까지 비활성 상태로 시작한다.

### 테이블: `security_actions`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `event_id` | `TEXT` | `PRIMARY KEY` | 연결된 보안 이벤트 ID |
| `action` | `TEXT` | `NOT NULL` | 수행할 조치 |
| `target_kind` | `TEXT` | `NOT NULL` | 조치 대상 종류 |
| `target` | `TEXT` | `NOT NULL` | 조치 대상 |
| `status` | `TEXT` | `NOT NULL` | 요청, 승인, 실패 등 처리 상태 |
| `ttl_sec` | `INTEGER` | `NOT NULL DEFAULT 3600` | 조치 유효 시간 |
| `expires_at` | `INTEGER` | `NOT NULL DEFAULT 0` | 만료 시각 |
| `requested_at` | `INTEGER` | `NOT NULL` | 요청 시각 |
| `decided_at` | `INTEGER` | `NOT NULL DEFAULT 0` | 승인/거부 결정 시각 |
| `decided_by` | `TEXT` | `NOT NULL DEFAULT ''` | 결정한 관리자 |
| `reason` | `TEXT` | `NOT NULL DEFAULT ''` | 결정 사유 |
| `job_id` | `TEXT` | `NOT NULL DEFAULT ''` | 비동기 조치 job ID |
| `error` | `TEXT` | `NOT NULL DEFAULT ''` | 실패 사유 |

### `security_actions` 인덱스

| 인덱스 | 컬럼 | 용도 |
|---|---|---|
| `idx_security_actions_status` | `status`, `requested_at DESC` | 상태별 최근 조치 조회 |

### 테이블: `file_baseline`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `path` | `TEXT` | `PRIMARY KEY` | 기준선을 잡은 파일 경로 |
| `sha256` | `TEXT` | `NOT NULL` | 파일 내용 hash |
| `size` | `INTEGER` | `NOT NULL` | 파일 크기 |
| `mode` | `INTEGER` | `NOT NULL` | 파일 권한/모드 |
| `mtime` | `INTEGER` | `NOT NULL` | 파일 수정 시각 |
| `refreshed_at` | `INTEGER` | `NOT NULL` | 기준선 갱신 시각 |
| `refreshed_by` | `TEXT` | `NOT NULL` | 기준선 갱신 주체 |

HIDS 스캔은 현재 파일 상태와 `file_baseline`을 비교한다. 기준선 갱신은 명시적 관리자 작업이어야 하며, 단순 스캔이 기준선을 자동 변경하면 침해 흔적을 지워버릴 수 있다.

---

## 8. Security Group DB

### 목적

Security Group DB는 VM에 적용할 네트워크 접근 정책을 저장한다. 데몬은 DB에 저장된 보안 그룹, 규칙, VM 바인딩을 읽어 nftables 규칙을 구성한다.

### 위치와 초기화

| 항목 | 값 |
|---|---|
| 기본 경로 | `/var/lib/purecvisor/security_groups.db` |
| 코드 | `src/modules/network/security_group.c` |
| 초기화 함수 | `_sg_db_init()` |
| 동시성 | WAL, `busy_timeout=3000` |
| 참조 무결성 | `PRAGMA foreign_keys=ON` |

### 테이블: `security_groups`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `name` | `TEXT` | `PRIMARY KEY` | 보안 그룹 이름 |
| `description` | `TEXT` | nullable | 설명 |

### 테이블: `sg_rules`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `id` | `INTEGER` | `PRIMARY KEY AUTOINCREMENT` | 규칙 내부 ID |
| `group_name` | `TEXT` | `NOT NULL`, FK | 소속 보안 그룹 |
| `direction` | `TEXT` | `NOT NULL DEFAULT 'ingress'` | ingress/egress 방향 |
| `protocol` | `TEXT` | `NOT NULL DEFAULT 'tcp'` | tcp/udp 등 프로토콜 |
| `port_start` | `INTEGER` | `NOT NULL DEFAULT 0` | 시작 포트 |
| `port_end` | `INTEGER` | `NOT NULL DEFAULT 0` | 끝 포트 |
| `source` | `TEXT` | `NOT NULL DEFAULT '0.0.0.0/0'` | 허용 출발지 CIDR |

`group_name`은 `security_groups(name)`을 참조하며, 그룹 삭제 시 규칙도 함께 삭제된다.

### 테이블: `sg_vm_bindings`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `group_name` | `TEXT` | `NOT NULL`, FK, 복합 PK | 보안 그룹 이름 |
| `vm_name` | `TEXT` | `NOT NULL`, 복합 PK | 적용 대상 VM 이름 |

`PRIMARY KEY(group_name, vm_name)`으로 같은 VM에 같은 그룹을 중복 연결하지 않는다. 그룹 삭제 시 바인딩도 함께 삭제된다.

---

## 9. Cloud Jobs DB

### 목적

Cloud Jobs DB는 `src/modules/cloud/cloud_migration.c`의 cloud migration 작업 상태를 저장한다. 현재 `purecvisor-single` 공개 범위 판단은 [PUBLIC_RELEASE_BOUNDARY.md](PUBLIC_RELEASE_BOUNDARY.md)를 우선해야 하며, 이 DB는 코드상 존재하는 기능별 상태 저장소로 이해한다.

### 위치와 초기화

| 항목 | 값 |
|---|---|
| 기본 경로 | `/var/lib/purecvisor/cloud_jobs.db` |
| 코드 | `src/modules/cloud/cloud_migration.c` |
| 초기화 함수 | `_cloud_db_init()` |
| 동시성 | WAL, `busy_timeout=3000` |
| 재시작 동작 | 완료되지 않은 작업을 `failed`로 표시 |

### 테이블: `cloud_jobs`

| 컬럼 | 타입 | 제약 | 의미 |
|---|---|---|---|
| `id` | `TEXT` | `PRIMARY KEY` | cloud 작업 ID |
| `type` | `TEXT` | `NOT NULL` | 작업 유형 |
| `vm_name` | `TEXT` | `NOT NULL` | 대상 VM |
| `status` | `TEXT` | `NOT NULL DEFAULT 'pending'` | pending, running, done, failed 등 |
| `progress` | `INTEGER` | `DEFAULT 0` | 진행률 |
| `error` | `TEXT` | nullable | 실패 사유 |
| `created_at` | `INTEGER` | nullable | 생성 시각 |
| `updated_at` | `INTEGER` | nullable | 갱신 시각 |

### 인덱스

| 인덱스 | 컬럼 | 용도 |
|---|---|---|
| `idx_cloud_jobs_vm_name` | `vm_name` | VM별 작업 조회 |
| `idx_cloud_jobs_status` | `status` | 상태별 작업 조회 |

---

## 10. 주요 상태 흐름

### 장시간 작업 흐름

```text
클라이언트 요청
  -> RPC/REST handler
  -> jobs 테이블에 job_id 생성
  -> accepted 응답 반환
  -> GTask worker 실행
  -> jobs.status/progress/result 갱신
  -> audit_log에 실제 최종 결과 기록
  -> WebSocket job completion broadcast
```

이 흐름에서 `jobs`는 진행률의 원천이고, `audit_log`는 책임 추적의 원천이다. 둘 중 하나만 갱신하면 UI, API, 감사 추적이 서로 다른 이야기를 하게 된다.

### VM 작업 충돌 방지 흐름

```text
VM 조작 요청
  -> vm_locks에서 VM별 락 획득 시도
  -> 성공하면 실제 libvirt/ZFS 작업 진행
  -> 성공/실패와 무관하게 unlock
  -> 데몬 크래시 후 재시작 시 고아 락 회수
```

운영자는 `vm_locks`를 “현재 작업 중인 VM 목록”으로 볼 수 있다. 다만 직접 삭제하기 전에 해당 PID가 실제로 죽었는지, 데몬의 자동 reconcile이 실패한 이유가 무엇인지 확인해야 한다.

### Security Guard 흐름

```text
탐지 또는 보안 RPC
  -> security_events에 이벤트 저장 또는 중복 이벤트 병합
  -> 필요 시 security_actions에 승인 대기 조치 저장
  -> 관리자 승인 후 비동기 worker 실행
  -> security_actions.status/job_id/error 갱신
  -> audit_log와 WebSocket completion으로 결과 전파
```

---

## 11. 스키마 변경 체크리스트

DB 구조를 바꾸는 변경은 단순 코드 수정이 아니라 운영 데이터 계약 변경이다. 아래 순서로 처리한다.

1. 변경 대상 DB와 테이블을 이 문서에서 먼저 찾는다.
2. 실제 생성/보정 SQL이 있는 C 파일을 수정한다.
3. 기존 DB 파일을 가진 노드에서 업그레이드해도 실패하지 않는지 확인한다.
4. `CREATE TABLE IF NOT EXISTS`만으로 해결되지 않는 변경이면 명시적 migration 또는 보정 로직을 둔다.
5. WAL 백업 단위와 rollback 방법을 운영 문서에 적는다.
6. API/RPC 응답 JSON이 바뀌면 UI, 테스트, 문서를 함께 갱신한다.
7. `git diff --check`와 관련 테스트를 실행한다.

권장 확인 명령:

```bash
rg -n "CREATE TABLE IF NOT EXISTS|CREATE INDEX IF NOT EXISTS|PRAGMA journal_mode|ALTER TABLE" src
git diff --check
make test
```

변경 영역별 추가 검증 예시는 다음과 같다.

| 변경 영역 | 추가 확인 |
|---|---|
| RBAC DB | `make check-rbac`, 인증/권한 관련 통합 테스트 |
| Audit DB | ADR-0018 worker-result audit 누락 여부, `scripts/check_audit_placement.py` |
| Job Queue DB | `tests/test_job_queue.c`, jobs RPC 조회 흐름 |
| VM 상태 DB | VM start/stop/delete/create 충돌 방지 테스트 |
| Security DB | `security.*` RPC, HIDS baseline refresh/scan |
| Security Group DB | nftables restore와 VM binding 동작 |

---

## 12. 소스 확인 위치

| 관심사 | 먼저 볼 파일 |
|---|---|
| 기본 DB 경로와 설정 로딩 | `src/utils/pcv_config.h`, `src/utils/pcv_config.c` |
| 데몬 시작 시 DB 초기화 순서 | `src/main.c` |
| VM 작업 락 | `src/modules/core/vm_state.c`, `src/modules/core/vm_state.h` |
| 감사 로그 | `src/modules/audit/pcv_audit.c`, `src/modules/audit/pcv_audit.h` |
| 비동기 작업 큐 | `src/utils/pcv_job_queue.c`, `src/utils/pcv_job_queue.h` |
| 사용자/세션/API key | `src/modules/auth/pcv_rbac.c`, `src/modules/auth/pcv_rbac.h` |
| Security Guard 상태 | `src/modules/security/security_store.c`, `src/modules/dispatcher/handler_security.c` |
| HIDS 파일 기준선 | `src/modules/security/hids_file_integrity.c` |
| 보안 그룹 | `src/modules/network/security_group.c` |
| Cloud migration 작업 | `src/modules/cloud/cloud_migration.c` |
