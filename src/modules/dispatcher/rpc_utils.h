#ifndef PURECVISOR_RPC_UTILS_H
#define PURECVISOR_RPC_UTILS_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* JSON-RPC 2.0 Standard Error Codes */
typedef enum {
    PURE_RPC_ERR_PARSE_ERROR     = -32700, // Invalid JSON was received by the server.
    PURE_RPC_ERR_INVALID_REQUEST = -32600, // The JSON sent is not a valid Request object.
    PURE_RPC_ERR_METHOD_NOT_FOUND= -32601, // The method does not exist / is not available.
    PURE_RPC_ERR_INVALID_PARAMS  = -32602, // Invalid method parameter(s).
    PURE_RPC_ERR_INTERNAL_ERROR  = -32603, // Internal JSON-RPC error.
    
    /* Application Specific Errors (-32000 to -32099) */
    PURE_RPC_ERR_ZFS_OPERATION   = -32000, // ZFS command execution failed
    PURE_RPC_ERR_VM_NOT_FOUND    = -32001  // Specified VM does not exist
} PureRpcErrorCode;

/* Error Response Builder Helper */
gchar* pure_rpc_build_error_response(const gchar *rpc_id, 
                                     PureRpcErrorCode code, 
                                     const gchar *message);

/* Success Response Builder Helper (추가 제안) */
gchar* pure_rpc_build_success_response(const gchar *rpc_id, 
                                       JsonNode *result_node);

G_END_DECLS

#endif /* PURECVISOR_RPC_UTILS_H */