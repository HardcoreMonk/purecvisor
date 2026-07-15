/**
 * @file handler_monitor.c
 * @brief 호스트/VM 모니터링 RPC 핸들러 — 단일 VM 메트릭 + 전체 Fleet 대시보드
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c -> handle_monitor_*()
 *                                              -> libvirt virConnectGetAllDomainStats
 *                                              -> /proc/meminfo, /proc/stat (호스트 메트릭)
 *                                              -> sysinfo() (커널 직접 조회)
 *
 * [처리하는 RPC 메서드] (2개)
 *   monitor.metrics -> handle_monitor_metrics : 단일 VM의 상세 CPU/메모리/디스크/네트워크 메트릭
 *   monitor.fleet   -> handle_monitor_fleet   : 전체 호스트 + 모든 VM 종합 대시보드
 *     - Prometheus /metrics 엔드포인트(REST)가 이 데이터를 text format으로 노출합니다.
 *     - Web UI 대시보드도 이 RPC를 호출합니다.
 *
 * [fire-and-forget 패턴 미사용]
 *   모든 메서드가 동기 응답입니다.
 *   libvirt stats API + /proc 파싱이 빠르게 완료됩니다.
 *
 * [주의사항]
 *   - 호스트 물리 메트릭은 /proc/meminfo, /proc/stat, sysinfo()에서 직접 수집합니다.
 *   - 디스크 크기 조회 시 블록 디바이스 lseek(SEEK_END) 또는 stat()을 사용합니다.
 *   - monitor.fleet 응답은 Prometheus exporter와 Web UI 양쪽에서 사용되므로
 *     필드 추가/삭제 시 양쪽 호환성을 확인해야 합니다.
 *
 * [에러 코드]
 *   -32602 : monitor.metrics의 필수 파라미터(vm_id) 누락
 *   -32000 : libvirt 연결 실패
 */
#include "rpc_utils.h"
#include <gio/gio.h>
#include "api/uds_server.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"
#include "modules/daemons/telemetry.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>
#include <sys/stat.h>  /* stat() for disk size */
#include <fcntl.h>       /* open/close for block device size */
#include <unistd.h>      /* lseek/close */
#include <ctype.h>
/* 리눅스 커널에서 직접 물리 자원을 수집하기 위한 헤더 */
#include <sys/sysinfo.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/statvfs.h> /* statvfs(): 물리 디스크 파티션 용량 조회 */

/**
 * silent_libvirt_error_func:
 * @userdata: 사용하지 않음
 * @err: libvirt 에러 구조체 (무시됨)
 *
 * libvirt의 기본 에러 핸들러를 대체하는 더미 함수.
 * virSetErrorFunc()에 등록하여 libvirt 내부 에러 메시지가
 * stderr에 출력되는 것을 방지합니다.
 *
 * [디자인 결정]
 *   monitor.fleet는 모든 VM의 통계를 한 번에 조회하므로,
 *   일시적으로 꺼진 VM에 대한 libvirt 에러가 대량으로 발생할 수 있습니다.
 *   이런 노이즈 로그를 억제하기 위해 음소거 핸들러를 사용합니다.
 */
static void silent_libvirt_error_func(void *userdata, virErrorPtr err) {
    /* 의도적으로 아무것도 출력하지 않음 — 에러 로그 스팸 방지 */
    (void)userdata; (void)err;
}

static gboolean
skip_proc_lines(FILE *stream, char *buf, gsize buf_size, guint lines)
{
    for (guint i = 0; i < lines; i++) {
        if (!fgets(buf, (int)buf_size, stream))
            return FALSE;
    }
    return TRUE;
}

/*
 * 라이프사이클 모듈(handler_vm_lifecycle.c)에서 제공하는 다형성 도메인 검색기.
 * identifier가 UUID 형식이면 UUID로, 아니면 이름으로 libvirt 도메인을 조회합니다.
 */
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

/**
 * handle_monitor_metrics:
 * @params: JSON-RPC params — {"vm_id": "VM이름 또는 UUID"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 단일 VM의 실시간 메트릭을 조회합니다.
 *
 * [반환 필드]
 *   state       : VM 상태 문자열 (RUNNING/PAUSED/SHUTOFF/...)
 *   vcpu        : 할당된 vCPU 수
 *   mem_max_mb  : 최대 메모리 (MB)
 *   mem_used_mb : 현재 사용 메모리 (MB) — balloon 기반
 *   cpu_time_ns : 누적 CPU 시간 (나노초) — delta로 사용률 계산 가능
 *
 * [의존성] libvirt virDomainGetInfo API 사용.
 *   virt_conn_pool 에서 커넥션을 빌려와 사용 후 반환합니다.
 *
 * [에러 코드]
 *   -32602 : vm_id 파라미터 누락
 *   -32000 : VM을 찾을 수 없거나 libvirt API 실패
 */
void handle_monitor_metrics(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing parameter: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    /* libvirt API 호출: VM의 실시간 메트릭(상태, vCPU, 메모리, CPU시간)을 구조체로 수집 */
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Failed to get metrics");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    /* libvirt 도메인 상태 열거형 → 사람이 읽을 수 있는 문자열 매핑 */
    const gchar *state_str = "UNKNOWN";
    switch (info.state) {
        case VIR_DOMAIN_RUNNING: state_str = "RUNNING"; break;
        case VIR_DOMAIN_BLOCKED: state_str = "BLOCKED"; break;
        case VIR_DOMAIN_PAUSED:  state_str = "PAUSED"; break;
        case VIR_DOMAIN_SHUTDOWN:state_str = "SHUTDOWN"; break;
        case VIR_DOMAIN_SHUTOFF: state_str = "SHUTOFF"; break;
        case VIR_DOMAIN_CRASHED: state_str = "CRASHED"; break;
    }

    /* 수집된 메트릭을 JSON 객체로 조립하여 응답 전송 */
    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    
    json_object_set_string_member(res_obj, "state", state_str);
    json_object_set_int_member(res_obj, "vcpu", info.nrVirtCpu);
    json_object_set_double_member(res_obj, "mem_max_mb", info.maxMem / 1024.0);
    json_object_set_double_member(res_obj, "mem_used_mb", info.memory / 1024.0);
    json_object_set_int_member(res_obj, "cpu_time_ns", info.cpuTime);

    /* Per-VM 네트워크 대역폭 메트릭: telemetry.c 캐시에서 UUID 기반 O(1) 조회
     * handle_monitor_metrics는 메인 스레드에서 동기 실행되므로 get_vm_metrics 호출 안전 */
    {
        extern VmMetrics* get_vm_metrics(const gchar *vm_id);
        char uuid_buf[VIR_UUID_STRING_BUFLEN];
        if (virDomainGetUUIDString(dom, uuid_buf) == 0) {
            VmMetrics *net = get_vm_metrics(uuid_buf);
            if (net) {
                JsonObject *net_obj = json_object_new();
                json_object_set_int_member(net_obj, "rx_bytes",   (gint64)net->rx_bytes);
                json_object_set_int_member(net_obj, "tx_bytes",   (gint64)net->tx_bytes);
                json_object_set_int_member(net_obj, "rx_packets", (gint64)net->rx_packets);
                json_object_set_int_member(net_obj, "tx_packets", (gint64)net->tx_packets);
                json_object_set_int_member(net_obj, "rx_errs",    (gint64)net->rx_errs);
                json_object_set_int_member(net_obj, "tx_errs",    (gint64)net->tx_errs);
                json_object_set_int_member(net_obj, "rx_drop",    (gint64)net->rx_drop);
                json_object_set_int_member(net_obj, "tx_drop",    (gint64)net->tx_drop);
                json_object_set_object_member(res_obj, "net", net_obj);
            }
        }
    }

    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}


/* =================================================================
 * monitor.fleet — 전체 호스트 + 모든 VM 종합 대시보드
 * =================================================================*/

/**
 * handle_monitor_fleet:
 * @params: 사용하지 않음 (빈 객체 허용)
 * @error: 실패 시 GError** 에 에러 정보 설정
 *
 * 호스트 물리 자원과 모든 VM 메트릭을 한 번에 수집하여 JSON 문자열로 반환합니다.
 *
 * [다른 핸들러와의 차이점]
 *   일반 핸들러는 (params, rpc_id, server, connection) 시그니처를 사용하지만,
 *   이 함수는 gchar* 를 직접 반환합니다. 이는 monitor.fleet가 여러 경로에서
 *   호출되기 때문입니다:
 *     1. RPC 디스패처 (dispatcher.c) → JSON-RPC 2.0 응답으로 래핑
 *     2. REST /api/v1/metrics (Prometheus exporter) → text format 변환
 *     3. Web UI 대시보드 → 직접 파싱
 *   따라서 JSON-RPC 래핑 없이 raw JSON을 반환하여 호출자가 용도에 맞게 사용합니다.
 *
 * [반환 JSON 구조]
 *   {
 *     "jsonrpc": "2.0", "id": "monitor-fleet",
 *     "result": {
 *       "fleet": [ { VM 메트릭 배열 } ],
 *       "host":  { 호스트 물리 메트릭 }
 *     }
 *   }
 *
 * [fleet 배열 각 VM 객체 필드]
 *   name, state, ip, mac, net_source, net_model, disk_path, disk_size, disk_bus,
 *   cdrom_path, vnc_port, uuid, vcpu, cpu_time_ns, mem_used_mb, mem_max_mb,
 *   disk_rd_bytes, disk_wr_bytes, net_rx_bytes, net_tx_bytes, autostart, persistent
 *
 * [host 객체 필드 (WhaTap W-1 확장 포함)]
 *   CPU:  cpus, cpu_model, cpu_total/idle/user/nice/system/iowait/irq/softirq/steal_ticks,
 *         cores[] (코어별 동일 구조), cpu_temp_c
 *   MEM:  mem_total/used/avail/free_gb, mem_percent, mem_buffers/cached_mb,
 *         swap_total/used_gb, mem_slab/sreclaimable_mb
 *   DISK: disk_total/used_gb, disk_percent, disk_rd/wr_bytes, disk_rd/wr_ios, disk_io_ticks_ms
 *   NET:  net_iface, net_rx/tx_bytes, net_rx/tx_packets, net_rx/tx_errs, net_rx/tx_drop
 *   기타: uptime_secs, load_1/5/15
 *
 * [IP 추출 3단계 폴백]
 *   1. libvirt DHCP lease (NAT/internal 네트워크)
 *   2. qemu-guest-agent (게스트 에이전트 설치 시)
 *   3. ARP 캐시 스캔 (bridge 모드 — MAC 기반 매핑)
 *
 * [디스크 크기 조회]
 *   일반 파일: stat().st_size
 *   블록 디바이스(zvol, LVM): lseek(SEEK_END) — st_size=0 이므로 직접 탐색
 *
 * @returns: JSON 문자열 (호출자가 g_free 해야 함), 실패 시 NULL + error 설정
 */
gchar *handle_monitor_fleet(JsonObject *params, GError **error) {
    /* libvirt 에러 로그 음소거 — 꺼진 VM 조회 시 발생하는 대량 에러 메시지 억제 */
    virSetErrorFunc(NULL, silent_libvirt_error_func);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Libvirt connection failed.");
        return NULL;
    }

    virDomainStatsRecordPtr *stats = NULL;
    unsigned int stats_flags = VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL | VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_BALLOON | VIR_DOMAIN_STATS_BLOCK | VIR_DOMAIN_STATS_INTERFACE;

    int count = virConnectGetAllDomainStats(conn, stats_flags, &stats, 0);
    if (count < 0) {
        virt_conn_pool_release(conn);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to fetch fleet stats.");
        return NULL;
    }

    JsonArray *fleet_array = json_array_new();

    for (int i = 0; i < count; i++) {
        virDomainStatsRecordPtr rec = stats[i];
        JsonObject *vm_obj = json_object_new();
        
        json_object_set_string_member(vm_obj, "name", virDomainGetName(rec->dom));

        /*
         * [디자인 결정] virConnectGetAllDomainStats의 배열 통계 대신
         * virDomainGetState()로 개별 VM 상태를 직접 조회합니다.
         * 배열 통계는 SHUTOFF 상태의 VM에서 부정확한 값을 반환할 수 있기 때문.
         */
        int state = VIR_DOMAIN_NOSTATE;
        int reason = 0;
        virDomainGetState(rec->dom, &state, &reason, 0);

        unsigned int vcpu = 0;
        unsigned long long cpu_time_ns = 0;
        unsigned long long mem_curr = 0, mem_max = 0, mem_unused = 0, mem_usable = 0;
        unsigned long long rd_bytes = 0, wr_bytes = 0, rx_bytes = 0, tx_bytes = 0;
        int has_balloon_unused = 0;
        int has_balloon_usable = 0;

        for (int j = 0; j < rec->nparams; j++) {
            virTypedParameterPtr p = &rec->params[j];
            if      (strcmp(p->field, "vcpu.current")    == 0) vcpu       = p->value.ui;
            else if (strcmp(p->field, "cpu.time")         == 0) cpu_time_ns = p->value.ul;
            else if (strcmp(p->field, "balloon.current") == 0) mem_curr   = p->value.ul;
            else if (strcmp(p->field, "balloon.maximum") == 0) mem_max    = p->value.ul;
            else if (strcmp(p->field, "balloon.unused")  == 0) { mem_unused = p->value.ul; has_balloon_unused = 1; }
            else if (strcmp(p->field, "balloon.usable")  == 0) { mem_usable = p->value.ul; has_balloon_usable = 1; }
            else if (strstr(p->field, ".rd.bytes")) rd_bytes += p->value.ul;
            else if (strstr(p->field, ".wr.bytes")) wr_bytes += p->value.ul;
            else if (strstr(p->field, ".rx.bytes")) rx_bytes += p->value.ul;
            else if (strstr(p->field, ".tx.bytes")) tx_bytes += p->value.ul;
        }

        // balloon.usable은 guest가 swap 없이 돌려줄 수 있는 메모리라 page cache 오탐을 줄인다.
        // 없으면 unused로 폴백하고, 둘 다 없으면 balloon 드라이버 미설치 → -1 로 "N/A" 표시
        double mem_used_mb = has_balloon_usable
            ? (double)(mem_curr > mem_usable ? mem_curr - mem_usable : 0) / 1024.0
            : has_balloon_unused
            ? (double)(mem_curr > mem_unused ? mem_curr - mem_unused : 0) / 1024.0
            : -1.0;

        /*
         * IP 주소 추출 — RUNNING 상태의 VM에서만 시도합니다.
         * SHUTOFF VM에 IP 조회를 시도하면 libvirt가 에러를 반환하므로 성능 낭비 방지.
         * 3단계 폴백: (1) DHCP lease → (2) guest agent → (3) ARP 캐시
         */
        char vm_ip[64] = "N/A";
        if (state == VIR_DOMAIN_RUNNING) {
            virDomainInterfacePtr *ifaces = NULL;

            // 1단계: libvirt DHCP lease (NAT/internal 네트워크)
            int iface_cnt = virDomainInterfaceAddresses(rec->dom, &ifaces,
                                VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE, 0);

            // 2단계: qemu-guest-agent (guest agent 설치된 경우)
            if (iface_cnt <= 0)
                iface_cnt = virDomainInterfaceAddresses(rec->dom, &ifaces,
                                VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT, 0);

            if (iface_cnt > 0 && ifaces) {
                // loopback(lo) 제외하고 첫 번째 유효 IPv4 추출
                for (int k = 0; k < iface_cnt && vm_ip[0] == 'N'; k++) {
                    if (!ifaces[k]) continue;
                    if (g_strcmp0(ifaces[k]->name, "lo") == 0) continue;
                    for (unsigned int a = 0; a < ifaces[k]->naddrs; a++) {
                        if (ifaces[k]->addrs[a].type == VIR_IP_ADDR_TYPE_IPV4) {
                            strncpy(vm_ip, ifaces[k]->addrs[a].addr, sizeof(vm_ip)-1);
                            break;
                        }
                    }
                }
                for (int k = 0; k < iface_cnt; k++)
                    if (ifaces[k]) virDomainInterfaceFree(ifaces[k]);
                free(ifaces);
            }

            /* 3단계: ARP 캐시 스캔 (bridge 모드 — MAC 기반 매핑)
             * LEASE/AGENT 모두 실패한 경우에만 시도.
             * [주의] popen("arp -n")은 command injection 위험이 없는 고정 명령이지만,
             * pcv_spawn_sync()를 쓰지 않는 이유: arp 출력을 행 단위로 스트리밍 파싱하기 위함.
             * 입력 데이터(mac_lower)는 libvirt XML에서 추출되어 사용자 입력이 아님 */
            if (g_strcmp0(vm_ip, "N/A") == 0) {
                char mac_lower[18] = {0};
                // mac_addr는 아래 XML 파싱 전이므로 XML에서 직접 추출
                char *xml_tmp = virDomainGetXMLDesc(rec->dom, 0);
                if (xml_tmp) {
                    char raw_mac[18] = {0};
                    char *mp = strstr(xml_tmp, "<mac address='");
                    if (mp) sscanf(mp, "<mac address='%17[^']'", raw_mac);
                    // MAC을 소문자로 정규화 (arp -n 출력 형식)
                    for (int i = 0; raw_mac[i]; i++)
                        mac_lower[i] = (char)tolower((unsigned char)raw_mac[i]);
                    free(xml_tmp);
                }
                if (mac_lower[0]) {
                    // arp -n 출력: "192.0.2.10  ether  52:54:00:31:ed:19 ..."
                    FILE *arp = popen("arp -n 2>/dev/null", "r");
                    if (arp) {
                        char line[256];
                        while (fgets(line, sizeof(line), arp)) {
                            char ip_buf[64]={0}, hw[8]={0}, mac_buf[18]={0};
                            if (sscanf(line, "%63s %7s %17s", ip_buf, hw, mac_buf) == 3) {
                                // MAC 소문자 비교
                                char mac_cmp[18]={0};
                                for (int i=0; mac_buf[i]; i++)
                                    mac_cmp[i]=(char)tolower((unsigned char)mac_buf[i]);
                                if (g_strcmp0(mac_cmp, mac_lower) == 0) {
                                    strncpy(vm_ip, ip_buf, sizeof(vm_ip)-1);
                                    break;
                                }
                            }
                        }
                        pclose(arp);
                    }
                }
            }
        }
        json_object_set_string_member(vm_obj, "ip", vm_ip);

        /* =================================================================
         * VM XML 파싱: MAC 주소, 네트워크 소스, 디스크 경로/크기, VNC 포트 추출
         *
         * [디자인 결정] libvirt API 대신 virDomainGetXMLDesc()의 XML을 직접 파싱합니다.
         *   이유: MAC, 디스크 경로, VNC 포트 등은 전용 API가 없거나 복잡하므로
         *   XML sscanf 파싱이 가장 간결합니다.
         *
         * [파싱 대상]
         *   1. MAC 주소 (<mac address='...'/>)
         *   2. 네트워크 소스 + 모델 (<interface> → <source> + <model>)
         *   3. 디스크 경로/크기/버스 (<disk> → <source file/dev> + <target bus>)
         *   4. CDROM 경로 (<disk device='cdrom'> → <source file>)
         *   5. VNC 포트 (<graphics type='vnc' port='...'/>)
         * ================================================================= */
        char mac_addr[32]    = "N/A";
        char net_source[128] = "N/A";
        char net_model[32]   = "N/A";
        char disk_path[256]  = "N/A";
        char disk_size[32]   = "N/A";
        char disk_bus[16]    = "N/A";
        char cdrom_path[256] = "(empty)";
        char vnc_port[16]    = "N/A";
        char vm_uuid[64]     = "N/A";

        // UUID
        char uuid_buf[VIR_UUID_STRING_BUFLEN];
        if (virDomainGetUUIDString(rec->dom, uuid_buf) == 0)
            strncpy(vm_uuid, uuid_buf, sizeof(vm_uuid)-1);

        char *xml = virDomainGetXMLDesc(rec->dom, 0);
        if (xml) {
            // 1. MAC 주소
            char *mac_ptr = strstr(xml, "<mac address='");
            if (mac_ptr) sscanf(mac_ptr, "<mac address='%17[^']'", mac_addr);

            // 2. 네트워크 소스 + 모델
            char *iface_ptr = strstr(xml, "<interface type='");
            if (iface_ptr) {
                char iface_type[32] = {0};
                sscanf(iface_ptr, "<interface type='%31[^']'", iface_type);
                char *source_ptr = strstr(iface_ptr, "<source ");
                if (source_ptr) {
                    char src_attr[32]={0}, src_val[64]={0};
                    if (sscanf(source_ptr, "<source %31[^=]='%63[^']'", src_attr, src_val)==2)
                        snprintf(net_source, sizeof(net_source), "%s (%s)", src_val, iface_type);
                    else
                        strncpy(net_source, iface_type, sizeof(net_source)-1);
                }
                // 네트워크 모델 (virtio, e1000, rtl8139 등)
                char *model_ptr = strstr(iface_ptr, "<model type='");
                if (model_ptr) sscanf(model_ptr, "<model type='%31[^']'", net_model);
            }

            // 3. 디스크 (첫 번째 비-cdrom 디스크)
            char *disk_ptr = strstr(xml, "<disk type=");
            while (disk_ptr) {
                // device='cdrom' 은 CD/DVD로 분리
                char *dev_ptr = strstr(disk_ptr, "device='");
                char dev_type[16] = {0};
                if (dev_ptr) sscanf(dev_ptr, "device='%15[^']'", dev_type);

                if (g_strcmp0(dev_type, "cdrom") == 0) {
                    // CD/DVD 경로
                    char *src_ptr = strstr(disk_ptr, "<source file='");
                    if (src_ptr) sscanf(src_ptr, "<source file='%255[^']'", cdrom_path);
                    else strncpy(cdrom_path, "(empty)", sizeof(cdrom_path)-1);
                } else if (g_strcmp0(disk_path, "N/A") == 0) {
                    // 첫 번째 실제 디스크
                    char *src_ptr = strstr(disk_ptr, "<source file='");
                    if (!src_ptr) src_ptr = strstr(disk_ptr, "<source dev='");
                    if (src_ptr) {
                        sscanf(src_ptr, "<source file='%255[^']'", disk_path);
                        if (g_strcmp0(disk_path, "N/A") == 0)
                            sscanf(src_ptr, "<source dev='%255[^']'", disk_path);
                    }
                    // 버스 타입 (nvme/sata/virtio/ide)
                    char *tgt_ptr = strstr(disk_ptr, "<target dev=");
                    if (tgt_ptr) {
                        char tgt_bus[16]={0};
                        sscanf(tgt_ptr, "<target dev='%*[^']' bus='%15[^']'", tgt_bus);
                        if (tgt_bus[0]) strncpy(disk_bus, tgt_bus, sizeof(disk_bus)-1);
                    }
                }
                // 다음 <disk 찾기
                char *next = strstr(disk_ptr+6, "<disk type=");
                if (!next) break;
                disk_ptr = next;
            }

            // 4. VNC 포트
            char *vnc_ptr = strstr(xml, "<graphics type='vnc'");
            if (vnc_ptr) {
                int vport = -1;
                sscanf(vnc_ptr, "<graphics type='vnc' port='%d'", &vport);
                if (vport == -1) snprintf(vnc_port, sizeof(vnc_port), "auto");
                else             snprintf(vnc_port, sizeof(vnc_port), ":%d", vport);
            }

            // 5. 디스크 용량
            // 일반 파일: stat.st_size
            // 블록 디바이스(/dev/zvol, /dev/sd*): st_size=0 → lseek(SEEK_END)로 크기 획득
            if (g_strcmp0(disk_path, "N/A") != 0) {
                off_t bytes = 0;
                struct stat st;
                if (stat(disk_path, &st) == 0) {
                    if (S_ISREG(st.st_mode)) {
                        // 일반 파일
                        bytes = st.st_size;
                    } else if (S_ISBLK(st.st_mode)) {
                        // 블록 디바이스 (zvol, LVM, raw disk)
                        int fd = open(disk_path, O_RDONLY | O_NONBLOCK);
                        if (fd >= 0) {
                            bytes = lseek(fd, 0, SEEK_END);
                            if (bytes < 0) bytes = 0;
                            close(fd);
                        }
                    }
                }
                if (bytes > 0) {
                    double gb = (double)bytes / (1024.0*1024.0*1024.0);
                    if (gb >= 1.0) snprintf(disk_size, sizeof(disk_size), "%.0f GB", gb);
                    else           snprintf(disk_size, sizeof(disk_size), "%.0f MB", gb*1024.0);
                }
            }
            free(xml);
        }
        json_object_set_string_member(vm_obj, "mac",        mac_addr);
        json_object_set_string_member(vm_obj, "net_source", net_source);
        json_object_set_string_member(vm_obj, "net_model",  net_model);
        json_object_set_string_member(vm_obj, "disk_path",  disk_path);
        json_object_set_string_member(vm_obj, "disk_size",  disk_size);
        json_object_set_string_member(vm_obj, "disk_bus",   disk_bus);
        json_object_set_string_member(vm_obj, "cdrom_path", cdrom_path);
        json_object_set_string_member(vm_obj, "vnc_port",   vnc_port);
        json_object_set_string_member(vm_obj, "uuid",       vm_uuid);
        // =================================================================

        /* libvirt 도메인 상태 → 사용자 표시용 문자열 변환 */
        const char *state_str = "UNKNOWN";               
        
        switch (state) {
            case VIR_DOMAIN_RUNNING: state_str = "RUNNING"; break;
            case VIR_DOMAIN_PAUSED: state_str = "PAUSED"; break;
            case VIR_DOMAIN_SHUTDOWN: state_str = "SHUTDOWN"; break;
            case VIR_DOMAIN_SHUTOFF: state_str = "OFFLINE"; break;
            case VIR_DOMAIN_CRASHED: state_str = "CRASHED"; break;
        }
        json_object_set_string_member(vm_obj, "state", state_str);

        /* VM 리소스 메트릭을 JSON 객체에 추가 */
        json_object_set_int_member(vm_obj, "vcpu", vcpu);
        json_object_set_int_member(vm_obj, "cpu_time_ns", (gint64)cpu_time_ns);
        json_object_set_double_member(vm_obj, "mem_used_mb", mem_used_mb);
        json_object_set_double_member(vm_obj, "mem_max_mb", (double)mem_max / 1024.0);
        json_object_set_int_member(vm_obj, "disk_rd_bytes", rd_bytes);
        json_object_set_int_member(vm_obj, "disk_wr_bytes", wr_bytes);
        json_object_set_int_member(vm_obj, "net_rx_bytes", rx_bytes);
        json_object_set_int_member(vm_obj, "net_tx_bytes", tx_bytes);

        /* VM 속성: 부팅 시 자동시작(autostart) + 영구 정의(persistent) 여부 */
        int autostart = 0;
        virDomainGetAutostart(rec->dom, &autostart);
        int persistent = virDomainIsPersistent(rec->dom);
        json_object_set_boolean_member(vm_obj, "autostart",  autostart ? TRUE : FALSE);
        json_object_set_boolean_member(vm_obj, "persistent", persistent > 0 ? TRUE : FALSE);

        json_array_add_object_element(fleet_array, vm_obj);
    }    
    
    virDomainStatsRecordListFree(stats);  /* libvirt 할당 메모리이므로 전용 함수로 해제 (g_free 아님) */

    /* =================================================================
     * 호스트 물리 자원 수집부 — /proc, /sys, sysinfo() 에서 직접 파싱
     *
     * [수집 항목]
     *   1. CPU: 코어 수, 모델명, /proc/stat 모드별 ticks, 온도
     *   2. MEM: /proc/meminfo (MemTotal, MemFree, MemAvailable, Buffers,
     *           Cached, SwapTotal, SwapFree, Slab, SReclaimable)
     *   3. DISK: statvfs("/") 루트 파티션, /proc/diskstats I/O 통계
     *   4. NET: /proc/net/dev 인터페이스별 bytes/packets/errors/drops
     *   5. 기타: uptime, load average
     * ================================================================= */
    JsonObject *host_obj = json_object_new();

    /* 1. CPU 코어 수 (온라인) 및 모델명 (/proc/cpuinfo에서 추출) */
    int cpu_cores = sysconf(_SC_NPROCESSORS_ONLN);
    json_object_set_int_member(host_obj, "cpus", cpu_cores);
    char cpu_model[128] = "Unknown Architecture";
    FILE *f_cpu = fopen("/proc/cpuinfo", "r");
    if (f_cpu) {
        char line[256];
        while (fgets(line, sizeof(line), f_cpu)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) { strncpy(cpu_model, colon + 2, sizeof(cpu_model)-1); cpu_model[strcspn(cpu_model, "\n")] = 0; break; }
            }
        }
        fclose(f_cpu);
    }
    json_object_set_string_member(host_obj, "cpu_model", cpu_model);

    /*
     * 2. CPU Ticks (WhaTap 스타일: 모드별 누적 jiffies를 raw로 전송)
     *
     * [설계 의도]
     *   서버는 /proc/stat의 누적값(jiffies)을 그대로 전송하고,
     *   Web UI가 이전 값과의 delta를 계산하여 백분율로 변환한다.
     *   → 서버 측 상태(이전 샘플)를 유지할 필요 없이, 클라이언트가 폴링 간격을
     *     자유롭게 설정할 수 있다.
     *
     * [/proc/stat "cpu" 행 필드 순서]
     *   cpu  user(u)  nice(n)  system(s)  idle(i)  iowait(io)  irq  softirq  steal
     *
     * [host_obj에 추가되는 필드]
     *   cpu_total_ticks   : 8개 모드 합산 (분모용)
     *   cpu_idle_ticks    : idle + iowait (유휴 시간)
     *   cpu_user_ticks    : [W-1] 사용자 공간 jiffies — UI에서 user% = delta(user) / delta(total) × 100
     *   cpu_nice_ticks    : nice 프로세스 jiffies
     *   cpu_system_ticks  : [W-1] 커널 공간 jiffies — 시스템 콜, 커널 스레드
     *   cpu_iowait_ticks  : [W-1] I/O 대기 jiffies — 디스크/네트워크 병목 감지
     *   cpu_irq_ticks     : [W-1] 하드웨어 인터럽트 jiffies
     *   cpu_softirq_ticks : [W-1] 소프트 인터럽트 jiffies (네트워크 패킷 처리 등)
     *   cpu_steal_ticks   : [W-1] 하이퍼바이저에 빼앗긴 jiffies (VM 환경에서 과할당 감지)
     *
     * [cores 배열 — 코어별 동일 구조]
     *   /proc/stat의 "cpu0", "cpu1", ... 행을 개별 파싱하여 코어별 부하를 제공.
     *   각 core_obj에 추가되는 필드:
     *     id      : 코어 번호 (0부터 시작)
     *     total   : 8개 모드 합산 (코어별 분모)
     *     idle    : idle + iowait (코어별 유휴)
     *     user    : [W-1] 사용자 공간 jiffies (코어별)
     *     system  : [W-1] 커널 공간 jiffies (코어별)
     *     iowait  : [W-1] I/O 대기 jiffies (코어별 — 특정 코어에 I/O 편중 확인)
     *     steal   : [W-1] 하이퍼바이저 빼앗김 jiffies (코어별)
     */
    unsigned long long u=0, n=0, s=0, i=0, io=0, irq=0, soft=0, steal=0;
    JsonArray *cores_array = json_array_new();
    FILE *f_stat = fopen("/proc/stat", "r");
    if (f_stat) {
        char line[256];
        while (fgets(line, sizeof(line), f_stat)) {
            if (strncmp(line, "cpu", 3) == 0) {
                if (line[3] == ' ') {
                    /* 집계 "cpu" 행 (전체 코어 합산) */
                    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &io, &irq, &soft, &steal);
                    json_object_set_int_member(host_obj, "cpu_total_ticks", u+n+s+i+io+irq+soft+steal);
                    json_object_set_int_member(host_obj, "cpu_idle_ticks", i+io);
                    /* [W-1] 모드별 raw ticks — UI가 delta로 백분율 계산 */
                    json_object_set_int_member(host_obj, "cpu_user_ticks",    u);
                    json_object_set_int_member(host_obj, "cpu_nice_ticks",    n);
                    json_object_set_int_member(host_obj, "cpu_system_ticks",  s);
                    json_object_set_int_member(host_obj, "cpu_iowait_ticks",  io);
                    json_object_set_int_member(host_obj, "cpu_irq_ticks",     irq);
                    json_object_set_int_member(host_obj, "cpu_softirq_ticks", soft);
                    json_object_set_int_member(host_obj, "cpu_steal_ticks",   steal);
                } else if (line[3] >= '0' && line[3] <= '9') {
                    /* 개별 "cpuN" 행 (코어별) */
                    int core_id; sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &core_id, &u, &n, &s, &i, &io, &irq, &soft, &steal);
                    JsonObject *core_obj = json_object_new();
                    json_object_set_int_member(core_obj, "id", core_id);
                    json_object_set_int_member(core_obj, "total", u+n+s+i+io+irq+soft+steal);
                    json_object_set_int_member(core_obj, "idle", i+io);
                    json_object_set_int_member(core_obj, "user", u);     /* [W-1] 코어별 user ticks */
                    json_object_set_int_member(core_obj, "system", s);   /* [W-1] 코어별 system ticks */
                    json_object_set_int_member(core_obj, "iowait", io);  /* [W-1] 코어별 iowait ticks */
                    json_object_set_int_member(core_obj, "steal", steal); /* [W-1] 코어별 steal ticks */
                    json_array_add_object_element(cores_array, core_obj);
                }
            }
        }
        fclose(f_stat);
    }
    json_object_set_array_member(host_obj, "cores", cores_array);

    /* 2b. Uptime — /proc/uptime 에서 부팅 후 경과 시간 (초) 읽기 */
    FILE *f_up = fopen("/proc/uptime", "r");
    if (f_up) {
        double up_sec = 0;
        if (fscanf(f_up, "%lf", &up_sec) == 1)
            json_object_set_double_member(host_obj, "uptime_secs", up_sec);
        fclose(f_up);
    }

    /* 2c. Load Average — 1분/5분/15분 평균 실행 큐 길이 */
    FILE *f_la = fopen("/proc/loadavg", "r");
    if (f_la) {
        double l1 = 0, l5 = 0, l15 = 0;
        if (fscanf(f_la, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            json_object_set_double_member(host_obj, "load_1", l1);
            json_object_set_double_member(host_obj, "load_5", l5);
            json_object_set_double_member(host_obj, "load_15", l15);
        }
        fclose(f_la);
    }

    /*
     * 2d. CPU 온도 (밀리도 → 섭씨 변환)
     *   우선순위: thermal_zone0 → hwmon (coretemp=Intel, k10temp=AMD) 폴백
     *   thermal_zone0이 0을 반환하면 하드웨어 센서 드라이버를 직접 탐색합니다.
     */
    {
        int milli = 0;
        FILE *f_temp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
        if (f_temp) { if (fscanf(f_temp, "%d", &milli) != 1) milli = 0; fclose(f_temp); }
        if (milli <= 0) {
            /* AMD k10temp 또는 Intel coretemp 폴백 — /sys/class/hwmon 순회 */
            GDir *hdir = g_dir_open("/sys/class/hwmon", 0, NULL);
            if (hdir) {
                const gchar *ent;
                while ((ent = g_dir_read_name(hdir)) != NULL) {
                    gchar *npath = g_strdup_printf("/sys/class/hwmon/%s/name", ent);
                    gchar *chip = NULL; gsize len = 0;
                    if (g_file_get_contents(npath, &chip, &len, NULL)) {
                        g_strstrip(chip);
                        if (g_strcmp0(chip, "k10temp") == 0 || g_strcmp0(chip, "coretemp") == 0) {
                            gchar *tpath = g_strdup_printf("/sys/class/hwmon/%s/temp1_input", ent);
                            FILE *tf = fopen(tpath, "r");
                            if (tf) { if (fscanf(tf, "%d", &milli) != 1) milli = 0; fclose(tf); }
                            g_free(tpath);
                            g_free(chip); g_free(npath);
                            break;
                        }
                        g_free(chip);
                    }
                    g_free(npath);
                }
                g_dir_close(hdir);
            }
        }
        if (milli > 0)
            json_object_set_double_member(host_obj, "cpu_temp_c", milli / 1000.0);
    }

    /*
     * 3. 메모리 (/proc/meminfo 파싱)
     *   기본: MemTotal, MemFree, MemAvailable
     *   WhaTap W-1 확장: Buffers, Cached, SwapTotal/Free, Slab, SReclaimable
     *   MemAvailable = MemFree + 회수 가능 캐시/버퍼 (커널 추정치)
     *   Used = Total - Available (실제 사용 중인 메모리)
     */
    unsigned long long mem_tot_kb = 0, mem_free_kb = 0, mem_avail_kb = 0;
    unsigned long long buffers_kb = 0, cached_kb = 0;
    unsigned long long swap_tot_kb = 0, swap_free_kb = 0;
    unsigned long long slab_kb = 0, sreclaimable_kb = 0;
    FILE *f_mem = fopen("/proc/meminfo", "r");
    if(f_mem) {
        char line[256];
        while(fgets(line, sizeof(line), f_mem)) {
            if(sscanf(line, "MemTotal: %llu kB", &mem_tot_kb) == 1) continue;
            if(sscanf(line, "MemFree: %llu kB", &mem_free_kb) == 1) continue;
            if(sscanf(line, "MemAvailable: %llu kB", &mem_avail_kb) == 1) continue;
            if(sscanf(line, "Buffers: %llu kB", &buffers_kb) == 1) continue;
            if(sscanf(line, "Cached: %llu kB", &cached_kb) == 1) continue;
            if(sscanf(line, "SwapTotal: %llu kB", &swap_tot_kb) == 1) continue;
            if(sscanf(line, "SwapFree: %llu kB", &swap_free_kb) == 1) continue;
            if(sscanf(line, "Slab: %llu kB", &slab_kb) == 1) continue;
            if(sscanf(line, "SReclaimable: %llu kB", &sreclaimable_kb) == 1) continue;
        }
        fclose(f_mem);
    }
    unsigned long long mem_used_kb = mem_tot_kb - mem_avail_kb;
    json_object_set_double_member(host_obj, "mem_total_gb", (double)mem_tot_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_used_gb", (double)mem_used_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_avail_gb", (double)mem_avail_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_free_gb", (double)mem_free_kb / 1048576.0);
    json_object_set_double_member(host_obj, "mem_percent", mem_tot_kb > 0 ? ((double)mem_used_kb / mem_tot_kb) * 100.0 : 0.0);
    json_object_set_double_member(host_obj, "mem_buffers_mb", (double)buffers_kb / 1024.0);
    json_object_set_double_member(host_obj, "mem_cached_mb", (double)cached_kb / 1024.0);
    json_object_set_double_member(host_obj, "swap_total_gb", (double)swap_tot_kb / 1048576.0);
    json_object_set_double_member(host_obj, "swap_used_gb", (double)(swap_tot_kb - swap_free_kb) / 1048576.0);
    /*
     * [W-1] 커널 슬랩 메모리 — /proc/meminfo "Slab" / "SReclaimable"
     *   mem_slab_mb         : 커널 슬랩 할당자 총 사용량 (MB) — dentry/inode 캐시 등 커널 객체
     *   mem_sreclaimable_mb : 슬랩 중 메모리 압박 시 회수 가능한 부분 (MB)
     *   (slab - sreclaimable = 회수 불가능한 커널 메모리)
     */
    json_object_set_double_member(host_obj, "mem_slab_mb", (double)slab_kb / 1024.0);
    json_object_set_double_member(host_obj, "mem_sreclaimable_mb", (double)sreclaimable_kb / 1024.0);

    /* 4. 호스트 루트 디스크 파티션 (/) — statvfs로 전체/사용/여유 공간 조회 */
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        unsigned long long total_disk = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long free_disk = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
        unsigned long long used_disk = total_disk - free_disk;
        json_object_set_double_member(host_obj, "disk_total_gb", (double)total_disk / 1073741824.0);
        json_object_set_double_member(host_obj, "disk_used_gb", (double)used_disk / 1073741824.0);
        json_object_set_double_member(host_obj, "disk_percent", total_disk > 0 ? ((double)used_disk / total_disk) * 100.0 : 0.0);
    }

    /* 5. 호스트 네트워크 — /proc/net/dev에서 메인 인터페이스 RX/TX 바이트 추출 */
    FILE *f_net = fopen("/proc/net/dev", "r");
    unsigned long long host_rx = 0, host_tx = 0;
    char host_iface[32] = "N/A";
    if (f_net) {
        char line[256];
        if (skip_proc_lines(f_net, line, sizeof(line), 2)) {
            while (fgets(line, sizeof(line), f_net)) {
                char iface[32]; unsigned long long rx, tx;
                if (sscanf(line, " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu", iface, &rx, &tx) == 3) {
                    if (strncmp(iface, "lo", 2) != 0) { // 루프백 무시하고 첫 물리 NIC 선택
                        strncpy(host_iface, iface, sizeof(host_iface)-1);
                        host_rx = rx; host_tx = tx; break;
                    }
                }
            }
        }
        fclose(f_net);
    }
    json_object_set_string_member(host_obj, "net_iface", host_iface);
    json_object_set_int_member(host_obj, "net_rx_bytes", host_rx);
    json_object_set_int_member(host_obj, "net_tx_bytes", host_tx);

    /*
     * [WhaTap W-1] Network Error/Drop — /proc/net/dev 재파싱 (전 NIC 합산)
     *
     * 위의 "신규 6" 블록은 첫 번째 물리 NIC의 RX/TX bytes만 추출했지만,
     * 이 블록은 lo를 제외한 모든 인터페이스의 패킷/에러/드롭을 합산한다.
     *
     * [/proc/net/dev sscanf 매핑]
     *   "iface: rb rp re rd [4개 스킵] tb tp te td"
     *   rb=RX bytes, rp=RX packets, re=RX errors, rd=RX drops
     *   tb=TX bytes, tp=TX packets, te=TX errors, td=TX drops
     *   → RX 5~8번째 필드(fifo/frame/compressed/multicast)는 %*u로 스킵
     *
     * [host_obj에 추가되는 필드]
     *   net_rx_packets : 수신 패킷 수 합산 (부팅 이후 누적)
     *   net_tx_packets : 송신 패킷 수 합산
     *   net_rx_errs    : 수신 에러 수 합산 (CRC, 프레임 에러 등)
     *   net_tx_errs    : 송신 에러 수 합산
     *   net_rx_drop    : 수신 드롭 수 합산 (커널 버퍼 부족 등)
     *   net_tx_drop    : 송신 드롭 수 합산
     */
    {
        FILE *f_nd = fopen("/proc/net/dev", "r");
        unsigned long long rx_errs=0, tx_errs=0, rx_drop=0, tx_drop=0;
        unsigned long long rx_pkts=0, tx_pkts=0;
        if (f_nd) {
            char ndl[512];
            if (skip_proc_lines(f_nd, ndl, sizeof(ndl), 2)) {
                while (fgets(ndl, sizeof(ndl), f_nd)) {
                    char nif[32];
                    unsigned long long rb, rp, re, rd, tb, tp, te, td;
                    if (sscanf(ndl, " %31[^:]: %llu %llu %llu %llu %*u %*u %*u %*u %llu %llu %llu %llu",
                               nif, &rb, &rp, &re, &rd, &tb, &tp, &te, &td) == 9) {
                        char *ns = nif; while (*ns == ' ') ns++;
                        if (strncmp(ns, "lo", 2) == 0) continue; /* 루프백 제외 */
                        rx_pkts += rp; tx_pkts += tp;
                        rx_errs += re; tx_errs += te;
                        rx_drop += rd; tx_drop += td;
                    }
                }
            }
            fclose(f_nd);
        }
        json_object_set_int_member(host_obj, "net_rx_packets", rx_pkts);
        json_object_set_int_member(host_obj, "net_tx_packets", tx_pkts);
        json_object_set_int_member(host_obj, "net_rx_errs", rx_errs);
        json_object_set_int_member(host_obj, "net_tx_errs", tx_errs);
        json_object_set_int_member(host_obj, "net_rx_drop", rx_drop);
        json_object_set_int_member(host_obj, "net_tx_drop", tx_drop);
    }

    /*
     * [WhaTap W-1] Host Disk I/O — /proc/diskstats 파싱 (whole-disk 합산)
     *
     * [/proc/diskstats 필드 매핑] (헤더 없음, 디바이스당 1행)
     *   maj min ddev rd_io rd_m rd_sec rd_t wr_io wr_m wr_sec wr_t io_n io_t io_w
     *   (총 14필드, 커널 4.18+에서 discard 4필드 추가되지만 여기서는 14만 사용)
     *
     *   rd_io  : 읽기 I/O 완료 횟수
     *   rd_sec : 읽기 섹터 수 (× 512 = 바이트, 커널 고정 섹터 크기)
     *   wr_io  : 쓰기 I/O 완료 횟수
     *   wr_sec : 쓰기 섹터 수 (× 512 = 바이트)
     *   io_t   : I/O에 소요된 총 시간 (ms) — 디스크 사용률(utilization) 계산용
     *
     * [whole-disk 필터링 — 파티션 이중 카운트 방지]
     *   sd[a-z]  3글자 : SATA/SAS 디스크 (sda OK, sda1 제외)
     *   vd[a-z]  3글자 : KVM virtio 디스크 (vda OK, vda1 제외)
     *   nvme*에 'p' 없음: NVMe 디스크 (nvme0n1 OK, nvme0n1p1 제외)
     *
     * [host_obj에 추가되는 필드]
     *   disk_rd_bytes    : 호스트 전체 디스크 읽기 바이트 (부팅 이후 누적)
     *   disk_wr_bytes    : 호스트 전체 디스크 쓰기 바이트
     *   disk_rd_ios      : 읽기 I/O 횟수
     *   disk_wr_ios      : 쓰기 I/O 횟수
     *   disk_io_ticks_ms : I/O 소요 시간 합산 (ms) — delta로 utilization% 계산 가능
     */
    {
        FILE *f_ds = fopen("/proc/diskstats", "r");
        unsigned long long h_rd_ios=0, h_wr_ios=0, h_rd_bytes=0, h_wr_bytes=0, h_io_ticks=0;
        if (f_ds) {
            char dsl[512];
            while (fgets(dsl, sizeof(dsl), f_ds)) {
                unsigned int maj, min;
                char ddev[64];
                unsigned long long rd_io, rd_m, rd_sec, rd_t, wr_io, wr_m, wr_sec, wr_t, io_n, io_t, io_w;
                int dn = sscanf(dsl, " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                    &maj, &min, ddev, &rd_io, &rd_m, &rd_sec, &rd_t, &wr_io, &wr_m, &wr_sec, &wr_t, &io_n, &io_t, &io_w);
                if (dn < 14) continue;
                /* whole-disk 필터: 파티션(sda1, nvme0n1p1 등)은 제외 */
                size_t dl = strlen(ddev);
                int keep = 0;
                if (strncmp(ddev,"sd",2)==0 && dl==3) keep=1;        /* sda, sdb ... */
                else if (strncmp(ddev,"vd",2)==0 && dl==3) keep=1;   /* vda, vdb ... */
                else if (strncmp(ddev,"nvme",4)==0 && !strchr(ddev,'p')) keep=1; /* nvme0n1 */
                if (!keep) continue;
                h_rd_ios   += rd_io;   h_wr_ios   += wr_io;
                h_rd_bytes += rd_sec*512; h_wr_bytes += wr_sec*512;  /* 섹터 × 512 = 바이트 */
                h_io_ticks += io_t;                                   /* I/O 소요 시간 (ms) */
            }
            fclose(f_ds);
        }
        json_object_set_int_member(host_obj, "disk_rd_bytes", h_rd_bytes);
        json_object_set_int_member(host_obj, "disk_wr_bytes", h_wr_bytes);
        json_object_set_int_member(host_obj, "disk_rd_ios",   h_rd_ios);
        json_object_set_int_member(host_obj, "disk_wr_ios",   h_wr_ios);
        json_object_set_int_member(host_obj, "disk_io_ticks_ms", h_io_ticks);
    }

    virt_conn_pool_release(conn);

    // =================================================================
    // 최종 JSON-RPC 2.0 조립
    // =================================================================
    JsonObject *rpc_resp = json_object_new();
    json_object_set_string_member(rpc_resp, "jsonrpc", "2.0");
    json_object_set_string_member(rpc_resp, "id", "monitor-fleet");
    
    JsonObject *result_obj = json_object_new();
    json_object_set_array_member(result_obj, "fleet", fleet_array);
    json_object_set_object_member(result_obj, "host", host_obj);
    json_object_set_object_member(rpc_resp, "result", result_obj);

    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, rpc_resp);
    
    gchar *response_str = json_to_string(root_node, FALSE);
    json_node_free(root_node);

    return response_str;
}
