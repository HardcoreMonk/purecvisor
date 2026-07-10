/**
 * @file vm_state.h
 * @brief SQLite WAL 기반 VM 오퍼레이션 락 — 동시 작업 방지 + 크래시 복구
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  VM에 대한 비동기 작업이 동시에 실행되는 것을 방지하는 배타적 잠금 시스템입니다.
 *  SQLite WAL(Write-Ahead Log) 모드를 사용하여 디스크에 락 상태를 영속화합니다.
 *  데몬 크래시 후 재시작 시 PID 확인으로 고아 락을 자동 회수합니다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  디스패처(dispatcher.c)가 RPC를 수신하면 핸들러(handler_vm_*.c)로 전달합니다.
 *  핸들러는 실제 작업을 수행하기 전에 lock_vm_operation()으로 잠금을 획득하고,
 *  작업 완료(성공/실패) 후 unlock_vm_operation()으로 잠금을 해제합니다.
 *
 *  [호출 흐름]
 *    클라이언트 → dispatcher.c → handler_vm_start.c
 *      → lock_vm_operation("web-prod", VM_OP_STARTING, &err)
 *      → 성공: GTask 워커 스레드에서 virDomainCreate() 실행
 *      → 완료: unlock_vm_operation("web-prod")
 *
 * ====================================================================
 *  사용 패턴
 * ====================================================================
 *   gchar *err = NULL;
 *   if (!lock_vm_operation(vm_name, VM_OP_STARTING, &err)) {
 *       // 이미 다른 작업 진행 중 — err에 사유가 설정됨
 *       // 예: "VM 'web-prod' is already locked (op: STOPPING, pid: 12345)"
 *       send_error_response(rpc_id, -32000, err);
 *       g_free(err);
 *       return;
 *   }
 *   // ... 작업 수행 (GTask 비동기) ...
 *   unlock_vm_operation(vm_name);  // 반드시 모든 경로에서 호출!
 *
 * ====================================================================
 *  include/core_state.h와의 비교
 * ====================================================================
 *  core_state.h는 인메모리(GHashTable) 기반의 레퍼런스 구현입니다.
 *  이 파일(vm_state.h)은 SQLite 기반의 프로덕션 구현으로 다음을 추가합니다:
 *    - 디스크 영속화: 데몬 크래시 후에도 락 상태 보존
 *    - 크래시 복구: 재시작 시 PID 확인으로 고아 락 자동 회수
 *    - 멀티스레드 안전: GMutex + BEGIN IMMEDIATE 트랜잭션
 *    - 확장된 열거형: VM_OP_NONE, VM_OP_CREATING, VM_OP_MIGRATING 추가
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - lock_vm_operation은 GMutex + SQLite 트랜잭션으로 이중 보호됨
 *  - 반드시 작업 완료 후 unlock_vm_operation을 호출해야 함 (미해제 시 영구 락)
 *  - 데몬 크래시 시에는 재시작 시 PID 확인으로 자동 회수됨
 *  - DB 초기화 실패 시 락 없이 진행 (인메모리 폴백, 경고만 출력)
 *
 * ====================================================================
 *  DB 상세
 * ====================================================================
 *  경로   : /var/lib/purecvisor/vm_state.db (pcv_config_get_db_path로 오버라이드 가능)
 *  모드   : WAL (Write-Ahead Log) — 동시 읽기 성능 향상, 크래시 안전성
 *  테이블 : vm_locks(vm_id TEXT PK, op_type INT, pid INT, locked_at INT)
 *  인덱스 : vm_id가 PRIMARY KEY이므로 자동 인덱스 (B-tree)
 */
/* src/modules/core/vm_state.h
 *
 * Sprint B-1: VM 상태 영속화 레이어
 */

#ifndef PURECVISOR_VM_STATE_H
#define PURECVISOR_VM_STATE_H

#include <glib.h>

G_BEGIN_DECLS

/* ── 오퍼레이션 타입 ──────────────────────────────────────── */

/**
 * VmPendingOp — VM에 대해 현재 진행 중인 비동기 작업의 종류
 *
 * [각 값의 사용 시점과 핸들러]
 *   VM_OP_NONE(0):      작업 없음 — DB에 기록되지 않음
 *   VM_OP_STARTING(1):  handler_vm_start.c → virDomainCreate() + CPU 튜닝
 *   VM_OP_STOPPING(2):  handler_vm_lifecycle.c → virDomainShutdown/Destroy
 *   VM_OP_DELETING(3):  handler_vm_lifecycle.c → virDomainUndefine + zvol 삭제
 *   VM_OP_CREATING(4):  dispatcher.c(인라인) → virt-install + ZFS zvol 생성
 *   VM_OP_TUNING(5):    handler_vm_hotplug.c → virDomainSetVcpus/SetMemory
 *   VM_OP_SNAPSHOT(6):  handler_snapshot.c → ZFS 스냅샷 create/rollback
 *   VM_OP_MIGRATING(7): handler_cluster.c → virDomainMigrate (라이브 마이그레이션)
 *
 * [왜 정수값이 0부터 시작하는가?]
 *   DB의 op_type 컬럼에 정수로 저장되며, lock_vm_operation()의
 *   에러 메시지에서 문자열로 변환할 때 배열 인덱스로 사용됩니다.
 *
 * [동시 실행이 허용되지 않는 이유]
 *   예를 들어 VM_OP_STARTING과 VM_OP_DELETING이 동시에 실행되면,
 *   하나는 VM을 부팅하려 하고 다른 하나는 삭제하려 하여
 *   정의되지 않은 동작(UB)이 발생합니다.
 *   이 잠금으로 한 번에 하나의 작업만 허용합니다.
 */
typedef enum {
    VM_OP_NONE      = 0, /**< 실행 중인 작업 없음 (Free 상태) */
    VM_OP_STARTING  = 1, /**< VM 부팅 및 리소스 튜닝 진행 중 */
    VM_OP_STOPPING  = 2, /**< VM 정상 종료(ACPI) 또는 강제 종료 진행 중 */
    VM_OP_DELETING  = 3, /**< VM 리소스 회수 및 삭제 진행 중 (되돌릴 수 없음) */
    VM_OP_CREATING  = 4, /**< VM 생성 진행 중 (zvol + virt-install) */
    VM_OP_TUNING    = 5, /**< 라이브 리소스(vCPU, Memory) 핫플러그 진행 중 */
    VM_OP_SNAPSHOT  = 6, /**< ZFS 스냅샷 생성/롤백 진행 중 */
    VM_OP_MIGRATING = 7, /**< 다른 호스트 노드로 라이브 마이그레이션 진행 중 */
} VmPendingOp;

/* ── 생명주기 API ─────────────────────────────────────────── */

/**
 * @brief SQLite DB를 열고 WAL 모드 활성화, 테이블 생성, 고아 락 회수를 수행합니다.
 *        main.c의 초기화 시퀀스 2단계에서 단 1회 호출해야 합니다.
 *
 * [내부 동작]
 *   1. GMutex 초기화
 *   2. DB 디렉토리 생성 (/var/lib/purecvisor/)
 *   3. SQLite DB 열기 (pcv_config_get_db_path())
 *   4. PRAGMA journal_mode=WAL + busy_timeout=5000
 *   5. vm_locks 테이블 CREATE IF NOT EXISTS
 *   6. Reconcile: 죽은 PID의 고아 락 자동 삭제
 *
 * @note DB 열기 실패 시 g_db=NULL로 남으며, lock/unlock은 graceful degradation.
 */
void init_pending_state_machine(void);

/**
 * @brief VM에 대한 오퍼레이션 락을 획득합니다.
 *        이미 락이 걸려 있으면 FALSE를 반환하고 err_msg에 이유를 설정합니다.
 *
 * [내부 동작]
 *   1. GMutex 획득
 *   2. BEGIN IMMEDIATE 트랜잭션 (쓰기 락 즉시 획득)
 *   3. SELECT로 기존 락 존재 여부 확인
 *   4. 기존 락이 있으면 PID 생존 확인 → 죽은 PID면 덮어쓰기
 *   5. INSERT로 새 락 기록
 *   6. COMMIT
 *
 * @param vm_id   대상 VM 식별자 (UUID 또는 이름, 예: "web-prod")
 * @param op_type VmPendingOp 값 (VM_OP_STARTING 등)
 * @param err_msg 실패 시 g_strdup_printf로 할당된 에러 문자열
 *                (호출자가 g_free 해야 함, NULL 전달 가능 — 에러 메시지 불필요 시)
 * @return TRUE = 락 획득 성공, FALSE = 이미 락 존재 또는 DB 에러
 */
[[nodiscard]] gboolean lock_vm_operation(const gchar *vm_id, gint op_type, gchar **err_msg);

/**
 * @brief VM 오퍼레이션 락을 해제합니다.
 *
 * [멱등성]
 *   vm_id에 해당하는 락이 없어도 에러를 발생시키지 않습니다.
 *   DELETE FROM은 해당 행이 없으면 아무 일도 하지 않습니다.
 *   따라서 이중 호출에도 안전합니다.
 *
 * @param vm_id 대상 VM 식별자 (NULL이면 즉시 반환)
 */
void unlock_vm_operation(const gchar *vm_id);

/**
 * @brief 데몬 종료 시 DB 연결을 닫고 뮤텍스를 해제합니다.
 *
 * [호출 시점]
 *   main.c의 cleanup 블록에서, 모든 워커 스레드가 종료된 후에 호출합니다.
 *   이 시점 이후로는 lock_vm_operation/unlock_vm_operation을 호출하면 안 됩니다.
 *
 * @note WAL 모드에서 sqlite3_close()는 WAL 파일을 체크포인트하고 정리합니다.
 */
void shutdown_pending_state_machine(void);

/**
 * @brief 현재 보유 중인 VM 락 수를 반환한다 (Prometheus 메트릭용).
 *
 * vm_locks 테이블의 행 수를 SELECT COUNT(*)로 조회한다.
 * DB 미초기화 시 0을 반환한다.
 *
 * @return 현재 활성 락 수
 */
gint pcv_vm_state_get_lock_count(void);

/**
 * @brief 만료된 잠금을 정리합니다.
 *
 * locked_at + LOCK_EXPIRY_SEC(기본 3600초)보다 오래된 잠금을 자동 삭제합니다.
 * GMainLoop 타이머(300초 주기)로 호출되어 핸들러 크래시 시 교착을 방지합니다.
 *
 * @return 정리된 잠금 수
 */
gint pcv_vm_state_cleanup_expired(void);

/* ── VM 런타임 상태 열거형 (libvirt 상태 매핑) ────────────── */

/**
 * PcvVmRuntimeState — libvirt 도메인 상태를 PureCVisor 내부 열거형으로 매핑
 *
 * VmPendingOp(오퍼레이션 락)과는 별도로, VM의 실시간 런타임 상태를
 * 조회하기 위한 열거형이다. virDomainGetState() 결과를 변환한다.
 */
typedef enum {
    PCV_VM_UNKNOWN   = 0, /**< 상태 조회 실패 또는 알 수 없는 상태 */
    PCV_VM_RUNNING   = 1, /**< 실행 중 (VIR_DOMAIN_RUNNING) */
    PCV_VM_STOPPED   = 2, /**< 정지됨 (VIR_DOMAIN_SHUTOFF) */
    PCV_VM_PAUSED    = 3, /**< 일시중지 (VIR_DOMAIN_PAUSED / PMSUSPENDED) */
    PCV_VM_MIGRATING = 4, /**< 마이그레이션 진행 중 */
    PCV_VM_ERROR     = 5, /**< 에러 상태 */
} PcvVmRuntimeState;

/**
 * @brief VM의 실시간 런타임 상태를 libvirt에서 조회한다.
 *
 * @param vm_name VM 이름
 * @return PcvVmRuntimeState
 */
PcvVmRuntimeState pcv_vm_state_get_runtime(const gchar *vm_name);

/**
 * @brief PcvVmRuntimeState를 문자열로 변환한다.
 *
 * @param state VM 런타임 상태
 * @return 상태 문자열 (정적 문자열 — 해제 불필요)
 */
const gchar *pcv_vm_state_runtime_str(PcvVmRuntimeState state);

G_END_DECLS

#endif /* PURECVISOR_VM_STATE_H */
