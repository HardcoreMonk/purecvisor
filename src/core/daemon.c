/**
 * @file daemon.c
 * @brief PureCVisor 엔진 코어 — libvirt 연결 + GMainLoop + 시그널 핸들링
 *
 * ====================================================================
 *  아키텍처에서의 위치
 * ====================================================================
 *  src/main.c가 이 모듈의 pv_init() / pv_run() / pv_cleanup()을 순서대로 호출합니다.
 *  main.c가 "무엇을 초기화할지" 결정하고, daemon.c는 "엔진 코어를 어떻게 초기화할지"를
 *  담당합니다. 현재는 main.c에서 GMainLoop를 직접 관리하므로 이 파일은 레거시 경로입니다.
 *
 *  [다른 모듈과의 관계]
 *    - include/purecvisor/core.h: 이 파일의 함수 선언 (공개 API)
 *    - src/main.c: 실제 운영 진입점 (이 파일 대신 16단계 직접 초기화)
 *    - src/modules/virt/virt_conn_pool.c: 프로덕션 libvirt 연결 풀 (이 파일의
 *      단일 연결 대신 풀링된 연결 사용)
 *    - src/api/drain.c: 프로덕션 graceful shutdown (이 파일의 단순 quit 대신)
 *
 * ====================================================================
 *  주요 흐름
 * ====================================================================
 *  pv_init()
 *    1. virEventRegisterDefaultImpl() — libvirt 이벤트를 GLib 이벤트 루프에 연결
 *       [왜 필요한가?]
 *         libvirt는 내부적으로 이벤트 핸들링(도메인 상태 변경 알림 등)을
 *         수행합니다. 기본 구현을 등록하면 GMainLoop의 poll에서
 *         libvirt 이벤트도 함께 처리됩니다.
 *    2. virConnectOpen("qemu:///system") — KVM 하이퍼바이저 연결
 *       [qemu:///system의 의미]
 *         qemu:// = QEMU/KVM 드라이버 사용
 *         ///system = 시스템 모드 (root 권한의 전체 VM 관리)
 *         ///session = 사용자 모드 (비특권, 제한된 기능)
 *    3. GMainLoop 생성
 *    4. SIGINT/SIGTERM 시그널 핸들러 등록 (g_unix_signal_add)
 *  pv_run()
 *    g_main_loop_run() — 이벤트 루프 진입 (블로킹)
 *  pv_cleanup()
 *    virConnectClose() → g_main_loop_unref() (초기화 역순 해제)
 *
 * ====================================================================
 *  핵심 설계 패턴
 * ====================================================================
 *  [Opaque Pointer]
 *    PvContext 구조체 정의를 이 .c 파일에 숨겨 헤더에는 포인터만 노출합니다.
 *    외부 모듈은 pv_get_instance()로 접근하되 내부 멤버에 직접 접근 불가합니다.
 *    이로써 구조체 멤버를 변경해도 이 헤더를 include하는 코드를 재컴파일할
 *    필요가 없습니다 (ABI 안정성).
 *
 *  [goto Cleanup]
 *    pv_init()에서 실패 시 goto로 이미 할당된 자원만 역순 해제합니다.
 *    C 프로젝트에서 에러 처리의 표준 관용구이며, Linux 커널에서도 널리 사용됩니다.
 *    try-catch가 없는 C에서 자원 누수를 방지하는 가장 깔끔한 방법입니다.
 *
 *  [싱글턴]
 *    g_ctx 정적 변수로 프로세스당 하나의 엔진 인스턴스만 존재합니다.
 *    {0}으로 제로 초기화하여 pv_init() 호출 전에도 안전한 상태입니다.
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - pv_log()는 이 파일에 정의된 간단한 stderr 로거입니다.
 *    프로덕션에서는 src/utils/pcv_log.h의 구조화된 로거(PCV_LOG_INFO 등)를 사용합니다.
 *  - virConnectOpen()은 블로킹 호출이므로 초기화 시에만 사용합니다.
 *    런타임에는 virt_conn_pool.c의 풀링된 연결을 사용합니다.
 *  - 이 파일은 레거시 경로입니다. 실제 운영에서는 main.c의 16단계 초기화가
 *    이 파일의 역할을 대체합니다.
 */
// src/core/daemon.c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <glib-unix.h> /* g_unix_signal_add() — UNIX 시그널을 GMainLoop 이벤트로 변환 */
#include <libvirt-glib/libvirt-glib.h> /* libvirt-gobject 초기화 (gvir_init) */
#include "purecvisor/core.h"

/*
 * [Opaque Pointer 구현]
 *
 * PvContext 구조체의 정의가 이 .c 파일에만 존재합니다.
 * core.h에서는 "typedef struct PvContext PvContext;"로 전방 선언만 합니다.
 * 따라서 외부 코드에서 ctx->loop 같은 직접 접근은 컴파일 에러가 됩니다.
 *
 * [각 멤버의 역할]
 *   loop:       GLib 이벤트 루프 — 모든 I/O와 타이머가 여기서 디스패치됨
 *   conn:       libvirt C API 연결 핸들 — virDomain* 함수 호출에 사용
 *   is_running: 엔진 상태 플래그 — pv_init 성공 시 1, pv_run 진입 조건
 */
struct PvContext {
    GMainLoop *loop;       /**< GLib 이벤트 루프 (프로그램의 심장) */
    virConnectPtr conn;    /**< libvirt 하이퍼바이저 연결 (qemu:///system) */
    int is_running;        /**< 엔진 실행 상태 (0=미초기화, 1=실행 중) */
};

/**
 * 프로세스 전역 엔진 컨텍스트 — 제로 초기화 (싱글턴)
 *
 * [= {0}의 의미]
 *   C11에서 = {0}은 모든 멤버를 0/NULL로 초기화합니다.
 *   loop=NULL, conn=NULL, is_running=0 상태입니다.
 *   pv_init() 호출 전에도 pv_get_instance()가 유효한 포인터를 반환할 수 있습니다.
 */
static PvContext g_ctx = {0};

/**
 * @brief 엔진 컨텍스트 싱글턴 접근자
 *
 * 외부 모듈(main.c 등)이 PvContext 내부에 직접 접근할 수 없으므로
 * 이 함수를 통해 포인터만 반환합니다 (Opaque Pointer 패턴).
 *
 * [사용 예시]
 *   PvContext *ctx = pv_get_instance();
 *   // ctx->loop;  ← 컴파일 에러! (구조체 정의가 보이지 않음)
 *
 * @return 전역 PvContext 인스턴스의 주소 (항상 non-NULL, 정적 변수이므로)
 */
PvContext* pv_get_instance(void) {
    return &g_ctx;
}

/**
 * @brief SIGINT/SIGTERM 시그널 핸들러 (GLib 시그널 콜백)
 *
 * GLib의 g_unix_signal_add()로 등록되어 SIGINT 또는 SIGTERM 수신 시 호출됩니다.
 * GMainLoop를 종료(quit)하여 pv_run()이 반환되도록 합니다.
 *
 * [GLib 시그널 처리의 안전성]
 *   g_unix_signal_add()는 내부적으로 self-pipe trick을 사용합니다.
 *   실제 UNIX 시그널 핸들러는 파이프에 바이트를 쓰기만 하고(async-signal-safe),
 *   GMainLoop가 파이프를 poll하여 이 콜백을 메인 스레드에서 호출합니다.
 *   따라서 이 콜백에서 g_main_loop_quit() 같은 async-signal-unsafe 함수를
 *   안전하게 호출할 수 있습니다.
 *
 * @param user_data PvContext 포인터 (pv_init에서 g_unix_signal_add 시 전달)
 * @return G_SOURCE_REMOVE — 시그널 소스를 GMainLoop에서 제거 (1회성 핸들러)
 *
 * @note G_SOURCE_REMOVE는 FALSE와 동일합니다 (GLib 매크로).
 *       TRUE를 반환하면 시그널 핸들러가 유지되어 다음 시그널도 처리합니다.
 */
static gboolean on_signal(gpointer user_data) {
    PvContext *ctx = (PvContext *)user_data;
    pv_log(LOG_INFO, "Signal received, stopping engine...");
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
}

/**
 * @brief 엔진 코어 초기화 — libvirt 연결 + GMainLoop + 시그널 핸들러 등록
 *
 * 4단계 초기화를 수행합니다:
 *   1. libvirt 이벤트 핸들러 등록 (GLib 이벤트 루프와 통합)
 *   2. qemu:///system 하이퍼바이저 연결
 *   3. GMainLoop 생성
 *   4. SIGINT/SIGTERM 핸들러 등록
 *
 * [goto Cleanup 패턴 상세]
 *   각 단계의 실패 처리에서 goto 라벨이 다릅니다:
 *     1단계 실패 → err_return (해제할 자원 없음)
 *     2단계 실패 → err_return (해제할 자원 없음)
 *     3단계 실패 → err_conn (conn만 해제)
 *   이렇게 하면 실패 시점에 따라 해제해야 할 자원만 정확히 해제됩니다.
 *   C++ RAII나 Go의 defer와 유사한 역할을 합니다.
 *
 * @return 0 성공, -1 실패
 *
 * @note 현재 main.c에서 GMainLoop를 직접 관리하므로 이 함수는 레거시 경로입니다.
 *       프로덕션에서는 main.c의 16단계 초기화 시퀀스가 이 역할을 대체합니다.
 */
int pv_init(void) {
    pv_log(LOG_INFO, "Initializing PureCVisor-engine (C11)...");

    /*
     * 1단계: Libvirt 이벤트 등록
     *
     * libvirt 라이브러리가 내부적으로 사용하는 이벤트 루프 구현체를 등록합니다.
     * virEventRegisterDefaultImpl()은 poll() 기반의 기본 구현을 제공합니다.
     * 이후 GMainLoop에서 libvirt 이벤트(도메인 상태 변경 등)도 함께 처리됩니다.
     *
     * [실패 원인]
     *   - libvirt 라이브러리가 설치되지 않은 경우 (링크 에러로 여기까지 도달 불가)
     *   - 내부 초기화 실패 (극히 드묾)
     */
    if (virEventRegisterDefaultImpl() < 0) {
        pv_log(LOG_ERR, "Failed to register libvirt event impl");
        goto err_return;
    }

    /*
     * 2단계: 하이퍼바이저 연결
     *
     * virConnectOpen()은 libvirtd 데몬에 연결합니다.
     * "qemu:///system"은 시스템 수준 QEMU/KVM 드라이버를 의미합니다.
     *
     * [블로킹 호출]
     *   이 함수는 libvirtd와의 소켓 연결이 완료될 때까지 블로킹됩니다 (~50ms).
     *   런타임에는 virt_conn_pool.c의 풀링된 연결을 사용하므로
     *   이 블로킹은 초기화 시 1회만 발생합니다.
     *
     * [실패 원인]
     *   - libvirtd 서비스가 실행되지 않는 경우
     *   - QEMU/KVM이 설치되지 않은 경우
     *   - 권한 부족 (root가 아닌 경우)
     */
    g_ctx.conn = virConnectOpen("qemu:///system");
    if (!g_ctx.conn) {
        pv_log(LOG_ERR, "Failed to connect to qemu:///system");
        goto err_return;
    }
    pv_log(LOG_INFO, "Connected to Hypervisor");

    /*
     * 3단계: GMainLoop 생성
     *
     * [파라미터 설명]
     *   NULL: 기본 GMainContext를 사용 (프로세스당 하나의 기본 컨텍스트)
     *   FALSE: is_running 초기값 (g_main_loop_run() 호출 시 TRUE로 전환)
     *
     * [GMainLoop vs GMainContext]
     *   GMainContext: 이벤트 소스(GSource)를 관리하는 컨테이너
     *   GMainLoop: GMainContext를 반복적으로 순회(iterate)하는 루프
     *   하나의 Context에 여러 Loop가 연결될 수 있지만, 보통 1:1입니다.
     */
    g_ctx.loop = g_main_loop_new(NULL, FALSE);
    if (!g_ctx.loop) {
        pv_log(LOG_ERR, "Failed to create GLib Main Loop");
        goto err_conn;  /* conn은 이미 열려있으므로 해제 필요 */
    }

    /*
     * 4단계: 시그널 핸들러 등록
     *
     * SIGINT(Ctrl+C)와 SIGTERM(systemctl stop)을 GMainLoop 이벤트로 변환합니다.
     * on_signal 콜백이 호출되면 g_main_loop_quit()로 루프를 종료합니다.
     *
     * [반환값을 사용하지 않는 이유]
     *   g_unix_signal_add()는 GSource ID를 반환합니다.
     *   이 ID로 g_source_remove()를 호출하여 핸들러를 제거할 수 있지만,
     *   시그널 핸들러는 프로그램 종료 시까지 유효하므로 ID를 저장하지 않습니다.
     */
    g_unix_signal_add(SIGINT, on_signal, &g_ctx);
    g_unix_signal_add(SIGTERM, on_signal, &g_ctx);

    g_ctx.is_running = 1;  /* 초기화 완료 플래그 — pv_run()의 진입 조건 */
    return 0;

err_conn:
    virConnectClose(g_ctx.conn);  /* conn이 열려있으므로 해제 */
    g_ctx.conn = NULL;
err_return:
    return -1;
}

/**
 * @brief 엔진 이벤트 루프 진입 (블로킹)
 *
 * g_main_loop_run()을 호출하여 이벤트 대기 상태에 진입합니다.
 * 이 함수는 SIGINT/SIGTERM으로 on_signal()이 g_main_loop_quit()를
 * 호출할 때까지 반환하지 않습니다.
 *
 * [내부 동작]
 *   g_main_loop_run()은 다음을 무한 반복합니다:
 *     1. GMainContext의 모든 GSource를 순회하여 이벤트 확인
 *     2. 이벤트가 없으면 poll()로 블로킹 대기
 *     3. 이벤트 발생 시 해당 콜백 호출
 *     4. g_main_loop_is_running()이 FALSE이면 반환
 *
 * @pre pv_init()이 성공적으로 호출되어 g_ctx.is_running=1, loop!=NULL이어야 합니다.
 *      조건을 만족하지 않으면 아무 일도 하지 않고 즉시 반환합니다.
 */
void pv_run(void) {
    if (g_ctx.is_running && g_ctx.loop) {
        pv_log(LOG_INFO, "Engine Loop Started.");
        g_main_loop_run(g_ctx.loop);  /* 시그널 수신까지 블로킹 */
    }
}

/**
 * @brief 엔진 자원 해제 — 초기화 역순으로 정리
 *
 * libvirt 연결을 닫고 GMainLoop 참조를 해제합니다.
 *
 * [멱등성(Idempotency)]
 *   각 자원에 대해 NULL 체크 후 해제하므로 이중 호출에도 안전합니다.
 *   해제 후 포인터를 NULL로 설정하여 dangling pointer를 방지합니다.
 *   이 패턴은 "safe delete" 또는 "idempotent cleanup"이라 불립니다.
 *
 * [참조 카운트]
 *   virConnectClose(): libvirt 내부 레퍼런스 카운트를 1 감소시킵니다.
 *     다른 코드가 이 연결을 참조하고 있으면 실제 해제가 지연됩니다.
 *   g_main_loop_unref(): GMainLoop의 참조 카운트를 1 감소시킵니다.
 *     카운트가 0이 되면 메모리가 실제로 해제됩니다.
 */
void pv_cleanup(void) {
    pv_log(LOG_INFO, "Cleaning up engine resources...");
    if (g_ctx.conn) {
        virConnectClose(g_ctx.conn);  /* libvirt 연결 해제 — 내부 레퍼런스 카운트 감소 */
        g_ctx.conn = NULL;            /* dangling pointer 방지 */
    }
    if (g_ctx.loop) {
        g_main_loop_unref(g_ctx.loop);  /* GMainLoop 참조 카운트 감소 → 0이면 메모리 해제 */
        g_ctx.loop = NULL;              /* dangling pointer 방지 */
    }
}

/**
 * @brief 간단한 stderr 로거 (레거시)
 *
 * 포맷 문자열과 가변 인자를 받아 stderr에 "[PureCVisor] [LEVEL] message\n" 형식으로 출력합니다.
 * 이 함수는 daemon.c 내부에서만 사용되는 간이 로거입니다.
 *
 * [가변 인자(variadic) 처리 흐름]
 *   1. va_list args: 가변 인자 목록을 가리키는 포인터
 *   2. va_start(args, fmt): args를 fmt 다음 인자 위치로 초기화
 *   3. vfprintf(stderr, fmt, args): args에서 인자를 순서대로 꺼내어 포맷팅
 *   4. va_end(args): args를 무효화 (스택 정리)
 *   반드시 va_start와 va_end를 쌍으로 호출해야 합니다.
 *
 * [switch-case의 default 처리]
 *   알 수 없는 level 값이 전달되면 "UNKNOWN" 문자열을 사용합니다.
 *   level_str을 "UNKNOWN"으로 초기화해두었으므로 switch에 default가
 *   없어도 안전합니다 (초기값이 폴백 역할).
 *
 * @param level 로그 레벨 (LOG_INFO=정보, LOG_ERR=에러, LOG_WARN=경고, LOG_DEBUG=디버그)
 * @param fmt   printf 호환 포맷 문자열
 * @param ...   가변 인자
 *
 * @note 프로덕션에서는 src/utils/pcv_log.h의 PCV_LOG_INFO/PCV_LOG_WARN 등
 *       구조화된 로거를 사용합니다. 이 함수는 daemon.c 레거시 코드용입니다.
 */
void pv_log(int level, const char *fmt, ...) {
    va_list args;
    const char *level_str = "UNKNOWN";  /* 기본값 — 알 수 없는 level에 대한 폴백 */

    switch(level) {
        case LOG_INFO: level_str = "[INFO]"; break;
        case LOG_ERR:  level_str = "[ERR ]"; break;
        // ... X-Macro expansion possible here
        // LOG_WARN, LOG_DEBUG를 추가하려면 case를 추가하면 됩니다.
        // 또는 core.h의 LOG_LEVELS X-Macro로 자동 생성할 수도 있습니다.
    }

    fprintf(stderr, "[PureCVisor] %s ", level_str);
    va_start(args, fmt);        /* 가변 인자 목록 초기화 */
    vfprintf(stderr, fmt, args); /* 포맷팅하여 stderr에 출력 */
    va_end(args);                /* 가변 인자 목록 정리 (필수) */
    fprintf(stderr, "\n");       /* 줄바꿈으로 로그 라인 구분 */
}
