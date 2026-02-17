# ==========================================
# PureCVisor-engine Makefile
# Target: Linux (GNU11), Single Process
# ==========================================

CC = gcc

# [PKG-CONFIG]
# Issue #1: libvirt-glib-1.0 추가
# Issue #2: gio-unix-2.0 (UNIX 시그널 처리용) 추가
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 libvirt libvirt-glib-1.0

# [CFLAGS: 컴파일 옵션]
# Issue #4: -Iinclude 추가 (헤더 경로 인식)
# -D_GNU_SOURCE: GNU 확장 기능 사용 (asprintf 등)
CFLAGS  = -std=gnu11 -Wall -Wextra -g -D_GNU_SOURCE
CFLAGS += -Iinclude -Isrc
CFLAGS += $(shell pkg-config --cflags $(PKGS))

# [LDFLAGS: 링킹 옵션]
# Issue #3: -lvirt 명시적 추가 (안전장치)
# 라이브러리 순서 중요: 사용자 지정 -> pkg-config
LDFLAGS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -lvirt 

# [SOURCE FILES]
# Issue #5: arena.c 포함 (메모리 관리자)
# Phase 1: Storage Driver & API Server 포함
SRCS = src/main.c \
       src/core/daemon.c \
       src/utils/arena.c \
       src/modules/storage/zfs_driver.c \
       src/api/uds_server.c

# [OBJECT FILES]
# src/foo.c -> obj/src/foo.o 로 변환
OBJS = $(SRCS:.c=.o)

# [TARGET]
TARGET = bin/purecvisord

# ==========================================
# Rules
# ==========================================

.PHONY: all clean dir

all: dir $(TARGET)

# 링킹 단계
# 주의: $(LDFLAGS)는 반드시 $(OBJS) 뒤에 와야 함 (Issue #3 해결)
$(TARGET): $(OBJS)
	@echo "  LINK    $@"
	@$(CC) $(OBJS) -o $@ $(LDFLAGS)

# 컴파일 단계
%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# 디렉토리 생성
dir:
	@mkdir -p bin

clean:
	@echo "  CLEAN"
	@rm -f $(OBJS) $(TARGET)
	@rm -rf bin