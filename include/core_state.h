/**
 * @file core_state.h
 * @brief VM 오퍼레이션 잠금 상태 머신 (인메모리 GHashTable 버전 — 레퍼런스 구현)
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  VM에 대한 비동기 작업(시작, 종료, 삭제, 튜닝, 스냅샷)이 동시에
 *  실행되지 않도록 오퍼레이션 단위 잠금을 제공하는 상태 머신이다.
 *  GMainLoop 싱글 스레드 이벤트 루프의 특성을 활용하여 Mutex 없이
 *  GHashTable 기반 O(1) 조회로 동시 작업 충돌을 방어한다.
 *
 *  [레퍼런스 구현 vs 프로덕션 구현]
 *    이 파일은 인메모리(GHashTable) 기반의 간단한 레퍼런스 구현이다.
 *    프로덕션에서는 src/modules/core/vm_state.c가 SQLite WAL 기반으로
 *    확장된 버전을 사용한다. SQLite 버전은 다음을 추가로 제공한다:
 *      - 크래시 복구: 데몬 재시작 시 PID 확인으로 고아 락 자동 회수
 *      - 디스크 영속화: 데몬 크래시 후에도 락 상태 보존
 *      - 멀티스레드 안전: GMutex + BEGIN IMMEDIATE 트랜잭션
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  코어 모듈(src/modules/core/)에 속한다.
 *  디스패처 핸들러가 VM 작업을 시작하기 전에 lock_vm_operation()을
 *  호출하고, GTask 완료 콜백에서 unlock_vm_operation()을 호출한다.
 *
 *  [호출 흐름]
 *    클라이언트(pcvctl/REST) → dispatcher.c (RPC 수신)
 *        |
 *    handler_vm_*.c (핸들러)
 *        | lock_vm_operation() -- 잠금 획득 (이 파일의 함수)
 *        | → 성공: GTask 워커 스레드에서 비동기 작업 실행
 *        | → 실패: 에러 응답 반환 ("VM is already locked")
 *        |
 *    GTask 완료 (콜백 또는 워커 종료)
 *        | unlock_vm_operation() -- 잠금 해제
 *
 * ====================================================================
 *  주요 흐름
 * ====================================================================
 *  1. init_pending_state_machine(): 서버 부팅 시 1회 호출, 해시 테이블 생성
 *  2. lock_vm_operation(vm_id, op): 해시 테이블에 VM ID 존재 여부 확인
 *     - 없으면 삽입 후 TRUE 반환 (잠금 성공)
 *     - 있으면 FALSE 반환 + 에러 메시지 설정 (충돌 감지)
 *  3. unlock_vm_operation(vm_id): 해시 테이블에서 항목 제거 (잠금 해제)
 *
 * ====================================================================
 *  핵심 설계 패턴
 * ====================================================================
 *  - Lock-Free 설계: GMainLoop 메인 스레드 전용이므로 Mutex 불필요.
 *    [왜 Lock-Free가 가능한가?]
 *      GMainLoop는 단일 스레드에서 이벤트를 순차적으로 디스패치한다.
 *      lock_vm_operation()과 unlock_vm_operation()이 모두 메인 스레드
 *      콜백에서 호출되므로, 두 호출이 동시에 실행되는 것이 불가능하다.
 *      따라서 GHashTable 접근에 Mutex가 필요하지 않다.
 *
 *    [프로덕션(vm_state.c)에서는 왜 Mutex를 사용하는가?]
 *      GTask 워커 스레드에서 lock/unlock을 호출할 수 있기 때문이다.
 *      여러 RPC가 동시에 도착하면 각각의 GTask가 별도 스레드에서
 *      lock_vm_operation()을 호출할 수 있으므로 GMutex + SQLite
 *      BEGIN IMMEDIATE로 이중 보호한다.
 *
 *  - GINT_TO_POINTER 캐스팅: VmPendingOp 열거값을 GHashTable value로
 *    저장할 때 포인터로 캐스팅하여 별도 힙 할당 없이 사용한다.
 *    [왜 힙 할당을 피하는가?]
 *      GHashTable의 value는 gpointer(void*) 타입이다.
 *      정수 값을 저장하려면 g_new(gint, 1)로 힙에 할당할 수도 있지만,
 *      GINT_TO_POINTER(op)로 정수를 포인터 크기 정수로 캐스팅하면
 *      힙 할당 없이 값을 저장할 수 있다. 이는 GLib의 표준 관용구이다.
 *      조회 시 GPOINTER_TO_INT()로 역변환한다.
 *
 *  - Key 복제: g_hash_table_insert 시 g_strdup(vm_id)로 키를 복제하여
 *    원본 문자열 수명과 무관하게 동작한다.
 *    [메모리 자동 해제]
 *      g_hash_table_new_full()의 key_destroy_func에 g_free를 등록하므로
 *      g_hash_table_remove()나 g_hash_table_destroy() 시
 *      키 문자열이 자동으로 g_free된다.
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - 이 파일의 모든 함수는 반드시 메인 스레드(GMainLoop)에서만 호출할 것.
 *    워커 스레드(GTask)나 백그라운드 스레드에서 직접 호출하면
 *    Thread-safe를 보장할 수 없다.
 *  - GTask 완료 후 unlock 누락 시 해당 VM이 영구 잠금 상태가 된다.
 *    반드시 성공/실패 모든 경로에서 unlock_vm_operation()을 호출할 것.
 *  - 실제 운영 코드에서는 vm_state.c(src/modules/core/vm_state.c)가
 *    SQLite WAL 기반으로 확장된 버전을 사용한다. 이 파일은 학습/참조용이다.
 *
 * ====================================================================
 *  레퍼런스 구현 vs 프로덕션 구현 비교
 * ====================================================================
 *  | 특성              | core_state.h (이 파일)  | vm_state.c (프로덕션)    |
 *  |-------------------|------------------------|--------------------------|
 *  | 저장소            | 인메모리 GHashTable    | SQLite WAL DB            |
 *  | 크래시 복구       | 불가 (메모리 손실)     | 가능 (PID 확인 자동 회수)|
 *  | 스레드 안전성     | 메인 스레드 전용       | GMutex + SQLite 직렬화   |
 *  | 열거형            | 5가지 (MIGRATING 없음) | 8가지 (MIGRATING 포함)   |
 *  | 성능              | O(1) 해시 조회         | SQLite 쿼리 (~0.1ms)     |
 *  | 복잡도            | ~70 LOC                | ~374 LOC                 |
 */

#include <glib.h>
#include <stdio.h>

/*
 * [헤더 파일에 구현이 포함된 이유]
 *   이 파일은 레퍼런스/학습용 단일 파일 구현이다.
 *   프로덕션에서는 vm_state.h(선언)와 vm_state.c(구현)로 분리되어 있다.
 *   단일 파일에 선언+구현을 넣은 것은 학습 편의를 위한 것이므로,
 *   여러 .c 파일에서 #include하면 링크 시 심볼 중복 에러가 발생한다.
 *   (static 함수/변수로 선언되어 있어 하나의 컴파일 유닛에서만 사용 가능)
 */

// 실제 환경에서는 "core_state.h" 등에 아래 Enum을 선언하여 인클루드 하는 것을 권장합니다.
// #include "core_state.h"

// --- [헤더 파일에 들어갈 내용 임시 선언] ---

/**
 * VmPendingOp — VM에 대해 현재 진행 중인 비동기 작업의 종류
 *
 * [각 값의 의미와 사용 시점]
 *   VM_OP_STARTING: vm.start RPC 처리 중 (libvirt virDomainCreate 호출)
 *   VM_OP_STOPPING: vm.stop RPC 처리 중 (virDomainShutdown/Destroy)
 *   VM_OP_DELETING: vm.delete RPC 처리 중 (virDomainUndefine + zvol 삭제)
 *   VM_OP_TUNING:   vm.set_vcpu/set_memory 핫플러그 처리 중
 *   VM_OP_SNAPSHOT: vm.snapshot.create/rollback 처리 중 (ZFS 스냅샷)
 *
 * [프로덕션(vm_state.h)과의 차이]
 *   프로덕션 버전에는 추가로 다음이 정의되어 있다:
 *     VM_OP_NONE(0): 작업 없음
 *     VM_OP_CREATING(4): VM 생성 중
 *     VM_OP_MIGRATING(7): 라이브 마이그레이션 중
 */
typedef enum {
    VM_OP_STARTING = 1, /**< VM 부팅 진행 중 */
    VM_OP_STOPPING,     /**< VM 종료 진행 중 */
    VM_OP_DELETING,     /**< VM 삭제 진행 중 (되돌릴 수 없는 작업) */
    VM_OP_TUNING,       /**< vCPU/메모리 핫플러그 진행 중 */
    VM_OP_SNAPSHOT      /**< ZFS 스냅샷 생성/롤백 진행 중 */
} VmPendingOp;

/**
 * @brief Pending 상태 머신 초기화 (서버 부팅 시 1회 호출)
 * @note 중복 호출 시 경고를 출력하고 무시한다 (멱등성 보장)
 */
void init_pending_state_machine(void);

/**
 * @brief 특정 VM에 비동기 작업 잠금을 획득한다
 * @param vm_id 대상 VM의 UUID 또는 이름
 * @param op 수행하고자 하는 작업의 종류 (VmPendingOp)
 * @param out_error_msg 잠금 실패 시 동적 할당된 에러 문자열 (호출자가 g_free 해야 함)
 * @return TRUE=잠금 획득 성공, FALSE=이미 다른 작업이 진행 중
 */
gboolean lock_vm_operation(const gchar *vm_id, VmPendingOp op, gchar **out_error_msg);

/**
 * @brief 특정 VM의 비동기 작업 잠금을 해제한다
 * @param vm_id 대상 VM의 UUID 또는 이름
 * @note 잠금이 없는 상태에서 호출해도 안전 (경고 로그만 출력)
 */
void unlock_vm_operation(const gchar *vm_id);
// ------------------------------------------

/**
 * 전역 Pending Lock 테이블 (오직 Main Thread에서만 접근)
 *
 * [GHashTable 구조]
 *   Key:   gchar* — VM UUID 문자열 (g_strdup으로 복제, g_free로 자동 해제)
 *   Value: VmPendingOp 열거형 상수 (GINT_TO_POINTER로 캐스팅, 힙 할당 없음)
 *
 * [왜 GHashTable인가?]
 *   O(1) 평균 조회 시간으로 VM 수가 증가해도 성능이 저하되지 않는다.
 *   GLib의 GHashTable은 오픈 어드레싱 해시맵으로 구현되어 있다.
 *
 * [static 선언의 의미]
 *   이 변수는 이 컴파일 유닛(파일)에서만 접근 가능하다.
 *   외부 모듈은 lock/unlock 함수를 통해서만 간접 접근한다 (캡슐화).
 */
static GHashTable *vm_pending_locks = NULL;

/**
 * @brief Pending 상태 머신 초기화 (서버 부팅 시 1회 호출)
 *
 * [동작]
 *   GHashTable을 생성하여 vm_pending_locks에 할당한다.
 *   이미 초기화된 상태에서 재호출하면 경고를 출력하고 무시한다 (멱등성).
 *
 * [g_hash_table_new_full 파라미터]
 *   hash_func:   g_str_hash — 문자열 해시 함수 (djb2 변형)
 *   equal_func:  g_str_equal — 문자열 비교 함수 (strcmp 래퍼)
 *   key_destroy: g_free — 키 제거 시 자동 메모리 해제
 *   val_destroy: NULL — 값은 GINT_TO_POINTER이므로 해제 불필요
 *
 * [호출 시점]
 *   main.c의 초기화 시퀀스 2단계에서 호출된다.
 *   디스패처/핸들러보다 먼저 초기화되어야 한다.
 */
void init_pending_state_machine(void) {
    if (vm_pending_locks != NULL) {
        g_warning("Pending state machine is already initialized.");
        return;
    }

    /*
     * Key: g_strdup()으로 동적 할당되므로 소멸 시 g_free 호출 필요.
     * Value: 단순 정수형을 포인터로 캐스팅해 넣으므로 소멸자(NULL) 불필요.
     *
     * [메모리 관리 정리]
     *   g_hash_table_insert(table, g_strdup("vm-1"), GINT_TO_POINTER(VM_OP_STARTING))
     *     → "vm-1" 문자열이 힙에 복제됨
     *   g_hash_table_remove(table, "vm-1")
     *     → key_destroy_func(g_free)가 자동 호출되어 복제된 문자열 해제
     *     → val_destroy_func(NULL)이므로 값은 해제하지 않음 (포인터 크기 정수)
     */
    vm_pending_locks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_message("✅ [Core] Lock-Free VM Pending State Machine initialized.");
}

/**
 * @brief 특정 VM에 비동기 작업 락(Lock)을 획득합니다.
 *
 * [사용 예시]
 *   gchar *err = NULL;
 *   if (!lock_vm_operation("web-prod", VM_OP_STARTING, &err)) {
 *       // 이미 다른 작업 진행 중 — 에러 응답 전송
 *       send_error_response(err);
 *       g_free(err);
 *       return;
 *   }
 *   // ... vm.start 작업 수행 ...
 *   unlock_vm_operation("web-prod");
 *
 * [G_UNLIKELY 매크로]
 *   GCC의 __builtin_expect()로 확장되어 분기 예측을 최적화한다.
 *   vm_pending_locks가 NULL인 경우는 극히 드물므로(초기화 실패 시만)
 *   CPU 파이프라인이 정상 경로를 선호하도록 힌트를 제공한다.
 *
 * @param vm_id          대상 가상 머신의 UUID 또는 이름
 * @param op             수행하고자 하는 작업의 종류 (VmPendingOp)
 * @param out_error_msg  락 획득 실패 시, 에러 메시지가 g_strdup_printf로
 *                       동적 할당되어 반환됨 (호출자가 g_free 해야 함)
 * @return 락 획득 성공 시 TRUE, 이미 다른 작업이 진행 중이면 FALSE
 */
gboolean lock_vm_operation(const gchar *vm_id, VmPendingOp op, gchar **out_error_msg) {
    if (G_UNLIKELY(vm_pending_locks == NULL)) {
        if (out_error_msg) *out_error_msg = g_strdup("State machine not initialized.");
        return FALSE;
    }

    /*
     * 1단계: 이미 진행 중인 작업이 있는지 O(1) 속도로 확인
     *
     * [g_hash_table_contains의 시간 복잡도]
     *   평균 O(1), 최악 O(n) (해시 충돌 시)
     *   VM 수가 수십~수백 대 수준이므로 사실상 상수 시간
     */
    if (g_hash_table_contains(vm_pending_locks, vm_id)) {
        /*
         * [GPOINTER_TO_INT 캐스팅]
         *   g_hash_table_lookup()은 gpointer(void*)를 반환한다.
         *   저장 시 GINT_TO_POINTER로 캐스팅했으므로
         *   GPOINTER_TO_INT로 역변환하여 원래 정수값을 얻는다.
         */
        VmPendingOp current_op = (VmPendingOp)GPOINTER_TO_INT(g_hash_table_lookup(vm_pending_locks, vm_id));

        /* 2단계: Race Condition 감지 — 에러 메시지 생성 및 거절 */
        if (out_error_msg) {
            *out_error_msg = g_strdup_printf(
                "Conflict: VM '%s' is currently busy with operation code %d. Please try again later.",
                vm_id, current_op
            );
        }
        g_debug("🔒 [State] Lock denied for VM %s (Currently executing op: %d)", vm_id, current_op);
        return FALSE;
    }

    /*
     * 3단계: 진행 중인 작업이 없다면, 락(Lock) 점유
     *
     * [g_hash_table_insert의 키 소유권]
     *   g_strdup(vm_id)로 키를 복제하여 넣으므로
     *   원본 문자열(vm_id)의 생명주기와 독립된다.
     *   원본이 g_free되어도 해시 테이블 내의 키는 유효하다.
     *
     * [GINT_TO_POINTER(op)]
     *   VmPendingOp 열거형 값(1~5)을 포인터 크기 정수로 캐스팅한다.
     *   64비트 시스템에서 포인터는 8바이트이므로 정수 값을 안전하게 담을 수 있다.
     *   이 기법으로 gint 값 하나를 위해 힙 할당하는 오버헤드를 제거한다.
     */
    g_hash_table_insert(vm_pending_locks, g_strdup(vm_id), GINT_TO_POINTER(op));
    g_debug("🔓 [State] Lock acquired for VM %s (Op: %d)", vm_id, op);

    return TRUE;
}

/**
 * @brief 특정 VM의 비동기 작업 락(Lock)을 해제합니다.
 *
 * 비동기 작업(GTask 등)이 성공/실패로 끝났을 때 반드시 호출되어야 합니다.
 * 호출하지 않으면 해당 VM은 영구적으로 잠금 상태가 되어
 * 이후 어떤 작업도 수행할 수 없게 됩니다.
 *
 * [g_hash_table_remove의 동작]
 *   1. 키(vm_id)를 해시하여 해당 버킷을 찾음
 *   2. 버킷에서 키를 비교하여 엔트리를 찾음
 *   3. key_destroy_func(g_free)를 호출하여 키 메모리 해제
 *   4. 엔트리를 테이블에서 제거
 *   반환값: 제거 성공 시 TRUE, 키가 없으면 FALSE
 *
 * [에지 케이스 처리]
 *   락이 없는데 해제하려는 경우 경고 로그를 출력한다.
 *   이는 다음 상황을 감지하기 위함이다:
 *     - unlock을 이중으로 호출한 버그
 *     - lock 없이 unlock을 호출한 로직 오류
 *     - 다른 핸들러가 이미 unlock한 후 재호출
 *
 * @param vm_id 대상 가상 머신의 UUID 또는 이름
 */
void unlock_vm_operation(const gchar *vm_id) {
    if (G_UNLIKELY(vm_pending_locks == NULL)) return;

    /*
     * 테이블에서 Key 제거
     *
     * g_hash_table_new_full에 등록한 key_destroy_func(g_free)가
     * 자동 호출되어 g_strdup으로 복제했던 키 문자열의 메모리가 해제된다.
     * 이로써 메모리 누수가 방지된다.
     */
    gboolean removed = g_hash_table_remove(vm_pending_locks, vm_id);

    if (removed) {
        g_debug("🔓 [State] Lock released for VM %s", vm_id);
    } else {
        /* 락이 없는데 풀려고 시도하는 논리적 버그(Edge-case) 감지용 */
        g_warning("⚠️ [State] Attempted to unlock VM %s, but no lock was found.", vm_id);
    }
}
