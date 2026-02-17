/* src/main.c */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

// Phase 2: 새로운 헤더 경로 사용
#include "api/uds_server.h"
#include "api/dispatcher.h" 

// 전역 루프 변수 (시그널 핸들러용)
static GMainLoop *loop = NULL;

// 시그널 핸들러 (Ctrl+C)
static void signal_handler(int signo) {
    if (loop && g_main_loop_is_running(loop)) {
        g_message("Caught signal %d, stopping...", signo);
        g_main_loop_quit(loop);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    g_message("Starting PureCVisor Engine (Phase 2)...");

    // 1. 시그널 처리 설정
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 2. 메인 루프 생성
    loop = g_main_loop_new(NULL, FALSE);

    // 3. UDS 서버 시작 (내부적으로 Dispatcher 포함)
    // 주의: uds_server_new 내부에서 이미 start가 수행됨
    UdsServer *server = uds_server_new("/tmp/purecvisor.sock");
    if (!server) {
        g_error("Failed to start UDS Server");
        return 1;
    }

    g_message("Engine is running. Listening on /tmp/purecvisor.sock");
    
    // 4. 이벤트 루프 실행 (Blocking)
    g_main_loop_run(loop);

    // 5. 종료 및 정리
    g_message("Engine stopping...");
    uds_server_free(server);
    g_main_loop_unref(loop);

    return 0;
}