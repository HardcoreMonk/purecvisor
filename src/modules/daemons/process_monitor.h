/**
 * @file process_monitor.h
 * @brief WhaTap 스타일 프로세스 모니터링 — /proc/[pid] 스캔 (20초 주기)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  아키텍처 위치
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   main.c
 *     └─ pcv_process_monitor_init()     데몬 부팅 시 백그라운드 스레드 시작
 *     └─ pcv_process_monitor_shutdown() 데몬 종료 시 스레드 join + 자원 해제
 *
 *   dispatcher.c (RPC 호출)
 *     └─ "monitor.metrics" 핸들러에서 pcv_process_monitor_get_top(n) 호출
 *        → 상위 N개 프로세스 정보를 JsonArray로 반환
 *
 *   rest_server.c (REST 호출)
 *     └─ GET/POST /api/v1/monitor/metrics → dispatcher를 경유하여 동일 API 사용
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  설계 개요 (WhaTap 프로세스 모니터링에서 차용한 개념)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   WhaTap APM의 프로세스 모니터링 에이전트처럼 아래 3가지를 주기적으로 수집:
 *     1) CPU% — 이전 샘플과 현재 샘플 사이의 tick(utime+stime) 변화량 기반
 *     2) MEM  — RSS(Resident Set Size) 페이지 수 → MB 변환
 *     3) I/O  — /proc/[pid]/io의 누적 read_bytes / write_bytes
 *
 *   수집 주기: 20초 (PROC_INTERVAL_SEC).
 *   WhaTap이 5초~30초 주기를 사용하는 것과 유사하게, 시스템 부하를 최소화
 *   하면서도 운영자에게 의미 있는 CPU% 델타를 제공할 수 있는 간격으로 설정.
 *
 *   Top N: get_top(n)으로 CPU% 기준 상위 N개만 추출 가능 — 대시보드에서
 *   가장 CPU를 많이 쓰는 프로세스를 빠르게 파악하는 용도.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  스레드 모델
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - GThread("proc-monitor"): 전용 백그라운드 스레드가 20초마다 /proc 스캔
 *   - GMutex(G.mu): 프로세스 목록(G.procs[]) 읽기/쓰기 시 상호 배제
 *   - get_top() / get_all()은 뮤텍스 잠금 후 JsonArray로 복사하여 반환
 *     → 호출자는 락 없이 안전하게 JsonArray를 사용/해제 가능
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  주의사항
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - /proc/[pid]/io 읽기에는 root 권한 또는 CAP_SYS_PTRACE가 필요.
 *     권한 부족 시 io_rd_bytes / io_wr_bytes가 0으로 남음 (정상 동작).
 *   - 최대 512개 프로세스만 수집 (PROC_MAX). 초과 시 나머지는 무시됨.
 *   - 첫 번째 수집 시에는 이전 tick이 없으므로 cpu_percent가 0.0.
 *     두 번째 수집(+20초)부터 유효한 CPU% 값이 계산됨.
 *   - 반환된 JsonArray의 소유권은 호출자에게 이전됨.
 *     사용 후 반드시 json_array_unref()로 해제해야 메모리 누수 방지.
 *
 * [제공 API]
 *   pcv_process_monitor_init()     — 데몬 부팅 시 호출 (스레드 생성 + 초기화)
 *   pcv_process_monitor_shutdown() — 종료 시 스레드 join + GHashTable 해제
 *   pcv_process_monitor_get_top(n) — CPU% 기준 상위 N개 프로세스 JsonArray 반환
 *   pcv_process_monitor_get_all()  — 전체 프로세스 목록 JsonArray 반환
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  사용 예시
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  [RPC Top 10 프로세스 조회]
 *    echo '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":10},"id":"1"}'
 *      | nc -U /var/run/purecvisor/daemon.sock
 *
 *  [REST API 전체 프로세스 조회]
 *    curl -s -H "Authorization: Bearer $TOKEN" http://localhost:80/api/v1/processes
 *
 *  [반환 JSON 예시]
 *    [
 *      {"pid":1234, "comm":"qemu-system-x86", "state":"S",
 *       "cpu_percent":25.3, "mem_mb":2048.0, "rss_kb":2097152,
 *       "io_rd_bytes":1234567, "io_wr_bytes":9876543},
 *      {"pid":5678, "comm":"purecvisormd", "state":"S",
 *       "cpu_percent":1.2, "mem_mb":45.6, "rss_kb":46764,
 *       "io_rd_bytes":456789, "io_wr_bytes":123456}
 *    ]
 */
#ifndef PURECVISOR_PROCESS_MONITOR_H
#define PURECVISOR_PROCESS_MONITOR_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * @brief 프로세스 모니터 초기화 — 백그라운드 스레드("proc-monitor") 시작
 *
 * main.c의 데몬 초기화 단계에서 한 번만 호출한다.
 * 내부적으로 sysconf()를 통해 PAGE_SIZE와 CLK_TCK를 캐싱하고,
 * 이전 tick 저장용 GHashTable을 생성한 뒤 수집 스레드를 기동한다.
 *
 * 호출 후 약 20초 뒤부터 유효한 CPU% 데이터가 수집된다.
 */
void        pcv_process_monitor_init(void);

/**
 * @brief 프로세스 모니터 종료 — 스레드 join + 자원 해제
 *
 * main.c의 데몬 종료 단계에서 호출한다.
 * G.running = FALSE로 설정하여 스레드 루프를 탈출시키고,
 * g_thread_join()으로 스레드 종료를 대기한 뒤 GHashTable과 GMutex를 해제한다.
 *
 * 이미 shutdown 되었거나 init이 호출되지 않은 경우 안전하게 무시한다.
 */
void        pcv_process_monitor_shutdown(void);

/**
 * @brief CPU% 기준 상위 N개 프로세스 정보를 JsonArray로 반환
 *
 * @param n  반환할 프로세스 수 (양수). 전체 수보다 크면 전체를 반환.
 * @return   JsonArray* — 각 요소는 JsonObject로 아래 필드를 포함:
 *           - "pid"         (gint64)  : 프로세스 ID
 *           - "comm"        (string)  : 프로세스 이름 (예: "purecvisorsd", "purecvisormd")
 *           - "state"       (string)  : 프로세스 상태 문자 ("S"=sleeping, "R"=running 등)
 *           - "cpu_percent" (double)  : CPU 사용률 (%) — 20초 간격 delta 기반
 *           - "mem_mb"      (double)  : RSS 메모리 사용량 (MB)
 *           - "rss_kb"      (gint64)  : RSS 메모리 사용량 (KB)
 *           - "io_rd_bytes" (gint64)  : 누적 디스크 읽기 바이트
 *           - "io_wr_bytes" (gint64)  : 누적 디스크 쓰기 바이트
 *
 * @note     소유권이 호출자에게 이전됨 — 사용 후 json_array_unref() 필수.
 * @note     뮤텍스 잠금 → 복사 → 해제 순서로 동작하므로 스레드 안전.
 */
JsonArray  *pcv_process_monitor_get_top(gint n);

/**
 * @brief 수집된 전체 프로세스 목록을 JsonArray로 반환
 *
 * 내부적으로 pcv_process_monitor_get_top(G.count)를 호출하여
 * CPU% 기준 내림차순 정렬된 전체 목록을 반환한다.
 *
 * @return   JsonArray* — get_top()과 동일 형식. json_array_unref() 필수.
 */
JsonArray  *pcv_process_monitor_get_all(void);

/**
 * @brief 프로세스 유형 필터를 적용하여 상위 N개를 JsonArray로 반환
 *
 * type_str이 유효한 유형("host"/"vm"/"container"/"system")이면
 * 해당 유형의 프로세스만 필터링하여 반환한다.
 * 반환 JSON에는 기존 필드 + "type"(string) + "cgroup"(string) 필드가 포함.
 *
 * @param n         반환할 최대 프로세스 수 (0이면 제한 없음)
 * @param type_str  프로세스 유형 필터 ("host"/"vm"/"container"/"system", NULL이면 필터 없음)
 * @return JsonArray* — 소유권 호출자 이전. json_array_unref() 필수.
 */
JsonArray  *pcv_process_monitor_get_filtered(gint n, const gchar *type_str);

G_END_DECLS

#endif /* PURECVISOR_PROCESS_MONITOR_H */
