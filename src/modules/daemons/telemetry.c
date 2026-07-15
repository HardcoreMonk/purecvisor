/**
 * @file telemetry.c
 * @brief Zero-Blocking Telemetry Daemon (Lock-Free Metrics Cache)
 *
 * [파일 역할]
 *   현재 에디션 데몬의 백그라운드 텔레메트리 수집기.
 *   전용 GThread에서 1초 간격으로 Libvirt Bulk API(virConnectGetAllDomainStats)를
 *   호출하여 모든 VM의 CPU/네트워크 통계를 수집하고, 메인 스레드에 Lock-Free로
 *   전달하여 RPC 핸들러(vm.metrics, monitor.fleet 등)가 O(1)로 조회할 수 있게 합니다.
 *
 * [아키텍처 위치]
 *   main.c (데몬 시작)
 *     -> init_telemetry_daemon(vm_manager)   [이 파일]
 *          -> GThread("telemetry-daemon")    [백그라운드 무한 루프]
 *               -> g_main_context_invoke()   [메인 스레드로 캐시 Push]
 *   handler_vm_lifecycle.c (vm.metrics RPC)
 *     -> get_vm_metrics(uuid)                [이 파일, O(1) 해시 조회]
 *
 * [주요 흐름 — 데이터 수집 사이클 (1초)]
 *   1. (Background Thread) virConnectGetAllDomainStats()로 전체 VM 통계 한번에 수집
 *   2. (Background Thread) 수집 데이터를 새 GHashTable(UUID -> VmMetrics)로 적재
 *   3. (Background Thread) g_main_context_invoke()로 메인 스레드에 새 캐시를 넘김
 *   4. (Main Thread)       기존 캐시 destroy + 포인터 스왑 (단일 스레드라 Mutex 불필요)
 *   5. (Main Thread)       vm-metrics-updated GIO 시그널 emit (구독자 갱신 트리거)
 *
 * [핵심 패턴 — Lock-Free 캐시 교체]
 *   - 백그라운드 스레드는 새 GHashTable을 "만들기만" 하고 전역 포인터를 건드리지 않음
 *   - g_main_context_invoke()로 메인 이벤트 루프에 스왑 작업을 위임
 *   - 메인 루프는 싱글 스레드이므로 RPC 핸들러와 캐시 스왑이 절대 동시 실행되지 않음
 *   - 결과: Mutex 없이 완전한 Thread Safety 달성
 *
 * [주의사항]
 *   - get_vm_metrics() 반환 포인터는 현재 이벤트 루프 턴에서만 유효
 *     (다음 캐시 스왑 시 이전 메모리가 해제됨 -> 즉시 JSON 직렬화 후 사용할 것)
 *   - get_vm_metrics()는 반드시 메인 스레드에서만 호출 (GTask 워커 스레드에서 호출 금지)
 *   - virConnectGetAllDomainStats 실패 시 캐시가 갱신되지 않음 (이전 데이터 유지)
 *   - vm_manager가 먼저 소멸해도 GWeakRef로 안전 (NULL 반환 -> 시그널 skip)
 */

#include <glib.h>
#include <libvirt/libvirt.h>
#include <string.h>

/* GIO P6: vm-metrics-updated 신호 emit */
#include "../virt/vm_manager.h"
#include "telemetry.h"

/* VmMetrics, get_vm_metrics, init_telemetry_daemon: telemetry.h 에 선언됨 */

/* * 글로벌 메트릭 캐시 (오직 Main Thread에서만 읽고 씁니다!)
 * Key: VM UUID (gchar*), Value: VmMetrics 구조체 포인터
 */
static GHashTable *global_metrics_cache = NULL;

/* GIO P6: vm-metrics-updated 신호 발신 대상 (약한 참조)
 * init_telemetry_daemon() 에서 설정됩니다.
 * GWeakRef 이므로 vm_manager 가 먼저 소멸해도 NULL 반환 → 안전.
 */
static GWeakRef g_signal_emitter_ref;


/* =========================================================
 * 1. 메인 스레드 영역 (Main Thread Only)
 * ========================================================= */

/**
 * @brief 백그라운드 스레드가 던져준 새로운 캐시로 전역 포인터를 교체합니다.
 * 이 함수는 g_main_context_invoke에 의해 메인 이벤트 루프 안에서 실행되므로, 
 * 다른 JSON-RPC 요청과 절대 겹치지 않아 완벽하게 Thread-safe 합니다.
 */
static gboolean update_metrics_cache_in_main_thread(gpointer user_data) {
    GHashTable *new_cache = (GHashTable *)user_data;

    // 1. 기존 캐시가 있다면 메모리 해제
    if (global_metrics_cache != NULL) {
        g_hash_table_destroy(global_metrics_cache);
    }

    // 2. 포인터 스왑 — 이후 RPC 조회는 새 캐시를 바라봄
    global_metrics_cache = new_cache;

    // 3. GIO P6: vm-metrics-updated 신호 emit
    //    GWeakRef 로 vm_manager 생존 여부 확인 후 emit.
    PureCVisorVmManager *mgr =
        PURECVISOR_VM_MANAGER(g_weak_ref_get(&g_signal_emitter_ref));
    if (mgr) {
        purecvisor_vm_manager_emit_metrics_updated(mgr, new_cache);
        g_object_unref(mgr);   /* g_weak_ref_get 이 ref 를 추가하므로 해제 필요 */
    }

    /*
     * Phase 2 A: WebSocket 실시간 push — VM 메트릭 브로드캐스트
     *
     * Web UI 대시보드가 ws://localhost:80/api/v1/ws/events 에 연결되어 있으면,
     * 매 1초마다 VM 수 정보를 포함한 메트릭 이벤트를 푸시한다.
     * 이를 통해 대시보드가 폴링 없이 실시간으로 VM 상태를 표시할 수 있다.
     *
     * pcv_ws_client_count() > 0 체크로 WebSocket 클라이언트가 없을 때는
     * 불필요한 JSON 직렬화 + 브로드캐스트를 건너뛰어 CPU를 절약한다.
     *
     * extern 선언: ws_server.c에 정의된 함수. 헤더 순환 의존 방지를 위해
     * 인라인 extern 사용. (ws_server.h를 include하면 헤더 체인이 복잡해짐)
     */
    {
        extern void pcv_ws_broadcast(const gchar *type, const gchar *payload_json);
        extern gint pcv_ws_client_count(void);
        if (pcv_ws_client_count() > 0 && new_cache) {
            GString *payload = g_string_new("{\"vm_count\":");
            g_string_append_printf(payload, "%u", g_hash_table_size(new_cache));
            g_string_append(payload, "}");
            pcv_ws_broadcast("metric", payload->str);
            g_string_free(payload, TRUE);  /* TRUE = 내부 char* 버퍼도 해제 */
        }
    }

    return G_SOURCE_REMOVE;
}

/**
 * @brief 외부 디스패처(API 핸들러)에서 특정 VM의 메트릭을 O(1) 속도로 조회할 때 사용합니다.
 * ⚠️ 반드시 Main Thread에서만 호출해야 합니다.
 */
VmMetrics* get_vm_metrics(const gchar *vm_id) {
    if (G_UNLIKELY(global_metrics_cache == NULL)) return NULL;
    return (VmMetrics*)g_hash_table_lookup(global_metrics_cache, vm_id);
}


/* =========================================================
 * 2. 백그라운드 데몬 영역 (Dedicated GThread)
 * ========================================================= */

/**
 * @brief 무한 루프를 돌며 Libvirt로부터 메트릭을 수집하는 전용 데몬 스레드
 */
static gpointer telemetry_worker_thread(gpointer data) {
    (void)data; // 미사용 파라미터 경고 방지

    /*
     * 1. 데몬 전용 독립적인 Libvirt 커넥션 개방 (Keep-Alive)
     *
     * 메인 스레드의 커넥션 풀(virt_conn_pool.c)과 별도로 전용 커넥션을 사용한다.
     * 이유: 이 스레드는 무한 루프로 1초마다 Bulk API를 호출하므로,
     * 공유 풀에서 매번 acquire/release 하는 것보다 전용 커넥션이 효율적이다.
     *
     * "qemu:///system"은 로컬 QEMU/KVM 하이퍼바이저에 대한 표준 URI.
     * 이 URI로 연결하면 libvirtd 데몬을 통해 시스템 레벨 VM을 관리할 수 있다.
     */
    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) {
        g_critical("🚨 [Telemetry] Failed to connect to Libvirt. Telemetry daemon shutting down.");
        return NULL;
    }
    /*
     * KeepAlive 설정: 5초 간격으로 핑, 3번 연속 실패 시 연결 종료.
     * libvirtd가 재시작되거나 네트워크가 끊기면 API 호출이 실패하게 되고,
     * 이후 루프에서 stats == NULL이 반환되어 캐시 갱신이 중단된다.
     * (데몬 재시작으로 복구 필요)
     */
    virConnectSetKeepAlive(conn, 5, 3);
    
    g_message("📡 [Telemetry] Background Daemon Thread started successfully.");

    // 2. 무한 폴링 루프
    while (TRUE) {
        virDomainStatsRecordPtr *stats = NULL;
        
        /*
         * 핵심: Bulk API를 사용하여 모든 VM의 통계를 한 번의 호출로 수집한다.
         * 개별 VM마다 virDomainGetInfo()/virDomainInterfaceStats()를 호출하면
         * VM 수에 비례하여 libvirtd와의 RPC 왕복이 증가하므로 비효율적이다.
         *
         * stats_flags 설명:
         *   VIR_DOMAIN_STATS_CPU_TOTAL  — CPU time (nanoseconds) 포함
         *   VIR_DOMAIN_STATS_INTERFACE  — 네트워크 인터페이스 RX/TX 포함
         *
         * 네 번째 인자 0은 "모든 도메인(실행 중+정지)"을 대상으로 수집.
         * VIR_CONNECT_GET_ALL_DOMAINS_STATS_RUNNING을 지정하면 실행 중만 수집.
         */
        unsigned int stats_flags = VIR_DOMAIN_STATS_CPU_TOTAL | VIR_DOMAIN_STATS_INTERFACE;
        int ret = virConnectGetAllDomainStats(conn, stats_flags, &stats, 0);

        if (ret >= 0 && stats != NULL) {
            
            // 3. 이번 주기의 새로운 해시 테이블 생성 (메인 스레드로 밀어넣을 용도)
            // Key는 g_strdup 된 문자열이므로 g_free, Value는 구조체 포인터이므로 g_free 로 소멸자 세팅
            GHashTable *new_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

            // 4. 리턴받은 Bulk 데이터 파싱 및 새 캐시에 적재
            for (int i = 0; stats[i] != NULL; i++) {
                virDomainStatsRecordPtr record = stats[i];
                virDomainPtr dom = record->dom;
                
                char uuid[VIR_UUID_STRING_BUFLEN];
                if (virDomainGetUUIDString(dom, uuid) < 0) continue;
                
                VmMetrics *metrics = g_new0(VmMetrics, 1);
                
                // Libvirt Typed Parameters (Key-Value) 파싱
                // 전 NIC 합산: net.0, net.1, ... 인덱스 무관하게 prefix/suffix 매칭
                for (int j = 0; j < record->nparams; j++) {
                    virTypedParameterPtr param = &record->params[j];

                    if (g_strcmp0(param->field, "cpu.time") == 0) {
                        metrics->cpu_time_ns = param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.bytes")) {
                        metrics->rx_bytes += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.bytes")) {
                        metrics->tx_bytes += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.pkts")) {
                        metrics->rx_packets += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.pkts")) {
                        metrics->tx_packets += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.errs")) {
                        metrics->rx_errs += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.errs")) {
                        metrics->tx_errs += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".rx.drop")) {
                        metrics->rx_drop += param->value.ul;
                    } else if (g_str_has_prefix(param->field, "net.") &&
                               g_str_has_suffix(param->field, ".tx.drop")) {
                        metrics->tx_drop += param->value.ul;
                    }
                }
                
                // 파싱 완료된 메트릭을 새 캐시에 삽입
                g_hash_table_insert(new_cache, g_strdup(uuid), metrics);
            }
            
            // Libvirt가 할당한 통계 레코드 메모리 해제
            virDomainStatsRecordListFree(stats);

            // 5. 🚀 메인 스레드로 새 캐시의 소유권을 완전히 넘김 (Push)
            // 이때 스레드 락킹 없이 이벤트 큐를 타고 안전하게 전달됩니다.
            g_main_context_invoke(NULL, update_metrics_cache_in_main_thread, new_cache);
        } else {
            g_warning("⚠️ [Telemetry] Failed to fetch domain stats from Libvirt.");
        }

        // 6. 1초 대기 후 다음 수집 주기 시작
        g_usleep(1000000); // 1,000,000 micro-seconds
    }

    // 도달하지 않는 코드 (무한 루프)
    virConnectClose(conn);
    return NULL;
}


/* =========================================================
 * 3. 초기화 (엔트리 포인트 연동)
 * ========================================================= */

/**
 * @brief 서버 기동 시 호출되어 백그라운드 텔레메트리 스레드를 생성합니다.
 *
 * @param vm_manager (transfer none): vm-metrics-updated 신호 발신 대상.
 *   내부적으로 GWeakRef 로 보관합니다.
 */
void init_telemetry_daemon(PureCVisorVmManager *vm_manager) {
    /* GIO P6: 약한 참조 초기화 — vm_manager 가 먼저 소멸해도 안전 */
    g_weak_ref_init(&g_signal_emitter_ref, vm_manager);

    GError *error = NULL;
    GThread *thread = g_thread_try_new("telemetry-daemon",
                                       telemetry_worker_thread, NULL, &error);
    if (!thread) {
        g_critical("Failed to create telemetry daemon thread: %s", error->message);
        g_error_free(error);
    }
}
