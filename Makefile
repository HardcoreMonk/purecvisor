                          
                                                 
                                                       
                                                                  
                                                            
                                                                  
                                                                   
                                                                             
 
                      
                                                   
                                                           
                                                            
                                                   
                                                            

                                                          
                                              
CC_BASE := $(shell command -v gcc-14 >/dev/null 2>&1 && echo gcc-14 || echo gcc)

                                                     
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
    CC = ccache $(CC_BASE)
    $(info [ccache] ENABLED — $(CCACHE))
else
    CC = $(CC_BASE)
    $(info [ccache] not found — install: sudo apt install ccache)
endif

                              
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 libvirt-glib-1.0 libvirt-gobject-1.0 libvirt lxc libsoup-3.0 libcrypto

                           

                 
CFLAGS  = -std=gnu23 -Wall -Wextra -D_GNU_SOURCE -Wno-unused-parameter
CFLAGS += -Iinclude -Isrc -Iinclude/purecvisor -I.
CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += -MMD -MP

                                                 
CFLAGS += -fstack-protector-strong
CFLAGS += -D_FORTIFY_SOURCE=2
CFLAGS += -fPIE
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -Wformat=2 -Wformat-security

                                         
BUILD ?= debug
ifeq ($(BUILD),release)
    CFLAGS  += -O2 -DNDEBUG -flto=auto
    LDFLAGS_EXTRA = -pie -Wl,-z,relro,-z,now,-z,noexecstack -flto=auto
else
    CFLAGS  += -g -O0
    LDFLAGS_EXTRA = -pie -Wl,-z,relro,-z,now,-z,noexecstack
endif
LDFLAGS_EXTRA += -Wl,--gc-sections

                                                                  
CFLAGS += $(CFLAGS_EXTRA)

LDFLAGS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -lvirt -lvirt-qemu -llxc -lsqlite3 -lm -lz $(LDFLAGS_EXTRA)

                                       
ifneq ($(shell pkg-config --exists libcap 2>/dev/null && echo yes),)
    CFLAGS  += -DHAVE_LIBCAP $(shell pkg-config --cflags libcap)
    LDFLAGS += $(shell pkg-config --libs libcap)
    $(info [D-3] libcap: ENABLED)
else
    $(info [D-3] libcap: not found — capability restriction will be skipped at runtime)
endif

ifneq ($(shell pkg-config --exists libseccomp 2>/dev/null && echo yes),)
    CFLAGS  += -DHAVE_SECCOMP $(shell pkg-config --cflags libseccomp)
    LDFLAGS += $(shell pkg-config --libs libseccomp)
    $(info [D-3] libseccomp: ENABLED)
else
    $(info [D-3] libseccomp: not found — seccomp filter will be skipped at runtime)
endif

                                                       
                                                      
                                                                
                                                              
ifneq ($(shell pkg-config --exists libbpf 2>/dev/null && echo yes),)
    CFLAGS  += -DHAVE_LIBBPF $(shell pkg-config --cflags libbpf)
    LDFLAGS += $(shell pkg-config --libs libbpf)
    $(info [D07] libbpf: ENABLED ($(shell pkg-config --modversion libbpf)))
else ifneq ($(wildcard /usr/include/bpf/libbpf.h),)
    CFLAGS  += -DHAVE_LIBBPF
    LDFLAGS += -lbpf
    $(info [D07] libbpf: ENABLED (header found, no pkg-config))
else
    $(info [D07] libbpf: not found — eBPF load disabled (install libbpf-dev))
endif

                                                            
                                    
 
                                                        
                                
 
                                                  
                                    
                                                            
CLI_CFLAGS  :=
CLI_LDFLAGS :=

ifneq ($(NO_READLINE),1)
    ifneq ($(shell pkg-config --exists readline 2>/dev/null && echo yes),)
        CLI_CFLAGS  += -DHAVE_READLINE $(shell pkg-config --cflags readline)
        CLI_LDFLAGS += $(shell pkg-config --libs readline)
        $(info [H]   readline: ENABLED  — REPL 히스토리·Tab완성 활성)
    else
        $(info [H]   readline: not found — fgets fallback 사용)
        $(info [H]   설치: sudo apt install libreadline-dev)
    endif
else
    $(info [H]   readline: DISABLED (NO_READLINE=1))
endif

                                                            
                               
                                                            
URING_SRCS :=
ifneq ($(shell test -f /usr/include/liburing.h && echo yes),)
    CFLAGS  += -DHAVE_LIBURING -DPCV_URING_ENABLED=1
    LDFLAGS += -luring
    URING_SRCS = src/io/pcv_uring.c src/io/pcv_uring_buf.c src/io/pcv_uring_socket.c
    $(info [U]   liburing: ENABLED)
else
    $(info [U]   liburing: not found — GLib I/O fallback)
endif

                                                            
                                                           
 
                                              
                                                 
                                                            
EDITION ?= single
EDITION_STATE_FILE = .edition-state

ifneq ($(EDITION),single)
    $(error purecvisor-single supports EDITION=single only)
endif

CFLAGS += -DPCV_CLUSTER_ENABLED=0
$(info [EDITION] Single Edge — public standalone build)

                       

                              
COMMON_CORE_SRCS = \
    src/bootstrap/pcv_bootstrap_info.c \
    src/modules/audit/pcv_audit_chain.c \
    src/modules/dispatcher/rpc_completion.c \
    src/modules/core/vm_state.c \
    src/modules/core/cpu_allocator.c \
    src/modules/virt/vm_config_builder.c \
    src/modules/virt/vm_clone_plan.c \
    src/modules/virt/vm_manager.c \
    src/modules/virt/circuit_breaker.c \
    src/modules/daemons/alert_silence.c \
    src/modules/daemons/alert_dlq.c \
    src/modules/daemons/update_check.c \
    src/modules/virt/cancellable_map.c \
    src/modules/virt/virt_conn_pool.c \
    src/modules/storage/zfs_driver.c \
    src/modules/lxc/lxc_owner.c \
    src/utils/logger.c \
    src/utils/pcv_validate.c \
    src/utils/pcv_ssrf.c \
    src/utils/pcv_error.c \
    src/utils/pcv_log.c \
    src/utils/pcv_spawn.c \
    src/utils/pcv_config.c \
    src/utils/pcv_privdrop.c \
    src/utils/pcv_crypto.c \
    src/utils/pcv_jwt.c \
    src/utils/pcv_totp.c \
    src/utils/pcv_txn.c \
    src/utils/pcv_worker_pool.c \
    src/utils/pcv_job_queue.c \
    src/utils/pcv_zfs_lock.c \
    src/utils/pcv_bpf.c

COMMON_SINGLE_ALLOWED_NET_SRCS = \
    src/modules/network/network_manager.c \
    src/modules/network/pcv_shared_bridge.c \
    src/modules/network/network_firewall.c \
    src/modules/network/network_dhcp.c \
    src/modules/network/vpc/vpc_model.c \
    src/modules/network/vpc/vpc_policy_nft.c \
    src/modules/network/vpc/vpc_store.c \
    src/modules/network/vpc/vpc_backend_ovn.c \
    src/modules/network/vpc/vpc_manager.c \
    src/modules/network/ovs_overlay_core.c \
    src/modules/network/ovn_core.c

SINGLE_BOOTSTRAP_SRCS = \
    src/bootstrap/pcv_bootstrap_single.c \
    src/bootstrap/pcv_rpc_bootstrap_single.c \
    src/bootstrap/pcv_single_cluster_manager_stub.c \
    src/bootstrap/pcv_single_federation_stub.c \
    src/bootstrap/pcv_single_scheduler_stub.c \
    src/bootstrap/pcv_single_etcd_lock_stub.c \
    src/modules/network/ovn_single_local.c

                 
DAEMON_COMMON_SRCS = \
    src/api/uds_server.c \
    src/api/dispatcher.c \
    src/api/daemon_config_policy.c \
    src/api/snapshot_verify_probe.c \
    src/api/vm_batch_policy.c \
    src/api/drain.c \
    src/api/rest_auth.c \
    src/api/rest_server.c \
    src/api/rest_client_identity.c \
    src/api/rest_middleware.c \
    src/api/grpc_server.c \
    src/modules/daemons/telemetry.c \
    src/modules/daemons/virt_events.c \
    src/modules/daemons/pcv_undefine_debounce.c \
    src/modules/daemons/pcv_vm_death_class.c \
    src/modules/daemons/pcv_trace.c \
    src/modules/daemons/pcv_webpush_crypto.c \
    src/modules/daemons/pcv_webpush.c \
    src/modules/dispatcher/rpc_utils.c \
    src/modules/dispatcher/handler_snapshot.c \
    src/modules/dispatcher/handler_vm_start.c \
    src/modules/dispatcher/handler_vnc.c \
    src/modules/dispatcher/handler_vm_lifecycle.c \
    src/modules/dispatcher/handler_vm_hotplug.c \
    src/modules/dispatcher/hotplug_affect_policy.c \
    src/modules/dispatcher/hotplug_nic_xml.c \
    $(COMMON_SINGLE_ALLOWED_NET_SRCS) \
    src/modules/dispatcher/handler_storage.c \
    src/modules/dispatcher/handler_monitor.c \
    src/modules/lxc/lxc_driver.c \
    src/modules/dispatcher/handler_container.c \
    src/modules/network/dpdk_manager.c \
    src/modules/network/sriov_manager.c \
    src/modules/storage/iscsi_manager.c \
    src/modules/storage/pcv_iscsi_node_db.c \
    src/modules/storage/pcv_lio.c \
    src/modules/dispatcher/handler_overlay.c \
    src/modules/dispatcher/handler_accel.c \
    src/modules/dispatcher/handler_template.c \
    src/modules/template/vm_template.c \
    src/modules/dispatcher/handler_auth.c \
    src/modules/auth/pcv_rbac.c \
    src/modules/daemons/ebpf_telemetry.c \
    src/modules/daemons/cgroup_psi.c \
    src/modules/daemons/alert_engine.c \
    src/modules/daemons/process_monitor.c \
    src/modules/backup/backup_scheduler.c \
    src/modules/dispatcher/handler_backup.c \
    src/modules/dispatcher/handler_security.c \
    src/modules/dispatcher/handler_tenant_overlay.c \
    src/modules/dispatcher/handler_vpc.c \
    src/modules/security/security_event.c \
    src/modules/security/security_store.c \
    src/modules/security/security_policy.c \
    src/modules/security/hips_actions.c \
    src/modules/security/hids_file_integrity.c \
    src/modules/security/pcv_suricata.c \
    src/modules/security/pcv_suricata_rules.c \
    src/modules/security/pcv_suricata_ips.c \
    src/modules/security/pcv_suricata_ips_rules.c \
    src/api/hot_reload.c \
    src/api/ws_server.c \
    src/modules/daemons/prometheus_exporter.c \
    src/modules/audit/pcv_audit.c \
    src/modules/storage/storage_tier.c \
    src/modules/accel/gpu_manager.c \
    src/modules/plugin/pcv_plugin_manager.c \
    src/utils/pcv_tls.c \
    src/utils/pcv_secure.c \
    src/modules/network/nfv_manager.c \
    src/modules/network/security_group.c \
    src/modules/network/security_group_nft.c \
    src/modules/network/vm_iface.c \
    src/modules/network/vm_vnet_cache.c \
    src/modules/network/network_firewall_host.c \
    src/modules/network/tenant_overlay_wg.c \
    src/modules/network/tenant_overlay.c \
    src/modules/network/pcv_qos.c \
    src/modules/network/pcv_qos_chaos.c \
    src/modules/ai/anomaly_detector.c \
    src/modules/ai/workload_predict.c \
    src/modules/ai/self_healing.c \
    src/modules/ai/self_healing_restart.c \
    src/modules/ai/restart_breaker.c \
    src/modules/ai/ai_agent.c \
    src/modules/cloud/cloud_migration.c \
    src/modules/cloud/aws_client.c \
    src/modules/cloud/disk_converter.c \
    $(URING_SRCS) \
    $(COMMON_CORE_SRCS)

DAEMON_SRCS = src/main.c $(DAEMON_COMMON_SRCS) $(SINGLE_BOOTSTRAP_SRCS)

                 
                                                                    
                                                                    
                                                     
                                                                             
                                                                        
                                                                            
                                                                           
                                                                 
                                                                               
TEST_COMMON_SRCS = \
    tests/test_stubs.c \
    tests/test_main.c \
    tests/test_validate.c \
    tests/test_circuit_breaker.c \
    tests/test_restart_breaker.c \
    tests/test_self_healing_restart.c \
    tests/test_self_healing_anomaly.c \
    tests/test_alert_silence.c \
    tests/test_alert_dlq.c \
    tests/test_update_check.c \
    tests/test_cancellable_map.c \
    tests/test_cpu_allocator.c \
    tests/test_config.c \
    tests/test_vm_signals.c \
    tests/test_spawn_launcher.c \
    tests/test_jwt.c \
    tests/test_totp.c \
    tests/test_rbac_totp.c \
    tests/test_network.c \
    tests/test_vpc_model.c \
    tests/test_vpc_policy.c \
    tests/test_vpc_store.c \
    tests/test_vpc_rbac.c \
    tests/test_security_group.c \
    tests/test_sg_nft_builder.c \
    tests/test_container.c \
    tests/test_container_owner_scope.c \
    tests/test_privdrop.c \
    tests/test_ovn.c \
    tests/test_dpdk.c \
    tests/test_sriov.c \
    tests/test_uring.c \
    tests/test_handler_params.c \
    tests/test_validate_ext.c \
    tests/test_vm_config.c \
    tests/test_vm_clone_plan.c \
    tests/test_alert_basic.c \
    tests/test_backup_basic.c \
    tests/test_lxc_basic.c \
    tests/test_ws_basic.c \
    tests/test_hotreload.c \
    tests/test_txn.c \
    tests/test_worker_pool.c \
    tests/test_job_queue.c \
    tests/test_vm_state.c \
    tests/test_log.c \
    tests/test_conn_pool.c \
    tests/test_zfs.c \
    tests/test_lio.c \
    src/modules/storage/pcv_lio.c \
    tests/test_iscsi.c \
    src/modules/storage/iscsi_manager.c \
    src/modules/storage/pcv_iscsi_node_db.c \
    tests/test_vm_manager.c \
    tests/test_rest_middleware.c \
    tests/test_rest_auth.c \
    tests/test_rpc_utils.c \
    tests/test_rpc_completion.c \
    tests/test_rpc_parse_guarded.c \
    tests/test_drain.c \
    tests/test_ai_agent.c \
    tests/test_prometheus.c \
    tests/test_plugin.c \
    tests/test_snapshot_rollback.c \
    tests/test_bootstrap.c \
    tests/test_bootstrap_rpc_registration.c \
    tests/test_security_event.c \
    tests/test_security_store.c \
    tests/test_security_policy.c \
    tests/test_security_actions.c \
    tests/test_hids_file_integrity.c \
    tests/test_vm_iface.c \
    tests/test_vm_vnet_cache.c \
    tests/test_tenant_overlay.c \
    tests/test_tls_autogen.c \
    tests/test_rest_transport.c \
    tests/test_rest_client_identity.c \
    tests/test_bpf_manifest.c \
    tests/test_bpf_integration.c \
    tests/test_shared_bridge_integration.c \
    tests/test_qos.c \
    tests/test_qos_chaos.c \
    tests/test_qos_integration.c \
    tests/test_trace.c \
    tests/test_trace_integration.c \
    tests/test_undefine_debounce.c \
    src/modules/daemons/pcv_undefine_debounce.c \
    tests/test_vm_death_class.c \
    src/modules/daemons/pcv_vm_death_class.c \
    tests/test_cgroup_psi.c \
    src/modules/daemons/cgroup_psi.c \
    tests/test_suricata.c \
    tests/test_suricata_integration.c \
    tests/test_suricata_ips.c \
    tests/test_suricata_ips_rules.c \
    src/modules/security/pcv_suricata_ips_rules.c \
    tests/test_secure.c \
    tests/test_webpush_crypto.c \
    src/modules/daemons/pcv_webpush_crypto.c \
    tests/test_webpush.c \
    src/modules/daemons/pcv_webpush.c \
    src/modules/dispatcher/handler_auth.c \
    src/modules/ai/restart_breaker.c \
    src/modules/ai/self_healing_restart.c \
    src/modules/ai/self_healing.c \
    tests/test_anomaly_autowatch.c \
    src/modules/ai/anomaly_detector.c \
    src/modules/daemons/alert_engine.c \
    tests/test_apikey.c \
    tests/test_audit_chain.c \
    tests/test_rbac_user_exists.c \
    tests/test_pbkdf2_verify.c \
    tests/test_handler_snapshot_verify.c \
    tests/test_handler_vm_batch.c \
    tests/test_hotplug_flags.c \
    tests/test_hotplug_nic_xml.c \
    src/modules/security/security_event.c \
    src/modules/security/security_store.c \
    src/modules/security/security_policy.c \
    src/modules/security/hips_actions.c \
    src/modules/security/hids_file_integrity.c \
    src/modules/security/pcv_suricata.c \
    src/modules/security/pcv_suricata_rules.c \
    src/modules/security/pcv_suricata_ips.c \
    src/modules/auth/pcv_rbac.c \
    src/modules/network/ovs_overlay_core.c \
    src/modules/network/ovn_core.c \
    src/api/rest_middleware.c \
    src/api/rest_auth.c \
    src/api/rest_client_identity.c \
    src/api/drain.c \
    src/api/daemon_config_policy.c \
    src/api/snapshot_verify_probe.c \
    src/api/vm_batch_policy.c \
    src/api/hot_reload.c \
    src/modules/dispatcher/rpc_utils.c \
    src/modules/dispatcher/handler_monitor.c \
    src/modules/dispatcher/hotplug_affect_policy.c \
    src/modules/dispatcher/hotplug_nic_xml.c \
    src/modules/network/dpdk_manager.c \
    src/modules/network/sriov_manager.c \
    src/modules/network/security_group.c \
    src/modules/network/security_group_nft.c \
    src/modules/network/vm_iface.c \
    src/modules/network/vm_vnet_cache.c \
    src/modules/network/network_firewall_host.c \
    src/modules/network/network_dhcp.c \
    src/modules/network/tenant_overlay_wg.c \
    src/modules/network/tenant_overlay.c \
    src/modules/dispatcher/handler_tenant_overlay.c \
    src/modules/network/pcv_qos.c \
    src/modules/network/pcv_qos_chaos.c \
    src/modules/daemons/pcv_trace.c \
    src/utils/pcv_tls.c \
    src/utils/pcv_secure.c \
    src/modules/network/network_manager.c \
    src/modules/network/pcv_shared_bridge.c \
    src/modules/network/network_firewall.c \
    src/modules/network/vpc/vpc_model.c \
    src/modules/network/vpc/vpc_policy_nft.c \
    src/modules/network/vpc/vpc_store.c \
    src/modules/network/vpc/vpc_backend_ovn.c \
    $(URING_SRCS) \
    $(COMMON_CORE_SRCS)

SINGLE_TEST_SRCS = \
    src/bootstrap/pcv_bootstrap_single.c \
    src/bootstrap/pcv_rpc_bootstrap_single.c \
    src/bootstrap/pcv_single_cluster_manager_stub.c \
    src/modules/network/ovn_single_local.c

TEST_SRCS = \
    $(TEST_COMMON_SRCS) \
    $(SINGLE_TEST_SRCS)

CLI_SRCS = src/cli/purecvisorctl.c src/cli/cli_rpc.c src/cli/cli_output.c

                               
DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)
TEST_OBJS   = $(TEST_SRCS:.c=.o)
TEST_OBJS  += tests/rest_transport_policy.o
TEST_OBJS  += tests/dispatcher_policy.o
CLI_OBJS    = $(CLI_SRCS:.c=.o)
DEPENDS     = $(DAEMON_SRCS:.c=.d) $(TEST_SRCS:.c=.d) $(CLI_SRCS:.c=.d)

                                                   
                                                
                                                          
                                  
-include $(DEPENDS)

ALL_DAEMON_SRCS = $(DAEMON_SRCS)
ALL_TEST_SRCS = $(TEST_SRCS)
ALL_EDITION_OBJS = $(ALL_DAEMON_SRCS:.c=.o) $(ALL_TEST_SRCS:.c=.o) $(CLI_SRCS:.c=.o)
ALL_EDITION_DEPS = $(ALL_DAEMON_SRCS:.c=.d) $(ALL_TEST_SRCS:.c=.d) $(CLI_SRCS:.c=.d)
CLEAN_REPORTS = test_results.txt test_results_tap.txt valgrind_report.txt sanitize_report.txt tsan_report.txt cppcheck_report.txt
CLEAN_FUZZ_ARTIFACTS = fuzz_pcv_validate fuzz_pcv_jwt fuzz_rpc_envelope fuzz_validate.txt fuzz_jwt.txt fuzz_rpc.txt
CLEAN_COVERAGE_ARTIFACTS = compile_commands.json *.gcda *.gcno *.gcov
CLEAN_PROTO_ARTIFACTS = proto/purecvisor.pb-c.o proto/purecvisor.pb-c.d
CLEAN_UI_ARTIFACTS = $(UI_DIR)/bundle.js $(UI_DIR)/index.prod.html

$(DAEMON_OBJS) $(TEST_OBJS) $(CLI_OBJS): $(EDITION_STATE_FILE)

FORCE:

$(EDITION_STATE_FILE): FORCE
	@prev_edition="$$(cat $@ 2>/dev/null || true)"; \
	if [ "$$prev_edition" != "$(EDITION)" ]; then \
		rm -f $(ALL_EDITION_OBJS) $(ALL_EDITION_DEPS) \
		      bin/purecvisorsd $(TEST_BIN) $(CLI_BIN); \
		printf '%s\n' "$(EDITION)" > $@; \
	fi

                          
DAEMON_BIN = bin/purecvisorsd
TEST_BIN   = test_runner
AUDIT_STARTUP_TEST_BIN = tests/audit_startup_test
CLI_BIN    = bin/pcvctl

                                                            
BPF_SRC      := src/bpf/pcv_lsm.bpf.c
BPF_OBJ      := build/bpf/pcv_lsm.bpf.o
BPF_SHARED_SRC := src/bpf/pcv_shared_bridge.bpf.c
BPF_SHARED_OBJ := build/bpf/pcv_shared_bridge.bpf.o
BPF_OBJECTS  := $(BPF_OBJ) $(BPF_SHARED_OBJ)
BPF_VMLINUX  := build/bpf/vmlinux.h
BPF_MANIFEST := build/bpf/manifest.json
BPF_INSTALL  := /usr/lib/purecvisor/bpf
CLANG        ?= clang
BPFTOOL      ?= bpftool
                                                  
BPF_CFLAGS   := -g -O2 -target bpf -D__TARGET_ARCH_x86 -Isrc/bpf -Ibuild/bpf -Wall

$(BPF_VMLINUX):
	@mkdir -p build/bpf
	@command -v $(BPFTOOL) >/dev/null || { echo "❌ bpftool 미설치 (sudo apt install linux-tools-common linux-tools-$$(uname -r))"; exit 1; }
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

$(BPF_OBJ): $(BPF_SRC) src/bpf/pcv_bpf_shared.h $(BPF_VMLINUX)
	@mkdir -p build/bpf
	@command -v $(CLANG) >/dev/null || { echo "❌ clang 미설치 (sudo apt install clang)"; exit 1; }
	$(CLANG) $(BPF_CFLAGS) -c $(BPF_SRC) -o $@
	@$(BPFTOOL) btf dump file $@ format raw >/dev/null 2>&1 || { echo "❌ BPF .o에 BTF 부재 — CO-RE 불가"; exit 1; }

$(BPF_SHARED_OBJ): $(BPF_SHARED_SRC) src/bpf/pcv_shared_bridge.h $(BPF_VMLINUX)
	@mkdir -p build/bpf
	@command -v $(CLANG) >/dev/null || { echo "❌ clang 미설치 (sudo apt install clang)"; exit 1; }
	$(CLANG) $(BPF_CFLAGS) -c $(BPF_SHARED_SRC) -o $@
	@$(BPFTOOL) btf dump file $@ format raw >/dev/null 2>&1 || { echo "❌ shared bridge BPF .o에 BTF 부재 — CO-RE 불가"; exit 1; }

$(BPF_MANIFEST): $(BPF_OBJECTS) include/purecvisor/version.h
	@printf '[{"name":"pcv_lsm","file":"pcv_lsm.bpf.o","sha256":"%s","min_daemon_version":"2.0","requires":["btf","lsm-bpf"],"hooks":["bprm_check_security","file_open"]},{"name":"pcv_shared_bridge","file":"pcv_shared_bridge.bpf.o","sha256":"%s","min_daemon_version":"2.0","requires":["btf"],"hooks":["physical_ingress","physical_egress","portal_ingress"],"loader":"network-tc"}]\n' \
		"$$(sha256sum $(BPF_OBJ) | cut -d' ' -f1)" \
		"$$(sha256sum $(BPF_SHARED_OBJ) | cut -d' ' -f1)" > $@

.PHONY: bpf bpf-install
bpf: $(BPF_OBJECTS) $(BPF_MANIFEST)

bpf-install: bpf
	install -d -m 0755 $(BPF_INSTALL)
	install -m 0644 $(BPF_OBJ) $(BPF_INSTALL)/pcv_lsm.bpf.o
	install -m 0644 $(BPF_SHARED_OBJ) $(BPF_INSTALL)/pcv_shared_bridge.bpf.o
	install -m 0644 $(BPF_MANIFEST) $(BPF_INSTALL)/manifest.json
                                                            

                                                            
       
                                                            
all: $(DAEMON_BIN) $(CLI_BIN)

                                                 
                                                       
                                                            
daemon: $(DAEMON_BIN)
cli:    $(CLI_BIN)

         
$(DAEMON_BIN): $(DAEMON_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking Daemon: $@"
	$(CC) -o $@ $(DAEMON_OBJS) $(LDFLAGS)

                                                            
                            
 
                                        
                                            
                              
                                                            
src/cli/%.o: src/cli/%.c
	@echo "🔨 Compiling CLI (readline=$(if $(findstring HAVE_READLINE,$(CLI_CFLAGS)),on,off)): $<"
	$(CC) $(CFLAGS) $(CLI_CFLAGS) -c $< -o $@

                                       
$(CLI_BIN): $(CLI_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking CLI Client: $@"
	$(CC) -o $@ $(CLI_OBJS) $(LDFLAGS) $(CLI_LDFLAGS)

             
test_runner: $(TEST_OBJS)
	@echo "🔗 Linking Test Runner: $@"
	$(CC) -o $(TEST_BIN) $(TEST_OBJS) $(LDFLAGS)

$(AUDIT_STARTUP_TEST_BIN): tests/test_audit_startup.o \
                           src/modules/audit/pcv_audit.o \
                           src/modules/audit/pcv_audit_chain.o \
                           src/modules/dispatcher/rpc_completion.o
	@echo "🔗 Linking Audit Startup Test: $@"
	$(CC) -o $@ $^ $(LDFLAGS)

tests/test_audit_startup.o: tests/test_audit_startup.c \
                            src/modules/audit/pcv_audit.h \
                            src/modules/audit/pcv_audit_chain.h

                                                             
                                                 
tests/rest_transport_policy.o: src/api/rest_server.c src/api/rest_server.h
	@echo "🔨 Compiling REST transport policy seam: $<"
	$(CC) $(CFLAGS) -DPCV_REST_TRANSPORT_POLICY_ONLY -c $< -o $@

                                                                          
                                                             
                                                          
                                                           
tests/dispatcher_policy.o: src/api/dispatcher.c src/api/dispatcher.h
	@echo "🔨 Compiling RBAC policy seam: $<"
	$(CC) $(CFLAGS) -DPCV_DISPATCHER_POLICY_ONLY -c $< -o $@

          
test: test_runner $(AUDIT_STARTUP_TEST_BIN) check-q35-hotplug-xml
	@echo "🧪 Running g_test_* suite..."
	@sudo ./$(TEST_BIN) -v > test_results.txt 2>&1; \
	 status=$$?; \
	 cat test_results.txt; \
	 exit $$status
	@echo "📄 Results saved to test_results.txt"
	@./$(AUDIT_STARTUP_TEST_BIN) -v

                                                         
                                                          
test-auto: test_runner $(AUDIT_STARTUP_TEST_BIN) check-q35-hotplug-xml
	@echo "🧪 Running automated g_test_* suite (host OVS mutation excluded)..."
	@sudo ./$(TEST_BIN) -v -s /dpdk/bridge_delete/idempotent > test_results.txt 2>&1; \
	 status=$$?; \
	 cat test_results.txt; \
	 exit $$status
	@echo "📄 Results saved to test_results.txt"
	@./$(AUDIT_STARTUP_TEST_BIN) -v

                                                             
                                                                     
check-q35-hotplug-xml:
	@command -v virt-xml-validate >/dev/null 2>&1 || { echo "❌ virt-xml-validate 필요 (libvirt-clients)"; exit 1; }
	@virt-xml-validate tests/fixtures/q35-hotplug-ports.xml domain
	@echo "✅ Q35 hotplug root-port XML schema validation passed"

test-tap: test_runner
	@echo "🧪 Running g_test_* suite (TAP output)..."
	@sudo ./$(TEST_BIN) --tap > test_results_tap.txt 2>&1; \
	 status=$$?; \
	 cat test_results_tap.txt; \
	 exit $$status

                                                              
                                                                 
VPC_CLI_LIVE_BIN ?= /usr/local/bin/pcvctl
test-vpc-cli-live:
	@test "$(PCV_VPC_CLI_LIVE)" = "1" || { \
		echo "❌ PCV_VPC_CLI_LIVE=1 명시 opt-in이 필요합니다"; \
		exit 1; \
	}
	@sudo env PCV_VPC_CLI_LIVE=1 PCVCTL="$(VPC_CLI_LIVE_BIN)" \
		PCV_VPC_CLI_LIVE_FAIL_AFTER="$(PCV_VPC_CLI_LIVE_FAIL_AFTER)" \
		bash tests/integration/test_vpc_cli_live.sh

             
%.o: %.c
	@echo "🔨 Compiling: $<"
	$(CC) $(CFLAGS) -c $< -o $@

                      

                                                                     
                                                             
                                                        
memcheck: test_runner
	@echo "🔍 Running Valgrind on test suite..."
	@command -v valgrind >/dev/null || { echo "❌ valgrind 미설치 (sudo apt-get install valgrind)"; exit 1; }
	@sudo env G_SLICE=always-malloc G_DEBUG=gc-friendly valgrind \
		--leak-check=full \
		--show-leak-kinds=all \
		--errors-for-leak-kinds=definite \
		--child-silent-after-fork=yes \
		--show-realloc-size-zero=no \
		--track-origins=yes \
		--error-exitcode=1 \
		--suppressions=/usr/share/glib-2.0/valgrind/glib.supp \
		--suppressions=tests/valgrind.supp \
		./$(TEST_BIN) -v > valgrind_report.txt 2>&1; \
	 status=$$?; \
	 printf "📄 Valgrind summary (full report: valgrind_report.txt)\n"; \
	 grep -E "LEAK SUMMARY|definitely lost|indirectly lost|possibly lost|ERROR SUMMARY" valgrind_report.txt | tail -n 8 || true; \
	 exit $$status

memcheck-daemon: $(DAEMON_BIN)
	@echo "🔍 Running Valgrind on daemon (5s timeout)..."
	G_SLICE=always-malloc G_DEBUG=gc-friendly valgrind \
		--leak-check=full \
		--show-leak-kinds=all \
		--track-origins=yes \
		./$(DAEMON_BIN)

                                                            
                            
                                                     
                                                               
                                                            
BASHCOMPDIR ?= /etc/bash_completion.d
ZSHCOMPDIR  ?= /usr/share/zsh/vendor-completions

install-completion:
	@if [ -d $(BASHCOMPDIR) ]; then \
	    install -Dm644 completion/purecvisorctl.bash \
	             $(BASHCOMPDIR)/purecvisorctl; \
	    echo "✓ bash completion → $(BASHCOMPDIR)/purecvisorctl"; \
	fi
	@if [ -d $(ZSHCOMPDIR) ]; then \
	    install -Dm644 completion/purecvisorctl.zsh \
	             $(ZSHCOMPDIR)/_purecvisorctl; \
	    echo "✓ zsh  completion → $(ZSHCOMPDIR)/_purecvisorctl"; \
	fi

install-completion-user:
	@mkdir -p ~/.bash_completion.d ~/.zsh/completions
	@install -m644 completion/purecvisorctl.bash ~/.bash_completion.d/purecvisorctl
	@install -m644 completion/purecvisorctl.zsh  ~/.zsh/completions/_purecvisorctl
	@echo "✓ 사용자 completion 설치 완료"
	@echo "  bash: source ~/.bash_completion.d/purecvisorctl"
	@echo "  zsh : fpath=(~/.zsh/completions \$$fpath) && compinit"

clean:
	@echo "🧹 Cleaning up build artifacts..."
	rm -f $(DAEMON_BIN) bin/purecvisorsd \
	      $(TEST_BIN) $(AUDIT_STARTUP_TEST_BIN) $(CLI_BIN) \
	      tests/test_audit_startup.o tests/test_audit_startup.d \
	      tests/rest_transport_policy.o tests/rest_transport_policy.d \
	      tests/dispatcher_policy.o tests/dispatcher_policy.d \
	      $(ALL_EDITION_OBJS) \
	      $(ALL_EDITION_DEPS) \
	      $(CLEAN_REPORTS) $(CLEAN_FUZZ_ARTIFACTS) \
	      $(CLEAN_COVERAGE_ARTIFACTS) $(CLEAN_PROTO_ARTIFACTS) \
	      $(CLEAN_UI_ARTIFACTS) \
	      .edition-single $(EDITION_STATE_FILE)
	rm -rf $(COV_DIR) dist

                                                               
UI_DIR = ui
                                                       
                                                        
PCV_UI_VERSION := $(shell sed -n 's/.*PCV_PRODUCT_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' include/purecvisor/version.h)
                                                                        
                                                                             
                                                              
UI_MODULES = $(UI_DIR)/modules/endpoints.js $(UI_DIR)/modules/api.js $(UI_DIR)/modules/ui.js \
    $(UI_DIR)/modules/filter-state.js \
    $(UI_DIR)/modules/uxlib.js $(UI_DIR)/modules/modal-core.js $(UI_DIR)/modules/modal.js $(UI_DIR)/modules/totp.js $(UI_DIR)/modules/charts.js \
    $(UI_DIR)/modules/shell.js \
    $(UI_DIR)/modules/metrics.js \
    $(UI_DIR)/modules/security.js \
    $(UI_DIR)/modules/monitor.js \
    $(UI_DIR)/modules/vm.js $(UI_DIR)/modules/vm-console.js $(UI_DIR)/modules/vm-lifecycle.js \
    $(UI_DIR)/modules/vm-guest.js \
    $(UI_DIR)/modules/container.js $(UI_DIR)/modules/network.js $(UI_DIR)/modules/vpc.js \
    $(UI_DIR)/modules/storage.js \
    $(UI_DIR)/modules/cloud.js $(UI_DIR)/modules/help.js \
    $(UI_DIR)/modules/nav.js $(UI_DIR)/modules/theme.js \
    $(UI_DIR)/modules/accounts.js $(UI_DIR)/modules/advanced.js \
    $(UI_DIR)/modules/selfhealing.js \
    $(UI_DIR)/modules/push.js \
    $(UI_DIR)/modules/mobile.js \
    $(UI_DIR)/app.js

                                                                
                                                                        
                                                                    
UI_CACHE_INPUTS = $(UI_DIR)/index.html $(UI_DIR)/docs.html $(UI_DIR)/guide.html \
    $(UI_DIR)/guide-content.md $(UI_DIR)/offline.html $(UI_DIR)/style.css \
    $(UI_DIR)/app.bundle.js $(UI_DIR)/i18n.js $(UI_DIR)/vendor/chart.umd.min.js \
    $(UI_DIR)/vendor/novnc/novnc.esm.js $(UI_DIR)/vendor/pretendard/pretendard.css \
    $(UI_DIR)/vendor/coolicons/coolicons.svg $(UI_DIR)/manifest.json \
    $(UI_DIR)/icon-192.png $(UI_DIR)/icon-512.png

                                                      
                                                         
ui-bundle: $(UI_MODULES)
	@for f in $(UI_DIR)/modules/*.js; do \
		case " $(UI_MODULES) " in \
			*" $$f "*) ;; \
			*) echo "❌ $$f 가 UI_MODULES 에 없음 — 번들 누락(BUG-22류). Makefile UI_MODULES 갱신 필요"; exit 1;; \
		esac; \
	done
	@echo "📦 Bundling UI: $(UI_DIR)/bundle.js"
	@cat $(UI_MODULES) > $(UI_DIR)/bundle.js
	@SRC=$$(cat $(UI_MODULES) | sha1sum | cut -c1-8); \
	if npx --no-install esbuild --version >/dev/null 2>&1; then \
		npx --no-install esbuild $(UI_DIR)/bundle.js --minify \
			--target=es2020 --supported:template-literal=false --log-level=warning \
			--banner:js="const PCV_UI_SOURCE_SHA1='$$SRC';" \
			--outfile=$(UI_DIR)/app.bundle.js; \
		echo "✅ Bundle: bundle.js $$(wc -c < $(UI_DIR)/bundle.js | tr -d ' ')B → app.bundle.js $$(wc -c < $(UI_DIR)/app.bundle.js | tr -d ' ')B (minified, src-sha1 $$SRC)"; \
	else \
		{ printf "const PCV_UI_SOURCE_SHA1='%s';\\n" "$$SRC"; cat $(UI_DIR)/bundle.js; } > $(UI_DIR)/app.bundle.js; \
		echo "⚠️  esbuild 없음(npm install 필요) — 무민파이 concat 폴백: app.bundle.js $$(wc -c < $(UI_DIR)/app.bundle.js | tr -d ' ')B"; \
	fi
	@H=$$(sha1sum $(UI_CACHE_INPUTS) | sha1sum | cut -c1-8); \
	sed -i -E "s|const CACHE_NAME ?= ?['\\\"]pcv-ui-v[^'\\\"]*['\\\"]|const CACHE_NAME=\\\"pcv-ui-v$$H\\\"|" $(UI_DIR)/sw.js; \
	echo "✅ sw.js CACHE_NAME → pcv-ui-v$$H ($(words $(UI_CACHE_INPUTS))개 프리캐시 입력 자동 bump)"

ui-prod: ui-bundle
	@echo "📦 Generating production index.html"
	@sed 's|<script src="modules/[^"]*"></script>||g; s|<script src="app.js"></script>|<script src="bundle.js"></script>|' \
		$(UI_DIR)/index.html > $(UI_DIR)/index.prod.html
	@echo "✅ Production: $(UI_DIR)/index.prod.html + $(UI_DIR)/bundle.js"

                                                            
                              
 
                                                            
                                      
                                        
                                                            
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -O1 -g -U_FORTIFY_SOURCE
                                                                             
                                                    
SAN_ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=0:halt_on_error=0:print_summary=1
SAN_UBSAN_OPTIONS ?= print_stacktrace=1:halt_on_error=1

                                                            
                                            
 
                                                      
                                            
 
                                            
                                           
                                                            
FUZZ_TIME ?= 60
FUZZ_FLAGS = -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g -U_FORTIFY_SOURCE
FUZZ_INC   = -Iinclude -Iinclude/purecvisor -Isrc $(shell pkg-config --cflags glib-2.0 gio-2.0 json-glib-1.0 libcrypto)
FUZZ_LIB   = $(shell pkg-config --libs glib-2.0 gio-2.0 json-glib-1.0 libcrypto)

fuzz: fuzz_pcv_validate fuzz_pcv_jwt fuzz_rpc_envelope

fuzz_pcv_validate: tests/fuzz/fuzz_pcv_validate.c src/utils/pcv_validate.c
	@command -v clang >/dev/null || { echo "❌ clang 미설치 (sudo apt install clang)"; exit 1; }
	@echo "🐛 Building libFuzzer harness: fuzz_pcv_validate"
	clang $(FUZZ_FLAGS) $(FUZZ_INC) \
		tests/fuzz/fuzz_pcv_validate.c src/utils/pcv_validate.c src/utils/pcv_error.c \
		$(FUZZ_LIB) -o $@

fuzz_pcv_jwt: tests/fuzz/fuzz_pcv_jwt.c src/utils/pcv_jwt.c src/utils/pcv_secure.c
	@command -v clang >/dev/null || { echo "❌ clang 미설치"; exit 1; }
	@echo "🐛 Building libFuzzer harness: fuzz_pcv_jwt"
	clang $(FUZZ_FLAGS) $(FUZZ_INC) \
		tests/fuzz/fuzz_pcv_jwt.c src/utils/pcv_jwt.c src/utils/pcv_secure.c \
		$(FUZZ_LIB) -o $@

                                                    
                                                         
                                                                                
                                                                 
              
check-standalone-builds:
	@echo "🔩 Checking standalone lifecycle API and JWT fuzz link..."
	@$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -fsyntax-only tests/test_lifecycle.c
	@linked=$$(mktemp --suffix=.so); missing=$$(mktemp --suffix=.so); \
	 trap 'rm -f "$$linked" "$$missing"' EXIT; \
	 $(CC) -shared -fPIC -std=gnu23 -Wall -Wextra -D_GNU_SOURCE $(FUZZ_INC) \
		tests/fuzz/fuzz_pcv_jwt.c src/utils/pcv_jwt.c src/utils/pcv_secure.c \
		$(FUZZ_LIB) -Wl,-z,defs -o "$$linked"; \
	 if $(CC) -shared -fPIC -std=gnu23 -Wall -Wextra -D_GNU_SOURCE $(FUZZ_INC) \
		tests/fuzz/fuzz_pcv_jwt.c src/utils/pcv_jwt.c \
		$(FUZZ_LIB) -Wl,-z,defs -o "$$missing" >/dev/null 2>&1; then \
		echo "❌ JWT standalone counterfactual unexpectedly linked without pcv_secure.c"; \
		exit 1; \
	 fi

fuzz_rpc_envelope: tests/fuzz/fuzz_rpc_envelope.c
	@command -v clang >/dev/null || { echo "❌ clang 미설치"; exit 1; }
	@echo "🐛 Building libFuzzer harness: fuzz_rpc_envelope"
	clang $(FUZZ_FLAGS) $(FUZZ_INC) \
		tests/fuzz/fuzz_rpc_envelope.c \
		$(FUZZ_LIB) -o $@

FUZZ_LSAN = LSAN_OPTIONS=suppressions=tests/fuzz/lsan.supp:print_suppressions=0

fuzz-run: fuzz
	@mkdir -p tests/fuzz/corpus_validate tests/fuzz/corpus_jwt tests/fuzz/corpus_rpc
	@echo "🐛 Fuzzing pcv_validate ($(FUZZ_TIME)s)..."
	@$(FUZZ_LSAN) ./fuzz_pcv_validate -max_total_time=$(FUZZ_TIME) -print_final_stats=1 tests/fuzz/corpus_validate > fuzz_validate.txt 2>&1; \
	 status=$$?; \
	 cat fuzz_validate.txt; \
	 exit $$status
	@echo "🐛 Fuzzing pcv_jwt ($(FUZZ_TIME)s)..."
	@$(FUZZ_LSAN) ./fuzz_pcv_jwt -max_total_time=$(FUZZ_TIME) -print_final_stats=1 tests/fuzz/corpus_jwt > fuzz_jwt.txt 2>&1; \
	 status=$$?; \
	 cat fuzz_jwt.txt; \
	 exit $$status
	@echo "🐛 Fuzzing rpc_envelope ($(FUZZ_TIME)s)..."
	@$(FUZZ_LSAN) ./fuzz_rpc_envelope -max_total_time=$(FUZZ_TIME) -print_final_stats=1 tests/fuzz/corpus_rpc > fuzz_rpc.txt 2>&1; \
	 status=$$?; \
	 cat fuzz_rpc.txt; \
	 exit $$status

sanitize:
	@PCV_SANITIZE_FLAGS="$(SAN_FLAGS)" \
	 PCV_SANITIZE_ASAN_OPTIONS="$(SAN_ASAN_OPTIONS)" \
	 PCV_SANITIZE_UBSAN_OPTIONS="$(SAN_UBSAN_OPTIONS)" \
	 bash scripts/run_sanitize_gate.sh
	@echo "📄 Report: sanitize_report.txt"

                                                            
                                             
 
                                                                     
 
                                
                                                            
                                                                   
                                                               
                                                          
                                                   
                                                                
                                           
                                                       
 
                                                                    
                                 
                                                            
TSAN_FLAGS = -fsanitize=thread -fno-omit-frame-pointer -O1 -g -U_FORTIFY_SOURCE
TSAN_OPTIONS_ENV ?= halt_on_error=0:second_deadlock_stack=1:history_size=4
TSAN_TEST_FILTER ?= /selfhealing/anomaly_track_race

tsan:
	@echo "🧪 Building test_runner with ThreadSanitizer..."
	$(MAKE) clean
	@$(MAKE) CFLAGS_EXTRA="$(TSAN_FLAGS)" LDFLAGS_EXTRA="$(TSAN_FLAGS)" test_runner; \
	 status=$$?; \
	 if [ $$status -ne 0 ]; then \
	     echo "❌ TSan 빌드 실패 (status=$$status)"; \
	     $(MAKE) clean >/dev/null; \
	     exit $$status; \
	 fi
	@echo "🧪 Running TSan diagnostic (filter=$(TSAN_TEST_FILTER), ASLR off)..."
	@echo "⚠️  GMutex 관련 race 는 위양성 — 리포트를 사람이 해석할 것."
	@setarch "$$(uname -m)" -R env TSAN_OPTIONS="$(TSAN_OPTIONS_ENV)" \
	     ./$(TEST_BIN) -p $(TSAN_TEST_FILTER) -v > tsan_report.txt 2>&1; \
	 status=$$?; \
	 cat tsan_report.txt; \
	 report_tmp="$$(mktemp)"; cp tsan_report.txt "$$report_tmp"; \
	 $(MAKE) clean >/dev/null; \
	 mv "$$report_tmp" tsan_report.txt; \
	 echo "📄 Report: tsan_report.txt (진단 전용 — GMutex 위양성 주의, exit=$$status)"

release:
	$(MAKE) BUILD=release all

                       
                                                                              
                                                         
                                                                   
deb: release ui-bundle
	@command -v dpkg-deb >/dev/null 2>&1 || { echo "make deb: dpkg-deb 필요 (sudo apt install dpkg-dev)"; exit 1; }
	@command -v fakeroot >/dev/null 2>&1 || { echo "make deb: fakeroot 필요 (sudo apt install fakeroot)"; exit 1; }
	@bash packaging/deb/build-deb.sh

single:
	$(MAKE) all

multi:
	@echo "purecvisor-single is Single Edge only; use the private Multi Edge repository." >&2
	@exit 2

                   
                                                     
                                                        
                                           
test-safe:
	@./scripts/run_auto_tests.sh --ci

test-all:
	@./scripts/run_auto_tests.sh --all --ci

test-integ:
	@./scripts/run_auto_tests.sh --tier 1 --ci
	@./scripts/run_auto_tests.sh --tier 2 --ci

                        
install-hooks:
	@echo "🔗 Installing pre-commit hook..."
	@cp scripts/pre-commit .git/hooks/pre-commit
	@chmod +x .git/hooks/pre-commit
	@echo "✅ pre-commit hook installed (.git/hooks/pre-commit)"

                 
                                                    
                                           
cppcheck:
	@echo "🔍 Running cppcheck static analysis..."
	@cppcheck --enable=warning,performance,portability \
		--suppress=missingIncludeSystem \
		--suppress=unknownMacro \
		--inline-suppr \
		--error-exitcode=0 \
		-I include -I src -I include/purecvisor \
		$(shell pkg-config --cflags-only-I glib-2.0 json-glib-1.0 libvirt 2>/dev/null) \
		src/ 2>&1 | tee cppcheck_report.txt
	@ERRS=$$(grep -c '\(error\)' cppcheck_report.txt 2>/dev/null || echo 0); \
	 WARNS=$$(grep -c '\(warning\)' cppcheck_report.txt 2>/dev/null || echo 0); \
	 PERFS=$$(grep -c '\(performance\)' cppcheck_report.txt 2>/dev/null || echo 0); \
	 echo "📊 cppcheck: $$ERRS error(s), $$WARNS warning(s), $$PERFS performance"
	@echo "📄 Report: cppcheck_report.txt"

cppcheck-strict: cppcheck
	@ERRS=$$(grep -c '\(error\)' cppcheck_report.txt 2>/dev/null || echo 0); \
	 if [ "$$ERRS" -gt 0 ]; then echo "❌ cppcheck errors found"; exit 1; fi
	@echo "✅ cppcheck strict: 0 errors"

check-rbac:
	@echo "🔐 Running ADR-0019 RBAC policy gate..."
	@python3 scripts/check_rbac_policies.py

check-secret-wipe:
	@echo "🔐 Running secret free-without-wipe gate (OVL-1 후속 완전성)..."
	@python3 scripts/check_secret_wipe.py --selftest
	@python3 scripts/check_secret_wipe.py

                                                                          
                                                                
                                                            
                                                       
                                                             
                                                  
                                                    
                                                   
                                                                         
             
check-rpc-consumers:
	@echo "🔗 Running AF-C4 RPC consumer contract gate (소비 ⊆ 등록)..."
	@python3 scripts/check_rpc_consumers.py
	@python3 scripts/tests/test_rpc_extract.py
	@python3 scripts/tests/test_orphan_gate.py
	@python3 scripts/tests/test_rpc_consumers_acceptance.py

check-dead-exports:
	@echo "🧹 Running dead export 게이트 (헤더 선언 사용처0)..."
	@python3 scripts/check_dead_exports.py
	@python3 scripts/tests/test_dead_exports.py

check-rpc-param-contract:
	@echo "🔑 Running RPC param-key contract gate (Stage 2)..."
	@python3 scripts/check_rpc_param_contract.py
	@python3 scripts/tests/test_param_gate.py
	@python3 scripts/tests/test_handler_extract.py
	@python3 scripts/tests/test_consumer_keys.py
	@python3 scripts/tests/test_contract_data.py
	@python3 scripts/tests/test_gate_acceptance.py

check-json-ingress:
	@echo "🛡  Running JSON 파싱 초크포인트 게이트..."
	@python3 scripts/check_json_ingress.py
	@python3 scripts/tests/test_json_ingress.py

check-safety-controls:
	@echo "🛟 Running 안전통제 효과 테스트 레지스트리 게이트..."
	@python3 scripts/check_safety_controls.py
	@python3 scripts/tests/test_safety_controls.py
	@python3 scripts/tests/test_safety_controls_acceptance.py

check-error-codes:
	@echo "🔢 Running raw 에러코드 리터럴 방지 게이트 (DISP-6)..."
	@python3 scripts/check_error_codes.py
	@python3 scripts/tests/test_error_codes.py

check-cli-exit-status: $(CLI_BIN)
	@echo "🧭 Running pcvctl 종료상태 0/1/2 production 효과 테스트..."
	@bash tests/integration/test_cli_exit_status.sh
	@echo "🧭 Running pcvctl 단계적 도움말·메뉴 가독성 테스트..."
	@bash tests/integration/test_cli_help_readability.sh
	@echo "🌐 Running pcvctl Local VPC route/params/terminal Job 효과 테스트..."
	@bash tests/integration/test_vpc_cli_surface.sh

check-audit-placement:
	@echo "📋 Running audit 배치 계약 게이트 (ADR-0018 — async registry/audit/WS completion)..."
	@python3 scripts/check_audit_placement.py
	@python3 scripts/tests/test_audit_placement.py

check-cors-anchor:
	@echo "🌐 Running CORS 오리진 앵커 검증 게이트 (Wave A / A05·V3·V13)..."
	@python3 scripts/check_cors_anchor.py
	@python3 scripts/tests/test_cors_anchor.py

check-secret-logging:
	@echo "🔒 Running 감사 로그 자격증명 마스킹 게이트 (Wave A / A09·V14·V16)..."
	@python3 scripts/check_secret_logging.py
	@python3 scripts/tests/test_secret_logging.py

check-ssrf-guard:
	@echo "🚫 Running 아웃바운드 리다이렉트 금지 게이트 (Wave A / A10·V4)..."
	@python3 scripts/check_ssrf_guard.py
	@python3 scripts/tests/test_ssrf_guard.py

check-grpc-authz:
	@echo "🔐 Running gRPC 인증/RBAC 게이트 (Wave B / A01·V8)..."
	@python3 scripts/check_grpc_authz.py
	@python3 scripts/tests/test_grpc_authz.py

check-ssrf-target-guard:
	@echo "🎯 Running 아웃바운드 대상 SSRF allowlist 게이트 (Wave B / A10·V4)..."
	@python3 scripts/check_ssrf_target_guard.py
	@python3 scripts/tests/test_ssrf_target_guard.py

check-audit-hashchain:
	@echo "⛓  Running 감사 로그 해시체인 게이트 (Wave B / A09·2.9)..."
	@python3 scripts/check_audit_hashchain.py
	@python3 scripts/tests/test_audit_hashchain.py

check-rng-safe:
	@echo "🎲 Running 보안 RNG/PBKDF2 하드닝 게이트 (Wave B / A02·V11)..."
	@python3 scripts/check_rng_safe.py
	@python3 scripts/tests/test_rng_safe.py

check-uds-authz:
	@echo "🔌 Running UDS root-only 접근 게이트 (Wave C / A01·V8)..."
	@python3 scripts/check_uds_authz.py
	@python3 scripts/tests/test_uds_authz.py

check-transport-bind:
	@echo "🌐 Running 평문 전송 루프백 바인딩 게이트 (Wave C / A02·V12)..."
	@python3 scripts/check_transport_bind.py
	@python3 scripts/tests/test_transport_bind.py

check-proxy-identity:
	@echo "🪪 Running 프록시 클라이언트 신원 단일 경계 게이트..."
	@python3 scripts/check_proxy_identity.py
	@python3 scripts/tests/test_proxy_identity.py

check-container-owner-scope:
	@echo "📦 Running 컨테이너 operator owner-scope 게이트 (B1 / A01 IDOR)..."
	@python3 scripts/check_container_owner_scope.py
	@python3 scripts/tests/test_container_owner_scope.py

check-mtls-wiring:
	@echo "🔐 Running mTLS 클라이언트 인증서 검증 배선 게이트 (C1 / A02·V12)..."
	@python3 scripts/check_mtls_wiring.py
	@python3 scripts/tests/test_mtls_wiring.py

check-tls-min-version:
	@echo "🔒 Running TLS 최소 버전 고정 게이트 (C2 / A02·V11·V12)..."
	@python3 scripts/check_tls_min_version.py
	@python3 scripts/tests/test_tls_min_version.py

check-security-headers:
	@echo "🛡  Running /ui 정적 응답 보안 헤더 게이트 (Q-1 / A05)..."
	@python3 scripts/check_security_headers.py
	@python3 scripts/tests/test_security_headers.py

check-password-policy:
	@echo "🔑 Running user-create 비밀번호 복잡도 정책 게이트 (Q-2 / A07)..."
	@python3 scripts/check_password_policy.py
	@python3 scripts/tests/test_password_policy.py

check-ws-token-url:
	@echo "🔗 Running WS URL-query 토큰 인증 제거 게이트 (Q-5 / A07)..."
	@python3 scripts/check_ws_token_url.py
	@python3 scripts/tests/test_ws_token_url.py

check-zpool-suspend-recover:
	@echo "💽 Running ZFS 풀 SUSPENDED 탐지+가드된 자동복구 게이트..."
	@python3 scripts/check_zpool_suspend_recover.py
	@python3 scripts/tests/test_zpool_suspend_recover.py

check-deb-apparmor:
	@echo "🛡  Running 2.0 deb AppArmor 미부착 게이트 (ADR-0028)..."
	@python3 scripts/check_deb_apparmor.py
	@python3 scripts/tests/test_deb_apparmor.py

check-public-comments: check-standalone-builds test_runner
	@echo "🧹 Running 공개 소스 주석 제거 게이트..."
	@python3 scripts/strip_source_comments.py --check
	@node scripts/check_javascript_comments.mjs
	@python3 scripts/check_spawn_capability_contract.py --self-test
	@python3 scripts/check_spawn_capability_contract.py
	@python3 scripts/check_test_runner_selectors.py --self-test
	@python3 scripts/check_test_runner_selectors.py --test-runner ./$(TEST_BIN)
	@python3 scripts/tests/test_script_safety_contracts.py
	@bash tests/integration/test_live_cleanup_contract.sh
	@bash tests/integration/test_public_comment_policy.sh

                                                               
                                                                            
                                                   
                                                            
                                                                
                            
check-vendor-integrity:
	@echo "📦 Running 벤더 자산 무결성 핀 게이트 (A03)..."
	@python3 scripts/check_vendor_integrity.py
	@python3 scripts/tests/test_vendor_integrity.py

check-npm-lockfile:
	@echo "🔒 Running npm 의존 핀·무결성 게이트 (A03)..."
	@python3 scripts/check_npm_lockfile.py
	@python3 scripts/tests/test_npm_lockfile.py

check-deb-supply-chain:
	@echo "🚚 Running deb 산출물 공급망 계약 게이트 (A03)..."
	@python3 scripts/check_deb_supply_chain.py
	@python3 scripts/tests/test_deb_supply_chain.py

                                                             
                                                                                   
 
                                                                  
                                                           
                              
                                                                   
                                                                              
                                                                            
check-fe-rpc-params:
	@echo "🔗 Running FE 요청 파라미터 계약 게이트..."
	@python3 scripts/check_fe_rpc_params.py
	@python3 scripts/check_fe_rpc_params.py --self-test
	@python3 scripts/tests/test_fe_rpc_params_acceptance.py

check-network-mode-contract:
	@echo "🔗 Running network.mode_set FE↔BE enum 계약 게이트..."
	@python3 scripts/check_network_mode_contract.py
	@python3 scripts/check_network_mode_contract.py --self-test
	@python3 scripts/tests/test_network_mode_contract.py

check-iscsi-chap-argv:
	@echo "🔐 Running iSCSI initiator CHAP argv 제거 게이트..."
	@python3 scripts/check_iscsi_chap_argv.py
	@python3 scripts/tests/test_iscsi_chap_argv.py

check-rpc-route-unique:
	@echo "🔀 Running RPC 라우트 중복 등록 게이트..."
	@python3 scripts/check_rpc_route_unique.py
	@python3 scripts/check_rpc_route_unique.py --self-test

check-rerror-guard:
	@echo "🛡  Running fetch r.error 가드 래칫..."
	@python3 scripts/check_rerror_guard.py
	@python3 scripts/check_rerror_guard.py --self-test
	@python3 scripts/tests/test_rerror_guard_acceptance.py

                                                                       
                                               
 
                                                    
                                                                       
                                              
                                                          
                                                        
                          
check-help-counts:
	@echo "🔢 Running help 카운트 정본 게이트..."
	@python3 scripts/check_help_counts.py --self-test
	@python3 scripts/check_help_counts.py

check-runtime-prereqs:
	@command -v strace >/dev/null || { \
		echo "❌ strace 미설치 (sudo apt install strace)"; \
		exit 1; \
	}
	@bash tests/integration/test_runtime_prereq_install.sh
	@bash tests/integration/test_nginx_termination_install.sh
	@bash tests/integration/test_deploy_runtime_prereq_contract.sh
	@bash tests/integration/test_host_tuning_install.sh
	@bash tests/integration/test_lio_modules_load_packaging.sh

                                                           
check-all: check-rbac check-rpc-consumers check-dead-exports check-rpc-param-contract check-json-ingress check-safety-controls check-error-codes check-cli-exit-status check-audit-placement check-cors-anchor check-secret-logging check-ssrf-guard check-grpc-authz check-ssrf-target-guard check-audit-hashchain check-rng-safe check-uds-authz check-transport-bind check-proxy-identity check-container-owner-scope check-mtls-wiring check-tls-min-version check-secret-wipe check-security-headers check-password-policy check-ws-token-url check-zpool-suspend-recover check-deb-apparmor check-public-comments check-vendor-integrity check-npm-lockfile check-deb-supply-chain check-fe-rpc-params check-network-mode-contract check-iscsi-chap-argv check-rpc-route-unique check-rerror-guard check-runtime-prereqs
	@echo "✅ 계약 게이트 전체 통과 (38게이트: RBAC + RPC consumers + dead exports + param contract + JSON ingress + safety controls + error codes + CLI exit status + audit placement + CORS anchor + secret logging + SSRF guard + gRPC authz + SSRF target guard + audit hashchain + RNG safe + UDS authz + transport bind + proxy identity + container owner-scope + mTLS wiring + TLS min-version + secret wipe + security headers + password policy + WS token URL + zpool suspend-recover + deb AppArmor 미부착 + public comments + 벤더 자산 무결성 + npm 의존 핀 + deb 공급망 + FE 요청 파라미터 + network mode enum + iSCSI CHAP argv 제거 + RPC 라우트 중복 등록 + r.error 가드 + runtime prerequisites)"

compile-commands:
	@echo "📝 Generating compile_commands.json..."
	@echo "[" > compile_commands.json
	@first=1; for f in $(DAEMON_SRCS) $(CLI_SRCS); do \
		[ $$first -eq 0 ] && echo "," >> compile_commands.json; first=0; \
		echo "  {\"directory\": \"$(CURDIR)\", \"command\": \"$(CC) $(CFLAGS) -c $$f\", \"file\": \"$$f\"}" >> compile_commands.json; \
	done
	@echo "]" >> compile_commands.json
	@echo "✅ compile_commands.json generated ($$(wc -l < compile_commands.json) lines)"

                   
                                          
COV_DIR = coverage_report
COV_MIN ?= 52
coverage:
	@echo "📊 Building with coverage instrumentation..."
	$(MAKE) clean
	$(MAKE) CFLAGS_EXTRA="--coverage" LDFLAGS_EXTRA="--coverage" test_runner
	@echo "🧪 Running tests with coverage..."
	@sudo ./$(TEST_BIN) -v > /dev/null 2>&1 || true
	@echo "📈 Generating coverage report..."
	@mkdir -p $(COV_DIR)
	@gcov -o . $(DAEMON_SRCS) 2>/dev/null | grep -A1 "File 'src/" | head -60
	@echo "📄 Coverage files: *.gcov (use 'lcov' for HTML report)"
	@echo "   Install lcov: sudo apt install lcov"
	@echo "   HTML report:  make coverage-html"

coverage-html: coverage
	@if command -v lcov >/dev/null 2>&1; then \
		echo "📊 Generating HTML coverage report..."; \
		lcov --capture --directory . --output-file $(COV_DIR)/coverage.info \
			--ignore-errors mismatch 2>/dev/null; \
		lcov --remove $(COV_DIR)/coverage.info '/usr/*' 'tests/*' 'proto/*' \
			--output-file $(COV_DIR)/coverage_filtered.info 2>/dev/null; \
		genhtml $(COV_DIR)/coverage_filtered.info --output-directory $(COV_DIR)/html \
			--title "PureCVisor Coverage" 2>/dev/null; \
		echo "✅ HTML report: $(COV_DIR)/html/index.html"; \
	else \
		echo "❌ lcov not installed. Run: sudo apt install lcov"; \
		exit 1; \
	fi

                                                              
                                       
coverage-check: coverage-html
	@PCT=$$(lcov --summary $(COV_DIR)/coverage_filtered.info 2>/dev/null | \
	        grep -oP 'lines\.\.\.\.\.\.: \K[0-9.]+' | head -1); \
	 echo "📊 Line coverage: $${PCT}%"; \
	 if [ -z "$$PCT" ]; then echo "❌ coverage 측정 실패"; exit 1; fi; \
	 awk "BEGIN { exit !($$PCT < $(COV_MIN)) }" && \
	     { echo "❌ coverage $${PCT}% < $(COV_MIN)% 임계값"; exit 1; } || \
	     echo "✅ coverage $${PCT}% ≥ $(COV_MIN)%"

.PHONY: all clean release deb single multi test_runner test test-auto test-tap test-vpc-cli-live \
        memcheck memcheck-daemon daemon cli sanitize tsan fuzz fuzz-run check-standalone-builds \
        install-completion install-completion-user ui-bundle ui-prod \
        install-hooks test-safe test-all test-integ \
        cppcheck cppcheck-strict check-rbac check-secret-wipe check-rpc-consumers check-dead-exports check-rpc-param-contract check-json-ingress check-safety-controls check-error-codes check-cli-exit-status check-audit-placement check-cors-anchor check-secret-logging check-ssrf-guard check-grpc-authz check-ssrf-target-guard check-audit-hashchain check-rng-safe check-uds-authz check-transport-bind check-proxy-identity check-container-owner-scope check-mtls-wiring check-tls-min-version check-security-headers check-password-policy check-ws-token-url check-zpool-suspend-recover check-deb-apparmor check-public-comments check-help-counts check-runtime-prereqs \
        check-vendor-integrity check-npm-lockfile check-deb-supply-chain \
        check-fe-rpc-params check-network-mode-contract check-iscsi-chap-argv check-rpc-route-unique check-rerror-guard check-q35-hotplug-xml \
        check-all compile-commands coverage coverage-html coverage-check
