/**
 * @file handler_monitor.h
 * @brief 호스트/VM 모니터링 RPC 핸들러 공개 인터페이스
 *
 * [RPC 메서드 매핑] (2개)
 *   monitor.metrics -> handle_monitor_metrics (동기 — 단일 VM 상세 메트릭)
 *   monitor.fleet   -> handle_monitor_fleet   (동기 — 전체 호스트+VM 종합 대시보드)
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [모든 핸들러 동기 응답 — fire-and-forget 미사용]
 *
 * [호출 경로]
 *   dispatcher.c -> handle_monitor_*() -> libvirt virConnectGetAllDomainStats
 *                                      -> /proc/meminfo, /proc/stat, sysinfo()
 *
 * [참고]
 *   monitor.fleet의 출력은 REST /metrics 엔드포인트(Prometheus text format)와
 *   TUI HOST 탭 양쪽에서 소비됩니다. 필드 변경 시 양쪽 호환성을 확인하세요.
 */

#ifndef PURECVISOR_HANDLER_MONITOR_H
#define PURECVISOR_HANDLER_MONITOR_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

void handle_monitor_metrics(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection);

void handle_monitor_fleet(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_MONITOR_H */
