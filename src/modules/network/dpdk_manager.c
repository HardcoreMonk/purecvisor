/**
 * @file dpdk_manager.c
 * @brief OVS-DPDK 매니저 -- 커널 바이패스 데이터플레인 관리
 *
 * ====================================================================
 * [아키텍처 위치]
 *   handler_accel.c --> dpdk_manager (이 파일)
 *
 *   handler_accel.c 에서 dpdk.* RPC 7개를 처리할 때 이 모듈의
 *   함수를 호출한다. DPDK(Data Plane Development Kit)를 통해
 *   커널 네트워크 스택을 바이패스하여 고성능 패킷 처리를 제공한다.
 *
 * [담당 RPC 메서드] (7개, handler_accel.c 경유)
 *   dpdk.status        - DPDK 초기화 상태 + OVS 버전 정보
 *   dpdk.bind          - NIC을 DPDK 드라이버에 바인딩 (dpdk-devbind.py)
 *   dpdk.unbind        - NIC을 커널 드라이버로 복원
 *   dpdk.list          - DPDK 바인딩된 디바이스 목록
 *   dpdk.bridge.create - DPDK 가속 OVS 브릿지 생성
 *   dpdk.bridge.delete - DPDK OVS 브릿지 삭제
 *   dpdk.hugepage.info - hugepage 할당 현황 (/sys/kernel/mm/hugepages)
 *
 * [핵심 동작 흐름 -- dpdk.bridge.create]
 *   1. ovs-vsctl add-br <name> -- --set bridge <name> datapath_type=netdev
 *   2. ovs-vsctl add-port <name> <dpdk_port>
 *        -- --set Interface <dpdk_port> type=dpdk
 *           options:dpdk-devargs=<pci_addr>
 *   VM 연결 시 vhost-user 소켓 사용:
 *     /var/run/purecvisor/vhost-<vm_name>.sock
 *
 * [DPDK 전제 조건]
 *   - OVS에 dpdk-init=true 설정 필요:
 *     ovs-vsctl set Open_vSwitch . other_config:dpdk-init=true
 *   - hugepage 할당 필요 (최소 2MB x 1024 = 2GB 권장):
 *     echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
 *   - dpdk-devbind.py (dpdk-tools 패키지) 설치 필요
 *
 * [Graceful Degradation]
 *   pcv_dpdk_init() 에서 _check_dpdk_init() 으로 OVS dpdk-init 확인.
 *   dpdk-init=true 아닌 경우:
 *     - G.available=FALSE 설정
 *     - status: available=false 포함 JSON 반환
 *     - bind/bridge.create 등: GError 반환
 *     - list: 빈 JsonArray 반환
 *
 * [vhost-user 소켓]
 *   pcv_dpdk_vhost_socket_path(vm_name) 으로 VM별 소켓 경로 생성.
 *   VM XML 에서 <interface type='vhostuser'> 로 참조.
 *   경로: /var/run/purecvisor/vhost-<vm_name>.sock
 *
 * [의존 모듈]
 *   pcv_spawn.h    - ovs-vsctl, dpdk-devbind.py 명령 실행
 *   pcv_log.h      - DPDK_LOG_DOM 도메인 로깅
 *   pcv_validate.h - PCI 주소 형식 검증
 *
 * [주의사항]
 *   - dpdk-devbind.py 는 root 권한 필요.
 *   - NIC을 DPDK에 바인딩하면 해당 NIC의 커널 네트워크 인터페이스가
 *     사라진다. 관리 NIC(eno1)을 바인딩하면 SSH 접속 불가.
 *   - hugepage 부족 시 OVS DPDK 데이터패스가 시작되지 않는다.
 *   - _check_dpdk_init() 은 ovs-vsctl get 명령의 출력에서
 *     따옴표 포함 문자열("true")을 처리한다.
 * ====================================================================
 */
#include "dpdk_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "../../include/purecvisor/pcv_validate.h"
#include <string.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <net/if.h>

#define DPDK_LOG_DOM    "dpdk_manager"
#define DPDK_SOCK_DIR   "/var/run/purecvisor"
#define DPDK_HUGEPAGE   "/sys/kernel/mm/hugepages"

static struct {
    gboolean available;
    gboolean initialized;
    GMutex   mu;
} G = {0};

/* ── helpers ──────────────────────────────────────────────────── */

/**
 * _run_cmd — 셸 명령 동기 실행 내부 헬퍼
 * @cmd: 실행할 셸 명령 문자열
 * @out: (nullable): 표준 출력을 받을 포인터
 * @error: (nullable): 에러 반환 포인터
 *
 * /bin/sh -c 형태로 pcv_spawn_sync()를 호출.
 * 실패 시 stderr를 PCV_LOG_WARN으로 기록.
 *
 * @return 성공 시 TRUE
 */
static gboolean
_run_cmd(const gchar *cmd, gchar **out, GError **error)
{
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok)
        PCV_LOG_WARN(DPDK_LOG_DOM, "cmd failed: %s  stderr=%s", cmd,
                     std_err ? std_err : "(null)");
    g_free(std_err);
    return ok;
}

/**
 * _check_dpdk_init — OVS의 DPDK 초기화 상태 확인
 *
 * ovs-vsctl get Open_vSwitch . other_config:dpdk-init 명령으로
 * dpdk-init=true 여부를 확인한다.
 * ovs-vsctl 출력은 따옴표를 포함할 수 있으므로 ("true") 양쪽 모두 검사.
 *
 * @return dpdk-init=true이면 TRUE
 */
static gboolean
_check_dpdk_init(void)
{
    gchar *out = NULL;
    if (!_run_cmd("ovs-vsctl get Open_vSwitch . other_config:dpdk-init 2>/dev/null",
                  &out, NULL)) {
        g_free(out);
        return FALSE;
    }
    gboolean yes = (out && (g_str_has_prefix(g_strstrip(out), "\"true") ||
                            g_strcmp0(g_strstrip(out), "true") == 0));
    g_free(out);
    return yes;
}

/* ── lifecycle ────────────────────────────────────────────────── */

/**
 * pcv_dpdk_init — OVS-DPDK 매니저 초기화
 *
 * _check_dpdk_init()으로 OVS에 dpdk-init=true가 설정되었는지 확인.
 * 미설정 시 G.available=FALSE로 graceful degradation 모드 진입.
 * 데몬 시작 시 main.c에서 호출.
 */
void
pcv_dpdk_init(void)
{
    g_mutex_init(&G.mu);
    G.available = _check_dpdk_init();
    G.initialized = TRUE;
    PCV_LOG_INFO(DPDK_LOG_DOM, "OVS-DPDK %s",
                 G.available ? "available" : "not available (dpdk-init != true)");
}

/** pcv_dpdk_shutdown — OVS-DPDK 매니저 종료. 뮤텍스 해제. */
void
pcv_dpdk_shutdown(void)
{
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/** pcv_dpdk_is_available — DPDK 가용 여부 반환 (dpdk-init=true 확인 결과) */
gboolean
pcv_dpdk_is_available(void)
{
    return G.available;
}

/* ── status ───────────────────────────────────────────────────── */

/**
 * pcv_dpdk_status — DPDK 전체 상태 조회
 *
 * 반환 JsonObject:
 *   available:    DPDK 가용 여부
 *   vdev_count:   DPDK 바인딩된 포트 수
 *   pmd_cpu_mask: PMD(Poll Mode Driver) CPU 마스크
 *   socket_mem:   DPDK 소켓 메모리 설정값
 *
 * DPDK 미가용 시 available=false + 기본값(0/"") 반환.
 * ovs-vsctl get 출력에서 따옴표를 제거하여 정리된 값을 반환한다.
 *
 * @return (transfer full): 상태 JsonObject (호출자 unref)
 */
JsonObject *
pcv_dpdk_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "available", G.available);

    if (!G.available) {
        json_object_set_int_member(obj, "vdev_count", 0);
        json_object_set_string_member(obj, "pmd_cpu_mask", "");
        json_object_set_string_member(obj, "socket_mem", "");
        return obj;
    }

    /* PMD CPU mask */
    gchar *pmd = NULL;
    if (_run_cmd("ovs-vsctl get Open_vSwitch . other_config:pmd-cpu-mask 2>/dev/null",
                 &pmd, NULL) && pmd) {
        g_strstrip(pmd);
        /* ovs-vsctl 출력은 따옴표 포함 가능 */
        gchar *clean = g_strdup(pmd);
        g_strdelimit(clean, "\"", ' ');
        g_strstrip(clean);
        json_object_set_string_member(obj, "pmd_cpu_mask", clean);
        g_free(clean);
    } else {
        json_object_set_string_member(obj, "pmd_cpu_mask", "0x0");
    }
    g_free(pmd);

    /* socket memory */
    gchar *smem = NULL;
    if (_run_cmd("ovs-vsctl get Open_vSwitch . other_config:dpdk-socket-mem 2>/dev/null",
                 &smem, NULL) && smem) {
        g_strstrip(smem);
        gchar *clean = g_strdup(smem);
        g_strdelimit(clean, "\"", ' ');
        g_strstrip(clean);
        json_object_set_string_member(obj, "socket_mem", clean);
        g_free(clean);
    } else {
        json_object_set_string_member(obj, "socket_mem", "");
    }
    g_free(smem);

    /* DPDK port count */
    gchar *ports = NULL;
    gint vdev_count = 0;
    if (_run_cmd("ovs-vsctl --columns=name,type find interface type=dpdk 2>/dev/null",
                 &ports, NULL) && ports) {
        /* 각 줄에 name 이 있으면 1개 포트 */
        gchar **lines = g_strsplit(ports, "\n", -1);
        for (gint i = 0; lines[i]; i++)
            if (g_str_has_prefix(g_strstrip(lines[i]), "name"))
                vdev_count++;
        g_strfreev(lines);
    }
    g_free(ports);
    json_object_set_int_member(obj, "vdev_count", vdev_count);

    return obj;
}

/**
 * pcv_dpdk_hugepage_info — hugepage 할당 현황 조회
 *
 * /sys/kernel/mm/hugepages/ 에서 1GB와 2MB hugepage의
 * 총 수(nr_hugepages)와 여유 수(free_hugepages)를 읽어 반환.
 *
 * 반환 JsonObject:
 *   hugepage_1g_total/free, hugepage_1g_size_mb(=1024)
 *   hugepage_2m_total/free, hugepage_2m_size_mb(=2)
 *   total_mb, free_mb (합산 MB)
 *
 * @return (transfer full): hugepage 정보 JsonObject (호출자 unref)
 */
JsonObject *
pcv_dpdk_hugepage_info(void)
{
    JsonObject *obj = json_object_new();

    /* 1GB hugepages */
    gchar *nr1g = NULL;
    gint64 total_1g = 0, free_1g = 0;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-1048576kB/nr_hugepages",
                            &nr1g, NULL, NULL) && nr1g)
        total_1g = g_ascii_strtoll(g_strstrip(nr1g), NULL, 10);
    g_free(nr1g);

    gchar *fr1g = NULL;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-1048576kB/free_hugepages",
                            &fr1g, NULL, NULL) && fr1g)
        free_1g = g_ascii_strtoll(g_strstrip(fr1g), NULL, 10);
    g_free(fr1g);

    json_object_set_int_member(obj, "hugepage_1g_total", total_1g);
    json_object_set_int_member(obj, "hugepage_1g_free", free_1g);
    json_object_set_int_member(obj, "hugepage_1g_size_mb", 1024);

    /* 2MB hugepages */
    gchar *nr2m = NULL;
    gint64 total_2m = 0, free_2m = 0;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-2048kB/nr_hugepages",
                            &nr2m, NULL, NULL) && nr2m)
        total_2m = g_ascii_strtoll(g_strstrip(nr2m), NULL, 10);
    g_free(nr2m);

    gchar *fr2m = NULL;
    if (g_file_get_contents(DPDK_HUGEPAGE "/hugepages-2048kB/free_hugepages",
                            &fr2m, NULL, NULL) && fr2m)
        free_2m = g_ascii_strtoll(g_strstrip(fr2m), NULL, 10);
    g_free(fr2m);

    json_object_set_int_member(obj, "hugepage_2m_total", total_2m);
    json_object_set_int_member(obj, "hugepage_2m_free", free_2m);
    json_object_set_int_member(obj, "hugepage_2m_size_mb", 2);

    gint64 total_mb = total_1g * 1024 + total_2m * 2;
    gint64 free_mb = free_1g * 1024 + free_2m * 2;
    json_object_set_int_member(obj, "total_mb", total_mb);
    json_object_set_int_member(obj, "free_mb", free_mb);

    return obj;
}

/* ── NIC binding ──────────────────────────────────────────────── */

/**
 * pcv_dpdk_bind — NIC을 DPDK 드라이버에 바인딩
 * @pci_addr: PCI 주소 (예: "0000:01:00.0")
 * @driver: (nullable): DPDK 드라이버 이름 (기본: "vfio-pci")
 * @error: 에러 반환 포인터
 *
 * dpdk-devbind.py --bind=<driver> <pci_addr> 명령을 실행한다.
 * dpdk-devbind.py의 경로가 시스템마다 다르므로 두 경로를 OR로 시도.
 *
 * [주의] 관리 NIC(eno1)을 바인딩하면 SSH 접속이 불가해진다.
 * DPDK 미가용 시 GError 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_dpdk_bind(const gchar *pci_addr, const gchar *driver, GError **error)
{
    if (!G.available) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 1,
                    "OVS-DPDK not available (dpdk-init != true)");
        return FALSE;
    }

    if (!pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid PCI address: %s", pci_addr ? pci_addr : "(null)");
        return FALSE;
    }

    const gchar *drv = driver ? driver : "vfio-pci";
    /* driver 이름 검증: [a-zA-Z0-9_-] 만 허용 (shell 인젝션 방지) */
    if (!pcv_validate_bridge_name(drv)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid driver name: %s", drv);
        return FALSE;
    }
    gchar *cmd = g_strdup_printf(
        "dpdk-devbind.py --bind=%s %s 2>&1 || "
        "python3 /usr/share/dpdk/usertools/dpdk-devbind.py --bind=%s %s 2>&1",
        drv, pci_addr, drv, pci_addr);

    g_mutex_lock(&G.mu);
    gboolean ok = _run_cmd(cmd, NULL, error);
    g_mutex_unlock(&G.mu);

    g_free(cmd);
    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "Bound %s to %s", pci_addr, drv);
    return ok;
}

/**
 * pcv_dpdk_unbind — NIC을 DPDK에서 언바인드 (커널 드라이버 복원, 멱등)
 * @pci_addr: PCI 주소
 * @error: 에러 반환 포인터
 *
 * 이미 언바인드 상태이면 성공으로 처리 ("; true" 사용).
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_dpdk_unbind(const gchar *pci_addr, GError **error)
{
    if (!pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid PCI address: %s", pci_addr ? pci_addr : "(null)");
        return FALSE;
    }

    /* 멱등: 이미 언바인드 상태이면 성공 */
    gchar *cmd = g_strdup_printf(
        "dpdk-devbind.py --unbind %s 2>/dev/null || "
        "python3 /usr/share/dpdk/usertools/dpdk-devbind.py --unbind %s 2>/dev/null; true",
        pci_addr, pci_addr);

    gboolean ok = _run_cmd(cmd, NULL, error);
    g_free(cmd);
    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "Unbound %s from DPDK driver", pci_addr);
    return ok;
}

/**
 * pcv_dpdk_list — DPDK 바인딩된 네트워크 디바이스 목록 조회
 *
 * dpdk-devbind.py --status-dev net 출력을 파싱하여
 * "Network devices using DPDK-compatible driver" 섹션의
 * 디바이스를 JsonArray로 반환.
 *
 * 각 디바이스: {pci_addr, driver, status("dpdk-bound")}
 * DPDK 미가용 시 빈 배열 반환.
 *
 * @return (transfer full): 디바이스 JsonObject 배열
 */
JsonArray *
pcv_dpdk_list(void)
{
    JsonArray *arr = json_array_new();

    if (!G.available)
        return arr;

    gchar *out = NULL;
    if (!_run_cmd(
            "dpdk-devbind.py --status-dev net 2>/dev/null || "
            "python3 /usr/share/dpdk/usertools/dpdk-devbind.py --status-dev net 2>/dev/null",
            &out, NULL) || !out) {
        g_free(out);
        return arr;
    }

    /*
     * 출력 형식 (DPDK 바인딩 섹션):
     * Network devices using DPDK-compatible driver
     * ============================================
     * 0000:01:00.0 'X710 ...' drv=vfio-pci unused=i40e
     */
    gboolean in_dpdk_section = FALSE;
    gchar **lines = g_strsplit(out, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (g_str_has_prefix(line, "Network devices using DPDK")) {
            in_dpdk_section = TRUE;
            continue;
        }
        if (g_str_has_prefix(line, "Network devices using kernel") ||
            g_str_has_prefix(line, "No 'network'") ||
            (line[0] == '\0' && in_dpdk_section && json_array_get_length(arr) > 0)) {
            in_dpdk_section = FALSE;
            continue;
        }
        if (line[0] == '=' || line[0] == '\0')
            continue;

        if (in_dpdk_section && strlen(line) > 12) {
            JsonObject *dev = json_object_new();
            /* PCI 주소 (첫 12자) */
            gchar pci[16] = {0};
            g_strlcpy(pci, line, MIN((gsize)13, strlen(line) + 1));
            g_strstrip(pci);
            json_object_set_string_member(dev, "pci_addr", pci);

            /* 드라이버 파싱 */
            gchar *drv_pos = strstr(line, "drv=");
            if (drv_pos) {
                drv_pos += 4;
                gchar *end = strpbrk(drv_pos, " \t");
                gchar *drv = end ? g_strndup(drv_pos, (gsize)(end - drv_pos))
                                 : g_strdup(drv_pos);
                json_object_set_string_member(dev, "driver", drv);
                g_free(drv);
            }

            json_object_set_string_member(dev, "status", "dpdk-bound");
            json_array_add_object_element(arr, dev);
        }
    }
    g_strfreev(lines);
    g_free(out);
    return arr;
}

/* ── DPDK bridge ──────────────────────────────────────────────── */

/**
 * pcv_dpdk_bridge_create — DPDK 가속 OVS 브릿지 생성
 * @name: 브릿지 이름
 * @dpdk_port: (nullable): DPDK 포트 PCI 주소 (있으면 자동 연결)
 * @error: 에러 반환 포인터
 *
 * 실행 순서:
 *   1) ovs-vsctl add-br <name> -- set bridge datapath_type=netdev
 *      (datapath_type=netdev가 DPDK 데이터패스 활성화의 핵심)
 *   2) ip link set <name> up
 *   3) (dpdk_port 있으면) add-port + type=dpdk + dpdk-devargs=<pci>
 *
 * DPDK 미가용 시 GError 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port, GError **error)
{
    if (!G.available) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 1,
                    "OVS-DPDK not available");
        return FALSE;
    }

    if (!pcv_validate_bridge_name(name)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid bridge name: 1-16 chars [a-zA-Z0-9_-]");
        return FALSE;
    }

    /* dpdk_port(PCI BDF) 검증 — 미지정(NULL/빈문자열) 허용, 값이 있으면 형식 강제 */
    if (dpdk_port && *dpdk_port && !pcv_validate_pci_addr(dpdk_port)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid dpdk_port PCI address: %s", dpdk_port);
        return FALSE;
    }

    g_mutex_lock(&G.mu);

    /* 브릿지 생성 (datapath_type=netdev) — argv 배열, 셸 미경유 */
    const gchar *br_argv[] = {
        "ovs-vsctl", "--may-exist", "add-br", name,
        "--", "set", "bridge", name, "datapath_type=netdev", NULL
    };
    gchar *serr = NULL;
    gboolean ok = pcv_spawn_sync(br_argv, NULL, &serr, error);
    if (!ok)
        PCV_LOG_WARN(DPDK_LOG_DOM, "ovs-vsctl add-br failed: %s",
                     serr ? serr : "(null)");
    g_free(serr);

    if (ok) {
        const gchar *up_argv[] = {"ip", "link", "set", name, "up", NULL};
        pcv_spawn_sync(up_argv, NULL, NULL, NULL);   /* best-effort */
    }

    /* DPDK 포트 추가 (선택) */
    if (ok && dpdk_port && *dpdk_port) {
        gchar *port_name = g_strdup_printf("dpdk-p-%s", name);
        /* dpdk-devargs 는 셸 분리 없이 단일 argv 원소로 그대로 전달 */
        gchar *devargs = g_strdup_printf("options:dpdk-devargs=%s", dpdk_port);
        const gchar *port_argv[] = {
            "ovs-vsctl", "--may-exist", "add-port", name, port_name,
            "--", "set", "interface", port_name, "type=dpdk", devargs, NULL
        };
        gchar *serr2 = NULL;
        ok = pcv_spawn_sync(port_argv, NULL, &serr2, error);
        if (!ok)
            PCV_LOG_WARN(DPDK_LOG_DOM, "ovs-vsctl add-port failed: %s",
                         serr2 ? serr2 : "(null)");
        g_free(serr2);
        g_free(devargs);
        g_free(port_name);
    }
    g_mutex_unlock(&G.mu);

    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "DPDK bridge '%s' created", name);
    return ok;
}

/**
 * pcv_dpdk_bridge_delete — DPDK OVS 브릿지 삭제 (멱등)
 * @name: 삭제할 브릿지 이름
 * @error: 에러 반환 포인터
 *
 * ovs-vsctl --if-exists del-br 로 멱등 삭제.
 * 브릿지 이름 누락 시 GError 반환.
 *
 * @return 성공 시 TRUE
 */
gboolean
pcv_dpdk_bridge_delete(const gchar *name, GError **error)
{
    if (!pcv_validate_bridge_name(name)) {
        g_set_error(error, g_quark_from_static_string("dpdk"), 2,
                    "Invalid bridge name: 1-16 chars [a-zA-Z0-9_-]");
        return FALSE;
    }

    /* 멱등 삭제 */
    gchar *cmd = g_strdup_printf("ovs-vsctl --if-exists del-br %s", name);
    g_mutex_lock(&G.mu);
    gboolean ok = _run_cmd(cmd, NULL, error);
    g_mutex_unlock(&G.mu);
    g_free(cmd);

    if (ok)
        PCV_LOG_INFO(DPDK_LOG_DOM, "DPDK bridge '%s' deleted", name);
    return ok;
}

/* ── vhost-user ───────────────────────────────────────────────── */

/**
 * pcv_dpdk_vhost_socket_path — VM별 vhost-user 소켓 경로 생성
 * @vm_name: VM 이름
 *
 * DPDK 가속 VM 연결 시 사용하는 vhost-user 소켓 경로를 생성.
 * VM XML에서 <interface type='vhostuser'>로 참조.
 * 경로: /var/run/purecvisor/vhost-<vm_name>.sock
 *
 * @return (transfer full): 소켓 경로 (호출자 g_free), vm_name=NULL이면 NULL
 */
gchar *
pcv_dpdk_vhost_socket_path(const gchar *vm_name)
{
    if (!vm_name)
        return NULL;
    return g_strdup_printf("%s/vhost-%s.sock", DPDK_SOCK_DIR, vm_name);
}

/* ── NET-1: 관리 NIC 보호 가드 ─────────────────────────────────── */

/* netdev가 <proc_base>/proc/net/route 의 기본경로(Destination 00000000) dev인지. */
gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base)
{
    if (!netdev) return FALSE;
    gchar *path = g_strdup_printf("%s/proc/net/route", proc_base ? proc_base : "");
    gchar *content = NULL;
    gboolean is_def = FALSE;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (gint i = 1; lines[i]; i++) {   /* lines[0]=헤더 */
            gchar **f = g_strsplit_set(lines[i], "\t ", -1);
            gchar *iface = NULL, *dest = NULL; gint n = 0;
            for (gchar **c = f; *c; c++) {
                if (**c == '\0') continue;  /* 연속 구분자 스킵 */
                if (n == 0) iface = *c; else if (n == 1) dest = *c;
                n++;
            }
            if (iface && dest && g_strcmp0(dest, "00000000") == 0 &&
                g_strcmp0(iface, netdev) == 0)
                is_def = TRUE;
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(path);
    return is_def;
}

/* pci_addr → 커널 netdev 이름 목록(sysfs). 빈 목록 = 커널 미관리. */
static GList *_dpdk_pci_netdevs(const gchar *pci_addr)
{
    gchar *dir = g_strdup_printf("/sys/bus/pci/devices/%s/net", pci_addr);
    GList *out = NULL;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const gchar *n;
        while ((n = g_dir_read_name(d))) out = g_list_prepend(out, g_strdup(n));
        g_dir_close(d);
    }
    g_free(dir);
    return out;
}

/* netdev가 UP + IPv4 주소 보유인지. getifaddrs 실패=TRUE(fail-secure)이되,
 * out_ifaddr_err로 실패 사유를 구분한다 (NET-1 M1: reason 오귀속 시정).
 * 판정값(TRUE/FALSE)과 정상 UP+IPv4 매치 경로는 무변경 — 호출자가 reason
 * 문구만 getifaddrs 실패/실제 매치로 분기하도록 out-param만 추가한다. */
static gboolean _dpdk_up_with_ipv4(const gchar *netdev, gboolean *out_ifaddr_err)
{
    if (out_ifaddr_err) *out_ifaddr_err = FALSE;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) {
        if (out_ifaddr_err) *out_ifaddr_err = TRUE;
        return TRUE;   /* fail-secure */
    }
    gboolean prot = FALSE;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (p->ifa_name && g_strcmp0(p->ifa_name, netdev) == 0 &&
            p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
            (p->ifa_flags & IFF_UP)) { prot = TRUE; break; }
    }
    freeifaddrs(ifa);
    return prot;
}

gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason)
{
    if (reason) *reason = NULL;
    if (!pci_addr || !*pci_addr) return TRUE;         /* fail-secure */
    /* NET-1: 형식검증 실패(경로순회 '..' 포함) BDF는 sysfs 탐침 없이 거부(fail-secure). */
    if (!pcv_validate_pci_addr(pci_addr)) {
        if (reason) *reason = g_strdup("refusing to bind: invalid PCI address");
        return TRUE;
    }
    GList *devs = _dpdk_pci_netdevs(pci_addr);
    if (!devs) return FALSE;                            /* 커널 미관리 = 통과 */
    gboolean prot = FALSE;
    for (GList *l = devs; l && !prot; l = l->next) {
        const gchar *nd = l->data;
        gboolean ifaddr_err = FALSE;
        if (_dpdk_up_with_ipv4(nd, &ifaddr_err)) {
            /* NET-1 M1: getifaddrs 실패(syscall 오류)와 실제 UP+IPv4 매치를
             * reason 문구에서 구분 — 이전에는 둘 다 "up with an IPv4 address"로
             * 오귀속되어, 열거 실패임에도 마치 실제 IPv4가 있는 것처럼 보고됐다. */
            if (reason) *reason = ifaddr_err
                ? g_strdup_printf(
                    "refusing to bind: interface enumeration failed for %s (fail-secure)", nd)
                : g_strdup_printf(
                    "refusing to bind: NIC %s is up with an IPv4 address", nd);
            prot = TRUE;
        } else if (pcv_dpdk_route_is_default_dev(nd, "")) {
            if (reason) *reason = g_strdup_printf(
                "refusing to bind: NIC %s carries the default route", nd);
            prot = TRUE;
        }
    }
    g_list_free_full(devs, g_free);
    return prot;
}
