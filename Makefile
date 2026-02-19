CC = gcc

# 필요한 라이브러리 목록
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 libvirt-glib-1.0 libvirt-gobject-1.0

# [CFLAGS 설정]
# -Wno-unused-parameter: GObject 콜백 특성상 발생하는 불필요한 경고 무시
CFLAGS  = -std=gnu11 -Wall -Wextra -g -D_GNU_SOURCE -Wno-unused-parameter
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

# 2. 메인 데몬용 소스
# [수정] src/utils/logger.c 추가됨 (링커 에러 해결)
DAEMON_SRCS = src/main.c \
              src/api/dispatcher.c \
              src/api/uds_server.c \
              src/modules/virt/vm_manager.c \
              src/utils/logger.c \
			  src/modules/dispatcher/handler_snapshot.c \
       		  src/modules/dispatcher/rpc_utils.c \
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
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRCS) $(LDFLAGS)

# 테스트 러너 빌드 규칙 (별도 타겟)
test_runner: $(TEST_SRCS)
	@echo "Building Test Runner: $@"
	$(CC) $(CFLAGS) -o $(TEST_BIN) $(TEST_SRCS) $(LDFLAGS)

clean:
	rm -f $(DAEMON_BIN) $(TEST_BIN) src/modules/virt/*.o src/modules/storage/*.o *.o src/utils/*.o