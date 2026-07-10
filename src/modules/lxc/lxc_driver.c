/**
 * @file lxc_driver.c
 * @brief LXC 컨테이너 드라이버 — 생성/시작/중지/삭제/exec/스냅샷 전체 구현
 *
 * [파일 역할]
 *   PureCVisor의 컨테이너 관리 백엔드. liblxc C API와 lxc-* CLI 도구를 조합하여
 *   LXC 시스템 컨테이너의 전체 생명주기(CRUD)와 런타임 조작(exec, metrics)을 제공합니다.
 *   ZFS 데이터셋 기반 스토리지와 연동하여 컨테이너별 격리된 파일시스템을 사용합니다.
 *
 * [아키텍처 위치]
 *   handler_container.c (container.* RPC 핸들러)
 *     -> pcv_lxc_create_async/start_async/stop_async/...  [이 파일]
 *          -> GTask 워커 스레드에서 liblxc API 또는 GSubprocess 실행
 *   dispatcher.c -> handler_container.c -> 이 파일
 *
 * [주요 흐름 — API별 구현 전략]
 *   create/destroy : GSubprocess 비동기 (lxc-create, lxc-destroy CLI 래핑)
 *     이유: liblxc C API의 lxc-download 템플릿이 내부적으로 프로세스를 spawn하므로
 *           CLI 래핑이 더 안정적이고 에러 핸들링이 명확함
 *   start/stop     : liblxc C API (c->start, c->shutdown, c->stop)
 *     이유: 빠르고 직접적, GTask 워커 스레드에서 동기 호출
 *   list/metrics   : liblxc C API 직접 호출 (동기, 빠름)
 *   exec           : pcv_spawn_sync() 폴백 (lxc-attach CLI)
 *     이유: GSubprocess가 seccomp 상속 환경에서 lxc-attach를 차단하므로
 *           /bin/sh -c 경유 pcv_spawn_sync()로 우회
 *   snapshot       : GSubprocess (zfs snapshot/rollback/destroy/list)
 *     경로: pcvpool/containers/<name>@<snap_name>
 *
 * [핵심 패턴]
 *   - 비동기 패턴: GTask + GAsyncReadyCallback (fire-and-forget 지원)
 *   - liblxc 핸들: lxc_container_new() -> 사용 -> lxc_container_put() (ref count)
 *   - IP 조회 폴백: liblxc get_ips()가 seccomp 환경에서 실패하면
 *                    lxc-info -iH CLI 출력을 파싱하여 IP를 획득
 *   - ZFS 연동: 컨테이너 생성 시 zfs create, 삭제 시 zfs destroy -r
 *
 * [주니어 참고 — liblxc C API 사용법]
 *   liblxc는 C 라이브러리로, 컨테이너를 프로그래밍적으로 관리합니다.
 *   핵심 패턴:
 *     struct lxc_container *c = lxc_container_new("name", "/path");
 *     c->start(c, 0, NULL);    // 컨테이너 시작
 *     c->stop(c);              // 컨테이너 정지
 *     c->shutdown(c, 30);      // 30초 대기 후 정상 종료
 *     c->is_running(c);        // 실행 여부 확인
 *     c->state(c);             // "RUNNING", "STOPPED" 등 상태 문자열
 *     c->get_ips(c, ...);      // IP 주소 조회
 *     lxc_container_put(c);    // 참조 카운트 감소 (해제)
 *
 * [주니어 참고 — 오퍼레이션 잠금 (v1.0)]
 *   GHashTable(g_ctr_locks)을 사용하여 컨테이너 이름별 잠금을 관리합니다.
 *   같은 컨테이너에 stop+delete가 동시에 요청되면 경쟁 조건이 발생합니다.
 *   _lock_container_op()이 FALSE를 반환하면 "이미 작업 중" 에러를 반환합니다.
 *   작업 완료 후 반드시 _unlock_container_op()을 호출해야 합니다.
 *
 * [주니어 참고 — cgroup2 리소스 제한 (v1.0)]
 *   cgroup(Control Groups)은 리눅스 커널의 리소스 제한 메커니즘입니다.
 *   컨테이너 시작 후 _apply_cgroup_limits()로 CPU/메모리 제한을 적용합니다.
 *     cpu.max: "50000 100000" → 100ms 기간 중 50ms만 CPU 사용 가능 (50%)
 *     memory.max: "536870912" → 512MB 메모리 상한 (초과 시 OOM kill)
 *   cgroup 파일 경로: /sys/fs/cgroup/lxc.payload.<이름>/cpu.max
 *
 * [주니어 참고 — CRIU checkpoint/restore (v1.0)]
 *   CRIU(Checkpoint/Restore In Userspace)는 실행 중인 프로세스의 상태를
 *   디스크에 저장(checkpoint)하고 나중에 복원(restore)하는 도구입니다.
 *   컨테이너 라이브 마이그레이션에 사용 — 현재 PureCVisor에서는
 *   pcv_lxc_checkpoint/restore RPC로 노출되어 있습니다.
 *     checkpoint: CRIU dump → 프로세스 메모리/파일/소켓 상태를 파일로 저장
 *     restore:    CRIU restore → 저장된 상태에서 프로세스를 재생성
 *
 * [주니어 참고 — seccomp 프로파일 관리 (v1.0)]
 *   seccomp(Secure Computing Mode)은 프로세스가 호출할 수 있는
 *   시스템콜을 제한하는 커널 보안 기능입니다.
 *   LXC 컨테이너는 기본적으로 seccomp 프로파일이 적용되어 있어
 *   mount, reboot, kexec_load 등 위험한 시스템콜이 차단됩니다.
 *   PureCVisor 데몬 자체의 seccomp과 컨테이너의 seccomp은 별개입니다.
 *   데몬의 seccomp이 liblxc API를 차단하는 문제 → pcv_spawn_sync 폴백으로 해결.
 *
 * [주의사항]
 *   - PCV_LXC_PATH(/var/lib/purecvisor/lxc)가 모든 lxc-* 명령의 -P 경로
 *   - 컨테이너 이름은 pcv_validate로 검증된 후 이 모듈에 전달됨
 *   - seccomp 환경에서 liblxc 일부 API가 실패할 수 있으므로 CLI 폴백 필수
 *   - 빌드 의존성: liblxc-dev (pkg-config: lxc)
 */
/* ========================================================================== */

#include "lxc_driver.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ftw.h>
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"

/* liblxc public C API */
#include <lxc/lxccontainer.h>

#define LXC_LOG_DOM "lxc_driver"

/* ══════════════════════════════════════════════════════════════════════════
 * 컨테이너 오퍼레이션 잠금 — 동시 stop+delete 등 경쟁 방지
 *
 * GHashTable 기반 이름별 잠금. 같은 컨테이너에 대해 stop/delete/clone 등
 * 파괴적 작업이 동시에 실행되는 것을 방지합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

static GHashTable *g_ctr_locks = nullptr;
static GMutex      g_ctr_lock_mu;

/**
 * _lock_container_op - 컨테이너 이름으로 오퍼레이션 잠금 획득
 * @return TRUE 잠금 성공, FALSE 이미 잠긴 상태 (동시 작업 진행 중)
 */
static gboolean
_lock_container_op(const gchar *name)
{
    g_mutex_lock(&g_ctr_lock_mu);
    if (!g_ctr_locks)
        g_ctr_locks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (g_hash_table_contains(g_ctr_locks, name)) {
        g_mutex_unlock(&g_ctr_lock_mu);
        return FALSE;  /* already locked */
    }
    g_hash_table_insert(g_ctr_locks, g_strdup(name), GINT_TO_POINTER(1));
    g_mutex_unlock(&g_ctr_lock_mu);
    return TRUE;
}

/**
 * _unlock_container_op - 컨테이너 오퍼레이션 잠금 해제
 */
static void
_unlock_container_op(const gchar *name)
{
    g_mutex_lock(&g_ctr_lock_mu);
    if (g_ctr_locks) g_hash_table_remove(g_ctr_locks, name);
    g_mutex_unlock(&g_ctr_lock_mu);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Rootless 컨테이너 지원 (C-6)
 *
 * daemon.conf [container] 섹션에서 전역 rootless 설정을 읽고,
 * container.create RPC의 "rootless" 파라미터로 per-container 오버라이드 가능.
 *
 * rootless=true이면 user namespace ID 매핑을 컨테이너 config에 추가하여
 * 컨테이너 내부 root(UID 0)가 호스트의 비특권 UID에 매핑됩니다.
 *
 * 전제조건: /etc/subuid, /etc/subgid에 매핑 범위가 등록되어 있어야 함.
 * 전제조건 미충족 시 privileged 모드로 graceful fallback합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _rootless_check_subid - /etc/subuid 또는 /etc/subgid 파일에서
 * uid_start~uid_start+uid_count 범위를 포함하는 항목이 있는지 확인
 *
 * @path:      "/etc/subuid" 또는 "/etc/subgid"
 * @uid_start: 매핑 시작 ID
 * @uid_count: 매핑 크기
 * @return:    TRUE이면 매핑 가능, FALSE이면 범위 부족
 */
static gboolean
_rootless_check_subid(const gchar *path, gint uid_start, gint uid_count)
{
    gchar *contents = nullptr;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return FALSE;

    gboolean found = FALSE;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        /* format: "user:start:count" or "uid:start:count" */
        gchar **fields = g_strsplit(lines[i], ":", 3);
        if (fields[0] && fields[1] && fields[2]) {
            gint64 start = g_ascii_strtoll(fields[1], NULL, 10);
            gint64 count = g_ascii_strtoll(fields[2], NULL, 10);
            if (start <= uid_start && start + count >= uid_start + uid_count) {
                found = TRUE;
                g_strfreev(fields);
                break;
            }
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(contents);
    return found;
}

/**
 * _rootless_apply_config - 컨테이너에 user namespace ID 매핑 설정 적용
 *
 * lxc.idmap 두 줄(uid/gid) + lxc.init.uid/gid = 0 설정.
 * /etc/subuid, /etc/subgid 검증 실패 시 FALSE 반환 (graceful fallback).
 *
 * @c:         liblxc 컨테이너 핸들
 * @uid_start: 호스트 UID 매핑 시작 (기본 100000)
 * @uid_count: 매핑 크기 (기본 65536)
 * @return:    TRUE 성공, FALSE 실패 (caller는 privileged로 fallback)
 */
static gboolean
_rootless_apply_config(struct lxc_container *c, gint uid_start, gint uid_count)
{
    /* 전제조건 검사: /etc/subuid, /etc/subgid */
    if (!_rootless_check_subid("/etc/subuid", uid_start, uid_count)) {
        PCV_LOG_WARN(LXC_LOG_DOM,
                     "rootless: /etc/subuid missing range %d:%d, falling back to privileged",
                     uid_start, uid_count);
        return FALSE;
    }
    if (!_rootless_check_subid("/etc/subgid", uid_start, uid_count)) {
        PCV_LOG_WARN(LXC_LOG_DOM,
                     "rootless: /etc/subgid missing range %d:%d, falling back to privileged",
                     uid_start, uid_count);
        return FALSE;
    }

    /* user namespace ID 매핑 설정 */
    gchar *uid_map = g_strdup_printf("u 0 %d %d", uid_start, uid_count);
    gchar *gid_map = g_strdup_printf("g 0 %d %d", uid_start, uid_count);

    gboolean ok = TRUE;
    if (!c->set_config_item(c, "lxc.idmap", uid_map)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "rootless: failed to set lxc.idmap (uid)");
        ok = FALSE;
    }
    if (ok && !c->set_config_item(c, "lxc.idmap", gid_map)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "rootless: failed to set lxc.idmap (gid)");
        ok = FALSE;
    }

    if (ok) {
        /* 컨테이너 내부 init은 UID/GID 0으로 실행 (호스트에서는 비특권) */
        c->set_config_item(c, "lxc.init.uid", "0");
        c->set_config_item(c, "lxc.init.gid", "0");
        PCV_LOG_INFO(LXC_LOG_DOM,
                     "rootless: applied idmap u/g 0 %d %d", uid_start, uid_count);
    }

    g_free(uid_map);
    g_free(gid_map);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 내부 유틸리티
 *
 * [주니어 참고] liblxc C API 기본 패턴
 *   liblxc는 참조 카운트(ref count) 방식으로 컨테이너 핸들을 관리합니다.
 *     lxc_container_new()  → 핸들 생성 (ref=1)
 *     lxc_container_put()  → 참조 해제 (ref--, ref==0이면 메모리 해제)
 *   사용 후 반드시 lxc_container_put()을 호출해야 메모리 누수가 없습니다.
 *   GLib의 g_object_unref()와 유사한 개념입니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * liblxc 컨테이너 핸들 획득 (내부 사용) — 호출 후 lxc_container_put() 필수
 *
 * [왜 is_defined 체크가 필요한가?]
 *   lxc_container_new()는 컨테이너가 존재하지 않아도 핸들을 반환합니다.
 *   (메모리만 할당하고 실제 설정 파일 존재 여부는 확인하지 않음)
 *   따라서 is_defined()로 /var/lib/purecvisor/lxc/<name>/config 존재를 확인해야 합니다.
 */
static struct lxc_container *
_lxc_get(const gchar *name, GError **error)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "liblxc: cannot allocate container handle for '%s'", name);
        return NULL;
    }
    if (!c->is_defined(c)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' does not exist in %s", name, PCV_LXC_PATH);
        lxc_container_put(c);
        return NULL;
    }
    return c;
}

/**
 * IP 주소를 liblxc에서 읽어 문자열로 반환 (호출자 g_free())
 *
 * [2단계 폴백 전략]
 *   1차: liblxc get_ips() C API — 가장 빠르고 정확
 *   2차: /proc/<init_pid>/net/fib_trie 파싱 — seccomp 환경에서 get_ips 실패 시 사용
 *
 * [왜 폴백이 필요한가?]
 *   데몬에 seccomp BPF가 적용되면, liblxc의 get_ips()가 내부적으로
 *   네트워크 네임스페이스 진입 시 차단될 수 있습니다.
 *   이 때 커널의 /proc/<pid>/net/fib_trie를 직접 파싱하여 IP를 가져옵니다.
 *   fib_trie는 커널 라우팅 테이블의 텍스트 덤프로, "/32 host LOCAL" 패턴이
 *   해당 네임스페이스에 할당된 로컬 IP를 나타냅니다.
 *
 * [실전 배포 교훈]
 *   최초 배포 시 컨테이너 IP가 모두 "N/A"로 표시 → seccomp 상속 문제 확인
 *   → fib_trie 폴백 추가로 해결
 */
static gchar *
_lxc_get_ip(struct lxc_container *c)
{
    if (!c->is_running(c)) return g_strdup("N/A");

    /* 1차: liblxc C API — 정상 환경에서는 이것으로 충분 */
    char **ips = c->get_ips(c, NULL, "inet", 0);
    if (ips && ips[0]) {
        gchar *result = g_strdup(ips[0]);
        for (int i = 0; ips[i]; i++) free(ips[i]);
        free(ips);
        return result;
    }

    /* 2차: /proc/<init_pid>/net/fib_trie에서 LOCAL 엔트리 파싱
     *   패턴: "  |-- X.X.X.X\n     /32 host LOCAL"
     *   127.x.x.x 와 0.0.0.0 은 제외 */
    pid_t pid = c->init_pid(c);
    if (pid > 0) {
        gchar *fib_path = g_strdup_printf("/proc/%d/net/fib_trie", pid);
        gchar *content = nullptr;
        if (g_file_get_contents(fib_path, &content, NULL, NULL) && content) {
            gchar **lines = g_strsplit(content, "\n", -1);
            for (int i = 0; lines[i] && lines[i + 1]; i++) {
                /* "/32 host LOCAL" 패턴 탐색 */
                gchar *next = g_strstrip(g_strdup(lines[i + 1]));
                if (g_str_has_prefix(next, "/32 host LOCAL")) {
                    /* 이전 줄에서 IP 추출: "  |-- X.X.X.X" */
                    gchar *ip_line = g_strstrip(g_strdup(lines[i]));
                    if (g_str_has_prefix(ip_line, "|-- ")) {
                        const gchar *ip = ip_line + 4;
                        if (!g_str_has_prefix(ip, "127.") && g_strcmp0(ip, "0.0.0.0") != 0) {
                            gchar *result = g_strdup(ip);
                            g_free(ip_line);
                            g_free(next);
                            g_strfreev(lines);
                            g_free(content);
                            g_free(fib_path);
                            return result;
                        }
                    }
                    g_free(ip_line);
                }
                g_free(next);
            }
            g_strfreev(lines);
            g_free(content);
        }
        g_free(fib_path);
    }
    return g_strdup("N/A");
}

/**
 * cgroup 파일에서 uint64 값 읽기 (cgroupv2 + cgroupv1 우선순위 자동 탐색)
 *
 * [왜 두 경로를 모두 시도하는가?]
 *   Ubuntu 22.04는 cgroupv2(unified)가 기본이지만, LXC 컨테이너가 cgroupv1
 *   호환 마운트를 사용하는 경우가 있습니다. 두 경로를 순서대로 시도하여
 *   어느 cgroup 버전이든 메트릭을 수집할 수 있도록 합니다.
 *
 *   cgroupv2 경로: /sys/fs/cgroup/lxc/<name>/<filename>
 *   cgroupv1 경로: /sys/fs/cgroup/memory/lxc/<name>/<filename>
 */
static guint64
_cgroup_read_u64(const gchar *cgroup_path, const gchar *filename)
{
    /* cgroupv2 경로: /sys/fs/cgroup/lxc/<n>/<filename> */
    gchar *path_v2 = g_strdup_printf("/sys/fs/cgroup/lxc/%s/%s",
                                     cgroup_path, filename);
    /* cgroupv1 경로: /sys/fs/cgroup/memory/lxc/<n>/<filename> */
    gchar *path_v1 = g_strdup_printf("/sys/fs/cgroup/memory/lxc/%s/%s",
                                     cgroup_path, filename);

    guint64 value = 0;
    gchar  *content = nullptr;

    if (g_file_get_contents(path_v2, &content, NULL, NULL) ||
        g_file_get_contents(path_v1, &content, NULL, NULL)) {
        value = (guint64)g_ascii_strtoull(g_strstrip(content), NULL, 10);
        g_free(content);
    }
    g_free(path_v2);
    g_free(path_v1);
    return value;
}

/** PcvLxcState → 문자열 */
static const gchar *
_state_to_str(PcvLxcState state)
{
    switch (state) {
        case PCV_LXC_STATE_STOPPED:  return "STOPPED";
        case PCV_LXC_STATE_STARTING: return "STARTING";
        case PCV_LXC_STATE_RUNNING:  return "RUNNING";
        case PCV_LXC_STATE_STOPPING: return "STOPPING";
        case PCV_LXC_STATE_FROZEN:   return "FROZEN";
        default:                     return "UNKNOWN";
    }
}

/** liblxc 상태 문자열 → PcvLxcState */
static PcvLxcState
_str_to_state(const char *s)
{
    if (!s)                        return PCV_LXC_STATE_UNKNOWN;
    if (!strcmp(s, "STOPPED"))     return PCV_LXC_STATE_STOPPED;
    if (!strcmp(s, "STARTING"))    return PCV_LXC_STATE_STARTING;
    if (!strcmp(s, "RUNNING"))     return PCV_LXC_STATE_RUNNING;
    if (!strcmp(s, "STOPPING"))    return PCV_LXC_STATE_STOPPING;
    if (!strcmp(s, "FROZEN"))      return PCV_LXC_STATE_FROZEN;
    return PCV_LXC_STATE_UNKNOWN;
}


/* ══════════════════════════════════════════════════════════════════════════
 * GSubprocess 래퍼 헬퍼 (A-1: 명령어 인젝션 방어)
 *
 * [왜 argv 배열을 직접 전달하는가?]
 *   g_spawn_command_line_sync("lxc-create -n " + user_input) 방식은
 *   user_input에 "; rm -rf /" 같은 셸 메타문자가 포함되면 위험합니다.
 *   argv 배열을 직접 전달하면 shell 해석을 거치지 않으므로
 *   명령어 인젝션 공격을 원천 차단합니다. (Sprint A-1 보안 강화)
 *
 * [주니어 참고]
 *   - _run_argv()      : 명령 실행 + 성공/실패만 반환
 *   - _run_argv_capture(): 명령 실행 + stdout 캡처 반환
 *   두 함수 모두 GSubprocess를 사용하며, 실패 시 stderr를 GError에 담아 반환합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * _run_argv: NULL 종료 argv 배열을 실행하고 성공 여부 반환.
 *            실패 시 *error 에 stderr 메시지 포함.
 */
static gboolean
_run_argv(const gchar * const *argv, GError **error)
{
    GSubprocess *proc = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    if (!proc) return FALSE;

    gchar *stderr_out = nullptr;
    gboolean ok = g_subprocess_communicate_utf8(
        proc, NULL, NULL, NULL, &stderr_out, error);

    if (ok && !g_subprocess_get_successful(proc)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "command failed: %s",
                    (stderr_out && *stderr_out)
                        ? g_strstrip(stderr_out) : "unknown error");
        ok = FALSE;
    }
    g_free(stderr_out);
    g_object_unref(proc);
    return ok;
}

/**
 * _run_argv_capture: stdout 을 캡처하여 반환 (호출자 g_free()).
 *                    실패 시 NULL 반환.
 */
static gchar *
_run_argv_capture(const gchar * const *argv, GError **error)
{
    GSubprocess *proc = g_subprocess_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error);
    if (!proc) return NULL;

    gchar *stdout_out = nullptr;
    gchar *stderr_out = nullptr;
    gboolean ok = g_subprocess_communicate_utf8(
        proc, NULL, NULL, &stdout_out, &stderr_out, error);

    if (ok && !g_subprocess_get_successful(proc)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "command failed: %s",
                    (stderr_out && *stderr_out)
                        ? g_strstrip(stderr_out) : "unknown error");
        g_free(stdout_out);
        stdout_out = nullptr;
    }
    g_free(stderr_out);
    g_object_unref(proc);
    return stdout_out;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 메모리 해제
 * ══════════════════════════════════════════════════════════════════════════*/

void
pcv_lxc_info_free(PcvLxcInfo *info)
{
    if (!info) return;
    g_free(info->name);
    g_free(info->state_str);
    g_free(info->ip_addr);
    g_free(info->image);
    g_free(info);
}

void
pcv_lxc_metrics_free(PcvLxcMetrics *m)
{
    if (!m) return;
    g_free(m->name);
    g_free(m->state_str);
    g_free(m->ip_addr);
    g_free(m);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 동기 조회 API
 * ══════════════════════════════════════════════════════════════════════════*/

gchar *
pcv_lxc_get_state(const gchar *name)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) return g_strdup("UNKNOWN");
    if (!c->is_defined(c)) {
        lxc_container_put(c);
        return g_strdup("UNKNOWN");
    }
    const char *state_str = c->state(c);
    gchar *result = g_strdup(state_str ? state_str : "UNKNOWN");
    lxc_container_put(c);
    return result;
}

/**
 * _ensure_zfs_mounts:
 * 컨테이너 ZFS 데이터셋이 마운트되어 있는지 확인하고, 안 되어 있으면 마운트.
 * 데몬 재시작 시 ZFS가 자동 마운트되지 않는 환경을 위한 안전장치.
 */
static void
_ensure_zfs_mounts(void)
{
    const gchar *mount_argv[] = { "zfs", "mount", "-a", NULL };
    GError *err = nullptr;
    _run_argv(mount_argv, &err);
    if (err) g_error_free(err);
}

GPtrArray *
pcv_lxc_list(GError **error)
{
    /* ZFS 컨테이너 데이터셋 자동 마운트 보장 */
    _ensure_zfs_mounts();

    GPtrArray *result = g_ptr_array_new_with_free_func(
                            (GDestroyNotify)pcv_lxc_info_free);

    /* lxc_list_all_containers: 정의된 + 실행 중인 컨테이너 이름 반환 */
    char **names = nullptr;
    int count = list_all_containers(PCV_LXC_PATH, &names, NULL);
    if (count < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "lxc_list_all_containers failed (path: %s)", PCV_LXC_PATH);
        return result;
    }

    for (int i = 0; i < count; i++) {
        struct lxc_container *c = lxc_container_new(names[i], PCV_LXC_PATH);
        if (!c) { free(names[i]); continue; }

        PcvLxcInfo *info    = g_new0(PcvLxcInfo, 1);
        info->name          = g_strdup(names[i]);
        const char *raw_st  = c->state(c);
        info->state         = _str_to_state(raw_st);
        info->state_str     = g_strdup(_state_to_str(info->state));
        info->ip_addr       = _lxc_get_ip(c);

        /* 이미지 정보: purecvisor.meta 파일에서 읽기 */
        {
            gchar *meta_path = g_strdup_printf("%s/%s/purecvisor.meta",
                                                PCV_LXC_PATH, names[i]);
            gchar *meta_content = nullptr;
            if (g_file_get_contents(meta_path, &meta_content, NULL, NULL) && meta_content) {
                g_strstrip(meta_content);
                info->image = (meta_content[0]) ? meta_content : g_strdup("unknown");
                if (!meta_content[0]) g_free(meta_content);
            } else {
                info->image = g_strdup("unknown");
            }
            g_free(meta_path);
        }

        g_ptr_array_add(result, info);
        lxc_container_put(c);
        free(names[i]);
    }
    if (names) free(names);
    return result;
}

PcvLxcMetrics *
pcv_lxc_get_metrics(const gchar *name, GError **error)
{
    struct lxc_container *c = _lxc_get(name, error);
    if (!c) return NULL;

    PcvLxcMetrics *m = g_new0(PcvLxcMetrics, 1);
    m->name      = g_strdup(name);
    const char *raw_st = c->state(c);
    m->state_str = g_strdup(_state_to_str(_str_to_state(raw_st)));
    m->ip_addr   = _lxc_get_ip(c);
    m->init_pid  = c->init_pid(c);

    /* cgroup 기반 메모리 메트릭 */
    m->mem_used_bytes  = _cgroup_read_u64(name, "memory.current");
    if (m->mem_used_bytes == 0) /* cgroupv1 fallback */
        m->mem_used_bytes = _cgroup_read_u64(name, "memory.usage_in_bytes");

    m->mem_limit_bytes = _cgroup_read_u64(name, "memory.max");
    if (m->mem_limit_bytes == 0)
        m->mem_limit_bytes = _cgroup_read_u64(name, "memory.limit_in_bytes");

    /* cgroup CPU 시간 (nanoseconds) */
    m->cpu_time_ns = _cgroup_read_u64(name, "cpuacct.usage");
    if (m->cpu_time_ns == 0) {
        /* cgroupv2: cpu.stat의 usage_usec → ns 변환 */
        gchar *path = g_strdup_printf("/sys/fs/cgroup/lxc/%s/cpu.stat", name);
        gchar *content = nullptr;
        if (g_file_get_contents(path, &content, NULL, NULL)) {
            gchar **lines = g_strsplit(content, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                if (g_str_has_prefix(lines[i], "usage_usec")) {
                    guint64 usec = g_ascii_strtoull(lines[i] + 11, NULL, 10);
                    m->cpu_time_ns = usec * 1000;
                    break;
                }
            }
            g_strfreev(lines);
            g_free(content);
        }
        g_free(path);
    }

    /* 네트워크 I/O: /proc/<init_pid>/net/dev 파싱 */
    if (m->init_pid > 0) {
        gchar *netdev_path = g_strdup_printf("/proc/%d/net/dev", m->init_pid);
        gchar *content = nullptr;
        if (g_file_get_contents(netdev_path, &content, NULL, NULL)) {
            gchar **lines = g_strsplit(content, "\n", -1);
            for (int i = 2; lines[i]; i++) { /* 첫 2줄 헤더 스킵 */
                gchar *line = g_strstrip(lines[i]);
                /* eth0 또는 veth 로 시작하는 인터페이스 */
                if (g_str_has_prefix(line, "eth") ||
                    g_str_has_prefix(line, "veth")) {
                    guint64 rx, tx;
                    /* 형식: iface: rx_bytes ... tx_bytes ... */
                    gchar **parts = g_strsplit(line, ":", 2);
                    if (parts[1]) {
                        if (sscanf(g_strstrip(parts[1]),
                                   "%" G_GUINT64_FORMAT
                                   " %*u %*u %*u %*u %*u %*u %*u"
                                   " %" G_GUINT64_FORMAT,
                                   &rx, &tx) == 2) {
                            m->net_rx_bytes += rx;
                            m->net_tx_bytes += tx;
                        }
                    }
                    g_strfreev(parts);
                }
            }
            g_strfreev(lines);
            g_free(content);
        }
        g_free(netdev_path);
    }

    lxc_container_put(c);
    return m;
}

/* ══════════════════════════════════════════════════════════════════════════
 * CREATE (비동기 — GSubprocess: ZFS create → lxc-create → config 적용)
 *
 * [컨테이너 생성 4단계]
 *   1. 이미지 파싱: "ubuntu:22.04" → distro="ubuntu", release="22.04"
 *   2. ZFS 데이터셋 생성: pcvpool/containers/<name> (rootfs 마운트포인트)
 *   3. lxc-create: download 템플릿으로 이미지 다운로드 (인터넷 필요)
 *   4. liblxc C API로 config 적용 (메모리/CPU/네트워크 제한)
 *
 * [왜 GTask 워커 스레드에서 실행하는가?]
 *   lxc-create의 download 템플릿이 인터넷에서 이미지를 받으므로
 *   수 분이 걸릴 수 있습니다. GMainLoop을 블로킹하면 데몬 전체가
 *   응답 불가가 되므로 반드시 워커 스레드에서 실행해야 합니다.
 *
 * [실패 시 롤백]
 *   lxc-create 실패 시 이미 생성된 ZFS 데이터셋을 zfs destroy -r로 정리합니다.
 *   부분 생성 상태가 남지 않도록 원자성을 보장합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    gchar *name;
    gchar *image;       /* "ubuntu:22.04" */
    guint  memory_mb;
    guint  vcpu_count;
    gchar *bridge;
    gint   rootless;    /* -1=global default, 0=force off, 1=force on */
} LxcCreateData;

static void
_lxc_create_data_free(LxcCreateData *d)
{
    g_free(d->name);
    g_free(d->image);
    g_free(d->bridge);
    g_free(d);
}

static void
_lxc_create_thread(GTask        *task,
                   gpointer      source_object __attribute__((unused)),
                   gpointer      task_data,
                   GCancellable *cancellable __attribute__((unused)))
{
    LxcCreateData *d = (LxcCreateData *)task_data;
    GError *error    = nullptr;

    /* ── 1. 이미지 파싱 "ubuntu:22.04" → distro="ubuntu", release="jammy" ── */
    gchar **parts   = g_strsplit(d->image, ":", 2);
    const gchar *distro  = parts[0] ? parts[0] : "ubuntu";
    const gchar *release = (parts[1] && parts[1][0]) ? parts[1] : "jammy";

    /* Ubuntu 버전 번호 → 코드네임 변환 (lxc-create download 템플릿 호환) */
    if (g_strcmp0(distro, "ubuntu") == 0) {
        static const struct { const char *ver; const char *code; } ubuntu_map[] = {
            {"20.04", "focal"}, {"22.04", "jammy"}, {"24.04", "noble"},
            {"24.10", "plucky"}, {"25.04", "questing"}, {NULL, NULL}
        };
        for (int i = 0; ubuntu_map[i].ver; i++) {
            if (g_strcmp0(release, ubuntu_map[i].ver) == 0) {
                release = ubuntu_map[i].code;
                break;
            }
        }
    }

    /* ── 2. ZFS 데이터셋 생성 (mountpoint는 컨테이너 디렉터리, rootfs 아님) ── */
    gchar *zfs_dataset  = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, d->name);
    gchar *mountpoint   = g_strdup_printf("%s/%s", PCV_LXC_PATH, d->name);
    {
        const gchar *zfs_argv[] = {
            "zfs", "create", "-o",
            g_strdup_printf("mountpoint=%s", mountpoint),
            zfs_dataset, NULL
        };
        if (!_run_argv(zfs_argv, &error)) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "ZFS create failed for '%s': %s",
                                    d->name,
                                    error ? error->message : "unknown");
            g_error_free(error);
            g_free((gpointer)zfs_argv[3]);
            g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
            return;
        }
        g_free((gpointer)zfs_argv[3]);

        /* ZFS 명시적 마운트 (g_subprocess에서 자동 마운트가 안 될 수 있음) */
        {
            const gchar *mount_argv[] = { "zfs", "mount", zfs_dataset, NULL };
            GError *mnt_err = nullptr;
            _run_argv(mount_argv, &mnt_err);
            if (mnt_err) g_error_free(mnt_err); /* 이미 마운트된 경우 무시 */
        }
    }

    /* ── 3. lxc-create (download template) ── */
    /*
     * lxc-create -P <path> -n <name> -t download \
     *            -- -d <distro> -r <release> -a amd64
     *
     * download template이 인터넷에서 이미지를 가져옴
     * 느릴 수 있으나 GTask 워커에서 실행하므로 main loop 블로킹 없음
     */
    error = nullptr;
    {
        /* lxc-create: argv 직접 전달 → shell 해석 없음 (A-1) */
        const gchar *lxc_argv[] = {
            "lxc-create", "-P", PCV_LXC_PATH,
            "-n", d->name,
            "-t", "download",
            "--", "-d", distro, "-r", release, "-a", "amd64",
            NULL
        };
        if (!_run_argv(lxc_argv, &error)) {
            PCV_LOG_WARN("lxc", "Container '%s' lxc-create FAILED (distro=%s release=%s): %s",
                         d->name, distro, release,
                         error ? error->message : "(no error)");
            /* 롤백: ZFS 데이터셋 삭제 */
            const gchar *rb_argv[] = {
                "zfs", "destroy", "-r", zfs_dataset, NULL
            };
            GError *rb_err = nullptr;
            _run_argv(rb_argv, &rb_err);
            if (rb_err) g_error_free(rb_err);

            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "lxc-create failed for '%s': %s",
                                    d->name,
                                    error ? error->message : "unknown");
            g_error_free(error);
            g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
            return;
        }
    }

    /* ── 4. liblxc C API로 config 적용 ── */
    struct lxc_container *c = lxc_container_new(d->name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Cannot open container '%s' after create", d->name);
        if (c) lxc_container_put(c);
        goto cleanup;
    }

    /* 4-a. 메모리 제한 (cgroup memory.limit) */
    guint memory_mb = (d->memory_mb > 0) ? d->memory_mb : 512;
    gchar *mem_val  = g_strdup_printf("%uM", memory_mb);
    c->set_config_item(c, "lxc.cgroup.memory.limit_in_bytes", mem_val);
    c->set_config_item(c, "lxc.cgroup2.memory.max",           mem_val);
    g_free(mem_val);

    /* 4-b. CPU 제한 (cpu.shares 기반) */
    guint vcpu = (d->vcpu_count > 0) ? d->vcpu_count : 1;
    gchar *cpu_shares = g_strdup_printf("%u", vcpu * 1024);
    c->set_config_item(c, "lxc.cgroup.cpu.shares", cpu_shares);
    g_free(cpu_shares);

    /* 4-c. 네트워크 (veth + bridge) */
    c->clear_config_item(c, "lxc.net");             /* 기존 net 설정 초기화 */
    c->set_config_item(c, "lxc.net.0.type",    "veth");
    c->set_config_item(c, "lxc.net.0.link",    d->bridge ? d->bridge : PCV_LXC_DEFAULT_BRIDGE);
    c->set_config_item(c, "lxc.net.0.flags",   "up");
    c->set_config_item(c, "lxc.net.0.hwaddr",  "00:16:3e:xx:xx:xx"); /* random MAC */

    /* 4-d. Rootless 컨테이너 설정 (C-6) */
    {
        gboolean want_rootless;
        if (d->rootless >= 0) {
            want_rootless = (d->rootless == 1);  /* per-container override */
        } else {
            /* global default from daemon.conf [container] rootless */
            const gchar *cfg = pcv_config_get_string("container", "rootless", "false");
            want_rootless = (g_ascii_strcasecmp(cfg, "true") == 0 ||
                             g_ascii_strcasecmp(cfg, "1") == 0 ||
                             g_ascii_strcasecmp(cfg, "yes") == 0);
        }
        if (want_rootless) {
            gint uid_start = pcv_config_get_int("container", "rootless_uid_start", 100000);
            gint uid_count = pcv_config_get_int("container", "rootless_uid_count", 65536);
            if (!_rootless_apply_config(c, uid_start, uid_count)) {
                PCV_LOG_WARN(LXC_LOG_DOM,
                             "rootless setup failed for '%s', continuing as privileged",
                             d->name);
            }
        }
    }

    /* 4-e. 이미지 정보 기록 — purecvisor.meta 파일에 저장 */
    {
        gchar *image_tag = g_strdup_printf("%s:%s", distro, release);
        gchar *meta_path = g_strdup_printf("%s/%s/purecvisor.meta",
                                            PCV_LXC_PATH, d->name);
        g_file_set_contents(meta_path, image_tag, -1, NULL);
        g_free(meta_path);
        g_free(image_tag);
    }

    /* config 저장 */
    if (!c->save_config(c, NULL)) {
        g_warning("lxc_driver: save_config failed for '%s' (non-fatal)", d->name);
    }

    lxc_container_put(c);
    g_task_return_boolean(task, TRUE);

cleanup:
    g_strfreev(parts);
    g_free(mountpoint);
    g_free(zfs_dataset);
}

void
pcv_lxc_create_async(const gchar        *name,
                     const gchar        *image,
                     guint               memory_mb,
                     guint               vcpu_count,
                     const gchar        *network_bridge,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    pcv_lxc_create_async_full(name, image, memory_mb, vcpu_count,
                              network_bridge, -1,
                              cancellable, callback, user_data);
}

void
pcv_lxc_create_async_full(const gchar        *name,
                          const gchar        *image,
                          guint               memory_mb,
                          guint               vcpu_count,
                          const gchar        *network_bridge,
                          gint                rootless,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    GTask *task         = g_task_new(NULL, cancellable, callback, user_data);
    LxcCreateData *data = g_new0(LxcCreateData, 1);
    data->name          = g_strdup(name);
    data->image         = g_strdup(image ? image : "ubuntu:22.04");
    data->memory_mb     = memory_mb;
    data->vcpu_count    = vcpu_count;
    data->bridge        = g_strdup(network_bridge ? network_bridge
                                                   : PCV_LXC_DEFAULT_BRIDGE);
    data->rootless      = rootless;  /* -1=global, 0=off, 1=on */

    g_task_set_task_data(task, data, (GDestroyNotify)_lxc_create_data_free);
    g_task_run_in_thread(task, _lxc_create_thread);
    g_object_unref(task);
}

gboolean
pcv_lxc_create_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

/* ══════════════════════════════════════════════════════════════════════════
 * DESTROY (비동기 — 강제 중지 → lxc-destroy → ZFS destroy)
 *
 * [삭제 3단계]
 *   1. 실행 중이면 liblxc c->stop()으로 강제 중지
 *   2. lxc-destroy -f : 컨테이너 설정 파일/rootfs 삭제
 *   3. zfs destroy -r : ZFS 데이터셋 + 스냅샷 전부 삭제
 *
 * [ZFS 실패 시 경고만 출력하는 이유]
 *   lxc-destroy가 이미 성공하여 컨테이너는 논리적으로 삭제된 상태입니다.
 *   ZFS 데이터셋 삭제가 실패해도 (busy snapshot 등) 사용자에게는
 *   성공을 반환하고 관리자가 수동으로 정리하도록 합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

static int
_pcv_recursive_unlink_cb(const char *fpath,
                         const struct stat *sb __attribute__((unused)),
                         int typeflag,
                         struct FTW *ftwbuf __attribute__((unused)))
{
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        return rmdir(fpath);
    }
    return unlink(fpath);
}

static void
_lxc_destroy_thread(GTask        *task,
                    gpointer      source_object __attribute__((unused)),
                    gpointer      task_data,
                    GCancellable *cancellable __attribute__((unused)))
{
    const gchar *name = (const gchar *)task_data;
    GError      *error = nullptr;

    /* 동시 작업 방지 잠금 */
    if (!_lock_container_op(name)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Container '%s' has a concurrent operation in progress", name);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "Container '%s' has a concurrent operation in progress", name);
        return;
    }

    /* 1. 실행 중이면 강제 중지 */
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (c && c->is_defined(c) && c->is_running(c)) {
        if (!c->stop(c)) {
            g_warning("lxc_driver: stop failed for '%s', continuing destroy", name);
        }
    }
    if (c) lxc_container_put(c);

    /* 2. lxc-destroy -f (강제) — argv 직접 전달 (A-1) */
    {
        gboolean lxc_destroy_ok = FALSE;
        const gchar *argv[] = {
            "lxc-destroy", "-P", PCV_LXC_PATH, "-n", name, "-f", NULL
        };
        if (_run_argv(argv, &error)) {
            lxc_destroy_ok = TRUE;
        } else {
            gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
            gboolean config_gone = !g_file_test(config_path, G_FILE_TEST_EXISTS);
            g_free(config_path);

            if (!config_gone) {
                _unlock_container_op(name);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "lxc-destroy failed for '%s': %s",
                                        name,
                                        error ? error->message : "unknown");
                g_error_free(error);
                return;
            }

            PCV_LOG_WARN(LXC_LOG_DOM,
                         "lxc-destroy reported failure for '%s' but config is already gone; continuing with ZFS cleanup",
                         name);
            if (error) {
                g_error_free(error);
                error = NULL;
            }
        }

        if (!lxc_destroy_ok) {
            PCV_LOG_INFO(LXC_LOG_DOM, "Container '%s' entering destroy cleanup fallback path", name);
        }
    }

    /* 3. ZFS 데이터셋 삭제 — argv 직접 전달 (A-1) */
    {
        gchar *zfs_target = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
        const gchar *argv[] = { "zfs", "destroy", "-r", zfs_target, NULL };
        GError *zfs_err = nullptr;
        if (!_run_argv(argv, &zfs_err)) {
            /* ZFS 실패는 경고만 (컨테이너는 이미 삭제됨) */
            g_warning("lxc_driver: ZFS destroy failed for '%s': %s",
                      name, zfs_err ? zfs_err->message : "unknown");
            if (zfs_err) g_error_free(zfs_err);
        }
        g_free(zfs_target);
    }

    {
        gchar *container_dir = g_strdup_printf("%s/%s", PCV_LXC_PATH, name);
        if (g_file_test(container_dir, G_FILE_TEST_IS_DIR) && g_rmdir(container_dir) != 0) {
            PCV_LOG_WARN(LXC_LOG_DOM,
                         "Post-destroy rmdir failed for '%s': %s",
                         container_dir, g_strerror(errno));
        }
        g_free(container_dir);
    }

    {
        gchar *container_dir = g_strdup_printf("%s/%s", PCV_LXC_PATH, name);
        if (g_file_test(container_dir, G_FILE_TEST_EXISTS)) {
            nftw(container_dir, _pcv_recursive_unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
        }
        g_free(container_dir);
    }

    _unlock_container_op(name);
    g_task_return_boolean(task, TRUE);
}

void
pcv_lxc_destroy_async(const gchar        *name,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, g_strdup(name), (GDestroyNotify)g_free);
    g_task_run_in_thread(task, _lxc_destroy_thread);
    g_object_unref(task);
}

gboolean
pcv_lxc_destroy_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

/* ══════════════════════════════════════════════════════════════════════════
 * CLONE (비동기 — lxc-copy + ZFS backend)
 *
 * [구현]
 *   lxc-copy -n <source> -N <target> -B zfs -P <path>
 *   ZFS 기반 Copy-on-Write 클론으로 빠르게 복제합니다.
 *
 * [잠금]
 *   소스/타겟 모두 동시 작업 방지 잠금을 획득합니다.
 *   소스에 대한 stop/delete와 clone이 동시에 실행되는 것을 방지합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct {
    gchar *source;
    gchar *target;
} CloneCtx;

static void _clone_ctx_free(gpointer p) {
    CloneCtx *ctx = p;
    g_free(ctx->source);
    g_free(ctx->target);
    g_free(ctx);
}

static void
_clone_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    CloneCtx *ctx = data;

    /* 소스/타겟 모두 잠금 획득 */
    if (!_lock_container_op(ctx->source)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Clone: source '%s' has a concurrent operation in progress",
                     ctx->source);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "source container has a concurrent operation in progress");
        return;
    }
    if (!_lock_container_op(ctx->target)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Clone: target '%s' has a concurrent operation in progress",
                     ctx->target);
        _unlock_container_op(ctx->source);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "target container has a concurrent operation in progress");
        return;
    }

    const gchar *argv[] = {
        "lxc-copy", "-P", PCV_LXC_PATH,
        "-n", ctx->source, "-N", ctx->target, "-B", "zfs", NULL
    };
    gchar *std_out = NULL, *std_err = nullptr;
    GError *err = nullptr;

    if (pcv_spawn_sync(argv, &std_out, &std_err, &err)) {
        PCV_LOG_INFO(LXC_LOG_DOM, "Cloned container '%s' -> '%s'",
                     ctx->source, ctx->target);
        g_task_return_boolean(task, TRUE);
    } else {
        PCV_LOG_WARN(LXC_LOG_DOM, "Clone failed '%s' -> '%s': %s",
                     ctx->source, ctx->target,
                     err ? err->message : (std_err ? std_err : "unknown"));
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "%s", err ? err->message
                                          : (std_err ? std_err : "unknown"));
        if (err) g_error_free(err);
    }
    g_free(std_out);
    g_free(std_err);

    _unlock_container_op(ctx->target);
    _unlock_container_op(ctx->source);
}

void
pcv_lxc_clone_async(const gchar *source, const gchar *target,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    if (!source || !target || !*source || !*target) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "source and target are required");
        g_object_unref(task);
        return;
    }

    CloneCtx *ctx = g_new0(CloneCtx, 1);
    ctx->source = g_strdup(source);
    ctx->target = g_strdup(target);
    g_task_set_task_data(task, ctx, _clone_ctx_free);
    g_task_run_in_thread(task, _clone_worker);
    g_object_unref(task);
}

gboolean
pcv_lxc_clone_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean
pcv_lxc_clone(const gchar *source, const gchar *target)
{
    if (!source || !target || !*source || !*target) return FALSE;
    pcv_lxc_clone_async(source, target, NULL, NULL, NULL);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * cgroup2 리소스 제한 — 컨테이너 시작 후 즉시 적용
 *
 * cpu.max: 100ms 기간 기준 CPU 쿼터 (e.g., 50% → 50000 100000)
 * memory.max: 절대 메모리 상한 (바이트 단위)
 *
 * cgroup 파일이 존재하지 않으면 (cgroup v1 환경 등) 조용히 무시합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

static void
_apply_cgroup_limits(const gchar *name, gint cpu_percent, gint64 memory_mb)
{
    if (cpu_percent > 0 && cpu_percent <= 100) {
        /* CPU 제한: cpu.max = quota period (100ms 기준) */
        gint quota = cpu_percent * 1000;  /* e.g., 50% → 50000us of 100000us */
        gchar *cpu_val  = g_strdup_printf("%d 100000", quota);
        gchar *cpu_path = g_strdup_printf("/sys/fs/cgroup/lxc.payload.%s/cpu.max", name);

        if (g_file_test(cpu_path, G_FILE_TEST_EXISTS)) {
            g_file_set_contents(cpu_path, cpu_val, -1, NULL);
            PCV_LOG_INFO(LXC_LOG_DOM, "Set CPU limit for '%s': %d%%", name, cpu_percent);
        }
        g_free(cpu_val);
        g_free(cpu_path);
    }

    if (memory_mb > 0) {
        gchar *mem_val  = g_strdup_printf("%" G_GINT64_FORMAT, memory_mb * 1024 * 1024);
        gchar *mem_path = g_strdup_printf("/sys/fs/cgroup/lxc.payload.%s/memory.max", name);

        if (g_file_test(mem_path, G_FILE_TEST_EXISTS)) {
            g_file_set_contents(mem_path, mem_val, -1, NULL);
            PCV_LOG_INFO(LXC_LOG_DOM, "Set memory limit for '%s': %" G_GINT64_FORMAT "MB",
                         name, memory_mb);
        }
        g_free(mem_val);
        g_free(mem_path);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * START (비동기 — GTask → pcv_spawn_sync로 lxc-start 실행)
 *
 * [왜 liblxc c->start() 대신 pcv_spawn_sync를 사용하는가?]
 *   원래는 liblxc C API의 c->start()를 직접 호출했으나,
 *   데몬에 seccomp BPF가 적용된 환경에서 liblxc 내부의 clone/mount 등
 *   시스템콜이 EPERM으로 차단되는 문제가 발생했습니다.
 *
 *   pcv_spawn_sync()로 lxc-start CLI를 외부 프로세스로 실행하면,
 *   lxc-start가 자체적으로 AppArmor 프로필을 전환하고 필요한 권한을
 *   획득하므로 seccomp 상속 문제를 우회할 수 있습니다.
 *
 * [알려진 이슈]
 *   PR_SET_NO_NEW_PRIVS=1이 설정되면 lxc-start의 AppArmor 전환이
 *   차단됩니다. 그래서 pcv_privdrop.c에서 NNP를 비활성화했습니다.
 *
 * [멱등성]
 *   이미 실행 중인 컨테이너에 start를 호출하면 TRUE를 즉시 반환합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

static void
_lxc_start_thread(GTask        *task,
                  gpointer      source_object __attribute__((unused)),
                  gpointer      task_data,
                  GCancellable *cancellable __attribute__((unused)))
{
    const gchar *name = (const gchar *)task_data;
    GError      *error = nullptr;

    struct lxc_container *c = _lxc_get(name, &error);
    if (!c) { g_task_return_error(task, error); return; }

    if (c->is_running(c)) {
        g_message("lxc_driver: container '%s' is already running", name);
        lxc_container_put(c);
        g_task_return_boolean(task, TRUE);
        return;
    }
    lxc_container_put(c);

    /* ZFS 마운트 보장 (dataset이 언마운트된 상태일 수 있음) */
    {
        gchar *zfs_ds = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
        const gchar *mount_argv[] = { "zfs", "mount", zfs_ds, NULL };
        GError *mnt_err = nullptr;
        _run_argv(mount_argv, &mnt_err);
        if (mnt_err) g_error_free(mnt_err); /* 이미 마운트된 경우 무시 */
        g_free(zfs_ds);
    }

    /* lxc-start를 pcv_spawn_sync로 실행 (GSubprocessLauncher 경유) */
    const gchar *start_argv[] = {
        "lxc-start", "-P", PCV_LXC_PATH, "-n", name, "-d", NULL
    };
    gchar *start_out = nullptr;
    gchar *start_err = nullptr;
    if (!pcv_spawn_sync(start_argv, &start_out, &start_err, &error)) {
        /* 실패 시 상세 에러 메시지 포함 (기존: stderr 무시로 원인 파악 불가) */
        const gchar *detail = (start_err && *start_err) ? g_strstrip(start_err)
                            : (start_out && *start_out) ? g_strstrip(start_out)
                            : (error ? error->message : "unknown");
        GError *rich = nullptr;
        g_set_error(&rich, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "lxc-start failed for '%s': %s", name, detail);
        if (error) g_error_free(error);
        g_task_return_error(task, rich);
    } else {
        /* 시작 성공 후 cgroup2 리소스 제한 적용 (LXC config에서 읽기) */
        struct lxc_container *cc = lxc_container_new(name, PCV_LXC_PATH);
        if (cc && cc->is_defined(cc)) {
            /* cgroup2.memory.max에서 메모리 제한 읽기 (e.g., "512M") */
            char mem_buf[64] = {0};
            gint64 mem_mb = 0;
            if (cc->get_config_item(cc, "lxc.cgroup2.memory.max", mem_buf, sizeof(mem_buf)) > 0) {
                gint64 val = g_ascii_strtoll(mem_buf, NULL, 10);
                if (val > 0) mem_mb = val;  /* "512M" → 512 (M suffix는 strtoll이 무시) */
            }
            /* cpu.shares에서 CPU 비율 추정 (1024 shares = 1 core ≈ 100%) */
            char cpu_buf[64] = {0};
            gint cpu_pct = 0;
            if (cc->get_config_item(cc, "lxc.cgroup.cpu.shares", cpu_buf, sizeof(cpu_buf)) > 0) {
                gint shares = (gint)g_ascii_strtoll(cpu_buf, NULL, 10);
                if (shares > 0) cpu_pct = (shares * 100) / 1024;
                if (cpu_pct > 100) cpu_pct = 100;
            }
            lxc_container_put(cc);
            if (cpu_pct > 0 || mem_mb > 0)
                _apply_cgroup_limits(name, cpu_pct, mem_mb);
        } else {
            if (cc) lxc_container_put(cc);
        }
        g_task_return_boolean(task, TRUE);
    }
    g_free(start_out);
    g_free(start_err);
}

void
pcv_lxc_start_async(const gchar        *name,
                    GCancellable       *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, g_strdup(name), (GDestroyNotify)g_free);
    g_task_run_in_thread(task, _lxc_start_thread);
    g_object_unref(task);
}

gboolean pcv_lxc_start_finish(GAsyncResult *result, GError **error)
{ return g_task_propagate_boolean(G_TASK(result), error); }

/* ══════════════════════════════════════════════════════════════════════════
 * STOP (비동기 — Graceful shutdown → 30초 타임아웃 → force stop)
 *
 * [정상 종료 vs 강제 종료]
 *   force=FALSE: c->shutdown(c, 30) → 컨테이너 init에 SIGPWR 전송 → 30초 대기
 *                타임아웃 시 c->stop(c)로 강제 종료 (SIGKILL)
 *   force=TRUE:  즉시 c->stop(c) 호출 (SIGKILL 전송, 데이터 손실 가능)
 *
 * [멱등성]
 *   이미 중지된 컨테이너에 stop을 호출하면 TRUE를 즉시 반환합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * LxcStopData — STOP 작업에 필요한 컨텍스트
 *
 * [_lxc_stop_data_free가 gpointer를 받는 이유 (스택 트램폴린 수정)]
 *   g_task_set_task_data()의 GDestroyNotify 시그니처는 void(*)(gpointer)입니다.
 *   LxcStopData*를 직접 받는 함수로 캐스팅하면 일부 ABI에서 스택 정렬 문제가
 *   발생할 수 있습니다. gpointer(void*)로 받아서 내부에서 캐스팅하는 것이
 *   GLib 콜백 패턴의 표준 방식입니다.
 */
typedef struct { gchar *name; gboolean force; } LxcStopData;
static void _lxc_stop_data_free(gpointer p) { LxcStopData *d = p; g_free(d->name); g_free(d); }

static void
_lxc_stop_thread(GTask        *task,
                 gpointer      source_object __attribute__((unused)),
                 gpointer      task_data,
                 GCancellable *cancellable __attribute__((unused)))
{
    LxcStopData *d = (LxcStopData *)task_data;
    GError      *error = nullptr;

    /* 동시 작업 방지 잠금 */
    if (!_lock_container_op(d->name)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Container '%s' has a concurrent operation in progress", d->name);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "Container '%s' has a concurrent operation in progress", d->name);
        return;
    }

    struct lxc_container *c = _lxc_get(d->name, &error);
    if (!c) { _unlock_container_op(d->name); g_task_return_error(task, error); return; }

    if (!c->is_running(c)) {
        g_message("lxc_driver: container '%s' is already stopped", d->name);
        lxc_container_put(c);
        _unlock_container_op(d->name);
        g_task_return_boolean(task, TRUE);
        return;
    }

    gboolean ok;
    if (d->force) {
        /* 즉시 강제 종료 */
        ok = c->stop(c);
    } else {
        /* SIGPWR 시그널로 정상 종료 → 30초 대기 → 강제 종료 */
        ok = c->shutdown(c, 30);
        if (!ok) {
            g_warning("lxc_driver: graceful shutdown timed out for '%s', forcing stop",
                      d->name);
            ok = c->stop(c);
        }
    }

    if (!ok) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "liblxc: stop failed for '%s': %s",
                    d->name, c->error_string);
        g_task_return_error(task, error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    lxc_container_put(c);
    _unlock_container_op(d->name);
}

void
pcv_lxc_stop_async(const gchar        *name,
                   gboolean            force,
                   GCancellable       *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    GTask *task        = g_task_new(NULL, cancellable, callback, user_data);
    LxcStopData *data  = g_new0(LxcStopData, 1);
    data->name         = g_strdup(name);
    data->force        = force;
    g_task_set_task_data(task, data, _lxc_stop_data_free);
    g_task_run_in_thread(task, _lxc_stop_thread);
    g_object_unref(task);
}

gboolean pcv_lxc_stop_finish(GAsyncResult *result, GError **error)
{ return g_task_propagate_boolean(G_TASK(result), error); }

/* ══════════════════════════════════════════════════════════════════════════
 * EXEC (비동기 — pcv_spawn_sync: lxc-attach)
 *
 * [왜 GSubprocess 대신 pcv_spawn_sync를 사용하는가?]
 *   GSubprocess는 부모 프로세스의 seccomp BPF 필터를 자식에게 상속합니다.
 *   lxc-attach는 컨테이너 네임스페이스에 진입하기 위해 setns/clone 등
 *   특수 시스템콜이 필요한데, 데몬의 seccomp 필터가 이를 차단합니다.
 *
 *   pcv_spawn_sync()는 /bin/sh -c 경유로 실행하여 seccomp 상속 문제를
 *   우회합니다. (실전 배포 중 발견된 컨테이너 exec 실패 버그의 해결책)
 *
 * [보안 고려]
 *   lxc-attach에 --clear-env 옵션을 전달하여 호스트 환경변수가
 *   컨테이너 내부로 유출되지 않도록 합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/* GSourceFunc 시그니처 래퍼: g_timeout_add_seconds 콜백으로 사용 (캐스트 경고 방지) */
static gboolean
_cancel_on_timeout(gpointer user_data)
{
    g_cancellable_cancel(G_CANCELLABLE(user_data));
    return G_SOURCE_REMOVE;
}

typedef struct { gchar *name; gchar **argv; } LxcExecData;

static void
_lxc_exec_data_free(LxcExecData *d)
{ g_free(d->name); g_strfreev(d->argv); g_free(d); }

static void
_lxc_exec_thread(GTask        *task,
                 gpointer      source_object __attribute__((unused)),
                 gpointer      task_data,
                 GCancellable *cancellable __attribute__((unused)))
{
    LxcExecData *d = (LxcExecData *)task_data;
    GError *error  = nullptr;

    /* lxc-attach를 pcv_spawn_sync로 실행 (seccomp/NNP 우회) */
    gchar *q_name = g_shell_quote(d->name);
    GString *cmd = g_string_new("lxc-attach --clear-env");
    g_string_append_printf(cmd, " -P %s -n %s --", PCV_LXC_PATH, q_name);
    g_free(q_name);
    for (int i = 0; d->argv[i]; i++) {
        gchar *q_arg = g_shell_quote(d->argv[i]);
        g_string_append_printf(cmd, " %s", q_arg);
        g_free(q_arg);
    }

    /* 컨테이너 exec에 60초 타임아웃 적용 (무한 블로킹 방지)
     * pcv_spawn_sync()는 전역 공용이므로 직접 수정 대신 여기서 인라인 처리 */
    const gchar *exec_argv[] = {"/bin/sh", "-c", cmd->str, NULL};
    gchar *stdout_out = nullptr;
    gchar *stderr_out = nullptr;

    GSubprocess *proc = g_subprocess_newv(
        exec_argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &error);
    g_string_free(cmd, TRUE);

    if (!proc) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec spawn failed: %s",
                                error ? error->message : "unknown");
        if (error) g_error_free(error);
        return;
    }

    GCancellable *cancel = g_cancellable_new();
    guint timer_id = g_timeout_add_seconds(60, _cancel_on_timeout, cancel);
    gboolean ok = g_subprocess_communicate_utf8(
        proc, NULL, cancel, &stdout_out, &stderr_out, &error);
    g_source_remove(timer_id);
    g_object_unref(cancel);

    if (!ok) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec failed (timeout or error): %s",
                                error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_free(stdout_out); g_free(stderr_out);
        g_object_unref(proc);
        return;
    }

    gboolean success = g_subprocess_get_successful(proc);
    g_object_unref(proc);

    if (!success) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec failed: %s",
                                (stderr_out && *stderr_out) ? stderr_out : "non-zero exit");
        g_free(stdout_out); g_free(stderr_out);
    } else {
        g_free(stderr_out);
        g_task_return_pointer(task,
                              stdout_out ? stdout_out : g_strdup(""),
                              (GDestroyNotify)g_free);
    }
}

void
pcv_lxc_exec_async(const gchar        *name,
                   const gchar       **argv,
                   GCancellable       *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    GTask *task      = g_task_new(NULL, cancellable, callback, user_data);
    LxcExecData *d   = g_new0(LxcExecData, 1);
    d->name          = g_strdup(name);
    d->argv          = g_strdupv((gchar **)argv);
    g_task_set_task_data(task, d, (GDestroyNotify)_lxc_exec_data_free);
    g_task_run_in_thread(task, _lxc_exec_thread);
    g_object_unref(task);
}

gchar *
pcv_lxc_exec_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ══════════════════════════════════════════════════════════════════════════
 * ZFS SNAPSHOT API (컨테이너 전용: pcvpool/containers/<n>@<snap>)
 *
 * [ZFS 스냅샷 개념]
 *   ZFS 스냅샷은 특정 시점의 파일시스템 상태를 CoW(Copy-on-Write)로 저장합니다.
 *   스냅샷 생성은 순간적(O(1))이며, 변경된 블록만 추가 공간을 사용합니다.
 *
 *   경로 형식: pcvpool/containers/<컨테이너명>@<스냅샷명>
 *     예: pcvpool/containers/web-app@daily-backup
 *
 * [4가지 작업]
 *   CREATE:   zfs snapshot <dataset>@<snap>
 *   ROLLBACK: zfs rollback -r <dataset>@<snap>  (-r: 이후 스냅샷도 삭제)
 *   DELETE:   zfs destroy <dataset>@<snap>
 *   LIST:     zfs list -H -t snapshot -o name <dataset>
 * ══════════════════════════════════════════════════════════════════════════*/

typedef struct { gchar *name; gchar *snap; } LxcSnapData;

static void
_snap_data_free(LxcSnapData *d) { g_free(d->name); g_free(d->snap); g_free(d); }

/* CREATE */
static void
_snap_create_thread(GTask *task, gpointer src __attribute__((unused)),
                    gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error  = nullptr;
    gchar *target = g_strdup_printf("%s/%s@%s", PCV_LXC_ZFS_BASE, d->name, d->snap);
    const gchar *argv[] = { "zfs", "snapshot", target, NULL };
    if (!_run_argv(argv, &error)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "snapshot create failed: %s",
                                error ? error->message : "unknown");
        g_error_free(error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_free(target);
}

void pcv_lxc_snapshot_create_async(const gchar *name, const gchar *snap_name,
                                    GCancellable *c, GAsyncReadyCallback cb,
                                    gpointer user_data)
{
    GTask *task     = g_task_new(NULL, c, cb, user_data);
    LxcSnapData *d  = g_new0(LxcSnapData, 1);
    d->name = g_strdup(name); d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, (GDestroyNotify)_snap_data_free);
    g_task_run_in_thread(task, _snap_create_thread);
    g_object_unref(task);
}

gboolean pcv_lxc_snapshot_create_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

/* ROLLBACK */
static void
_snap_rollback_thread(GTask *task, gpointer src __attribute__((unused)),
                      gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error  = nullptr;
    gchar *target = g_strdup_printf("%s/%s@%s", PCV_LXC_ZFS_BASE, d->name, d->snap);
    const gchar *argv[] = { "zfs", "rollback", "-r", target, NULL };
    if (!_run_argv(argv, &error)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "snapshot rollback failed: %s",
                                error ? error->message : "unknown");
        g_error_free(error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_free(target);
}

void pcv_lxc_snapshot_rollback_async(const gchar *name, const gchar *snap_name,
                                      GCancellable *c, GAsyncReadyCallback cb,
                                      gpointer user_data)
{
    GTask *task     = g_task_new(NULL, c, cb, user_data);
    LxcSnapData *d  = g_new0(LxcSnapData, 1);
    d->name = g_strdup(name); d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, (GDestroyNotify)_snap_data_free);
    g_task_run_in_thread(task, _snap_rollback_thread);
    g_object_unref(task);
}

gboolean pcv_lxc_snapshot_rollback_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

/* DELETE */
static void
_snap_delete_thread(GTask *task, gpointer src __attribute__((unused)),
                    gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error  = nullptr;
    gchar *target = g_strdup_printf("%s/%s@%s", PCV_LXC_ZFS_BASE, d->name, d->snap);
    const gchar *argv[] = { "zfs", "destroy", target, NULL };
    if (!_run_argv(argv, &error)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "snapshot delete failed: %s",
                                error ? error->message : "unknown");
        g_error_free(error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_free(target);
}

void pcv_lxc_snapshot_delete_async(const gchar *name, const gchar *snap_name,
                                    GCancellable *c, GAsyncReadyCallback cb,
                                    gpointer user_data)
{
    GTask *task     = g_task_new(NULL, c, cb, user_data);
    LxcSnapData *d  = g_new0(LxcSnapData, 1);
    d->name = g_strdup(name); d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, (GDestroyNotify)_snap_data_free);
    g_task_run_in_thread(task, _snap_delete_thread);
    g_object_unref(task);
}

gboolean pcv_lxc_snapshot_delete_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

/* LIST */
static void
_snap_list_thread(GTask *task, gpointer src __attribute__((unused)),
                  gpointer td, GCancellable *c __attribute__((unused)))
{
    const gchar *name = (const gchar *)td;
    GError *error = nullptr;
    gchar *dataset = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
    const gchar *argv[] = {
        "zfs", "list", "-H", "-t", "snapshot", "-o", "name", dataset, NULL
    };
    gchar *stdout_out = _run_argv_capture(argv, &error);
    g_free(dataset);

    if (!stdout_out) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "snapshot list failed: %s",
                                error ? error->message : "zfs list error");
        if (error) g_error_free(error);
        return;
    }

    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
    gchar **lines     = g_strsplit(stdout_out ? stdout_out : "", "\n", -1);
    g_free(stdout_out);

    for (int i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!line[0]) continue;
        /* 형식: pcvpool/containers/<n>@<snap_name> → '@' 이후만 추출 */
        const gchar *at = strchr(line, '@');
        if (at) g_ptr_array_add(result, g_strdup(at + 1));
    }
    g_strfreev(lines);
    g_task_return_pointer(task, result,
                          (GDestroyNotify)g_ptr_array_unref);
}

void pcv_lxc_snapshot_list_async(const gchar *name,
                                   GCancellable *c, GAsyncReadyCallback cb,
                                   gpointer user_data)
{
    GTask *task = g_task_new(NULL, c, cb, user_data);
    g_task_set_task_data(task, g_strdup(name), (GDestroyNotify)g_free);
    g_task_run_in_thread(task, _snap_list_thread);
    g_object_unref(task);
}

GPtrArray *pcv_lxc_snapshot_list_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_pointer(G_TASK(r), e); }

/* ══════════════════════════════════════════════════════════════════════════
 * cgroup 리소스 제한 설정 (v2 + v1 자동 감지, 실행 중/정지 분기)
 *
 * [기능]
 *   LXC 컨테이너의 CPU/메모리 리소스 제한을 동적으로 변경합니다.
 *   - 실행 중: cgroup 파일에 직접 기록 (재시작 없이 즉시 적용)
 *   - 정지 상태: LXC config 파일만 업데이트 (다음 시작 시 적용)
 *
 * [cgroup v2 경로] (Ubuntu 22.04+ 기본, unified hierarchy)
 *   CPU:    /sys/fs/cgroup/lxc.payload.<name>/cpu.max
 *           형식: "<quota> <period>" (예: "50000 100000" = 50% CPU)
 *   메모리: /sys/fs/cgroup/lxc.payload.<name>/memory.max
 *           형식: "<bytes>" (예: "536870912" = 512MB)
 *
 * [cgroup v1 경로] (레거시 호스트)
 *   CPU:    /sys/fs/cgroup/cpu/lxc/<name>/cpu.cfs_quota_us
 *           형식: "<quota>" (예: "50000" = 50% with default 100ms period)
 *   메모리: /sys/fs/cgroup/memory/lxc/<name>/memory.limit_in_bytes
 *           형식: "<bytes>"
 *
 * [주의사항]
 *   - cpu_percent: 1-800 범위 (100 = 1 코어 전체, 200 = 2 코어, ...)
 *   - memory_mb: 최소 4MB 이상 권장
 *   - cgroup v1/v2 자동 감지: v2 경로 시도 후 실패 시 v1 폴백
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * cgroup 파일에 값 기록 — v2 경로 시도 후 실패 시 v1 폴백
 * @return TRUE 성공 (v2 또는 v1), FALSE 양쪽 모두 실패
 */
static gboolean
_cgroup_write_with_fallback(const gchar *path_v2, const gchar *val_v2,
                             const gchar *path_v1, const gchar *val_v1,
                             GError **error)
{
    /* cgroup v2 시도 */
    if (g_file_test(path_v2, G_FILE_TEST_EXISTS)) {
        if (g_file_set_contents(path_v2, val_v2, -1, error))
            return TRUE;
        /* v2 경로 존재하지만 쓰기 실패 — 에러 전파 */
        return FALSE;
    }

    /* cgroup v1 폴백 */
    if (path_v1 && g_file_test(path_v1, G_FILE_TEST_EXISTS)) {
        if (g_file_set_contents(path_v1, val_v1 ? val_v1 : val_v2, -1, error))
            return TRUE;
        return FALSE;
    }

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "cgroup path not found: %s (v2) / %s (v1)", path_v2,
                path_v1 ? path_v1 : "N/A");
    return FALSE;
}

/**
 * 정지된 컨테이너의 LXC config에 리소스 제한 기록
 * liblxc API: c->set_config_item() + c->save_config()
 */
static gboolean
_set_limits_config(const gchar *name, gint cpu_percent, gint memory_mb,
                   gint cpu_weight, gint memory_low_mb, gint memory_high_mb,
                   gint64 io_read_bps, gint pids_max, GError **error)
{
    struct lxc_container *c = _lxc_get(name, error);
    if (!c) return FALSE;

    gboolean ok = TRUE;

    if (cpu_percent > 0) {
        gchar *v2_val = g_strdup_printf("%d 100000", cpu_percent * 1000);
        gchar *v1_val = g_strdup_printf("%d", cpu_percent * 1000);
        if (!c->set_config_item(c, "lxc.cgroup2.cpu.max", v2_val)) {
            /* cgroup v1 config fallback */
            if (!c->set_config_item(c, "lxc.cgroup.cpu.cfs_quota_us", v1_val)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to set CPU config for '%s'", name);
                ok = FALSE;
            }
        }
        g_free(v2_val);
        g_free(v1_val);
    }

    if (ok && cpu_weight > 0) {
        gchar *val = g_strdup_printf("%d", cpu_weight);
        if (!c->set_config_item(c, "lxc.cgroup2.cpu.weight", val)) {
            g_warning("Failed to set cpu.weight config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && memory_mb > 0) {
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_mb * 1024 * 1024);
        if (!c->set_config_item(c, "lxc.cgroup2.memory.max", val)) {
            if (!c->set_config_item(c, "lxc.cgroup.memory.limit_in_bytes", val)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to set memory config for '%s'", name);
                ok = FALSE;
            }
        }
        g_free(val);
    }

    if (ok && memory_low_mb > 0) {
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_low_mb * 1024 * 1024);
        if (!c->set_config_item(c, "lxc.cgroup2.memory.low", val)) {
            g_warning("Failed to set memory.low config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && memory_high_mb > 0) {
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_high_mb * 1024 * 1024);
        if (!c->set_config_item(c, "lxc.cgroup2.memory.high", val)) {
            g_warning("Failed to set memory.high config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && io_read_bps > 0) {
        gchar *val = g_strdup_printf("8:0 rbps=%" G_GINT64_FORMAT, io_read_bps);
        if (!c->set_config_item(c, "lxc.cgroup2.io.max", val)) {
            g_warning("Failed to set io.max config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && pids_max > 0) {
        gchar *val = g_strdup_printf("%d", pids_max);
        if (!c->set_config_item(c, "lxc.cgroup2.pids.max", val)) {
            g_warning("Failed to set pids.max config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok) {
        gchar *cfg_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
        if (!c->save_config(c, cfg_path)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to save config for '%s'", name);
            ok = FALSE;
        }
        g_free(cfg_path);
    }

    lxc_container_put(c);
    return ok;
}

gboolean
pcv_lxc_set_resource_limits(const gchar *name, gint cpu_percent,
                             gint memory_mb, gint cpu_weight,
                             gint memory_low_mb, gint memory_high_mb,
                             gint64 io_read_bps, gint pids_max,
                             GError **error)
{
    if (!name || !*name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Container name is required");
        return FALSE;
    }

    /* 컨테이너 실행 상태 확인 */
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }
    gboolean running = c->is_running(c);
    lxc_container_put(c);

    /* 정지 상태: config 파일만 업데이트 (다음 시작 시 적용) */
    if (!running) {
        return _set_limits_config(name, cpu_percent, memory_mb, cpu_weight,
                                  memory_low_mb, memory_high_mb,
                                  io_read_bps, pids_max, error);
    }

    /* 실행 중: cgroup 파일에 직접 기록 (즉시 적용) */
    gboolean success = TRUE;

    /* CPU 제한 설정: cpu_percent * 1000 quota / 100000 period */
    if (cpu_percent > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/cpu.max", name);
        gchar *v1_path = g_strdup_printf(
            "/sys/fs/cgroup/cpu/lxc/%s/cpu.cfs_quota_us", name);
        gchar *v2_val = g_strdup_printf("%d 100000", cpu_percent * 1000);
        gchar *v1_val = g_strdup_printf("%d", cpu_percent * 1000);

        if (!_cgroup_write_with_fallback(v2_path, v2_val, v1_path, v1_val, error)) {
            g_prefix_error(error, "Failed to set CPU limit for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path); g_free(v1_path);
        g_free(v2_val);  g_free(v1_val);
    }

    /* cpu.weight 설정 (cgroup v2 전용, 1-10000) */
    if (success && cpu_weight > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/cpu.weight", name);
        gchar *val = g_strdup_printf("%d", cpu_weight);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set cpu.weight for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

    /* 메모리 제한 설정: memory_mb * 1024 * 1024 bytes */
    if (success && memory_mb > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/memory.max", name);
        gchar *v1_path = g_strdup_printf(
            "/sys/fs/cgroup/memory/lxc/%s/memory.limit_in_bytes", name);
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_mb * 1024 * 1024);

        if (!_cgroup_write_with_fallback(v2_path, val, v1_path, val, error)) {
            g_prefix_error(error, "Failed to set memory limit for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path); g_free(v1_path);
        g_free(val);
    }

    /* memory.low 소프트 제한 (cgroup v2 전용) */
    if (success && memory_low_mb > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/memory.low", name);
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_low_mb * 1024 * 1024);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set memory.low for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

    /* memory.high 하드 제한 (cgroup v2 전용, OOM 전 throttling) */
    if (success && memory_high_mb > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/memory.high", name);
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_high_mb * 1024 * 1024);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set memory.high for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

    /* io.max 읽기 대역폭 제한 (cgroup v2 전용, major:minor 8:0 = /dev/sda) */
    if (success && io_read_bps > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/io.max", name);
        gchar *val = g_strdup_printf("8:0 rbps=%" G_GINT64_FORMAT, io_read_bps);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set io.max for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

    /* pids.max 최대 프로세스 수 제한 (cgroup v2 전용) */
    if (success && pids_max > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/pids.max", name);
        gchar *val = g_strdup_printf("%d", pids_max);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set pids.max for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

    /* 실행 중에도 config 업데이트 (재시작 후에도 제한 유지) */
    if (success) {
        GError *cfg_err = nullptr;
        if (!_set_limits_config(name, cpu_percent, memory_mb, cpu_weight,
                                memory_low_mb, memory_high_mb,
                                io_read_bps, pids_max, &cfg_err)) {
            /* config 저장 실패는 경고만 — cgroup 적용은 이미 성공 */
            g_clear_error(&cfg_err);
        }
    }

    return success;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 네트워크 관리 — NIC list / attach / detach / bandwidth
 * VM(libvirt) 수준의 네트워크 관리를 LXC에서도 제공
 * ══════════════════════════════════════════════════════════════════════════*/

void
pcv_lxc_nic_info_free(PcvLxcNicInfo *nic)
{
    if (!nic) return;
    g_free(nic->name);
    g_free(nic->type);
    g_free(nic->bridge);
    g_free(nic->hwaddr);
    g_free(nic->ipv4);
    g_free(nic->veth_peer);
    g_free(nic);
}

/**
 * NIC 목록 조회 — lxc config 파싱 (lxc.net.X.type/link/hwaddr)
 * 실행 중이면 lxc-attach ip -4 addr 로 IP 정보 병합
 */
GPtrArray *
pcv_lxc_nic_list(const gchar *name, GError **error)
{
    GPtrArray *nics = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_lxc_nic_info_free);

    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return nics;
    }
    if (!c->is_defined(c)) {
        lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not defined", name);
        return nics;
    }

    /* lxc.net.X 설정 파싱 (최대 16 NIC) */
    for (int idx = 0; idx < 16; idx++) {
        gchar key_type[64], key_link[64], key_hwaddr[64], key_name[64];
        g_snprintf(key_type,   sizeof(key_type),   "lxc.net.%d.type",   idx);
        g_snprintf(key_link,   sizeof(key_link),   "lxc.net.%d.link",   idx);
        g_snprintf(key_hwaddr, sizeof(key_hwaddr), "lxc.net.%d.hwaddr", idx);
        g_snprintf(key_name,   sizeof(key_name),   "lxc.net.%d.name",   idx);

        char buf[256] = {0};
        int ret = c->get_config_item(c, key_type, buf, sizeof(buf));
        if (ret <= 0 || buf[0] == '\0') break;

        PcvLxcNicInfo *nic = g_new0(PcvLxcNicInfo, 1);
        nic->type = g_strdup(buf);

        buf[0] = '\0';
        c->get_config_item(c, key_link, buf, sizeof(buf));
        nic->bridge = g_strdup(buf[0] ? buf : "none");

        buf[0] = '\0';
        c->get_config_item(c, key_hwaddr, buf, sizeof(buf));
        nic->hwaddr = g_strdup(buf[0] ? buf : "auto");

        buf[0] = '\0';
        c->get_config_item(c, key_name, buf, sizeof(buf));
        nic->name = g_strdup(buf[0] ? buf : g_strdup_printf("eth%d", idx));

        nic->ipv4 = g_strdup("");
        nic->veth_peer = g_strdup("");
        g_ptr_array_add(nics, nic);
    }

    /* 실행 중이면 IP 주소 가져오기 */
    if (c->is_running(c) && nics->len > 0) {
        gchar *cmd_line = g_strdup_printf(
            "lxc-attach -P %s -n %s -- ip -4 -o addr show 2>/dev/null",
            PCV_LXC_PATH, name);
        gchar *out = nullptr;
        const gchar *argv[] = {"/bin/sh", "-c", cmd_line, NULL};
        if (pcv_spawn_sync(argv, &out, NULL, NULL) && out) {
            /* 파싱: "2: eth0    inet 10.0.3.42/24 ..." */
            gchar **lines = g_strsplit(out, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                for (guint n = 0; n < nics->len; n++) {
                    PcvLxcNicInfo *ni = g_ptr_array_index(nics, n);
                    if (g_strstr_len(lines[i], -1, ni->name)) {
                        /* inet X.X.X.X/Y 추출 */
                        const gchar *inet = g_strstr_len(lines[i], -1, "inet ");
                        if (inet) {
                            inet += 5;
                            const gchar *sp = strchr(inet, ' ');
                            g_free(ni->ipv4);
                            ni->ipv4 = sp ? g_strndup(inet, (gsize)(sp - inet))
                                          : g_strdup(inet);
                        }
                        break;
                    }
                }
            }
            g_strfreev(lines);
        }
        g_free(out);
        g_free(cmd_line);
    }

    lxc_container_put(c);
    return nics;
}

/**
 * NIC 추가 — lxc config에 lxc.net.N 항목 추가 + 실행 중이면 동적 연결
 */
gboolean
pcv_lxc_nic_attach(const gchar *name, const gchar *bridge,
                     const gchar *hwaddr, GError **error)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        if (c) lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }

    const gchar *br = bridge ? bridge : PCV_LXC_DEFAULT_BRIDGE;

    /* 다음 빈 인덱스 찾기 */
    int idx = 0;
    for (; idx < 16; idx++) {
        gchar key[64]; char buf[32] = {0};
        g_snprintf(key, sizeof(key), "lxc.net.%d.type", idx);
        if (c->get_config_item(c, key, buf, sizeof(buf)) <= 0 || buf[0] == '\0')
            break;
    }

    gchar k1[64], k2[64], k3[64], k4[64];
    g_snprintf(k1, sizeof(k1), "lxc.net.%d.type",   idx);
    g_snprintf(k2, sizeof(k2), "lxc.net.%d.link",   idx);
    g_snprintf(k3, sizeof(k3), "lxc.net.%d.flags",  idx);
    g_snprintf(k4, sizeof(k4), "lxc.net.%d.name",   idx);

    c->set_config_item(c, k1, "veth");
    c->set_config_item(c, k2, br);
    c->set_config_item(c, k3, "up");

    gchar ethname[16];
    g_snprintf(ethname, sizeof(ethname), "eth%d", idx);
    c->set_config_item(c, k4, ethname);

    if (hwaddr && *hwaddr) {
        gchar k5[64];
        g_snprintf(k5, sizeof(k5), "lxc.net.%d.hwaddr", idx);
        c->set_config_item(c, k5, hwaddr);
    }

    c->save_config(c, NULL);

    /* 실행 중이면 동적으로 인터페이스 추가 */
    if (c->is_running(c)) {
        gchar *cmd = g_strdup_printf(
            "ip link add %s-host type veth peer name %s && "
            "ip link set %s-host master %s && "
            "ip link set %s-host up && "
            "ip link set %s netns $(cat /sys/fs/cgroup/lxc.payload.%s/cgroup.procs | head -1) && "
            "nsenter -t $(cat /sys/fs/cgroup/lxc.payload.%s/cgroup.procs | head -1) -n ip link set %s up",
            ethname, ethname, ethname, br, ethname, ethname, name, name, ethname);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync(argv, NULL, NULL, NULL);
        g_free(cmd);
    }

    lxc_container_put(c);
    return TRUE;
}

/**
 * NIC 제거 — lxc config에서 lxc.net.N 설정 삭제
 */
gboolean
pcv_lxc_nic_detach(const gchar *name, const gchar *nic_name, GError **error)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        if (c) lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }

    /* 해당 NIC의 인덱스 찾기 */
    int target = -1;
    for (int idx = 0; idx < 16; idx++) {
        gchar key[64]; char buf[64] = {0};
        g_snprintf(key, sizeof(key), "lxc.net.%d.name", idx);
        if (c->get_config_item(c, key, buf, sizeof(buf)) <= 0) break;
        if (g_strcmp0(buf, nic_name) == 0) { target = idx; break; }
    }

    if (target < 0) {
        /* 인덱스로 직접 시도 (eth1 → 1) */
        if (g_str_has_prefix(nic_name, "eth"))
            target = atoi(nic_name + 3);
    }

    if (target <= 0) { /* eth0 (인덱스 0) 삭제 방지 */
        lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Cannot remove primary NIC (eth0) or NIC not found: %s", nic_name);
        return FALSE;
    }

    /* 실행 중이면 먼저 인터페이스 제거 */
    if (c->is_running(c)) {
        gchar *cmd = g_strdup_printf(
            "lxc-attach -P %s -n %s -- ip link del %s 2>/dev/null",
            PCV_LXC_PATH, name, nic_name);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync(argv, NULL, NULL, NULL);
        g_free(cmd);
    }

    /* config에서 해당 net 항목 clear */
    const gchar *keys[] = {"type", "link", "flags", "name", "hwaddr",
                            "ipv4.address", "ipv4.gateway", NULL};
    for (int i = 0; keys[i]; i++) {
        gchar key[64];
        g_snprintf(key, sizeof(key), "lxc.net.%d.%s", target, keys[i]);
        c->clear_config_item(c, key);
    }
    c->save_config(c, NULL);

    lxc_container_put(c);
    return TRUE;
}

/**
 * 대역폭 제한 — tc qdisc (호스트측 veth에 적용)
 */
gboolean
pcv_lxc_set_bandwidth(const gchar *name, const gchar *nic_name,
                        guint inbound_kbps, guint outbound_kbps,
                        GError **error)
{
    if (!name || !*name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Container name required");
        return FALSE;
    }

    /* 호스트측 veth 이름 찾기: ip link show type veth */
    const gchar *iface = nic_name ? nic_name : "eth0";
    gchar *find_cmd = g_strdup_printf(
        "ip link show type veth 2>/dev/null | "
        "awk -F'[@:]' '/%s/{gsub(/ /,\"\",$$2); print $$2}'",
        name);
    gchar *host_veth = nullptr;
    const gchar *argv_find[] = {"/bin/sh", "-c", find_cmd, NULL};
    pcv_spawn_sync(argv_find, &host_veth, NULL, NULL);
    g_free(find_cmd);

    if (!host_veth || !*host_veth) {
        /* 폴백: 컨테이너 내부 tc 적용 */
        g_free(host_veth);
        if (inbound_kbps > 0) {
            gchar *cmd = g_strdup_printf(
                "lxc-attach -P %s -n %s -- sh -c '"
                "tc qdisc del dev %s root 2>/dev/null; "
                "tc qdisc add dev %s root tbf rate %dkbit burst 32kbit latency 50ms"
                "'", PCV_LXC_PATH, name, iface, iface, inbound_kbps);
            const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
            pcv_spawn_sync(argv, NULL, NULL, NULL);
            g_free(cmd);
        }
        return TRUE;
    }

    g_strstrip(host_veth);

    /* tc qdisc 적용 (호스트측 veth) */
    if (outbound_kbps > 0) {
        gchar *cmd = g_strdup_printf(
            "tc qdisc del dev %s root 2>/dev/null; "
            "tc qdisc add dev %s root tbf rate %dkbit burst 32kbit latency 50ms",
            host_veth, host_veth, outbound_kbps);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync(argv, NULL, NULL, NULL);
        g_free(cmd);
    }

    /* ingress (inbound to container = egress from container perspective) */
    if (inbound_kbps > 0) {
        gchar *cmd = g_strdup_printf(
            "lxc-attach -P %s -n %s -- sh -c '"
            "tc qdisc del dev %s root 2>/dev/null; "
            "tc qdisc add dev %s root tbf rate %dkbit burst 32kbit latency 50ms"
            "'", PCV_LXC_PATH, name, iface, iface, inbound_kbps);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync(argv, NULL, NULL, NULL);
        g_free(cmd);
    }

    g_free(host_veth);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * CRIU 기반 체크포인트/복원 (CE-A9)
 *
 * lxc-checkpoint CLI를 통해 CRIU 체크포인트(상태 저장)와 복원을 수행합니다.
 * CRIU가 설치되지 않은 환경에서는 graceful 실패합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * pcv_lxc_checkpoint — CRIU 기반 컨테이너 체크포인트 (상태 저장)
 *
 * @param name            컨테이너 이름
 * @param checkpoint_dir  체크포인트 이미지 저장 디렉터리
 * @return TRUE 성공, FALSE 실패 (CRIU 미설치 포함)
 */
gboolean
pcv_lxc_checkpoint(const gchar *name, const gchar *checkpoint_dir)
{
    if (!name || !checkpoint_dir) return FALSE;

    /* 체크포인트 디렉터리 생성 */
    g_mkdir_with_parents(checkpoint_dir, 0700);

    const gchar *argv[] = {
        "lxc-checkpoint", "-P", PCV_LXC_PATH,
        "-n", name, "-D", checkpoint_dir, "-s", NULL
    };
    gchar *std_err = nullptr;
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &err);

    if (ok) {
        PCV_LOG_INFO(LXC_LOG_DOM, "Checkpointed container '%s' to %s",
                     name, checkpoint_dir);
    } else {
        PCV_LOG_WARN(LXC_LOG_DOM, "Checkpoint failed for '%s': %s",
                     name, err ? err->message :
                     (std_err ? std_err : "CRIU not available"));
        if (err) g_error_free(err);
    }
    g_free(std_err);
    return ok;
}

/**
 * pcv_lxc_restore — CRIU 기반 컨테이너 복원
 *
 * @param name            컨테이너 이름
 * @param checkpoint_dir  체크포인트 이미지 디렉터리
 * @return TRUE 성공, FALSE 실패
 */
gboolean
pcv_lxc_restore(const gchar *name, const gchar *checkpoint_dir)
{
    if (!name || !checkpoint_dir) return FALSE;

    const gchar *argv[] = {
        "lxc-checkpoint", "-P", PCV_LXC_PATH,
        "-n", name, "-D", checkpoint_dir, "-r", NULL
    };
    gchar *std_err = nullptr;
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &err);

    if (ok) {
        PCV_LOG_INFO(LXC_LOG_DOM, "Restored container '%s' from %s",
                     name, checkpoint_dir);
    } else {
        PCV_LOG_WARN(LXC_LOG_DOM, "Restore failed for '%s': %s",
                     name, err ? err->message :
                     (std_err ? std_err : "unknown"));
        if (err) g_error_free(err);
    }
    g_free(std_err);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SECCOMP 프로파일 관리 (R-7)
 *
 * 컨테이너별 seccomp 프로파일 적용/조회 기능.
 * /etc/purecvisor/seccomp/<profile>.seccomp 파일을 LXC 설정에 연결합니다.
 * ══════════════════════════════════════════════════════════════════════════*/

#define PCV_SECCOMP_DIR "/etc/purecvisor/seccomp"

/**
 * pcv_lxc_set_seccomp_profile — 컨테이너에 seccomp 프로파일 적용
 *
 * LXC 설정 파일에 lxc.seccomp.profile 항목을 추가/변경한다.
 * 기존 seccomp 설정이 있으면 제거 후 새로 추가한다.
 */
gboolean
pcv_lxc_set_seccomp_profile(const gchar *name, const gchar *profile_name)
{
    if (!name || !profile_name) return FALSE;

    gchar *profile_path = g_strdup_printf("%s/%s.seccomp", PCV_SECCOMP_DIR, profile_name);

    /* 프로파일 파일 존재 확인 */
    if (!g_file_test(profile_path, G_FILE_TEST_EXISTS)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Seccomp profile not found: %s", profile_path);
        g_free(profile_path);
        return FALSE;
    }

    /* LXC 설정 파일에 seccomp 프로파일 경로 추가 */
    gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);

    /* 기존 seccomp 설정 제거 후 새로 추가 */
    gchar *contents = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(config_path, &contents, &len, NULL)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Cannot read container config: %s", config_path);
        g_free(profile_path);
        g_free(config_path);
        return FALSE;
    }

    GString *new_config = g_string_new("");
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (!g_str_has_prefix(*l, "lxc.seccomp.profile"))
            g_string_append_printf(new_config, "%s\n", *l);
    }
    g_string_append_printf(new_config, "lxc.seccomp.profile = %s\n", profile_path);
    g_strfreev(lines);
    g_free(contents);

    gboolean ok = g_file_set_contents(config_path, new_config->str, -1, NULL);
    g_string_free(new_config, TRUE);
    g_free(config_path);

    if (ok)
        PCV_LOG_INFO(LXC_LOG_DOM, "Set seccomp profile '%s' for container '%s'",
                     profile_name, name);
    else
        PCV_LOG_WARN(LXC_LOG_DOM, "Failed to write seccomp config for '%s'", name);

    g_free(profile_path);
    return ok;
}

/**
 * pcv_lxc_get_seccomp_profile — 컨테이너의 현재 seccomp 프로파일 조회
 *
 * @return 프로파일 경로 문자열 (호출자 g_free 필수), 미설정 시 NULL
 */
gchar *
pcv_lxc_get_seccomp_profile(const gchar *name)
{
    if (!name) return NULL;
    gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
    gchar *contents = nullptr;
    if (!g_file_get_contents(config_path, &contents, NULL, NULL)) {
        g_free(config_path);
        return NULL;
    }
    g_free(config_path);

    gchar *result = nullptr;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (g_str_has_prefix(*l, "lxc.seccomp.profile")) {
            gchar *eq = strchr(*l, '=');
            if (eq) result = g_strdup(g_strstrip(eq + 1));
            break;
        }
    }
    g_strfreev(lines);
    g_free(contents);
    return result;
}
