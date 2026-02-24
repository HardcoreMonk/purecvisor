// =================================================================
// src/modules/dispatcher/handler_monitor.c
// =================================================================
#include "rpc_utils.h"
#include <gio/gio.h>
#include "api/uds_server.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>

// 라이프사이클 모듈에 뚫어둔 다형성 검색기 외부 참조
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

void handle_monitor_metrics(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing parameter: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn = virConnectOpen("qemu:///system");
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virConnectClose(conn); return;
    }

    // 🚀 Libvirt API 타격: 가상 머신의 실시간 메트릭 정보를 구조체로 긁어옵니다.
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, libvirt_err ? libvirt_err->message : "Failed to get metrics");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virDomainFree(dom); virConnectClose(conn); return;
    }

    // 상태 코드 변환 매핑
    const gchar *state_str = "UNKNOWN";
    switch (info.state) {
        case VIR_DOMAIN_RUNNING: state_str = "RUNNING"; break;
        case VIR_DOMAIN_BLOCKED: state_str = "BLOCKED"; break;
        case VIR_DOMAIN_PAUSED:  state_str = "PAUSED"; break;
        case VIR_DOMAIN_SHUTDOWN:state_str = "SHUTDOWN"; break;
        case VIR_DOMAIN_SHUTOFF: state_str = "SHUTOFF"; break;
        case VIR_DOMAIN_CRASHED: state_str = "CRASHED"; break;
    }

    // JSON 객체로 조립
    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    
    json_object_set_string_member(res_obj, "state", state_str);
    json_object_set_int_member(res_obj, "vcpu", info.nrVirtCpu);
    json_object_set_double_member(res_obj, "mem_max_mb", info.maxMem / 1024.0);
    json_object_set_double_member(res_obj, "mem_used_mb", info.memory / 1024.0);
    json_object_set_int_member(res_obj, "cpu_time_ns", info.cpuTime);

    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    virDomainFree(dom);
    virConnectClose(conn);
}