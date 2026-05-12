



























#include "handler_vnc.h"
#include "rpc_utils.h"
#include "modules/virt/virt_conn_pool.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>
#include <stdlib.h>






extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);























void handle_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing parameter: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }


    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Hypervisor Connection Failed");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }


    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }








    gchar *xml = virDomainGetXMLDesc(dom, 0);
    gint vnc_port = -1;
    gchar *host = g_strdup("127.0.0.1");

    if (xml) {











        gchar *vnc_tag = strstr(xml, "<graphics type='vnc'");
        if (vnc_tag) {
            gchar *port_attr = strstr(vnc_tag, "port='");
            if (!port_attr) port_attr = strstr(vnc_tag, "port=\"");

            if (port_attr) {
                vnc_port = atoi(port_attr + 6);
            }


            gchar *listen_attr = strstr(vnc_tag, "listen='");
            if (!listen_attr) listen_attr = strstr(vnc_tag, "listen=\"");
            if (listen_attr && strncmp(listen_attr + 8, "0.0.0.0", 7) == 0) {
                g_free(host);
                host = g_strdup("0.0.0.0");
            }
        }
        free(xml);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);


    if (vnc_port != -1 && vnc_port != 0) {

        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "host", host);
        json_object_set_int_member(res_obj, "port", vnc_port);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {



        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "host", host);
        json_object_set_int_member(res_obj, "port", -1);
        json_object_set_string_member(res_obj, "message",
            "VNC not available — VM is not running or has no VNC graphics adapter");
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(host);
}