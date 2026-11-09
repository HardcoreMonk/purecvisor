   
                      
                                                                   
  
                       
                                                                 
                                                         
  
          
                                                         
                                                                
                                                                   
                                                        
  
         
                                                    
                                                                 
                                                
                                                                
                                                     
  
                                   
                                                                        
                                                                 
                                                                                     
                                                                        
                                                     
  
                               
                                                                            
                                                             
                                                                
                                                               
                                                        
  
                                         
                                                                           
                                                             
                                                                                 
                                                              
                                                
  
                          
                                                                       
                                                                        
                                                                            
                                                        
                               
  
                           
                                                                              
                                                                                       
                                                                                           
                                                                                        
                                                                        
                                                                              
                                                                          
                                                                    
  
                 
                                                    
                                                                 
                                                                    
                                                                              
                                                    
  
                       
                                                     
                                                 
                                                     
               
  
         
                                                                                    
                                                                                    
                                                      
                                
   
#include "api/drain.h"
#include "handler_vpc.h"

#include "rpc_utils.h"
#include "api/uds_server.h"
#include "api/ws_server.h"
#include "modules/audit/pcv_audit.h"
#include "modules/dispatcher/rpc_completion.h"                                 
#include "modules/auth/pcv_rbac.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/network/vpc/vpc_mgmt_guard.h"                                         
#include "modules/network/vpc/vpc_model.h"
#include "modules/resource/resource_gate.h"                                              
#include "modules/resource/resource_registry.h"                                 
#include "utils/pcv_job_queue.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"

#define VPC_LOG "vpc"

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
    PcvJobRef *job_ref;                                     
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
                                                                       
                                                                
    gboolean confirm_required;
    gchar *reserved_hit;
    guint confirm_timeout_sec;
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
    case PCV_VPC_ERROR_PORT_GROUP_MISSING:
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
    g_autofree gchar *resolved = NULL;
    if (admin) {
        tenant = _string_member(params, "tenant");
    } else {
        if (!actor || !*actor) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                        "인증 subject가 없어 tenant scope를 결정할 수 없습니다");
            return FALSE;
        }
                                                                         
        PcvResCaller caller = {0};
        pcv_res_caller_init(&caller, params, NULL);
        resolved = g_steal_pointer(&caller.tenant);
        pcv_res_caller_clear(&caller);
        tenant = resolved;
    }
    if (tenant && !pcv_vpc_name_is_valid(tenant)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "tenant 형식이 유효하지 않습니다");
        return FALSE;
    }
                                                                                        
                                                                                                
                                                              
    if ((tenant_required || !admin) && (!tenant || !*tenant)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "이 작업에는 tenant가 필요합니다");
        return FALSE;
    }
    if (actor_out)
        *actor_out = g_strdup(pcv_rpc_completion_actor() ? pcv_rpc_completion_actor() : PCV_AUDIT_ACTOR_LOCAL);
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
    pcv_job_unref(data->job_ref);
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
    g_free(data->reserved_hit);
    if (data->allowed_sources)
        g_ptr_array_unref(data->allowed_sources);
    g_free(data);
}

                                                                         
                                                                        
                                                                                   
static PcvVpcMgmtPending *
_pending_entry(const VpcWorkerData *data, JsonObject *result)
{
    gboolean first = data->op == VPC_OP_CREATE;
    JsonObject *subnet = result;
    if (first) {
        JsonNode *node = result ? json_object_get_member(result, "subnet") : NULL;
        subnet = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    }
    PcvVpcMgmtPending *p = g_new0(PcvVpcMgmtPending, 1);
    p->subnet_id = g_strdup(_string_member(subnet, "id"));
    p->vpc_id = g_strdup(first ? _string_member(result, "id") : data->vpc_id);
    p->tenant = g_strdup(data->tenant);
    p->cidr = g_strdup(data->cidr);
    p->requested_by = g_strdup(data->actor);
    p->revert_vpc = first;
    p->deadline_unix = g_get_real_time() / G_USEC_PER_SEC + (gint64)data->confirm_timeout_sec;
    return p;
}

                                                                
                                                                            
                                                                          
                                                
static void
_revert_unrecorded(const PcvVpcMgmtPending *p, const GError *ledger_error, GError **error)
{
    GError *e = NULL;
    gboolean reverted = FALSE;
    if (p->subnet_id)
        reverted = pcv_vpc_subnet_delete(p->subnet_id, p->tenant, &e) ||
                   g_error_matches(e, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND);
    else                                                       
        g_set_error_literal(&e, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "subnet id unknown");
    if (reverted && p->revert_vpc && p->vpc_id) {
        g_clear_error(&e);
        reverted = pcv_vpc_delete(p->vpc_id, p->tenant, &e) ||
                   g_error_matches(e, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND) ||
                   g_error_matches(e, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT);
    }
    PCV_LOG_WARN(VPC_LOG, "확인 대기 원장 기록 실패(%s) — subnet %s 원복 %s%s%s",
                 ledger_error ? ledger_error->message : "unknown",
                 p->subnet_id ? p->subnet_id : "-", reverted ? "완료" : "실패",
                 reverted ? "" : ": ", reverted ? "" : (e ? e->message : "unknown"));
    if (reverted)
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "confirm guard unavailable — subnet reverted");
    else
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "confirm guard unavailable — subnet revert failed: %s",
                    e ? e->message : "unknown");
    g_clear_error(&e);
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
    pcv_job_progress(data->job_ref, 10, "Local VPC mutation running");
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

                                                                         
                                                                        
    if (ok && data->confirm_required) {
        PcvVpcMgmtPending *pending = _pending_entry(data, result);
        g_autoptr(GError) ledger_error = NULL;
        if (pcv_vpc_mgmt_pending_add(pending, &ledger_error)) {
            pcv_vpc_mgmt_arm_pending(pending);
            g_autofree gchar *warning = pcv_vpc_mgmt_warning_text(
                data->reserved_hit, data->confirm_timeout_sec, pending->subnet_id);
            json_object_set_string_member(result, "warning", warning);
            json_object_set_boolean_member(result, "confirm_pending", TRUE);
            json_object_set_int_member(result, "confirm_deadline_unix", pending->deadline_unix);
            json_object_set_string_member(result, "subnet_id", pending->subnet_id);
        } else {
            ok = FALSE;
            _revert_unrecorded(pending, ledger_error, &error);
        }
        pcv_vpc_mgmt_pending_free(pending);
    }

    gint64 duration_ms = (g_get_monotonic_time() - started) / 1000;
    PureRpcErrorCode code = ok ? 0 : _rpc_code(error);
    g_autofree gchar *result_json = ok
        ? _object_json(result) : _error_json(error ? error->message : NULL);
    pcv_audit_log(data->actor, data->method, data->target, ok ? "ok" : "fail",
                  code, duration_ms, "local");
    pcv_ws_finish_job(data->job_ref, ok ? PCV_JOB_COMPLETED : PCV_JOB_FAILED,
                       result_json, ok ? NULL : (error ? error->message : "unknown"));
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
    data->job_ref = pcv_job_admit(data->method, data->target, NULL, data->method);
    if (!data->job_ref) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                    "VPC Job을 만들 수 없습니다");
        _worker_data_free(data);
        return;
    }
    data->job_id = g_strdup(pcv_job_id(data->job_ref));
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "job_id", data->job_id);
    json_object_set_string_member(accepted, "method", data->method);
    if (data->confirm_required) {
        g_autofree gchar *warning = pcv_vpc_mgmt_warning_text(
            data->reserved_hit, data->confirm_timeout_sec, NULL);
        json_object_set_string_member(accepted, "warning", warning);
        json_object_set_int_member(accepted, "confirm_timeout_sec", data->confirm_timeout_sec);
    }
    _send_object(server, connection, rpc_id, accepted);

    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, data, (GDestroyNotify)_worker_data_free);
    g_task_run_in_thread(task, _vpc_worker);
    g_object_unref(task);
}

                                                                     
                                                                                 
                                                                                         
                                                               
                                                                     
                                                                                          
                                                                    
                                                    
static gboolean
_mgmt_admission(VpcWorkerData *data, const gchar *cidr, const gchar *rpc_id,
                UdsServer *server, GSocketConnection *connection)
{
    if (!cidr)
        return TRUE;
                                                                                                                 
    if (!data->actor_is_admin) {                                                                
        GPtrArray *pool = pcv_res_pool_list(data->tenant);
        gboolean inside = pcv_vpc_mgmt_pool_covers(pool, cidr);                                             
        if (!pool)
            PCV_LOG_WARN(VPC_LOG, "테넌트 주소 풀을 읽지 못해 %s 를 풀 밖으로 판정한다", data->method);
        else
            g_ptr_array_unref(pool);
        if (!inside) {
            _send_error(server, connection, rpc_id, PURE_RPC_ERR_FORBIDDEN,
                        "Permission denied: subnet CIDR is outside this tenant's address pool");
            pcv_audit_log(data->actor, data->method, data->target, "denied",
                          PURE_RPC_ERR_FORBIDDEN, 0, "vpc");
            _worker_data_free(data);
            return FALSE;
        }
    }
    g_autoptr(GError) error = NULL;
    GPtrArray *reserved = pcv_vpc_mgmt_reserved_set(NULL, &error);
    const gchar *hit = "예약 대역 계산 실패";
    gboolean overlaps = TRUE;
    if (reserved)
        overlaps = pcv_vpc_mgmt_cidr_overlaps_any(cidr, reserved, &hit);
    else
        PCV_LOG_WARN(VPC_LOG, "예약 대역 계산 실패 — %s 를 겹침으로 판정한다: %s",
                     data->method, error ? error->message : "unknown");
    gboolean admin = data->actor_is_admin;
                                                                                                              
    if (overlaps && !admin) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_FORBIDDEN,
                    "Permission denied: subnet CIDR overlaps a reserved host management range");
        pcv_audit_log(data->actor, data->method, data->target, "denied",
                      PURE_RPC_ERR_FORBIDDEN, 0, "vpc");
        _worker_data_free(data);
    } else if (overlaps) {
        data->confirm_required = TRUE;
        data->reserved_hit = g_strdup(hit);                                         
        data->confirm_timeout_sec = pcv_vpc_mgmt_confirm_timeout_sec();
        pcv_audit_log(data->actor, "vpc.reserved.warning", data->target, "warn", 0, 0, "vpc");
    }
    if (reserved)
        g_ptr_array_free(reserved, TRUE);
    return !overlaps || admin;
}

static gboolean
_read_scope(JsonObject *params, gchar **tenant, GError **error)
{
    return _caller_scope(params, FALSE, NULL, tenant, NULL, error);
}

                                                                   
                                                                        
                                                                            
static void
_decorate_subnets(JsonArray *subnets)
{
    for (guint i = 0; subnets && i < json_array_get_length(subnets); i++) {
        JsonNode *node = json_array_get_element(subnets, i);
        if (!node || !JSON_NODE_HOLDS_OBJECT(node))
            continue;
        JsonObject *subnet = json_node_get_object(node);
        gint64 deadline = 0;
        gboolean pending = pcv_vpc_mgmt_pending_lookup(_string_member(subnet, "id"), &deadline);
        json_object_set_boolean_member(subnet, "confirm_pending", pending);
        if (pending)
            json_object_set_int_member(subnet, "confirm_deadline_unix", deadline);
        else
            json_object_set_null_member(subnet, "confirm_deadline_unix");
    }
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
    _decorate_subnets(subnets);
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
    if (array && kind == 0)
        _decorate_subnets(array);
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
    if (object) {
                                                                               
                                                          
        guint config_errors = 0;
        GPtrArray *reserved = pcv_vpc_mgmt_reserved_set(&config_errors, NULL);
        if (reserved)
            g_ptr_array_free(reserved, TRUE);
        json_object_set_int_member(object, "pending_confirmations", pcv_vpc_mgmt_pending_count(tenant));
        json_object_set_boolean_member(object, "boot_reconcile_skipped",
                                       pcv_vpc_mgmt_boot_reconcile_skipped());
        json_object_set_int_member(object, "reserved_config_errors", config_errors);
        _send_object(server, connection, rpc_id, object);
        return;
    }
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
    PcvVpcIpv4Cidr parsed_cidr;                                                    
    gboolean valid_initial_subnet = !has_initial_subnet ||
        (has_subnet_name && has_subnet_cidr && pcv_vpc_name_is_valid(subnet_name) &&
         subnet_cidr && pcv_vpc_cidr_parse(subnet_cidr, &parsed_cidr, NULL, NULL) &&
         subnet_mtu >= 68 && subnet_mtu <= 9216);
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
    if (!_mgmt_admission(data, data->cidr, rpc_id, server, connection))
        return;
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
    PcvVpcIpv4Cidr parsed_cidr;                                                    
    if (!data || !_require_uuid(params, "vpc_id", &vpc_id, &error) ||
        !name || !cidr || !pcv_vpc_cidr_parse(cidr, &parsed_cidr, NULL, NULL) ||
        mtu < 68 || mtu > 9216 ||
        !_require_revision(params, data ? &data->expected_revision : NULL, &error)) {
        if (!error) g_set_error(&error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                                "subnet name, cidr 또는 mtu가 유효하지 않습니다");
        _reject_worker(data, error, rpc_id, server, connection); return;
    }
    data->vpc_id = g_strdup(vpc_id); data->name = g_strdup(name); data->cidr = g_strdup(cidr);
    data->mtu = (gint)mtu;
    data->target = g_strdup_printf("%s/%s/%s[%s]", data->tenant, vpc_id, name, cidr);
    if (!_mgmt_admission(data, data->cidr, rpc_id, server, connection))
        return;
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
                                                                           
    if (pcv_vpc_mgmt_pending_lookup(subnet_id, NULL)) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_CONFLICT,
                    "subnet is awaiting confirmation — run vpc.subnet.confirm first");
        _worker_data_free(data); return;
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

                                                                                 
                                                                      
                                                                 
                                                                           
                                                           
                                                                  
void
handle_vpc_subnet_confirm(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                          GSocketConnection *connection)
{
    g_autoptr(GError) error = NULL; const gchar *subnet_id = NULL;
    if (!_require_uuid(params, "subnet_id", &subnet_id, &error)) {
        _send_error(server, connection, rpc_id, _rpc_code(error), error->message);
        return;
    }
    const gchar *actor = pcv_rpc_completion_actor() ? pcv_rpc_completion_actor()
                                                    : PCV_AUDIT_ACTOR_LOCAL;
    if (pcv_vpc_mgmt_confirm(subnet_id, &error)) {
        pcv_audit_log(actor, "vpc.subnet.confirm", subnet_id, "ok", 0, 0, "vpc");
        JsonObject *object = json_object_new();
        json_object_set_string_member(object, "subnet_id", subnet_id);
        json_object_set_boolean_member(object, "confirmed", TRUE);
        _send_object(server, connection, rpc_id, object);
        return;
    }
    PureRpcErrorCode code = _rpc_code(error);
    pcv_audit_log(actor, "vpc.subnet.confirm", subnet_id, "fail", code, 0, "vpc");
    _send_error(server, connection, rpc_id, code,
                error ? error->message : "subnet confirmation failed");
}

                                                                    
                                                                                                     
                                                                                                 

                                                    
static PureRpcErrorCode
_pool_rpc_code(const GError *error)
{
    return (error && error->domain == PCV_RES_ERROR && error->code == PCV_RES_ERROR_INVALID)
        ? PURE_RPC_ERR_INVALID_PARAMS : PURE_RPC_ERR_INTERNAL_ERROR;
}

                                                                                     
                                                                                                
                                                                                           
                    
static GPtrArray *
_pool_cidrs_param(JsonObject *params, gchar **msg)
{
    JsonNode *node = params && json_object_has_member(params, "cidrs") ? json_object_get_member(params, "cidrs") : NULL;
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) {
        *msg = g_strdup("Missing required parameter: cidrs (array of strings)");
        return NULL;
    }
    JsonArray *in = json_node_get_array(node);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < json_array_get_length(in) && !*msg; i++) {
        JsonNode *item = json_array_get_element(in, i);
        const gchar *cidr = item && JSON_NODE_HOLDS_VALUE(item) &&
                            json_node_get_value_type(item) == G_TYPE_STRING ? json_node_get_string(item) : NULL;
        const gchar *hit = NULL;
        if (!pcv_res_pool_cidr_is_valid(cidr))
            *msg = g_strdup_printf("cidrs[%u] must be a canonical private IPv4 network (RFC1918 or 100.64.0.0/10): %.64s",
                                   i, cidr ? cidr : "(not a string)");
        else if (pcv_vpc_mgmt_cidr_overlaps_any(cidr, out, &hit))
            *msg = g_strdup_printf("cidrs must not repeat or overlap each other: %s, %s", hit, cidr);
        else
            g_ptr_array_add(out, g_strdup(cidr));
    }
    if (*msg)
        g_clear_pointer(&out, g_ptr_array_unref);
    return out;
}

                                                                                 
                                                                                
static JsonArray *
_pool_warnings(const gchar *tenant, GPtrArray *cidrs)
{
    JsonArray *warnings = json_array_new();
    JsonArray *all = pcv_res_pool_list_all();
    for (guint i = 0; all && i < json_array_get_length(all); i++) {
        JsonObject *entry = json_array_get_object_element(all, i);
        const gchar *other = json_object_get_string_member_with_default(entry, "tenant", "");
        JsonNode *cn = json_object_get_member(entry, "cidrs");
        if (g_strcmp0(other, tenant) == 0 || !cn || !JSON_NODE_HOLDS_ARRAY(cn))
            continue;
        GPtrArray *set = g_ptr_array_new();                                   
        for (guint j = 0; j < json_array_get_length(json_node_get_array(cn)); j++)
            g_ptr_array_add(set, (gpointer)json_array_get_string_element(json_node_get_array(cn), j));
        for (guint k = 0; k < cidrs->len; k++) {
            const gchar *cidr = g_ptr_array_index(cidrs, k);
            if (pcv_vpc_mgmt_cidr_overlaps_any(cidr, set, NULL)) {
                g_autofree gchar *w = g_strdup_printf("overlaps pool of tenant %s: %s", other, cidr);
                json_array_add_string_element(warnings, w);
            }
        }
        g_ptr_array_unref(set);
    }
    if (all)
        json_array_unref(all);
    else
        json_array_add_string_element(warnings, "other tenants' pools could not be read — overlap not checked");
    GPtrArray *reserved = pcv_vpc_mgmt_reserved_set(NULL, NULL);
    for (guint k = 0; reserved && k < cidrs->len; k++) {
        const gchar *cidr = g_ptr_array_index(cidrs, k);
        if (pcv_vpc_mgmt_cidr_overlaps_any(cidr, reserved, NULL)) {
            g_autofree gchar *w = g_strdup_printf("overlaps reserved management range: %s", cidr);
            json_array_add_string_element(warnings, w);
        }
    }
    if (reserved)
        g_ptr_array_free(reserved, TRUE);
    else
        json_array_add_string_element(warnings, "reserved management ranges could not be computed — overlap not checked");
    return warnings;
}

                                                                                          
                                                                                                
                                                                                          
                                                                                             
                                                                                    
void
handle_vpc_pool_set(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                    GSocketConnection *connection)
{
    const gchar *tenant = _string_member(params, "tenant");
    if (!pcv_res_name_is_valid(tenant)) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                    "tenant must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}");
        return;
    }
    const gchar *actor = pcv_rpc_completion_actor() ? pcv_rpc_completion_actor()
                                                    : PCV_AUDIT_ACTOR_LOCAL;
    g_autofree gchar *target = g_strdup_printf("pool:%s", tenant);
    g_autofree gchar *msg = NULL;
    g_autoptr(GError) error = NULL;
    PureRpcErrorCode code = PURE_RPC_ERR_INVALID_PARAMS;
    GPtrArray *cidrs = _pool_cidrs_param(params, &msg);
    JsonObject *status = cidrs ? pcv_vpc_status(tenant, &error) : NULL;
    JsonNode *sn = status ? json_object_get_member(status, "subnet_cidrs") : NULL;
    if (cidrs && (!sn || !JSON_NODE_HOLDS_ARRAY(sn))) {                                              
        code = status ? PURE_RPC_ERR_INTERNAL_ERROR : _rpc_code(error);
        msg = g_strdup_printf("cannot read the tenant's subnets: %s", error ? error->message : "subnet_cidrs missing");
    } else if (cidrs) {                                                                    
        g_autofree gchar *outside = pcv_vpc_mgmt_pool_outside(cidrs, json_node_get_array(sn));
        if (outside) {
            code = PURE_RPC_ERR_CONFLICT;
            msg = g_strdup_printf("pool change would leave subnets outside the pool: %s", outside);
        }
    }
    if (status)
        json_object_unref(status);
    if (!msg && !pcv_res_pool_set(tenant, cidrs, &error)) {
        code = _pool_rpc_code(error);
        msg = g_strdup(error ? error->message : "vpc pool write failed");
    }
    if (msg) {
        pcv_audit_log(actor, "vpc.pool.set", target, "fail", code, 0, "vpc");
        _send_error(server, connection, rpc_id, code, msg);
        if (cidrs)
            g_ptr_array_unref(cidrs);
        return;
    }
    pcv_audit_log(actor, "vpc.pool.set", target, "ok", 0, 0, "vpc");
    GPtrArray *saved = pcv_res_pool_list(tenant);                                      
    GPtrArray *shown = saved ? saved : cidrs;
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < shown->len; i++)
        json_array_add_string_element(arr, g_ptr_array_index(shown, i));
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "tenant", tenant);
    json_object_set_array_member(object, "cidrs", arr);
    json_object_set_array_member(object, "warnings", _pool_warnings(tenant, cidrs));
    if (saved)
        g_ptr_array_unref(saved);
    g_ptr_array_unref(cidrs);
    _send_object(server, connection, rpc_id, object);
}

                                                                                              
                                                                                                      
void
handle_vpc_pool_list(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                     GSocketConnection *connection)
{
    JsonNode *tn = params && json_object_has_member(params, "tenant") ? json_object_get_member(params, "tenant") : NULL;
    const gchar *tenant = (tn && !JSON_NODE_HOLDS_NULL(tn)) ? _string_member(params, "tenant") : NULL;
    if (tn && !JSON_NODE_HOLDS_NULL(tn) && !pcv_res_name_is_valid(tenant)) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                    "tenant must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}");
        return;
    }
    JsonArray *out = NULL;
    if (tenant) {
        GPtrArray *pool = pcv_res_pool_list(tenant);
        if (pool) {
            JsonArray *cidrs = json_array_new();
            for (guint i = 0; i < pool->len; i++)
                json_array_add_string_element(cidrs, g_ptr_array_index(pool, i));
            JsonObject *entry = json_object_new();
            json_object_set_string_member(entry, "tenant", tenant);
            json_object_set_array_member(entry, "cidrs", cidrs);
            out = json_array_new();
            json_array_add_object_element(out, entry);
            g_ptr_array_unref(pool);
        }
    } else {
        out = pcv_res_pool_list_all();
    }
    if (!out) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                    "vpc pool registry is unavailable");
        return;
    }
    _send_array(server, connection, rpc_id, out);
}
