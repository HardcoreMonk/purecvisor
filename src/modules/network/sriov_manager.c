/**
 * @file sriov_manager.c
 * @brief SR-IOV 매니저 -- VF 생성/관리 + PCI Passthrough
 *
 * ====================================================================
 * [아키텍처 위치]
 *   handler_accel.c --> sriov_manager (이 파일)
 *
 *   handler_accel.c 에서 sriov.* RPC 7개를 처리할 때 이 모듈의
 *   함수를 호출한다. SR-IOV(Single Root I/O Virtualization)를 통해
 *   물리 NIC의 VF(Virtual Function)를 생성하고 VM에 직접 할당한다.
 *
 * [담당 RPC 메서드] (7개, handler_accel.c 경유)
 *   sriov.status  - SR-IOV 전체 상태 (지원 NIC 목록, 활성 VF 수)
 *   sriov.enable  - PF에 VF 생성 (sysfs sriov_numvfs 쓰기)
 *   sriov.disable - PF의 VF 전부 제거 (sriov_numvfs=0)
 *   sriov.list    - PF의 VF 목록 + PCI 주소 + 상태
 *   sriov.set     - VF 속성 설정 (MAC, VLAN, spoofchk)
 *   sriov.attach  - VM에 VF PCI passthrough (vfio-pci 바인딩 + virsh attach)
 *   sriov.detach  - VM에서 VF 분리 (virsh detach-device)
 *
 * [핵심 동작 흐름 -- sriov.attach]
 *   1. pcv_sriov_vf_pci_addr(pf, vf_index) 로 VF의 PCI 주소 조회
 *      예: "0000:03:10.0"
 *   2. 기존 드라이버 언바인드 (echo PCI주소 > /sys/.../unbind)
 *   3. vfio-pci 드라이버 바인딩 (echo PCI주소 > /sys/.../bind)
 *   4. libvirt hostdev XML 생성:
 *        <hostdev mode='subsystem' type='pci' managed='yes'>
 *          <source><address domain=... bus=... slot=... function=.../></source>
 *        </hostdev>
 *   5. virsh attach-device --live 로 실행 중 VM에 핫플러그
 *
 * [sysfs 인터페이스]
 *   /sys/class/net/<pf>/device/sriov_totalvfs  - 최대 VF 수 (읽기전용)
 *   /sys/class/net/<pf>/device/sriov_numvfs    - 현재 VF 수 (읽기/쓰기)
 *   /sys/class/net/<pf>/device/virtfnN         - VF N의 PCI 디바이스 심볼릭 링크
 *
 * [Graceful Degradation]
 *   _pf_supports_sriov() 로 sriov_totalvfs 파일 존재+값>0 확인.
 *   SR-IOV 미지원 NIC:
 *     - enable: GError 반환
 *     - list: 빈 JsonArray 반환
 *     - status: available=false 포함 JSON 반환
 *
 * [의존 모듈]
 *   pcv_spawn.h    - ip link set, virsh attach/detach 명령 실행
 *   pcv_log.h      - SRIOV_LOG_DOM 도메인 로깅
 *   pcv_validate.h - VM 이름, PCI 주소 검증
 *
 * [주의사항]
 *   - sriov_numvfs 쓰기는 root 권한 + IOMMU 활성화 필요.
 *     BIOS에서 VT-d/IOMMU 활성화 + 커널 파라미터 intel_iommu=on.
 *   - VF 수는 SRIOV_MAX_VFS(64)로 클램프된다.
 *   - vfio-pci 바인딩 전 해당 VF를 사용 중인 다른 VM이 없는지
 *     확인해야 한다 (현재 커널 레벨에서 거부되지만 에러 메시지가
 *     불친절할 수 있음).
 *   - detach 시 PCI 주소 형식: "0000:03:10.0" (domain:bus:slot.function).
 * ====================================================================
 */
#include "sriov_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "../../include/purecvisor/pcv_validate.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <glib/gstdio.h>

#define SRIOV_LOG_DOM    "sriov_manager"
#define SRIOV_SYSFS_NET  "/sys/class/net"
#define SRIOV_MAX_VFS    64

static struct {
    gboolean initialized;
    GMutex   mu;
} G = {0};

/* ── helpers ──────────────────────────────────────────────────── */

/**
 * _run_shell — 셸 기능(리다이렉션, 파이프, echo>) 필요 시 전용 실행 헬퍼
 *
 * [보안] 이 헬퍼는 오직 하드코딩된 안전한 명령 또는 커널/sysfs 파생 값
 * (인터페이스 이름, PCI BDF 등 화이트리스트 문자만 가능한 값)에만 사용한다.
 * 사용자 RPC 입력(vm_name, mac 등)은 절대 이 경로로 흘려보내지 않는다.
 * 사용자 입력이 필요한 외부 명령은 pcv_spawn_sync(argv[]) 로 셸 없이 실행한다.
 */
static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok)
        PCV_LOG_WARN(SRIOV_LOG_DOM, "cmd(shell) failed: %s  stderr=%s", cmd,
                     std_err ? std_err : "(null)");
    g_free(std_err);
    return ok;
}

/**
 * _write_sysfs — sysfs 속성 파일에 값을 직접 쓴다 ("echo <val> > <path>" 대체).
 *
 * [왜 g_file_set_contents가 아닌가]
 *   g_file_set_contents()는 원자성 보장을 위해 "<path>.XXXXXX" 임시 파일을
 *   같은 디렉토리에 만든 뒤 rename() 한다. sysfs는 임의 파일 생성/rename을
 *   허용하지 않으므로 이 방식은 항상 실패한다. sysfs 속성은 fopen("w")로
 *   기존 파일을 열어 직접 write() 해야 한다(셸의 '>' 리다이렉션과 동일).
 * [보안] path는 화이트리스트 검증된 인터페이스 이름으로만 구성된다.
 */
/* 테스트 격리: PCV_SRIOV_SYSFS_ROOT 로 sysfs 베이스 override (vm_state.c 선례). */
static const gchar *
_sriov_sysfs_net(void)
{
    const gchar *env = g_getenv("PCV_SRIOV_SYSFS_ROOT");
    return (env && *env) ? env : SRIOV_SYSFS_NET;
}

static gboolean
_write_sysfs(const gchar *path, const gchar *val, GError **error)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        g_set_error(error, g_quark_from_static_string("sriov"), 10,
                    "sysfs open failed: %s (%s)", path, g_strerror(errno));
        return FALSE;
    }
    size_t len = strlen(val);
    gboolean ok = (fwrite(val, 1, len, f) == len);
    if (fclose(f) != 0) ok = FALSE;
    if (!ok && error && !*error)
        g_set_error(error, g_quark_from_static_string("sriov"), 11,
                    "sysfs write failed: %s", path);
    return ok;
}

/**
 * _pf_supports_sriov — PF(Physical Function)의 SR-IOV 지원 여부 확인
 * @pf: 물리 NIC 이름 (예: "eno1")
 *
 * /sys/class/net/<pf>/device/sriov_totalvfs 파일을 읽어
 * 최대 VF 수가 0보다 크면 SR-IOV 지원으로 판단.
 *
 * @return SR-IOV 지원 시 TRUE
 */
static gboolean
_pf_supports_sriov(const gchar *pf)
{
    gchar *path = g_strdup_printf("%s/%s/device/sriov_totalvfs", SRIOV_SYSFS_NET, pf);
    gchar *content = NULL;
    gboolean ok = g_file_get_contents(path, &content, NULL, NULL);
    g_free(path);
    if (ok && content) {
        gint64 total = g_ascii_strtoll(g_strstrip(content), NULL, 10);
        g_free(content);
        return total > 0;
    }
    g_free(content);
    return FALSE;
}

/**
 * _pf_current_vfs — PF에 현재 활성화된 VF 수 조회
 * @pf: 물리 NIC 이름
 *
 * /sys/class/net/<pf>/device/sriov_numvfs 파일에서 현재 VF 수를 읽어 반환.
 *
 * @return 현재 VF 수 (읽기 실패 시 0)
 */
static gint
_pf_current_vfs(const gchar *pf)
{
    gchar *path = g_strdup_printf("%s/%s/device/sriov_numvfs", SRIOV_SYSFS_NET, pf);
    gchar *content = NULL;
    gint num = 0;
    if (g_file_get_contents(path, &content, NULL, NULL) && content)
        num = (gint)g_ascii_strtoll(g_strstrip(content), NULL, 10);
    g_free(content);
    g_free(path);
    return num;
}

/**
 * _pf_max_vfs — PF가 지원하는 최대 VF 수 조회
 * @pf: 물리 NIC 이름
 *
 * /sys/class/net/<pf>/device/sriov_totalvfs 파일에서 읽기.
 * 하드웨어/펌웨어에 의해 결정되는 읽기전용 값.
 *
 * @return 최대 VF 수 (읽기 실패 시 0)
 */
static gint
_pf_max_vfs(const gchar *pf)
{
    gchar *path = g_strdup_printf("%s/%s/device/sriov_totalvfs", SRIOV_SYSFS_NET, pf);
    gchar *content = NULL;
    gint num = 0;
    if (g_file_get_contents(path, &content, NULL, NULL) && content)
        num = (gint)g_ascii_strtoll(g_strstrip(content), NULL, 10);
    g_free(content);
    g_free(path);
    return num;
}

/* ── lifecycle ────────────────────────────────────────────────── */

/** pcv_sriov_init — SR-IOV 매니저 초기화. 뮤텍스 생성. 데몬 시작 시 main.c에서 호출. */
void
pcv_sriov_init(void)
{
    g_mutex_init(&G.mu);
    G.initialized = TRUE;
    PCV_LOG_INFO(SRIOV_LOG_DOM, "SR-IOV manager initialized");
}

/** pcv_sriov_shutdown — SR-IOV 매니저 종료. 뮤텍스 해제. */
void
pcv_sriov_shutdown(void)
{
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/* ── status ───────────────────────────────────────────────────── */

/**
 * pcv_sriov_status — SR-IOV 전체 상태 조회
 *
 * /sys/class/net/ 디렉토리를 순회하여 SR-IOV 지원 PF(Physical Function)를 검색.
 * 각 PF에 대해 이름, PCI 주소, 최대/현재 VF 수, 드라이버, IOMMU 상태를 수집.
 *
 * 반환 JsonObject:
 *   {available: true/false, physical_functions: [{name, pci_addr, max_vfs, current_vfs, driver, iommu_enabled}, ...]}
 *
 * @return (transfer full): 상태 JsonObject (호출자 unref)
 */
JsonObject *
pcv_sriov_status(void)
{
    JsonObject *obj = json_object_new();
    JsonArray *pfs = json_array_new();

    /* /sys/class/net 에서 SR-IOV 지원 PF 검색 */
    GDir *dir = g_dir_open(SRIOV_SYSFS_NET, 0, NULL);
    gboolean any_sriov = FALSE;
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (!_pf_supports_sriov(name))
                continue;
            any_sriov = TRUE;

            JsonObject *pf = json_object_new();
            json_object_set_string_member(pf, "name", name);
            json_object_set_int_member(pf, "max_vfs", _pf_max_vfs(name));
            json_object_set_int_member(pf, "current_vfs", _pf_current_vfs(name));

            /* PCI 주소 */
            gchar *pci_link = g_strdup_printf("%s/%s/device", SRIOV_SYSFS_NET, name);
            gchar *pci_real = g_file_read_link(pci_link, NULL);
            if (pci_real) {
                gchar *pci_base = g_path_get_basename(pci_real);
                json_object_set_string_member(pf, "pci_addr", pci_base);
                g_free(pci_base);
                g_free(pci_real);
            }
            g_free(pci_link);

            /* 드라이버 */
            gchar *drv_link = g_strdup_printf("%s/%s/device/driver", SRIOV_SYSFS_NET, name);
            gchar *drv_real = g_file_read_link(drv_link, NULL);
            if (drv_real) {
                gchar *drv_name = g_path_get_basename(drv_real);
                json_object_set_string_member(pf, "driver", drv_name);
                g_free(drv_name);
                g_free(drv_real);
            }
            g_free(drv_link);

            /* IOMMU 그룹 확인 */
            gchar *iommu_path = g_strdup_printf("%s/%s/device/iommu_group",
                                                 SRIOV_SYSFS_NET, name);
            gboolean iommu_ok = g_file_test(iommu_path, G_FILE_TEST_EXISTS);
            json_object_set_boolean_member(pf, "iommu_enabled", iommu_ok);
            g_free(iommu_path);

            json_array_add_object_element(pfs, pf);
        }
        g_dir_close(dir);
    }

    json_object_set_boolean_member(obj, "available", any_sriov);
    json_object_set_array_member(obj, "physical_functions", pfs);
    return obj;
}

/* ── VF management ────────────────────────────────────────────── */

/**
 * pcv_sriov_enable — PF에 VF(Virtual Function) 생성
 * @pf: 물리 NIC 이름 (예: "eno1")
 * @num_vfs: 생성할 VF 수 (1 ~ max_vfs 범위)
 * @error: 에러 반환 포인터
 *
 * sysfs sriov_numvfs 에 값을 써서 VF를 생성한다.
 * sysfs 요구사항: VF 수 변경 전 반드시 0으로 리셋해야 함.
 *
 * [전제 조건]
 *   - root 권한 필요
 *   - BIOS에서 VT-d/IOMMU 활성화 + 커널 파라미터 intel_iommu=on
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_sriov_enable(const gchar *pf, gint num_vfs, GError **error)
{
    if (!pf || strlen(pf) == 0) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "PF name required");
        return FALSE;
    }

    /* 입력 검증 (화이트리스트) — sysfs 경로/셸 인젝션 차단 */
    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }

    if (!_pf_supports_sriov(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "PF '%s' does not support SR-IOV", pf);
        return FALSE;
    }

    gint max_vfs = _pf_max_vfs(pf);
    if (num_vfs < 1 || num_vfs > max_vfs) {
        g_set_error(error, g_quark_from_static_string("sriov"), 3,
                    "num_vfs must be 1~%d for PF '%s'", max_vfs, pf);
        return FALSE;
    }

    gchar *path = g_strdup_printf("%s/%s/device/sriov_numvfs", SRIOV_SYSFS_NET, pf);
    gchar *val = g_strdup_printf("%d", num_vfs);

    g_mutex_lock(&G.mu);
    /* 먼저 기존 VF 해제 (sysfs 요구사항: 변경 전 0으로 설정) */
    gint current = _pf_current_vfs(pf);
    if (current > 0)
        _write_sysfs(path, "0", NULL);

    gboolean ok = _write_sysfs(path, val, error);
    g_mutex_unlock(&G.mu);

    g_free(path);
    g_free(val);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Enabled %d VFs on PF '%s'", num_vfs, pf);
    return ok;
}

/**
 * pcv_sriov_disable — PF의 모든 VF 제거
 * @pf: 물리 NIC 이름
 * @error: 에러 반환 포인터
 *
 * sriov_numvfs=0 으로 설정하여 모든 VF를 제거. 결과는 두 갈래(NET-3):
 *   - PF/sysfs 자체가 없어 열기부터 실패하는 경우(제거할 VF가 없는 상태) → 멱등
 *     성공 처리, TRUE 반환(에러 없음).
 *   - sysfs 쓰기 자체가 거부되는 경우(예: VF 사용 중 EBUSY) → 거짓 성공을 반환하지
 *     않고 FALSE + GError 전파. 호출자는 VF가 실제로는 제거되지 않았음을 알 수 있다.
 *
 * @return 성공(또는 멱등 무동작) 시 TRUE, sysfs 쓰기 거부 시 FALSE(+@error)
 */
/* PCV_SAFETY_CONTROL: sriov-disable — sriov_numvfs=0으로 PF의 모든 VF 실제 제거 (NET-3) */
gboolean
pcv_sriov_disable(const gchar *pf, GError **error)
{
    if (!pf || strlen(pf) == 0) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "PF name required");
        return FALSE;
    }

    /* 입력 검증 (화이트리스트) — sysfs 경로/셸 인젝션 차단 */
    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }

    /* 멱등: VF 없어도 성공. 셸(echo>) 대신 sysfs 직접 쓰기. */
    gchar *path = g_strdup_printf("%s/%s/device/sriov_numvfs", _sriov_sysfs_net(), pf);

    g_mutex_lock(&G.mu);
    GError *werr = NULL;
    gboolean ok = _write_sysfs(path, "0", &werr);
    g_mutex_unlock(&G.mu);
    g_free(path);

    if (!ok) {
        if (g_error_matches(werr, g_quark_from_static_string("sriov"), 10)) {
            /* open 실패(PF/sysfs 부재) — 제거할 VF 없음 → 멱등 성공 */
            PCV_LOG_DEBUG(SRIOV_LOG_DOM, "sriov_numvfs absent, idempotent no-op: %s", werr->message);
            g_clear_error(&werr);
        } else {
            /* write 거부(EBUSY 등) — VF 잔존인데 성공 오보 금지(NET-3) */
            PCV_LOG_WARN(SRIOV_LOG_DOM, "sriov disable failed on PF '%s': %s", pf, werr ? werr->message : "(write)");
            g_propagate_error(error, werr);
            return FALSE;
        }
    }

    PCV_LOG_INFO(SRIOV_LOG_DOM, "Disabled VFs on PF '%s'", pf);
    return TRUE;
}

/**
 * pcv_sriov_list — PF의 VF(Virtual Function) 목록 조회
 * @pf: (nullable): 특정 PF 이름. NULL이면 모든 SR-IOV PF의 VF를 순회.
 *
 * 각 VF에 대해 PF 이름, VF 인덱스, PCI 주소, 드라이버, MAC 주소를 수집.
 * VF PCI 주소: /sys/class/net/<pf>/device/virtfnN 심볼릭 링크에서 추출.
 * MAC 주소: ip link show <pf> 출력에서 "vf N MAC XX:XX:XX" 패턴 파싱.
 *
 * @return (transfer full): [{pf, vf_index, pci_addr, driver, mac}, ...] JsonObject 배열
 */
JsonArray *
pcv_sriov_list(const gchar *pf)
{
    JsonArray *arr = json_array_new();

    /* PF 미지정이면 모든 SR-IOV PF 순회 */
    GDir *net_dir = g_dir_open(SRIOV_SYSFS_NET, 0, NULL);
    if (!net_dir)
        return arr;

    const gchar *iface;
    while ((iface = g_dir_read_name(net_dir)) != NULL) {
        if (pf && g_strcmp0(iface, pf) != 0)
            continue;
        if (!_pf_supports_sriov(iface))
            continue;

        gint num = _pf_current_vfs(iface);
        for (gint i = 0; i < num; i++) {
            JsonObject *vf = json_object_new();
            json_object_set_string_member(vf, "pf", iface);
            json_object_set_int_member(vf, "vf_index", i);

            /* VF PCI 주소 (symlink: virtfnN → ../0000:xx:xx.x) */
            gchar *vf_link = g_strdup_printf("%s/%s/device/virtfn%d",
                                              SRIOV_SYSFS_NET, iface, i);
            gchar *vf_real = g_file_read_link(vf_link, NULL);
            if (vf_real) {
                gchar *pci = g_path_get_basename(vf_real);
                json_object_set_string_member(vf, "pci_addr", pci);

                /* 드라이버 확인 */
                gchar *drv_link = g_strdup_printf("/sys/bus/pci/devices/%s/driver", pci);
                gchar *drv_real = g_file_read_link(drv_link, NULL);
                if (drv_real) {
                    gchar *drv = g_path_get_basename(drv_real);
                    json_object_set_string_member(vf, "driver", drv);
                    g_free(drv);
                    g_free(drv_real);
                }
                g_free(drv_link);
                g_free(pci);
                g_free(vf_real);
            }
            g_free(vf_link);

            /* VF MAC (ip -j link show 파싱 대신 sysfs) */
            gchar *mac_cmd = g_strdup_printf(
                "ip link show %s 2>/dev/null | grep 'vf %d' | "
                "sed -n 's/.*MAC \\([^ ]*\\).*/\\1/p'", iface, i);
            gchar *mac_out = NULL;
            if (_run_shell(mac_cmd, &mac_out, NULL) && mac_out) {
                g_strstrip(mac_out);
                if (strlen(mac_out) > 0)
                    json_object_set_string_member(vf, "mac", mac_out);
            }
            g_free(mac_out);
            g_free(mac_cmd);

            json_array_add_object_element(arr, vf);
        }
    }
    g_dir_close(net_dir);
    return arr;
}

/* ── VF configuration ─────────────────────────────────────────── */

/**
 * pcv_sriov_set — VF 속성 설정 (MAC, VLAN, spoofchk)
 * @pf: 물리 NIC 이름
 * @vf_index: 설정 대상 VF 인덱스 (0부터 시작)
 * @mac: (nullable): MAC 주소 설정 (NULL이면 건너뜀)
 * @vlan: VLAN ID 설정 (-1이면 건너뜀)
 * @spoofchk: 스푸핑 체크 설정 (0=off, 1=on, -1이면 건너뜀)
 * @error: 에러 반환 포인터
 *
 * ip link set <pf> vf <index> [mac X] [vlan N] [spoofchk on/off] 실행.
 * 각 속성은 독립적으로 설정되며, 앞 속성 실패 시 이후 속성은 건너뛴다.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_sriov_set(const gchar *pf, gint vf_index,
              const gchar *mac, gint vlan,
              gint spoofchk, GError **error)
{
    if (!pf || strlen(pf) == 0) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "PF name required");
        return FALSE;
    }

    /* 입력 검증 (화이트리스트) — 형식 검증을 상태 검사보다 먼저 수행 */
    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }
    if (mac && strlen(mac) > 0 && !pcv_validate_mac(mac)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 5,
                    "Invalid MAC address: %s", mac);
        return FALSE;
    }

    gint current = _pf_current_vfs(pf);
    if (vf_index < 0 || vf_index >= current) {
        g_set_error(error, g_quark_from_static_string("sriov"), 3,
                    "VF index %d out of range (0~%d) for PF '%s'",
                    vf_index, current - 1, pf);
        return FALSE;
    }

    gchar *vf_str = g_strdup_printf("%d", vf_index);

    g_mutex_lock(&G.mu);
    gboolean ok = TRUE;

    /* 모든 외부 명령은 argv 배열로 셸 없이 실행 (g_shell_parse_argv 미경유) */
    if (mac && strlen(mac) > 0) {
        const gchar *argv[] = {"ip", "link", "set", pf, "vf", vf_str,
                               "mac", mac, NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "ip link set mac failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
    }

    if (ok && vlan >= 0) {
        gchar *vlan_str = g_strdup_printf("%d", vlan);
        const gchar *argv[] = {"ip", "link", "set", pf, "vf", vf_str,
                               "vlan", vlan_str, NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "ip link set vlan failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
        g_free(vlan_str);
    }

    if (ok && spoofchk >= 0) {
        const gchar *argv[] = {"ip", "link", "set", pf, "vf", vf_str,
                               "spoofchk", spoofchk ? "on" : "off", NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "ip link set spoofchk failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
    }

    g_mutex_unlock(&G.mu);
    g_free(vf_str);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Set VF %d on PF '%s'", vf_index, pf);
    return ok;
}

/* ── VM attachment ────────────────────────────────────────────── */

/**
 * pcv_sriov_vf_pci_addr — VF의 PCI 주소 조회
 * @pf: 물리 NIC 이름
 * @vf_index: VF 인덱스
 *
 * /sys/class/net/<pf>/device/virtfn<index> 심볼릭 링크를 읽어
 * VF의 PCI 주소(예: "0000:03:10.0")를 반환.
 *
 * @return (transfer full): PCI 주소 문자열 (호출자 g_free), 실패 시 NULL
 */
gchar *
pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index)
{
    gchar *vf_link = g_strdup_printf("%s/%s/device/virtfn%d",
                                      SRIOV_SYSFS_NET, pf, vf_index);
    gchar *vf_real = g_file_read_link(vf_link, NULL);
    g_free(vf_link);
    if (!vf_real)
        return NULL;
    gchar *pci = g_path_get_basename(vf_real);
    g_free(vf_real);
    return pci;
}

/**
 * pcv_sriov_attach_vm — VM에 VF PCI passthrough 연결
 * @vm_name: 대상 VM 이름
 * @pf: 물리 NIC 이름
 * @vf_index: 연결할 VF 인덱스
 * @error: 에러 반환 포인터
 *
 * 실행 순서:
 *   1) VF PCI 주소 조회 (pcv_sriov_vf_pci_addr)
 *   2) 기존 커널 드라이버에서 언바인드 (echo > unbind)
 *   3) vfio-pci 드라이버에 바인딩 (driver_override + echo > bind)
 *   4) libvirt hostdev XML 생성 (PCI domain:bus:slot.function 형식)
 *   5) virsh attach-device --live 로 실행 중 VM에 핫플러그
 *
 * [주의] vfio-pci 바인딩은 IOMMU 활성화가 전제 조건.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_sriov_attach_vm(const gchar *vm_name, const gchar *pf,
                     gint vf_index, GError **error)
{
    if (!vm_name || !pf) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "vm_name and pf required");
        return FALSE;
    }

    /* 입력 검증 (화이트리스트) — virsh argv + sysfs 경로 인젝션 차단 */
    if (!pcv_validate_vm_name(vm_name)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 5,
                    "Invalid VM name: %s", vm_name);
        return FALSE;
    }
    if (!pcv_validate_iface_name(pf)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 6,
                    "Invalid PF name: %s", pf);
        return FALSE;
    }

    gchar *pci = pcv_sriov_vf_pci_addr(pf, vf_index);
    if (!pci) {
        g_set_error(error, g_quark_from_static_string("sriov"), 4,
                    "Cannot resolve PCI address for VF %d on PF '%s'",
                    vf_index, pf);
        return FALSE;
    }

    /* VFIO-PCI 바인딩 */
    g_mutex_lock(&G.mu);

    /* unbind from kernel driver */
    gchar *drv_link = g_strdup_printf("/sys/bus/pci/devices/%s/driver", pci);
    gchar *drv_real = g_file_read_link(drv_link, NULL);
    if (drv_real) {
        gchar *drv_name = g_path_get_basename(drv_real);
        if (g_strcmp0(drv_name, "vfio-pci") != 0) {
            gchar *cmd = g_strdup_printf(
                "echo %s > /sys/bus/pci/drivers/%s/unbind 2>/dev/null; "
                "echo vfio-pci > /sys/bus/pci/devices/%s/driver_override && "
                "echo %s > /sys/bus/pci/drivers/vfio-pci/bind",
                pci, drv_name, pci, pci);
            if (!_run_shell(cmd, NULL, error)) {
                g_free(cmd);
                g_free(drv_name);
                g_free(drv_real);
                g_free(drv_link);
                g_free(pci);
                g_mutex_unlock(&G.mu);
                return FALSE;
            }
            g_free(cmd);
        }
        g_free(drv_name);
        g_free(drv_real);
    }
    g_free(drv_link);

    /* virsh attach-device로 PCI passthrough 연결 */
    /* PCI 주소 파싱: 0000:01:10.0 → domain=0x0000 bus=0x01 slot=0x10 function=0x0 */
    guint domain = 0, bus = 0, slot = 0, func = 0;
    sscanf(pci, "%x:%x:%x.%x", &domain, &bus, &slot, &func);

    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source>\n"
        "    <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
        "  </source>\n"
        "</hostdev>", domain, bus, slot, func);

    /* 임시 XML 파일 작성 (예측 가능한 /tmp 경로 대신 mkstemp 기반) */
    gchar *xml_path = NULL;
    gint fd = g_file_open_tmp("pcv-sriov-attach-XXXXXX.xml", &xml_path, error);
    gboolean ok = (fd >= 0);
    if (ok) {
        g_close(fd, NULL);
        ok = g_file_set_contents(xml_path, xml, -1, error);
    }
    g_free(xml);

    if (ok) {
        /* 셸 미경유 argv — vm_name/xml_path 재파싱 없음 */
        const gchar *argv[] = {"virsh", "attach-device", vm_name, xml_path,
                               "--live", NULL};
        gchar *serr = NULL;
        ok = pcv_spawn_sync(argv, NULL, &serr, error);
        if (!ok)
            PCV_LOG_WARN(SRIOV_LOG_DOM, "virsh attach-device failed: %s",
                         serr ? serr : "(null)");
        g_free(serr);
    }

    if (xml_path) {
        g_unlink(xml_path);
        g_free(xml_path);
    }
    g_mutex_unlock(&G.mu);
    g_free(pci);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Attached VF %d (PF %s) to VM '%s'",
                     vf_index, pf, vm_name);
    return ok;
}

/**
 * pcv_sriov_detach_vm — VM에서 VF PCI 디바이스 분리 (멱등)
 * @vm_name: 대상 VM 이름
 * @pci_addr: 분리할 PCI 주소 (예: "0000:03:10.0")
 * @error: 에러 반환 포인터
 *
 * virsh detach-device --live 로 PCI passthrough 디바이스 분리.
 * 이미 분리된 상태이면 에러를 무시 ("; true"로 멱등 처리).
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_sriov_detach_vm(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!vm_name || !pci_addr) {
        g_set_error(error, g_quark_from_static_string("sriov"), 1,
                    "vm_name and pci_addr required");
        return FALSE;
    }

    /* 입력 검증 (화이트리스트) — virsh argv 인젝션 차단 */
    if (!pcv_validate_vm_name(vm_name)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 5,
                    "Invalid VM name: %s", vm_name);
        return FALSE;
    }

    if (!pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("sriov"), 2,
                    "Invalid PCI address: %s", pci_addr);
        return FALSE;
    }

    guint domain = 0, bus = 0, slot = 0, func = 0;
    sscanf(pci_addr, "%x:%x:%x.%x", &domain, &bus, &slot, &func);

    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='yes'>\n"
        "  <source>\n"
        "    <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
        "  </source>\n"
        "</hostdev>", domain, bus, slot, func);

    gchar *xml_path = NULL;
    gint fd = g_file_open_tmp("pcv-sriov-detach-XXXXXX.xml", &xml_path, error);
    if (fd < 0) {
        g_free(xml);
        return FALSE;
    }
    g_close(fd, NULL);
    gboolean ok = g_file_set_contents(xml_path, xml, -1, error);
    g_free(xml);

    if (ok) {
        /* 멱등: 이미 분리된 상태이면 virsh가 에러를 반환하지만 무시.
         * 셸 미경유 argv — vm_name/xml_path 재파싱 없음. */
        const gchar *argv[] = {"virsh", "detach-device", vm_name, xml_path,
                               "--live", NULL};
        gchar *serr = NULL;
        if (!pcv_spawn_sync(argv, NULL, &serr, NULL))
            PCV_LOG_DEBUG(SRIOV_LOG_DOM,
                          "virsh detach-device (ignored, idempotent): %s",
                          serr ? serr : "(null)");
        g_free(serr);
    }

    g_unlink(xml_path);
    g_free(xml_path);

    if (ok)
        PCV_LOG_INFO(SRIOV_LOG_DOM, "Detached PCI %s from VM '%s'", pci_addr, vm_name);
    return ok;
}
