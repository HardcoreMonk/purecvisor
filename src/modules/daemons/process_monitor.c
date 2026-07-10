/**
 * @file process_monitor.c
 * @brief WhaTap 스타일 프로세스 모니터링 — /proc/[pid]/stat + /proc/[pid]/io
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  아키텍처 위치
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   main.c (데몬 초기화)
 *     └─ pcv_process_monitor_init()     → GThread("proc-monitor") 생성
 *     └─ pcv_process_monitor_shutdown() → 스레드 join + 자원 해제
 *
 *   dispatcher.c (RPC 라우팅)
 *     └─ "monitor.metrics" → handler_monitor.c
 *        → pcv_process_monitor_get_top(n) 호출하여 JsonArray 응답 구성
 *
 *   rest_server.c (HTTP 라우팅)
 *     └─ GET/POST /api/v1/monitor/metrics → dispatcher 경유 → 동일 API
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  WhaTap 프로세스 모니터링에서 차용한 핵심 개념
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   WhaTap의 인프라 에이전트는 호스트의 모든 프로세스를 주기적으로 스캔하여
 *   CPU%, 메모리, 디스크 I/O를 수집하고, Top N 프로세스를 대시보드에 표시한다.
 *   본 모듈은 이 방식을 순수 C + /proc 파일시스템으로 구현한 것이다.
 *
 *   차용 포인트:
 *     - 20초 주기 수집: 시스템 부하 최소화 vs 관찰 해상도 균형 (WhaTap 5~30초)
 *     - Top N 패턴: CPU% 기준 내림차순 정렬 후 상위 N개만 추출
 *     - CPU% 델타 계산: 이전 샘플 tick vs 현재 tick의 차이 / 경과 시간
 *     - 3대 지표: CPU%, MEM(RSS), I/O(read/write bytes)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  스레드 모델
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   [GThread "proc-monitor"]  (백그라운드, 20초 주기)
 *     │
 *     ├─ _collect_processes()
 *     │    ├─ opendir("/proc") → 숫자 디렉터리만 필터 (= PID)
 *     │    ├─ _parse_proc_stat(pid, &p)  ← /proc/[pid]/stat 파싱
 *     │    ├─ _parse_proc_io(pid, &p)    ← /proc/[pid]/io 파싱
 *     │    ├─ CPU% 델타 계산 (prev_ticks GHashTable 참조)
 *     │    ├─ qsort(CPU% 내림차순)
 *     │    └─ GMutex 잠금 → G.procs[] 갱신 → 해제
 *     │
 *     └─ g_usleep(20초) 후 반복
 *
 *   [GMainLoop 스레드] (RPC/REST 요청 처리)
 *     │
 *     └─ pcv_process_monitor_get_top(n)
 *          └─ GMutex 잠금 → G.procs[] 복사 → JsonArray 생성 → 해제
 *
 *   GMutex(G.mu)가 G.procs[]와 G.count를 보호하여 두 스레드 간 경쟁 조건 방지.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  주의사항
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - /proc/[pid]/io는 root 권한(또는 CAP_SYS_PTRACE)이 있어야 읽기 가능.
 *     권한 없으면 fopen 실패 → io_rd_bytes, io_wr_bytes가 0으로 남음 (정상).
 *   - 첫 번째 수집 시 prev_ticks가 비어 있어 cpu_percent = 0.0.
 *     두 번째 수집(+20초)부터 유효한 값이 계산됨.
 *   - PROC_MAX(512) 초과 프로세스는 수집되지 않음.
 *   - 프로세스가 수집 도중 종료되면 /proc/[pid]/stat fopen 실패 → skip.
 */
#include "process_monitor.h"
#include "utils/pcv_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>

/*
 * ============================================================================
 *  [주니어 개발자 필독] 프로세스 모니터 핵심 개념
 * ============================================================================
 *
 *  CPU% 델타 계산 원리:
 *    /proc/[pid]/stat의 utime+stime은 부팅 이후 누적 tick입니다.
 *    20초 전 값과 현재 값의 차이를 경과 시간으로 나누면 순간 사용률:
 *      cpu% = 100 × (현재_tick - 이전_tick) / (CLK_TCK × 경과초)
 *    prev_ticks GHashTable에 PID별 이전 tick을 저장합니다.
 *
 *  프로세스 분류 (ProcType):
 *    /proc/[pid]/cgroup 경로와 cmdline으로 VM/컨테이너/시스템 프로세스를
 *    구분합니다. Web UI에서 유형별 필터링에 활용됩니다.
 *
 *  주의: /proc/[pid]/io는 root 권한이 필요합니다. 권한 없으면
 *  io_rd_bytes=0으로 남는 것이 정상 동작입니다.
 * ============================================================================
 */

/** 로그 도메인 — journalctl에서 "proc_mon" 태그로 필터 가능 */
#define PROC_LOG_DOM      "proc_mon"

/** 수집 주기 (초). WhaTap 에이전트의 5~30초 범위 중 20초를 채택 */
#define PROC_INTERVAL_SEC 20

/** 최대 수집 프로세스 수. 일반 서버에서 200~400개 프로세스를 기대 */
#define PROC_MAX          512

/**
 * @enum ProcType
 * @brief 프로세스 분류 유형 — cgroup 경로 및 cmdline으로 결정
 */
typedef enum {
    PROC_HOST      = 0,  /**< 일반 호스트 프로세스 */
    PROC_VM        = 1,  /**< libvirt/qemu VM 프로세스 */
    PROC_CONTAINER = 2,  /**< LXC 컨테이너 프로세스 */
    PROC_SYSTEM    = 3   /**< systemd 서비스 프로세스 */
} ProcType;

/** ProcType → 문자열 매핑 (JSON 출력용) */
static const gchar *_proc_type_str[] = { "host", "vm", "container", "system" };

/**
 * @struct ProcInfo
 * @brief 단일 프로세스의 모니터링 데이터를 저장하는 구조체
 *
 * /proc/[pid]/stat 와 /proc/[pid]/io 에서 파싱한 값을 보관한다.
 * _collect_processes()에서 임시 배열(tmp[])에 채운 뒤,
 * qsort 후 G.procs[]로 복사된다.
 */
typedef struct {
    /**
     * 프로세스 ID.
     * /proc 디렉터리의 숫자 이름에서 atoi()로 변환.
     * _parse_proc_stat()이 성공하면 pid > 0, 실패하면 0 (skip 기준).
     */
    gint     pid;

    /**
     * 프로세스 이름 (커널이 부여한 command name).
     * /proc/[pid]/stat의 2번째 필드 — 괄호로 감싸져 있다: (comm)
     * 예: "(purecvisormd)", "(kworker/0:1-events)"
     *
     * 주의: comm에는 공백, 괄호, 특수문자가 포함될 수 있으므로
     * 단순 sscanf가 아닌 strchr('(') ~ strrchr(')') 범위로 추출한다.
     * 최대 63바이트 (64바이트 버퍼 - NUL 종단).
     */
    gchar    comm[64];

    /**
     * 프로세스 상태 문자.
     * /proc/[pid]/stat의 3번째 필드 (괄호 닫힌 후 첫 번째).
     *   R = Running (실행 중 / 실행 대기)
     *   S = Sleeping (인터럽트 가능 대기 — 가장 흔함)
     *   D = Disk sleep (인터럽트 불가 대기 — I/O 블로킹)
     *   Z = Zombie (종료되었으나 부모가 wait 안 함)
     *   T = Stopped (시그널로 중지)
     *   t = Tracing stop (디버거에 의해 중지)
     *   X = Dead (커널 2.6.0+ 에서는 보이지 않음)
     */
    gchar    state;

    /**
     * 누적 사용자 모드 CPU tick 수.
     * /proc/[pid]/stat의 14번째 필드 (utime).
     * 단위: clock tick (보통 1/100초, sysconf(_SC_CLK_TCK) = 100).
     *
     * CPU% 계산에 사용: delta(utime + stime) / (CLK_TCK * 경과초) * 100
     */
    guint64  utime;

    /**
     * 누적 커널 모드 CPU tick 수.
     * /proc/[pid]/stat의 15번째 필드 (stime).
     * utime과 합산하여 총 CPU 소비 tick을 구한다.
     */
    guint64  stime;

    /**
     * RSS (Resident Set Size) — 물리 메모리에 상주하는 페이지 수.
     * /proc/[pid]/stat의 24번째 필드.
     *
     * 단위: 페이지 (pages). 바이트로 변환하려면 page_size를 곱한다.
     *   MB = rss_pages * page_size / (1024 * 1024)
     *   KB = rss_pages * (page_size / 1024)
     *
     * page_size는 sysconf(_SC_PAGESIZE)로 조회 (보통 4096 바이트).
     */
    glong    rss_pages;

    /**
     * 누적 디스크 읽기 바이트 수.
     * /proc/[pid]/io의 "read_bytes:" 행에서 파싱.
     *
     * 이 값은 실제 스토리지 I/O만 반영한다 (페이지 캐시 히트 제외).
     * root 권한이 없으면 /proc/[pid]/io를 읽을 수 없어 0으로 남음.
     */
    guint64  io_rd_bytes;

    /**
     * 누적 디스크 쓰기 바이트 수.
     * /proc/[pid]/io의 "write_bytes:" 행에서 파싱.
     * io_rd_bytes와 동일한 제약 조건(root 권한 필요).
     */
    guint64  io_wr_bytes;

    /**
     * CPU 사용률 (%).
     * _collect_processes()에서 아래 공식으로 계산:
     *
     *   delta_ticks = (현재 utime+stime) - (이전 utime+stime)
     *   cpu_percent = 100.0 * delta_ticks / (CLK_TCK * 경과_초)
     *
     * 첫 번째 수집 시에는 이전 값이 없으므로 0.0.
     * 멀티코어 시스템에서는 100%를 초과할 수 있다 (예: 2코어 = 최대 200%).
     */
    gdouble  cpu_percent;

    /**
     * cgroup 경로 (/proc/[pid]/cgroup에서 추출).
     * cgroup v2: "0::/<path>" 형식에서 "/" 이후.
     * 예: "/system.slice/purecvisormd.service", "/lxc/mycontainer"
     */
    gchar    cgroup[256];

    /**
     * 프로세스 분류 유형 (ProcType).
     * cgroup 경로와 comm 이름으로 자동 분류:
     *   PROC_HOST(0), PROC_VM(1), PROC_CONTAINER(2), PROC_SYSTEM(3)
     */
    ProcType type;
} ProcInfo;

/**
 * @brief 모듈 전역 상태 — 파일 스코프 정적 변수
 *
 * 단일 전역 구조체(G)에 모든 상태를 모아 관리한다.
 * {0}으로 zero-initialize되므로 초기 상태가 명확하다.
 */
static struct {
    /** 수집 스레드 핸들. shutdown 시 g_thread_join()으로 종료 대기 */
    GThread    *thread;

    /** 스레드 루프 제어 플래그. FALSE로 설정하면 다음 주기에 루프 탈출 */
    gboolean    running;

    /** init() 호출 여부. 이중 shutdown 방지 가드 */
    gboolean    initialized;

    /**
     * 프로세스 목록 보호용 뮤텍스.
     * - 수집 스레드: 잠금 → G.procs[]/G.count 갱신 → 해제
     * - API 호출자: 잠금 → G.procs[] 읽기 → JsonArray 복사 → 해제
     */
    GMutex      mu;

    /**
     * 수집된 프로세스 정보 배열 (CPU% 내림차순 정렬 상태).
     * 최대 PROC_MAX(512)개. 실제 유효 항목 수는 G.count로 추적.
     */
    ProcInfo    procs[PROC_MAX];

    /** G.procs[]에 저장된 유효 프로세스 수 */
    gint        count;

    /**
     * 시스템 페이지 크기 (바이트).
     * sysconf(_SC_PAGESIZE)로 초기화. 보통 4096.
     * RSS 페이지 → MB/KB 변환에 사용:
     *   MB = rss_pages * page_size / (1024 * 1024)
     */
    glong       page_size;

    /**
     * 초당 clock tick 수.
     * sysconf(_SC_CLK_TCK)로 초기화. 리눅스 기본값 = 100.
     * CPU% 계산의 분모에 사용: CLK_TCK * 경과_초 = 해당 기간의 총 tick 수
     *
     * 예: 20초 간격이면 총 tick = 100 * 20 = 2000.
     *     delta_ticks가 200이면 CPU% = 200/2000 * 100 = 10%.
     */
    glong       clk_tck;

    /**
     * 이전 수집 시점의 모노토닉 시각 (마이크로초).
     * g_get_monotonic_time()으로 취득.
     *
     * 현재 시각과의 차이(dt_sec)가 CPU% 계산의 "경과 시간"이 된다.
     * 첫 수집 시 0이므로 dt_sec = 0 → CPU% 계산 skip.
     */
    gint64      prev_time_us;

    /**
     * 이전 수집 시점의 PID별 누적 tick(utime+stime) 저장소.
     *
     * 키: PID (gint → GINT_TO_POINTER 매크로로 포인터 크기 정수로 변환)
     *     - GINT_TO_POINTER(pid): gint 값을 gpointer(void*)로 캐스팅.
     *       GHashTable이 키를 gpointer로 관리하므로 이 변환이 필요.
     *       실제 메모리 주소가 아니라 정수 값 자체를 포인터 크기로 저장하는 트릭.
     *
     * 값: utime+stime 합산 tick (guint64 → GSIZE_TO_POINTER로 저장)
     *     - GSIZE_TO_POINTER(ticks): 64비트 정수를 gpointer로 캐스팅.
     *     - 조회 시: GPOINTER_TO_SIZE(val)로 역변환하여 guint64 복원.
     *
     * 해시 함수: g_direct_hash (포인터 값 자체를 해시로 사용)
     * 비교 함수: g_direct_equal (포인터 값 직접 비교)
     *
     * 이 테이블 덕분에 프로세스가 생성/소멸되어도 동적으로 추적 가능.
     * shutdown 시 g_hash_table_destroy()로 해제.
     */
    GHashTable *prev_ticks;
} G = {0};

/**
 * @brief /proc/[pid]/cgroup 파일에서 cgroup 경로를 추출한다.
 *
 * cgroup v2 형식: "0::/<path>" — "0::" 이후의 경로를 반환.
 * cgroup v1 형식: 여러 줄 중 "0::" 시작 줄 우선, 없으면 첫 줄의 마지막 ":"이후.
 * 추출 실패 시 "/" 를 버퍼에 복사.
 *
 * @param pid     대상 프로세스 ID
 * @param buf     결과를 저장할 버퍼
 * @param bufsize 버퍼 크기
 */
static void
_get_process_cgroup(gint pid, gchar *buf, gsize bufsize)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);

    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        g_strlcpy(buf, "/", bufsize);
        return;
    }

    /* cgroup v2: "0::/<path>" 형식 우선 검색 */
    gchar **lines = g_strsplit(content, "\n", -1);
    gboolean found = FALSE;
    for (gint i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "0::")) {
            g_strlcpy(buf, lines[i] + 3, bufsize);  /* "0::" 이후 복사 */
            found = TRUE;
            break;
        }
    }

    /* cgroup v1 폴백: 첫 줄의 마지막 ':' 이후 */
    if (!found && lines[0]) {
        const gchar *last_colon = strrchr(lines[0], ':');
        if (last_colon)
            g_strlcpy(buf, last_colon + 1, bufsize);
        else
            g_strlcpy(buf, "/", bufsize);
    } else if (!found) {
        g_strlcpy(buf, "/", bufsize);
    }

    g_strfreev(lines);
    g_free(content);
}

/**
 * @brief cgroup 경로와 프로세스 이름으로 프로세스 유형을 분류한다.
 *
 * 분류 규칙 (우선순위 순):
 *   1. cgroup에 "/lxc/" 또는 "/lxc.payload." 포함 → PROC_CONTAINER
 *   2. cgroup에 "/machine.slice/" 포함 또는 comm에 "qemu" 포함 → PROC_VM
 *   3. cgroup에 ".service" 포함 → PROC_SYSTEM
 *   4. 그 외 → PROC_HOST
 *
 * @param cgroup  cgroup 경로 문자열
 * @param comm    프로세스 이름 (/proc/[pid]/stat의 comm 필드)
 * @return ProcType 분류 결과
 */
static ProcType
_classify_process(const gchar *cgroup, const gchar *comm)
{
    if (strstr(cgroup, "/lxc/") || strstr(cgroup, "/lxc.payload."))
        return PROC_CONTAINER;
    if (strstr(cgroup, "/machine.slice/") || strstr(comm, "qemu"))
        return PROC_VM;
    if (strstr(cgroup, ".service"))
        return PROC_SYSTEM;
    return PROC_HOST;
}

/**
 * @brief /proc/[pid]/stat 파일을 파싱하여 ProcInfo를 채운다.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  /proc/[pid]/stat 파일 형식 (man 5 proc 참고)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   한 줄짜리 파일이며, 공백으로 구분된 필드들로 구성된다:
 *
 *     pid (comm) state ppid pgrp session tty_nr tpgid flags
 *     minflt cminflt majflt cmajflt utime stime cutime cstime
 *     priority nice num_threads itrealvalue starttime vsize rss ...
 *
 *   필드 번호 (1-based):
 *     1  = pid           프로세스 ID
 *     2  = (comm)        실행 파일 이름 (괄호로 감쌈)
 *     3  = state         프로세스 상태 (R/S/D/Z/T 등)
 *     4  = ppid          부모 프로세스 ID
 *     5  = pgrp          프로세스 그룹 ID
 *     6  = session       세션 ID
 *     7  = tty_nr        제어 터미널
 *     8  = tpgid         포그라운드 프로세스 그룹 ID
 *     9  = flags         커널 플래그
 *     10 = minflt        마이너 페이지 폴트 횟수
 *     11 = cminflt       자식 마이너 페이지 폴트
 *     12 = majflt        메이저 페이지 폴트 횟수
 *     13 = cmajflt       자식 메이저 페이지 폴트
 *     14 = utime    ★    사용자 모드 CPU tick (이 함수에서 추출)
 *     15 = stime    ★    커널 모드 CPU tick  (이 함수에서 추출)
 *     16 = cutime        대기 종료된 자식의 utime
 *     17 = cstime        대기 종료된 자식의 stime
 *     18 = priority      스케줄링 우선순위
 *     19 = nice          nice 값
 *     20 = num_threads   스레드 수
 *     21 = itrealvalue   (사용 안 됨, 항상 0)
 *     22 = starttime     부팅 이후 프로세스 시작 시각 (tick)
 *     23 = vsize         가상 메모리 크기 (바이트)
 *     24 = rss      ★    RSS 페이지 수 (이 함수에서 추출)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  comm 필드 파싱이 까다로운 이유
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   comm에는 공백, 괄호, 특수문자가 포함될 수 있다:
 *     예: "(Web Content)", "(gdbus)", "(kworker/0:1-events)"
 *
 *   따라서 단순 sscanf("%s")로 파싱하면 공백에서 잘린다.
 *   해결법:
 *     1) strchr(buf, '(')  → 첫 번째 여는 괄호 위치
 *     2) strrchr(buf, ')') → 마지막 닫는 괄호 위치
 *     3) 사이의 문자열을 comm에 복사
 *
 *   strrchr을 쓰는 이유: comm 자체에 ')'가 포함될 수 있으므로
 *   가장 마지막 ')'를 닫는 괄호로 간주해야 안전하다.
 *
 *   ')' 이후의 나머지 필드(3번~24번)는 sscanf로 순서대로 파싱한다.
 *   필드 3(state)~13(cmajflt)까지 11개 중, 14(utime)·15(stime)·24(rss)만
 *   추출하고 나머지는 %*d / %*u / %*lu 로 스킵한다.
 *
 * @param pid  대상 프로세스 ID
 * @param p    결과를 채울 ProcInfo 포인터 (pid=0이면 파싱 실패를 의미)
 */
static void
_parse_proc_stat(gint pid, ProcInfo *p)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* /proc/[pid]/stat: pid (comm) state ppid ... utime stime ... rss ... */
    gchar buf[1024];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return; }
    fclose(f);

    /*
     * comm 추출: 첫 번째 '('와 마지막 ')' 사이의 문자열.
     * strchr  = 첫 번째 '(' 찾기 (comm 시작)
     * strrchr = 마지막 ')' 찾기 (comm 끝)
     * 이렇게 해야 comm 안에 ')' 문자가 있어도 안전하다.
     */
    gchar *start = strchr(buf, '(');
    gchar *end = strrchr(buf, ')');
    if (!start || !end || end <= start) return;

    /* comm 문자열 복사 (괄호 제외, 최대 63바이트) */
    gsize clen = (gsize)(end - start - 1);
    if (clen >= sizeof(p->comm)) clen = sizeof(p->comm) - 1;
    memcpy(p->comm, start + 1, clen);
    p->comm[clen] = '\0';

    /*
     * ')' 다음부터가 필드 3(state) 이후.
     * end + 2 = ')' 다음의 공백을 건너뛴 위치 (= 필드 3 시작).
     *
     * sscanf 형식 문자열이 파싱하는 필드 매핑:
     *   %c   → 필드  3: state (프로세스 상태 문자)
     *   %d   → 필드  4: ppid  (부모 PID — 사용하지 않지만 위치 맞춤용)
     *   %*d  → 필드  5: pgrp (스킵)
     *   %*d  → 필드  6: session (스킵)
     *   %*d  → 필드  7: tty_nr (스킵)
     *   %*d  → 필드  8: tpgid (스킵)
     *   %*u  → 필드  9: flags (스킵)
     *   %*u  → 필드 10: minflt (스킵)
     *   %*u  → 필드 11: cminflt (스킵)
     *   %*u  → 필드 12: majflt (스킵)
     *   %*u  → 필드 13: cmajflt (스킵)
     *   %lu  → 필드 14: utime  ★ 사용자 모드 CPU tick
     *   %lu  → 필드 15: stime  ★ 커널 모드 CPU tick
     *   %*d  → 필드 16: cutime (스킵)
     *   %*d  → 필드 17: cstime (스킵)
     *   %*d  → 필드 18: priority (스킵)
     *   %*d  → 필드 19: nice (스킵)
     *   %*d  → 필드 20: num_threads (스킵)
     *   %*d  → 필드 21: itrealvalue (스킵)
     *   %*u  → 필드 22: starttime (스킵)
     *   %*u  → 필드 23: vsize (스킵)
     *   %ld  → 필드 24: rss   ★ 물리 메모리 페이지 수
     *
     * n >= 5 이면 state, ppid, utime, stime, rss 모두 성공적으로 파싱된 것.
     */
    gchar *rest = end + 2; /* skip ') ' */
    gchar state;
    gint ppid;
    unsigned long utime, stime;
    long rss;

    /* Fields: state(3) ppid(4) pgrp(5) session(6) tty_nr(7) tpgid(8)
     * flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13)
     * utime(14) stime(15) cutime(16) cstime(17) priority(18) nice(19)
     * num_threads(20) itrealvalue(21) starttime(22) vsize(23) rss(24) */
    int n = sscanf(rest,
        "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
        &state, &ppid, &utime, &stime, &rss);
    if (n >= 5) {
        p->pid = pid;
        p->state = state;
        p->utime = utime;
        p->stime = stime;
        p->rss_pages = rss;
    }
}

/**
 * @brief /proc/[pid]/io 파일을 파싱하여 I/O 바이트 정보를 채운다.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  /proc/[pid]/io 파일 형식 (man 5 proc 참고)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   여러 줄로 구성된 key: value 형식:
 *
 *     rchar: 123456789         ← read() 시스템 콜로 읽은 총 바이트 (캐시 포함)
 *     wchar: 987654321         ← write() 시스템 콜로 쓴 총 바이트 (캐시 포함)
 *     syscr: 12345             ← read() 시스템 콜 횟수
 *     syscw: 6789              ← write() 시스템 콜 횟수
 *     read_bytes: 45678912     ← 실제 스토리지에서 읽은 바이트 ★ (이 함수에서 추출)
 *     write_bytes: 12345678    ← 실제 스토리지에 쓴 바이트    ★ (이 함수에서 추출)
 *     cancelled_write_bytes: 0 ← 취소된 쓰기 바이트
 *
 *   rchar/wchar는 페이지 캐시 히트도 포함하므로 실제 디스크 I/O와 다르다.
 *   read_bytes/write_bytes가 실제 블록 디바이스 I/O를 반영하므로 이것을 수집.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  root 권한 필요성
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   /proc/[pid]/io 파일은 커널 설정에 따라 읽기 권한이 제한된다.
 *   - 자기 자신의 프로세스: 항상 읽기 가능
 *   - 다른 프로세스: root 또는 CAP_SYS_PTRACE 권한 필요
 *
 *   현재 에디션 데몬은 root로 실행되므로 보통 문제없다.
 *   만약 권한이 없으면 fopen() 실패 → 함수가 조기 리턴 → io 필드는 0 유지.
 *
 * @param pid  대상 프로세스 ID
 * @param p    결과를 채울 ProcInfo 포인터 (io_rd_bytes, io_wr_bytes 갱신)
 */
static void
_parse_proc_io(gint pid, ProcInfo *p)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/io", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;

    gchar line[128];
    while (fgets(line, sizeof(line), f)) {
        guint64 val;
        if (sscanf(line, "read_bytes: %lu", &val) == 1)
            p->io_rd_bytes = val;
        else if (sscanf(line, "write_bytes: %lu", &val) == 1)
            p->io_wr_bytes = val;
    }
    fclose(f);
}

/**
 * @brief qsort 비교 함수 — CPU% 내림차순 정렬
 *
 * CPU%가 높은 프로세스가 배열 앞쪽에 오도록 정렬한다.
 * get_top(n)에서 배열 앞에서 n개만 잘라내면 Top N이 된다.
 *
 * @return  양수(b > a), 0(같음), 음수(a > b) — 내림차순
 */
static gint
_sort_by_cpu(gconstpointer a, gconstpointer b)
{
    const ProcInfo *pa = a, *pb = b;
    if (pb->cpu_percent > pa->cpu_percent) return 1;
    if (pb->cpu_percent < pa->cpu_percent) return -1;
    return 0;
}

/**
 * @brief /proc 디렉터리를 스캔하여 전체 프로세스 정보를 수집한다.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  CPU% 델타 계산 원리
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   리눅스 커널은 각 프로세스의 CPU 사용 시간을 "tick" 단위로 누적 기록한다.
 *   tick의 주파수는 sysconf(_SC_CLK_TCK)로 조회하며, 보통 100Hz (= 10ms/tick).
 *
 *   CPU% 계산을 위해 두 시점 사이의 tick 변화량을 경과 시간으로 나눈다:
 *
 *     [이전 수집]           [현재 수집]
 *     prev_time_us ────────── now_us         (모노토닉 시계, 마이크로초)
 *     prev_ticks[pid] ─────── total_ticks    (utime + stime 누적 tick)
 *
 *     dt_sec = (now_us - prev_time_us) / 1,000,000   ← 경과 시간 (초)
 *     delta  = total_ticks - prev_ticks[pid]          ← tick 변화량
 *
 *     cpu_percent = 100.0 * delta / (CLK_TCK * dt_sec)
 *
 *   예시 (20초 간격, CLK_TCK=100):
 *     prev_ticks = 5000,  total_ticks = 5400  → delta = 400
 *     cpu% = 100 * 400 / (100 * 20) = 20.0%
 *
 *   멀티코어 환경에서는 여러 코어에서 동시에 tick이 쌓이므로
 *   cpu_percent가 100%를 초과할 수 있다 (예: 4코어 풀로드 = 400%).
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  prev_ticks GHashTable 동작 방식
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   키(PID)와 값(tick)을 모두 포인터 크기 정수로 저장하는 GLib 관용 패턴:
 *
 *   저장:
 *     g_hash_table_insert(prev_ticks,
 *         GINT_TO_POINTER(pid),       // 키: PID 정수 → void* 캐스팅
 *         GSIZE_TO_POINTER(ticks));   // 값: tick 정수 → void* 캐스팅
 *
 *   조회:
 *     gpointer val = g_hash_table_lookup(prev_ticks, GINT_TO_POINTER(pid));
 *     guint64 prev = GPOINTER_TO_SIZE(val);  // void* → 정수 역변환
 *
 *   이 방식은 별도의 메모리 할당 없이 정수를 해시 테이블에 저장할 수 있어
 *   성능과 메모리 효율이 좋다. GLib에서 매우 흔하게 사용되는 패턴이다.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  처리 흐름
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   1. opendir("/proc") → 디렉터리 엔트리 순회
 *   2. 숫자로 시작하는 엔트리만 필터 (= PID 디렉터리)
 *   3. 각 PID에 대해:
 *      a) _parse_proc_stat()  → comm, state, utime, stime, rss 추출
 *      b) _parse_proc_io()    → io_rd_bytes, io_wr_bytes 추출
 *      c) prev_ticks에서 이전 tick 조회 → delta 계산 → cpu_percent 산출
 *      d) prev_ticks에 현재 tick 저장 (다음 주기용)
 *   4. qsort()로 CPU% 내림차순 정렬
 *   5. GMutex 잠금 → G.procs[]에 결과 복사 → 해제
 *   6. prev_time_us 갱신 (다음 주기의 dt_sec 계산용)
 */
static void
_collect_processes(void)
{
    DIR *d = opendir("/proc");
    if (!d) return;

    ProcInfo tmp[PROC_MAX];
    gint count = 0;

    /* 현재 모노토닉 시각 (마이크로초). wall clock이 아니라 NTP 조정에 영향받지 않음 */
    gint64 now_us = g_get_monotonic_time();

    /*
     * 이전 수집 시점으로부터의 경과 시간 (초).
     * 첫 수집 시 prev_time_us == 0이므로 dt_sec = 0 → CPU% 계산 skip.
     */
    gdouble dt_sec = (G.prev_time_us > 0)
        ? (gdouble)(now_us - G.prev_time_us) / G_USEC_PER_SEC
        : 0.0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < PROC_MAX) {
        /* /proc 아래에서 숫자로 시작하는 디렉터리만 PID 디렉터리 */
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        gint pid = atoi(ent->d_name);
        if (pid <= 0) continue;

        ProcInfo *p = &tmp[count];
        memset(p, 0, sizeof(*p));

        /* /proc/[pid]/stat에서 comm, state, utime, stime, rss 파싱 */
        _parse_proc_stat(pid, p);
        if (p->pid == 0) continue; /* 파싱 실패 (프로세스가 이미 종료됨 등) */

        /* /proc/[pid]/io에서 read_bytes, write_bytes 파싱 (root 권한 필요) */
        _parse_proc_io(pid, p);

        /* /proc/[pid]/cgroup에서 cgroup 경로 추출 + 프로세스 유형 분류 */
        _get_process_cgroup(pid, p->cgroup, sizeof(p->cgroup));
        p->type = _classify_process(p->cgroup, p->comm);

        /*
         * CPU% 델타 계산:
         *   total_ticks = 현재 utime + stime (누적 CPU tick)
         *   prev_val    = 이전 수집 시점의 total_ticks (GHashTable에서 조회)
         *   delta       = total_ticks - prev_ticks (이 기간 동안 소비한 tick)
         *   cpu_percent = 100 * delta / (CLK_TCK * dt_sec)
         *
         *   CLK_TCK * dt_sec = 해당 기간에 가능한 총 tick 수 (단일 코어 기준)
         */
        guint64 total_ticks = p->utime + p->stime;
        gpointer prev_val = g_hash_table_lookup(G.prev_ticks, GINT_TO_POINTER(pid));
        if (prev_val && dt_sec > 0.0) {
            guint64 prev_ticks = GPOINTER_TO_SIZE(prev_val);
            guint64 delta = (total_ticks >= prev_ticks) ? total_ticks - prev_ticks : 0;
            p->cpu_percent = 100.0 * (gdouble)delta / ((gdouble)G.clk_tck * dt_sec);
        }
        /* 현재 tick을 저장 → 다음 수집 시 delta 계산에 사용 */
        g_hash_table_insert(G.prev_ticks, GINT_TO_POINTER(pid), GSIZE_TO_POINTER(total_ticks));
        count++;
    }
    closedir(d);

    /*
     * CPU% 내림차순 정렬 → procs[0]이 가장 CPU를 많이 쓰는 프로세스.
     * qsort는 C 표준 라이브러리의 정렬 함수.
     * _sort_by_cpu()가 음수/0/양수를 반환하여 비교 순서를 결정한다.
     * 시간 복잡도: O(n log n), n ≤ PROC_MAX(512)이므로 충분히 빠르다.
     *
     * 정렬 후 배열 구조:
     *   tmp[0]   — CPU% 가장 높은 프로세스 (예: qemu-system-x86 25.3%)
     *   tmp[1]   — 두 번째로 높은 프로세스
     *   ...
     *   tmp[n-1] — CPU% 가장 낮은 프로세스 (예: kthreadd 0.0%)
     */
    qsort(tmp, count, sizeof(ProcInfo), _sort_by_cpu);

    /*
     * 뮤텍스 잠금 후 결과를 G.procs[]에 복사.
     * API 호출(get_top/get_all)과의 경쟁 조건을 방지한다.
     * memcpy 한 번으로 원자적 갱신 효과를 낸다.
     */
    g_mutex_lock(&G.mu);
    memcpy(G.procs, tmp, count * sizeof(ProcInfo));
    G.count = count;
    g_mutex_unlock(&G.mu);

    /* 다음 주기의 dt_sec 계산을 위해 현재 시각 저장 */
    G.prev_time_us = now_us;
}

/**
 * @brief 수집 스레드 메인 루프 — 20초마다 _collect_processes() 호출
 *
 * g_thread_new("proc-monitor", _proc_thread, NULL)으로 생성된다.
 * G.running이 FALSE가 되면 루프를 빠져나와 스레드가 종료된다.
 *
 * 주의: g_usleep()은 시그널에 의해 조기 리턴될 수 있지만,
 * G.running 체크가 루프 상단에 있으므로 문제없다.
 *
 * @param data  미사용 (NULL)
 * @return NULL
 */
static gpointer
_proc_thread(gpointer data)
{
    (void)data;
    PCV_LOG_INFO(PROC_LOG_DOM, "Process monitor started (interval=%ds)", PROC_INTERVAL_SEC);

    while (G.running) {
        _collect_processes();
        g_usleep(PROC_INTERVAL_SEC * G_USEC_PER_SEC);
    }

    PCV_LOG_INFO(PROC_LOG_DOM, "Process monitor stopped");
    return NULL;
}

/* ── JSON 변환 ──────────────────────────────────────────────── */

/**
 * @brief ProcInfo 구조체를 JSON 객체로 변환한다.
 *
 * REST API 및 RPC 응답에 포함될 JSON 형식을 생성한다.
 *
 * 반환 JSON 예시:
 *   {
 *     "pid": 1234,
 *     "comm": "purecvisormd",
 *     "state": "S",
 *     "cpu_percent": 12.5,
 *     "mem_mb": 45.67,
 *     "rss_kb": 46766,
 *     "io_rd_bytes": 1234567,
 *     "io_wr_bytes": 9876543
 *   }
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  RSS → MB / KB 변환 공식
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   rss_pages는 페이지 단위이므로 바이트로 변환하려면 page_size를 곱한다.
 *
 *   mem_mb 계산:
 *     mem_mb = rss_pages * page_size / (1024 * 1024)
 *
 *     예: rss_pages=11691, page_size=4096
 *         → 11691 * 4096 / 1048576 = 45.67 MB
 *
 *   rss_kb 계산:
 *     rss_kb = rss_pages * (page_size / 1024)
 *
 *     예: rss_pages=11691, page_size=4096
 *         → 11691 * 4 = 46764 KB
 *
 *   page_size는 init() 시 sysconf(_SC_PAGESIZE)로 캐싱 (G.page_size).
 *   대부분의 리눅스 시스템에서 4096 바이트 (4KB).
 *
 * @param p  변환할 ProcInfo 포인터
 * @return   JsonObject* — 소유권이 호출자에게 이전됨
 */
static JsonObject *
_proc_to_json(const ProcInfo *p)
{
    JsonObject *obj = json_object_new();
    json_object_set_int_member   (obj, "pid",       p->pid);
    json_object_set_string_member(obj, "comm",      p->comm);
    gchar state_str[2] = {p->state, '\0'};
    json_object_set_string_member(obj, "state",     state_str);
    json_object_set_double_member(obj, "cpu_percent",p->cpu_percent);
    json_object_set_double_member(obj, "mem_mb",
        (gdouble)p->rss_pages * G.page_size / (1024.0 * 1024.0));
    json_object_set_int_member   (obj, "rss_kb",    p->rss_pages * (G.page_size / 1024));
    json_object_set_int_member   (obj, "io_rd_bytes",p->io_rd_bytes);
    json_object_set_int_member   (obj, "io_wr_bytes",p->io_wr_bytes);
    json_object_set_string_member(obj, "type",      _proc_type_str[p->type]);
    json_object_set_string_member(obj, "cgroup",    p->cgroup);
    return obj;
}

/* ── 공개 API ───────────────────────────────────────────────── */

/**
 * @brief 프로세스 모니터 초기화 — 시스템 상수 캐싱 + 스레드 생성
 *
 * 호출 순서:
 *   1. GMutex 초기화
 *   2. sysconf(_SC_PAGESIZE) → G.page_size (보통 4096)
 *   3. sysconf(_SC_CLK_TCK)  → G.clk_tck  (보통 100, 실패 시 100 fallback)
 *   4. prev_ticks GHashTable 생성 (g_direct_hash/equal — 정수 키 직접 비교)
 *   5. GThread("proc-monitor") 생성 → 백그라운드 수집 시작
 *
 * main.c에서 데몬 초기화 중 한 번만 호출한다.
 */
void
pcv_process_monitor_init(void)
{
    g_mutex_init(&G.mu);
    G.page_size = sysconf(_SC_PAGESIZE);
    G.clk_tck = sysconf(_SC_CLK_TCK);
    if (G.clk_tck <= 0) G.clk_tck = 100;
    G.prev_ticks = g_hash_table_new(g_direct_hash, g_direct_equal);
    G.running = TRUE;
    G.initialized = TRUE;
    G.thread = g_thread_new("proc-monitor", _proc_thread, NULL);
}

/**
 * @brief 프로세스 모니터 종료 — 스레드 중지 + 자원 해제
 *
 * 호출 순서:
 *   1. G.running = FALSE → 스레드 루프 탈출 신호
 *   2. g_thread_join() → 스레드 종료 대기 (최대 20초 후 깨어남)
 *   3. prev_ticks GHashTable 해제
 *   4. GMutex 해제
 *   5. G.initialized = FALSE → 이중 shutdown 방지
 *
 * main.c에서 데몬 종료 시 호출한다.
 * init()이 호출되지 않았으면(initialized == FALSE) 아무것도 하지 않는다.
 */
void
pcv_process_monitor_shutdown(void)
{
    if (!G.initialized) return;
    G.running = FALSE;
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = NULL;
    }
    if (G.prev_ticks) {
        g_hash_table_destroy(G.prev_ticks);
        G.prev_ticks = NULL;
    }
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}

/**
 * @brief CPU% 기준 상위 N개 프로세스를 JsonArray로 반환
 *
 * 스레드 안전: GMutex 잠금 → procs[] 순회 → JsonArray에 복사 → 해제.
 * 따라서 호출자는 반환된 JsonArray를 락 없이 자유롭게 사용할 수 있다.
 *
 * G.procs[]는 이미 CPU% 내림차순으로 정렬되어 있으므로,
 * 앞에서 n개만 잘라내면 Top N이 된다.
 *
 * @param n  반환할 프로세스 수. n <= 0 또는 n >= count이면 전체 반환.
 * @return   JsonArray* — 소유권이 호출자에게 이전됨. json_array_unref() 필수.
 */
JsonArray *
pcv_process_monitor_get_top(gint n)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    gint limit = (n > 0 && n < G.count) ? n : G.count;
    for (gint i = 0; i < limit; i++)
        json_array_add_object_element(arr, _proc_to_json(&G.procs[i]));
    g_mutex_unlock(&G.mu);
    return arr;
}

/**
 * @brief 수집된 전체 프로세스 목록을 JsonArray로 반환
 *
 * pcv_process_monitor_get_top(G.count)를 호출하는 편의 함수.
 * CPU% 내림차순 정렬된 전체 목록이 반환된다.
 *
 * @return   JsonArray* — 소유권이 호출자에게 이전됨. json_array_unref() 필수.
 */
JsonArray *
pcv_process_monitor_get_all(void)
{
    return pcv_process_monitor_get_top(G.count);
}

/**
 * @brief 프로세스 유형 문자열을 ProcType 열거형으로 변환한다.
 *
 * @param type_str  "host", "vm", "container", "system" 중 하나
 * @return ProcType 값. 인식 불가 시 -1 반환
 */
static gint
_parse_type_filter(const gchar *type_str)
{
    if (!type_str || !*type_str) return -1;
    if (g_strcmp0(type_str, "host") == 0)      return PROC_HOST;
    if (g_strcmp0(type_str, "vm") == 0)        return PROC_VM;
    if (g_strcmp0(type_str, "container") == 0) return PROC_CONTAINER;
    if (g_strcmp0(type_str, "system") == 0)    return PROC_SYSTEM;
    return -1;
}

/**
 * @brief 프로세스 유형 필터를 적용하여 상위 N개를 JsonArray로 반환
 *
 * type_str이 유효한 유형("host"/"vm"/"container"/"system")이면
 * 해당 유형의 프로세스만 필터링하여 반환한다.
 * type_str이 NULL이거나 인식 불가면 필터 없이 전체 반환.
 *
 * @param n         반환할 최대 프로세스 수 (0이면 제한 없음)
 * @param type_str  프로세스 유형 필터 (NULL이면 필터 없음)
 * @return JsonArray* — 소유권 호출자 이전. json_array_unref() 필수.
 */
JsonArray *
pcv_process_monitor_get_filtered(gint n, const gchar *type_str)
{
    gint type_filter = _parse_type_filter(type_str);

    /* 필터 없으면 기존 함수로 위임 */
    if (type_filter < 0)
        return (n > 0) ? pcv_process_monitor_get_top(n) : pcv_process_monitor_get_all();

    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    gint added = 0;
    for (gint i = 0; i < G.count; i++) {
        if ((gint)G.procs[i].type != type_filter) continue;
        json_array_add_object_element(arr, _proc_to_json(&G.procs[i]));
        added++;
        if (n > 0 && added >= n) break;
    }
    g_mutex_unlock(&G.mu);
    return arr;
}
