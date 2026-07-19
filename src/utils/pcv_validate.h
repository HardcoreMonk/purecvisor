/**
 * @file pcv_validate.h
 * @brief 입력값 검증 레이어 — 공개 헤더 (상수, 함수 선언)
 *
 * Sprint A-3에서 도입, Sprint I(네트워크), Phase 4(SR-IOV/DPDK)에서 확장.
 * 디스패처(dispatcher.c)가 RPC 핸들러 호출 전에 모든 입력값을 검증합니다.
 *
 * [사용 위치]
 *   dispatcher.c: vm.create 파라미터 검증 → pcv_validate_vm_create_params()
 *   network_manager.c: bridge/cidr 검증 → pcv_validate_network_create_params()
 *   handler_accel.c: PCI 주소 검증 → pcv_validate_pci_addr()
 *   handler_container.c: 이미지/명령 검증 → pcv_validate_container_image/exec_cmd()
 *   handler_template.c: 템플릿 이름 검증 → pcv_validate_vm_name()
 *
 * [include 경로]
 *   src/utils/ 내부: #include "pcv_validate.h"
 *   다른 디렉터리:   #include "../../utils/pcv_validate.h"
 *   또는:            #include "purecvisor/pcv_validate.h"
 *
 * [보안 원칙]
 *   화이트리스트 방식 — 허용된 문자/범위/형식만 통과, 나머지 전부 거부.
 *   검증 실패 시 GError에 사용자 친화적 메시지 설정 + FALSE 반환.
 *   이 헤더의 상수(PCV_MAX_*, PCV_MIN_*)가 검증 규칙의 정의 원천입니다.
 *
 * [주의사항]
 *   상수 변경 시 단위 테스트(tests/test_validate.c)의 경계값 테스트도
 *   함께 업데이트해야 합니다.
 */

#ifndef PURECVISOR_VALIDATE_H
#define PURECVISOR_VALIDATE_H

#include <glib.h>

/* ── 런타임 디렉토리 ──────────────────────────────────── */
/**
 * PCV_NETWORK_RUNDIR - PureCVisor 네트워크 전용 런타임 디렉토리
 *
 * dnsmasq의 PID 파일, 설정 파일, DHCP 임대 DB, 브릿지 메타데이터를
 * /tmp 대신 이 경로에 저장합니다.
 *
 * [파일 목록 (/var/run/purecvisor/network/)]
 *   dnsmasq-<br>.pid    : dnsmasq 프로세스 PID (network.delete에서 종료에 사용)
 *   dnsmasq-<br>.conf   : dnsmasq 설정 파일 (DHCP 범위, 인터페이스 지정)
 *   dnsmasq-<br>.leases : DHCP 임대 DB (클라이언트 MAC→IP 매핑)
 *   dnsmasq-<br>.meta   : 브릿지 메타데이터 JSON ({"mode":"nat","cidr":"10.0.0.1/24"})
 *
 * daemon 시작 시 pcv_network_rundir_init()으로 생성(mode 0700).
 */
#define PCV_NETWORK_RUNDIR   "/var/run/purecvisor/network"

/* ── 길이 상한 ────────────────────────────────────────── */
/*
 * 각 상수는 해당 자원의 시스템 제한에 맞춰 설정되어 있습니다.
 * 변경 시 관련 시스템 제한을 반드시 확인하세요.
 */
#define PCV_MAX_VM_NAME      64    /* libvirt domain name + ZFS zvol 이름 안전 범위 */
#define PCV_MAX_SNAP_NAME    128   /* ZFS 스냅샷 이름 (256자 전체 경로 중 여유분) */
#define PCV_MAX_BRIDGE_NAME  16    /* IFNAMSIZ=16 (Linux 네트워크 인터페이스 이름 최대) */
#define PCV_MAX_IFACE_NAME   15    /* IFNAMSIZ-1=15 (커널 인터페이스 이름 실제 길이, NUL 제외) */
#define PCV_MAX_ISO_PATH     512   /* 절대 경로 최대 길이 (ext4 PATH_MAX=4096 내) */
#define PCV_MAX_CIDR_LEN     49    /* IPv6 최대: "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128" = 49자 */
#define PCV_MAX_REMOTE_HOST  253   /* DNS FQDN 전체 길이 제한 */
#define PCV_MAX_SSH_USER     64    /* Linux 사용자명 실용 상한 */

/* LXC 전용 상한 */
#define PCV_MAX_CONTAINER_IMAGE  128   /* "ubuntu:22.04" 형식 (distro + release) */
#define PCV_MAX_EXEC_CMD         1024  /* container.exec 명령어 길이 (DoS 방지) */

/* PCI 주소 상한 */
#define PCV_MAX_PCI_ADDR         16    /* "0000:3b:00.1" 형식 (DDDD:BB:SS.F) */

/* ── 범위 상한 ────────────────────────────────────────── */
/*
 * 하드웨어/소프트웨어 실용 범위에 맞춘 값입니다.
 * VM 생성 시 이 범위를 벗어나면 JSON-RPC -32602 에러가 반환됩니다.
 */
#define PCV_MIN_MEMORY_MB    128               /* QEMU 최소 메모리 (128MB) */
#define PCV_MAX_MEMORY_MB    (1024 * 1024)     /* 1TB (단일 VM 실용 상한) */
#define PCV_MIN_VCPU         1                 /* 최소 1 vCPU */
#define PCV_MAX_VCPU         256               /* QEMU SMP 상한 (x86_64) */
#define PCV_MIN_DISK_GB      1                 /* 최소 1GB 디스크 */
#define PCV_MAX_DISK_GB      65536             /* 64TB (ZFS zvol 실용 상한) */

/* ── 문자셋 검증 ─────────────────────────────────────── */

/**
 * pcv_validate_vm_name:
 * VM 이름 검증. [a-zA-Z0-9_-], 1~PCV_MAX_VM_NAME자.
 * libvirt domain 이름, ZFS zvol 이름, 셸 인수에 안전한 문자만 허용.
 */
[[nodiscard]] gboolean pcv_validate_vm_name(const gchar *name);

/**
 * pcv_validate_snap_name:
 * ZFS 스냅샷 이름 검증. [a-zA-Z0-9_-], 1~PCV_MAX_SNAP_NAME자.
 * '@' 뒤에 오는 스냅샷 부분만 이 함수로 검증합니다.
 */
gboolean pcv_validate_snap_name(const gchar *name);

/**
 * pcv_validate_bridge_name:
 * 네트워크 브리지 이름 검증. [a-zA-Z0-9_-], 1~PCV_MAX_BRIDGE_NAME자.
 * Linux IFNAMSIZ(16) 제한을 준수합니다.
 */
gboolean pcv_validate_bridge_name(const gchar *name);

/**
 * pcv_validate_remote_host:
 * backup.replicate 대상 노드 host/IP 검증.
 * DNS hostname/FQDN 또는 IPv4 리터럴만 허용하고, 셸 메타문자/공백/경로 문자를
 * 차단합니다.
 */
gboolean pcv_validate_remote_host(const gchar *host);

/**
 * pcv_validate_ssh_user:
 * backup.replicate SSH 사용자명 검증.
 * [a-zA-Z0-9_.-] 문자만 허용하고 '-' 시작은 option injection 방지를 위해
 * 거부합니다.
 */
gboolean pcv_validate_ssh_user(const gchar *user);

/**
 * pcv_validate_cidr:
 * IPv4 또는 IPv6 CIDR 형식 검증.
 *   - IPv4: "a.b.c.d/prefix" (각 octet 0~255, prefix 0~32)
 *   - IPv6: "xxxx:...:xxxx/prefix" (hex 그룹, prefix 0~128, :: 축약 허용)
 *
 * 예) "10.0.0.1/24" → TRUE,  "fd00::1/64" → TRUE,  "256.0.0.1/24" → FALSE
 */
gboolean pcv_validate_cidr(const gchar *cidr);

/**
 * pcv_validate_private_cidr:
 * pcv_validate_cidr() + 사설 대역 강제 (RFC1918, RFC6598 100.64/10, fc00::/7).
 * 0.0.0.0/0, 공인 IP, link-local, 멀티캐스트, loopback 거부.
 * network.create의 cidr 안전성 보장 (B4-C2).
 */
gboolean pcv_validate_private_cidr(const gchar *cidr);

/**
 * pcv_validate_network_create_params:
 * network.create 통합 검증 함수 (bridge_name, mode, cidr, physical_if).
 * 모드별 필수 파라미터가 다름:
 *   nat/isolated/routed: bridge_name + cidr 필수
 *   bridge: bridge_name + physical_if 필수 (cidr 불필요)
 */
gboolean pcv_validate_network_create_params(const gchar  *bridge_name,
                                            const gchar  *mode,
                                            const gchar  *cidr,
                                            const gchar  *physical_if,
                                            GError      **error);

/**
 * pcv_network_rundir_init:
 * PCV_NETWORK_RUNDIR 디렉토리를 생성합니다 (mode 0700).
 * daemon 시작 시 1회 호출. 이미 존재하면 무시 (멱등성).
 */
void pcv_network_rundir_init(void);

/**
 * pcv_validate_iso_path:
 * ISO 이미지 경로 검증.
 *  - 절대 경로 (/ 로 시작)
 *  - ".." 경로 순회 차단 (directory traversal 방지)
 *  - 1~PCV_MAX_ISO_PATH 자
 */
gboolean pcv_validate_iso_path(const gchar *path);

/**
 * pcv_validate_base_image_path:
 * vm.create base_image(cloud image) 경로 검증 (CMP-3 확장).
 *  - 절대 경로 (/ 로 시작)
 *  - ".." 경로 순회 차단
 *  - 확장자 allowlist: .qcow2/.qcow/.img/.raw
 * base_image는 qemu-img convert 입력으로 host FS에서 직접 읽히므로 iso_path와 동일한
 * 신뢰경계 검증이 필요하다 (임의 호스트 파일 흡입·경로순회 차단).
 */
gboolean pcv_validate_base_image_path(const gchar *path);

/**
 * 정수 범위 검증 — VM 생성 파라미터의 수치 값 검증
 *
 * pcv_validate_memory_mb(): 128 ~ 1,048,576 MB
 * pcv_validate_vcpu():      1 ~ 256
 * pcv_validate_disk_gb():   1 ~ 65,536 GB
 */
gboolean pcv_validate_memory_mb(gint64 mb);
gboolean pcv_validate_vcpu(gint64 count);
gboolean pcv_validate_disk_gb(gint64 gb);

/**
 * pcv_validate_vm_create_params:
 * vm.create 전체 파라미터 통합 검증.
 * GError를 설정하는 통합 검증 함수 (dispatcher에서 편리하게 사용).
 * 실패 시 error에 사용자 친화적 메시지를 담고 FALSE 반환.
 * iso_path와 bridge는 nullable — NULL이면 해당 검증을 건너뜁니다.
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
 * pcv_validate_container_image:
 * 컨테이너 이미지 이름 검증. "distro:release" 형식.
 *  - distro : [a-z][a-z0-9-]* (소문자 영문자로 시작)
 *  - release: [a-z0-9][a-z0-9._-]*
 *  - 콜론(:) 구분자 필수
 *  - 1~PCV_MAX_CONTAINER_IMAGE 자
 * 예) "ubuntu:22.04", "debian:bookworm", "alpine:3.18"
 */
gboolean pcv_validate_container_image(const gchar *image);

/**
 * pcv_validate_exec_cmd:
 * container.exec 명령어 문자열 검증.
 *  - NULL 바이트 없음 (C 문자열 안전성)
 *  - 1~PCV_MAX_EXEC_CMD 자 (DoS 방지)
 *  (lxc-attach + /bin/sh -c 로 컨테이너 내부에서 실행됨)
 */
gboolean pcv_validate_exec_cmd(const gchar *cmd);

/* ── PCI 주소 검증 (SR-IOV / DPDK Phase 4) ───────────── */

/**
 * pcv_validate_pci_addr:
 * PCI BDF 주소 검증. DDDD:BB:SS.F (domain:bus:slot.function) 형식.
 *  - 각 필드 16진수, 콜론/점 구분
 *  - 범위: D(0~FFFF), B(0~FF), S(0~1F), F(0~7)
 *  - 경로 순회(..) 차단 (sysfs 인젝션 방지)
 *  - 예: "0000:01:00.0", "0000:3b:10.1"
 */
gboolean pcv_validate_pci_addr(const gchar *addr);

/* ── 추가 검증 함수 (입력 검증 강화) ────────────────────── */

/**
 * pcv_validate_disk_size_gb:
 * 디스크 크기(GB) 검증 — REST/RPC 핸들러용 간편 범위 검증.
 * 허용: 1 ~ 2048 GB (2TB, 일반 VM 실용 상한).
 * pcv_validate_disk_gb()는 ZFS zvol 최대 64TB를 허용하지만
 * 이 함수는 일반 디스크 작업에 적합한 보수적 상한을 적용합니다.
 */
gboolean pcv_validate_disk_size_gb(gint size);

/**
 * pcv_validate_port:
 * TCP/UDP 포트 번호 범위 검증.
 * 허용: 1 ~ 65535 (0은 와일드카드이므로 제외).
 */
gboolean pcv_validate_port(gint port);

/**
 * pcv_validate_zvol_name:
 * ZFS zvol 이름 검증.
 * 허용: [a-zA-Z0-9][a-zA-Z0-9_.-]{0,63} (영숫자 시작, 최대 64자).
 * ".." 시퀀스 차단 (경로 순회 방지).
 */
gboolean pcv_validate_zvol_name(const gchar *name);

/* ── 네트워크/방화벽 입력 검증 (whitelist) ─────────────── */

/**
 * pcv_validate_iface_name:
 * Linux 네트워크 인터페이스 이름 검증. [a-zA-Z0-9_.-], 1~PCV_MAX_IFACE_NAME(15)자.
 * bridge_name과 달리 '.'을 허용해 VLAN 서브인터페이스(eth0.100)를 수용합니다.
 * 선행 '-'는 option injection 방지를 위해 명시적으로 거부합니다.
 */
gboolean pcv_validate_iface_name(const gchar *name);

/**
 * pcv_validate_mac:
 * MAC 주소 검증. 정확히 "xx:xx:xx:xx:xx:xx" (17자, 16진수 + 콜론).
 * 콜론은 위치 2,5,8,11,14에만 허용하고 나머지는 16진수만 허용합니다.
 * 그 외 길이/문자/형식은 모두 거부합니다.
 */
gboolean pcv_validate_mac(const gchar *mac);

/**
 * pcv_validate_ip_literal:
 * IPv4 또는 IPv6 리터럴 검증 (CIDR 접미사 불허).
 * inet_pton(AF_INET) 또는 inet_pton(AF_INET6)으로 판정하며,
 * NULL/빈 문자열을 거부합니다.
 */
gboolean pcv_validate_ip_literal(const gchar *ip);

/**
 * pcv_validate_ipv6_prefix:
 * "<ipv6-literal>/<len>" 형식 검증 (len 0~128, 마지막 '/'로 분리).
 * 주소부는 inet_pton(AF_INET6) 통과 + [0-9a-fA-F:] 문자셋만 허용하며,
 * 문자열 어디든 개행('\n')/공백(' ')이 있으면 거부합니다.
 * dnsmasq 설정 인젝션 방어 — inet_pton 단독에 의존하지 않습니다.
 */
gboolean pcv_validate_ipv6_prefix(const gchar *prefix);

/**
 * pcv_validate_l4_proto:
 * L4 프로토콜 이름 검증. "tcp" | "udp" | "icmp" (소문자, 대소문자 구분).
 * 그 외 값과 NULL을 거부합니다.
 */
gboolean pcv_validate_l4_proto(const gchar *proto);

/**
 * pcv_validate_password_complexity:
 * 신규 사용자 생성 비밀번호 강도 정책(Q-2 / A07): 최소 길이 12 + 4개 문자군
 * (소문자/대문자/숫자/특수) 중 3종 이상. 실패 시 @reason(선택)에 정적 사유
 * 문자열을 설정한다(호출자 해제 불요). auth.user.create 생성 경로 전용 —
 * 기존 사용자·로그인 경로에는 적용하지 않는다.
 */
gboolean pcv_validate_password_complexity(const gchar *password,
                                          const gchar **reason);

#endif /* PURECVISOR_VALIDATE_H */
