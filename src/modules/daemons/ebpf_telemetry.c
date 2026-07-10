/**
 * @file ebpf_telemetry.c
 * @brief eBPF 스타일 심층 텔레메트리 (5초 간격 백그라운드 수집)
 *
 * [파일 역할]
 *   telemetry.c가 VM 기본 메트릭(CPU time, net RX/TX)만 제공하는 반면,
 *   이 모듈은 호스트 시스템 전체 + VM별 심층 메트릭(디스크 I/O, 메모리 balloon,
 *   네트워크 패킷 수 등)을 5초 간격으로 수집합니다.
 *   "eBPF"라는 이름이지만 실제로는 /proc 파싱 + libvirt 통계를 사용합니다.
 *   (향후 실제 eBPF kprobe 연동으로 확장 가능한 구조)
 *
 * [아키텍처 위치]
 *   main.c -> pcv_ebpf_telemetry_init()           [이 파일]
 *     -> GThread("ebpf-telem")                    [5초 간격 수집 루프]
 *   handler_cluster.c (telemetry.host RPC)
 *     -> pcv_ebpf_telemetry_get_host()            [이 파일]
 *   handler_cluster.c (telemetry.vm RPC)
 *     -> pcv_ebpf_telemetry_get_vm(name)          [이 파일]
 *   handler_cluster.c (telemetry.all RPC)
 *     -> pcv_ebpf_telemetry_get_all_vms()         [이 파일]
 *   rest_server.c (GET /internal/telemetry)
 *     -> pcv_ebpf_telemetry_get_host()            [클러스터 스케줄러용]
 *
 * [수집 대상 메트릭]
 *   호스트 (/proc 파싱):
 *     - CPU 사용률 %: /proc/stat (user+sys+irq+softirq / total delta)
 *     - 메모리: /proc/meminfo (MemTotal, MemAvailable)
 *     - Load average 1분: /proc/loadavg
 *     - 네트워크 I/O: /proc/net/dev (lo 제외 전 인터페이스 합산)
 *   VM별 (virConnectGetAllDomainStats):
 *     - CPU time (ns) + 상태
 *     - 메모리 balloon (max/current KB)
 *     - 블록 디바이스 I/O (rd/wr bytes, reqs) — 전 디스크 합산
 *     - 네트워크 I/O (rx/tx bytes, pkts) — 전 NIC 합산
 *
 * [핵심 패턴 — GMutex 보호 공유 상태]
 *   - 전역 구조체 G에 host 메트릭 + VM 배열(최대 64개)을 보관
 *   - 백그라운드 스레드가 G.mu 잠금 하에 데이터를 갱신
 *   - 조회 API도 G.mu 잠금 하에 JsonObject를 생성하여 반환
 *   - telemetry.c(Lock-Free)와 달리 이 모듈은 Mutex 방식 사용
 *     (5초 간격이라 경합 빈도가 낮으므로 단순한 Mutex로 충분)
 *
 * [주의사항]
 *   - EBPF_MAX_VMS(64)를 초과하는 VM은 수집에서 제외됨
 *   - _collect_vm_metrics()는 매 호출마다 virConnectOpen/Close를 수행
 *     (5초 간격이므로 커넥션 유지 대비 리소스 절약 선택)
 *   - CPU 사용률은 이전 샘플과의 delta로 계산 (첫 샘플은 0%)
 *   - shutdown() 호출 시 g_thread_join()으로 스레드 종료 대기
 */
#include "ebpf_telemetry.h"
#include <libvirt/libvirt.h>
#include <string.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#include "utils/pcv_log.h"
#include "prometheus_exporter.h"
#include "modules/ai/anomaly_detector.h"
#include "modules/ai/workload_predict.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/virt/circuit_breaker.h"
#include "modules/core/vm_state.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_tls.h"
#include "utils/pcv_worker_pool.h"
#include "modules/audit/pcv_audit.h"
#include "modules/storage/zfs_driver.h"

/*
 * ============================================================================
 *  [주니어 개발자 필독] eBPF 텔레메트리 핵심 개념
 * ============================================================================
 *
 *  이 모듈은 "eBPF"라는 이름이지만 실제로는 /proc 파싱 + libvirt 통계를
 *  사용합니다. 향후 실제 eBPF kprobe 연동으로 확장 가능합니다.
 *
 *  telemetry.c(1초, Lock-Free)와 달리 이 모듈은 5초 간격으로 수집하며
 *  GMutex로 동기화합니다. 5초 간격이라 경합 빈도가 낮아 Mutex로 충분합니다.
 *
 *  CPU 사용률 계산: 이전 샘플과 현재 샘플의 /proc/stat 차이(delta)로
 *  순간 사용률을 계산합니다. 첫 샘플은 비교 대상이 없어 0%입니다.
 *
 *  Prometheus 연동: 수집된 호스트 메트릭은 node_* 접두사로 126개
 *  Prometheus 게이지에 push됩니다 (별도 node_exporter 불필요).
 * ============================================================================
 */

#define EBPF_LOG_DOM       "ebpf_telem"
#define EBPF_INTERVAL_SEC  5
#define EBPF_MAX_VMS       64

/* ── per-VM 확장 메트릭 구조체 ────────────────────────────────── */

/**
 * @struct VmExtMetrics
 * @brief 단일 VM의 심층 리소스 메트릭 (telemetry.c의 VmMetrics보다 상세)
 *
 * telemetry.c의 VmMetrics는 CPU time + Net RX/TX 3개 필드만 제공하지만,
 * 이 구조체는 메모리 Balloon, 블록 디스크 I/O, 네트워크 패킷 수까지 포함한다.
 *
 * 데이터 소스: virConnectGetAllDomainStats() Bulk API (5초 간격)
 *
 * [Balloon 메모리 필드 설명]
 *   mem_max_kb  — VM에 할당된 최대 메모리 (virt-install --memory 값)
 *   mem_used_kb — 현재 balloon 드라이버가 보고하는 사용 중 메모리
 *                 (게스트 OS에 virtio-balloon 드라이버가 필요)
 *                 드라이버 미설치 시 0으로 보고될 수 있음.
 *
 * [블록/네트워크 합산]
 *   VM에 디스크가 2개(vda, vdb) 있으면 block.0 + block.1의 바이트를 합산.
 *   VM에 NIC가 2개 있으면 net.0 + net.1의 바이트를 합산.
 *   인덱스에 무관하게 g_str_has_prefix + g_str_has_suffix로 매칭.
 */
typedef struct {
    gchar    name[64];        /**< VM 이름 (예: "web-prod"). virDomainGetName()으로 취득. */
    /* CPU */
    gdouble  cpu_percent;     /**< CPU 사용률 (%). 현재 미사용 — 향후 delta 계산용 예약. */
    guint64  cpu_time_ns;     /**< 누적 CPU 시간 (나노초). libvirt param: "cpu.time" */
    /* Memory — Balloon 기반 */
    guint64  mem_max_kb;      /**< VM 최대 메모리 (kB). libvirt param: "balloon.maximum" */
    guint64  mem_used_kb;     /**< VM 현재 메모리 (kB). libvirt param: "balloon.current" */
    /* Disk I/O — 전 디스크 합산 */
    guint64  disk_rd_bytes;   /**< 누적 디스크 읽기 바이트. "block.*.rd.bytes" 합산 */
    guint64  disk_wr_bytes;   /**< 누적 디스크 쓰기 바이트. "block.*.wr.bytes" 합산 */
    guint64  disk_rd_reqs;    /**< 누적 디스크 읽기 요청 수. "block.*.rd.reqs" 합산 */
    guint64  disk_wr_reqs;    /**< 누적 디스크 쓰기 요청 수. "block.*.wr.reqs" 합산 */
    /* Network I/O — 전 NIC 합산 */
    guint64  net_rx_bytes;    /**< 누적 네트워크 수신 바이트. "net.*.rx.bytes" 합산 */
    guint64  net_tx_bytes;    /**< 누적 네트워크 송신 바이트. "net.*.tx.bytes" 합산 */
    guint64  net_rx_pkts;     /**< 누적 수신 패킷 수. "net.*.rx.pkts" 합산 */
    guint64  net_tx_pkts;     /**< 누적 송신 패킷 수. "net.*.tx.pkts" 합산 */
    /* State */
    gint     state;           /**< VM 상태. libvirt param: "state.state"
                               *   1=running, 2=blocked, 3=paused, 4=shutdown,
                               *   5=shutoff, 6=crashed, 7=pmsuspended */
} VmExtMetrics;

/* ── host metrics ─────────────────────────────────────────────── */
/*
 * [W-1 추가] HostMetrics — 호스트 전체의 CPU/메모리/네트워크/디스크 메트릭을 보관하는 구조체.
 *
 * 모든 필드는 5초 간격 백그라운드 스레드(_ebpf_thread)에서 갱신되며,
 * 조회 시 G.mu GMutex 잠금 하에 읽어 JSON으로 변환한다.
 *
 * 데이터 소스 요약:
 *   CPU     → /proc/stat         (jiffies 누적값의 delta로 백분율 계산)
 *   Memory  → /proc/meminfo      (kB 단위 절대값)
 *   Swap    → /proc/meminfo      (kB 단위)
 *   PgFault → /proc/vmstat       (부팅 이후 누적 카운트)
 *   Load    → /proc/loadavg      (소수점 실수)
 *   Network → /proc/net/dev      (바이트/패킷/에러/드롭 — 부팅 이후 누적)
 *   Disk    → /proc/diskstats    (섹터→바이트 변환, whole-disk만 집계)
 */

typedef struct {
    /*
     * CPU — 전체(aggregate) + 모드별 백분율 (WhaTap 스타일 분해)
     * 데이터 소스: /proc/stat "cpu" 행 (user nice system idle iowait irq softirq steal)
     * 단위: % (0.0 ~ 100.0)
     * 계산: _collect_host_cpu()에서 이전 샘플과의 delta 비율로 산출
     */
    gdouble  cpu_percent;       /* 전체 CPU 사용률 = 100 - idle% - iowait% */
    gdouble  cpu_user;          /* 사용자 공간 코드 실행 시간 비율 (%) — /proc/stat 1번째 필드 */
    gdouble  cpu_system;        /* 커널 공간 코드 실행 시간 비율 (%) — /proc/stat 3번째 필드 */
    gdouble  cpu_nice;          /* nice 우선순위 조정된 사용자 프로세스 비율 (%) — /proc/stat 2번째 필드 */
    gdouble  cpu_iowait;        /* I/O 대기 시간 비율 (%) — /proc/stat 5번째 필드, 디스크 병목 지표 */
    gdouble  cpu_steal;         /* 하이퍼바이저에 의해 빼앗긴 시간 비율 (%) — /proc/stat 8번째 필드, VM 환경 지표 */
    gdouble  cpu_irq;           /* 하드웨어 인터럽트 처리 시간 비율 (%) — /proc/stat 6번째 필드 */
    gdouble  cpu_softirq;       /* 소프트 인터럽트(네트워크/타이머) 처리 시간 비율 (%) — /proc/stat 7번째 필드 */
    gdouble  cpu_idle;          /* 유휴 시간 비율 (%) — /proc/stat 4번째 필드 */

    /*
     * Memory — 확장 필드 (Slab/Swap/PageFault 포함)
     * 데이터 소스: /proc/meminfo + /proc/vmstat
     * 단위: kB (킬로바이트) — 커널이 kB로 출력하므로 그대로 저장
     */
    guint64  mem_total_kb;      /* 전체 물리 메모리 (kB) — /proc/meminfo "MemTotal" */
    guint64  mem_avail_kb;      /* 스왑 없이 즉시 사용 가능한 메모리 (kB) — /proc/meminfo "MemAvailable" */
    guint64  mem_free_kb;       /* 완전히 미사용 메모리 (kB) — /proc/meminfo "MemFree" (캐시 제외) */
    guint64  mem_buffers_kb;    /* 블록 디바이스 I/O 버퍼 (kB) — /proc/meminfo "Buffers" */
    guint64  mem_cached_kb;     /* 페이지 캐시 (kB) — /proc/meminfo "Cached" (tmpfs 포함) */
    guint64  mem_slab_kb;       /* 커널 슬랩 할당자 사용량 (kB) — /proc/meminfo "Slab" (dentry/inode 캐시 등) */
    guint64  mem_sreclaimable_kb;/* 슬랩 중 회수 가능한 부분 (kB) — /proc/meminfo "SReclaimable" */
    guint64  swap_total_kb;     /* 전체 스왑 크기 (kB) — /proc/meminfo "SwapTotal" */
    guint64  swap_free_kb;      /* 미사용 스왑 (kB) — /proc/meminfo "SwapFree" */
    guint64  pgfault;           /* 마이너 페이지 폴트 누적 횟수 — /proc/vmstat "pgfault" (부팅 이후) */
    guint64  pgmajfault;        /* 메이저 페이지 폴트 누적 횟수 — /proc/vmstat "pgmajfault" (디스크 I/O 수반) */

    /*
     * Load average — 1분/5분/15분 평균 실행 대기 프로세스 수
     * 데이터 소스: /proc/loadavg
     * 단위: 무차원 실수 (코어 수 대비 비교하여 부하 판단)
     */
    gdouble  load_1m;
    gdouble  load_5m;           /* [W-1 추가] 5분 로드 — 중기 부하 추세 확인 */
    gdouble  load_15m;          /* [W-1 추가] 15분 로드 — 장기 부하 추세 확인 */

    /*
     * Network I/O — 에러/드롭 포함 (WhaTap 패리티)
     * 데이터 소스: /proc/net/dev (lo 제외 전 인터페이스 합산)
     * 단위: bytes / packets / errors / drops — 부팅 이후 누적 카운터
     */
    guint64  net_rx_bytes;
    guint64  net_tx_bytes;
    guint64  net_rx_packets;    /* [W-1 추가] 수신 패킷 수 — /proc/net/dev RX 2번째 필드 */
    guint64  net_tx_packets;    /* [W-1 추가] 송신 패킷 수 — /proc/net/dev TX 2번째 필드 */
    guint64  net_rx_errs;       /* [W-1 추가] 수신 에러 수 — /proc/net/dev RX 3번째 필드 (CRC 등) */
    guint64  net_tx_errs;       /* [W-1 추가] 송신 에러 수 — /proc/net/dev TX 3번째 필드 */
    guint64  net_rx_drop;       /* [W-1 추가] 수신 드롭 수 — /proc/net/dev RX 4번째 필드 (버퍼 부족 등) */
    guint64  net_tx_drop;       /* [W-1 추가] 송신 드롭 수 — /proc/net/dev TX 4번째 필드 */

    /*
     * Disk I/O — 호스트 전체 합산 (WhaTap 패리티)
     * 데이터 소스: /proc/diskstats (whole-disk만 필터링)
     * 필터 규칙: sd[a-z] 3글자, vd[a-z] 3글자, nvme*n*에 'p'없는 것만 (파티션 제외)
     * 단위: bytes (섹터×512 변환), I/O 횟수, ms (io_ticks)
     */
    guint64  disk_rd_bytes;     /* [W-1 추가] 디스크 읽기 바이트 — rd_sectors × 512 */
    guint64  disk_wr_bytes;     /* [W-1 추가] 디스크 쓰기 바이트 — wr_sectors × 512 */
    guint64  disk_rd_ios;       /* [W-1 추가] 디스크 읽기 I/O 횟수 — /proc/diskstats 4번째 필드 */
    guint64  disk_wr_ios;       /* [W-1 추가] 디스크 쓰기 I/O 횟수 — /proc/diskstats 8번째 필드 */
    guint64  disk_io_ticks_ms;  /* [W-1 추가] I/O에 소요된 총 시간 (ms) — /proc/diskstats 13번째 필드 */
} HostMetrics;

/* ── 모듈 전역 상태 ──────────────────────────────────────────── */

/**
 * @brief eBPF 텔레메트리 모듈의 전역 상태
 *
 * 단일 정적 구조체(G)에 모든 상태를 모아 관리한다.
 * {0}으로 zero-initialize되므로 init 호출 전 모든 필드가 0/NULL/FALSE.
 *
 * 스레드 안전성:
 *   - thread, running, initialized: init/shutdown에서만 변경 (메인 스레드 단독)
 *   - vms[], vm_count, host: G.mu 락 하에서만 읽기/쓰기
 *   - Prometheus 레지스트리 push: prometheus_exporter.c 내부 락으로 별도 보호됨
 */
static struct {
    GThread      *thread;                  /**< "ebpf-telem" 수집 스레드 핸들 */
    gboolean      running;                 /**< 스레드 루프 제어 플래그 */
    VmExtMetrics  vms[EBPF_MAX_VMS];       /**< VM별 확장 메트릭 배열 (최대 64개) */
    gint          vm_count;                /**< 현재 수집된 VM 수 */
    HostMetrics   host;                    /**< 호스트 전체 메트릭 (5초마다 갱신) */
    GMutex        mu;                      /**< vms[]/host 동시 접근 보호용 뮤텍스 */
    gboolean      initialized;            /**< init() 호출 여부 (이중 shutdown 방지) */
} G = {0};

/* ── host metric collectors ───────────────────────────────────── */

/*
 * _collect_host_cpu() — /proc/stat 파싱으로 CPU 모드별 사용률(%) 계산
 *
 * [원리]
 *   /proc/stat의 "cpu" 행은 부팅 이후 누적 jiffies를 8개 모드로 출력한다:
 *     user  nice  system  idle  iowait  irq  softirq  steal
 *   (인덱스 0~7)
 *
 *   CPU 사용률은 "순간값"이 아니라 두 시점 사이의 delta 비율로 계산해야 한다.
 *   따라서 static 변수 prev[8]에 이전 샘플을 보관하고, 현재 - 이전의 차이(dt)를
 *   분모로 사용하여 각 모드의 백분율을 산출한다.
 *
 * [prev[8] delta 배열 사용법]
 *   - prev[0~7]: 이전 호출 시 읽은 8개 모드의 누적 jiffies
 *   - prev_total: 이전 호출 시 8개 모드의 합계
 *   - 최초 호출(prev_total==0): delta를 구할 수 없으므로 0%를 반환하고 현재값을 prev에 저장
 *   - 이후 호출: cur[i] - prev[i]가 해당 모드의 delta jiffies
 *
 * [_CPU_PCT 매크로 원리]
 *   _CPU_PCT(idx) = 100.0 * (cur[idx] - prev[idx]) / dt
 *   → dt(전체 delta jiffies) 대비 해당 모드의 delta를 백분율로 변환
 *   → 모든 모드의 합은 항상 100%에 수렴
 *
 * [cpu_percent 공식]
 *   cpu_percent = 100 - idle% - iowait%
 *   → idle은 완전 유휴, iowait는 I/O 대기(CPU가 놀고 있음)
 *   → 이 둘을 빼면 "실제 CPU를 사용한 비율"이 된다
 *   → WhaTap/Datadog 등 APM 도구의 표준 공식과 동일
 */
static void
_collect_host_cpu(HostMetrics *h)
{
    static guint64 prev[8] = {0};  /* user,nice,sys,idle,iowait,irq,softirq,steal */
    static guint64 prev_total = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    char buf[256];
    if (fgets(buf, sizeof(buf), f)) {
        unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
        if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) == 8) {
            guint64 cur[8] = {user, nice, sys, idle, iowait, irq, softirq, steal};
            guint64 total = user + nice + sys + idle + iowait + irq + softirq + steal;

            if (prev_total == 0) {
                /* 최초 샘플: delta를 구할 수 없으므로 0% 반환, 현재값만 저장 */
                prev_total = total;
                memcpy(prev, cur, sizeof(prev));
                h->cpu_percent = 0.0;
            } else {
                guint64 dt = total - prev_total;  /* 전체 delta jiffies (분모) */
                if (dt > 0) {
                    /* _CPU_PCT: 해당 모드의 delta jiffies / 전체 delta × 100 → 백분율 */
#define _CPU_PCT(idx) (100.0 * (gdouble)(cur[idx] - prev[idx]) / (gdouble)dt)
                    h->cpu_user    = _CPU_PCT(0);  /* idx 0 = user */
                    h->cpu_nice    = _CPU_PCT(1);  /* idx 1 = nice */
                    h->cpu_system  = _CPU_PCT(2);  /* idx 2 = system */
                    h->cpu_idle    = _CPU_PCT(3);  /* idx 3 = idle */
                    h->cpu_iowait  = _CPU_PCT(4);  /* idx 4 = iowait */
                    h->cpu_irq     = _CPU_PCT(5);  /* idx 5 = irq */
                    h->cpu_softirq = _CPU_PCT(6);  /* idx 6 = softirq */
                    h->cpu_steal   = _CPU_PCT(7);  /* idx 7 = steal */
#undef _CPU_PCT
                    /* 실제 CPU 사용률 = 전체(100%) - 유휴(idle) - I/O 대기(iowait) */
                    h->cpu_percent = 100.0 - h->cpu_idle - h->cpu_iowait;
                }
                prev_total = total;
                memcpy(prev, cur, sizeof(prev));
            }
        }
    }
    fclose(f);
}

/*
 * _collect_host_mem() — /proc/meminfo + /proc/vmstat 파싱
 *
 * [/proc/meminfo 파싱 필드] (모두 kB 단위)
 *   기존 필드:
 *     MemTotal     — 전체 물리 RAM
 *     MemAvailable — 스왑 없이 할당 가능한 메모리 (커널 3.14+)
 *   W-1 추가 필드:
 *     MemFree      — 완전 미사용 메모리 (MemAvailable과 다름: 캐시/버퍼 미포함)
 *     Buffers      — raw 블록 디바이스 I/O 버퍼 (메타데이터 캐시)
 *     Cached       — 페이지 캐시 (파일 내용 캐시, tmpfs 포함)
 *     Slab         — 커널 슬랩 할당자 총 사용량 (dentry/inode 캐시 등)
 *     SReclaimable — Slab 중 메모리 압박 시 회수 가능한 부분
 *     SwapTotal    — 전체 스왑 공간
 *     SwapFree     — 미사용 스왑 공간
 *
 * [/proc/vmstat 파싱 필드] (부팅 이후 누적 카운트)
 *   pgfault    — 마이너 페이지 폴트 (메모리 내에서 해결, 디스크 I/O 없음)
 *   pgmajfault — 메이저 페이지 폴트 (디스크에서 페이지를 읽어야 함 — 성능 영향 큼)
 *   두 값의 급증은 메모리 부족 또는 워킹셋 초과를 의미한다.
 */
static void
_collect_host_mem(HostMetrics *h)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        guint64 val;
        if (sscanf(line, "MemTotal: %lu kB", &val) == 1)
            h->mem_total_kb = val;
        else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1)
            h->mem_avail_kb = val;
        else if (sscanf(line, "MemFree: %lu kB", &val) == 1)
            h->mem_free_kb = val;                  /* [W-1] 완전 미사용 메모리 */
        else if (sscanf(line, "Buffers: %lu kB", &val) == 1)
            h->mem_buffers_kb = val;               /* [W-1] 블록 디바이스 버퍼 */
        else if (sscanf(line, "Cached: %lu kB", &val) == 1)
            h->mem_cached_kb = val;                /* [W-1] 페이지 캐시 */
        else if (sscanf(line, "Slab: %lu kB", &val) == 1)
            h->mem_slab_kb = val;                  /* [W-1] 커널 슬랩 할당자 */
        else if (sscanf(line, "SReclaimable: %lu kB", &val) == 1)
            h->mem_sreclaimable_kb = val;          /* [W-1] 회수 가능 슬랩 */
        else if (sscanf(line, "SwapTotal: %lu kB", &val) == 1)
            h->swap_total_kb = val;                /* [W-1] 전체 스왑 */
        else if (sscanf(line, "SwapFree: %lu kB", &val) == 1)
            h->swap_free_kb = val;                 /* [W-1] 미사용 스왑 */
    }
    fclose(f);

    /*
     * [W-1] 페이지 폴트 — /proc/vmstat에서 파싱
     * pgfault: 마이너(디스크 I/O 없음), pgmajfault: 메이저(디스크 I/O 수반)
     */
    f = fopen("/proc/vmstat", "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        guint64 val;
        if (sscanf(line, "pgfault %lu", &val) == 1)
            h->pgfault = val;
        else if (sscanf(line, "pgmajfault %lu", &val) == 1)
            h->pgmajfault = val;
    }
    fclose(f);
}

/**
 * @brief /proc/loadavg 파싱으로 로드 평균(1/5/15분)을 수집한다.
 *
 * /proc/loadavg 형식: "0.12 0.34 0.56 1/234 5678"
 *   앞 3개 필드가 각각 1분/5분/15분 로드 평균.
 *   로드 평균 = 실행 대기 중인 프로세스 수의 지수 이동 평균.
 *   코어 수 대비 비교하여 시스템 부하를 판단한다.
 *   (예: 4코어 시스템에서 load_1m=4.0이면 CPU 포화 상태)
 *
 * @param h  결과를 저장할 HostMetrics 포인터 (load_1m/5m/15m 갱신)
 */
static void
_collect_host_load(HostMetrics *h)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return;
    double l1, l5, l15;
    if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
        h->load_1m  = l1;
        h->load_5m  = l5;
        h->load_15m = l15;
    }
    fclose(f);
}

/*
 * _collect_host_net() — /proc/net/dev 파싱으로 호스트 네트워크 I/O 수집
 *
 * [/proc/net/dev 형식] (각 인터페이스당 1행, 헤더 2행 스킵)
 *   인터페이스:  RX(8필드)                          TX(8필드)
 *               bytes packets errs drop fifo frame compressed multicast
 *                                                    bytes packets errs drop fifo colls carrier compressed
 *
 * [sscanf 파싱 매핑]
 *   RX측: rb(bytes) rp(packets) re(errs) rd(drop) — 1~4번째 필드
 *   TX측: tb(bytes) tp(packets) te(errs) td(drop) — 9~12번째 필드
 *   RX 5~8번째(fifo/frame/compressed/multicast)와
 *   TX 5~8번째(fifo/colls/carrier/compressed)는 %*u로 스킵
 *
 * [집계 방식]
 *   lo(루프백) 제외, 나머지 전 인터페이스의 값을 합산한다.
 *   (eno1, eno2, pcvbr0, pcvoverlay0 등 모두 포함)
 *
 * [W-1 추가 필드]
 *   rx_packets/tx_packets — 패킷 수 (bytes와 별개로 패킷 크기 추세 파악)
 *   rx_errs/tx_errs       — 에러 수 (CRC 에러, 프레임 에러 등)
 *   rx_drop/tx_drop       — 드롭 수 (커널 버퍼 부족, 큐 오버플로우 등)
 */
static void
_collect_host_net(HostMetrics *h)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    guint64 rx_total = 0, tx_total = 0;
    guint64 rx_pkt = 0, tx_pkt = 0;
    guint64 rx_err = 0, tx_err = 0;
    guint64 rx_drp = 0, tx_drp = 0;
    char line[512];
    /* 헤더 2행 스킵 ("Inter-|..." 과 "face |..." 행) */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        guint64 rb, rp, re, rd;  /* RX: bytes, packets, errs, drop */
        guint64 tb, tp, te, td;  /* TX: bytes, packets, errs, drop */
        /*
         * sscanf 패턴: "iface: RX(bytes packets errs drop) [4필드 스킵] TX(bytes packets errs drop)"
         * %*u 4개로 RX의 fifo/frame/compressed/multicast를 건너뛰어 TX측 4필드에 도달
         */
        if (sscanf(line,
            " %31[^:]: %lu %lu %lu %lu %*u %*u %*u %*u %lu %lu %lu %lu",
            iface, &rb, &rp, &re, &rd, &tb, &tp, &te, &td) == 9) {
            g_strstrip(iface);
            if (g_strcmp0(iface, "lo") == 0) continue;  /* 루프백 제외 */
            rx_total += rb; rx_pkt += rp; rx_err += re; rx_drp += rd;
            tx_total += tb; tx_pkt += tp; tx_err += te; tx_drp += td;
        }
    }
    fclose(f);
    h->net_rx_bytes   = rx_total;
    h->net_tx_bytes   = tx_total;
    h->net_rx_packets = rx_pkt;    /* [W-1] 수신 패킷 합산 */
    h->net_tx_packets = tx_pkt;    /* [W-1] 송신 패킷 합산 */
    h->net_rx_errs    = rx_err;    /* [W-1] 수신 에러 합산 */
    h->net_tx_errs    = tx_err;    /* [W-1] 송신 에러 합산 */
    h->net_rx_drop    = rx_drp;    /* [W-1] 수신 드롭 합산 */
    h->net_tx_drop    = tx_drp;    /* [W-1] 송신 드롭 합산 */
}

/*
 * [W-1 추가] _collect_host_disk_io() — /proc/diskstats 파싱으로 호스트 디스크 I/O 집계
 *
 * [/proc/diskstats 형식] (각 디바이스당 1행, 헤더 없음)
 *   major minor name
 *     rd_ios  rd_merges  rd_sectors  rd_ticks
 *     wr_ios  wr_merges  wr_sectors  wr_ticks
 *     io_now  io_ticks   io_weighted_ticks
 *   (커널 4.18+ 에서 discard 4필드 추가, 여기서는 14필드만 사용)
 *
 * [sscanf 매핑 — 14필드 파싱]
 *   필드  3: dev      — 디바이스 이름 (sda, nvme0n1, vda 등)
 *   필드  4: rd_io    — 읽기 I/O 완료 횟수
 *   필드  5: rd_m     — 읽기 머지 횟수 (스킵)
 *   필드  6: rd_sec   — 읽기 섹터 수 (× 512 = 바이트)
 *   필드  7: rd_t     — 읽기 소요 시간 ms (스킵)
 *   필드  8: wr_io    — 쓰기 I/O 완료 횟수
 *   필드  9: wr_m     — 쓰기 머지 횟수 (스킵)
 *   필드 10: wr_sec   — 쓰기 섹터 수 (× 512 = 바이트)
 *   필드 11: wr_t     — 쓰기 소요 시간 ms (스킵)
 *   필드 12: io_now   — 현재 진행 중인 I/O 수 (스킵)
 *   필드 13: io_tk    — I/O에 소요된 총 시간 ms (디스크 사용률 계산용)
 *   필드 14: io_wt    — 가중 I/O 시간 ms (스킵)
 *
 * [whole-disk 필터링 로직]
 *   파티션(sda1, nvme0n1p1 등)을 제외하고 물리 디스크만 집계해야 정확한 I/O량을 얻는다.
 *   (파티션까지 합산하면 같은 I/O가 디스크+파티션에서 이중 카운트됨)
 *   필터 규칙:
 *     - sd[a-z]  → 이름 길이 정확히 3글자 (sda OK, sda1 제외)
 *     - vd[a-z]  → 이름 길이 정확히 3글자 (vda OK, vda1 제외)
 *     - nvme*    → 이름에 'p'가 없는 것 (nvme0n1 OK, nvme0n1p1 제외)
 *
 * [섹터 → 바이트 변환]
 *   리눅스 커널은 섹터 크기를 항상 512바이트로 고정한다.
 *   (물리 디스크의 실제 섹터 크기와 무관)
 *   따라서 rd_sec * 512 = 읽기 바이트, wr_sec * 512 = 쓰기 바이트.
 */
/* ── host disk I/O aggregate ── /proc/diskstats ──────────────── */
static void
_collect_host_disk_io(HostMetrics *h)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;

    guint64 rd_bytes = 0, wr_bytes = 0, rd_ios = 0, wr_ios = 0, io_ticks = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned int major, minor;
        char dev[64];
        unsigned long long rd_io, rd_m, rd_sec, rd_t;
        unsigned long long wr_io, wr_m, wr_sec, wr_t;
        unsigned long long io_now, io_tk, io_wt;
        int n = sscanf(line,
            " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &major, &minor, dev,
            &rd_io, &rd_m, &rd_sec, &rd_t,
            &wr_io, &wr_m, &wr_sec, &wr_t,
            &io_now, &io_tk, &io_wt);
        if (n < 14) continue;
        /* whole disks only: sd[a-z] 3글자, vd[a-z] 3글자, nvme에 'p' 없는 것 */
        size_t dlen = strlen(dev);
        if (dlen == 0) continue;
        gboolean keep = FALSE;
        if (strncmp(dev, "sd", 2) == 0 && dlen == 3) keep = TRUE;       /* sda, sdb ... */
        else if (strncmp(dev, "vd", 2) == 0 && dlen == 3) keep = TRUE;  /* vda, vdb ... (KVM virtio) */
        else if (strncmp(dev, "nvme", 4) == 0 && strstr(dev, "p") == NULL) keep = TRUE; /* nvme0n1 (파티션 p1 제외) */
        if (!keep) continue;

        rd_ios   += rd_io;
        wr_ios   += wr_io;
        rd_bytes += rd_sec * 512;   /* 섹터 × 512 = 바이트 */
        wr_bytes += wr_sec * 512;
        io_ticks += io_tk;          /* I/O 소요 시간 (ms) */
    }
    fclose(f);
    h->disk_rd_bytes   = rd_bytes;
    h->disk_wr_bytes   = wr_bytes;
    h->disk_rd_ios     = rd_ios;
    h->disk_wr_ios     = wr_ios;
    h->disk_io_ticks_ms = io_ticks;
}

/* ══════════════════════════════════════════════════════════════════
 * node_exporter 호환 콜렉터 — /proc, /sys 파싱 → Prometheus 레지스트리
 * ══════════════════════════════════════════════════════════════════ */

/*
 * ── 1. node_cpu_seconds_total{cpu,mode} ── /proc/stat ──────────────
 *
 * Prometheus node_exporter 호환 CPU 메트릭을 수집한다.
 * _collect_host_cpu()가 호스트 전체 합산 백분율을 구한다면,
 * 이 함수는 "코어별(per-CPU) × 모드별(mode)" 누적 초를 Prometheus 레지스트리에 push한다.
 *
 * [출력 메트릭]
 *   node_cpu_seconds_total{cpu="0",mode="user"}   — 코어0의 user 누적 초
 *   node_cpu_seconds_total{cpu="0",mode="idle"}   — 코어0의 idle 누적 초
 *   ... (코어 × 8모드)
 *   node_context_switches_total                     — ctxt (컨텍스트 스위치 누적)
 *   node_forks_total                                — processes (fork 누적)
 *   node_procs_running                              — 현재 실행 중 프로세스 수
 *   node_procs_blocked                              — 현재 I/O 대기 프로세스 수
 *
 * [/proc/stat "cpu" 행 vs "cpuN" 행]
 *   "cpu " (공백): 전체 합산 → _collect_host_cpu()에서 사용
 *   "cpu0", "cpu1", ...: 코어별 → 이 함수에서 사용
 *   sscanf("cpu%d ...")로 코어 번호를 추출하면 "cpu " 행은 매칭 실패 → 자동 스킵
 *
 * [jiffies → 초 변환]
 *   /proc/stat의 값은 USER_HZ(보통 100) 단위 누적 jiffies.
 *   Prometheus 규격은 초(seconds) 단위이므로 100.0으로 나눠 변환한다.
 *
 * [_SET_CPU 매크로]
 *   레이블 문자열 조립 + pcv_prom_gauge_set_labels 호출을 한 줄로 압축하는 편의 매크로.
 *   모드 이름("user","nice",...)을 문자열 리터럴로 받아 cpu="N",mode="xxx" 레이블을 생성.
 */
static void
_collect_node_cpu(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        int cpuid;
        unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
        if (pcv_ebpf_proc_stat_is_cpu_core_line(buf) &&
            sscanf(buf, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &cpuid, &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) == 9) {
            char cpu[8];
            g_snprintf(cpu, sizeof(cpu), "%d", cpuid);
            char lbl[64];
#define _SET_CPU(mode, val) \
            g_snprintf(lbl, sizeof(lbl), "cpu=\"%s\",mode=\"" mode "\"", cpu); \
            pcv_prom_gauge_set_labels("node_cpu_seconds_total", lbl, (gdouble)(val) / 100.0);
            _SET_CPU("user",    user);
            _SET_CPU("nice",    nice);
            _SET_CPU("system",  sys);
            _SET_CPU("idle",    idle);
            _SET_CPU("iowait",  iowait);
            _SET_CPU("irq",     irq);
            _SET_CPU("softirq", softirq);
            _SET_CPU("steal",   steal);
#undef _SET_CPU
        }
        /* context switches & forks */
        unsigned long long ctxt;
        if (sscanf(buf, "ctxt %llu", &ctxt) == 1)
            pcv_prom_gauge_set_labels("node_context_switches_total", "", (gdouble)ctxt);
        unsigned long long forks;
        if (sscanf(buf, "processes %llu", &forks) == 1)
            pcv_prom_gauge_set_labels("node_forks_total", "", (gdouble)forks);
        unsigned long long procs_running;
        if (sscanf(buf, "procs_running %llu", &procs_running) == 1)
            pcv_prom_gauge_set_labels("node_procs_running", "", (gdouble)procs_running);
        unsigned long long procs_blocked;
        if (sscanf(buf, "procs_blocked %llu", &procs_blocked) == 1)
            pcv_prom_gauge_set_labels("node_procs_blocked", "", (gdouble)procs_blocked);
    }
    fclose(f);
}

/*
 * ── 2. node_memory_* ── /proc/meminfo 전체 파싱 ─────────────────
 *
 * /proc/meminfo의 모든 "Key: Value kB" 형식 행을 파싱하여
 * node_memory_{Key}_bytes 메트릭으로 Prometheus 레지스트리에 push한다.
 *
 * [출력 메트릭 예시] (총 60+개, /proc/meminfo 행 수만큼)
 *   node_memory_MemTotal_bytes       = MemTotal × 1024
 *   node_memory_MemAvailable_bytes   = MemAvailable × 1024
 *   node_memory_Slab_bytes           = Slab × 1024
 *   ...
 *
 * [단위 변환]
 *   /proc/meminfo는 kB(킬로바이트) 단위 → × 1024로 바이트 변환.
 *   Prometheus node_exporter 표준이 바이트(bytes) 단위이므로 이 변환이 필요.
 *
 * [_collect_host_mem()과의 차이]
 *   _collect_host_mem(): 선택된 9개 필드만 → HostMetrics 구조체에 저장 (내부 사용)
 *   _collect_node_meminfo(): 전체 필드 → Prometheus 레지스트리에 push (외부 노출)
 */
static void
_collect_node_meminfo(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        unsigned long long val;
        if (sscanf(line, "%63[^:]: %llu kB", key, &val) == 2) {
            /* node_memory_MemTotal_bytes 등으로 변환 */
            /* Prometheus 메트릭 이름: [a-zA-Z_:]+ 만 허용 — 괄호를 언더스코어로 치환 */
            for (char *p = key; *p; p++) {
                if (*p == '(' || *p == ')') *p = '_';
            }
            char metric[128];
            g_snprintf(metric, sizeof(metric), "node_memory_%s_bytes", key);
            pcv_prom_gauge_set_labels(metric, "", (gdouble)val * 1024.0);
        }
    }
    fclose(f);
}

/*
 * ── 3. node_filesystem_* ── /proc/mounts + statvfs() ────────────
 *
 * /proc/mounts에서 마운트된 파일시스템 목록을 읽고, statvfs()로 각 마운트의
 * 용량/여유/inode 정보를 수집한다.
 *
 * [출력 메트릭] (마운트포인트마다 6개)
 *   node_filesystem_size_bytes{device,mountpoint,fstype}    — 전체 크기
 *   node_filesystem_free_bytes{device,mountpoint,fstype}    — 여유 (root 포함)
 *   node_filesystem_avail_bytes{device,mountpoint,fstype}   — 여유 (일반 사용자)
 *   node_filesystem_files{device,mountpoint,fstype}         — 전체 inode 수
 *   node_filesystem_files_free{device,mountpoint,fstype}    — 여유 inode 수
 *   node_filesystem_readonly{device,mountpoint,fstype}      — 읽기 전용 여부 (0|1)
 *
 * [필터링 규칙]
 *   - 디바이스 경로가 '/'로 시작하거나 fstype이 "zfs"인 것만 포함
 *   - tmpfs, devtmpfs, proc, sysfs는 제외 (가상 파일시스템)
 *   - 이 규칙으로 물리 디스크/ZFS 파일시스템만 수집
 */
static void
_collect_node_filesystem(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char dev[128], mount[128], fstype[32];
        if (sscanf(line, "%127s %127s %31s", dev, mount, fstype) < 3)
            continue;
        /* 실 물리/ZFS 파일시스템만 */
        if (dev[0] != '/' && strncmp(fstype, "zfs", 3) != 0)
            continue;
        /* tmpfs, devtmpfs, proc 등 skip */
        if (strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "devtmpfs") == 0 ||
            strcmp(fstype, "proc") == 0 || strcmp(fstype, "sysfs") == 0)
            continue;

        struct statvfs vfs;
        if (statvfs(mount, &vfs) != 0) continue;

        gdouble total = (gdouble)vfs.f_blocks * vfs.f_frsize;
        gdouble free_b = (gdouble)vfs.f_bfree * vfs.f_frsize;
        gdouble avail = (gdouble)vfs.f_bavail * vfs.f_frsize;
        gdouble files = (gdouble)vfs.f_files;
        gdouble ffree = (gdouble)vfs.f_ffree;

        char lbl[256];
        g_snprintf(lbl, sizeof(lbl),
            "device=\"%s\",mountpoint=\"%s\",fstype=\"%s\"", dev, mount, fstype);

        pcv_prom_gauge_set_labels("node_filesystem_size_bytes", lbl, total);
        pcv_prom_gauge_set_labels("node_filesystem_free_bytes", lbl, free_b);
        pcv_prom_gauge_set_labels("node_filesystem_avail_bytes", lbl, avail);
        pcv_prom_gauge_set_labels("node_filesystem_files", lbl, files);
        pcv_prom_gauge_set_labels("node_filesystem_files_free", lbl, ffree);
        gdouble ro = (vfs.f_flag & ST_RDONLY) ? 1.0 : 0.0;
        pcv_prom_gauge_set_labels("node_filesystem_readonly", lbl, ro);
    }
    fclose(f);
}

/*
 * ── 4. node_disk_* ── /proc/diskstats 디바이스별 ──────────────────
 *
 * /proc/diskstats를 파싱하여 디바이스별 디스크 I/O 메트릭을 수집한다.
 * _collect_host_disk_io()는 전체 합산이지만, 이 함수는 디바이스별 개별 메트릭.
 *
 * [출력 메트릭] (디바이스마다 최대 14개)
 *   node_disk_reads_completed_total{device}       — 읽기 I/O 완료 횟수
 *   node_disk_reads_merged_total{device}          — 읽기 머지 횟수
 *   node_disk_read_bytes_total{device}            — 읽기 바이트 (섹터×512)
 *   node_disk_read_time_seconds_total{device}     — 읽기 소요 시간 (ticks/1000)
 *   node_disk_writes_completed_total{device}      — 쓰기 I/O 완료 횟수
 *   node_disk_writes_merged_total{device}         — 쓰기 머지 횟수
 *   node_disk_written_bytes_total{device}         — 쓰기 바이트 (섹터×512)
 *   node_disk_write_time_seconds_total{device}    — 쓰기 소요 시간
 *   node_disk_io_now{device}                      — 현재 진행 중 I/O 수
 *   node_disk_io_time_seconds_total{device}       — I/O 소요 총 시간
 *   node_disk_io_time_weighted_seconds_total{device} — 가중 I/O 시간
 *   (커널 4.18+ discard 3개 추가)
 *
 * [파티션 필터링]
 *   sda1, nvme0n1p1 등 파티션을 제외하고 물리 디스크(sda, nvme0n1, vda, dm-* 등)만
 *   수집한다. nvme, dm-, zd 접두사는 파티션 판별 로직에서 예외 처리.
 */
static void
_collect_node_diskstats(void)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned int major, minor;
        char dev[64];
        unsigned long long rd_ios, rd_merges, rd_sectors, rd_ticks;
        unsigned long long wr_ios, wr_merges, wr_sectors, wr_ticks;
        unsigned long long io_now, io_ticks, io_wt;
        unsigned long long dc_ios = 0, dc_merges = 0, dc_sectors = 0, dc_ticks = 0;

        int n = sscanf(line,
            " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &major, &minor, dev,
            &rd_ios, &rd_merges, &rd_sectors, &rd_ticks,
            &wr_ios, &wr_merges, &wr_sectors, &wr_ticks,
            &io_now, &io_ticks, &io_wt,
            &dc_ios, &dc_merges, &dc_sectors, &dc_ticks);
        if (n < 14) continue;

        /* Skip partitions (keep whole disks: sda, nvme0n1, vda etc) */
        size_t dlen = strlen(dev);
        if (dlen > 0 && dev[dlen-1] >= '0' && dev[dlen-1] <= '9') {
            /* Check if it's a partition like sda1 (has alpha+digit) vs nvme0n1 (keep) */
            if (strncmp(dev, "nvme", 4) != 0 &&
                strncmp(dev, "dm-", 3) != 0 &&
                strncmp(dev, "zd", 2) != 0) {
                /* sda1, vda1 etc - check if char before last digit is alpha */
                gboolean is_part = FALSE;
                for (int i = (int)dlen - 1; i >= 0 && dev[i] >= '0' && dev[i] <= '9'; i--) {
                    if (i > 0 && ((dev[i-1] >= 'a' && dev[i-1] <= 'z') || (dev[i-1] >= 'A' && dev[i-1] <= 'Z'))) {
                        /* Check if the alpha char is not the start of "nvme" etc */
                        if (i > 1) { is_part = TRUE; break; }
                    }
                }
                if (is_part) continue;
            }
        }

        char lbl[64];
        g_snprintf(lbl, sizeof(lbl), "device=\"%s\"", dev);

        pcv_prom_gauge_set_labels("node_disk_reads_completed_total", lbl, (gdouble)rd_ios);
        pcv_prom_gauge_set_labels("node_disk_reads_merged_total", lbl, (gdouble)rd_merges);
        pcv_prom_gauge_set_labels("node_disk_read_bytes_total", lbl, (gdouble)rd_sectors * 512.0);
        pcv_prom_gauge_set_labels("node_disk_read_time_seconds_total", lbl, (gdouble)rd_ticks / 1000.0);
        pcv_prom_gauge_set_labels("node_disk_writes_completed_total", lbl, (gdouble)wr_ios);
        pcv_prom_gauge_set_labels("node_disk_writes_merged_total", lbl, (gdouble)wr_merges);
        pcv_prom_gauge_set_labels("node_disk_written_bytes_total", lbl, (gdouble)wr_sectors * 512.0);
        pcv_prom_gauge_set_labels("node_disk_write_time_seconds_total", lbl, (gdouble)wr_ticks / 1000.0);
        pcv_prom_gauge_set_labels("node_disk_io_now", lbl, (gdouble)io_now);
        pcv_prom_gauge_set_labels("node_disk_io_time_seconds_total", lbl, (gdouble)io_ticks / 1000.0);
        pcv_prom_gauge_set_labels("node_disk_io_time_weighted_seconds_total", lbl, (gdouble)io_wt / 1000.0);
        if (n >= 18) {
            pcv_prom_gauge_set_labels("node_disk_discards_completed_total", lbl, (gdouble)dc_ios);
            pcv_prom_gauge_set_labels("node_disk_discards_merged_total", lbl, (gdouble)dc_merges);
            pcv_prom_gauge_set_labels("node_disk_discard_time_seconds_total", lbl, (gdouble)dc_ticks / 1000.0);
        }
    }
    fclose(f);
}

/*
 * ── 5. node_network_* ── /proc/net/dev 인터페이스별 ────────────
 *
 * /proc/net/dev를 파싱하여 인터페이스별 네트워크 I/O 메트릭을 수집한다.
 * _collect_host_net()은 전체 합산(lo 제외)이지만, 이 함수는 인터페이스별 개별 메트릭.
 * lo(루프백)도 포함하여 Prometheus에서 필요 시 필터링할 수 있게 한다.
 *
 * [출력 메트릭] (인터페이스마다 9개)
 *   node_network_receive_bytes_total{device}       — 수신 바이트 (누적)
 *   node_network_receive_packets_total{device}     — 수신 패킷 수
 *   node_network_receive_errs_total{device}        — 수신 에러 수
 *   node_network_receive_drop_total{device}        — 수신 드롭 수
 *   node_network_receive_multicast_total{device}   — 수신 멀티캐스트 수
 *   node_network_transmit_bytes_total{device}      — 송신 바이트
 *   node_network_transmit_packets_total{device}    — 송신 패킷 수
 *   node_network_transmit_errs_total{device}       — 송신 에러 수
 *   node_network_transmit_drop_total{device}       — 송신 드롭 수
 *
 * [/proc/net/dev 전체 16필드 파싱]
 *   RX 8필드 + TX 8필드를 sscanf로 한 번에 읽는다.
 *   _collect_host_net()은 4+4만 읽고 중간을 스킵하지만,
 *   여기서는 RX의 multicast까지 포함하여 17개 값을 모두 추출한다.
 */
static void
_collect_node_netdev(void)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    char line[512];
    /* Skip 2 header lines */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        /* /proc/net/dev: RX 8 fields + TX 8 fields = 16 numbers */
        unsigned long long rb, rp, re, rd, rfi, rfr, rc, rmu;
        unsigned long long tb, tp, te, td, tfi, tco, tcr, tcomp;
        if (sscanf(line,
            " %31[^:]: %llu %llu %llu %llu %llu %llu %llu %llu"
            " %llu %llu %llu %llu %llu %llu %llu %llu",
            iface, &rb, &rp, &re, &rd, &rfi, &rfr, &rc, &rmu,
            &tb, &tp, &te, &td, &tfi, &tco, &tcr, &tcomp) != 17)
            continue;

        g_strstrip(iface);
        char lbl[64];
        g_snprintf(lbl, sizeof(lbl), "device=\"%s\"", iface);

        pcv_prom_gauge_set_labels("node_network_receive_bytes_total", lbl, (gdouble)rb);
        pcv_prom_gauge_set_labels("node_network_receive_packets_total", lbl, (gdouble)rp);
        pcv_prom_gauge_set_labels("node_network_receive_errs_total", lbl, (gdouble)re);
        pcv_prom_gauge_set_labels("node_network_receive_drop_total", lbl, (gdouble)rd);
        pcv_prom_gauge_set_labels("node_network_receive_multicast_total", lbl, (gdouble)rmu);
        pcv_prom_gauge_set_labels("node_network_transmit_bytes_total", lbl, (gdouble)tb);
        pcv_prom_gauge_set_labels("node_network_transmit_packets_total", lbl, (gdouble)tp);
        pcv_prom_gauge_set_labels("node_network_transmit_errs_total", lbl, (gdouble)te);
        pcv_prom_gauge_set_labels("node_network_transmit_drop_total", lbl, (gdouble)td);
    }
    fclose(f);
}

/*
 * ── 6. node_vmstat_* ── /proc/vmstat (선별 항목) ─────────────────
 *
 * /proc/vmstat에서 성능 진단에 유용한 7개 항목만 선별하여 수집한다.
 *
 * [출력 메트릭]
 *   node_vmstat_pgfault        — 마이너 페이지 폴트 누적 (메모리 내 해결)
 *   node_vmstat_pgmajfault     — 메이저 페이지 폴트 누적 (디스크 I/O 수반)
 *   node_vmstat_pgpgin         — 페이지 인 바이트 (디스크→메모리)
 *   node_vmstat_pgpgout        — 페이지 아웃 바이트 (메모리→디스크)
 *   node_vmstat_pswpin         — 스왑 인 페이지 수
 *   node_vmstat_pswpout        — 스왑 아웃 페이지 수
 *   node_vmstat_oom_kill       — OOM Kill 발생 횟수 (커널 4.13+)
 *
 * [전체 수집하지 않는 이유]
 *   /proc/vmstat에는 수백 개의 항목이 있지만, 대부분 커널 내부용이다.
 *   위 7개가 운영/모니터링에 실질적으로 유용한 항목이다.
 */
static void
_collect_node_vmstat(void)
{
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        unsigned long long val;
        if (sscanf(line, "%63s %llu", key, &val) == 2) {
            /* Export key vmstat entries */
            if (strcmp(key, "pgfault") == 0 || strcmp(key, "pgmajfault") == 0 ||
                strcmp(key, "pgpgin") == 0 || strcmp(key, "pgpgout") == 0 ||
                strcmp(key, "pswpin") == 0 || strcmp(key, "pswpout") == 0 ||
                strcmp(key, "oom_kill") == 0) {
                char metric[128];
                g_snprintf(metric, sizeof(metric), "node_vmstat_%s", key);
                pcv_prom_gauge_set_labels(metric, "", (gdouble)val);
            }
        }
    }
    fclose(f);
}

/*
 * ── 7. node_sockstat_* ── /proc/net/sockstat (소켓 통계) ─────────
 *
 * /proc/net/sockstat에서 TCP/UDP 소켓 사용 현황을 수집한다.
 *
 * [출력 메트릭]
 *   node_sockstat_sockets_used     — 사용 중인 전체 소켓 수
 *   node_sockstat_TCP_inuse        — 사용 중인 TCP 소켓 수
 *   node_sockstat_TCP_orphan       — 고아 TCP 소켓 수 (close 후 FIN_WAIT)
 *   node_sockstat_TCP_tw           — TIME_WAIT 상태 TCP 소켓 수
 *   node_sockstat_TCP_alloc        — 할당된 TCP 소켓 수 (inuse + tw + orphan)
 *   node_sockstat_TCP_mem          — TCP 메모리 사용 (페이지 단위)
 *   node_sockstat_UDP_inuse        — 사용 중인 UDP 소켓 수
 *   node_sockstat_UDP_mem          — UDP 메모리 사용 (페이지 단위)
 *
 * [진단 활용]
 *   TCP_tw 급증 → 짧은 연결 반복 (keepalive 미사용)
 *   TCP_orphan 급증 → 애플리케이션이 소켓을 제대로 닫지 않음
 *   TCP_mem 급증 → 커널 TCP 버퍼 메모리 소비 과다
 */
static void
_collect_node_sockstat(void)
{
    FILE *f = fopen("/proc/net/sockstat", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* "sockets: used 123" */
        unsigned long long v;
        if (sscanf(line, "sockets: used %llu", &v) == 1)
            pcv_prom_gauge_set_labels("node_sockstat_sockets_used", "", (gdouble)v);
        /* "TCP: inuse 10 orphan 0 tw 5 alloc 20 mem 3" */
        unsigned long long inuse, orphan, tw, alloc, mem;
        if (sscanf(line, "TCP: inuse %llu orphan %llu tw %llu alloc %llu mem %llu",
                   &inuse, &orphan, &tw, &alloc, &mem) == 5) {
            pcv_prom_gauge_set_labels("node_sockstat_TCP_inuse", "", (gdouble)inuse);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_orphan", "", (gdouble)orphan);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_tw", "", (gdouble)tw);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_alloc", "", (gdouble)alloc);
            pcv_prom_gauge_set_labels("node_sockstat_TCP_mem", "", (gdouble)mem);
        }
        unsigned long long udp_inuse, udp_mem;
        if (sscanf(line, "UDP: inuse %llu mem %llu", &udp_inuse, &udp_mem) == 2) {
            pcv_prom_gauge_set_labels("node_sockstat_UDP_inuse", "", (gdouble)udp_inuse);
            pcv_prom_gauge_set_labels("node_sockstat_UDP_mem", "", (gdouble)udp_mem);
        }
    }
    fclose(f);
}

/*
 * ── 8. node_pressure_* ── /proc/pressure/{cpu,io,memory} (PSI) ──
 *
 * 커널 4.20+의 PSI(Pressure Stall Information)를 수집한다.
 * PSI는 리소스(CPU/IO/메모리) 부족으로 인해 태스크가 대기한 시간을 측정한다.
 *
 * [/proc/pressure/cpu 형식]
 *   some avg10=0.00 avg60=0.00 avg300=0.00 total=12345678
 *   (cpu에는 "full" 행이 없음 — CPU는 항상 부분 점유 가능)
 *
 * [/proc/pressure/{io,memory} 형식]
 *   some avg10=... avg60=... avg300=... total=...
 *   full avg10=... avg60=... avg300=... total=...
 *
 *   some: 하나 이상의 태스크가 리소스 부족으로 대기한 시간
 *   full: 모든 CPU가 리소스 부족으로 대기한 시간 (더 심각한 지표)
 *   total: 부팅 이후 총 대기 시간 (마이크로초 → 초로 변환하여 저장)
 *
 * [출력 메트릭]
 *   node_pressure_cpu_some_seconds_total
 *   node_pressure_io_some_seconds_total
 *   node_pressure_io_full_seconds_total
 *   node_pressure_memory_some_seconds_total
 *   node_pressure_memory_full_seconds_total
 *
 * @param resource  "cpu", "io", "memory" 중 하나
 */
static void
_collect_node_pressure_file(const char *resource)
{
    char path[64];
    g_snprintf(path, sizeof(path), "/proc/pressure/%s", resource);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char type[16];
        double avg10, avg60, avg300;
        unsigned long long total_us;
        if (sscanf(line, "%15s avg10=%lf avg60=%lf avg300=%lf total=%llu",
                   type, &avg10, &avg60, &avg300, &total_us) == 5) {
            if (strcmp(type, "some") == 0 || strcmp(type, "full") == 0) {
                char metric[128];
                g_snprintf(metric, sizeof(metric),
                    "node_pressure_%s_%s_seconds_total", resource, type);
                pcv_prom_gauge_set_labels(metric, "", (gdouble)total_us / 1e6);
            }
        }
    }
    fclose(f);
}

/** @brief PSI 3개 리소스(cpu/io/memory)를 순차 수집하는 래퍼 함수 */
static void
_collect_node_pressure(void)
{
    _collect_node_pressure_file("cpu");
    _collect_node_pressure_file("io");
    _collect_node_pressure_file("memory");
}

/*
 * ── 9. node_hwmon_temp_celsius ── /sys/class/hwmon (하드웨어 온도) ──
 *
 * /sys/class/hwmon 디렉터리를 스캔하여 온도 센서 값을 수집한다.
 * 서버/데스크톱의 CPU, GPU, 칩셋 등 하드웨어 온도를 모니터링한다.
 *
 * [파일 구조]
 *   /sys/class/hwmon/hwmon0/name          → 칩 이름 (예: "coretemp", "k10temp")
 *   /sys/class/hwmon/hwmon0/temp1_input   → 센서1 온도 (밀리섭씨, 예: 45000 = 45.0도C)
 *   /sys/class/hwmon/hwmon0/temp1_crit    → 센서1 임계 온도 (밀리섭씨)
 *   /sys/class/hwmon/hwmon0/temp2_input   → 센서2 온도
 *   ... (최대 temp16까지 탐색)
 *
 * [출력 메트릭]
 *   node_hwmon_temp_celsius{chip="coretemp",sensor="temp1"}       — 현재 온도 (섭씨)
 *   node_hwmon_temp_crit_celsius{chip="coretemp",sensor="temp1"}  — 임계 온도 (섭씨)
 *
 * [밀리섭씨 → 섭씨 변환]
 *   커널은 정수 밀리섭씨(1/1000도) 단위로 출력. / 1000.0 하여 섭씨로 변환.
 */
static void
_collect_node_hwmon(void)
{
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* Read chip name */
        char name_path[256], chip_name[64] = "unknown";
        g_snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", ent->d_name);
        FILE *nf = fopen(name_path, "r");
        if (nf) {
            if (fgets(chip_name, sizeof(chip_name), nf))
                chip_name[strcspn(chip_name, "\n")] = 0;
            fclose(nf);
        }

        /* Scan temp*_input files */
        for (int i = 1; i <= 16; i++) {
            char tpath[256];
            g_snprintf(tpath, sizeof(tpath), "/sys/class/hwmon/%s/temp%d_input", ent->d_name, i);
            FILE *tf = fopen(tpath, "r");
            if (!tf) break;

            int milli = 0;
            if (fscanf(tf, "%d", &milli) == 1) {
                char lbl[128];
                g_snprintf(lbl, sizeof(lbl), "chip=\"%s\",sensor=\"temp%d\"", chip_name, i);
                pcv_prom_gauge_set_labels("node_hwmon_temp_celsius", lbl, (gdouble)milli / 1000.0);
            }
            fclose(tf);

            /* crit threshold */
            g_snprintf(tpath, sizeof(tpath), "/sys/class/hwmon/%s/temp%d_crit", ent->d_name, i);
            tf = fopen(tpath, "r");
            if (tf) {
                int crit = 0;
                if (fscanf(tf, "%d", &crit) == 1) {
                    char lbl[128];
                    g_snprintf(lbl, sizeof(lbl), "chip=\"%s\",sensor=\"temp%d\"", chip_name, i);
                    pcv_prom_gauge_set_labels("node_hwmon_temp_crit_celsius", lbl, (gdouble)crit / 1000.0);
                }
                fclose(tf);
            }
        }
    }
    closedir(d);
}

/*
 * ── 10. misc: 기타 시스템 메트릭 ─────────────────────────────────
 *
 * 다양한 /proc, /sys 소스에서 기타 시스템 메트릭을 수집한다.
 *
 * [출력 메트릭]
 *   node_boot_time_seconds          — 시스템 부팅 Unix epoch (초)
 *   node_time_seconds               — 현재 시각 Unix epoch (초)
 *   node_uptime_seconds             — 부팅 이후 경과 시간 (초)
 *   node_load1 / node_load5 / node_load15 — 로드 평균 1/5/15분
 *   node_entropy_available_bits     — /dev/random 엔트로피 풀 크기
 *   node_filefd_allocated           — 할당된 파일 디스크립터 수
 *   node_filefd_maximum             — 최대 파일 디스크립터 수
 *   node_nf_conntrack_entries       — netfilter conntrack 현재 항목 수
 *   node_nf_conntrack_entries_limit — netfilter conntrack 최대 한도
 *   node_arp_entries{device}        — ARP 테이블 엔트리 수
 *   node_network_mtu_bytes{device}  — NIC MTU (바이트)
 *   node_network_speed_bytes{device}— NIC 링크 속도 (Mbps→Bytes/s 변환)
 *   node_network_carrier{device}    — NIC 캐리어 상태 (0=다운, 1=업)
 *
 * [데이터 소스]
 *   - sysinfo(2): 부팅 시각 계산 (현재 시각 - uptime)
 *   - /proc/loadavg: 로드 평균
 *   - /proc/uptime: 부팅 이후 경과 시간
 *   - /proc/sys/kernel/random/entropy_avail: 엔트로피
 *   - /proc/sys/fs/file-nr: 파일 디스크립터
 *   - /proc/sys/net/netfilter/nf_conntrack_{count,max}: conntrack
 *   - /proc/net/arp: ARP 엔트리 수 (헤더 1줄 제외)
 *   - /sys/class/net/{dev}/mtu, speed, carrier: NIC 메타데이터
 *
 * [speed Mbps→Bytes/s 변환]
 *   /sys/class/net/{dev}/speed는 Mbps 단위 (예: 1000 = 1Gbps).
 *   Prometheus 규격은 Bytes/s이므로 x 125000 (= 1000000 / 8)으로 변환.
 */
static void
_collect_node_misc(void)
{
    /* boot_time */
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        gdouble boot = (gdouble)(g_get_real_time() / G_USEC_PER_SEC) - (gdouble)si.uptime;
        pcv_prom_gauge_set_labels("node_boot_time_seconds", "", boot);
        pcv_prom_gauge_set_labels("node_time_seconds", "",
            (gdouble)(g_get_real_time() / G_USEC_PER_SEC));
    }

    /* load averages */
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        double l1, l5, l15;
        if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            pcv_prom_gauge_set_labels("node_load1", "", l1);
            pcv_prom_gauge_set_labels("node_load5", "", l5);
            pcv_prom_gauge_set_labels("node_load15", "", l15);
        }
        fclose(f);
    }

    /* uptime */
    f = fopen("/proc/uptime", "r");
    if (f) {
        double up;
        if (fscanf(f, "%lf", &up) == 1)
            pcv_prom_gauge_set_labels("node_uptime_seconds", "", up);
        fclose(f);
    }

    /* entropy */
    f = fopen("/proc/sys/kernel/random/entropy_avail", "r");
    if (f) {
        int ent;
        if (fscanf(f, "%d", &ent) == 1)
            pcv_prom_gauge_set_labels("node_entropy_available_bits", "", (gdouble)ent);
        fclose(f);
    }

    /* file descriptors */
    f = fopen("/proc/sys/fs/file-nr", "r");
    if (f) {
        unsigned long long allocated, unused, max_fd;
        if (fscanf(f, "%llu %llu %llu", &allocated, &unused, &max_fd) == 3) {
            pcv_prom_gauge_set_labels("node_filefd_allocated", "", (gdouble)allocated);
            pcv_prom_gauge_set_labels("node_filefd_maximum", "", (gdouble)max_fd);
        }
        fclose(f);
    }

    /* conntrack */
    f = fopen("/proc/sys/net/netfilter/nf_conntrack_count", "r");
    if (f) {
        unsigned long long ct;
        if (fscanf(f, "%llu", &ct) == 1)
            pcv_prom_gauge_set_labels("node_nf_conntrack_entries", "", (gdouble)ct);
        fclose(f);
    }
    f = fopen("/proc/sys/net/netfilter/nf_conntrack_max", "r");
    if (f) {
        unsigned long long ct;
        if (fscanf(f, "%llu", &ct) == 1)
            pcv_prom_gauge_set_labels("node_nf_conntrack_entries_limit", "", (gdouble)ct);
        fclose(f);
    }

    /* ARP entries */
    f = fopen("/proc/net/arp", "r");
    if (f) {
        char line[256];
        int count = -1; /* skip header */
        while (fgets(line, sizeof(line), f)) count++;
        if (count > 0)
            pcv_prom_gauge_set_labels("node_arp_entries", "device=\"eno1\"", (gdouble)count);
        fclose(f);
    }

    /* network speed / MTU / carrier from /sys/class/net */
    DIR *d = opendir("/sys/class/net");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0) continue;
            char lbl[64];
            g_snprintf(lbl, sizeof(lbl), "device=\"%s\"", ent->d_name);

            /* MTU */
            char path[256];
            g_snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", ent->d_name);
            FILE *mf = fopen(path, "r");
            if (mf) {
                int mtu;
                if (fscanf(mf, "%d", &mtu) == 1)
                    pcv_prom_gauge_set_labels("node_network_mtu_bytes", lbl, (gdouble)mtu);
                fclose(mf);
            }

            /* speed (may fail for virtual devices) */
            g_snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ent->d_name);
            mf = fopen(path, "r");
            if (mf) {
                int speed;
                if (fscanf(mf, "%d", &speed) == 1 && speed > 0)
                    pcv_prom_gauge_set_labels("node_network_speed_bytes",
                        lbl, (gdouble)speed * 125000.0); /* Mbps → Bytes/s */
                fclose(mf);
            }

            /* carrier */
            g_snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ent->d_name);
            mf = fopen(path, "r");
            if (mf) {
                int carrier;
                if (fscanf(mf, "%d", &carrier) == 1)
                    pcv_prom_gauge_set_labels("node_network_carrier", lbl, (gdouble)carrier);
                fclose(mf);
            }
        }
        closedir(d);
    }
}

/*
 * ── VM 메트릭 수집 (libvirt Bulk API) ───────────────────────────
 *
 * virConnectGetAllDomainStats()로 실행 중인 모든 VM의 확장 메트릭을 한 번에 수집.
 *
 * [telemetry.c의 _collect와의 차이]
 *   telemetry.c: CPU time + net RX/TX만 수집 → 1초 간격 Lock-Free 캐시
 *   이 함수: CPU + Memory Balloon + Block I/O + Network I/O 전체 수집
 *            → 5초 간격, GMutex 보호, G.vms[] 배열에 저장
 *
 * [libvirt Stats 플래그]
 *   VIR_DOMAIN_STATS_STATE     — 도메인 상태 (running/paused/...)
 *   VIR_DOMAIN_STATS_CPU_TOTAL — CPU time 누적 (ns)
 *   VIR_DOMAIN_STATS_BALLOON   — 메모리 balloon (max/current KB)
 *   VIR_DOMAIN_STATS_BLOCK     — 블록 디바이스 I/O (rd/wr bytes, reqs)
 *   VIR_DOMAIN_STATS_INTERFACE — 네트워크 I/O (rx/tx bytes, pkts)
 *
 * [블록/네트워크 합산 방식]
 *   "block.0.rd.bytes", "block.1.rd.bytes" 등 인덱스별 필드를 합산한다.
 *   g_str_has_prefix + g_str_has_suffix로 인덱스에 무관하게 매칭.
 *   이렇게 하면 디스크/NIC 수에 관계없이 VM당 총합을 구할 수 있다.
 *
 * [커넥션 관리]
 *   매 호출마다 virConnectOpen/Close를 수행한다.
 *   5초 간격이므로 커넥션 유지보다 단순함을 선택.
 *   (telemetry.c는 1초 간격이라 영구 커넥션 사용)
 *
 * [EBPF_MAX_VMS(64) 제한]
 *   64개를 초과하는 VM은 수집에서 제외된다.
 *   실운영 환경(3노드 10VM)에서는 충분한 크기.
 */
static void
_collect_vm_metrics(void)
{
    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) return;

    virDomainStatsRecordPtr *stats = NULL;
    unsigned int flags = VIR_CONNECT_GET_ALL_DOMAINS_STATS_RUNNING;
    gint nstats = virConnectGetAllDomainStats(conn,
        VIR_DOMAIN_STATS_STATE | VIR_DOMAIN_STATS_CPU_TOTAL |
        VIR_DOMAIN_STATS_BALLOON | VIR_DOMAIN_STATS_BLOCK |
        VIR_DOMAIN_STATS_INTERFACE,
        &stats, flags);

    if (nstats < 0) {
        virConnectClose(conn);
        return;
    }

    g_mutex_lock(&G.mu);
    G.vm_count = 0;

    for (gint i = 0; i < nstats && G.vm_count < EBPF_MAX_VMS; i++) {
        virDomainStatsRecordPtr rec = stats[i];
        const gchar *name = virDomainGetName(rec->dom);
        if (!name) continue;

        VmExtMetrics *vm = &G.vms[G.vm_count++];
        memset(vm, 0, sizeof(*vm));
        g_strlcpy(vm->name, name, sizeof(vm->name));

        /* Parse typed params */
        for (gint j = 0; j < rec->nparams; j++) {
            virTypedParameterPtr p = &rec->params[j];

            if (g_strcmp0(p->field, "state.state") == 0)
                vm->state = p->value.i;
            else if (g_strcmp0(p->field, "cpu.time") == 0)
                vm->cpu_time_ns = p->value.ul;
            else if (g_strcmp0(p->field, "balloon.maximum") == 0)
                vm->mem_max_kb = p->value.ul;
            else if (g_strcmp0(p->field, "balloon.current") == 0)
                vm->mem_used_kb = p->value.ul;
            /* Block stats: block.0.rd.bytes, block.0.wr.bytes, etc */
            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".rd.bytes"))
                vm->disk_rd_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".wr.bytes"))
                vm->disk_wr_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".rd.reqs"))
                vm->disk_rd_reqs += p->value.ul;
            else if (g_str_has_prefix(p->field, "block.") &&
                     g_str_has_suffix(p->field, ".wr.reqs"))
                vm->disk_wr_reqs += p->value.ul;
            /* Net stats: net.0.rx.bytes, etc */
            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".rx.bytes"))
                vm->net_rx_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".tx.bytes"))
                vm->net_tx_bytes += p->value.ul;
            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".rx.pkts"))
                vm->net_rx_pkts += p->value.ul;
            else if (g_str_has_prefix(p->field, "net.") &&
                     g_str_has_suffix(p->field, ".tx.pkts"))
                vm->net_tx_pkts += p->value.ul;
        }
    }
    g_mutex_unlock(&G.mu);

    virDomainStatsRecordListFree(stats);
    virConnectClose(conn);
}

/* ── 백그라운드 수집 스레드 ────────────────────────────────────── */

/**
 * @brief eBPF 텔레메트리 메인 수집 스레드 — 5초마다 전체 메트릭 수집.
 *
 * 실행 흐름 (매 주기):
 *   1. 호스트 메트릭 수집 (CPU/메모리/로드/네트워크/디스크 I/O)
 *      → 로컬 변수에 먼저 채운 뒤 G.mu 잠금 하에 G.host로 복사
 *   2. VM 메트릭 수집 (libvirt Bulk API)
 *      → _collect_vm_metrics() 내부에서 G.mu 잠금 하에 G.vms[] 갱신
 *   3. node_exporter 호환 콜렉터 10개 실행
 *      → Prometheus 레지스트리(prometheus_exporter.c)에 직접 push
 *   4. AI Ops 평가 (이상 탐지 + 워크로드 예측)
 *   5. 5초 대기 후 반복
 *
 * @param data  미사용 (NULL)
 * @return NULL
 */
static gpointer
_ebpf_thread(gpointer data)
{
    (void)data;
    PCV_LOG_INFO(EBPF_LOG_DOM, "eBPF telemetry thread started (interval=%ds)", EBPF_INTERVAL_SEC);

    while (G.running) {
        /* Collect host metrics */
        HostMetrics h = {0};
        _collect_host_cpu(&h);
        _collect_host_mem(&h);
        _collect_host_load(&h);
        _collect_host_net(&h);
        _collect_host_disk_io(&h);

        g_mutex_lock(&G.mu);
        G.host = h;
        g_mutex_unlock(&G.mu);

        /* Collect VM metrics */
        _collect_vm_metrics();

        /*
         * per-VM 네트워크 I/O 메트릭 노출
         *
         * _collect_vm_metrics()가 G.vms[]에 수집한 libvirt 통계를
         * Prometheus Gauge로 노출한다. vm_name 레이블로 VM별 구분.
         *
         * 출력 메트릭:
         *   purecvisor_vm_net_rx_bytes_total{vm_name="..."} — VM 수신 바이트 누적
         *   purecvisor_vm_net_tx_bytes_total{vm_name="..."} — VM 송신 바이트 누적
         */
        {
            g_mutex_lock(&G.mu);
            for (gint i = 0; i < G.vm_count; i++) {
                gchar lbl[128];
                g_snprintf(lbl, sizeof(lbl), "vm_name=\"%s\"", G.vms[i].name);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_rx_bytes_total",
                                          lbl, (gdouble)G.vms[i].net_rx_bytes);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_tx_bytes_total",
                                          lbl, (gdouble)G.vms[i].net_tx_bytes);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_rx_packets_total",
                                          lbl, (gdouble)G.vms[i].net_rx_pkts);
                pcv_prom_gauge_set_labels("purecvisor_vm_net_tx_packets_total",
                                          lbl, (gdouble)G.vms[i].net_tx_pkts);
            }
            g_mutex_unlock(&G.mu);
        }

        /* node_exporter 호환 콜렉터 — Prometheus 레지스트리에 직접 push */
        _collect_node_cpu();
        _collect_node_meminfo();
        _collect_node_filesystem();
        _collect_node_diskstats();
        _collect_node_netdev();
        _collect_node_vmstat();
        _collect_node_sockstat();
        _collect_node_pressure();
        _collect_node_hwmon();
        _collect_node_misc();

        /* AI Ops: 이상 탐지 + 워크로드 예측 평가 */
        pcv_anomaly_evaluate();
        pcv_predict_evaluate();

        /*
         * libvirt 커넥션 풀 메트릭 노출
         *
         * virt_conn_pool.c가 관리하는 libvirt 커넥션 풀의 상태를
         * Prometheus Gauge로 노출한다.
         *
         * 출력 메트릭:
         *   purecvisor_connpool_idle   — 유휴 커넥션 수 (반환된 상태)
         *   purecvisor_connpool_active — 사용 중 커넥션 수 (= total - idle)
         *   purecvisor_connpool_max    — 풀 최대 크기 (daemon.conf에서 설정)
         *
         * Grafana에서 active가 max에 근접하면 커넥션 풀 고갈 경보를 설정할 수 있다.
         * idle이 0이면 모든 커넥션이 사용 중이므로, 새 요청이 커넥션 대기 상태에 빠질 수 있다.
         */
        {
            guint idle = 0, total = 0, max = 0;
            virt_conn_pool_stats(&idle, &total, &max);
            pcv_prom_gauge_set_labels("purecvisor_connpool_idle", "", (gdouble)idle);
            pcv_prom_gauge_set_labels("purecvisor_connpool_active", "", (gdouble)(total - idle));
            pcv_prom_gauge_set_labels("purecvisor_connpool_max", "", (gdouble)max);
            pcv_prom_gauge_set_labels("purecvisor_connpool_wait_seconds", "",
                                      virt_conn_pool_wait_avg_seconds());
        }

        /* 워커 스레드 풀 메트릭 */
        pcv_prom_gauge_set_labels("purecvisor_worker_pool_pending", "",
                                  (gdouble)pcv_worker_pool_get_pending());

        /* 감사 로그 큐 깊이 + 드롭 카운터 메트릭 */
        pcv_prom_gauge_set_labels("purecvisor_audit_queue_depth", "",
                                  (gdouble)pcv_audit_get_queue_depth());
        pcv_prom_gauge_set_labels("purecvisor_audit_dropped_total", "",
                                  (gdouble)pcv_audit_get_dropped_count());

        /* ── ZFS Pool Health Metrics (60초 주기) ──────────── */
        {
            static gint64 last_pool_check = 0;
            gint64 now_pool_us = g_get_monotonic_time();
            if (now_pool_us - last_pool_check >= 60 * G_USEC_PER_SEC) {
                last_pool_check = now_pool_us;
                ZfsPoolHealth zh;
                if (pcv_zfs_pool_health("pcvpool", &zh)) {
                    gdouble state_val = 0.0;
                    if (g_strcmp0(zh.state, "DEGRADED") == 0) state_val = 1.0;
                    else if (g_strcmp0(zh.state, "FAULTED") == 0) state_val = 2.0;
                    else if (g_strcmp0(zh.state, "UNAVAIL") == 0) state_val = 3.0;

                    pcv_prom_gauge_set_labels("purecvisor_zpool_state", "", state_val);
                    pcv_prom_gauge_set_labels("purecvisor_zpool_errors_read", "",
                                              (gdouble)zh.errors_read);
                    pcv_prom_gauge_set_labels("purecvisor_zpool_errors_write", "",
                                              (gdouble)zh.errors_write);
                    pcv_prom_gauge_set_labels("purecvisor_zpool_errors_cksum", "",
                                              (gdouble)zh.errors_cksum);
                    if (zh.scrub_age_sec >= 0) {
                        pcv_prom_gauge_set_labels("purecvisor_zpool_scrub_age_seconds", "",
                                                  (gdouble)zh.scrub_age_sec);
                    }
                    pcv_prom_gauge_set_labels("purecvisor_zpool_capacity_percent", "",
                                              zh.capacity_pct);
                }
            }
        }

        /* ── ZFS Capacity Forecast Recording (3600초 = 1시간 주기) ── */
        {
            static gint64 last_cap_record = 0;
            gint64 now_cap_us = g_get_monotonic_time();
            if (now_cap_us - last_cap_record >= (gint64)3600 * G_USEC_PER_SEC) {
                last_cap_record = now_cap_us;
                pcv_zfs_capacity_record("pcvpool");
            }
        }

        /*
         * keepalived VRRP 메트릭 수집
         *
         * PureCVisor 3노드 클러스터는 keepalived를 통해 VIP(192.0.2.100)를 관리한다.
         * MASTER 노드가 VIP를 소유하며, MASTER 장애 시 BACKUP 노드로 자동 페일오버된다.
         *
         * 출력 메트릭:
         *   purecvisor_keepalived_active    — keepalived 프로세스 실행 여부 (0|1)
         *   purecvisor_keepalived_vip_owner — 이 노드가 VIP를 소유하고 있는지 (0|1)
         *   purecvisor_keepalived_master    — VRRP MASTER 역할인지 (0|1)
         *
         * 수집 방법:
         *   1. "systemctl is-active keepalived" → 프로세스 상태 확인
         *   2. "ip -o addr show dev pcvbr0" → VIP 문자열 검색으로 소유 여부 판별
         *
         * pcv_spawn_sync()를 사용하여 system()/popen() 대신 argv 배열 기반으로
         * 안전하게 외부 명령을 실행한다. (Command Injection 방어)
         */
        {
            gchar *ka_out = NULL;
            const gchar *ka_argv[] = {"systemctl", "is-active", "keepalived", NULL};
            gboolean ka_running = pcv_spawn_sync(ka_argv, &ka_out, NULL, NULL);
            pcv_prom_gauge_set_labels("purecvisor_keepalived_active", "",
                                      ka_running ? 1.0 : 0.0);
            g_free(ka_out);

            /* VIP 소유 여부: ip addr show 에서 VIP(192.0.2.100) 검색 */
            gchar *ip_out = NULL;
            const gchar *ip_argv[] = {"ip", "-o", "addr", "show", "dev", "pcvbr0", NULL};
            if (pcv_spawn_sync(ip_argv, &ip_out, NULL, NULL) && ip_out) {
                gboolean has_vip = (g_strstr_len(ip_out, -1, "192.0.2.100") != NULL);
                pcv_prom_gauge_set_labels("purecvisor_keepalived_vip_owner", "",
                                          has_vip ? 1.0 : 0.0);
                /* VRRP 역할: VIP 소유 = MASTER(1), 미소유 = BACKUP(0) */
                pcv_prom_gauge_set_labels("purecvisor_keepalived_master", "",
                                          has_vip ? 1.0 : 0.0);
            }
            g_free(ip_out);
        }

        /* ── Circuit Breaker + VM Lock 메트릭 ──────────────── */
        pcv_prom_gauge_set_labels("purecvisor_circuit_breaker_state", "",
                                  (gdouble)cb_get_state());
        pcv_prom_gauge_set_labels("purecvisor_circuit_breaker_failures_total", "",
                                  (gdouble)cb_get_failure_count());
        pcv_prom_gauge_set_labels("purecvisor_vm_locks_held", "",
                                  (gdouble)pcv_vm_state_get_lock_count());

        /* ── TLS Certificate Expiry Monitoring ─────────────── */
        /*
         * 인증서 만료 일수를 Prometheus 메트릭으로 노출하고,
         * 만료 임박 시 경고 로그를 출력합니다.
         * 매 루프(5초)마다 메트릭을 갱신하지만, 경고 로그는
         * 3600초(1시간)마다 한 번만 출력하여 로그 폭주를 방지합니다.
         */
        {
            gint64 cert_days = pcv_tls_get_cert_expiry_days();
            if (cert_days >= 0) {
                pcv_prom_gauge_set_labels("purecvisor_tls_cert_expiry_days", "",
                                          (gdouble)cert_days);
            }
            /* 1시간마다 만료 경고 체크 (EBPF_INTERVAL_SEC=5, 720회 = 3600초) */
            {
                static guint cert_check_counter = 0;
                if (++cert_check_counter >= 720) {
                    cert_check_counter = 0;
                    pcv_tls_check_expiry_warning();
                }
            }
        }

        g_usleep(EBPF_INTERVAL_SEC * G_USEC_PER_SEC);
    }

    PCV_LOG_INFO(EBPF_LOG_DOM, "eBPF telemetry thread stopped");
    return NULL;
}

/* ── 공개 API ────────────────────────────────────────────────── */

/**
 * @brief eBPF 텔레메트리 모듈을 초기화하고 수집 스레드를 시작한다.
 *
 * GMutex 초기화 + "ebpf-telem" GThread 생성.
 * main.c에서 데몬 시작 시 1회 호출.
 */
void
pcv_ebpf_telemetry_init(void)
{
    g_mutex_init(&G.mu);
    G.running = TRUE;
    G.initialized = TRUE;
    G.thread = g_thread_new("ebpf-telem", _ebpf_thread, NULL);
    PCV_LOG_INFO(EBPF_LOG_DOM, "eBPF telemetry initialized");
}

/**
 * @brief eBPF 텔레메트리 모듈을 종료하고 수집 스레드를 정지시킨다.
 *
 * 동작 순서:
 *   1. initialized 미설정이면 조기 반환 (init 미호출 또는 이미 종료됨)
 *   2. G.running=FALSE → 스레드 루프 탈출 유도
 *   3. g_thread_join()으로 스레드 종료 대기 (최대 EBPF_INTERVAL_SEC초 후 깨어남)
 *   4. GMutex 해제
 *   5. initialized=FALSE → 이중 shutdown 방지
 *
 * 호출 컨텍스트: 메인 스레드에서 데몬 종료(drain) 시 1회 호출.
 */
void
pcv_ebpf_telemetry_shutdown(void)
{
    if (!G.initialized) return;
    G.running = FALSE;
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = NULL;
    }
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/*
 * pcv_ebpf_telemetry_get_host() — HostMetrics를 JSON으로 직렬화하여 반환
 *
 * [호출자]
 *   handler_cluster.c (telemetry.host RPC)
 *   rest_server.c     (GET /internal/telemetry — 클러스터 스케줄러용)
 *
 * [반환 JSON 필드 목록 — 총 33개]
 *
 *  << CPU (9필드) — 단위: % (0.0~100.0) >>
 *   cpu_percent   : 전체 CPU 사용률 (= 100 - idle - iowait)
 *   cpu_user      : 사용자 공간 실행 비율
 *   cpu_system    : 커널 공간 실행 비율
 *   cpu_nice      : nice 프로세스 비율
 *   cpu_iowait    : I/O 대기 비율
 *   cpu_steal     : 하이퍼바이저가 빼앗은 비율
 *   cpu_irq       : 하드웨어 인터럽트 비율
 *   cpu_softirq   : 소프트 인터럽트 비율
 *   cpu_idle      : 유휴 비율
 *
 *  << Memory (12필드) — kB 또는 % >>
 *   mem_total_kb       : 전체 물리 RAM (kB)
 *   mem_avail_kb       : 사용 가능 메모리 (kB)
 *   mem_free_kb        : [W-1] 미사용 메모리 (kB)
 *   mem_buffers_kb     : [W-1] 블록 디바이스 버퍼 (kB)
 *   mem_cached_kb      : [W-1] 페이지 캐시 (kB)
 *   mem_slab_kb        : [W-1] 커널 슬랩 (kB)
 *   mem_sreclaimable_kb: [W-1] 회수 가능 슬랩 (kB)
 *   swap_total_kb      : [W-1] 전체 스왑 (kB)
 *   swap_free_kb       : [W-1] 미사용 스왑 (kB)
 *   pgfault            : [W-1] 마이너 페이지 폴트 누적 횟수
 *   pgmajfault         : [W-1] 메이저 페이지 폴트 누적 횟수
 *   mem_percent        : 메모리 사용률 % (= 100 × (1 - avail/total))
 *
 *  << Load Average (3필드) — 무차원 실수 >>
 *   load_1m / load_5m / load_15m
 *
 *  << Network I/O (8필드) — 부팅 이후 누적값 >>
 *   net_rx_bytes / net_tx_bytes         : 바이트
 *   net_rx_packets / net_tx_packets     : [W-1] 패킷 수
 *   net_rx_errs / net_tx_errs           : [W-1] 에러 수
 *   net_rx_drop / net_tx_drop           : [W-1] 드롭 수
 *
 *  << Disk I/O (5필드) — 부팅 이후 누적값 >>
 *   disk_rd_bytes / disk_wr_bytes       : [W-1] 바이트
 *   disk_rd_ios / disk_wr_ios           : [W-1] I/O 횟수
 *   disk_io_ticks_ms                    : [W-1] I/O 소요 시간 (ms)
 *
 * [스레드 안전성]
 *   G.mu 잠금 하에 HostMetrics를 읽고, JSON 빌드 완료 후 해제.
 *   호출자는 반환된 JsonObject*의 소유권을 받는다 (사용 후 g_object_unref 필요).
 */
/* ── LOW-10: get_host() 캐시 — 2초 이내 재호출 시 복사본 반환 ────── */
static JsonObject *g_cached_host_obj = nullptr;
static gint64      g_cached_host_ts  = 0;
static GMutex      g_host_cache_mu;

JsonObject *
pcv_ebpf_telemetry_get_host(void)
{
    gint64 now = g_get_monotonic_time();
    g_mutex_lock(&g_host_cache_mu);
    if (g_cached_host_obj && (now - g_cached_host_ts) < 2 * G_USEC_PER_SEC) {
        JsonObject *ref = json_object_ref(g_cached_host_obj);
        g_mutex_unlock(&g_host_cache_mu);
        return ref;
    }
    g_mutex_unlock(&g_host_cache_mu);

    JsonObject *obj = json_object_new();
    g_mutex_lock(&G.mu);

    /* CPU — 전체 사용률 + 8개 모드별 백분율 (단위: %) */
    json_object_set_double_member(obj, "cpu_percent",    G.host.cpu_percent);
    json_object_set_double_member(obj, "cpu_user",       G.host.cpu_user);
    json_object_set_double_member(obj, "cpu_system",     G.host.cpu_system);
    json_object_set_double_member(obj, "cpu_nice",       G.host.cpu_nice);
    json_object_set_double_member(obj, "cpu_iowait",     G.host.cpu_iowait);
    json_object_set_double_member(obj, "cpu_steal",      G.host.cpu_steal);
    json_object_set_double_member(obj, "cpu_irq",        G.host.cpu_irq);
    json_object_set_double_member(obj, "cpu_softirq",    G.host.cpu_softirq);
    json_object_set_double_member(obj, "cpu_idle",       G.host.cpu_idle);

    /* Memory — 기본(total/avail) + W-1 확장(free/buffers/cached/slab/swap/pgfault) */
    json_object_set_int_member   (obj, "mem_total_kb",       G.host.mem_total_kb);
    json_object_set_int_member   (obj, "mem_avail_kb",       G.host.mem_avail_kb);
    json_object_set_int_member   (obj, "mem_free_kb",        G.host.mem_free_kb);
    json_object_set_int_member   (obj, "mem_buffers_kb",     G.host.mem_buffers_kb);
    json_object_set_int_member   (obj, "mem_cached_kb",      G.host.mem_cached_kb);
    json_object_set_int_member   (obj, "mem_slab_kb",        G.host.mem_slab_kb);
    json_object_set_int_member   (obj, "mem_sreclaimable_kb",G.host.mem_sreclaimable_kb);
    json_object_set_int_member   (obj, "swap_total_kb",      G.host.swap_total_kb);
    json_object_set_int_member   (obj, "swap_free_kb",       G.host.swap_free_kb);
    json_object_set_int_member   (obj, "pgfault",            G.host.pgfault);
    json_object_set_int_member   (obj, "pgmajfault",         G.host.pgmajfault);
    /* mem_percent: 사용률(%) = 100 × (1 - MemAvailable / MemTotal) */
    gdouble mem_pct = G.host.mem_total_kb > 0
        ? 100.0 * (1.0 - (gdouble)G.host.mem_avail_kb / (gdouble)G.host.mem_total_kb)
        : 0.0;
    json_object_set_double_member(obj, "mem_percent", mem_pct);

    /* Load average 1/5/15분 */
    json_object_set_double_member(obj, "load_1m",        G.host.load_1m);
    json_object_set_double_member(obj, "load_5m",        G.host.load_5m);
    json_object_set_double_member(obj, "load_15m",       G.host.load_15m);

    /* Network I/O — bytes + [W-1] packets/errs/drop (부팅 이후 누적) */
    json_object_set_int_member   (obj, "net_rx_bytes",   G.host.net_rx_bytes);
    json_object_set_int_member   (obj, "net_tx_bytes",   G.host.net_tx_bytes);
    json_object_set_int_member   (obj, "net_rx_packets", G.host.net_rx_packets);
    json_object_set_int_member   (obj, "net_tx_packets", G.host.net_tx_packets);
    json_object_set_int_member   (obj, "net_rx_errs",    G.host.net_rx_errs);
    json_object_set_int_member   (obj, "net_tx_errs",    G.host.net_tx_errs);
    json_object_set_int_member   (obj, "net_rx_drop",    G.host.net_rx_drop);
    json_object_set_int_member   (obj, "net_tx_drop",    G.host.net_tx_drop);

    /* Disk I/O — [W-1] whole-disk 합산 (bytes/ios/ticks, 부팅 이후 누적) */
    json_object_set_int_member   (obj, "disk_rd_bytes",    G.host.disk_rd_bytes);
    json_object_set_int_member   (obj, "disk_wr_bytes",    G.host.disk_wr_bytes);
    json_object_set_int_member   (obj, "disk_rd_ios",      G.host.disk_rd_ios);
    json_object_set_int_member   (obj, "disk_wr_ios",      G.host.disk_wr_ios);
    json_object_set_int_member   (obj, "disk_io_ticks_ms", G.host.disk_io_ticks_ms);

    g_mutex_unlock(&G.mu);

    /* 캐시 갱신 */
    g_mutex_lock(&g_host_cache_mu);
    if (g_cached_host_obj) json_object_unref(g_cached_host_obj);
    g_cached_host_obj = json_object_ref(obj);
    g_cached_host_ts  = g_get_monotonic_time();
    g_mutex_unlock(&g_host_cache_mu);

    return obj;
}

/**
 * @brief VmExtMetrics 구조체를 JSON 객체로 변환한다 (내부 헬퍼).
 *
 * pcv_ebpf_telemetry_get_vm()과 pcv_ebpf_telemetry_get_all_vms()에서 공통 사용.
 * VM별 확장 메트릭(이름, 상태, CPU, 메모리 Balloon, 디스크 I/O, 네트워크 I/O)을
 * JSON 키-값 쌍으로 직렬화한다.
 *
 * @param vm  변환할 VmExtMetrics 포인터 (읽기 전용)
 * @return JsonObject* — 호출자가 소유권을 받음. json_object_unref() 또는
 *         json_array_add_object_element()로 이전해야 함.
 */
static JsonObject *
_vm_to_json(const VmExtMetrics *vm)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "name",          vm->name);
    json_object_set_int_member   (obj, "state",         vm->state);
    json_object_set_int_member   (obj, "cpu_time_ns",   vm->cpu_time_ns);
    json_object_set_int_member   (obj, "mem_max_kb",    vm->mem_max_kb);
    json_object_set_int_member   (obj, "mem_used_kb",   vm->mem_used_kb);
    json_object_set_int_member   (obj, "disk_rd_bytes", vm->disk_rd_bytes);
    json_object_set_int_member   (obj, "disk_wr_bytes", vm->disk_wr_bytes);
    json_object_set_int_member   (obj, "disk_rd_reqs",  vm->disk_rd_reqs);
    json_object_set_int_member   (obj, "disk_wr_reqs",  vm->disk_wr_reqs);
    json_object_set_int_member   (obj, "net_rx_bytes",  vm->net_rx_bytes);
    json_object_set_int_member   (obj, "net_tx_bytes",  vm->net_tx_bytes);
    json_object_set_int_member   (obj, "net_rx_pkts",   vm->net_rx_pkts);
    json_object_set_int_member   (obj, "net_tx_pkts",   vm->net_tx_pkts);
    return obj;
}

/**
 * @brief 특정 VM의 확장 메트릭을 JSON으로 반환한다.
 *
 * G.mu 잠금 하에 G.vms[] 배열을 선형 탐색하여 vm_name이 일치하는 항목을 찾는다.
 * 찾으면 _vm_to_json()으로 JSON 변환하여 반환, 못 찾으면 에러 JSON 반환.
 *
 * @param vm_name  조회할 VM 이름 (예: "web-prod")
 * @return JsonObject* — 호출자가 소유권을 받음. json_object_unref() 필요.
 *         VM 미발견 시: {"error": "VM not found in telemetry cache"}
 *
 * 호출 컨텍스트: handler_cluster.c (telemetry.vm RPC)
 * 스레드 안전성: 내부 G.mu 락으로 보호됨.
 */
JsonObject *
pcv_ebpf_telemetry_get_vm(const gchar *vm_name)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.vm_count; i++) {
        if (g_strcmp0(G.vms[i].name, vm_name) == 0) {
            JsonObject *obj = _vm_to_json(&G.vms[i]);
            g_mutex_unlock(&G.mu);
            return obj;
        }
    }
    g_mutex_unlock(&G.mu);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "error", "VM not found in telemetry cache");
    return obj;
}

/**
 * @brief 모든 VM의 확장 메트릭을 JsonArray로 반환한다.
 *
 * G.mu 잠금 하에 G.vms[] 배열의 전체 항목(G.vm_count개)을
 * _vm_to_json()으로 변환하여 JsonArray에 추가한다.
 *
 * @return JsonArray* — 호출자가 소유권을 받음. json_array_unref() 필요.
 *         VM이 없으면 빈 배열 반환.
 *
 * 호출 컨텍스트: handler_cluster.c (telemetry.all RPC)
 * 스레드 안전성: 내부 G.mu 락으로 보호됨.
 */
JsonArray *
pcv_ebpf_telemetry_get_all_vms(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.vm_count; i++)
        json_array_add_object_element(arr, _vm_to_json(&G.vms[i]));
    g_mutex_unlock(&G.mu);
    return arr;
}
