/**
 * @file main.c
 * @brief PureCVisor 에디션 데몬의 진입점 — 전체 서브시스템 오케스트레이션
 *
 * ====================================================================
 *  아키텍처에서의 위치
 * ====================================================================
 *  최상위 진입점 — 모든 서브시스템의 생성/시작/정지/해제를 총괄합니다.
 *  이 파일은 코드 로직을 직접 구현하지 않고, 각 모듈의 init/start/stop/cleanup
 *  함수를 올바른 순서로 호출하는 "오케스트레이터의 오케스트레이터"입니다.
 *
 *  다른 모듈과의 관계:
 *    - src/core/daemon.c: 레거시 코어 초기화 (pv_init/pv_run/pv_cleanup)
 *      → 현재 main.c가 직접 16단계 초기화를 수행하므로 daemon.c는 사용되지 않음
 *    - src/api/dispatcher.c: JSON-RPC 메서드 라우팅 (130개 메서드)
 *    - src/api/uds_server.c: Unix Domain Socket 서버 (JSON-RPC 2.0)
 *    - src/api/rest_server.c: HTTP REST API 서버 (libsoup3)
 *    - src/modules/virt/vm_manager.c: VM 생명주기 관리 (GObject 기반)
 *    - src/modules/core/vm_state.c: SQLite WAL 기반 VM 상태 머신
 *    - src/modules/core/cpu_allocator.c: NUMA 인식 CPU 코어 할당기
 *    - src/bootstrap/pcv_bootstrap_*.c: 에디션별 bootstrap 경계
 *
 * ====================================================================
 *  핵심 개념
 * ====================================================================
 *  PureCVisor는 fork 없이 단일 프로세스로 동작합니다. GMainLoop가 유일한
 *  이벤트 루프이며, 모든 I/O(UDS 소켓, REST HTTP, 타이머, UNIX 시그널)가
 *  이 루프에서 디스패치됩니다. REST 서버만 별도 GThread에서 자체 GMainLoop를
 *  돌리는데, 이는 동기 UDS 호출 시 데드락을 방지하기 위함입니다.
 *
 *  [왜 단일 프로세스인가?]
 *    멀티프로세스(fork) 모델은 상태 공유가 어렵고 IPC 오버헤드가 발생합니다.
 *    단일 프로세스 + 이벤트 루프 모델은 Redis, Nginx(워커 제외), Node.js와
 *    같은 아키텍처로, 공유 상태(인메모리 캐시, 커넥션 풀 등)에 대한
 *    동기화 비용이 최소화됩니다. 무거운 작업은 GTask 워커 스레드로 오프로드합니다.
 *
 * ====================================================================
 *  초기화 순서 (의존성 그래프 — 순서 변경 시 크래시 위험)
 * ====================================================================
 *   1.  root 권한 확인 (geteuid() == 0, libvirt/nftables/OVS에 필요)
 *   2.  로깅 + 설정 시스템 (다른 모든 모듈이 pcv_config를 참조)
 *   3.  libvirt-gobject 타입 시스템 (gvir_init_object — GObject 타입 등록)
 *   4.  코어 모듈: SQLite WAL 상태 머신, 커넥션 풀(max=8), CPU 할당기(NUMA)
 *   5.  보안: CAP_NET_ADMIN 등 최소 권한만 유지 + seccomp BPF 필터 적용
 *   6.  프로세스 스폰 런처 (pcv_spawn — dnsmasq, zfs, ovs-vsctl 등 외부 명령 실행)
 *   7.  libvirt 연결 + 디스패처(130개 RPC 라우터) + VM 매니저
 *   8.  텔레메트리 데몬 (백그라운드 1초 간격, virConnectGetAllDomainStats)
 *   9.  UDS 소켓 서버 (/var/run/purecvisor/daemon.sock, JSON-RPC 2.0)
 *  10.  JWT 초기화 + REST API 서버 (libsoup3, 포트 80/443, 별도 GThread)
 *  11.  에디션별 클러스터 bootstrap (Multi만 활성, Single은 stub 경계만 유지)
 *  12.  공용 네트워크/스토리지 초기화 + 에디션별 오버레이/SDN 자동화 경계
 *  13.  eBPF 심층 텔레메트리 (5초 간격, /proc 기반 호스트+VM 메트릭)
 *  14.  에디션별 스케줄러/프록시 bootstrap (Multi만 활성)
 *  15.  에디션별 오버레이 자동화 (Single은 로컬 브리지까지만, Multi는 피어 자동화 포함)
 *  16.  sd_notify READY=1 → GMainLoop 진입 (이벤트 대기)
 *
 * ====================================================================
 *  종료 흐름
 * ====================================================================
 *   SIGINT/SIGTERM → on_signal_received() → pcv_drain_begin(30초)
 *     → 새 요청 거부 + inflight RPC 완료 대기 + sd_notify STOPPING=1
 *     → GMainLoop 종료 → cleanup 블록에서 초기화 역순 자원 해제
 *
 * ====================================================================
 *  핵심 설계 패턴
 * ====================================================================
 *  - goto cleanup: 초기화 중 실패 시 이미 생성된 자원만 역순 해제
 *  - Graceful Degradation: 비필수 모듈(OVN, DPDK 등) 실패 시 경고만 출력하고 계속
 *  - Fire-and-Forget: GTask 비동기 작업은 응답 전송 후 백그라운드에서 실행
 *  - sd_notify: systemd Type=notify 서비스 프로토콜로 상태 통지
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - 초기화 중 하나라도 실패하면 goto cleanup으로 이미 생성된 자원만 해제합니다.
 *  - REST 서버는 반드시 UDS 서버 이후에 시작해야 합니다 (REST→UDS 브릿지 의존).
 *  - 클러스터 매니저는 REST 서버 이후에 시작합니다 (피어 통신에 REST 필요, Multi 전용).
 *  - OVN/OVS 코어는 에디션 공용이고, 자동화 경로만 클러스터 빌드에 링크됩니다.
 *  - global_allocator는 extern으로 다른 모듈에서 참조하므로 main 종료 전까지 유효해야 합니다.
 *
 * ====================================================================
 *  주석 독자 기준
 * ====================================================================
 *  이 코드베이스의 주석은 두 독자를 동시에 고려합니다.
 *    1. 주니어 개발자: 호출 순서, 소유권, 스레드, 실패 시 정리 경로를 보고
 *       바로 운영 코드 수정에 투입될 수 있어야 합니다.
 *    2. 비전공 운영자/기획자: "왜 이 검사를 하는가", "무엇을 보호하는가",
 *       "실패하면 사용자에게 어떤 영향이 있는가"를 큰 흐름으로 이해할 수
 *       있어야 합니다.
 *
 *  그래서 핵심 경로의 주석은 단순 번역이 아니라 의도와 위험을 함께 적습니다.
 *  예를 들어 "권한 확인"이라고만 쓰지 않고, "다른 사용자의 VM을 조작하지
 *  못하게 막는다"처럼 보호 대상을 명시합니다.
 */

/* ── 표준 라이브러리 ──────────────────────────────────────────── */
#include <unistd.h>     /* geteuid() — 현재 프로세스의 유효 사용자 ID 조회 */
#include <glib.h>       /* GLib 핵심: GMainLoop, g_message, g_free 등 */
#include <glib-unix.h>  /* g_unix_signal_add() — SIGINT/SIGTERM 핸들링 */
#include <libvirt-gobject/libvirt-gobject.h>  /* GObject 기반 libvirt 바인딩 */
#include <stdio.h>      /* fprintf, printf */
#include <libvirt/libvirt.h>  /* libvirt C API (Raw API) */

/* ── PureCVisor 내부 모듈 헤더 ────────────────────────────────── */
#include "api/uds_server.h"       /* Unix Domain Socket JSON-RPC 서버 */
#include "api/dispatcher.h"       /* RPC 메서드 라우터 (130개 메서드 분배) */
#include "api/rest_server.h"      /* HTTP REST API 서버 (libsoup3, 포트 80/443) */
#include "api/grpc_server.h"     /* gRPC 서버 (protobuf-c, 포트 50051) */
#include "modules/virt/vm_manager.h"  /* VM 생명주기 관리 (GObject 기반) */
#include "api/drain.h"            /* Graceful Drain: inflight RPC 카운터 + systemd STOPPING */
#include "utils/logger.h"         /* 로깅 초기화 */
#include "utils/pcv_log.h"        /* 구조화된 JSON 로깅 (PCV_LOG_INFO/WARN 등) */
#include "utils/pcv_config.h"     /* GKeyFile 기반 설정 시스템 (daemon.conf 파싱) */
#include "utils/pcv_privdrop.h"   /* 권한 격하: CAP_NET_ADMIN 등만 유지 + seccomp 적용 */
#include "utils/pcv_validate.h"   /* 입력값 검증 (VM 이름, CIDR, MAC 등) */

/* ── 코어 모듈 ────────────────────────────────────────────────── */
#include "modules/core/vm_state.h"      /* SQLite WAL 기반 VM 상태 머신 (원자적 전이) */
#include "modules/core/cpu_allocator.h" /* NUMA 인식 배타적 CPU 코어 할당기 */

/* ── 백그라운드 데몬 ──────────────────────────────────────────── */
#include "modules/daemons/telemetry.h"      /* 1초 간격 VM 메트릭 수집 (virConnectGetAllDomainStats) */
#include "modules/daemons/virt_events.h"    /* libvirt 도메인 이벤트 구독 (자가 치유) */
#include "modules/daemons/ebpf_telemetry.h" /* 5초 간격 심층 메트릭 (호스트 CPU/MEM + VM 디스크/네트워크 I/O) */
#include "modules/daemons/alert_engine.h"   /* WhaTap-style threshold alert + Webhook */
#include "modules/daemons/process_monitor.h" /* WhaTap-style process monitoring */
#include "modules/daemons/update_check.h"   /* 버전 알림 — GitHub 최신 릴리스 조회+캐시 */

/* ── 유틸리티 ─────────────────────────────────────────────────── */
#include "utils/pcv_spawn.h"      /* GSubprocessLauncher 기반 외부 프로세스 실행 */
#include "utils/pcv_worker_pool.h" /* 제한된 GThreadPool 워커 풀 (vm.clone/backup 등) */
#include "purecvisor/pcv_validate.h" /* 공개 헤더: pcv_network_rundir_init 등 */
#include "purecvisor/version.h"      /* PCV_PRODUCT_VERSION — 기동 배너 */
#include "utils/pcv_jwt.h"       /* JWT HS256 서명/검증 (REST API 인증) */

/* ── 가상화 서브시스템 ────────────────────────────────────────── */
#include "modules/virt/virt_conn_pool.h"  /* libvirt 연결 풀링 (max=8, 서킷 브레이커 내장) */
#include "modules/virt/cancellable_map.h" /* GCancellable 맵: 진행 중 비동기 작업 취소 지원 */

/* ── 클러스터 모듈 ────────────────────────────────────────────── */
#include "bootstrap/pcv_bootstrap.h"          /* 에디션별 bootstrap 경계 */

/* ── 네트워크/스토리지 모듈 ───────────────────────────────────── */
#include "modules/network/ovs_overlay.h"   /* 에디션 공용 OVS overlay 코어 + Multi 자동화 */
#include "modules/network/network_manager.h" /* QoS 재수화(pcv_qos_restore) 부팅 배선 */
#include "modules/network/ovn_manager.h"   /* 에디션 공용 OVN 코어 + Multi 자동화 */
#include "modules/storage/iscsi_manager.h" /* iSCSI 타겟/이니시에이터 (tgtadm/iscsiadm) */
#include "modules/network/dpdk_manager.h"  /* OVS-DPDK 커널 바이패스 (Phase 4) */
#include "modules/network/sriov_manager.h" /* SR-IOV VF/PCI passthrough (Phase 4) */
#include "modules/network/security_group.h" /* 보안 그룹 복원 (pcv_security_group_restore) */
#include "io/pcv_uring.h"                  /* io_uring 비동기 I/O (Phase U-1) */
#include "api/hot_reload.h"                /* 제로 다운타임 업그레이드 (영역 D) */
#include "api/ws_server.h"                 /* WebSocket 이벤트 스트림 (영역 A) */
#include "modules/storage/storage_tier.h"  /* 스토리지 티어링 (영역 E) */
#include "modules/daemons/prometheus_exporter.h" /* Prometheus 메트릭 (영역 G) */
#include "modules/audit/pcv_audit.h"       /* 감사 로그 (영역 J) */
#include "utils/pcv_job_queue.h"           /* 통합 작업 큐 */
#include "modules/accel/gpu_manager.h"     /* GPU Passthrough (영역 B) */
#include "modules/plugin/pcv_plugin_manager.h" /* 플러그인 시스템 (영역 H) */
#include "utils/pcv_tls.h"                 /* mTLS 인증 (영역 I) */
#include "modules/network/nfv_manager.h"   /* NFV LB/FW/Chain (영역 F) */
#include "modules/backup/backup_scheduler.h" /* ZFS 스냅샷 자동 백업 스케줄러 */
#include "modules/auth/pcv_rbac.h"           /* RBAC 멀티테넌트 인증 */
#include "modules/template/vm_template.h"    /* VM 템플릿 관리 */

/* ═══════════════════════════════════════════════════════════════════
 * 전역 변수
 *
 * [전역 변수 사용 원칙]
 *   PureCVisor는 단일 프로세스 아키텍처이므로 몇 가지 핵심 싱글턴만
 *   전역 변수로 관리합니다. 이 변수들은 main() 함수의 스택에서 관리할 수
 *   없는 수명(모든 모듈이 참조)을 가지므로 파일 스코프 static 또는
 *   extern으로 선언됩니다.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * GMainLoop — GLib 이벤트 루프 (프로그램의 심장)
 *
 * [GMainLoop의 역할]
 *   프로그램의 유일한 이벤트 디스패처입니다. 이벤트 소스(GSource)를
 *   감시하고, 이벤트 발생 시 등록된 콜백을 호출합니다.
 *   - fd 감시: UDS 소켓, io_uring eventfd
 *   - 타이머: 텔레메트리(1초), eBPF(5초), 백업(5분), etcd keepalive(3초)
 *   - UNIX 시그널: SIGINT, SIGTERM (g_unix_signal_add)
 *
 * [왜 static인가?]
 *   on_signal_received() 콜백에서 g_main_loop_quit()를 호출해야 하므로
 *   파일 스코프에서 접근 가능해야 합니다. 다른 파일에서는 접근하지 않으므로
 *   extern이 아닌 static입니다.
 *
 * [수명]
 *   main() 초기화 중 g_main_loop_new()로 생성 → g_main_loop_run()에서
 *   블로킹 → 종료 시그널 수신 후 g_main_loop_quit() → cleanup에서
 *   g_main_loop_unref()로 해제.
 */
static GMainLoop *loop;

/**
 * 글로벌 CPU 할당기 인스턴스 — NUMA 인식 배타적 코어 할당
 *
 * [extern 참조 패턴]
 *   vm_manager.c, handler_vm_start.c 등에서 `extern CpuAllocator *global_allocator;`로
 *   직접 참조합니다. 이는 함수 파라미터로 전달하기보다 전역 접근이 편리하기 때문입니다.
 *   main()의 cleanup 이전까지 항상 유효합니다.
 *
 * [NULL 안전성]
 *   NULL이면 할당기 미초기화 상태이며, 코어 피닝 없이 VM을 생성합니다 (graceful fallback).
 *   이는 격리 코어(isolcpus)가 설정되지 않은 개발 환경에서 유용합니다.
 *
 * [수명]
 *   main() 초기화 시 cpu_allocator_new() → scan_and_register_host_topology()
 *   → ... → cleanup 시 cpu_allocator_free() (현재 명시적 해제 없음, 프로세스 종료로 회수)
 */
CpuAllocator *global_allocator = NULL;

/* ═══════════════════════════════════════════════════════════════════
 * 시그널 핸들러
 *
 * [UNIX 시그널과 GMainLoop의 관계]
 *   g_unix_signal_add()는 UNIX 시그널을 GMainLoop의 GSource로 변환합니다.
 *   커널이 프로세스에 시그널을 전달하면, GLib가 내부 파이프(self-pipe trick)를
 *   통해 메인 루프에 알림을 전달하고, 다음 이벤트 루프 반복에서 콜백이 호출됩니다.
 *   이 방식은 시그널 핸들러 내에서 async-signal-unsafe 함수를 안전하게 호출할 수
 *   있게 합니다 (g_message, pcv_drain_begin 등은 async-signal-unsafe).
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief SIGINT(Ctrl+C) 또는 SIGTERM(systemctl stop) 수신 시 호출되는 콜백
 *
 * [Graceful Shutdown 시퀀스]
 *   1. pcv_drain_begin() 호출 — 새 RPC 요청 수신을 거부 상태로 전환
 *   2. 진행 중인 RPC(inflight)가 완료될 때까지 대기 (최대 drain_timeout초)
 *   3. systemd에 STOPPING 상태 알림 (sd_notify("STOPPING=1"))
 *   4. drain 타임아웃 경과 또는 모든 inflight 완료 시 g_main_loop_quit() 호출
 *   5. GMainLoop가 종료되면 main()의 cleanup 블록이 실행됨
 *
 * [왜 FALSE를 반환하는가?]
 *   GLib의 GSource 콜백 규약에서:
 *   - G_SOURCE_CONTINUE(TRUE): 소스를 유지하여 다음 이벤트도 처리
 *   - G_SOURCE_REMOVE(FALSE): 소스를 제거하여 콜백이 다시 호출되지 않음
 *   종료 시그널은 1회만 처리하면 되므로 FALSE를 반환합니다.
 *
 * @param user_data 사용하지 않음 (g_unix_signal_add 시 NULL 전달)
 * @return FALSE — 시그널 소스를 GMainLoop에서 제거 (1회성 핸들러)
 */

/**
 * BUG-17 fix (F-2): systemd watchdog heartbeat를 GMainLoop 타이머가 아닌
 * 전용 스레드에서 전송한다.
 *
 * [이전 구현의 문제]
 *   `g_timeout_add_seconds(_watchdog_notify_cb, ...)`로 GMainLoop 타이머 등록.
 *   ZFS 복제 등 장시간 블로킹 작업이 메인 루프를 점유하면 콜백이 실행되지 못해
 *   systemd가 서비스를 재시작(SIGABRT) → **정상 작업 중 불필요한 킬**.
 *
 * [새 구현]
 *   독립 GThread에서 `g_usleep(interval_us)` 루프로 주기적 `sd_notify(WATCHDOG=1)`.
 *   GMainLoop 상태와 무관하게 watchdog 송신. 실제로 daemon이 deadlock 나면
 *   이 스레드도 영향받지 않도록 순수 libc/glibc 호출만 사용.
 *
 * [liveness 힌트]
 *   필요 시 별도 heartbeat 카운터 추가 가능하나, 현재는 "데몬 프로세스가 살아있음"
 *   자체를 liveness proxy로 사용. SIGABRT/SEGV는 프로세스 종료로 watchdog 중단됨.
 */
static GThread   *g_watchdog_thread = NULL;
static volatile gint g_watchdog_stop = 0;  /* atomic: 0=run, 1=stop */

static gpointer
_watchdog_thread_func(gpointer data)
{
    guint64 interval_us = GPOINTER_TO_SIZE(data);
    if (interval_us < 1000000) interval_us = 1000000;  /* safety: min 1s */

    while (!g_atomic_int_get(&g_watchdog_stop)) {
        pcv_drain_notify_watchdog();   /* sd_notify("WATCHDOG=1") */
        /* 100ms 단위로 나눠 자서 stop signal에 빠르게 반응 */
        guint64 slept = 0;
        while (slept < interval_us &&
               !g_atomic_int_get(&g_watchdog_stop)) {
            g_usleep(100000);  /* 100ms */
            slept += 100000;
        }
    }
    return NULL;
}

static gboolean on_signal_received(gpointer user_data) {
    (void)user_data;  /* 컴파일러 미사용 경고 억제 (-Wunused-parameter) */
    if (!loop) return G_SOURCE_REMOVE;
    g_message("Signal received, initiating graceful shutdown...");

    /*
     * drain 타임아웃은 daemon.conf의 drain_timeout 값 (기본 30초, 최소 5초)
     * pcv_drain_begin()은 내부적으로:
     *   1. atomic 플래그를 설정하여 새 RPC 수신 거부
     *   2. GTimeout 소스를 등록하여 타임아웃 경과 시 강제 종료
     *   3. inflight 카운터가 0이 되면 즉시 g_main_loop_quit() 호출
     */
    pcv_drain_begin(loop, pcv_config_get_drain_timeout());

    return FALSE;  /* FALSE를 반환하면 GLib가 이 시그널 핸들러를 자동 제거합니다 */
}

/**
 * @brief SIGHUP 수신 시 daemon.conf를 런타임에 재로드하는 콜백
 *
 * 비파괴적 설정만 재로드: [alert] 임계값, rate_limit, etcd_timeout, log_level.
 * port, socket_path, TLS 인증서 등 구조적 설정은 데몬 재시작 필요.
 * pcv_config_reload()가 GKeyFile을 교체하므로 이후 pcv_config_get_int/get_string
 * 호출은 새 값을 반환한다.
 *
 * @return TRUE — 소스를 유지하여 다음 SIGHUP도 처리 (반복 핸들러)
 */
static gboolean on_sighup_received(gpointer user_data) {
    (void)user_data;
    g_message("[main] SIGHUP received, reloading configuration");
    pcv_config_reload();
    pcv_log_load_module_levels();  /* 모듈별 로그 레벨 재로드 */
    return TRUE;  /* TRUE = 핸들러 유지 (반복 수신) */
}

/* ═══════════════════════════════════════════════════════════════════
 * 호스트 토폴로지 스캔 (CPU 코어 등록)
 *
 * [CPU 토폴로지 배경 지식]
 *   현대 서버는 NUMA(Non-Uniform Memory Access) 아키텍처를 사용합니다.
 *   CPU가 여러 NUMA 노드로 나뉘고, 각 노드에 메모리가 직결됩니다.
 *   VM의 vCPU와 메모리를 같은 NUMA 노드에 배치하면 메모리 접근 지연이
 *   최소화됩니다 (로컬 접근 vs 리모트 접근).
 *
 *   격리 코어(isolcpus)는 커널 스케줄러가 일반 프로세스를 배치하지 않는
 *   코어입니다. VM에 배타적으로 할당하면 CPU 캐시 오염 없이
 *   예측 가능한 성능을 보장합니다.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief 호스트 CPU 토폴로지 스캔 — NUMA 노드/격리 코어를 할당기에 등록
 *
 * 호스트의 CPU 토폴로지를 스캔하여 CPU 할당기에 코어를 등록합니다.
 *
 * [현재 구현: 하드코딩 4코어]
 *   현재는 개발/테스트 편의를 위해 4코어를 하드코딩하고 있습니다.
 *   코어 0, 1은 시스템용(비격리), 코어 2, 3은 VM 전용(격리)으로 설정합니다.
 *
 * [프로덕션 개선 방향]
 *   실제 환경에서는 /sys/devices/system/cpu/ 에서 동적으로 읽어야 합니다:
 *   - /sys/devices/system/cpu/cpu{N}/topology/core_id → 물리 코어 번호
 *   - /sys/devices/system/node/node{N}/cpulist → NUMA 매핑
 *   - /sys/devices/system/cpu/isolated → isolcpus 커널 파라미터 목록
 *   - /proc/cpuinfo의 "siblings" 필드 → 하이퍼스레딩 여부
 *
 * @param alloc CPU 할당기 인스턴스 (cpu_allocator_new()로 생성된 것)
 *
 * @note cpu_allocator_add_core()의 파라미터:
 *   (alloc, logical_id, physical_id, numa_node, is_isolated)
 *   - logical_id: OS가 부여한 CPU 번호 (/proc/cpuinfo의 processor 필드)
 *   - physical_id: 물리 코어 번호 (HT 시 같은 물리코어에 2개 논리 ID)
 *   - numa_node: NUMA 노드 번호 (0번부터)
 *   - is_isolated: TRUE면 VM 전용 격리 코어 (isolcpus 커널 파라미터)
 */
static void scan_and_register_host_topology(CpuAllocator *alloc) {
    g_message("[Init] Scanning Host Topology and Isolated CPUs...");

    /*
     * 예시: 4코어 1소켓 시스템 (NUMA 노드 0)
     *   코어 0, 1: 호스트 OS + 데몬 프로세스용 (비격리)
     *   코어 2, 3: VM 전용 격리 코어 (isolcpus=2,3)
     *
     * [격리 코어 수와 VM 용량의 관계]
     *   격리 코어 2개 = 동시에 최대 2개의 1-vCPU VM 또는 1개의 2-vCPU VM을
     *   배타적으로 수용 가능합니다. 오버커밋 없이 1:1 매핑됩니다.
     *   격리 코어가 부족하면 cpu_allocator_allocate_exclusive()가 FALSE를
     *   반환하고, vm_manager는 코어 피닝 없이 VM을 생성합니다 (graceful fallback).
     */
    cpu_allocator_add_core(alloc, 0, 0, 0, FALSE);  /* 코어0: 시스템용 (비격리) */
    cpu_allocator_add_core(alloc, 1, 1, 0, FALSE);  /* 코어1: 시스템용 (비격리) */
    cpu_allocator_add_core(alloc, 2, 2, 0, TRUE);   /* 코어2: VM 전용 격리 */
    cpu_allocator_add_core(alloc, 3, 3, 0, TRUE);   /* 코어3: VM 전용 격리 */

    g_message("[Init] Host Topology mapped to In-Memory Allocator.");
}

/* ═══════════════════════════════════════════════════════════════════
 * GObject 신호 프로브 (VM 이벤트 디버깅용)
 *
 * [GObject 신호 시스템이란?]
 *   GObject의 신호(Signal)는 Observer 패턴의 구현입니다.
 *   객체(VmManager)가 특정 이벤트 발생 시 신호를 "emit"하면,
 *   그 신호에 연결(connect)된 모든 콜백이 순차적으로 호출됩니다.
 *
 *   PureCVisorVmManager는 다음 3가지 신호를 emit합니다:
 *     - "vm-started": VM 부팅 완료 시
 *     - "vm-stopped": VM 종료 시
 *     - "vm-metrics-updated": 텔레메트리 데몬이 메트릭 캐시를 갱신할 때
 *
 * [디버깅 활용법]
 *   sudo journalctl -u purecvisorsd | grep "signal_probe"
 *   이 프로브를 통해 VM 이벤트 파이프라인이 정상 작동하는지 확인 가능.
 *   프로덕션 메트릭 수집은 텔레메트리 데몬이 담당합니다.
 * ═══════════════════════════════════════════════════════════════════ */

#define SIG_PROBE_DOM "signal_probe"  /* 로그 도메인 — grep 필터링용 */

/**
 * @brief VM 시작 이벤트 수신 시 호출되는 GObject 신호 콜백 (디버깅 전용)
 *
 * PureCVisorVmManager가 "vm-started" GObject 신호를 emit하면 트리거됩니다.
 * 프로덕션 메트릭 수집은 텔레메트리 데몬이 담당하며,
 * 이 콜백은 journalctl 로그로 이벤트 발생을 확인하는 디버깅 목적입니다.
 *
 * @param mgr       VM 매니저 인스턴스 (사용하지 않음 — __attribute__((unused))로 경고 억제)
 * @param vm_name   시작된 VM의 이름 (예: "web-prod")
 * @param user_data 사용자 데이터 (NULL — g_signal_connect 시 NULL 전달)
 *
 * @note __attribute__((unused))는 GCC의 __attribute__((unused))로 확장되어
 *       미사용 파라미터 경고(-Wunused-parameter)를 억제합니다.
 */
static void
_on_vm_started_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                     const gchar         *vm_name,
                     gpointer             user_data __attribute__((unused)))
{
    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-started RECEIVED — vm_name='%s'", vm_name);
}

/**
 * @brief VM 중지 이벤트 수신 시 호출되는 GObject 신호 콜백 (디버깅 전용)
 *
 * "vm-stopped" 신호는 VM이 정상 종료(ACPI shutdown), 강제 종료(virDomainDestroy),
 * 또는 예기치 않은 크래시로 중지될 때 emit됩니다.
 *
 * @param mgr       VM 매니저 인스턴스 (사용하지 않음)
 * @param vm_name   중지된 VM의 이름
 * @param user_data 사용자 데이터 (NULL)
 */
static void
_on_vm_stopped_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                     const gchar         *vm_name,
                     gpointer             user_data __attribute__((unused)))
{
    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-stopped RECEIVED — vm_name='%s'", vm_name);
}

/**
 * @brief VM 메트릭 캐시 갱신 시 호출되는 GObject 신호 콜백 (디버깅 전용)
 *
 * 텔레메트리 데몬이 1초마다 virConnectGetAllDomainStats()로 메트릭을 수집하고
 * 캐시를 교체할 때 "vm-metrics-updated" 신호를 emit합니다.
 *
 * [cache 파라미터의 구조]
 *   GHashTable: key=VM UUID 문자열(gchar*), value=VmMetrics* 구조체
 *   VmMetrics에는 CPU 사용률, 네트워크 RX/TX 바이트, 디스크 I/O 등이 포함됩니다.
 *
 * [디버깅 출력]
 *   전체 VM 수와 첫 번째 VM의 UUID만 로그에 출력하여
 *   텔레메트리 파이프라인이 정상 동작하는지 확인합니다.
 *   모든 VM의 메트릭을 출력하면 1초마다 수십 줄의 로그가 생기므로
 *   첫 번째 항목만 샘플링합니다.
 *
 * @param mgr       VM 매니저 인스턴스 (사용하지 않음)
 * @param cache     새로 수집된 메트릭 해시테이블 (key=VM UUID, value=VmMetrics*)
 * @param user_data 사용자 데이터 (NULL)
 */
static void
_on_metrics_updated_probe(PureCVisorVmManager *mgr __attribute__((unused)),
                          GHashTable          *cache,
                          gpointer             user_data __attribute__((unused)))
{
    guint n = cache ? g_hash_table_size(cache) : 0;

    /* 디버깅용: 첫 번째 VM의 UUID만 출력 (전체 출력은 로그 폭주 유발) */
    const gchar *first_uuid = NULL;
    if (cache && n > 0) {
        GHashTableIter it;
        gpointer key;
        g_hash_table_iter_init(&it, cache);
        g_hash_table_iter_next(&it, &key, NULL);
        first_uuid = (const gchar *)key;
    }

    PCV_LOG_DEBUG(SIG_PROBE_DOM,
                 "[GIO P6] vm-metrics-updated RECEIVED — "
                 "vm_count=%u first_uuid=%s",
                 n, first_uuid ? first_uuid : "(none)");
}

/* ═══════════════════════════════════════════════════════════════════
 * main() — 프로그램 진입점
 *
 * [전체 흐름 요약]
 *   1. 권한 확인 (root 필수)
 *   2. 16단계 초기화 (의존성 순서 엄수)
 *   3. sd_notify("READY=1") → systemd에 기동 완료 통지
 *   4. g_main_loop_run() → 이벤트 루프 진입 (블로킹)
 *   5. 종료 시그널 → graceful drain → cleanup (초기화 역순)
 *
 * [systemd 연동]
 *   systemd에서는 Type=notify로 실행되므로 sd_notify("READY=1")을
 *   보내야 서비스가 "started" 상태로 전환됩니다. 이 호출 전에
 *   TimeoutStartSec이 경과하면 systemd가 서비스를 강제 종료합니다.
 *
 * [에러 처리 전략]
 *   필수 모듈(UDS 서버 등) 실패: return 1로 즉시 종료
 *   비필수 모듈(OVN, DPDK 등) 실패: 경고 로그 출력 후 계속 진행
 *   libvirt 연결 실패: DEGRADED 모드 진입 (REST/클러스터는 유지)
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief 프로그램 진입점 — 16단계 초기화 → GMainLoop → Cleanup
 *
 * @param argc 커맨드라인 인자 수 (gvir_init_object에 전달)
 * @param argv 커맨드라인 인자 배열 (gvir_init_object에 전달)
 * @return 0 정상 종료, 1 초기화 실패
 */
int main(int argc, char *argv[]) {
    const PcvBootstrapEditionInfo *edition_info = pcv_bootstrap_get_edition_info();

    /* ═════════════════════════════════════════════════════════════
     * 0단계: root 권한 검증
     *
     * [왜 root가 필요한가?]
     *   KVM/QEMU: /dev/kvm 접근, virConnectOpen("qemu:///system")
     *   ZFS: zfs create/destroy/send/recv (루트 풀 조작)
     *   OVS: ovs-vsctl (Open vSwitch 관리)
     *   nftables: nft (방화벽 규칙 관리)
     *   CAP_NET_BIND_SERVICE: 포트 80/443 바인딩
     *
     * [보안 고려사항]
     *   root로 시작하지만, 초기화 완료 후 pcv_privdrop_apply_all()로
     *   최소 capability만 유지합니다 (Principle of Least Privilege).
     *   seccomp BPF로 허용된 syscall만 실행 가능하도록 제한합니다.
     * ═════════════════════════════════════════════════════════════ */
    if (edition_info) {
        g_message("[init] Edition bootstrap: %s (cluster=%s)",
                  edition_info->edition_name,
                  edition_info->cluster_enabled ? "enabled" : "disabled");
    }

    if (geteuid() != 0) {
        fprintf(stderr, "\n\x1b[31m[!] CRITICAL ERROR: INSUFFICIENT PRIVILEGES\x1b[0m\n");
        fprintf(stderr, "    The PureCVisor Daemon MUST be run as root.\n");
        fprintf(stderr, "    Please execute using sudo: \x1b[33msudo %s\x1b[0m\n\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    /* ═════════════════════════════════════════════════════════════
     * libvirt stderr 억제
     *
     * [문제 상황]
     *   libvirt 라이브러리는 초기화 시 대량의 디버그 메시지를 stderr에
     *   출력합니다. systemd 환경에서 이 메시지들이 journal에 혼재되면
     *   PureCVisor 자체 로그를 찾기 어렵습니다.
     *
     * [해결]
     *   LIBVIRT_LOG_OUTPUTS="1:file:/dev/null" — 로그를 /dev/null로 리다이렉트
     *   LIBVIRT_LOG_FILTERS="1:libvirt" — 레벨 1(DEBUG) 이상만 필터링
     *   virInitialize()가 호출되기 전에 환경변수를 설정해야 합니다.
     * ═════════════════════════════════════════════════════════════ */
    g_setenv("LIBVIRT_LOG_OUTPUTS", "1:file:/dev/null", TRUE);
    g_setenv("LIBVIRT_LOG_FILTERS", "1:libvirt", TRUE);

    GError *error = NULL;

    /* ═════════════════════════════════════════════════════════════
     * 1단계: 로깅 + 설정 시스템 초기화
     *
     * [초기화 순서가 중요한 이유]
     *   다른 모든 모듈이 PCV_LOG_INFO/pcv_config_get_*()를 호출합니다.
     *   따라서 로깅과 설정은 가장 먼저 초기화되어야 합니다.
     *
     * [설정 파일 위치]
     *   /etc/purecvisor/daemon.conf (GKeyFile INI 형식)
     *   환경 변수 > daemon.conf > 컴파일 기본값 순으로 우선순위 적용
     * ═════════════════════════════════════════════════════════════ */
    purecvisor_logger_init();    /* 로그 포맷터 설정 (구조화된 JSON) */
    pcv_config_init();           /* daemon.conf 파싱 → g_cfg 전역 구조체 */
    pcv_log_load_module_levels(); /* daemon.conf [logging] 모듈별 로그 레벨 */

    /*
     * GLib 2.36 미만 호환성: g_type_init()
     *
     * GLib 2.36부터는 GObject 타입 시스템이 자동 초기화되지만,
     * 그 이전 버전에서는 명시적으로 g_type_init()을 호출해야 합니다.
     * 이 매크로 체크는 구버전 GLib에서도 컴파일/실행이 가능하게 합니다.
     */
    #if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
    #endif

    /*
     * libvirt-gobject 타입 시스템 초기화
     *
     * GVirConnection, GVirDomain 등의 GObject 기반 libvirt 타입을
     * GObject 타입 시스템에 등록합니다. gvir_connection_new() 등을
     * 호출하기 전에 반드시 실행해야 합니다.
     *
     * argc/argv를 받는 이유: GTK+ 스타일의 GLib 초기화 규약을 따름
     * (실제로 커맨드라인 인자를 파싱하지는 않음)
     */
    gvir_init_object(&argc, &argv);

    g_message("Starting PureCVisor Engine...");

    /* ── 초기화 단계 타이밍 매크로 ──────────────────────────────────
     *
     * [목적]
     *   각 초기화 단계의 소요 시간을 측정하여 journalctl에 출력합니다.
     *   느린 단계를 식별하여 부팅 시간을 최적화할 때 참고합니다.
     *   예: "[init] Stage 4 (libvirt-dispatcher) completed in 127ms"
     *
     * [STAGE_BEGIN 매크로]
     *   단계 번호를 증가시키고 시작 시각을 기록합니다.
     *   do-while(0) 래핑은 C 매크로의 관용구로, 세미콜론과 함께
     *   안전하게 사용할 수 있게 합니다 (if-else 문에서의 dangling else 방지).
     *
     * [STAGE_END 매크로]
     *   시작 시각과의 차이를 밀리초로 계산하여 로그에 출력합니다.
     *   label 파라미터는 단계 이름 문자열입니다 (STAGE_BEGIN의 label과 동일해야 함).
     *
     * [g_get_monotonic_time]
     *   시스템 기동 이후 경과한 마이크로초를 반환합니다.
     *   gettimeofday와 달리 NTP 시간 조정에 영향받지 않아 경과 시간 측정에 적합합니다.
     * ────────────────────────────────────────────────────────────── */
    gint64 init_total_start = g_get_monotonic_time();
    gint64 stage_start;
    gint   stage_num = 0;

#define STAGE_BEGIN(label) do { \
    stage_num++; \
    stage_start = g_get_monotonic_time(); \
    (void)0; } while(0)

#define STAGE_END(label) do { \
    gint64 _ms = (g_get_monotonic_time() - stage_start) / 1000; \
    g_message("[init] Stage %d (%s) completed in %ldms", stage_num, (label), (long)_ms); \
    } while(0)

    /* ═════════════════════════════════════════════════════════════
     * 2단계: 코어 메모리 상태 및 백그라운드 데몬 초기화
     *
     * [이 단계에서 초기화하는 모듈들]
     *   A. VM 상태 머신 (SQLite WAL) — 동시 오퍼레이션 방지
     *   B. libvirt 커넥션 풀 — 연결 재사용 + 서킷 브레이커
     *   C. 취소 가능 맵 — 비동기 작업 취소 지원
     *   D. Graceful Drain — 종료 시 inflight 추적
     *   E. CPU 할당기 — NUMA 인식 코어 배타적 할당
     *   F. libvirt 이벤트 데몬 — 도메인 이벤트 구독
     * ═════════════════════════════════════════════════════════════ */

    /*
     * A. VM 상태 머신 초기화 (SQLite WAL 모드)
     *
     * [역할]
     *   동시에 여러 RPC가 같은 VM을 조작하는 것을 방지합니다.
     *   예: vm.start와 vm.delete가 동시에 호출되면
     *       상태 머신이 "operation in progress" 잠금을 걸어 충돌을 방지합니다.
     *
     * [내부 동작]
     *   SQLite DB를 열고, WAL 모드 활성화, vm_locks 테이블 생성,
     *   이전 실행 시 남은 고아 락을 PID 확인으로 자동 회수합니다.
     *
     * [DB 위치]
     *   /var/lib/purecvisor/vm_state.db (ZFS pcvpool/state에 마운트 시 sync=always)
     */
    STAGE_BEGIN("core-modules");
    init_pending_state_machine();

    /*
     * B. libvirt 커넥션 풀 초기화
     *
     * [왜 풀링이 필요한가?]
     *   libvirt 연결(virConnectOpen)은 비용이 높은 작업입니다 (~50ms).
     *   매 RPC마다 새 연결을 맺으면 성능이 크게 저하됩니다.
     *   풀에서 미리 생성된 연결을 빌려 쓰고 반환하면 연결 비용이 최초 1회로 줄어듭니다.
     *
     * [서킷 브레이커]
     *   libvirt 데몬이 장애 시 연속 실패를 감지하고 연결 시도를 일시 중단합니다.
     *   CLOSED → OPEN (실패 임계치 초과) → HALF_OPEN (탐색적 재시도) → CLOSED
     *
     * [pool_max_conn]
     *   daemon.conf의 pool_max_conn 설정값 (기본 8개, 범위 1-64)
     *   동시 VM 작업 수에 비례하여 설정합니다.
     */
    virt_conn_pool_init((guint)pcv_config_get_pool_max_conn());

    /*
     * C. VM 작업 취소 맵 초기화
     *
     * [GCancellable이란?]
     *   GLib의 비동기 작업 취소 메커니즘입니다. 각 비동기 작업에
     *   GCancellable을 연결하면, 나중에 g_cancellable_cancel()을 호출하여
     *   진행 중인 작업을 안전하게 취소할 수 있습니다.
     *
     * [cmap의 역할]
     *   VM ID → GCancellable 매핑을 관리합니다.
     *   vm.create 중 vm.delete가 요청되면 cmap에서 해당 VM의 GCancellable을
     *   찾아 취소 시그널을 전송합니다.
     */
    cmap_init();

    /*
     * D. Graceful Drain 초기화
     *
     * [inflight 카운터란?]
     *   현재 진행 중인 RPC 요청의 수를 atomic 변수로 추적합니다.
     *   RPC 수신 시 +1, 응답 전송 시 -1.
     *   종료 시 이 카운터가 0이 될 때까지 기다립니다.
     *
     * [systemd 연동]
     *   sd_notify("STOPPING=1")로 systemd에 종료 진행 중임을 알립니다.
     *   sd_notify("WATCHDOG=1")로 watchdog을 리셋합니다.
     */
    pcv_drain_init();

    /*
     * E. CPU 할당기 초기화
     *
     * NUMA 토폴로지를 메모리에 매핑하고, VM 생성 시
     * 격리된 코어를 배타적으로 할당합니다.
     * global_allocator에 할당하여 다른 모듈에서 extern으로 참조합니다.
     */
    global_allocator = cpu_allocator_new();
    scan_and_register_host_topology(global_allocator);

    /*
     * F. 백그라운드 데몬 스레드 기동
     *
     * [virt_events 데몬의 역할]
     *   libvirt의 virConnectDomainEventRegisterAny()를 호출하여
     *   도메인 이벤트(VM 시작/중지/크래시/마이그레이션)를 구독합니다.
     *   예기치 않은 VM 종료를 감지하면 자가 치유(self-healing) 로직을
     *   트리거합니다 (예: VM 자동 재시작, 다른 노드로 failover).
     *
     * [왜 텔레메트리 데몬보다 먼저 시작하는가?]
     *   virt_events는 libvirt 이벤트 핸들러를 등록만 하고 별도 스레드를
     *   생성하지 않습니다. 텔레메트리 데몬은 vm_manager 생성 후에
     *   시작해야 하므로(GObject 신호 의존) 4단계에서 초기화합니다.
     */
    init_virt_events_daemon();
    STAGE_END("core-modules");

    /* ═════════════════════════════════════════════════════════════
     * 3단계: 보안 강화 — Capability Drop + seccomp BPF
     *
     * [권한 격하의 원리]
     *   Linux Capabilities는 root 권한을 세분화한 것입니다.
     *   pcv_privdrop_apply_all()은 다음을 수행합니다:
     *     1. 불필요한 capability를 제거 (CAP_NET_ADMIN, CAP_SYS_ADMIN 등만 유지)
     *     2. PR_SET_NO_NEW_PRIVS 설정 (exec 시 추가 권한 획득 불가)
     *     3. seccomp BPF 필터 적용 (허용된 syscall만 실행 가능)
     *
     * [seccomp BPF란?]
     *   커널 레벨에서 프로세스가 호출할 수 있는 시스템 콜을 필터링합니다.
     *   허용 목록에 없는 syscall 호출 시 EPERM이 반환됩니다.
     *   이를 통해 데몬이 해킹당하더라도 공격 표면을 최소화합니다.
     *
     * [주의: seccomp과 LXC 컨테이너]
     *   seccomp 필터는 자식 프로세스(fork/exec)에 상속됩니다.
     *   lxc-start/lxc-attach는 clone()/mount() 등의 syscall이 필요하므로
     *   컨테이너 관련 syscall은 화이트리스트에 추가되어 있습니다.
     * ═════════════════════════════════════════════════════════════ */
    STAGE_BEGIN("security");
    pcv_privdrop_apply_all();

    /*
     * GSubprocessLauncher 싱글턴 초기화
     *
     * [pcv_spawn의 역할]
     *   외부 프로세스(zfs, ip, ovs-vsctl, dnsmasq, virsh 등)를 실행할 때
     *   사용하는 GSubprocessLauncher 래퍼입니다.
     *
     * [왜 system()/popen()을 사용하지 않는가?]
     *   system()과 popen()은 셸을 통해 명령을 실행하므로
     *   커맨드 인젝션 공격에 취약합니다. pcv_spawn_sync()는
     *   argv 배열로 직접 execve()를 호출하여 이 위험을 제거합니다.
     *
     * [seccomp 적용 후 초기화 이유]
     *   pcv_spawn_launcher_init()은 GSubprocessLauncher 객체를 생성만 합니다.
     *   실제 프로세스 스폰은 런타임에 발생하며, seccomp 필터가 적용된
     *   상태에서도 fork/execve는 허용되어 있습니다.
     */
    pcv_spawn_launcher_init();
    pcv_worker_pool_init();       /* 제한된 워커 스레드 풀 (daemon.conf worker_threads) */
    pcv_update_check_init();      /* 버전 알림 — GitHub 최신 릴리스 조회 준비 */

    /*
     * /var/run/purecvisor/ 디렉토리 생성
     *
     * 네트워크 메타데이터 파일이 저장되는 런타임 디렉토리입니다:
     *   - dnsmasq-<br>.meta: 브릿지 모드/CIDR JSON
     *   - dnsmasq-<br>.conf: dnsmasq 설정 파일
     *   - dnsmasq-<br>.pid: dnsmasq PID 파일
     *   - daemon.sock: UDS 소켓 파일
     */
    pcv_network_rundir_init();
    /* SG 복원 — pcv_spawn_launcher_init()(위) 이후에만 호출 가능 (spawn 의존).
     * 주의: 기존 코드는 restore 를 어디서도 호출하지 않았다 (잠복 버그). */
    pcv_security_group_restore();
    STAGE_END("security");

    /* ═════════════════════════════════════════════════════════════
     * 4단계: libvirt 연결 + 디스패처 + 텔레메트리
     *
     * [이 단계의 핵심]
     *   libvirt 연결 → 디스패처 생성 → VM 매니저 GObject 신호 연결
     *   → 텔레메트리 데몬 시작 (VM 매니저 의존)
     * ═════════════════════════════════════════════════════════════ */

    /*
     * 메인 스레드용 libvirt-gobject 연결
     *
     * [libvirt-gobject vs libvirt Raw C API]
     *   libvirt-gobject: GObject 래핑. GVirConnection, GVirDomain 등.
     *     GObject 속성/신호 시스템과 통합되어 이벤트 기반 프로그래밍에 적합.
     *     주로 메인 스레드에서 상태 조회(vm.list 등)에 사용.
     *   libvirt Raw C API: virConnectOpen(), virDomainCreate() 등.
     *     워커 스레드에서 무거운 작업(vm.create, migrate 등)에 사용.
     *     커넥션 풀(virt_conn_pool)을 통해 관리됩니다.
     *
     * [DEGRADED 모드]
     *   libvirt 연결 실패 시 (libvirtd 미실행 등) 데몬을 종료하지 않고
     *   DEGRADED 모드로 진입합니다. VM 작업은 불가하지만 REST API,
     *   클러스터 쿼리, 설정 조회 등은 계속 동작합니다.
     *   이는 모니터링/관리 기능을 유지하기 위한 설계입니다.
     */
    STAGE_BEGIN("libvirt-dispatcher");
    gboolean libvirt_degraded = FALSE;
    GVirConnection *conn = gvir_connection_new(pcv_config_get_libvirt_uri());
    if (!gvir_connection_open(conn, NULL, &error)) {
        g_warning("libvirt connection failed: %s — entering DEGRADED mode "
                  "(VM operations unavailable, REST/cluster queries still active)",
                  error->message);
        g_error_free(error);
        error = NULL;
        libvirt_degraded = TRUE;
        /* degraded 모드: conn은 유효하지만 열려있지 않음.
         * 디스패처/텔레메트리는 NULL 안전하므로 빈 결과를 반환. */
    }

    /*
     * 디스패처 생성 + libvirt 연결 설정
     *
     * [디스패처의 역할]
     *   JSON-RPC 메서드 이름(예: "vm.start")을 해당 핸들러 함수
     *   (handler_vm_start.c의 handle_vm_start)에 라우팅합니다.
     *   130개의 RPC 메서드가 g_strcmp0() else-if 체인으로 매핑됩니다.
     *
     * [GObject 생명주기]
     *   purecvisor_dispatcher_new()는 GObject를 생성합니다.
     *   내부에서 PureCVisorVmManager도 함께 생성됩니다.
     *   cleanup 시 g_object_unref()로 해제하면 내부 객체도 연쇄 해제됩니다.
     */
    PureCVisorDispatcher *dispatcher = purecvisor_dispatcher_new();
    purecvisor_dispatcher_set_connection(dispatcher, conn);

    /*
     * GObject 신호 프로브 연결 (디버깅용)
     *
     * [g_signal_connect의 원리]
     *   GObject의 Observer 패턴 등록 함수입니다.
     *   _mgr 객체가 해당 신호를 emit하면 콜백이 호출됩니다.
     *   마지막 인자 NULL은 user_data로 콜백에 전달됩니다.
     *
     * [신호 이름 상수]
     *   PCV_VM_SIGNAL_STARTED: "vm-started"
     *   PCV_VM_SIGNAL_STOPPED: "vm-stopped"
     *   PCV_VM_SIGNAL_METRICS_UPDATED: "vm-metrics-updated"
     *   이 상수들은 vm_manager.h에 정의되어 있습니다.
     */
    PureCVisorVmManager *_mgr =
        purecvisor_dispatcher_get_vm_manager(dispatcher);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_STARTED,
                     G_CALLBACK(_on_vm_started_probe), NULL);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_STOPPED,
                     G_CALLBACK(_on_vm_stopped_probe), NULL);
    g_signal_connect(_mgr, PCV_VM_SIGNAL_METRICS_UPDATED,
                     G_CALLBACK(_on_metrics_updated_probe), NULL);

    /*
     * 텔레메트리 데몬 시작 (1초 간격 백그라운드 수집)
     *
     * [동작 원리]
     *   별도 GThread에서 1초 간격으로 virConnectGetAllDomainStats()를 호출하여
     *   모든 VM의 CPU 사용시간, 네트워크 RX/TX, 디스크 I/O 메트릭을 수집합니다.
     *   수집 완료 시 vm_manager에 "vm-metrics-updated" GObject 신호를 emit하여
     *   구독자(프로브, Prometheus exporter 등)에게 통지합니다.
     *
     * [왜 vm_manager 이후에 시작하는가?]
     *   텔레메트리 데몬이 vm_manager의 GObject 신호를 emit하므로,
     *   vm_manager가 먼저 생성되고 신호 핸들러가 연결된 후에 시작해야 합니다.
     *   순서가 바뀌면 신호가 emit되어도 수신자가 없어 메트릭이 유실됩니다.
     */
    init_telemetry_daemon(_mgr);
    STAGE_END("libvirt-dispatcher");

    /* ═════════════════════════════════════════════════════════════
     * 5단계: UDS 소켓 서버 시작
     *
     * [UDS(Unix Domain Socket)란?]
     *   같은 호스트 내 프로세스 간 통신(IPC)을 위한 소켓입니다.
     *   TCP/IP와 달리 네트워크 스택을 거치지 않아 오버헤드가 적습니다.
     *   pcvctl(CLI)가 이 소켓으로 데몬과 통신합니다.
     *
     * [프로토콜]
     *   JSON-RPC 2.0: {"jsonrpc":"2.0","method":"vm.list","params":{},"id":"1"}
     *   한 연결에 하나의 요청/응답 후 소켓이 닫힙니다 (비영속 연결).
     *
     * [소켓 경로]
     *   /var/run/purecvisor/daemon.sock (pcv_config_get_socket_path())
     *   io_uring 모드에서는 io_uring ACCEPT/READ/WRITE로 I/O를 처리합니다.
     * ═════════════════════════════════════════════════════════════ */
    STAGE_BEGIN("uds-server");
    UdsServer *server = uds_server_new(pcv_config_get_socket_path());
    uds_server_set_dispatcher(server, dispatcher);

    if (!uds_server_start(server, &error)) {
        g_critical("Failed to start UDS server: %s", error->message);
        g_error_free(error);
        return 1;  /* UDS 서버는 필수 — 실패 시 데몬을 시작할 수 없음 */
    }
    STAGE_END("uds-server");

    /* ═════════════════════════════════════════════════════════════
     * 6단계: JWT + REST API 서버 시작
     *
     * [JWT(JSON Web Token) 인증 흐름]
     *   1. 클라이언트가 POST /api/v1/auth/token에 {username, password} 전송
     *   2. 서버가 RBAC DB에서 사용자 인증 후 JWT 토큰 발급 (HS256 서명)
     *   3. 이후 요청에 "Authorization: Bearer <token>" 헤더 포함
     *   4. 서버가 각 요청에서 JWT 검증 + RBAC 역할 확인 (VIEWER/OPERATOR/ADMIN)
     *
     * [REST 서버와 UDS의 관계]
     *   REST 요청 → rest_server.c → UDS 소켓으로 JSON-RPC 전달 → dispatcher
     *   REST 서버는 HTTP를 JSON-RPC로 변환하는 "브릿지" 역할입니다.
     *   따라서 UDS 서버가 먼저 시작되어야 REST 서버가 동작합니다.
     *
     * [별도 GThread]
     *   REST 서버는 libsoup3의 SoupServer를 사용하며,
     *   자체 GMainLoop를 별도 스레드에서 실행합니다.
     *   이는 REST→UDS 동기 호출 시 메인 GMainLoop가 데드락되는 것을 방지합니다.
     * ═════════════════════════════════════════════════════════════ */
    STAGE_BEGIN("rest-grpc");
    pcv_tls_init_from_config();   /* REST 서버 시작 전에 TLS 초기화 (HTTPS 443 활성화) */
    {
        /* R-6: 시크릿 관리 — 환경 변수 PCV_SECRET_AUTH_JWT_SECRET 우선 로드 */
        gchar *jwt_sec = pcv_config_get_secret("auth", "jwt_secret", NULL);
        if (jwt_sec && *jwt_sec) {
            pcv_jwt_init(jwt_sec);
            g_free(jwt_sec);
        } else {
            g_free(jwt_sec);
            pcv_jwt_init(pcv_config_get_jwt_secret());  /* 기존 fallback */
        }
    }

    PcvRestServer *rest_server = pcv_rest_server_new(dispatcher, 0);
    if (!pcv_rest_server_start(rest_server, &error)) {
        g_critical("Failed to start REST server: %s", error->message);
        g_error_free(error);
        /* REST 서버 실패는 치명적이지 않음 — UDS 경로는 계속 동작 */
        g_object_unref(rest_server);
        rest_server = NULL;
        g_warning("REST API unavailable — continuing with UDS only");
    }

    /* ═════════════════════════════════════════════════════════════
     * 6b단계: gRPC 서버 (포트 50051, protobuf-c 바이너리 프로토콜)
     *
     * 이중 구조: REST(외부 클라이언트) + gRPC(내부 서비스/CLI)
     * daemon.conf [grpc] enabled=true 시 활성화됩니다.
     * ═════════════════════════════════════════════════════════════ */
    pcv_grpc_server_start();
    STAGE_END("rest-grpc");

    /* ═════════════════════════════════════════════════════════════
     * 7단계: 클러스터 매니저 초기화
     *
     * [3노드 HA 클러스터 아키텍처]
     *   etcd를 분산 합의 저장소로 사용합니다:
     *     - 리더 선출: CAS(Compare-And-Swap) 트랜잭션
     *     - TTL 기반 lease: 리더 장애 감지 (기본 10초)
     *     - VM XML 동기화: 페일오버 시 VM 자동 define+start
     *
     * [etcd_endpoints가 비어있으면?]
     *   클러스터 기능이 비활성화되고 단일 노드 모드로 동작합니다.
     *   이 경우 cluster.* RPC는 에러를 반환합니다.
     *
     * [설정 키 설명]
     *   etcd_endpoints: "http://192.0.2.10:2379,http://..."
     *   node_name: 현재 노드의 고유 이름 (hostname 기본값)
     *   node_ip: 클러스터 내 이 노드의 IP
     *   peer_ssh_ip: ZFS 복제 대상 SSH 주소
     *   repl_pool: 복제 대상 ZFS 풀 (기본: pcvpool/vms)
     *   repl_rpo: 복제 주기(초) (RPO=Recovery Point Objective, 기본 300초=5분)
     * ═════════════════════════════════════════════════════════════ */
    STAGE_BEGIN("cluster");
    pcv_bootstrap_init_cluster_manager();

    /* ═════════════════════════════════════════════════════════════
     * 8단계: OVS 오버레이 + iSCSI + OVN 초기화
     *
     * [OVS 오버레이란?]
     *   Open vSwitch + VXLAN 터널로 물리 네트워크 위에 가상 L2 네트워크를
     *   구성합니다. Single Edge는 로컬 브리지와 수동 peer 구성을 제공하고,
     *   클러스터 빌드는 여기에 자동 풀메시까지 추가합니다.
     *
     * [iSCSI란?]
     *   IP 네트워크를 통해 블록 스토리지를 공유하는 프로토콜입니다.
     *   노드 간 공유 스토리지가 필요할 때 tgtadm(타겟)과 iscsiadm(이니시에이터)으로
     *   ZFS zvol을 네트워크 디스크로 노출합니다.
     *
     * [OVN이란?]
     *   OVS 위에서 동작하는 SDN(Software-Defined Networking) 컨트롤 플레인입니다.
     *   논리 스위치/라우터/ACL/NAT을 선언적으로 관리합니다.
     *   Geneve 터널로 캡슐화하며, VXLAN 대비 메타데이터 유연성이 높습니다.
     *
     * [Graceful Degradation]
     *   OVS/OVN/DPDK/SR-IOV 미설치 시 초기화만 건너뛰고 데몬은 계속 동작합니다.
     *   기본 Linux Bridge 네트워킹은 항상 사용 가능합니다.
     * ═════════════════════════════════════════════════════════════ */
    STAGE_END("cluster");

    STAGE_BEGIN("network-storage");
    pcv_overlay_init(pcv_config_get_string("overlay", "tunnel_ip", ""));
    /* AF-N3: 영속 메타(/var/lib/purecvisor/overlay)에서 오버레이 재구성.
     * pcv_overlay_init 직후 배치 — OVS(ovsdb/ovs-vswitchd)는 systemd 선행
     * 서비스라 이 시점에 ovs-vsctl 가용. best-effort: 개별 실패는 WARN 후
     * 계속하며 부팅을 막지 않는다. [E2E 재부팅-복원 검증은 별도 게이트] */
    pcv_overlay_restore();
    pcv_iscsi_init();
    pcv_ovn_init();

    /* Phase 4: OVS-DPDK + SR-IOV (graceful degradation — 미설치 시 비활성화) */
    pcv_dpdk_init();   /* DPDK: 커널 바이패스 고속 패킷 처리 (dpdk-devbind.py 기반) */
    pcv_sriov_init();  /* SR-IOV: PCIe 가상 함수(VF) 분할 + vfio-pci passthrough */

    /*
     * io_uring 비동기 I/O 초기화 (조건부 컴파일)
     *
     * [io_uring이란?]
     *   Linux 5.1에서 도입된 고성능 비동기 I/O 인터페이스입니다.
     *   SQ(Submission Queue)에 I/O 요청을 넣고, CQ(Completion Queue)에서
     *   완료를 확인하는 링 버퍼 방식으로 동작합니다.
     *   시스템 콜 횟수를 최소화하여 epoll 대비 높은 IOPS를 달성합니다.
     *
     * [PureCVisor에서의 활용]
     *   UDS 소켓의 ACCEPT/READ/WRITE를 io_uring으로 처리합니다.
     *   eventfd를 GMainLoop에 등록하여 io_uring 완료 이벤트를
     *   GLib 이벤트 루프와 통합합니다 (eventfd 브릿지 패턴).
     *
     * [빌드 조건]
     *   PCV_USE_URING=1: Makefile에서 liburing-dev 감지 시 자동 설정
     *   PCV_USE_URING=0: liburing 미설치 → 이 블록이 컴파일에서 제외
     *                    기존 GLib GSocketService 기반 I/O가 사용됨
     *
     * [queue_depth=1024의 의미]
     *   SQ에 동시에 1024개의 I/O 요청을 대기시킬 수 있습니다.
     *   UDS + REST worker burst 중 SQE 고갈과 submit EAGAIN을 낮춥니다.
     *   너무 크면 메모리 낭비, 너무 작으면 I/O 요청이 대기합니다.
     */
#if PCV_USE_URING
    {
        GError *uring_err = NULL;
        PcvUringCtx *uring = pcv_uring_new(PCV_URING_DEFAULT_QUEUE_DEPTH, &uring_err);
        if (uring) {
            g_message("[main] io_uring initialized (queue_depth=%u, eventfd=%d)",
                      PCV_URING_DEFAULT_QUEUE_DEPTH, uring->event_fd);
            /* Phase U-2 완료: UDS 서버가 io_uring eventfd를 직접 사용 */
        } else {
            /* io_uring 실패는 비치명적 — GLib I/O fallback으로 동작 계속 */
            g_warning("[main] io_uring init failed: %s — using GLib I/O fallback",
                      uring_err ? uring_err->message : "unknown");
            if (uring_err) g_error_free(uring_err);
        }
    }
#endif

    STAGE_END("network-storage");

    /* ═════════════════════════════════════════════════════════════
     * 영역 D/G/J: 제로 다운타임 + Prometheus + 감사 로그
     *
     * [이 모듈들의 공통점]
     *   모두 운영 가시성(observability)과 관련된 모듈입니다.
     *   데몬의 핵심 기능(VM 관리)이 아닌 부가 기능이므로
     *   실패해도 데몬 동작에 영향을 주지 않습니다.
     * ═════════════════════════════════════════════════════════════ */

    /*
     * G: Prometheus 메트릭 레지스트리
     *
     * /api/v1/metrics 엔드포인트에서 text format으로 노출됩니다.
     * node_* 65개 + purecvisor_* 26개 = 91개 메트릭을 자체 수집하므로
     * 별도 node_exporter가 불필요합니다.
     */
    STAGE_BEGIN("observability");
    pcv_prom_init();

    /*
     * J: 감사 로그 (Audit Log)
     *
     * 모든 RPC 호출과 REST 요청을 SQLite DB와 텍스트 파일에 이중 기록합니다.
     * 보안 감사, 장애 분석, 규정 준수(compliance)에 사용됩니다.
     * 30일 보존 정책으로 자동 정리되며, DB는 1GB 상한이 있습니다.
     */
    pcv_audit_init(pcv_config_get_string("audit", "db_path",
                   "/var/lib/purecvisor/pcv_audit.db"));

    /* 통합 작업 큐 — 비동기 작업(export/import/backup 등) 상태 추적 */
    pcv_job_queue_init();

    /*
     * D: 제로 다운타임 업그레이드
     *
     * SIGUSR2 시그널 수신 시 새 바이너리를 exec하여
     * 기존 소켓 FD를 상속합니다 (exec-and-inherit 패턴).
     * 클라이언트 연결이 끊기지 않으므로 무중단 업그레이드가 가능합니다.
     */
    pcv_hot_reload_init(pcv_bootstrap_get_daemon_binary_path(), -1);

    /* E: 스토리지 티어링 (SSD → HDD 자동 계층화) */
    pcv_storage_tier_init();

    /*
     * B: GPU Passthrough
     *
     * lspci로 NVIDIA/AMD GPU를 열거하고, mdevctl로 vGPU 프로파일을 관리합니다.
     * vfio-pci 드라이버를 바인딩하여 VM에 GPU를 직접 할당(passthrough)합니다.
     * GPU가 없는 시스템에서는 graceful degradation으로 빈 목록을 반환합니다.
     */
    pcv_gpu_init();

    /*
     * H: 플러그인 시스템 (GModule 기반 동적 로딩)
     *
     * /etc/purecvisor/plugins.d/ 디렉토리의 .so 파일을 스캔하여
     * 런타임에 로드합니다. ABI v1 규약을 따르는 플러그인은
     * 디스패처에 새 RPC 메서드를 동적으로 등록할 수 있습니다.
     */
    pcv_plugin_manager_init("/etc/purecvisor/plugins.d");

    /*
     * I: mTLS 인증 (상호 TLS 인증)
     *
     * daemon.conf [tls] enabled=true 시 활성화됩니다.
     * 서버 인증서 + 클라이언트 인증서로 양방향 인증을 수행합니다.
     * HTTP(80) + HTTPS(443) 동시 리스닝을 지원합니다.
     * TLS 설정 실패 시 HTTP만으로 graceful degradation합니다.
     * (pcv_tls_init_from_config()는 REST 서버 시작 전에 이미 호출됨)
     */

    STAGE_END("observability");

    STAGE_BEGIN("extensions");
    /* F: NFV (Network Function Virtualization) — 가상 LB/FW/서비스 체인 */
    pcv_nfv_init();

    /* C: 멀티 클러스터 페더레이션 (사이트/노드 동적 가입/탈퇴) */
    pcv_bootstrap_init_federation();

    STAGE_END("extensions");

    /* ═════════════════════════════════════════════════════════════
     * 9단계: eBPF 텔레메트리 + AI Ops + 알림 + 프로세스 모니터
     *
     * [WhaTap-style 모니터링 스택]
     *   이 단계에서 초기화되는 모듈들은 함께 작동하여
     *   "자체 호스팅 APM(Application Performance Monitoring)"을 구성합니다:
     *     - eBPF 텔레메트리: /proc 기반 메트릭 수집 (5초 간격)
     *     - AI Ops: 이상 탐지(Z-score) + 예측(EMA) + 자가 치유
     *     - Alert Engine: 임계값 기반 알림 + Slack/Telegram Webhook
     *     - Process Monitor: /proc/[pid] 스캔으로 Top N 프로세스 추적
     * ═════════════════════════════════════════════════════════════ */

    /*
     * eBPF 텔레메트리 (5초 간격 심층 메트릭)
     *
     * [수집 항목] (10개 콜렉터)
     *   CPU: per-core 모드별 (user/nice/system/idle/iowait/irq/softirq/steal/guest)
     *   Memory: 60+ 필드 (MemTotal/Free/Available/Buffers/Cached/Slab/SReclaimable/Swap 등)
     *   Disk: diskstats (IOPS, I/O 시간, 대기 시간)
     *   Filesystem: 마운트 포인트별 사용량
     *   Network: per-interface RX/TX 바이트/패킷/Error/Drop
     *   VMstat: pgfault/pgmajfault (페이지 폴트)
     *   Sockstat: TCP/UDP 소켓 수
     *   Pressure: PSI (Pressure Stall Information) — CPU/MEM/IO 압력
     *   HWmon: 온도/팬 센서
     *   Misc: boot_time/entropy/filefd/conntrack/ARP/NIC 메타
     *
     * [왜 eBPF라는 이름인가?]
     *   초기 설계에서는 eBPF 프로그램을 커널에 로드하여 메트릭을 수집할
     *   계획이었으나, /proc 파일시스템 기반으로 충분한 메트릭을 얻을 수
     *   있어 현재는 /proc 파싱 방식입니다. 이름은 레거시로 유지됩니다.
     */
    STAGE_BEGIN("monitoring");
    pcv_ebpf_telemetry_init();

    /*
     * AI Ops: 이상 탐지 + 워크로드 예측 + Self-Healing 초기화
     *
     * [구성 요소]
     *   pcv_anomaly: 시계열 메트릭의 이상 패턴 감지 (Z-score 기반)
     *     → 메트릭이 평균에서 표준편차의 N배 이상 벗어나면 이상으로 판단
     *   pcv_predict: CPU/메모리 사용량 추세 예측 (EMA + OLS 5분 예측)
     *     → 향후 5분 내 임계치 초과가 예상되면 사전 경고 발생
     *   pcv_healing: 이상 감지 시 자동 복구 액션 (Policy-based)
     *     → VM 재시작, 다른 노드로 마이그레이션, 스케일링 등
     *   pcv_agent: 외부 AI 프로바이더 연동 (Claude/OpenAI/Gemini/Ollama)
     *     → 4개 프로바이더의 합의(consensus)로 자가 치유 액션 결정
     *
     * [AI 프로바이더 설정 — daemon.conf [ai] 섹션]
     *   각 프로바이더의 API 키, 모델명, 엔드포인트를 읽습니다.
     *   예: claude_api_key=sk-xxx, claude_model=claude-3-opus-20240229
     *       openai_api_key=sk-yyy, openai_model=gpt-4-turbo
     *   API 키가 비어있는 프로바이더는 비활성화됩니다.
     *
     * [extern 선언 이유]
     *   AI 모듈의 헤더가 아직 통합되지 않았으므로 함수 프로토타입을
     *   인라인으로 선언합니다. 향후 ai_agent.h 헤더로 통합 예정입니다.
     */
    {
        /* BUG-18 F-1: ZFS pool 직렬화 락 — anomaly/healing보다 먼저 초기화
         * (이후 모든 zvol create/destroy가 락을 사용) */
        extern void pcv_zfs_pool_lock_init(void);
        pcv_zfs_pool_lock_init();

        extern void pcv_anomaly_init(void);
        extern void pcv_predict_init(void);
        extern void pcv_healing_init(void);
        extern void pcv_agent_init(void);
        pcv_anomaly_init();
        pcv_predict_init();
        pcv_healing_init();
        pcv_agent_init();

        /* daemon.conf [ai] 섹션에서 프로바이더별 API 키 로딩 */
        extern void pcv_agent_configure(int provider, const gchar *model,
                                         const gchar *api_key, const gchar *endpoint);

        /*
         * 프로바이더 ID 매핑: 0=Claude, 1=OpenAI, 2=Gemini, 3=Ollama(로컬)
         *
         * [테이블 기반 설정 로딩 패턴]
         *   프로바이더마다 별도 코드 블록을 만들지 않고,
         *   { id, prefix } 배열을 순회하며 설정 키를 동적으로 조합합니다.
         *   새 프로바이더 추가 시 배열에 항목만 추가하면 됩니다.
         */
        static const struct { int id; const gchar *prefix; } _ai_provs[] = {
            { 0, "claude" }, { 1, "openai" }, { 2, "gemini" }, { 3, "ollama" }
        };

        /*
         * 각 프로바이더 prefix를 순회하며 daemon.conf에서 설정을 읽습니다.
         *
         * [동적 키 조합 예시]
         *   prefix="claude" → "claude_api_key", "claude_model", "claude_endpoint"
         *   prefix="openai" → "openai_api_key", "openai_model", "openai_endpoint"
         *
         * [G_N_ELEMENTS 매크로]
         *   배열의 요소 수를 컴파일 타임에 계산합니다.
         *   sizeof(array) / sizeof(array[0])와 동일합니다.
         *   배열 크기 변경 시 자동으로 루프 횟수가 조정됩니다.
         */
        for (gsize i = 0; i < G_N_ELEMENTS(_ai_provs); i++) {
            gchar key_k[64], model_k[64], ep_k[64];
            g_snprintf(key_k,   sizeof(key_k),   "%s_api_key",  _ai_provs[i].prefix);
            g_snprintf(model_k, sizeof(model_k),  "%s_model",    _ai_provs[i].prefix);
            g_snprintf(ep_k,    sizeof(ep_k),     "%s_endpoint", _ai_provs[i].prefix);
            gchar *api_key        = pcv_config_get_secret("ai", key_k,   "");
            const gchar *model    = pcv_config_get_string("ai", model_k,  NULL);
            const gchar *endpoint = pcv_config_get_string("ai", ep_k,     NULL);
            /* API 키가 비어있으면 해당 프로바이더를 건너뜀 (graceful skip) */
            if (api_key && *api_key)
                pcv_agent_configure(_ai_provs[i].id, model, api_key, endpoint);
            g_free(api_key);
        }
    }

    /*
     * Alert Engine: WhaTap-style threshold alerts + Webhook dispatch
     *
     * [동작 원리]
     *   5초 간격으로 CPU/MEM/DISK 메트릭을 임계값과 비교합니다.
     *   eval_period(기본 30초) 동안 임계값을 지속 초과하면 알림을 발생시킵니다.
     *   Slack/Telegram/Generic Webhook으로 POST 요청을 전송합니다.
     *   최근 100개 알림을 링 버퍼에 저장하여 alert.history RPC로 조회 가능합니다.
     */
    pcv_alert_engine_init();

    /*
     * Process Monitor: WhaTap-style /proc/[pid] scan
     *
     * 20초 간격으로 /proc/[pid]/stat + /proc/[pid]/io를 스캔하여
     * CPU% delta를 계산하고, Top N 프로세스를 정렬합니다.
     * 최대 512개 프로세스를 추적합니다.
     */
    pcv_process_monitor_init();
    STAGE_END("monitoring");

    STAGE_BEGIN("auth-templates");
    /*
     * RBAC: Role-Based Access Control
     *
     * SQLite DB에 사용자/역할을 관리합니다.
     * 초기화 시 admin 사용자가 자동 생성됩니다 (비밀번호: admin).
     * 3단계 역할: VIEWER(읽기만) < OPERATOR(VM 조작) < ADMIN(전체 권한)
     * REST API는 JWT로 사용자명을 검증하고, DB의 현재 역할을 확인해
     * 권한 부족 시 403 Forbidden을 반환합니다.
     */
    pcv_rbac_init("/var/lib/purecvisor/rbac.db");

    /*
     * VM 템플릿: 사전 정의된 VM 구성을 저장하고 재사용합니다.
     * /etc/purecvisor/templates/ 디렉토리에 JSON 파일로 관리됩니다.
     * 프리셋: ubuntu-small(1vCPU/1GB), ubuntu-medium(2/4GB), ubuntu-large(4/8GB)
     */
    pcv_vm_template_init();

    /*
     * 백업 스케줄러: 정책 기반 ZFS 스냅샷 자동 생성/삭제
     *
     * 5분 간격으로 백업 정책을 평가하고, 주기(interval)가 경과한
     * VM에 대해 ZFS 스냅샷을 자동 생성합니다.
     * 보존 정책(retention)에 따라 오래된 스냅샷은 자동 삭제됩니다.
     */
    pcv_backup_scheduler_init();
    pcv_security_group_resync_timer_init();   /* [I2-R1] vnet stale HIT 완화 주기 resync */
    /* NET-4/5: QoS/overlay 재수화 주기 reconcile — 부팅 시 vnet/OVS 미가용이었어도
     * 이후 늦게 생성된 vnet/OVS 에 persisted QoS/overlay 를 최종 적용(부팅1회성 무동작
     * 제거). SG resync 선례와 동일하게 worker-offload + in-flight guard + shutdown
     * g_source_remove. */
    pcv_qos_reconcile_timer_init();
    pcv_overlay_reconcile_timer_init();
    STAGE_END("auth-templates");

    STAGE_BEGIN("scheduler-proxy");
    pcv_bootstrap_init_scheduler_proxy();

    STAGE_END("scheduler-proxy");

    /* ═════════════════════════════════════════════════════════════
     * 10단계: OVS 오버레이 초기화 + 에디션별 자동화
     *
     * [자동 프로비저닝이란?]
     *   daemon.conf [overlay] 섹션에 default_bridge가 설정되어 있으면
     *   부팅 시 OVS 브릿지 + VXLAN 터널을 자동 생성합니다.
     *   재부팅 후 수동 ovs-vsctl 실행이 불필요합니다.
     *
     * [에디션 경계]
     *   Single Edge는 로컬 브리지 생성과 수동 peer 구성을 지원합니다.
     *   클러스터 빌드는 여기에 peer 자동화와 풀메시 토폴로지를 추가합니다.
     * ═════════════════════════════════════════════════════════════ */
    STAGE_BEGIN("overlay-provision");
    pcv_bootstrap_init_runtime_network();

    /* AF-N2: 가장 늦은 네트워크 스테이지에서 QoS 재수화.
     * tc 는 대상 vnet* 인터페이스가 존재해야 성공 — init_runtime_network 로
     * 런타임 네트워크가 올라온 직후에 배치한다. best-effort: 대상 iface 미존재
     * 시 tc 실패는 내부적으로 무시되고 부팅은 계속(크래시 없음).
     * [E2E 재부팅-복원 검증(tc 재적용 실관측)은 별도 게이트] */
    pcv_qos_restore();

    STAGE_END("overlay-provision");

    /* ── 전체 초기화 완료 시간 ──────────────────────────────────── */
    {
        gint64 total_ms = (g_get_monotonic_time() - init_total_start) / 1000;
        g_message("[init] All %d stages completed in %ldms", stage_num, (long)total_ms);
    }

    /* ═════════════════════════════════════════════════════════════
     * Post-Init Health Self-Check
     *
     * 모든 초기화 완료 후 핵심 서브시스템의 연결 상태를 검증합니다.
     * 문제 발견 시 DEGRADED 경고를 출력하고 계속 진행합니다.
     * ═════════════════════════════════════════════════════════════ */
    {
        gint health_errors = 0;

        /* 1. libvirt 커넥션 풀 검증 */
        if (!libvirt_degraded) {
            virConnectPtr test_conn = virt_conn_pool_acquire();
            if (!test_conn) {
                g_warning("[init] HEALTH: libvirt connection pool unavailable");
                health_errors++;
            } else {
                virt_conn_pool_release(test_conn);
            }
        }

        /* 2. VM 상태 DB 검증 */
        gint lock_count = pcv_vm_state_get_lock_count();
        if (lock_count < 0) {
            g_warning("[init] HEALTH: vm_state DB unavailable");
            health_errors++;
        }

        /* 3. UDS 소켓 존재 확인 */
        if (!g_file_test(pcv_config_get_socket_path(), G_FILE_TEST_EXISTS)) {
            g_warning("[init] HEALTH: UDS socket not found at %s",
                      pcv_config_get_socket_path());
            health_errors++;
        }

        if (health_errors > 0)
            g_warning("[init] Degraded startup: %d service(s) unavailable", health_errors);
        else
            g_message("[init] Health self-check passed");
    }

    /* ═════════════════════════════════════════════════════════════
     * 11단계: GMainLoop 진입
     *
     * [이벤트 루프의 동작 원리]
     *   g_main_loop_run()은 내부적으로 다음을 반복합니다:
     *     1. GMainContext에 등록된 모든 GSource를 순회
     *     2. prepare: 각 소스가 이벤트 대기 상태인지 확인
     *     3. poll: epoll_wait/io_uring으로 I/O 이벤트 대기
     *     4. check: 실제로 이벤트가 발생했는지 확인
     *     5. dispatch: 발생한 이벤트의 콜백 함수 호출
     *     6. 1로 돌아감
     *
     *   이 루프는 g_main_loop_quit()가 호출될 때까지 계속됩니다.
     *
     * [등록된 이벤트 소스 (이 시점)]
     *   - UDS 소켓: 클라이언트 연결 수신
     *   - SIGINT/SIGTERM: UNIX 시그널 (self-pipe trick)
     *   - 텔레메트리 타이머: 1초 간격
     *   - eBPF 텔레메트리 타이머: 5초 간격
     *   - 백업 스케줄러 타이머: 5분 간격
     *   - etcd keepalive 타이머: 3초 간격
     *   - io_uring eventfd: I/O 완료 이벤트 (PCV_USE_URING=1 시)
     * ═════════════════════════════════════════════════════════════ */

    /*
     * GMainLoop 생성
     *   첫 번째 인자 NULL = 기본 GMainContext (프로세스당 하나)
     *   두 번째 인자 FALSE = "is_running" 초기값 (run() 호출 시 TRUE로 전환)
     */
    loop = g_main_loop_new(NULL, FALSE);

    /* SIGINT(Ctrl+C)와 SIGTERM(systemctl stop) 시그널 핸들러 등록 */
    g_unix_signal_add(SIGINT, on_signal_received, NULL);
    g_unix_signal_add(SIGTERM, on_signal_received, NULL);
    /* SIGHUP: daemon.conf 런타임 재로드 (kill -HUP <pid> 또는 systemctl reload) */
    g_unix_signal_add(SIGHUP, on_sighup_received, NULL);

    /*
     * systemd에 기동 완료 통지 (sd_notify("READY=1"))
     *
     * [systemd Type=notify 프로토콜]
     *   Type=notify 서비스는 프로세스가 명시적으로 "준비 완료"를 알려야 합니다.
     *   sd_notify("READY=1")을 호출하면:
     *     - systemd가 서비스 상태를 "activating" → "active (running)"으로 전환
     *     - 이 서비스에 의존하는 다른 서비스가 시작됨
     *     - TimeoutStartSec 타이머가 중지됨
     *
     *   이 호출이 없으면 TimeoutStartSec(기본 90초) 경과 후
     *   systemd가 서비스를 강제 종료(SIGKILL)합니다.
     */
    pcv_drain_notify_ready();

    gint rest_port = pcv_config_get_int("daemon", "rest_port", 8080);

    /* 기동 완료 배너 — journalctl에 출력되어 관리자가 확인 가능 */
    g_message("═══════════════════════════════════════════════════════");
    g_message("  PureCVisor Engine v%s — All systems operational", PCV_PRODUCT_VERSION);
    g_message("═══════════════════════════════════════════════════════");
    g_message("  UDS  : /var/run/purecvisor/daemon.sock (io_uring)");
    g_message("  REST : http://0.0.0.0:%d/api/v1/", rest_port);
    g_message("  Web  : http://0.0.0.0:%d/ui/", rest_port);
    g_message("  WS   : ws://0.0.0.0:%d/api/v1/ws/events", rest_port);
    g_message("  RPC  : 130 methods registered");
    g_message("  REST : 88 endpoints active");
    if (libvirt_degraded)
        g_message("  MODE : *** DEGRADED (libvirt unavailable) ***");
    g_message("═══════════════════════════════════════════════════════");
    g_message("Daemon is running. Waiting for requests...");

    /* ── systemd Watchdog heartbeat 설정 (BUG-17 fix / F-2) ──────
     *
     * systemd WatchdogSec= 이 활성화되어 있으면 WATCHDOG_USEC 환경변수가
     * 설정됩니다. 이 타임아웃의 절반 간격으로 WATCHDOG=1 을 전송하여
     * 서비스가 건강함을 systemd에 알립니다.
     *
     * 기존 구현은 GMainLoop 타이머를 썼으나 ZFS 복제 등 장시간 블로킹
     * 작업이 주루프를 점유하면 watchdog이 실행되지 못해 systemd가 프로세스를
     * 재시작하는 false-positive가 있었음 (BUG-17). 이제 전용 GThread에서
     * 독립적으로 송신.
     */
    {
        guint64 watchdog_usec = pcv_drain_get_watchdog_usec();
        if (watchdog_usec > 0) {
            guint64 interval_us = watchdog_usec / 2;
            if (interval_us < 5000000) interval_us = 5000000;  /* min 5s */
            g_atomic_int_set(&g_watchdog_stop, 0);
            g_watchdog_thread = g_thread_new("pcv-watchdog",
                _watchdog_thread_func, GSIZE_TO_POINTER((gsize)interval_us));
            g_message("[main] systemd watchdog enabled via dedicated thread "
                      "(interval=%.1fs, timeout=%luus)",
                      interval_us / 1000000.0,
                      (unsigned long)watchdog_usec);
        }
    }

    /* 이 호출은 종료 시그널이 올 때까지 블로킹됩니다 */
    g_main_loop_run(loop);

    /* ═════════════════════════════════════════════════════════════
     * 12단계: 정리 (Cleanup) — 초기화의 역순으로 자원 해제
     *
     * [종료 순서 원칙]
     *   외부 서비스(REST/UDS) → 백그라운드 스레드 → 코어 상태 → 유틸리티
     *
     * [왜 역순인가?]
     *   초기화 시 A → B → C 순서로 생성되었으면,
     *   C가 B에 의존하고 B가 A에 의존할 수 있습니다.
     *   C → B → A 역순으로 해제해야 이미 해제된 자원에 접근하는
     *   use-after-free 버그를 방지합니다.
     *
     * [스레드 join 우선]
     *   백그라운드 스레드(eBPF, Alert, Process Monitor 등)를 먼저 join하여
     *   스레드가 종료된 후에 해당 스레드가 사용하던 자원(DB, 풀 등)을
     *   해제합니다. 그렇지 않으면 race condition이 발생합니다.
     * ═════════════════════════════════════════════════════════════ */
    g_message("Cleaning up resources before exit...");

    /* ── [0] Watchdog 스레드 정지 (BUG-17 fix / F-2) ─────────────
     * sd_notify("STOPPING=1")은 pcv_drain이 이미 전송함. 이후 추가 WATCHDOG=1은
     * 의미가 없고, systemd가 종료 처리 중이므로 스레드를 정지시킨다. */
    if (g_watchdog_thread) {
        g_atomic_int_set(&g_watchdog_stop, 1);
        g_thread_join(g_watchdog_thread);  /* 최대 100ms 이내 복귀 */
        g_watchdog_thread = NULL;
        g_message("[main] watchdog thread stopped");
    }

    /* ── [1] 외부 접점 종료 — 더 이상 새 요청을 받지 않음 ──────── */

    /* gRPC 서버 종료 */
    pcv_grpc_server_stop();

    /* REST 서버 종료 (NULL 체크: 시작 실패 시 NULL일 수 있음) */
    if (rest_server) {
        pcv_rest_server_stop(rest_server);
        g_object_unref(rest_server);  /* GObject 참조 카운트 감소 → 0이면 해제 */
    }

    /* JWT 키 메모리 제거 (REST 인증에 사용되므로 REST 종료 후 해제) */
    pcv_jwt_shutdown();

    /*
     * GObject 기반 객체 해제 (의존성 역순)
     *
     * [g_object_unref의 원리]
     *   GObject의 참조 카운트를 1 감소시킵니다.
     *   카운트가 0이 되면 dispose → finalize 순서로 정리 콜백이 호출됩니다.
     *   finalize에서 내부 자원(메모리, 파일 핸들 등)이 해제됩니다.
     *
     * [해제 순서]
     *   server(UDS) → dispatcher → conn(libvirt) → loop(GMainLoop)
     *   server가 dispatcher를 참조하고, dispatcher가 conn을 참조하므로
     *   이 순서를 지켜야 합니다.
     */
    g_object_unref(server);     /* UDS 서버 — 소켓 파일 삭제 + fd 닫기 */
    g_object_unref(dispatcher); /* 디스패처 — 핸들러 참조 해제 */
    g_object_unref(conn);       /* libvirt-gobject 연결 — virConnectClose 내부 호출 */
    g_main_loop_unref(loop);    /* 이벤트 루프 — 등록된 GSource 참조 해제 */

    /* ── [2] 클러스터/네트워크/스토리지 모듈 종료 (초기화 역순) ── */
    pcv_bootstrap_shutdown_cluster_stack(); /* 에디션별 클러스터 스택 */
    pcv_backup_scheduler_shutdown();  /* 백업 스케줄러 타이머 해제 */
    pcv_security_group_resync_timer_shutdown();  /* [I2-R1] resync 타이머 해제 */
    pcv_qos_reconcile_timer_shutdown();      /* NET-4: QoS reconcile 타이머 해제 (torn-down 서브시스템 타격 방지) */
    pcv_overlay_reconcile_timer_shutdown();  /* NET-5: overlay reconcile 타이머 해제 */
    pcv_vm_template_shutdown();       /* VM 템플릿 */
    pcv_rbac_shutdown();              /* RBAC DB 종료 */
    /* [감사 AF-3] AI-Ops self-healing 타이머/상태 정지 — teardown 중 헬링 타이머가
     * 이미 해제된 서브시스템(cpu_allocator/ws/audit)을 건드리는 것을 방지한다.
     * shutdown 함수는 self_healing.c에 이미 구현돼 있었으나 cleanup 배선만 누락됐다. */
    extern void pcv_healing_shutdown(void);
    pcv_healing_shutdown();
    pcv_process_monitor_shutdown();   /* Process monitor 스레드 join */
    pcv_alert_engine_shutdown();      /* Alert engine 스레드 join */
    pcv_ebpf_telemetry_shutdown();    /* eBPF 텔레메트리 스레드 join */
    pcv_iscsi_shutdown();             /* iSCSI 매니저 */
    pcv_ovn_shutdown();               /* OVN SDN (OVS보다 먼저 — OVN이 OVS 위에서 동작) */
    pcv_overlay_shutdown();           /* OVS 오버레이 */

    /* ── [3] 코어 모듈 종료 — 모든 워커 스레드 종료 후에만 안전 ── */
    pcv_drain_shutdown();             /* drain 스레드 join + 자원 해제 */
    shutdown_pending_state_machine(); /* SQLite DB 닫기 — 더 이상 lock/unlock 호출 없음 */
    cmap_shutdown();                  /* 진행 중 작업 취소 + 맵 해제 */
    virt_conn_pool_shutdown();        /* libvirt 커넥션 풀 전체 연결 해제 */

    /*
     * ── [4] 유틸리티 종료 — 반드시 마지막에 해제 ──────────────
     *
     * [왜 마지막인가?]
     *   위의 [1]~[3] shutdown 함수들이 내부에서 pcv_config (설정 조회)나
     *   pcv_spawn (외부 프로세스 종료) 를 사용할 수 있으므로,
     *   유틸리티는 모든 모듈이 완전히 종료된 후에만 해제합니다.
     *   예: cluster_manager_shutdown()이 etcd에 lease 해제를 전송할 때
     *       pcv_config_get_string("cluster", "etcd_endpoints")를 호출할 수 있음.
     */
    pcv_job_queue_shutdown();         /* 작업 큐 DB 닫기 */
    pcv_config_shutdown();            /* 설정 메모리 (GKeyFile) 해제 */
    pcv_worker_pool_shutdown();       /* 워커 스레드 풀 정리 */
    pcv_spawn_launcher_shutdown();    /* GSubprocessLauncher 해제 */
    pcv_log_shutdown();               /* 감사 로그 파일 닫기 */

    g_message("PureCVisor Engine exited cleanly.");
    return 0;
}
