/**
 * @file pcv_validate.h
 * @brief 입력값 검증 공개 API -- RPC 파라미터 보안 검증 레이어
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  디스패처 핸들러가 JSON-RPC 파라미터를 비즈니스 로직에 전달하기 전에
 *  호출하는 입력값 검증 함수 모음이다. VM 이름, 스냅샷 이름, 브릿지 이름,
 *  ISO 경로, 수치 범위, 컨테이너 이미지, exec 명령어, PCI 주소 등
 *  모든 사용자 입력을 검증하여 인젝션/경로 순회 공격을 방어한다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  공개 헤더(include/purecvisor/)에 속하며, 데몬 내부 모듈과
 *  테스트 코드 양쪽에서 포함한다.
 *
 *    클라이언트 -> JSON-RPC 요청
 *        |
 *    dispatcher.c (파라미터 추출)
 *        | pcv_validate_*() 호출 -- 이 헤더의 함수들
 *        | 실패 시 즉시 에러 응답 반환, 핸들러 진입 안 함
 *        |
 *    handler_*.c (검증 통과한 파라미터로 비즈니스 로직 수행)
 *
 * ====================================================================
 *  주요 검증 카테고리
 * ====================================================================
 *  - 문자셋 검증: 이름류 필드는 [a-zA-Z0-9_-]만 허용
 *  - 경로 검증: ISO 경로는 절대 경로 필수, ".." 경로 순회 차단
 *  - 수치 범위: memory(128MB~1TB), vcpu(1~256), disk(1GB~64TB)
 *  - 컨테이너: 이미지 "distro:release" 형식, exec 명령어 NULL 바이트 차단
 *  - PCI 주소: "DDDD:BB:SS.F" 16진수 형식 검증 (SR-IOV/DPDK용)
 *  - 통합 검증: pcv_validate_vm_create_params()로 VM 생성 전체 파라미터
 *    한 번에 검증하고, 실패 시 GError에 상세 메시지 설정
 *
 * ====================================================================
 *  핵심 패턴
 * ====================================================================
 *  - 방어적 프로그래밍: 모든 함수는 NULL 입력에 안전하며, FALSE 반환.
 *  - 상한 매크로(PCV_MAX_*): 길이 상한을 매크로로 중앙 관리.
 *    핸들러 코드에서 하드코딩하지 않고 이 매크로를 참조할 것.
 *  - GError 통합: pcv_validate_vm_create_params()는 PCV_VALIDATE_ERROR
 *    도메인(pcv_error.h)으로 에러를 설정한다.
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - include 경로 규칙:
 *    pcv_validate.c에서는 #include "pcv_validate.h" (상대경로 없이)
 *    다른 모듈에서는 #include "../../utils/pcv_validate.h" (상대경로)
 *  - 새 검증 함수 추가 시: 이 헤더에 선언 + pcv_validate.c에 구현 +
 *    tests/test_validate.c에 테스트 케이스 추가.
 */

#ifndef PURECVISOR_VALIDATE_H
#define PURECVISOR_VALIDATE_H

#include <glib.h>

/* ── 길이 상한 ────────────────────────────────────────── */
#define PCV_MAX_VM_NAME      64
#define PCV_MAX_SNAP_NAME    128
#define PCV_MAX_BRIDGE_NAME  16
#define PCV_MAX_IFACE_NAME   15    /* IFNAMSIZ-1, VLAN subif '.' 허용 */
#define PCV_MAX_ISO_PATH     512
#define PCV_MAX_REMOTE_HOST  253
#define PCV_MAX_SSH_USER     64

/* LXC 전용 상한 */
#define PCV_MAX_CONTAINER_IMAGE  128   /* "ubuntu:22.04" 형식 */
#define PCV_MAX_EXEC_CMD         1024  /* container.exec cmd */

/* PCI 주소 상한 */
#define PCV_MAX_PCI_ADDR         16    /* "0000:3b:00.1" 형식 */

/* ── 범위 상한 ────────────────────────────────────────── */
#define PCV_MIN_MEMORY_MB    128
#define PCV_MAX_MEMORY_MB    (1024 * 1024)   // 1 TB
#define PCV_MIN_VCPU         1
#define PCV_MAX_VCPU         256
#define PCV_MIN_DISK_GB      1
#define PCV_MAX_DISK_GB      65536            // 64 TB

/* ── 문자셋 검증 ─────────────────────────────────────── */

/**
 * VM 이름: [a-zA-Z0-9_-], 1~PCV_MAX_VM_NAME 자
 */
gboolean pcv_validate_vm_name(const gchar *name);

/**
 * 스냅샷 이름: [a-zA-Z0-9_-], 1~PCV_MAX_SNAP_NAME 자
 */
gboolean pcv_validate_snap_name(const gchar *name);

/**
 * 네트워크 브리지 이름: [a-zA-Z0-9_-], 1~PCV_MAX_BRIDGE_NAME 자
 */
gboolean pcv_validate_bridge_name(const gchar *name);

/**
 * 원격 복제 대상 host/IP:
 *  - DNS hostname/FQDN 또는 IPv4 리터럴
 *  - 1~PCV_MAX_REMOTE_HOST 자
 *  - shell metachar, 공백, 경로 문자 차단
 */
gboolean pcv_validate_remote_host(const gchar *host);

/**
 * SSH 사용자명:
 *  - [a-zA-Z0-9_.-], 1~PCV_MAX_SSH_USER 자
 *  - '-'로 시작하는 option-injection 형태 차단
 */
gboolean pcv_validate_ssh_user(const gchar *user);

/**
 * ISO 경로:
 *  - 절대 경로 (/ 로 시작)
 *  - ".." 경로 순회 차단
 *  - 1~PCV_MAX_ISO_PATH 자
 */
gboolean pcv_validate_iso_path(const gchar *path);

/**
 * 정수 범위 검증
 */
gboolean pcv_validate_memory_mb(gint64 mb);
gboolean pcv_validate_vcpu(gint64 count);
gboolean pcv_validate_disk_gb(gint64 gb);

/**
 * GError를 설정하는 통합 검증 함수 (dispatcher에서 편리하게 사용)
 * 실패 시 error에 메시지를 담고 FALSE 반환
 */
gboolean pcv_validate_vm_create_params(const gchar  *name,
                                       gint64        vcpu,
                                       gint64        memory_mb,
                                       gint64        disk_gb,
                                       const gchar  *iso_path,   // nullable
                                       const gchar  *bridge,     // nullable
                                       GError      **error);

/* ── LXC 컨테이너 전용 검증 ──────────────────────────── */

/**
 * 컨테이너 이미지: "distro:release" 형식
 *  - distro : [a-z][a-z0-9-]* (소문자 영문자로 시작)
 *  - release: [a-z0-9][a-z0-9._-]*
 *  - 콜론(:) 구분자 필수
 *  - 1~PCV_MAX_CONTAINER_IMAGE 자
 * 예) "ubuntu:22.04", "debian:bookworm", "alpine:3.18"
 */
gboolean pcv_validate_container_image(const gchar *image);

/**
 * container.exec 명령어:
 *  - NULL 바이트 없음
 *  - 1~PCV_MAX_EXEC_CMD 자
 *  (lxc-attach + /bin/sh -c 로 컨테이너 내부에서 실행됨)
 */
gboolean pcv_validate_exec_cmd(const gchar *cmd);

/* ── PCI 주소 검증 (SR-IOV / DPDK Phase 4) ───────────── */

/**
 * PCI 주소: DDDD:BB:SS.F (domain:bus:slot.function)
 *  - 각 필드 16진수, 콜론/점 구분
 *  - 예: "0000:01:00.0", "0000:3b:10.1"
 *  - 경로 순회(..) 차단
 */
gboolean pcv_validate_pci_addr(const gchar *addr);

/**
 * CIDR: IPv4 a.b.c.d/0-32 또는 IPv6 xxxx::xxxx/0-128
 */
gboolean pcv_validate_cidr(const gchar *cidr);

/**
 * 네트워크 생성 통합 검증: bridge_name + mode + cidr + physical_if
 * mode별 필수 파라미터 규칙 적용 (nat→cidr, bridge→physical_if)
 */
gboolean pcv_validate_network_create_params(const gchar *bridge_name,
                                            const gchar *mode,
                                            const gchar *cidr,
                                            const gchar *physical_if,
                                            GError **error);

/**
 * pcv_validate_private_cidr: pcv_validate_cidr() + RFC1918 사설 대역 강제.
 * 공인 IP, link-local, 멀티캐스트, loopback 거부.
 */
gboolean pcv_validate_private_cidr(const gchar *cidr);

/**
 * pcv_validate_port: TCP/UDP 포트 번호 범위 검증 (1 ~ 65535).
 * 포트 0(와일드카드)은 제외.
 */
gboolean pcv_validate_port(gint port);

/**
 * pcv_validate_disk_size_gb: 디스크 크기(GB) 범위 검증 (1 ~ 2048).
 * 일반 VM 디스크 작업에 적합한 보수적 상한.
 */
gboolean pcv_validate_disk_size_gb(gint size);

/**
 * pcv_validate_zvol_name: ZFS zvol 이름 검증.
 * 영숫자로 시작, [a-zA-Z0-9_.-] 허용, 최대 64자, ".." 순회 차단.
 */
gboolean pcv_validate_zvol_name(const gchar *name);

/* ── 네트워크/방화벽 입력 검증 (whitelist) ─────────────── */

/**
 * pcv_validate_iface_name: Linux ifname 검증. [a-zA-Z0-9_.-], 1~15자.
 * bridge_name과 달리 '.'(VLAN subif) 허용, 선행 '-'는 거부.
 */
gboolean pcv_validate_iface_name(const gchar *name);

/**
 * pcv_validate_mac: "xx:xx:xx:xx:xx:xx" (17자, hex + 콜론 위치 2,5,8,11,14).
 */
gboolean pcv_validate_mac(const gchar *mac);

/**
 * pcv_validate_ip_literal: IPv4/IPv6 리터럴 (inet_pton, CIDR 접미사 불허).
 */
gboolean pcv_validate_ip_literal(const gchar *ip);

/**
 * pcv_validate_ipv6_prefix: "<ipv6>/<len>" (len 0~128, 마지막 '/'로 분리).
 * 주소부 inet_pton(AF_INET6) + [0-9a-fA-F:]만 허용, 개행/공백 어디든 거부.
 */
gboolean pcv_validate_ipv6_prefix(const gchar *prefix);

/**
 * pcv_validate_l4_proto: "tcp" | "udp" | "icmp" (소문자, 대소문자 구분).
 */
gboolean pcv_validate_l4_proto(const gchar *proto);

#endif /* PURECVISOR_VALIDATE_H */
