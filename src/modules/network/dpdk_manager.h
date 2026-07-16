/**
 * @file dpdk_manager.h
 * @brief OVS-DPDK 매니저 공개 인터페이스 -- 커널 바이패스 가속
 *
 * ====================================================================
 * [역할]
 *   handler_accel.c 에서 dpdk.* RPC 7개 처리 시 호출.
 *   DPDK 기반 OVS 데이터플레인을 관리하여 고성능 네트워크 제공.
 *
 * [DPDK 기본 개념 (주니어 참고)]
 *   DPDK(Data Plane Development Kit)는 Intel이 개발한
 *   커널 바이패스 네트워크 프레임워크이다.
 *
 *   [왜 커널 바이패스인가?]
 *     일반적인 패킷 처리 경로:
 *       NIC -> 커널 인터럽트 -> 커널 네트워크 스택 -> 사용자 공간
 *     매 패킷마다 컨텍스트 스위칭과 메모리 복사가 발생하여 느리다.
 *
 *     DPDK 경로:
 *       NIC -> DPDK PMD(사용자 공간에서 직접 폴링) -> 애플리케이션
 *     커널을 완전히 우회하여 수백만 pps(packets per second) 처리 가능.
 *
 *   [Hugepage]
 *     DPDK는 TLB 미스를 줄이기 위해 2MB 또는 1GB 대형 페이지를 사용.
 *     최소 2GB(2MB x 1024) 할당 필요.
 *     /sys/kernel/mm/hugepages/ 에서 확인 가능.
 *
 *   [PMD (Poll Mode Driver)]
 *     인터럽트 대신 busy-polling으로 NIC에서 패킷을 가져온다.
 *     전용 CPU 코어를 소비하지만 지연(latency)이 매우 낮다.
 *     pmd-cpu-mask로 PMD에 할당할 CPU 코어를 지정.
 *
 *   [vhost-user]
 *     VM과 DPDK 간 데이터 교환을 위한 Unix 소켓 인터페이스.
 *     VM XML에서 <interface type='vhostuser'>로 소켓 경로를 지정.
 *     소켓 경로: /var/run/purecvisor/vhost-<vm_name>.sock
 *
 * [호출 흐름]
 *   handler_accel.c (dpdk.* RPC) --> pcv_dpdk_*()
 *     --> dpdk-devbind.py (NIC 바인딩)
 *     --> ovs-vsctl (DPDK 브릿지 생성, datapath_type=netdev)
 *   VM 연결: vhost-user 소켓 (/var/run/purecvisor/vhost-<vm>.sock)
 *
 * [함수 분류]
 *   Lifecycle : init(dpdk-init 확인), shutdown, is_available
 *   Status    : status(OVS DPDK 상태), hugepage_info(할당 현황)
 *   NIC 바인딩: bind(DPDK 드라이버), unbind(커널 복원), list(목록)
 *   브릿지    : bridge_create/delete (datapath_type=netdev)
 *   소켓      : vhost_socket_path (VM별 소켓 경로 생성)
 *
 * [전제 조건]
 *   - OVS dpdk-init=true 설정:
 *     ovs-vsctl set Open_vSwitch . other_config:dpdk-init=true
 *   - hugepage 할당 (2MB x 1024 이상 권장):
 *     echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
 *   - dpdk-devbind.py (dpdk-tools 패키지)
 *
 * [Graceful Degradation]
 *   DPDK 미설치/미설정 시:
 *     is_available() -> FALSE
 *     status()       -> available=false 포함 JSON
 *     bind/bridge_create -> GError 반환
 *     list()         -> 빈 배열
 *
 * [주의]
 *   관리 NIC(eno1)을 DPDK에 바인딩하면 SSH 접속 불가.
 *   bind 전에 반드시 대상 NIC이 관리용이 아닌지 확인할 것.
 * ====================================================================
 */
#ifndef PURECVISOR_DPDK_MANAGER_H
#define PURECVISOR_DPDK_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * OVS-DPDK Manager -- 커널 바이패스 데이터플레인 관리
 *
 * Phase 4: OVS-DPDK 가속 브릿지, hugepage 관리, PMD 설정
 *
 * 내부적으로 ovs-vsctl, dpdk-devbind.py 명령을 실행합니다.
 * Graceful degradation: DPDK 미설치 시 available=false, 에러 반환
 */

/* ---- Lifecycle ---- */
void     pcv_dpdk_init(void);           /* OVS dpdk-init=true 여부 확인 */
void     pcv_dpdk_shutdown(void);       /* 뮤텍스 해제 */
gboolean pcv_dpdk_is_available(void);   /* DPDK 가용 여부 */

/* ---- Status ---- */
JsonObject *pcv_dpdk_status(void);          /* available, vdev_count, pmd_cpu_mask, socket_mem */
JsonObject *pcv_dpdk_hugepage_info(void);   /* 1G/2M hugepage 총수/여유수/크기(MB) */

/* ---- NIC binding ---- */
gboolean    pcv_dpdk_bind(const gchar *pci_addr, const gchar *driver, GError **error);
gboolean    pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
JsonArray  *pcv_dpdk_list(void);            /* DPDK 바인딩된 디바이스 목록 */

/* NET-1: pci_addr가 호스트 사용 중 NIC(UP+IPv4 또는 기본경로)이면 TRUE(+reason,
 * 호출자 g_free). NULL/빈=TRUE(fail-secure). 형식검증(pcv_validate_pci_addr) 실패=
 * TRUE(fail-secure, sysfs 탐침 없이 거부). net 디렉토리 없음=FALSE(통과).
 * getifaddrs 실패=TRUE(fail-secure) — 이 경우 reason은 "interface enumeration
 * failed ... (fail-secure)"로, 실제 UP+IPv4 매치와 구분해 오귀속을 방지한다
 * (NET-1 M1). */
gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason);
/* 테스트 주입용: netdev가 <proc_base>/proc/net/route 기본경로 dev인지. 프로덕션 proc_base="". */
gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base);

/* ---- DPDK-accelerated OVS bridge ----
 *
 * [datapath_type=netdev]
 *   일반 OVS 브릿지는 커널 데이터패스를 사용하지만,
 *   datapath_type=netdev 설정 시 DPDK 사용자공간 데이터패스로 전환된다.
 *   이것이 DPDK 가속의 핵심 설정이다. */
gboolean pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port, GError **error);
gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);

/* ---- vhost-user socket for VM attachment ---- */
/**
 * pcv_dpdk_vhost_socket_path -- VM별 vhost-user 소켓 경로 생성
 * @vm_name: VM 이름
 * @return (transfer full): "/var/run/purecvisor/vhost-<vm_name>.sock"
 *         호출자가 g_free() 해야 함. vm_name=NULL이면 NULL 반환.
 */
gchar *pcv_dpdk_vhost_socket_path(const gchar *vm_name);

G_END_DECLS

#endif /* PURECVISOR_DPDK_MANAGER_H */
