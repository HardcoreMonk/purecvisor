CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
# 필요한 라이브러리 목록
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 libvirt-glib-1.0 libvirt-gobject-1.0

# pkg-config 설정
INCLUDES = $(shell pkg-config --cflags $(PKGS))
LIBS = $(shell pkg-config --libs $(PKGS))

# 헤더 경로
CFLAGS  = -std=gnu11 -Wall -Wextra -g -D_GNU_SOURCE
CFLAGS += -Iinclude -Isrc
CFLAGS += $(shell pkg-config --cflags $(PKGS))

# [LDFLAGS: 링킹 옵션]
# -lvirt: libvirt 저수준 API 링크 (안전장치)
LDFLAGS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -lvirt 

# --- 소스 파일 정의 ---
# 1. 공통 모듈 (테스트와 메인 모두 사용)
COMMON_SRCS = src/modules/virt/vm_config_builder.c \
              src/modules/storage/zfs_driver.c

# 2. 메인 데몬용 소스 (Phase 1, 2 파일들 포함)
# 주의: src/main.c, src/api/dispatcher.c 등이 있다고 가정
DAEMON_SRCS = src/main.c \
              src/api/dispatcher.c \
              src/api/uds_server.c \
              src/modules/virt/vm_manager.c \
              $(COMMON_SRCS)

# 3. 테스트 러너용 소스
TEST_SRCS = test_runner.c $(COMMON_SRCS)

# --- 타겟 정의 ---
DAEMON_BIN = bin/purecvisord
TEST_BIN = test_runner

# 기본 타겟: 데몬 빌드
all: $(DAEMON_BIN)

# 데몬 빌드 규칙
$(DAEMON_BIN): $(DAEMON_SRCS)
	@mkdir -p bin
	@echo "Building Daemon: $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(DAEMON_SRCS) $(LIBS)

# 테스트 러너 빌드 규칙 (별도 타겟)
test_runner: $(TEST_SRCS)
	@echo "Building Test Runner: $@"
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TEST_BIN) $(TEST_SRCS) $(LIBS)

clean:
	rm -f $(DAEMON_BIN) $(TEST_BIN) src/modules/virt/*.o src/modules/storage/*.o *.o