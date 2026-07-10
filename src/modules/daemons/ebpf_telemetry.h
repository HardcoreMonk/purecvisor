/**
 * @file ebpf_telemetry.h
 * @brief eBPF 스타일 심층 텔레메트리 공개 헤더
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  파일 역할
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   호스트 시스템(/proc 기반) + VM별 심층 메트릭(libvirt 기반)을
 *   5초 간격으로 수집하는 백그라운드 데몬의 공개 API를 선언합니다.
 *
 *   "eBPF"라는 이름이지만 실제로는 /proc 파싱 + libvirt 통계를 사용합니다.
 *   (향후 실제 eBPF kprobe/tracepoint 연동으로 확장 가능한 구조)
 *
 *   이 모듈은 PureCVisor의 자체 node_exporter 역할을 수행하여,
 *   별도의 node_exporter 바이너리 없이 65개 이상의 node_* 메트릭을
 *   Prometheus 형식으로 제공합니다.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  아키텍처 위치
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   main.c
 *     ├─ pcv_ebpf_telemetry_init()      데몬 부팅 시 스레드 시작
 *     └─ pcv_ebpf_telemetry_shutdown()  데몬 종료 시 스레드 join
 *
 *   handler_cluster.c (RPC 호출)
 *     ├─ "telemetry.host" → pcv_ebpf_telemetry_get_host()
 *     ├─ "telemetry.vm"   → pcv_ebpf_telemetry_get_vm(name)
 *     └─ "telemetry.all"  → pcv_ebpf_telemetry_get_all_vms()
 *
 *   rest_server.c (REST 호출)
 *     └─ GET /internal/telemetry → pcv_ebpf_telemetry_get_host()
 *        (클러스터 스케줄러가 원격 노드의 리소스를 조회할 때 사용)
 *
 *   alert_engine.c (알림 평가)
 *     └─ pcv_ebpf_telemetry_get_host() → CPU%, 메모리% 읽기
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  수집 메트릭 총괄 (10개 콜렉터)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   1. node_cpu_seconds_total{cpu,mode}  — 코어별 모드별 CPU 누적 초
 *   2. node_memory_*_bytes               — /proc/meminfo 전체 (60+필드)
 *   3. node_filesystem_*{device,mount}   — statvfs 파일시스템 용량/inode
 *   4. node_disk_*{device}               — /proc/diskstats 디바이스별 I/O
 *   5. node_network_*{device}            — /proc/net/dev 인터페이스별 I/O
 *   6. node_vmstat_*                     — pgfault/pswpin/oom_kill 등 7개
 *   7. node_sockstat_*                   — TCP/UDP 소켓 통계 8개
 *   8. node_pressure_*                   — PSI(cpu/io/memory) 5개
 *   9. node_hwmon_temp_celsius           — 하드웨어 온도 센서
 *  10. misc (boot_time, uptime, entropy, filefd, conntrack, ARP, NIC meta)
 *
 *   + 호스트 집계 메트릭 (HostMetrics 구조체): CPU%, 메모리, 로드, 네트워크, 디스크
 *   + VM별 심층 메트릭 (VmExtMetrics 배열): CPU/메모리/블록I/O/네트워크
 *   + keepalived VRRP 메트릭 3개: active, vip_owner, master
 *   + 커넥션 풀 메트릭 3개: connpool_idle, connpool_active, connpool_max
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  제공 API
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   init/shutdown : 데몬 생명주기 관리
 *   get_host()    : 호스트 CPU%/메모리/로드/네트워크/디스크 → JsonObject (caller unref)
 *                   반환 JSON에 33개 필드 포함 (CPU 9 + 메모리 12 + 로드 3 + 네트워크 8 + 디스크 5)
 *   get_vm(name)  : 특정 VM 심층 메트릭 → JsonObject (caller unref)
 *                   VM 미발견 시 {"error": "VM not found in telemetry cache"} 반환
 *   get_all_vms() : 전체 VM 심층 메트릭 배열 → JsonArray (caller unref)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  스레드 안전성
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - 모든 get_* 함수는 내부 GMutex 잠금 후 JsonObject를 새로 생성하여 반환
 *   - 반환된 JsonObject/JsonArray의 소유권은 호출자에게 이전 (unref 필요)
 *   - 스레드 안전: 어떤 스레드에서든 호출 가능 (내부 Mutex 보호)
 *   - telemetry.c(Lock-Free)와 달리 이 모듈은 Mutex 방식 사용
 *     (5초 간격이라 경합 빈도가 낮으므로 단순한 Mutex로 충분)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  주의사항
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - 내부 최대 VM 수 제한: EBPF_MAX_VMS(64). 초과 시 나머지 VM은 수집 제외.
 *   - 첫 번째 CPU 수집 시에는 delta 기준점이 없어 cpu_percent가 0.0.
 *     두 번째 수집(+5초)부터 유효한 CPU% 값이 계산됨.
 *   - /proc/pressure/ (PSI)는 커널 4.20+ 에서만 사용 가능.
 *     파일이 없으면 조용히 스킵.
 */

#ifndef PURECVISOR_EBPF_TELEMETRY_H
#define PURECVISOR_EBPF_TELEMETRY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

static inline gboolean
pcv_ebpf_proc_stat_is_cpu_core_line(const gchar *line)
{
    return line &&
           line[0] == 'c' &&
           line[1] == 'p' &&
           line[2] == 'u' &&
           g_ascii_isdigit((guchar)line[3]);
}

/**
 * @brief eBPF 텔레메트리 모듈을 초기화하고 수집 스레드("ebpf-telem")를 시작한다.
 *
 * main.c에서 데몬 시작 시 1회 호출. GMutex 초기화 + GThread 생성.
 * 호출 후 5초 뒤부터 유효한 CPU% 데이터가 수집된다.
 */
void        pcv_ebpf_telemetry_init(void);

/**
 * @brief eBPF 텔레메트리 모듈을 종료하고 수집 스레드를 정지시킨다.
 *
 * G.running=FALSE → g_thread_join() → GMutex 해제.
 * main.c에서 데몬 종료(drain) 시 1회 호출.
 */
void        pcv_ebpf_telemetry_shutdown(void);

/* ── 캐시된 메트릭 조회 API (스레드 안전) ── */

/**
 * @brief 호스트 전체 메트릭을 JsonObject로 반환한다.
 *
 * 반환 JSON에 33개 필드 포함 (CPU 9개 모드별 % + 메모리 12개 kB/% +
 * 로드 3개 + 네트워크 8개 + 디스크 5개).
 *
 * @return JsonObject* — 호출자가 소유권을 받음. 사용 후 json_object_unref() 필요.
 *
 * 호출 컨텍스트: 임의 스레드. 내부 G.mu 락으로 보호됨.
 */
JsonObject *pcv_ebpf_telemetry_get_host(void);

/**
 * @brief 특정 VM의 확장 메트릭을 JsonObject로 반환한다.
 *
 * @param vm_name  조회할 VM 이름 (예: "web-prod")
 * @return JsonObject* — 호출자가 소유권을 받음. json_object_unref() 필요.
 *         VM 미발견 시: {"error": "VM not found in telemetry cache"}
 */
JsonObject *pcv_ebpf_telemetry_get_vm(const gchar *vm_name);

/**
 * @brief 모든 VM의 확장 메트릭을 JsonArray로 반환한다.
 *
 * @return JsonArray* — 호출자가 소유권을 받음. json_array_unref() 필요.
 *         VM이 없으면 빈 배열 반환.
 */
JsonArray  *pcv_ebpf_telemetry_get_all_vms(void);

G_END_DECLS

#endif /* PURECVISOR_EBPF_TELEMETRY_H */
