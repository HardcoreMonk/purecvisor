/**
 * @file pcv_spawn.c
 * @brief GSubprocessLauncher 기반 외부 프로세스 실행 — 중앙 프로세스 스폰 헬퍼
 *
 * Sprint C-2(GIO P3)에서 도입, GIO P7에서 GSubprocessLauncher 통합 싱글턴으로 개선.
 * PureCVisor 데몬이 외부 명령(nft, zfs, ovs-vsctl, lxc-*, ssh 등)을 실행할 때
 * 사용하는 유일한 공통 인터페이스입니다.
 *
 * [아키텍처 위치]
 *   main.c → pcv_spawn_launcher_init() (seccomp 적용 직후)
 *     │
 *     ├─ handler_storage.c  → pcv_spawn_sync({"zfs","list",..}, &out, &err, &error)
 *     ├─ network_firewall.c → pcv_spawn_fire({"nft","add","table",..})
 *     ├─ zfs_driver.c       → pcv_spawn_sync({"zfs","send",..}, ...)
 *     ├─ lxc_driver.c       → pcv_spawn_sync({"lxc-info",..}, ...)
 *     ├─ vm_manager.c       → pcv_spawn_sync({"virt-install",..}, ...)
 *     ├─ network_bridge.c   → pcv_spawn_sync({"ip","link",..}, ...)
 *     ├─ overlay_manager.c  → pcv_spawn_sync({"ovs-vsctl",..}, ...)
 *     └─ 기타 모듈...
 *     │
 *   main.c → pcv_spawn_launcher_shutdown() (g_main_loop_quit 이후)
 *
 * [왜 system()/popen() 대신 이 모듈을 사용하는가?]
 *   1. Command Injection 방어:
 *      system("zfs list " + user_input) → 셸 인젝션 취약
 *      pcv_spawn_sync({"zfs","list",user_input,NULL}, ...) → argv 배열, 셸 해석 없음
 *
 *   2. 환경 통일:
 *      GSubprocessLauncher가 PATH, HOME, LANG, cwd를 고정하여
 *      자식 프로세스가 예측 가능한 환경에서 실행됩니다.
 *
 *   3. stdout/stderr 캡처:
 *      g_subprocess_communicate_utf8()로 자식 출력을 안전하게 수집합니다.
 *
 * [함수 선택 기준]
 *   pcv_spawn_sync():
 *     - 종료 코드 + stdout/stderr 캡처 필요
 *     - 실패 시 GError 반환
 *     - GTask 워커 스레드 내부에서 블로킹 호출 (GMainLoop 차단 금지!)
 *     - 사용처: zfs, virt-install, lxc-info, ovs-vsctl 등
 *
 *   pcv_spawn_fire():
 *     - fire-and-forget: 결과 무시, 실패해도 에러 없이 반환
 *     - nft/sysctl 등 "best-effort" 명령에 적합
 *     - 프로세스 완료를 기다리지 않음 (SIGCHLD는 GLib 내부 처리)
 *     - 사용처: nftables 규칙 추가/삭제, sysctl 파라미터 설정
 *
 *   pcv_spawn_pipe_sync():
 *     - producer stdout을 consumer stdin으로 연결
 *     - 셸 파이프나 임시 파일 없이 대용량 스트림을 전달
 *     - 사용처: zfs send → zfs recv
 *
 * [싱글턴 GSubprocessLauncher 패턴]
 *   g_launcher 전역 싱글턴이 모든 자식 프로세스의 환경을 통일합니다:
 *     - PATH=/usr/sbin:/usr/bin:/sbin:/bin (시스템 명령 경로)
 *     - HOME=/root (root 실행 환경)
 *     - LANG=C.UTF-8 (locale 일관성 — 영문 에러 메시지 보장)
 *     - cwd="/" (상대경로 의존 방지)
 *   launcher 미초기화 시 g_subprocess_newv() 폴백 (defensive fallback).
 *
 * [다른 모듈과의 관계]
 *   - pcv_validate.c  : 외부 명령 인수로 사용되는 값의 사전 검증
 *   - pcv_privdrop.c  : seccomp 필터가 fork/exec/clone syscall을 허용
 *   - pcv_log.c       : 실행 실패 시 경고 로그
 *
 * [주의사항]
 *   - pcv_spawn_sync()는 블로킹 함수: GMainLoop 스레드에서 직접 호출 금지
 *     반드시 GTask 워커 스레드(g_task_run_in_thread) 내에서 호출할 것
 *   - stdout_out/stderr_out가 NULL이면 해당 스트림을 /dev/null로 리다이렉트
 *   - argv는 NULL로 끝나는 배열 (셸 해석 없음 — 인젝션 방지)
 *   - GSubprocessLauncher의 flags는 매 호출마다 재설정됨 (이전 호출 잔류 방지)
 */

#include "pcv_spawn.h"
#include "pcv_log.h"

#include <gio/gio.h>
#include <signal.h>
#include <string.h>

/** SPAWN_LOG_DOM - 이 모듈의 로그 도메인. journalctl 필터링에 사용 */
#define SPAWN_LOG_DOM "pcv_spawn"

/* -------------------------------------------------------------------------
 * [GIO P7] 전역 GSubprocessLauncher 싱글턴
 * -------------------------------------------------------------------------
 * NULL이면 아직 초기화되지 않은 상태 → sync/fire는 g_subprocess_newv()
 * 폴백 경로로 동작합니다. 이 방어적 설계로 launcher 초기화 전에도
 * spawn 함수가 호출되면 기능이 동작합니다 (경고 로그와 함께).
 */
static GSubprocessLauncher *g_launcher = NULL;
/* [R11] set_flags+spawnv 를 원자화 — 공유 g_launcher flags 를 동시 워커 spawn 이
 * 레이스하는 것을 방지. 정적 GMutex 는 zero-init 로 유효(g_mutex_init 불요). */
static GMutex g_spawn_launcher_mu;

/* =========================================================================
 * GIO P7: GSubprocessLauncher 생명주기
 * ========================================================================= */

/**
 * pcv_spawn_launcher_init - 전역 GSubprocessLauncher 생성 및 환경 설정
 *
 * main.c에서 seccomp 적용 직후, 외부 프로세스를 호출하기 전에 1회 호출.
 *
 * [설정되는 환경변수]
 *   PATH:  /usr/sbin:/usr/bin:/sbin:/bin
 *          → nft, zfs, ovs-vsctl 등 시스템 유틸리티 검색 경로
 *   HOME:  /root
 *          → root 사용자 홈 디렉터리 (SSH 키 등에 사용)
 *   LANG:  C.UTF-8
 *          → 에러 메시지가 영문으로 출력되어 로그 파싱에 일관성 보장
 *   cwd:   /
 *          → 상대 경로 의존을 방지하고 chroot 환경과의 일관성 유지
 *
 * [GSubprocessLauncher 장점]
 *   - 환경변수를 한 번 설정하면 모든 자식 프로세스에 자동 적용
 *   - GSubprocess 개별 생성보다 효율적 (환경 설정 캐시)
 *   - setenv() 대비 스레드 안전 (launcher 내부에서 관리)
 *
 * [이중 호출 방지]
 *   G_UNLIKELY 매크로로 이중 호출을 감지합니다.
 *   G_UNLIKELY는 컴파일러에게 "이 조건은 거의 발생하지 않는다"고
 *   힌트를 주어 정상 경로의 브랜치 예측을 최적화합니다.
 */
void
pcv_spawn_launcher_init(void)
{
    if (G_UNLIKELY(g_launcher != NULL)) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "pcv_spawn_launcher_init() called twice — ignored");
        return;
    }

    /*
     * G_SUBPROCESS_FLAGS_NONE: 기본 플래그 (stdout/stderr를 상속)
     * 실제 플래그는 _spawn_with_flags()에서 매 호출마다 override됩니다.
     * 여기서 설정한 플래그는 의미 없지만, 초기화 파라미터로 필수입니다.
     */
    g_launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);

    /* ------------------------------------------------------------------
     * 공통 환경변수 설정
     *
     * g_subprocess_launcher_setenv()의 마지막 파라미터 TRUE:
     *   기존 환경변수가 있어도 덮어씁니다 (override=TRUE).
     *   이렇게 하면 데몬의 상속된 환경과 무관하게
     *   자식 프로세스가 항상 동일한 환경에서 실행됩니다.
     * ------------------------------------------------------------------ */
    g_subprocess_launcher_setenv(g_launcher, "PATH",
        "/usr/sbin:/usr/bin:/sbin:/bin", TRUE);
    g_subprocess_launcher_setenv(g_launcher, "HOME",
        "/root", TRUE);
    g_subprocess_launcher_setenv(g_launcher, "LANG",
        "C.UTF-8", TRUE);

    /* ------------------------------------------------------------------
     * 작업 디렉터리: "/" 로 고정
     *
     * 자식 프로세스가 상대 경로를 사용해도 "/" 기준이므로
     * 데몬의 cwd 변경에 영향받지 않습니다.
     * ------------------------------------------------------------------ */
    g_subprocess_launcher_set_cwd(g_launcher, "/");

    PCV_LOG_INFO(SPAWN_LOG_DOM,
                 "GSubprocessLauncher initialized "
                 "(cwd=/, PATH=/usr/sbin:..., LANG=C.UTF-8)");
}

/**
 * pcv_spawn_launcher_shutdown - 전역 launcher 해제
 *
 * main 루프 종료 직후 호출합니다.
 * g_clear_object: NULL 체크 + g_object_unref + 포인터를 NULL로 설정.
 * 이후 spawn 호출 시 g_subprocess_newv() 폴백 경로로 동작합니다.
 */
void
pcv_spawn_launcher_shutdown(void)
{
    /* [B-1] _spawn_with_flags 의 launcher 사용과 같은 뮤텍스로 해제를 동기화.
     * 비동기화 시 TOCTOU: spawn 쪽 NULL 검사 통과 직후 여기서 unref 되면
     * 해제된 객체에 set_flags/spawnv (shutdown 창에서만 발생하는 레이스). */
    g_mutex_lock(&g_spawn_launcher_mu);
    gboolean had_launcher = (g_launcher != NULL);
    g_clear_object(&g_launcher);
    g_mutex_unlock(&g_spawn_launcher_mu);
    if (had_launcher)
        PCV_LOG_INFO(SPAWN_LOG_DOM, "GSubprocessLauncher shutdown.");
}

/* =========================================================================
 * 내부 헬퍼: launcher 경유 GSubprocess 생성
 * =========================================================================
 * launcher가 초기화된 경우: g_subprocess_launcher_spawnv() 사용
 * 미초기화 폴백:             g_subprocess_newv() 사용
 *
 * 두 경로 모두 동일한 GSubprocess*를 반환합니다.
 * 차이점: launcher 경로는 환경변수/cwd가 통일되고,
 *         폴백 경로는 현재 프로세스 환경을 그대로 상속합니다.
 */
static GSubprocess *
_spawn_with_flags(const gchar * const *argv,
                  GSubprocessFlags     flags,
                  GError             **error)
{
    /* [B-1] NULL 검사와 사용을 같은 임계영역으로 — shutdown 의 g_clear_object
     * 와의 TOCTOU 차단. 폴백 분기 판단도 락 안에서 확정한다. */
    g_mutex_lock(&g_spawn_launcher_mu);
    if (g_launcher != NULL) {
        /*
         * launcher의 stdout/stderr 플래그를 매 호출마다 덮어씁니다.
         *
         * [왜 매번 재설정하는가?]
         *   GSubprocessLauncher는 한번 설정된 플래그를 내부에 보존합니다.
         *   pcv_spawn_sync()는 STDOUT_PIPE|STDERR_PIPE를 사용하고,
         *   pcv_spawn_fire()는 STDOUT_SILENCE|STDERR_SILENCE를 사용합니다.
         *   재설정하지 않으면 이전 호출의 플래그가 잔류하여
         *   의도하지 않은 동작이 발생할 수 있습니다.
         */
        /* [R11] set_flags 는 공유 launcher 를 변이 → spawnv 까지 원자화해야 동시
         * 워커 spawn 이 서로의 flags 를 덮어쓰지 않는다. 임계영역은 fork 창뿐 —
         * spawnv 는 자식 실행을 기다리지 않고 즉시 반환(대기는 communicate 단계).
         * (락 획득은 [B-1]로 NULL 검사 이전으로 이동) */
        g_subprocess_launcher_set_flags(g_launcher, flags);
        GSubprocess *proc = g_subprocess_launcher_spawnv(g_launcher, argv, error);
        g_mutex_unlock(&g_spawn_launcher_mu);
        return proc;
    }
    g_mutex_unlock(&g_spawn_launcher_mu);

    /* 폴백 경로 — launcher 없이 직접 생성 (경고 로그) */
    PCV_LOG_WARN(SPAWN_LOG_DOM,
                 "launcher not initialized, falling back to g_subprocess_newv");
    return g_subprocess_newv(argv, flags, error);
}

/* =========================================================================
 * 공개 API
 * ========================================================================= */

/**
 * pcv_spawn_sync - 동기식 자식 프로세스 실행 (블로킹)
 * @argv:       NULL로 끝나는 argv 배열 (셸 해석 없음, 인젝션 방지)
 *              예: (const gchar*[]){"zfs", "list", "-H", NULL}
 * @stdout_out: (nullable, out) stdout 캡처 결과. 불필요하면 NULL → /dev/null
 * @stderr_out: (nullable, out) stderr 캡처 결과. 불필요하면 NULL → /dev/null
 * @error:      (nullable, out) GError** — 프로세스 실행 실패 또는 exit != 0
 *
 * @return: 자식 프로세스가 exit code 0으로 종료하면 TRUE.
 *          프로세스 실행 실패 또는 exit code != 0이면 FALSE + error 설정.
 *
 * [동작 흐름]
 *   1. stdout/stderr 캡처 여부에 따라 GSubprocessFlags 결정
 *      NULL인 파라미터는 SILENCE (→ /dev/null)
 *   2. _spawn_with_flags()로 자식 프로세스 생성
 *   3. g_subprocess_communicate_utf8()로 블로킹 대기 + 출력 수집
 *   4. 종료 코드 확인: 0이 아니면 stderr 내용으로 GError 설정
 *
 * [블로킹 주의]
 *   이 함수는 자식 프로세스가 종료될 때까지 블로킹됩니다.
 *   GMainLoop 스레드(메인 이벤트 루프)에서 직접 호출하면
 *   모든 RPC/REST 요청이 블로킹되므로 반드시 GTask 워커 스레드에서
 *   호출해야 합니다.
 *
 *   올바른 사용 패턴:
 *     void my_handler(...) {
 *         GTask *task = g_task_new(NULL, NULL, NULL, NULL);
 *         g_task_run_in_thread(task, my_worker);  // 워커 스레드에서 실행
 *         g_object_unref(task);
 *     }
 *     void my_worker(GTask *task, ...) {
 *         pcv_spawn_sync(argv, &out, &err, &error);  // 여기서 블로킹 OK
 *     }
 *
 * [에러 메시지]
 *   stderr 캡처가 활성화된 경우, 에러 메시지에 자식 프로세스의
 *   stderr 출력을 포함합니다. 이를 통해 "왜 실패했는지"를
 *   로그에서 확인할 수 있습니다.
 */
/* [R5] 종료코드 판정 — 성공이면 TRUE, 아니면 stderr 로 GError 생성 후 FALSE.
 * 기존 pcv_spawn_sync 의 판정 로직을 두 경로가 공유하도록 추출. */
static gboolean
_spawn_judge_exit(GSubprocess *proc, const gchar *arg0,
                  gchar *captured_stderr, GError **error)
{
    if (g_subprocess_get_successful(proc))
        return TRUE;
    if (error && !*error) {
        const gchar *msg = (captured_stderr && *captured_stderr)
                           ? g_strstrip(captured_stderr)
                           : "process exited with non-zero status";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", arg0, msg);
    }
    return FALSE;
}

/* [R5] communicate_utf8_async 완료 — GAsyncResult 붙잡고 루프 탈출 신호 */
typedef struct {
    gboolean      done;
    gboolean      timed_out;
    GAsyncResult *res;   /* ref 보관, finish 에서 사용 */
} SpawnWaitState;

static void
_spawn_communicate_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    SpawnWaitState *st = user_data;
    st->res  = g_object_ref(res);
    st->done = TRUE;
}

typedef struct {
    GSubprocess    *proc;
    GCancellable   *cancel;
    SpawnWaitState *st;
} SpawnTimeoutCtx;

static gboolean
_spawn_timeout_cb(gpointer user_data)
{
    SpawnTimeoutCtx *tc = user_data;
    tc->st->timed_out = TRUE;
    g_cancellable_cancel(tc->cancel);    /* async communicate 취소 → done 콜백 유도 */
    g_subprocess_force_exit(tc->proc);   /* hung child SIGKILL → fd/락 해제 */
    return G_SOURCE_REMOVE;              /* 1회성 */
}

gboolean
pcv_spawn_sync_timeout(const gchar * const *argv,
                       gchar              **stdout_out,
                       gchar              **stderr_out,
                       guint                timeout_sec,
                       GError             **error)
{
    g_return_val_if_fail(argv && argv[0], FALSE);

    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_NONE;
    if (stdout_out) flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDOUT_SILENCE;
    if (stderr_out) flags |= G_SUBPROCESS_FLAGS_STDERR_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDERR_SILENCE;

    GSubprocess *proc = _spawn_with_flags(argv, flags, error);
    if (!proc)
        return FALSE;

    /* timeout_sec == 0: 기존 동기 무제한 경로 (234 호출처 바이트 동일 회귀) */
    if (timeout_sec == 0) {
        gchar *captured_out = NULL, *captured_err = NULL;
        if (!g_subprocess_communicate_utf8(proc, NULL, NULL,
                                           stdout_out ? &captured_out : NULL,
                                           stderr_out ? &captured_err : NULL,
                                           error)) {
            g_object_unref(proc);
            return FALSE;
        }
        if (stdout_out) *stdout_out = captured_out;
        if (stderr_out) *stderr_out = captured_err;
        gboolean ok = _spawn_judge_exit(proc, argv[0],
                                        stderr_out ? *stderr_out : NULL, error);
        g_object_unref(proc);
        return ok;
    }

    /* timeout_sec > 0: private context + async communicate + 타이머 */
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GCancellable *cancel = g_cancellable_new();
    SpawnWaitState st = { FALSE, FALSE, NULL };

    g_subprocess_communicate_utf8_async(proc, NULL, cancel,
                                        _spawn_communicate_done, &st);

    SpawnTimeoutCtx tc = { proc, cancel, &st };
    GSource *tsrc = g_timeout_source_new_seconds(timeout_sec);
    g_source_set_callback(tsrc, _spawn_timeout_cb, &tc, NULL);
    g_source_attach(tsrc, ctx);

    while (!st.done)
        g_main_context_iteration(ctx, TRUE);

    gchar *captured_out = NULL, *captured_err = NULL;
    GError *fin_err = NULL;
    g_subprocess_communicate_utf8_finish(proc, st.res,
                                         stdout_out ? &captured_out : NULL,
                                         stderr_out ? &captured_err : NULL,
                                         &fin_err);
    g_clear_object(&st.res);
    g_source_destroy(tsrc);
    g_source_unref(tsrc);
    g_object_unref(cancel);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);

    if (st.timed_out) {
        g_free(captured_out);
        g_free(captured_err);
        g_clear_error(&fin_err);
        g_object_unref(proc);
        PCV_LOG_WARN(SPAWN_LOG_DOM, "'%s' %us 타임아웃 — 자식 SIGKILL", argv[0], timeout_sec);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "%s: timed out after %us", argv[0], timeout_sec);
        return FALSE;
    }

    if (fin_err) {
        g_free(captured_out);
        g_free(captured_err);
        g_object_unref(proc);
        g_propagate_error(error, fin_err);
        return FALSE;
    }

    if (stdout_out) *stdout_out = captured_out;
    if (stderr_out) *stderr_out = captured_err;
    gboolean ok = _spawn_judge_exit(proc, argv[0],
                                    stderr_out ? *stderr_out : NULL, error);
    g_object_unref(proc);
    return ok;
}

gboolean
pcv_spawn_sync(const gchar * const *argv,
               gchar              **stdout_out,
               gchar              **stderr_out,
               GError             **error)
{
    return pcv_spawn_sync_timeout(argv, stdout_out, stderr_out, 0, error);
}

/**
 * pcv_spawn_sync_env - envp를 이 자식에만 전달하는 동기 실행 (블로킹)
 *
 * 공유 싱글턴 g_launcher를 쓰지 않고 호출 단위 전용 GSubprocessLauncher를
 * 새로 만든다. 이렇게 하면 AWS 자격증명 같은 값을 g_setenv()로 데몬 전역
 * environ에 주입(→/proc/pid/environ 잔류 + 이후 spawn 상속 + 동시 spawn race)
 * 하지 않고, 이 자식 프로세스에만 안전하게 전달할 수 있다.
 *
 * 공통 환경(PATH/HOME/LANG/cwd)은 pcv_spawn_launcher_init()과 동일하게 세팅한 뒤,
 * 호출자가 넘긴 envp("KEY=VALUE") 항목을 g_subprocess_launcher_setenv()로 얹는다.
 * launcher는 공유 상태가 아니므로 뮤텍스가 필요 없으며, 호출 종료 시 unref한다.
 */
gboolean
pcv_spawn_sync_env(const gchar * const *argv,
                   const gchar * const *envp,
                   gchar              **stdout_out,
                   gchar              **stderr_out,
                   GError             **error)
{
    g_return_val_if_fail(argv && argv[0], FALSE);

    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_NONE;
    if (stdout_out) flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDOUT_SILENCE;
    if (stderr_out) flags |= G_SUBPROCESS_FLAGS_STDERR_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDERR_SILENCE;

    /* 호출 단위 전용 launcher — 공유 싱글턴이 아니므로 뮤텍스 불필요.
     * flags를 생성 시 확정하므로 set_flags 재설정도 필요 없다. */
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(flags);

    /* pcv_spawn_launcher_init()과 동일한 공통 환경 복제 (환경 통일) */
    g_subprocess_launcher_setenv(launcher, "PATH",
        "/usr/sbin:/usr/bin:/sbin:/bin", TRUE);
    g_subprocess_launcher_setenv(launcher, "HOME", "/root", TRUE);
    g_subprocess_launcher_setenv(launcher, "LANG", "C.UTF-8", TRUE);
    g_subprocess_launcher_set_cwd(launcher, "/");

    /* 호출자 envp("KEY=VALUE")를 이 자식에만 얹는다 (데몬 전역 environ 무변) */
    for (const gchar * const *e = envp; e && *e; e++) {
        const gchar *eq = strchr(*e, '=');
        if (!eq)
            continue;
        gchar *key = g_strndup(*e, (gsize)(eq - *e));
        g_subprocess_launcher_setenv(launcher, key, eq + 1, TRUE);
        g_free(key);
    }

    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, error);
    if (!proc) {
        g_object_unref(launcher);
        return FALSE;
    }

    gchar *captured_out = NULL, *captured_err = NULL;
    if (!g_subprocess_communicate_utf8(proc, NULL, NULL,
                                       stdout_out ? &captured_out : NULL,
                                       stderr_out ? &captured_err : NULL,
                                       error)) {
        g_object_unref(proc);
        g_object_unref(launcher);
        return FALSE;
    }
    if (stdout_out) *stdout_out = captured_out;
    if (stderr_out) *stderr_out = captured_err;

    gboolean ok = _spawn_judge_exit(proc, argv[0],
                                    stderr_out ? *stderr_out : NULL, error);
    g_object_unref(proc);
    g_object_unref(launcher);
    return ok;
}

static gchar *
_read_stream_to_string(GInputStream *stream)
{
    if (!stream)
        return g_strdup("");

    GString *buf = g_string_new(NULL);
    guint8 chunk[4096];
    GError *local_error = NULL;

    for (;;) {
        gssize n = g_input_stream_read(stream, chunk, sizeof(chunk),
                                       NULL, &local_error);
        if (n > 0) {
            g_string_append_len(buf, (const gchar *)chunk, n);
            continue;
        }
        if (n < 0) {
            PCV_LOG_WARN(SPAWN_LOG_DOM, "stream read failed: %s",
                         local_error ? local_error->message : "unknown");
            g_clear_error(&local_error);
        }
        break;
    }

    return g_string_free(buf, FALSE);
}

static gchar *
_combine_pipe_stderr(const gchar *producer_err, const gchar *consumer_err)
{
    gboolean has_producer = producer_err && *producer_err;
    gboolean has_consumer = consumer_err && *consumer_err;

    if (has_producer && has_consumer)
        return g_strdup_printf("producer: %s\nconsumer: %s",
                               producer_err, consumer_err);
    if (has_producer)
        return g_strdup(producer_err);
    if (has_consumer)
        return g_strdup(consumer_err);
    return g_strdup("");
}

/**
 * pcv_spawn_pipe_sync:
 * producer stdout을 consumer stdin으로 직접 연결합니다.
 *
 * [왜 필요한가]
 * `zfs send | zfs recv` 같은 작업은 스트림 크기가 VM 디스크 크기와 같다.
 * pcv_spawn_sync() stdout 캡처는 메모리에 모으고, 셸 리다이렉션은 command
 * injection 방어 원칙을 흐린다. 이 함수는 GLib stream splice로 bytes를
 * 바로 넘겨서 `/tmp` 임시 파일과 `/bin/sh -c`를 모두 피한다.
 */
gboolean
pcv_spawn_pipe_sync(const gchar * const *producer_argv,
                    const gchar * const *consumer_argv,
                    gchar              **consumer_stdout_out,
                    gchar              **combined_stderr_out,
                    GError             **error)
{
    g_return_val_if_fail(producer_argv && producer_argv[0], FALSE);
    g_return_val_if_fail(consumer_argv && consumer_argv[0], FALSE);

    if (consumer_stdout_out)
        *consumer_stdout_out = NULL;
    if (combined_stderr_out)
        *combined_stderr_out = NULL;

    GSubprocess *producer = NULL;
    GSubprocess *consumer = NULL;
    gchar *producer_err = NULL;
    gchar *consumer_err = NULL;
    gchar *consumer_out = NULL;
    gboolean ok = FALSE;

    producer = _spawn_with_flags(producer_argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error);
    if (!producer)
        goto cleanup;

    GSubprocessFlags consumer_flags =
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    consumer_flags |= consumer_stdout_out ? G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                          : G_SUBPROCESS_FLAGS_STDOUT_SILENCE;
    consumer = _spawn_with_flags(consumer_argv, consumer_flags, error);
    if (!consumer) {
        g_subprocess_force_exit(producer);
        (void)g_subprocess_wait(producer, NULL, NULL);
        goto cleanup;
    }

    GInputStream *producer_stdout = g_subprocess_get_stdout_pipe(producer);
    GOutputStream *consumer_stdin = g_subprocess_get_stdin_pipe(consumer);

    struct sigaction old_pipe_action;
    struct sigaction ignore_pipe_action = {0};
    ignore_pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe_action.sa_mask);
    gboolean restore_sigpipe =
        sigaction(SIGPIPE, &ignore_pipe_action, &old_pipe_action) == 0;

    gboolean spliced = g_output_stream_splice(
        consumer_stdin,
        producer_stdout,
        G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
        G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
        NULL,
        error);

    if (restore_sigpipe)
        sigaction(SIGPIPE, &old_pipe_action, NULL);

    if (!spliced) {
        g_subprocess_force_exit(producer);
        g_subprocess_force_exit(consumer);
        goto collect_output;
    }

collect_output:
    if (consumer_stdout_out)
        consumer_out = _read_stream_to_string(g_subprocess_get_stdout_pipe(consumer));
    producer_err = _read_stream_to_string(g_subprocess_get_stderr_pipe(producer));
    consumer_err = _read_stream_to_string(g_subprocess_get_stderr_pipe(consumer));

    GError *producer_wait_error = NULL;
    GError *consumer_wait_error = NULL;
    gboolean producer_ok = g_subprocess_wait_check(producer, NULL,
                                                   &producer_wait_error);
    gboolean consumer_ok = g_subprocess_wait_check(consumer, NULL,
                                                   &consumer_wait_error);

    ok = producer_ok && consumer_ok;
    if (!ok && error && !*error) {
        gchar *combined = _combine_pipe_stderr(producer_err, consumer_err);
        const gchar *detail = combined && *combined ? combined
                                                    : "pipeline exited with non-zero status";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s -> %s failed: %s",
                    producer_argv[0], consumer_argv[0], detail);
        g_free(combined);
    }

    g_clear_error(&producer_wait_error);
    g_clear_error(&consumer_wait_error);

cleanup:
    if (combined_stderr_out)
        *combined_stderr_out = _combine_pipe_stderr(producer_err, consumer_err);
    if (consumer_stdout_out)
        *consumer_stdout_out = g_steal_pointer(&consumer_out);

    g_free(producer_err);
    g_free(consumer_err);
    g_free(consumer_out);
    g_clear_object(&producer);
    g_clear_object(&consumer);
    return ok;
}

/**
 * pcv_spawn_fire - 비동기 fire-and-forget 프로세스 실행
 * @argv: NULL로 끝나는 argv 배열
 *        예: (const gchar*[]){"nft", "add", "table", "inet", "purecvisor", NULL}
 *
 * 프로세스를 시작하고 결과를 무시합니다.
 * 실패해도 에러 없이 반환합니다 (best-effort).
 *
 * [사용처]
 *   - nftables 규칙 추가/삭제: 방화벽 설정은 best-effort
 *   - sysctl 파라미터 설정: ip_forward, bridge-nf-call-iptables 등
 *   - 실패해도 데몬 동작에 영향 없는 보조 명령
 *
 * [동작 흐름]
 *   1. stdout/stderr를 /dev/null로 리다이렉트 (SILENCE)
 *   2. 프로세스 시작
 *   3. g_object_unref(proc)로 핸들 해제 — 프로세스 완료를 기다리지 않음
 *   4. SIGCHLD 시그널은 GLib GMainLoop가 내부적으로 처리
 *
 * [왜 프로세스 완료를 기다리지 않는가?]
 *   nft 규칙 추가 같은 명령은 수 밀리초에 완료됩니다.
 *   결과를 확인할 필요가 없으므로 기다리면 불필요한 지연만 발생합니다.
 *   GLib이 SIGCHLD를 처리하여 좀비 프로세스를 방지합니다.
 *
 * [에러 처리]
 *   프로세스 시작 자체가 실패하면 (실행 파일 미존재 등)
 *   WARN 로그만 출력하고 반환합니다. 데몬 동작에 영향 없습니다.
 */
void
pcv_spawn_fire(const gchar * const *argv)
{
    g_return_if_fail(argv && argv[0]);

    GError     *error = NULL;
    GSubprocess *proc = _spawn_with_flags(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error);

    if (!proc) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "fire failed [%s]: %s",
                     argv[0], error->message);
        g_error_free(error);
        return;
    }

    /* 프로세스 완료를 기다리지 않음 — 자식은 GLib 내부에서 SIGCHLD 처리 */
    g_object_unref(proc);
}
