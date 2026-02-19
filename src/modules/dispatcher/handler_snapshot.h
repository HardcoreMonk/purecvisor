// src/modules/dispatcher/handler_snapshot.h

#ifndef PURECVISOR_HANDLER_SNAPSHOT_H
#define PURECVISOR_HANDLER_SNAPSHOT_H

#include <glib.h>
#include <gio/gio.h>                 // GSocketConnection을 위해 추가
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* UdsServer 전방 선언 (헤더 순환 참조 방지) */
typedef struct _UdsServer UdsServer;

/**
 * @brief VM ZFS 스냅샷 생성 핸들러 (vm.snapshot.create)
 */
void handle_vm_snapshot_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM ZFS 스냅샷 목록 조회 핸들러 (vm.snapshot.list)
 */
void handle_vm_snapshot_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM ZFS 스냅샷 롤백 핸들러 (vm.snapshot.rollback)
 */
void handle_vm_snapshot_rollback(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM ZFS 스냅샷 삭제 핸들러 (vm.snapshot.delete)
 */
void handle_vm_snapshot_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_SNAPSHOT_H */