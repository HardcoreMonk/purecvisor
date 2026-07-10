/**
 * @file core.h
 * @brief PureCVisor 코어 공개 API — 데몬 컨텍스트, 라이프사이클, 로깅
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  현재 에디션 데몬의 최상위 컨텍스트(PvContext)와 라이프사이클 API를
 *  외부에 노출하는 헤더이다. 데몬 초기화(pv_init), 이벤트 루프 실행
 *  (pv_run), 정리(pv_cleanup)의 3단계로 데몬 수명을 관리한다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  공개 헤더(include/purecvisor/)의 최상위. main.c에서 이 헤더만
 *  포함하면 데몬을 초기화하고 구동할 수 있다.
 *
 *  [호출 체인]
 *    main.c
 *        | pv_init()    → PvContext 싱글턴 생성, libvirt 연결, 서브시스템 초기화
 *        | pv_run()     → GMainLoop 진입 (UDS + REST + 텔레메트리 + 백업 등)
 *        | pv_cleanup() → 자원 해제, libvirt 연결 종료
 *
 *  [다른 모듈과의 관계]
 *    - src/core/daemon.c: 이 헤더에 선언된 함수들의 구현 (레거시 경로)
 *    - src/main.c: 실제 운영에서는 이 API 대신 16단계 초기화를 직접 수행
 *    - include/daemon.h: 데몬 전용 확장 API (현재 예약 상태)
 *    - include/core_state.h: VM 오퍼레이션 잠금 (인메모리 레퍼런스 구현)
 *
 *  [현재 상태: 레거시]
 *    현재 main.c에서는 이 API를 직접 호출하지 않고, 각 서브시스템의
 *    init 함수를 직접 호출하는 16단계 초기화를 수행한다.
 *    이 API는 daemon.c의 레거시 경로이며, 테스트나 단순 구동에 사용 가능하다.
 *
 * ====================================================================
 *  핵심 설계 패턴
 * ====================================================================
 *
 *  [1] Opaque Pointer (불투명 포인터)
 *    PvContext 구조체의 멤버(GMainLoop*, virConnectPtr, is_running)는
 *    daemon.c에서만 정의되어 있다. 이 헤더에서는 "struct PvContext" 전방 선언만
 *    하고 typedef로 PvContext를 노출한다.
 *
 *    [왜 Opaque Pointer를 사용하는가?]
 *      - 캡슐화: 외부 코드가 PvContext의 내부 멤버에 직접 접근 불가
 *      - ABI 안정성: 구조체 멤버 변경 시 이 헤더를 include하는 코드 재컴파일 불필요
 *      - 의존성 최소화: libvirt/GLib 헤더를 이 헤더에 포함하지 않아도 됨
 *        (단, 현재는 libvirt.h를 포함하고 있어 이 장점은 부분적)
 *
 *    [Opaque Pointer 사용 패턴]
 *      // 외부 코드 (main.c 등)
 *      PvContext *ctx = pv_get_instance();  // 포인터만 얻음
 *      pv_init();                           // 함수를 통해서만 상태 변경
 *      // ctx->loop;  ← 컴파일 에러! (구조체 정의가 보이지 않으므로)
 *
 *  [2] Singleton (싱글턴)
 *    pv_get_instance()가 전역 유일한 PvContext를 반환한다.
 *    daemon.c 내부의 static PvContext g_ctx = {0}이 싱글턴 인스턴스이다.
 *    new/destroy 없이 프로세스 수명 동안 유일하게 존재한다.
 *
 *    [왜 싱글턴인가?]
 *      PureCVisor 데몬은 프로세스당 하나만 실행된다.
 *      GMainLoop, libvirt 연결 등 프로세스 전역 자원을 관리하므로
 *      복수의 컨텍스트가 존재하면 자원 충돌이 발생한다.
 *
 *  [3] X-Macro (엑스 매크로)
 *    LOG_LEVELS 매크로로 로그 레벨 열거형과 문자열을
 *    단일 소스에서 동기화하여 유지보수 부담을 줄인다.
 *
 *    [X-Macro의 동작 원리]
 *      1. LOG_LEVELS 매크로는 X(name, str) 형태의 호출 목록을 정의
 *      2. 사용처에서 X를 원하는 형태로 재정의(#define X ...)한 후 LOG_LEVELS를 확장
 *      3. 열거형 생성: #define X(name, str) name,  → LOG_INFO, LOG_WARN, ...
 *      4. 문자열 배열: #define X(name, str) str,   → "[INFO] ", "[WARN] ", ...
 *      이 기법으로 새 로그 레벨 추가 시 LOG_LEVELS에 한 줄만 추가하면
 *      열거형과 문자열이 자동으로 동기화된다.
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - pv_init()은 프로세스당 1회만 호출할 것. 중복 호출 시 정의되지 않은 동작(UB).
 *  - pv_run()은 GMainLoop가 종료될 때까지 블로킹된다.
 *    (SIGINT/SIGTERM → on_signal() → g_main_loop_quit()로 종료)
 *  - pv_cleanup()은 pv_run() 반환 후 1회만 호출할 것.
 *    NULL 체크 후 해제하므로 이중 호출에도 안전하지만, 권장하지 않음.
 *  - libvirt/libvirt.h 의존: 빌드 시 libvirt-dev 패키지가 필수.
 *    Ubuntu: sudo apt install libvirt-dev
 */

#ifndef PURECVISOR_CORE_H
#define PURECVISOR_CORE_H

#include <glib.h>
#include <libvirt/libvirt.h>

/**
 * PvContext — 데몬 엔진의 핵심 컨텍스트 (Opaque Pointer)
 *
 * [구조체 멤버 (daemon.c에서만 접근 가능)]
 *   GMainLoop *loop;      — GLib 이벤트 루프 (프로그램의 심장)
 *   virConnectPtr conn;   — libvirt 하이퍼바이저 연결 (qemu:///system)
 *   int is_running;       — 엔진 실행 상태 플래그 (1=실행 중, 0=미초기화)
 *
 * [전방 선언(forward declaration)의 의미]
 *   "struct PvContext"가 어딘가에 정의되어 있다고 컴파일러에 알려준다.
 *   구조체의 크기나 멤버는 모르지만, 포인터(PvContext*)는 사용할 수 있다.
 *   이를 "불완전 타입(incomplete type)"이라 한다.
 *   sizeof(PvContext)는 컴파일 에러가 발생한다 (크기를 모르므로).
 */
typedef struct PvContext PvContext;

/**
 * LOG_LEVELS — X-Macro를 이용한 로그 레벨 정의
 *
 * [사용법 예시]
 *   pv_log(LOG_INFO, "Started VM '%s'", vm_name);
 *   pv_log(LOG_ERR,  "Failed to connect: %s", error->message);
 *
 * [X-Macro 확장 과정]
 *   아래 enum LogLevel 정의에서:
 *     #define X(name, str) name,
 *     LOG_LEVELS
 *     #undef X
 *   는 다음으로 확장된다:
 *     LOG_INFO, LOG_WARN, LOG_ERR, LOG_DEBUG,
 *
 *   daemon.c의 pv_log() 구현에서 같은 매크로를 문자열 배열로 확장할 수 있다.
 *   현재는 switch-case로 직접 매핑하고 있다.
 */
#define LOG_LEVELS \
    X(LOG_INFO,  "[INFO] ") \
    X(LOG_WARN,  "[WARN] ") \
    X(LOG_ERR,   "[ERR ] ") \
    X(LOG_DEBUG, "[DBG ] ")

/**
 * enum LogLevel — 로그 레벨 열거형
 *
 * [값]
 *   LOG_INFO  = 0: 정상 동작 정보 (기동, 종료, 요청 처리 등)
 *   LOG_WARN  = 1: 경고 (비치명적 오류, graceful degradation 등)
 *   LOG_ERR   = 2: 에러 (치명적이지만 복구 가능한 오류)
 *   LOG_DEBUG = 3: 디버그 (개발/진단용 상세 정보)
 *
 * [프로덕션 로거와의 관계]
 *   이 열거형은 daemon.c의 레거시 pv_log() 함수에서 사용된다.
 *   프로덕션에서는 src/utils/pcv_log.h의 PCV_LOG_INFO/WARN/ERR을 사용하며,
 *   GLib의 g_message/g_warning/g_critical과 통합되어 있다.
 */
enum LogLevel {
#define X(name, str) name,
    LOG_LEVELS
#undef X
};

/* ── 싱글턴 접근자 ─────────────────────────────────────────── */

/**
 * @brief 전역 유일한 데몬 컨텍스트를 반환한다
 *
 * daemon.c 내부의 static PvContext g_ctx의 주소를 반환한다.
 * 항상 non-NULL이며, pv_init() 호출 전에도 유효한 포인터이지만
 * 내부 멤버는 초기화되지 않은 상태(0으로 초기화)이다.
 *
 * @return PvContext* — 전역 싱글턴 인스턴스 (항상 non-NULL)
 */
PvContext* pv_get_instance(void);

/* ── 라이프사이클 API ──────────────────────────────────────── */

/**
 * @brief 엔진 코어 초기화 — libvirt 연결 + GMainLoop + 시그널 핸들러 등록
 *
 * [4단계 초기화]
 *   1. virEventRegisterDefaultImpl(): libvirt 이벤트를 GLib 이벤트 루프에 통합
 *   2. virConnectOpen("qemu:///system"): KVM 하이퍼바이저에 연결
 *   3. g_main_loop_new(): GLib 이벤트 루프 생성
 *   4. g_unix_signal_add(): SIGINT/SIGTERM 핸들러 등록
 *
 * [실패 처리]
 *   어느 단계에서든 실패하면 이미 할당된 자원만 역순으로 해제한다
 *   (goto cleanup 패턴).
 *
 * @return 0=성공, -1=실패
 * @note 프로세스당 1회만 호출할 것. 현재 main.c에서는 사용하지 않는 레거시 경로.
 */
int pv_init(void);

/**
 * @brief 이벤트 루프 진입 (블로킹)
 *
 * g_main_loop_run()을 호출하여 이벤트 대기 상태에 진입한다.
 * SIGINT/SIGTERM으로 g_main_loop_quit()가 호출될 때까지 반환하지 않는다.
 *
 * @pre pv_init()이 성공적으로 호출되어 is_running=1, loop!=NULL이어야 한다.
 */
void pv_run(void);

/**
 * @brief 엔진 자원 해제 — libvirt 연결 닫기 + GMainLoop 해제
 *
 * 초기화의 역순으로 자원을 해제한다:
 *   1. virConnectClose(conn) — libvirt 연결 종료
 *   2. g_main_loop_unref(loop) — 이벤트 루프 메모리 해제
 *
 * NULL 체크 후 해제하므로 이중 호출에도 안전하다 (멱등성).
 * 해제 후 포인터를 NULL로 설정하여 dangling pointer를 방지한다.
 */
void pv_cleanup(void);

/* ── 로깅 ──────────────────────────────────────────────────── */

/**
 * @brief 간단한 stderr 로거 (레거시)
 *
 * [출력 형식]
 *   [PureCVisor] [INFO]  메시지 텍스트\n
 *   [PureCVisor] [ERR ]  에러 메시지\n
 *
 * [printf 호환 가변 인자]
 *   내부적으로 va_start/vfprintf/va_end를 사용한다.
 *   printf와 동일한 포맷 문자열(%s, %d, %f 등)을 지원한다.
 *
 * @param level 로그 레벨 (LOG_INFO, LOG_WARN, LOG_ERR, LOG_DEBUG)
 * @param fmt   printf 호환 포맷 문자열
 * @param ...   가변 인자
 *
 * @note 프로덕션에서는 src/utils/pcv_log.h의 PCV_LOG_INFO/WARN 등을 사용한다.
 *       이 함수는 daemon.c 레거시 코드에서만 사용된다.
 */
void pv_log(int level, const char *fmt, ...);

#endif /* PURECVISOR_CORE_H */
