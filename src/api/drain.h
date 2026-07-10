/**
 * @file drain.h
 * @brief Graceful Drain 공개 인터페이스 — 안전한 프로세스 종료 보장
 *
 * 아키텍처 위치:
 *   main.c, uds_server.c에서 include합니다.
 *   main.c: pcv_drain_init() → pcv_drain_begin() → pcv_drain_shutdown()
 *   uds_server.c: pcv_drain_inc() / pcv_drain_dec() / pcv_drain_is_shutdown()
 *
 * 호출 순서 (반드시 이 순서를 지킬 것):
 *   [기동 시]
 *     1. pcv_drain_init()            — main.c에서 GMainLoop 시작 전 1회 호출
 *                                      뮤텍스/조건변수 초기화, inflight=0, shutdown=0
 *
 *   [운영 중 — 매 요청마다]
 *     2. pcv_drain_inc()             — uds_server.c: 요청 디스패치 직전 호출
 *                                      FALSE 반환 시 → 요청 거부 (-32000 에러 응답)
 *     3. pcv_drain_dec()             — uds_server.c: 응답 전송 직후 (단일 경로)
 *                                      inflight가 0이 되면 drain 스레드를 깨움
 *
 *   [종료 시 — SIGTERM/SIGINT 수신]
 *     4. pcv_drain_begin(loop, 30)   — on_signal_received()에서 호출
 *        내부 동작:
 *          a. shutdown_flag = 1 설정 (이후 pcv_drain_inc()는 FALSE 반환)
 *          b. pcv_drain_notify_stopping() → $NOTIFY_SOCKET에 "STOPPING=1" 전송
 *          c. drain 스레드 기동 → inflight==0 대기 (최대 30초)
 *          d. 대기 완료 또는 타임아웃 → g_main_loop_quit()
 *
 *   [cleanup]
 *     5. pcv_drain_shutdown()        — main.c cleanup 블록에서 호출
 *                                      drain 스레드 join + 뮤텍스/조건변수 해제
 *
 * 신규 요청 차단 메커니즘:
 *   pcv_drain_is_shutdown() == TRUE이면 uds_server.c가
 *   JSON-RPC -32000 "Server shutting down" 에러를 즉시 반환합니다.
 *
 * sd_notify 지원:
 *   pcv_drain_notify_stopping(): "STOPPING=1" 전송 (systemd Type=notify 서비스)
 *   pcv_drain_notify_ready():    "READY=1" 전송 (기동 완료 알림)
 *   libsystemd 링크 없이 Unix datagram 소켓으로 직접 전송합니다.
 */

#ifndef PURECVISOR_DRAIN_H
#define PURECVISOR_DRAIN_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pcv_drain_init:
 * main.c 에서 GMainLoop 시작 전 1회 호출.
 */
void pcv_drain_init(void);

/**
 * pcv_drain_inc:
 * 요청 처리 시작 시 inflight 카운터를 1 증가.
 * 종료 중(shutdown)이면 FALSE 반환 → 호출자는 요청을 거부해야 함.
 */
gboolean pcv_drain_inc(void);

/**
 * pcv_drain_dec:
 * 요청 처리 완료(응답 전송) 시 inflight 카운터를 1 감소.
 * inflight 가 0 이 되면 대기 중인 drain 스레드를 깨웁니다.
 */
void pcv_drain_dec(void);

/**
 * pcv_drain_is_shutdown:
 * 종료 플래그 확인 (uds_server.c 요청 수락 전 체크).
 */
gboolean pcv_drain_is_shutdown(void);

/**
 * pcv_drain_get_inflight:
 * 현재 처리 중인 요청 수 반환 (진단/로그용).
 */
gint pcv_drain_get_inflight(void);

/**
 * pcv_drain_notify_stopping:
 * systemd $NOTIFY_SOCKET 에 "STOPPING=1" 을 전송합니다.
 * libsystemd 없이 Unix datagram 소켓으로 직접 전송 (이식성 우선).
 * NOTIFY_SOCKET 환경변수가 없으면 무시합니다.
 */
void pcv_drain_notify_stopping(void);

/**
 * pcv_drain_begin:
 * SIGTERM/SIGINT 핸들러에서 호출.
 * shutdown 플래그 설정 → sd_notify → drain 스레드 기동.
 *
 * @param loop  drain 완료(또는 타임아웃) 후 quit() 할 GMainLoop
 * @param timeout_sec  최대 대기 시간 (권장: 30)
 */
void pcv_drain_begin(GMainLoop *loop, guint timeout_sec);

/**
 * pcv_drain_notify_ready:
 * systemd $NOTIFY_SOCKET 에 "READY=1" 을 전송합니다.
 * Type=notify 서비스에서 데몬 기동 완료 시 1회 호출.
 */
void pcv_drain_notify_ready(void);

/**
 * pcv_drain_notify_watchdog:
 * systemd $NOTIFY_SOCKET 에 "WATCHDOG=1" 을 전송합니다.
 * WatchdogSec 설정 시 주기적으로 호출하여 서비스 건강 상태를 알립니다.
 * NOTIFY_SOCKET 환경변수가 없으면 무시합니다.
 */
void pcv_drain_notify_watchdog(void);

/**
 * pcv_drain_get_watchdog_usec:
 * systemd WATCHDOG_USEC 환경변수를 파싱하여 watchdog 타임아웃(마이크로초)을 반환합니다.
 * 미설정 또는 0이면 0을 반환합니다 (watchdog 비활성).
 */
guint64 pcv_drain_get_watchdog_usec(void);

/**
 * pcv_drain_cancel:
 * drain 상태를 취소하고 서비스를 재개합니다.
 * hot_reload.c에서 execve() 실패 시 호출하여 즉시 요청 수락을 복원합니다.
 * shutdown_flag를 0으로 리셋하여 pcv_drain_inc()가 다시 TRUE를 반환하도록 합니다.
 */
void pcv_drain_cancel(void);

/**
 * pcv_drain_shutdown:
 * main.c cleanup 에서 drain 스레드 join + 자원 해제.
 */
void pcv_drain_shutdown(void);

G_END_DECLS

#endif /* PURECVISOR_DRAIN_H */
