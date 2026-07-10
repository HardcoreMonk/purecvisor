#ifndef PURECVISOR_HOT_RELOAD_H
#define PURECVISOR_HOT_RELOAD_H

/**
 * @file hot_reload.h
 * @brief 제로 다운타임 업그레이드 — SIGUSR2 + drain + execve
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   현재 에디션 데몬의 무중단 바이너리 교체(hot-upgrade) 인터페이스.
 *   systemd 서비스를 재시작하지 않고, 프로세스 내에서 새 바이너리로
 *   전환(execve)하여 클라이언트 연결을 끊지 않는 것이 목표이다.
 *
 * [동작 흐름 — 상태 머신]
 *   1. IDLE        : 정상 운영 상태. SIGUSR2 시그널 대기 중.
 *   2. DRAINING    : SIGUSR2 수신 또는 pcv_hot_reload_prepare() 호출 →
 *                    drain.c의 그레이스풀 드레인 시작 →
 *                    진행 중인 모든 RPC(inflight) 완료를 대기한다.
 *   3. READY       : inflight RPC가 0이 되어 안전하게 교체 가능한 상태.
 *   4. EXECUTING   : execve() 호출 직전. 이 상태에서 프로세스 이미지가
 *                    새 바이너리로 교체된다. 성공 시 이 코드로 복귀하지 않음.
 *
 * [UDS 소켓 FD 전달]
 *   execve() 호출 전, 현재 리스닝 중인 UDS 소켓의 파일 디스크립터를
 *   환경변수(PCV_LISTEN_FD)로 전달한다. 새 프로세스는 이 FD를 상속받아
 *   기존 소켓을 그대로 사용하므로 클라이언트 재접속이 불필요하다.
 *   이는 systemd Socket Activation과 유사한 원리이다.
 *
 * [실패 시 안전장치]
 *   - execve() 실패 시 상태를 IDLE로 롤백하고 기존 프로세스가 계속 동작
 *   - drain 타임아웃(30초) 초과 시 강제 진행 또는 롤백 (drain.c 참조)
 *
 * [의존 모듈]
 *   - drain.c/drain.h      : inflight RPC 카운터 관리 및 그레이스풀 드레인
 *   - uds_server.c         : UDS 소켓 FD 제공
 *   - pcv_config.c         : 바이너리 경로 설정
 *
 * [사용 방법]
 *   - 시그널: kill -SIGUSR2 <edition_daemon_pid>
 *   - RPC:   daemon.upgrade.prepare 메서드 호출 (pcv_hot_reload_prepare)
 *   - CLI:   pcvctl upgrade prepare
 * ──────────────────────────────────────────────────────────────
 */

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief 핫 리로드 서브시스템 초기화 — SIGUSR2 시그널 핸들러를 등록한다.
 *
 * 데몬 기동 시 1회 호출된다. 내부적으로 g_unix_signal_add()를 사용하여
 * GMainLoop 이벤트 루프에 SIGUSR2 핸들러를 등록한다.
 *
 * @param binary_path  새 바이너리 경로
 *                     (예: "/usr/local/bin/purecvisorsd").
 *                     execve() 시 이 경로의 바이너리가 실행된다.
 *                     NULL이면 /proc/self/exe를 사용한다.
 * @param uds_listen_fd 현재 리스닝 중인 UDS 소켓의 파일 디스크립터.
 *                      execve() 시 이 FD를 환경변수로 전달하여
 *                      새 프로세스가 동일한 소켓을 상속받도록 한다.
 */
void pcv_hot_reload_init(const gchar *binary_path, int uds_listen_fd);

/**
 * @brief 업그레이드 상태 머신의 상태 열거형
 *
 * 상태 전이: IDLE → DRAINING → READY → EXECUTING → (새 프로세스)
 *            IDLE ← (실패 시 롤백)
 */
typedef enum {
    PCV_UPGRADE_IDLE,       /* 정상 운영 중 — 업그레이드 요청 대기 */
    PCV_UPGRADE_DRAINING,   /* 드레인 진행 중 — 새 RPC 수신 거부, inflight 완료 대기 */
    PCV_UPGRADE_READY,      /* 드레인 완료 — execve() 호출 가능 상태 */
    PCV_UPGRADE_EXECUTING   /* execve() 호출 직전 — 이후 프로세스 이미지 교체 */
} PcvUpgradeState;

/**
 * @brief 현재 업그레이드 상태를 반환한다.
 *
 * 스레드 안전: 내부적으로 atomic 읽기를 사용하므로 어디서든 호출 가능.
 * 모니터링, /health 프로브, RPC 응답에서 현재 상태를 확인할 때 사용한다.
 *
 * @return 현재 PcvUpgradeState 값
 */
PcvUpgradeState pcv_hot_reload_get_state(void);

/**
 * @brief 수동으로 업그레이드를 트리거한다 (daemon.upgrade.prepare RPC용).
 *
 * SIGUSR2를 보내지 않고 RPC 또는 CLI로 업그레이드를 시작할 때 사용한다.
 * 내부적으로 SIGUSR2 핸들러와 동일한 로직을 수행한다:
 *   1. 상태를 DRAINING으로 전환
 *   2. drain 시작
 *   3. inflight 완료 후 execve()
 *
 * @param error  실패 시 GError가 설정된다. 호출자가 g_error_free()로 해제.
 *               이미 DRAINING/EXECUTING 상태이면 에러를 반환한다.
 * @return TRUE: 업그레이드 프로세스 시작 성공, FALSE: 실패(이미 진행 중 등)
 */
gboolean pcv_hot_reload_prepare(GError **error);

/**
 * @brief 현재 데몬의 버전 문자열을 반환한다.
 *
 * 빌드 시 컴파일 상수로 결정되는 버전 정보를 반환한다.
 * /health 엔드포인트, 시작 배너, 업그레이드 전후 버전 비교에 사용.
 *
 * @return 버전 문자열 (정적 메모리, 해제 불필요). 예: "1.0"
 */
const gchar *pcv_hot_reload_get_version(void);

G_END_DECLS

#endif /* PURECVISOR_HOT_RELOAD_H */
