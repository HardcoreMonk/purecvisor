# ==========================================================
# PureCvisor Async Hypervisor Orchestrator Makefile
# ==========================================================

# gcc-14가 있으면 우선 사용한다. Ubuntu 24.04 기본 gcc-13은 -std=gnu23을
# 지원하지 않고 -std=gnu2x까지만 지원하므로 C23 빌드가 즉시 실패한다.
CC_BASE := $(shell command -v gcc-14 >/dev/null 2>&1 && echo gcc-14 || echo gcc)

# ccache 자동 감지 (있으면 컴파일 캐시 사용 — clean 후 재빌드 5~10배 가속)
CCACHE := $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
    CC = ccache $(CC_BASE)
    $(info [ccache] ENABLED — $(CCACHE))
else
    CC = $(CC_BASE)
    $(info [ccache] not found — install: sudo apt install ccache)
endif

# --- [1. 의존성 패키지 및 라이브러리] ---
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 libvirt-glib-1.0 libvirt-gobject-1.0 libvirt lxc libsoup-3.0 libcrypto

# --- [2. 컴파일러 및 링커 옵션] ---

# [2-1] 공통 기본 플래그
CFLAGS  = -std=gnu23 -Wall -Wextra -D_GNU_SOURCE -Wno-unused-parameter
CFLAGS += -Iinclude -Isrc -Iinclude/purecvisor -I.
CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += -MMD -MP

# [2-2] 보안 강화 플래그 (D-1: Sprint D Build Hardening)
CFLAGS += -fstack-protector-strong
CFLAGS += -D_FORTIFY_SOURCE=2
CFLAGS += -fPIE
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -Wformat=2 -Wformat-security

# [2-3] debug / release 모드 선택 (기본: debug)
BUILD ?= debug
ifeq ($(BUILD),release)
    CFLAGS  += -O2 -DNDEBUG -flto=auto
    LDFLAGS_EXTRA = -pie -Wl,-z,relro,-z,now,-z,noexecstack -flto=auto
else
    CFLAGS  += -g -O0
    LDFLAGS_EXTRA = -pie -Wl,-z,relro,-z,now,-z,noexecstack
endif
LDFLAGS_EXTRA += -Wl,--gc-sections

# [2-3b] CFLAGS_EXTRA / LDFLAGS_EXTRA — sanitize/coverage 등 외부 주입용
CFLAGS += $(CFLAGS_EXTRA)

LDFLAGS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -lvirt -lvirt-qemu -llxc -lsqlite3 -lm -lz $(LDFLAGS_EXTRA)

# [2-4] D-3: libcap / libseccomp 조건부 감지
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

# ──────────────────────────────────────────────────────────
# [2-5] Sprint H: CLI 전용 readline 감지
#
#   readline 은 CLI(purecvisorctl)의 REPL/히스토리/Tab완성에만 사용.
#   데몬(purecvisorsd)·TUI(purecvisortui) 빌드에는 영향 없음.
#
#   CLI_CFLAGS  : src/cli/purecvisorctl.c 컴파일 시 추가
#   CLI_LDFLAGS : bin/pcvctl 링킹 시 추가
# ──────────────────────────────────────────────────────────
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

# ──────────────────────────────────────────────────────────
# [2-6] io_uring 감지 (Phase U-1)
# ──────────────────────────────────────────────────────────
URING_SRCS :=
ifneq ($(shell test -f /usr/include/liburing.h && echo yes),)
    CFLAGS  += -DHAVE_LIBURING -DPCV_URING_ENABLED=1
    LDFLAGS += -luring
    URING_SRCS = src/io/pcv_uring.c src/io/pcv_uring_buf.c src/io/pcv_uring_socket.c
    $(info [U]   liburing: ENABLED)
else
    $(info [U]   liburing: not found — GLib I/O fallback)
endif

# ──────────────────────────────────────────────────────────
# [2-7] Edition 고정: purecvisor-single 공개 리포는 Single Edge 전용
#
#   이 리포는 오픈소스 공개 범위이므로 클러스터/Multi Edge 빌드 경로를
#   제공하지 않는다. 상용 Multi Edge 코드는 별도 비공개 리포에서 관리한다.
# ──────────────────────────────────────────────────────────
EDITION ?= single
EDITION_STATE_FILE = .edition-state

ifneq ($(EDITION),single)
    $(error purecvisor-single supports EDITION=single only)
endif

CFLAGS += -DPCV_CLUSTER_ENABLED=0
$(info [EDITION] Single Edge — public standalone build)

# --- [3. 소스 파일 정의] ---

# [3-1] 공통 allowlist manifests
COMMON_CORE_SRCS = \
    src/bootstrap/pcv_bootstrap_info.c \
    src/modules/core/vm_state.c \
    src/modules/core/cpu_allocator.c \
    src/modules/virt/vm_config_builder.c \
    src/modules/virt/vm_clone_plan.c \
    src/modules/virt/vm_manager.c \
    src/modules/virt/circuit_breaker.c \
    src/modules/virt/cancellable_map.c \
    src/modules/virt/virt_conn_pool.c \
    src/modules/storage/zfs_driver.c \
    src/utils/logger.c \
    src/utils/pcv_validate.c \
    src/utils/pcv_error.c \
    src/utils/pcv_log.c \
    src/utils/pcv_spawn.c \
    src/utils/pcv_config.c \
    src/utils/pcv_privdrop.c \
    src/utils/pcv_jwt.c \
    src/utils/pcv_txn.c \
    src/utils/pcv_worker_pool.c \
    src/utils/pcv_job_queue.c \
    src/utils/pcv_zfs_lock.c

COMMON_SINGLE_ALLOWED_NET_SRCS = \
    src/modules/network/network_manager.c \
    src/modules/network/network_firewall.c \
    src/modules/network/network_dhcp.c \
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

# [3-2] 메인 데몬용 소스
DAEMON_COMMON_SRCS = \
    src/api/uds_server.c \
    src/api/dispatcher.c \
    src/api/drain.c \
    src/api/rest_auth.c \
    src/api/rest_server.c \
    src/api/rest_middleware.c \
    src/api/grpc_server.c \
    src/modules/daemons/telemetry.c \
    src/modules/daemons/virt_events.c \
    src/modules/dispatcher/rpc_utils.c \
    src/modules/dispatcher/handler_snapshot.c \
    src/modules/dispatcher/handler_vm_start.c \
    src/modules/dispatcher/handler_vnc.c \
    src/modules/dispatcher/handler_vm_lifecycle.c \
    src/modules/dispatcher/handler_vm_hotplug.c \
    $(COMMON_SINGLE_ALLOWED_NET_SRCS) \
    src/modules/dispatcher/handler_storage.c \
    src/modules/dispatcher/handler_monitor.c \
    src/modules/lxc/lxc_driver.c \
    src/modules/dispatcher/handler_container.c \
    src/modules/network/dpdk_manager.c \
    src/modules/network/sriov_manager.c \
    src/modules/storage/iscsi_manager.c \
    src/modules/dispatcher/handler_overlay.c \
    src/modules/dispatcher/handler_accel.c \
    src/modules/dispatcher/handler_template.c \
    src/modules/template/vm_template.c \
    src/modules/dispatcher/handler_auth.c \
    src/modules/auth/pcv_rbac.c \
    src/modules/daemons/ebpf_telemetry.c \
    src/modules/daemons/alert_engine.c \
    src/modules/daemons/process_monitor.c \
    src/modules/backup/backup_scheduler.c \
    src/modules/dispatcher/handler_backup.c \
    src/modules/dispatcher/handler_security.c \
    src/modules/security/security_event.c \
    src/modules/security/security_store.c \
    src/modules/security/security_policy.c \
    src/modules/security/hips_actions.c \
    src/modules/security/hids_file_integrity.c \
    src/api/hot_reload.c \
    src/api/ws_server.c \
    src/modules/daemons/prometheus_exporter.c \
    src/modules/audit/pcv_audit.c \
    src/modules/storage/storage_tier.c \
    src/modules/accel/gpu_manager.c \
    src/modules/plugin/pcv_plugin_manager.c \
    src/utils/pcv_tls.c \
    src/modules/network/nfv_manager.c \
    src/modules/network/security_group.c \
    src/modules/network/security_group_nft.c \
    src/modules/network/vm_iface.c \
    src/modules/network/vm_vnet_cache.c \
    src/modules/network/network_firewall_host.c \
    src/modules/ai/anomaly_detector.c \
    src/modules/ai/workload_predict.c \
    src/modules/ai/self_healing.c \
    src/modules/ai/restart_breaker.c \
    src/modules/ai/ai_agent.c \
    src/modules/cloud/cloud_migration.c \
    src/modules/cloud/aws_client.c \
    src/modules/cloud/disk_converter.c \
    $(URING_SRCS) \
    $(COMMON_CORE_SRCS)

DAEMON_SRCS = src/main.c $(DAEMON_COMMON_SRCS) $(SINGLE_BOOTSTRAP_SRCS)

# [3-3] 테스트 러너 소스
TEST_COMMON_SRCS = \
    tests/test_stubs.c \
    tests/test_main.c \
    tests/test_validate.c \
    tests/test_circuit_breaker.c \
    tests/test_restart_breaker.c \
    tests/test_cancellable_map.c \
    tests/test_cpu_allocator.c \
    tests/test_config.c \
    tests/test_vm_signals.c \
    tests/test_spawn_launcher.c \
    tests/test_jwt.c \
    tests/test_network.c \
    tests/test_security_group.c \
    tests/test_sg_nft_builder.c \
    tests/test_container.c \
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
    tests/test_vm_manager.c \
    tests/test_rest_middleware.c \
    tests/test_rest_auth.c \
    tests/test_rpc_utils.c \
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
    src/modules/ai/restart_breaker.c \
    tests/test_apikey.c \
    src/modules/security/security_event.c \
    src/modules/security/security_store.c \
    src/modules/security/security_policy.c \
    src/modules/security/hips_actions.c \
    src/modules/security/hids_file_integrity.c \
    src/modules/network/ovs_overlay_core.c \
    src/modules/network/ovn_core.c \
    src/api/rest_middleware.c \
    src/api/rest_auth.c \
    src/api/drain.c \
    src/api/hot_reload.c \
    src/modules/dispatcher/rpc_utils.c \
    src/modules/network/dpdk_manager.c \
    src/modules/network/sriov_manager.c \
    src/modules/network/security_group.c \
    src/modules/network/security_group_nft.c \
    src/modules/network/vm_iface.c \
    src/modules/network/vm_vnet_cache.c \
    src/modules/network/network_firewall_host.c \
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
TUI_SRCS = src/tui/purecvisortui.c src/tui/tui_widgets.c src/tui/tui_rpc.c

# --- [4. 오브젝트 및 의존성 파일 변환] ---
DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)
TEST_OBJS   = $(TEST_SRCS:.c=.o)
CLI_OBJS    = $(CLI_SRCS:.c=.o)
TUI_OBJS    = $(TUI_SRCS:.c=.o)
DEPENDS     = $(DAEMON_SRCS:.c=.d) $(TEST_SRCS:.c=.d) $(CLI_SRCS:.c=.d) $(TUI_SRCS:.c=.d)

# 헤더 의존 자동 추적: -MMD 로 생성된 .d 를 실제로 include 해야 헤더(예:
# version.h) 변경이 재컴파일을 트리거한다. 누락 시 헤더만 바꾼 증분 빌드가
# stale object 를 남긴다 (v1.1.0 릴리스에서 version.h 범프가 안 잡힌 원인).
# '-' 접두: 최초 빌드(.d 부재) 시 경고 없이 진행.
-include $(DEPENDS)

ALL_DAEMON_SRCS = $(DAEMON_SRCS)
ALL_TEST_SRCS = $(TEST_SRCS)
ALL_EDITION_OBJS = $(ALL_DAEMON_SRCS:.c=.o) $(ALL_TEST_SRCS:.c=.o) $(CLI_SRCS:.c=.o) $(TUI_SRCS:.c=.o)
ALL_EDITION_DEPS = $(ALL_DAEMON_SRCS:.c=.d) $(ALL_TEST_SRCS:.c=.d) $(CLI_SRCS:.c=.d) $(TUI_SRCS:.c=.d)
CLEAN_REPORTS = test_results.txt test_results_tap.txt valgrind_report.txt sanitize_report.txt cppcheck_report.txt
CLEAN_FUZZ_ARTIFACTS = fuzz_pcv_validate fuzz_pcv_jwt fuzz_rpc_envelope fuzz_validate.txt fuzz_jwt.txt fuzz_rpc.txt
CLEAN_COVERAGE_ARTIFACTS = compile_commands.json *.gcda *.gcno *.gcov
CLEAN_PROTO_ARTIFACTS = proto/purecvisor.pb-c.o proto/purecvisor.pb-c.d
CLEAN_UI_ARTIFACTS = $(UI_DIR)/bundle.js $(UI_DIR)/index.prod.html

$(DAEMON_OBJS) $(TEST_OBJS) $(CLI_OBJS) $(TUI_OBJS): $(EDITION_STATE_FILE)

FORCE:

$(EDITION_STATE_FILE): FORCE
	@prev_edition="$$(cat $@ 2>/dev/null || true)"; \
	if [ "$$prev_edition" != "$(EDITION)" ]; then \
		rm -f $(ALL_EDITION_OBJS) $(ALL_EDITION_DEPS) \
		      bin/purecvisorsd $(TEST_BIN) $(CLI_BIN) $(TUI_BIN); \
		printf '%s\n' "$(EDITION)" > $@; \
	fi

# --- [5. 빌드 타겟 이름 정의] ---
DAEMON_BIN = bin/purecvisorsd
TEST_BIN   = test_runner
CLI_BIN    = bin/pcvctl
TUI_BIN    = bin/pcvtui

# ==========================================================
# 기본 타겟
# ==========================================================
all: $(DAEMON_BIN) $(CLI_BIN) $(TUI_BIN)

daemon: $(DAEMON_BIN)
cli:    $(CLI_BIN)
tui:    $(TUI_BIN)

# [데몬 링킹]
$(DAEMON_BIN): $(DAEMON_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking Daemon: $@"
	$(CC) -o $@ $(DAEMON_OBJS) $(LDFLAGS)

# ──────────────────────────────────────────────────────────
# [CLI 전용 컴파일 규칙] — Sprint H
#
#   공통 %.o 규칙 대신 명시적 패턴으로 CLI_CFLAGS 적용.
#   이 규칙이 없으면 readline 매크로(-DHAVE_READLINE)가
#   purecvisorctl.o 에 전달되지 않음.
# ──────────────────────────────────────────────────────────
src/cli/%.o: src/cli/%.c
	@echo "🔨 Compiling CLI (readline=$(if $(findstring HAVE_READLINE,$(CLI_CFLAGS)),on,off)): $<"
	$(CC) $(CFLAGS) $(CLI_CFLAGS) -c $< -o $@

# [CLI 링킹] — CLI_LDFLAGS(-lreadline) 추가
$(CLI_BIN): $(CLI_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking CLI Client: $@"
	$(CC) -o $@ $(CLI_OBJS) $(LDFLAGS) $(CLI_LDFLAGS)

# [TUI 링킹]
$(TUI_BIN): $(TUI_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking TUI Client: $@"
	$(CC) -o $@ $(TUI_OBJS) $(LDFLAGS) -lncursesw -lpthread

# [테스트 러너 링킹]
test_runner: $(TEST_OBJS)
	@echo "🔗 Linking Test Runner: $@"
	$(CC) -o $(TEST_BIN) $(TEST_OBJS) $(LDFLAGS)

# [테스트 실행]
test: test_runner
	@echo "🧪 Running g_test_* suite..."
	@sudo ./$(TEST_BIN) -v > test_results.txt 2>&1; \
	 status=$$?; \
	 cat test_results.txt; \
	 exit $$status
	@echo "📄 Results saved to test_results.txt"

test-tap: test_runner
	@echo "🧪 Running g_test_* suite (TAP output)..."
	@sudo ./$(TEST_BIN) --tap > test_results_tap.txt 2>&1; \
	 status=$$?; \
	 cat test_results_tap.txt; \
	 exit $$status

# [공통 컴파일 규칙]
%.o: %.c
	@echo "🔨 Compiling: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# --- [6. 유틸리티 타겟] ---

memcheck: test_runner
	@echo "🔍 Running Valgrind on test suite..."
	@command -v valgrind >/dev/null || { echo "❌ valgrind 미설치 (sudo apt-get install valgrind)"; exit 1; }
	@sudo env G_SLICE=always-malloc G_DEBUG=gc-friendly valgrind \
		--leak-check=full \
		--show-leak-kinds=all \
		--errors-for-leak-kinds=definite \
		--child-silent-after-fork=yes \
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

# ──────────────────────────────────────────────────────────
# [completion 설치] — Sprint H
#   make install-completion      → bash + zsh 자동완성 설치
#   make install-completion-user → ~/.bash_completion.d (비root)
# ──────────────────────────────────────────────────────────
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
	      $(TEST_BIN) $(CLI_BIN) $(TUI_BIN) \
	      $(ALL_EDITION_OBJS) \
	      $(ALL_EDITION_DEPS) \
	      $(CLEAN_REPORTS) $(CLEAN_FUZZ_ARTIFACTS) \
	      $(CLEAN_COVERAGE_ARTIFACTS) $(CLEAN_PROTO_ARTIFACTS) \
	      $(CLEAN_UI_ARTIFACTS) \
	      .edition-single $(EDITION_STATE_FILE)
	rm -rf $(COV_DIR) dist

# ── UI Bundle ────────────────────────────────────────────────
UI_DIR = ui
# 번들 헤더 버전 — version.h(PCV_PRODUCT_VERSION) 단일 소스에서 파생.
# date 대신 이 값을 박아 ui-bundle 산출을 결정적(reproducible)으로 만든다.
PCV_UI_VERSION := $(shell sed -n 's/.*PCV_PRODUCT_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' include/purecvisor/version.h)
# 순서 의존: ui(escapeHtml/H 빌더) → uxlib(filterEditionItems/customConfirm) →
# modal/charts/security → 나머지. nav.js 가 top-level 에서 PCV.filterEditionItems 를
# 호출하므로 uxlib 는 반드시 nav 이전. (누락 시 번들이 nav 에서 throw → 뒷부분 미실행.)
UI_MODULES = $(UI_DIR)/modules/endpoints.js $(UI_DIR)/modules/api.js $(UI_DIR)/modules/ui.js \
    $(UI_DIR)/modules/uxlib.js $(UI_DIR)/modules/modal.js $(UI_DIR)/modules/charts.js \
    $(UI_DIR)/modules/security.js \
    $(UI_DIR)/modules/monitor.js \
    $(UI_DIR)/modules/vm.js $(UI_DIR)/modules/vm-console.js $(UI_DIR)/modules/vm-lifecycle.js \
    $(UI_DIR)/modules/vm-guest.js \
    $(UI_DIR)/modules/container.js $(UI_DIR)/modules/network.js \
    $(UI_DIR)/modules/storage.js \
    $(UI_DIR)/modules/cloud.js $(UI_DIR)/modules/help.js \
    $(UI_DIR)/modules/nav.js $(UI_DIR)/modules/theme.js \
    $(UI_DIR)/modules/accounts.js $(UI_DIR)/modules/advanced.js \
    $(UI_DIR)/modules/selfhealing.js \
    $(UI_DIR)/app.js

ui-bundle: $(UI_MODULES)
	@for f in $(UI_DIR)/modules/*.js; do \
		case " $(UI_MODULES) " in \
			*" $$f "*) ;; \
			*) echo "❌ $$f 가 UI_MODULES 에 없음 — 번들 누락(BUG-22류). Makefile UI_MODULES 갱신 필요"; exit 1;; \
		esac; \
	done
	@echo "📦 Bundling UI: $(UI_DIR)/bundle.js"
	@cat $(UI_MODULES) > $(UI_DIR)/bundle.js
	@echo "/* PureCVisor UI Bundle v$(PCV_UI_VERSION) — $$(wc -l < $(UI_DIR)/bundle.js) LOC (deterministic: no build timestamp) */" >> $(UI_DIR)/bundle.js
	@SRC=$$(cat $(UI_MODULES) | sha1sum | cut -c1-8); \
	if npx --no-install esbuild --version >/dev/null 2>&1; then \
		npx --no-install esbuild $(UI_DIR)/bundle.js --minify --sourcemap --sources-content=true \
			--target=es2020 --log-level=warning \
			--banner:js="/* PureCVisor UI Bundle v$(PCV_UI_VERSION) (minified; src-sha1 $$SRC; source map: app.bundle.js.map) */" \
			--outfile=$(UI_DIR)/app.bundle.js; \
		echo "✅ Bundle: bundle.js $$(wc -c < $(UI_DIR)/bundle.js | tr -d ' ')B → app.bundle.js $$(wc -c < $(UI_DIR)/app.bundle.js | tr -d ' ')B (minified +map, src-sha1 $$SRC)"; \
	else \
		cp $(UI_DIR)/bundle.js $(UI_DIR)/app.bundle.js; \
		rm -f $(UI_DIR)/app.bundle.js.map; \
		echo "⚠️  esbuild 없음(npm install 필요) — 무민파이 concat 폴백: app.bundle.js $$(wc -c < $(UI_DIR)/app.bundle.js | tr -d ' ')B"; \
	fi
	@H=$$(sha1sum $(UI_DIR)/app.bundle.js | cut -c1-8); \
	sed -i "s|const CACHE_NAME = 'pcv-ui-v[^']*';|const CACHE_NAME = 'pcv-ui-v$$H';|" $(UI_DIR)/sw.js; \
	echo "✅ sw.js CACHE_NAME → pcv-ui-v$$H (프리캐시 무효화 자동 bump)"

ui-prod: ui-bundle
	@echo "📦 Generating production index.html"
	@sed 's|<script src="modules/[^"]*"></script>||g; s|<script src="app.js"></script>|<script src="bundle.js"></script>|' \
		$(UI_DIR)/index.html > $(UI_DIR)/index.prod.html
	@echo "✅ Production: $(UI_DIR)/index.prod.html + $(UI_DIR)/bundle.js"

# ──────────────────────────────────────────────────────────
# [Sanitize 빌드] — ASan + UBSan
#
#   make sanitize        → test_runner를 ASan/UBSan으로 빌드 후 실행
#   Valgrind 대비 ~10배 빠르며 race/UB까지 검출.
#   pre-commit/CI에서 Valgrind 보완 게이트로 사용.
# ──────────────────────────────────────────────────────────
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -O1 -g -U_FORTIFY_SOURCE
# seccomp subprocess tests are incompatible with LeakSanitizer's process-exit
# checks; leak coverage is handled by make memcheck.
SAN_ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=0:halt_on_error=0:print_summary=1
SAN_UBSAN_OPTIONS ?= print_stacktrace=1:halt_on_error=1

# ──────────────────────────────────────────────────────────
# [libFuzzer 빌드] — clang + -fsanitize=fuzzer
#
#   make fuzz                   → fuzz_pcv_validate 빌드
#   make fuzz-run FUZZ_TIME=300 → 300초 동안 퍼징
#
#   대상: src/utils/pcv_validate.c (입력 검증 레이어)
#   harness: tests/fuzz/fuzz_pcv_validate.c
# ──────────────────────────────────────────────────────────
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

fuzz_pcv_jwt: tests/fuzz/fuzz_pcv_jwt.c src/utils/pcv_jwt.c
	@command -v clang >/dev/null || { echo "❌ clang 미설치"; exit 1; }
	@echo "🐛 Building libFuzzer harness: fuzz_pcv_jwt"
	clang $(FUZZ_FLAGS) $(FUZZ_INC) \
		tests/fuzz/fuzz_pcv_jwt.c src/utils/pcv_jwt.c \
		$(FUZZ_LIB) -o $@

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
	@echo "🧪 Building test_runner with ASan + UBSan..."
	$(MAKE) clean
	@$(MAKE) CFLAGS_EXTRA="$(SAN_FLAGS)" LDFLAGS_EXTRA="$(SAN_FLAGS)" test_runner; \
	 status=$$?; \
	 if [ $$status -ne 0 ]; then \
	     $(MAKE) clean >/dev/null; \
	     exit $$status; \
	 fi
	@echo "🧪 Running ASan/UBSan test_runner..."
	@ASAN_OPTIONS="$(SAN_ASAN_OPTIONS)" \
	 UBSAN_OPTIONS="$(SAN_UBSAN_OPTIONS)" \
	 sudo -E ./$(TEST_BIN) -v > sanitize_report.txt 2>&1; \
	 status=$$?; \
	 cat sanitize_report.txt; \
	 report_tmp="$$(mktemp)"; \
	 cp sanitize_report.txt "$$report_tmp"; \
	 $(MAKE) clean >/dev/null; \
	 mv "$$report_tmp" sanitize_report.txt; \
	 exit $$status
	@echo "📄 Report: sanitize_report.txt"

release:
	$(MAKE) BUILD=release all

# --- [.deb 설치 패키지] ---
# make deb          → dist/purecvisor-single_<ver>.0_amd64.deb (release 빌드 기반)
# make deb DEB_PATCH=3 → ..._<ver>.3_amd64.deb (patch 지정)
# 버전은 include/purecvisor/version.h(PCV_PRODUCT_VERSION) 단일 소스에서 파생.
deb: release ui-bundle
	@command -v dpkg-deb >/dev/null 2>&1 || { echo "make deb: dpkg-deb 필요 (sudo apt install dpkg-dev)"; exit 1; }
	@command -v fakeroot >/dev/null 2>&1 || { echo "make deb: fakeroot 필요 (sudo apt install fakeroot)"; exit 1; }
	@bash packaging/deb/build-deb.sh

single:
	$(MAKE) all

multi:
	@echo "purecvisor-single is Single Edge only; use the private Multi Edge repository." >&2
	@exit 2

# --- [자동화 테스트] ---
# make test-safe       → Tier 0(유닛) + Tier 1(SAFE 통합)
# make test-all        → Tier 0+1+2 (MODERATE 포함, 데몬 필요)
# make test-integ      → Tier 1+2 (통합 테스트만)
test-safe:
	@./scripts/run_auto_tests.sh --ci

test-all:
	@./scripts/run_auto_tests.sh --all --ci

test-integ:
	@./scripts/run_auto_tests.sh --tier 1 --ci
	@./scripts/run_auto_tests.sh --tier 2 --ci

# --- [Git Hooks 설치] ---
install-hooks:
	@echo "🔗 Installing pre-commit hook..."
	@cp scripts/pre-commit .git/hooks/pre-commit
	@chmod +x .git/hooks/pre-commit
	@echo "✅ pre-commit hook installed (.git/hooks/pre-commit)"

# --- [정적 분석] ---
# cppcheck: 소스 코드 정적 분석 (NULL 역참조, 메모리 누수, 미사용 변수 등)
# compile_commands.json: clang-tidy/IDE 연동용
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

check-rpc-consumers:
	@echo "🔗 Running AF-C4 RPC consumer contract gate (소비 ⊆ 등록)..."
	@python3 scripts/check_rpc_consumers.py

# check-all: 계약 게이트 일괄 (CI/릴리스용) — RBAC 정책 + RPC 소비⊆등록
check-all: check-rbac check-rpc-consumers
	@echo "✅ 계약 게이트 전체 통과 (RBAC + RPC consumers)"

compile-commands:
	@echo "📝 Generating compile_commands.json..."
	@echo "[" > compile_commands.json
	@first=1; for f in $(DAEMON_SRCS) $(CLI_SRCS) $(TUI_SRCS); do \
		[ $$first -eq 0 ] && echo "," >> compile_commands.json; first=0; \
		echo "  {\"directory\": \"$(CURDIR)\", \"command\": \"$(CC) $(CFLAGS) -c $$f\", \"file\": \"$$f\"}" >> compile_commands.json; \
	done
	@echo "]" >> compile_commands.json
	@echo "✅ compile_commands.json generated ($$(wc -l < compile_commands.json) lines)"

# --- [코드 커버리지] ---
# make coverage: gcov 빌드 + 테스트 실행 + 리포트 생성
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

# coverage-check: lcov 요약에서 line 커버리지를 추출, COV_MIN 미만이면 exit 1
# CI Gate 8 차단용 — coverage-html 실행 후 호출
coverage-check: coverage-html
	@PCT=$$(lcov --summary $(COV_DIR)/coverage_filtered.info 2>/dev/null | \
	        grep -oP 'lines\.\.\.\.\.\.: \K[0-9.]+' | head -1); \
	 echo "📊 Line coverage: $${PCT}%"; \
	 if [ -z "$$PCT" ]; then echo "❌ coverage 측정 실패"; exit 1; fi; \
	 awk "BEGIN { exit !($$PCT < $(COV_MIN)) }" && \
	     { echo "❌ coverage $${PCT}% < $(COV_MIN)% 임계값"; exit 1; } || \
	     echo "✅ coverage $${PCT}% ≥ $(COV_MIN)%"

.PHONY: all clean release deb single multi test_runner test test-tap \
        memcheck memcheck-daemon daemon cli tui sanitize fuzz fuzz-run \
        install-completion install-completion-user ui-bundle ui-prod \
        install-hooks test-safe test-all test-integ \
        cppcheck cppcheck-strict check-rbac check-rpc-consumers check-all compile-commands coverage coverage-html coverage-check
