/**
 * @file handler_container.h
 * @brief LXC 컨테이너 RPC 핸들러 공개 인터페이스
 *
 * [RPC 메서드 매핑] (18개)
 *   container.create            -> handle_container_create            (비동기 콜백)
 *   container.destroy           -> handle_container_destroy           (비동기 콜백)
 *   container.start             -> handle_container_start             (비동기 콜백)
 *   container.stop              -> handle_container_stop              (비동기 콜백)
 *   container.list              -> handle_container_list              (비동기 콜백)
 *   container.metrics           -> handle_container_metrics           (비동기 콜백)
 *   container.exec              -> handle_container_exec              (비동기 콜백)
 *   container.snapshot.create   -> handle_container_snapshot_create   (비동기 콜백)
 *   container.snapshot.rollback -> handle_container_snapshot_rollback (비동기 콜백)
 *   container.snapshot.delete   -> handle_container_snapshot_delete   (비동기 콜백)
 *   container.snapshot.list     -> handle_container_snapshot_list     (비동기 콜백)
 *   container.logs              -> handle_container_logs              (동기)
 *   container.volume.attach     -> handle_container_volume_attach     (동기)
 *   container.volume.detach     -> handle_container_volume_detach     (동기)
 *   container.volume.list       -> handle_container_volume_list       (동기)
 *   container.env.set           -> handle_container_env_set           (동기)
 *   container.env.list          -> handle_container_env_list          (동기)
 *   container.env.delete        -> handle_container_env_delete        (동기)
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [호출 경로]
 *   dispatcher.c -> handle_container_*() -> lxc_driver.c (pcv_lxc_*_async)
 *
 * [주의사항]
 *   - UdsServer 전방 선언(typedef struct _UdsServer UdsServer)으로
 *     헤더 순환 참조를 방지합니다. 실제 정의는 api/uds_server.h에 있습니다.
 */

#ifndef PURECVISOR_HANDLER_CONTAINER_H
#define PURECVISOR_HANDLER_CONTAINER_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

/* ── 생명주기 ──────────────────────────────────────────────────────────── */
void handle_container_create  (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_destroy (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_start   (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_stop    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

/* ── 조회 ──────────────────────────────────────────────────────────────── */
void handle_container_list    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_metrics (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

/* ── Exec ──────────────────────────────────────────────────────────────── */
void handle_container_exec    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

/* ── 스냅샷 ────────────────────────────────────────────────────────────── */
void handle_container_snapshot_create   (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_rollback (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_delete   (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_list     (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

/* ── 로그 ──────────────────────────────────────────────────────────────── */
void handle_container_logs    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

/* ── 볼륨 마운트 ──────────────────────────────────────────────────────── */
void handle_container_volume_attach(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_volume_detach(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_volume_list  (JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

/* ── 환경변수 ──────────────────────────────────────────────────────────── */
void handle_container_env_set    (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

void handle_container_env_list   (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

void handle_container_env_delete (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

/* ── 헬스체크 프로브 ──────────────────────────────────────────────────── */
void handle_container_health_set   (JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_health_get   (JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_health_delete(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_CONTAINER_H */