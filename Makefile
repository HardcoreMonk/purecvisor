# ==========================================================
# PureCvisor Async Hypervisor Orchestrator Makefile
# ==========================================================

CC = gcc

# --- [1. 의존성 패키지 및 라이브러리] ---
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 libvirt-glib-1.0 libvirt-gobject-1.0 libvirt

# --- [2. 컴파일러 및 링커 옵션] ---
CFLAGS  = -std=gnu11 -Wall -Wextra -g -D_GNU_SOURCE -Wno-unused-parameter
CFLAGS += -Iinclude -Isrc
CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += -MMD -MP

LDFLAGS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -lvirt 

# --- [3. 소스 파일 정의] ---

# [3-1] 공통 모듈
COMMON_SRCS = \
    src/modules/core/vm_state.c \
    src/modules/core/cpu_allocator.c \
    src/modules/daemons/telemetry.c \
    src/modules/daemons/virt_events.c \
    src/modules/virt/vm_config_builder.c \
    src/modules/storage/zfs_driver.c \
    src/utils/logger.c

# [3-2] 메인 데몬용 소스
DAEMON_SRCS = \
    src/main.c \
    src/api/uds_server.c \
    src/api/dispatcher.c \
    src/modules/virt/vm_manager.c \
    src/modules/dispatcher/rpc_utils.c \
    src/modules/dispatcher/handler_snapshot.c \
    src/modules/dispatcher/handler_vm_start.c \
    src/modules/dispatcher/handler_vnc.c \
    src/modules/dispatcher/handler_vm_lifecycle.c \
    src/modules/dispatcher/handler_vm_hotplug.c \
    src/modules/network/network_manager.c \
    src/modules/network/network_firewall.c \
    src/modules/network/network_dhcp.c \
    src/modules/dispatcher/handler_storage.c \
    $(COMMON_SRCS)

# [3-3] 테스트 러너 및 CLI 소스
TEST_SRCS = test_runner.c $(COMMON_SRCS)
CLI_SRCS  = src/cli/purecvisorctl.c

# --- [4. 오브젝트 및 의존성 파일 변환] ---
DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)
TEST_OBJS   = $(TEST_SRCS:.c=.o)
CLI_OBJS    = $(CLI_SRCS:.c=.o)
DEPENDS     = $(DAEMON_SRCS:.c=.d) $(TEST_SRCS:.c=.d) $(CLI_SRCS:.c=.d)

# --- [5. 빌드 타겟 이름 정의] ---
DAEMON_BIN = bin/purecvisord
TEST_BIN   = test_runner
CLI_BIN    = bin/purecvisorctl

# ==========================================================
# 🚀 기본 타겟 (반드시 파일의 첫 번째 타겟이어야 함)
# ==========================================================
all: $(DAEMON_BIN) $(CLI_BIN)

# 명시적 호출용 타겟
daemon: $(DAEMON_BIN)
cli: $(CLI_BIN)

# [데몬 링킹]
$(DAEMON_BIN): $(DAEMON_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking Daemon: $@"
	$(CC) -o $@ $(DAEMON_OBJS) $(LDFLAGS)

# [CLI 클라이언트 링킹]
$(CLI_BIN): $(CLI_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking CLI Client: $@"
	$(CC) -o $@ $(CLI_OBJS) $(LDFLAGS)

# [테스트 러너 링킹]
test_runner: $(TEST_OBJS)
	@echo "🔗 Linking Test Runner: $@"
	$(CC) -o $(TEST_BIN) $(TEST_OBJS) $(LDFLAGS)

# [공통 컴파일 규칙 (모든 .c -> .o)]
%.o: %.c
	@echo "🔨 Compiling: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# --- [6. 유틸리티 타겟] ---

# [Valgrind 메모리 누수 검증]
memcheck: $(DAEMON_BIN)
	@echo "🔍 Running Valgrind Memory Check..."
	G_SLICE=always-malloc G_DEBUG=gc-friendly valgrind \
		--leak-check=full \
		--show-leak-kinds=all \
		--track-origins=yes \
		./$(DAEMON_BIN)

# [빌드 부산물 정리]
clean:
	@echo "🧹 Cleaning up build artifacts..."
	rm -f $(DAEMON_BIN) $(TEST_BIN) $(CLI_BIN) $(DAEMON_OBJS) $(TEST_OBJS) $(CLI_OBJS) $(DEPENDS)

-include $(DEPENDS)

.PHONY: all clean test_runner memcheck daemon cli