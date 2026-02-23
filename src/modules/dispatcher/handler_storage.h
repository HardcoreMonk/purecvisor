// src/modules/dispatcher/handler_storage.h
#ifndef HANDLER_STORAGE_H
#define HANDLER_STORAGE_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

// ZFS Pool 목록 조회
void handle_storage_pool_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

// ZVOL (블록 디바이스) 목록 조회
void handle_storage_zvol_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

// 기존 list 함수들 아래에 추가
void handle_storage_zvol_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_storage_zvol_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#endif // HANDLER_STORAGE_H