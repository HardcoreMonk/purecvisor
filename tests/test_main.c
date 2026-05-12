


























#include <glib.h>

void test_validate_register(void);
void test_circuit_breaker_register(void);
void test_cancellable_map_register(void);
void test_cpu_allocator_register(void);
void test_config_register(void);
void test_vm_signals_register(void);
void test_spawn_launcher_register(void);
void test_jwt_register(void);
void test_network_register(void);
void test_container_register(void);
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

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);






    g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_FLAG_RECURSION);


    g_log_set_handler(NULL,
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);

    g_log_set_handler("circuit_breaker",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);
    g_log_set_handler("conn_pool",
                      G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL,
                      _test_log_handler, NULL);

    test_validate_register();
    test_circuit_breaker_register();
    test_cancellable_map_register();
    test_cpu_allocator_register();
    test_config_register();
    test_vm_signals_register();
    test_spawn_launcher_register();
    test_jwt_register();
    test_network_register();
    test_container_register();
    test_privdrop_register();
    test_ovn_register();
    test_sriov_register();
    test_uring_register();
    test_handler_params_register();
    test_validate_ext_register();
    test_vm_config_register();
    test_vm_clone_plan_register();
    test_alert_basic_register();
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
    test_dpdk_register();

    return g_test_run();
}
