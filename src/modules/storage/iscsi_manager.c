/**
 * @file iscsi_manager.c
 * @brief iSCSI 타겟/이니시에이터 매니저 (tgtadm/iscsiadm)
 *
 * ZFS zvol을 iSCSI LUN으로 익스포트하고
 * 원격 노드에서 iSCSI로 마운트할 수 있게 합니다.
 *
 * [비전공자 설명]
 * iSCSI는 네트워크 케이블을 통해 "원격 서버의 디스크를 내 서버에 직접 꽂힌
 * 디스크처럼" 보이게 하는 기술입니다. PureCVisor에서는 TrueNAS 같은 외부
 * 스토리지의 zvol을 VM 디스크로 연결할 때 이 계층이 사용됩니다.
 *
 * [주니어 참고]
 * target은 디스크를 제공하는 쪽, initiator는 그 디스크에 접속하는 쪽입니다.
 * 이 파일은 tgtadm/iscsiadm CLI를 감싸므로, 실패 원인은 커널 모듈, 방화벽,
 * ACL, IQN 오타, 라우팅 경로 중 어디에서든 발생할 수 있습니다. 에러 메시지를
 * 단순히 "mount 실패"로 뭉개지 말고 stdout/stderr와 대상 IQN을 함께 봐야 합니다.
 *
 * 타겟 생성 흐름:
 *   tgtadm --op new --mode target (IQN 생성)
 *   tgtadm --op new --mode logicalunit (zvol backing store)
 *   tgtadm --op bind (initiator 허용)
 *
 * 이니시에이터 흐름:
 *   iscsiadm -m discovery (타겟 검색)
 *   iscsiadm -m node --login (로그인)
 *   /dev/disk/by-path/ 에서 디바이스 경로 탐색
 *
 * IQN 형식: iqn.2026-03.purecvisor:<vm_name>
 */
#include "iscsi_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"
#include <string.h>

/** 로그 도메인 — journalctl에서 iscsi_mgr로 필터링 가능 */
#define ISCSI_LOG_DOM   "iscsi_mgr"

/** IQN (iSCSI Qualified Name) 접두사 — RFC 3720 형식 */
#define ISCSI_IQN_PFX   "iqn.2026-03.purecvisor"

/** 동시에 관리 가능한 최대 iSCSI 타겟 수 (정적 배열 크기) */
#define ISCSI_MAX_TID   64

/* ── 내부 상태 ────────────────────────────────────────────────────
 * 정적 배열 + 카운터 방식의 간단한 타겟 관리.
 * 해시 테이블 대신 정적 배열을 사용하는 이유:
 *   - iSCSI 타겟 수가 ISCSI_MAX_TID(64) 이하로 제한적
 *   - 선형 탐색으로 충분한 성능 (O(64) = 상수 시간)
 *   - 메모리 할당/해제 오버헤드 최소화
 * ────────────────────────────────────────────────────────────────── */

/**
 * IscsiTarget:
 * 개별 iSCSI 타겟의 인메모리 레코드.
 * vm_name으로 타겟을 식별하고, tid로 tgtadm 명령을 제어합니다.
 */
typedef struct {
    gchar *vm_name;   /* VM 이름 — IQN의 suffix이자 검색 키 */
    gint   tid;       /* tgtadm 타겟 ID (1부터 순차 할당) */
} IscsiTarget;

/** 전역 iSCSI 관리 상태 — 프로세스당 1개 인스턴스 */
static struct {
    IscsiTarget targets[ISCSI_MAX_TID];  /* 타겟 레코드 배열 */
    gint        count;                    /* 현재 등록된 타겟 수 */
    gint        next_tid;                 /* 다음에 할당할 tgtadm tid */
    GMutex      mu;                       /* 모든 상태 접근 직렬화 */
    gboolean    initialized;              /* 초기화 여부 플래그 */
} G = {0};

/**
 * _run:
 * tgtadm/iscsiadm CLI를 동기 실행하는 내부 헬퍼입니다.
 * NOTE: 실제로는 셸(/bin/sh -c)이 아니라 g_shell_parse_argv 로 토큰화한 뒤
 *       pcv_spawn_sync(execvp 계열)로 실행합니다 — 즉 셸 연산자
 *       (파이프/리다이렉트/글롭)는 해석되지 않고 리터럴 argv 로 전달됩니다.
 *       셸 연산자나 외부 입력 인자가 필요한 명령은 _run_argv 를 쓰십시오.
 *
 * @param cmd   명령 문자열 (셸 연산자 금지 — tid 등 신뢰 인자만)
 * @param out   stdout 출력 버퍼 (NULL 가능 — 필요 없을 때)
 * @param error GError** (NULL 가능)
 * @return TRUE: exit code 0, FALSE: 실패
 */
static gboolean
_run(const gchar *cmd, gchar **out, GError **error)
{
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(ISCSI_LOG_DOM, "cmd failed: %s → %s", cmd, std_err);
    g_free(std_err);
    g_strfreev(parsed);
    return ok;
}

/**
 * _run_argv:
 * argv 배열을 g_shell_parse_argv 없이 직접 pcv_spawn_sync(execvp)로 실행합니다.
 * _run 과 달리 문자열 재토큰화가 없어, 인자에 공백/따옴표/셸 연산자가 있어도
 * 재분할·인젝션이 발생하지 않습니다(M-2). target_ip 등 외부 입력을 인자로 받는
 * iSCSI initiator 명령에 사용합니다.
 */
static gboolean
_run_argv(const gchar *const *argv, gchar **out, GError **error)
{
    gchar *std_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &std_err, error);
    if (!ok && std_err)
        PCV_LOG_WARN(ISCSI_LOG_DOM, "cmd failed: %s → %s",
                     (argv && argv[0]) ? argv[0] : "?", std_err);
    g_free(std_err);
    return ok;
}

/**
 * _find_iscsi_device:
 * /dev/disk/by-path/ 에서 target_ip 를 포함하고 "lun-1" 로 끝나는 항목을 찾아
 * 전체 경로를 반환합니다(호출자 g_free). 없으면 NULL.
 * 이전에는 `ls` + 글롭(ip 포함, lun-1 접미) + 파이프 head 셸 명령이었으나
 * 셸 부재로 글롭 미확장·파이프 리터럴화되어 항상 실패했습니다(M-2).
 */
static gchar *
_find_iscsi_device(const gchar *target_ip)
{
    const gchar *dir = "/dev/disk/by-path";
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    gchar *found = NULL;
    const gchar *name;
    while ((name = g_dir_read_name(d))) {
        if (g_strstr_len(name, -1, target_ip) && g_str_has_suffix(name, "lun-1")) {
            found = g_build_filename(dir, name, NULL);
            break;
        }
    }
    g_dir_close(d);
    return found;
}

/**
 * _find_target:
 * VM 이름으로 타겟 레코드를 선형 탐색합니다.
 *
 * @param vm_name 검색할 VM 이름
 * @return 찾은 IscsiTarget 포인터, 없으면 NULL
 *
 * 주의: 호출자가 G.mu 뮤텍스를 보유한 상태에서만 호출해야 합니다.
 */
static IscsiTarget *
_find_target(const gchar *vm_name)
{
    for (gint i = 0; i < G.count; i++)
        if (g_strcmp0(G.targets[i].vm_name, vm_name) == 0)
            return &G.targets[i];
    return NULL;
}

/* ── 생명주기 (초기화/종료) ────────────────────────────────────── */

/**
 * pcv_iscsi_init:
 * iSCSI 매니저를 초기화합니다. 데몬 시작 시 1회 호출됩니다.
 * tid는 1부터 시작 — tgtadm에서 tid=0은 예약되어 있습니다.
 */
void
pcv_iscsi_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;
    G.next_tid = 1;
    G.initialized = TRUE;
    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI manager initialized");
}

/**
 * pcv_iscsi_shutdown:
 * iSCSI 매니저를 종료합니다. 인메모리 타겟 레코드를 모두 해제합니다.
 *
 * 주의: tgtadm 타겟 자체는 삭제하지 않습니다 (tgt 데몬이 관리).
 * 데몬 재시작 시 tgt 타겟은 유지되지만 인메모리 레코드는 소실됩니다.
 */
void
pcv_iscsi_shutdown(void)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++)
        g_free(G.targets[i].vm_name);
    G.count = 0;
    g_mutex_unlock(&G.mu);
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/* ── 타겟 관리 (생성/삭제/목록) ────────────────────────────────── */

/**
 * pcv_iscsi_target_create:
 * ZFS zvol을 iSCSI LUN으로 익스포트하는 타겟을 생성합니다.
 *
 * 생성 단계 (3단계 tgtadm 호출):
 *   1. tgtadm --op new --mode target: IQN으로 타겟 생성
 *   2. tgtadm --op new --mode logicalunit: zvol을 LUN 1로 연결
 *   3. tgtadm --op bind: 모든 이니시에이터에 접근 허용 (ALL)
 *
 * 멱등성: 동일 VM의 타겟이 이미 존재하면 TRUE를 즉시 반환합니다.
 * 롤백: 2단계 실패 시 1단계에서 생성한 타겟을 자동 삭제합니다.
 *
 * @param vm_name   VM 이름 — IQN suffix로 사용
 * @param zvol_path ZFS zvol 블록 디바이스 경로 (예: /dev/zvol/pcvpool/vms/web-prod)
 * @param error     실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean
pcv_iscsi_target_create(const gchar *vm_name, const gchar *zvol_path, GError **error)
{
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "iSCSI not initialized");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    if (_find_target(vm_name)) {
        g_mutex_unlock(&G.mu);
        return TRUE;  /* idempotent */
    }
    if (G.count >= ISCSI_MAX_TID) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Max iSCSI targets reached");
        return FALSE;
    }

    gint tid = G.next_tid++;
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);

    /* 1. Create target */
    gchar *cmd1 = g_strdup_printf(
        "tgtadm --lld iscsi --op new --mode target --tid %d --targetname %s",
        tid, iqn);
    if (!_run(cmd1, NULL, error)) {
        g_free(cmd1); g_free(iqn);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    g_free(cmd1);

    /* 2. Add backing store (LUN 1) */
    gchar *cmd2 = g_strdup_printf(
        "tgtadm --lld iscsi --op new --mode logicalunit --tid %d --lun 1 --backing-store %s",
        tid, zvol_path);
    if (!_run(cmd2, NULL, error)) {
        g_free(cmd2); g_free(iqn);
        /* Cleanup: delete the target we just created */
        gchar *del = g_strdup_printf("tgtadm --lld iscsi --op delete --mode target --tid %d --force", tid);
        _run(del, NULL, NULL);
        g_free(del);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }
    g_free(cmd2);

    /* 3. 모든 이니시에이터에 바인드 (접근 허용)
     * soft-fail: 바인드 실패해도 타겟 자체는 유효하므로 에러 무시 */
    gchar *cmd3 = g_strdup_printf(
        "tgtadm --lld iscsi --op bind --mode target --tid %d --initiator-address ALL",
        tid);
    _run(cmd3, NULL, NULL);  /* soft-fail OK */
    g_free(cmd3);

    /* 4. CHAP 인증 설정 (daemon.conf [iscsi] 섹션에 설정된 경우)
     * soft-fail: CHAP 설정 실패해도 타겟 자체는 사용 가능 */
    {
        const gchar *chap_user = pcv_config_get_string("iscsi", "chap_user", NULL);
        gchar *chap_pass = pcv_config_get_secret("iscsi", "chap_password", NULL);
        if (chap_user && chap_pass && *chap_user && *chap_pass) {
            /* M-2류: _run(g_shell_parse_argv 재토큰화) 대신 argv 직접 전달 —
             * chap_pass 에 공백/따옴표가 있어도 재분할되지 않고 단일 인자로 전달(STO-4). */
            const gchar *acc_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "new", "--mode", "account",
                                       "--user", chap_user, "--password", chap_pass, NULL};
            _run_argv(acc_argv, NULL, NULL);  /* soft-fail: 계정 이미 존재 가능 */

            gchar *tid_str = g_strdup_printf("%d", tid);
            const gchar *bind_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "bind", "--mode", "account",
                                        "--tid", tid_str, "--user", chap_user, NULL};
            if (_run_argv(bind_argv, NULL, NULL))
                PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP account bound: user=%s tid=%d", chap_user, tid);
            else
                PCV_LOG_WARN(ISCSI_LOG_DOM, "CHAP bind failed for tid=%d (non-fatal)", tid);
            g_free(tid_str);
        }
        g_free(chap_pass);
    }

    /* 5. 인메모리 레코드에 등록 — 이후 조회/삭제에 사용 */
    IscsiTarget *t = &G.targets[G.count++];
    t->vm_name = g_strdup(vm_name);
    t->tid = tid;

    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI target created: tid=%d iqn=%s backing=%s",
                 tid, iqn, zvol_path);
    g_free(iqn);
    return TRUE;
}

/**
 * pcv_iscsi_target_delete:
 * iSCSI 타겟을 삭제합니다. --force 플래그로 활성 세션도 강제 종료합니다.
 *
 * 멱등성: 존재하지 않는 타겟 삭제 시 TRUE를 반환합니다.
 * 배열 정리: 삭제된 슬롯에 마지막 요소를 이동 (swap-remove 패턴).
 *
 * @param vm_name VM 이름
 * @param error   실패 시 GError** 설정
 * @return TRUE: 성공 (이미 없는 경우 포함), FALSE: tgtadm 실패
 */
gboolean
pcv_iscsi_target_delete(const gchar *vm_name, GError **error)
{
    if (!G.initialized) return TRUE;

    g_mutex_lock(&G.mu);
    IscsiTarget *t = _find_target(vm_name);
    if (!t) {
        g_mutex_unlock(&G.mu);
        return TRUE;  /* idempotent */
    }

    gchar *cmd = g_strdup_printf(
        "tgtadm --lld iscsi --op delete --mode target --tid %d --force", t->tid);
    gboolean del_ok = _run(cmd, NULL, error);
    g_free(cmd);

    if (!del_ok) {
        /* M-3: 이전에는 _run 반환값을 무시하고 무조건 TRUE 를 반환 → tgtadm 삭제가
         * 실패(타겟 busy 등)해도 성공으로 보고. 이제 실제 결과를 반영한다. 인메모리
         * 레코드는 그대로 두어(swap-remove 안 함) 재시도 가능하게 하고, tgtd 에 타겟이
         * 잔존할 수 있음을 FALSE 로 호출자에 알린다. */
        PCV_LOG_WARN(ISCSI_LOG_DOM,
                     "tgtadm target delete failed (tid=%d, vm=%s) — may persist in tgtd",
                     t->tid, vm_name);
        g_mutex_unlock(&G.mu);
        return FALSE;
    }

    g_free(t->vm_name);
    /* swap-remove: 삭제된 슬롯에 배열 마지막 요소를 복사하여 빈 공간 제거
     * 순서 보장이 불필요하므로 O(1) 삭제가 가능합니다 */
    gint idx = (gint)(t - G.targets);
    if (idx < G.count - 1)
        G.targets[idx] = G.targets[G.count - 1];
    G.count--;

    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI target deleted: %s", vm_name);
    return TRUE;
}

/**
 * pcv_iscsi_target_list:
 * 등록된 모든 iSCSI 타겟을 JSON 배열로 반환합니다.
 * 각 요소: {"vm_name": "...", "tid": N, "iqn": "iqn.2026-03.purecvisor:..."}
 *
 * @return JsonArray* — 호출자가 소유. 빈 배열일 수 있음.
 */
JsonArray *
pcv_iscsi_target_list(void)
{
    JsonArray *arr = json_array_new();
    if (!G.initialized) return arr;

    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.count; i++) {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm_name", G.targets[i].vm_name);
        json_object_set_int_member(obj, "tid", G.targets[i].tid);
        gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, G.targets[i].vm_name);
        json_object_set_string_member(obj, "iqn", iqn);
        g_free(iqn);
        json_array_add_object_element(arr, obj);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

/* ── CHAP 인증 관리 ──────────────────────────────────────────────── */

/**
 * pcv_iscsi_target_set_chap:
 * 기존 iSCSI 타겟에 CHAP 인증을 동적으로 설정/변경합니다.
 *
 * tgtadm 호출 2단계:
 *   1. 계정 생성: tgtadm --op new --mode account (이미 존재하면 soft-fail)
 *   2. 계정 바인딩: tgtadm --op bind --mode account --tid <tid>
 *
 * @param vm_name        VM 이름 — 타겟 ID 조회에 사용
 * @param chap_user      CHAP 사용자명
 * @param chap_password  CHAP 비밀번호
 * @param error          실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 타겟 없음 또는 바인딩 실패
 */
gboolean
pcv_iscsi_target_set_chap(const gchar *vm_name, const gchar *chap_user,
                           const gchar *chap_password, GError **error)
{
    if (!G.initialized) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "iSCSI not initialized");
        return FALSE;
    }
    if (!vm_name || !chap_user || !chap_password ||
        !*chap_user || !*chap_password) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "CHAP user and password are required");
        return FALSE;
    }

    g_mutex_lock(&G.mu);
    IscsiTarget *t = _find_target(vm_name);
    if (!t) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "iSCSI target not found: %s", vm_name);
        return FALSE;
    }
    gint tid = t->tid;
    g_mutex_unlock(&G.mu);

    /* 1. Create account (soft-fail if already exists)
     * M-2류: _run(g_shell_parse_argv 재토큰화) 대신 argv 직접 전달 — chap_password 에
     * 공백/따옴표가 있어도 재분할되지 않고 단일 인자로 전달(STO-4). */
    const gchar *acc_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "new", "--mode", "account",
                               "--user", chap_user, "--password", chap_password, NULL};
    _run_argv(acc_argv, NULL, NULL);

    /* 2. Bind account to target */
    gchar *tid_str = g_strdup_printf("%d", tid);
    const gchar *bind_argv[] = {"tgtadm", "--lld", "iscsi", "--op", "bind", "--mode", "account",
                                "--tid", tid_str, "--user", chap_user, NULL};
    gboolean ok = _run_argv(bind_argv, NULL, error);
    g_free(tid_str);

    if (ok)
        PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP set for %s: user=%s tid=%d",
                     vm_name, chap_user, tid);
    return ok;
}

/* ── 이니시에이터 관리 (원격 노드에서 iSCSI 볼륨 마운트/언마운트) ──── */

/**
 * pcv_iscsi_initiator_connect:
 * 원격 iSCSI 타겟에 연결하고 로컬 블록 디바이스 경로를 반환합니다.
 *
 * 연결 단계 (4단계 iscsiadm 호출):
 *   1. Discovery: sendtargets로 타겟 검색 (이미 발견된 경우 soft-fail)
 *   2. CHAP 인증 설정 (daemon.conf에 설정된 경우)
 *   3. Login: 타겟에 로그인 → 커널이 /dev/sd* 디바이스를 자동 생성
 *   4. 디바이스 경로 탐색: /dev/disk/by-path/ 에서 디바이스 찾기
 *
 * @param target_ip    iSCSI 타겟 서버 IP (예: "192.0.2.19")
 * @param vm_name      VM 이름 — IQN suffix로 사용하여 타겟 식별
 * @param device_path  (out) 연결된 블록 디바이스 경로 (NULL 가능 — 불필요 시)
 *                     호출자가 g_free()로 해제
 * @param error        실패 시 GError** 설정
 * @return TRUE: 로그인 성공, FALSE: 실패
 */
gboolean
pcv_iscsi_initiator_connect(const gchar *target_ip, const gchar *vm_name,
                             gchar **device_path, GError **error)
{
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);

    /* M-2: 전 명령을 argv 직접 실행(_run_argv)으로 전환 — 셸 부재로 리터럴화되던
     * 리다이렉트(2>/dev/null·2>&1)를 제거하고, target_ip/chap 인자를 g_shell
     * 재토큰화 없이 단일 argv 원소로 전달(인젝션 표면 제거). */

    /* 1. Discovery (soft-fail if already discovered) */
    const gchar *disc[] = { "iscsiadm", "-m", "discovery", "-t", "sendtargets",
                            "-p", target_ip, NULL };
    _run_argv(disc, NULL, NULL);

    /* 2. CHAP authentication setup (if configured in daemon.conf) */
    {
        const gchar *chap_user = pcv_config_get_string("iscsi", "chap_user", NULL);
        gchar *chap_pass = pcv_config_get_secret("iscsi", "chap_password", NULL);
        if (chap_user && chap_pass && *chap_user && *chap_pass) {
            const gchar *a_method[] = { "iscsiadm", "-m", "node", "-T", iqn, "-p", target_ip,
                "--op=update", "-n", "node.session.auth.authmethod", "-v", "CHAP", NULL };
            _run_argv(a_method, NULL, NULL);

            const gchar *a_user[] = { "iscsiadm", "-m", "node", "-T", iqn, "-p", target_ip,
                "--op=update", "-n", "node.session.auth.username", "-v", chap_user, NULL };
            _run_argv(a_user, NULL, NULL);

            const gchar *a_pass[] = { "iscsiadm", "-m", "node", "-T", iqn, "-p", target_ip,
                "--op=update", "-n", "node.session.auth.password", "-v", chap_pass, NULL };
            _run_argv(a_pass, NULL, NULL);

            PCV_LOG_INFO(ISCSI_LOG_DOM, "CHAP auth configured for %s@%s", chap_user, target_ip);
        }
        g_free(chap_pass);
    }

    /* 3. Login (에러는 error 파라미터로 캡처 — 2>&1 불필요) */
    const gchar *login[] = { "iscsiadm", "-m", "node", "--targetname", iqn,
                             "--portal", target_ip, "--login", NULL };
    if (!_run_argv(login, NULL, error)) {
        g_free(iqn);
        return FALSE;
    }

    /* 4. Find device path — 셸 glob/pipe 대신 /dev/disk/by-path 를 C 로 스캔 */
    if (device_path) {
        *device_path = _find_iscsi_device(target_ip);
    }

    PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI initiator connected: %s@%s", iqn, target_ip);
    g_free(iqn);
    return TRUE;
}

/**
 * pcv_iscsi_initiator_disconnect:
 * 원격 iSCSI 타겟에서 로그아웃합니다.
 * 로그아웃 후 커널이 /dev/sd* 디바이스를 자동 제거합니다.
 *
 * @param target_ip iSCSI 타겟 서버 IP
 * @param vm_name   VM 이름 — IQN suffix로 타겟 식별
 * @param error     실패 시 GError** 설정
 * @return TRUE: 로그아웃 성공, FALSE: 실패
 */
gboolean
pcv_iscsi_initiator_disconnect(const gchar *target_ip, const gchar *vm_name,
                                GError **error)
{
    gchar *iqn = g_strdup_printf("%s:%s", ISCSI_IQN_PFX, vm_name);
    /* M-2: argv 직접 실행, 2>&1 제거(에러는 error 파라미터로 캡처) */
    const gchar *logout[] = { "iscsiadm", "-m", "node", "--targetname", iqn,
                              "--portal", target_ip, "--logout", NULL };
    gboolean ok = _run_argv(logout, NULL, error);
    g_free(iqn);

    if (ok)
        PCV_LOG_INFO(ISCSI_LOG_DOM, "iSCSI initiator disconnected: %s@%s", vm_name, target_ip);
    return ok;
}
