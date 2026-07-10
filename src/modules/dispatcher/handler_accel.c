/**
 * @file handler_accel.c
 * @brief 네트워크 성능 가속 RPC 핸들러 — OVS-DPDK + SR-IOV (14개 메서드)
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c ("dpdk.*","sriov.*") -> handle_dpdk/sriov_*()
 *                                                                    -> dpdk_manager.c (DPDK CLI 래퍼)
 *                                                                    -> sriov_manager.c (sysfs + ip link)
 *
 * [섹션 1] OVS-DPDK (7개 RPC)
 *   dpdk.status        -> handle_dpdk_status        : DPDK 런타임 상태 (hugepage, PMD, vdev)
 *   dpdk.bind          -> handle_dpdk_bind          : NIC을 DPDK 드라이버(vfio-pci)에 바인딩
 *   dpdk.unbind        -> handle_dpdk_unbind        : NIC을 커널 드라이버로 복원
 *   dpdk.list          -> handle_dpdk_list          : DPDK 바인딩 디바이스 목록
 *   dpdk.bridge.create -> handle_dpdk_bridge_create : DPDK 가속 OVS 브릿지 생성
 *   dpdk.bridge.delete -> handle_dpdk_bridge_delete : DPDK 브릿지 삭제
 *   dpdk.hugepage.info -> handle_dpdk_hugepage_info : hugepage 할당 현황 (NUMA 노드별)
 *
 * [섹션 2] SR-IOV (7개 RPC)
 *   sriov.status  -> handle_sriov_status  : SR-IOV 지원 PF 목록 + VF 현황
 *   sriov.enable  -> handle_sriov_enable  : PF에 VF 생성 (sysfs sriov_numvfs 쓰기)
 *   sriov.disable -> handle_sriov_disable : PF의 모든 VF 해제
 *   sriov.list    -> handle_sriov_list    : VF 목록 (PCI 주소, 드라이버, MAC)
 *   sriov.set     -> handle_sriov_set     : VF 속성 설정 (MAC/VLAN/spoofchk)
 *   sriov.attach  -> handle_sriov_attach  : VM에 VF PCI passthrough 연결 (vfio-pci 바인딩 + virsh)
 *   sriov.detach  -> handle_sriov_detach  : VM에서 VF 분리 (virsh detach-device)
 *
 * [fire-and-forget 패턴 미사용]
 *   모든 핸들러가 동기 응답입니다.
 *   DPDK/SR-IOV 설정 명령이 즉시 완료됩니다.
 *
 * [주의사항]
 *   - sriov.attach는 두 단계로 동작합니다:
 *     a. VF를 vfio-pci 드라이버에 바인딩 (PCI passthrough 준비)
 *     b. virsh attach-device로 VM에 연결
 *   - DPDK 사용 시 hugepage가 사전에 할당되어 있어야 합니다.
 *   - dpdk.bridge.create는 OVS에 datapath_type=netdev로 DPDK 브릿지를 생성합니다.
 *
 * [에러 코드]
 *   -32602 (PURE_RPC_ERR_INVALID_PARAMS) :
 *       필수 파라미터 누락 (pci_addr, pf, vm_name 등)
 *   -32000 (PURE_RPC_ERR_INTERNAL_ERROR) :
 *       외부 명령(dpdk-devbind, ip link, virsh) 실행 실패
 *
 * [이 파일의 핸들러 코드 패턴]
 *   14개 핸들러 모두 동일한 구조를 따릅니다:
 *   1. 필수 파라미터 존재 확인 (json_object_has_member)
 *   2. 파라미터 추출 (json_object_get_string/int_member)
 *   3. 매니저 함수 호출 (pcv_dpdk_* 또는 pcv_sriov_*)
 *   4. 실패 시: error_response 전송 + GError 해제
 *   5. 성공 시: JsonObject 결과 빌드 → success_response 전송
 *
 * [코드 스타일 참고]
 *   이 파일은 핸들러가 14개로 많아 변수명을 짧게 사용합니다:
 *     p=params, id=rpc_id, s=server, c=connection, r=resp, n=node
 *   파일 상단의 핸들러 시그니처와 매핑하여 읽으면 됩니다.
 */

/*
 * --- 헤더 인클루드 설명 ---
 *
 * handler_accel.h  : 이 파일의 공개 함수 선언 (14개 핸들러)
 * dpdk_manager.h   : DPDK 코어 모듈 — pcv_dpdk_status/bind/unbind/list/
 *                    bridge_create/bridge_delete/hugepage_info 함수
 *                    내부적으로 dpdk-devbind.py 스크립트와 ovs-vsctl 명령을 실행
 * sriov_manager.h  : SR-IOV 코어 모듈 — pcv_sriov_status/enable/disable/list/
 *                    set/attach_vm/detach_vm 함수
 *                    내부적으로 sysfs 파일 쓰기와 ip link, virsh 명령을 실행
 * rpc_utils.h      : JSON-RPC 응답 빌더 + 에러코드 상수 + UDS 전송 함수
 */
#include "handler_accel.h"
#include "modules/network/dpdk_manager.h"
#include "modules/network/sriov_manager.h"
#include "modules/dispatcher/rpc_utils.h"
#include "purecvisor/pcv_validate.h"

/* ══════════════════════════════════════════════════════════════
 * OVS-DPDK 핸들러
 *
 * DPDK (Data Plane Development Kit)는 커널 바이패스를 통한 고성능 패킷 처리 라이브러리.
 * OVS와 결합하여 userspace 네트워크 가속을 제공합니다.
 *
 * DPDK 사용 전 필수 조건:
 *   1. hugepage 사전 할당 (dpdk.hugepage.info로 확인)
 *   2. NIC을 DPDK 호환 드라이버(vfio-pci)에 바인딩 (dpdk.bind)
 *   3. OVS에 datapath_type=netdev 브릿지 생성 (dpdk.bridge.create)
 * ══════════════════════════════════════════════════════════════ */

/**
 * handle_dpdk_status:
 * @p: 사용하지 않음 (빈 객체 허용)
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * DPDK 런타임 상태 조회: hugepage 할당 현황, PMD(Poll Mode Driver) 스레드, vdev 목록 등.
 * pcv_dpdk_status()가 JsonObject를 직접 반환 → 소유권 node에 이전.
 */
void handle_dpdk_status(JsonObject *p __attribute__((unused)), const gchar *id,
                         UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_dpdk_status();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_dpdk_hugepage_info:
 * @p: 사용하지 않음
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * hugepage 할당 현황 조회 (NUMA 노드별 2MB/1GB 페이지 수).
 * DPDK는 hugepage 위에서 메모리 관리를 수행하므로,
 * DPDK 사용 전 충분한 hugepage가 할당되었는지 확인하는 용도.
 */
void handle_dpdk_hugepage_info(JsonObject *p __attribute__((unused)), const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_dpdk_hugepage_info();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_dpdk_bind:
 * @p: {"pci_addr", "driver"?} — pci_addr 필수, driver 선택 (기본: vfio-pci)
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * NIC을 DPDK 호환 드라이버에 바인딩합니다.
 * dpdk-devbind.py --bind <driver> <pci_addr> 실행.
 *
 * [주의] 바인딩 후 해당 NIC은 커널 네트워크 스택에서 사라집니다.
 *   - 관리용 NIC(eno1)에 실행하면 원격 접속 불가!
 *   - 전용 데이터 NIC에만 사용하세요.
 *
 * 성공 시 반환: {"status":"bound", "pci_addr":"..."}
 */
void handle_dpdk_bind(JsonObject *p, const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

    /*
     * driver: DPDK 호환 드라이버 이름. 미지정 시 dpdk_manager 내부에서
     * "vfio-pci"를 기본값으로 사용합니다.
     * 사용 가능한 드라이버: vfio-pci (권장, IOMMU 필요), igb_uio (레거시)
     */
    const gchar *drv = json_object_has_member(p, "driver")
        ? json_object_get_string_member(p, "driver") : NULL;

    /*
     * pcv_dpdk_bind: dpdk-devbind.py --bind <driver> <pci_addr> 실행.
     * 내부적으로 pcv_spawn_sync()를 사용하여 argv 배열로 프로세스를 실행합니다
     * (Command Injection 방어 — system()/popen() 사용하지 않음).
     */
    GError *err = NULL;
    if (!pcv_dpdk_bind(pci, drv, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "dpdk bind failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "bound");
    json_object_set_string_member(res, "pci_addr", pci);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_dpdk_unbind:
 * @p: {"pci_addr"} — 필수
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * NIC을 커널 드라이버로 복원합니다 (DPDK 바인딩 해제).
 * dpdk-devbind.py --unbind <pci_addr> 실행 후 원래 드라이버로 재바인딩.
 *
 * 성공 시 반환: {"status":"unbound", "pci_addr":"..."}
 */
void handle_dpdk_unbind(JsonObject *p, const gchar *id,
                         UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

    GError *err = NULL;
    if (!pcv_dpdk_unbind(pci, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "dpdk unbind failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "unbound");
    json_object_set_string_member(res, "pci_addr", pci);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_dpdk_list:
 * @p: 사용하지 않음
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * DPDK에 바인딩된 디바이스 목록을 JSON 배열로 반환합니다.
 * dpdk-devbind.py --status 결과를 파싱하여 PCI 주소, 드라이버, 인터페이스명 포함.
 */
void handle_dpdk_list(JsonObject *p __attribute__((unused)), const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    JsonArray *arr = pcv_dpdk_list();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_dpdk_bridge_create:
 * @p: {"name", "dpdk_port"?} — name 필수, dpdk_port 선택
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * DPDK 가속 OVS 브릿지를 생성합니다.
 * ovs-vsctl add-br <name> -- set bridge <name> datapath_type=netdev 실행.
 * dpdk_port 지정 시 DPDK vhost-user 포트도 함께 추가합니다.
 *
 * [디자인 결정] datapath_type=netdev는 OVS가 DPDK를 통해
 *   userspace에서 직접 패킷을 처리하게 하는 설정입니다.
 *   일반 OVS 브릿지(system datapath)와 달리 커널 바이패스가 적용됩니다.
 *
 * 성공 시 반환: {"status":"created", "name":"..."}
 */
void handle_dpdk_bridge_create(JsonObject *p, const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "name")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *name = json_object_get_string_member(p, "name");

    /* 입력 검증 (defense-in-depth) — 매니저 계층에서도 재검증됨 */
    if (!pcv_validate_bridge_name(name)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    /*
     * dpdk_port: DPDK vhost-user 포트 이름. 미지정 시 브릿지만 생성.
     * vhost-user 포트는 DPDK 앱(VM 등)이 소켓을 통해 연결하는 가상 NIC.
     */
    const gchar *port = json_object_has_member(p, "dpdk_port")
        ? json_object_get_string_member(p, "dpdk_port") : NULL;

    if (port && *port && !pcv_validate_pci_addr(port)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: dpdk_port");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    /*
     * pcv_dpdk_bridge_create 내부 실행 명령:
     *   1. ovs-vsctl add-br <name> -- set bridge <name> datapath_type=netdev
     *      → datapath_type=netdev: OVS가 커널 모듈 대신 DPDK userspace로 패킷 처리
     *      → 일반 OVS 브릿지(system datapath)와의 핵심 차이점
     *   2. (dpdk_port 지정 시) ovs-vsctl add-port <name> <dpdk_port>
     *      -- set Interface <dpdk_port> type=dpdk options:dpdk-devargs=<pci_addr>
     */
    GError *err = NULL;
    if (!pcv_dpdk_bridge_create(name, port, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "dpdk bridge create failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "name", name);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_dpdk_bridge_delete:
 * @p: {"name"} — 필수
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * DPDK OVS 브릿지를 삭제합니다 (ovs-vsctl del-br).
 * 브릿지에 연결된 모든 포트도 함께 제거됩니다.
 *
 * 성공 시 반환: {"status":"deleted"}
 */
void handle_dpdk_bridge_delete(JsonObject *p, const gchar *id,
                                UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "name")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *name = json_object_get_string_member(p, "name");

    GError *err = NULL;
    if (!pcv_dpdk_bridge_delete(name, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "dpdk bridge delete failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/* ══════════════════════════════════════════════════════════════
 * SR-IOV 핸들러
 *
 * SR-IOV (Single Root I/O Virtualization)는 하나의 물리 NIC(PF, Physical Function)을
 * 여러 가상 NIC(VF, Virtual Function)으로 분할하여 VM에 직접 할당하는 하드웨어 가상화 기술.
 *
 * 성능 이점: VM이 하이퍼바이저를 거치지 않고 NIC에 직접 접근하므로
 *   virtio보다 훨씬 낮은 지연시간과 높은 처리량을 제공합니다.
 *
 * 전형적인 사용 흐름:
 *   1. sriov.enable  → PF에 VF 생성 (sysfs sriov_numvfs 쓰기)
 *   2. sriov.set     → VF의 MAC/VLAN/spoofchk 설정
 *   3. sriov.attach  → VF를 vfio-pci에 바인딩 + VM에 PCI passthrough 연결
 *   4. (VM 삭제 시) sriov.detach → VM에서 VF 분리
 *   5. sriov.disable → 모든 VF 해제 (PF 복원)
 * ══════════════════════════════════════════════════════════════ */

/**
 * handle_sriov_status:
 * @p: 사용하지 않음
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * SR-IOV 지원 PF(Physical Function) 목록과 각 PF의 VF 현황을 조회합니다.
 * sysfs /sys/class/net/{dev}/device/sriov_totalvfs 등을 읽어 정보를 수집합니다.
 */
void handle_sriov_status(JsonObject *p __attribute__((unused)), const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    JsonObject *obj = pcv_sriov_status();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, obj);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_sriov_enable:
 * @p: {"pf", "num_vfs"?} — pf 필수, num_vfs 선택 (기본: 1)
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * PF에 VF(Virtual Function)를 생성합니다.
 * /sys/class/net/<pf>/device/sriov_numvfs 에 num_vfs 값을 쓰는 방식.
 *
 * [주의] VF 수는 PF 하드웨어 한계(sriov_totalvfs) 이하여야 합니다.
 *   초과 시 커널이 거부하며 pcv_sriov_enable()이 GError를 반환합니다.
 *
 * 성공 시 반환: {"status":"enabled", "pf":"...", "num_vfs":N}
 */
void handle_sriov_enable(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pf = json_object_get_string_member(p, "pf");

    /*
     * num_vfs: 생성할 VF(Virtual Function) 수. 기본값 1.
     * PF 하드웨어 한계(sriov_totalvfs)를 초과하면 커널이 거부합니다.
     * 일반적으로 Intel NIC은 최대 64개, Mellanox는 127개까지 지원.
     *
     * json_object_get_int_member()는 gint64를 반환하므로 (gint)로 캐스트합니다.
     * VF 수는 양의 정수이므로 범위 초과 위험은 없습니다.
     */
    gint num = json_object_has_member(p, "num_vfs")
        ? (gint)json_object_get_int_member(p, "num_vfs") : 1;

    /*
     * pcv_sriov_enable 내부:
     *   /sys/class/net/<pf>/device/sriov_numvfs 에 num 값을 쓰기 (echo N > sysfs)
     *   커널이 PCI 디바이스를 재설정하고 VF PCI 디바이스를 생성합니다.
     *   lspci 에서 새 VF가 보이기까지 수 초 소요될 수 있습니다.
     */
    GError *err = NULL;
    if (!pcv_sriov_enable(pf, num, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "sriov enable failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "enabled");
    json_object_set_string_member(res, "pf", pf);
    json_object_set_int_member(res, "num_vfs", num);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_sriov_disable:
 * @p: {"pf"} — 필수
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * PF의 모든 VF를 해제합니다 (sriov_numvfs=0 쓰기).
 *
 * [주의] VM에 연결된 VF가 있는 상태에서 disable 하면
 *   VM 내부에서 NIC이 사라집니다. 먼저 sriov.detach 를 수행하세요.
 *
 * 성공 시 반환: {"status":"disabled", "pf":"..."}
 */
void handle_sriov_disable(JsonObject *p, const gchar *id,
                           UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    const gchar *pf = json_object_get_string_member(p, "pf");

    /* 입력 검증 (defense-in-depth) */
    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_sriov_disable(pf, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "sriov disable failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "disabled");
    json_object_set_string_member(res, "pf", pf);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_sriov_list:
 * @p: {"pf"?} — pf 선택 (NULL이면 모든 PF의 VF 목록)
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * VF 목록을 JSON 배열로 반환합니다.
 * pf 지정 시 해당 PF의 VF만, 미지정 시 전체 VF를 포함합니다.
 * 각 VF 항목: PCI 주소, 현재 바인딩 드라이버, MAC 주소, VLAN 등.
 */
void handle_sriov_list(JsonObject *p, const gchar *id,
                        UdsServer *s, GSocketConnection *c)
{
    const gchar *pf = json_object_has_member(p, "pf")
        ? json_object_get_string_member(p, "pf") : NULL;

    JsonArray *arr = pcv_sriov_list(pf);
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_sriov_set:
 * @p: {"pf", "vf_index", "mac"?, "vlan"?, "spoofchk"?} — pf, vf_index 필수
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * VF의 속성을 설정합니다:
 *   - mac      : VF의 MAC 주소 변경 (ip link set <pf> vf <idx> mac <addr>)
 *   - vlan     : VF의 VLAN 태깅 (ip link set <pf> vf <idx> vlan <id>)
 *   - spoofchk : MAC 스푸핑 방지 (0=off, 1=on) — 보안 강화용
 *
 * 값이 -1인 선택 파라미터는 무시됩니다 (현재 설정 유지).
 *
 * 성공 시 반환: {"status":"configured"}
 */
void handle_sriov_set(JsonObject *p, const gchar *id,
                       UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "pf") || !json_object_has_member(p, "vf_index")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: pf, vf_index");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *pf = json_object_get_string_member(p, "pf");
    gint vf_idx = (gint)json_object_get_int_member(p, "vf_index");

    /* 입력 검증 (defense-in-depth) */
    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    /*
     * 선택적 파라미터 — 지정하지 않으면 현재 설정을 유지합니다.
     *
     * mac: VF의 MAC 주소. 미지정(NULL) 시 변경하지 않음.
     *   실행 명령: ip link set <pf> vf <idx> mac <addr>
     *
     * vlan: VF의 VLAN 태깅. -1이면 변경하지 않음. 0이면 VLAN 해제.
     *   실행 명령: ip link set <pf> vf <idx> vlan <id>
     *
     * spoofchk: MAC 스푸핑 방지. -1이면 변경하지 않음.
     *   0=off(VM이 임의 MAC 사용 가능), 1=on(보안 강화)
     *   실행 명령: ip link set <pf> vf <idx> spoofchk on|off
     *
     * pcv_sriov_set() 내부에서 -1인 파라미터는 건너뜁니다 (선택적 적용).
     */
    const gchar *mac = json_object_has_member(p, "mac")
        ? json_object_get_string_member(p, "mac") : NULL;
    gint vlan = json_object_has_member(p, "vlan")
        ? (gint)json_object_get_int_member(p, "vlan") : -1;
    gint spoof = json_object_has_member(p, "spoofchk")
        ? (gint)json_object_get_int_member(p, "spoofchk") : -1;

    if (mac && !pcv_validate_mac(mac)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: mac");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    GError *err = NULL;
    if (!pcv_sriov_set(pf, vf_idx, mac, vlan, spoof, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "sriov set failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "configured");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_sriov_attach:
 * @p: {"vm_name", "pf", "vf_index"?} — vm_name, pf 필수, vf_index 선택 (기본: 0)
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * VM에 VF를 PCI passthrough로 연결합니다.
 *
 * [2단계 동작]
 *   a. VF를 vfio-pci 드라이버에 바인딩 (PCI passthrough 준비)
 *      → echo <vf_pci_addr> > /sys/bus/pci/drivers/vfio-pci/bind
 *   b. virsh attach-device <vm_name> 으로 VM에 PCI 디바이스 추가
 *      → 핫플러그로 VM 재시작 없이 NIC 추가
 *
 * [성능 장점] VM이 하이퍼바이저를 경유하지 않고 NIC에 직접 접근하여
 *   virtio 대비 10~30% 낮은 지연시간, 더 높은 패킷 처리량을 달성합니다.
 *
 * 성공 시 반환: {"status":"attached", "vm_name":"...", "pf":"...", "vf_index":N}
 */
void handle_sriov_attach(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "vm_name") || !json_object_has_member(p, "pf")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: vm_name, pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *vm = json_object_get_string_member(p, "vm_name");
    const gchar *pf = json_object_get_string_member(p, "pf");

    /* 입력 검증 (defense-in-depth) */
    if (!pcv_validate_vm_name(vm)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: vm_name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    if (!pcv_validate_iface_name(pf)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: pf");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    /*
     * vf_index: PF 내에서의 VF 인덱스 (0부터 시작). 기본값 0 (첫 번째 VF).
     * 예: sriov.enable으로 4개 VF를 생성했다면 vf_index는 0~3 범위.
     */
    gint vf_idx = json_object_has_member(p, "vf_index")
        ? (gint)json_object_get_int_member(p, "vf_index") : 0;

    /*
     * pcv_sriov_attach_vm 내부 2단계 동작:
     *
     * [a단계] VF를 vfio-pci 드라이버에 바인딩 (PCI passthrough 준비)
     *   - VF의 PCI 주소를 sysfs에서 조회 (/sys/class/net/<pf>/device/virtfnN)
     *   - 현재 커널 드라이버에서 언바인딩 (echo <addr> > /sys/bus/pci/drivers/<drv>/unbind)
     *   - vfio-pci에 바인딩 (echo <addr> > /sys/bus/pci/drivers/vfio-pci/bind)
     *   - IOMMU 그룹 확인 (/sys/bus/pci/devices/<addr>/iommu_group)
     *
     * [b단계] virsh attach-device로 VM에 PCI 디바이스 추가 (핫플러그)
     *   - <hostdev mode='subsystem' type='pci'> XML 생성
     *   - virsh attach-device <vm_name> <xml> --live 실행
     *   - VM 재시작 없이 즉시 NIC이 추가됨 (게스트 OS에서 새 네트워크 인터페이스 감지)
     */
    GError *err = NULL;
    if (!pcv_sriov_attach_vm(vm, pf, vf_idx, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "sriov attach failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "attached");
    json_object_set_string_member(res, "vm_name", vm);
    json_object_set_string_member(res, "pf", pf);
    json_object_set_int_member(res, "vf_index", vf_idx);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}

/**
 * handle_sriov_detach:
 * @p: {"vm_name", "pci_addr"} — 둘 다 필수
 * @id: JSON-RPC 요청 ID
 * @s: UDS 서버 인스턴스
 * @c: 클라이언트 소켓 연결
 *
 * VM에서 VF PCI 디바이스를 분리합니다.
 * virsh detach-device <vm_name> <device_xml> 실행.
 *
 * [주의] 분리 후 VF는 vfio-pci에 바인딩된 상태로 남습니다.
 *   커널 드라이버로 복원하려면 별도로 드라이버 재바인딩이 필요합니다.
 *
 * 성공 시 반환: {"status":"detached"}
 */
void handle_sriov_detach(JsonObject *p, const gchar *id,
                          UdsServer *s, GSocketConnection *c)
{
    if (!json_object_has_member(p, "vm_name") || !json_object_has_member(p, "pci_addr")) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Missing: vm_name, pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    const gchar *vm = json_object_get_string_member(p, "vm_name");
    const gchar *pci = json_object_get_string_member(p, "pci_addr");

    /* 입력 검증 (defense-in-depth) */
    if (!pcv_validate_vm_name(vm)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: vm_name");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }
    if (!pcv_validate_pci_addr(pci)) {
        gchar *r = pure_rpc_build_error_response(id, -32602, "Invalid: pci_addr");
        pure_uds_server_send_response(s, c, r); g_free(r); return;
    }

    /*
     * pcv_sriov_detach_vm: virsh detach-device <vm_name> <device_xml> --live 실행.
     *
     * 분리 후 VF는 vfio-pci에 바인딩된 상태로 남습니다.
     * 커널 드라이버로 복원하려면 별도로 드라이버 재바인딩이 필요합니다:
     *   echo <pci_addr> > /sys/bus/pci/drivers/vfio-pci/unbind
     *   echo <pci_addr> > /sys/bus/pci/drivers/<original_drv>/bind
     * 또는 sriov.disable로 VF 자체를 해제할 수 있습니다.
     */
    GError *err = NULL;
    if (!pcv_sriov_detach_vm(vm, pci, &err)) {
        gchar *r = pure_rpc_build_error_response(id, -32000,
            err ? err->message : "sriov detach failed");
        pure_uds_server_send_response(s, c, r);
        g_free(r); if (err) g_error_free(err); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "detached");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(id, n);
    pure_uds_server_send_response(s, c, r);
    g_free(r);
}
