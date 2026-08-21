   
                      
                                                                   
  
                       
                                                                 
                                                         
  
          
                                                         
                                                                
                                                                   
                                                        
  
         
                                                    
                                                                 
                                                
                                                                
                                                     
  
                                   
                                                                        
                                                                 
                                                                            
                                                     
  
                               
                                                                            
                                                             
                                                                
                                                               
                                                        
  
                                         
                                                                           
                                                             
                                                                                 
                                                              
                                                
  
                          
                                                                       
                                                                        
                                                                            
                                                        
                               
  
                           
                                                                      
                                                                         
                                                             
                                                                    
  
                 
                                                    
                                                                 
                                                                    
                                                                              
                                                    
  
                       
                                                     
                                                 
                                                     
               
  
         
                                                                                    
                                                                                    
                                                      
                             
   
#include "handler_vpc.h"

#include "rpc_utils.h"
#include "api/uds_server.h"
#include "api/ws_server.h"
#include "modules/audit/pcv_audit.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/network/vpc/vpc_model.h"
#include "utils/pcv_job_queue.h"
#include "utils/pcv_validate.h"

typedef enum {
    VPC_OP_CREATE,
    VPC_OP_DELETE,
    VPC_OP_EGRESS_SET,
    VPC_OP_SUBNET_CREATE,
    VPC_OP_SUBNET_DELETE,
    VPC_OP_ATTACHMENT_CREATE,
    VPC_OP_ATTACHMENT_DELETE,
    VPC_OP_SERVICE_PUBLISH,
    VPC_OP_SERVICE_UNPUBLISH,
    VPC_OP_RECONCILE,
} VpcWorkerOp;

typedef struct {
    VpcWorkerOp op;
    gchar *method;
    gchar *job_id;
    gchar *target;
    gchar *actor;
    gchar *tenant;
    gchar *id;
    gchar *vpc_id;
    gchar *name;
    gchar *subnet_name;
    gchar *mode;
    gchar *backend;
    gchar *cidr;
    gchar *vm;
    gchar *requested_ip;
    gchar *protocol;
    gchar *listen_address;
    gint mtu;
    gint listen_port;
    gint target_port;
    gint64 expected_revision;
    gboolean actor_is_admin;
    GPtrArray *allowed_sources;
} VpcWorkerData;

static const gchar *
_string_member(JsonObject *params, const gchar *key)
{
    if (!params || !json_object_has_member(params, key))
        return NULL;
    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(node);
}

static gboolean
_int_member(JsonObject *params, const gchar *key, gint64 *out)
{
    if (!params || !out || !json_object_has_member(params, key))
        return FALSE;
    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return FALSE;
    GType type = json_node_get_value_type(node);
    if (type != G_TYPE_INT64 && type != G_TYPE_INT && type != G_TYPE_LONG)
        return FALSE;
    *out = json_node_get_int(node);
    return TRUE;
}

static PureRpcErrorCode
_rpc_code(const GError *error)
{
    if (!error || error->domain != PCV_VPC_ERROR)
        return PURE_RPC_ERR_INTERNAL_ERROR;
    switch ((PcvVpcError)error->code) {
    case PCV_VPC_ERROR_INVALID_ARGUMENT:
        return PURE_RPC_ERR_INVALID_PARAMS;
    case PCV_VPC_ERROR_NOT_FOUND:
        return PURE_RPC_ERR_NOT_FOUND;
    case PCV_VPC_ERROR_CONFLICT:
    case PCV_VPC_ERROR_STALE_REVISION:
    case PCV_VPC_ERROR_STATE:
        return PURE_RPC_ERR_CONFLICT;
    case PCV_VPC_ERROR_IO:
    default:
        return PURE_RPC_ERR_INTERNAL_ERROR;
    }
}

static void
_send_error(UdsServer *server, GSocketConnection *connection, const gchar *rpc_id,
            PureRpcErrorCode code, const gchar *message)
{
    g_autofree gchar *response = pure_rpc_build_error_response(rpc_id, code, message);
    pure_uds_server_send_response(server, connection, response);
}

static void
_send_object(UdsServer *server, GSocketConnection *connection, const gchar *rpc_id,
             JsonObject *object)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    g_autofree gchar *response = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, response);
}

static void
_send_array(UdsServer *server, GSocketConnection *connection, const gchar *rpc_id,
            JsonArray *array)
{
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, array);
    g_autofree gchar *response = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, response);
}

                                                     
static gboolean
_caller_scope(JsonObject *params,
              gboolean tenant_required,
              gchar **actor_out,
              gchar **tenant_out,
              gboolean *admin_out,
              GError **error)
{
    const gchar *actor = _string_member(params, "_pcv_caller_sub");
    gint64 role_value = PCV_ROLE_ADMIN;
    (void)_int_member(params, "_pcv_caller_role", &role_value);
    if (role_value < PCV_ROLE_VIEWER || role_value > PCV_ROLE_ADMIN)
        role_value = PCV_ROLE_ADMIN;
    gboolean admin = role_value == PCV_ROLE_ADMIN;
    const gchar *tenant = NULL;
    if (admin) {
        tenant = _string_member(params, "tenant");
    } else {
        if (!actor || !*actor) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                        "인증 subject가 없어 tenant scope를 결정할 수 없습니다");
            return FALSE;
        }
        tenant = pcv_rbac_get_tenant(actor);
    }
    if (tenant && !pcv_vpc_name_is_valid(tenant)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "tenant 형식이 유효하지 않습니다");
        return FALSE;
    }
    if (tenant_required && (!tenant || !*tenant)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "이 작업에는 tenant가 필요합니다");
        return FALSE;
    }
    if (actor_out)
        *actor_out = g_strdup(actor && *actor ? actor : "local-admin");
    if (tenant_out)
        *tenant_out = tenant ? g_strdup(tenant) : NULL;
    if (admin_out)
        *admin_out = admin;
    return TRUE;
}

static gboolean
_require_uuid(JsonObject *params, const gchar *key, const gchar **value_out,
              GError **error)
{
    const gchar *value = _string_member(params, key);
    if (!value || !g_uuid_string_is_valid(value)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "%s UUID가 필요합니다", key);
        return FALSE;
    }
    if (value_out)
        *value_out = value;
    return TRUE;
}

static gchar *
_object_json(JsonObject *object)
{
    if (!object)
        return g_strdup("{\"ok\":true}");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, object);
    gchar *text = json_to_string(node, FALSE);
    json_node_free(node);
    return text;
}

static gchar *
_error_json(const gchar *message)
{
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "error", message ? message : "unknown");
    gchar *text = _object_json(object);
    json_object_unref(object);
    return text;
}

static void
_worker_data_free(VpcWorkerData *data)
{
    if (!data)
        return;
    g_free(data->method);
    g_free(data->job_id);
    g_free(data->target);
    g_free(data->actor);
    g_free(data->tenant);
    g_free(data->id);
    g_free(data->vpc_id);
    g_free(data->name);
    g_free(data->subnet_name);
    g_free(data->mode);
    g_free(data->backend);
    g_free(data->cidr);
    g_free(data->vm);
    g_free(data->requested_ip);
    g_free(data->protocol);
    g_free(data->listen_address);
    if (data->allowed_sources)
        g_ptr_array_unref(data->allowed_sources);
    g_free(data);
}

static void
_vpc_worker(GTask *task,
            gpointer source_object G_GNUC_UNUSED,
            gpointer task_data,
            GCancellable *cancellable G_GNUC_UNUSED)
{
    VpcWorkerData *data = task_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonObject) result = NULL;
    gint64 started = g_get_monotonic_time();
    pcv_job_update_status(data->job_id, PCV_JOB_RUNNING, 10, "Local VPC mutation running");
    gboolean ok = FALSE;

    switch (data->op) {
    case VPC_OP_CREATE:
        ok = pcv_vpc_create(data->name, data->tenant, data->mode, data->backend,
                            data->subnet_name, data->cidr, data->mtu,
                            &result, &error);
        break;
    case VPC_OP_DELETE:
        ok = pcv_vpc_delete(data->id, data->tenant, &error);
        break;
    case VPC_OP_EGRESS_SET:
        ok = pcv_vpc_egress_set(data->vpc_id, data->tenant, data->mode,
                                data->expected_revision, &result, &error);
        break;
    case VPC_OP_SUBNET_CREATE:
        ok = pcv_vpc_subnet_create(data->vpc_id, data->tenant, data->name, data->cidr,
                                   data->mtu, data->expected_revision, &result, &error);
        break;
    case VPC_OP_SUBNET_DELETE:
        ok = pcv_vpc_subnet_delete(data->id, data->tenant, &error);
        break;
    case VPC_OP_ATTACHMENT_CREATE:
        ok = pcv_vpc_attachment_create(data->id, data->tenant, data->vm, data->actor,
                                       data->actor_is_admin, data->requested_ip,
                                       &result, &error);
        break;
    case VPC_OP_ATTACHMENT_DELETE:
        ok = pcv_vpc_attachment_delete(data->id, data->tenant, data->actor,
                                       data->actor_is_admin, &error);
        break;
    case VPC_OP_SERVICE_PUBLISH:
        ok = pcv_vpc_service_publish(data->id, data->tenant, data->protocol,
                                     data->listen_address, data->listen_port,
                                     data->target_port, data->allowed_sources,
                                     data->actor_is_admin, &result, &error);
        break;
    case VPC_OP_SERVICE_UNPUBLISH:
        ok = pcv_vpc_service_unpublish(data->id, data->tenant, &error);
        break;
    case VPC_OP_RECONCILE:
        ok = pcv_vpc_reconcile(&error);
        break;
    }

    gint64 duration_ms = (g_get_monotonic_time() - started) / 1000;
    PureRpcErrorCode code = ok ? 0 : _rpc_code(error);
    g_autofree gchar *result_json = ok
        ? _object_json(result) : _error_json(error ? error->message : NULL);
    pcv_job_set_result(data->job_id, ok ? PCV_JOB_COMPLETED : PCV_JOB_FAILED, result_json);
    pcv_audit_log(data->actor, data->method, data->target, ok ? "ok" : "fail",
                  code, duration_ms, "local");
    pcv_ws_broadcast_job_complete_mt(data->job_id, data->method,
                                     ok ? "completed" : "failed",
                                     ok ? NULL : (error ? error->message : "unknown"));
    if (ok)
        g_task_return_boolean(task, TRUE);
    else if (error)
        g_task_return_error(task, g_steal_pointer(&error));
    else
        g_task_return_new_error(task, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                                "Local VPC mutation failed");
}

                                                                                                                                                                                                           
                                                               
                                          
static void
_enqueue(VpcWorkerData *data,
         const gchar *rpc_id,
         UdsServer *server,
         GSocketConnection *connection)
{
    data->job_id = pcv_job_create(data->method, data->target, NULL);
    if (!data->job_id) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                    "VPC Job을 만들 수 없습니다");
        _worker_data_free(data);
        return;
    }
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "job_id", data->job_id);
    json_object_set_string_member(accepted, "method", data->method);
    _send_object(server, connection, rpc_id, accepted);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, data, (GDestroyNotify)_worker_data_free);
    g_task_run_in_thread(task, _vpc_worker);
    g_object_unref(task);
}

static gboolean
_read_scope(JsonObject *params, gchar **tenant, GError **error)
{
    return _caller_scope(params, FALSE, NULL, tenant, NULL, error);
}

void
handle_vpc_list(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                GSocketConnection *connection)
{
    g_autofree gchar *tenant = NULL; g_autoptr(GError) error = NULL;
    if (!_read_scope(params, &tenant, &error)) goto fail;
    JsonArray *array = pcv_vpc_list(tenant, &error);
    if (array) { _send_array(server, connection, rpc_id, array); return; }
fail:
    _send_error(server, connection, rpc_id, _rpc_code(error),
                error ? error->message : "VPC 목록 조회 실패");
}

void
handle_vpc_get(JsonObject *params, const gchar *rpc_id, UdsServer *server,
               GSocketConnection *connection)
{
    g_autofree gchar *tenant = NULL; g_autoptr(GError) error = NULL; const gchar *id = NULL;
    if (!_read_scope(params, &tenant, &error) || !_require_uuid(params, "vpc_id", &id, &error))
        goto fail;
    JsonObject *object = pcv_vpc_get(id, tenant, &error);
    if (!object) goto fail;
    JsonArray *subnets = pcv_vpc_subnet_list(id, tenant, &error);
    JsonArray *attachments = subnets ? pcv_vpc_attachment_list(id, tenant, &error) : NULL;
    JsonArray *services = attachments ? pcv_vpc_service_list(id, tenant, &error) : NULL;
    if (!subnets || !attachments || !services) {
        if (subnets) json_array_unref(subnets);
        if (attachments) json_array_unref(attachments);
        if (services) json_array_unref(services);
        json_object_unref(object);
        goto fail;
    }
    json_object_set_array_member(object, "subnets", subnets);
    json_object_set_array_member(object, "attachments", attachments);
    json_object_set_array_member(object, "service_publishes", services);
    _send_object(server, connection, rpc_id, object);
    return;
fail:
    _send_error(server, connection, rpc_id, _rpc_code(error),
                error ? error->message : "VPC 조회 실패");
}

static void
_handle_child_list(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                   GSocketConnection *connection, gint kind)
{
    g_autofree gchar *tenant = NULL; g_autoptr(GError) error = NULL; const gchar *id = NULL;
    if (!_read_scope(params, &tenant, &error) || !_require_uuid(params, "vpc_id", &id, &error))
        goto fail;
    JsonArray *array = kind == 0 ? pcv_vpc_subnet_list(id, tenant, &error)
        : kind == 1 ? pcv_vpc_attachment_list(id, tenant, &error)
                    : pcv_vpc_service_list(id, tenant, &error);
    if (array) { _send_array(server, connection, rpc_id, array); return; }
fail:
    _send_error(server, connection, rpc_id, _rpc_code(error),
                error ? error->message : "VPC child 목록 조회 실패");
}

void handle_vpc_subnet_list(JsonObject *p, const gchar *i, UdsServer *s, GSocketConnection *c)
{ _handle_child_list(p, i, s, c, 0); }
void handle_vpc_attachment_list(JsonObject *p, const gchar *i, UdsServer *s, GSocketConnection *c)
{ _handle_child_list(p, i, s, c, 1); }
void handle_vpc_service_list(JsonObject *p, const gchar *i, UdsServer *s, GSocketConnection *c)
{ _handle_child_list(p, i, s, c, 2); }

void
handle_vpc_status(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                  GSocketConnection *connection)
{
    g_autofree gchar *tenant = NULL; g_autoptr(GError) error = NULL;
    if (!_read_scope(params, &tenant, &error)) goto fail;
    JsonObject *object = pcv_vpc_status(tenant, &error);
    if (object) { _send_object(server, connection, rpc_id, object); return; }
fail:
    _send_error(server, connection, rpc_id, _rpc_code(error),
                error ? error->message : "VPC 상태 조회 실패");
}

void
handle_vpc_backend_list(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                        GSocketConnection *connection)
{
    g_autofree gchar *tenant = NULL; g_autoptr(GError) error = NULL;
    if (!_read_scope(params, &tenant, &error)) goto fail;
    JsonArray *array = pcv_vpc_backend_list(&error);
    if (array) { _send_array(server, connection, rpc_id, array); return; }
fail:
    _send_error(server, connection, rpc_id, _rpc_code(error),
                error ? error->message : "VPC backend 상태 조회 실패");
}

static VpcWorkerData *_new_worker(JsonObject *params, VpcWorkerOp op,
                                  const gchar *method, gboolean tenant_required,
                                  GError **error);
static void _reject_worker(VpcWorkerData *data, GError *error,
                           const gchar *rpc_id, UdsServer *server,
                           GSocketConnection *connection);
static void _enqueue_id_operation(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection,
                                  VpcWorkerOp op, const gchar *method,
                                  const gchar *key);

void
handle_vpc_create(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                  GSocketConnection *connection)
{
    GError *error = NULL;
    VpcWorkerData *data = _new_worker(params, VPC_OP_CREATE,
                                     "vpc.create", TRUE, &error);
    const gchar *name = _string_member(params, "name");
    const gchar *mode = _string_member(params, "egress_mode");
    const gchar *backend = json_object_has_member(params, "backend")
        ? _string_member(params, "backend") : "linux";
    const gchar *subnet_name = _string_member(params, "subnet_name");
    const gchar *subnet_cidr = _string_member(params, "subnet_cidr");
    gboolean has_subnet_name = json_object_has_member(params, "subnet_name");
    gboolean has_subnet_cidr = json_object_has_member(params, "subnet_cidr");
    gboolean has_subnet_mtu = json_object_has_member(params, "subnet_mtu");
    gboolean has_initial_subnet = has_subnet_name || has_subnet_cidr || has_subnet_mtu;
    gint64 subnet_mtu = 1500;
    if (has_subnet_mtu && !_int_member(params, "subnet_mtu", &subnet_mtu))
        subnet_mtu = -1;
    gboolean valid_initial_subnet = !has_initial_subnet ||
        (has_subnet_name && has_subnet_cidr && pcv_vpc_name_is_valid(subnet_name) &&
         subnet_cidr && subnet_mtu >= 68 && subnet_mtu <= 9216);
    if (!data || !pcv_vpc_name_is_valid(name) || !pcv_vpc_egress_mode_is_valid(mode) ||
        !pcv_vpc_backend_is_valid(backend) ||
        !valid_initial_subnet) {
        if (!error) g_set_error(&error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                                "VPC name/egress/backend 또는 첫 subnet 묶음이 유효하지 않습니다");
        _reject_worker(data, error, rpc_id, server, connection); return;
    }
    data->name = g_strdup(name); data->mode = g_strdup(mode);
    data->backend = g_strdup(backend);
    data->subnet_name = g_strdup(subnet_name); data->cidr = g_strdup(subnet_cidr);
    data->mtu = (gint)subnet_mtu;
    data->target = has_initial_subnet
        ? g_strdup_printf("%s/%s[%s:%s]", data->tenant, name, subnet_name, subnet_cidr)
        : g_strdup_printf("%s/%s", data->tenant, name);
    _enqueue(data, rpc_id, server, connection);
}

void
handle_vpc_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                  GSocketConnection *connection)
{
    _enqueue_id_operation(params, rpc_id, server, connection,
                          VPC_OP_DELETE, "vpc.delete", "vpc_id");
}

static VpcWorkerData *
_new_worker(JsonObject *params, VpcWorkerOp op, const gchar *method,
            gboolean tenant_required, GError **error)
{
    VpcWorkerData *data = g_new0(VpcWorkerData, 1);
    data->op = op;
    data->method = g_strdup(method);
    data->expected_revision = -1;
    if (!_caller_scope(params, tenant_required, &data->actor, &data->tenant,
                       &data->actor_is_admin, error)) {
        _worker_data_free(data);
        return NULL;
    }
    return data;
}

static gboolean
_require_revision(JsonObject *params, gint64 *revision, GError **error)
{
    if (!_int_member(params, "expected_revision", revision) || *revision < 1) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "expected_revision 양의 정수가 필요합니다");
        return FALSE;
    }
    return TRUE;
}

static void
_reject_worker(VpcWorkerData *data, GError *error, const gchar *rpc_id,
               UdsServer *server, GSocketConnection *connection)
{
    _send_error(server, connection, rpc_id, _rpc_code(error),
                error ? error->message : "VPC 요청이 유효하지 않습니다");
    _worker_data_free(data);
    g_clear_error(&error);
}

void
handle_vpc_egress_set(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                      GSocketConnection *connection)
{
    GError *error = NULL; const gchar *id = NULL;
    VpcWorkerData *data = _new_worker(params, VPC_OP_EGRESS_SET, "vpc.egress.set", TRUE, &error);
    const gchar *mode = _string_member(params, "egress_mode");
    if (!data || !_require_uuid(params, "vpc_id", &id, &error) ||
        !pcv_vpc_egress_mode_is_valid(mode) ||
        !_require_revision(params, data ? &data->expected_revision : NULL, &error)) {
        if (!error) g_set_error(&error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                                "egress_mode가 유효하지 않습니다");
        _reject_worker(data, error, rpc_id, server, connection); return;
    }
    data->vpc_id = g_strdup(id); data->mode = g_strdup(mode);
    data->target = g_strdup_printf("%s/%s", data->tenant, id);
    _enqueue(data, rpc_id, server, connection);
}

void
handle_vpc_subnet_create(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                         GSocketConnection *connection)
{
    GError *error = NULL; const gchar *vpc_id = NULL;
    VpcWorkerData *data = _new_worker(params, VPC_OP_SUBNET_CREATE,
                                     "vpc.subnet.create", TRUE, &error);
    const gchar *name = _string_member(params, "name");
    const gchar *cidr = _string_member(params, "cidr");
    gint64 mtu = 1500;
    if (json_object_has_member(params, "mtu") && !_int_member(params, "mtu", &mtu))
        mtu = -1;
    if (!data || !_require_uuid(params, "vpc_id", &vpc_id, &error) ||
        !name || !cidr || mtu < 68 || mtu > 9216 ||
        !_require_revision(params, data ? &data->expected_revision : NULL, &error)) {
        if (!error) g_set_error(&error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                                "subnet name, cidr 또는 mtu가 유효하지 않습니다");
        _reject_worker(data, error, rpc_id, server, connection); return;
    }
    data->vpc_id = g_strdup(vpc_id); data->name = g_strdup(name); data->cidr = g_strdup(cidr);
    data->mtu = (gint)mtu;
    data->target = g_strdup_printf("%s/%s/%s[%s]", data->tenant, vpc_id, name, cidr);
    _enqueue(data, rpc_id, server, connection);
}

static void
_enqueue_id_operation(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                      GSocketConnection *connection, VpcWorkerOp op,
                      const gchar *method, const gchar *key)
{
    GError *error = NULL; const gchar *id = NULL;
    VpcWorkerData *data = _new_worker(params, op, method, TRUE, &error);
    if (!data || !_require_uuid(params, key, &id, &error)) {
        _reject_worker(data, error, rpc_id, server, connection); return;
    }
    data->id = g_strdup(id);
    data->target = g_strdup_printf("%s/%s", data->tenant, id);
    _enqueue(data, rpc_id, server, connection);
}

void handle_vpc_subnet_delete(JsonObject *p, const gchar *i, UdsServer *s, GSocketConnection *c)
{ _enqueue_id_operation(p, i, s, c, VPC_OP_SUBNET_DELETE, "vpc.subnet.delete", "subnet_id"); }
void handle_vpc_attachment_delete(JsonObject *p, const gchar *i, UdsServer *s, GSocketConnection *c)
{ _enqueue_id_operation(p, i, s, c, VPC_OP_ATTACHMENT_DELETE, "vpc.attachment.delete", "attachment_id"); }
void handle_vpc_service_unpublish(JsonObject *p, const gchar *i, UdsServer *s, GSocketConnection *c)
{ _enqueue_id_operation(p, i, s, c, VPC_OP_SERVICE_UNPUBLISH, "vpc.service.unpublish", "publish_id"); }

void
handle_vpc_attachment_create(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                             GSocketConnection *connection)
{
    GError *error = NULL; const gchar *subnet_id = NULL;
    VpcWorkerData *data = _new_worker(params, VPC_OP_ATTACHMENT_CREATE,
                                     "vpc.attachment.create", TRUE, &error);
    const gchar *vm = _string_member(params, "vm");
    const gchar *requested_ip = _string_member(params, "ip_address");
    if (!data || !_require_uuid(params, "subnet_id", &subnet_id, &error) ||
        !vm || !pcv_validate_vm_name(vm)) {
        if (!error) g_set_error(&error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                                "유효한 vm이 필요합니다");
        _reject_worker(data, error, rpc_id, server, connection); return;
    }
    data->id = g_strdup(subnet_id); data->vm = g_strdup(vm);
    data->requested_ip = requested_ip ? g_strdup(requested_ip) : NULL;
    data->target = g_strdup_printf("%s/%s@%s", data->tenant, vm, subnet_id);
    _enqueue(data, rpc_id, server, connection);
}

static GPtrArray *
_source_array(JsonObject *params, GError **error)
{
    if (!params || !json_object_has_member(params, "allowed_sources"))
        goto invalid;
    JsonNode *node = json_object_get_member(params, "allowed_sources");
    if (!node || !JSON_NODE_HOLDS_ARRAY(node))
        goto invalid;
    JsonArray *input = json_node_get_array(node);
    if (json_array_get_length(input) == 0)
        goto invalid;
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < json_array_get_length(input); i++) {
        JsonNode *item = json_array_get_element(input, i);
        if (!item || !JSON_NODE_HOLDS_VALUE(item) ||
            json_node_get_value_type(item) != G_TYPE_STRING) {
            g_ptr_array_unref(out); goto invalid;
        }
        const gchar *source_text = json_node_get_string(item);
        PcvVpcIpv4Cidr source = {0}; g_autofree gchar *canonical = NULL;
        if (!pcv_vpc_cidr_parse(source_text, &source, &canonical, error)) {
            g_ptr_array_unref(out); return NULL;
        }
        if (g_strcmp0(source_text, canonical) != 0) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                        "allowed source CIDR은 canonical 형식이어야 합니다: %s", source_text);
            g_ptr_array_unref(out); return NULL;
        }
        g_ptr_array_add(out, g_strdup(source_text));
    }
    return out;
invalid:
    g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                "allowed_sources 비어 있지 않은 문자열 배열이 필요합니다");
    return NULL;
}

void
handle_vpc_service_publish(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                           GSocketConnection *connection)
{
    GError *error = NULL; const gchar *attachment_id = NULL;
    VpcWorkerData *data = _new_worker(params, VPC_OP_SERVICE_PUBLISH,
                                     "vpc.service.publish", TRUE, &error);
    const gchar *protocol = _string_member(params, "protocol");
    const gchar *listen_address = _string_member(params, "listen_address");
    gint64 listen_port = 0, target_port = 0;
    GPtrArray *sources = data ? _source_array(params, &error) : NULL;
    if (!data || !_require_uuid(params, "attachment_id", &attachment_id, &error) ||
        !pcv_vpc_protocol_is_valid(protocol) || !listen_address ||
        !_int_member(params, "listen_port", &listen_port) ||
        !_int_member(params, "target_port", &target_port) ||
        !pcv_vpc_port_is_valid((gint)listen_port) ||
        !pcv_vpc_port_is_valid((gint)target_port) || !sources) {
        if (sources) g_ptr_array_unref(sources);
        if (!error) g_set_error(&error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                                "Service Publish 파라미터가 유효하지 않습니다");
        _reject_worker(data, error, rpc_id, server, connection); return;
    }
    data->id = g_strdup(attachment_id); data->protocol = g_strdup(protocol);
    data->listen_address = g_strdup(listen_address);
    data->listen_port = (gint)listen_port; data->target_port = (gint)target_port;
    data->allowed_sources = sources;
    data->target = g_strdup_printf("%s/%s %s %s:%d->%d sources=%u",
        data->tenant, attachment_id, protocol, data->listen_address,
        data->listen_port, data->target_port, sources->len);
    _enqueue(data, rpc_id, server, connection);
}

void
handle_vpc_reconcile(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                     GSocketConnection *connection)
{
    GError *error = NULL;
    VpcWorkerData *data = _new_worker(params, VPC_OP_RECONCILE,
                                     "vpc.reconcile", FALSE, &error);
    if (!data) { _reject_worker(NULL, error, rpc_id, server, connection); return; }
    data->target = g_strdup("all");
    _enqueue(data, rpc_id, server, connection);
}
