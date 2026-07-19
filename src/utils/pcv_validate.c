/**
 * @file pcv_validate.c
 * @brief 입력값 검증 레이어 — RPC 파라미터 보안 검증의 최전방 방어선
 *
 * Sprint A-3에서 도입, Sprint I(네트워크), Phase 4(SR-IOV/DPDK)에서 확장.
 * 디스패처(dispatcher.c)가 RPC 파라미터를 핸들러에 넘기기 전에
 * 이 모듈의 함수로 모든 입력값을 검증합니다.
 *
 * [아키텍처 위치]
 *   클라이언트 → UDS/REST → dispatcher.c → pcv_validate_*() → 핸들러
 *   검증 실패 시 GError + JSON-RPC -32602 (Invalid params) 응답 즉시 반환.
 *   핸들러까지 도달하는 파라미터는 항상 검증 완료 상태입니다.
 *
 *   이 모듈은 "신뢰 경계(trust boundary)" 역할을 합니다:
 *     외부 입력 (신뢰할 수 없음) → [검증] → 내부 모듈 (신뢰할 수 있음)
 *
 * [검증 함수 분류]
 *   문자셋 검증 (safe token):
 *     pcv_validate_vm_name()      : [a-zA-Z0-9_-], 1~64자
 *     pcv_validate_snap_name()    : [a-zA-Z0-9_-], 1~128자
 *     pcv_validate_bridge_name()  : [a-zA-Z0-9_-], 1~16자
 *
 *   경로/형식 검증:
 *     pcv_validate_iso_path()     : 절대경로, ".." 순회 차단
 *     pcv_validate_cidr()         : IPv4/IPv6 CIDR (IPv4: "a.b.c.d/0-32", IPv6: "xxxx::xxxx/0-128")
 *     pcv_validate_pci_addr()     : PCI "DDDD:BB:SS.F" 16진수 형식
 *     pcv_validate_container_image(): "distro:release" 형식
 *     pcv_validate_exec_cmd()     : NULL 바이트 없음, 1~1024자
 *
 *   범위 검증:
 *     pcv_validate_memory_mb()    : 128 ~ 1,048,576 (1TB)
 *     pcv_validate_vcpu()         : 1 ~ 256
 *     pcv_validate_disk_gb()      : 1 ~ 65,536 (64TB)
 *
 *   통합 검증 (GError 반환):
 *     pcv_validate_vm_create_params()      : vm.create 전체 파라미터
 *     pcv_validate_network_create_params() : network.create 전체 파라미터
 *
 *   유틸리티:
 *     pcv_network_rundir_init()   : /var/run/purecvisor/network 디렉터리 생성
 *
 * [보안 원칙]
 *   - 화이트리스트 방식: 허용된 문자/범위만 통과, 나머지 거부
 *     (블랙리스트는 우회 가능하므로 사용하지 않음)
 *   - 경로 순회 차단: ".." 패턴 명시적 차단 (iso_path, pci_addr)
 *   - 셸 인젝션 방지: 검증된 safe token만 외부 명령 인수로 사용
 *     (pcv_spawn.c의 argv 배열 방식과 조합하여 이중 방어)
 *   - 길이 제한: 모든 문자열에 최대 길이 적용 (버퍼 오버플로 방지)
 *
 * [다른 모듈과의 관계]
 *   - dispatcher.c       : vm.create 파라미터 검증 → pcv_validate_vm_create_params()
 *   - network_manager.c  : bridge/cidr 검증 → pcv_validate_network_create_params()
 *   - handler_accel.c    : PCI 주소 검증 → pcv_validate_pci_addr()
 *   - handler_container.c: 이미지/명령 검증 → pcv_validate_container_image/exec_cmd()
 *   - handler_template.c : 템플릿 이름 검증 → pcv_validate_vm_name()
 *
 * [include 경로 규칙]
 *   pcv_validate.c → #include "pcv_validate.h" (상대경로 없이)
 *   다른 모듈에서 → #include "../../utils/pcv_validate.h" (상대경로)
 *   또는 → #include "purecvisor/pcv_validate.h" (include/ 경로)
 *
 * [주의사항]
 *   - bridge 모드에서는 cidr 미검증 (물리 NIC 브릿지이므로 IP 불필요)
 *   - pci_addr 검증: sscanf %n으로 전체 소비 확인 (부분 매칭 방지)
 */

#include "pcv_validate.h"
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>   /* struct in_addr / in6_addr */
#include <arpa/inet.h>    /* inet_pton (IP 리터럴 검증) */

/* ── 내부 헬퍼 ────────────────────────────────────────── */

/**
 * is_safe_token - 안전한 식별자 문자셋 검증 (화이트리스트 방식)
 * @s:       검증할 문자열
 * @max_len: 허용 최대 길이
 *
 * @return: TRUE이면 모든 문자가 허용 범위 내
 *
 * [허용 문자]
 *   a-z, A-Z : 알파벳 대소문자
 *   0-9      : 숫자
 *   -        : 하이픈 (DNS 호환, VM 이름에 자주 사용)
 *   _        : 언더스코어 (프로그래밍 관례)
 *
 * [차단 문자 (보안상)]
 *   공백, 점(.), 슬래시(/), 세미콜론(;), 파이프(|), 앰퍼샌드(&),
 *   백틱(`), 달러($) 등 셸 메타문자가 모두 차단됩니다.
 *
 * [사용처]
 *   VM 이름, 스냅샷 이름, 브릿지 이름 등 외부 명령의 인수로 사용되는
 *   식별자를 검증합니다. 이 검증을 통과한 문자열은 셸 인젝션에 안전합니다.
 *
 * [왜 g_ascii_isalnum()을 사용하는가?]
 *   g_unichar_isalnum()은 유니코드 전체 범위를 허용하여
 *   제어 문자나 특수 문자가 통과할 수 있습니다.
 *   g_ascii_isalnum()은 ASCII 범위로 한정하여 안전합니다.
 */
static gboolean is_safe_token(const gchar *s, gsize max_len) {
    if (!s || *s == '\0') return FALSE;       /* NULL 또는 빈 문자열 거부 */
    gsize len = strlen(s);
    if (len > max_len) return FALSE;          /* 길이 초과 거부 */
    for (const gchar *p = s; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;                     /* 비허용 문자 발견 → 즉시 거부 */
    }
    return TRUE;
}

static gboolean
is_ipv4_literal(const gchar *host)
{
    gchar **parts = g_strsplit(host, ".", 5);
    if (!parts || g_strv_length(parts) != 4) {
        g_strfreev(parts);
        return FALSE;
    }

    for (guint i = 0; i < 4; i++) {
        gsize len = strlen(parts[i]);
        if (len == 0 || len > 3) {
            g_strfreev(parts);
            return FALSE;
        }
        for (const gchar *p = parts[i]; *p; p++) {
            if (!g_ascii_isdigit(*p)) {
                g_strfreev(parts);
                return FALSE;
            }
        }
        gint value = atoi(parts[i]);
        if (value < 0 || value > 255) {
            g_strfreev(parts);
            return FALSE;
        }
    }

    g_strfreev(parts);
    return TRUE;
}

/* ── 공개 API ─────────────────────────────────────────── */

/**
 * pcv_validate_vm_name - VM 이름 검증
 * @name: 검증할 VM 이름 문자열
 *
 * @return: TRUE이면 유효, FALSE이면 거부
 *
 * 허용: [a-zA-Z0-9_-], 최대 64자 (PCV_MAX_VM_NAME).
 * libvirt domain 이름 규칙과 호환되도록 설계됨.
 *
 * [사용 예시]
 *   if (!pcv_validate_vm_name(params_name)) {
 *       g_set_error(error, ..., "Invalid VM name");
 *       return;
 *   }
 *
 * [libvirt 호환]
 *   libvirt는 domain 이름에 더 넓은 문자셋을 허용하지만,
 *   ZFS zvol 이름, 셸 명령 인수 등에서 안전하게 사용하기 위해
 *   ASCII 영숫자 + 하이픈 + 언더스코어로 제한합니다.
 */
gboolean pcv_validate_vm_name(const gchar *name) {
    return is_safe_token(name, PCV_MAX_VM_NAME);
}

/**
 * pcv_validate_snap_name - ZFS 스냅샷 이름 검증
 * @name: 검증할 스냅샷 이름 문자열
 *
 * @return: TRUE이면 유효
 *
 * 허용: [a-zA-Z0-9_-], 최대 128자 (PCV_MAX_SNAP_NAME).
 * ZFS 스냅샷명은 '@' 뒤에 오는 부분만 검증 (데이터셋 경로는 별도).
 *
 * [ZFS 스냅샷 형식]
 *   전체 경로: pcvpool/vms/web-prod@daily-20260326
 *   '@' 뒤 부분(daily-20260326)만 이 함수로 검증합니다.
 *   128자 제한은 ZFS의 256자 전체 경로 제한에서 데이터셋 경로를 제외한 여유분입니다.
 */
gboolean pcv_validate_snap_name(const gchar *name) {
    return is_safe_token(name, PCV_MAX_SNAP_NAME);
}

/**
 * pcv_validate_bridge_name - 브릿지 인터페이스 이름 검증
 * @name: 검증할 브릿지 이름 문자열
 *
 * @return: TRUE이면 유효
 *
 * 허용: [a-zA-Z0-9_-], 최대 16자 (PCV_MAX_BRIDGE_NAME).
 * Linux 네트워크 인터페이스 이름 길이 제한(IFNAMSIZ=16)에 맞춤.
 *
 * [IFNAMSIZ 제한]
 *   Linux 커널의 네트워크 인터페이스 이름은 최대 15자 + NUL 종단자 = 16바이트입니다.
 *   <linux/if.h>에서 IFNAMSIZ=16으로 정의됩니다.
 *   16자를 초과하는 브릿지 이름은 ip link/brctl 명령에서 거부됩니다.
 */
gboolean pcv_validate_bridge_name(const gchar *name) {
    return is_safe_token(name, PCV_MAX_BRIDGE_NAME);
}

gboolean
pcv_validate_remote_host(const gchar *host)
{
    if (!host || *host == '\0') return FALSE;

    gsize len = strlen(host);
    if (len > PCV_MAX_REMOTE_HOST) return FALSE;
    if (host[0] == '-' || host[0] == '.') return FALSE;
    if (g_str_has_suffix(host, ".")) return FALSE;

    gboolean digit_dot_only = TRUE;
    for (const gchar *p = host; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '.')
            return FALSE;
        if (!g_ascii_isdigit(*p) && *p != '.')
            digit_dot_only = FALSE;
    }

    if (digit_dot_only)
        return is_ipv4_literal(host);

    gchar **labels = g_strsplit(host, ".", -1);
    if (!labels) return FALSE;

    gboolean ok = TRUE;
    for (guint i = 0; labels[i]; i++) {
        const gchar *label = labels[i];
        gsize label_len = strlen(label);
        if (label_len == 0 || label_len > 63) {
            ok = FALSE;
            break;
        }
        if (!g_ascii_isalnum(label[0]) ||
            !g_ascii_isalnum(label[label_len - 1])) {
            ok = FALSE;
            break;
        }
        for (const gchar *p = label; *p; p++) {
            if (!g_ascii_isalnum(*p) && *p != '-') {
                ok = FALSE;
                break;
            }
        }
        if (!ok) break;
    }

    g_strfreev(labels);
    return ok;
}

gboolean
pcv_validate_ssh_user(const gchar *user)
{
    if (!user || *user == '\0') return FALSE;

    gsize len = strlen(user);
    if (len > PCV_MAX_SSH_USER) return FALSE;
    if (user[0] == '-') return FALSE;
    if (!g_ascii_isalnum(user[0]) && user[0] != '_') return FALSE;

    for (const gchar *p = user; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '.' && *p != '-')
            return FALSE;
    }

    return TRUE;
}

/**
 * pcv_validate_iso_path - ISO 이미지 경로 검증
 * @path: 검증할 파일 경로
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   1. 절대 경로만 허용 ('/'로 시작)
 *   2. ".." 경로 순회(directory traversal) 차단
 *   3. 최대 길이 PCV_MAX_ISO_PATH (512자)
 *
 * [보안 의미]
 *   vm.create/vm.mount_iso에서 ISO 경로를 QEMU에 전달하므로,
 *   경로 순회 공격을 방지하여 임의 파일 접근을 차단합니다.
 *
 *   공격 시나리오:
 *     악의적 입력: "/pcvpool/iso/../../../etc/shadow"
 *     → ".." 패턴 탐지 → 거부 → shadow 파일 접근 차단
 *
 * [상대 경로 차단 이유]
 *   상대 경로("./iso/ubuntu.iso")는 데몬의 cwd에 의존하므로
 *   예측 불가능한 파일에 접근할 수 있습니다.
 *   절대 경로만 허용하여 명확성을 보장합니다.
 *
 * [주의]
 *   파일 존재 여부는 검증하지 않습니다 (존재 검증은 핸들러에서 수행).
 *   심볼릭 링크를 통한 우회도 차단하지 않습니다.
 *   실제 파일 접근 시 realpath() 등의 추가 검증을 고려할 수 있습니다.
 */
gboolean pcv_validate_iso_path(const gchar *path) {
    if (!path || *path == '\0') return FALSE;
    if (strlen(path) > PCV_MAX_ISO_PATH) return FALSE;

    /* 절대 경로만 허용 — 상대 경로는 cwd 의존으로 위험 */
    if (path[0] != '/') return FALSE;

    /*
     * ".." 경로 순회 차단:
     *   "/../" : 중간 위치 경로 순회
     *   "/.."  : 경로 끝에서의 순회 (접미사)
     *   ".."   : 경로 전체가 ".."인 경우
     */
    if (strstr(path, "/../") != NULL) return FALSE;
    if (g_str_has_suffix(path, "/.."))  return FALSE;
    if (g_strcmp0(path, "..") == 0)     return FALSE;

    /* 확장자 검증: .iso 또는 .img만 허용 (대소문자 무시) */
    gchar *lower = g_ascii_strdown(path, -1);
    gboolean ext_ok = g_str_has_suffix(lower, ".iso") ||
                      g_str_has_suffix(lower, ".img");
    g_free(lower);
    if (!ext_ok) return FALSE;

    return TRUE;
}

/**
 * pcv_validate_base_image_path - vm.create base_image(cloud image) 경로 검증
 * @path: 검증할 base image 파일 경로
 *
 * @return: TRUE이면 유효
 *
 * [배경 — CMP-3 확장]
 *   vm.create의 base_image는 vm_manager.c에서 qemu-img convert의 입력으로 host FS에서
 *   직접 읽혀 새 VM 디스크(zvol)에 기록된다. iso_path와 동일한 신뢰경계 문제 —
 *   미검증 시 "/etc/shadow" 같은 임의 호스트 파일을 VM 디스크로 흡입하거나 경로순회로
 *   시스템 파일에 접근할 수 있다. iso_path와 같은 패턴으로 실검증한다.
 *
 * [검증 규칙 — iso_path와 동형]
 *   1. 절대 경로만 허용 ('/'로 시작)
 *   2. ".." 경로 순회 차단
 *   3. 최대 길이 PCV_MAX_ISO_PATH (512자)
 *   4. 확장자 allowlist: .qcow2/.qcow/.img/.raw (cloud image 통용 포맷; iso와 달리
 *      base_image는 디스크 이미지이므로 확장자 집합만 다르다)
 *
 * [주의]
 *   iso_path와 동일하게 파일 존재/심볼릭링크는 검증하지 않는다(호출측 g_file_test로 별도).
 */
gboolean pcv_validate_base_image_path(const gchar *path) {
    if (!path || *path == '\0') return FALSE;
    if (strlen(path) > PCV_MAX_ISO_PATH) return FALSE;

    /* 절대 경로만 허용 — 상대 경로는 cwd 의존으로 위험 */
    if (path[0] != '/') return FALSE;

    /* ".." 경로 순회 차단 (iso_path와 동일 규칙) */
    if (strstr(path, "/../") != NULL) return FALSE;
    if (g_str_has_suffix(path, "/.."))  return FALSE;
    if (g_strcmp0(path, "..") == 0)     return FALSE;

    /* 확장자 allowlist: cloud/디스크 이미지 통용 포맷 (대소문자 무시) */
    gchar *lower = g_ascii_strdown(path, -1);
    gboolean ext_ok = g_str_has_suffix(lower, ".qcow2") ||
                      g_str_has_suffix(lower, ".qcow")  ||
                      g_str_has_suffix(lower, ".img")   ||
                      g_str_has_suffix(lower, ".raw");
    g_free(lower);
    if (!ext_ok) return FALSE;

    return TRUE;
}

/**
 * pcv_validate_memory_mb - 메모리 크기(MB) 범위 검증
 * @mb: 검증할 메모리 크기 (MB 단위)
 *
 * @return: TRUE이면 유효 범위 내
 *
 * 범위: 128MB ~ 1,048,576MB (1TB)
 * 최소값 128MB: QEMU가 안정적으로 동작하는 최소 메모리
 * 최대값 1TB: 단일 VM에 할당 가능한 물리적 상한
 */
gboolean pcv_validate_memory_mb(gint64 mb) {
    return mb >= PCV_MIN_MEMORY_MB && mb <= PCV_MAX_MEMORY_MB;
}

/**
 * pcv_validate_vcpu - vCPU 수 범위 검증
 * @count: 검증할 vCPU 수
 *
 * @return: TRUE이면 유효 범위 내
 *
 * 범위: 1 ~ 256
 * 최소값 1: 모든 VM은 최소 1개 CPU 필요
 * 최대값 256: QEMU의 SMP 제한 (x86_64 기준)
 */
gboolean pcv_validate_vcpu(gint64 count) {
    return count >= PCV_MIN_VCPU && count <= PCV_MAX_VCPU;
}

/**
 * pcv_validate_disk_gb - 디스크 크기(GB) 범위 검증
 * @gb: 검증할 디스크 크기 (GB 단위)
 *
 * @return: TRUE이면 유효 범위 내
 *
 * 범위: 1GB ~ 65,536GB (64TB)
 * 최소값 1GB: OS 설치에 필요한 최소 디스크
 * 최대값 64TB: ZFS zvol 단일 볼륨 실용 상한
 */
gboolean pcv_validate_disk_gb(gint64 gb) {
    return gb >= PCV_MIN_DISK_GB && gb <= PCV_MAX_DISK_GB;
}

/**
 * pcv_validate_vm_create_params - vm.create 전체 파라미터 통합 검증
 * @name:      VM 이름 (필수, [a-zA-Z0-9_-], 1~64자)
 * @vcpu:      vCPU 수 (1~256)
 * @memory_mb: 메모리 크기 MB (128~1048576)
 * @disk_gb:   디스크 크기 GB (1~65536)
 * @iso_path:  ISO 경로 (NULL 가능 — NULL이면 ISO 검증 생략, 디스크부트)
 * @bridge:    네트워크 브릿지 이름 (NULL 가능 — NULL이면 기본 네트워크)
 * @error:     실패 시 GError 설정 (호출자가 g_error_free()로 해제)
 *
 * @return: 모든 파라미터 유효 시 TRUE, 하나라도 실패 시 FALSE + error 설정
 *
 * dispatcher.c에서 vm.create 핸들러 진입 전에 호출하여
 * 잘못된 파라미터가 하위 모듈(vm_manager, virt-install)까지
 * 전달되는 것을 방지합니다.
 *
 * [검증 순서와 우선순위]
 *   가장 기본적인 항목(name)부터 검증하여, 첫 번째 실패에서
 *   즉시 반환합니다. 에러 메시지에 실패 원인과 허용 범위를 포함하여
 *   클라이언트가 오류를 빠르게 수정할 수 있도록 합니다.
 *
 * [GError 패턴]
 *   에러 도메인: G_IO_ERROR (GIO 표준 도메인)
 *   에러 코드: G_IO_ERROR_INVALID_ARGUMENT (파라미터 검증 실패)
 *   메시지: 검증 실패 원인 + 허용 범위 (영문, 사용자 친화적)
 */
gboolean pcv_validate_vm_create_params(const gchar  *name,
                                       gint64        vcpu,
                                       gint64        memory_mb,
                                       gint64        disk_gb,
                                       const gchar  *iso_path,
                                       const gchar  *bridge,
                                       GError      **error) {
    if (!pcv_validate_vm_name(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid VM name '%s': must be 1-%d chars [a-zA-Z0-9_-]",
            name ? name : "(null)", PCV_MAX_VM_NAME);
        return FALSE;
    }
    if (!pcv_validate_vcpu(vcpu)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid vcpu %" G_GINT64_FORMAT ": must be %d-%d",
            vcpu, PCV_MIN_VCPU, PCV_MAX_VCPU);
        return FALSE;
    }
    if (!pcv_validate_memory_mb(memory_mb)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid memory_mb %" G_GINT64_FORMAT ": must be %d-%d",
            memory_mb, PCV_MIN_MEMORY_MB, PCV_MAX_MEMORY_MB);
        return FALSE;
    }
    if (!pcv_validate_disk_gb(disk_gb)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid disk_size_gb %" G_GINT64_FORMAT ": must be %d-%d",
            disk_gb, PCV_MIN_DISK_GB, PCV_MAX_DISK_GB);
        return FALSE;
    }
    /* iso_path가 NULL이면 디스크 부팅 (ISO 불필요) → 검증 생략 */
    if (iso_path && !pcv_validate_iso_path(iso_path)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid iso_path '%s': must be absolute path without '..'", iso_path);
        return FALSE;
    }
    /* bridge가 NULL이면 기본 네트워크 사용 → 검증 생략 */
    if (bridge && !pcv_validate_bridge_name(bridge)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid network_bridge '%s': must be 1-%d chars [a-zA-Z0-9_-]",
            bridge, PCV_MAX_BRIDGE_NAME);
        return FALSE;
    }
    return TRUE;
}

/* ══════════════════════════════════════════════════════
 * LXC 컨테이너 전용 검증
 *
 * container.create, container.exec RPC 핸들러에서 사용됩니다.
 * LXC 다운로드 템플릿(lxc-download)의 이미지 명명 규칙에 맞춤.
 * ══════════════════════════════════════════════════════*/

/**
 * pcv_validate_container_image - LXC 컨테이너 이미지 이름 검증
 * @image: "distro:release" 형식 문자열 (예: "ubuntu:22.04", "alpine:3.18")
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - 콜론(:)으로 distro:release 분리 (콜론 필수, 정확히 1개)
 *   - distro: 소문자 영문으로 시작, [a-z0-9-] 허용
 *     (예: "ubuntu", "debian", "alpine", "rocky-linux")
 *   - release: [a-z0-9._-] 허용
 *     (예: "22.04", "bookworm", "3.18", "9.3-minimal")
 *   - 최대 길이 PCV_MAX_CONTAINER_IMAGE (128자)
 *
 * [LXC 다운로드 템플릿과의 관계]
 *   lxc-download는 "distro/release/arch" 형식을 사용하지만,
 *   PureCVisor는 "distro:release" 형식으로 단순화했습니다.
 *   아키텍처(arch)는 호스트와 동일하게 자동 결정됩니다.
 *
 * [왜 대문자를 금지하는가?]
 *   LXC 이미지 저장소(images.linuxcontainers.org)의 모든 이미지가
 *   소문자 이름을 사용합니다. 대문자 입력은 오타일 가능성이 높습니다.
 */
gboolean
pcv_validate_container_image(const gchar *image)
{
    if (!image) return FALSE;

    gsize len = strlen(image);
    if (len == 0 || len > PCV_MAX_CONTAINER_IMAGE) return FALSE;

    /* 콜론으로 distro:release 분리 */
    const gchar *colon = strchr(image, ':');
    if (!colon || colon == image) return FALSE;       /* 콜론 없거나 distro가 빈 문자열 */
    if (*(colon + 1) == '\0') return FALSE;           /* release가 빈 문자열 */

    /* distro: 소문자 영문으로 시작, [a-z0-9-] 허용 */
    for (const gchar *p = image; p < colon; p++) {
        gchar c = *p;
        /* 첫 문자는 반드시 소문자 알파벳 (숫자/하이픈으로 시작 금지) */
        if (p == image && !(c >= 'a' && c <= 'z')) return FALSE;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return FALSE;
    }

    /* release: [a-z0-9._-] 허용 (버전 번호에 점, 언더스코어 사용) */
    for (const gchar *p = colon + 1; *p; p++) {
        gchar c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '.' || c == '_' || c == '-'))
            return FALSE;
    }

    return TRUE;
}

/**
 * pcv_validate_exec_cmd - 컨테이너 exec 명령어 문자열 검증
 * @cmd: 실행할 명령어 문자열
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - NULL 바이트 포함 금지 (C 문자열 안전성)
 *   - 1~1024자 길이 제한 (PCV_MAX_EXEC_CMD)
 *
 * [보안 고려사항]
 *   실제 명령어 실행은 lxc-attach를 통하므로 셸 메타문자는
 *   LXC가 처리합니다. 여기서는 기본 무결성만 검증합니다.
 *   - NULL 바이트: C 문자열 처리에서 예기치 않은 절단 방지
 *   - 길이 제한: 메모리 소모 공격 방지
 *
 * [NULL 바이트 검사의 의미]
 *   strlen()은 첫 NULL에서 멈추므로, 이론적으로 for 루프에서
 *   NULL을 만날 수 없습니다. 그러나 방어적 프로그래밍 원칙에 따라
 *   명시적으로 확인합니다 (향후 바이너리 입력 처리 시 안전).
 */
gboolean
pcv_validate_exec_cmd(const gchar *cmd)
{
    if (!cmd) return FALSE;

    gsize len = strlen(cmd);
    if (len == 0 || len > PCV_MAX_EXEC_CMD) return FALSE;

    /* NULL 바이트 포함 여부 (strlen 이후라도 명시적 확인 — 방어적 프로그래밍) */
    for (gsize i = 0; i < len; i++) {
        if (cmd[i] == '\0') return FALSE;
    }

    return TRUE;
}

/* ══════════════════════════════════════════════════════
 * Sprint I: 네트워크 모듈 검증 강화
 *
 * network.create, network.mode_set 등 네트워크 관련 RPC에서
 * 사용되는 CIDR, 브릿지 모드, 물리 인터페이스 이름을 검증합니다.
 * ══════════════════════════════════════════════════════*/

#include <sys/stat.h>
#include <errno.h>

/**
 * _validate_ipv4_cidr - IPv4 CIDR 내부 검증 헬퍼
 * @ip_part: '/' 앞의 IP 문자열 (예: "10.0.0.1")
 * @prefix: 프리픽스 길이
 *
 * @return: TRUE이면 유효한 IPv4 CIDR
 */
static gboolean _validate_ipv4_cidr(const gchar *ip_part, gint prefix)
{
    if (prefix > 32) return FALSE;

    gchar **octs = g_strsplit(ip_part, ".", 5);  /* 5: 4개 초과 감지용 */

    /* 정확히 4 옥텟이어야 함 */
    if (!octs || g_strv_length(octs) != 4) {
        g_strfreev(octs);
        return FALSE;
    }
    for (int i = 0; i < 4; i++) {
        /* 각 옥텟이 순수 숫자인지 확인 */
        for (const gchar *p = octs[i]; *p; p++)
            if (!g_ascii_isdigit(*p)) { g_strfreev(octs); return FALSE; }
        /* 옥텟 범위 검사 (0~255) */
        gint v = atoi(octs[i]);
        if (v < 0 || v > 255) { g_strfreev(octs); return FALSE; }
    }
    g_strfreev(octs);
    return TRUE;
}

/**
 * _validate_ipv6_cidr - IPv6 CIDR 내부 검증 헬퍼
 * @ip_part: '/' 앞의 IPv6 문자열 (예: "fd00::1", "2001:db8::1")
 * @prefix: 프리픽스 길이
 *
 * [검증 규칙]
 *   - 각 그룹은 1~4자리 16진수
 *   - :: 축약은 최대 1회 허용
 *   - 축약 없을 때 정확히 8그룹, 축약 시 8그룹 이하
 *   - prefix: 0~128
 *
 * @return: TRUE이면 유효한 IPv6 CIDR
 */
static gboolean _validate_ipv6_cidr(const gchar *ip_part, gint prefix)
{
    if (prefix > 128) return FALSE;
    if (!ip_part || strlen(ip_part) == 0) return FALSE;

    /* :: 축약 횟수 확인 (최대 1회) */
    const gchar *dcolon = strstr(ip_part, "::");
    gboolean has_dcolon = (dcolon != NULL);
    if (has_dcolon) {
        /* 두 번째 :: 가 있으면 무효 */
        if (strstr(dcolon + 2, "::") != NULL) return FALSE;
    }

    /* ':' 구분으로 그룹 분리 */
    gchar **groups = g_strsplit(ip_part, ":", -1);
    if (!groups) return FALSE;

    guint n_groups = g_strv_length(groups);

    /* :: 축약 시 빈 문자열 그룹이 생김 — 그룹 수 확인 */
    guint non_empty = 0;
    for (guint i = 0; i < n_groups; i++) {
        if (strlen(groups[i]) > 0) non_empty++;
    }

    /* 축약 없으면 정확히 8그룹, 축약 있으면 비어있지 않은 그룹이 7개 이하 */
    if (!has_dcolon && non_empty != 8) {
        g_strfreev(groups);
        return FALSE;
    }
    if (has_dcolon && non_empty > 7) {
        g_strfreev(groups);
        return FALSE;
    }

    /* 각 그룹이 유효한 16진수인지 확인 (1~4자리) */
    for (guint i = 0; i < n_groups; i++) {
        const gchar *g = groups[i];
        gsize len = strlen(g);
        if (len == 0) continue;  /* :: 축약으로 인한 빈 그룹 */
        if (len > 4) { g_strfreev(groups); return FALSE; }
        for (const gchar *p = g; *p; p++) {
            if (!g_ascii_isxdigit(*p)) { g_strfreev(groups); return FALSE; }
        }
    }

    g_strfreev(groups);
    return TRUE;
}

/**
 * pcv_validate_cidr - IPv4 또는 IPv6 CIDR 표기법 검증
 * @cidr: "a.b.c.d/prefix" 또는 "xxxx:...:xxxx/prefix" 형식 문자열
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - IPv4: "a.b.c.d/prefix" (4 옥텟 각 0~255, prefix 0~32)
 *   - IPv6: "xxxx:...:xxxx/prefix" (hex 그룹, prefix 0~128, :: 축약 허용)
 *   - 최대 길이: PCV_MAX_CIDR_LEN (49자)
 *   - '.' 포함 시 IPv4로, ':' 포함 시 IPv6로 판별
 *
 * [사용처]
 *   network.create RPC의 cidr 파라미터 검증.
 *   dnsmasq DHCP 범위 계산, nftables 규칙 생성에 사용됩니다.
 *
 * [주의: CIDR 의미론 검증 미포함]
 *   네트워크 주소 정합성(예: 10.0.0.5/24 -> 호스트 비트 존재)은
 *   검증하지 않습니다. 형식적 유효성만 확인합니다.
 *   이는 dnsmasq와 nftables가 자체적으로 의미론을 검증하기 때문입니다.
 */
/* B4-C2: IPv4 사설 대역 / link-local / loopback / 멀티캐스트 검증 */
static gboolean
_is_safe_private_ipv4(const gchar *ip_str)
{
    guint o[4];
    if (sscanf(ip_str, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) != 4) return FALSE;
    if (o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255) return FALSE;
    /* RFC 1918 */
    if (o[0] == 10) return TRUE;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return TRUE;
    if (o[0] == 192 && o[1] == 168) return TRUE;
    /* RFC 6598 — Carrier-grade NAT (운영자 격리 환경) */
    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return TRUE;
    return FALSE;
}

/* B4-C2 (Phase 2 fix): network.create 등 사용자 입력 CIDR이 사설 대역인지
 * 검증. RFC1918 + RFC6598만 허용. 0.0.0.0/0, 공인 IP 대역, link-local,
 * 멀티캐스트, loopback 모두 거부 — 잘못된 NAT 설정으로 외부 라우팅 / 라우팅
 * 충돌 방지. IPv6는 fc00::/7 (ULA) 만 허용. */
gboolean pcv_validate_private_cidr(const gchar *cidr)
{
    if (!pcv_validate_cidr(cidr)) return FALSE;
    const gchar *slash = g_strrstr(cidr, "/");
    if (!slash) return FALSE;
    gchar *ip_part = g_strndup(cidr, (gsize)(slash - cidr));
    gboolean result = FALSE;
    if (strchr(ip_part, '.')) {
        result = _is_safe_private_ipv4(ip_part);
    } else if (strchr(ip_part, ':')) {
        /* fc00::/7 ULA */
        result = (g_ascii_strncasecmp(ip_part, "fc", 2) == 0 ||
                  g_ascii_strncasecmp(ip_part, "fd", 2) == 0);
    }
    g_free(ip_part);
    return result;
}

gboolean pcv_validate_cidr(const gchar *cidr)
{
    if (!cidr || strlen(cidr) > PCV_MAX_CIDR_LEN) return FALSE;

    /* '/' 분리 — g_strrstr은 마지막 '/'를 찾음 */
    const gchar *slash = g_strrstr(cidr, "/");
    if (!slash || slash == cidr) return FALSE;  /* '/' 없거나 IP 부분 빈 문자열 */

    /* prefix 부분이 순수 숫자인지 확인 (앞뒤 공백, 부호 등 거부) */
    for (const gchar *p = slash + 1; *p; p++)
        if (!g_ascii_isdigit(*p)) return FALSE;
    if (*(slash + 1) == '\0') return FALSE;  /* prefix 부분 빈 문자열 */

    gint prefix = atoi(slash + 1);
    if (prefix < 0 || prefix > 128) return FALSE;

    /* IP 부분 추출 */
    gchar *ip_part = g_strndup(cidr, (gsize)(slash - cidr));

    /* IPv4 vs IPv6 판별: '.' 포함 시 IPv4, ':' 포함 시 IPv6 */
    gboolean result;
    if (strchr(ip_part, '.') != NULL) {
        result = _validate_ipv4_cidr(ip_part, prefix);
    } else if (strchr(ip_part, ':') != NULL) {
        result = _validate_ipv6_cidr(ip_part, prefix);
    } else {
        result = FALSE;  /* '.'도 ':'도 없으면 무효 */
    }

    g_free(ip_part);
    return result;
}

/**
 * pcv_validate_network_create_params - network.create 통합 검증
 * @bridge_name: 생성할 브릿지 이름 (필수, 1~16자 safe token)
 * @mode:        네트워크 모드 (NULL이면 "nat" 기본값 적용)
 *               유효값: "nat", "isolated", "routed", "bridge"
 * @cidr:        IP 대역 (bridge 모드 외 필수, "a.b.c.d/prefix" 형식)
 * @physical_if: 물리 NIC 이름 (bridge 모드 필수, 다른 모드에서는 무시)
 * @error:       실패 시 GError 설정
 *
 * @return: 모든 파라미터 유효 시 TRUE, 하나라도 실패 시 FALSE
 *
 * dispatcher.c에서 network.create 핸들러 진입 전에 호출합니다.
 *
 * [모드별 필수 파라미터]
 *   nat:      bridge_name + cidr (필수)
 *   isolated: bridge_name + cidr (필수)
 *   routed:   bridge_name + cidr (필수)
 *   bridge:   bridge_name + physical_if (필수), cidr 불필요
 *
 * [bridge 모드에서 cidr이 불필요한 이유]
 *   bridge 모드는 물리 NIC를 직접 브릿지에 연결합니다.
 *   VM이 외부 네트워크의 DHCP 서버로부터 IP를 직접 할당받으므로
 *   내부 IP 대역 설정(cidr)이 필요하지 않습니다.
 */
gboolean pcv_validate_network_create_params(const gchar  *bridge_name,
                                            const gchar  *mode,
                                            const gchar  *cidr,
                                            const gchar  *physical_if,
                                            GError      **error)
{
    /* 1. bridge_name: 모든 모드에서 필수 */
    if (!pcv_validate_bridge_name(bridge_name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid bridge_name '%s': 1-%d chars [a-zA-Z0-9_-]",
            bridge_name ? bridge_name : "(null)", PCV_MAX_BRIDGE_NAME);
        return FALSE;
    }

    /* 2. mode 유효성: 허용된 4가지 모드만 통과 (NULL은 "nat"으로 기본 처리) */
    if (mode && g_strcmp0(mode,"nat") != 0 && g_strcmp0(mode,"isolated") != 0
             && g_strcmp0(mode,"routed") != 0 && g_strcmp0(mode,"bridge") != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Invalid mode '%s': must be nat | isolated | routed | bridge", mode);
        return FALSE;
    }

    /* 3. cidr — bridge 모드 외 필수 (bridge 모드는 물리 NIC가 IP를 관리)
     * B4-C2 (Phase 2 fix): pcv_validate_cidr → pcv_validate_private_cidr 전환.
     * 0.0.0.0/0, 공인 IP, link-local, 멀티캐스트 차단 — RFC1918+RFC6598+ULA만 허용. */
    const gchar *eff_mode = mode ? mode : "nat";  /* NULL이면 "nat" 기본값 */
    if (g_strcmp0(eff_mode, "bridge") != 0) {
        if (!cidr || !pcv_validate_private_cidr(cidr)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid or non-private cidr '%s': must be RFC1918 (10/8, 172.16/12, 192.168/16), "
                "RFC6598 (100.64/10), or fc00::/7 — public/link-local/multicast 거부",
                cidr ? cidr : "(null)");
            return FALSE;
        }
    }

    /* 4. physical_if — bridge 모드에서 필수 (슬레이브할 물리 NIC 지정) */
    if (g_strcmp0(eff_mode, "bridge") == 0) {
        if (!physical_if || !pcv_validate_bridge_name(physical_if)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid or missing physical_if '%s' for bridge mode",
                physical_if ? physical_if : "(null)");
            return FALSE;
        }
    }

    return TRUE;
}

/* ── PCI 주소 검증 (SR-IOV / DPDK Phase 4) ──────────── */

/**
 * pcv_validate_pci_addr - PCI BDF 주소 검증 (SR-IOV / DPDK Phase 4)
 * @addr: "DDDD:BB:SS.F" 형식 PCI 주소 문자열
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - 형식: DDDD:BB:SS.F (Domain:Bus:Slot.Function)
 *   - 16진수 범위: D(0~FFFF), B(0~FF), S(0~1F), F(0~7)
 *   - ".." 경로 순회 차단 (sysfs 경로 인젝션 방지)
 *   - sscanf %n으로 전체 문자열 소비 확인 (부분 매칭 방지)
 *
 * [사용처]
 *   - sriov.attach: VF(Virtual Function)의 PCI 주소 → vfio-pci 바인딩
 *   - dpdk.bind: NIC PCI 주소 → DPDK 드라이버 바인딩
 *   - handler_accel.c의 모든 PCI 관련 RPC
 *
 * [보안 의미]
 *   PCI 주소는 /sys/bus/pci/devices/<addr> 경로로 변환됩니다.
 *   ".." 패턴을 허용하면 /sys 내 임의 경로에 접근할 수 있으므로 차단합니다.
 *
 * [sscanf %n 트릭]
 *   %n은 sscanf가 현재까지 소비한 문자 수를 기록합니다.
 *   consumed == len 확인으로 문자열 전체가 PCI 주소 형식인지 보장합니다.
 *   예: "0000:01:00.0extra" → consumed=12, len=17 → FALSE (부분 매칭 방지)
 *
 * [PCI 주소 구조]
 *   Domain(16비트):  0000~FFFF (대부분 시스템에서 0000)
 *   Bus(8비트):      00~FF (PCI 버스 번호)
 *   Slot(5비트):     00~1F (디바이스 슬롯, 32개)
 *   Function(3비트): 0~7 (멀티함수 디바이스의 함수 번호)
 *
 *   예시: "0000:3b:00.0" → Domain=0, Bus=59, Slot=0, Function=0
 */
gboolean pcv_validate_pci_addr(const gchar *addr) {
    if (!addr || *addr == '\0') return FALSE;
    gsize len = strlen(addr);
    if (len > PCV_MAX_PCI_ADDR) return FALSE;

    /* 경로 순회 차단 — sysfs 인젝션 방지 */
    if (strstr(addr, "..")) return FALSE;

    /* 형식: DDDD:BB:SS.F — 정확히 매칭 */
    guint d, b, s, f;
    gint consumed = 0;
    gint n = sscanf(addr, "%x:%x:%x.%x%n", &d, &b, &s, &f, &consumed);
    if (n != 4) return FALSE;                  /* 4개 필드 모두 파싱 성공 필요 */
    if ((gsize)consumed != len) return FALSE;   /* 문자열 전체 소비 확인 */

    /* 각 필드 범위 검증 */
    if (d > 0xFFFF || b > 0xFF || s > 0x1F || f > 0x7) return FALSE;

    return TRUE;
}

/**
 * pcv_network_rundir_init - 네트워크 런타임 디렉터리 생성
 *
 * PCV_NETWORK_RUNDIR (/var/run/purecvisor/network)을 생성합니다.
 * 이미 존재하면 아무것도 하지 않습니다 (멱등성).
 *
 * [사용처]
 *   데몬 시작 시 1회 호출 (main.c 또는 네트워크 모듈 초기화에서).
 *   이 디렉터리에 dnsmasq의 PID/conf/lease/meta 파일이 저장됩니다.
 *
 * [0700 권한]
 *   root만 읽기/쓰기/실행 가능.
 *   dnsmasq PID 파일과 DHCP 임대 정보가 포함되므로 보안을 위해 제한합니다.
 *
 * [g_mkdir_with_parents]
 *   /var/run/purecvisor/ 가 없어도 부모 디렉터리까지 재귀 생성합니다.
 *   mkdir -p 와 동일한 동작입니다.
 */
void pcv_network_rundir_init(void)
{
    /* 부모 포함 재귀 생성 (이미 존재하면 0 반환, 실패 시 -1) */
    if (g_mkdir_with_parents(PCV_NETWORK_RUNDIR, 0700) < 0) {
        g_printerr("[network] Warning: cannot create rundir '%s': %s\n",
                   PCV_NETWORK_RUNDIR, g_strerror(errno));
    }
}

/* ══════════════════════════════════════════════════════
 * 입력 검증 강화 — 추가 검증 함수
 * ══════════════════════════════════════════════════════*/

/**
 * pcv_validate_disk_size_gb - 디스크 크기(GB) 간편 범위 검증
 * @size: 디스크 크기 (GB)
 *
 * @return: TRUE이면 유효 (1 ~ 2048)
 *
 * REST/RPC 핸들러에서 일반 디스크 작업(생성, 리사이즈)의
 * 보수적 상한을 적용할 때 사용합니다.
 */
gboolean pcv_validate_disk_size_gb(gint size) {
    return size >= 1 && size <= 2048;
}

/**
 * pcv_validate_port - TCP/UDP 포트 번호 범위 검증
 * @port: 검증할 포트 번호
 *
 * @return: TRUE이면 유효 (1 ~ 65535)
 *
 * [포트 0 제외 이유]
 *   포트 0은 OS가 임의 포트를 할당하는 와일드카드입니다.
 *   사용자 입력으로 0을 받으면 의도와 다른 포트가 바인딩되므로 거부합니다.
 */
gboolean pcv_validate_port(gint port) {
    return port >= 1 && port <= 65535;
}

/**
 * pcv_validate_zvol_name - ZFS zvol 이름 검증
 * @name: 검증할 zvol 이름 문자열
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - 영숫자로 시작 [a-zA-Z0-9]
 *   - 이후 [a-zA-Z0-9_.-] 허용, 최대 64자
 *   - ".." 시퀀스 차단 (ZFS 경로 순회 방지)
 *
 * [is_safe_token과의 차이]
 *   zvol 이름은 점(.)을 허용하지만, ".."는 차단합니다.
 *   이는 ZFS 데이터셋 경로 구분자를 악용한 순회를 방지합니다.
 */
gboolean pcv_validate_zvol_name(const gchar *name) {
    if (!name || *name == '\0') return FALSE;
    gsize len = strlen(name);
    if (len > 64) return FALSE;

    /* ".." 시퀀스 차단 — ZFS 경로 순회 방지 */
    if (strstr(name, "..") != NULL) return FALSE;

    /* 첫 문자: 영숫자만 허용 */
    if (!g_ascii_isalnum(name[0])) return FALSE;

    /* 나머지 문자: [a-zA-Z0-9_.-] */
    for (gsize i = 1; i < len; i++) {
        gchar c = name[i];
        if (!g_ascii_isalnum(c) && c != '_' && c != '.' && c != '-')
            return FALSE;
    }

    return TRUE;
}

/* ══════════════════════════════════════════════════════
 * 네트워크/방화벽 입력 검증 (whitelist)
 *
 * VLAN 서브인터페이스, MAC 주소, IP 리터럴, IPv6 프리픽스,
 * L4 프로토콜 이름 등 네트워크·방화벽 RPC 파라미터를 검증합니다.
 * 다른 보안 수정(nftables/dnsmasq 규칙 생성 등)이 이 위에 쌓입니다.
 * ══════════════════════════════════════════════════════*/

/**
 * pcv_validate_iface_name - Linux 네트워크 인터페이스 이름 검증
 * @name: 검증할 인터페이스 이름 (예: "eth0", "eth0.100", "br-lan")
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - [a-zA-Z0-9_.-] 문자만 허용
 *   - 길이 1~PCV_MAX_IFACE_NAME (15자, IFNAMSIZ-1)
 *   - 선행 '-' 거부 (option injection 방지)
 *   - NULL/빈 문자열 거부
 *
 * [bridge_name과의 차이]
 *   pcv_validate_bridge_name()은 '.'을 허용하지 않지만, 이 함수는
 *   '.'을 허용하여 VLAN 서브인터페이스(eth0.100)를 수용합니다.
 *   대신 커널 실제 이름 길이 제한(15자)을 적용합니다.
 */
gboolean pcv_validate_iface_name(const gchar *name) {
    if (!name || *name == '\0') return FALSE;

    gsize len = strlen(name);
    if (len > PCV_MAX_IFACE_NAME) return FALSE;

    /* 선행 '-' 거부 — "ip link" 등의 옵션으로 오인되는 것 방지 */
    if (name[0] == '-') return FALSE;

    for (const gchar *p = name; *p; p++) {
        gchar c = *p;
        if (!g_ascii_isalnum(c) && c != '_' && c != '.' && c != '-')
            return FALSE;
    }
    return TRUE;
}

/**
 * pcv_validate_mac - MAC 주소 검증
 * @mac: 검증할 MAC 주소 문자열 (예: "52:54:00:12:34:56")
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - 정확히 17자 ("xx:xx:xx:xx:xx:xx")
 *   - 콜론은 위치 2,5,8,11,14 에만 존재 (인덱스 % 3 == 2)
 *   - 나머지 위치는 16진수 (0-9a-fA-F)
 *   - 그 외 모든 길이/문자/형식 거부
 *
 * [보안 의미]
 *   MAC 주소는 dnsmasq DHCP 정적 매핑, ebtables/nftables 규칙 등에
 *   전달되므로, 공백/개행/추가 토큰이 섞인 입력(예: "... vlan 4095")을
 *   길이 검사(17자 고정)로 원천 차단합니다.
 */
gboolean pcv_validate_mac(const gchar *mac) {
    if (!mac) return FALSE;
    if (strlen(mac) != 17) return FALSE;

    for (gint i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            /* 위치 2,5,8,11,14: 콜론 */
            if (mac[i] != ':') return FALSE;
        } else {
            /* 그 외 위치: 16진수 */
            if (!g_ascii_isxdigit(mac[i])) return FALSE;
        }
    }
    return TRUE;
}

/**
 * pcv_validate_ip_literal - IPv4/IPv6 리터럴 검증
 * @ip: 검증할 IP 문자열 (예: "10.0.0.1", "fd00::1")
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - inet_pton(AF_INET, ...) 또는 inet_pton(AF_INET6, ...) 성공
 *   - CIDR 접미사("/24") 불허 — inet_pton은 '/'를 거부하므로 자동 차단
 *   - NULL/빈 문자열 거부
 *
 * [inet_pton을 쓰는 이유]
 *   IP 리터럴의 정규 표현은 미묘한 경계(옥텟 범위, :: 축약 규칙 등)가
 *   많아 수기 파싱보다 커널/libc의 inet_pton 판정이 안전합니다.
 *   inet_pton은 선행/후행 공백, '/', 부분 매칭을 모두 거부합니다.
 */
gboolean pcv_validate_ip_literal(const gchar *ip) {
    if (!ip || *ip == '\0') return FALSE;

    struct in_addr  a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, ip, &a4) == 1) return TRUE;
    if (inet_pton(AF_INET6, ip, &a6) == 1) return TRUE;
    return FALSE;
}

/**
 * pcv_validate_ipv6_prefix - IPv6 프리픽스("<addr>/<len>") 검증
 * @prefix: 검증할 프리픽스 문자열 (예: "fd00:1::/64")
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   1. 문자열 어디든 개행('\n')/공백(' ')이 있으면 거부
 *   2. 마지막 '/'로 주소부/프리픽스 길이 분리 ('/' 없으면 거부)
 *   3. 프리픽스 길이: 순수 숫자, 0~128
 *   4. 주소부 문자셋: [0-9a-fA-F:] 만 허용 (그 외 문자 거부)
 *   5. 주소부: inet_pton(AF_INET6) 통과
 *
 * [보안 의미 — dnsmasq 설정 인젝션 방어]
 *   이 값은 dnsmasq 설정("dhcp-range=...")에 삽입될 수 있으므로,
 *   개행/공백을 통한 추가 지시어 삽입(예: "fd00::/64\ndhcp-script=...")을
 *   반드시 차단해야 합니다. inet_pton 단독은 문자셋/개행을 걸러주지
 *   못할 수 있으므로, 명시적 문자셋 화이트리스트를 함께 적용합니다.
 */
gboolean pcv_validate_ipv6_prefix(const gchar *prefix) {
    if (!prefix || *prefix == '\0') return FALSE;

    /* (1) 개행/공백 — 어디든 존재하면 거부 (config injection 차단) */
    for (const gchar *p = prefix; *p; p++) {
        if (*p == '\n' || *p == ' ') return FALSE;
    }

    /* (2) 마지막 '/'로 분리 */
    const gchar *slash = g_strrstr(prefix, "/");
    if (!slash || slash == prefix) return FALSE;   /* '/' 없거나 주소부 빈 문자열 */

    /* (3) 프리픽스 길이: 순수 숫자, 0~128 */
    if (*(slash + 1) == '\0') return FALSE;         /* 길이 부분 빈 문자열 */
    for (const gchar *p = slash + 1; *p; p++)
        if (!g_ascii_isdigit(*p)) return FALSE;
    gint plen = atoi(slash + 1);
    if (plen < 0 || plen > 128) return FALSE;

    /* 주소부 추출 */
    gchar *addr = g_strndup(prefix, (gsize)(slash - prefix));

    /* (4) 주소부 문자셋 화이트리스트: [0-9a-fA-F:] 만 */
    gboolean ok = TRUE;
    for (const gchar *p = addr; *p; p++) {
        if (!g_ascii_isxdigit(*p) && *p != ':') { ok = FALSE; break; }
    }

    /* (5) 주소부 inet_pton(AF_INET6) 판정 */
    if (ok) {
        struct in6_addr a6;
        if (inet_pton(AF_INET6, addr, &a6) != 1) ok = FALSE;
    }

    g_free(addr);
    return ok;
}

/**
 * pcv_validate_l4_proto - L4 프로토콜 이름 검증
 * @proto: 검증할 프로토콜 문자열
 *
 * @return: TRUE이면 유효
 *
 * [검증 규칙]
 *   - "tcp", "udp", "icmp" 중 정확히 하나 (소문자, 대소문자 구분)
 *   - 그 외 값과 NULL 거부
 *
 * [대소문자 구분 이유]
 *   nftables/iptables 규칙 생성에 그대로 사용되므로 정규화된 소문자
 *   토큰만 허용합니다. "TCP" 등은 오타/우회 시도로 간주하여 거부합니다.
 */
gboolean pcv_validate_l4_proto(const gchar *proto) {
    if (!proto) return FALSE;
    return g_strcmp0(proto, "tcp") == 0 ||
           g_strcmp0(proto, "udp") == 0 ||
           g_strcmp0(proto, "icmp") == 0;
}

/**
 * pcv_validate_password_complexity - 사용자 생성 비밀번호 강도 정책 (Q-2 / A07)
 * @password: 검증할 평문 비밀번호
 * @reason:   (out, 선택) 실패 사유 문자열 포인터. 정적 문자열을 가리키게 되며
 *            호출자가 해제하지 않는다. 성공 시 건드리지 않는다.
 *
 * @return: 정책 충족 시 TRUE
 *
 * [정책 — 신규 사용자 생성 시에만 적용]
 *   1. 최소 길이 12자
 *   2. 4개 문자군(소문자 / 대문자 / 숫자 / 특수문자) 중 3종 이상 포함
 *
 * 특수문자는 "소문자·대문자·숫자·공백이 아닌 출력 가능 ASCII" 로 넓게 본다
 * (구두점 전반 + 기타 기호). 공백은 문자군으로 세지 않지만 길이에는 포함된다.
 *
 * [적용 범위]
 *   auth.user.create 핸들러(handle_auth_user_create)의 생성 경로에서만 호출한다.
 *   기존 사용자·로그인·비밀번호 변경 경로에는 영향을 주지 않는다(무락아웃).
 *
 * [비개발자용 영향 설명]
 *   운영자가 새 계정을 만들 때 지나치게 단순한 비밀번호(예: "password1")를
 *   저장하지 못하게 막아, 크리덴셜 스터핑/무차별 대입에 대한 1차 방어선을 높인다.
 */
gboolean pcv_validate_password_complexity(const gchar *password,
                                          const gchar **reason) {
    if (!password) {
        if (reason) *reason = "Password is required";
        return FALSE;
    }

    /* 길이 검사 — 바이트 길이 기준(정책 목적상 ASCII 상정, 멀티바이트도 하한만 강화). */
    if (strlen(password) < 12) {
        if (reason)
            *reason = "Password must be at least 12 characters long";
        return FALSE;
    }

    /* 4개 문자군 존재 여부 집계 */
    gboolean has_lower = FALSE, has_upper = FALSE,
             has_digit = FALSE, has_special = FALSE;
    for (const guchar *p = (const guchar *)password; *p; p++) {
        guchar c = *p;
        if (c >= 'a' && c <= 'z')       has_lower = TRUE;
        else if (c >= 'A' && c <= 'Z')  has_upper = TRUE;
        else if (c >= '0' && c <= '9')  has_digit = TRUE;
        else if (c > ' ' && c < 127)    has_special = TRUE;  /* 출력 가능 ASCII 기호 */
    }

    gint classes = (has_lower ? 1 : 0) + (has_upper ? 1 : 0)
                 + (has_digit ? 1 : 0) + (has_special ? 1 : 0);
    if (classes < 3) {
        if (reason)
            *reason = "Password must include at least 3 of 4 character "
                      "classes: lowercase, uppercase, digit, special";
        return FALSE;
    }

    return TRUE;
}
