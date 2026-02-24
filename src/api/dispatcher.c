/* src/api/dispatcher.c */

#include "dispatcher.h"
#include "uds_server.h"
#include "../modules/virt/vm_manager.h"
#include <json-glib/json-glib.h>
#include <glib.h>
#include "../modules/dispatcher/handler_snapshot.h"
#include "../modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vnc.h"
#include "modules/dispatcher/handler_vm_start.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/network/network_manager.h"
#include "modules/dispatcher/handler_storage.h"

struct _PureCVisorDispatcher {
    GObject parent_instance;
    PureCVisorVmManager *vm_manager;
};

G_DEFINE_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, G_TYPE_OBJECT)

/* Context for async callbacks */
typedef struct {
    PureCVisorDispatcher *dispatcher;
    gint request_id;
    UdsServer *server;
    GSocketConnection *connection;
} DispatcherRequestContext;

static void dispatcher_request_context_free(DispatcherRequestContext *ctx) {
    if (ctx->dispatcher) g_object_unref(ctx->dispatcher);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static void _send_json_response(DispatcherRequestContext *ctx, JsonBuilder *builder) {
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gsize len;
    gchar *json_str = json_generator_to_data(gen, &len);
    
    // Add newline delimiter
    GString *msg = g_string_new(json_str);
    g_string_append_c(msg, '\n');

    pure_uds_server_send_response(ctx->server, ctx->connection, msg->str);

    g_string_free(msg, TRUE);
    g_free(json_str);
    g_object_unref(gen);
    g_object_unref(builder);
    json_node_free(root);
}

static void _send_error(DispatcherRequestContext *ctx, int code, const char *message) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    
    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, message);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "id");
    if (ctx->request_id != -1)
        json_builder_add_int_value(builder, ctx->request_id);
    else
        json_builder_add_null_value(builder);
    
    json_builder_end_object(builder);
    _send_json_response(ctx, builder);
}

static void _send_success_bool(DispatcherRequestContext *ctx, gboolean result) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    
    json_builder_set_member_name(builder, "result");
    json_builder_add_boolean_value(builder, result);

    json_builder_set_member_name(builder, "id");
    json_builder_add_int_value(builder, ctx->request_id);
    json_builder_end_object(builder);
    _send_json_response(ctx, builder);
}

/* --- Callbacks --- */

static void on_create_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = (DispatcherRequestContext *)user_data;
    GError *err = NULL;
    
    if (purecvisor_vm_manager_create_vm_finish(PURECVISOR_VM_MANAGER(source), res, &err)) {
        _send_success_bool(ctx, TRUE);
    } else {
        _send_error(ctx, -32000, err ? err->message : "Create failed");
        if (err) g_error_free(err);
    }
    dispatcher_request_context_free(ctx);
}

/* --- Handlers --- */

static void handle_vm_create(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    if (!json_object_has_member(params, "name")) {
        _send_error(ctx, -32602, "Missing parameter: name");
        dispatcher_request_context_free(ctx);
        return;
    }

    const gchar *name = json_object_get_string_member(params, "name");
    gint vcpu = 1;
    gint memory_mb = 1024;
    gint disk_size_gb = 50;
    const gchar *iso_path = NULL;
    const gchar *bridge = NULL;

    if (json_object_has_member(params, "vcpu")) vcpu = json_object_get_int_member(params, "vcpu");
    if (json_object_has_member(params, "memory_mb")) memory_mb = json_object_get_int_member(params, "memory_mb");
    if (json_object_has_member(params, "disk_size_gb")) disk_size_gb = json_object_get_int_member(params, "disk_size_gb");
    if (json_object_has_member(params, "iso_path")) iso_path = json_object_get_string_member(params, "iso_path");
    if (json_object_has_member(params, "network_bridge")) {
        bridge = json_object_get_string_member(params, "network_bridge");
    }

    purecvisor_vm_manager_create_vm_async(self->vm_manager, 
                                          name, 
                                          vcpu, 
                                          memory_mb,
                                          disk_size_gb,  
                                          iso_path,
                                          bridge,   
                                          on_create_finished, 
                                          ctx);
}



/* --- Initialization --- */

static void purecvisor_dispatcher_finalize(GObject *object) {
    PureCVisorDispatcher *self = PURECVISOR_DISPATCHER(object);
    if (self->vm_manager) g_object_unref(self->vm_manager);
    G_OBJECT_CLASS(purecvisor_dispatcher_parent_class)->finalize(object);
}

static void purecvisor_dispatcher_class_init(PureCVisorDispatcherClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_dispatcher_finalize;
}

static void purecvisor_dispatcher_init(PureCVisorDispatcher *self) {
    self->vm_manager = purecvisor_vm_manager_new(NULL); 
}

PureCVisorDispatcher *purecvisor_dispatcher_new(void) {
    return g_object_new(PURECVISOR_TYPE_DISPATCHER, NULL);
}

void purecvisor_dispatcher_set_connection(PureCVisorDispatcher *self, GVirConnection *conn) {
    if (self->vm_manager) g_object_unref(self->vm_manager);
    self->vm_manager = purecvisor_vm_manager_new(conn);
}

void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self, 
                                   UdsServer *server, 
                                   GSocketConnection *connection, 
                                   const gchar *request_json) {
    JsonParser *parser = json_parser_new();
    GError *err = NULL;

    if (!json_parser_load_from_data(parser, request_json, -1, &err)) {
        g_error_free(err);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    
    const gchar *method = json_object_get_string_member(obj, "method");
    
    // --- ID 추출 로직 강화 (정수형/문자열 모두 지원) ---
    gint id = -1;
    gchar *rpc_id_str = NULL; // Phase 6 헬퍼용 문자열 ID
    
    if (json_object_has_member(obj, "id")) {
        JsonNode *id_node = json_object_get_member(obj, "id");
        if (json_node_get_value_type(id_node) == G_TYPE_STRING) {
            rpc_id_str = g_strdup(json_node_get_string(id_node));
            id = 0; // 문자열 ID일 경우 Phase 5 구조체를 위해 더미 값 할당
        } else {
            id = json_node_get_int(id_node);
            rpc_id_str = g_strdup_printf("%d", id);
        }
    }

    JsonObject *params = NULL;
    if (json_object_has_member(obj, "params")) {
        params = json_object_get_object_member(obj, "params");
    }

    DispatcherRequestContext *ctx = g_new0(DispatcherRequestContext, 1);
    ctx->dispatcher = g_object_ref(self);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->request_id = id;

    // --- 라우팅 ---
    
    if (g_strcmp0(method, "vm.create") == 0) {
        handle_vm_create(self, params, ctx);
    } else if (g_strcmp0(method, "vm.start") == 0) {
        // 우리가 만든 최신 비동기 & Lock-Free 스케줄러 핸들러로 교체!
        handle_vm_start_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
        // handle_vm_start(self, params, ctx);
    // 🚀 [수정됨] 새롭게 추가된 생명주기 API 라우팅
    } else if (g_strcmp0(method, "vm.stop") == 0) {
        handle_vm_stop_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "vm.delete") == 0) {
        handle_vm_delete_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "vm.list") == 0) {
        handle_vm_list_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "vm.snapshot.create") == 0) {
        handle_vm_snapshot_create(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx); // Phase 6는 자체 context(RpcAsyncContext)를 쓰므로 메모리 해제
    } else if (g_strcmp0(method, "vm.snapshot.list") == 0) {
        handle_vm_snapshot_list(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "vm.snapshot.rollback") == 0) {
        handle_vm_snapshot_rollback(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);

    // 🚀 [수정됨] 새롭게 분리된 핫플러그 API 라우팅
    } else if (g_strcmp0(method, "vm.set_memory") == 0) {
        handle_vm_set_memory_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "vm.set_vcpu") == 0) {
        handle_vm_set_vcpu_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);

    } else if (g_strcmp0(method, "vm.snapshot.delete") == 0) {
        handle_vm_snapshot_delete(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "network.create") == 0) {
        handle_network_create_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    // 🚀 [추가] 네트워크 삭제 API 라우팅
    } else if (g_strcmp0(method, "network.delete") == 0) {
        handle_network_delete_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    // 🚀 [수정됨] VNC 요청을 else if 체인 안으로 편입하고, Phase 6 파라미터 적용
    } else if (g_strcmp0(method, "get_vnc_info") == 0) {
        handle_vnc_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx); // 비동기로 별도 동작하므로 기존 ctx는 해제

    } else if (g_strcmp0(method, "vm.limit") == 0) {
        handle_vm_limit_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);

    // 🚀 [신규 추가] 실시간 텔레메트리 API 라우팅
    } else if (g_strcmp0(method, "vm.metrics") == 0) {
        handle_vm_metrics_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    
    } else if (g_strcmp0(method, "storage.pool.list") == 0) {
        handle_storage_pool_list_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "storage.zvol.list") == 0) {
        handle_storage_zvol_list_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);

    // ... (기존 storage.pool.list 와 storage.zvol.list 아래에 추가) ...
    } else if (g_strcmp0(method, "storage.zvol.create") == 0) {
        handle_storage_zvol_create_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "storage.zvol.delete") == 0) {
        handle_storage_zvol_delete_request(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);

    } else if (g_strcmp0(method, "device.disk.attach") == 0) {
        handle_device_disk_attach(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else if (g_strcmp0(method, "device.disk.detach") == 0) {
        handle_device_disk_detach(params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);

    } else {
        // Method Not Found
        gchar *err_resp = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_METHOD_NOT_FOUND, "Method not found");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);    
        dispatcher_request_context_free(ctx);
    }
    

    g_free(rpc_id_str); // 문자열 ID 메모리 정리
    g_object_unref(parser);
}