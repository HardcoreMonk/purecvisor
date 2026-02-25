/* src/main.c */
#include <unistd.h> // geteuid() 함수를 위해 필수!
#include <glib.h>
#include <glib-unix.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <stdio.h>

#include "api/uds_server.h"
#include "api/dispatcher.h"
#include "utils/logger.h"

// Phase 7 신규 코어 모듈 및 데몬 헤더
#include "modules/core/vm_state.h"
#include "modules/core/cpu_allocator.h"
#include "modules/daemons/telemetry.h"
#include "modules/daemons/virt_events.h"

#define SOCKET_PATH "/tmp/purecvisor.sock"

static GMainLoop *loop;

// 🚀 Phase 7: 글로벌 Allocator 선언 (extern으로 다른 파일에서 참조)
// gpointer global_allocator = NULL;
// 🚀 [수정 1] gpointer 대신 헤더에 명시된 정확한 타입 사용
CpuAllocator *global_allocator = NULL;

static gboolean on_signal_received(gpointer user_data) {
    (void)user_data;
    g_message("🛑 Signal received, initiating graceful shutdown...");
    g_main_loop_quit(loop);
    return FALSE;
}

// 🚀 [수정 2] 누락되었던 토폴로지 스캔 함수 추가 (main 함수 위쪽에 배치)
static void scan_and_register_host_topology(CpuAllocator *alloc) {
    g_message("🔍 [Init] Scanning Host Topology and Isolated CPUs...");
    
    // 예시: 0번 NUMA 노드에 속한 4개의 코어 중 2개(2번, 3번)가 격리(Isolated)되었다고 가정
    cpu_allocator_add_core(alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(alloc, 1, 1, 0, FALSE);
    cpu_allocator_add_core(alloc, 2, 2, 0, TRUE);  // 🌟 VM 전용 격리 코어
    cpu_allocator_add_core(alloc, 3, 3, 0, TRUE);  // 🌟 VM 전용 격리 코어
    
    g_message("✅ [Init] Host Topology mapped to In-Memory Allocator.");
}
int main(int argc, char *argv[]) {

    // =================================================================
    // 🛡️ 0단계: Root(관리자) 권한 강제 검증 방어벽
    // =================================================================
    if (geteuid() != 0) {
        // \x1b[31m 은 터미널에 붉은색 글씨를 출력하는 ANSI 표준 코드입니다.
        fprintf(stderr, "\n\x1b[31m[!] CRITICAL ERROR: INSUFFICIENT PRIVILEGES\x1b[0m\n");
        fprintf(stderr, "    The PureCVisor Daemon MUST be run as root.\n");
        fprintf(stderr, "    Please execute using sudo: \x1b[33msudo %s\x1b[0m\n\n", argv[0]);
        exit(EXIT_FAILURE); // 권한이 없으면 자비 없이 즉시 프로세스를 종료합니다.
    }

    GError *error = NULL;

    // 1. Logger & Type System Init
    purecvisor_logger_init();
    
    #if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
    #endif

    gvir_init_object(&argc, &argv);

    g_message("🚀 Starting PureCVisor Engine (Upgrading to Phase 7)...");

    // =================================================================
    // 🌟 [Phase 7 추가 구간] 코어 메모리 상태 및 백그라운드 데몬 초기화
    // =================================================================
    
    // A. Race Condition 방어용 인메모리 Lock 테이블 초기화
    init_pending_state_machine();
    
    // B. NUMA 및 CPU 코어 할당자 초기화
    global_allocator = cpu_allocator_new();
    // 호스트 토폴로지 스캔 및 등록 로직 (추후 구현)
    scan_and_register_host_topology(global_allocator);

    // C. 백그라운드 데몬 스레드 기동
    init_telemetry_daemon();    // 메트릭 폴링 데몬
    init_virt_events_daemon();  // 자가 치유(Self-Healing) 데몬
    
    // =================================================================

    // 2. Libvirt Connection (Main Thread용 가벼운 조회 커넥션)
    // 💡 Phase 7에서는 이 커넥션을 디스패처의 "가벼운 상태 조회" 용도로만 쓰고,
    // 무거운 구동/핫플러그 작업은 워커 스레드 내부에서 Raw API로 새로 맺습니다.
    GVirConnection *conn = gvir_connection_new("qemu:///system");
    if (!gvir_connection_open(conn, NULL, &error)) {
        g_critical("Failed to connect to libvirt: %s", error->message);
        g_error_free(error);
        return 1;
    }

    // 3. Components Init
    PureCVisorDispatcher *dispatcher = purecvisor_dispatcher_new();
    purecvisor_dispatcher_set_connection(dispatcher, conn);

    // UDS Server 생성
    UdsServer *server = uds_server_new(SOCKET_PATH);
    
    // Dispatcher 연결
    uds_server_set_dispatcher(server, dispatcher);

    // 4. Start Server
    if (!uds_server_start(server, &error)) {
        g_critical("Failed to start UDS server: %s", error->message);
        g_error_free(error);
        return 1;
    }

    // 5. Main Loop
    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_signal_received, NULL);
    g_unix_signal_add(SIGTERM, on_signal_received, NULL);

    g_message("⚡ Daemon is running. Waiting for requests...");
    g_main_loop_run(loop);

    // 6. Cleanup
    g_message("🧹 Cleaning up resources before exit...");
    g_object_unref(server);
    g_object_unref(dispatcher);
    g_object_unref(conn);
    g_main_loop_unref(loop);

    // alloc 해제 등 Phase 7 클린업 코드 추가 가능

    g_message("👋 PureCvisor Engine exited cleanly.");
    return 0;
}