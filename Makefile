# ==========================================================
# PureCvisor Async Hypervisor Orchestrator Makefile
# ==========================================================

CC = gcc

# --- [1. 의존성 패키지 및 라이브러리] ---
# GLib 생태계 및 Libvirt (고수준 GObject 바인딩 + 저수준 원시 API 모두 포함)
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 libvirt-glib-1.0 libvirt-gobject-1.0 libvirt

# --- [2. 컴파일러 및 링커 옵션] ---
# -Wno-unused-parameter: GObject 콜백 매개변수 경고 무시
# -MMD -MP: 헤더 파일(.h) 변경 시 관련 소스코드 자동 재빌드를 위한 의존성 파일(.d) 생성
CFLAGS  = -std=gnu11 -Wall -Wextra -g -D_GNU_SOURCE -Wno-unused-parameter
CFLAGS += -Iinclude -Isrc
CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += -MMD -MP

LDFLAGS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -lvirt 

# --- [3. 소스 파일 정의] ---

# [3-1] 공통 모듈 (Core, Daemons, Storage, Virt)
COMMON_SRCS = \
	src/modules/core/vm_state.c \
	src/modules/core/cpu_allocator.c \
	src/modules/daemons/telemetry.c \
	src/modules/daemons/virt_events.c \
	src/modules/virt/vm_config_builder.c \
	src/modules/storage/zfs_driver.c \
	src/utils/logger.c

# [3-2] 메인 데몬용 소스 (Entry, API, Dispatchers)
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
	$(COMMON_SRCS)

# [3-3] 테스트 러너용 소스
TEST_SRCS = \
	test_runner.c \
	$(COMMON_SRCS)

# --- [4. 오브젝트 및 의존성 파일 변환] ---
DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)
TEST_OBJS   = $(TEST_SRCS:.c=.o)
DEPENDS     = $(DAEMON_SRCS:.c=.d) $(TEST_SRCS:.c=.d)

# --- [5. 빌드 타겟] ---
DAEMON_BIN = bin/purecvisord
TEST_BIN   = test_runner

# 기본 타겟: 데몬 빌드
all: $(DAEMON_BIN)

# [데몬 링킹]
$(DAEMON_BIN): $(DAEMON_OBJS)
	@mkdir -p bin
	@echo "🔗 Linking Daemon: $@"
	$(CC) -o $@ $(DAEMON_OBJS) $(LDFLAGS)

# [테스트 러너 링킹]
test_runner: $(TEST_OBJS)
	@echo "🔗 Linking Test Runner: $@"
	$(CC) -o $(TEST_BIN) $(TEST_OBJS) $(LDFLAGS)

# [개별 소스 컴파일 (증분 빌드)]
%.o: %.c
	@echo "🔨 Compiling: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# --- [6. 유틸리티 타겟] ---

# [Valgrind 메모리 누수 검증 (Zero-Leak)]
# GLib Slice Allocator를 우회하여 Valgrind 오탐을 방지합니다.
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
	rm -f $(DAEMON_BIN) $(TEST_BIN) $(DAEMON_OBJS) $(TEST_OBJS) $(DEPENDS)

# 컴파일러가 생성한 의존성 파일 포함 (헤더 파일 변경 감지용)
-include $(DEPENDS)

# 더미 타겟 선언
.PHONY: all clean test_runner memcheck