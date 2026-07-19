
#ifndef PURECVISOR_RPC_UTILS_H
#define PURECVISOR_RPC_UTILS_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {

    PURE_RPC_ERR_PARSE_ERROR     = -32700,

    PURE_RPC_ERR_INVALID_REQUEST = -32600,

    PURE_RPC_ERR_METHOD_NOT_FOUND= -32601,

    PURE_RPC_ERR_INVALID_PARAMS  = -32602,

    PURE_RPC_ERR_INTERNAL_ERROR  = -32603,

    PURE_RPC_ERR_ZFS_OPERATION   = -32000,

    PURE_RPC_ERR_VM_NOT_FOUND    = -32001,

    PURE_RPC_ERR_CONFLICT        = -32002,

    PURE_RPC_ERR_TIMEOUT         = -32003,

    PURE_RPC_ERR_BUSY            = -32004,

    PURE_RPC_ERR_NOT_FOUND       = -32005,

    PURE_RPC_ERR_FORBIDDEN       = -32006
} PureRpcErrorCode;

gchar* pure_rpc_build_error_response(const gchar *rpc_id,
                                     PureRpcErrorCode code,
                                     const gchar *message);

gchar* pure_rpc_build_success_response(const gchar *rpc_id,
                                       JsonNode *result_node);

gboolean pcv_rpc_params_get_int_alias(JsonObject *params,
                                      const gchar *primary_key,
                                      const gchar *alias_key,
                                      gint *out_value);

#define PCV_RPC_JSON_MAX_DEPTH 128

gboolean pcv_rpc_json_depth_ok(const gchar *json, gint max_depth);

#define PCV_RPC_JSON_MAX_BYTES (1u * 1024u * 1024u)

gboolean pcv_rpc_parse_guarded(const gchar *data, gssize len,
                               JsonParser **parser, GError **err);

G_END_DECLS

#endif
