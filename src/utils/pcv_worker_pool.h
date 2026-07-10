/**
 * @file pcv_worker_pool.h
 * @brief 제한된 GThreadPool 기반 워커 스레드 풀 — 무거운 비동기 작업용
 *
 * == 설계 의도 ==
 *   GLib의 기본 g_task_run_in_thread()는 공유 스레드 풀을 사용하며
 *   동시 스레드 수에 제한이 없어 리소스 고갈 위험이 있습니다.
 *   이 모듈은 daemon.conf [daemon] worker_threads (기본 8)로 제한된
 *   전용 스레드 풀을 제공하여 vm.create, vm.clone, backup 등
 *   무거운 작업의 동시성을 제어합니다.
 *
 * == 사용법 ==
 *   // 기존 g_task_run_in_thread() 대신:
 *   pcv_worker_pool_push(task, my_worker_func);
 *
 * == 초기화 순서 ==
 *   main.c: pcv_worker_pool_init()  (pcv_config_init 이후)
 *   종료:   pcv_worker_pool_shutdown()
 */

#ifndef PCV_WORKER_POOL_H
#define PCV_WORKER_POOL_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * pcv_worker_pool_init:
 * 제한된 스레드 풀을 생성합니다.
 * daemon.conf [daemon] worker_threads 키로 최대 동시 스레드 수를 설정합니다.
 * main.c에서 pcv_config_init() 이후 1회 호출합니다.
 */
void pcv_worker_pool_init(void);

/**
 * pcv_worker_pool_shutdown:
 * 스레드 풀을 종료합니다. 대기 중인 작업은 완료된 후 종료됩니다.
 * main.c 종료 시 호출합니다.
 */
void pcv_worker_pool_shutdown(void);

/**
 * pcv_worker_pool_push:
 * GTask를 제한된 스레드 풀에서 실행합니다.
 * g_task_run_in_thread()의 대체 함수입니다.
 *
 * @param task  GTask 객체 (ref가 1 증가됨 — 내부에서 unref)
 * @param func  GTaskThreadFunc 워커 함수
 */
void pcv_worker_pool_push(GTask *task, GTaskThreadFunc func);

/**
 * pcv_worker_pool_get_pending:
 * 풀에 대기 중인 작업 수를 반환합니다.
 * Prometheus 메트릭 수집에 사용됩니다.
 *
 * @return 대기 중인 작업 수 (풀 미초기화 시 0)
 */
guint pcv_worker_pool_get_pending(void);

G_END_DECLS

#endif /* PCV_WORKER_POOL_H */
