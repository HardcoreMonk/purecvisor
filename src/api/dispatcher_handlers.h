/**
 * @file dispatcher_handlers.h
 * @brief 디스패처 인라인 핸들러 함수 선언
 *
 * dispatcher.c에서 분리된 RPC 핸들러 래퍼 함수들입니다.
 * 기능 변경 없이 구조 분리만 수행했습니다.
 *
 * 모든 함수는 PcvDispatchHandler 시그니처를 따릅니다:
 *   void func(JsonObject *params, const gchar *rpc_id,
 *             UdsServer *server, GSocketConnection *connection)
 *
 * vm.create는 self(vm_manager) 접근이 필요하여 dispatcher.c에 남아있습니다.
 */

#ifndef PCV_DISPATCHER_HANDLERS_H
#define PCV_DISPATCHER_HANDLERS_H

#include <json-glib/json-glib.h>
#include <glib.h>
#include <gio/gio.h>
#include "uds_server.h"

/* ── 페이지네이션 헬퍼 (dispatcher.c와 공유) ──────────────────── */
JsonNode *pcv_paginate_array(JsonArray *full_array, gint offset, gint limit);
void pcv_get_pagination_params(JsonObject *params, gint *out_offset, gint *out_limit);

/* ── VM 코어 핸들러 ───────────────────────────────────────────── */
void disp_handle_vm_delete_status(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_vm_resize_disk(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_vm_clone(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_vm_export_ova(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_vm_import_ova(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── 모니터 / 알림 ───────────────────────────────────────────── */
void disp_handle_monitor_fleet(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_monitor_processes(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_history(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_config_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_config_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_config_reload(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_silence(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_silence_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_routing(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_alert_action_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── AI Agent ─────────────────────────────────────────────────── */
void disp_handle_agent_config_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_agent_config_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_agent_history(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_healing_history(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── Cloud Migration ──────────────────────────────────────────── */
void disp_handle_vm_import_ec2(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_vm_export_ec2(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cloud_migration_status(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cloud_jobs_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cloud_job_cancel(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── Auth / API Key ───────────────────────────────────────────── */
void disp_handle_apikey_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_apikey_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_apikey_revoke(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_session_revoke(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── VM Batch / Filter / Pool ─────────────────────────────────── */
void disp_handle_vm_batch(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_vm_list_filtered(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_pool_conninfo(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── Config / Health / DB ─────────────────────────────────────── */
void disp_handle_config_reload(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_health_deep(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_snapshot_verify(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_jobs_persist_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_db_migration_status(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── 컨테이너 확장 ───────────────────────────────────────────── */
void disp_handle_container_snapshot_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_snapshot_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_snapshot_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_clone(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_memory_stats(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_health_check(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_set_limits(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_nic_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_nic_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_nic_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_container_set_bandwidth(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── Jobs ─────────────────────────────────────────────────────── */
void disp_handle_jobs_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_jobs_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_jobs_cancel(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── Prometheus / Webhooks ────────────────────────────────────── */
void disp_handle_prometheus_sd(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_vm_event_webhook_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── 쿼터 ─────────────────────────────────────────────────────── */
void disp_handle_quota_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── 스토리지 확장 ────────────────────────────────────────────── */
void disp_handle_storage_pool_health(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_storage_pool_forecast(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── 보안 그룹 ────────────────────────────────────────────────── */
void disp_handle_security_group_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_security_group_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_security_group_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_security_group_add_rule(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_security_group_apply(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── 스냅샷 스케줄 ────────────────────────────────────────────── */
void disp_handle_snapshot_schedule_status(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── daemon.config ────────────────────────────────────────────── */
void disp_handle_daemon_config_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_daemon_config_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* ── 클러스터 (PCV_CLUSTER_ENABLED) ──────────────────────────── */
#if PCV_CLUSTER_ENABLED
void disp_handle_cluster_maintenance_enter(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_maintenance_exit(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_affinity_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_affinity_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_affinity_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_node_label_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_node_label_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_node_label_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_node_drain(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_node_resume(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_upgrade_status(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_config_push(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void disp_handle_cluster_config_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
#endif /* PCV_CLUSTER_ENABLED */

#endif /* PCV_DISPATCHER_HANDLERS_H */
