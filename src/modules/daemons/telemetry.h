/**
 * @file telemetry.h
 * @brief Zero-Blocking Telemetry Daemon 공개 헤더
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  파일 역할
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   백그라운드 데몬 스레드가 Libvirt Bulk API(virConnectGetAllDomainStats)를 통해
 *   1초 단위로 수집한 VM 리소스 통계를 Lock-Free 인메모리 캐시로 제공하는 인터페이스.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  아키텍처 위치
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   src/modules/daemons/telemetry.c 의 공개 API를 선언합니다.
 *   데몬 시작 시 main.c에서 init_telemetry_daemon()을 1회 호출하고,
 *   이후 RPC 핸들러(handler_vm_lifecycle.c, handler_monitor.c)에서
 *   get_vm_metrics(uuid)로 캐시된 메트릭을 O(1) 조회합니다.
 *
 *   호출 관계도:
 *     main.c (데몬 부팅)
 *       └─ init_telemetry_daemon(vm_manager)   ← 1회 호출
 *            └─ GThread("telemetry-daemon")    ← 백그라운드 무한 루프
 *
 *     handler_vm_lifecycle.c (vm.metrics RPC)
 *       └─ get_vm_metrics(uuid)                ← O(1) 해시 조회
 *
 *     handler_monitor.c (monitor.fleet RPC)
 *       └─ get_vm_metrics(uuid)                ← 동일 API
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  주요 자료구조
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   VmMetrics — 단일 VM의 CPU 시간(ns), 네트워크 RX/TX 바이트를 담는 구조체.
 *   GHashTable(UUID -> VmMetrics)이 전역 캐시로 유지되며, 1초마다 교체됩니다.
 *
 *   이 모듈은 "기본(경량)" 메트릭만 수집합니다:
 *     - cpu_time_ns: VM의 누적 CPU 시간 (나노초)
 *     - rx_bytes / tx_bytes: 주 NIC의 누적 네트워크 트래픽
 *
 *   심층 메트릭(디스크 I/O, 메모리 balloon 등)이 필요하면
 *   ebpf_telemetry.c 모듈을 참조하세요.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  Lock-Free 캐시 교체 패턴 상세
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   일반적인 멀티스레드 프로그래밍에서는 Mutex로 공유 데이터를 보호하지만,
 *   이 모듈은 GLib 이벤트 루프의 특성을 활용하여 Mutex 없이 Thread Safety를 달성:
 *
 *   [백그라운드 스레드]                    [메인 이벤트 루프 스레드]
 *     1. 새 GHashTable 생성                  (다른 RPC 처리 중)
 *     2. libvirt 데이터 파싱 → 새 캐시에 적재
 *     3. g_main_context_invoke(swap_fn)     4. swap_fn 실행: 기존 캐시 파괴 + 포인터 교체
 *        ─ 이벤트 큐에 작업 등록 ─────→       ─ 메인 루프 내에서 단독 실행
 *     5. g_usleep(1초)                       (다른 RPC 처리 재개)
 *
 *   g_main_context_invoke()는 "메인 이벤트 루프의 다음 턴에 이 함수를 실행해줘"라는 의미.
 *   메인 이벤트 루프는 싱글 스레드이므로, swap_fn과 RPC 핸들러가 동시 실행될 수 없음.
 *   결과: Mutex 오버헤드 없이 완전한 Thread Safety.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  ebpf_telemetry.c와의 역할 분담
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   | 구분          | telemetry.c (이 모듈)        | ebpf_telemetry.c          |
 *   |---------------|----------------------------|---------------------------|
 *   | 수집 대상     | VM CPU time, Net RX/TX     | 호스트+VM 전체 심층 메트릭 |
 *   | 수집 간격     | 1초                        | 5초                       |
 *   | 동기화 방식   | Lock-Free (캐시 스왑)      | GMutex                    |
 *   | 용도          | vm.metrics RPC 실시간 응답  | Prometheus /metrics 노출   |
 *   | 캐시 키       | VM UUID                    | VM Name                   |
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  주의사항
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - get_vm_metrics() 반환 포인터는 현재 메인 루프 턴에서만 유효
 *   - 호출자가 반환 포인터를 g_free() 해서는 안 됨 (캐시 소유)
 *   - 반드시 메인 스레드(GMainLoop 컨텍스트)에서만 호출할 것
 *   - GTask 워커 스레드에서 호출하면 데이터 레이스 발생 가능
 *   - 반환받은 데이터는 즉시 JSON 직렬화 후 포인터를 폐기할 것
 */

#ifndef PURECVISOR_DAEMONS_TELEMETRY_H
#define PURECVISOR_DAEMONS_TELEMETRY_H

#include <glib.h>
#include "../virt/vm_manager.h"   /* GIO P6: PureCVisorVmManager */

/* C++ 호환성을 위한 Name Mangling 방지 매크로 */
G_BEGIN_DECLS

/* =========================================================
 * 1. 자료구조 정의 (Data Structures)
 * ========================================================= */

/**
 * @struct VmMetrics
 * @brief 단일 가상 머신의 실시간 리소스 사용량 통계
 *
 * Libvirt의 virConnectGetAllDomainStats() Bulk API에서 반환되는
 * virTypedParameter 배열에서 3개 핵심 필드만 추출하여 저장하는 경량 구조체.
 *
 * 이 구조체의 인스턴스는 g_new0()으로 힙에 할당되며,
 * GHashTable의 값으로 저장된다. 캐시 교체 시 g_free()로 해제됨.
 *
 * [cpu_time_ns 활용법 — CPU 사용률(%) 계산]
 *   누적값이므로 CPU 사용률(%)을 구하려면 두 시점의 차이를 이용해야 한다:
 *     cpu% = (cpu_time_ns_현재 - cpu_time_ns_이전) / (경과시간_ns) * 100
 *   TUI(pcvtui)에서 이 공식으로 VM별 CPU 사용률 막대 그래프를 그린다.
 *
 * [rx_bytes / tx_bytes 활용법 — 네트워크 전송률]
 *   마찬가지로 누적값이므로, 두 시점의 차이를 수집 간격으로 나누면
 *   초당 전송률(bytes/sec)을 구할 수 있다.
 *
 * 추후 스토리지 블록 I/O, 메모리 사용량(Ballooning), 상세 네트워크 큐 상태 등을
 * 모니터링해야 할 경우 이 구조체에 필드를 추가하여 확장하면 됩니다.
 * (또는 심층 메트릭은 ebpf_telemetry.c의 VmExtMetrics를 사용)
 */
typedef struct {
    guint64 cpu_time_ns; /**< 누적 CPU 사용 시간 (nanoseconds).
                          *   Libvirt param: "cpu.time".
                          *   단조 증가하며, VM 재시작 시 0으로 리셋됨. */
    guint64 rx_bytes;    /**< 전 NIC 누적 수신 바이트. "net.*.rx.bytes" 합산. */
    guint64 tx_bytes;    /**< 전 NIC 누적 송신 바이트. "net.*.tx.bytes" 합산. */
    guint64 rx_packets;  /**< 전 NIC 누적 수신 패킷 수. "net.*.rx.pkts" 합산. */
    guint64 tx_packets;  /**< 전 NIC 누적 송신 패킷 수. "net.*.tx.pkts" 합산. */
    guint64 rx_errs;     /**< 전 NIC 누적 수신 에러 수. "net.*.rx.errs" 합산. */
    guint64 tx_errs;     /**< 전 NIC 누적 송신 에러 수. "net.*.tx.errs" 합산. */
    guint64 rx_drop;     /**< 전 NIC 누적 수신 드롭 수. "net.*.rx.drop" 합산. */
    guint64 tx_drop;     /**< 전 NIC 누적 송신 드롭 수. "net.*.tx.drop" 합산. */
} VmMetrics;


/* =========================================================
 * 2. 데몬 생명주기 API
 * ========================================================= */

/**
 * @brief 백그라운드 텔레메트리 데몬 스레드를 기동합니다.
 *
 * @param vm_manager (transfer none): 메트릭 캐시 교체 시 "vm-metrics-updated"
 *   신호를 발생시킬 PureCVisorVmManager 인스턴스.
 *   내부적으로 약한 참조(GWeakRef)를 보관하므로 소유권은 이전되지 않습니다.
 *
 * 서버 부팅 시 main.c 에서 단 1회 호출되어야 합니다.
 * 호출 직후 GThread가 생성되어 내부적으로 무한 루프를 돌며 캐시를 갱신합니다.
 *
 * [내부 동작 순서]
 *   1. vm_manager에 대한 GWeakRef 초기화 (소멸 안전)
 *   2. "telemetry-daemon" GThread 생성
 *   3. 스레드 내부: qemu:///system에 영구 Libvirt 커넥션 개방
 *   4. 스레드 내부: 무한 루프 — 1초마다 Bulk Stats 수집 → 캐시 교체
 *
 * [GWeakRef 사용 이유]
 *   vm_manager 객체가 데몬보다 먼저 소멸할 수 있다.
 *   g_weak_ref_get()은 이미 소멸된 객체에 대해 NULL을 반환하므로,
 *   시그널 emit을 안전하게 건너뛸 수 있다. (dangling pointer 방지)
 */
void init_telemetry_daemon(PureCVisorVmManager *vm_manager);


/* =========================================================
 * 3. 데이터 조회 API (Main Thread Only)
 * ========================================================= */

/**
 * @brief 특정 VM의 최신 통계 메트릭을 O(1) 속도로 조회합니다.
 *
 * @param vm_id 통계를 조회할 가상 머신의 UUID 문자열
 * @return VmMetrics 구조체 포인터. (캐시에 해당 VM이 없으면 NULL 반환)
 *
 * @warning [메모리 수명 주의]
 * 반환된 VmMetrics 포인터는 현재 메인 루프 사이클에서만 유효합니다.
 * 백그라운드 데몬이 1초 뒤 새로운 캐시를 밀어넣어(Push) 포인터를 스왑하면
 * 기존 메모리는 파괴되므로, 반환받은 데이터를 JSON 등으로 직렬화(Copy)한 뒤에는
 * 포인터를 오래 보관하지 마십시오. 호출자가 반환된 포인터를 g_free() 해서도 안 됩니다.
 *
 * @warning 반드시 싱글 스레드 이벤트 루프(Main Thread) 컨텍스트에서만 호출해야 안전합니다.
 */
VmMetrics* get_vm_metrics(const gchar *vm_id);

G_END_DECLS

#endif /* PURECVISOR_DAEMONS_TELEMETRY_H */