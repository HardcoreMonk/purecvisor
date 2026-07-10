/**
 * @file hot_reload.c
 * @brief 제로 다운타임 업그레이드 — SIGUSR2 + drain + execve
 *
 * [동작 흐름]
 *   1. SIGUSR2 수신 (g_unix_signal_add)
 *   2. pcv_drain_begin() → 새 연결 거부
 *   3. inflight RPC 완료 대기 (최대 30초)
 *   4. UDS 소켓 FD를 환경변수 PCV_UPGRADE_FD로 전달
 *   5. execve(binary_path) → PID 유지하면서 바이너리 교체
 *
 * [실패 시]
 *   execve 실패하면 drain 해제하고 기존 서비스 재개.
 *   systemd Restart=on-failure로 추가 보호.
 */
#include "hot_reload.h"
#include "drain.h"
#include "purecvisor/version.h"
#include "utils/pcv_log.h"
#include <glib-unix.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#define HR_LOG_DOM "hot_reload"
#define PCV_VERSION PCV_PRODUCT_VERSION

/**
 * 모듈 전역 상태 — 핫 리로드 컨텍스트
 *
 * binary_path: execve()로 교체할 새 바이너리 경로
 * uds_fd:      UDS listen 소켓 FD (execve 후에도 유지되어 새 프로세스가 계승)
 * state:       현재 업그레이드 상태 머신 (IDLE → DRAINING → READY → EXECUTING)
 * mu:          state 필드 접근 보호용 뮤텍스 (멀티스레드 안전)
 */
static struct {
    gchar          *binary_path;
    int             uds_fd;
    PcvUpgradeState state;
    GMutex          mu;
} G = {0};

/**
 * 바이너리 교체 실행 — execve()로 현재 프로세스를 새 바이너리로 대체
 *
 * drain 완료 후 g_idle_add()로 스케줄되어 GMainLoop 유휴 시점에 실행됩니다.
 *
 * 동작 순서:
 *   1. UDS 소켓 FD의 FD_CLOEXEC 플래그 해제 (execve 후에도 FD 유지)
 *   2. 환경변수 PCV_UPGRADE_FD, LISTEN_FDS, LISTEN_PID 설정
 *   3. execv() 호출 — 성공 시 현재 코드는 더 이상 실행되지 않음
 *   4. execv() 실패 시: 상태를 IDLE로 복원하여 서비스 재개
 *
 * 디자인 결정:
 *   - PID가 유지되므로 systemd는 서비스 재시작으로 인식하지 않음
 *   - FD 계승으로 새 프로세스가 기존 소켓을 즉시 사용 가능 (무중단)
 *   - execv 실패 시 pcv_drain_cancel()로 즉시 서비스 재개
 *
 * @param user_data 미사용 (GSourceFunc 시그니처 충족용)
 * @return G_SOURCE_REMOVE — 1회성 idle 콜백이므로 항상 제거
 */
static gboolean
_do_upgrade(gpointer user_data __attribute__((unused)))
{
    g_mutex_lock(&G.mu);
    G.state = PCV_UPGRADE_EXECUTING;
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(HR_LOG_DOM, "Executing binary upgrade: %s", G.binary_path);

    /* UDS 소켓 FD를 환경변수로 전달 */
    if (G.uds_fd >= 0) {
        /* FD_CLOEXEC 해제 — execve 후에도 FD 유지 */
        int flags = fcntl(G.uds_fd, F_GETFD);
        if (flags >= 0)
            fcntl(G.uds_fd, F_SETFD, flags & ~FD_CLOEXEC);

        gchar fd_str[16];
        g_snprintf(fd_str, sizeof(fd_str), "%d", G.uds_fd);
        g_setenv("PCV_UPGRADE_FD", fd_str, TRUE);
        g_setenv("LISTEN_FDS", "1", TRUE);
        gchar pid_str[16];
        g_snprintf(pid_str, sizeof(pid_str), "%d", getpid());
        g_setenv("LISTEN_PID", pid_str, TRUE);
    }

    /* execve — 성공하면 이 줄 이후는 실행되지 않음 */
    gchar *argv[] = {G.binary_path, NULL};
    execv(G.binary_path, argv);

    /* execve 실패 시 도달 */
    PCV_LOG_WARN(HR_LOG_DOM, "execve failed: %s — resuming service", strerror(errno));
    g_mutex_lock(&G.mu);
    G.state = PCV_UPGRADE_IDLE;
    g_mutex_unlock(&G.mu);

    /* drain 해제하여 서비스 즉시 재개 */
    pcv_drain_cancel();

    return G_SOURCE_REMOVE;
}

/**
 * drain 완료 확인 타이머 — 100ms 간격으로 inflight RPC가 0인지 확인
 *
 * SIGUSR2 수신 후 pcv_drain_begin()으로 새 요청을 거부한 뒤,
 * 기존 진행 중인(inflight) RPC가 모두 완료될 때까지 폴링합니다.
 *
 * inflight == 0이 되면:
 *   1. 상태를 READY로 전환
 *   2. g_idle_add()로 _do_upgrade를 스케줄하여 바이너리 교체 실행
 *
 * inflight > 0이면: G_SOURCE_CONTINUE를 반환하여 100ms 후 재확인
 *
 * @param user_data 미사용
 * @return G_SOURCE_CONTINUE=계속 폴링, G_SOURCE_REMOVE=drain 완료
 */
static gboolean
_drain_check_timer(gpointer user_data __attribute__((unused)))
{
    if (!(pcv_drain_get_inflight() == 0)) {
        /* 아직 inflight RPC 남음 — 계속 대기 */
        return G_SOURCE_CONTINUE;
    }

    g_mutex_lock(&G.mu);
    G.state = PCV_UPGRADE_READY;
    g_mutex_unlock(&G.mu);

    PCV_LOG_INFO(HR_LOG_DOM, "Drain complete — proceeding with upgrade");
    g_idle_add(_do_upgrade, NULL);
    return G_SOURCE_REMOVE;
}

/**
 * SIGUSR2 시그널 핸들러 — 핫 리로드 시작 트리거
 *
 * 관리자가 `kill -USR2 <pid>` 또는 systemd reload 명령을 보내면 호출됩니다.
 * g_unix_signal_add()로 등록되어 GMainLoop 이벤트로 안전하게 처리됩니다
 * (실제 Unix 시그널 핸들러가 아니므로 async-signal-safe 제약 없음).
 *
 * 동작:
 *   1. 이미 업그레이드 진행 중이면 무시 (멱등성)
 *   2. 상태를 DRAINING으로 전환
 *   3. pcv_drain_begin()으로 새 연결 거부 시작
 *   4. 100ms 타이머로 drain 완료 폴링 시작
 *
 * @param user_data 미사용
 * @return G_SOURCE_CONTINUE — 시그널 핸들러를 계속 유지
 */
static gboolean
_on_sigusr2(gpointer user_data __attribute__((unused)))
{
    PCV_LOG_INFO(HR_LOG_DOM, "SIGUSR2 received — initiating hot reload");

    g_mutex_lock(&G.mu);
    if (G.state != PCV_UPGRADE_IDLE) {
        PCV_LOG_WARN(HR_LOG_DOM, "Upgrade already in progress (state=%d)", G.state);
        g_mutex_unlock(&G.mu);
        return G_SOURCE_CONTINUE;
    }
    G.state = PCV_UPGRADE_DRAINING;
    g_mutex_unlock(&G.mu);

    /* drain 시작 (NULL=기본 GMainLoop, 30초 타임아웃) */
    pcv_drain_begin(NULL, 30);

    /* 100ms마다 drain 완료 확인 */
    g_timeout_add(100, _drain_check_timer, NULL);

    return G_SOURCE_CONTINUE;
}

/**
 * 핫 리로드 모듈 초기화 — SIGUSR2 시그널 핸들러 등록
 *
 * main.c의 데몬 초기화 과정에서 호출됩니다.
 * 이 함수 호출 후부터 SIGUSR2로 무중단 바이너리 교체가 가능합니다.
 *
 * @param binary_path    교체할 바이너리 경로
 *                       (예: "/usr/local/bin/purecvisorsd")
 *                       g_strdup()으로 복사하여 보관합니다.
 * @param uds_listen_fd  UDS listen 소켓 FD. execve 시 새 프로세스에 계승됩니다.
 *                       -1이면 FD 계승을 수행하지 않습니다.
 */
void
pcv_hot_reload_init(const gchar *binary_path, int uds_listen_fd)
{
    g_mutex_init(&G.mu);
    G.binary_path = g_strdup(binary_path);
    G.uds_fd = uds_listen_fd;
    G.state = PCV_UPGRADE_IDLE;

    g_unix_signal_add(SIGUSR2, _on_sigusr2, NULL);

    PCV_LOG_INFO(HR_LOG_DOM, "Hot reload ready (binary=%s, uds_fd=%d)",
                 binary_path, uds_listen_fd);
}

/**
 * 현재 업그레이드 상태 조회 (스레드 안전)
 *
 * 상태 머신:
 *   IDLE      → 대기 중 (SIGUSR2 수신 가능)
 *   DRAINING  → 새 요청 거부 중, inflight RPC 완료 대기
 *   READY     → drain 완료, 바이너리 교체 직전
 *   EXECUTING → execve() 호출 중 (정상이면 이 상태에서 프로세스가 교체됨)
 *
 * @return 현재 PcvUpgradeState 값
 */
PcvUpgradeState
pcv_hot_reload_get_state(void)
{
    g_mutex_lock(&G.mu);
    PcvUpgradeState s = G.state;
    g_mutex_unlock(&G.mu);
    return s;
}

/**
 * 프로그래밍 방식으로 핫 리로드를 시작 — SIGUSR2 시뮬레이션
 *
 * REST API나 RPC 핸들러에서 핫 리로드를 트리거할 때 사용합니다.
 * 내부적으로 _on_sigusr2()를 직접 호출합니다.
 *
 * 이미 업그레이드가 진행 중이면 에러를 설정하고 FALSE를 반환합니다.
 *
 * @param error 에러 반환 (이미 진행 중일 때)
 * @return TRUE=시작 성공, FALSE=이미 진행 중
 */
gboolean
pcv_hot_reload_prepare(GError **error)
{
    g_mutex_lock(&G.mu);
    if (G.state != PCV_UPGRADE_IDLE) {
        g_mutex_unlock(&G.mu);
        g_set_error(error, g_quark_from_static_string("hot_reload"), 1,
                    "Upgrade already in progress");
        return FALSE;
    }
    g_mutex_unlock(&G.mu);

    /* SIGUSR2 시뮬레이션 */
    _on_sigusr2(NULL);
    return TRUE;
}

/**
 * 현재 바이너리 버전 문자열 반환
 *
 * 핫 리로드 전후로 버전을 비교하여 업그레이드 성공 여부를 확인할 때 사용합니다.
 *
 * @return 정적 버전 문자열 (예: "1.0") — 해제 불필요
 */
const gchar *
pcv_hot_reload_get_version(void)
{
    return PCV_VERSION;
}
