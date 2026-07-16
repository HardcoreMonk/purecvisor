/**
 * @file virt_events.c
 * @brief Libvirt Lifecycle Event Listener & Self-Healing Daemon
 *
 * [파일 역할]
 *   VM이 예기치 않게 종료(OOM Kill, 호스트 크래시, 사용자 virsh destroy 등)되었을 때
 *   이를 실시간으로 감지하고, 해당 VM에 할당된 물리 CPU 코어 등 인메모리 자원을
 *   자동 회수(Self-Healing)하는 백그라운드 데몬입니다.
 *
 * [아키텍처 위치]
 *   main.c (데몬 시작)
 *     -> init_virt_events_daemon()                [이 파일]
 *          -> GThread("libvirt-events")           [전용 이벤트 루프 스레드]
 *               -> virEventRunDefaultImpl()       [블로킹 이벤트 펌핑]
 *               -> domain_lifecycle_cb()          [이벤트 발생 시 콜백]
 *                    -> g_main_context_invoke()   [메인 스레드로 작업 위임]
 *                         -> handle_vm_core_release_in_main_thread()  [코어 회수: 이름 키]
 *                         -> handle_vm_death_in_main_thread()         [self-healing: UUID]
 *
 * [주요 흐름 — VM 종료 감지 및 자원 회수]
 *   1. 전용 스레드에서 virEventRunDefaultImpl()로 Libvirt 이벤트를 블로킹 대기
 *   2. VM이 STOPPED/CRASHED 이벤트를 발생시키면 domain_lifecycle_cb() 호출
 *   3. [CMP-2] 코어 회수: 모든 종단 정지(STOPPED/CRASHED, graceful 포함)에서 VM 이름
 *      복사본을 메인 스레드로 위임 → handle_vm_core_release_in_main_thread()가
 *      cpu_allocator_free_vm_cores(name)로 회수. owner 키(=이름)와 일치해야 누수 없음.
 *   4. [self-healing] crash 계열(CRASHED / STOPPED+FAILED/CRASHED)만 UUID를 위임 →
 *      handle_vm_death_in_main_thread()가 vm-unresponsive 재시작 파이프라인 트리거.
 *
 * [핵심 패턴 — 이벤트 스레드 -> 메인 스레드 격리]
 *   - Libvirt 이벤트 콜백은 이벤트 스레드에서 실행됨
 *   - CPU Allocator 등 인메모리 자원 관리자는 메인 스레드 전용
 *   - g_main_context_invoke()로 스레드 간 안전한 작업 위임 (Mutex 불필요)
 *   - 식별자 문자열(코어 회수용 이름 / self-healing용 UUID)은 g_strdup()로 복사해
 *     소유권을 메인 스레드 콜백에 이전(각 콜백이 g_free)
 *
 * [주의사항]
 *   - domain_lifecycle_cb()에서 절대로 global_allocator를 직접 접근하지 말 것
 *     (이벤트 스레드이므로 데이터 레이스 발생)
 *   - virConnectSetKeepAlive(5, 3)으로 커넥션 끊김을 방어하지만,
 *     libvirtd 재시작 시 이벤트 수신이 중단될 수 있음 (데몬 재시작 필요)
 *   - VIR_DOMAIN_EVENT_STARTED도 로깅하여 GIO 시그널 디버깅에 활용
 */

#include <glib.h>
#include <libvirt/libvirt.h>
#include <string.h>

// --- [외부 참조: 메인 스레드의 자원 관리자] ---
#include "modules/core/cpu_allocator.h"
#include "modules/ai/self_healing.h"   /* BUG-20: vm 크래시 → healing 파이프라인 */
#include "../network/security_group.h"   /* I-2: SG vnet 캐시 정합 훅 */
#include "../../utils/pcv_worker_pool.h"
// ---------------------------------------------

/* ============================================================
 * vm-reboot-loop 감지 (1.0)
 *
 * VM이 짧은 시간 내 반복 재시작되는 패턴을 감지하여
 * synthetic anomaly "vm-reboot-loop"을 발생시킨다.
 *
 * 알고리즘:
 *   - per-VM 링버퍼 (최근 5개 stop 시각, monotonic μs)
 *   - VM_STOP 이벤트마다 push
 *   - 윈도우(REBOOT_LOOP_WINDOW_SEC=600s) 내 REBOOT_LOOP_THRESHOLD(5)회 도달 시 alert
 *
 * 메모리: per-VM 5×8B = 40B. 1000 VM도 40KB 미만.
 * 정리: VM이 명시적으로 삭제될 때까지 entry 유지 (rare) — 추후 LRU 도입 가능
 * ============================================================ */
#define REBOOT_LOOP_WINDOW_SEC  600
#define REBOOT_LOOP_THRESHOLD   5
#define REBOOT_LOOP_RING        5

typedef struct {
    gint64 stop_us[REBOOT_LOOP_RING];
    gint   pos;
    gint   count;
    gint64 last_alert_us;        /* 5분 쿨다운 */
} VmRebootTracker;

static GHashTable *g_reboot_trackers = NULL;   /* uuid(char*) → VmRebootTracker* */
static GMutex      g_reboot_mu;
static gboolean    g_reboot_init = FALSE;

static void
_reboot_tracker_init_once(void)
{
    if (g_reboot_init) return;
    g_mutex_init(&g_reboot_mu);
    g_reboot_trackers = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    g_reboot_init = TRUE;
}

/**
 * _track_vm_stop:
 * @uuid: VM UUID 문자열
 * @vm_name: VM 이름 (로그용)
 *
 * VM stop 이벤트 시각을 기록하고, 윈도우 내 임계 초과면 alert.
 * pcv_healing_on_anomaly("vm-reboot-loop", count, 99.0, 0.0, NULL) 호출.
 */
static void
_track_vm_stop(const gchar *uuid, const gchar *vm_name)
{
    if (!uuid) return;
    _reboot_tracker_init_once();

    g_mutex_lock(&g_reboot_mu);
    VmRebootTracker *t = g_hash_table_lookup(g_reboot_trackers, uuid);
    if (!t) {
        t = g_new0(VmRebootTracker, 1);
        g_hash_table_insert(g_reboot_trackers, g_strdup(uuid), t);
    }

    gint64 now = g_get_monotonic_time();
    gint64 window_us = (gint64)REBOOT_LOOP_WINDOW_SEC * G_USEC_PER_SEC;

    /* 새 stop 시각 push */
    t->stop_us[t->pos] = now;
    t->pos = (t->pos + 1) % REBOOT_LOOP_RING;
    if (t->count < REBOOT_LOOP_RING) t->count++;

    /* 윈도우 내 stop 횟수 카운트 */
    gint recent = 0;
    for (gint i = 0; i < t->count; i++) {
        if (t->stop_us[i] > 0 && (now - t->stop_us[i]) <= window_us) recent++;
    }

    /* 임계 도달 + 5분 쿨다운 검사 */
    gboolean alert = (recent >= REBOOT_LOOP_THRESHOLD) &&
                     (now - t->last_alert_us > 300 * G_USEC_PER_SEC);
    if (alert) t->last_alert_us = now;
    g_mutex_unlock(&g_reboot_mu);

    if (alert) {
        g_warning("🔁 [vm-reboot-loop] VM %s (%s) stopped %d times within %ds — possible boot failure or OOM",
                  vm_name ? vm_name : "(unknown)", uuid, recent, REBOOT_LOOP_WINDOW_SEC);
        /* synthetic anomaly: ADR-0020 규칙 4 (trigger_metric 비공백).
         * vm-reboot-loop 는 alert_only 정책이므로 restart target 불필요 → NULL. */
        pcv_healing_on_anomaly("vm-reboot-loop", (gdouble)recent, 99.0, 0.0, NULL);
    }
}

void init_virt_events_daemon(void);

/* =========================================================
 * 1. 메인 스레드 영역 (Main Thread Only)
 * ========================================================= */

/**
 * @brief 정지/종료된 VM의 CPU 코어를 owner 키(=VM 이름)로 회수하는 메인 루프 콜백
 * ⚠️ 이 함수는 반드시 g_main_context_invoke를 통해 메인 스레드에서만 실행되어야 합니다.
 *
 * [CMP-2] cpu_allocator는 owner_vm_id를 VM 이름으로 기록한다(allocate: vm.start의 vm_id,
 * reconcile: virDomainGetName). 따라서 release도 반드시 이름 키여야 한다. 이전에는
 * crash 경로가 UUID로 free해 키 불일치로 코어가 누수됐고(owner=이름 ≠ uuid → no-op),
 * graceful stop(STOPPED/SHUTDOWN)은 free 경로 자체가 없어 정지된 VM이 코어를 계속
 * 점유했다(재시작 후 reconcile은 ACTIVE VM만 재마킹 → 상태 불일치). 이 콜백을 모든
 * power-off 이벤트에 배선해 두 창을 함께 닫는다. self-healing 재시작과는 분리(여기서는
 * 재시작을 트리거하지 않음 — 정상 정지 VM을 되살리면 안 됨).
 */
static gboolean handle_vm_core_release_in_main_thread(gpointer user_data) {
    gchar *vm_name = (gchar *)user_data;
    if (global_allocator != NULL && vm_name != NULL) {
        cpu_allocator_free_vm_cores(global_allocator, vm_name);
    }
    g_free(vm_name);   /* 이벤트 스레드에서 g_strdup한 이름 소유권 해제 (Zero-Leak) */
    return G_SOURCE_REMOVE;
}

/**
 * @brief 크래시 계열 VM의 self-healing(재시작) 파이프라인을 트리거하는 메인 루프 콜백
 * ⚠️ 이 함수는 반드시 g_main_context_invoke를 통해 메인 스레드에서만 실행되어야 합니다.
 *
 * [CMP-2] 코어 회수는 handle_vm_core_release_in_main_thread(이름 키)로 분리했다.
 * 여기서는 restart 대상 식별자(UUID)만 self_healing에 전달한다.
 */
static gboolean handle_vm_death_in_main_thread(gpointer user_data) {
    gchar *vm_id = (gchar *)user_data;

    g_warning("🚨 [Self-Healing] Detected CRASHED-like event for VM %s. Triggering healing...", vm_id);

    // 1. BUG-20 fix: AI Ops 파이프라인 연결 — vm-unresponsive 정책 트리거.
    //    self_healing.c의 "vm-unresponsive" 정책이 trigger_metric="vm-unresponsive"로
    //    등록되어 있고 restart 액션을 갖는다. Z=99.0은 확정적 이상을 의미하는
    //    senti nel 값(정책 trigger_zscore=0이므로 항상 매칭).
    // AF-1: vm_id(UUID)를 restart 대상으로 전달 → self_healing 이 active 모드에서
    //    running-guard 통과 시 실제 재시작을 워커로 오프로드한다.
    pcv_healing_on_anomaly("vm-unresponsive", 1.0, 99.0, 0.0, vm_id);

    // 2. 향후 네트워크 브릿지 포트 정리(OVS port 딜리트)나 ZFS 데이터셋 언마운트 등의
    // 추가적인 클린업 로직을 이곳에 배치할 수 있습니다.

    // 3. 백그라운드 스레드에서 동적 할당해 넘겨준 UUID 문자열 메모리 해제 (Zero-Leak)
    g_free(vm_id);

    return G_SOURCE_REMOVE; // 콜백 1회성 실행 후 제거
}


/* =========================================================
 * 2. 백그라운드 데몬 영역 (Event Thread)
 * ========================================================= */

/* I-2: SG 디스패치 재동기화를 워커 스레드에서 수행 (sync_vm 은 virsh+nft 블로킹 —
 * libvirt 이벤트 스레드/메인 루프에서 직접 호출 금지). task_data = g_strdup(vm). */
static void
_sg_event_sync_worker(GTask *task, gpointer src, gpointer task_data, GCancellable *c)
{
    (void)src; (void)c;
    const gchar *vm = task_data;
    if (vm) pcv_security_group_sync_vm(vm);
    g_task_return_boolean(task, TRUE);
}

/* [R7] 바인딩 VM 의 SG vnet 재동기화를 worker pool 로 오프로드하는 공통 경로.
 * lifecycle/device 콜백이 공유한다. 이벤트 스레드 블로킹 금지 → sync_vm(virsh+nft)은
 * 워커에서. vm_name NULL/미바인딩이면 no-op(값싼 게이트, virsh 없음). */
static void
_schedule_sg_sync(const char *vm_name)
{
    if (!vm_name || !pcv_security_group_vm_is_bound(vm_name))
        return;
    GTask *sgt = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(sgt, g_strdup(vm_name), g_free);
    pcv_worker_pool_push(sgt, _sg_event_sync_worker);
    g_object_unref(sgt);  /* GTask는 워커 풀이 참조를 유지 → 여기서 unref 안전 (dispatcher.c 관례) */
}

/**
 * @brief Libvirt의 Lifecycle 이벤트가 발생했을 때 호출되는 비동기 콜백
 * ⚠️ 이 함수는 Libvirt 내부 이벤트 스레드에서 실행되므로 절대 직접 Allocator를 건드려선 안 됩니다.
 */
static int domain_lifecycle_cb(virConnectPtr conn, virDomainPtr dom,
                               int event, int detail, void *opaque)
{
    (void)conn; (void)opaque; // 미사용 파라미터 경고 방지 (detail은 A6-6에서 사용)

    char uuid[VIR_UUID_STRING_BUFLEN];
    const char *vm_name = virDomainGetName(dom);
    virDomainGetUUIDString(dom, uuid);

    /* I-2: 바인딩된 VM 의 라이프사이클 변화 시 SG vnet 캐시를 재동기화한다.
     * 모든 분기보다 앞에 두어 STARTED 의 조기 return 에 걸리지 않게 한다.
     * [R7] 오프로드 본체는 _schedule_sg_sync 로 추출 (device 콜백과 공유). */
    if (event == VIR_DOMAIN_EVENT_STARTED ||
        event == VIR_DOMAIN_EVENT_STOPPED ||
        event == VIR_DOMAIN_EVENT_SHUTDOWN ||
        event == VIR_DOMAIN_EVENT_CRASHED)
        _schedule_sg_sync(vm_name);

    /* ── GIO P6: vm-started 신호 로그 ────────────────────────────────── */
    if (event == VIR_DOMAIN_EVENT_STARTED) {
        g_log("signal_probe", G_LOG_LEVEL_DEBUG,
              "[GIO P6] vm-started RECEIVED — vm_name='%s' uuid='%s'",
              vm_name ? vm_name : "(unknown)", uuid);
        return 0;
    }

    /* ── GIO P6: vm-stopped 신호 로그 ────────────────────────────────── */
    if (event == VIR_DOMAIN_EVENT_STOPPED || event == VIR_DOMAIN_EVENT_SHUTDOWN) {
        g_log("signal_probe", G_LOG_LEVEL_DEBUG,
              "[GIO P6] vm-stopped RECEIVED — vm_name='%s' uuid='%s'",
              vm_name ? vm_name : "(unknown)", uuid);
        /* 1.0: vm-reboot-loop 추적 (per-VM 윈도우 카운터) */
        _track_vm_stop(uuid, vm_name);
    }

    /* [CMP-2] 전원이 꺼진 VM의 CPU 코어를 owner 키(VM 이름)로 회수한다. crash 경로의
     * UUID free(키 불일치 누수)와 graceful stop 무회수를 함께 닫는다. 종단 이벤트인
     * STOPPED/CRASHED 에만 회수한다 — SHUTDOWN(종료 진행 중)은 뒤이어 STOPPED 가 오므로
     * 중복 발사(두 번째 free 의 'no cores found' 경고 소음)를 피한다. 이벤트 스레드에서
     * allocator 직접 접근 금지 → 이름 복사본을 메인 스레드로 위임(g_main_context_invoke).
     * 재시작 트리거와 무관하게 항상 회수한다(정상 정지 VM은 재시작 안 함, 코어만 반납). */
    if ((event == VIR_DOMAIN_EVENT_STOPPED ||
         event == VIR_DOMAIN_EVENT_CRASHED) && vm_name) {
        g_main_context_invoke(NULL, handle_vm_core_release_in_main_thread,
                              g_strdup(vm_name));
    }

    /* [감사 A6-6] 이전에는 STOPPED 전체에 대해 self-healing "vm-unresponsive"
     * 재시작을 트리거해, 운영자가 정상 종료(ACPI shutdown·vm.stop)하거나 마이그레이션·
     * 저장한 VM까지 자동 재시작할 위험이 있었다(AF-1 restart 실배선 시 운영자와 충돌).
     * detail로 크래시 계열만 구분해 트리거한다: CRASHED 이벤트, 또는 STOPPED 중
     * 하이퍼바이저 실패(FAILED)·게스트 크래시(CRASHED)만. 정상종료(SHUTDOWN)·강제정지
     * (DESTROYED)·마이그레이션(MIGRATED)·저장(SAVED)은 제외. */
    gboolean crash_like =
        (event == VIR_DOMAIN_EVENT_CRASHED) ||
        (event == VIR_DOMAIN_EVENT_STOPPED &&
         (detail == VIR_DOMAIN_EVENT_STOPPED_FAILED ||
          detail == VIR_DOMAIN_EVENT_STOPPED_CRASHED));
    if (crash_like) {
        if (virDomainGetUUIDString(dom, uuid) == 0) {
            // 🚀 핵심: 메인 스레드로 안전하게 넘겨주기 위해 문자열 복사본 생성
            gchar *uuid_copy = g_strdup(uuid);
            // Event 스레드에서 Main 스레드의 큐(Queue)로 작업을 밀어넣음 (스레드 격리)
            g_main_context_invoke(NULL, handle_vm_death_in_main_thread, uuid_copy);
        }
    }
    
    return 0;
}

/* [I2-R2] NIC 핫플러그(도메인 재시작 없는 device add/remove) 즉시 감지 →
 * 바인딩 VM 이면 SG vnet 재동기화를 worker 로 오프로드. 이벤트 스레드 블로킹 금지.
 * VIR_DOMAIN_EVENT_ID_DEVICE_ADDED/REMOVED 는 동일 시그니처라 콜백 하나로 양쪽 처리.
 * 필터 없음 — sync_vm 은 멱등이라 디스크 등 NIC 아닌 device 여도 무해(같은 vnet 재해석). */
static void
_domain_device_cb(virConnectPtr conn, virDomainPtr dom,
                  const char *devAlias, void *opaque)
{
    (void)conn; (void)devAlias; (void)opaque;
    /* [R7] lifecycle 콜백과 동일 오프로드 경로 — _schedule_sg_sync 가 null/미바인딩 게이트 포함. */
    _schedule_sg_sync(virDomainGetName(dom));
}

/* [I2-R2] device add/remove 이벤트 best-effort 등록. 실패해도 치명 아님
 * (즉시성만 저하 → I2-R1 주기 resync 가 fallback). id<0 이면 등록 실패. */
static void
_register_device_callbacks(virConnectPtr conn, int *added_id, int *removed_id)
{
    *added_id = virConnectDomainEventRegisterAny(conn, NULL,
        VIR_DOMAIN_EVENT_ID_DEVICE_ADDED,
        VIR_DOMAIN_EVENT_CALLBACK(_domain_device_cb), NULL, NULL);
    *removed_id = virConnectDomainEventRegisterAny(conn, NULL,
        VIR_DOMAIN_EVENT_ID_DEVICE_REMOVED,
        VIR_DOMAIN_EVENT_CALLBACK(_domain_device_cb), NULL, NULL);
    if (*added_id < 0 || *removed_id < 0)
        g_warning("⚠️ [Events] device 콜백 등록 실패 [ADDED=%s REMOVED=%s] — NIC 핫플러그 "
                  "즉시성 저하 (I2-R1 주기 resync 로 fallback)",
                  *added_id   < 0 ? "FAIL" : "ok",
                  *removed_id < 0 ? "FAIL" : "ok");
    else
        g_message("🛡️ [Events] device add/remove 리스너 등록 "
                  "(NIC 핫플러그 즉시 SG 재동기화)");
}

/* [I2-R2] device 콜백 best-effort 해제 (id<0 이면 no-op, 중복 해제 방지). */
static void
_deregister_device_callbacks(virConnectPtr conn, int *added_id, int *removed_id)
{
    if (*added_id >= 0) {
        virConnectDomainEventDeregisterAny(conn, *added_id);
        *added_id = -1;
    }
    if (*removed_id >= 0) {
        virConnectDomainEventDeregisterAny(conn, *removed_id);
        *removed_id = -1;
    }
}

/**
 * @brief Libvirt 이벤트 루프를 무한히 펌핑하는 전용 데몬 스레드
 */
static gpointer libvirt_event_loop_thread(gpointer data) {
    (void)data;

    // 1. Libvirt 기본 이벤트 루프 구현체 초기화 (이벤트 수신에 필수)
    virEventRegisterDefaultImpl();

    // 2. 이벤트를 수신할 영구 커넥션 개방
    virConnectPtr event_conn = virConnectOpen("qemu:///system");
    if (!event_conn) {
        g_critical("🚨 [Events] Failed to open Libvirt connection for events. Self-Healing disabled.");
        return NULL;
    }

    // 3. 커넥션 끊어짐 방어 (KeepAlive 5초 간격, 3번 실패 시 종료)
    virConnectSetKeepAlive(event_conn, 5, 3);

    /*
     * 4. Lifecycle 이벤트 콜백 등록
     *
     * virConnectDomainEventRegisterAny() 인자 설명:
     *   event_conn  — 이벤트를 수신할 Libvirt 커넥션
     *   NULL        — 모든 도메인(VM) 대상. 특정 VM만 감시하려면 virDomainPtr 전달.
     *   VIR_DOMAIN_EVENT_ID_LIFECYCLE — Lifecycle 이벤트 유형 지정
     *                                   (START/STOP/CRASH/PAUSE/RESUME 등)
     *   domain_lifecycle_cb — 이벤트 발생 시 호출될 콜백 함수
     *   NULL (opaque)       — 콜백에 전달할 사용자 데이터 (불필요)
     *   NULL (freecb)       — opaque 해제 함수 (불필요)
     *
     * 반환값: 음수이면 등록 실패. 양수이면 콜백 ID (해제 시 사용).
     *
     * VIR_DOMAIN_EVENT_CALLBACK() 매크로:
     *   콜백 함수 포인터를 virConnectDomainEventGenericCallback 타입으로
     *   안전하게 캐스팅하는 Libvirt 편의 매크로.
     */
    int callback_id = virConnectDomainEventRegisterAny(
        event_conn,
        NULL,
        VIR_DOMAIN_EVENT_ID_LIFECYCLE,
        VIR_DOMAIN_EVENT_CALLBACK(domain_lifecycle_cb),
        NULL,  /* opaque (사용 안 함) */
        NULL   /* free callback (사용 안 함) */
    );

    if (callback_id < 0) {
        g_critical("🚨 [Events] Failed to register Libvirt lifecycle callback.");
        virConnectClose(event_conn);
        return NULL;
    }

    g_message("🛡️ [Events] Libvirt Lifecycle Listener & Self-Healing Daemon Started.");

    /* [I2-R2] device add/remove 리스너 등록 (best-effort — lifecycle 과 달리 비치명) */
    int dev_added_id = -1, dev_removed_id = -1;
    _register_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);

    /*
     * 5. 무한 루프: Libvirt 이벤트 펌핑
     *
     * virEventRunDefaultImpl()은 Libvirt의 내부 이벤트 루프를 한 번 돌린다.
     * 이벤트가 없으면 블로킹 대기하며, 이벤트가 발생하면 등록된 콜백
     * (domain_lifecycle_cb)을 호출한 뒤 반환한다.
     *
     * 이 함수는 반드시 virEventRegisterDefaultImpl()을 호출한 후에 사용해야 한다.
     * (위 1단계에서 이미 호출 완료)
     *
     * 반환값 < 0: Libvirt 내부 오류 (libvirtd 재시작 등).
     * 이 경우 1초 대기 후 재시도한다. 연속 실패 시에도 루프는 계속 돌며,
     * libvirtd가 복구되면 자동으로 이벤트 수신이 재개된다.
     */
    while (TRUE) {
        if (virEventRunDefaultImpl() < 0) {
            g_warning("⚠️ [Events] Error running Libvirt event loop. Checking connection...");
        }

        /* libvirtd 재시작 감지 → 자동 재연결 */
        if (!virConnectIsAlive(event_conn)) {
            g_warning("⚠️ [Events] libvirtd connection lost — attempting reconnect");

            /* 기존 콜백 해제 및 연결 정리 */
            if (callback_id >= 0) {
                virConnectDomainEventDeregisterAny(event_conn, callback_id);
                callback_id = -1;
            }
            _deregister_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);  /* [I2-R2] */
            virConnectClose(event_conn);
            event_conn = NULL;

            /* 재연결 시도 (최대 30초 대기, 5초 간격) */
            for (int retry = 0; retry < 6; retry++) {
                g_usleep(5 * G_USEC_PER_SEC);
                event_conn = virConnectOpen("qemu:///system");
                if (event_conn && virConnectIsAlive(event_conn)) {
                    virConnectSetKeepAlive(event_conn, 5, 3);
                    callback_id = virConnectDomainEventRegisterAny(
                        event_conn, NULL, VIR_DOMAIN_EVENT_ID_LIFECYCLE,
                        VIR_DOMAIN_EVENT_CALLBACK(domain_lifecycle_cb),
                        NULL, NULL);
                    if (callback_id >= 0) {
                        g_message("🛡️ [Events] Reconnected to libvirtd after %ds (callback_id=%d)",
                                  (retry + 1) * 5, callback_id);
                        _register_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);  /* [I2-R2] */
                        break;
                    }
                    /* 콜백 등록 실패 — 연결 닫고 재시도 */
                    virConnectClose(event_conn);
                    event_conn = NULL;
                }
                g_warning("⚠️ [Events] Reconnect attempt %d/6 failed", retry + 1);
            }

            if (!event_conn || !virConnectIsAlive(event_conn)) {
                g_warning("⚠️ [Events] Failed to reconnect after 30s — will retry next loop iteration");
                g_usleep(5 * G_USEC_PER_SEC);
                continue;
            }
        }
    }

    // 도달하지 않는 코드 (무한 루프)
    virConnectDomainEventDeregisterAny(event_conn, callback_id);
    _deregister_device_callbacks(event_conn, &dev_added_id, &dev_removed_id);  /* [I2-R2] */
    virConnectClose(event_conn);
    return NULL;
}


/* =========================================================
 * 3. 초기화 (엔트리 포인트 연동)
 * ========================================================= */

/**
 * @brief 서버 기동 시 메인 함수에서 호출되어 이벤트 데몬을 생성합니다.
 */
void init_virt_events_daemon(void) {
    GError *error = NULL;
    GThread *thread = g_thread_try_new("libvirt-events", libvirt_event_loop_thread, NULL, &error);
    
    if (!thread) {
        g_critical("Failed to create Libvirt events daemon thread: %s", error->message);
        g_error_free(error);
    }
}
