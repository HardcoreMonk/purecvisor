/* src/api/dispatcher.c */

#include "dispatcher.h"
#include "uds_server.h"  // [Fix] 누락된 헤더 추가
#include "../modules/virt/vm_manager.h"
#include <json-glib/json-glib.h>
#include <glib.h>

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

    uds_server_send_response(ctx->server, ctx->connection, msg->str);

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

static void on_start_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = (DispatcherRequestContext *)user_data;
    GError *err = NULL;

    if (purecvisor_vm_manager_start_vm_finish(PURECVISOR_VM_MANAGER(source), res, &err)) {
        _send_success_bool(ctx, TRUE);
    } else {
        _send_error(ctx, -32000, err ? err->message : "Start failed");
        if (err) g_error_free(err);
    }
    dispatcher_request_context_free(ctx);
}

static void on_stop_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = (DispatcherRequestContext *)user_data;
    GError *err = NULL;

    if (purecvisor_vm_manager_stop_vm_finish(PURECVISOR_VM_MANAGER(source), res, &err)) {
        _send_success_bool(ctx, TRUE);
    } else {
        _send_error(ctx, -32000, err ? err->message : "Stop failed");
        if (err) g_error_free(err);
    }
    dispatcher_request_context_free(ctx);
}

static void on_delete_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = (DispatcherRequestContext *)user_data;
    GError *err = NULL;

    if (purecvisor_vm_manager_delete_vm_finish(PURECVISOR_VM_MANAGER(source), res, &err)) {
        _send_success_bool(ctx, TRUE);
    } else {
        _send_error(ctx, -32000, err ? err->message : "Delete failed");
        if (err) g_error_free(err);
    }
    dispatcher_request_context_free(ctx);
}

static void on_list_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    DispatcherRequestContext *ctx = (DispatcherRequestContext *)user_data;
    GError *err = NULL;
    JsonNode *result_node = purecvisor_vm_manager_list_vms_finish(PURECVISOR_VM_MANAGER(source), res, &err);

    if (result_node) {
        JsonBuilder *builder = json_builder_new();
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "jsonrpc");
        json_builder_add_string_value(builder, "2.0");
        
        // Add result node manually? json-glib builder doesn't support adding existing node directly easily
        // Instead, we construct the wrapper manually or use generator on the node.
        // For simplicity: Let's assume list_vms returns a JsonNode that is an ARRAY.
        
        // Hack: Create a wrapper object using string manipulation or proper node manipulation
        // Proper way:
        JsonNode *root = json_node_new(JSON_NODE_OBJECT);
        JsonObject *root_obj = json_object_new();
        json_object_set_string_member(root_obj, "jsonrpc", "2.0");
        json_object_set_member(root_obj, "result", result_node); // Transfer ownership
        json_object_set_int_member(root_obj, "id", ctx->request_id);
        json_node_set_object(root, root_obj);

        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, root);
        gsize len;
        gchar *json_str = json_generator_to_data(gen, &len);
        
        GString *msg = g_string_new(json_str);
        g_string_append_c(msg, '\n');
        uds_server_send_response(ctx->server, ctx->connection, msg->str);

        g_string_free(msg, TRUE);
        g_free(json_str);
        g_object_unref(gen);
        json_node_free(root); 
        g_object_unref(builder); // Unused here
    } else {
        _send_error(ctx, -32000, err ? err->message : "List failed");
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
    gint disk_size_gb = 10; // 기본값 10GB
    const gchar *iso_path = NULL; // Optional
    const gchar *bridge = NULL;     // [Fix] 변수 선언 추가

    if (json_object_has_member(params, "vcpu")) vcpu = json_object_get_int_member(params, "vcpu");
    if (json_object_has_member(params, "memory_mb")) memory_mb = json_object_get_int_member(params, "memory_mb");
    // [Added] 파라미터 파싱 사용
    if (json_object_has_member(params, "disk_size_gb")) disk_size_gb = json_object_get_int_member(params, "disk_size_gb");
    if (json_object_has_member(params, "iso_path")) iso_path = json_object_get_string_member(params, "iso_path");
    // [Added] Parse Bridge
    if (json_object_has_member(params, "network_bridge")) {
        bridge = json_object_get_string_member(params, "network_bridge");
    }
    // vm_manager로 전달하는 인자에 bridge 추가 필요
    // 하지만 현재 purecvisor_vm_manager_create_vm_async 함수는 bridge 인자가 없음.
    // -> 해결책: 구조체를 넘기거나 함수 인자를 늘려야 함.
    // -> Phase 5 아키텍처상, 인자를 늘리는 것보다 Config 객체를 Manager 내부에서 생성하는 현재 방식을 유지하되,
    //    Dispatcher -> Manager로 전달하는 인자를 확장하는 것이 맞음.
    
    // 여기서는 간단히 Manager의 create_vm_async 시그니처를 수정해야 함.
    // 일단 컴파일을 위해 기존 호출 유지 (Bridge 적용은 Manager 수정 후 가능)
    // 아키텍트 결정: Manager 수정을 포함하여 진행하시겠습니까? (Yes)
    purecvisor_vm_manager_create_vm_async(self->vm_manager, 
                                          name, 
                                          vcpu, 
                                          memory_mb,
                                          disk_size_gb, // 전달! 
                                          iso_path,
                                          bridge, // bridge 추가  
                                          on_create_finished, 
                                          ctx);
}

static void handle_vm_start(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    if (!json_object_has_member(params, "name")) {
        _send_error(ctx, -32602, "Missing parameter: name");
        dispatcher_request_context_free(ctx);
        return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    
    purecvisor_vm_manager_start_vm_async(self->vm_manager, name, on_start_finished, ctx);
}

static void handle_vm_stop(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    if (!json_object_has_member(params, "name")) {
        _send_error(ctx, -32602, "Missing parameter: name");
        dispatcher_request_context_free(ctx);
        return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    // Force option removed in Phase 5 basic implementation for simplicity, or we can assume force is internal
    purecvisor_vm_manager_stop_vm_async(self->vm_manager, name, on_stop_finished, ctx);
}

static void handle_vm_delete(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    if (!json_object_has_member(params, "name")) {
        _send_error(ctx, -32602, "Missing parameter: name");
        dispatcher_request_context_free(ctx);
        return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    
    purecvisor_vm_manager_delete_vm_async(self->vm_manager, name, on_delete_finished, ctx);
}

static void handle_vm_list(PureCVisorDispatcher *self, JsonObject *params, DispatcherRequestContext *ctx) {
    // Phase 5 List (No args needed)
    purecvisor_vm_manager_list_vms_async(self->vm_manager, on_list_finished, ctx);
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
    // Connection을 외부에서 주입받아야 하지만 편의상 NULL로 초기화 후 Main에서 설정 권장.
    // 여기서는 NULL로 생성 (main.c에서 생성자 호출 시 수정 필요할 수 있음)
    // **수정**: vm_manager_new는 Connection을 필요로 함.
    // Dispatcher 생성 시점에는 아직 Connection이 없을 수 있으므로 Main에서 주입하도록 변경하거나
    // 여기서는 NULL로 두고 나중에 Set 하도록 해야함. 
    // 임시: NULL (동작 안할 수 있음, Main.c 수정 필요할 수도 있음)
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
        // Parse error handling...
        g_error_free(err);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);
    
    const gchar *method = json_object_get_string_member(obj, "method");
    gint id = -1;
    if (json_object_has_member(obj, "id")) {
        id = json_object_get_int_member(obj, "id");
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

    if (g_strcmp0(method, "vm.create") == 0) {
        handle_vm_create(self, params, ctx);
    } else if (g_strcmp0(method, "vm.start") == 0) {
        handle_vm_start(self, params, ctx);
    } else if (g_strcmp0(method, "vm.stop") == 0) {
        handle_vm_stop(self, params, ctx);
    } else if (g_strcmp0(method, "vm.delete") == 0) {
        handle_vm_delete(self, params, ctx);
    } else if (g_strcmp0(method, "vm.list") == 0) {
        handle_vm_list(self, params, ctx);
    } else {
        _send_error(ctx, -32601, "Method not found");
        dispatcher_request_context_free(ctx);
    }

    g_object_unref(parser);
}