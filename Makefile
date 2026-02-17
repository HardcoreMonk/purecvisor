# Makefile for PureCVisor-engine

CC = gcc
# [핵심] -Iinclude : 프로젝트 내 헤더 파일(purecvisor/core.h)을 찾기 위해 필수
CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -fPIC -g -Iinclude
LDFLAGS =

# ---------------------------------------------------------
# [1] 라이브러리 경로 설정 (pkg-config)
# ---------------------------------------------------------

# GLib-2.0
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS = $(shell pkg-config --libs glib-2.0)

# Libvirt
LIBVIRT_CFLAGS = $(shell pkg-config --cflags libvirt)
LIBVIRT_LIBS = $(shell pkg-config --libs libvirt)

# Libvirt-GLib (이 부분이 누락되어 libvirt-glib.h 에러 발생)
LIBVIRT_GLIB_CFLAGS = $(shell pkg-config --cflags libvirt-glib-1.0)
LIBVIRT_GLIB_LIBS = $(shell pkg-config --libs libvirt-glib-1.0)

# ---------------------------------------------------------
# [2] 컴파일/링크 플래그 병합
# ---------------------------------------------------------

# CFLAGS에 모든 라이브러리의 헤더 경로 추가
CFLAGS += $(GLIB_CFLAGS) $(LIBVIRT_CFLAGS) $(LIBVIRT_GLIB_CFLAGS)

# LDFLAGS에 모든 라이브러리 링크 정보 추가
# (링크 에러 방지를 위해 -lvirt 등을 명시적으로 추가)
LDFLAGS += $(GLIB_LIBS) $(LIBVIRT_LIBS) $(LIBVIRT_GLIB_LIBS) -lvirt

# ---------------------------------------------------------
# [3] 빌드 규칙
# ---------------------------------------------------------

TARGET = bin/purecvisord

# 소스 파일 자동 탐색
SRCS = $(wildcard src/*.c) \
       $(wildcard src/core/*.c) \
       $(wildcard src/utils/*.c)

OBJS = $(patsubst src/%.c, obj/%.o, $(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# 컴파일 규칙
obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
# 실제 실행되는 명령어를 확인하려면 아래 줄의 @를 제거하세요
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf bin obj

.PHONY: all clean