/**
 * @file vm_manager.c
 * @brief VM 생명주기 관리 — 비동기 GTask 기반 CRUD + 동적 리소스 튜닝
 *
 * == 아키텍처에서의 위치 ==
 *   디스패처(dispatcher.c) → handler_vm_*.c → vm_manager → libvirt API + ZFS
 *   이 모듈은 PureCVisor의 핵심 엔진으로, VM의 전체 생명주기를 관리합니다.
 *
 * == 비동기 처리 패턴 (GTask) ==
 *   모든 무거운 작업은 GTask 워커 스레드에서 실행되어 GMainLoop를 블로킹하지 않습니다.
 *   1. _async() 호출 → GTask + TaskData 구조체 생성
 *   2. g_task_run_in_thread()로 워커 스레드에 위임
 *   3. 워커에서 virt_conn_pool_acquire() → libvirt Raw API → release()
 *   4. g_task_return_*()로 결과 설정 → 메인 스레드의 콜백에서 _finish() 호출
 *
 * == VM 생성 흐름 (create) ==
 *   1. ZFS zvol 생성: zfs create -V <size>G pcvpool/vms/<name>
 *   2. VM XML 빌드: vm_config_builder + NIC XML 직접 패치
 *      - 브릿지 모드: <interface type='bridge'> + OVS 자동 감지(virtualport)
 *      - DPDK 모드: <interface type='vhostuser'>
 *      - SR-IOV 모드: <hostdev mode='subsystem' type='pci'>
 *   3. virDomainDefineXML로 libvirt에 등록
 *   4. etcd에 VM XML 동기화 (pcv_cluster_sync_vm_xml)
 *   5. RBAC owner metadata 기록:
 *      VM을 만든 사용자명을 libvirt XML metadata의 pcv:owner에 저장합니다.
 *      이후 dispatcher.c는 이 이름표를 보고 operator가 자신의 VM만 조작하게
 *      막습니다. 비전공자 관점에서는 "VM에 소유자 스티커를 붙여 두고,
 *      같은 스티커의 사용자만 전원 버튼을 누르게 하는 장치"입니다.
 *
 * == VM 삭제 흐름 (delete) ==
 *   1. 실행 중이면 virDomainDestroy로 강제 종료
 *   2. virDomainUndefineFlags로 정의 제거
 *   3. ZFS destroy를 별도 GTask로 fire-and-forget (지수 백오프 재시도)
 *   4. etcd에서 VM XML 제거 (pcv_cluster_remove_vm_xml)
 *
 * == ZFS 삭제 재시도 ==
 *   virDomainDestroy 직후 zvol 디바이스가 커널에서 아직 release 안 될 수 있음
 *   → 최대 5회 지수 백오프(500ms→1s→2s→4s→8s)로 재시도
 *
 * == GObject Signal (GIO P6) ==
 *   - "vm-started": start_vm_finish()에서 emit
 *   - "vm-stopped": stop_vm_finish()에서 emit
 *   - "vm-metrics-updated": telemetry.c에서 메인 스레드로 emit
 *
 * == 주의사항 ==
 *   - GVirConnection은 자체 도메인 캐시를 가짐 → virt_conn_pool의 virConnectPtr로
 *     define한 도메인이 보이지 않을 수 있음. create/start/stop/delete는 모두
 *     raw libvirt API(virDomainLookupByName) 사용. list만 GVirConnection 경유.
 *   - ZFS destroy는 fire-and-forget → 실패해도 RPC 응답에는 성공으로 반환됨
 *   - 스토리지 풀: pcv_config_get_zvol_pool() (기본: pcvpool/vms)
 */
/*
 * PureCVisor Engine - VM Manager Module
 * Phase 5: Advanced Lifecycle & Monitoring
 *
 * Capabilities:
 * - Async VM Management (Create, Start, Stop, Delete) with GTask.
 * - Live State Inspection via ID check (Linker-safe).
 * - Dynamic VNC Port Parsing via XML Regex.
 * - ZFS Storage Orchestration.
 */

/* ── 헤더 의존성 ──────────────────────────────────────────────────────────
 *
 * [의존성 계층]
 *   vm_manager.h       : 이 모듈의 공개 API 선언 (PureCVisorVmManager 타입 등)
 *   virt_conn_pool.h   : libvirt 연결 풀 — acquire/release로 연결을 빌려 사용
 *   vm_config_builder.h: VM XML 뼈대를 조립하는 빌더 패턴 모듈
 *   zfs_driver.h       : ZFS zvol 생성/삭제 래퍼 (purecvisor_zfs_destroy_volume 등)
 *   pcv_config.h       : daemon.conf 설정 조회 (zvol_pool, image_dir 등)
 *   cluster_manager.h  : etcd VM XML 동기화 (클러스터 모드에서만 링크됨)
 *   pcv_spawn.h        : system()/popen() 대신 안전한 프로세스 스폰 (argv 배열 기반)
 *   pcv_log.h          : 모듈별 로그 레벨 지원 로깅 매크로
 *
 * [libvirt 이중 인터페이스]
 *   libvirt-gobject: GObject 래퍼 — GVirConnection, GVirDomain 등
 *     → vm.list에서만 사용 (도메인 캐시 기반 열거가 편리)
 *   libvirt/libvirt.h: C Raw API — virConnectPtr, virDomainPtr 등
 *     → create/start/stop/delete에서 사용 (캐시 불일치 문제 회피)
 *   이 두 인터페이스를 혼용하는 이유는 GVirConnection 캐시와
 *   virt_conn_pool의 virConnectPtr가 서로 독립적이기 때문입니다.
 * ──────────────────────────────────────────────────────────────────────── */
#include "vm_manager.h"
#include "virt_conn_pool.h"
#include "vm_config_builder.h"
#include "../storage/zfs_driver.h"
#include "../audit/pcv_audit.h"
#include "utils/pcv_config.h"
#include "api/ws_server.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "../../utils/pcv_spawn.h"  /* GIO P3: pcv_spawn_sync */
#include "../../utils/pcv_log.h"    /* PCV_LOG_INFO / PCV_LOG_WARN */
#include "modules/dispatcher/rpc_utils.h"  /* PURE_RPC_ERR_* (DISP-6) */
#include "modules/core/vm_state.h"          /* unlock_vm_operation (resize_disk 락 하드닝) */

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#define PCV_VM_METADATA_URI "urn:purecvisor:metadata"

/**
 * PureCVisorVmManager 내부 구조체 정의
 *
 * [왜 GObject 상속 구조를 사용하는가? — 주니어 필독]
 *   GObject는 C 언어에서 객체 지향 프로그래밍을 구현하는 GLib의 타입 시스템입니다.
 *   C에는 class/extends가 없으므로 GObject가 이를 매크로와 함수 포인터로 구현합니다.
 *
 *   GObject를 사용하는 3가지 이유:
 *   1. 참조 카운팅: g_object_ref/unref로 메모리를 안전하게 관리합니다.
 *      GTask가 vm_manager를 참조하는 동안 해제되지 않도록 보장합니다.
 *   2. 시그널 시스템: g_signal_emit/connect로 옵저버 패턴을 구현합니다.
 *      "vm-started", "vm-stopped" 이벤트를 virt_events, telemetry 등에 전달합니다.
 *   3. libvirt-gobject 호환: GVirConnection, GVirDomain 등이 모두 GObject이므로
 *      같은 패턴으로 메모리를 관리할 수 있습니다.
 *
 *   G_DEFINE_TYPE 매크로가 자동으로 생성하는 것들:
 *   - PURECVISOR_TYPE_VM_MANAGER: GType 식별자
 *   - PURECVISOR_IS_VM_MANAGER(obj): 타입 체크 매크로
 *   - purecvisor_vm_manager_get_type(): GType 등록 함수
 *   - purecvisor_vm_manager_parent_class: 부모 클래스 포인터
 *
 * GVirConnection은 list_vms에서만 사용 (도메인 캐시 기반 조회).
 * create/start/stop/delete는 virt_conn_pool의 raw virConnectPtr를 사용합니다.
 */
struct _PureCVisorVmManager {
    GObject parent_instance;
    GVirConnection *conn;   /* libvirt-gobject 연결 — vm.list 전용 */
};

/* GObject 타입 시스템 등록 매크로
 * PURECVISOR_TYPE_VM_MANAGER, PURECVISOR_IS_VM_MANAGER() 등의 매크로를 자동 생성합니다. */
G_DEFINE_TYPE(PureCVisorVmManager, purecvisor_vm_manager, G_TYPE_OBJECT)

/* --------------------------------------------------------------------------
 * [GIO P6] Signal ID 배열
 * class_init에서 g_signal_new()로 채워집니다.
 * 시그널은 GObject의 이벤트 시스템으로, 옵저버 패턴을 구현합니다.
 * g_signal_connect()로 핸들러를 등록하고 g_signal_emit()으로 발신합니다.
 * -------------------------------------------------------------------------- */
typedef enum {
    SIGNAL_VM_STARTED = 0,       /* VM 시작 성공 시 발신 */
    SIGNAL_VM_STOPPED,           /* VM 정지 성공 시 발신 */
    SIGNAL_VM_METRICS_UPDATED,   /* 텔레메트리 갱신 시 발신 (1초 주기) */
    N_SIGNALS
} PcvVmManagerSignalId;

static guint signals[N_SIGNALS] = { 0 };

/* --------------------------------------------------------------------------
 * [Helper] Live XML 파싱을 통한 VNC 포트 추출
 * -------------------------------------------------------------------------- */

/**
 * _extract_vnc_port_from_domain:
 * 실행 중인 VM의 Live XML에서 VNC 포트 번호를 추출합니다.
 *
 * libvirt는 autoport=yes 설정 시 5900번부터 순차적으로 포트를 할당하며,
 * 할당된 포트 번호는 실행 중인 도메인의 Live Config XML에서만 확인할 수 있습니다.
 * Inactive Config에는 port=-1만 기록되어 있습니다.
 *
 * 파싱 방법: XML 문자열에서 정규식 <graphics[^>]+port='(\d+)'>를 매칭합니다.
 * libvirt XML 구조가 보장되므로 전체 XML 파서 대신 정규식으로 충분합니다.
 *
 * @param dom GVirDomain* — 실행 중인 도메인 객체
 * @return VNC 포트 번호 (성공), -1 (VM이 꺼져 있거나 VNC 미설정)
 */
static gint _extract_vnc_port_from_domain(GVirDomain *dom) {
    GError *err = nullptr;
    GVirConfigDomain *config = nullptr;
    gchar *xml_data = nullptr;
    gint port = -1;

    // 실행 중인 도메인의 Live Config 가져오기 (Flag: 0 -> Current/Live)
    config = gvir_domain_get_config(dom, 0, &err);
    if (err) {
        // VM이 꺼져있거나 Config를 가져올 수 없는 경우 무시
        g_error_free(err);
        return -1;
    }

    xml_data = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(config));
    g_object_unref(config);

    if (!xml_data) return -1;

    // XML을 가져온 후 Regex 부분만 교체:
    // "port='(\d+)'" 패턴을 찾되, 앞부분에 "<graphics"가 있는지 확인하는 것이 정석이나,
    // libvirt XML에서 port 속성을 가진 주요 태그는 graphics와 serial/console 임.
    // 간단하고 강력하게: "graphics type='vnc'" 가 포함된 라인 주변의 port를 찾는 복잡한 로직 대신,
    // 전체 XML에서 "port='(\d+)'"를 찾되, VNC 포트 범위(5900~)인 것을 찾는 휴리스틱 사용 가능.
    // 또는, 속성 순서에 유연한 Regex 사용:
    
    // 패턴: <graphics [^>]*port='(\d+)'
    GRegex *regex = g_regex_new("<graphics[^>]+port='(\\d+)'",
                                G_REGEX_CASELESS | G_REGEX_MULTILINE, 0, NULL);
    
    GMatchInfo *match_info;
    if (g_regex_match(regex, xml_data, 0, &match_info)) {
        gchar *port_str = g_match_info_fetch(match_info, 1);
        if (port_str) {
            port = (gint)g_ascii_strtoll(port_str, NULL, 10);
            g_free(port_str);
        }
    }

    g_match_info_free(match_info);
    g_regex_unref(regex);
    g_free(xml_data);

    return port;
}

/* --------------------------------------------------------------------------
 * [GObject] 초기화 및 소멸자
 * finalize: GObject 참조 카운트가 0이 될 때 호출 — 연결 해제
 * class_init: 클래스 최초 로드 시 1회 호출 — 시그널 등록
 * init: 인스턴스 생성 시 호출 — 필드 초기화
 * -------------------------------------------------------------------------- */

/**
 * purecvisor_vm_manager_finalize:
 * GObject 소멸자. GVirConnection 참조를 해제합니다.
 * 부모 클래스의 finalize를 체인 호출하여 완전한 정리를 보장합니다.
 */
static void purecvisor_vm_manager_finalize(GObject *object) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(object);
    if (self->conn) {
        g_object_unref(self->conn);
    }
    G_OBJECT_CLASS(purecvisor_vm_manager_parent_class)->finalize(object);
}

/**
 * purecvisor_vm_manager_class_init:
 * GObject 클래스 초기화. 소멸자 등록 및 3개의 시그널을 선언합니다.
 * 이 함수는 GType 시스템이 이 클래스를 처음 사용할 때 1회만 호출됩니다.
 */
static void purecvisor_vm_manager_class_init(PureCVisorVmManagerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_vm_manager_finalize;

    /* ------------------------------------------------------------------
     * GIO P6: 신호 등록
     * ------------------------------------------------------------------ */

    /**
     * PureCVisorVmManager::vm-started:
     * @mgr:     발신 vm_manager 인스턴스
     * @vm_name: 기동에 성공한 VM 이름 (gchar*)
     *
     * purecvisor_vm_manager_start_vm_finish() 가 TRUE 를 반환한 직후
     * 메인 스레드에서 emit 됩니다.
     */
    signals[SIGNAL_VM_STARTED] =
        g_signal_new(PCV_VM_SIGNAL_STARTED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,                  /* class_offset: 가상 메서드 없음 */
                     NULL, NULL,         /* accumulator, accu_data */
                     NULL,              /* c_marshaller: GLib 자동 선택 */
                     G_TYPE_NONE,        /* return type */
                     1,                  /* n_params */
                     G_TYPE_STRING);     /* param: vm_name */

    /**
     * PureCVisorVmManager::vm-stopped:
     * @mgr:     발신 vm_manager 인스턴스
     * @vm_name: 정지에 성공한 VM 이름 (gchar*)
     *
     * purecvisor_vm_manager_stop_vm_finish() 가 TRUE 를 반환한 직후
     * 메인 스레드에서 emit 됩니다.
     */
    signals[SIGNAL_VM_STOPPED] =
        g_signal_new(PCV_VM_SIGNAL_STOPPED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_STRING);     /* param: vm_name */

    /**
     * PureCVisorVmManager::vm-metrics-updated:
     * @mgr:   발신 vm_manager 인스턴스
     * @cache: (transfer none): 교체된 메트릭 해시테이블 (VmMetrics* 값)
     *
     * telemetry 데몬이 메트릭 캐시를 스왑한 직후
     * g_main_context_invoke 를 통해 메인 스레드에서 emit 됩니다.
     * 핸들러에서 cache 의 수명은 다음 emit 시까지만 유효합니다.
     * g_free() / g_hash_table_destroy() 절대 금지.
     */
    signals[SIGNAL_VM_METRICS_UPDATED] =
        g_signal_new(PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_POINTER);   /* param: GHashTable *cache */
}

/** 인스턴스 초기화 — 모든 필드를 NULL로 초기화 */
static void purecvisor_vm_manager_init(PureCVisorVmManager *self) {
    self->conn = nullptr;
}

/**
 * purecvisor_vm_manager_new:
 * 새 VM 매니저 인스턴스를 생성합니다.
 *
 * @param conn GVirConnection* — libvirt-gobject 연결 (vm.list에서 사용, NULL 가능)
 * @return PureCVisorVmManager* — 호출자가 g_object_unref()로 해제
 */
PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn) {
    PureCVisorVmManager *self = g_object_new(PURECVISOR_TYPE_VM_MANAGER, NULL);
    if (conn) {
        self->conn = g_object_ref(conn);
    }
    return self;
}

/* --------------------------------------------------------------------------
 * [비동기 태스크] VM 생성 (Create VM)
 *
 * 전체 흐름:
 *   1. ZFS zvol 프로비저닝 (zfs create -V)
 *   2. VM XML 빌더(vm_config_builder)로 도메인 XML 조립
 *   3. NIC XML 직접 패치 (bridge/dpdk/sriov 분기)
 *   4. virDomainDefineXML로 libvirt에 등록
 *   5. etcd에 VM XML 동기화 (클러스터 페일오버 대비)
 * -------------------------------------------------------------------------- */

/**
 * CreateVmTaskData:
 * GTask에 전달되는 VM 생성 파라미터 묶음.
 * GTask는 void*로 데이터를 전달하므로 구조체로 패키징합니다.
 */
typedef struct {
    PureCVisorVmManager *manager;     /* vm_manager 인스턴스 (ref 보유) */
    gchar *name;                      /* VM 이름 — ZFS 데이터셋 이름으로도 사용 */
    gint vcpu;                        /* 가상 CPU 개수 */
    gint ram_mb;                      /* 메모리 크기 (MB) */
    gint disk_size_gb;                /* ZFS zvol 크기 (GB, 0이면 기본 50GB) */
    gchar *disk_path;                 /* ZFS zvol 블록 디바이스 경로 */
    gchar *iso_path;                  /* ISO 파일 경로 (NULL이면 CD-ROM 미생성) */
    gchar *network_bridge;            /* 브릿지 이름 (NULL이면 NIC 미생성) */
    gint   vlan_id;                   /* 802.1Q VLAN ID (0이면 태깅 없음) */
    gchar *nic_type;                  /* NIC 타입: "bridge"(기본) / "dpdk" / "sriov" */
    gchar *pci_addr;                  /* SR-IOV VF PCI 주소 (sriov 모드 전용) */
    gint boot_mode;                   /* 0=BIOS, 1=UEFI, 2=UEFI+SecureBoot */
    gboolean tpm;                     /* TRUE=TPM 2.0 에뮬레이터 추가 */
    gint cpu_mode;                    /* 0=Single Edge 기본(host-passthrough), 1=host-passthrough, 2=host-model */
    gboolean hugepages;               /* TRUE=2MB huge pages 사용 */
    gchar *storage_type;              /* "zvol"(기본), "qcow2", "raw" */
    gchar *storage_pool;              /* zvol 부모 데이터셋: pcvpool/vms, tank/vms 등 */
    gchar *image_dir;                 /* qcow2/raw 파일 디스크 저장 디렉터리 */
    gchar *base_image;                /* BUG-16: base cloud image 경로 (qcow2→zvol 자동 기록) */
    gchar *owner;                     /* RBAC: 인증된 VM 생성자 username */
} CreateVmTaskData;

/** CreateVmTaskData 메모리 해제 — GTask의 destroy notify로 등록됨 */
static void create_vm_task_data_free(CreateVmTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data->disk_path);
    g_free(data->iso_path);
    g_free(data->network_bridge);
    g_free(data->nic_type);
    g_free(data->pci_addr);
    g_free(data->storage_type);
    g_free(data->storage_pool);
    g_free(data->image_dir);
    g_free(data->base_image);
    g_free(data->owner);
    g_free(data);
}

static gchar *
_vm_xml_with_owner_metadata(const gchar *xml, const gchar *owner)
{
    /* [왜 XML에 owner를 넣는가?]
     * VM의 진짜 상태와 정의는 libvirt가 관리합니다. 별도 DB에만 owner를
     * 저장하면 DB와 libvirt가 어긋났을 때 권한 판단이 틀릴 수 있습니다.
     * 그래서 VM 정의 XML 내부 metadata에 owner를 함께 저장합니다.
     *
     * [주니어 참고]
     * owner 문자열은 XML 문법을 깨거나 태그를 주입하지 못하도록
     * g_markup_escape_text()로 escape합니다. 삽입 위치는 <name> 바로 뒤를
     * 우선하고, 구조가 예상과 다르면 <devices> 또는 </domain> 앞에 넣습니다. */
    if (!xml || !owner || !*owner)
        return g_strdup(xml);

    gchar *safe_owner = g_markup_escape_text(owner, -1);
    gchar *metadata = g_strdup_printf(
        "  <metadata>\n"
        "    <pcv:owner xmlns:pcv='%s'>%s</pcv:owner>\n"
        "  </metadata>\n",
        PCV_VM_METADATA_URI, safe_owner);
    g_free(safe_owner);

    const gchar *insert = strstr(xml, "</name>");
    gchar *patched = NULL;
    if (insert) {
        insert += strlen("</name>");
        patched = g_strdup_printf("%.*s%s%s",
                                  (gint)(insert - xml), xml,
                                  metadata, insert);
    } else {
        insert = strstr(xml, "<devices>");
        if (insert) {
            patched = g_strdup_printf("%.*s%s%s",
                                      (gint)(insert - xml), xml,
                                      metadata, insert);
        } else {
            const gchar *end = strstr(xml, "</domain>");
            patched = end
                ? g_strdup_printf("%.*s%s%s", (gint)(end - xml), xml, metadata, end)
                : g_strdup(xml);
        }
    }

    g_free(metadata);
    return patched;
}

/**
 * create_vm_thread — GTask 워커에서 VM 생성 전체 파이프라인 실행
 *
 * [호출 시점] purecvisor_vm_manager_create_vm_async()에서 g_task_run_in_thread()로 위임
 * [동작] 1→ZFS zvol/qcow2 디스크 프로비저닝 → 2→vm_config_builder로 XML 뼈대 생성
 *        → 3→NIC/UEFI/TPM/CPU XML 직접 패치 → 4→virDomainDefineXML로 libvirt 등록
 *        → 5→etcd VM XML 동기화 (클러스터 페일오버 대비)
 * [스레드] GLib 스레드풀의 워커 스레드 (GMainLoop 블로킹 없음)
 * [주의] 실패 시 롤백: 생성된 zvol/qcow2를 자동 삭제합니다 (원자성 보장).
 *        GCancellable로 취소된 경우에도 디스크 정리 후 반환합니다.
 *        이 함수가 반환하면 GTask 콜백이 메인 스레드에서 호출됩니다.
 *
 * @param task        GTask 객체
 * @param task_data   CreateVmTaskData* — 생성 파라미터
 * @param cancellable GCancellable (취소 시 디스크 정리 후 반환)
 */
static void create_vm_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    CreateVmTaskData *data = (CreateVmTaskData *)task_data;
    GError *error = nullptr;
    PCV_LOG_INFO("vm_manager", "VM '%s' creation worker started (vcpu=%d ram=%dMB disk=%dGB stype=%s)",
                 data ? data->name : "(null)",
                 data ? data->vcpu : -1,
                 data ? data->ram_mb : -1,
                 data ? data->disk_size_gb : -1,
                 (data && data->storage_type) ? data->storage_type : "(auto)");

    /* [A1 fix] 시작 시점 취소 체크 — 요청 수락 후 워커 스케줄링 전까지의 지연 중 취소됐으면 조기 종료 */
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "vm.create cancelled before start");
        return;
    }

    /* 1. 디스크 프로비저닝 — zvol 우선, qcow2 폴백
     *
     * 전략:
     *   (1) ZFS 풀(pcvpool/vms) 존재 여부 확인
     *   (2) ZFS 풀 있음 → zfs create -V (zvol 블록 디바이스)
     *   (3) ZFS 풀 없음 → qemu-img create -f qcow2 (파일 디스크 폴백)
     *
     * disk_path 결과:
     *   zvol: "/dev/zvol/pcvpool/vms/web-prod"        → DISK_BLOCK + RAW
     *   qcow2: "/var/lib/libvirt/images/web-prod.qcow2" → DISK_FILE + QCOW2
     */
    /* [디스크 크기 검증]
     *   0이면 기본 50GB, 최대 2048GB(2TB) 제한.
     *   2TB 제한 이유: ZFS zvol은 논리적으로 더 커질 수 있지만,
     *   실수로 수 TB를 할당하면 풀 공간이 고갈될 수 있습니다.
     *   프로덕션에서는 이 제한을 daemon.conf로 조정할 수 있습니다. */
    gint final_disk_size = (data->disk_size_gb > 0) ? data->disk_size_gb : 50;
    if (final_disk_size > 2048) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "disk_size_gb (%d) exceeds 2TB limit", final_disk_size);
        return;
    }
    const gchar *zvol_pool = (data->storage_pool && *data->storage_pool)
        ? data->storage_pool
        : pcv_config_get_zvol_pool();
    const gchar *image_dir = (data->image_dir && *data->image_dir)
        ? data->image_dir
        : pcv_config_get_image_dir();
    gchar *disk_path = nullptr;
    gboolean use_zvol = FALSE;

    /* storage_type 파라미터에 따른 스토리지 모드 결정
     *   "zvol"  → ZFS zvol 블록 디바이스 (풀 없으면 에러)
     *   "qcow2" → qcow2 파일 디스크
     *   "raw"   → raw 파일 디스크
     *   NULL    → 자동 감지 (ZFS 풀 있으면 zvol, 없으면 qcow2 폴백)
     */
    const gchar *st = data->storage_type;
    gboolean use_file_raw = FALSE;  /* raw 파일 모드 여부 */

    if (st && g_strcmp0(st, "qcow2") == 0) {
        use_zvol = FALSE;
    } else if (st && g_strcmp0(st, "raw") == 0) {
        use_zvol = FALSE;
        use_file_raw = TRUE;
    } else if (st && g_strcmp0(st, "zvol") == 0) {
        /* zvol 명시 지정: ZFS 풀 존재 여부 확인, 없으면 에러 */
        const gchar *pool_chk_argv[] = {"zfs", "list", "-H",
                                         zvol_pool, NULL};
        gchar *chk_err = nullptr;
        GError *chk_e = nullptr;
        use_zvol = pcv_spawn_sync(pool_chk_argv, NULL, &chk_err, &chk_e);
        g_free(chk_err);
        if (chk_e) g_error_free(chk_e);
        if (!use_zvol) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "storage_type 'zvol' requested but ZFS pool '%s' not found",
                zvol_pool);
            return;
        }
    } else {
        /* 자동 감지 (기본): ZFS 풀 있으면 zvol, 없으면 qcow2 폴백 */
        const gchar *pool_chk_argv[] = {"zfs", "list", "-H",
                                         zvol_pool, NULL};
        gchar *chk_err = nullptr;
        GError *chk_e = nullptr;
        use_zvol = pcv_spawn_sync(pool_chk_argv, NULL, &chk_err, &chk_e);
        g_free(chk_err);
        if (chk_e) g_error_free(chk_e);
    }

    /* ADR-0011: zvol I/O 펜싱 — 클러스터 모드에서 소유권 미보유 시 zvol 생성 차단 */
#if PCV_CLUSTER_ENABLED
    if (use_zvol && !pcv_cluster_check_zvol_fence()) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "zvol I/O fence: this node does not hold zvol pool ownership (ADR-0011). "
            "Only the cluster leader with confirmed ownership can create zvol-backed VMs.");
        return;
    }
#endif

    if (use_zvol) {
        /* === zvol 모드: ZFS 블록 디바이스 === */
        gchar *zvol_name = g_strdup_printf("%s/%s",
                                            zvol_pool, data->name);
        gchar *zvol_dev  = g_strdup_printf("/dev/zvol/%s", zvol_name);

        /* 이미 존재하는 zvol 사전 감지 */
        {
            const gchar *chk_argv[] = {"zfs", "list", "-H", "-t", "volume",
                                        zvol_name, NULL};
            gchar *chk_err = nullptr;
            GError *chk_e = nullptr;
            gboolean exists = pcv_spawn_sync(chk_argv, NULL, &chk_err, &chk_e);
            g_free(chk_err);
            if (chk_e) g_error_free(chk_e);
            if (exists) {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "ZFS dataset '%s' already exists — delete the VM first",
                    zvol_name);
                g_free(zvol_name); g_free(zvol_dev);
                return;
            }
        }

        gchar *size_str = g_strdup_printf("%dG", final_disk_size);
        const gchar *zfs_argv[] = {"zfs", "create", "-V", size_str,
                                    zvol_name, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
            gchar *err_msg = error ? error->message
                                   : (std_err ? g_strstrip(std_err)
                                              : "Unknown ZFS error");
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "ZFS Provisioning Failed: %s", err_msg);
            if (error) g_error_free(error);
            g_free(std_err); g_free(size_str);
            g_free(zvol_name); g_free(zvol_dev);
            return;
        }
        g_free(std_err); g_free(size_str);
        disk_path = zvol_dev;
        g_free(zvol_name);
        PCV_LOG_INFO("vm_manager", "VM '%s': zvol disk created at %s (%dG)",
                     data->name, zvol_pool, final_disk_size);

        /* BUG-16 fix: base_image가 지정되면 qemu-img convert로 zvol에 자동 기록.
         * cloud image(qcow2) → zvol(raw) 변환하여 OS가 바로 부팅 가능.
         * udevadm settle로 zvol 디바이스 노드 생성을 보장한 후 기록. */
        if (data->base_image && *data->base_image &&
            g_file_test(data->base_image, G_FILE_TEST_EXISTS)) {
            const gchar *udev_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
            (void)pcv_spawn_sync(udev_argv, NULL, NULL, NULL);
            const gchar *conv_argv[] = {
                "qemu-img", "convert", "-f", "qcow2", "-O", "raw",
                data->base_image, disk_path, NULL
            };
            gchar *conv_err = NULL;
            gboolean conv_ok = pcv_spawn_sync(conv_argv, NULL, &conv_err, NULL);
            if (conv_ok) {
                PCV_LOG_INFO("vm_manager", "VM '%s': base image '%s' written to zvol",
                             data->name, data->base_image);
            } else {
                PCV_LOG_WARN("vm_manager", "VM '%s': base image write failed: %s",
                             data->name, conv_err ? conv_err : "unknown");
            }
            g_free(conv_err);
        }
    } else {
        /* === 파일 디스크 모드: qcow2 또는 raw === */
        const gchar *fmt = use_file_raw ? "raw" : "qcow2";
        const gchar *ext = use_file_raw ? "img" : "qcow2";
        /* 이미지 디렉터리 자동 생성 */
        if (!g_file_test(image_dir, G_FILE_TEST_IS_DIR)) {
            const gchar *mkdir_argv[] = {"mkdir", "-p", image_dir, NULL};
            (void)pcv_spawn_sync(mkdir_argv, NULL, NULL, NULL);
        }

        disk_path = g_strdup_printf("%s/%s.%s", image_dir, data->name, ext);

        /* 기존 파일 존재 확인 */
        if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                "Disk image '%s' already exists — delete the VM first",
                disk_path);
            g_free(disk_path);
            return;
        }

        gchar *size_str = g_strdup_printf("%dG", final_disk_size);
        const gchar *qimg_argv[] = {"qemu-img", "create", "-f", fmt,
                                     disk_path, size_str, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(qimg_argv, NULL, &std_err, &error)) {
            gchar *err_msg = error ? error->message
                                   : (std_err ? g_strstrip(std_err)
                                              : "Unknown qemu-img error");
            PCV_LOG_WARN("vm_manager", "VM '%s' qemu-img FAILED: %s (stderr=%s)",
                         data->name, err_msg, std_err ? std_err : "(none)");
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "%s Provisioning Failed: %s", fmt, err_msg);
            g_unlink(disk_path);
            if (error) g_error_free(error);
            g_free(std_err); g_free(size_str); g_free(disk_path);
            return;
        }
        g_free(std_err); g_free(size_str);
        PCV_LOG_INFO("vm_manager", "VM '%s': %s disk created at %s (%dG)",
                      data->name, fmt, disk_path, final_disk_size);
    }

    /* 2. VM 뼈대(XML) 조립 — disk_path에 따라 block/file 자동 감지 */
    PureCVisorVmConfig *config = purecvisor_vm_config_new(data->name,
                                                          data->vcpu,
                                                          data->ram_mb);
    purecvisor_vm_config_set_disk(config, disk_path);

    if (data->iso_path) purecvisor_vm_config_set_iso(config, data->iso_path);
    if (data->cpu_mode > 0) purecvisor_vm_config_set_cpu_mode(config, data->cpu_mode);
    if (data->hugepages) purecvisor_vm_config_set_hugepages(config, TRUE);
    if (data->boot_mode > 0) purecvisor_vm_config_set_boot_mode(config, data->boot_mode);
    if (data->tpm) purecvisor_vm_config_set_tpm(config, TRUE);
    /* network_bridge / vlan_id 는 아래 XML 패치에서 직접 처리 */

    GVirConfigDomain *domain_config = purecvisor_vm_config_build(config);
    
    gchar *base_xml = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(domain_config));

    /* ── [Sprint G / B-1] <interface> + 선택적 VLAN 태깅 XML 직접 삽입 ────
     *
     * libvirt-gobject 에는 GVIR_CONFIG_DOMAIN_INTERFACE_MODEL_VIRTIO 상수가
     * 없고 gvir_config_object_get_xml_node() 도 공개 API 가 아니므로
     * gvir_config_object_to_xml() 이 반환한 XML 문자열에서 </devices> 를
     * 찾아 그 바로 앞에 <interface> 블록을 삽입합니다.
     *
     * VLAN 태깅(vlan_id > 0):
     *   <interface ...>
     *     ...
     *     <vlan><tag id="N"/></vlan>
     *   </interface>
     * ──────────────────────────────────────────────────────────────────── */
    /* ── NIC XML 패치 ──────────────────────────────────────────────────────
     *
     * [왜 XML 문자열 직접 패치인가?]
     *   libvirt-gobject API에는 NIC 모델(virtio)을 설정하는 상수가 없고,
     *   XML 노드에 직접 접근하는 gvir_config_object_get_xml_node()도 비공개입니다.
     *   따라서 gvir_config_object_to_xml()이 반환한 XML 문자열에서
     *   </devices> 태그를 찾아 그 바로 앞에 <interface> 블록을 삽입합니다.
     *
     * [NIC 타입 분기]
     *   "bridge" (기본): Linux Bridge 또는 OVS Bridge에 virtio NIC 연결
     *   "dpdk":         OVS-DPDK 가속 브릿지에 vhost-user 소켓으로 연결
     *   "sriov":        물리 NIC의 VF(Virtual Function)를 VM에 PCI passthrough
     *
     * [OVS 자동 감지]
     *   bridge 모드에서 ovs-vsctl br-exists 명령으로 해당 브릿지가
     *   Open vSwitch 브릿지인지 Linux Bridge인지 자동 판별합니다.
     *   OVS이면 <virtualport type='openvswitch'/>을 추가해야
     *   libvirt가 ovs-vsctl로 포트를 관리합니다.
     * ──────────────────────────────────────────────────────────────────── */
    gchar *final_xml = nullptr;
    if (data->network_bridge && strlen(data->network_bridge) > 0) {
        gchar *iface_xml = nullptr;
        const gchar *nic_type = data->nic_type ? data->nic_type : "bridge";

        if (g_strcmp0(nic_type, "dpdk") == 0) {
            /* ── Phase 4: DPDK vhost-user 인터페이스 ─────────────────
             * OVS-DPDK 가속 브릿지에 vhost-user 소켓으로 연결
             * vhost-user 소켓: /var/run/purecvisor/vhost-<vm_name>.sock */
            iface_xml = g_strdup_printf(
                "    <interface type='vhostuser'>\n"
                "      <source type='unix' path='/var/run/purecvisor/vhost-%s.sock' mode='server'/>\n"
                "      <model type='virtio'/>\n"
                "      <driver queues='2'/>\n"
                "    </interface>\n",
                data->name);
        } else if (g_strcmp0(nic_type, "sriov") == 0 && data->pci_addr) {
            /* ── Phase 4: SR-IOV PCI passthrough ────────────────────
             * VF를 직접 VM에 할당 (vfio-pci 바인딩은 sriov_manager에서 처리)
             * PCI 주소: 0000:01:10.0 → domain=0x0000 bus=0x01 slot=0x10 func=0x0 */
            guint dom = 0, bus = 0, slot = 0, func = 0;
            sscanf(data->pci_addr, "%x:%x:%x.%x", &dom, &bus, &slot, &func);
            iface_xml = g_strdup_printf(
                "    <hostdev mode='subsystem' type='pci' managed='yes'>\n"
                "      <source>\n"
                "        <address domain='0x%04x' bus='0x%02x' slot='0x%02x' function='0x%x'/>\n"
                "      </source>\n"
                "    </hostdev>\n",
                dom, bus, slot, func);
        } else {
            /* ── 기본: Linux Bridge / OVS bridge 인터페이스 ──────── */
            gchar *vlan_xml = (data->vlan_id >= 1 && data->vlan_id <= 4094)
                ? g_strdup_printf("      <vlan><tag id=\"%d\"/></vlan>\n", data->vlan_id)
                : g_strdup("");

            /* OVS 자동 감지: ovs-vsctl br-exists는 해당 브릿지가
             * OVS 브릿지이면 종료코드 0(성공), 아니면 2(실패)를 반환합니다.
             * pcv_spawn_sync는 종료코드 0이면 TRUE를 반환합니다. */
            const gchar *ovs_argv[] = {"ovs-vsctl", "br-exists", data->network_bridge, NULL};
            gboolean is_ovs = pcv_spawn_sync(ovs_argv, NULL, NULL, NULL);

            /* OVS 브릿지이면 <virtualport type='openvswitch'/>를 추가해야
             * libvirt가 ovs-vsctl add-port로 포트를 연결합니다.
             * Linux Bridge이면 빈 문자열 (brctl addif 방식으로 자동 처리) */
            gchar *virtualport_xml = is_ovs
                ? g_strdup("      <virtualport type='openvswitch'/>\n")
                : g_strdup("");

            /* XML 특수문자 이스케이프 — 브릿지 이름에 <, >, & 등이 포함될 경우
             * XML 파싱 오류를 방지합니다 (방어적 프로그래밍) */
            gchar *safe_bridge = g_markup_escape_text(data->network_bridge, -1);
            iface_xml = g_strdup_printf(
                "    <interface type='bridge'>\n"
                "      <source bridge='%s'/>\n"
                "%s"
                "      <model type='virtio'/>\n"
                "%s"
                "    </interface>\n",
                safe_bridge, virtualport_xml, vlan_xml);
            g_free(safe_bridge);
            g_free(virtualport_xml);
            g_free(vlan_xml);
        }

        /* </devices> 직전에 삽입 */
        gchar *insert_point = strstr(base_xml, "</devices>");
        if (insert_point) {
            gsize prefix_len = (gsize)(insert_point - base_xml);
            final_xml = g_strdup_printf("%.*s%s%s",
                                        (gint)prefix_len, base_xml,
                                        iface_xml,
                                        insert_point);
        } else {
            /* </devices> 없으면 그대로 사용 (이상 케이스) */
            final_xml = g_strdup(base_xml);
        }
        g_free(iface_xml);
        g_free(base_xml);
    } else {
        /* 브릿지 없으면 XML 그대로 사용 */
        final_xml = base_xml;
    }

    if (data->owner && *data->owner) {
        gchar *owned_xml = _vm_xml_with_owner_metadata(final_xml, data->owner);
        g_free(final_xml);
        final_xml = owned_xml;
    }

    /* 3b. Video 해상도 패치 — 기본 1024×768
     * libvirt-gobject에는 video resolution 설정 API가 없으므로
     * XML 문자열에서 <model type="virtio"/> 를 해상도 포함 버전으로 교체합니다.
     *
     * 변환 전: <model type="virtio"/>
     * 변환 후: <model type="virtio" vram="65536" heads="1">
     *            <resolution x="1024" y="768"/>
     *          </model>
     */
    {
        const gchar *old_video = "<model type=\"virtio\"/>";
        const gchar *new_video =
            "<model type=\"virtio\" vram=\"65536\" heads=\"1\">\n"
            "          <resolution x=\"1024\" y=\"768\"/>\n"
            "        </model>";
        gchar *pos = strstr(final_xml, old_video);
        if (pos) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(pos - final_xml), final_xml,
                new_video, pos + strlen(old_video));
            g_free(final_xml);
            final_xml = patched;
        }
    }

    /* CPU 모드는 vm_config_builder가 구조화 API로 설정합니다.
     * 여기서 문자열 패치를 다시 수행하면 <cpu> 중복 XML이 만들어질 수 있습니다. */

    /* 3d. Huge Pages 패치 — 2MB hugepage 활성화
     * </domain> 직전에 <memoryBacking> 블록을 삽입합니다.
     * 호스트에 충분한 hugepage가 사전 할당되어 있어야 합니다.
     * (echo N > /proc/sys/vm/nr_hugepages 또는 sysctl vm.nr_hugepages=N) */
    if (data->hugepages) {
        const gchar *hp_xml = "  <memoryBacking><hugepages><page size='2048' unit='KiB'/></hugepages></memoryBacking>\n";
        gchar *end = strstr(final_xml, "</domain>");
        if (end) {
            gchar *patched = g_strdup_printf("%.*s%s%s", (gint)(end - final_xml), final_xml, hp_xml, end);
            g_free(final_xml);
            final_xml = patched;
        }
    }

    /* 3e. UEFI 부팅 모드 패치 — OVMF 펌웨어 로더 삽입
     * libvirt-gobject의 OS 설정 API로는 UEFI 로더를 지정할 수 없으므로
     * XML 문자열에서 <type> 태그를 UEFI 로더 포함 버전으로 교체합니다.
     *
     * boot_mode == 1: UEFI (OVMF_CODE_4M.fd 또는 OVMF_CODE.fd)
     * boot_mode == 2: UEFI + SecureBoot (OVMF_CODE_4M.ms.fd + secure='yes')
     *
     * OVMF 경로 자동 감지: Ubuntu/Debian → RHEL/Fedora 순서로 탐색
     * per-VM NVRAM: /var/lib/libvirt/qemu/nvram/<vm>_VARS.fd */
    /* ── 3e. UEFI 부팅 모드 패치 ─────────────────────────────────────────
     *
     * [UEFI란?]
     *   BIOS를 대체하는 현대적 펌웨어 인터페이스입니다.
     *   SecureBoot 등 최신 guest OS 기능에 필요합니다.
     *   QEMU/KVM에서는 OVMF (Open Virtual Machine Firmware) 구현을 사용합니다.
     *
     * [OVMF 파일 구성]
     *   OVMF_CODE: 펌웨어 코드 (읽기 전용, 모든 VM이 공유)
     *   OVMF_VARS: 펌웨어 변수 저장소 (VM별 복사본 필요 — NVRAM)
     *   _4M 변형: 4MB 변수 영역 (최신 Ubuntu 22.04+에서 기본)
     *   .ms.fd: Microsoft 인증서 번들 포함 (SecureBoot용)
     *
     * [경로 자동 감지]
     *   Ubuntu/Debian과 RHEL/Fedora에서 OVMF 파일 경로가 다릅니다.
     *   g_file_test()로 파일 존재 여부를 확인하여 첫 번째 존재하는 경로를 사용합니다.
     *   어떤 경로도 없으면 기본 경로(Ubuntu)로 폴백합니다.
     *
     * [per-VM NVRAM]
     *   각 VM마다 독립적인 NVRAM 파일이 필요합니다.
     *   /var/lib/libvirt/qemu/nvram/<vm_name>_VARS.fd 경로로 생성됩니다.
     *   libvirt가 template에서 자동 복사합니다.
     * ──────────────────────────────────────────────────────────────────── */
    if (data->boot_mode >= 1) {
        const gchar *old_os = "<type machine=\"q35\">hvm</type>";

        /* OVMF 로더 경로 자동 감지 (SecureBoot vs 일반 UEFI) */
        const gchar *loader_path = nullptr;
        const gchar *nvram_tpl = nullptr;

        if (data->boot_mode == 2) {
            /* SecureBoot: MS 인증서 번들 포함 OVMF (4M 변형 우선) */
            static const gchar *sb_loaders[] = {
                "/usr/share/OVMF/OVMF_CODE_4M.ms.fd",      /* Ubuntu 22.04+ */
                "/usr/share/OVMF/OVMF_CODE.secboot.fd",     /* Ubuntu legacy */
                "/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd",/* RHEL/Fedora   */
                NULL
            };
            static const gchar *sb_nvrams[] = {
                "/usr/share/OVMF/OVMF_VARS_4M.ms.fd",
                "/usr/share/OVMF/OVMF_VARS.fd",
                "/usr/share/edk2/ovmf/OVMF_VARS.fd",
                NULL
            };
            for (gint i = 0; sb_loaders[i]; i++) {
                if (g_file_test(sb_loaders[i], G_FILE_TEST_EXISTS)) {
                    loader_path = sb_loaders[i];
                    nvram_tpl = sb_nvrams[i];
                    break;
                }
            }
            if (!loader_path) {
                loader_path = sb_loaders[0]; /* fallback to default path */
                nvram_tpl = sb_nvrams[0];
            }
        } else {
            /* 일반 UEFI (4M 변형 우선) */
            static const gchar *uefi_loaders[] = {
                "/usr/share/OVMF/OVMF_CODE_4M.fd",          /* Ubuntu 22.04+ */
                "/usr/share/OVMF/OVMF_CODE.fd",              /* Ubuntu legacy */
                "/usr/share/edk2/ovmf/OVMF_CODE.fd",         /* RHEL/Fedora   */
                NULL
            };
            static const gchar *uefi_nvrams[] = {
                "/usr/share/OVMF/OVMF_VARS_4M.fd",
                "/usr/share/OVMF/OVMF_VARS.fd",
                "/usr/share/edk2/ovmf/OVMF_VARS.fd",
                NULL
            };
            for (gint i = 0; uefi_loaders[i]; i++) {
                if (g_file_test(uefi_loaders[i], G_FILE_TEST_EXISTS)) {
                    loader_path = uefi_loaders[i];
                    nvram_tpl = uefi_nvrams[i];
                    break;
                }
            }
            if (!loader_path) {
                loader_path = uefi_loaders[0];
                nvram_tpl = uefi_nvrams[0];
            }
        }

        /* per-VM NVRAM 경로: /var/lib/libvirt/qemu/nvram/<vm>_VARS.fd */
        gchar *nvram_path = g_strdup_printf(
            "/var/lib/libvirt/qemu/nvram/%s_VARS.fd", data->name);

        /* XML 패치: <loader> + <nvram> + SecureBoot secure 속성 */
        const gchar *secure_attr = data->boot_mode == 2 ? " secure=\"yes\"" : "";
        gchar *new_os = g_strdup_printf(
            "<type machine=\"q35\">hvm</type>\n"
            "      <loader readonly=\"yes\" type=\"pflash\"%s>%s</loader>\n"
            "      <nvram template=\"%s\">%s</nvram>",
            secure_attr, loader_path, nvram_tpl, nvram_path);

        gchar *pos = strstr(final_xml, old_os);
        if (pos) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(pos - final_xml), final_xml,
                new_os, pos + strlen(old_os));
            g_free(final_xml);
            final_xml = patched;
        }
        g_free(new_os);
        g_free(nvram_path);
    }

    /* 3f. TPM 2.0 에뮬레이터 패치 — swtpm 백엔드
     * </devices> 직전에 <tpm> 디바이스를 삽입합니다.
     * swtpm 패키지가 설치되어 있어야 합니다 (apt install swtpm).
     * UEFI SecureBoot와 함께 사용하면 최신 guest OS의 TPM 요구사항을 충족합니다. */
    if (data->tpm) {
        const gchar *tpm_xml =
            "    <tpm model='tpm-tis'>\n"
            "      <backend type='emulator' version='2.0'/>\n"
            "    </tpm>\n";
        gchar *insert = strstr(final_xml, "</devices>");
        if (insert) {
            gchar *patched = g_strdup_printf("%.*s%s%s",
                (gint)(insert - final_xml), final_xml,
                tpm_xml, insert);
            g_free(final_xml);
            final_xml = patched;
        }
    }

    /* 3g. Watchdog 디바이스 — i6300esb 모델 + reset 액션
     * daemon.conf [vm] watchdog_enabled (기본값 true)로 제어합니다.
     * 게스트 OS가 watchdog을 pet하지 않으면 VM이 자동 재부팅됩니다. */
    {
        const gchar *wd_cfg = pcv_config_get_string("vm", "watchdog_enabled", "true");
        if (g_ascii_strcasecmp(wd_cfg, "true") == 0 ||
            g_ascii_strcasecmp(wd_cfg, "1") == 0 ||
            g_ascii_strcasecmp(wd_cfg, "yes") == 0) {
            const gchar *wd_xml =
                "    <watchdog model='i6300esb' action='reset'/>\n";
            gchar *insert = strstr(final_xml, "</devices>");
            if (insert) {
                gchar *patched = g_strdup_printf("%.*s%s%s",
                    (gint)(insert - final_xml), final_xml,
                    wd_xml, insert);
                g_free(final_xml);
                final_xml = patched;
            }
        }
    }

    /* ── 4. libvirt에 VM XML 등록 ─────────────────────────────────────────
     *
     * [virt_conn_pool_acquire/release 패턴]
     *   libvirt 연결(virConnectPtr)은 생성 비용이 높으므로 풀에서 빌려 사용합니다.
     *   acquire: 유휴 연결을 풀에서 가져옴 (없으면 새로 생성)
     *   release: 사용 완료 후 풀에 반납 (닫지 않고 재활용)
     *   반드시 acquire와 release가 짝을 이루어야 합니다 (연결 누수 방지).
     *
     * [virDomainDefineXML]
     *   XML 문자열로 VM을 libvirt에 영구 등록합니다 (persistent domain).
     *   등록만 하고 시작하지는 않습니다. 시작은 별도 virDomainCreate 호출이 필요합니다.
     *   성공하면 virDomainPtr를 반환하며, virDomainFree로 해제해야 합니다.
     *
     * [etcd 동기화]
     *   클러스터 모드(PCV_CLUSTER_ENABLED)에서는 VM XML을 etcd에 저장합니다.
     *   다른 노드가 이 VM을 페일오버로 인계받을 때 XML을 조회하여 define합니다.
     * ──────────────────────────────────────────────────────────────────── */
    /* ── GCancellable 취소 검사 ─────────────────────────────────────────
     * zvol/디스크 생성 후, libvirt 등록 전에 취소 요청을 확인합니다.
     * 취소된 경우 생성된 디스크를 정리하고 조기 반환합니다. */
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        /* 취소 요청 — 생성된 디스크 정리 후 반환 */
        if (disk_path && *disk_path) {
            if (g_str_has_prefix(disk_path, "/dev/zvol/")) {
                gchar *zvol_name = g_strdup(disk_path + strlen("/dev/zvol/"));
                const gchar *zfs_argv[] = {"zfs", "destroy", "-f", zvol_name, NULL};
                (void)pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
                g_free(zvol_name);
            } else if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
                g_unlink(disk_path);
            }
        }
        PCV_LOG_INFO("vm_manager", "VM '%s' creation cancelled before define", data->name);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
            "VM creation cancelled");
        g_free(final_xml);
        g_object_unref(domain_config);
        purecvisor_vm_config_free(config);
        g_free(disk_path);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    virDomainPtr dom = virDomainDefineXML(conn, final_xml);

    if (!dom) {
        /* [libvirt 에러 처리 패턴]
         *   virGetLastError(): 스레드-로컬 에러 구조체를 반환합니다.
         *   libvirt_err->message에 사람이 읽을 수 있는 에러 메시지가 있습니다.
         *   서버 로그에는 상세 에러를 기록하되, RPC 응답에는 일반적인 메시지만
         *   반환합니다 (보안: 내부 경로/설정 노출 방지). */
        virErrorPtr libvirt_err = virGetLastError();
        PCV_LOG_WARN("vm_manager", "virDomainDefineXML failed: %s",
                     libvirt_err ? libvirt_err->message : "unknown");

        /* Rollback: 정의 실패 시 생성된 zvol/디스크 정리 */
        if (disk_path && *disk_path) {
            if (g_str_has_prefix(disk_path, "/dev/zvol/")) {
                /* ZFS zvol 삭제 */
                gchar *zvol_name = g_strdup(disk_path + strlen("/dev/zvol/"));
                const gchar *zfs_argv[] = {"zfs", "destroy", "-f", zvol_name, NULL};
                (void)pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
                g_free(zvol_name);
                PCV_LOG_WARN("vm_manager", "Rolled back zvol for failed VM define: %s", disk_path);
            } else {
                /* qcow2/raw 파일 삭제 */
                if (g_file_test(disk_path, G_FILE_TEST_EXISTS)) {
                    g_unlink(disk_path);
                    PCV_LOG_WARN("vm_manager", "Rolled back disk file for failed VM define: %s", disk_path);
                }
            }
        }

        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "VM operation failed — check server logs for details");
    } else {
        /* BUG-19 fix (F-3): post-define XML 검증 + 1회 redefine 재시도
         *
         * 데몬이 define 중간에 SIGABRT/SIGKILL로 종료되고 재시작되면 libvirt는
         * 이전 define 결과를 보존하나, 파라미터가 요청과 다를 수 있다 (예:
         * config fallback으로 network_bridge가 virbr0가 됨). 여기서 실제 저장된
         * XML을 읽어 final_xml과 주요 요소를 비교, 불일치 시 undefine 후
         * redefine으로 교정한다. */
        gchar *stored = virDomainGetXMLDesc(dom, 0);
        if (stored && final_xml) {
            /* 단순 substring 검증: <source bridge='...' /> 가 기대값과 일치하는지 */
            const gchar *expected_br = strstr(final_xml, "<source bridge='");
            const gchar *actual_br   = strstr(stored,    "<source bridge='");
            gboolean bridge_match = TRUE;
            if (expected_br && actual_br) {
                expected_br += strlen("<source bridge='");
                actual_br   += strlen("<source bridge='");
                const gchar *e_end = strchr(expected_br, '\'');
                const gchar *a_end = strchr(actual_br,   '\'');
                if (e_end && a_end) {
                    gsize el = (gsize)(e_end - expected_br);
                    gsize al = (gsize)(a_end - actual_br);
                    if (el != al || strncmp(expected_br, actual_br, el) != 0) {
                        bridge_match = FALSE;
                        PCV_LOG_WARN("vm_manager",
                            "Post-define bridge mismatch for '%s' "
                            "(expected=%.*s, stored=%.*s) — redefining",
                            data->name, (int)el, expected_br, (int)al, actual_br);
                    }
                }
            }
            if (!bridge_match) {
                /* undefine → redefine (1회만 재시도, 무한 루프 방지) */
                virDomainUndefine(dom);
                virDomainFree(dom);
                dom = virDomainDefineXML(conn, final_xml);
                if (!dom) {
                    PCV_LOG_WARN("vm_manager",
                        "Redefine after mismatch failed for '%s'", data->name);
                }
            }
        }
        g_free(stored);
        if (dom) {
            virDomainFree(dom);
            /* VM XML을 etcd에 동기화 (페일오버 대비) */
#if PCV_CLUSTER_ENABLED
            pcv_cluster_sync_vm_xml(data->name);
#endif
            g_task_return_boolean(task, TRUE);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "VM redefine failed after parameter mismatch");
        }
    }

    /* ── 메모리 정리 ─────────────────────────────────────────────────────
     *
     * [역순 해제 원칙]
     *   할당 순서의 역순으로 해제하면 use-after-free를 방지할 수 있습니다.
     *   예: final_xml은 domain_config에서 파생되었으므로 먼저 해제합니다.
     *
     * [각 자원의 해제 방법]
     *   virConnectPtr  → virt_conn_pool_release() (풀에 반납, 닫지 않음)
     *   gchar*         → g_free() (GLib 힙 메모리)
     *   GVirConfigDomain* → g_object_unref() (GObject 참조 카운트 감소)
     *   PureCVisorVmConfig* → purecvisor_vm_config_free() (커스텀 구조체)
     *
     * [GTask 데이터(CreateVmTaskData)는?]
     *   g_task_set_task_data()에 등록한 destroy notify
     *   (create_vm_task_data_free)가 GTask 해제 시 자동 호출합니다. */
    virt_conn_pool_release(conn);
    g_free(final_xml);
    g_object_unref(domain_config);
    purecvisor_vm_config_free(config);
    g_free(disk_path);
}
 

/**
 * purecvisor_vm_manager_create_vm_async:
 * VM 생성 비동기 요청을 시작합니다.
 *
 * @param self           vm_manager 인스턴스
 * @param name           VM 이름 (ZFS 데이터셋 이름과 동일)
 * @param vcpu           가상 CPU 개수
 * @param ram_mb         메모리 크기 (MB)
 * @param disk_size_gb   디스크 크기 (GB, 0이면 기본 50GB)
 * @param iso_path       ISO 파일 경로 (NULL 가능)
 * @param network_bridge 브릿지 이름 (NULL 가능)
 * @param vlan_id        VLAN ID (0이면 태깅 없음)
 * @param storage_pool   zvol 부모 데이터셋 (NULL이면 daemon.conf 기본값)
 * @param image_dir      qcow2/raw 파일 디스크 저장 디렉터리 (NULL이면 daemon.conf 기본값)
 * @param callback       완료 콜백 (메인 스레드에서 호출)
 * @param user_data      콜백 사용자 데이터
 */
void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           gint vcpu,
                                           gint ram_mb,
                                           gint disk_size_gb,
                                           const gchar *iso_path,
                                           const gchar *network_bridge,
                                           gint         vlan_id,
                                           gint         boot_mode,
                                           gboolean     tpm,
                                           gint         cpu_mode,
                                           gboolean     hugepages,
                                           const gchar *storage_type,
                                           const gchar *storage_pool,
                                           const gchar *image_dir,
                                           const gchar *nic_type,
                                           const gchar *pci_addr,
                                           const gchar *base_image,     /* BUG-16: cloud image 경로 */
                                           const gchar *owner,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
    /* ── GTask 비동기 패턴 설명 ──────────────────────────────────────────
     *
     * [g_task_new]
     *   GTask 객체를 생성합니다. GTask는 GLib의 비동기 작업 프레임워크입니다.
     *   - self: source_object — 작업의 소유자 (결과 조회 시 참조)
     *   - NULL: cancellable — 취소 토큰 (현재 미사용)
     *   - callback: 작업 완료 시 메인 스레드에서 호출되는 콜백
     *   - user_data: 콜백에 전달할 사용자 데이터
     *
     * [g_new0]
     *   g_malloc + memset(0) — 구조체를 0으로 초기화하여 할당합니다.
     *   NULL 포인터와 0 값으로 초기화되므로 안전합니다.
     *
     * [g_object_ref(self)]
     *   GTask가 실행 중인 동안 vm_manager가 해제되지 않도록 참조를 증가합니다.
     *   create_vm_task_data_free()에서 g_object_unref로 짝을 맞춥니다.
     *
     * [g_task_set_task_data]
     *   워커 스레드에서 접근할 데이터를 설정합니다.
     *   destroy notify로 free 함수를 등록하면 GTask 해제 시 자동 호출됩니다.
     *
     * [g_task_run_in_thread]
     *   GLib 스레드풀에서 create_vm_thread를 실행합니다.
     *   GMainLoop를 블로킹하지 않습니다.
     *
     * [g_object_unref(task)]
     *   GTask의 참조 카운트를 감소시킵니다.
     *   워커 스레드가 아직 실행 중이어도 GTask 내부적으로 참조를 잡고 있으므로
     *   즉시 해제되지 않습니다. 워커 완료 + 콜백 실행 후 최종 해제됩니다.
     * ──────────────────────────────────────────────────────────────────── */
    /* [A1 fix] GTask에 cancellable 연결 — worker의 g_cancellable_is_cancelled() 체크 + drain cleanup */
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    CreateVmTaskData *data = g_new0(CreateVmTaskData, 1);

    data->manager = g_object_ref(self);
    data->name = g_strdup(name);
    data->vcpu = vcpu;
    data->ram_mb = ram_mb;
    data->disk_size_gb = disk_size_gb; // [Added]
    data->iso_path = iso_path ? g_strdup(iso_path) : NULL;
    /* VP-1: 브릿지 결정 단일화 — 미지정/""→기본 네트워크(pcvnat0), "none"→NULL(NIC 미부착).
     * resolve가 g_strdup된 소유 문자열을 반환하므로 create_vm_task_data_free()가 해제. */
    data->network_bridge = purecvisor_vm_resolve_network_bridge(network_bridge);
    data->vlan_id = vlan_id; // [Sprint G]
    data->boot_mode = boot_mode;
    data->tpm = tpm;
    data->cpu_mode = cpu_mode;
    data->hugepages = hugepages;
    data->storage_type = storage_type ? g_strdup(storage_type) : NULL;
    data->storage_pool = storage_pool ? g_strdup(storage_pool) : NULL;
    data->image_dir = image_dir ? g_strdup(image_dir) : NULL;
    data->nic_type = nic_type ? g_strdup(nic_type) : NULL;
    data->pci_addr = pci_addr ? g_strdup(pci_addr) : NULL;
    data->base_image = base_image ? g_strdup(base_image) : NULL;
    data->owner = owner && *owner ? g_strdup(owner) : NULL;

    g_task_set_task_data(task, data, (GDestroyNotify)create_vm_task_data_free);
    g_task_run_in_thread(task, create_vm_thread);
    g_object_unref(task);
}

/**
 * purecvisor_vm_manager_create_vm_finish:
 * VM 생성 비동기 작업의 결과를 회수합니다.
 * 콜백 내부에서 호출하여 성공/실패를 확인합니다.
 *
 * @return TRUE: 성공, FALSE: 실패 (error에 원인 설정)
 */
gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/**
 * purecvisor_vm_resolve_network_bridge:
 * VP-1 — vm.create/템플릿의 브릿지 결정 단일화.
 * NULL/"" → config network.default_bridge(기본 pcvnat0), "none" → NULL(NIC 미부착),
 * 그 외 → 그대로. 반환값은 g_free 필요.
 */
gchar *
purecvisor_vm_resolve_network_bridge(const gchar *requested)
{
    if (!requested || !*requested)
        return g_strdup(pcv_config_get_string("network", "default_bridge", "pcvnat0"));
    if (g_strcmp0(requested, "none") == 0)
        return NULL;
    return g_strdup(requested);
}


/* --------------------------------------------------------------------------
 * [비동기 태스크] VM 시작/정지/삭제 공통 구조체
 * start/stop/delete는 모두 VM 이름만 필요하므로 동일한 구조체를 공유합니다.
 * -------------------------------------------------------------------------- */

/**
 * LifecycleTaskData:
 * VM 라이프사이클 작업(start/stop/delete)의 GTask 데이터.
 */
typedef struct {
    PureCVisorVmManager *manager;
    gchar *name;
} LifecycleTaskData;

/** LifecycleTaskData 메모리 해제 — GTask의 destroy notify로 등록됨 */
static void lifecycle_task_data_free(LifecycleTaskData *data) {
    if (data->manager) g_object_unref(data->manager);
    g_free(data->name);
    g_free(data);
}

/**
 * start_vm_thread_impl — GTask 워커에서 VM을 부팅 (virDomainCreate)
 *
 * [호출 시점] purecvisor_vm_manager_start_vm_async()에서 g_task_run_in_thread()로 위임
 * [동작] 1→virt_conn_pool에서 libvirt 연결 빌려옴 → 2→virDomainLookupByName으로 도메인 조회
 *        → 3→virDomainCreate()로 도메인 부팅 → 4→연결 반납
 * [스레드] GLib 스레드풀의 워커 스레드 (virDomainCreate는 블로킹 — QEMU 프로세스 기동까지 대기)
 * [주의] GVirConnection이 아닌 virt_conn_pool의 raw virConnectPtr를 사용합니다.
 *        GVirConnection은 자체 도메인 캐시를 가지므로 create_vm에서
 *        pool 경유로 define한 도메인을 찾지 못할 수 있습니다.
 *        virDomainCreate() 성공 후 finish()에서 "vm-started" 시그널이 emit됩니다.
 */
static void start_vm_thread_impl(GTask *task,
                                 gpointer source_object __attribute__((unused)),
                                 gpointer task_data,
                                 GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;

    /* GVirConnection 은 독립 캐시를 가지므로 virt_conn_pool 의 virConnectPtr 로
     * define 된 도메인을 찾지 못합니다. create_vm 과 동일하게 pool 경유 raw API
     * 를 사용해 조회합니다. */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (!dom) {
        virErrorPtr e = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", e ? e->message : data->name);
        virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainCreate(dom);
    if (rc != 0) {
        virErrorPtr e = virGetLastError();
        PCV_LOG_WARN("vm_manager", "virDomainCreate failed for '%s': %s",
                     data->name, e ? e->message : "unknown error");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM operation failed — check server logs for details");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/**
 * purecvisor_vm_manager_start_vm_async:
 * VM 시작 비동기 요청. 완료 시 콜백에서 start_vm_finish()를 호출합니다.
 *
 * @param self     vm_manager 인스턴스
 * @param name     시작할 VM 이름
 * @param callback 완료 콜백
 * @param user_data 콜백 사용자 데이터
 */
void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, start_vm_thread_impl);
    g_object_unref(task);
}

/**
 * purecvisor_vm_manager_start_vm_finish:
 * VM 시작 결과 회수 + vm-started 시그널 발신.
 * 성공 시 GIO P6 시그널을 emit하여 등록된 핸들러에 알림합니다.
 *
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {
        /* GIO P6: vm-started 신호 emit.
         * task_data 에 저장된 vm_name 을 꺼내 신호 파라미터로 전달합니다.
         * finish() 는 GAsyncReadyCallback (메인 스레드) 에서 호출되므로
         * g_signal_emit 이 안전합니다. */
        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STARTED], 0, data->name);
    }
    return ok;
}

/* --------------------------------------------------------------------------
 * [비동기 태스크] VM 정지 (Stop VM)
 * ACPI shutdown을 먼저 시도하고, 게스트가 응답하지 않으면 destroy로 강제 종료합니다.
 * -------------------------------------------------------------------------- */

/**
 * stop_vm_thread_impl — GTask 워커에서 VM 정지 (ACPI → 강제종료 폴백)
 *
 * [호출 시점] purecvisor_vm_manager_stop_vm_async()에서 g_task_run_in_thread()로 위임
 * [동작] 1→virDomainShutdown(ACPI 파워 버튼) 시도
 *        → 2→성공 시 최대 30초 폴링(1초 간격)으로 정상 종료 대기
 *        → 3→30초 내 미종료 시 virDomainDestroy(강제 kill)
 *        → 4→ACPI 자체가 실패하면 즉시 virDomainDestroy
 * [스레드] GLib 스레드풀의 워커 스레드 (최대 30초 블로킹 가능 — 폴링 루프)
 * [주의] virDomainShutdown은 비동기 — 호출 즉시 반환되며 VM이 실제로
 *        중지되기까지 수 초~수십 초 걸립니다. 따라서 폴링이 필요합니다.
 *        finish()에서 "vm-stopped" 시그널이 emit됩니다.
 */
static void stop_vm_thread_impl(GTask *task,
                                gpointer source_object __attribute__((unused)),
                                gpointer task_data,
                                GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (!dom) {
        virErrorPtr e = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", e ? e->message : data->name);
        virt_conn_pool_release(conn);
        return;
    }

    /* ── ACPI 셧다운 → 강제 종료 폴백 ────────────────────────────────────
     * virDomainShutdown: ACPI 파워 버튼 이벤트를 게스트에 전송
     *   → 게스트 OS가 정상 종료 절차(init 0, shutdown -h)를 수행
     *   → 게스트에 ACPI 드라이버가 없거나 hang 상태이면 실패 반환
     *
     * virDomainDestroy: QEMU 프로세스를 즉시 kill (SIGKILL 유사)
     *   → ACPI 실패 시 최후 수단으로 사용 (데이터 손실 가능)
     *
     * [주의] virDomainShutdown은 비동기 — 호출 즉시 반환되며
     * VM이 실제로 중지되기까지는 수 초~수십 초 걸릴 수 있습니다.
     * 이 함수에서는 중지 "요청" 자체가 성공하면 TRUE를 반환합니다. */
    int rc = virDomainShutdown(dom);
    if (rc != 0) {
        /* ACPI 셧다운 실패 — 즉시 강제 종료 */
        if (virDomainDestroy(dom) < 0) {
            PCV_LOG_WARN("vm_manager", "virDomainDestroy failed for '%s'", data->name);
            /* Still return TRUE to unblock caller, but log the failure */
        }
        goto stop_done;
    }

    /* ── 정상 종료 대기 — 최대 30초 폴링 (1초 간격) ──────────────────────
     * virDomainShutdown은 비동기 — 호출 즉시 반환되며 VM이 실제로
     * 중지되기까지 수 초~수십 초 걸릴 수 있습니다.
     * 게스트 OS가 정상 종료할 시간을 주고, 타임아웃 시 강제 종료합니다. */
    for (int poll_i = 0; poll_i < 30; poll_i++) {
        g_usleep(G_USEC_PER_SEC);
        int state = 0;
        if (virDomainGetState(dom, &state, NULL, 0) == 0) {
            if (state == VIR_DOMAIN_SHUTOFF) {
                PCV_LOG_INFO("vm_manager", "VM '%s' shut down gracefully after %ds",
                             data->name, poll_i + 1);
                goto stop_done;
            }
        }
    }
    PCV_LOG_WARN("vm_manager", "VM '%s' graceful shutdown timed out (30s) — force destroying",
                 data->name);
    if (virDomainDestroy(dom) < 0) {
        PCV_LOG_WARN("vm_manager", "virDomainDestroy (post-timeout) failed for '%s'", data->name);
    }

stop_done:
    /* 종료 여부와 관계없이 신호 emit — 중지 요청 자체는 성공 */
    g_task_return_boolean(task, TRUE);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/**
 * purecvisor_vm_manager_stop_vm_async:
 * VM 정지 비동기 요청. 완료 시 콜백에서 stop_vm_finish()를 호출합니다.
 */
void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *self,
                                         const gchar *name,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, stop_vm_thread_impl);
    g_object_unref(task);
}

/**
 * purecvisor_vm_manager_stop_vm_finish:
 * VM 정지 결과 회수 + vm-stopped 시그널 발신.
 *
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error) {
    gboolean ok = g_task_propagate_boolean(G_TASK(res), error);
    if (ok) {
        /* GIO P6: vm-stopped 신호 emit */
        LifecycleTaskData *data = g_task_get_task_data(G_TASK(res));
        g_signal_emit(manager, signals[SIGNAL_VM_STOPPED], 0, data->name);
    }
    return ok;
}

/* --------------------------------------------------------------------------
 * [비동기 태스크] VM 삭제 (Delete VM)
 *
 * 삭제 흐름 (2단계):
 *   1단계: virDomainDestroy(강제종료) + virDomainUndefine(정의 제거) → 즉시 RPC 응답
 *   2단계: 스토리지 삭제 → 별도 GTask에서 fire-and-forget
 *          - zvol 모드: zfs destroy (지수 백오프 재시도)
 *          - qcow2 모드: unlink 파일 삭제
 *
 * ZFS를 별도 스레드로 분리한 이유:
 *   - virDomainDestroy 직후 zvol이 EBUSY일 수 있음 (커널 release 지연)
 *   - 5GB+ 볼륨 삭제에 수십 초 소요 → RPC 소켓 타임아웃 방지
 * -------------------------------------------------------------------------- */
/* ── ZFS 백그라운드 삭제 헬퍼 ─────────────────────────────────────────────
 * virDomainUndefine 완료 즉시 RPC 응답을 보내고, ZFS destroy 는 별도 스레드에서
 * 비동기로 수행합니다. 5GB+ 볼륨은 수십 초가 걸리므로 소켓 타임아웃을 방지합니다.
 * ──────────────────────────────────────────────────────────────────────── */
/** ZFS 백그라운드 삭제 작업의 GTask 데이터 */
typedef struct {
    gchar *vm_name;   /* 삭제할 VM 이름 (ZFS 데이터셋 이름) */
} ZfsDestroyData;

static void _zfs_destroy_data_free(gpointer p) {
    ZfsDestroyData *d = p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d);
}

/* ZFS destroy 재시도 파라미터:
 * virDomainDestroy 직후 zvol 디바이스가 EBUSY 일 수 있음 → 지수 백오프 */
constexpr int ZFS_RETRY_MAX = 5;
static const guint ZFS_RETRY_MS[ZFS_RETRY_MAX] = {500, 1000, 2000, 4000, 8000};

/* ── VM 삭제 상태 추적 (비동기 ZFS/qcow2 삭제 진행 상황) ──────
 * vm.delete는 즉시 응답하지만 스토리지 삭제는 백그라운드에서 진행됩니다.
 * 이 해시테이블로 삭제 상태를 추적하고 vm.delete.status RPC로 조회합니다.
 *
 * 상태: "pending" → "deleting" → "done" / "failed"
 * 60초 후 자동 정리 (TTL)
 * ──────────────────────────────────────────────────────────── */
/* [스레드 안전성]
 *   GHashTable은 스레드-안전하지 않으므로 GMutex로 보호합니다.
 *   _delete_status_set()은 백그라운드 GTask에서,
 *   pcv_vm_delete_status_get()은 RPC 핸들러 스레드에서 호출됩니다.
 *   g_hash_table_replace: key가 이미 있으면 value만 교체 (메모리 자동 해제) */
static GHashTable *g_delete_status = nullptr;  /* key=vm_name, value=status_str */
static GMutex      g_delete_status_mu;

/** 삭제 상태를 업데이트하는 내부 헬퍼 (뮤텍스 보호) */
static void _delete_status_set(const gchar *vm, const gchar *status) {
    g_mutex_lock(&g_delete_status_mu);
    if (!g_delete_status)
        g_delete_status = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_replace(g_delete_status, g_strdup(vm), g_strdup(status));
    g_mutex_unlock(&g_delete_status_mu);
}

/**
 * pcv_vm_delete_status_get:
 * VM 삭제 작업의 현재 상태를 조회합니다.
 * vm.delete.status RPC 핸들러에서 호출됩니다.
 *
 * [반환 값]
 *   "pending":   삭제 요청됨, 아직 스토리지 삭제 시작 전
 *   "deleting":  스토리지 삭제 진행 중 (ZFS destroy 또는 qcow2 unlink)
 *   "done":      삭제 완료
 *   "failed":    삭제 실패 (ZFS EBUSY 재시도 모두 실패 등)
 *   "not_found": 해당 VM의 삭제 이력 없음
 *   "unknown":   vm 파라미터가 NULL
 *
 * @param vm VM 이름
 * @return 상태 문자열 (정적 또는 해시테이블 내부 — 복사 불필요, 변경 금지)
 */
const gchar *pcv_vm_delete_status_get(const gchar *vm) {
    if (!vm) return "unknown";
    g_mutex_lock(&g_delete_status_mu);
    const gchar *st = g_delete_status
        ? g_hash_table_lookup(g_delete_status, vm) : NULL;
    g_mutex_unlock(&g_delete_status_mu);
    return st ? st : "not_found";
}

/** 데몬 종료 시 호출 — g_delete_status 해시테이블 해제 */
void pcv_vm_manager_cleanup(void) {
    g_mutex_lock(&g_delete_status_mu);
    if (g_delete_status) {
        g_hash_table_destroy(g_delete_status);
        g_delete_status = nullptr;
    }
    g_mutex_unlock(&g_delete_status_mu);
}

/**
 * _zfs_destroy_thread — 백그라운드 ZFS zvol/qcow2 삭제 워커 (fire-and-forget)
 *
 * [호출 시점] delete_vm_thread_impl()에서 g_task_run_in_thread()로 fire-and-forget 실행
 * [동작] 1→qcow2 파일 존재 시 g_unlink로 삭제
 *        → 2→ZFS 데이터셋 존재 확인 (zfs list)
 *        → 3→purecvisor_zfs_destroy_volume()로 zvol 삭제 시도
 *        → 4→EBUSY 시 지수 백오프(500ms→1s→2s→4s→8s)로 최대 5회 재시도
 *        → 5→etcd에서 VM XML 제거 (클러스터 모드)
 * [스레드] GLib 스레드풀의 별도 워커 스레드 (delete_vm_thread_impl과 독립)
 * [주의] fire-and-forget: RPC 응답은 이미 전송됨. 결과는 g_delete_status로만 추적.
 *        EBUSY가 아닌 에러(권한 부족, 데이터셋 없음 등)는 재시도하지 않고 즉시 실패.
 *        메모리 해제: 이 함수 내부에서 ZfsDestroyData를 직접 해제합니다
 *        (GTask destroy notify 미등록).
 */
static void
_zfs_destroy_thread(GTask    *task __attribute__((unused)),
                    gpointer  source_object __attribute__((unused)),
                    gpointer  task_data,
                    GCancellable *cancellable __attribute__((unused)))
{
    ZfsDestroyData *d = (ZfsDestroyData *)task_data;
    GError *err = nullptr;
    _delete_status_set(d->vm_name, "deleting");

    /* qcow2 파일 디스크 확인 — 파일이 있으면 삭제 후 종료 */
    gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2",
                                         pcv_config_get_image_dir(), d->vm_name);
    if (g_file_test(qcow2_path, G_FILE_TEST_EXISTS)) {
        if (g_unlink(qcow2_path) == 0) {
            PCV_LOG_INFO("vm_manager",
                         "qcow2 disk removed: %s", qcow2_path);
        } else {
            PCV_LOG_WARN("vm_manager",
                         "qcow2 disk remove failed: %s", qcow2_path);
        }
        g_free(qcow2_path);
        /* Fall through to also check and delete zvol if both exist */
    } else {
        g_free(qcow2_path);
    }

    /* zvol 모드 — ZFS 데이터셋 삭제 */
    gchar *zfs_dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), d->vm_name);
    const gchar *check_argv[] = {"zfs", "list", "-H", "-o", "name",
                                 zfs_dataset, NULL};
    gboolean exists = pcv_spawn_sync(check_argv, NULL, NULL, NULL);

    if (!exists) {
        PCV_LOG_WARN("vm_manager",
                     "ZFS dataset '%s' not found — skipped", zfs_dataset);
        g_free(zfs_dataset);
        goto cleanup;
    }

    PCV_LOG_INFO("vm_manager", "ZFS destroy (bg): %s", zfs_dataset);

    /* ── 재시도 루프 ─────────────────────────────────────────────────
     * virDomainDestroy 직후 zvol 블록 디바이스 (/dev/zvol/…) 가
     * 커널 레벨에서 아직 release 되지 않아 EBUSY 가 반환될 수 있습니다.
     * 최대 5회, 지수 백오프(500ms → 1s → 2s → 4s → 8s) 로 재시도합니다.
     * ─────────────────────────────────────────────────────────────── */
    for (guint attempt = 0; attempt < ZFS_RETRY_MAX; attempt++) {

        if (attempt > 0) {
            PCV_LOG_INFO("vm_manager",
                         "ZFS destroy retry %u/%u for '%s' (wait %ums)",
                         attempt, ZFS_RETRY_MAX - 1,
                         zfs_dataset, ZFS_RETRY_MS[attempt - 1]);
            g_usleep((gulong)ZFS_RETRY_MS[attempt - 1] * 1000UL);
        }

        g_clear_error(&err);
        gboolean ok = purecvisor_zfs_destroy_volume(pcv_config_get_zvol_pool(),
                                                     d->vm_name, &err);
        if (ok) {
            PCV_LOG_INFO("vm_manager",
                         "ZFS dataset removed: %s (attempt %u)",
                         zfs_dataset, attempt + 1);
            goto zfs_cleanup;
        }

        /* EBUSY 가 아닌 다른 오류는 즉시 종료 */
        gboolean is_busy = err &&
            (strstr(err->message, "dataset is busy") != nullptr ||
             strstr(err->message, "busy")             != nullptr ||
             strstr(err->message, "EBUSY")            != nullptr);

        if (!is_busy) {
            PCV_LOG_WARN("vm_manager",
                         "ZFS destroy failed (non-retryable) for %s: %s",
                         zfs_dataset, err ? err->message : "unknown");
            goto zfs_cleanup;
        }

        PCV_LOG_WARN("vm_manager",
                     "ZFS destroy: device busy for '%s', will retry",
                     zfs_dataset);
    }

    /* 모든 재시도 소진 */
    PCV_LOG_WARN("vm_manager",
                 "ZFS destroy gave up after %u attempts for '%s': %s",
                 ZFS_RETRY_MAX, zfs_dataset,
                 err ? err->message : "unknown");

zfs_cleanup:
    g_free(zfs_dataset);
cleanup:
    /* 삭제 상태 업데이트 */
    _delete_status_set(d->vm_name, err ? "failed" : "done");
    /* etcd에서 VM XML 제거 */
#if PCV_CLUSTER_ENABLED
    pcv_cluster_remove_vm_xml(d->vm_name);
#endif
    if (err) g_error_free(err);
    /* d는 GTask의 task_data destroy notify(_zfs_destroy_data_free)가 해제 */
}

/**
 * delete_vm_thread_impl — GTask 워커에서 VM 삭제 (2단계 파이프라인)
 *
 * [호출 시점] purecvisor_vm_manager_delete_vm_async()에서 g_task_run_in_thread()로 위임
 * [동작] 1단계(즉시): virDomainDestroy(강제종료) + virDomainUndefineFlags(정의 제거)
 *        → 이 시점에서 g_task_return_boolean(TRUE)으로 RPC 응답 전송
 *        2단계(fire-and-forget): ZFS zvol/qcow2 삭제를 별도 GTask에서 비동기 실행
 *        → 최대 5회 지수 백오프 재시도 (500ms→1s→2s→4s→8s)
 * [스레드] 1단계: 현재 워커 스레드 / 2단계: 새 GTask 워커 스레드
 * [주의] fire-and-forget 패턴: 응답 먼저 전송 후 스토리지 삭제 비동기 실행.
 *        finish() TRUE 시점에 zvol이 아직 남아있을 수 있습니다.
 *        vm.delete.status RPC로 삭제 진행 상태를 조회할 수 있습니다.
 *        zvol 롤백의 원자성: virDomainDestroy 직후 zvol이 EBUSY일 수 있으므로
 *        지수 백오프로 재시도하여 커널 release 타이밍을 기다립니다.
 */
static void delete_vm_thread_impl(GTask *task,
                                  gpointer source_object __attribute__((unused)),
                                  gpointer task_data,
                                  GCancellable *cancellable __attribute__((unused))) {
    LifecycleTaskData *data = (LifecycleTaskData *)task_data;
    GError *err __attribute__((unused)) = nullptr;

    /* ── 1. libvirt 도메인 정지 + undefine ──────────────────────────────
     * start/stop 와 동일하게 virt_conn_pool 의 virConnectPtr 로 조회합니다.
     * GVirConnection 은 독립 캐시를 가지므로 create_vm 이 pool 경유로
     * define 한 도메인을 찾지 못하는 버그를 방지합니다.              */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, data->name);
    if (dom) {
        /* ── 1-1. 실행 중이면 강제 종료 ──────────────────────────────────
         * virDomainGetState로 현재 상태를 조회합니다.
         * RUNNING 또는 PAUSED 상태이면 virDomainDestroy로 강제 종료합니다.
         * virDomainDestroy: SIGKILL과 유사 — QEMU 프로세스를 즉시 종료
         * virDomainShutdown(ACPI)과 달리 게스트 OS 응답을 기다리지 않습니다.
         * best-effort: destroy 실패해도 undefine 계속 진행 (VM이 이미 중지된 경우) */
        virDomainState state = VIR_DOMAIN_NOSTATE;
        int reason = 0;
        virDomainGetState(dom, (int *)&state, &reason, 0);
        if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED) {
            virDomainDestroy(dom);   /* best-effort: 실패해도 진행 */
        }

        /* ── 1-2. 도메인 정의(undefine) 제거 ─────────────────────────────
         * VIR_DOMAIN_UNDEFINE_NVRAM: UEFI VM의 NVRAM 파일 자동 삭제
         * VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA: 스냅샷 메타데이터 정리
         * VIR_DOMAIN_UNDEFINE_REMOVE_ALL_STORAGE는 사용하지 않습니다.
         *   → 이 플래그는 libvirt managed storage pool에만 적용되며,
         *     ZFS zvol은 libvirt 관리 외부이므로 아래 ZFS 단계에서 직접 삭제합니다. */
        int rc = virDomainUndefineFlags(dom, VIR_DOMAIN_UNDEFINE_NVRAM |
                                             VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA);
        if (rc != 0) {
            /* snapshot metadata 플래그가 없는 구버전 libvirt 폴백 */
            rc = virDomainUndefine(dom);
        }
        virDomainFree(dom);

        if (rc != 0) {
            virErrorPtr e = virGetLastError();
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "virDomainUndefine failed: %s",
                                    e ? e->message : "unknown error");
            virt_conn_pool_release(conn);
            return;
        }
    }
    /* dom == NULL 이면 libvirt 에 등록되지 않은 VM — ZFS 정리는 계속 진행 */

    virt_conn_pool_release(conn);

    /* ── 2. ZFS dataset 삭제 (비동기, fire-and-forget) ──────────────────
     * virDomainUndefine 완료 후 즉시 TRUE 를 반환해 RPC 응답을 보냅니다.
     * ZFS destroy 는 별도 GTask 스레드에서 백그라운드로 실행됩니다.
     * 5GB+ 볼륨 삭제는 수십 초 소요 → 소켓 타임아웃을 유발하지 않습니다. */
    /* ── Fire-and-Forget 패턴 ────────────────────────────────────────────
     *
     * [핵심 개념]
     *   ZFS 삭제를 "발사 후 잊기" 방식으로 실행합니다.
     *   g_task_return_boolean(task, TRUE)을 먼저 호출하여 RPC 응답을 즉시 보내고,
     *   ZFS destroy는 별도 GTask에서 백그라운드로 실행합니다.
     *
     * [왜 이렇게 하는가?]
     *   1. ZFS destroy는 수 초~수십 초 걸릴 수 있음 → RPC 소켓 타임아웃 방지
     *   2. virDomainDestroy 직후 zvol이 EBUSY일 수 있음 → 재시도 필요
     *   3. 사용자 입장에서는 VM undefine이 핵심 — 스토리지 정리는 부수 작업
     *
     * [콜백 NULL, NULL]
     *   GTask 생성 시 callback=NULL, user_data=NULL입니다.
     *   결과를 받을 필요가 없기 때문입니다 (fire-and-forget).
     *   삭제 상태는 g_delete_status 해시테이블로 별도 추적합니다.
     *
     * [주의: destroy notify도 NULL]
     *   ZfsDestroyData의 메모리 해제는 _zfs_destroy_thread 내부에서 직접 합니다.
     *   (g_free(d->vm_name) + g_free(d))
     * ──────────────────────────────────────────────────────────────────── */
    _delete_status_set(data->name, "pending");
    ZfsDestroyData *zd = g_new0(ZfsDestroyData, 1);
    zd->vm_name = g_strdup(data->name);

    GTask *zfs_task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(zfs_task, zd, _zfs_destroy_data_free);
    g_task_run_in_thread(zfs_task, _zfs_destroy_thread);
    g_object_unref(zfs_task);

    g_task_return_boolean(task, TRUE);
}
/**
 * purecvisor_vm_manager_delete_vm_async:
 * VM 삭제 비동기 요청. 완료 시 콜백에서 delete_vm_finish()를 호출합니다.
 * 주의: ZFS 삭제는 별도 스레드에서 진행되므로 finish() TRUE 시점에 zvol이 아직 남아있을 수 있습니다.
 */
void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    LifecycleTaskData *data = g_new0(LifecycleTaskData, 1);
    data->manager = g_object_ref(self);
    data->name = g_strdup(name);

    g_task_set_task_data(task, data, (GDestroyNotify)lifecycle_task_data_free);
    g_task_run_in_thread(task, delete_vm_thread_impl);
    g_object_unref(task);
}

/** VM 삭제 결과 회수 — TRUE이면 libvirt undefine 성공 (ZFS 삭제는 비동기 진행 중) */
gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* --------------------------------------------------------------------------
 * [비동기 태스크] VM 목록 조회 (List VMs)
 *
 * 다른 작업(create/start/stop/delete)과 달리 GVirConnection을 사용합니다.
 * GVirConnection.fetch_domains()가 libvirt 도메인 캐시를 갱신한 후
 * get_domains()로 전체 목록을 가져옵니다.
 *
 * 각 VM에 대해 JSON 객체를 생성합니다:
 *   {"name", "uuid", "state", "vnc_port"}
 * -------------------------------------------------------------------------- */

/**
 * list_vms_thread — GTask 워커에서 전체 VM 목록을 JSON 배열로 조립
 *
 * [호출 시점] purecvisor_vm_manager_list_vms_async()에서 g_task_run_in_thread()로 위임
 * [동작] 1→GVirConnection.fetch_domains()로 libvirt 도메인 캐시 갱신
 *        → 2→get_domains()로 전체 도메인 리스트 획득
 *        → 3→각 도메인에서 name/uuid/state/vnc_port 추출하여 JSON 객체 생성
 *        → 4→JsonNode 배열로 결과 반환
 * [스레드] GLib 스레드풀의 워커 스레드 (fetch_domains가 블로킹 — libvirt 통신)
 * [주의] create/start/stop/delete와 달리 GVirConnection을 사용합니다.
 *        GVirConnection의 도메인 캐시 기반 열거가 list에서는 편리하기 때문입니다.
 *        VNC 포트는 실행 중인 VM만 Live XML에서 추출 가능합니다 (shutoff VM은 -1).
 */
static void list_vms_thread(GTask *task,
                            gpointer source_object __attribute__((unused)), 
                            gpointer task_data, 
                            GCancellable *cancellable __attribute__((unused))) {
    PureCVisorVmManager *self = PURECVISOR_VM_MANAGER(task_data);
    GList *domains, *l;
    JsonBuilder *builder = json_builder_new();
    GError *err = nullptr;

    json_builder_begin_array(builder);

    /* 1. GVirConnection 도메인 캐시 갱신
     * 실패해도 캐시된 데이터로 진행 (부분적 결과라도 반환) */
    if (!gvir_connection_fetch_domains(self->conn, NULL, &err)) {
        if (err) g_error_free(err);
    }

    /* 2. 캐시에서 전체 도메인 목록 획득 */
    domains = gvir_connection_get_domains(self->conn);

    /* 3. 각 도메인을 JSON 객체로 변환
     *
     * [GList 순회]
     *   GLib의 이중 연결 리스트. l->data에 GVirDomain* 포인터가 저장됩니다.
     *   l->next로 다음 노드로 이동합니다. NULL이면 리스트 끝입니다. */
    for (l = domains; l != nullptr; l = l->next) {
        GVirDomain *dom = GVIR_DOMAIN(l->data);
        const gchar *name = gvir_domain_get_name(dom);
        const gchar *uuid = gvir_domain_get_uuid(dom);

        /* VM 상태 판별: libvirt는 실행 중인 도메인에 양수 ID를 할당합니다.
         * dom_id > 0: 실행 중 (running)
         * dom_id < 0 또는 에러: 꺼져 있음 (shutoff)
         * 이 방식이 virDomainGetState()보다 가볍고 빠릅니다 (XML 파싱 불필요). */
        gint dom_id = gvir_domain_get_id(dom, NULL);

        const gchar *state_str = "shutoff";
        gboolean is_active = FALSE;

        if (dom_id > 0) {
            state_str = "running";
            is_active = TRUE;
        }

        /* VNC 포트 추출 — 실행 중인 VM만 Live XML에서 포트 확인 가능 */
        gint vnc_port = -1;
        if (is_active) {
            vnc_port = _extract_vnc_port_from_domain(dom);
        }

        /* JSON 객체 조립 */
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        
        json_builder_set_member_name(builder, "uuid");
        json_builder_add_string_value(builder, uuid);
        
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(builder, state_str);
        
        json_builder_set_member_name(builder, "vnc_port");
        if (vnc_port > 0) {
            json_builder_add_int_value(builder, vnc_port);
        } else {
            json_builder_add_null_value(builder);
        }
        json_builder_end_object(builder);
    }

    /* [GLib 메모리 관리: g_list_free_full]
     *   GList의 모든 노드를 순회하며 각 데이터에 g_object_unref를 호출한 후
     *   리스트 노드 자체도 해제합니다. GVirDomain은 GObject이므로
     *   g_object_unref로 참조 카운트를 감소시켜야 합니다. */
    g_list_free_full(domains, (GDestroyNotify)g_object_unref);
    json_builder_end_array(builder);

    /* JsonNode 루트 노드 추출 — GTask 결과로 반환
     *
     * [g_task_return_pointer의 세 번째 인자]
     *   destroy notify 함수입니다. GTask 결과를 소비하지 않으면
     *   (g_task_propagate_pointer를 호출하지 않으면) 이 함수로 자동 해제됩니다.
     *   정상적으로는 list_vms_finish에서 propagate하여 호출자가 소유권을 가집니다. */
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);

    g_task_return_pointer(task, root, (GDestroyNotify)json_node_free);
}

/**
 * purecvisor_vm_manager_list_vms_async:
 * VM 목록 조회 비동기 요청. self를 task_data로 전달 (ref 증가).
 */
void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
    GTask *task = g_task_new(self, NULL, callback, user_data);
    
    // We pass 'self' as task data to access connection in thread
    // Ref it to ensure safety
    g_task_set_task_data(task, g_object_ref(self), (GDestroyNotify)g_object_unref);
    
    g_task_run_in_thread(task, list_vms_thread);
    g_object_unref(task);
}

/**
 * purecvisor_vm_manager_list_vms_finish:
 * VM 목록 조회 결과 회수.
 *
 * @return JsonNode* — JSON 배열 (호출자가 json_node_free로 해제)
 */
JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager __attribute__((unused)),
                                                GAsyncResult *res,
                                                GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* =========================================================================
 * 런타임 리소스 튜닝 (vCPU / Memory 동적 조정)
 *
 * 실행 중인 VM의 CPU 수와 메모리 크기를 라이브로 변경합니다.
 * virDomainSetVcpusFlags / virDomainSetMemoryFlags를 사용하며
 * VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG 플래그로
 * 즉시 적용 + 영구 반영(재부팅 후에도 유지)을 동시에 수행합니다.
 * ========================================================================= */

/**
 * ResourceTuningData:
 * vCPU/메모리 튜닝 GTask의 공통 데이터 구조체.
 * vm_name과 target_value(CPU 수 또는 MB)를 전달합니다.
 */
typedef struct {
    gchar *vm_name;       /* 대상 VM 이름 */
    guint target_value;   /* 목표 값 (vCPU 수 또는 메모리 MB) */
} ResourceTuningData;

/** ResourceTuningData 메모리 해제 */
static void resource_tuning_data_free(ResourceTuningData *data) {
    if (data) {
        g_free(data->vm_name);
        g_free(data);
    }
}

/**
 * set_memory_thread_impl:
 * GTask 워커 스레드에서 VM 메모리를 동적 조정합니다.
 * target_value (MB) → KB로 변환 후 virDomainSetMemoryFlags 호출.
 *
 * LIVE + CONFIG 플래그: 즉시 적용 + 재부팅 후에도 유지
 */
static void set_memory_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;

    /* 1. 연결 풀에서 libvirt 커넥션 획득 */
    virConnectPtr raw_conn = virt_conn_pool_acquire();
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }

    /* 2. VM 도메인 객체 조회 */
    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virt_conn_pool_release(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }

    /* 3. 동적 메모리 조절 — MB를 KB로 변환 (libvirt는 KB 단위)
     *
     * [VIR_DOMAIN_AFFECT_LIVE]
     *   실행 중인 VM에 즉시 적용 (QEMU의 balloon 디바이스를 통해 게스트에 통지).
     *   게스트 OS에 virtio-balloon 드라이버가 있어야 실제 메모리가 조정됩니다.
     *
     * [VIR_DOMAIN_AFFECT_CONFIG]
     *   영구 설정(XML)에도 반영하여 VM 재부팅 후에도 변경이 유지됩니다.
     *   두 플래그를 OR 연산으로 결합하면 즉시 적용 + 영구 반영이 동시에 됩니다.
     *
     * [제한 사항]
     *   현재 최대 메모리(maxMemory) 이하로만 설정 가능합니다.
     *   초과하면 libvirt가 에러를 반환합니다.
     *   최대 메모리 확장은 VM 재부팅이 필요합니다. */
    guint memory_kb = data->target_value * 1024;
    int ret = virDomainSetMemoryFlags(raw_domain, memory_kb, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    
    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, 
                                "Memory tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    /* 4. 자원 해제 */
    virDomainFree(raw_domain);
    virt_conn_pool_release(raw_conn);
}

/**
 * purecvisor_vm_manager_set_memory_async:
 * VM 메모리 동적 조정 비동기 요청.
 *
 * @param memory_mb 목표 메모리 크기 (MB)
 */
void purecvisor_vm_manager_set_memory_async(PureCVisorVmManager *self, const gchar *name, guint memory_mb, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = memory_mb;
    
    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_memory_thread_impl);
    g_object_unref(task);
}

/** 메모리 튜닝 결과 회수 */
gboolean purecvisor_vm_manager_set_memory_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/**
 * set_vcpu_thread_impl:
 * GTask 워커 스레드에서 VM vCPU 수를 동적 조정합니다.
 * virDomainSetVcpusFlags로 LIVE + CONFIG 동시 적용.
 *
 * 주의: 현재 vCPU 최대값(maxvcpu) 이하로만 설정 가능합니다.
 * 최대값 초과 시 libvirt가 에러를 반환합니다.
 */
static void set_vcpu_thread_impl(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    ResourceTuningData *data = (ResourceTuningData *)task_data;

    /* 1. 연결 풀에서 libvirt 커넥션 획득 */
    virConnectPtr raw_conn = virt_conn_pool_acquire();
    if (!raw_conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open raw libvirt connection");
        return;
    }

    virDomainPtr raw_domain = virDomainLookupByName(raw_conn, data->vm_name);
    if (!raw_domain) {
        virt_conn_pool_release(raw_conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM '%s' not found", data->vm_name);
        return;
    }

    /* 2. vCPU 개수 동적 조절 (LIVE + CONFIG) */
    int ret = virDomainSetVcpusFlags(raw_domain, data->target_value, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    
    if (ret < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, 
                                "vCPU tuning failed: %s", vir_err ? vir_err->message : "Unknown error");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(raw_domain);
    virt_conn_pool_release(raw_conn);
}

/**
 * purecvisor_vm_manager_set_vcpu_async:
 * VM vCPU 수 동적 조정 비동기 요청.
 *
 * @param vcpu_count 목표 vCPU 수
 */
void purecvisor_vm_manager_set_vcpu_async(PureCVisorVmManager *self, const gchar *name, guint vcpu_count, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ResourceTuningData *data = g_new0(ResourceTuningData, 1);
    data->vm_name = g_strdup(name);
    data->target_value = vcpu_count;
    
    g_task_set_task_data(task, data, (GDestroyNotify)resource_tuning_data_free);
    g_task_run_in_thread(task, set_vcpu_thread_impl);
    g_object_unref(task);
}

/** vCPU 튜닝 결과 회수 */
gboolean purecvisor_vm_manager_set_vcpu_finish(PureCVisorVmManager *self, GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* --------------------------------------------------------------------------
 * VM 디스크 라이브 리사이즈
 *
 * 실행 중인 VM의 블록 디바이스 크기를 변경합니다.
 * ZFS zvol이면 zfs set volsize, qcow2이면 qemu-img resize 후
 * virDomainBlockResize로 게스트에 크기 변경을 통지합니다.
 * -------------------------------------------------------------------------- */

/** 디스크 리사이즈 GTask 데이터 */
typedef struct {
    gchar   *name;          /* VM 이름 */
    gint     new_size_gb;   /* 새 디스크 크기 (GB) */
    gchar   *target;        /* 블록 디바이스 타겟 (예: "vda") */
    gboolean holds_lock;    /* [CMP-10류] 호출자가 VM_OP_TUNING 락 획득했는지 */
} ResizeDiskData;

static void resize_disk_data_free(ResizeDiskData *d) {
    /* [락 하드닝] 단일 해제 지점: fire-and-forget이라 콜백이 없어 GDestroyNotify가
     * 성공·실패 모든 경로의 유일 종착지. holds_lock=TRUE(핸들러가 획득)일 때만 해제
     * → 이중해제/무해제 없이 acquire:unlock 1:1. unlock은 d->name을 읽으므로 free 앞. */
    if (d->holds_lock) unlock_vm_operation(d->name);
    g_free(d->name);
    g_free(d->target);
    g_free(d);
}

static void
audit_resize_disk_success(ResizeDiskData *d)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(NULL, "vm.resize_disk", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.resize_disk",
                                     "completed", NULL);
    g_free(job_id);
    g_free(target);
}

static void
audit_resize_disk_failure(ResizeDiskData *d, const gchar *error_msg)
{
    gchar *target = g_strdup_printf("%s:%s", d->name, d->target ? d->target : "vda");
    gchar *job_id = g_strdup_printf("vm.resize_disk:%s", target);
    pcv_audit_log(NULL, "vm.resize_disk", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.resize_disk",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

/**
 * resize_disk_thread:
 * GTask 워커 스레드에서 디스크 리사이즈를 수행합니다.
 *
 * 단계:
 *   1. VM이 존재하는지 확인
 *   2. VM XML에서 디스크 소스 경로를 파싱
 *   3. ZFS zvol이면 zfs set volsize=<size>G, qcow2이면 qemu-img resize
 *   4. virDomainBlockResize로 게스트에 크기 변경 통지 (VM 실행 중일 때만)
 */
static void resize_disk_thread(GTask *task, gpointer source_object __attribute__((unused)),
                                gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    ResizeDiskData *d = (ResizeDiskData *)task_data;
    GError *error = nullptr;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        audit_resize_disk_failure(d, "Failed to acquire libvirt connection");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, d->name);
    if (!dom) {
        gchar *msg = g_strdup_printf("VM '%s' not found", d->name);
        audit_resize_disk_failure(d, msg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "VM '%s' not found", d->name);
        virt_conn_pool_release(conn);
        g_free(msg);
        return;
    }

    /* ── VM XML에서 디스크 소스 경로 파악 ────────────────────────────────
     *
     * [왜 XML 파싱이 필요한가?]
     *   VM의 디스크가 ZFS zvol인지 qcow2 파일인지는 VM XML에서만 확인 가능합니다.
     *   virDomainGetXMLDesc(dom, 0)은 현재 설정의 XML 문자열을 반환합니다.
     *
     * [디스크 유형 판별]
     *   zvol:  <source dev='/dev/zvol/pcvpool/vms/name'/> → 블록 디바이스
     *   qcow2: <source file='/var/lib/libvirt/images/name.qcow2'/> → 파일
     *
     * [리사이즈 방법 분기]
     *   zvol:  zfs set volsize=<N>G (ZFS 속성 변경, 즉시 적용)
     *   qcow2: qemu-img resize (파일 크기 확장)
     *   이후 virDomainBlockResize로 게스트에 크기 변경을 통지합니다. */
    gchar *xml = virDomainGetXMLDesc(dom, 0);
    gboolean is_zvol = FALSE;
    gchar *disk_source = nullptr;

    if (xml) {
        /* ZFS zvol: <source dev='/dev/zvol/...'/>  */
        gchar *dev_tag = strstr(xml, "<source dev='");
        if (dev_tag) {
            dev_tag += strlen("<source dev='");
            gchar *end = strchr(dev_tag, '\'');
            if (end) {
                disk_source = g_strndup(dev_tag, (gsize)(end - dev_tag));
                is_zvol = g_str_has_prefix(disk_source, "/dev/zvol/") ||
                          g_str_has_prefix(disk_source, "/dev/zd");
            }
        }
        /* qcow2: <source file='/path/to/disk.qcow2'/> */
        if (!disk_source) {
            gchar *file_tag = strstr(xml, "<source file='");
            if (file_tag) {
                file_tag += strlen("<source file='");
                gchar *end = strchr(file_tag, '\'');
                if (end) {
                    disk_source = g_strndup(file_tag, (gsize)(end - file_tag));
                    is_zvol = FALSE;
                }
            }
        }
        g_free(xml);
    }

    if (!disk_source) {
        gchar *msg = g_strdup_printf("Cannot determine disk source for VM '%s'", d->name);
        audit_resize_disk_failure(d, msg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Cannot determine disk source for VM '%s'", d->name);
        g_free(msg);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    /* 스토리지 레벨 리사이즈 */
    if (is_zvol) {
        /* ZFS zvol: /dev/zvol/pcvpool/vms/name -> pcvpool/vms/name */
        const gchar *dataset = disk_source + strlen("/dev/zvol/");
        gchar *size_str = g_strdup_printf("%dG", d->new_size_gb);
        gchar *prop_str = g_strdup_printf("volsize=%s", size_str);
        const gchar *argv[] = {"zfs", "set", prop_str, (gchar *)dataset, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            const gchar *err_msg = error ? error->message : (std_err ? std_err : "unknown");
            gchar *msg = g_strdup_printf("zfs set volsize failed: %s", err_msg);
            audit_resize_disk_failure(d, msg);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "zfs set volsize failed: %s",
                err_msg);
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(prop_str);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_free(msg);
            return;
        }
        g_free(std_err);
        g_free(size_str);
        g_free(prop_str);
    } else {
        /* qcow2: qemu-img resize */
        gchar *size_str = g_strdup_printf("%dG", d->new_size_gb);
        const gchar *argv[] = {"qemu-img", "resize", disk_source, size_str, NULL};
        gchar *std_err = nullptr;

        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            const gchar *err_msg = error ? error->message : (std_err ? std_err : "unknown");
            gchar *msg = g_strdup_printf("qemu-img resize failed: %s", err_msg);
            audit_resize_disk_failure(d, msg);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                "qemu-img resize failed: %s",
                err_msg);
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(disk_source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_free(msg);
            return;
        }
        g_free(std_err);
        g_free(size_str);
    }

    /* ── 게스트에 크기 변경 통지 (실행 중일 때만) ───────────────────────
     *
     * [virDomainBlockResize의 역할]
     *   스토리지 레벨에서 디스크 크기를 변경한 후, QEMU에 블록 디바이스
     *   크기가 변경되었음을 통지합니다. QEMU가 게스트 OS에 SCSI sense를
     *   전달하면 게스트에서 디스크 크기 변경을 감지할 수 있습니다.
     *   게스트에서 파티션 확장은 별도로 수행해야 합니다 (growpart 등).
     *
     * [target 파라미터]
     *   VM XML의 <target dev='vda'/> 에 해당하는 블록 디바이스 이름입니다.
     *   일반적으로 첫 번째 디스크는 "vda"입니다.
     *
     * [실패 시 처리]
     *   스토리지 리사이즈는 이미 성공했으므로 BlockResize 실패는
     *   에러로 반환하지 않고 경고만 남깁니다.
     *   VM 재부팅 시 자동으로 새 크기를 인식합니다. */
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) == 0 && info.state == VIR_DOMAIN_RUNNING) {
        const gchar *target = d->target ? d->target : "vda";
        unsigned long long new_size_kb = (unsigned long long)d->new_size_gb * 1024ULL * 1024ULL;
        int rc = virDomainBlockResize(dom, target, new_size_kb, 0);
        if (rc < 0) {
            virErrorPtr e = virGetLastError();
            PCV_LOG_WARN("vm_manager", "virDomainBlockResize failed for '%s': %s",
                         d->name, e ? e->message : "unknown");
            /* 스토리지 리사이즈는 성공했으므로 에러로 반환하지 않고 경고만 남김 */
        }
    }

    PCV_LOG_INFO("vm_manager", "VM '%s': disk resized to %dG (%s)",
                  d->name, d->new_size_gb, is_zvol ? "zvol" : "qcow2");

    g_free(disk_source);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
    audit_resize_disk_success(d);
    g_task_return_boolean(task, TRUE);
}

/**
 * purecvisor_vm_resize_disk:
 * VM 디스크 리사이즈를 fire-and-forget으로 실행합니다.
 * 디스패처에서 응답 전송 후 호출됩니다.
 */
void purecvisor_vm_resize_disk(const gchar *name, gint new_size_gb, const gchar *target,
                                gboolean holds_lock) {
    ResizeDiskData *d = g_new0(ResizeDiskData, 1);
    d->name = g_strdup(name);
    d->new_size_gb = new_size_gb;
    d->target = target ? g_strdup(target) : g_strdup("vda");
    d->holds_lock = holds_lock;   /* 핸들러가 VM_OP_TUNING을 획득했으면 여기서 소유권 인수 */

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)resize_disk_data_free);
    g_task_run_in_thread(task, resize_disk_thread);
    g_object_unref(task);
}

/* --------------------------------------------------------------------------
 * [GIO P6] Public emit helper — vm-metrics-updated
 *
 * 텔레메트리 데몬이 메인 스레드에서 g_main_context_invoke 콜백 내부에서
 * 이 함수를 호출합니다.  메인 스레드 전용이므로 g_signal_emit 이 안전합니다.
 * -------------------------------------------------------------------------- */
/**
 * purecvisor_vm_manager_emit_metrics_updated:
 * 텔레메트리 데몬이 새 메트릭 캐시를 스왑한 후 이 함수를 호출합니다.
 *
 * [호출 컨텍스트]
 *   telemetry.c → g_main_context_invoke() → 이 함수 (메인 스레드)
 *   g_signal_emit은 메인 스레드에서만 안전하므로
 *   g_main_context_invoke로 메인 스레드로 전환한 후 호출됩니다.
 *
 * [g_return_if_fail 매크로]
 *   GLib의 방어적 프로그래밍 매크로입니다.
 *   self가 PureCVisorVmManager 타입이 아니면 경고를 출력하고 즉시 반환합니다.
 *   릴리즈 빌드에서도 동작합니다 (assert와 달리 프로세스를 종료하지 않음).
 *
 * @param cache GHashTable* — VmMetrics 값의 해시테이블 (소유권 이전 없음)
 */
void
purecvisor_vm_manager_emit_metrics_updated(PureCVisorVmManager *self,
                                           GHashTable          *cache)
{
    g_return_if_fail(PURECVISOR_IS_VM_MANAGER(self));
    g_signal_emit(self, signals[SIGNAL_VM_METRICS_UPDATED], 0, cache);
}
