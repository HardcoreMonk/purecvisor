#include "modules/daemons/vm_config_history_nudge.h"

#include <string.h>

#include "api/dispatcher_owner_scope.h"
#include "modules/daemons/vm_config_history.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_utils.h"

static const PcvVmCfgNudgeEntry k_nudge[] = {
    {"vm.create", NULL, NULL},
    {"vm.delete", NULL, NULL},
    {"vm.limit", NULL, NULL},
    {"vm.clone", NULL, NULL},
    {"vm.import.ova", NULL, NULL},
    {"vm.resize_disk", "target", "new_size_gb"},
    {"vm.disk.live_resize", "target", "new_size_gb"},
    {"vm.start", NULL, NULL},
    {"vm.set_vcpu", NULL, NULL},
    {"vm.set_memory", NULL, NULL},
    {"vm.pin_vcpu", NULL, NULL},
    {"vm.blkio.set", NULL, NULL},
    {"vm.set_bandwidth", NULL, NULL},
    {"vm.mount_iso", NULL, NULL},
    {"vm.eject", NULL, NULL},
    {"vm.usb.attach", NULL, NULL},
    {"vm.usb.detach", NULL, NULL},
    {"vm.autostart", NULL, NULL},
    {"vm.rename", NULL, NULL},
    {"vm.owner.set", NULL, NULL},
    {"vm.snapshot.rollback", NULL, NULL},
    {"vm.security_group.set", NULL, NULL},
    {"vm.guest.agent.ensure_channel", NULL, NULL},
    {"device.disk.attach", NULL, NULL},
    {"device.disk.detach", NULL, NULL},
    {"device.nic.attach", NULL, NULL},
    {"device.nic.detach", NULL, NULL},
    {"device.gpu.attach", NULL, NULL},
    {"device.gpu.detach", NULL, NULL},
    {"sriov.attach", NULL, NULL},
    {"sriov.detach", NULL, NULL},
    {"qos.vm.set", NULL, NULL},
    {"tenant_overlay.attach_vm", NULL, NULL},
    {"tenant_overlay.detach_vm", NULL, NULL},
};

static const struct {
    const gchar *method;
    const gchar *reason;
} k_exempt[] = {
    {"vm.stop", "상태 전이 — 구성 변경 아님"},
    {"vm.pause", "상태 전이 — 구성 변경 아님"},
    {"vm.resume", "상태 전이 — 구성 변경 아님"},
    {"vm.batch", "일괄 상태 전이 — 구성 변경은 대조 스캔"},
    {"vm.guest.ping", "게스트 안 동작"},
    {"vm.guest.agent.status", "게스트 안 동작(읽기)"},
    {"vm.guest.fsinfo", "게스트 안 동작(읽기)"},
    {"vm.guest.exec", "게스트 안 동작 — 정의 아님"},
    {"vm.guest.shutdown", "게스트 안 동작 — 상태 전이"},
    {"vm.list", "읽기"},
    {"vm.metrics", "읽기"},
    {"vm.inspect", "읽기"},
    {"vm.vnc", "콘솔 접속 — 정의 아님"},
    {"vm.memory.stats", "읽기"},
    {"vm.cpu.stats", "읽기"},
    {"vm.usb.list", "읽기"},
    {"vm.blkio.get", "읽기"},
    {"vm.numa.info", "호스트 읽기"},
    {"vm.event.webhook.list", "읽기"},
    {"vm.delete.status", "읽기"},
    {"vm.sla.report", "읽기"},
    {"vm.billing.report", "읽기"},
    {"vm.schedule.list", "읽기"},
    {"vm.schedule.set", "백엔드 미구현 — 정의 아님"},
    {"vm.snapshot.list", "읽기"},
    {"vm.snapshot.create", "ZFS 스냅숏 — 정의 XML 아님"},
    {"vm.snapshot.delete", "ZFS 스냅숏 — 정의 XML 아님"},
    {"vm.snapshot.delete_all", "ZFS 스냅숏 — 정의 XML 아님"},
    {"vm.snapshot.schedule.list", "읽기"},
    {"vm.snapshot.schedule.set", "스냅숏 일정 — XML 밖 상태(범위 밖)"},
    {"vm.snapshot.schedule.delete", "스냅숏 일정 — XML 밖 상태(범위 밖)"},
    {"vm.export.ova", "내보내기 — 정의 읽기"},
    {"vm.config.history.list", "이 기능의 조회"},
    {"vm.config.history.get", "이 기능의 조회"},
    {"device.nic.list", "읽기"},
    {"sriov.status", "읽기"},
    {"sriov.list", "읽기"},
    {"sriov.enable", "호스트 PF 설정 — VM 정의 아님"},
    {"sriov.disable", "호스트 PF 설정 — VM 정의 아님"},
    {"sriov.set", "호스트 PF 설정 — VM 정의 아님"},
    {"qos.vm.get", "읽기"},
};

PcvVmCfgNudgeClass
pcv_vm_cfg_nudge_classify(const gchar *method, const PcvVmCfgNudgeEntry **entry)
{
    if (entry)
        *entry = NULL;
    if (!method)
        return PCV_VMCH_NUDGE_UNKNOWN;
    for (guint i = 0; i < G_N_ELEMENTS(k_nudge); i++) {
        if (!strcmp(k_nudge[i].method, method)) {
            if (entry)
                *entry = &k_nudge[i];
            return PCV_VMCH_NUDGE_INTENT;
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS(k_exempt); i++) {
        if (!strcmp(k_exempt[i].method, method))
            return PCV_VMCH_NUDGE_EXEMPT;
    }
    return PCV_VMCH_NUDGE_UNKNOWN;
}

void
pcv_vm_cfg_nudge_authorized_hook(const gchar *method, JsonObject *params, gpointer user_data)
{
    (void)user_data;
    const PcvVmCfgNudgeEntry *e = NULL;
    if (!params || pcv_vm_cfg_nudge_classify(method, &e) != PCV_VMCH_NUDGE_INTENT)
        return;
    const gchar *target = pcv_disp_dispatcher_audit_target_from_params(method, params);
    if (!target || !*target)
        return;
    const gchar *dev = NULL;
    gint64 bytes = 0;
    if (e->dev_param && json_object_has_member(params, e->dev_param) &&
        JSON_NODE_HOLDS_VALUE(json_object_get_member(params, e->dev_param)) &&
        json_node_get_value_type(json_object_get_member(params, e->dev_param)) == G_TYPE_STRING)
        dev = json_object_get_string_member(params, e->dev_param);
    if (e->size_gb_param && json_object_has_member(params, e->size_gb_param) &&
        JSON_NODE_HOLDS_VALUE(json_object_get_member(params, e->size_gb_param))) {
        gint64 gb = json_object_get_int_member(params, e->size_gb_param);
        bytes = (gb > 0 && gb < (G_MAXINT64 >> 30)) ? gb << 30 : 0;
    }
    pcv_vm_cfg_hist_intent_open(method, target, pcv_rpc_completion_actor(), pcv_rpc_completion_actor_tenant(),
                                (dev && bytes > 0) ? dev : NULL, (dev && bytes > 0) ? bytes : 0);
}

void
pcv_vm_cfg_nudge_audit_tap(const gchar *username, const gchar *method, const gchar *target, const gchar *result,
                           gint error_code)
{
    if (pcv_vm_cfg_nudge_classify(method, NULL) != PCV_VMCH_NUDGE_INTENT)
        return;

    if (error_code == PURE_RPC_ERR_FORBIDDEN || error_code == PURE_RPC_ERR_INVALID_PARAMS)
        result = "denied";
    pcv_vm_cfg_hist_intent_close(method, username, target, result);
}
