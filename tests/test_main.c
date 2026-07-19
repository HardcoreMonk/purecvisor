
#include <glib.h>

#include <sched.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

void test_validate_register(void);
void test_circuit_breaker_register(void);
void test_restart_breaker_register(void);
void test_self_healing_restart_register(void);
void test_self_healing_anomaly_register(void);
void test_cancellable_map_register(void);
void test_cpu_allocator_register(void);
void test_config_register(void);
void test_vm_signals_register(void);
void test_spawn_launcher_register(void);
void test_jwt_register(void);
void test_network_register(void);
void test_security_group_register(void);
void test_sg_nft_builder_register(void);
void test_container_register(void);
void test_container_owner_scope_register(void);
void test_privdrop_register(void);
void test_ovn_register(void);
void test_dpdk_register(void);
void test_sriov_register(void);
void test_uring_register(void);
void test_handler_params_register(void);
void test_validate_ext_register(void);
void test_vm_config_register(void);
void test_vm_clone_plan_register(void);
void test_alert_basic_register(void);
void test_alert_silence_register(void);
void test_alert_dlq_register(void);
void test_update_check_register(void);
void test_backup_basic_register(void);
void test_lxc_basic_register(void);
void test_ws_basic_register(void);
void test_hotreload_register(void);
void test_txn_register(void);
void test_worker_pool_register(void);
void test_job_queue_register(void);
void test_vm_state_register(void);
void test_log_register(void);
void test_conn_pool_register(void);
void test_zfs_register(void);
void test_vm_manager_register(void);
void test_rest_middleware_register(void);
void test_rest_auth_register(void);
void test_rpc_utils_register(void);
void test_rpc_parse_guarded_register(void);
void test_drain_register(void);
void test_ai_agent_register(void);
void test_prometheus_register(void);
void test_plugin_register(void);
void test_snapshot_rollback_register(void);
void test_bootstrap_register(void);
void test_bootstrap_rpc_registration_register(void);
void test_security_event_register(void);
void test_security_store_register(void);
void test_security_policy_register(void);
void test_security_actions_register(void);
void test_hids_file_integrity_register(void);
void test_vm_iface_register(void);
void test_vm_vnet_cache_register(void);
void test_apikey_register(void);
void test_rbac_user_exists_register(void);
void test_pbkdf2_verify_register(void);
void test_handler_snapshot_verify_register(void);
void test_handler_vm_batch_register(void);
void test_hotplug_flags_register(void);
void test_audit_chain_register(void);

static void
_test_log_handler(const gchar    *log_domain,
                  GLogLevelFlags  log_level,
                  const gchar    *message,
                  gpointer        user_data)
{
    (void)user_data;

    if (log_level & G_LOG_LEVEL_WARNING) {

        g_test_message("WARN [%s] %s",
                       log_domain ? log_domain : "?", message);
        return;
    }

    g_log_default_handler(log_domain, log_level, message, user_data);
}

static void
_isolate_netns(void)
{
    if (geteuid() != 0) {
        return;
    }

    if (unshare(CLONE_NEWNET) != 0) {
        fprintf(stderr,
                "FATAL: root 로 test_runner 를 실행하려 했으나 network "
                "namespace 격리(unshare(CLONE_NEWNET))에 실패했습니다: %s\n"
                "격리 없이 root 로 테스트를 계속하면 /security_group 스위트가 "
                "호스트 네트워크에 nft drop 체인을 설치해 네트워크 전체가 "
                "다운될 수 있습니다 (2026-07-04 gti12 장애 재현). 실행을 "
                "중단합니다.\n",
                strerror(errno));
        _exit(1);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr,
                "FATAL: netns 격리 후 lo 설정용 소켓 생성 실패: %s\n",
                strerror(errno));
        _exit(1);
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
        fprintf(stderr,
                "FATAL: lo 인터페이스 플래그 조회 실패: %s\n", strerror(errno));
        close(sock);
        _exit(1);
    }

    ifr.ifr_flags |= IFF_UP;

    if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0) {
        fprintf(stderr,
                "FATAL: lo 인터페이스 UP 설정 실패: %s\n", strerror(errno));
        close(sock);
        _exit(1);
    }

    close(sock);
}

int main(int argc, char *argv[]) {
    _isolate_netns();

    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);

    g_test_init(&argc, &argv, NULL);

    g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_FLAG_RECURSION);

    g_log_set_handler(NULL,
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);

    g_log_set_handler("circuit_breaker",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);
    g_log_set_handler("restart_breaker",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);
    g_log_set_handler("conn_pool",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);

    test_validate_register();
    test_circuit_breaker_register();
    test_restart_breaker_register();
    test_self_healing_restart_register();
    test_self_healing_anomaly_register();
    test_cancellable_map_register();
    test_cpu_allocator_register();
    test_config_register();
    test_vm_signals_register();
    test_spawn_launcher_register();
    test_jwt_register();
    test_network_register();
    test_security_group_register();
    test_sg_nft_builder_register();
    test_container_register();
    test_container_owner_scope_register();
    test_privdrop_register();
    test_ovn_register();
    test_sriov_register();
    test_uring_register();
    test_handler_params_register();
    test_validate_ext_register();
    test_vm_config_register();
    test_vm_clone_plan_register();
    test_alert_basic_register();
    test_alert_silence_register();
    test_alert_dlq_register();
    test_update_check_register();
    test_backup_basic_register();
    test_lxc_basic_register();
    test_ws_basic_register();
    test_hotreload_register();
    test_txn_register();
    test_worker_pool_register();
    test_job_queue_register();
    test_vm_state_register();
    test_log_register();
    test_conn_pool_register();
    test_zfs_register();
    test_vm_manager_register();
    test_rest_middleware_register();
    test_rest_auth_register();
    test_rpc_utils_register();
    test_rpc_parse_guarded_register();
    test_drain_register();
    test_ai_agent_register();
    test_prometheus_register();
    test_plugin_register();
    test_snapshot_rollback_register();
    test_bootstrap_register();
    test_bootstrap_rpc_registration_register();
    test_security_event_register();
    test_security_store_register();
    test_security_policy_register();
    test_security_actions_register();
    test_hids_file_integrity_register();
    test_vm_iface_register();
    test_vm_vnet_cache_register();
    test_apikey_register();
    test_rbac_user_exists_register();
    test_pbkdf2_verify_register();
    test_handler_snapshot_verify_register();
    test_handler_vm_batch_register();
    test_hotplug_flags_register();
    test_audit_chain_register();
    test_dpdk_register();

    return g_test_run();
}
