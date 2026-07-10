/**
 * @file virt_events.h
 * @brief Libvirt Lifecycle Event Listener & Self-Healing Daemon 공개 헤더
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  파일 역할
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   VM이 OOM Kill, 호스트 장애, virsh destroy 등으로 비정상 종료되었을 때
 *   이를 즉각 감지하고, CPU Allocator에 묶인 물리 코어를 강제 회수(Rollback)하는
 *   자가 치유(Self-Healing) 데몬의 공개 인터페이스입니다.
 *
 *   이 기능이 없으면, 비정상 종료된 VM이 사용하던 격리 코어가
 *   영원히 "사용 중" 상태로 남아 다른 VM에 할당할 수 없게 됩니다.
 *   (리소스 누수 → 시간이 지나면 격리 코어가 고갈되어 새 VM 생성 불가)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  아키텍처 위치 및 이벤트 흐름
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   main.c에서 init_virt_events_daemon()을 1회 호출하면,
 *   전용 GThread("libvirt-events")가 생성되어 Libvirt 이벤트 루프를 무한 펌핑합니다.
 *
 *   이벤트 흐름:
 *     [Libvirt 이벤트 스레드]
 *       VM STOPPED/CRASHED 이벤트 발생
 *         → domain_lifecycle_cb() 콜백 호출
 *            → UUID 복사 (g_strdup)
 *               → g_main_context_invoke()로 메인 스레드에 작업 위임
 *
 *     [메인 이벤트 루프 스레드]
 *       handle_vm_death_in_main_thread() 실행
 *         → cpu_allocator_free_vm_cores() 호출 (격리 코어 해제)
 *         → g_free(uuid) (메모리 정리)
 *
 *   핵심: 이벤트 스레드에서 직접 자원 관리자를 건드리지 않고,
 *   g_main_context_invoke()를 통해 메인 스레드로 안전하게 위임한다.
 *   이렇게 하면 Mutex 없이도 Thread Safety가 보장된다.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  감시하는 Libvirt 이벤트 종류
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   VIR_DOMAIN_EVENT_STOPPED   — 정상 종료 (virsh shutdown/destroy)
 *   VIR_DOMAIN_EVENT_CRASHED   — 비정상 종료 (SEGV, OOM Kill 등)
 *   VIR_DOMAIN_EVENT_STARTED   — VM 시작 (로그 기록 전용, 자원 회수 없음)
 *   VIR_DOMAIN_EVENT_SHUTDOWN  — 게스트 OS 종료 시그널 (로그 기록)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  사용법
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   데몬 시작 시 1회 호출:
 *     init_virt_events_daemon();
 *   이후 GMainLoop가 실행되면 자동으로 이벤트를 감시합니다.
 *   별도의 종료(shutdown) 함수는 없습니다 (프로세스 종료 시 스레드 자동 정리).
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  주의사항
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - 이 함수는 내부적으로 qemu:///system에 영구 커넥션을 맺으므로
 *     libvirtd가 실행 중이어야 합니다.
 *   - libvirtd가 재시작되면 이벤트 수신이 중단됩니다.
 *     이 경우 현재 에디션 데몬을 재시작해야 이벤트 감시가 복구됩니다.
 *   - 스레드 생성 실패 시 g_critical 로그만 남기고 데몬은 계속 실행됩니다.
 *     (Self-Healing 없이도 기본 기능은 동작)
 */

#ifndef PURECVISOR_DAEMONS_VIRT_EVENTS_H
#define PURECVISOR_DAEMONS_VIRT_EVENTS_H

#include <glib.h>

/* C++ 호환성을 위한 Name Mangling 방지 매크로 */
G_BEGIN_DECLS

/* =========================================================
 * 데몬 생명주기 API
 * ========================================================= */

/**
 * @brief Libvirt Lifecycle 이벤트 감시 데몬을 기동합니다.
 *
 * 서버 부팅 시 main.c 에서 단 1회 호출되어야 합니다.
 * 호출 직후 GThread가 생성되며, 내부적으로 qemu:///system 에 대한
 * 영구적인 이벤트 수신 커넥션을 맺고 무한 루프를 펌핑합니다.
 * * 이벤트가 발생하면 g_main_context_invoke를 통해 메인 루프(Main Thread)로
 * 자원 회수 명령을 안전하게 넘겨주어 Thread-safe를 보장합니다.
 */
void init_virt_events_daemon(void);

G_END_DECLS

#endif /* PURECVISOR_DAEMONS_VIRT_EVENTS_H */
