
#ifndef PCV_HANDLER_UTIL_H
#define PCV_HANDLER_UTIL_H

#include <json-glib/json-glib.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/virt/virt_conn_pool.h"

#define PCV_REQUIRE_PARAM(params_obj, key, out_var, rpc_id, srv, conn)       \
    do {                                                                      \
        if (!(params_obj) || !json_object_has_member((params_obj), (key))) {  \
            gchar *_e = pure_rpc_build_error_response(                        \
                (rpc_id), -32602, "Missing required parameter: " key);        \
            pure_uds_server_send_response((srv), (conn), _e);                \
            g_free(_e);                                                       \
            return;                                                           \
        }                                                                     \
        (out_var) = json_object_get_string_member((params_obj), (key));       \
        if (!(out_var) || (out_var)[0] == '\0') {                             \
            gchar *_e = pure_rpc_build_error_response(                        \
                (rpc_id), -32602, "Empty or invalid parameter: " key);        \
            pure_uds_server_send_response((srv), (conn), _e);                \
            g_free(_e);                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

#define PCV_REQUIRE_PARAM_OR(params_obj, key1, key2, out_var, rpc_id, srv, conn) \
    do {                                                                          \
        (out_var) = NULL;                                                         \
        if ((params_obj) && json_object_has_member((params_obj), (key1)))         \
            (out_var) = json_object_get_string_member((params_obj), (key1));      \
        if ((!(out_var) || (out_var)[0] == '\0') &&                               \
            (params_obj) && json_object_has_member((params_obj), (key2)))         \
            (out_var) = json_object_get_string_member((params_obj), (key2));      \
        if (!(out_var) || (out_var)[0] == '\0') {                                 \
            gchar *_e = pure_rpc_build_error_response(                            \
                (rpc_id), -32602,                                                 \
                "Missing required parameter: " key1 " or " key2);                \
            pure_uds_server_send_response((srv), (conn), _e);                    \
            g_free(_e);                                                           \
            return;                                                               \
        }                                                                         \
    } while (0)

#define PCV_REQUIRE_VIRT_CONN(out_conn, rpc_id, srv, conn)                   \
    do {                                                                      \
        (out_conn) = virt_conn_pool_acquire();                               \
        if (!(out_conn)) {                                                    \
            gchar *_e = pure_rpc_build_error_response(                        \
                (rpc_id), -32000,                                             \
                "Hypervisor connection pool exhausted");                      \
            pure_uds_server_send_response((srv), (conn), _e);               \
            g_free(_e);                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

#endif
