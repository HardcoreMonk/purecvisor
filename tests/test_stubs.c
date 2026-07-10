/* tests/test_stubs.c
 *
 * ============================================================================
 *  이 파일이 존재하는 이유 (WHY stubs exist)
 * ============================================================================
 *  테스트 바이너리(test_runner)는 DAEMON_SRCS를 링크하지 않는다.
 *  그런데 COMMON_SRCS에 속하는 vm_manager.c가 Single Edge 런타임 경계와
 *  OVN 관리 함수 일부를 호출한다:
 *
 *    vm_manager.c (COMMON_SRCS)
 *        → pcv_cluster_sync_vm_xml()     -- Single Edge에서는 no-op
 *        → pcv_cluster_remove_vm_xml()   -- Single Edge에서는 no-op
 *        → pcv_ovn_switch_list()         -- ovn_manager.c (DAEMON_SRCS)
 *
 *  테스트 빌드 시 이 함수들의 심볼이 해결되지 않아 링크 에러가 발생한다.
 *  이 파일이 빈 구현(스텁)을 제공하여 링크 에러를 해결한다.
 *
 *  스텁의 동작 규칙:
 *  - 클러스터 동기화 함수 (sync/remove/notify): 아무 동작도 하지 않음
 *  - pcv_cluster_check_zvol_fence(): 항상 TRUE (펜싱 검사 통과)
 *  - pcv_ovn_is_available(): 항상 FALSE (OVN 미설치 시뮬레이션)
 *  - OVN 목록 함수 (switch_list 등): 빈 JsonArray 반환
 *  - OVN 삭제 함수 (switch_delete 등): 항상 TRUE (멱등 삭제)
 *  - pcv_ovn_status(): {"available": false} 반환
 *
 *  Single Edge 빌드에서도 사용: PCV_CLUSTER_ENABLED=0일 때 cluster .c 파일이
 *  제외되므로 동일한 스텁이 필요하다.
 * ============================================================================
 */

#include <glib.h>

/* ── 클러스터 관련 스텁 ──
 * vm_manager.c에서 VM 생성/삭제 시 클러스터 동기화를 호출한다.
 * 테스트 환경에서는 동기화가 불필요하므로 빈 구현. */
void pcv_cluster_sync_vm_xml(const gchar *name __attribute__((unused)),
                              const gchar *xml __attribute__((unused))) { }
void pcv_cluster_remove_vm_xml(const gchar *name __attribute__((unused))) { }
void pcv_cluster_notify_config_reload(void) { }
gboolean pcv_cluster_check_zvol_fence(void) { return TRUE; }

/* ── VP-1 기본 NAT 네트워크 ensure 스텁 ──
 * pcv_bootstrap_single.c(SINGLE_TEST_SRCS)의 init_runtime_network가 network 모듈
 * (DAEMON_SRCS, test_runner 미링크)의 bridge/nat/dhcp ensure를 호출한다.
 * 테스트는 이 함수 주소만 non-null 검사하고 실행하지 않으므로, 링크 해결용
 * 성공 no-op 스텁으로 충분하다 (network_manager.c 전체 링크 시 RPC 핸들러 연쇄 회피). */
gboolean network_bridge_create(const gchar *bridge_name __attribute__((unused)),
                               const gchar *cidr __attribute__((unused)),
                               gint mtu __attribute__((unused)),
                               GError **error __attribute__((unused))) { return TRUE; }
gboolean network_firewall_setup_nat(const gchar *bridge_name __attribute__((unused)),
                                    const gchar *cidr __attribute__((unused)),
                                    GError **error __attribute__((unused))) { return TRUE; }
gboolean network_dhcp_start(const gchar *bridge_name __attribute__((unused)),
                            const gchar *cidr __attribute__((unused)),
                            GError **error __attribute__((unused))) { return TRUE; }
/* bootstrap의 기본 네트워크 meta 기록 (VP-5 잔여) */
void pcv_network_meta_save(const gchar *bridge_name __attribute__((unused)),
                           const gchar *mode __attribute__((unused)),
                           const gchar *cidr __attribute__((unused))) { }
/* bootstrap의 기본 네트워크 ensure(31273a3)가 DNS 포함 변형을 호출 */
gboolean network_dhcp_start_ex(const gchar *bridge_name __attribute__((unused)),
                               const gchar *cidr __attribute__((unused)),
                               gboolean dns_enabled __attribute__((unused)),
                               const gchar *upstream_dns __attribute__((unused)),
                               GError **error __attribute__((unused))) { return TRUE; }

/* ── Prometheus exporter stub (1.0 ZFS lock 메트릭) ──
 * pcv_zfs_lock.c가 PCV가 prom_gauge_set_labels로 메트릭을 push.
 * 테스트에서는 무시. */
void pcv_prom_gauge_set_labels(const gchar *n __attribute__((unused)),
    const gchar *l __attribute__((unused)),
    double v __attribute__((unused))) { }

/* ── fire-and-forget audit / WebSocket stubs ──
 * vm_manager.c의 비동기 작업 완료 경로가 audit와 WS 완료 이벤트를 호출한다.
 * test_runner는 실제 audit worker와 WebSocket 서버를 링크하지 않으므로, 테스트
 * 환경에서는 no-op으로 두어 코어 단위 테스트가 외부 데몬 상태에 묶이지 않게 한다. */
static gint g_test_audit_call_count = 0;
static gchar g_test_audit_last_method[128] = {0};
static gchar g_test_audit_last_target[256] = {0};
static gint g_test_alert_call_count = 0;
static gchar g_test_alert_last_event_id[128] = {0};

void pcv_test_audit_reset(void)
{
    g_test_audit_call_count = 0;
    g_test_audit_last_method[0] = '\0';
    g_test_audit_last_target[0] = '\0';
}

gint pcv_test_audit_call_count(void)
{
    return g_test_audit_call_count;
}

const gchar *pcv_test_audit_last_method(void)
{
    return g_test_audit_last_method;
}

const gchar *pcv_test_audit_last_target(void)
{
    return g_test_audit_last_target;
}

void pcv_test_alert_reset(void)
{
    g_test_alert_call_count = 0;
    g_test_alert_last_event_id[0] = '\0';
}

gint pcv_test_alert_call_count(void)
{
    return g_test_alert_call_count;
}

const gchar *pcv_test_alert_last_event_id(void)
{
    return g_test_alert_last_event_id;
}

void pcv_alert_record_security_event(const gchar *event_id,
                                     const gchar *severity __attribute__((unused)),
                                     const gchar *summary __attribute__((unused)))
{
    g_test_alert_call_count++;
    g_strlcpy(g_test_alert_last_event_id, event_id ? event_id : "",
              sizeof g_test_alert_last_event_id);
}

void pcv_audit_log(const gchar *username __attribute__((unused)),
                   const gchar *method,
                   const gchar *target,
                   const gchar *result __attribute__((unused)),
                   gint error_code __attribute__((unused)),
                   gint64 duration_ms __attribute__((unused)),
                   const gchar *src_ip __attribute__((unused)))
{
    g_test_audit_call_count++;
    g_strlcpy(g_test_audit_last_method, method ? method : "",
              sizeof g_test_audit_last_method);
    g_strlcpy(g_test_audit_last_target, target ? target : "",
              sizeof g_test_audit_last_target);
}

void pcv_ws_broadcast_job_complete(const gchar *job_id __attribute__((unused)),
                                   const gchar *method __attribute__((unused)),
                                   const gchar *status __attribute__((unused)),
                                   const gchar *error_msg __attribute__((unused))) { }

gboolean pcv_rbac_apikey_revoke(const gchar *client_name __attribute__((unused)),
                                GError **error __attribute__((unused)))
{
    return TRUE;
}

/* ── ZFS pool 분산 락 stubs (BUG-18 Phase 2) ──
 * zfs_driver.c가 PCV_CLUSTER_ENABLED일 때 etcd inflight lock을 호출.
 * 테스트 환경에서는 etcd 없으므로 stub. */
typedef struct PcvEtcdClient PcvEtcdClient;
PcvEtcdClient *pcv_cluster_get_etcd(void) { return NULL; }
gboolean pcv_etcd_acquire_inflight_lock(PcvEtcdClient *c __attribute__((unused)),
    const gchar *p __attribute__((unused)),
    const gchar *n __attribute__((unused)),
    const gchar *o __attribute__((unused)),
    gint t __attribute__((unused)),
    GError **e __attribute__((unused))) { return TRUE; }
gboolean pcv_etcd_release_inflight_lock(PcvEtcdClient *c __attribute__((unused)),
    const gchar *p __attribute__((unused)),
    GError **e __attribute__((unused))) { return TRUE; }
gint pcv_etcd_compute_inflight_ttl(const gchar *op __attribute__((unused)),
    gint size_gb __attribute__((unused))) { return 60; }
