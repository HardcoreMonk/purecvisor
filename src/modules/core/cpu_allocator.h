/**
 * @file cpu_allocator.h
 * @brief NUMA 인식 배타적 CPU 코어 할당기 헤더
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  VM 전용으로 격리(isolated)된 물리 CPU 코어를 VM별로 배타적으로
 *  할당/해제하는 NUMA 인식 할당기의 공개 API를 정의합니다.
 *  GMutex로 멀티스레드 안전성을 보장합니다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  [호출 흐름]
 *    main.c
 *      → cpu_allocator_new() → global_allocator 생성
 *      → scan_and_register_host_topology() → cpu_allocator_add_core() x N
 *    handler_vm_start.c (vm.create/vm.start 시)
 *      → cpu_allocator_allocate_exclusive(global_allocator, vm_id, numa, vcpu, &out)
 *      → 성공: 할당된 코어 번호로 libvirt XML에 <vcpupin> 생성
 *      → 실패: 코어 피닝 없이 VM 생성 (graceful fallback)
 *    handler_vm_lifecycle.c (vm.delete 시)
 *      → cpu_allocator_free_vm_cores(global_allocator, vm_id)
 *
 *  [다른 모듈과의 관계]
 *    - cpu_allocator.c: 이 헤더의 구현 파일
 *    - main.c: global_allocator 생성 및 토폴로지 등록
 *    - vm_manager.c: VM 생성 시 코어 할당 요청
 *    - handler_vm_start.c: VM 시작 시 CPU 튜닝 (cgroup + vcpupin)
 *
 * ====================================================================
 *  불투명 타입 (Opaque Type)
 * ====================================================================
 *  CpuAllocator의 실제 필드(GArray *cores, GMutex mutex)는
 *  cpu_allocator.c에 정의됩니다. 외부에서는 cpu_allocator_*() 함수로만
 *  접근합니다. 이로써 내부 구현 변경 시 이 헤더를 include하는
 *  코드를 재컴파일할 필요가 없습니다 (ABI 안정성).
 *
 * ====================================================================
 *  전역 인스턴스
 * ====================================================================
 *  global_allocator는 main.c에서 cpu_allocator_new()로 생성되며,
 *  데몬 전체에서 하나의 할당기만 사용합니다.
 *  다른 모듈에서 `extern CpuAllocator *global_allocator;`로 참조합니다.
 *
 * ====================================================================
 *  크래시 복구
 * ====================================================================
 *  인메모리 데이터이므로 데몬 재시작 시 토폴로지가 초기화됩니다.
 *  main.c의 scan_and_register_host_topology()가 재등록합니다.
 *  VM별 CPU 바인딩은 libvirt XML에 기록되므로 별도 복구 불필요합니다.
 *
 * ====================================================================
 *  NUMA 배경 지식
 * ====================================================================
 *  [NUMA(Non-Uniform Memory Access)]
 *    멀티소켓/멀티노드 서버에서 CPU가 여러 NUMA 노드로 나뉩니다.
 *    각 노드에 메모리가 직결되어 있어:
 *      - 로컬 메모리 접근: ~80ns (같은 NUMA 노드)
 *      - 리모트 메모리 접근: ~120-200ns (다른 NUMA 노드)
 *    VM의 vCPU와 메모리를 같은 NUMA 노드에 배치하면
 *    메모리 지연을 30-60% 줄일 수 있습니다.
 *
 *  [격리 코어(isolcpus)]
 *    커널 부팅 파라미터 isolcpus=2,3,4,5로 지정된 코어입니다.
 *    커널 스케줄러가 이 코어에 일반 프로세스를 배치하지 않으므로
 *    VM에 배타적으로 할당하면 CPU 캐시 오염 없이
 *    예측 가능한 성능을 보장합니다.
 */

#ifndef PURECVISOR_CPU_ALLOCATOR_H
#define PURECVISOR_CPU_ALLOCATOR_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS

/* ── 불투명 타입 ──────────────────────────────────────── */

/**
 * CpuAllocator — NUMA 인식 배타적 CPU 코어 할당기 (Opaque Pointer)
 *
 * [내부 구조 (cpu_allocator.c에서만 접근 가능)]
 *   GArray *cores:  CoreSlot 배열 — 시스템의 모든 CPU 코어 상태
 *   GMutex mutex:   동시 할당/해제 보호용 뮤텍스
 *
 * [사용 패턴]
 *   CpuAllocator *alloc = cpu_allocator_new();
 *   cpu_allocator_add_core(alloc, 0, 0, 0, FALSE);  // 코어 등록
 *   // ... VM 생성 시 ...
 *   GArray *cpus = NULL;
 *   if (cpu_allocator_allocate_exclusive(alloc, "web-prod", 0, 2, &cpus)) {
 *       // cpus에 할당된 코어 번호 목록 (예: [2, 3])
 *       // libvirt XML에 <vcpupin vcpu='0' cpuset='2'/> 등으로 매핑
 *       g_array_free(cpus, TRUE);
 *   }
 */
typedef struct _CpuAllocator CpuAllocator;

/* ── 전역 인스턴스 (main.c에서 정의) ─────────────────── */

/**
 * 데몬 전역 CPU 할당기 인스턴스
 *
 * [extern 선언의 의미]
 *   이 변수의 정의(실제 메모리 할당)는 main.c에 있습니다.
 *   이 헤더를 include하는 다른 .c 파일에서는 "외부에 정의된 변수를 참조"합니다.
 *   링커가 main.c의 정의와 연결합니다.
 *
 * [NULL 안전]
 *   데몬 초기화 전이나 CPU 할당기 미사용 환경에서는 NULL입니다.
 *   NULL이면 코어 피닝 없이 VM을 생성합니다 (graceful fallback).
 */
extern CpuAllocator *global_allocator;

/* ── 생명주기 API ─────────────────────────────────────── */

/**
 * @brief 새 CpuAllocator 인스턴스를 생성합니다.
 *        main.c에서 1회 호출 후 global_allocator에 할당합니다.
 *
 * @return 새로 할당된 CpuAllocator 인스턴스
 *         (cpu_allocator_free()로 해제, 실패 시 NULL 반환 안 함 — OOM 시 abort)
 */
CpuAllocator *cpu_allocator_new(void);

/**
 * @brief CpuAllocator를 해제합니다.
 *        모든 CoreSlot의 owner 문자열 + 배열 + 뮤텍스를 정리합니다.
 *
 * @param alloc 해제할 CpuAllocator (NULL이면 무시 — NULL 안전)
 */
void cpu_allocator_free(CpuAllocator *alloc);

/* ── 토폴로지 등록 API ────────────────────────────────── */

/**
 * @brief 호스트 CPU 코어 하나를 할당자에 등록합니다.
 *
 * main.c의 scan_and_register_host_topology()에서 각 코어를 하나씩 등록합니다.
 * 등록된 코어는 이후 cpu_allocator_allocate_exclusive()에서 할당 대상이 됩니다.
 *
 * @param alloc        CpuAllocator 인스턴스 (NULL이면 무시)
 * @param logical_id   OS 기준 논리 CPU 번호 (0-based, /proc/cpuinfo의 processor)
 * @param physical_id  물리 코어 번호 (하이퍼스레딩 시 같은 물리코어에 2개 논리 ID)
 * @param numa_node    NUMA 노드 번호 (0부터 시작)
 * @param is_isolated  VM 전용 격리 코어 여부 (isolcpus 커널 파라미터로 지정된 코어)
 *
 * @note 코어 등록은 초기화 시에만 수행합니다. 런타임 중 동적 추가는 지원하지 않습니다.
 */
void cpu_allocator_add_core(CpuAllocator *alloc,
                             guint logical_id,
                             guint physical_id,
                             guint numa_node,
                             gboolean is_isolated);

/* ── 할당/해제 API ────────────────────────────────────── */

/**
 * @brief 특정 NUMA 노드에서 vcpu_count개의 격리 코어를 VM에 배타적으로 할당합니다.
 *
 * [2단계 탐색 알고리즘]
 *   1차: 요청된 NUMA 노드에서 격리 코어(is_isolated=TRUE) 중 미할당 코어 탐색
 *        → NUMA 지역성을 유지하여 메모리 접근 성능 최적화
 *   2차: 요청 NUMA에 부족하면 다른 NUMA 노드에서 탐색 (fallback)
 *        → 성능이 약간 떨어지지만 할당은 가능
 *   실패: 전체 격리 코어가 부족하면 FALSE 반환
 *
 * [out_cpus 소유권]
 *   성공 시 *out_cpus에 GArray<guint> (논리 CPU ID 목록)이 반환됩니다.
 *   호출자가 g_array_free(*out_cpus, TRUE)로 해제해야 합니다.
 *   실패 시 *out_cpus는 변경되지 않습니다.
 *
 * @param alloc          CpuAllocator 인스턴스 (NULL이면 FALSE 반환)
 * @param vm_id          할당 대상 VM 식별자 (이름 또는 UUID)
 * @param numa_node      선호 NUMA 노드 (격리 코어가 부족하면 다른 노드로 fallback)
 * @param vcpu_count     필요한 코어 수 (0이면 FALSE 반환)
 * @param out_cpus       할당된 logical_id 목록 (GArray<guint>), 호출자가 해제
 * @param out_numa_node  실제 할당된 NUMA 노드 (모든 코어가 같은 노드이면 해당 노드 번호,
 *                       혼합 NUMA이면 -1). NULL 전달 시 무시. NUMA 메모리 바인딩에 사용.
 * @return TRUE = 할당 성공, FALSE = 코어 부족 또는 잘못된 파라미터
 */
gboolean cpu_allocator_allocate_exclusive(CpuAllocator *alloc,
                                          const gchar  *vm_id,
                                          guint         numa_node,
                                          guint         vcpu_count,
                                          GArray      **out_cpus,
                                          gint         *out_numa_node);

/**
 * @brief VM에 할당된 모든 코어를 반환합니다.
 *
 * vm_id로 소유된 모든 CoreSlot의 owner_vm_id를 NULL로 초기화합니다.
 * 이후 다른 VM이 해당 코어를 할당받을 수 있습니다.
 *
 * [호출 시점]
 *   - vm.delete: VM 삭제 시 CPU 자원 반환
 *   - vm.stop: (선택적) VM 중지 시 코어 반환 (현재는 delete 시에만)
 *
 * [경고 로그]
 *   vm_id에 해당하는 코어가 없으면 경고 로그를 출력합니다.
 *   이중 해제(double free)나 잘못된 VM 이름을 감지하기 위함입니다.
 *
 * @param alloc  CpuAllocator 인스턴스 (NULL이면 무시)
 * @param vm_id  반환할 VM 식별자 (NULL이면 무시)
 */
void cpu_allocator_free_vm_cores(CpuAllocator *alloc, const gchar *vm_id);

/**
 * @brief 현재 할당 상태를 디버그 출력합니다.
 *
 * 각 코어의 논리 ID, 물리 ID, NUMA 노드, 격리 여부, 소유 VM을 표 형태로 출력합니다.
 * journalctl -u purecvisorsd | grep cpu_allocator 또는
 * journalctl -u purecvisormd | grep cpu_allocator 로 확인 가능합니다.
 *
 * @param alloc CpuAllocator 인스턴스 (NULL이면 무시)
 */
void cpu_allocator_dump(CpuAllocator *alloc);

/**
 * @brief CPU 오버커밋 모드를 설정합니다.
 *
 * daemon.conf [cpu] allow_overcommit=true 설정 시 호출됩니다.
 * 활성화하면 격리 코어가 부족할 때 이미 할당된 코어를 공유할 수 있습니다.
 *
 * @param allow TRUE = 오버커밋 허용, FALSE = 배타적 할당 (기본값)
 */
void cpu_allocator_set_overcommit(gboolean allow);

/**
 * @brief 특정 코어를 특정 VM에 할당된 것으로 표시합니다 (reconcile용).
 *
 * @param alloc       CpuAllocator 인스턴스
 * @param logical_id  할당 표시할 논리 CPU 번호
 * @param vm_name     소유 VM 이름
 */
void cpu_allocator_mark_used(CpuAllocator *alloc, guint logical_id, const gchar *vm_name);

/**
 * @brief 데몬 재시작 시 실행 중인 VM의 CPU 핀을 할당기에 재등록합니다.
 *
 * libvirt에서 실행 중인 VM 목록을 조회하고, 각 VM의 vcpupin 설정을
 * 읽어 할당기에 다시 등록합니다. 인메모리 할당 상태를 libvirt XML과 동기화합니다.
 *
 * @param alloc CPU 할당기 (NULL이면 무시)
 * @param conn  libvirt 연결 핸들 (NULL이면 무시)
 */
void cpu_allocator_reconcile(CpuAllocator *alloc, virConnectPtr conn);

/**
 * @brief NUMA 토폴로지 정보를 JSON으로 반환합니다.
 *
 * 전체 코어 목록, NUMA 노드별 통계(total/isolated/allocated/free)를 포함합니다.
 * vm.numa.info RPC에서 호출됩니다.
 *
 * @param alloc CpuAllocator 인스턴스 (NULL이면 빈 결과 반환)
 * @return JsonObject* — 호출자가 json_object_unref()로 해제
 */
JsonObject *cpu_allocator_get_numa_info(CpuAllocator *alloc);

G_END_DECLS

#endif /* PURECVISOR_CPU_ALLOCATOR_H */
