/**
 * @file cpu_allocator.c
 * @brief NUMA 인식 배타적 CPU 코어 할당기 — VM별 격리 코어 관리
 *
 * ====================================================================
 *  아키텍처에서의 위치
 * ====================================================================
 *  [호출 흐름]
 *    main.c → cpu_allocator_new() → global_allocator에 할당
 *    main.c → scan_and_register_host_topology() → cpu_allocator_add_core() x N
 *    vm_manager.c → create_vm 시 → cpu_allocator_allocate_exclusive()
 *    vm_manager.c → delete_vm 시 → cpu_allocator_free_vm_cores()
 *
 *  [다른 모듈과의 관계]
 *    - cpu_allocator.h: 이 파일의 공개 API 선언 + Opaque 타입 정의
 *    - main.c: global_allocator 생성/등록, extern으로 다른 모듈에 노출
 *    - handler_vm_start.c: VM 시작 시 코어 할당 + libvirt XML vcpupin 설정
 *    - handler_vm_lifecycle.c: VM 삭제 시 코어 반환
 *
 * ====================================================================
 *  할당 전략
 * ====================================================================
 *  1. 요청된 NUMA 노드에서 격리 코어(is_isolated=TRUE) 중 미할당(owner=NULL) 탐색
 *     → NUMA 지역성을 유지하여 메모리 접근 지연 최소화
 *  2. 해당 NUMA에 부족하면 다른 NUMA 노드로 fallback (성능 저하 감수)
 *     → 성능이 다소 떨어지지만 VM 생성 자체는 가능
 *  3. 전체 격리 코어가 부족하면 FALSE 반환 (할당 실패)
 *     → 호출자(vm_manager)가 graceful fallback: 코어 피닝 없이 VM 생성
 *
 *  [왜 오버커밋을 하지 않는가?]
 *    배타적(exclusive) 할당은 하나의 코어를 하나의 VM만 사용합니다.
 *    오버커밋(같은 코어를 여러 VM이 공유)하면 CPU 경합이 발생하여
 *    예측 불가능한 성능 저하가 생깁니다. 특히 DB, 실시간 처리,
 *    DPDK/SR-IOV 워크로드에서는 배타적 할당이 필수입니다.
 *
 * ====================================================================
 *  격리 코어 (Isolated Cores)
 * ====================================================================
 *  커널 부팅 파라미터 isolcpus=N,M,...으로 지정된 코어입니다.
 *  일반 프로세스가 스케줄링되지 않으므로 VM에 배타적 할당이 가능합니다.
 *
 *  [설정 방법]
 *    /etc/default/grub:
 *      GRUB_CMDLINE_LINUX="isolcpus=2,3,4,5 nohz_full=2,3,4,5"
 *    update-grub && reboot
 *
 *  [확인 방법]
 *    cat /sys/devices/system/cpu/isolated
 *    → 2-5  (격리된 코어 목록)
 *
 *  [nohz_full의 역할]
 *    격리 코어에서 타이머 인터럽트(tick)를 제거하여
 *    VM의 CPU 사이클을 100% 활용할 수 있게 합니다.
 *
 * ====================================================================
 *  인메모리 관리
 * ====================================================================
 *  SQLite가 아닌 GArray + GMutex로 관리합니다.
 *  데몬 재시작 시 /proc/cpuinfo, /sys/devices/system/cpu/를 다시 스캔하므로
 *  영속화가 불필요합니다. 인메모리 관리는 SQLite보다 빠르고
 *  WAL 잠금 경합도 없습니다.
 *
 * ====================================================================
 *  스레드 안전성
 * ====================================================================
 *  모든 공개 함수가 GMutex로 보호됩니다.
 *  GTask 워커 스레드에서 vm.create와 vm.delete가 동시에 호출될 수 있으므로
 *  allocate와 free가 경쟁하는 상황을 뮤텍스로 직렬화합니다.
 */
/* src/modules/core/cpu_allocator.c
 *
 * Sprint B-1: 인메모리 CPU 코어 전용 할당자 구현
 *
 * 격리 코어(is_isolated=TRUE)를 VM별로 배타적으로 관리합니다.
 * 전체 코어 목록은 GArray<CoreSlot>에 저장하며 GMutex로 보호합니다.
 */

#include "cpu_allocator.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <libvirt/libvirt.h>

/* ── 오버커밋 설정 ───────────────────────────────────────── */

/**
 * daemon.conf [cpu] allow_overcommit=true 설정 시 배타적 할당 우회 허용.
 * 오버커밋 모드에서는 격리 코어가 부족해도 이미 할당된 코어를 공유할 수 있다.
 */
static gboolean _allow_overcommit = FALSE;

void cpu_allocator_set_overcommit(gboolean allow) {
    _allow_overcommit = allow;
    if (allow)
        PCV_LOG_INFO("cpu_allocator", "CPU overcommit enabled — non-exclusive pinning allowed");
    else
        PCV_LOG_INFO("cpu_allocator", "CPU overcommit disabled — exclusive pinning enforced");
}

/* ── 내부 자료구조 ────────────────────────────────────── */

/**
 * CoreSlot — CPU 코어 1개의 할당 상태를 추적하는 구조체
 *
 * [각 필드의 출처]
 *   logical_id:  /proc/cpuinfo의 "processor" 필드
 *   physical_id: /proc/cpuinfo의 "core id" 필드
 *                (하이퍼스레딩 시 2개의 logical_id가 같은 physical_id를 공유)
 *   numa_node:   /sys/devices/system/node/node{N}/cpulist에서 확인
 *   is_isolated: /sys/devices/system/cpu/isolated에서 확인
 *   owner_vm_id: 이 할당기가 내부적으로 관리하는 소유권 정보
 *
 * [NUMA 노드란?]
 *   NUMA(Non-Uniform Memory Access) 시스템에서는 CPU가 여러 노드로 나뉘며,
 *   각 노드는 자신만의 메모리 뱅크를 가집니다. 같은 NUMA 노드의 CPU가
 *   해당 노드의 메모리에 접근하면 빠르고(로컬 ~80ns),
 *   다른 노드의 메모리에 접근하면 느립니다(리모트 ~120-200ns).
 *   VM의 vCPU와 메모리를 같은 NUMA 노드에 배치하면
 *   메모리 지연(latency)을 최소화할 수 있습니다.
 *
 * [격리 코어(is_isolated)란?]
 *   커널 부팅 파라미터 isolcpus=2,3,4,5로 지정된 코어입니다.
 *   커널 스케줄러가 이 코어에 일반 프로세스를 배치하지 않으므로
 *   VM에 배타적으로 할당하면 CPU 노이즈가 없는 안정적인 성능을 보장합니다.
 *   (DPDK/SR-IOV 환경에서 특히 중요)
 *
 * [owner_vm_id의 메모리 관리]
 *   g_strdup()으로 할당되며, 코어 반환(free_vm_cores) 시 g_free()로 해제됩니다.
 *   NULL이면 미할당(free) 상태이며, 다른 VM이 할당받을 수 있습니다.
 */
typedef struct {
    guint     logical_id;   /**< OS 논리 CPU 번호 (예: /proc/cpuinfo의 processor) */
    guint     physical_id;  /**< 물리 코어 번호 (하이퍼스레딩 시 같은 물리코어에 2개 논리 ID) */
    guint     numa_node;    /**< NUMA 노드 번호 (0, 1, ...) */
    gboolean  is_isolated;  /**< TRUE = isolcpus에 포함된 VM 전용 격리 코어 */
    gchar    *owner_vm_id;  /**< 현재 소유 VM 이름 (NULL = 미할당, 할당 가능) */
} CoreSlot;

/**
 * CpuAllocator — 전체 CPU 코어 할당 상태를 관리하는 싱글턴
 *
 * [왜 SQLite가 아닌 인메모리 관리인가?]
 *   1. 성능: 인메모리 GArray 조회는 ~1ns, SQLite 쿼리는 ~0.1ms
 *   2. 단순성: WAL 잠금 경합, 트랜잭션 관리가 불필요
 *   3. 영속화 불필요: 데몬 재시작 시 /sys/ 파일시스템을 다시 스캔하여 복원
 *   4. VM별 CPU 바인딩은 libvirt XML에 기록되므로 데몬 크래시 후에도 유효
 *
 * [스레드 안전성]
 *   모든 공개 함수가 g_mutex_lock/unlock으로 보호됩니다.
 *   handler_vm_*.c의 GTask 워커 스레드에서 안전하게 호출 가능합니다.
 *   (vm.create와 vm.delete가 동시 발생 가능)
 *
 * [struct _CpuAllocator 명명 규칙]
 *   GLib/GObject의 관습을 따릅니다. 헤더에서 typedef struct _Type Type;으로
 *   불투명 타입을 선언하고, .c에서 struct _Type { ... };으로 정의합니다.
 */
struct _CpuAllocator {
    GArray  *cores;   /**< CoreSlot 배열 — 시스템의 모든 CPU 코어 상태 */
    GMutex   mutex;   /**< 동시 할당/해제 보호용 뮤텍스 */
};

/* ── 생명주기 ─────────────────────────────────────────── */

/**
 * @brief CPU 할당기 인스턴스 생성
 *
 * 빈 CoreSlot 배열과 GMutex를 초기화합니다.
 * 생성 후 cpu_allocator_add_core()로 코어를 등록해야 합니다.
 *
 * [g_new0의 동작]
 *   g_malloc()으로 메모리를 할당하고 0으로 초기화합니다.
 *   OOM(Out of Memory) 시 g_error()가 호출되어 프로그램이 abort됩니다.
 *   GLib 프로그래밍에서는 malloc 실패 체크를 하지 않는 것이 관습입니다.
 *
 * [g_array_new 파라미터]
 *   FALSE: zero_terminated — 배열 끝에 0 요소를 추가하지 않음
 *   TRUE:  clear — 새 요소를 0으로 초기화 (CoreSlot의 owner_vm_id=NULL 보장)
 *   sizeof(CoreSlot): 각 요소의 크기 (값 타입으로 저장, 포인터가 아닌 값 복사)
 *
 * @return 새로 할당된 CpuAllocator (cpu_allocator_free()로 해제)
 */
CpuAllocator *cpu_allocator_new(void) {
    CpuAllocator *alloc = g_new0(CpuAllocator, 1);
    alloc->cores = g_array_new(FALSE, TRUE, sizeof(CoreSlot));
    g_mutex_init(&alloc->mutex);
    return alloc;
}

/**
 * @brief CPU 할당기 해제 — 모든 CoreSlot의 owner 문자열 + 배열 + 뮤텍스 정리
 *
 * [해제 순서]
 *   1. 뮤텍스 획득 (다른 스레드와의 경쟁 방지)
 *   2. 각 CoreSlot의 owner_vm_id 문자열 해제 (g_strdup으로 할당된 것)
 *   3. GArray 해제 (TRUE: 요소 배열 메모리도 함께 해제)
 *   4. 뮤텍스 해제 + clear (OS 리소스 반환)
 *   5. 구조체 자체 해제
 *
 * [g_array_free의 TRUE 파라미터]
 *   TRUE: 내부 배열 메모리도 해제
 *   FALSE: 내부 배열 포인터만 반환하고 해제하지 않음 (소유권 이전)
 *
 * @param alloc CPU 할당기 (NULL이면 무시 — NULL 안전)
 */
void cpu_allocator_free(CpuAllocator *alloc) {
    if (!alloc) return;
    g_mutex_lock(&alloc->mutex);

    /* 각 코어 슬롯의 owner 문자열 해제 */
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        g_free(slot->owner_vm_id);  /* g_strdup으로 할당된 문자열 — NULL이면 no-op */
    }
    g_array_free(alloc->cores, TRUE);  /* 배열 + 내부 메모리 해제 */

    g_mutex_unlock(&alloc->mutex);
    g_mutex_clear(&alloc->mutex);  /* 뮤텍스 OS 리소스 해제 */
    g_free(alloc);                 /* 구조체 자체 해제 */
}

/* ── 토폴로지 등록 ────────────────────────────────────── */

/**
 * @brief CPU 코어를 할당기에 등록 — 호스트 토폴로지 스캔 시 호출
 *
 * main.c의 scan_and_register_host_topology()에서 각 코어를 하나씩 등록합니다.
 * 등록된 코어는 이후 cpu_allocator_allocate_exclusive()에서 할당 대상이 됩니다.
 *
 * [designated initializer]
 *   C11의 .field = value 구문으로 CoreSlot을 초기화합니다.
 *   명시하지 않은 필드(owner_vm_id)는 자동으로 0/NULL로 초기화됩니다.
 *   이 방식은 필드 순서에 의존하지 않아 구조체 변경에 안전합니다.
 *
 * [g_array_append_val의 동작]
 *   GArray 끝에 slot의 값을 복사합니다 (memcpy).
 *   GArray가 가득 차면 내부적으로 g_realloc으로 배열을 확장합니다 (2배 성장).
 *   따라서 포인터가 아닌 값이 복사되므로 slot 변수의 수명과 무관합니다.
 *
 * @param alloc       CPU 할당기 (NULL이면 무시)
 * @param logical_id  OS 논리 CPU 번호 (/proc/cpuinfo의 processor 필드)
 * @param physical_id 물리 코어 번호 (하이퍼스레딩 시 같은 물리코어에 2개 논리 ID)
 * @param numa_node   NUMA 노드 번호 (0부터 시작)
 * @param is_isolated TRUE이면 isolcpus로 격리된 VM 전용 코어
 */
void cpu_allocator_add_core(CpuAllocator *alloc,
                             guint logical_id,
                             guint physical_id,
                             guint numa_node,
                             gboolean is_isolated) {
    if (!alloc) return;
    g_mutex_lock(&alloc->mutex);

    /* C11 designated initializer로 CoreSlot 초기화 */
    CoreSlot slot = {
        .logical_id  = logical_id,
        .physical_id = physical_id,
        .numa_node   = numa_node,
        .is_isolated = is_isolated,
        .owner_vm_id = NULL,       /* 미할당 상태 (할당 가능) */
    };
    g_array_append_val(alloc->cores, slot);  /* 값 복사로 배열에 추가 */

    g_mutex_unlock(&alloc->mutex);
    g_message("[cpu_allocator] Registered CPU %u (phys=%u, numa=%u, isolated=%s)",
              logical_id, physical_id, numa_node, is_isolated ? "YES" : "NO");
}

/* ── 배타적 할당 ──────────────────────────────────────── */

/**
 * @brief VM에 격리 코어를 배타적으로 할당
 *
 * [2단계 탐색 알고리즘]
 *   1차 시도: 요청된 NUMA 노드에서 격리 코어 중 미할당 코어 탐색
 *     → NUMA 지역성을 유지하여 메모리 접근 성능 최적화
 *     → 조건: is_isolated=TRUE && owner_vm_id==NULL && numa_node==요청값
 *   2차 시도: 요청 NUMA에 부족하면 다른 NUMA 노드에서 탐색 (fallback)
 *     → 조건: is_isolated=TRUE && owner_vm_id==NULL (NUMA 무시)
 *     → 성능이 약간 떨어지지만 할당은 가능
 *   실패: 전체 격리 코어가 부족하면 FALSE 반환
 *     → 호출자가 graceful fallback (코어 피닝 없이 VM 생성)
 *
 * [GPtrArray candidates의 역할]
 *   CoreSlot 포인터를 수집하는 임시 배열입니다.
 *   할당 후보 코어를 먼저 수집한 뒤, 충분한 수가 모이면
 *   실제 할당(owner_vm_id 설정)을 수행합니다.
 *   이 2단계 접근은 원자적 할당을 보장합니다:
 *   - 충분한 코어가 없으면 아무 것도 할당하지 않음 (부분 할당 방지)
 *
 * [배타적(exclusive) 할당의 의미]
 *   한 번 VM에 할당된 코어는 owner_vm_id가 설정되어 다른 VM이 사용할 수 없습니다.
 *   이는 오버커밋(overcommit) 없는 1:1 매핑을 보장합니다.
 *   고성능/저지연이 필요한 워크로드(DB, 실시간 처리)에 적합합니다.
 *
 * [out_cpus 소유권 이전]
 *   성공 시 *out_cpus에 GArray<guint> (논리 CPU ID 목록)이 설정됩니다.
 *   호출자가 g_array_free(*out_cpus, TRUE)로 해제해야 합니다.
 *   이는 GLib의 "호출자 소유(caller-owned)" 규약입니다.
 *
 * @param alloc      CPU 할당기
 * @param vm_id      할당 대상 VM 식별자
 * @param numa_node  선호 NUMA 노드
 * @param vcpu_count 필요한 코어 수
 * @param out_cpus   할당된 코어 번호 목록 (GArray<guint>, 호출자 소유)
 * @return TRUE=할당 성공, FALSE=코어 부족
 */
gboolean cpu_allocator_allocate_exclusive(CpuAllocator *alloc,
                                          const gchar  *vm_id,
                                          guint         numa_node,
                                          guint         vcpu_count,
                                          GArray      **out_cpus,
                                          gint         *out_numa_node) {
    if (!alloc || !vm_id || vcpu_count == 0) return FALSE;

    g_mutex_lock(&alloc->mutex);

    /*
     * 1차 시도: 요청 NUMA 노드에서 격리 코어 탐색
     *
     * [GPtrArray]
     *   GLib의 포인터 배열. CoreSlot*를 저장합니다.
     *   GArray와 달리 값이 아닌 포인터를 저장합니다.
     *   candidates->len < vcpu_count 조건으로 필요한 수만큼만 수집합니다.
     */
    GPtrArray *candidates = g_ptr_array_new();
    for (guint i = 0; i < alloc->cores->len && candidates->len < vcpu_count; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        /* 3가지 조건: 격리 코어 + 미할당 + 요청 NUMA 노드 */
        if (slot->is_isolated && slot->owner_vm_id == NULL && slot->numa_node == numa_node) {
            g_ptr_array_add(candidates, slot);
        }
    }

    /*
     * 2차 시도: NUMA-그룹 정렬 fallback (다른 노드의 격리 코어)
     *
     * [왜 candidates를 다시 생성하는가?]
     *   1차에서 일부만 찾은 경우, 해당 코어는 요청 NUMA의 것입니다.
     *   2차에서는 NUMA 무관하게 전체 격리 코어에서 탐색하므로
     *   새로운 candidates 배열로 시작합니다.
     *
     * [NUMA 그룹 정렬 — P5-2 단편화 방지]
     *   단순 배열 순서로 수집하면 여러 NUMA 노드에서 코어가 뒤섞여
     *   cross-NUMA 할당이 불필요하게 많아집니다.
     *   2-pass 접근: 먼저 요청 NUMA 노드의 코어를 수집하고,
     *   부족한 만큼만 다른 NUMA 노드에서 보충합니다.
     *   이렇게 하면 최대한 동일 NUMA 노드에서 코어를 확보하고
     *   cross-NUMA 코어를 최소화합니다.
     */
    /* [P5-2 수정] 2-pass fallback이 필요한 이유:
     * 단순 배열 순서 스캔(1-pass)으로는 여러 NUMA 노드의 코어가 뒤섞여 할당된다.
     * 2-pass: (1) 요청 NUMA 노드 우선 수집 → (2) 부족분만 다른 NUMA에서 보충.
     * cross-NUMA 코어를 최소화하여 메모리 접근 지연(~120ns vs ~80ns)을 줄인다. */
    if (candidates->len < vcpu_count) {
        g_ptr_array_free(candidates, TRUE);  /* 1차 시도 결과 폐기 */
        candidates = g_ptr_array_new();

        /* 2a: 요청 NUMA 노드의 미할당 격리 코어 우선 수집 */
        for (guint i = 0; i < alloc->cores->len && candidates->len < vcpu_count; i++) {
            CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
            if (slot->is_isolated && slot->owner_vm_id == NULL && slot->numa_node == numa_node) {
                g_ptr_array_add(candidates, slot);
            }
        }

        /* 2b: 부족분은 다른 NUMA 노드에서 보충 — cross-NUMA 최소화 */
        for (guint i = 0; i < alloc->cores->len && candidates->len < vcpu_count; i++) {
            CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
            if (slot->is_isolated && slot->owner_vm_id == NULL && slot->numa_node != numa_node) {
                g_ptr_array_add(candidates, slot);
            }
        }
    }

    if (candidates->len < vcpu_count) {
        /* 부분 할당을 하지 않는 이유: 4코어 요청에 2코어만 할당하면
         * VM이 비대칭 NUMA 배치로 예측 불가능한 성능 저하를 겪는다.
         * 호출자(vm_manager)가 코어 피닝 없이 VM을 생성하는 편이 낫다. */
        g_warning("[cpu_allocator] Not enough isolated cores for VM '%s': "
                  "need %u, available %u", vm_id, vcpu_count, candidates->len);
        g_ptr_array_free(candidates, TRUE);
        g_mutex_unlock(&alloc->mutex);
        return FALSE;
    }

    /*
     * 실제 할당 수행 — owner_vm_id를 설정하고 논리 CPU ID를 결과 배열에 추가
     *
     * [g_strdup(vm_id)의 이유]
     *   호출자의 vm_id 문자열이 나중에 g_free될 수 있으므로
     *   할당기 내부에 복사본을 저장합니다.
     *   반환(free_vm_cores) 시 g_free()로 해제됩니다.
     */
    GArray *result = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < vcpu_count; i++) {
        CoreSlot *slot = g_ptr_array_index(candidates, i);
        slot->owner_vm_id = g_strdup(vm_id);  /* 소유권 설정 (문자열 복사) */
        guint lid = slot->logical_id;
        g_array_append_val(result, lid);       /* 논리 CPU ID를 결과 배열에 추가 */
    }

    /* cross-NUMA 감지: 모든 코어가 같은 노드이면 해당 노드 번호 반환.
     * 혼합 할당이면 -1 반환 → 호출자가 NUMA 메모리 바인딩을 생략한다.
     * 잘못된 노드에 메모리를 바인딩하면 리모트 접근으로 성능이 오히려 악화된다. */
    gint actual_numa = -1;
    if (out_numa_node) {
        CoreSlot *first = g_ptr_array_index(candidates, 0);
        gboolean all_same = TRUE;
        for (guint i = 1; i < vcpu_count; i++) {
            CoreSlot *s = g_ptr_array_index(candidates, i);
            if (s->numa_node != first->numa_node) {
                all_same = FALSE;
                break;
            }
        }
        actual_numa = all_same ? (gint)first->numa_node : -1;
        *out_numa_node = actual_numa;
    }

    /* 주의: TRUE로 배열만 해제. CoreSlot*는 alloc->cores가 소유하므로 해제하면 안 된다. */
    g_ptr_array_free(candidates, TRUE);
    g_mutex_unlock(&alloc->mutex);

    if (out_numa_node && actual_numa >= 0)
        g_message("[cpu_allocator] Allocated %u core(s) to VM '%s' on NUMA node %d",
                  vcpu_count, vm_id, actual_numa);
    else
        g_message("[cpu_allocator] Allocated %u core(s) to VM '%s' (cross-NUMA)",
                  vcpu_count, vm_id);
    *out_cpus = result;  /* 소유권을 호출자에게 이전 */
    return TRUE;
}

/* ── 코어 반환 ────────────────────────────────────────── */

/**
 * @brief VM 삭제/중지 시 할당된 코어를 반환
 *
 * vm_id로 소유된 모든 CoreSlot의 owner_vm_id를 NULL로 초기화합니다.
 * 이후 다른 VM이 해당 코어를 할당받을 수 있습니다.
 *
 * [전체 배열 스캔]
 *   특정 vm_id의 코어가 어디에 있는지 인덱스를 모르므로
 *   전체 배열을 O(n)으로 스캔합니다. 코어 수가 수십~수백 개이므로
 *   성능 문제가 없습니다.
 *
 * [배열 단편화(fragmentation)가 문제가 되지 않는 이유]
 *   GArray<CoreSlot>은 시스템의 모든 CPU 코어를 고정 크기로 보유하는 정적 배열입니다.
 *   코어 반환 시 owner_vm_id를 NULL로 변경할 뿐 배열 요소를 제거하지 않습니다.
 *   따라서 배열 크기는 변하지 않고, 할당은 항상 선형 스캔으로 수행하므로
 *   "빈 슬롯이 흩어져 있는" 상태는 정상적인 동작입니다.
 *   중요한 것은 NUMA 지역성인데, 이는 allocate_exclusive()의 2단계 탐색에서
 *   보장합니다 (1차: 요청 NUMA 노드 우선, 2차: NUMA 그룹별 정렬 fallback).
 *
 * [g_strcmp0의 NULL 안전성]
 *   g_strcmp0()은 strcmp()와 달리 NULL 인자에 안전합니다.
 *   owner_vm_id가 NULL이면 FALSE를 반환합니다 (비교하지 않음).
 *
 * [경고 로그]
 *   vm_id에 해당하는 코어가 없으면 경고 로그를 출력합니다.
 *   이는 다음 상황을 감지하기 위함입니다:
 *     - 이중 해제(double free): 이미 반환한 코어를 다시 반환
 *     - 잘못된 VM 이름: 오타나 로직 오류
 *     - 코어 피닝 없이 생성된 VM: 할당 자체가 없었으므로 반환할 것도 없음
 *
 * @param alloc CPU 할당기 (NULL이면 무시)
 * @param vm_id 반환할 VM 식별자 (NULL이면 무시)
 */
void cpu_allocator_free_vm_cores(CpuAllocator *alloc, const gchar *vm_id) {
    if (!alloc || !vm_id) return;

    g_mutex_lock(&alloc->mutex);

    /* O(n) 전체 스캔이 허용되는 이유: 코어 수는 수십~수백 개이고,
     * VM 삭제는 드문 연산이므로 인덱스 구조(해시맵)의 복잡성이 불필요하다. */
    guint freed = 0;
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        if (slot->owner_vm_id && g_strcmp0(slot->owner_vm_id, vm_id) == 0) {
            g_free(slot->owner_vm_id);   /* 복사된 vm_id 문자열 해제 */
            slot->owner_vm_id = NULL;    /* 미할당 상태로 전환 */
            freed++;
        }
    }

    g_mutex_unlock(&alloc->mutex);

    if (freed > 0)
        g_message("[cpu_allocator] Freed %u core(s) from VM '%s'", freed, vm_id);
    else
        g_warning("[cpu_allocator] free_vm_cores: no cores found for VM '%s'", vm_id);
}

/* ── 코어 사용 표시 (외부 재등록용) ──────────────────────── */

/**
 * @brief 특정 코어를 특정 VM에 할당된 것으로 표시한다 (reconcile용).
 *
 * 데몬 재시작 시 cpu_allocator_reconcile()에서 호출되어,
 * 실행 중인 VM의 pinned 코어를 할당기에 다시 등록한다.
 *
 * @param alloc       CPU 할당기
 * @param logical_id  할당 표시할 논리 CPU 번호
 * @param vm_name     소유 VM 이름
 */
void cpu_allocator_mark_used(CpuAllocator *alloc, guint logical_id, const gchar *vm_name) {
    if (!alloc || !vm_name) return;
    g_mutex_lock(&alloc->mutex);
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        if (slot->logical_id == logical_id) {
            if (slot->owner_vm_id) {
                /* 이미 다른 VM이 소유 — 오버커밋 모드가 아니면 경고 */
                if (!_allow_overcommit && g_strcmp0(slot->owner_vm_id, vm_name) != 0) {
                    PCV_LOG_WARN("cpu_allocator",
                        "Core %u already owned by '%s', re-marking for '%s'",
                        logical_id, slot->owner_vm_id, vm_name);
                }
                g_free(slot->owner_vm_id);
            }
            slot->owner_vm_id = g_strdup(vm_name);
            break;
        }
    }
    g_mutex_unlock(&alloc->mutex);
}

/* ── libvirt XML 기반 재시작 복구 (Reconcile) ────────────── */

/**
 * cpu_allocator_reconcile — 재시작 복구: 실행 중인 VM의 CPU 핀을 할당기에 재등록
 *
 * [호출 시점] main.c 데몬 초기화 후반부, libvirt 연결이 확립된 후
 * [동작] 1→virConnectListAllDomains로 실행 중인 VM 목록 조회
 *        → 2→각 VM의 virDomainGetVcpus()로 현재 pinned CPU 조회
 *        → 3→cpu_allocator_mark_used()로 할당기에 재등록
 * [스레드] 메인 스레드 (초기화 시 1회)
 * [주의] 인메모리 할당기는 데몬 재시작 시 초기화되므로, libvirt XML에 기록된
 *        vcpupin 정보를 읽어와 할당 상태를 복원해야 합니다.
 *        이 과정이 없으면 이미 사용 중인 코어를 새 VM에 이중 할당하게 됩니다.
 *
 * @param alloc CPU 할당기
 * @param conn  libvirt 연결 핸들
 */
void
cpu_allocator_reconcile(CpuAllocator *alloc, virConnectPtr conn)
{
    if (!alloc || !conn) return;

    virDomainPtr *domains = NULL;
    int n = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (n <= 0 || !domains) return;

    guint total_reconciled = 0;
    for (int i = 0; i < n; i++) {
        const char *name = virDomainGetName(domains[i]);
        int ncpus = virDomainGetVcpusFlags(domains[i], VIR_DOMAIN_AFFECT_LIVE);
        if (ncpus <= 0) { virDomainFree(domains[i]); continue; }

        virVcpuInfoPtr info = g_new0(virVcpuInfo, ncpus);
        int maplen = VIR_CPU_MAPLEN(1024);
        unsigned char *cpumaps = g_new0(unsigned char, ncpus * maplen);

        if (virDomainGetVcpus(domains[i], info, ncpus, cpumaps, maplen) >= 0) {
            guint vm_cores = 0;
            for (int v = 0; v < ncpus; v++) {
                if (info[v].cpu >= 0) {
                    /* 이 코어를 VM에 할당된 것으로 재등록 */
                    cpu_allocator_mark_used(alloc, (guint)info[v].cpu, name);
                    vm_cores++;
                }
            }
            if (vm_cores > 0) {
                PCV_LOG_INFO("cpu_allocator",
                    "Reconciled %u vCPU(s) for VM '%s'", vm_cores, name);
                total_reconciled += vm_cores;
            }
        }

        g_free(info);
        g_free(cpumaps);
        virDomainFree(domains[i]);
    }
    /* 주의: domains는 libvirt가 malloc()으로 할당 — g_free()가 아닌 free() 사용.
     * 반면 info/cpumaps는 g_new0으로 할당했으므로 g_free()로 해제한다. */
    free(domains);

    PCV_LOG_INFO("cpu_allocator",
        "Reconcile complete: %u core(s) recovered from %d running VM(s)",
        total_reconciled, n);
}

/* ── 디버그 덤프 ──────────────────────────────────────── */

/**
 * @brief 전체 CPU 코어 할당 맵을 로그로 출력 (디버깅/진단용)
 *
 * 각 코어의 논리 ID, 물리 ID, NUMA 노드, 격리 여부, 소유 VM을 표 형태로 출력합니다.
 *
 * [출력 예시]
 *   [cpu_allocator] === Core Allocation Map ===
 *     CPU  0 (phys=0, numa=0, isolated=NO ) → [free]
 *     CPU  1 (phys=1, numa=0, isolated=NO ) → [free]
 *     CPU  2 (phys=2, numa=0, isolated=YES) → web-prod
 *     CPU  3 (phys=3, numa=0, isolated=YES) → db-master
 *   [cpu_allocator] ===========================
 *
 * [확인 방법]
 *   sudo journalctl -u purecvisorsd | grep cpu_allocator
 *   sudo journalctl -u purecvisormd | grep cpu_allocator
 *
 * [뮤텍스 보호]
 *   뮤텍스를 잡은 상태에서 출력하므로 할당/해제와 동시 접근이 안전합니다.
 *   출력 중에 다른 스레드가 코어를 할당/해제하지 못합니다.
 *
 * @param alloc CPU 할당기 (NULL이면 무시)
 */
void cpu_allocator_dump(CpuAllocator *alloc) {
    if (!alloc) return;
    g_mutex_lock(&alloc->mutex);

    g_message("[cpu_allocator] === Core Allocation Map ===");
    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);
        g_message("  CPU %2u (phys=%u, numa=%u, isolated=%s) → %s",
                  slot->logical_id,
                  slot->physical_id,
                  slot->numa_node,
                  slot->is_isolated ? "YES" : "NO ",
                  slot->owner_vm_id ? slot->owner_vm_id : "[free]");
    }
    g_message("[cpu_allocator] ===========================");

    g_mutex_unlock(&alloc->mutex);
}

/**
 * cpu_allocator_get_numa_info:
 * NUMA 토폴로지 정보를 JsonObject로 반환합니다.
 *
 * [반환 형식]
 *   {
 *     "total_cores": 8,
 *     "numa_nodes": [
 *       { "node": 0, "total": 4, "isolated": 2, "allocated": 1, "free": 1 },
 *       { "node": 1, "total": 4, "isolated": 2, "allocated": 0, "free": 2 }
 *     ],
 *     "cores": [
 *       { "logical_id": 0, "physical_id": 0, "numa_node": 0,
 *         "isolated": false, "owner": null },
 *       ...
 *     ]
 *   }
 *
 * @param alloc CPU 할당기 (NULL이면 빈 결과 반환)
 * @return JsonObject* — 호출자가 소유 (json_object_unref로 해제)
 */
JsonObject *cpu_allocator_get_numa_info(CpuAllocator *alloc) {
    JsonObject *result = json_object_new();
    JsonArray *cores_arr = json_array_new();

    if (!alloc) {
        json_object_set_int_member(result, "total_cores", 0);
        json_object_set_array_member(result, "numa_nodes", json_array_new());
        json_object_set_array_member(result, "cores", cores_arr);
        return result;
    }

    g_mutex_lock(&alloc->mutex);

    /* NUMA 노드별 통계를 위한 해시 테이블 (numa_node -> counts) */
    GHashTable *numa_stats = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    typedef struct { guint total; guint isolated; guint allocated; } NumaStat;

    for (guint i = 0; i < alloc->cores->len; i++) {
        CoreSlot *slot = &g_array_index(alloc->cores, CoreSlot, i);

        /* 코어별 JSON */
        JsonObject *core_obj = json_object_new();
        json_object_set_int_member(core_obj, "logical_id", slot->logical_id);
        json_object_set_int_member(core_obj, "physical_id", slot->physical_id);
        json_object_set_int_member(core_obj, "numa_node", slot->numa_node);
        json_object_set_boolean_member(core_obj, "isolated", slot->is_isolated);
        if (slot->owner_vm_id)
            json_object_set_string_member(core_obj, "owner", slot->owner_vm_id);
        else
            json_object_set_null_member(core_obj, "owner");
        json_array_add_object_element(cores_arr, core_obj);

        /* NUMA 통계 집계 */
        NumaStat *ns = g_hash_table_lookup(numa_stats, GUINT_TO_POINTER(slot->numa_node));
        if (!ns) {
            ns = g_new0(NumaStat, 1);
            g_hash_table_insert(numa_stats, GUINT_TO_POINTER(slot->numa_node), ns);
        }
        ns->total++;
        if (slot->is_isolated) ns->isolated++;
        if (slot->owner_vm_id) ns->allocated++;
    }

    json_object_set_int_member(result, "total_cores", (gint64)alloc->cores->len);
    json_object_set_array_member(result, "cores", cores_arr);

    /* NUMA 노드 요약 배열 */
    JsonArray *numa_arr = json_array_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, numa_stats);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        NumaStat *ns = value;
        JsonObject *nobj = json_object_new();
        json_object_set_int_member(nobj, "node", (gint64)GPOINTER_TO_UINT(key));
        json_object_set_int_member(nobj, "total", ns->total);
        json_object_set_int_member(nobj, "isolated", ns->isolated);
        json_object_set_int_member(nobj, "allocated", ns->allocated);
        json_object_set_int_member(nobj, "free", ns->isolated - ns->allocated);
        json_array_add_object_element(numa_arr, nobj);
    }
    json_object_set_array_member(result, "numa_nodes", numa_arr);

    g_hash_table_destroy(numa_stats);
    g_mutex_unlock(&alloc->mutex);
    return result;
}
