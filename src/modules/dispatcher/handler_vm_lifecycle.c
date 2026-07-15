/**
 * @file handler_vm_lifecycle.c
 * @brief VM 라이프사이클 RPC 핸들러 — 목록/종료/삭제/메트릭/VNC/제한
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c -> handle_vm_*_request()
 *                                              -> libvirt (virDomainShutdown, virDomainUndefine 등)
 *                                              -> vm_state.c (SQLite WAL 오퍼레이션 잠금)
 *
 * [처리하는 RPC 메서드] (6개)
 *   vm.list    -> handle_vm_list_request     : 전체 VM 목록 (이름, 상태, UUID)
 *   vm.stop    -> handle_vm_stop_request     : VM 종료 (graceful shutdown -> force destroy)
 *   vm.delete  -> handle_vm_delete_request   : VM 삭제 (종료 + undefine + ZFS zvol 제거)
 *   vm.metrics -> handle_vm_metrics_request  : 단일 VM CPU/메모리/디스크 메트릭
 *   vm.limit   -> (인라인)                    : VM CPU/메모리 cgroup 제한 설정
 *   vm.vnc     -> (인라인)                    : VNC 포트 정보 조회
 *
 * [fire-and-forget 패턴 사용 여부]
 *   - vm.stop, vm.delete: fire-and-forget 사용 (응답 먼저 전송 -> GTask 비동기 실행)
 *   - vm.list, vm.metrics: 동기 응답 (조회 결과 즉시 반환)
 *
 * [주의사항]
 *   - pure_virt_get_domain() 함수는 이 파일에 정의되어 있으며, VM 이름 또는
 *     UUID 어느 쪽으로든 도메인을 검색하는 다형성 함수입니다.
 *     다른 핸들러(handler_vm_start.c, handler_vnc.c 등)에서 extern으로 참조합니다.
 *   - VmLifecycleCtx 내 server/connection은 ref 카운트를 증가시켜 저장합니다.
 *   - vm.delete는 ZFS zvol까지 삭제하므로 되돌릴 수 없습니다.
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터(vm_id) 누락
 *   -32001 : 지정한 VM이 존재하지 않음
 *   -32000 : libvirt/ZFS 작업 실패
 */
#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "../audit/pcv_audit.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/virt/cancellable_map.h"  /* M1: vm.delete GCancellable 등록 */
#include "modules/virt/vm_manager.h"       /* VP-1: purecvisor_vm_resolve_network_bridge */
#include "utils/pcv_config.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_log.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"
#include "../network/security_group.h"

/* =================================================================
 * 공통 컨텍스트 구조체
 *
 * 모든 VM 라이프사이클 RPC 핸들러가 공유하는 비동기 작업 컨텍스트입니다.
 * GTask의 task_data로 전달되어 워커 스레드와 콜백 간 데이터를 운반합니다.
 *
 * [메모리 소유권 규칙]
 *   - gchar* 필드: g_strdup()으로 복사, free_lifecycle_ctx()에서 g_free()
 *   - server/connection: g_object_ref()로 참조 카운트 증가, g_object_unref()로 해제
 *   - GTask에 free_lifecycle_ctx를 GDestroyNotify로 등록하여 자동 해제
 * ================================================================= */
typedef struct {
    gchar *vm_id;           /**< VM 이름 또는 UUID (pure_virt_get_domain에 전달) */
    gchar *action;          /**< 실행할 동작 문자열: "start"/"stop"/"reset"/"pause"/"resume"/"limit" */
    gint cpu_quota;         /**< CPU cgroup 제한 퍼센티지 (예: 50 = 50%, -1 = 무제한) */
    gint mem_quota_mb;      /**< 메모리 cgroup 제한(MB 단위, -1 = 무제한) */
    gchar *rpc_id;          /**< JSON-RPC 요청 ID (응답 매칭용) */
    UdsServer *server;      /**< UDS 서버 인스턴스 (ref 카운트 증가됨) */
    GSocketConnection *connection; /**< 클라이언트 소켓 연결 (ref 카운트 증가됨) */
    /* vm.metrics 워커가 계산한 결과를 콜백에 전달하기 위한 출력 필드 */
    gint out_cpu_pct;       /**< 측정된 CPU 사용률 (0~100%) */
    gint out_mem_pct;       /**< 측정된 메모리 사용률 (0~100%) */
    gint out_vcpu;          /**< vCPU 수 */
    gint64 out_memory_mb;   /**< 할당 메모리(MB) */
    gint64 out_disk_rd;     /**< 디스크 읽기 바이트 */
    gint64 out_disk_wr;     /**< 디스크 쓰기 바이트 */
    gint64 out_net_rx;      /**< 네트워크 수신 바이트 */
    gint64 out_net_tx;      /**< 네트워크 송신 바이트 */
    gint64 out_disk_rd_req; /**< disk IOPS read */
    gint64 out_disk_wr_req; /**< disk IOPS write */
    gint64 out_net_rx_pkts; /**< network rx packets */
    gint64 out_net_tx_pkts; /**< network tx packets */
    /* 페이지네이션 파라미터 (vm.list용) */
    gint page_offset;       /**< 시작 인덱스 (0-based, 기본값 0) */
    gint page_limit;        /**< 최대 항목 수 (0 = 전체, 하위 호환) */
    /* CMP-1: 이 op가 lock_vm_operation을 획득했는지. 콜백 unlock을 이 플래그로 게이트해
     * 락 미획득 op(pause/resume/limit)가 남의 락을 지우지 못하게 한다. g_new0 기본 FALSE. */
    gboolean holds_lock;
} VmLifecycleCtx;

/**
 * free_lifecycle_ctx:
 * VmLifecycleCtx 구조체의 모든 필드를 안전하게 해제합니다.
 * GTask의 GDestroyNotify 콜백으로 등록되어 태스크 완료 시 자동 호출됩니다.
 *
 * [주의] server/connection은 NULL 체크 후 unref — 부분 초기화된 ctx도 안전하게 처리
 */
static void free_lifecycle_ctx(gpointer data) {
    if (!data) return;
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->action);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static gchar *
_extract_domain_disk_source_attr(const gchar *xml, const gchar *attr)
{
    if (!xml || !attr || !*attr)
        return NULL;

    const gchar *p = xml;
    while ((p = strstr(p, "<disk")) != NULL) {
        const gchar *tag_end = strchr(p, '>');
        const gchar *disk_end = strstr(p, "</disk>");
        if (!tag_end || !disk_end || disk_end <= tag_end)
            return NULL;

        gboolean is_primary_disk =
            g_strstr_len(p, tag_end - p, "device='disk'") ||
            g_strstr_len(p, tag_end - p, "device=\"disk\"");
        if (!is_primary_disk) {
            p = disk_end + strlen("</disk>");
            continue;
        }

        gchar *needle = g_strdup_printf("<source %s='", attr);
        const gchar *start = g_strstr_len(tag_end, disk_end - tag_end, needle);
        gchar quote = '\'';
        if (!start) {
            g_free(needle);
            needle = g_strdup_printf("<source %s=\"", attr);
            start = g_strstr_len(tag_end, disk_end - tag_end, needle);
            quote = '"';
        }

        if (start) {
            start += strlen(needle);
            const gchar *end = strchr(start, quote);
            g_free(needle);
            if (end && end <= disk_end)
                return g_strndup(start, end - start);
            return NULL;
        }

        g_free(needle);
        p = disk_end + strlen("</disk>");
    }

    return NULL;
}

typedef enum {
    PCV_VM_RENAME_DISK_NONE = 0,
    PCV_VM_RENAME_DISK_ZVOL,
    PCV_VM_RENAME_DISK_FILE,
} PcvVmRenameDiskKind;

typedef struct {
    PcvVmRenameDiskKind disk_kind;
    gchar *old_disk_path;
    gchar *new_disk_path;
    gchar *old_dataset;
    gchar *new_dataset;
    gchar *old_nvram_path;
    gchar *new_nvram_path;
} PcvVmRenamePlan;

static const gchar *_guest_get_vm_name(JsonObject *params);
virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

static void
_vm_rename_plan_clear(PcvVmRenamePlan *plan)
{
    if (!plan) return;
    g_free(plan->old_disk_path);
    g_free(plan->new_disk_path);
    g_free(plan->old_dataset);
    g_free(plan->new_dataset);
    g_free(plan->old_nvram_path);
    g_free(plan->new_nvram_path);
    memset(plan, 0, sizeof(*plan));
}

static void
_send_rpc_error(UdsServer *server, GSocketConnection *connection,
                const gchar *rpc_id, gint code, const gchar *message)
{
    gchar *err = pure_rpc_build_error_response(rpc_id, code,
                    message ? message : "Unknown error");
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
}

static const gchar *
_vm_rename_get_new_name(JsonObject *params)
{
    if (!params) return NULL;
    if (json_object_has_member(params, "new_name"))
        return json_object_get_string_member(params, "new_name");
    if (json_object_has_member(params, "target_name"))
        return json_object_get_string_member(params, "target_name");
    if (json_object_has_member(params, "target"))
        return json_object_get_string_member(params, "target");
    return NULL;
}

static xmlNodePtr
_xml_direct_child(xmlNodePtr parent, const gchar *name)
{
    if (!parent || !name) return NULL;
    for (xmlNodePtr cur = parent->children; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrcmp(cur->name, BAD_CAST name) == 0)
            return cur;
    }
    return NULL;
}

static xmlNodePtr
_xml_find_primary_disk_source(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrcmp(cur->name, BAD_CAST "disk") == 0) {
            xmlChar *device = xmlGetProp(cur, BAD_CAST "device");
            gboolean is_disk = device && xmlStrcmp(device, BAD_CAST "disk") == 0;
            if (device) xmlFree(device);
            if (is_disk) {
                xmlNodePtr source = _xml_direct_child(cur, "source");
                if (source)
                    return source;
            }
        }

        xmlNodePtr child = _xml_find_primary_disk_source(cur->children);
        if (child)
            return child;
    }
    return NULL;
}

static gboolean
_vm_rename_build_zvol_target(const gchar *old_name, const gchar *new_name,
                             const gchar *old_path, PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    if (!g_str_has_prefix(old_path, "/dev/zvol/")) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename supports only ZFS zvol or standard qcow2/raw primary disks");
        return FALSE;
    }

    gchar *dataset = g_strdup(old_path + strlen("/dev/zvol/"));
    gchar *slash = strrchr(dataset, '/');
    if (!slash || slash == dataset || *(slash + 1) == '\0') {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not parse source zvol dataset path");
        g_free(dataset);
        return FALSE;
    }

    const gchar *leaf = slash + 1;
    if (g_strcmp0(leaf, old_name) != 0) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename requires the primary zvol leaf '%s' to match VM name '%s'",
                leaf, old_name);
        }
        g_free(dataset);
        return FALSE;
    }

    gchar *parent = g_strndup(dataset, (gsize)(slash - dataset));
    gchar *target_dataset = g_strdup_printf("%s/%s", parent, new_name);
    gchar *target_path = g_strdup_printf("/dev/zvol/%s", target_dataset);

    plan->disk_kind = PCV_VM_RENAME_DISK_ZVOL;
    plan->old_disk_path = g_strdup(old_path);
    plan->new_disk_path = target_path;
    plan->old_dataset = dataset;
    plan->new_dataset = target_dataset;

    g_free(parent);
    return TRUE;
}

static gboolean
_vm_rename_file_ext_allowed(const gchar *base, const gchar *old_name,
                            const gchar **ext_out)
{
    static const gchar *exts[] = { ".qcow2", ".raw", ".img", NULL };
    for (gint i = 0; exts[i]; i++) {
        gchar *expected = g_strdup_printf("%s%s", old_name, exts[i]);
        gboolean match = g_strcmp0(base, expected) == 0;
        g_free(expected);
        if (match) {
            if (ext_out) *ext_out = exts[i];
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
_vm_rename_build_file_target(const gchar *old_name, const gchar *new_name,
                             const gchar *old_path, PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    gchar *base = g_path_get_basename(old_path);
    gchar *dir = g_path_get_dirname(old_path);
    const gchar *ext = NULL;

    if (!_vm_rename_file_ext_allowed(base, old_name, &ext)) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename requires primary file disk basename to be %s.qcow2, %s.raw, or %s.img",
                old_name, old_name, old_name);
        }
        g_free(base);
        g_free(dir);
        return FALSE;
    }

    gchar *new_base = g_strdup_printf("%s%s", new_name, ext);
    gchar *target_path = g_build_filename(dir, new_base, NULL);

    plan->disk_kind = PCV_VM_RENAME_DISK_FILE;
    plan->old_disk_path = g_strdup(old_path);
    plan->new_disk_path = target_path;

    g_free(new_base);
    g_free(base);
    g_free(dir);
    return TRUE;
}

static gboolean
_vm_rename_prepare_nvram(xmlNodePtr root, const gchar *old_name,
                         const gchar *new_name, PcvVmRenamePlan *plan,
                         gchar **error_msg)
{
    xmlNodePtr os = _xml_direct_child(root, "os");
    xmlNodePtr nvram = os ? _xml_direct_child(os, "nvram") : NULL;
    if (!nvram)
        return TRUE;

    xmlChar *content = xmlNodeGetContent(nvram);
    if (!content || !*content) {
        if (content) xmlFree(content);
        return TRUE;
    }

    gchar *old_path = g_strdup((const gchar *)content);
    xmlFree(content);
    g_strstrip(old_path);
    if (!*old_path) {
        g_free(old_path);
        return TRUE;
    }

    gchar *base = g_path_get_basename(old_path);
    gchar *expected = g_strdup_printf("%s_VARS.fd", old_name);
    if (g_strcmp0(base, expected) != 0) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename found non-standard NVRAM path '%s'; expected basename '%s'",
                old_path, expected);
        }
        g_free(base);
        g_free(expected);
        g_free(old_path);
        return FALSE;
    }

    gchar *dir = g_path_get_dirname(old_path);
    gchar *new_base = g_strdup_printf("%s_VARS.fd", new_name);
    gchar *new_path = g_build_filename(dir, new_base, NULL);
    xmlNodeSetContent(nvram, BAD_CAST new_path);

    plan->old_nvram_path = old_path;
    plan->new_nvram_path = new_path;

    g_free(new_base);
    g_free(dir);
    g_free(base);
    g_free(expected);
    return TRUE;
}

static gchar *
_vm_rename_build_patched_xml(const gchar *xml,
                             const gchar *old_name,
                             const gchar *new_name,
                             PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "pcv-vm-rename.xml",
                                  NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                                XML_PARSE_NOWARNING);
    if (!doc) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename failed to parse domain XML");
        return NULL;
    }

    gchar *patched = NULL;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr name_node = root ? _xml_direct_child(root, "name") : NULL;
    if (!name_node) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not find domain <name> node");
        goto cleanup;
    }
    xmlNodeSetContent(name_node, BAD_CAST new_name);

    xmlNodePtr source = root ? _xml_find_primary_disk_source(root) : NULL;
    if (!source) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename requires a primary disk source");
        goto cleanup;
    }

    xmlChar *dev = xmlGetProp(source, BAD_CAST "dev");
    xmlChar *file = dev ? NULL : xmlGetProp(source, BAD_CAST "file");
    if (dev) {
        if (!_vm_rename_build_zvol_target(old_name, new_name, (const gchar *)dev,
                                          plan, error_msg)) {
            xmlFree(dev);
            goto cleanup;
        }
        xmlSetProp(source, BAD_CAST "dev", BAD_CAST plan->new_disk_path);
        xmlFree(dev);
    } else if (file) {
        if (!_vm_rename_build_file_target(old_name, new_name, (const gchar *)file,
                                          plan, error_msg)) {
            xmlFree(file);
            goto cleanup;
        }
        xmlSetProp(source, BAD_CAST "file", BAD_CAST plan->new_disk_path);
        xmlFree(file);
    } else {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not find primary disk dev/file source");
        goto cleanup;
    }

    if (!_vm_rename_prepare_nvram(root, old_name, new_name, plan, error_msg))
        goto cleanup;

    xmlChar *out = NULL;
    int out_len = 0;
    xmlDocDumpMemory(doc, &out, &out_len);
    if (!out || out_len <= 0) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename failed to serialize patched XML");
        if (out) xmlFree(out);
        goto cleanup;
    }
    patched = g_strdup((const gchar *)out);
    xmlFree(out);

cleanup:
    xmlFreeDoc(doc);
    if (!patched)
        _vm_rename_plan_clear(plan);
    return patched;
}

static gboolean
_vm_rename_zfs_exists(const gchar *dataset)
{
    const gchar *argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
    return pcv_spawn_sync(argv, NULL, NULL, NULL);
}

static gboolean
_vm_rename_zfs_rename(const gchar *from, const gchar *to, gchar **error_msg)
{
    gchar *stderr_s = NULL;
    const gchar *argv[] = {"zfs", "rename", from, to, NULL};
    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_s, NULL);
    if (!ok && error_msg) {
        *error_msg = g_strdup_printf("zfs rename %s -> %s failed: %s",
                                     from, to, stderr_s ? stderr_s : "unknown error");
    }
    g_free(stderr_s);
    return ok;
}

static gboolean
_vm_rename_storage_apply(PcvVmRenamePlan *plan, gchar **error_msg)
{
    if (plan->disk_kind == PCV_VM_RENAME_DISK_ZVOL) {
        if (!_vm_rename_zfs_exists(plan->old_dataset)) {
            if (error_msg)
                *error_msg = g_strdup_printf("source zvol dataset not found: %s",
                                             plan->old_dataset);
            return FALSE;
        }
        if (_vm_rename_zfs_exists(plan->new_dataset)) {
            if (error_msg)
                *error_msg = g_strdup_printf("target zvol dataset already exists: %s",
                                             plan->new_dataset);
            return FALSE;
        }
        return _vm_rename_zfs_rename(plan->old_dataset, plan->new_dataset, error_msg);
    }

    if (plan->disk_kind == PCV_VM_RENAME_DISK_FILE) {
        if (!g_file_test(plan->old_disk_path, G_FILE_TEST_EXISTS)) {
            if (error_msg)
                *error_msg = g_strdup_printf("source disk file not found: %s",
                                             plan->old_disk_path);
            return FALSE;
        }
        if (g_file_test(plan->new_disk_path, G_FILE_TEST_EXISTS)) {
            if (error_msg)
                *error_msg = g_strdup_printf("target disk file already exists: %s",
                                             plan->new_disk_path);
            return FALSE;
        }
        if (g_rename(plan->old_disk_path, plan->new_disk_path) != 0) {
            if (error_msg)
                *error_msg = g_strdup_printf("disk file rename failed: %s",
                                             g_strerror(errno));
            return FALSE;
        }
        return TRUE;
    }

    if (error_msg)
        *error_msg = g_strdup("vm.rename has no storage plan");
    return FALSE;
}

static void
_vm_rename_storage_rollback(PcvVmRenamePlan *plan)
{
    if (!plan) return;
    if (plan->disk_kind == PCV_VM_RENAME_DISK_ZVOL) {
        if (plan->new_dataset && plan->old_dataset &&
            _vm_rename_zfs_exists(plan->new_dataset)) {
            gchar *ignored = NULL;
            if (!_vm_rename_zfs_rename(plan->new_dataset, plan->old_dataset, &ignored)) {
                PCV_LOG_ERROR("vm_rename", "storage rollback failed: %s",
                              ignored ? ignored : "unknown error");
            }
            g_free(ignored);
        }
    } else if (plan->disk_kind == PCV_VM_RENAME_DISK_FILE) {
        if (plan->new_disk_path && plan->old_disk_path &&
            g_file_test(plan->new_disk_path, G_FILE_TEST_EXISTS)) {
            if (g_rename(plan->new_disk_path, plan->old_disk_path) != 0) {
                PCV_LOG_ERROR("vm_rename", "disk rollback failed: %s",
                              g_strerror(errno));
            }
        }
    }
}

static gboolean
_vm_rename_nvram_apply(PcvVmRenamePlan *plan, gchar **error_msg)
{
    if (!plan->old_nvram_path || !plan->new_nvram_path)
        return TRUE;

    if (g_file_test(plan->new_nvram_path, G_FILE_TEST_EXISTS)) {
        if (error_msg)
            *error_msg = g_strdup_printf("target NVRAM file already exists: %s",
                                         plan->new_nvram_path);
        return FALSE;
    }

    if (!g_file_test(plan->old_nvram_path, G_FILE_TEST_EXISTS))
        return TRUE;

    if (g_rename(plan->old_nvram_path, plan->new_nvram_path) != 0) {
        if (error_msg)
            *error_msg = g_strdup_printf("NVRAM file rename failed: %s",
                                         g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static void
_vm_rename_nvram_rollback(PcvVmRenamePlan *plan)
{
    if (!plan || !plan->old_nvram_path || !plan->new_nvram_path)
        return;
    if (g_file_test(plan->new_nvram_path, G_FILE_TEST_EXISTS)) {
        if (g_rename(plan->new_nvram_path, plan->old_nvram_path) != 0) {
            PCV_LOG_ERROR("vm_rename", "NVRAM rollback failed: %s",
                          g_strerror(errno));
        }
    }
}

/* =================================================================
 * vm.rename — 정지된 VM 이름과 표준 스토리지 경로를 함께 변경
 * ================================================================= */

void
handle_vm_rename_request(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *old_name = _guest_get_vm_name(params);
    const gchar *new_name = _vm_rename_get_new_name(params);

    if (!old_name || !new_name) {
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "Invalid params: 'name' and 'new_name' are required");
        return;
    }
    if (!pcv_validate_vm_name(old_name) || !pcv_validate_vm_name(new_name)) {
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "Invalid VM name: only [A-Za-z0-9_-], max 64 chars");
        return;
    }
    if (g_strcmp0(old_name, new_name) == 0) {
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "new_name must be different from current name");
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, old_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32001, "VM not found.");
        return;
    }

    virDomainPtr existing = virDomainLookupByName(conn, new_name);
    if (existing) {
        virDomainFree(existing);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "Target VM name already exists.");
        return;
    }
    virResetLastError();

    int active = virDomainIsActive(dom);
    if (active < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "Could not verify VM power state.");
        return;
    }
    if (active == 1) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "vm.rename requires the VM to be shut off.");
        return;
    }

    int snap_count = virDomainSnapshotNum(dom, 0);
    if (snap_count > 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "vm.rename is blocked while libvirt snapshot metadata exists.");
        return;
    }
    if (snap_count < 0)
        virResetLastError();

    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!xml)
        xml = virDomainGetXMLDesc(dom, 0);
    if (!xml) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "Failed to read domain XML.");
        return;
    }

    PcvVmRenamePlan plan = {0};
    gchar *err_msg = NULL;
    gchar *new_xml = _vm_rename_build_patched_xml(xml, old_name, new_name,
                                                  &plan, &err_msg);
    if (!new_xml) {
        free(xml);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000, err_msg);
        g_free(err_msg);
        return;
    }

    gboolean storage_moved = FALSE;
    gboolean nvram_moved = FALSE;
    gboolean old_undefined = FALSE;

    if (!_vm_rename_storage_apply(&plan, &err_msg))
        goto fail;
    storage_moved = TRUE;

    if (!_vm_rename_nvram_apply(&plan, &err_msg))
        goto fail;
    nvram_moved = plan.old_nvram_path && plan.new_nvram_path &&
                  g_file_test(plan.new_nvram_path, G_FILE_TEST_EXISTS);

    int undef_rc = virDomainUndefineFlags(dom, VIR_DOMAIN_UNDEFINE_KEEP_NVRAM);
    if (undef_rc < 0) {
        virResetLastError();
        undef_rc = virDomainUndefine(dom);
    }
    if (undef_rc < 0) {
        const gchar *vir_err = virGetLastErrorMessage();
        err_msg = g_strdup_printf("Failed to undefine old VM: %s",
                                  vir_err ? vir_err : "unknown error");
        virResetLastError();
        goto fail;
    }
    old_undefined = TRUE;
    virDomainFree(dom);
    dom = NULL;

    virDomainPtr new_dom = virDomainDefineXML(conn, new_xml);
    if (!new_dom) {
        const gchar *vir_err = virGetLastErrorMessage();
        err_msg = g_strdup_printf("Failed to define renamed VM: %s",
                                  vir_err ? vir_err : "unknown error");
        virResetLastError();
        goto fail;
    }
    virDomainFree(new_dom);

#if PCV_CLUSTER_ENABLED
    pcv_cluster_remove_vm_xml(old_name);
    pcv_cluster_sync_vm_xml(new_name);
#endif

    pcv_audit_log(NULL, "vm.rename", new_name, "ok", 0, 0, "local");

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "status", "renamed");
    json_object_set_string_member(res_obj, "old_name", old_name);
    json_object_set_string_member(res_obj, "new_name", new_name);
    json_object_set_string_member(res_obj, "storage_type",
        plan.disk_kind == PCV_VM_RENAME_DISK_ZVOL ? "zvol" : "file");
    json_object_set_string_member(res_obj, "old_disk", plan.old_disk_path);
    json_object_set_string_member(res_obj, "new_disk", plan.new_disk_path);
    json_object_set_boolean_member(res_obj, "disk_renamed", storage_moved);
    json_object_set_boolean_member(res_obj, "nvram_renamed", nvram_moved);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    _vm_rename_plan_clear(&plan);
    g_free(new_xml);
    free(xml);
    virt_conn_pool_release(conn);
    return;

fail:
    pcv_audit_log(NULL, "vm.rename", old_name, "fail", -32000, 0, "local");
    if (nvram_moved)
        _vm_rename_nvram_rollback(&plan);
    if (storage_moved)
        _vm_rename_storage_rollback(&plan);
    if (old_undefined) {
        virDomainPtr rollback_dom = virDomainDefineXML(conn, xml);
        if (rollback_dom)
            virDomainFree(rollback_dom);
        else
            PCV_LOG_ERROR("vm_rename", "failed to restore original XML for '%s'",
                          old_name);
    }

    if (dom)
        virDomainFree(dom);
    _send_rpc_error(server, connection, rpc_id, -32000,
                    err_msg ? err_msg : "vm.rename failed");
    g_free(err_msg);
    _vm_rename_plan_clear(&plan);
    g_free(new_xml);
    free(xml);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * 1. VM.LIST (상태 조회) 비동기 워커 및 콜백
 *
 * [동기 조회 패턴] fire-and-forget이 아닙니다.
 * GTask 워커에서 libvirt 조회를 수행하고, 콜백에서 결과를 소켓으로 전송합니다.
 * libvirt API가 짧은 시간 블로킹될 수 있으므로 메인 루프 차단 방지를 위해
 * GTask 워커 스레드에서 실행합니다.
 * ================================================================= */

/**
 * vm_list_worker:
 * GTask 워커 스레드에서 실행됩니다.
 * libvirt에 연결하여 모든 도메인(VM)을 열거하고 JSON 배열로 변환합니다.
 *
 * 반환 형식: [{"uuid":"...", "name":"...", "state":"running|shutoff|unknown"}, ...]
 *
 * [참고] virConnectListAllDomains(conn, &domains, 0)의 두 번째 인자 0은
 * 필터 없이 모든 도메인(실행 중 + 중지됨)을 가져온다는 의미입니다.
 */
static void vm_list_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    (void)source_obj; (void)task_data;
    /* 타임아웃 취소 확인 */
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "vm.list timed out (30s)");
        return;
    }
    /* libvirt 하이퍼바이저에 연결 (로컬 QEMU/KVM) */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    /* 모든 도메인 목록 가져오기 (running + shutoff + paused 등) */
    virDomainPtr *domains;
    int ret = virConnectListAllDomains(conn, &domains, 0);
    if (ret < 0) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to list domains.");
        return;
    }

    /* 각 도메인을 JSON 객체로 변환하여 배열에 추가 */
    JsonArray *array = json_array_new();
    for (int i = 0; i < ret; i++) {
        JsonObject *vm_obj = json_object_new();
        char uuid[VIR_UUID_STRING_BUFLEN];
        virDomainGetUUIDString(domains[i], uuid);

        json_object_set_string_member(vm_obj, "uuid", uuid);
        json_object_set_string_member(vm_obj, "name", virDomainGetName(domains[i]));

        /* 도메인 상태를 문자열로 매핑 (running/shutoff/unknown) */
        virDomainInfo info;
        if (virDomainGetInfo(domains[i], &info) < 0) {
            /* 상태 조회 실패 — 이 도메인 건너뜀 (크래시 방지) */
            virDomainFree(domains[i]);
            json_object_unref(vm_obj);
            continue;
        }
        const char *state_str = (info.state == VIR_DOMAIN_RUNNING)    ? "running" :
                                (info.state == VIR_DOMAIN_SHUTOFF)    ? "shutoff" :
                                (info.state == VIR_DOMAIN_PAUSED)     ? "paused" :
                                (info.state == VIR_DOMAIN_SHUTDOWN)   ? "shutdown" :
                                (info.state == VIR_DOMAIN_CRASHED)    ? "crashed" :
                                (info.state == VIR_DOMAIN_PMSUSPENDED)? "pmsuspended" : "unknown";
        json_object_set_string_member(vm_obj, "state", state_str);
        json_object_set_int_member(vm_obj, "vcpu", (gint64)info.nrVirtCpu);
        json_object_set_int_member(vm_obj, "memory_mb", (gint64)(info.maxMem / 1024));

        /* --- 확장 필드: XML 파싱 + libvirt API --- */
        char *xml = virDomainGetXMLDesc(domains[i], VIR_DOMAIN_XML_INACTIVE);
        if (xml) {
            /* storage_type: block(zvol) vs file(qcow2/raw) */
            const char *storage_type = "unknown";
            if (strstr(xml, "<disk type='block'"))
                storage_type = "zvol";
            else if (strstr(xml, "<disk type='file'"))
                storage_type = "qcow2";
            json_object_set_string_member(vm_obj, "storage_type", storage_type);

            /* boot_mode: UEFI/SecureBoot 감지 */
            const char *boot_mode = "bios";
            const char *loader_pos = strstr(xml, "<loader");
            if (loader_pos) {
                boot_mode = strstr(loader_pos, "secure='yes'") ? "uefi-secureboot" : "uefi";
            }
            json_object_set_string_member(vm_obj, "boot_mode", boot_mode);

            /* disk_format: <driver ... type='raw'|'qcow2'> */
            const char *disk_format = "unknown";
            const char *drv = strstr(xml, "<driver");
            if (drv) {
                const char *drv_type = strstr(drv, "type='qcow2'");
                if (drv_type)
                    disk_format = "qcow2";
                else if (strstr(drv, "type='raw'"))
                    disk_format = "raw";
            }
            json_object_set_string_member(vm_obj, "disk_format", disk_format);

            /* disk_path: <source dev='...'> 또는 <source file='...'> */
            const char *disk_path = NULL;
            const char *src_dev = strstr(xml, "<source dev='");
            const char *src_file = strstr(xml, "<source file='");
            if (src_dev) {
                src_dev += 13; /* strlen("<source dev='") */
                const char *end = strchr(src_dev, '\'');
                if (end) {
                    gchar *path = g_strndup(src_dev, (gsize)(end - src_dev));
                    json_object_set_string_member(vm_obj, "disk_path", path);
                    g_free(path);
                    disk_path = "set";
                }
            } else if (src_file) {
                src_file += 14; /* strlen("<source file='") */
                const char *end = strchr(src_file, '\'');
                if (end) {
                    gchar *path = g_strndup(src_file, (gsize)(end - src_file));
                    json_object_set_string_member(vm_obj, "disk_path", path);
                    g_free(path);
                    disk_path = "set";
                }
            }
            if (!disk_path)
                json_object_set_string_member(vm_obj, "disk_path", "");

            /* network_count: <interface type= 출현 횟수 */
            int net_count = 0;
            const char *p = xml;
            while ((p = strstr(p, "<interface type=")) != NULL) {
                net_count++;
                p++;
            }
            json_object_set_int_member(vm_obj, "network_count", (gint64)net_count);

            free(xml);  /* libvirt는 libc malloc 사용 */
        }

        /* auto_start: virDomainGetAutostart() */
        int autostart_val = 0;
        virDomainGetAutostart(domains[i], &autostart_val);
        json_object_set_boolean_member(vm_obj, "auto_start", autostart_val ? TRUE : FALSE);

        /* snapshot_count: virDomainSnapshotNum() */
        int snap_count = virDomainSnapshotNum(domains[i], 0);
        json_object_set_int_member(vm_obj, "snapshot_count", (gint64)(snap_count >= 0 ? snap_count : 0));

        json_array_add_object_element(array, vm_obj);
        virDomainFree(domains[i]);  /* 각 도메인 핸들 해제 (연결은 유지) */
    }
    free(domains);      /* [주니어 참고] libvirt API가 libc malloc()으로 할당한 배열이므로
                         * 반드시 libc free()로 해제해야 합니다. g_free()를 쓰면 안 됩니다!
                         * 일반 규칙: g_new/g_strdup → g_free, malloc → free, GObject → g_object_unref */
    virt_conn_pool_release(conn);  /* 커넥션 풀에 연결을 반환 (닫지 않고 재사용) */

    /* 결과 JsonNode를 GTask에 반환 — 콜백에서 소켓 전송에 사용됨 */
    JsonNode *root_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(root_node, array);  /* array 소유권 → root_node로 이전 */
    g_task_return_pointer(task, root_node, (GDestroyNotify)json_node_free);
}

/**
 * vm_list_callback:
 * GTask 워커 완료 후 메인 스레드에서 호출되는 콜백입니다.
 *
 * [콜백 기반 응답 패턴] (fire-and-forget이 아님)
 *   - 워커가 성공하면: JSON 결과를 소켓으로 전송
 *   - 워커가 실패하면: 에러 메시지를 소켓으로 전송
 *   - 두 경우 모두 ctx는 GTask의 GDestroyNotify(free_lifecycle_ctx)가 자동 해제
 */
static void vm_list_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;

    /* 타임아웃 소스 제거 (워커가 정상 완료된 경우) */
    guint tid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(task), "timeout_id"));
    if (tid > 0) g_source_remove(tid);

    /* 워커가 g_task_return_pointer()로 반환한 JsonNode를 꺼냄 */
    JsonNode *result_node = g_task_propagate_pointer(task, &error);
    /* ADR-0018: 워커 실제 결과 audit (target=빈문자열, 조회성 RPC) */
    pcv_audit_log(NULL, "vm.list", "", error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");
    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);  /* GError는 반드시 수동 해제 */
    } else if (ctx->page_limit > 0 && JSON_NODE_HOLDS_ARRAY(result_node)) {
        /* 페이지네이션 적용: offset/limit가 있으면 슬라이싱 */
        JsonArray *full = json_node_get_array(result_node);
        gint total = (gint)json_array_get_length(full);
        gint off = ctx->page_offset < 0 ? 0 : ctx->page_offset;
        if (off > total) off = total;

        JsonArray *paged = json_array_new();
        for (gint i = off; i < total && i < off + ctx->page_limit; i++)
            json_array_add_element(paged, json_array_dup_element(full, (guint)i));

        JsonObject *pg = json_object_new();
        json_object_set_array_member(pg, "items", paged);
        json_object_set_int_member(pg, "total", total);
        json_object_set_int_member(pg, "offset", off);
        json_object_set_int_member(pg, "limit", ctx->page_limit);
        json_object_set_boolean_member(pg, "has_more", off + ctx->page_limit < total);

        JsonNode *pg_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(pg_node, pg);
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, pg_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
        json_node_free(result_node);
    } else {
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
        /* result_node 소유권은 pure_rpc_build_success_response에 이전됨 — free 금지 */
    }
}

/**
 * handle_vm_list_request:
 * vm.list RPC 진입점 — 전체 VM 목록을 조회합니다.
 *
 * [비동기 패턴] GTask로 워커 스레드에서 libvirt 조회 → 콜백에서 응답 전송
 * 파라미터 없음 (params 무시)
 */
void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    /* 페이지네이션 파라미터 추출 (없으면 0 = 전체 반환, 하위 호환) */
    ctx->page_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    ctx->page_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;

    /* 범위 검증 — 메모리 폭발 방지 */
    if (ctx->page_offset < 0 || ctx->page_offset > 100000 ||
        (ctx->page_limit != 0 && (ctx->page_limit < 0 || ctx->page_limit > 10000))) {
        gchar *err = pure_rpc_build_error_response(rpc_id,
            PURE_RPC_ERR_INVALID_PARAMS,
            "Pagination out of range: offset 0-100000, limit 1-10000");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(ctx->rpc_id); g_object_unref(ctx->server);
        g_object_unref(ctx->connection); g_free(ctx);
        return;
    }

    /* [주니어 참고] GTask 비동기 패턴의 핵심 3단계:
     *   1. g_task_new(NULL, NULL, 콜백함수, 콜백데이터): 태스크 생성
     *      - 첫 번째 NULL: source_object (여기선 미사용)
     *      - 두 번째 NULL: GCancellable (취소 토큰, 여기선 미사용)
     *   2. g_task_set_task_data(task, ctx, 해제함수): 워커에 전달할 데이터 설정
     *      - 해제함수(free_lifecycle_ctx)가 GDestroyNotify로 등록되어 태스크 종료 시 자동 호출
     *   3. g_task_run_in_thread(task, 워커함수): GLib 스레드풀에서 워커 실행
     *      - 워커 완료 시 콜백이 메인 루프에서 자동 호출됨 */
    GCancellable *cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, cancel, vm_list_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    /* 30초 타임아웃 — libvirt 행 방지 (워커 완료 시 콜백에서 소스 제거) */
    guint timeout_id = g_timeout_add_seconds(30, (GSourceFunc)(void(*)(void))g_cancellable_cancel, cancel);
    g_object_set_data(G_OBJECT(task), "timeout_id", GUINT_TO_POINTER(timeout_id));
    g_task_run_in_thread(task, vm_list_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}

/* =================================================================
 * 2. VM.STOP & VM.DELETE 공용 워커 및 콜백 (Lock-Free 방어 적용)
 *
 * vm.stop, vm.pause, vm.resume, vm.limit 등 모든 "액션" 계열 RPC가
 * vm_action_worker() 하나를 공유합니다.
 * ctx->action 문자열로 분기하여 각각 다른 libvirt API를 호출합니다.
 * ================================================================= */

/**
 * pure_virt_get_domain:
 * @conn: libvirt 연결 핸들
 * @identifier: VM 이름 또는 UUID 문자열
 *
 * [다형성 검색 함수] VM을 UUID로 먼저 검색하고, 실패하면 이름으로 재검색합니다.
 *
 * 이 함수는 handler_vm_start.c, handler_vnc.c 등 다른 핸들러에서
 * extern 선언으로 참조합니다. 따라서 이 파일이 링크 순서상 먼저 포함되어야 합니다.
 *
 * [에러 처리]
 *   - UUID 검색 실패 시 virResetLastError()로 에러 상태를 초기화한 후
 *     이름 검색을 시도합니다. 이를 생략하면 이전 에러가 누적될 수 있습니다.
 *   - 둘 다 실패하면 NULL 반환 — 호출자가 에러 처리를 담당합니다.
 *
 * @returns: virDomainPtr (성공) 또는 NULL (실패). 호출자가 virDomainFree() 해야 합니다.
 */
virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier) {
    /* 1차 시도: UUID 문자열로 검색 (예: "550e8400-e29b-...") */
    virDomainPtr dom = virDomainLookupByUUIDString(conn, identifier);
    if (!dom) {
        /* UUID 검색 실패 → 에러 상태 초기화 후 이름으로 재시도 */
        virResetLastError();
        /* 2차 시도: VM 이름으로 검색 (예: "web-prod") */
        dom = virDomainLookupByName(conn, identifier);
    }
    return dom;
}
/**
 * vm_action_worker:
 * GTask 워커 스레드에서 실행되는 VM 액션 통합 처리기입니다.
 *
 * [분기 구조] ctx->action 문자열에 따라 다른 libvirt API를 호출합니다:
 *   "start"  → virDomainCreate()          — VM 시작 (이미 실행 중이면 스킵)
 *   "stop"   → virDomainDestroy()         — VM 강제 종료 (이미 꺼져 있으면 스킵)
 *   "reset"  → virDomainDestroy() + Create() — 강제 재시작
 *   "pause"  → virDomainSuspend()         — VM 일시정지 (실행 중이어야 함)
 *   "resume" → virDomainResume()          — 일시정지 해제 (paused 상태여야 함)
 *   "limit"  → virDomainSetScheduler/Memory() — CPU/메모리 cgroup 제한 적용
 *
 * [상태 검사] 각 액션은 실행 전에 VM의 현재 상태를 확인합니다.
 *   예: pause는 실행 중인 VM만 가능, resume은 paused 상태인 VM만 가능
 *
 * [에러 패턴] 실패 시 g_set_error() + goto 또는 즉시 return으로 에러 전파
 */
static void vm_action_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    (void)source_obj;
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;
    GError *error = NULL;

    /* 타임아웃 취소 확인 */
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "vm.%s timed out (30s)", ctx->action ? ctx->action : "unknown");
        return;
    }

    /* 1단계: libvirt 하이퍼바이저에 연결 */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    /*
     * 2단계: pure_virt_get_domain() 다형성 검색
     *   - UUID 또는 이름 어느 쪽으로든 VM을 찾을 수 있습니다.
     *   - REST API는 이름으로, TUI는 UUID로 요청할 수 있어 양쪽 모두 지원합니다.
     */
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);

    /* VM을 찾지 못한 경우 — 에러 코드 G_IO_ERROR_NOT_FOUND
     * [주니어 참고] g_set_error()로 GError를 생성한 뒤 g_task_return_error()로
     * GTask에 에러를 전달합니다. 이렇게 하면 콜백에서 g_task_propagate_boolean()이
     * FALSE를 반환하고 error를 통해 에러 메시지를 받을 수 있습니다. */
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM not found: %s", ctx->vm_id);
        virt_conn_pool_release(conn);
        g_task_return_error(task, error);
        return;
    }

    /* 3단계: ctx->action 문자열에 따른 분기 처리 (안전한 상태 검사 포함)
     *
     * [주니어 참고] g_strcmp0()은 NULL-safe 문자열 비교 함수입니다.
     * strcmp()와 달리 NULL 입력 시 segfault가 발생하지 않습니다.
     * GLib 코드에서는 항상 g_strcmp0()을 사용하는 습관을 들이세요. */

    /* --- "start" 분기: VM 기동 --- */
    if (g_strcmp0(ctx->action, "start") == 0) {
        if (virDomainIsActive(dom)) {
            g_print("VM '%s' is already running. Skipping start sequence.\n", ctx->vm_id);
        } else if (virDomainCreate(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
    /* --- "stop" 분기: VM 강제 종료 (virDomainDestroy = SIGKILL 수준) --- */
    else if (g_strcmp0(ctx->action, "stop") == 0) {
        if (!virDomainIsActive(dom)) {
            g_print("VM '%s' is already shut off. Skipping stop sequence.\n", ctx->vm_id);
        } else if (virDomainDestroy(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to stop VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
    /* --- "reset" 분기: 강제 종료 후 즉시 재시작 --- */
    else if (g_strcmp0(ctx->action, "reset") == 0) {
        if (virDomainIsActive(dom)) {
            if (virDomainDestroy(dom) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to destroy VM before reset: %s", ctx->vm_id);
                virDomainFree(dom); virt_conn_pool_release(conn);
                g_task_return_error(task, error); return;
            }
        }
        if (virDomainCreate(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to reset VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
    /* --- "pause" 분기: VM 일시정지 (vCPU 실행 중단, 메모리 유지) ---
     * [전제조건] VM이 running 상태여야 합니다. shutoff 상태에서는 pause 불가. */
    else if (g_strcmp0(ctx->action, "pause") == 0) {
        if (!virDomainIsActive(dom)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM '%s' is not running, cannot pause.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (virDomainSuspend(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to pause VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
    /* --- "resume" 분기: 일시정지 해제 ---
     * [전제조건] VM이 paused 상태여야 합니다. running/shutoff에서는 resume 불가.
     * virDomainIsActive()는 paused도 "active"로 판단하므로, 정확한 상태 확인을 위해
     * virDomainGetInfo()로 VIR_DOMAIN_PAUSED 여부를 직접 비교합니다. */
    else if (g_strcmp0(ctx->action, "resume") == 0) {
        virDomainInfo info;
        if (virDomainGetInfo(dom, &info) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to query VM state: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (info.state != VIR_DOMAIN_PAUSED) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM '%s' is not paused.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (virDomainResume(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to resume VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }
    /* --- "limit" 분기: CPU/메모리 cgroup v2 실시간 제한 적용 ---
     * [전제조건] VM이 active 상태여야 합니다 (live 설정은 실행 중 VM에만 적용 가능).
     *
     * [보안 참고] cpu_quota, mem_quota_mb 값은 JSON-RPC 파라미터에서 추출됩니다.
     * 음수(-1)는 제한 해제를 의미하며, 이 값은 커널 CFS 스케줄러에 직접 전달됩니다. */
    else if (g_strcmp0(ctx->action, "limit") == 0) {
        if (!virDomainIsActive(dom)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Entity '%s' is not active. Cannot apply live limits.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }

        /*
         * CPU Cgroup v2 (cpu.max) 실시간 제어
         *   - VIR_DOMAIN_SCHEDULER_VCPU_QUOTA → cgroup의 cpu.max quota 값
         *   - period 기본값 100,000us(100ms)이므로 1% = 1,000us
         *   - cpu_quota=-1이면 CFS Quota 무제한 (cpu.max = "max 100000")
         */
        if (ctx->cpu_quota > 0) {
            virTypedParameter params[1];
            // VIR_DOMAIN_SCHEDULER_VCPU_QUOTA가 cgroup의 cpu.max quota 값으로 맵핑됩니다.
            strncpy(params[0].field, VIR_DOMAIN_SCHEDULER_VCPU_QUOTA, VIR_TYPED_PARAM_FIELD_LENGTH);
            params[0].type = VIR_TYPED_PARAM_LLONG;
            
            // 기본 period가 100,000us(100ms)이므로, 1%는 1,000us에 해당합니다.
            params[0].value.l = (long long)ctx->cpu_quota * 1000;

            // -1 이면 제한 해제(Unlimited), 그 외에는 지정된 퍼센티지 적용
            if (ctx->cpu_quota == -1) {
                params[0].value.l = -1; // 커널 CFS Quota 무제한
            } else {
                params[0].value.l = (long long)ctx->cpu_quota * 1000;
            }

            if (virDomainSetSchedulerParametersFlags(dom, params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject cgroup limits to kernel.");
                virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
            }
        }

        /*
         * Memory Cgroup v2 (memory.max) 실시간 제어
         *   - VIR_DOMAIN_MEMORY_HARD_LIMIT → cgroup의 memory.max
         *   - MB 단위를 KiB로 변환하여 전달 (libvirt 내부 단위가 KiB)
         *   - mem_quota_mb=-1이면 VIR_DOMAIN_MEMORY_PARAM_UNLIMITED (무제한)
         */
        if (ctx->mem_quota_mb > 0) {
            virTypedParameter mem_params[1];
            // Libvirt의 HARD_LIMIT이 cgroup의 memory.max와 직접 매핑됩니다.
            strncpy(mem_params[0].field, VIR_DOMAIN_MEMORY_HARD_LIMIT, VIR_TYPED_PARAM_FIELD_LENGTH);
            mem_params[0].type = VIR_TYPED_PARAM_ULLONG;
            mem_params[0].value.ul = (unsigned long long)ctx->mem_quota_mb * 1024; // MB -> KiB 변환

            // -1 이면 제한 해제(Unlimited), 그 외에는 MB를 KiB로 변환하여 적용
            if (ctx->mem_quota_mb == -1) {
                mem_params[0].value.ul = VIR_DOMAIN_MEMORY_PARAM_UNLIMITED; // Libvirt 무제한 상수
            } else {
                mem_params[0].value.ul = (unsigned long long)ctx->mem_quota_mb * 1024;
            }

            if (virDomainSetMemoryParameters(dom, mem_params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject memory limits to kernel.");
                virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
            }
        }
    }


    /* 4단계: 자원 해제 및 성공 반환 (모든 분기를 통과한 경우) */
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    /* stop/reset 은 vnet 소멸/재배정 — 죽은 vnet 의 SG 디스패치 항목을 정리해야
     * 커널이 같은 이름(vnetN)을 다른 VM 에 재사용할 때 오적용이 없다 (spec §4-3 (3)).
     * 워커 스레드 컨텍스트 (pcv_spawn_sync 블로킹 규약). */
    if (g_strcmp0(ctx->action, "stop") == 0 || g_strcmp0(ctx->action, "reset") == 0)
        pcv_security_group_sync_vm(ctx->vm_id);

    g_task_return_boolean(task, TRUE);

}

/**
 * vm_action_callback:
 * vm_action_worker 완료 후 메인 스레드에서 호출되는 콜백입니다.
 *
 * [중요] 이 콜백에서 응답을 전송합니다 (fire-and-forget 패턴이 아님).
 * vm.stop, vm.pause, vm.resume, vm.limit은 모두 이 콜백을 통해 응답합니다.
 *
 * [락 해제] CMP-1: 이 op가 lock_vm_operation을 실제로 획득한 경우(ctx->holds_lock)
 * 에만 unlock_vm_operation()을 호출한다. 락을 획득한 op(현재 stop)는 반드시 해제해야
 * 후속 RPC 영구 차단을 막고, 락 미획득 op(pause/resume/limit)는 호출하면 안 된다
 * (남의 락을 지워 동시 vm.delete 직렬화를 무력화 — 조건부화 없이 무조건 unlock 금지).
 */
static void vm_action_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;

    /* 타임아웃 소스 제거 */
    guint action_tid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(task), "timeout_id"));
    if (action_tid > 0) g_source_remove(action_tid);

    gboolean success = g_task_propagate_boolean(task, &error);
    /* CMP-1: 이 op가 실제로 락을 획득했을 때만 해제한다. 락 미획득 op(pause/resume/
     * limit)가 무조건 unlock하던 것이 동시 vm.delete의 락을 삭제해 AF-P1 직렬화를
     * 무력화했다(destroy/create 경합). */
    if (ctx->holds_lock) unlock_vm_operation(ctx->vm_id);

    /* 감사 로그: VM 액션 결과 기록 */
    /* ADR-0018-audit: vm.stop, vm.pause, vm.resume, vm.limit
     * (동적 메서드명 — pcv_strdup_printf("vm.%s") 형태로 등록 검출 회피, 정적 분석용 annotation) */
    {
        gchar *audit_method = g_strdup_printf("vm.%s", ctx->action ? ctx->action : "unknown");
        pcv_audit_log(NULL, audit_method, ctx->vm_id,
                      success ? "ok" : "fail",
                      success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        g_free(audit_method);
    }

    if (!success) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, json_node_new(JSON_NODE_NULL));
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

/**
 * handle_vm_stop_request:
 * vm.stop RPC 진입점 — VM을 강제 종료(destroy)합니다.
 *
 * [처리 흐름]
 *   1. 파라미터 검증 (vm_id 필수)
 *   2. lock_vm_operation()으로 오퍼레이션 잠금 획득 (동시 start/stop 방지)
 *   3. ctx->action = "stop" 설정 후 vm_action_worker에 위임
 *   4. 콜백에서 응답 전송 + 잠금 해제
 *
 * [오퍼레이션 잠금] lock_vm_operation(vm_id, 2, &err_msg)
 *   - 2 = VM_OP_STOPPING 상수
 *   - 이미 다른 작업 중이면 에러 응답 즉시 반환
 */
void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");

    /* [주니어 참고] 오퍼레이션 잠금(Operation Lock) 패턴:
     * 동일 VM에 대해 start/stop/delete가 동시에 실행되면 상태 불일치가 발생합니다.
     * lock_vm_operation()은 SQLite WAL 기반 원자적 잠금으로 이를 방지합니다.
     * 잠금이 실패하면 (다른 작업 진행 중) 즉시 에러를 반환합니다.
     * 잠금 해제는 콜백(vm_action_callback)에서 unlock_vm_operation()으로 수행합니다.
     * 잠금 해제를 빠뜨리면 해당 VM이 영구적으로 잠기므로 주의하세요! */
    gchar *err_msg = NULL;
    if (!lock_vm_operation(vm_id, 2, &err_msg)) { // 2 = OP_STOPPING
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(err_msg); return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    /* 워커 스레드에게 "이것은 stop 명령이야"라고 알려주는 액션 문자열 설정 */
    ctx->action = g_strdup("stop");
    ctx->holds_lock = TRUE;   /* CMP-1: stop은 위 lock_vm_operation에서 락을 획득함 */

    GCancellable *cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, cancel, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));
    guint tid = g_timeout_add_seconds(30, (GSourceFunc)(void(*)(void))g_cancellable_cancel, cancel);
    g_object_set_data(G_OBJECT(task), "timeout_id", GUINT_TO_POINTER(tid));
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}


/**
 * handle_vm_pause_request:
 * vm.pause RPC 진입점 — 실행 중인 VM을 일시정지(suspend)합니다.
 * vCPU 실행이 중단되지만 메모리는 그대로 유지됩니다.
 *
 * [참고] pause는 오퍼레이션 잠금 없이 실행됩니다.
 * pause/resume은 빠르게 완료되며 상태 충돌 위험이 낮기 때문입니다.
 */
void handle_vm_pause_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp); g_free(err_resp); return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    ctx->action = g_strdup("pause");
    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

/**
 * handle_vm_resume_request:
 * vm.resume RPC 진입점 — 일시정지된 VM을 재개(resume)합니다.
 * paused 상태의 VM만 resume 가능 — 그 외 상태에서는 워커가 에러 반환합니다.
 */
void handle_vm_resume_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp); g_free(err_resp); return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    ctx->action = g_strdup("resume");
    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

/**
 * handle_vm_limit_request:
 * vm.limit RPC 진입점 — VM의 CPU/메모리 cgroup 제한을 실시간으로 설정합니다.
 *
 * 파라미터:
 *   vm_id*: 대상 VM 이름 또는 UUID
 *   cpu   : CPU 제한 퍼센트 (예: 50 = 50%, -1 = 무제한) [선택]
 *   mem   : 메모리 제한 MB (예: 1024 = 1GB, -1 = 무제한) [선택]
 *
 * [보안] cpu/mem 값의 범위 검증은 커널 CFS/cgroup이 담당합니다.
 * 음수(-1)는 제한 해제의 의미이므로, 0 이하 값도 허용합니다.
 */
void handle_vm_limit_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    /* Fix 1 + 6: NULL param check + range validation */
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    /* Fix 6: CPU/MEM range validation */
    if (json_object_has_member(params, "cpu")) {
        gint64 cpu_val = json_object_get_int_member(params, "cpu");
        if (cpu_val != -1 && (cpu_val <= 0 || cpu_val >= 10000000)) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
                "Invalid params: 'cpu' must be -1 (unlimited) or 1..9999999 (microseconds)");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp);
            return;
        }
    }
    if (json_object_has_member(params, "mem")) {
        gint64 mem_val = json_object_get_int_member(params, "mem");
        if (mem_val != -1 && (mem_val <= 0 || mem_val > 1048576)) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
                "Invalid params: 'mem' must be -1 (unlimited) or 1..1048576 MB");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp);
            return;
        }
    }

    const gchar *vm_id_str = json_object_get_string_member_with_default(params, "vm_id", NULL);
    if (!vm_id_str || !*vm_id_str) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' must be non-empty string");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id_str);
    ctx->action = g_strdup("limit");
    
    if (json_object_has_member(params, "cpu")) {
        ctx->cpu_quota = json_object_get_int_member(params, "cpu");
    }

    /* JSON에서 mem 값 추출 (선택 파라미터, 없으면 0 = 미적용) */
    if (json_object_has_member(params, "mem")) {
        ctx->mem_quota_mb = json_object_get_int_member(params, "mem");
    }
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    
    // 상태값(deleted/created) 대신 일반 성공 메시지를 띄우기 위해 태그 생략 또는 FALSE 세팅
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE)); 
    
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

/* =================================================================
 * [비동기 콜백] JSON 응답 조립 (Metrics)
 *
 * vm_metrics_worker의 결과(ctx->out_cpu_pct, ctx->out_mem_pct)를
 * JSON 객체 {"cpu": N, "mem": N}으로 조립하여 클라이언트에 전송합니다.
 * ================================================================= */
static void vm_metrics_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _success = g_task_propagate_boolean(task, &error);
    /* ADR-0018: 워커 실제 결과 audit (target=vm_id) */
    pcv_audit_log(NULL, "vm.metrics", ctx->vm_id ?: "",
                  _success ? "ok" : "fail",
                  _success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (_success) {
        /* 성공 시 확장된 VM 메트릭 JSON 조립 */
        JsonObject *result_obj = json_object_new();
        json_object_set_int_member(result_obj, "cpu", ctx->out_cpu_pct);
        json_object_set_int_member(result_obj, "mem", ctx->out_mem_pct);
        json_object_set_int_member(result_obj, "vcpu", ctx->out_vcpu);
        json_object_set_int_member(result_obj, "memory_mb", ctx->out_memory_mb);
        json_object_set_int_member(result_obj, "disk_rd", ctx->out_disk_rd);
        json_object_set_int_member(result_obj, "disk_wr", ctx->out_disk_wr);
        json_object_set_int_member(result_obj, "net_rx", ctx->out_net_rx);
        json_object_set_int_member(result_obj, "net_tx", ctx->out_net_tx);
        json_object_set_int_member(result_obj, "disk_rd_req", ctx->out_disk_rd_req);
        json_object_set_int_member(result_obj, "disk_wr_req", ctx->out_disk_wr_req);
        json_object_set_int_member(result_obj, "net_rx_pkts", ctx->out_net_rx_pkts);
        json_object_set_int_member(result_obj, "net_tx_pkts", ctx->out_net_tx_pkts);

        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, result_obj);

        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp); // json_node는 헬퍼 내부 로직에 따라 해제 유무 확인
    } else {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    }
}

/* =================================================================
 * [워커 스레드] Libvirt CPU & Memory 샘플링
 *
 * CPU 사용률은 100ms 간격의 cpuTime 델타로 계산합니다.
 * 메모리 사용률은 balloon usable 값을 우선 사용합니다.
 *
 * [주의] g_usleep(100000)으로 100ms 블로킹하므로 반드시 워커 스레드에서 실행해야 합니다.
 * 메인 루프에서 실행하면 전체 이벤트 처리가 100ms 동안 멈춥니다.
 * ================================================================= */
static void vm_metrics_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        /* Fix 4: return error instead of silent success with zero metrics */
        g_warning("vm_metrics_worker: connection pool exhausted for VM '%s'",
                  ctx->vm_id ? ctx->vm_id : "(null)");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Hypervisor connection pool exhausted");
        return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);

    /* VM이 꺼져있거나 존재하지 않으면 점유율 0%로 반환하여 UI 붕괴 방지 */
    if (!dom || !virDomainIsActive(dom)) {
        ctx->out_cpu_pct = 0;
        ctx->out_mem_pct = 0;
    } else {
        virDomainInfo info1, info2;

        /*
         * CPU 델타 계산법:
         *   1. 시점 T1에서 cpuTime(나노초) 스냅샷
         *   2. 100ms(100,000us) 대기
         *   3. 시점 T2에서 cpuTime 스냅샷
         *   4. CPU% = (T2.cpuTime - T1.cpuTime) / (100ms * vCPU수) * 100
         *
         * wall_diff = 100ms를 나노초로 변환(100,000,000ns) * vCPU 수
         * 멀티코어 VM에서 합산 CPU 시간이 벽시계 시간을 초과할 수 있으므로
         * 100%를 상한으로 클램핑합니다.
         */
        if (virDomainGetInfo(dom, &info1) < 0) {
            ctx->out_cpu_pct = 0; ctx->out_mem_pct = 0;
            goto metrics_cleanup;
        }
        g_usleep(100000);  /* 100ms 대기 (워커 스레드에서만 호출해야 함) */
        if (virDomainGetInfo(dom, &info2) < 0) {
            ctx->out_cpu_pct = 0; ctx->out_mem_pct = 0;
            goto metrics_cleanup;
        }

        unsigned long long time_diff = info2.cpuTime - info1.cpuTime;
        unsigned long long wall_diff = 100000000ULL * info1.nrVirtCpu; /* 100ms in 나노초 * vCPU 수 */
        ctx->out_cpu_pct = (wall_diff > 0) ? (int)((time_diff * 100) / wall_diff) : 0;
        if (ctx->out_cpu_pct > 100) ctx->out_cpu_pct = 100;

        /*
         * 메모리 사용률 계산:
         *   RSS는 QEMU 프로세스가 호스트에 resident로 잡은 양이라 guest page cache까지
         *   압박으로 오인할 수 있다. VM 안에서 즉시 회수 가능한 balloon.usable을 우선하고,
         *   없을 때만 unused/RSS 순서로 폴백한다.
         */
        virDomainMemoryStatStruct mem_stats[VIR_DOMAIN_MEMORY_STAT_NR];
        int nr_stats = virDomainMemoryStats(dom, mem_stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
        unsigned long long mem_actual = info2.memory;
        unsigned long long mem_usable = 0;
        unsigned long long mem_unused = 0;
        unsigned long long mem_rss = 0;
        for (int i = 0; i < nr_stats; i++) {
            switch (mem_stats[i].tag) {
            case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
                mem_actual = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_USABLE:
                mem_usable = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_UNUSED:
                mem_unused = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_RSS:
                mem_rss = mem_stats[i].val;
                break;
            default:
                break;
            }
        }

        unsigned long long mem_used = info2.memory / 5; /* 텔레메트리 실패 시 보수적 기본값 */
        if (mem_actual > 0 && mem_usable > 0 && mem_actual > mem_usable)
            mem_used = mem_actual - mem_usable;
        else if (mem_actual > 0 && mem_unused > 0 && mem_actual > mem_unused)
            mem_used = mem_actual - mem_unused;
        else if (mem_actual > 0 && mem_rss > 0)
            mem_used = mem_rss < mem_actual ? mem_rss : mem_actual;

        ctx->out_mem_pct = (mem_actual > 0) ? (int)((mem_used * 100) / mem_actual) : 0;
        if (ctx->out_mem_pct > 100) ctx->out_mem_pct = 100;

        /* vcpu, memory 기본 정보 */
        ctx->out_vcpu = (gint)info2.nrVirtCpu;
        ctx->out_memory_mb = (gint64)(info2.maxMem / 1024);

        /* disk I/O 통계 (vda 기본 블록 디바이스) */
        virDomainBlockStatsStruct blk_stats;
        if (virDomainBlockStats(dom, "vda", &blk_stats, sizeof(blk_stats)) == 0) {
            ctx->out_disk_rd = blk_stats.rd_bytes;
            ctx->out_disk_wr = blk_stats.wr_bytes;
            ctx->out_disk_rd_req = blk_stats.rd_req;
            ctx->out_disk_wr_req = blk_stats.wr_req;
        }

        /* 네트워크 I/O 통계 — XML에서 첫 번째 인터페이스 이름 추출 */
        gchar *xml = virDomainGetXMLDesc(dom, 0);
        if (xml) {
            /* <target dev='vnetN'/> 패턴에서 인터페이스 이름 추출 */
            gchar *tgt = strstr(xml, "<target dev='");
            if (tgt) {
                tgt += 13; /* strlen("<target dev='") */
                gchar *end = strchr(tgt, '\'');
                if (end) {
                    gchar *iface = g_strndup(tgt, end - tgt);
                    virDomainInterfaceStatsStruct if_stats;
                    if (virDomainInterfaceStats(dom, iface, &if_stats, sizeof(if_stats)) == 0) {
                        ctx->out_net_rx = if_stats.rx_bytes;
                        ctx->out_net_tx = if_stats.tx_bytes;
                        ctx->out_net_rx_pkts = if_stats.rx_packets;
                        ctx->out_net_tx_pkts = if_stats.tx_packets;
                    }
                    g_free(iface);
                }
            }
            free(xml);
        }
    }

metrics_cleanup:
    if (dom) virDomainFree(dom);
    if (conn) virt_conn_pool_release(conn);

    g_task_return_boolean(task, TRUE);
}

/* =================================================================
 * [API 진입점] vm.metrics — 단일 VM의 CPU/메모리 사용률 조회
 *
 * 파라미터: vm_id* (VM 이름 또는 UUID)
 * 응답: {"cpu": 45, "mem": 82}
 *
 * [비동기 패턴] 100ms CPU 샘플링이 필요하므로 GTask 워커 스레드에서 실행.
 * 콜백에서 결과를 JSON으로 조립하여 응답합니다 (fire-and-forget 아님).
 * ================================================================= */
void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_metrics_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx); // 해제 함수 이름 주의!
    g_task_run_in_thread(task, vm_metrics_worker);
    g_object_unref(task);
}

/* =================================================================
 * [API 진입점] vm.vnc — 실시간 VNC 포트 조회
 *
 * 파라미터: vm_id* (VM 이름 또는 UUID)
 * 응답: {"vnc_port": "5900"}
 *
 * [동작] VM의 XML 정의에서 <graphics type='vnc' port='...'> 를 문자열 파싱으로 추출합니다.
 * XML 파서 대신 strstr을 사용하여 의존성과 오버헤드를 최소화합니다.
 *
 * [전제조건] VM이 running 상태여야 합니다 (VNC는 실행 중 VM에만 할당됨).
 * ================================================================= */
void handle_vm_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }
    virDomainInfo info;
    virDomainGetInfo(dom, &info);
    if (info.state != VIR_DOMAIN_RUNNING) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM is not running. No VNC port active.");
        pure_uds_server_send_response(server, connection, err); g_free(err); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
    gchar *xml = virDomainGetXMLDesc(dom, 0);
    /* [주니어 참고] VNC 포트 추출 — 간이 XML 파싱 기법
     * strstr()로 "graphics type='vnc' port='" 문자열을 찾은 뒤,
     * 포인터 산술(+26)으로 port 값의 시작 위치로 이동합니다.
     * 26은 검색 문자열의 길이입니다: "graphics type='vnc' port='" = 26자.
     * 그 후 다음 따옴표(')까지의 문자열을 포트 번호로 추출합니다.
     *
     * XML 파서(libxml2 등) 대신 strstr을 쓰는 이유:
     *   - 외부 의존성 추가 없이 간단한 값 하나만 추출
     *   - libvirt XML 형식이 고정되어 있어 실전에서 안전 */
    gchar *port_start = strstr(xml, "graphics type='vnc' port='");
    if (port_start) {
        port_start += 26;  /* "graphics type='vnc' port='" 문자열 길이만큼 건너뜀 */
        gchar *port_end = strchr(port_start, '\'');
        if (port_end) {
            gchar *port_str = g_strndup(port_start, port_end - port_start);
            JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
            JsonObject *res_obj = json_object_new();
            json_object_set_string_member(res_obj, "vnc_port", port_str);
            json_node_take_object(res_node, res_obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp); g_free(port_str);
        }
    } else {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VNC Graphics adapter not found in XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    }
    g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
}

/* ===================================================================================================
 * [vm.delete] VM 삭제 핸들러 — fire-and-forget 패턴 사용
 *
 * [fire-and-forget 패턴 설명]
 *   1. 클라이언트에게 즉시 {"status":"accepted"} 응답 전송 (소켓 닫힘)
 *   2. GTask 워커 스레드에서 libvirt undefine + ZFS destroy를 백그라운드 실행
 *   3. 결과는 journalctl 로그에만 기록 (콜백에서 send_response 호출 금지!)
 *
 * [이 패턴을 사용하는 이유]
 *   ZFS destroy + fuser + wipefs + partx 등의 정리 작업이 수십 초 걸릴 수 있어
 *   동기 응답을 기다리면 클라이언트 측 타임아웃이 발생합니다.
 *
 * [되돌릴 수 없는 작업]
 *   libvirt undefine + ZFS destroy를 수행하므로 VM과 디스크가 영구 삭제됩니다.
 * =================================================================================================== */

/* ── GTask 비동기 컨텍스트 ──────────────────────────────────────────────── */
typedef struct {
    gchar        *vm_id;
    gchar        *rpc_id;
    UdsServer    *server;
    GSocketConnection *connection;
} VmDeleteCtx;

static void
_vm_delete_ctx_free(VmDeleteCtx *ctx)
{
    g_free(ctx->vm_id);
    g_free(ctx->rpc_id);
    g_object_unref(ctx->server);
    g_object_unref(ctx->connection);
    g_free(ctx);
}

/**
 * _vm_delete_worker:
 * GTask 워커 스레드에서 실행되는 VM 삭제 작업입니다.
 *
 * [fire-and-forget 워커]
 *   이 워커가 실행되기 전에 "accepted" 응답은 이미 전송되었습니다.
 *   에러 발생 시 클라이언트에 직접 알릴 수 없으며, 콜백에서 로그만 남깁니다.
 *
 * [원자성 보장 — 2026-04-11 C1 fix]
 *   libvirt XML을 undefine 이전에 스냅샷으로 저장합니다.
 *   ZFS destroy가 실패하면 저장된 XML로 VM 정의를 재생성(롤백)하여
 *   최소한 "XML 없음 + zvol 존재" 불일치 상태를 회피합니다.
 *   재정의도 실패하면 ERROR 레벨 로그로 수동 복구를 안내합니다.
 *
 * [실행 순서]
 *   1. 존재 검증: 모두 없으면 idempotent 성공 (W2 fix)
 *   2. XML 스냅샷 저장 (롤백 대비)
 *   3. libvirt destroy + undefine
 *   4. ZFS 엑소시즘 + destroy
 *   5. ZFS 실패 시 → XML 재정의 롤백
 *   6. 파일 디스크(qcow2/raw) unlink (실패 시 에러 반환 — C3 fix)
 */
static void
_vm_delete_worker(GTask *task, gpointer src __attribute__((unused)),
                  gpointer task_data, GCancellable *cancel)
{
    VmDeleteCtx *ctx = task_data;
    const gchar  *vm_id = ctx->vm_id;

    /* [M1 fix] 시작 시점 취소 체크 — 요청 수락 후 워커 스케줄링 전까지의 지연 */
    if (cancel && g_cancellable_is_cancelled(cancel)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "vm.delete cancelled before start");
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    virDomainPtr  dom  = conn ? pure_virt_get_domain(conn, vm_id) : NULL;

    gchar *zvol_path  = g_strdup_printf("/dev/zvol/%s/%s", pcv_config_get_zvol_pool(), vm_id);
    gchar *zfs_dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
    gboolean zfs_exists = FALSE;

    /* qcow2/raw 파일 디스크 경로 추출 + [C1 fix] 전체 XML 스냅샷 저장 (롤백 대비) */
    gchar *file_disk_path = NULL;
    gchar *saved_xml      = NULL;  /* C1: ZFS destroy 실패 시 재정의용 */
    if (dom) {
        char *xml = virDomainGetXMLDesc(dom, 0);
        if (xml) {
            saved_xml = g_strdup(xml);  /* 전체 XML 복사 — rollback용 */
            /* device='disk' 블록 안의 파일 디스크만 추출한다.
             * CD-ROM ISO도 <source file='...'>를 쓰므로, XML 전체에서 첫 source
             * file을 잡으면 VM 삭제가 설치 ISO를 지우는 치명적인 오판이 된다. */
            file_disk_path = _extract_domain_disk_source_attr(xml, "file");
            /* 커스텀 storage_pool로 생성한 VM은 zvol 경로가 기본 풀과 다르다.
             * device='disk' 블록의 <source dev='/dev/zvol/...'> 값을 신뢰해 삭제
             * 대상을 다시 계산해야 선택 저장 위치 VM도 안전하게 정리된다. */
            gchar *xml_zvol_path = _extract_domain_disk_source_attr(xml, "dev");
            if (xml_zvol_path) {
                if (g_str_has_prefix(xml_zvol_path, "/dev/zvol/")) {
                    g_free(zvol_path);
                    g_free(zfs_dataset);
                    zvol_path = xml_zvol_path;
                    zfs_dataset = g_strdup(xml_zvol_path + strlen("/dev/zvol/"));
                } else {
                    g_free(xml_zvol_path);
                }
            }
            free(xml);
        }
    }
    zfs_exists = zvol_path && access(zvol_path, F_OK) == 0;

    /* 1단계: 존재 검증 — [W2 fix] 아무것도 없으면 idempotent 성공 반환
     * (CLAUDE.md "멱등성" 원칙: network.delete 등은 대상이 없어도 성공 반환) */
    gboolean file_exists = file_disk_path && access(file_disk_path, F_OK) == 0;
    if (!dom && !zfs_exists && !file_exists) {
        g_free(zvol_path); g_free(zfs_dataset); g_free(file_disk_path); g_free(saved_xml);
        if (conn) virt_conn_pool_release(conn);
        PCV_LOG_INFO("vm_delete", "VM '%s': already absent (idempotent success)", vm_id);
        g_task_return_boolean(task, TRUE);
        return;
    }

    /*
     * 2단계: libvirt destroy + undefine
     *
     * 실행/일시정지 중이면 먼저 강제 종료(destroy)한 뒤 정의를 제거(undefine)합니다.
     * UndefineFlags로 스냅샷 메타데이터와 managed save 파일도 함께 삭제합니다.
     * FLAGS 기반 undefine이 실패하면(구버전 libvirt 호환) 기본 undefine으로 폴백합니다.
     */
    if (dom) {
        virDomainInfo info;
        virDomainGetInfo(dom, &info);
        if (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_PAUSED)
            virDomainDestroy(dom);
        int undef_rc = virDomainUndefineFlags(dom,
                VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA |
                VIR_DOMAIN_UNDEFINE_MANAGED_SAVE);
        if (undef_rc < 0) {
            /* 구버전 libvirt 폴백 — 반환값 검증 (이전엔 무시) */
            if (virDomainUndefine(dom) < 0) {
                virErrorPtr e = virGetLastError();
                PCV_LOG_WARN("vm_delete", "VM '%s': undefine failed: %s",
                             vm_id, e ? e->message : "unknown");
            }
        }
        virDomainFree(dom);
    }
    if (conn) virt_conn_pool_release(conn);

    /* 삭제된 VM 의 디스패치 항목 정리 (워커 스레드, pcv_spawn_sync 블로킹 규약). */
    pcv_security_group_sync_vm(vm_id);

    /*
     * 3단계: ZFS 블록 디바이스 강제 해제 + destroy ("엑소시즘")
     *
     * ZFS zvol 삭제 전에 해당 블록 디바이스를 사용 중인 모든 프로세스와 커널 참조를
     * 강제로 해제해야 합니다. 그렇지 않으면 "dataset is busy" 에러가 발생합니다.
     *
     * [엑소시즘 파이프라인 상세]
     *   fuser -k -9     : zvol을 열고 있는 모든 프로세스에 SIGKILL 전송
     *   pvs/vgchange    : LVM이 zvol 위에 생성된 경우 VG 비활성화 (OS 설치 잔재)
     *   wipefs -a        : 파일시스템/파티션 시그니처 삭제
     *   dd if=/dev/zero  : 처음 10MB를 0으로 덮어써서 파티션 테이블 완전 제거
     *   partx -d         : 커널 파티션 테이블 캐시에서 제거
     *   kpartx -d        : 디바이스 매퍼 파티션 매핑 제거
     *   partprobe        : 커널에 파티션 변경 알림
     *   udevadm settle   : udev 이벤트 처리 완료 대기 (최대 5초)
     *   sleep 1          : 커널 비동기 해제 안정화 대기
     *
     * [왜 이렇게 공격적인가?]
     *   VM에 OS를 설치하면 zvol 위에 파티션+LVM+파일시스템이 생성됩니다.
     *   커널이 이를 자동 감지하여 참조를 유지하므로, 단순 zfs destroy가 실패합니다.
     *   실전 배포에서 발견된 문제를 해결하기 위해 이 파이프라인이 추가되었습니다.
     */
    gboolean  zfs_success = TRUE;
    gchar    *zfs_err_msg = g_strdup("Success");
    gboolean  exorcism_partial = FALSE;  /* M2: 부분 성공 추적 */

    if (zfs_exists) {
        /* 블록 디바이스 참조 해제 (개별 argv 방식 — command injection 방지)
           M2 fix: 각 단계 실패를 로깅 (이전엔 완전 무시) */
        GError *spawn_e = NULL;

        const gchar *fuser_argv[] = {"fuser", "-k", "-9", zvol_path, NULL};
        if (!pcv_spawn_sync(fuser_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': fuser partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit (non-fatal)");
            exorcism_partial = TRUE;
        }
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        const gchar *wipefs_argv[] = {"wipefs", "-a", zvol_path, NULL};
        if (!pcv_spawn_sync(wipefs_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': wipefs partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit");
            exorcism_partial = TRUE;
        }
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        gchar *dd_of = g_strdup_printf("of=%s", zvol_path);
        const gchar *dd_argv[] = {"dd", "if=/dev/zero", dd_of,
                                   "bs=1M", "count=10", "status=none", NULL};
        if (!pcv_spawn_sync(dd_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': dd zero partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit");
            exorcism_partial = TRUE;
        }
        g_free(dd_of);
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        const gchar *partx_argv[] = {"partx", "-d", zvol_path, NULL};
        (void)pcv_spawn_sync(partx_argv, NULL, NULL, NULL);  /* 파티션 없으면 실패 — 무해 */

        const gchar *kpartx_argv[] = {"kpartx", "-d", zvol_path, NULL};
        (void)pcv_spawn_sync(kpartx_argv, NULL, NULL, NULL);

        const gchar *partprobe_argv[] = {"partprobe", NULL};
        (void)pcv_spawn_sync(partprobe_argv, NULL, NULL, NULL);

        const gchar *udevadm_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
        (void)pcv_spawn_sync(udevadm_argv, NULL, NULL, NULL);

        g_usleep(1 * G_USEC_PER_SEC);

        /* ZFS 재귀적 삭제: -R 플래그로 하위 스냅샷과 클론까지 모두 삭제 */
        gchar *zfs_stderr = NULL;
        const gchar *zfs_argv[] = {"zfs", "destroy", "-R", zfs_dataset, NULL};
        if (!pcv_spawn_sync(zfs_argv, NULL, &zfs_stderr, NULL)) {
            zfs_success = FALSE;
            g_free(zfs_err_msg);
            zfs_err_msg = zfs_stderr ? g_strdup(zfs_stderr) : g_strdup("zfs destroy failed");
        }
        g_free(zfs_stderr);
    }

    g_free(zvol_path);
    g_free(zfs_dataset);

    if (!zfs_success) {
        /* [C1 fix] ZFS destroy 실패 → 저장된 XML로 VM 정의 롤백 시도
           최소한 "XML 없음 + zvol 존재" 불일치를 회피하고, 사용자가 재시도 가능한
           상태로 되돌린다. 재정의도 실패하면 ERROR 레벨로 수동 복구 안내. */
        gboolean redefined = FALSE;
        if (saved_xml) {
            virConnectPtr rc = virt_conn_pool_acquire();
            if (rc) {
                virDomainPtr rdom = virDomainDefineXML(rc, saved_xml);
                if (rdom) {
                    virDomainFree(rdom);
                    redefined = TRUE;
                    PCV_LOG_WARN("vm_delete",
                        "VM '%s': ZFS destroy failed — definition restored from saved XML. "
                        "Delete can be retried.", vm_id);
                } else {
                    virErrorPtr e = virGetLastError();
                    PCV_LOG_ERROR("vm_delete",
                        "VM '%s': ZFS destroy failed AND redefine failed: %s. "
                        "Manual recovery required: restore XML and investigate zvol state.",
                        vm_id, e ? e->message : "unknown");
                }
                virt_conn_pool_release(rc);
            }
        }

        gchar *reason = g_strdup_printf(
            "ZFS destroy failed%s: %s",
            redefined ? " (VM definition rolled back)" : " (VM XML gone — manual recovery)",
            zfs_err_msg);
        g_free(zfs_err_msg);
        g_free(file_disk_path);
        g_free(saved_xml);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", reason);
        g_free(reason);
        return;
    }
    g_free(zfs_err_msg);
    g_free(saved_xml);  /* ZFS 성공 → 롤백 불필요 */

    /*
     * 4단계: qcow2/raw 파일 디스크 삭제
     *
     * ZFS zvol이 아닌 파일 기반 디스크(qcow2, raw)인 경우
     * libvirt XML에서 추출한 경로의 파일을 삭제합니다.
     * /dev/ 경로(zvol)는 이미 3단계에서 처리되었으므로 건너뜁니다.
     *
     * [C3 fix] unlink() 실패는 더 이상 무시하지 않는다 — 상태 불일치 방지
     */
    if (file_disk_path && !g_str_has_prefix(file_disk_path, "/dev/")) {
        if (access(file_disk_path, F_OK) == 0) {
            if (unlink(file_disk_path) == 0) {
                PCV_LOG_INFO("vm_delete", "VM '%s': disk file deleted: %s",
                             vm_id, file_disk_path);
            } else {
                int err = errno;
                PCV_LOG_ERROR("vm_delete", "VM '%s': failed to delete disk file '%s': %s",
                              vm_id, file_disk_path, g_strerror(err));
                gchar *reason = g_strdup_printf(
                    "VM definition removed, but disk file cleanup failed: %s (%s). "
                    "Manual cleanup required.",
                    file_disk_path, g_strerror(err));
                g_free(file_disk_path);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", reason);
                g_free(reason);
                return;
            }
        }
    }
    g_free(file_disk_path);

    if (exorcism_partial) {
        PCV_LOG_WARN("vm_delete", "VM '%s': delete succeeded with partial exorcism (check above warnings)", vm_id);
    }
    g_task_return_boolean(task, TRUE);
}

/**
 * _vm_delete_callback:
 * GTask 워커 완료 후 메인 스레드에서 호출되는 콜백입니다.
 *
 * [fire-and-forget 콜백 — send_response 호출 금지!]
 *   "accepted" 응답은 handle_vm_delete_request()에서 이미 전송되었으므로,
 *   이 시점에서 소켓은 닫혀 있습니다. 응답 전송을 시도하면 크래시/UB가 발생합니다.
 *   성공/실패 모두 로그로만 기록합니다.
 *
 * [컨텍스트 해제] _vm_delete_ctx_free()로 수동 해제합니다.
 *   GTask의 GDestroyNotify에 NULL을 전달했으므로 자동 해제되지 않습니다.
 */
static void
_vm_delete_callback(GObject *src __attribute__((unused)), GAsyncResult *res,
                    gpointer user_data)
{
    VmDeleteCtx *ctx = user_data;
    GError      *err = NULL;

    /* [감사 AF-P1] 오퍼레이션 잠금 해제 — 반드시 최우선(컨테이너 핸들러 규약과 동일).
     * GTask 콜백은 워커 완료 시 항상 실행되므로 여기서 해제하면 모든 경로를 커버한다. */
    unlock_vm_operation(ctx->vm_id);

    gboolean ok = g_task_propagate_boolean(G_TASK(res), &err);
    /* [W1 fix] vm.delete 감사 로그 — 성공/실패 모두 기록 */
    pcv_audit_log(NULL, "vm.delete", ctx->vm_id,
                  ok ? "ok" : "fail",
                  ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION,
                  0, "local");
    /* [M1 fix] cancellable_map에서 제거 — drain cleanup + 메모리 누수 방지 */
    cmap_remove(ctx->vm_id);
    if (!ok) {
        g_warning("[vm.delete] background worker failed for '%s': %s",
                  ctx->vm_id, err ? err->message : "unknown");
        if (err) g_error_free(err);
    } else {
        g_message("[vm.delete] ZFS destroy complete for '%s'", ctx->vm_id);
    }
    _vm_delete_ctx_free(ctx);
}

/**
 * handle_vm_delete_request:
 * vm.delete RPC 진입점 — VM 정의와 ZFS 디스크를 영구 삭제합니다.
 *
 * [fire-and-forget 패턴]
 *   1. 파라미터 검증 (vm_id 필수)
 *   2. 즉시 {"status":"accepted"} 응답 전송 (소켓 닫힘!)
 *   3. GTask 워커 스레드에서 libvirt undefine + ZFS destroy 백그라운드 실행
 *   4. 콜백에서 로그만 기록 (send_response 호출 금지)
 *
 * [이 패턴을 사용하는 이유]
 *   ZFS 엑소시즘(fuser/wipefs/dd/partx) + zfs destroy는 수십 초 소요될 수 있어
 *   동기 응답을 기다리면 클라이언트 타임아웃이 발생합니다.
 *
 * @param params: { "vm_id": "<VM 이름 또는 UUID>" }
 */
void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id || !pcv_validate_vm_name(vm_id)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing or invalid param: vm_id (alphanumeric, -, _ only)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    /* [감사 AF-P1] vm.delete를 오퍼레이션 잠금으로 보호한다. 이전에는 VM delete가
     * 무락이라 동시 vm.create(ZFS create)/vm.start와 무보호 병행했다(ZFS destroy -r
     * vs create 경합, virDomainDestroy+fuser -k vs virDomainCreate). 컨테이너
     * delete 핸들러와 동일 패턴. 잠금은 _vm_delete_callback에서 반드시 해제한다.
     * accepted 응답 전에 획득해야 실패 시 busy로 거부할 수 있다. */
    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_DELETING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
                       lock_err ? lock_err : "VM is busy (another operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }

    /* ── 즉시 accepted 응답 전송 (fire-and-forget 패턴) ──────────────────
     * ZFS destroy + exorcism은 수십 초 걸릴 수 있으므로 먼저 응답을 보내고
     * 백그라운드 GTask에서 실제 삭제를 수행한다.
     *
     * [주니어 참고] fire-and-forget 패턴의 핵심:
     *   1. 여기서 send_response() 호출 → 소켓이 즉시 닫힘
     *   2. 이후 GTask 워커가 백그라운드에서 실제 작업 수행
     *   3. 콜백(_vm_delete_callback)에서 send_response() 호출 금지!
     *      (소켓이 이미 닫혀 있으므로 호출하면 크래시/정의되지 않은 동작 발생)
     *   4. 클라이언트는 vm.delete.status RPC로 진행 상태를 별도 조회 */
    JsonNode *acc_node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(acc_node, "accepted");
    gchar *acc_resp = pure_rpc_build_success_response(rpc_id, acc_node);
    pure_uds_server_send_response(server, connection, acc_resp);  /* 이 시점에서 소켓 닫힘! */
    g_free(acc_resp);

    VmDeleteCtx *ctx = g_new0(VmDeleteCtx, 1);
    ctx->vm_id      = g_strdup(vm_id);
    ctx->rpc_id     = g_strdup(rpc_id);
    ctx->server     = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    /* [M1 fix] GCancellable 생성 + cancellable_map 등록
       - drain 시 cmap_cancel_all()로 전파 → 워커 조기 종료
       - 동일 VM에 대한 중복 delete 요청 시 기존 것 자동 교체 + unref */
    GCancellable *cancel = g_cancellable_new();
    cmap_register(vm_id, cancel);

    /* [주니어 참고] 여기서 GDestroyNotify에 NULL을 전달한 이유:
     * fire-and-forget 패턴에서는 콜백(_vm_delete_callback)에서
     * ctx를 수동으로 해제(_vm_delete_ctx_free)합니다.
     * GTask 자동 해제와 수동 해제가 겹치면 이중 해제(double-free) 크래시가
     * 발생하므로, 의도적으로 NULL을 전달하여 자동 해제를 비활성화합니다. */
    GTask *task = g_task_new(NULL, cancel, _vm_delete_callback, ctx);
    g_task_set_task_data(task, ctx, NULL);  /* NULL = 자동 해제 비활성화 (수동 해제) */
    g_task_run_in_thread(task, _vm_delete_worker);
    g_object_unref(task);
    g_object_unref(cancel);  /* GTask + cmap이 ref를 유지 */
}
/* ─────────────────────────────────────────────────────────────────────────
 * [P0-Fix#2,#3] vm.create : 신규 가상 머신(KVM) 생성
 *   Fix#2 : <n> XML 오타 → <name> 수정
 *   Fix#3 : vcpu / memory_mb / disk_size_gb / iso_path / network_bridge
 *           TUI 파라미터 실제 XML 반영 (기존 하드코딩 제거)
 *
 * [다른 핸들러와의 차이점]
 *   이 함수는 dispatcher.c에서 직접 호출되며, 다른 핸들러와 달리
 *   (server, connection)을 받지 않고 GError**를 통해 에러를 반환합니다.
 *   응답 JSON 문자열을 직접 반환하고, dispatcher.c가 이를 소켓으로 전송합니다.
 *
 * [시그니처가 다른 이유]
 *   vm.create는 프로젝트 초기에 구현되어 dispatcher.c 인라인 호출 방식을 사용합니다.
 *   다른 핸들러는 이후 리팩터링되어 (params, rpc_id, server, connection) 시그니처를
 *   사용하지만, vm.create는 기존 호출 규약을 유지합니다 (호환성 보존).
 *
 * [처리 흐름]
 *   1. JSON 파라미터 파싱 (name, vcpu, memory_mb, disk_size_gb, iso_path, network_bridge)
 *   2. ZFS zvol 블록 디스크 XML 조립 (disk_size_gb > 0인 경우)
 *   3. CD-ROM ISO 마운트 XML 조립 (iso_path 있는 경우)
 *   4. 네트워크 XML 조립 (OVS 브릿지 자동 감지 포함)
 *   5. 전체 libvirt 도메인 XML 조합 (KVM, Q35 머신 타입, host-passthrough CPU)
 *   6. virDomainDefineXML()로 영구적 VM 정의 (Persistent — 재부팅 후에도 유지)
 *
 * @param params: JSON-RPC params 객체
 * @param error: GError 포인터 (실패 시 에러 메시지 설정)
 * @return: JSON 응답 문자열 (성공) 또는 NULL (실패, error에 사유 설정)
 * ───────────────────────────────────────────────────────────────────────── */
gchar *handle_vm_create(JsonObject *params, GError **error) {

    /* ── 파라미터 파싱 ─────────────────────────────────────────────────── */
    const gchar *vm_name = NULL;
    if (json_object_has_member(params, "name"))
        vm_name = json_object_get_string_member(params, "name");

    if (!vm_name || strlen(vm_name) == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "VM name is required.");
        return NULL;
    }

    /* vcpu: TUI 입력값, 기본 2, 최소 1 */
    gint vcpu = 2;
    if (json_object_has_member(params, "vcpu"))
        vcpu = (gint)json_object_get_int_member(params, "vcpu");
    if (vcpu < 1) vcpu = 1;

    /* memory_mb: TUI 입력값(GiB→MB 변환 후 전달), 기본 2048 MB */
    gint64 memory_mb = 2048;
    if (json_object_has_member(params, "memory_mb"))
        memory_mb = json_object_get_int_member(params, "memory_mb");
    if (memory_mb < 512) memory_mb = 512;
    gint64 memory_kib = memory_mb * 1024;

    /* disk_size_gb: 0이면 ZVol 미생성 (디스크 섹션 생략) */
    gint disk_size_gb = 0;
    if (json_object_has_member(params, "disk_size_gb"))
        disk_size_gb = (gint)json_object_get_int_member(params, "disk_size_gb");

    /* iso_path: CD-ROM 선택적 마운트 */
    const gchar *iso_path = NULL;
    if (json_object_has_member(params, "iso_path"))
        iso_path = json_object_get_string_member(params, "iso_path");

    /* VP-1: 브릿지 결정 단일화 — 미지정/""→관리형 기본 네트워크(pcvnat0),
     * "none"→NIC 미부착. req_bridge는 JSON 소유(const), net_bridge는 resolve가
     * 반환한 소유 문자열(gchar*)이므로 이 함수의 모든 return 전에 g_free 필요. */
    const gchar *req_bridge = NULL;
    if (json_object_has_member(params, "network_bridge"))
        req_bridge = json_object_get_string_member(params, "network_bridge");

    /* BUG-15 fix: ovn_switch 파라미터 → br-int 자동 설정 + OVN 포트 생성
     * 사용자가 ovn_switch를 지정하면 network_bridge를 br-int로 오버라이드하고,
     * VM 생성 후 OVN 논리 포트를 자동 생성합니다. (resolve는 br-int를 그대로 통과) */
    const gchar *ovn_switch = NULL;
    if (json_object_has_member(params, "ovn_switch")) {
        ovn_switch = json_object_get_string_member(params, "ovn_switch");
        if (ovn_switch && *ovn_switch) {
            req_bridge = "br-int";  /* OVN 통합 브릿지 */
        }
    }
    gchar *net_bridge = purecvisor_vm_resolve_network_bridge(req_bridge);

    /* ── 파라미터 통합 검증 (신뢰 경계) ────────────────────────────────── */
    GError *validate_err = NULL;
    if (!pcv_validate_vm_create_params(vm_name, vcpu, (gint)memory_mb,
                                       disk_size_gb, iso_path, net_bridge,
                                       &validate_err)) {
        g_propagate_error(error, validate_err);
        g_free(net_bridge);
        return NULL;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Libvirt connection failed.");
        g_free(net_bridge);
        return NULL;
    }

    /* ── 스토리지 타입 선택 ─────────────────────────────────────────── */
    const gchar *storage_type = "zvol";  /* 기본값: ZFS zvol */
    if (json_object_has_member(params, "storage_type"))
        storage_type = json_object_get_string_member(params, "storage_type");

    /* ── 디스크 섹션 (ZVol 블록 디바이스 또는 qcow2 파일) ──────────── */
    gchar *disk_xml = g_strdup("");
    if (disk_size_gb > 0) {
        if (g_strcmp0(storage_type, "qcow2") == 0) {
            /* qcow2 파일 디스크 — image_dir 경로에 생성 */
            const gchar *img_dir = pcv_config_get_image_dir();
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", img_dir, vm_name);
            /* qemu-img create */
            gchar *size_str = g_strdup_printf("%dG", disk_size_gb);
            const gchar *qimg_argv[] = {
                "qemu-img", "create", "-f", "qcow2", qcow2_path, size_str, NULL
            };
            GError *qerr = NULL;
            pcv_spawn_sync(qimg_argv, NULL, NULL, &qerr);
            if (qerr) {
                g_warning("qemu-img create failed: %s", qerr->message);
                g_error_free(qerr);
            }
            g_free(size_str);
            g_free(disk_xml);
            disk_xml = g_strdup_printf(
                "    <disk type='file' device='disk'>"
                  "<driver name='qemu' type='qcow2'/>"
                  "<source file='%s'/>"
                  "<target dev='vda' bus='virtio'/>"
                "</disk>", qcow2_path);
            g_free(qcow2_path);
        } else if (g_strcmp0(storage_type, "raw") == 0) {
            /* raw 파일 디스크 — 스파스(sparse) 할당, I/O 오버헤드 최소 */
            const gchar *img_dir = pcv_config_get_image_dir();
            gchar *raw_path = g_strdup_printf("%s/%s.raw", img_dir, vm_name);
            gchar *size_str = g_strdup_printf("%dG", disk_size_gb);
            const gchar *qimg_argv[] = {
                "qemu-img", "create", "-f", "raw", raw_path, size_str, NULL
            };
            GError *qerr = NULL;
            pcv_spawn_sync(qimg_argv, NULL, NULL, &qerr);
            if (qerr) {
                g_warning("qemu-img create (raw) failed: %s", qerr->message);
                g_error_free(qerr);
            }
            g_free(size_str);
            g_free(disk_xml);
            disk_xml = g_strdup_printf(
                "    <disk type='file' device='disk'>"
                  "<driver name='qemu' type='raw' cache='none' io='native'/>"
                  "<source file='%s'/>"
                  "<target dev='vda' bus='virtio'/>"
                "</disk>", raw_path);
            g_free(raw_path);
        } else {
            /* ZVol 블록 디바이스 (기본) */
            gchar *zvol_dev = g_strdup_printf("/dev/zvol/%s/%s", pcv_config_get_zvol_pool(), vm_name);
            g_free(disk_xml);
            disk_xml = g_strdup_printf(
                "    <disk type='block' device='disk'>"
                  "<driver name='qemu' type='raw'/>"
                  "<source dev='%s'/>"
                  "<target dev='vda' bus='virtio'/>"
                "</disk>", zvol_dev);
            g_free(zvol_dev);
        }
    }

    /* ── CD-ROM 섹션 ───────────────────────────────────────────────────── */
    gchar *cdrom_xml = g_strdup("");
    if (iso_path && strlen(iso_path) > 0) {
        g_free(cdrom_xml);
        cdrom_xml = g_strdup_printf(
            "    <disk type='file' device='cdrom'>"
              "<driver name='qemu' type='raw'/>"
              "<source file='%s'/>"
              "<target dev='sda' bus='sata'/>"
              "<readonly/>"
            "</disk>", iso_path);
    }

    /* ── 부팅 순서: ISO 있으면 cdrom 우선 ─────────────────────────────── */
    const gchar *boot_xml = (iso_path && strlen(iso_path) > 0)
        ? "<boot dev='cdrom'/><boot dev='hd'/>"
        : "<boot dev='hd'/>";

    /* ── 네트워크 섹션 ─────────────────────────────────────────────────── */
    gchar *net_xml = NULL;
    if (net_bridge && strlen(net_bridge) > 0) {
        /* OVS 브릿지 감지 → virtualport 자동 추가 */
        const gchar *ovs_av[] = {"ovs-vsctl", "br-exists", net_bridge, NULL};
        gboolean is_ovs = pcv_spawn_sync(ovs_av, NULL, NULL, NULL);

        net_xml = is_ovs
            ? g_strdup_printf(
                "    <interface type='bridge'>"
                  "<source bridge='%s'/>"
                  "<virtualport type='openvswitch'/>"
                  "<model type='virtio'/>"
                "</interface>", net_bridge)
            : g_strdup_printf(
                "    <interface type='bridge'>"
                  "<source bridge='%s'/>"
                  "<model type='virtio'/>"
                "</interface>", net_bridge);
    } else {
        net_xml = g_strdup(
            "    <interface type='network'>"
              "<source network='default'/>"
              "<model type='virtio'/>"
            "</interface>");
    }

    /*
     * ── [Fix#2] <name> 수정 + [Fix#3] 동적 파라미터 조합 XML ─────────
     *
     * libvirt 도메인 XML 전체를 g_strdup_printf()로 조립합니다.
     *
     * [머신 타입] pc-q35-7.2: PCIe 네이티브 지원 (Q35 칩셋)
     * [CPU 모드] host-passthrough: 호스트 CPU 기능을 게스트에 그대로 노출
     *           (성능 최적화, 마이그레이션 시 동일 CPU 필요)
     * [VNC] port='-1' autoport='yes': QEMU가 자동으로 빈 포트 할당
     *       listen='0.0.0.0': 외부에서 VNC 접속 가능 (noVNC 통합용)
     * [QEMU Guest Agent] virtio 채널로 게스트 에이전트와 통신
     *                    (게스트 IP 조회, 파일시스템 freeze/thaw 등)
     */
    gchar *xml_str = g_strdup_printf(
        "<domain type='kvm'>"
          "<name>%s</name>"                               /* Fix#2: <n>→<name> */
          "<memory unit='KiB'>%lld</memory>"
          "<currentMemory unit='KiB'>%lld</currentMemory>"
          "<vcpu placement='static'>%d</vcpu>"            /* Fix#3: 동적 vcpu  */
          "<os>"
            "<type arch='x86_64' machine='pc-q35-7.2'>hvm</type>"
            "%s"                                          /* boot_xml           */
          "</os>"
          "<features><acpi/><apic/></features>"
          "<cpu mode='host-passthrough' check='none'/>"
          "<devices>"
            "<emulator>/usr/bin/qemu-system-x86_64</emulator>"
            "%s"                                          /* disk_xml           */
            "%s"                                          /* cdrom_xml          */
            "%s"                                          /* net_xml            */
            "<graphics type='vnc' port='-1' autoport='yes' listen='0.0.0.0'/>"
            "<video><model type='virtio'/></video>"
            "<channel type='unix'>"
              "<target type='virtio' name='org.qemu.guest_agent.0'/>"
            "</channel>"
          "</devices>"
        "</domain>",
        vm_name,
        (long long)memory_kib, (long long)memory_kib,    /* Fix#3: 동적 memory */
        vcpu,
        boot_xml,
        disk_xml, cdrom_xml, net_xml);

    g_free(disk_xml);
    g_free(cdrom_xml);
    g_free(net_xml);
    g_free(net_bridge);  /* VP-1: resolve 소유 문자열 해제 (이후 net_bridge 미사용) */

    /*
     * XML을 Libvirt에 밀어 넣어 영구적인(Persistent) VM으로 정의합니다.
     * virDomainDefineXML()은 VM을 "정의만" 합니다 (시작하지 않음).
     * VM을 시작하려면 vm.start RPC를 별도로 호출해야 합니다.
     *
     * [Persistent vs Transient]
     *   DefineXML = Persistent: 호스트 재부팅 후에도 VM 정의가 유지됩니다.
     *   CreateXML = Transient: VM 종료 시 정의가 사라집니다 (일회성).
     */
    virDomainPtr dom = virDomainDefineXML(conn, xml_str);
    g_free(xml_str);
    if (!dom) {
        virErrorPtr libvirt_err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create VM: %s", libvirt_err ? libvirt_err->message : "Unknown Libvirt Error");
        virt_conn_pool_release(conn);
        return NULL;
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);

    /*
     * 성공 JSON 응답 직접 조립
     *
     * [다른 핸들러와의 차이점]
     *   다른 핸들러는 pure_rpc_build_success_response()를 사용하지만,
     *   vm.create는 dispatcher.c에 직접 JSON 문자열을 반환하므로
     *   수동으로 JSON-RPC 2.0 형식을 조립합니다.
     *
     *   반환 형식: {"jsonrpc":"2.0","id":"create-req","result":{"status":"success","message":"..."}}
     */
    /* BUG-15: OVN 스위치 지정 시 자동 포트 생성 + VM 인터페이스 바인딩
     * vm.create 완료 후, OVN 논리 포트를 생성하고 VM의 MAC 주소로 설정합니다.
     * 이를 통해 VM이 OVN 오버레이 네트워크에 자동 참여합니다. */
    if (ovn_switch && *ovn_switch) {
        gchar *port_name = g_strdup_printf("%s-port", vm_name);
        /* ovn-nbctl lsp-add <switch> <port> */
        const gchar *add_argv[] = {"ovn-nbctl", "lsp-add", ovn_switch, port_name, NULL};
        pcv_spawn_sync(add_argv, NULL, NULL, NULL);
        /* ovn-nbctl lsp-set-addresses <port> "dynamic" (OVN DHCP 자동 할당) */
        const gchar *addr_argv[] = {"ovn-nbctl", "lsp-set-addresses", port_name, "dynamic", NULL};
        pcv_spawn_sync(addr_argv, NULL, NULL, NULL);
        PCV_LOG_INFO("vm_manager", "OVN port '%s' created on switch '%s' for VM '%s'",
                     port_name, ovn_switch, vm_name);
        g_free(port_name);
    }

    JsonObject *rpc_resp = json_object_new();
    json_object_set_string_member(rpc_resp, "jsonrpc", "2.0");
    json_object_set_string_member(rpc_resp, "id", "create-req");
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "status", "success");
    json_object_set_string_member(res_obj, "message", "VM Created Successfully.");
    json_object_set_object_member(rpc_resp, "result", res_obj);

    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, rpc_resp);
    gchar *response_str = json_to_string(root_node, FALSE);
    json_node_free(root_node);

    return response_str;
}

/* =================================================================
 * Guest Agent 연동 RPC 핸들러 (3개)
 *
 * qemu-guest-agent와 통신하여 게스트 OS를 직접 제어합니다.
 * virDomainQemuAgentCommand() API를 사용하며 -lvirt-qemu 링크가 필요합니다.
 *
 * [공통 파라미터 추출 매크로 대신 인라인 헬퍼]
 *   "name" 파라미터를 우선 사용하고, 없으면 "vm_id" 폴백
 * ================================================================= */

/**
 * _guest_get_vm_name:
 * params에서 "name" 또는 "vm_id" 키를 추출합니다.
 * 둘 다 없으면 NULL을 반환합니다.
 */
static const gchar *
_guest_get_vm_name(JsonObject *params)
{
    if (!params) return NULL;
    if (json_object_has_member(params, "name"))
        return json_object_get_string_member(params, "name");
    if (json_object_has_member(params, "vm_id"))
        return json_object_get_string_member(params, "vm_id");
    return NULL;
}

static gboolean
_guest_agent_xml_has_channel(const gchar *xml)
{
    return xml && strstr(xml, "org.qemu.guest_agent.0") != NULL;
}

static void
_guest_agent_add_install_commands(JsonObject *obj)
{
    JsonObject *cmds = json_object_new();
    json_object_set_string_member(cmds, "debian_ubuntu",
        "sudo apt update && sudo apt install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_string_member(cmds, "rhel_rocky_fedora",
        "sudo dnf install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_string_member(cmds, "suse",
        "sudo zypper install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_object_member(obj, "install_commands", cmds);
}

/* =================================================================
 * vm.guest.agent.status — channel/agent 상태 진단
 * ================================================================= */

void
handle_vm_guest_agent_status_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
                         "Failed to connect to Libvirt.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
                         "VM not found.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gboolean running = virDomainIsActive(dom) == 1;
    char *config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    char *live_xml = virDomainGetXMLDesc(dom, 0);
    gboolean channel_configured = _guest_agent_xml_has_channel(config_xml);
    gboolean channel_live = _guest_agent_xml_has_channel(live_xml);
    gboolean agent_ping = FALSE;
    gchar *agent_error = NULL;

    if (running && channel_live) {
        char *result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-ping\"}", 3, 0);
        if (result) {
            agent_ping = TRUE;
            free(result);
        } else {
            const char *vir_err = virGetLastErrorMessage();
            agent_error = g_strdup(vir_err ? vir_err : "unknown error");
            virResetLastError();
        }
    }

    const gchar *status = "channel_missing";
    const gchar *message = "Guest agent channel is not configured.";
    if (agent_ping) {
        status = "ok";
        message = "Guest agent is responding.";
    } else if (!running && (channel_configured || channel_live)) {
        status = "vm_stopped";
        message = "Guest agent channel is configured; start the VM to verify the agent.";
    } else if (running && channel_configured && !channel_live) {
        status = "reboot_required";
        message = "Guest agent channel is configured for the next boot; restart the VM or attach it live.";
    } else if (running && channel_live) {
        status = "agent_unavailable";
        message = "Guest agent channel exists, but qemu-guest-agent is not responding in the guest.";
    }

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", vm_name);
    json_object_set_string_member(res_obj, "status", status);
    json_object_set_string_member(res_obj, "message", message);
    json_object_set_boolean_member(res_obj, "running", running);
    json_object_set_boolean_member(res_obj, "channel_present", channel_configured || channel_live);
    json_object_set_boolean_member(res_obj, "channel_configured", channel_configured);
    json_object_set_boolean_member(res_obj, "channel_live", channel_live);
    json_object_set_boolean_member(res_obj, "agent_ping", agent_ping);
    json_object_set_boolean_member(res_obj, "reboot_required", running && channel_configured && !channel_live);
    json_object_set_boolean_member(res_obj, "package_required", running && channel_live && !agent_ping);
    json_object_set_boolean_member(res_obj, "can_ensure_channel", TRUE);
    if (agent_error)
        json_object_set_string_member(res_obj, "agent_error", agent_error);
    _guest_agent_add_install_commands(res_obj);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    g_free(agent_error);
    if (config_xml) free(config_xml);
    if (live_xml) free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * vm.guest.agent.ensure_channel — qemu guest agent channel 보정
 * ================================================================= */

void
handle_vm_guest_agent_ensure_channel_request(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
                         "Failed to connect to Libvirt.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
                         "VM not found.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gboolean running = virDomainIsActive(dom) == 1;
    gboolean persistent = virDomainIsPersistent(dom) == 1;
    char *config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    char *live_xml = virDomainGetXMLDesc(dom, 0);
    gboolean channel_configured = _guest_agent_xml_has_channel(config_xml);
    gboolean channel_live = _guest_agent_xml_has_channel(live_xml);
    gboolean config_changed = FALSE;
    gboolean live_changed = FALSE;
    gchar *config_error = NULL;
    gchar *live_error = NULL;

    const gchar *channel_xml =
        "<channel type='unix'>"
          "<target type='virtio' name='org.qemu.guest_agent.0'/>"
        "</channel>";

    if (!channel_configured && persistent) {
        if (virDomainAttachDeviceFlags(dom, channel_xml, VIR_DOMAIN_AFFECT_CONFIG) == 0) {
            channel_configured = TRUE;
            config_changed = TRUE;
        } else {
            const char *vir_err = virGetLastErrorMessage();
            config_error = g_strdup(vir_err ? vir_err : "unknown error");
            virResetLastError();
        }
    }

    if (running && !channel_live) {
        if (virDomainAttachDeviceFlags(dom, channel_xml, VIR_DOMAIN_AFFECT_LIVE) == 0) {
            channel_live = TRUE;
            live_changed = TRUE;
        } else {
            const char *vir_err = virGetLastErrorMessage();
            live_error = g_strdup(vir_err ? vir_err : "unknown error");
            virResetLastError();
        }
    }

    if (!channel_configured && !channel_live) {
        gchar *msg = g_strdup_printf(
            "Failed to add guest agent channel.%s%s%s%s",
            config_error ? " config: " : "", config_error ? config_error : "",
            live_error ? " live: " : "", live_error ? live_error : "");
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(msg);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "name", vm_name);
        json_object_set_string_member(res_obj, "status",
            (config_changed || live_changed) ? "updated" : "already_configured");
        json_object_set_boolean_member(res_obj, "running", running);
        json_object_set_boolean_member(res_obj, "persistent", persistent);
        json_object_set_boolean_member(res_obj, "changed", config_changed || live_changed);
        json_object_set_boolean_member(res_obj, "channel_configured", channel_configured);
        json_object_set_boolean_member(res_obj, "channel_live", channel_live);
        json_object_set_boolean_member(res_obj, "reboot_required", channel_configured && running && !channel_live);
        json_object_set_boolean_member(res_obj, "install_required", TRUE);
        if (config_error)
            json_object_set_string_member(res_obj, "config_warning", config_error);
        if (live_error)
            json_object_set_string_member(res_obj, "live_warning", live_error);
        _guest_agent_add_install_commands(res_obj);

        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(config_error);
    g_free(live_error);
    if (config_xml) free(config_xml);
    if (live_xml) free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * vm.guest.fsinfo — qemu guest agent 파일시스템 사용량 조회
 * ================================================================= */

static gboolean
_guest_fsinfo_get_int64(JsonObject *obj, const gchar *member, gint64 *out)
{
    if (!obj || !member || !json_object_has_member(obj, member))
        return FALSE;
    *out = json_object_get_int_member(obj, member);
    return TRUE;
}

static gboolean
_guest_fsinfo_should_count(const gchar *type)
{
    static const gchar *skip[] = {
        "tmpfs", "devtmpfs", "proc", "sysfs", "devpts",
        "cgroup", "cgroup2", "squashfs", "overlay", NULL
    };

    if (!type || !*type)
        return TRUE;
    for (guint i = 0; skip[i]; i++) {
        if (g_strcmp0(type, skip[i]) == 0)
            return FALSE;
    }
    return TRUE;
}

static void
_guest_fsinfo_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                     gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

    char *agent_result = virDomainQemuAgentCommand(
        dom, "{\"execute\":\"guest-get-fsinfo\"}", 10, 0);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!agent_result) {
        const char *vir_err = virGetLastErrorMessage();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-get-fsinfo failed on '%s': %s",
                                ctx->vm_id, vir_err ? vir_err : "unknown error");
        return;
    }

    GError *parse_error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, agent_result, -1, &parse_error)) {
        free(agent_result);
        g_object_unref(parser);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to parse guest-get-fsinfo response: %s",
                                parse_error ? parse_error->message : "invalid JSON");
        g_clear_error(&parse_error);
        return;
    }
    free(agent_result);

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *root_obj = root && JSON_NODE_HOLDS_OBJECT(root)
        ? json_node_get_object(root) : NULL;
    if (!root_obj || !json_object_has_member(root_obj, "return")) {
        g_object_unref(parser);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-get-fsinfo response missing return array.");
        return;
    }

    JsonArray *raw_filesystems = json_object_get_array_member(root_obj, "return");
    JsonArray *filesystems = json_array_new();
    gint64 total_bytes = 0;
    gint64 used_bytes = 0;

    for (guint i = 0; raw_filesystems && i < json_array_get_length(raw_filesystems); i++) {
        JsonObject *raw = json_array_get_object_element(raw_filesystems, i);
        if (!raw)
            continue;

        const gchar *name = json_object_has_member(raw, "name")
            ? json_object_get_string_member(raw, "name") : "";
        const gchar *mountpoint = json_object_has_member(raw, "mountpoint")
            ? json_object_get_string_member(raw, "mountpoint") : "";
        const gchar *type = json_object_has_member(raw, "type")
            ? json_object_get_string_member(raw, "type") : "";

        gint64 fs_total = 0;
        gint64 fs_used = 0;
        gboolean has_total = _guest_fsinfo_get_int64(raw, "total-bytes", &fs_total);
        gboolean has_used = _guest_fsinfo_get_int64(raw, "used-bytes", &fs_used);

        JsonObject *fs_obj = json_object_new();
        json_object_set_string_member(fs_obj, "name", name);
        json_object_set_string_member(fs_obj, "mountpoint", mountpoint);
        json_object_set_string_member(fs_obj, "type", type);
        if (has_total)
            json_object_set_int_member(fs_obj, "total_bytes", fs_total);
        if (has_used)
            json_object_set_int_member(fs_obj, "used_bytes", fs_used);
        if (has_total && has_used && fs_total >= fs_used) {
            json_object_set_int_member(fs_obj, "available_bytes", fs_total - fs_used);
            if (fs_total > 0) {
                double pct = ((double)fs_used * 100.0) / (double)fs_total;
                json_object_set_double_member(fs_obj, "usage_percent", pct);
            }
        }

        if (json_object_has_member(raw, "disk")) {
            JsonArray *disks = json_object_get_array_member(raw, "disk");
            if (disks && json_array_get_length(disks) > 0) {
                JsonObject *disk = json_array_get_object_element(disks, 0);
                if (disk && json_object_has_member(disk, "dev"))
                    json_object_set_string_member(fs_obj, "device",
                                                  json_object_get_string_member(disk, "dev"));
            }
        }

        if (has_total && has_used && fs_total > 0 && _guest_fsinfo_should_count(type)) {
            total_bytes += fs_total;
            used_bytes += fs_used;
        }

        json_array_add_object_element(filesystems, fs_obj);
    }

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", ctx->vm_id);
    json_object_set_string_member(res_obj, "status", "ok");
    json_object_set_int_member(res_obj, "total_bytes", total_bytes);
    json_object_set_int_member(res_obj, "used_bytes", used_bytes);
    if (total_bytes > 0)
        json_object_set_double_member(res_obj, "usage_percent",
                                      ((double)used_bytes * 100.0) / (double)total_bytes);
    json_object_set_array_member(res_obj, "filesystems", filesystems);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    g_object_unref(parser);
    g_task_return_pointer(task, res_node, (GDestroyNotify)json_node_free);
}

static void
_guest_fsinfo_callback(GObject *source_obj __attribute__((unused)),
                       GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    JsonNode *result_node = g_task_propagate_pointer(task, &error);

    pcv_audit_log(NULL, "vm.guest.fsinfo", ctx->vm_id ?: "",
                  error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");

    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id,
            PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_fsinfo_request(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_fsinfo_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_fsinfo_worker);
    g_object_unref(task);
}

/* =================================================================
 * vm.guest.ping — qemu-guest-agent 응답 확인
 *
 * [동작] guest-ping QMP 명령을 5초 타임아웃으로 전송합니다.
 *   - 응답이 오면: {"agent":"connected","name":"<vm>"}
 *   - 에이전트 미설치/미실행: -32000 에러
 *
 * [동기 패턴] agent ping은 최대 5초이므로 GTask 워커 사용
 * ================================================================= */

static void
_guest_ping_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                   gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

    /* guest-ping: 5초 타임아웃 — 에이전트 응답 확인 */
    char *result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-ping\"}",
                                              5, 0);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!result) {
        const char *vir_err = virGetLastErrorMessage();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Guest agent not available on '%s': %s",
                                ctx->vm_id, vir_err ? vir_err : "unknown error");
        return;
    }

    free(result);  /* virDomainQemuAgentCommand returns malloc'd string */
    g_task_return_boolean(task, TRUE);
}

static void
_guest_ping_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _ok = g_task_propagate_boolean(task, &error);
    /* ADR-0018: 워커 결과 audit */
    pcv_audit_log(NULL, "vm.guest.ping", ctx->vm_id ?: "",
                  _ok ? "ok" : "fail",
                  _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (!_ok) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "agent", "connected");
        json_object_set_string_member(res_obj, "name", ctx->vm_id);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, res_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_ping_request(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_ping_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_ping_worker);
    g_object_unref(task);
}

/* =================================================================
 * vm.guest.exec — 게스트 OS에서 명령 실행 (guest-exec + guest-exec-status)
 *
 * [보안] command는 비어 있으면 안 되며 최대 1024자로 제한합니다.
 * [동작]
 *   1. guest-exec로 /bin/sh -c "<command>" 실행 → PID 획득
 *   2. guest-exec-status를 exited==true까지 폴링(200ms×최대 50회)해 결과 수집
 *   3. base64 디코딩 후 반환: {"name":"...", "exitcode":0, "exited":true,
 *      "stdout":"...", "stderr":"..."} — 예산 초과 시 exited:false
 *
 * [타임아웃] agent 명령 30초
 * ================================================================= */

/**
 * GuestExecCtx:
 * guest.exec 전용 컨텍스트 — command 필드 추가
 */
typedef struct {
    gchar *vm_id;
    gchar *command;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
} GuestExecCtx;

static void
_guest_exec_ctx_free(gpointer data)
{
    if (!data) return;
    GuestExecCtx *ctx = (GuestExecCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->command);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static void
_guest_exec_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                   gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    GuestExecCtx *ctx = (GuestExecCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

    /* 1단계: guest-exec로 명령 실행 — /bin/sh -c "<command>"
     *
     * [주니어 참고] 이스케이프 처리가 필요한 이유:
     *   사용자 명령(command)을 JSON 문자열 안에 포함시키므로,
     *   큰따옴표(")와 백슬래시(\)가 있으면 JSON이 깨집니다.
     *   예: 'echo "hello"' → 'echo \"hello\"' 로 변환해야 합니다. */
    GString *safe_cmd = g_string_new(NULL);
    for (const gchar *p = ctx->command; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(safe_cmd, '\\');
        g_string_append_c(safe_cmd, *p);
    }

    gchar *exec_json = g_strdup_printf(
        "{\"execute\":\"guest-exec\",\"arguments\":"
        "{\"path\":\"/bin/sh\",\"arg\":[\"-c\",\"%s\"],\"capture-output\":true}}",
        safe_cmd->str);
    g_string_free(safe_cmd, TRUE);

    char *exec_result = virDomainQemuAgentCommand(dom, exec_json,
                                                   30, 0);
    g_free(exec_json);

    if (!exec_result) {
        const char *vir_err = virGetLastErrorMessage();
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-exec failed on '%s': %s",
                                ctx->vm_id, vir_err ? vir_err : "unknown error");
        return;
    }

    /* 2단계: exec_result에서 PID 추출 */
    JsonParser *parser = json_parser_new();
    gint64 pid = -1;
    if (json_parser_load_from_data(parser, exec_result, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "return")) {
                JsonObject *ret_obj = json_object_get_object_member(obj, "return");
                if (ret_obj && json_object_has_member(ret_obj, "pid"))
                    pid = json_object_get_int_member(ret_obj, "pid");
            }
        }
    }
    g_object_unref(parser);
    free(exec_result);

    if (pid < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to parse guest-exec PID from response.");
        return;
    }

    /* 3단계: guest-exec-status 폴링 — exited==true까지 (200ms × 최대 50회 ≈ 10s)
     * QEMU GA는 미종료 프로세스에 {"exited": false}만 반환(exitcode/out-data 없음)
     * 하므로 단발 조회로는 오래 걸리는 명령의 결과를 놓친다 (VP-3).
     * 예산 소진 시 exited=false 그대로 반환 — 호출자가 실행 중임을 구분 가능. */
    gint64 exitcode = -1;
    gboolean exited = FALSE;
    gchar *stdout_decoded = NULL;
    gchar *stderr_decoded = NULL;

    gchar *status_json = g_strdup_printf(
        "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":%" G_GINT64_FORMAT "}}",
        pid);

    for (int attempt = 0; attempt < 50; attempt++) {
        char *status_result = virDomainQemuAgentCommand(dom, status_json,
                                                         30, 0);
        if (!status_result) {
            g_free(status_json);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "guest-exec-status failed for PID %" G_GINT64_FORMAT, pid);
            return;
        }

        /* 결과 파싱 — exited 확인 후, 종료 시에만 exitcode/out-data/err-data 수집 */
        JsonParser *sp = json_parser_new();
        if (json_parser_load_from_data(sp, status_result, -1, NULL)) {
            JsonNode *sroot = json_parser_get_root(sp);
            if (sroot && JSON_NODE_HOLDS_OBJECT(sroot)) {
                JsonObject *sobj = json_node_get_object(sroot);
                if (json_object_has_member(sobj, "return")) {
                    JsonObject *sret = json_object_get_object_member(sobj, "return");
                    if (sret) {
                        exited = json_object_get_boolean_member_with_default(
                                     sret, "exited", FALSE);
                        if (exited) {
                            if (json_object_has_member(sret, "exitcode"))
                                exitcode = json_object_get_int_member(sret, "exitcode");

                            if (json_object_has_member(sret, "out-data")) {
                                const gchar *b64 = json_object_get_string_member(sret, "out-data");
                                if (b64) {
                                    gsize out_len = 0;
                                    guchar *decoded = g_base64_decode(b64, &out_len);
                                    stdout_decoded = g_strndup((const gchar *)decoded, out_len);
                                    g_free(decoded);
                                }
                            }

                            if (json_object_has_member(sret, "err-data")) {
                                const gchar *b64 = json_object_get_string_member(sret, "err-data");
                                if (b64) {
                                    gsize out_len = 0;
                                    guchar *decoded = g_base64_decode(b64, &out_len);
                                    stderr_decoded = g_strndup((const gchar *)decoded, out_len);
                                    g_free(decoded);
                                }
                            }
                        }
                    }
                }
            }
        }
        g_object_unref(sp);
        free(status_result);

        if (exited) break;
        g_usleep(200000);  /* 200ms 후 재조회 */
    }
    g_free(status_json);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    /* 5단계: 결과 JSON 노드 조립 */
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", ctx->vm_id);
    json_object_set_int_member(res_obj, "exitcode", exitcode);
    json_object_set_boolean_member(res_obj, "exited", exited);
    json_object_set_string_member(res_obj, "stdout", stdout_decoded ? stdout_decoded : "");
    json_object_set_string_member(res_obj, "stderr", stderr_decoded ? stderr_decoded : "");

    g_free(stdout_decoded);
    g_free(stderr_decoded);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    g_task_return_pointer(task, res_node, (GDestroyNotify)json_node_free);
}

static void
_guest_exec_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    GuestExecCtx *ctx = (GuestExecCtx *)user_data;
    GError *error = NULL;

    JsonNode *result_node = g_task_propagate_pointer(task, &error);
    /* ADR-0018: 워커 결과 audit (보안 민감 — 명령 실행) */
    pcv_audit_log(NULL, "vm.guest.exec", ctx->vm_id ?: "",
                  error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");
    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_exec_request(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    const gchar *command = NULL;
    if (params && json_object_has_member(params, "command"))
        command = json_object_get_string_member(params, "command");

    if (!command || strlen(command) == 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'command' must be non-empty");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    if (strlen(command) > 1024) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'command' exceeds 1024 characters");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    GuestExecCtx *ctx = g_new0(GuestExecCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->command = g_strdup(command);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_exec_callback, ctx);
    g_task_set_task_data(task, ctx, _guest_exec_ctx_free);
    g_task_run_in_thread(task, _guest_exec_worker);
    g_object_unref(task);
}

/* =================================================================
 * vm.guest.shutdown — 게스트 에이전트 경유 정상 종료 (ACPI 폴백)
 *
 * [동작]
 *   1차: VIR_DOMAIN_SHUTDOWN_GUEST_AGENT 플래그로 깔끔한 종료 시도
 *   2차: 에이전트 실패 시 virDomainShutdown() ACPI 전원 버튼 폴백
 *
 * [응답] {"status":"shutdown_initiated","name":"<vm>","method":"guest-agent|acpi"}
 * ================================================================= */

static void
_guest_shutdown_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                       gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

    /* 1차: guest-agent 경유 종료 시도 */
    const gchar *method_used = "guest-agent";
    int rc = virDomainShutdownFlags(dom, VIR_DOMAIN_SHUTDOWN_GUEST_AGENT);
    if (rc < 0) {
        /* 2차: ACPI 전원 버튼 폴백 */
        virResetLastError();
        method_used = "acpi";
        rc = virDomainShutdown(dom);
        if (rc < 0) {
            const char *vir_err = virGetLastErrorMessage();
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Failed to shutdown VM '%s': %s",
                                    ctx->vm_id, vir_err ? vir_err : "unknown error");
            return;
        }
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);

    /* 결과 JSON: method_used를 action 필드에 임시 저장 (콜백에서 읽음) */
    g_free(ctx->action);
    ctx->action = g_strdup(method_used);
    g_task_return_boolean(task, TRUE);
}

static void
_guest_shutdown_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _ok = g_task_propagate_boolean(task, &error);
    /* ADR-0018: 워커 결과 audit */
    pcv_audit_log(NULL, "vm.guest.shutdown", ctx->vm_id ?: "",
                  _ok ? "ok" : "fail",
                  _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (!_ok) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "status", "shutdown_initiated");
        json_object_set_string_member(res_obj, "name", ctx->vm_id);
        json_object_set_string_member(res_obj, "method", ctx->action ? ctx->action : "unknown");
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, res_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_shutdown_request(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_shutdown_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_shutdown_worker);
    g_object_unref(task);
}
