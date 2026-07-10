/**
 * @file pcv_spawn.h
 * @brief GSubprocessLauncher 기반 외부 프로세스 실행 — 공개 헤더
 *
 * Sprint C-2(GIO P3)에서 도입, GIO P7에서 GSubprocessLauncher 싱글턴으로 통합.
 * PureCVisor 데몬이 외부 명령을 실행할 때 사용하는 공통 래퍼입니다.
 *
 * [왜 이 모듈이 필요한가?]
 *   1. Command Injection 방어: argv 배열 방식으로 셸 해석 없이 실행
 *   2. 환경 통일: 모든 자식 프로세스가 동일한 PATH/HOME/LANG/cwd로 실행
 *   3. 중앙 관리: 외부 명령 실행을 한 곳에서 로깅/제어 가능
 *
 * [함수 선택 기준]
 *   pcv_spawn_sync()  — 종료코드 + stdout/stderr 캡처 필요. 실패 시 GError 반환.
 *                        블로킹 함수 → GTask 워커 스레드에서만 호출!
 *                        사용처: handler_storage, zfs_driver, vm_manager,
 *                        network_manager, network_dhcp, lxc_driver, overlay_manager
 *
 *   pcv_spawn_fire()  — fire-and-forget. 결과 무시, 실패해도 에러 없이 반환.
 *                        non-블로킹 (프로세스 완료 대기 없음)
 *                        사용처: network_firewall (nft/sysctl), best-effort 명령
 *
 *   pcv_spawn_pipe_sync()
 *                     — producer stdout을 consumer stdin으로 직접 연결.
 *                        셸 파이프와 임시 파일 없이 대용량 스트림을 흘려보낼 때 사용.
 *                        사용처: zfs send | zfs recv 같은 복제 경로
 *
 * [초기화 순서]
 *   main.c: pcv_spawn_launcher_init() (seccomp 적용 직후, 외부 프로세스 호출 전)
 *         → pcv_spawn_sync/fire 사용 가능
 *         → pcv_spawn_launcher_shutdown() (g_main_loop_quit 이후)
 *
 * [주의사항]
 *   - pcv_spawn_sync()는 블로킹: GMainLoop 스레드가 아닌 GTask 워커에서만 호출
 *   - argv는 NULL-terminated 배열 (셸 해석 없음 — 명령 인젝션 방지)
 *   - launcher 미초기화 시 g_subprocess_newv() 폴백 동작 보장
 *   - 호출자가 stdout_out/stderr_out 문자열을 g_free()로 해제
 */

#ifndef PURECVISOR_SPAWN_H
#define PURECVISOR_SPAWN_H

#include <glib.h>

G_BEGIN_DECLS

/* =========================================================================
 * GIO P7: GSubprocessLauncher 통합 인스턴스 생명주기
 *
 * 모든 외부 프로세스 호출(pcv_spawn_sync / pcv_spawn_fire)은 이 싱글턴
 * GSubprocessLauncher를 통해 실행됩니다. 중앙에서 환경변수/cwd/
 * stdout/stderr 정책을 일관되게 관리합니다.
 *
 * 초기화 순서:
 *   1. main.c에서 root 체크 + seccomp 적용 직후 pcv_spawn_launcher_init()
 *   2. 이후 pcv_spawn_sync / pcv_spawn_fire 호출 가능
 *   3. 프로그램 종료 직전 pcv_spawn_launcher_shutdown()
 *
 * launcher가 초기화되지 않은 상태에서 sync/fire를 호출하면
 * GSubprocess 기본 경로로 폴백하여 동작은 유지됩니다 (defensive fallback).
 * 단, 환경변수/cwd 통일이 적용되지 않으므로 경고 로그가 출력됩니다.
 * ========================================================================= */

/**
 * pcv_spawn_launcher_init:
 * 전역 GSubprocessLauncher를 생성하고 공통 환경변수를 설정합니다.
 *   - PATH=/usr/sbin:/usr/bin:/sbin:/bin (시스템 유틸리티 검색 경로)
 *   - HOME=/root  (root로 실행 중)
 *   - LANG=C.UTF-8 (영문 에러 메시지 보장)
 *   - cwd: /  (상대경로 의존 방지)
 * main.c에서 반드시 1회만 호출해야 합니다 (이중 호출 시 경고 로그).
 */
void pcv_spawn_launcher_init(void);

/**
 * pcv_spawn_launcher_shutdown:
 * 전역 GSubprocessLauncher를 해제합니다.
 * main.c 종료 직전(g_main_loop_quit 이후)에 호출합니다.
 * 이후 spawn 호출 시 g_subprocess_newv() 폴백 경로로 동작합니다.
 */
void pcv_spawn_launcher_shutdown(void);

/**
 * pcv_spawn_sync:
 * @argv:       NULL로 끝나는 argv 배열 (셸 해석 없음).
 *              예: {"zfs", "list", "-H", NULL}
 * @stdout_out: (nullable) (out): stdout 캡처. 불필요하면 NULL (→ /dev/null).
 *              호출자가 g_free()로 해제.
 * @stderr_out: (nullable) (out): stderr 캡처. 불필요하면 NULL (→ /dev/null).
 *              호출자가 g_free()로 해제.
 * @error:      GError** (nullable)
 *
 * 자식 프로세스가 exit code 0으로 종료하면 TRUE.
 * 프로세스 실행 실패 또는 exit code != 0이면 FALSE + error 설정.
 *
 * [중요] 블로킹 함수: GTask 워커 스레드에서만 호출할 것!
 * GMainLoop 스레드에서 호출하면 모든 RPC/REST가 블로킹됩니다.
 */
gboolean pcv_spawn_sync(const gchar * const *argv,
                        gchar              **stdout_out,
                        gchar              **stderr_out,
                        GError             **error);

/**
 * pcv_spawn_sync_timeout:
 * @argv:       NULL-terminated argv (셸 해석 없음).
 * @stdout_out: (nullable)(out): stdout 캡처. 호출자가 g_free().
 * @stderr_out: (nullable)(out): stderr 캡처. 호출자가 g_free().
 * @timeout_sec: 자식 완료 대기 상한(초). 0 이면 무제한(기존 pcv_spawn_sync 동작).
 * @error:      GError** (nullable). 타임아웃 시 G_IO_ERROR_TIMED_OUT.
 *
 * pcv_spawn_sync 와 동일하나 timeout_sec>0 이면 초과 시 자식을 SIGKILL 하고
 * FALSE + G_IO_ERROR_TIMED_OUT 을 반환한다.
 * [중요] 블로킹 — GTask 워커 스레드에서만 호출.
 */
gboolean pcv_spawn_sync_timeout(const gchar * const *argv,
                                gchar **stdout_out, gchar **stderr_out,
                                guint timeout_sec, GError **error);

/**
 * pcv_spawn_sync_env:
 * @argv:       NULL로 끝나는 argv 배열 (셸 해석 없음).
 * @envp:       (nullable) NULL로 끝나는 "KEY=VALUE" 환경변수 배열.
 *              이 자식 프로세스에만 적용되며 데몬 전역 environ은 건드리지 않는다.
 *              NULL이면 pcv_spawn_sync()와 동일한 공통 환경만 적용.
 * @stdout_out: (nullable)(out): stdout 캡처. 호출자가 g_free().
 * @stderr_out: (nullable)(out): stderr 캡처. 호출자가 g_free().
 * @error:      GError** (nullable)
 *
 * pcv_spawn_sync()와 동일하나, 공유 싱글턴 launcher가 아니라 **호출 단위 전용**
 * GSubprocessLauncher를 새로 만들어 @envp 항목을 그 자식에만 얹는다. 따라서
 * AWS 자격증명 같은 민감한 값을 g_setenv()로 데몬 전역 environ에 주입하지 않고
 * 특정 자식에게만 전달할 수 있다 (전역 오염 + 이후 spawn 상속 + 동시 spawn race 제거).
 * 공통 환경(PATH/HOME/LANG/cwd)은 pcv_spawn_launcher_init()과 동일하게 세팅된다.
 * 공유 상태가 아니므로 뮤텍스가 필요 없다.
 *
 * [중요] 블로킹 — GTask 워커 스레드에서만 호출.
 */
gboolean pcv_spawn_sync_env(const gchar * const *argv,
                            const gchar * const *envp,
                            gchar              **stdout_out,
                            gchar              **stderr_out,
                            GError             **error);

/**
 * pcv_spawn_fire:
 * @argv: NULL로 끝나는 argv 배열.
 *        예: {"nft", "add", "table", "inet", "purecvisor", NULL}
 *
 * 프로세스를 시작하고 결과를 무시. 실패해도 에러 없이 반환.
 * nft/sysctl 등 "best-effort" 명령에 사용.
 * 프로세스 완료를 기다리지 않음 (non-블로킹).
 */
void pcv_spawn_fire(const gchar * const *argv);

/**
 * pcv_spawn_pipe_sync:
 * @producer_argv: NULL로 끝나는 producer argv. stdout이 consumer로 연결된다.
 * @consumer_argv: NULL로 끝나는 consumer argv. stdin으로 producer 출력을 받는다.
 * @consumer_stdout_out: (nullable) (out): consumer stdout 캡처. 불필요하면 NULL.
 * @combined_stderr_out: (nullable) (out): 양쪽 stderr를 합친 문자열. 불필요하면 NULL.
 * @error: GError** (nullable)
 *
 * producer stdout -> consumer stdin을 GLib stream splice로 연결한다.
 * 셸을 거치지 않으므로 `|`, `>`, `<` 같은 셸 문법이 해석되지 않는다.
 * 대용량 VM 디스크 스트림을 `/tmp` 파일이나 메모리 버퍼로 만들지 않고
 * 바로 전달하기 위한 API다.
 *
 * [중요] 블로킹 함수: GTask 워커 스레드에서만 호출할 것.
 */
gboolean pcv_spawn_pipe_sync(const gchar * const *producer_argv,
                             const gchar * const *consumer_argv,
                             gchar              **consumer_stdout_out,
                             gchar              **combined_stderr_out,
                             GError             **error);

G_END_DECLS

#endif /* PURECVISOR_SPAWN_H */
