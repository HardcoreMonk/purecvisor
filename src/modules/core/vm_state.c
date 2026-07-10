/**
 * @file vm_state.c
 * @brief SQLite WAL 기반 VM 상태 머신 — 원자적 상태 전이 + 크래시 복구
 *
 * ====================================================================
 *  아키텍처에서의 위치
 * ====================================================================
 *  handler_vm_*.c → lock_vm_operation() → (작업 수행) → unlock_vm_operation()
 *
 *  VM에 대한 동시 오퍼레이션(시작+삭제 등)을 방지하는 분산 락입니다.
 *  SQLite에 기록하므로 데몬이 크래시해도 상태가 보존됩니다.
 *
 *  [다른 모듈과의 관계]
 *    - vm_state.h: 이 파일의 공개 API 선언
 *    - include/core_state.h: 인메모리 GHashTable 버전 (레퍼런스 구현, 비프로덕션)
 *    - main.c: init_pending_state_machine() 호출 (초기화 2단계)
 *    - handler_vm_start.c, handler_vm_lifecycle.c, handler_snapshot.c 등:
 *      lock_vm_operation()/unlock_vm_operation() 호출자
 *    - src/utils/pcv_config.h: pcv_config_get_db_path()로 DB 경로 조회
 *
 * ====================================================================
 *  핵심 설계
 * ====================================================================
 *  [SQLite WAL 모드]
 *    WAL(Write-Ahead Log)은 SQLite의 저널 모드 중 하나입니다.
 *    기존 롤백 저널과 달리 변경사항을 별도 WAL 파일에 추가(append)합니다.
 *    장점:
 *      - 동시 읽기 성능 향상: 쓰기 중에도 읽기가 가능 (MVCC와 유사)
 *      - 크래시 안전성: WAL 파일에서 커밋된 트랜잭션만 복구
 *      - 쓰기 성능: 순차 쓰기(sequential write)로 랜덤 I/O 최소화
 *    주의:
 *      - WAL 파일이 커질 수 있음 (체크포인트 필요)
 *      - NFS 등 네트워크 파일시스템에서는 사용 불가
 *
 *  [PID 기반 고아 락 감지]
 *    데몬이 크래시하면 DB에 이전 PID의 락이 남아있습니다.
 *    재시작 시 kill(pid, 0)으로 해당 프로세스가 살아있는지 확인합니다.
 *    죽은 PID의 락은 자동 삭제하여 "영구 잠금" 상태를 방지합니다.
 *
 *  [BEGIN IMMEDIATE 트랜잭션]
 *    일반 BEGIN: 첫 SELECT까지 쓰기 락을 미룸 → TOCTOU 경쟁 조건 가능
 *    BEGIN IMMEDIATE: 즉시 RESERVED 락 획득 → 다른 writer 차단
 *    "읽기 → 판단 → 쓰기" 사이에 다른 스레드가 끼어드는 것을 방지합니다.
 *
 *  [busy_timeout=5000]
 *    다른 스레드가 쓰기 락을 잡고 있을 때 최대 5초간 재시도합니다.
 *    5초 후에도 획득 못하면 SQLITE_BUSY 에러를 반환합니다.
 *
 * ====================================================================
 *  재시작 시 Reconcile 흐름
 * ====================================================================
 *  1. init_pending_state_machine() 호출
 *  2. vm_locks 테이블의 모든 행 스캔
 *  3. 각 행의 PID가 살아있는지 kill(pid, 0)으로 확인
 *  4. 죽은 PID의 락은 자동 삭제 (고아 락 회수)
 *  5. 회수된 락 수를 로그로 출력
 *
 * ====================================================================
 *  DB 스키마
 * ====================================================================
 *  테이블: vm_locks
 *    - vm_id      TEXT PRIMARY KEY  (VM 이름 또는 UUID)
 *    - op_type    INTEGER           (VmPendingOp 열거형 값)
 *    - pid        INTEGER           (락을 획득한 프로세스 PID)
 *    - locked_at  INTEGER           (Unix timestamp, 초 단위)
 *
 * ====================================================================
 *  DB 경로
 * ====================================================================
 *  pcv_config_get_db_path() → 기본: /var/lib/purecvisor/vm_state.db
 *  ZFS 데이터셋 pcvpool/state에 마운트 시 sync=always로 WAL 안전성 보장
 */
/* src/modules/core/vm_state.c
 *
 * Sprint B-1: VM 상태 영속화 레이어 구현
 *
 * SQLite WAL 모드를 사용하여 VM 오퍼레이션 락을 디스크에 기록합니다.
 * 데몬 재시작 시 이전 PID가 살아있는지 확인하여 고아 락을 자동 회수합니다.
 */

/* ── 헤더 의존성 ──────────────────────────────────────────────────────────
 *
 * pcv_config.h : daemon.conf에서 DB 경로를 조회 (pcv_config_get_db_path)
 * vm_state.h   : 이 모듈의 공개 API (init/lock/unlock/shutdown 등)
 * sqlite3.h    : SQLite3 C API (sqlite3_open, sqlite3_exec, sqlite3_prepare_v2 등)
 * glib.h       : GLib 유틸리티 (GMutex, g_warning, g_message, GPtrArray 등)
 * unistd.h     : POSIX API — getpid() (현재 PID), kill() (프로세스 존재 확인)
 * signal.h     : kill(pid, 0) 시그널 상수 — 프로세스 존재 여부 확인
 * sys/stat.h   : mkdir() — DB 디렉토리 생성
 * errno.h      : errno 전역 변수 — kill() 실패 원인 (ESRCH, EPERM)
 * ──────────────────────────────────────────────────────────────────────── */
#include "../../utils/pcv_config.h"
#include "vm_state.h"
#include <sqlite3.h>
#include <glib.h>
#include <libvirt/libvirt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>   /* getpid() — 현재 프로세스 ID 조회 */
#include <signal.h>   /* kill() — 프로세스 존재 확인 (시그널 0) */
#include <sys/stat.h> /* mkdir() — 디렉토리 생성 */
#include <errno.h>    /* errno — kill() 실패 원인 확인 */

/* ── 상수 ─────────────────────────────────────────────── */

/**
 * DB 디렉토리와 경로 기본값
 *
 * [실제 경로 결정]
 *   pcv_config_get_db_path()가 daemon.conf의 설정을 먼저 확인합니다.
 *   설정이 없으면 이 기본값을 사용합니다.
 *   ZFS 환경에서는 /pcvpool/state/vm_state.db에 마운트될 수 있습니다.
 */
#define DB_DIR  "/var/lib/purecvisor"
#define DB_PATH "/var/lib/purecvisor/vm_state.db"

/**
 * 잠금 만료 시간 (초) — 오퍼레이션 타입별 차등 (W4 fix, 2026-04-11)
 *
 * 기존: 모든 오퍼레이션이 300초(5분)로 고정 → 라이브 마이그레이션(수십 분 소요)
 *       중 만료되어 동시 start/stop이 발생하는 위험.
 *
 * 개선: start/stop/pause/resume/reset 등 빠른 작업 → 60초
 *       create/delete (ZFS) → 600초 (10분)
 *       migrate (라이브 마이그레이션) → 1800초 (30분)
 *       기본(알 수 없는 op) → 300초
 *
 * libvirt hang이나 크래시 복구 시 해당 TTL 이후 자동 해제됩니다.
 */
#define LOCK_EXPIRY_DEFAULT   300
#define LOCK_EXPIRY_FAST      60    /* start/stop/pause/resume/reset */
#define LOCK_EXPIRY_ZFS       600   /* create/delete (ZFS destroy 포함) */
#define LOCK_EXPIRY_MIGRATE   1800  /* 라이브 마이그레이션 */

/* 레거시 호환 — 타임아웃 스위퍼는 가장 긴 값을 기준으로 cutoff */
#define LOCK_EXPIRY_SEC       LOCK_EXPIRY_MIGRATE

/**
 * op_type → expiry 매핑. op_type 상수는 handler에서 사용:
 *   VM_OP_CREATING=1 / VM_OP_STOPPING=2 / VM_OP_STARTING=3 /
 *   VM_OP_DELETING=4 / VM_OP_SNAPSHOTTING=5 / VM_OP_MIGRATING=6 / ...
 */
static gint _lock_expiry_for_op(gint op_type) {
    switch (op_type) {
        case 1: /* CREATING */
        case 4: /* DELETING */
            return LOCK_EXPIRY_ZFS;
        case 2: /* STOPPING */
        case 3: /* STARTING */
            return LOCK_EXPIRY_FAST;
        case 6: /* MIGRATING */
            return LOCK_EXPIRY_MIGRATE;
        default:
            return LOCK_EXPIRY_DEFAULT;
    }
}

/* ── 모듈 내부 상태 ───────────────────────────────────── */

/**
 * 전역 SQLite DB 핸들 — 데몬 전체에서 공유하는 싱글턴
 *
 * [왜 전역 변수인가?]
 *   vm_state는 데몬 전체에서 단 하나의 SQLite 인스턴스를 공유합니다.
 *   모든 RPC 핸들러가 lock_vm_operation/unlock_vm_operation을 호출하므로
 *   싱글턴 패턴이 적합합니다. GLib 이벤트 루프 기반 단일 프로세스 아키텍처에서
 *   파일 스코프 static 변수로 구현합니다.
 *
 * [NULL의 의미]
 *   g_db가 NULL이면 SQLite 초기화에 실패한 상태입니다.
 *   이 경우 lock/unlock은 graceful degradation으로 락 없이 동작합니다.
 *   (동시 오퍼레이션 충돌 위험이 있지만, 데몬이 동작하지 않는 것보다 나음)
 */
static sqlite3  *g_db   = NULL;

/**
 * DB 접근을 보호하는 뮤텍스
 *
 * [g_db_mutex의 역할]
 *   SQLite 자체가 쓰기 직렬화를 지원하지만, g_db 포인터 자체의 NULL 체크와
 *   BEGIN IMMEDIATE 트랜잭션의 원자성을 보장하기 위해 GMutex를 추가합니다.
 *   GTask 워커 스레드에서 동시에 lock/unlock이 호출될 수 있기 때문입니다.
 *
 * [GMutex vs pthread_mutex]
 *   GMutex는 GLib의 뮤텍스 추상화입니다. Linux에서는 내부적으로
 *   futex를 사용하여 커널 컨텍스트 스위치 없이 빠르게 락을 획득합니다.
 *   g_mutex_init()으로 초기화, g_mutex_clear()로 해제합니다.
 */
static GMutex    g_db_mutex;

/* ── 내부 헬퍼 ────────────────────────────────────────── */

/**
 * @brief PID가 현재 시스템에 살아있는지 확인
 *
 * [kill(pid, 0)의 원리]
 *   POSIX 표준: 시그널 번호 0은 실제로 시그널을 전송하지 않지만,
 *   프로세스 존재 여부와 권한 확인은 수행합니다.
 *   - 반환값 0: 프로세스 존재 (시그널 전송 권한 있음)
 *   - 반환값 -1, errno=ESRCH: 프로세스가 존재하지 않음
 *   - 반환값 -1, errno=EPERM: 프로세스 존재하지만 시그널 권한 없음
 *     → 우리 데몬은 root로 실행되므로 EPERM은 발생하지 않습니다.
 *
 * [고아 락 감지에서의 활용]
 *   데몬 크래시 후 재시작하면 이전 PID는 이미 죽어있습니다.
 *   (PID는 OS가 재사용할 수 있지만, 데몬 PID가 재사용될 확률은 극히 낮음)
 *   이 함수로 확인하여 고아 락을 자동으로 회수합니다.
 *
 * @param pid 확인할 프로세스 ID (0 이하이면 무조건 FALSE)
 * @return TRUE=살아있음, FALSE=죽었거나 유효하지 않은 PID
 */
static gboolean pid_is_alive(gint pid) {
    if (pid <= 0) return FALSE;  /* 유효하지 않은 PID — 0은 현재 프로세스 그룹 */
    /* kill(pid, 0) : 시그널 전송 없이 프로세스 존재만 확인 */
    return (kill((pid_t)pid, 0) == 0);
}

/**
 * @brief SQLite 에러 코드를 GLib 경고 로그로 출력하는 헬퍼
 *
 * 정상 완료 코드(SQLITE_OK, SQLITE_DONE, SQLITE_ROW)가 아닌 경우에만
 * 경고를 출력합니다. 모든 SQLite 호출 후 이 함수를 호출하여
 * 에러를 놓치지 않도록 합니다.
 *
 * [SQLite 반환 코드]
 *   SQLITE_OK(0):    성공
 *   SQLITE_DONE(101): sqlite3_step()이 모든 행을 처리 완료
 *   SQLITE_ROW(100):  sqlite3_step()이 결과 행을 반환 (더 읽을 행 있음)
 *   SQLITE_BUSY(5):   다른 프로세스가 DB를 잠금 (busy_timeout 초과)
 *   SQLITE_ERROR(1):  SQL 구문 오류 또는 런타임 오류
 *
 * @param context 호출 컨텍스트 문자열 (예: "PRAGMA journal_mode=WAL")
 * @param rc      sqlite3_exec() 등의 반환값
 */
static void log_sqlite_error(const gchar *context, int rc) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        g_warning("[vm_state] %s: SQLite error %d: %s", context, rc, sqlite3_errmsg(g_db));
    }
}

/* ── 공개 API 구현 ────────────────────────────────────── */

/**
 * @brief VM 상태 머신 초기화 — SQLite DB 열기 + 테이블 생성 + 고아 락 회수
 *
 * 데몬 시작 시 main.c의 초기화 2단계에서 호출됩니다.
 * 6단계 초기화를 수행합니다:
 *   1. GMutex 초기화
 *   2. DB 디렉토리 생성 (/var/lib/purecvisor/)
 *   3. SQLite DB 열기 (pcv_config_get_db_path())
 *   4. WAL 모드 + busy_timeout 설정
 *   5. vm_locks 테이블 생성 (IF NOT EXISTS — 멱등성)
 *   6. Reconcile: 죽은 PID의 고아 락 자동 삭제
 *
 * [g_mkdir_with_parents의 역할]
 *   mkdir -p와 동일하게 중간 디렉토리도 함께 생성합니다.
 *   이미 존재하면 성공을 반환합니다 (멱등성).
 *
 * [WAL 모드 활성화 이유]
 *   기본 저널 모드(DELETE)에서는 쓰기 시 읽기가 차단됩니다.
 *   WAL 모드에서는 쓰기 중에도 읽기가 가능하여
 *   여러 GTask 워커가 동시에 lock/unlock을 호출할 때 성능이 향상됩니다.
 *
 * @note DB 열기 실패 시 g_db=NULL로 남으며, 이후 lock/unlock 호출은
 *       graceful degradation으로 락 없이 동작합니다.
 */
void init_pending_state_machine(void) {
    g_mutex_init(&g_db_mutex);

    /* 1. DB 디렉토리 생성 (없으면 — mkdir -p 동등) */
    /* 테스트 격리: PCV_VM_STATE_DB_PATH > daemon.conf > 기본값 */
    const gchar *env_path = g_getenv("PCV_VM_STATE_DB_PATH");
    const gchar *db_path = (env_path && *env_path) ? env_path : pcv_config_get_db_path();
    gchar *db_dir = g_path_get_dirname(db_path);  /* 경로에서 디렉토리 부분 추출 */
    if (g_mkdir_with_parents(db_dir, 0700) != 0) {
        g_warning("[vm_state] Failed to create DB directory: %s", db_dir);
    }

    /* 2. SQLite DB 열기 (파일이 없으면 자동 생성됨) */
    int rc = sqlite3_open(db_path, &g_db);
    g_free(db_dir);  /* g_path_get_dirname()이 g_malloc한 문자열 해제 */
    if (rc != SQLITE_OK) {
        g_critical("[vm_state] Cannot open database '%s': %s", db_path, sqlite3_errmsg(g_db));
        g_db = NULL;  /* 실패 시 NULL로 설정 — lock/unlock이 graceful degradation */
        return;
    }

    /* 3. WAL 모드 활성화 — 동시 읽기 성능 향상, 크래시 안전성 보장 */
    rc = sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    log_sqlite_error("PRAGMA journal_mode=WAL", rc);

    /* 3-1. WAL 자동 체크포인트 제한 — 1000 페이지마다 WAL→DB 병합 (무한 WAL 성장 방지) */
    rc = sqlite3_exec(g_db, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);
    log_sqlite_error("PRAGMA wal_autocheckpoint", rc);

    /* 4. busy_timeout: 다른 스레드가 쓰기 락 중일 때 최대 5초 대기 후 재시도 */
    rc = sqlite3_exec(g_db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    log_sqlite_error("PRAGMA busy_timeout", rc);

    /* 5. 테이블 생성 (IF NOT EXISTS — 이미 존재하면 무시, 멱등성 보장) */
    const gchar *create_sql =
        "CREATE TABLE IF NOT EXISTS vm_locks ("
        "  vm_id      TEXT    PRIMARY KEY,"  /* VM 이름 또는 UUID — B-tree 인덱스 자동 생성 */
        "  op_type    INTEGER NOT NULL,"     /* VmPendingOp 열거형 값 (0~7) */
        "  pid        INTEGER NOT NULL,"     /* 락 획득 프로세스 PID */
        "  locked_at  INTEGER NOT NULL"      /* Unix timestamp (초 단위) */
        ");";
    rc = sqlite3_exec(g_db, create_sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_critical("[vm_state] Failed to create vm_locks table: %s", sqlite3_errmsg(g_db));
        return;
    }

    /* ── 6. Reconcile: 고아 락 회수 ──────────────────────── */
    /*
     * [Reconcile(재조정) 로직]
     *   1. vm_locks 테이블의 모든 행을 SELECT
     *   2. 각 행의 PID가 살아있는지 kill(pid, 0)으로 확인
     *   3. 죽은 PID의 vm_id를 수집 (GPtrArray에 저장)
     *   4. 수집된 vm_id들을 일괄 DELETE
     *
     * [왜 SELECT 후 별도 DELETE인가?]
     *   SQLite의 sqlite3_step() 루프 중에 같은 테이블을 수정하면
     *   결과 집합이 불안정해질 수 있습니다. 따라서 먼저 삭제 대상을
     *   수집한 후 루프 종료 후 일괄 삭제합니다.
     */
    const gchar *select_sql = "SELECT vm_id, pid FROM vm_locks;";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(g_db, select_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        /*
         * GPtrArray: 고아 락의 vm_id를 수집하는 동적 포인터 배열
         * g_free를 free_func로 등록하여 배열 해제 시 문자열도 자동 해제
         */
        GPtrArray *stale_ids = g_ptr_array_new_with_free_func(g_free);

        /* 모든 락 행을 순회하며 죽은 PID 찾기 */
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *vm_id = (const gchar *)sqlite3_column_text(stmt, 0);
            gint         pid   = sqlite3_column_int(stmt, 1);

            if (!pid_is_alive(pid)) {
                /* PID가 죽었음 — 삭제 대상으로 수집 */
                g_ptr_array_add(stale_ids, g_strdup(vm_id));
            }
        }
        sqlite3_finalize(stmt);  /* prepared statement 자원 해제 */

        /* 수집된 고아 락 일괄 삭제 */
        for (guint i = 0; i < stale_ids->len; i++) {
            const gchar *stale_id = g_ptr_array_index(stale_ids, i);
            /*
             * [SQL 인젝션 주의]
             *   여기서는 g_strdup_printf로 직접 SQL을 조합하고 있습니다.
             *   vm_id는 내부 DB에서 읽은 값이므로 외부 입력이 아닙니다.
             *   하지만 보안 모범 사례로는 prepared statement를 사용하는 것이 좋습니다.
             */
            sqlite3_stmt *del_stmt = NULL;
            int del_rc = sqlite3_prepare_v2(g_db,
                "DELETE FROM vm_locks WHERE vm_id = ?;", -1, &del_stmt, NULL);
            if (del_rc == SQLITE_OK) {
                sqlite3_bind_text(del_stmt, 1, stale_id, -1, SQLITE_STATIC);
                sqlite3_step(del_stmt);
                sqlite3_finalize(del_stmt);
            }
            g_message("[vm_state] Reconcile: released stale lock for VM '%s' (dead PID %d)",
                      stale_id, 0);
        }

        if (stale_ids->len > 0) {
            g_message("[vm_state] Reconcile: %u stale lock(s) released.", stale_ids->len);
        } else {
            g_message("[vm_state] Reconcile: no stale locks found.");
        }
        g_ptr_array_free(stale_ids, TRUE);  /* TRUE: 배열 + 요소 모두 해제 */
    }

    g_message("[vm_state] SQLite WAL state machine initialized at '%s'.", db_path);
}

/**
 * @brief VM에 대한 배타적 오퍼레이션 락 획득
 *
 * [왜 이 락이 필요한가?]
 *   같은 VM에 vm.start와 vm.delete가 동시에 도착하면 정의되지 않은 동작이
 *   발생합니다. 이 함수로 한 번에 하나의 오퍼레이션만 허용합니다.
 *
 * [트랜잭션: BEGIN IMMEDIATE의 의미]
 *   일반 BEGIN: 첫 SELECT까지 쓰기 락을 미룹니다.
 *     → 문제: SELECT 후 INSERT 사이에 다른 스레드가 끼어들 수 있음 (TOCTOU)
 *   BEGIN IMMEDIATE: 즉시 RESERVED 락을 획득합니다.
 *     → 다른 writer가 동시에 트랜잭션을 시작하는 것을 방지
 *     → "읽기 → 판단 → 쓰기" 전체가 원자적으로 실행됨
 *
 * [고아 락 자동 해제]
 *   기존 락이 존재하더라도 해당 PID가 죽은 상태라면 덮어쓰기를 허용합니다.
 *   이는 데몬 크래시 후 재시작 시 이전 락이 영원히 남는 것을 방지합니다.
 *
 * [DB 미초기화 시 graceful degradation]
 *   g_db가 NULL이면 (SQLite 열기 실패) 락 없이 TRUE를 반환합니다.
 *   락 없이 진행하면 동시 오퍼레이션 충돌 위험이 있지만,
 *   데몬이 아예 동작하지 않는 것보다는 낫다는 설계 판단입니다.
 *
 * [err_msg의 소유권]
 *   실패 시 *err_msg에 g_strdup_printf로 할당된 문자열이 설정됩니다.
 *   호출자가 g_free()로 해제해야 합니다 (GLib 메모리 관리 규약).
 *   err_msg이 NULL이면 에러 메시지를 설정하지 않습니다.
 *
 * @param vm_id   대상 VM 식별자 (이름 또는 UUID)
 * @param op_type VmPendingOp 열거형 값
 * @param err_msg 실패 사유 (출력 파라미터, NULL 가능)
 * @return TRUE=락 획득 성공, FALSE=실패
 */
gboolean lock_vm_operation(const gchar *vm_id, gint op_type, gchar **err_msg) {
    if (!g_db) {
        /* 위험: DB 미초기화 시 락 없이 TRUE 반환 — 동시 오퍼레이션 충돌 가능.
         * 그래도 데몬이 아예 멈추는 것보다 낫다는 설계 판단 (graceful degradation). */
        g_warning("[vm_state] DB not initialized, skipping lock for VM '%s'", vm_id);
        return TRUE;
    }

    g_mutex_lock(&g_db_mutex);  /* 다른 GTask 워커 스레드와의 경쟁 방지 */

    gboolean result = FALSE;
    sqlite3_stmt *stmt = NULL;

    /*
     * 현재 시간(Unix timestamp, 초 단위)과 프로세스 PID
     *
     * [g_get_real_time()의 반환값]
     *   마이크로초(usec) 단위의 Unix timestamp를 반환합니다.
     *   G_USEC_PER_SEC(1000000)으로 나누어 초 단위로 변환합니다.
     *
     * [getpid()의 반환값]
     *   현재 프로세스(데몬)의 PID를 반환합니다.
     *   고아 락 감지에 사용됩니다 (재시작 시 이전 PID와 비교).
     */
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
    gint   pid = (gint)getpid();

    /* 주의: BEGIN IMMEDIATE 필수 — 일반 BEGIN은 첫 SELECT까지 쓰기 락을 미루므로
     * SELECT→INSERT 사이에 다른 GTask 워커가 끼어들어 동일 VM을 이중 잠금할 수 있다. */
    int rc = sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        if (err_msg) *err_msg = g_strdup_printf("DB transaction error: %s", sqlite3_errmsg(g_db));
        goto done;
    }

    /*
     * 기존 락 존재 여부 확인 (SELECT)
     *
     * [Prepared Statement 사용 이유]
     *   sqlite3_prepare_v2() + sqlite3_bind_text()로 파라미터를 바인딩하면
     *   SQL 인젝션을 방지하고, SQLite가 쿼리 계획을 캐시할 수 있습니다.
     *   '?' 플레이스홀더에 값이 안전하게 바인딩됩니다.
     */
    /* [P0-3 수정] 3컬럼을 단일 SELECT로 동시 조회해야 하는 이유:
     * 이전에는 op_type과 pid/locked_at을 별도 SELECT 2개로 조회했다.
     * 그 사이에 다른 스레드가 해당 행을 갱신하면 op_type과 pid가 서로 다른 락의 값이 된다.
     * 단일 SELECT + BEGIN IMMEDIATE 조합으로 읽기-판단-쓰기 전체를 원자적으로 보장한다. */
    const gchar *check_sql =
        "SELECT op_type, pid, locked_at FROM vm_locks WHERE vm_id = ?;";
    rc = sqlite3_prepare_v2(g_db, check_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (err_msg) *err_msg = g_strdup("DB prepare error");
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        goto done;
    }

    /*
     * SQLITE_STATIC: 바인딩된 문자열이 sqlite3_step() 완료까지 유효함을 보장
     * (vm_id는 호출자의 스택/힙에 있으므로 이 함수 내에서는 항상 유효)
     */
    sqlite3_bind_text(stmt, 1, vm_id, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        /* 기존 락이 존재함 — 단일 SELECT에서 op_type, pid, locked_at 동시 조회 */
        gint existing_op  = sqlite3_column_int(stmt, 0);
        gint existing_pid = sqlite3_column_int(stmt, 1);
        gint64 locked_at_val = sqlite3_column_int64(stmt, 2);
        sqlite3_finalize(stmt);
        stmt = NULL;

        /* [W4 fix] 락 만료 판정 — 기존 락의 op_type에 따라 차등 TTL 적용.
         * 빠른 작업(start/stop)은 60초 후 stale, 라이브 마이그레이션은 30분까지 유효.
         * 이전 고정 300초는 migrate 조기 만료 + fast op 지연 해제 양쪽 모두 문제. */
        gint ttl = _lock_expiry_for_op(existing_op);
        gboolean lock_expired = (locked_at_val > 0 && (now - locked_at_val) >= ttl);

        /* 두 조건 모두 충족해야 유효한 락: PID 생존 + 미만료.
         * PID만 확인하면 데몬 hang 시 영구 잠금, 만료만 확인하면 PID 재사용 오탐. */
        if (pid_is_alive(existing_pid) && !lock_expired) {
            /*
             * 살아있는 프로세스가 잡고 있고 만료되지 않음 — 거부
             *
             * [op_names 배열]
             *   VmPendingOp 열거형 값(0~7)을 사람이 읽을 수 있는 문자열로 변환합니다.
             *   에러 메시지에 "STARTING", "DELETING" 등으로 표시하여
             *   운영자가 어떤 작업 때문에 거부되었는지 즉시 파악할 수 있습니다.
             */
            static const gchar *op_names[] = {
                "NONE", "STARTING", "STOPPING", "DELETING",
                "CREATING", "TUNING", "SNAPSHOT", "MIGRATING"
            };
            const gchar *op_name = (existing_op >= 0 && existing_op <= 7)
                                   ? op_names[existing_op] : "UNKNOWN";
            if (err_msg)
                *err_msg = g_strdup_printf(
                    "VM '%s' is already locked (op: %s, pid: %d)", vm_id, op_name, existing_pid);
            sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
            goto done;
        } else {
            /*
             * 고아 락 발견 — 재시작 전 PID가 죽었으므로 덮어쓰기 허용
             *
             * [왜 DELETE 후 INSERT인가?]
             *   vm_id가 PRIMARY KEY이므로 기존 행이 있으면 INSERT가 실패합니다.
             *   먼저 DELETE로 기존 행을 제거한 후 새 행을 INSERT합니다.
             *   (INSERT OR REPLACE도 가능하지만, 명시적 DELETE가 더 명확합니다)
             */
            if (lock_expired)
                g_warning("[vm_state] Overwriting expired lock for VM '%s' (age %ld sec, PID %d)",
                          vm_id, (long)(now - locked_at_val), existing_pid);
            else
                g_warning("[vm_state] Overwriting stale lock for VM '%s' (dead PID %d)",
                          vm_id, existing_pid);
            sqlite3_stmt *del_stmt = NULL;
            int del_rc = sqlite3_prepare_v2(g_db,
                "DELETE FROM vm_locks WHERE vm_id = ?;", -1, &del_stmt, NULL);
            if (del_rc == SQLITE_OK) {
                sqlite3_bind_text(del_stmt, 1, vm_id, -1, SQLITE_STATIC);
                sqlite3_step(del_stmt);
                sqlite3_finalize(del_stmt);
            }
        }
    } else {
        sqlite3_finalize(stmt);  /* 행이 없음 — statement 정리 */
        stmt = NULL;
    }

    /* 새 락 INSERT — vm_id, op_type, pid, locked_at */
    const gchar *insert_sql =
        "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES (?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(g_db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (err_msg) *err_msg = g_strdup("DB insert prepare error");
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        goto done;
    }

    /* 파라미터 바인딩 (1-based 인덱스) */
    sqlite3_bind_text (stmt, 1, vm_id,   -1, SQLITE_STATIC);  /* vm_id TEXT */
    sqlite3_bind_int  (stmt, 2, op_type);                      /* op_type INTEGER */
    sqlite3_bind_int  (stmt, 3, pid);                          /* pid INTEGER */
    sqlite3_bind_int64(stmt, 4, now);                          /* locked_at INTEGER (64비트) */

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        /* INSERT 실패 — PRIMARY KEY 충돌 또는 기타 에러 */
        if (err_msg)
            *err_msg = g_strdup_printf("DB insert failed: %s", sqlite3_errmsg(g_db));
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        goto done;
    }

    /* 트랜잭션 커밋 — 여기까지 오면 락 획득 성공 */
    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    result = TRUE;

done:
    /* 흔한 실수: goto done을 쓸 때 stmt finalize와 mutex unlock을 빠뜨리면
     * 메모리 누수(stmt)와 데드락(mutex)이 발생한다. 모든 에러 경로가 여기를 거친다. */
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_db_mutex);
    return result;
}

/**
 * @brief VM 오퍼레이션 완료 후 락 해제
 *
 * [호출 시점]
 *   handler_vm_*.c에서 오퍼레이션(start/stop/delete 등)이 완료된 후 호출합니다.
 *   GTask 콜백이나 워커 스레드 내부에서 호출될 수 있습니다.
 *   반드시 성공/실패 모든 경로에서 호출해야 합니다.
 *
 * [멱등성]
 *   vm_id에 해당하는 락이 없어도 에러를 발생시키지 않습니다.
 *   DELETE FROM은 해당 행이 없으면 아무 일도 하지 않습니다 (affected_rows=0).
 *   따라서 이중 호출에도 안전합니다.
 *
 * [NULL 안전]
 *   g_db가 NULL이거나 vm_id가 NULL이면 즉시 반환합니다.
 *   이는 graceful degradation 상태에서의 안전한 동작을 보장합니다.
 *
 * @param vm_id 대상 VM 식별자 (NULL이면 즉시 반환)
 */
void unlock_vm_operation(const gchar *vm_id) {
    if (!g_db || !vm_id) return;

    g_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    const gchar *sql = "DELETE FROM vm_locks WHERE vm_id = ?;";
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, vm_id, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        log_sqlite_error("unlock DELETE", rc);
        sqlite3_finalize(stmt);
    }

    g_mutex_unlock(&g_db_mutex);
}

/**
 * @brief 현재 보유 중인 VM 락 수를 반환 (Prometheus 메트릭용)
 *
 * [용도]
 *   Prometheus exporter에서 purecvisor_vm_locks_held 게이지 메트릭으로 노출합니다.
 *   모니터링 대시보드에서 동시 진행 중인 VM 오퍼레이션 수를 시각화할 수 있습니다.
 *   비정상적으로 높으면 (예: >10) 락 해제 실패를 의심해야 합니다.
 *
 * [SELECT COUNT(*)]
 *   vm_locks 테이블의 전체 행 수를 반환합니다.
 *   SQLite는 이 쿼리를 B-tree 전체 스캔으로 처리하지만,
 *   행 수가 수십 개 수준이므로 성능 이슈는 없습니다.
 *
 * @return 현재 잠금 수 (DB 미초기화 시 0)
 */
gint pcv_vm_state_get_lock_count(void) {
    if (!g_db) return 0;

    g_mutex_lock(&g_db_mutex);
    gint count = 0;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT COUNT(*) FROM vm_locks;", -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_db_mutex);
    return count;
}

/**
 * @brief 만료된 잠금을 일괄 정리
 *
 * locked_at + LOCK_EXPIRY_SEC보다 오래된 잠금을 삭제합니다.
 * GMainLoop 타이머(300초 주기)에서 호출되어 교착을 자동 방지합니다.
 *
 * [왜 타이머 기반 정리가 필요한가?]
 *   lock_vm_operation에서 만료 체크를 하지만, 이는 같은 VM에 대한
 *   새 오퍼레이션이 도착해야만 동작합니다. 아무도 접근하지 않는
 *   VM의 만료된 잠금은 이 타이머로만 정리됩니다.
 *
 * [cutoff 계산]
 *   현재 시간 - LOCK_EXPIRY_SEC(300초) = cutoff
 *   locked_at < cutoff인 잠금은 만료된 것으로 간주합니다.
 *
 * [SELECT → DELETE 분리 패턴]
 *   init_pending_state_machine의 reconcile과 동일한 이유:
 *   sqlite3_step() 루프 중 같은 테이블을 수정하면 불안정해질 수 있으므로
 *   먼저 삭제 대상을 수집한 후 일괄 삭제합니다.
 *
 * @return 정리된 잠금 수 (DB 미초기화 시 0)
 */
gint pcv_vm_state_cleanup_expired(void) {
    if (!g_db) return 0;

    g_mutex_lock(&g_db_mutex);

    /* cutoff: 이 시각 이전에 잠긴 락은 만료 처리.
     * 타이머 기반 정리가 필요한 이유: lock_vm_operation의 만료 체크는 같은 VM에
     * 새 요청이 올 때만 동작한다. 아무도 접근하지 않는 VM의 고아 락은 여기서만 정리된다. */
    gint64 cutoff = (gint64)g_get_real_time() / G_USEC_PER_SEC - LOCK_EXPIRY_SEC;
    gint cleaned = 0;

    /* 만료된 잠금 조회 → 삭제 */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db,
        "SELECT vm_id, op_type, pid, locked_at FROM vm_locks WHERE locked_at < ?;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        GPtrArray *expired_ids = g_ptr_array_new_with_free_func(g_free);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *vm_id = (const gchar *)sqlite3_column_text(stmt, 0);
            gint op_type = sqlite3_column_int(stmt, 1);
            gint pid = sqlite3_column_int(stmt, 2);
            gint64 locked_at = sqlite3_column_int64(stmt, 3);
            gint64 now_sec = (gint64)g_get_real_time() / G_USEC_PER_SEC;
            g_warning("[vm_state] Expired lock: VM '%s' op=%d pid=%d age=%ld sec",
                      vm_id, op_type, pid, (long)(now_sec - locked_at));
            g_ptr_array_add(expired_ids, g_strdup(vm_id));
        }
        sqlite3_finalize(stmt);

        for (guint i = 0; i < expired_ids->len; i++) {
            const gchar *eid = g_ptr_array_index(expired_ids, i);
            sqlite3_stmt *del = NULL;
            int del_rc = sqlite3_prepare_v2(g_db,
                "DELETE FROM vm_locks WHERE vm_id = ?;", -1, &del, NULL);
            if (del_rc == SQLITE_OK) {
                sqlite3_bind_text(del, 1, eid, -1, SQLITE_STATIC);
                sqlite3_step(del);
                sqlite3_finalize(del);
                cleaned++;
            }
        }
        if (cleaned > 0)
            g_message("[vm_state] Cleanup: %d expired lock(s) removed.", cleaned);
        g_ptr_array_free(expired_ids, TRUE);
    }

    g_mutex_unlock(&g_db_mutex);
    return cleaned;
}

/* ── VM 런타임 상태 조회 ─────────────────────────────────── */

/**
 * @brief VM의 실시간 런타임 상태를 libvirt에서 조회한다.
 *
 * virDomainGetState()를 통해 VM의 현재 상태를 조회하고,
 * PcvVmRuntimeState 열거형으로 매핑하여 반환한다.
 * 커넥션 풀을 통해 libvirt 연결을 획득/반환한다.
 *
 * @param vm_name VM 이름
 * @return PcvVmRuntimeState (UNKNOWN/RUNNING/STOPPED/PAUSED/MIGRATING/ERROR)
 */
PcvVmRuntimeState
pcv_vm_state_get_runtime(const gchar *vm_name)
{
    if (!vm_name) return PCV_VM_UNKNOWN;

    extern virConnectPtr virt_conn_pool_acquire(void);
    extern void          virt_conn_pool_release(virConnectPtr);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) return PCV_VM_UNKNOWN;

    virDomainPtr dom = virDomainLookupByName(conn, vm_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        return PCV_VM_STOPPED;
    }

    int state = 0, reason = 0;
    virDomainGetState(dom, &state, &reason, 0);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    switch (state) {
        case VIR_DOMAIN_RUNNING:   return PCV_VM_RUNNING;
        case VIR_DOMAIN_PAUSED:    return PCV_VM_PAUSED;
        case VIR_DOMAIN_SHUTOFF:   return PCV_VM_STOPPED;
        case VIR_DOMAIN_PMSUSPENDED: return PCV_VM_PAUSED;
        default:                   return PCV_VM_UNKNOWN;
    }
}

/**
 * @brief PcvVmRuntimeState를 문자열로 변환한다.
 *
 * @param state VM 런타임 상태
 * @return 상태 문자열 (정적 — 해제 불필요)
 */
const gchar *
pcv_vm_state_runtime_str(PcvVmRuntimeState state)
{
    switch (state) {
        case PCV_VM_RUNNING:   return "running";
        case PCV_VM_STOPPED:   return "stopped";
        case PCV_VM_PAUSED:    return "paused";
        case PCV_VM_MIGRATING: return "migrating";
        case PCV_VM_ERROR:     return "error";
        default:               return "unknown";
    }
}

/**
 * @brief VM 상태 머신 종료 — SQLite DB를 안전하게 닫고 뮤텍스 해제
 *
 * main.c의 cleanup 블록에서 호출됩니다.
 * 모든 워커 스레드가 종료된 후에 호출해야 합니다
 * (lock/unlock 호출이 더 이상 없는 상태).
 *
 * [sqlite3_close()의 동작]
 *   WAL 모드에서 close()는 다음을 수행합니다:
 *     1. WAL 파일의 미반영 트랜잭션을 체크포인트 (메인 DB에 반영)
 *     2. WAL 파일과 SHM(공유 메모리) 파일 삭제
 *     3. DB 파일 핸들 닫기
 *
 * [g_mutex_clear()의 의미]
 *   뮤텍스가 차지하는 OS 리소스를 해제합니다.
 *   clear 후 해당 뮤텍스를 사용하면 정의되지 않은 동작(UB)입니다.
 *   따라서 이 함수 호출 후 lock/unlock을 호출하면 안 됩니다.
 *
 * @note 이 함수 호출 후에는 lock_vm_operation/unlock_vm_operation을
 *       호출하면 안 됩니다 (뮤텍스가 해제된 상태).
 */
void shutdown_pending_state_machine(void) {
    g_mutex_lock(&g_db_mutex);
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;  /* 이후 lock/unlock 호출 시 NULL 체크로 안전하게 반환 */
        g_message("[vm_state] SQLite DB closed.");
    }
    g_mutex_unlock(&g_db_mutex);
    g_mutex_clear(&g_db_mutex);  /* 뮤텍스 OS 리소스 해제 — 이후 사용 불가 */
}
