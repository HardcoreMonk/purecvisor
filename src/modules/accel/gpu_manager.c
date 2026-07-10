/**
 * @file gpu_manager.c
 * @brief GPU Passthrough (vGPU) 매니저 — sysfs 열거 + mdevctl + vfio-pci
 *
 * [파일 역할]
 *   호스트의 GPU 장치를 열거하고, vGPU(Mediated Device) 생성/삭제 및
 *   PCI Passthrough(vfio-pci)를 통한 VM 직접 연결을 관리하는 모듈.
 *   외부 명령(lspci, mdevctl, virsh)을 래핑하여 RPC/REST API로 노출한다.
 *
 * [아키텍처 위치]
 *   handler_accel.c (gpu.* RPC 핸들러)
 *     → pcv_gpu_list/info/vgpu_create/attach/detach()  [이 파일]
 *       → pcv_spawn_sync() 경유 외부 명령 실행
 *
 * [핵심 패턴]
 *   - lspci: VGA/3D/Display 컨트롤러를 grep으로 필터링하여 GPU 목록 생성
 *   - sysfs: /sys/bus/pci/devices/0000:<addr>/sriov_totalvfs로 SR-IOV 지원 확인
 *   - sysfs: /sys/bus/pci/devices/0000:<addr>/mdev_supported_types로 vGPU 지원 확인
 *   - mdevctl: vGPU 인스턴스 생성/삭제/목록 (mdevctl start/stop/list)
 *   - virsh: attach-device/detach-device로 VM에 PCI 장치 연결/해제
 *
 * [PCI Passthrough 원리]
 *   GPU를 VM에 직접 할당(passthrough)하려면:
 *   1. 호스트에서 GPU를 vfio-pci 드라이버에 바인딩
 *   2. libvirt XML에 <hostdev type='pci'> 정의
 *   3. virsh attach-device로 VM에 연결
 *   domain/bus/slot/function 4개 필드로 PCI 주소를 XML에 인코딩한다.
 *
 * [스레드 안전]
 *   모든 함수가 동기적이며 전역 상태 없음. RPC 핸들러에서 직접 호출.
 *
 * [외부 의존성]
 *   lspci (pciutils), mdevctl, virsh (libvirt-clients)
 */
#include "gpu_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include <string.h>
#include <stdio.h>
#include <glib/gstdio.h>

#define GPU_LOG_DOM "gpu_manager"

/**
 * _run:
 * @cmd:   실행할 셸 명령어 문자열
 * @out:   stdout 출력 (NULL 가능, 호출자 g_free)
 * @error: GError 반환
 *
 * /bin/sh -c 경유로 셸 명령을 동기 실행하는 내부 헬퍼.
 * pcv_spawn_sync()를 래핑하여 에러 시 로그를 남긴다.
 *
 * Returns: 명령 성공 시 TRUE
 */
/** argv 분리 실행 (shell 미경유, 인젝션 방지) */
static gboolean
_run(const gchar *cmd, gchar **out, GError **error)
{
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &se, error);
    if (!ok) PCV_LOG_WARN(GPU_LOG_DOM, "cmd failed: %s  err=%s", cmd, se ? se : "");
    g_free(se);
    g_strfreev(parsed);
    return ok;
}
/** shell 기능 필요 시 전용 (하드코딩 명령만 사용할 것) */
static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &se, error);
    if (!ok) PCV_LOG_WARN(GPU_LOG_DOM, "cmd failed: %s  err=%s", cmd, se ? se : "");
    g_free(se);
    return ok;
}

/** pcv_gpu_init: GPU 매니저 초기화 (현재 로그만 출력, 상태 없음) */
void pcv_gpu_init(void)  { PCV_LOG_INFO(GPU_LOG_DOM, "GPU manager initialized"); }
/** pcv_gpu_shutdown: GPU 매니저 종료 (현재 no-op) */
void pcv_gpu_shutdown(void) {}

/**
 * pcv_gpu_list:
 *
 * 호스트의 모든 GPU 장치를 열거하여 JsonArray로 반환한다.
 * lspci -nn 출력에서 VGA/3D/Display 키워드를 필터링하고,
 * 각 GPU에 대해 SR-IOV VF 수와 mdev(vGPU) 지원 여부를 추가로 조회한다.
 *
 * Returns: (transfer full): JsonArray* (각 원소: {pci_addr, description, sriov_vfs, mdev_supported})
 */
JsonArray *pcv_gpu_list(void)
{
    JsonArray *arr = json_array_new();
    /* lspci 기반 GPU 열거 */
    gchar *out = NULL;
    if (_run_shell("lspci -nn 2>/dev/null | grep -iE 'VGA|3D|Display'", &out, NULL) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gint i = 0; lines[i] && lines[i][0]; i++) {
            JsonObject *gpu = json_object_new();
            /* PCI 주소 (첫 7자: "00:02.0") */
            gchar pci[16] = {0};
            g_strlcpy(pci, lines[i], 8);
            json_object_set_string_member(gpu, "pci_addr", g_strstrip(pci));
            json_object_set_string_member(gpu, "description", lines[i]);
            /* SR-IOV 지원 확인 */
            gchar *sriov_cmd = g_strdup_printf(
                "test -f /sys/bus/pci/devices/0000:%s/sriov_totalvfs && cat /sys/bus/pci/devices/0000:%s/sriov_totalvfs 2>/dev/null || echo 0", pci, pci);
            gchar *vfs = NULL;
            if (_run(sriov_cmd, &vfs, NULL) && vfs)
                json_object_set_int_member(gpu, "sriov_vfs", g_ascii_strtoll(g_strstrip(vfs), NULL, 10));
            g_free(vfs); g_free(sriov_cmd);
            /* mdev 지원 */
            gchar *mdev_cmd = g_strdup_printf("test -d /sys/bus/pci/devices/0000:%s/mdev_supported_types && echo yes || echo no", pci);
            gchar *mdev = NULL;
            if (_run(mdev_cmd, &mdev, NULL) && mdev)
                json_object_set_boolean_member(gpu, "mdev_supported", g_strcmp0(g_strstrip(mdev), "yes") == 0);
            g_free(mdev); g_free(mdev_cmd);
            json_array_add_object_element(arr, gpu);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/**
 * pcv_gpu_info:
 * @pci_addr: PCI 주소 (예: "00:02.0")
 *
 * lspci -v로 해당 GPU의 상세 정보를 조회하여 JsonObject로 반환한다.
 *
 * Returns: (transfer full): JsonObject* (json_object_unref 필요)
 */
JsonObject *pcv_gpu_info(const gchar *pci_addr)
{
    JsonObject *obj = json_object_new();
    if (!pci_addr) return obj;
    json_object_set_string_member(obj, "pci_addr", pci_addr);
    gchar *cmd = g_strdup_printf("lspci -v -s %s 2>/dev/null", pci_addr);
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out)
        json_object_set_string_member(obj, "detail", out);
    g_free(out); g_free(cmd);
    return obj;
}

/**
 * pcv_gpu_vgpu_types:
 * @pci_addr: PCI 주소
 *
 * 해당 GPU가 지원하는 vGPU(mdev) 타입 목록을 반환한다.
 * sysfs의 mdev_supported_types 디렉터리를 ls로 조회한다.
 *
 * Returns: (transfer full): JsonArray* (문자열 배열)
 */
JsonArray *pcv_gpu_vgpu_types(const gchar *pci_addr)
{
    JsonArray *arr = json_array_new();
    if (!pci_addr) return arr;
    gchar *cmd = g_strdup_printf("ls /sys/bus/pci/devices/0000:%s/mdev_supported_types/ 2>/dev/null", pci_addr);
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out) {
        gchar **types = g_strsplit(g_strstrip(out), "\n", -1);
        for (gint i = 0; types[i] && types[i][0]; i++)
            json_array_add_string_element(arr, types[i]);
        g_strfreev(types);
    }
    g_free(out); g_free(cmd);
    return arr;
}

/**
 * pcv_gpu_vgpu_create:
 * @pci_addr: 부모 GPU의 PCI 주소
 * @type:     vGPU 타입 (mdev_supported_types 중 하나)
 * @uuid_out: 생성된 vGPU의 UUID (out 파라미터, 호출자 g_free)
 * @error:    GError 반환
 *
 * mdevctl start 명령으로 vGPU 인스턴스를 생성한다.
 * 생성된 UUID는 virsh attach-device로 VM에 연결할 때 사용된다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_gpu_vgpu_create(const gchar *pci_addr, const gchar *type,
                              gchar **uuid_out, GError **error)
{
    if (!pci_addr || !type) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1, "pci_addr and type required");
        return FALSE;
    }
    gchar *cmd = g_strdup_printf("mdevctl start --parent 0000:%s --type %s 2>&1", pci_addr, type);
    gchar *out = NULL;
    gboolean ok = _run(cmd, &out, error);
    if (ok && out && uuid_out) *uuid_out = g_strdup(g_strstrip(out));
    g_free(out); g_free(cmd);
    return ok;
}

/**
 * pcv_gpu_vgpu_delete:
 * @uuid:  삭제할 vGPU 인스턴스의 UUID
 * @error: GError 반환
 *
 * mdevctl stop 명령으로 vGPU 인스턴스를 중지/삭제한다.
 * "; true" 접미사로 이미 존재하지 않는 UUID에 대해서도 에러를 무시한다 (멱등성).
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_gpu_vgpu_delete(const gchar *uuid, GError **error)
{
    if (!uuid) { g_set_error(error, g_quark_from_static_string("gpu"), 1, "uuid required"); return FALSE; }
    gchar *cmd = g_strdup_printf("mdevctl stop --uuid %s 2>&1; true", uuid);
    gboolean ok = _run(cmd, NULL, error);
    g_free(cmd);
    return ok;
}

/**
 * pcv_gpu_vgpu_list:
 *
 * mdevctl list로 현재 생성된 모든 vGPU 인스턴스 목록을 반환한다.
 * 각 줄을 하나의 JsonObject로 포장하여 JsonArray에 담는다.
 *
 * Returns: (transfer full): JsonArray* (각 원소: {"entry":"..."})
 */
JsonArray *pcv_gpu_vgpu_list(void)
{
    JsonArray *arr = json_array_new();
    gchar *out = NULL;
    if (_run_shell("mdevctl list 2>/dev/null", &out, NULL) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gint i = 0; lines[i] && lines[i][0]; i++) {
            JsonObject *v = json_object_new();
            json_object_set_string_member(v, "entry", lines[i]);
            json_array_add_object_element(arr, v);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

/**
 * pcv_gpu_attach:
 * @vm_name:  GPU를 연결할 VM 이름
 * @pci_addr: PCI 주소 (예: "00:02.0" 또는 "0000:00:02.0")
 * @error:    GError 반환
 *
 * PCI 주소를 파싱하여 libvirt <hostdev> XML을 생성하고,
 * virsh attach-device로 실행 중인 VM에 GPU를 핫플러그한다.
 *
 * PCI 주소 파싱:
 *   "00:02.0" → domain=0, bus=0, slot=2, function=0
 *   sscanf으로 16진수 파싱 후 XML의 domain/bus/slot/function 필드에 매핑
 *
 * 임시 파일(/tmp/pcv-gpu-<vm>.xml)을 생성하여 virsh에 전달하고 완료 후 삭제합니다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_gpu_attach(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!vm_name || !pci_addr) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1, "vm_name and pci_addr required");
        return FALSE;
    }
    guint d=0,b=0,s=0,f=0;
    sscanf(pci_addr, "%x:%x:%x.%x", &d, &b, &s, &f);
    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source><address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/></source>\n"
        "</hostdev>", d, b, s, f);
    gchar *path = g_strdup_printf("/tmp/pcv-gpu-%s.xml", vm_name);
    g_file_set_contents(path, xml, -1, NULL);
    gchar *cmd = g_strdup_printf("virsh attach-device %s %s --live 2>&1", vm_name, path);
    gboolean ok = _run(cmd, NULL, error);
    g_unlink(path);
    g_free(cmd); g_free(path); g_free(xml);
    return ok;
}

/**
 * pcv_gpu_detach:
 * @vm_name:  GPU를 분리할 VM 이름
 * @pci_addr: PCI 주소
 * @error:    GError 반환
 *
 * virsh detach-device로 VM에서 GPU를 핫언플러그한다.
 * attach와 동일한 XML 생성 로직을 사용하며, 실패 시에도 TRUE를 반환한다
 * ("; true" 접미사로 멱등성 확보 — 이미 분리된 상태에서도 안전).
 *
 * Returns: 성공 시 TRUE
 */
gboolean pcv_gpu_detach(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!vm_name || !pci_addr) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1, "vm_name and pci_addr required");
        return FALSE;
    }
    guint d=0,b=0,s=0,f=0;
    sscanf(pci_addr, "%x:%x:%x.%x", &d, &b, &s, &f);
    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source><address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/></source>\n"
        "</hostdev>", d, b, s, f);
    gchar *path = g_strdup_printf("/tmp/pcv-gpu-detach-%s.xml", vm_name);
    g_file_set_contents(path, xml, -1, NULL);
    gchar *cmd = g_strdup_printf("virsh detach-device %s %s --live 2>/dev/null; true", vm_name, path);
    gboolean ok = _run(cmd, NULL, error);
    g_unlink(path);
    g_free(cmd); g_free(path); g_free(xml);
    return ok;
}
