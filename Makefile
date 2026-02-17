# ==========================================
# PureCVisor-engine Makefile
# Target: Linux (GNU11), Single Process
# Phase: 2 (JSON Dispatcher & Libvirt Integration)
# ==========================================

CC = gcc

# [PKG-CONFIG]
# Phase 1: glib-2.0 gio-2.0 gio-unix-2.0
# Phase 2 Update: 
#   - json-glib-1.0 : JSON 명령 파싱용 (Task A)
#   - libvirt-gobject-1.0 : VM 관리를 위한 GObject 래퍼 (Task B 대비, 기존 libvirt-glib 상위 호환)
PKGS = glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0 libvirt-glib-1.0 libvirt-gobject-1.0

# [CFLAGS: 컴파일 옵션]
# -D_GNU_SOURCE: GNU 확장 기능 사용
# -Iinclude -Isrc: 헤더 파일 경로 지정
CFLAGS  = -std=gnu11 -Wall -Wextra -g -D_GNU_SOURCE
CFLAGS += -Iinclude -Isrc
CFLAGS += $(shell pkg-config --cflags $(PKGS))

# [LDFLAGS: 링킹 옵션]
# -lvirt: libvirt 저수준 API 링크 (안전장치)
LDFLAGS  = $(shell pkg-config --libs $(PKGS))
LDFLAGS += -lvirt 

# [SOURCE FILES]
# Phase 0: main.c, daemon.c, arena.c
# Phase 1: zfs_driver.c, uds_server.c
# Phase 2: dispatcher.c (New)
SRCS = src/main.c \
       src/core/daemon.c \
       src/utils/arena.c \
       src/modules/storage/zfs_driver.c \
       src/api/uds_server.c \
       src/api/dispatcher.c \
	   src/modules/virt/vm_manager.c

# [OBJECT FILES]
# 소스 파일(.c)을 오브젝트 파일(.o)로 변환 (경로 유지)
OBJS = $(SRCS:.c=.o)

# [TARGET]
# 최종 바이너리 경로
TARGET = bin/purecvisord

# ==========================================
# Rules
# ==========================================

.PHONY: all clean dir

all: dir $(TARGET)

# 링킹 단계
$(TARGET): $(OBJS)
	@echo "  LINK    $@"
	@$(CC) $(OBJS) -o $@ $(LDFLAGS)

# 컴파일 단계
%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# 디렉토리 생성 (bin 폴더가 없으면 생성)
dir:
	@mkdir -p bin

clean:
	@echo "  CLEAN"
	@rm -f $(OBJS) $(TARGET)
	@rm -rf bin