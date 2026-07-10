/**
 * @file purecvisortui.c
 * @brief ncursesw 기반 Single Edge TUI (pcvtui) -- VM/NET/STG/CTR/HOST/OVN 뷰
 *
 * ====================================================================
 *  파일 역할
 * ====================================================================
 *  pcvtui는 현재 에디션 데몬의 ncursesw 터미널 대시보드이다.
 *  UDS 소켓으로 JSON-RPC 2.0 요청을 보내고, 응답을 파싱하여
 *  실시간 컬러 테이블, 브레일 그래프, 스파크라인 등으로 시각화한다.
 *  단일 파일 ~4,947 LOC 구조이며, GThread 백그라운드 폴링으로
 *  UI 블로킹 없이 데이터를 갱신한다.
 *
 * ====================================================================
 *  아키텍처 위치
 * ====================================================================
 *  클라이언트 계층. pcvctl(CLI)과 동일하게 UDS 소켓만으로 통신한다.
 *
 *    ncursesw 화면
 *        |
 *    purecvisortui (이 파일)
 *        | UDS JSON-RPC 2.0
 *    purecvisorsd (Single Edge 데몬)
 *        | dispatcher.c
 *    핸들러 -> libvirt / ZFS / LXC / OVS ...
 *
 * ====================================================================
 *  주요 흐름
 * ====================================================================
 *  1. main(): ncursesw 초기화 + 컬러 페어 등록 + GThread 백그라운드 시작
 *  2. GThread가 주기적(1s/3s/10s)으로 UDS RPC 호출, GMutex로 데이터 교환
 *  3. 메인 스레드: getch() 이벤트 루프 -> handle_key_*() 키 디스패치
 *  4. draw_view_*() 함수가 현재 탭에 맞는 화면 렌더링
 *  5. DirtyFlags 비트마스크로 변경된 패널만 선택적 재드로우
 *
 * ====================================================================
 *  탭 구성 (F1~F7)
 * ====================================================================
 *  F1: VM 뷰   -- 목록 + 인스펙터 (상태/VNC/핫플러그/스냅샷)
 *               CPU/MEM 퍼센트, 6단계 정렬 (NAME/CPU/MEM/STATE/VCPU/RAM)
 *  F2: NET 뷰  -- 브릿지 목록 (NAT/isolated/routed/bridge), 10초 자동 갱신
 *  F3: STG 뷰  -- ZFS 풀/zvol 관리, 용량 바 (M/K 단위 변환)
 *  F4: CTR 뷰  -- LXC 컨테이너 관리, metrics/exec 지원
 *  F5: HOST 뷰 -- btop 스타일 호스트 대시보드 (CPU 코어별 바)
 *  F6: HA 뷰   -- 클러스터 역할/RPO/노드맵/전체 VM 목록 (3초 폴링)
 *  F7: OVN 뷰  -- OVN 논리 스위치/라우터/ACL/NAT/멀티테넌트
 *
 * ====================================================================
 *  핵심 패턴
 * ====================================================================
 *  - Elm Architecture (Model/Update/View):
 *    TuiState(모델) + handle_key_*(업데이트) + draw_view_*(뷰) 분리.
 *  - GThread + GMutex atomic swap:
 *    백그라운드 스레드가 RPC 결과를 수집하고, GMutex 잠금 후
 *    포인터를 교환하여 메인 스레드에 전달. UI 블로킹 제로.
 *  - 브레일 시각화 (BrailleChart):
 *    유니코드 U+2800~U+28FF 브레일 문자로 셀 1개 = 2x4 픽셀.
 *    스파크라인 대비 8배 해상도의 시계열 그래프 렌더링.
 *  - Ratatui 패턴 차용:
 *    draw_sparkline(), draw_scrollbar(), draw_tab_bar(), draw_table(),
 *    create_popup(), pcv_layout_split() 등 위젯 패턴 적용.
 *  - DirtyFlags 비트마스크: 변경된 패널만 재드로우 (더블 버퍼 diff).
 *
 * ====================================================================
 *  키 바인딩 요약
 * ====================================================================
 *  F1~F7 : 탭 전환
 *  t     : TOP 모드 전환 (BRAILLE/SPARK/COMPACT/BTOP)
 *  s     : 정렬 토글 (6단계)
 *  /     : 이름 인크리멘탈 필터
 *  h     : 도움말 오버레이
 *  H     : 메모리 핫플러그 팝업
 *  v     : vCPU 핫플러그 (vm.set_vcpu)
 *  r     : VM 이름 변경 (vm.rename)
 *  k     : VM 복제 (vm.clone)
 *  O     : OVA 내보내기 (vm.export.ova)
 *  U     : OVA 가져오기 (vm.import.ova)
 *  W     : VM 네트워크 대역폭 제한 (vm.set_bandwidth)
 *  Y     : USB 패스스루 목록/연결/분리 (vm.usb.*)
 *  e     : ISO 이젝트 (vm.eject)
 *  l     : VM 스냅샷 목록 오버레이
 *  r     : VM 스냅샷 롤백 (오버레이 내)
 *  D     : 삭제 (스냅샷/브릿지/컨테이너, 컨텍스트별)
 *  L     : CPU/MEM 쿼터 (vm.limit)
 *  V     : VNC 주소 조회 (vm.vnc)
 *  R     : 갱신 주기 토글 (1s/3s/10s)
 *  N     : VM NIC 목록 (device.nic.list)
 *  +     : NIC 핫플러그 추가 (device.nic.attach)
 *  -     : NIC 핫플러그 제거 (device.nic.detach)
 *  *     : 다중 선택
 *  Esc   : 필터 해제 / 팝업 닫기
 *
 * ====================================================================
 *  주의사항
 * ====================================================================
 *  - C_WHITE 컬러 상수는 선언되어 있지 않다. 사용하면 컴파일 에러 발생.
 *    반드시 C_CPU, C_MEM, C_DIM 등 정의된 상수만 사용할 것.
 *  - 컬러 페어 상수: C_CPU=1, C_MEM=2, C_FLEET=3, C_GREEN=4, C_RED=5,
 *    C_YELLOW=6, C_DIM=7, C_HIGHLIGHT=8, C_LOG=9, C_TITLE=10, C_CYAN=11,
 *    C_TAB_ACT=12, C_SPARK_CPU=13, C_SPARK_NET=14, C_GRAD_BASE=15~21
 *  - 이 파일은 단일 파일 ~4,947 LOC 구조이다. 분할 시 TuiState 구조체와
 *    GThread 공유 데이터의 소유권에 주의해야 한다.
 *  - ncursesw 와이드 문자 지원이 필수이다 (한글/브레일 문자 렌더링).
 *
 * ====================================================================
 *  새 탭(뷰) 추가 방법 (주니어 개발자 필독)
 * ====================================================================
 *  예: F8 키에 "AUDIT" 탭을 추가한다고 가정.
 *
 *  1단계: TuiView 열거형에 항목 추가
 *    TUI_VIEW_AUDIT 을 TUI_VIEW_COUNT 바로 앞에 삽입.
 *    TUI_VIEW_COUNT가 자동으로 8이 된다.
 *
 *  2단계: 탭 바 레이블 배열에 추가
 *    tab_labels[] = {"VM","NET","STG","CTR","HOST","HA","OVN","AUDIT"}
 *
 *  3단계: 데이터 구조체 정의 (필요 시)
 *    AuditInfo 구조체를 정의하고, TuiState에 배열 + 카운트 추가.
 *
 *  4단계: 백그라운드 폴링 스레드에서 RPC 호출 추가
 *    bg_poll_thread() 안에서 tui_send_request("audit.list", ...)로
 *    데이터를 수집하고, GMutex 잠금 후 포인터를 교환.
 *
 *  5단계: 화면 렌더링 함수 작성
 *    static void draw_view_audit(TuiState *st) { ... }
 *    draw_panel(), draw_table(), draw_bar() 등 tui_widgets.c의
 *    위젯 함수를 조합하여 화면을 구성.
 *
 *  6단계: 메인 이벤트 루프에 연결
 *    - draw_current_view()의 switch에 case TUI_VIEW_AUDIT 추가
 *    - handle_key_*()에 F8 키 매핑 추가
 *
 * ====================================================================
 *  ncurses 컬러 상수 시스템
 * ====================================================================
 *  TUI의 모든 색상은 tui_widgets.h에 정의된 C_* 상수를 사용한다.
 *  이 상수들은 ncurses COLOR_PAIR(n) 인덱스이다.
 *
 *  main()에서 init_pair()로 초기화:
 *    init_pair(C_CPU,   COLOR_CYAN,    COLOR_BLACK);  // CPU 관련 색
 *    init_pair(C_MEM,   COLOR_MAGENTA, COLOR_BLACK);  // 메모리 관련 색
 *    init_pair(C_GREEN, COLOR_GREEN,   COLOR_BLACK);  // 정상 상태
 *    init_pair(C_RED,   COLOR_RED,     COLOR_BLACK);  // 에러/경고
 *    ...
 *
 *  사용 방법:
 *    wattron(win, COLOR_PAIR(C_GREEN) | A_BOLD);
 *    mvwprintw(win, y, x, "RUNNING");
 *    wattroff(win, COLOR_PAIR(C_GREEN) | A_BOLD);
 *
 *  그래디언트 (사용률 기반 자동 색상):
 *    int cp = pcv_color_for_pct(ratio);  // 0.0~1.0 → C_GRAD_0~C_GRAD_6
 *    wattron(win, COLOR_PAIR(cp));
 *
 *  [중요] C_WHITE는 미선언 — 사용 시 컴파일 에러. 반드시 정의된 상수만 사용.
 */
#include <ncurses.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <time.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/file.h>     /* B7-C4: flock */
#include <fcntl.h>        /* B7-C4: open flags */
#include <errno.h>        /* B7-C4: errno */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

/* ── 분리된 모듈 include ──────────────────────────────────────────── */
#include "tui_rpc.h"       /* RPC 통신 + JSON 유틸 (safe_str/safe_double/safe_int/format_bytes) */
#include "tui_widgets.h"   /* 위젯 렌더링 (draw_panel/draw_bar/draw_sparkline/braille 등) */

// =============================================================================
// DEFINES — 상수 정의
// =============================================================================
//
// 이 섹션의 상수들은 TUI의 데이터 구조 크기와 동작 한계를 정의한다.
// 실제 운영 환경의 VM/네트워크/스토리지 수가 이 값을 초과하면
// 초과분은 표시되지 않는다. 필요 시 상한을 늘리고 재빌드할 것.
//

#define MAX_VMS         64    /* 표시 가능한 최대 VM 수 */
#define MAX_NET         32    /* 표시 가능한 최대 브릿지 수 */
#define MAX_POOL        16    /* 표시 가능한 최대 ZFS 풀 수 */
#define MAX_ZVOL        128   /* 표시 가능한 최대 zvol 수 */
#define MAX_CTR         64    /* 표시 가능한 최대 컨테이너 수 */
#define MAX_SNAP        64    /* 스냅샷 오버레이에서 표시할 최대 스냅샷 수 */

#define LOG_BUF_SIZE    64    /* 이벤트 로그 환형 버퍼 크기 (최근 64개 이벤트) */
#define LOG_LINE_MAX    256   /* 로그 한 줄 최대 길이 */
#define MAX_CORES       256   /* btop 뷰에서 표시할 최대 CPU 코어 수 */
// (CPU_HIST_SIZE / NET_HIST_SIZE → HIST_SIZE 로 통합)

// ── 상단 패널 표시 모드 ───────────────────────────────────────────────────────
//
// 't' 키로 순환 전환된다 (BRAILLE → SPARK → COMPACT → BTOP → BRAILLE).
// 터미널 너비에 따라 자동 폴백되기도 한다 (예: 50열 미만 → COMPACT 강제).
//
typedef enum {
    TOP_MODE_BRAILLE = 0,   // 브레일 고해상도 차트 (기본) — 2x4 픽셀/셀, CPU+MEM+NET
    TOP_MODE_SPARK,          // 단일 라인 스파크라인 — ▁▂▃▄▅▆▇█ UTF-8 블록 문자
    TOP_MODE_COMPACT,        // 텍스트 전용 (좁은 터미널) — CPU:X% MEM:X% DSK:X%
    TOP_MODE_BTOP,           // btop 스타일 — 코어별 CPU 바 + MEM/SWP 세그먼트 + Load/Temp
    TOP_MODE_COUNT           // 모드 총 개수 (순환용)
} TopPanelMode;

// ── 히스토리 버퍼 (브레일 차트용 확장) ──────────────────────────────────────
#define HIST_SIZE   120     // 2분 (1초 간격 기준)
#define VM_MINI_HIST 20     // VM 인라인 미니 차트 포인트 수

// =============================================================================
// DATA TYPES
// =============================================================================

typedef enum {
    TUI_VIEW_VM = 0,
    TUI_VIEW_NET,           // F2: Bridge 네트워크
    TUI_VIEW_STG,           // F3: 스토리지
    TUI_VIEW_CTR,           // F4: 컨테이너
    TUI_VIEW_HOST,          // F5: 호스트 대시보드
    TUI_VIEW_OVN,           // F7: OVN SDN
    TUI_VIEW_COUNT
} TuiView;

// ScrollState, Spinner → tui_widgets.h로 이동

// ── 이벤트 로그 엔트리 ───────────────────────────────────────────────────────
typedef enum { LOG_INFO=0, LOG_WARN, LOG_ERROR, LOG_SUCCESS, LOG_SYS } LogLevel;
typedef struct {
    gchar    msg[LOG_LINE_MAX];
    time_t   ts;
    LogLevel level;
} LogEntry;

// ── VM 메트릭 ────────────────────────────────────────────────────────────────
typedef struct {
    gchar  name[64];
    gchar  state[32];
    gchar  ip[64];
    gchar  mac[32];
    gchar  net_source[64];
    gint   vcpu;
    gdouble mem_max_mb;
    gdouble mem_used_mb;
    double  mem_percent;
    int     is_running;
    unsigned long long disk_rd;
    unsigned long long disk_wr;
    unsigned long long net_rx;
    unsigned long long net_tx;
    gint64  cpu_time_ns;      /* monitor.fleet에서 수신한 누적 CPU time */
    gint64  prev_cpu_time_ns; /* 이전 폴링의 CPU time (CPU% 계산용) */
    gint    live_cpu_pct;
    gint    live_mem_pct;
    gboolean has_live;
    // K1-VM: VMware 스타일 디바이스 정보
    gchar  uuid[64];
    gchar  net_model[32];
    gchar  disk_path[256];
    gchar  disk_size[32];
    gchar  disk_bus[16];
    gchar  cdrom_path[256];
    gchar  vnc_port[16];
    gboolean autostart;
    gboolean persistent;
} VMMetrics;

// ── 네트워크 정보 (K1: VMware VNE 필드 확장) ───────────────────────────────
typedef struct {
    gchar    name[64];
    gchar    state[8];       // "up" / "down"
    gchar    ip_cidr[48];    // "10.10.0.1/24"
    gchar    mode[16];       // "nat" / "bridge" / "isolated" / "routed"
    gboolean dhcp;           // K1: dnsmasq 동작 여부
    gchar    phys[32];       // K1: 물리 업링크 NIC ("-" if none)
    gchar    subnet[48];     // K1: 서브넷 주소 "10.10.0.0/24"
    gchar    slaves[128];    // K1: 연결된 vnet 인터페이스 목록
    int      vm_count;       // K1: 연결된 VM 수 (캐시)
} NetInfo;

// ── 스토리지 정보 ─────────────────────────────────────────────────────────────
typedef struct {
    gchar name[64];
    gchar size[16];
    gchar alloc[16];
    gchar free_[16];
    gchar health[16];
} PoolInfo;

typedef struct {
    gchar path[128];
    gchar size[16];
    gchar used[16];
} ZvolInfo;

// ── 컨테이너 정보 ─────────────────────────────────────────────────────────────
typedef struct {
    gchar name[64];
    gchar state[32];
    gchar ip_addr[64];
    gchar image[64];
    // ── 캐시된 메트릭 (container.metrics RPC 결과) ──
    gdouble  cpu_percent;
    gdouble  mem_used_mb;
    gdouble  mem_limit_mb;
    gdouble  net_rx_mb;
    gdouble  net_tx_mb;
    gboolean metrics_loaded;   // 메트릭 조회 완료 여부
} CtrInfo;

// ── 호스트 메트릭 ─────────────────────────────────────────────────────────────
typedef struct {
    gchar  cpu_model[128];
    gint   cpus;
    gdouble mem_total_gb;
    gdouble mem_percent;
    gdouble disk_percent;
    gchar  net_iface[32];
    unsigned long long cpu_total_ticks;
    unsigned long long cpu_idle_ticks;
    unsigned long long net_rx_bytes;
    unsigned long long net_tx_bytes;
    // btop 확장 필드
    gdouble uptime_secs;
    gdouble load_1, load_5, load_15;
    gdouble cpu_temp_c;
    gdouble swap_total_gb, swap_used_gb;
    gdouble mem_buffers_mb, mem_cached_mb;
    gdouble mem_used_gb, mem_avail_gb, mem_free_gb;
    gdouble disk_total_gb, disk_used_gb;
    // 코어별 tick (델타 계산용)
    int    core_count;
    unsigned long long core_total[MAX_CORES];
    unsigned long long core_idle[MAX_CORES];
} HostMetrics;

// ── FleetSnapshot (백그라운드 스레드 → UI 스레드 원자적 교환) ────────────────
typedef struct {
    VMMetrics  fleet[MAX_VMS];
    int        fleet_count;
    HostMetrics host;
    gboolean   valid;
    gchar      error[256];
} FleetSnapshot;

// ── TuiEvent (rat-salsa 이벤트 큐 패턴) ─────────────────────────────────────
typedef enum {
    TUI_EVENT_FLEET_UPDATED,
    TUI_EVENT_NET_UPDATED,
    TUI_EVENT_STG_UPDATED,
    TUI_EVENT_CTR_UPDATED,
    TUI_EVENT_CMD_DONE,
    TUI_EVENT_CMD_FAIL,
} TuiEventType;

typedef struct {
    TuiEventType type;
    gchar        message[256];  // 로그 메시지 또는 오류 내용
} TuiEvent;

// ── Dirty 플래그 (더블 버퍼 diff 패턴) ─────────────────────────────────────
typedef struct {
    gboolean top;
    gboolean roster;
    gboolean detail;
    gboolean log_area;
    gboolean tab_bar;
    gboolean all;
} DirtyFlags;

// ── 스냅샷 오버레이 상태 ─────────────────────────────────────────────────────
typedef struct {
    gboolean active;
    gchar    vm_name[64];
    gchar    snaps[MAX_SNAP][128];
    int      count;
    int      selected;
    ScrollState scroll;
} SnapOverlay;

// ── TuiState 통합 구조체 (Elm Model) ─────────────────────────────────────────
//
// Elm Architecture의 Model에 해당한다.
// TUI의 모든 화면 상태를 단일 구조체에 보관하며,
// 키 입력(Update)으로 변경 → draw_view_*(View)로 렌더링된다.
//
// 스레드 안전성:
//   - 이 구조체는 메인 스레드(getch 루프)에서만 직접 읽고 쓴다.
//   - 백그라운드 fleet_worker 스레드는 FleetSnapshot을 통해 간접적으로 데이터를 전달하며,
//     process_events()에서 GMutex 보호 하에 TuiState로 복사한다.
//   - 따라서 g_state 필드에 직접 접근하는 코드는 메인 스레드에서만 실행되어야 한다.
//
typedef struct {
    // 현재 활성 뷰 탭 (F1~F7)
    TuiView  current_view;

    // ── VM 뷰 ─────────────────────────────────────────────────────────────
    struct {
        VMMetrics    fleet[MAX_VMS];
        int          fleet_count;
        int          selected;
        ScrollState  scroll;
        gchar        filter[64];
        gboolean     filter_active;
        SnapOverlay  snap_overlay;
        int          sort_mode;     // U3: 0=NAME 1=CPU 2=MEM 3=STATE
        gboolean     multi_sel[MAX_VMS]; // A3: 다중 선택 비트맵
        int          multi_count;        // A3: 선택된 VM 수
    } vm;

    // ── 네트워크 뷰 ───────────────────────────────────────────────────────
    struct {
        NetInfo      bridges[MAX_NET];
        int          bridge_count;
        int          selected;
        ScrollState  scroll;
        gboolean     needs_refresh;
        gboolean     ovn_focus;       // Tab 토글: OVN 패널 포커스
        gint         ovn_selected;    // OVN 목록 내 선택 인덱스
        // OVN SDN 상태
        struct {
            gboolean available;
            gint     switch_count;
            gint     router_count;
            gchar    switches[16][32];
            gchar    routers[16][32];
            gint     sw_count;
            gint     rt_count;
            // 인스펙터 캐시 (선택 변경 시 갱신)
            gint     detail_for;            // 캐시 대상 인덱스 (-1=없음)
            gchar    detail_ports[8][64];   // 포트 이름 배열
            gint     detail_port_count;
            gchar    detail_acls[8][80];    // ACL 규칙 (스위치용)
            gint     detail_acl_count;
            gchar    detail_nats[4][80];    // NAT 규칙 (라우터용)
            gint     detail_nat_count;
            gchar    detail_port_mac[8][24];   // 라우터 포트 MAC
            gchar    detail_port_net[8][32];   // 라우터 포트 네트워크
        } ovn;

        // Phase 4: DPDK / SR-IOV 캐시
        struct {
            gboolean dpdk_available;
            gint     dpdk_vdev_count;
            gchar    dpdk_pmd_mask[16];
            gboolean sriov_available;
            gint     sriov_pf_count;
            gchar    sriov_pfs[4][32];    // PF 이름
            gint     sriov_vf_counts[4];  // PF별 VF 수
            gint64   last_fetch;
        } accel;
    } net;

    // ── 스토리지 뷰 ───────────────────────────────────────────────────────
    struct {
        PoolInfo     pools[MAX_POOL];
        int          pool_count;
        ZvolInfo     zvols[MAX_ZVOL];
        int          zvol_count;
        int          selected;
        ScrollState  scroll;
        gboolean     needs_refresh;
    } stg;

    // ── 컨테이너 뷰 ───────────────────────────────────────────────────────
    struct {
        CtrInfo      ctrs[MAX_CTR];
        int          ctr_count;
        int          selected;
        ScrollState  scroll;
        gboolean     needs_refresh;
    } ctr;

    // ── 호스트 / 히스토리 ─────────────────────────────────────────────────
    HostMetrics  host;
    double       cpu_hist[HIST_SIZE];
    double       mem_hist[HIST_SIZE];    // MEM %
    double       dsk_hist[HIST_SIZE];    // DISK %
    double       net_rx_hist[HIST_SIZE]; // RX KB/s
    double       net_tx_hist[HIST_SIZE]; // TX KB/s
    int          hist_pos;
    int          hist_count;             // 실제 쌓인 포인트 수 (최대 HIST_SIZE)
    double       cpu_pct;
    unsigned long long last_cpu_total;
    unsigned long long last_cpu_idle;
    unsigned long long last_rx;
    unsigned long long last_tx;
    unsigned long long rx_speed;
    unsigned long long tx_speed;

    // ── 코어별 CPU (btop) ─────────────────────────────────────────────────
    int          core_count;
    double       core_pct[MAX_CORES];
    unsigned long long last_core_total[MAX_CORES];
    unsigned long long last_core_idle[MAX_CORES];

    // ── 상단 패널 모드 ────────────────────────────────────────────────────
    TopPanelMode top_mode;

    // ── 로그 환형 버퍼 ────────────────────────────────────────────────────
    LogEntry     log_buf[LOG_BUF_SIZE];
    int          log_head;
    int          log_count;
    int          log_scroll;
    gboolean     log_paused;   // U4: 스크롤 멈춤 (PgUp 시)
    int          log_offset;   // U4: 뷰포트 오프셋 (0=최신)

    // ── 스피너 ────────────────────────────────────────────────────────────
    Spinner      spinner;

    // ── 갱신 주기 ─────────────────────────────────────────────────────────
    int          refresh_ms;     // 1000 / 3000 / 10000
    gboolean     pending_fetch;  // 뷰 전환 후 즉시 갱신 필요

    // ── 더티 플래그 ───────────────────────────────────────────────────────
    DirtyFlags   dirty;

    // ── 글로벌 오류 ───────────────────────────────────────────────────────
    gchar        error[256];
} TuiState;

// =============================================================================
// GLOBALS (스레드 공유)
// =============================================================================
//
// 멀티 스레드 데이터 흐름:
//
//   fleet_worker (백그라운드 GThread)
//       ↓ GMutex 보호 하에 FleetSnapshot 기록
//   g_fleet_snap
//       ↓ process_events()에서 메인 스레드로 복사
//   g_state (메인 스레드 전용)
//       ↓ draw_tui()에서 렌더링
//   ncurses 화면
//
//   비동기 RPC 결과는 GAsyncQueue를 통해 TuiEvent로 전달된다.
//   async_rpc_worker() → g_event_queue → process_events()
//
static GAsyncQueue *g_event_queue   = NULL;   /* TuiEvent 비동기 이벤트 큐 (스레드 안전) */
static GMutex       g_fleet_mu;               /* g_fleet_snap 보호용 뮤텍스 */
static FleetSnapshot g_fleet_snap   = {0};    /* 백그라운드→메인 전달용 스냅샷 */
static gint         g_quit_fleet    = 0;      /* fleet_worker 종료 플래그 (g_atomic_int) */

/** 전역 TUI 상태 — 메인 스레드에서만 접근 */
static TuiState g_state = {
    .current_view = TUI_VIEW_VM,
    .refresh_ms   = 3000,  /* 기본 3s */
    .vm.filter_active = FALSE,
};

// =============================================================================
// UTILITY
// =============================================================================
// U4: 레벨 기반 로그 푸시
static void push_log_level(const gchar *msg, LogLevel level) {
    time_t rawtime; struct tm *ti; char ts[10];
    time(&rawtime); ti = localtime(&rawtime);
    strftime(ts, sizeof(ts), "%H:%M:%S", ti);

    int idx = g_state.log_head % LOG_BUF_SIZE;
    snprintf(g_state.log_buf[idx].msg, LOG_LINE_MAX, "[%s] %s", ts, msg);
    g_state.log_buf[idx].ts    = rawtime;
    g_state.log_buf[idx].level = level;
    g_state.log_head = (g_state.log_head + 1) % LOG_BUF_SIZE;
    if (g_state.log_count < LOG_BUF_SIZE) g_state.log_count++;
    // 자동 스크롤: 멈춤 중이 아니면 최신으로 복귀
    if (!g_state.log_paused) g_state.log_offset = 0;
    g_state.dirty.log_area = TRUE;
}

// 호환 래퍼 — 기존 push_log() 호출 유지
static void push_log(const gchar *msg) {
    // 레벨 자동 감지
    LogLevel lv = LOG_INFO;
    if (strstr(msg,"ERR")||strstr(msg,"FAIL")||strstr(msg,"fail")) lv = LOG_ERROR;
    else if (strstr(msg,"WARN")||strstr(msg,"warn"))               lv = LOG_WARN;
    else if (strstr(msg,"OK")||strstr(msg,"ok")||strstr(msg,"SUCCESS")) lv = LOG_SUCCESS;
    else if (strstr(msg,"→")||strstr(msg,"Established")||strstr(msg,"F1=")) lv = LOG_SYS;
    push_log_level(msg, lv);
}

// safe_str, safe_double, safe_int, format_bytes → tui_rpc.c로 이동

static gboolean vm_state_is_shutoff(const gchar *state) {
    return g_strcmp0(state, "shutoff") == 0 ||
           g_strcmp0(state, "shut off") == 0 ||
           g_strcmp0(state, "SHUTOFF") == 0 ||
           g_strcmp0(state, "SHUT OFF") == 0;
}

// spinner_start/stop/tick/draw, PcConstraint, pcv_layout_split → tui_widgets.c로 이동

// tui_send_request → tui_rpc.c로 이동

// ── 비동기 RPC 워커 (GThread + GAsyncQueue) ─────────────────────────────────
//
// UI 블로킹 없이 RPC를 실행하기 위한 워커 패턴.
// send_async_rpc()가 호출되면:
//   1. AsyncRpcCtx를 힙 할당하고 method/params/메시지를 복사
//   2. 스피너 애니메이션 시작
//   3. 새 GThread 생성 → async_rpc_worker() 실행
//   4. 워커가 동기 RPC 호출 후 결과를 TuiEvent로 포장하여 g_event_queue에 푸시
//   5. 메인 스레드의 process_events()가 이벤트를 소비하고 로그에 기록
//
// 소유권 규칙:
//   - params의 소유권은 send_async_rpc() 호출 시 워커로 이전된다.
//     tui_send_request()가 내부적으로 params를 JSON-RPC에 포함시켜 해제한다.
//   - AsyncRpcCtx는 워커가 g_free()로 해제한다.
//
typedef struct {
    gchar        method[128];   /* RPC 메서드명 */
    JsonObject  *params;        /* JSON-RPC params (소유권 인계됨) */
    gchar        log_ok[128];   /* 성공 시 이벤트 로그 메시지 */
    gchar        log_fail[64];  /* 실패 시 이벤트 로그 접두사 */
} AsyncRpcCtx;

static gpointer async_rpc_worker(gpointer data) {
    AsyncRpcCtx *ctx = data;
    GError *err = NULL;
    gchar  *resp = tui_send_request(ctx->method, ctx->params, &err);

    TuiEvent *ev = g_new0(TuiEvent, 1);
    if (err) {
        ev->type = TUI_EVENT_CMD_FAIL;
        snprintf(ev->message, sizeof(ev->message),
                 "%s FAILED: %s", ctx->log_fail, err->message);
        g_error_free(err);
    } else if (resp) {
        // JSON 파싱으로 error 여부 확인
        JsonParser *p = json_parser_new();
        gboolean has_err = FALSE;
        if (json_parser_load_from_data(p, resp, -1, NULL)) {
            JsonObject *root_obj = json_node_get_object(json_parser_get_root(p));
            has_err = json_object_has_member(root_obj, "error");
            if (has_err) {
                /* B7-M4: has_member != object null-safe */
                JsonObject *err_obj = json_object_get_object_member(root_obj, "error");
                ev->type = TUI_EVENT_CMD_FAIL;
                snprintf(ev->message, sizeof(ev->message),
                         "%s ERR: %s", ctx->log_fail,
                         err_obj ? safe_str(err_obj, "message", "unknown")
                                 : "malformed error response");
            }
        }
        g_object_unref(p);
        if (!has_err) {
            ev->type = TUI_EVENT_CMD_DONE;
            /* B7-M5: strncpy NUL termination guarantee */
            strncpy(ev->message, ctx->log_ok, sizeof(ev->message)-1);
            ev->message[sizeof(ev->message)-1] = '\0';
        }
        g_free(resp);
    } else {
        ev->type = TUI_EVENT_CMD_FAIL;
        snprintf(ev->message, sizeof(ev->message), "%s: no response", ctx->log_fail);
    }

    g_async_queue_push(g_event_queue, ev);
    g_free(ctx);
    return NULL;
}

// 호출 후 params 소유권이 worker로 넘어감
static void send_async_rpc(const gchar *method, JsonObject *params,
                           const gchar *log_ok, const gchar *log_fail) {
    AsyncRpcCtx *ctx = g_new0(AsyncRpcCtx, 1);
    /* B7-M5: strncpy NUL termination guarantee */
    strncpy(ctx->method,   method,   sizeof(ctx->method)-1);
    ctx->method[sizeof(ctx->method)-1] = '\0';
    strncpy(ctx->log_ok,   log_ok,   sizeof(ctx->log_ok)-1);
    ctx->log_ok[sizeof(ctx->log_ok)-1] = '\0';
    strncpy(ctx->log_fail, log_fail, sizeof(ctx->log_fail)-1);
    ctx->log_fail[sizeof(ctx->log_fail)-1] = '\0';
    ctx->params = params;
    spinner_start(&g_state.spinner, log_fail);
    g_thread_new("rpc-async", async_rpc_worker, ctx);
}

// =============================================================================
// BACKGROUND FLEET THREAD (A1 — UI 블로킹 제거)
// =============================================================================
//
// 백그라운드 스레드가 주기적으로 "monitor.fleet" RPC를 호출하여
// 호스트 메트릭(CPU/MEM/DISK/NET) + VM 목록을 수집한다.
//
// 데이터 흐름:
//   1. tui_send_request("monitor.fleet") → JSON 응답 수신
//   2. JSON 파싱 → FleetSnapshot 구조체에 저장
//   3. GMutex 잠금 → g_fleet_snap에 원자적 교환
//   4. TUI_EVENT_FLEET_UPDATED 이벤트를 g_event_queue에 푸시
//   5. g_state.refresh_ms 간격으로 대기 (10ms 단위로 분할하여 빠른 종료 가능)
//
// 종료 조건: g_quit_fleet 플래그가 1로 설정되면 루프 탈출
//
static gpointer fleet_worker(gpointer unused __attribute__((unused))) {
    while (!g_atomic_int_get(&g_quit_fleet)) {
        GError *err = NULL;
        gchar  *resp = tui_send_request("monitor.fleet", NULL, &err);

        FleetSnapshot snap = {0};
        if (err) {
            strncpy(snap.error, err->message, sizeof(snap.error)-1);
            g_error_free(err);
        } else if (resp) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, resp, -1, NULL)) {
                JsonObject *root_obj = json_node_get_object(json_parser_get_root(p));
                if (json_object_has_member(root_obj, "error")) {
                    JsonObject *eo = json_object_get_object_member(root_obj, "error");
                    strncpy(snap.error, safe_str(eo,"message","error"),
                            sizeof(snap.error)-1);
                } else if (json_object_has_member(root_obj, "result")) {
                    JsonObject *res = json_object_get_object_member(root_obj, "result");

                    // host 메트릭
                    if (json_object_has_member(res, "host")) {
                        JsonObject *h = json_object_get_object_member(res, "host");
                        strncpy(snap.host.cpu_model,
                                safe_str(h,"cpu_model","?"),
                                sizeof(snap.host.cpu_model)-1);
                        snap.host.cpus        = (gint)safe_int(h,"cpus");
                        snap.host.cpu_total_ticks = (unsigned long long)safe_int(h,"cpu_total_ticks");
                        snap.host.cpu_idle_ticks  = (unsigned long long)safe_int(h,"cpu_idle_ticks");
                        snap.host.mem_total_gb    = safe_double(h,"mem_total_gb");
                        snap.host.mem_percent     = safe_double(h,"mem_percent");
                        snap.host.disk_percent    = safe_double(h,"disk_percent");
                        strncpy(snap.host.net_iface,
                                safe_str(h,"net_iface","N/A"),
                                sizeof(snap.host.net_iface)-1);
                        snap.host.net_rx_bytes    = (unsigned long long)safe_int(h,"net_rx_bytes");
                        snap.host.net_tx_bytes    = (unsigned long long)safe_int(h,"net_tx_bytes");
                        // btop 확장 필드
                        snap.host.uptime_secs     = safe_double(h,"uptime_secs");
                        snap.host.load_1          = safe_double(h,"load_1");
                        snap.host.load_5          = safe_double(h,"load_5");
                        snap.host.load_15         = safe_double(h,"load_15");
                        snap.host.cpu_temp_c      = safe_double(h,"cpu_temp_c");
                        snap.host.swap_total_gb   = safe_double(h,"swap_total_gb");
                        snap.host.swap_used_gb    = safe_double(h,"swap_used_gb");
                        snap.host.mem_buffers_mb  = safe_double(h,"mem_buffers_mb");
                        snap.host.mem_cached_mb   = safe_double(h,"mem_cached_mb");
                        snap.host.mem_used_gb     = safe_double(h,"mem_used_gb");
                        snap.host.mem_avail_gb    = safe_double(h,"mem_avail_gb");
                        snap.host.mem_free_gb     = safe_double(h,"mem_free_gb");
                        snap.host.disk_total_gb   = safe_double(h,"disk_total_gb");
                        snap.host.disk_used_gb    = safe_double(h,"disk_used_gb");
                        // 코어별 tick
                        if (json_object_has_member(h, "cores")) {
                            JsonArray *ca = json_object_get_array_member(h, "cores");
                            int nc = (int)json_array_get_length(ca);
                            snap.host.core_count = nc < MAX_CORES ? nc : MAX_CORES;
                            for (int c = 0; c < snap.host.core_count; c++) {
                                JsonObject *co = json_array_get_object_element(ca, c);
                                snap.host.core_total[c] = (unsigned long long)safe_int(co,"total");
                                snap.host.core_idle[c]  = (unsigned long long)safe_int(co,"idle");
                            }
                        }
                    }

                    // fleet 배열
                    if (json_object_has_member(res, "fleet")) {
                        JsonArray *arr = json_object_get_array_member(res, "fleet");
                        if (arr) {
                            int n = (int)json_array_get_length(arr);
                            snap.fleet_count = n < MAX_VMS ? n : MAX_VMS;
                            for (int i = 0; i < snap.fleet_count; i++) {
                                JsonObject *vo = json_array_get_object_element(arr, i);
                                VMMetrics  *vm = &snap.fleet[i];
                                strncpy(vm->name,       safe_str(vo,"name","?"),    sizeof(vm->name) - 1);
                                vm->name[sizeof(vm->name) - 1] = '\0';
                                strncpy(vm->state,      safe_str(vo,"state","?"),   sizeof(vm->state) - 1);
                                vm->state[sizeof(vm->state) - 1] = '\0';
                                strncpy(vm->ip,         safe_str(vo,"ip","N/A"),    sizeof(vm->ip) - 1);
                                vm->ip[sizeof(vm->ip) - 1] = '\0';
                                strncpy(vm->mac,        safe_str(vo,"mac","N/A"),   sizeof(vm->mac) - 1);
                                vm->mac[sizeof(vm->mac) - 1] = '\0';
                                strncpy(vm->net_source, safe_str(vo,"net_source","N/A"), sizeof(vm->net_source) - 1);
                                vm->net_source[sizeof(vm->net_source) - 1] = '\0';
                                // K1-VM 신규 필드
                                strncpy(vm->uuid,       safe_str(vo,"uuid",      "N/A"),     sizeof(vm->uuid) - 1);
                                vm->uuid[sizeof(vm->uuid) - 1] = '\0';
                                strncpy(vm->net_model,  safe_str(vo,"net_model", "N/A"),     sizeof(vm->net_model) - 1);
                                vm->net_model[sizeof(vm->net_model) - 1] = '\0';
                                strncpy(vm->disk_path,  safe_str(vo,"disk_path", "N/A"),    sizeof(vm->disk_path) - 1);
                                vm->disk_path[sizeof(vm->disk_path) - 1] = '\0';
                                strncpy(vm->disk_size,  safe_str(vo,"disk_size", "N/A"),     sizeof(vm->disk_size) - 1);
                                vm->disk_size[sizeof(vm->disk_size) - 1] = '\0';
                                strncpy(vm->disk_bus,   safe_str(vo,"disk_bus",  "N/A"),     sizeof(vm->disk_bus) - 1);
                                vm->disk_bus[sizeof(vm->disk_bus) - 1] = '\0';
                                strncpy(vm->cdrom_path, safe_str(vo,"cdrom_path","(empty)"), sizeof(vm->cdrom_path) - 1);
                                vm->cdrom_path[sizeof(vm->cdrom_path) - 1] = '\0';
                                strncpy(vm->vnc_port,   safe_str(vo,"vnc_port",  "N/A"),     sizeof(vm->vnc_port) - 1);
                                vm->vnc_port[sizeof(vm->vnc_port) - 1] = '\0';
                                vm->autostart  = json_object_has_member(vo,"autostart")
                                    ? json_object_get_boolean_member(vo,"autostart") : FALSE;
                                vm->persistent = json_object_has_member(vo,"persistent")
                                    ? json_object_get_boolean_member(vo,"persistent") : FALSE;
                                vm->vcpu       = (gint)safe_int(vo,"vcpu");
                                vm->cpu_time_ns = safe_int(vo,"cpu_time_ns");
                                vm->mem_max_mb = safe_double(vo,"mem_max_mb");
                                vm->mem_used_mb= safe_double(vo,"mem_used_mb");
                                vm->disk_rd    = (unsigned long long)safe_int(vo,"disk_rd_bytes");
                                vm->disk_wr    = (unsigned long long)safe_int(vo,"disk_wr_bytes");
                                vm->net_rx     = (unsigned long long)safe_int(vo,"net_rx_bytes");
                                vm->net_tx     = (unsigned long long)safe_int(vo,"net_tx_bytes");
                                // mem_used_mb=-1: balloon 드라이버 미설치 → N/A
                                vm->mem_percent= (vm->mem_used_mb < 0.0)
                                    ? -1.0
                                    : (vm->mem_max_mb > 0)
                                        ? (vm->mem_used_mb / vm->mem_max_mb) * 100.0
                                        : 0.0;
                                vm->is_running = (g_ascii_strcasecmp(vm->state, "running") == 0);
                            }
                            snap.valid = TRUE;
                        }
                    }
                }
            }
            g_object_unref(p);
            g_free(resp);
        }

        // 원자적 스왑 → UI 스레드는 mutex로 읽기
        g_mutex_lock(&g_fleet_mu);
        g_fleet_snap = snap;
        g_mutex_unlock(&g_fleet_mu);

        // FLEET_UPDATED 이벤트 발행
        TuiEvent *ev = g_new0(TuiEvent, 1);
        ev->type = TUI_EVENT_FLEET_UPDATED;
        g_async_queue_push(g_event_queue, ev);

        // 갱신 주기 대기 (10ms 단위 체크로 빠른 종료 가능)
        int wait_steps = g_state.refresh_ms / 10;
        for (int i = 0; i < wait_steps; i++) {
            if (g_atomic_int_get(&g_quit_fleet)) break;
            g_usleep(10000);
        }
    }
    return NULL;
}

// =============================================================================
// EVENT PROCESSING (main loop에서 호출)
// =============================================================================
//
// Elm Architecture의 Update 단계 중 비동기 이벤트 처리 부분.
// 메인 루프(getch)에서 매 반복마다 호출되어 GAsyncQueue의 이벤트를 소비한다.
//
// 이벤트 타입별 처리:
//   FLEET_UPDATED  → FleetSnapshot을 TuiState로 복사, CPU% 델타 계산, 히스토리 갱신
//   CMD_DONE       → 성공 메시지를 이벤트 로그에 기록, 스피너 중지
//   CMD_FAIL       → 실패 메시지를 이벤트 로그에 기록, 스피너 중지
//
// CPU% 계산 로직 (delta 방식):
//   CPU% = (delta_cpu_time_ns / 1초) / vcpu_count * 100
//   이전 폴링의 cpu_time_ns를 저장하여 차이를 계산한다.
//   첫 폴링에서는 이전값이 0이므로 CPU%가 표시되지 않는다.
//
static void process_events(void) {
    TuiEvent *ev;
    while ((ev = g_async_queue_try_pop(g_event_queue)) != NULL) {
        switch (ev->type) {
        case TUI_EVENT_FLEET_UPDATED: {
            // FleetSnapshot → TuiState.vm 복사
            g_mutex_lock(&g_fleet_mu);
            FleetSnapshot snap = g_fleet_snap;
            g_mutex_unlock(&g_fleet_mu);

            if (snap.error[0]) {
                strncpy(g_state.error, snap.error, sizeof(g_state.error)-1);
                g_state.vm.fleet_count = 0;
            } else if (snap.valid) {
                g_state.error[0] = '\0';
                g_state.vm.fleet_count = snap.fleet_count;
                // live_metrics: cpu_time_ns 차이로 CPU% 자동 계산
                for (int i = 0; i < snap.fleet_count; i++) {
                    VMMetrics *src = &snap.fleet[i];
                    VMMetrics *dst = &g_state.vm.fleet[i];
                    gint64 prev_cpu = dst->cpu_time_ns;
                    gint  old_mem = dst->live_mem_pct;
                    *dst = *src;
                    dst->prev_cpu_time_ns = prev_cpu;
                    dst->live_mem_pct = old_mem;
                    // CPU% = (delta_cpu_time / delta_wall_time) / vcpu_count * 100
                    if (prev_cpu > 0 && src->cpu_time_ns > prev_cpu && src->vcpu > 0) {
                        gint64 delta_ns = src->cpu_time_ns - prev_cpu;
                        // 폴링 간격 약 1초 = 1,000,000,000 ns
                        double cpu_pct = (double)delta_ns / 1000000000.0 / src->vcpu * 100.0;
                        if (cpu_pct > 100.0) cpu_pct = 100.0;
                        dst->live_cpu_pct = (gint)cpu_pct;
                        dst->has_live = TRUE;
                    }
                }

                // 호스트 메트릭 + 히스토리 업데이트
                g_state.host = snap.host;
                unsigned long long ct = snap.host.cpu_total_ticks;
                unsigned long long ci = snap.host.cpu_idle_ticks;
                if (g_state.last_cpu_total > 0 && (ct - g_state.last_cpu_total) > 0) {
                    g_state.cpu_pct = 100.0
                        * ((ct - g_state.last_cpu_total)
                           - (ci - g_state.last_cpu_idle))
                        / (ct - g_state.last_cpu_total);
                }
                g_state.last_cpu_total = ct;
                g_state.last_cpu_idle  = ci;

                // 코어별 CPU % 델타 계산
                g_state.core_count = snap.host.core_count;
                for (int c = 0; c < snap.host.core_count && c < MAX_CORES; c++) {
                    unsigned long long cct = snap.host.core_total[c];
                    unsigned long long cci = snap.host.core_idle[c];
                    if (g_state.last_core_total[c] > 0 && (cct - g_state.last_core_total[c]) > 0) {
                        g_state.core_pct[c] = 100.0
                            * ((double)(cct - g_state.last_core_total[c])
                               - (double)(cci - g_state.last_core_idle[c]))
                            / (double)(cct - g_state.last_core_total[c]);
                    }
                    g_state.last_core_total[c] = cct;
                    g_state.last_core_idle[c]  = cci;
                }

                unsigned long long rx = snap.host.net_rx_bytes;
                unsigned long long tx = snap.host.net_tx_bytes;
                if (g_state.last_rx > 0) {
                    g_state.rx_speed = rx - g_state.last_rx;
                    g_state.tx_speed = tx - g_state.last_tx;
                }
                g_state.last_rx = rx; g_state.last_tx = tx;

                // 5채널 히스토리 기록 (환형 버퍼)
                int pos = g_state.hist_pos % HIST_SIZE;
                g_state.cpu_hist[pos]    = g_state.cpu_pct;
                g_state.mem_hist[pos]    = snap.host.mem_percent;
                g_state.dsk_hist[pos]    = snap.host.disk_percent;
                g_state.net_rx_hist[pos] = (double)g_state.rx_speed / 1024.0;
                g_state.net_tx_hist[pos] = (double)g_state.tx_speed / 1024.0;
                g_state.hist_pos++;
                if (g_state.hist_count < HIST_SIZE) g_state.hist_count++;

                scroll_select_clamp(&g_state.vm.selected, g_state.vm.fleet_count);
            }
            g_state.dirty.top    = TRUE;
            g_state.dirty.roster = TRUE;
            g_state.dirty.detail = TRUE;
            break;
        }
        case TUI_EVENT_CMD_DONE:
            push_log(ev->message);
            spinner_stop(&g_state.spinner);
            g_state.dirty.all = TRUE;
            break;
        case TUI_EVENT_CMD_FAIL:
            push_log(ev->message);
            spinner_stop(&g_state.spinner);
            g_state.dirty.log_area = TRUE;
            break;
        default: break;
        }
        g_free(ev);
    }
}

// =============================================================================
// DRAWING PRIMITIVES — 기본 그리기 함수들
// =============================================================================
//
// Ratatui(Rust TUI 프레임워크)의 위젯 패턴을 C/ncurses로 포팅한 것.
// draw_panel(), draw_bar(), draw_sparkline(), draw_scrollbar(), draw_table(),
// create_popup() 등의 범용 위젯을 조합하여 각 뷰를 구성한다.
//

// draw_panel → tui_widgets.c로 이동

// draw_bar → tui_widgets.c로 이동

// draw_sparkline -> tui_widgets.c

// BRAILLE ENGINE, draw_y_axis, draw_x_axis -> tui_widgets.c
// =============================================================================
// (placeholder for removed braille engine block)
// Ratatui Canvas::BrailleSet 포팅 — ncurses/C 구현
//
//  해상도: 셀 1개 = 2(가로) × 4(세로) 픽셀
//  W셀 × H행 = (W×2) × (H×4) 실픽셀 — 스파크라인 대비 8배
//
//  렌더링 파이프라인:
//   1. 데이터 포인트 → 픽셀 좌표 변환
//   2. 인접 포인트 간 선형 보간 (Bresenham-style)
//   3. 픽셀 → 셀(BrailleGrid) 비트 축적
//   4. 그라디언트 컬러 (값 비율에 따라 셀별 색상 분기)
//   5. Y축 레이블 / 경고선 / 임계선 오버레이
// =============================================================================

// bgrid_init, bgrid_set_pixel -> tui_widgets.c

// ── 두 픽셀 사이 선 보간 (Bresenham) ─────────────────────────────────────────
// bgrid_draw_line -> tui_widgets.c

// ── 데이터 배열 → BrailleGrid ────────────────────────────────────────────────
// data[]: 환형 버퍼, hist_pos: 현재 기록 위치, n: 버퍼 총 크기
// max_val: 최대 기준값, min_val: 최소(보통 0)
// bgrid_plot_series -> tui_widgets.c

// bgrid_render, braille_color_*, draw_y_axis, draw_x_axis -> tui_widgets.c

// =============================================================================
// TOP PANEL DRAW MODES
// =============================================================================

// ── 모드 1: 브레일 고해상도 차트 (화려한 풀 버전) ────────────────────────────
// 레이아웃:
//  ┌──────── CPU UTILIZATION ────────┬──── NET I/O ──────┐
//  │  100│⣿⣿⠛⠛⣤⣀⣀⠀…               │100│⡇⠀⠀NET RX    │
//  │   75│…                         │ 50│              TX│
//  │   50│…                         │  0└──────────────  │
//  │   25│…                         │ HOST: model  DISK% │
//  │    0└──────────────────────────│ MEM% [████░░░░]    │
//  └─── [CPU%]  vCPU:N  -120s ... NOW └──────────────────┘
static void draw_top_braille(WINDOW *win, int y0, int w, int panel_h) {
    // 패널 분할: 좌측(CPU 차트) / 우측(NET + 요약)
    int left_w  = (w * 2) / 3;
    int right_w = w - left_w;

    // ── 좌측 CPU 브레일 차트 ─────────────────────────────────────────────
    int y_label_w = 5;  // "100 " 너비
    int chart_w_cells = (left_w - y_label_w - 3) / 1;  // 셀 = 문자 1개
    int chart_h_cells = panel_h - 3;  // 상하 테두리 + X축 레이블
    if (chart_w_cells < 4 || chart_h_cells < 2) goto compact_fallback;

    chart_w_cells = chart_w_cells < MAX_CHART_W ? chart_w_cells : MAX_CHART_W;
    chart_h_cells = chart_h_cells < MAX_CHART_H ? chart_h_cells : MAX_CHART_H;

    {
        // 제목
        wattron(win, COLOR_PAIR(C_CH_CPU)|A_BOLD);
        mvwprintw(win, y0, 2, "  CPU UTILIZATION ");
        wattroff(win, COLOR_PAIR(C_CH_CPU)|A_BOLD);
        // 현재값 표시
        int cpu_cp = pcv_color_for_pct(g_state.cpu_pct / 100.0);
        wattron(win, COLOR_PAIR(cpu_cp)|A_BOLD);
        mvwprintw(win, y0, 20, "%.1f%%", g_state.cpu_pct);
        wattroff(win, COLOR_PAIR(cpu_cp)|A_BOLD);

        // CPU 브레일 그리드
        BrailleGrid cpu_grid;
        bgrid_init(&cpu_grid, chart_w_cells, chart_h_cells);
        bgrid_plot_series(&cpu_grid,
                          g_state.cpu_hist, HIST_SIZE, g_state.hist_pos,
                          0.0, 100.0);

        // 경고선 위치 계산 (75% = 행 25%에서 위)
        int warn_row = (int)(0.25 * chart_h_cells);  // 75% 값이 위에서 25%
        int crit_row = (int)(0.15 * chart_h_cells);  // 85% 값
        int chart_x  = y_label_w + 1;

        bgrid_render(win, &cpu_grid,
                     y0 + 1, chart_x,
                     braille_color_gradient,
                     warn_row, crit_row);

        // Y축 레이블
        draw_y_axis(win, y0 + 1, chart_h_cells,
                    0.0, 100.0, "%", 1);

        // X축 타임스탬프
        draw_x_axis(win, y0 + 1 + chart_h_cells, chart_x,
                    chart_w_cells, g_state.hist_count, g_state.refresh_ms);

        // ── V7: MEM 오버레이 — 전체 폭, 하단 점 스타일(투명 겹침) ──────
        BrailleGrid mem_overlay;
        bgrid_init(&mem_overlay, chart_w_cells, chart_h_cells);
        bgrid_plot_series(&mem_overlay,
                          g_state.mem_hist, HIST_SIZE, g_state.hist_pos,
                          0.0, 100.0);
        // V7: 브레일 셀의 하단 2픽셀(비트 0x04, 0x20 = 3행,6행)만 렌더
        // CPU 곡선과 겹쳐도 하단 도트만 찍어 투명감 부여
        wattron(win, COLOR_PAIR(C_CH_MEM) | A_DIM);
        char utf8[4];
        const uint8_t MEM_DOT_MASK = 0x44; // 브레일 하단 2행 비트
        for (int cy = 0; cy < chart_h_cells; cy++) {
            for (int cx = 0; cx < chart_w_cells; cx++) {
                uint8_t cell = mem_overlay.grid[cy][cx];
                if (!cell) continue;
                // 하단 픽셀만 남기고 CPU 셀과 OR하지 않고 독립 렌더
                uint8_t dot_cell = cell & MEM_DOT_MASK;
                if (!dot_cell) dot_cell = cell; // 하단 픽셀 없으면 원본 유지
                braille_to_utf8(dot_cell, utf8);
                // CPU 곡선이 있는 셀은 MEM dim으로 겹침, 없는 셀은 MEM 단독
                mvwaddstr(win, y0 + 1 + cy, chart_x + cx, utf8);
            }
        }
        wattroff(win, COLOR_PAIR(C_CH_MEM) | A_DIM);

        // 범례 (V7: MEM 퍼센트 실시간 표시)
        wattron(win, COLOR_PAIR(C_CH_CPU)|A_BOLD);
        mvwprintw(win, y0+1+chart_h_cells+1, chart_x,    "● CPU");
        wattroff(win, COLOR_PAIR(C_CH_CPU)|A_BOLD);
        wattron(win, COLOR_PAIR(C_CH_MEM)|A_DIM);
        mvwprintw(win, y0+1+chart_h_cells+1, chart_x+8,  "○ MEM %3d%%",
                  (int)g_state.host.mem_percent);
        wattroff(win, COLOR_PAIR(C_CH_MEM)|A_DIM);
        wattron(win, COLOR_PAIR(C_CHART_WARN));
        mvwaddstr(win, y0+1+chart_h_cells+1, chart_x+19, "▷75%WARN");
        wattroff(win, COLOR_PAIR(C_CHART_WARN));
        wattron(win, COLOR_PAIR(C_CHART_CRIT)|A_BOLD);
        mvwaddstr(win, y0+1+chart_h_cells+1, chart_x+28, "▶85%CRIT");
        wattroff(win, COLOR_PAIR(C_CHART_CRIT)|A_BOLD);
    }

    // ── 우측: NET I/O 브레일 + 요약 ──────────────────────────────────────
    {
        int rx  = right_w - 2;
        int ry0 = y0;
        int net_chart_w = rx - 2;
        int net_chart_h = (panel_h - 3) / 2;
        if (net_chart_w < 4 || net_chart_h < 2) goto right_text;

        net_chart_w = net_chart_w < MAX_CHART_W ? net_chart_w : MAX_CHART_W;
        net_chart_h = net_chart_h < MAX_CHART_H ? net_chart_h : MAX_CHART_H;

        // NET 제목
        wattron(win, COLOR_PAIR(C_CH_NETDL)|A_BOLD);
        mvwprintw(win, ry0, left_w + 2, "  NET I/O  ");
        wattroff(win, COLOR_PAIR(C_CH_NETDL)|A_BOLD);

        // NET RX 차트
        double net_max = 0;
        for (int i = 0; i < HIST_SIZE; i++) {
            if (g_state.net_rx_hist[i] > net_max) net_max = g_state.net_rx_hist[i];
            if (g_state.net_tx_hist[i] > net_max) net_max = g_state.net_tx_hist[i];
        }
        if (net_max < 1.0) net_max = 1.0;

        BrailleGrid net_rx_grid, net_tx_grid;
        bgrid_init(&net_rx_grid, net_chart_w, net_chart_h);
        bgrid_init(&net_tx_grid, net_chart_w, net_chart_h);
        bgrid_plot_series(&net_rx_grid,
                          g_state.net_rx_hist, HIST_SIZE, g_state.hist_pos,
                          0.0, net_max);
        bgrid_plot_series(&net_tx_grid,
                          g_state.net_tx_hist, HIST_SIZE, g_state.hist_pos,
                          0.0, net_max);

        int nx = left_w + 2;
        // RX 렌더링
        bgrid_render(win, &net_rx_grid, ry0+1, nx, braille_color_netdl, -1, -1);
        // TX 오버레이
        wattron(win, COLOR_PAIR(C_CH_NETUL)|A_DIM);
        char utf8[4];
        for (int cy = 0; cy < net_chart_h; cy++) {
            for (int cx = 0; cx < net_chart_w; cx++) {
                if (net_tx_grid.grid[cy][cx]) {
                    braille_to_utf8(net_tx_grid.grid[cy][cx], utf8);
                    mvwaddstr(win, ry0+1+cy, nx+cx, utf8);
                }
            }
        }
        wattroff(win, COLOR_PAIR(C_CH_NETUL)|A_DIM);

        // 범례 + 현재 속도
        char rx_s[16], tx_s[16];
        format_bytes(g_state.rx_speed, rx_s, sizeof(rx_s));
        format_bytes(g_state.tx_speed, tx_s, sizeof(tx_s));
        int leg_y = ry0 + 1 + net_chart_h;
        wattron(win, COLOR_PAIR(C_CH_NETDL)|A_BOLD);
        mvwprintw(win, leg_y, nx, "▲DL:%s/s", rx_s);
        wattroff(win, COLOR_PAIR(C_CH_NETDL)|A_BOLD);
        wattron(win, COLOR_PAIR(C_CH_NETUL)|A_DIM);
        mvwprintw(win, leg_y, nx + net_chart_w/2, "▼UP:%s/s", tx_s);
        wattroff(win, COLOR_PAIR(C_CH_NETUL)|A_DIM);

        // 하단 요약
right_text:;
        int sum_y = ry0 + 1 + panel_h - 4;
        if (sum_y < ry0 + 1) sum_y = ry0 + 1;
        int sum_x = left_w + 2;

        wattron(win, A_BOLD);
        int host_w = right_w - 4;
        if (host_w > 80) host_w = 80;
        mvwprintw(win, sum_y, sum_x, "HOST: %.*s [%d cores]",
            host_w - 12, g_state.host.cpu_model, g_state.host.cpus);
        wattroff(win, A_BOLD);

        int mem_cp = pcv_color_for_pct(g_state.host.mem_percent/100.0);
        wattron(win, COLOR_PAIR(mem_cp)|A_BOLD);
        mvwprintw(win, sum_y+1, sum_x, "MEM: %4.1f%%", g_state.host.mem_percent);
        wattroff(win, COLOR_PAIR(mem_cp)|A_BOLD);
        draw_bar(win, sum_y+1, sum_x+10, right_w-14, g_state.host.mem_percent, C_MEM);

        int dsk_cp = pcv_color_for_pct(g_state.host.disk_percent/100.0);
        wattron(win, COLOR_PAIR(dsk_cp)|A_BOLD);
        mvwprintw(win, sum_y+2, sum_x, "DSK: %4.1f%%", g_state.host.disk_percent);
        wattroff(win, COLOR_PAIR(dsk_cp)|A_BOLD);
        draw_bar(win, sum_y+2, sum_x+10, right_w-14, g_state.host.disk_percent, C_CH_DSK);
    }
    return;

compact_fallback:
    // 브레일이 불가능할 만큼 좁으면 텍스트 fallback
    wattron(win, A_BOLD);
    mvwprintw(win, y0+1, 2, "CPU:%.1f%% MEM:%.1f%% DSK:%.1f%%",
              g_state.cpu_pct,
              g_state.host.mem_percent,
              g_state.host.disk_percent);
    wattroff(win, A_BOLD);
}

// ── 모드 2: 단일 라인 스파크라인 (기존 + NET 추가) ────────────────────────────
static void draw_top_sparkline(WINDOW *win, int y0, int w, int panel_h __attribute__((unused))) {
    int half = w / 2;
    wattron(win, A_BOLD);
    mvwprintw(win, y0+1, 2, "HOST: %s (%d Cores)",
              g_state.host.cpu_model, g_state.host.cpus);
    wattroff(win, A_BOLD);

    mvwprintw(win, y0+2, 2, "CPU: %5.1f%%", g_state.cpu_pct);
    draw_bar(win, y0+2, 14, half - 32, g_state.cpu_pct, C_CPU);
    draw_sparkline(win, y0+2, half - 18, 16,
                   g_state.cpu_hist, HIST_SIZE, g_state.hist_pos,
                   100.0, C_SPARK_CPU);

    mvwprintw(win, y0+3, 2, "RAM: %5.1f%%", g_state.host.mem_percent);
    draw_bar(win, y0+3, 14, half - 16, g_state.host.mem_percent, C_MEM);

    mvwprintw(win, y0+4, 2, "DSK: %5.1f%%", g_state.host.disk_percent);
    draw_bar(win, y0+4, 14, half - 16, g_state.host.disk_percent, C_GREEN);

    int rc = half + 2;
    char rx_s[12], tx_s[12];
    format_bytes(g_state.rx_speed, rx_s, sizeof(rx_s));
    format_bytes(g_state.tx_speed, tx_s, sizeof(tx_s));
    mvwprintw(win, y0+2, rc, "NET: %s", g_state.host.net_iface);
    mvwprintw(win, y0+3, rc, "DL:%-9s  UP:%s/s", rx_s, tx_s);

    double net_max = 1.0;
    for (int i = 0; i < HIST_SIZE; i++)
        if (g_state.net_rx_hist[i] > net_max) net_max = g_state.net_rx_hist[i];
    draw_sparkline(win, y0+3, rc+30, w - rc - 32,
                   g_state.net_rx_hist, HIST_SIZE, g_state.hist_pos,
                   net_max, C_SPARK_NET);
}

// ── 모드 3: 간략 텍스트 ────────────────────────────────────────────────────────
static void draw_top_compact(WINDOW *win, int y0, int w __attribute__((unused)), int panel_h __attribute__((unused))) {
    char rx_s[12], tx_s[12];
    format_bytes(g_state.rx_speed, rx_s, sizeof(rx_s));
    format_bytes(g_state.tx_speed, tx_s, sizeof(tx_s));
    wattron(win, A_BOLD);
    mvwprintw(win, y0+1, 2,
              "CPU:%.1f%%  MEM:%.1f%%  DSK:%.1f%%  NET DL:%s/s TX:%s/s",
              g_state.cpu_pct, g_state.host.mem_percent,
              g_state.host.disk_percent, rx_s, tx_s);
    wattroff(win, A_BOLD);
}

// ── 모드 4: btop 스타일 코어별 CPU + 메모리 세그먼트 ─────────────────────────
static void draw_top_btop(WINDOW *win, int y0, int w, int panel_h) {
    int nc = g_state.core_count > 0 ? g_state.core_count : g_state.host.cpus;
    if (nc <= 0) nc = 1;

    // 헤더: CPU 모델 + Temp + Load
    wattron(win, A_BOLD);
    mvwprintw(win, y0, 2, " CPU ");
    wattroff(win, A_BOLD);
    int cpu_cp = pcv_color_for_pct(g_state.cpu_pct / 100.0);
    wattron(win, COLOR_PAIR(cpu_cp)|A_BOLD);
    mvwprintw(win, y0, 7, "%.1f%%", g_state.cpu_pct);
    wattroff(win, COLOR_PAIR(cpu_cp)|A_BOLD);

    // Temp
    if (g_state.host.cpu_temp_c > 0) {
        int t_cp = g_state.host.cpu_temp_c > 80 ? C_RED :
                   g_state.host.cpu_temp_c > 60 ? C_YELLOW : C_GREEN;
        wattron(win, COLOR_PAIR(t_cp));
        mvwprintw(win, y0, 15, "%.0f°C", g_state.host.cpu_temp_c);
        wattroff(win, COLOR_PAIR(t_cp));
    }

    // Load
    wattron(win, COLOR_PAIR(C_LOAD));
    mvwprintw(win, y0, 22, "Load: %.2f %.2f %.2f",
              g_state.host.load_1, g_state.host.load_5, g_state.host.load_15);
    wattroff(win, COLOR_PAIR(C_LOAD));

    // Uptime
    if (g_state.host.uptime_secs > 0) {
        int up_d = (int)(g_state.host.uptime_secs / 86400);
        int up_h = (int)((long)g_state.host.uptime_secs % 86400) / 3600;
        int up_m = (int)((long)g_state.host.uptime_secs % 3600) / 60;
        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, y0, w - 22, "Up %dd %dh %dm", up_d, up_h, up_m);
        wattroff(win, COLOR_PAIR(C_DIM));
    }

    // 코어별 CPU 바 (2열 그리드)
    int cols = 2;
    int col_w = (w - 4) / cols;
    int bar_w = col_w - 14;  // "cpuNN 100% " + bar
    if (bar_w < 4) bar_w = 4;
    int avail_rows = panel_h - 4;  // 헤더 1행 + MEM/SWP 2행 + 여백
    int rows_per_col = (nc + cols - 1) / cols;
    if (rows_per_col > avail_rows) rows_per_col = avail_rows;

    for (int i = 0; i < nc && i < rows_per_col * cols; i++) {
        int col = i / rows_per_col;
        int row = i % rows_per_col;
        int cx = 2 + col * col_w;
        int cy = y0 + 1 + row;
        double pct = (i < g_state.core_count) ? g_state.core_pct[i] : 0.0;
        int ccp = pcv_color_for_pct(pct / 100.0);

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, cy, cx, "cpu%-3d", i);
        wattroff(win, COLOR_PAIR(C_DIM));

        draw_bar(win, cy, cx + 6, bar_w, pct, ccp);

        wattron(win, COLOR_PAIR(ccp)|A_BOLD);
        mvwprintw(win, cy, cx + 6 + bar_w + 1, "%5.1f%%", pct);
        wattroff(win, COLOR_PAIR(ccp)|A_BOLD);
    }

    // MEM 세그먼트 바
    int mem_y = y0 + 1 + (rows_per_col < avail_rows ? rows_per_col : avail_rows);
    if (mem_y < y0 + panel_h - 2) {
        int mem_cp = pcv_color_for_pct(g_state.host.mem_percent / 100.0);
        wattron(win, COLOR_PAIR(mem_cp)|A_BOLD);
        mvwprintw(win, mem_y, 2, "MEM");
        wattroff(win, COLOR_PAIR(mem_cp)|A_BOLD);
        draw_bar(win, mem_y, 6, w/2 - 10, g_state.host.mem_percent, C_MEM);
        mvwprintw(win, mem_y, w/2 - 3, "%.1fG/%.1fG %4.1f%%",
                  g_state.host.mem_used_gb, g_state.host.mem_total_gb,
                  g_state.host.mem_percent);
        // Buffers/Cached 라벨
        wattron(win, COLOR_PAIR(C_CACHED));
        mvwprintw(win, mem_y, w/2 + 18, "Buf:%.0fM Cch:%.0fM",
                  g_state.host.mem_buffers_mb, g_state.host.mem_cached_mb);
        wattroff(win, COLOR_PAIR(C_CACHED));
    }

    // SWP 바
    if (mem_y + 1 < y0 + panel_h - 1 && g_state.host.swap_total_gb > 0.001) {
        double swp_pct = (g_state.host.swap_used_gb / g_state.host.swap_total_gb) * 100.0;
        int swp_cp = pcv_color_for_pct(swp_pct / 100.0);
        wattron(win, COLOR_PAIR(swp_cp));
        mvwprintw(win, mem_y + 1, 2, "SWP");
        wattroff(win, COLOR_PAIR(swp_cp));
        draw_bar(win, mem_y + 1, 6, w/2 - 10, swp_pct, C_SWAP);
        mvwprintw(win, mem_y + 1, w/2 - 3, "%.1fG/%.1fG %4.1f%%",
                  g_state.host.swap_used_gb, g_state.host.swap_total_gb, swp_pct);
    }
}

// ── VM 인라인 브레일 미니 차트 (로스터 행에 삽입) ────────────────────────────
// vm.metrics API에서 수집한 live_hist 기반 2행 미니 브레일
// 공간이 허용하는 경우 VM 로스터 선택 행 아래에 서브 차트 표시
__attribute__((unused)) static void draw_vm_mini_braille(WINDOW *win, int row_y, int x,
                                 const double *cpu_hist, const double *mem_hist,
                                 int hist_pos, int hist_n, int width) {
    if (width < 8) return;
    int cells = width / 2;
    if (cells > MAX_CHART_W) cells = MAX_CHART_W;

    BrailleGrid cg, mg;
    bgrid_init(&cg, cells, 2);
    bgrid_init(&mg, cells, 2);
    bgrid_plot_series(&cg, cpu_hist, hist_n, hist_pos, 0.0, 100.0);
    bgrid_plot_series(&mg, mem_hist, hist_n, hist_pos, 0.0, 100.0);

    char utf8[4];
    // CPU: 위 행, MEM: 아래 행 (같은 컬럼에 분리 렌더)
    for (int cy = 0; cy < 2; cy++) {
        int cp = (cy == 0) ? C_CH_CPU : C_CH_MEM;
        const BrailleGrid *g = (cy == 0) ? &cg : &mg;
        wattron(win, COLOR_PAIR(cp));
        for (int cx = 0; cx < cells; cx++) {
            braille_to_utf8(g->grid[cy][cx], utf8);
            mvwaddstr(win, row_y + cy, x + cx, utf8);
        }
        wattroff(win, COLOR_PAIR(cp));
    }
    // 레이블
    wattron(win, COLOR_PAIR(C_CH_CPU)|A_BOLD); mvwprintw(win, row_y,   x+cells+1, "C"); wattroff(win, COLOR_PAIR(C_CH_CPU)|A_BOLD);
    wattron(win, COLOR_PAIR(C_CH_MEM)|A_BOLD); mvwprintw(win, row_y+1, x+cells+1, "M"); wattroff(win, COLOR_PAIR(C_CH_MEM)|A_BOLD);
}

// =============================================================================
// UNIFIED TOP PANEL DISPATCH
// =============================================================================
static void draw_top_panel(WINDOW *win, int y, int w, int h) {
    draw_panel(win, y, 0, h, w, "host metrics", C_CPU);

    // t 모드 표시
    static const char *mode_labels[] = {"[BRAILLE]","[SPARK]","[COMPACT]","[BTOP]"};
    wattron(win, COLOR_PAIR(C_DIM));
    mvwprintw(win, y, w-14, " t:%s ", mode_labels[g_state.top_mode]);
    wattroff(win, COLOR_PAIR(C_DIM));

    switch (g_state.top_mode) {
    case TOP_MODE_BRAILLE:  draw_top_braille(win, y, w, h);   break;
    case TOP_MODE_SPARK:    draw_top_sparkline(win, y, w, h); break;
    case TOP_MODE_COMPACT:  draw_top_compact(win, y, w, h);   break;
    case TOP_MODE_BTOP:     draw_top_btop(win, y, w, h);      break;
    default:                draw_top_sparkline(win, y, w, h); break;
    }

    // 스피너 (비동기 명령 실행 중)
    spinner_tick(&g_state.spinner);
    spinner_draw(win, y + h - 2, w - 24, &g_state.spinner);
}

// ── Ratatui Scrollbar + ScrollState 차용 ──────────────────────────────────
// draw_scrollbar -> tui_widgets.c

// ── Ratatui Tabs 차용 ────────────────────────────────────────────────────────
// T1: 탭 배지 데이터 헬퍼
static void tab_badge(char *out, size_t sz, int idx) {
    // 각 탭별 카운트 + 활성(●)/비활성(○) 상태 인디케이터
    switch (idx) {
    case 0: { // VM
        int run = 0, total = g_state.vm.fleet_count;
        for (int i = 0; i < total; i++)
            if (g_state.vm.fleet[i].is_running) run++;
        // 실행중 VM 있으면 녹색 인디케이터
        snprintf(out, sz, "F1 VM %d/%d", run, total);
        break;
    }
    case 1: { // NET (F2)
        int cnt = g_state.net.bridge_count;
        snprintf(out, sz, "F2 NET %d", cnt);
        break;
    }
    case 2: { // STG (F3)
        int cnt = g_state.stg.zvol_count;
        snprintf(out, sz, "F3 STG %d", cnt);
        break;
    }
    case 3: { // CTR (F4)
        int cnt = g_state.ctr.ctr_count;
        snprintf(out, sz, "F4 CTR %d", cnt);
        break;
    }
    case 4: { // HOST (F5)
        snprintf(out, sz, "F5 HOST");
        break;
    }
    case 5: { // CLUSTER (F6)
        int local_vms = g_state.vm.fleet_count;
        snprintf(out, sz, "F6 HA %d+", local_vms);
        break;
    }
    case 6: { // OVN (F7)
        snprintf(out, sz, "F7 OVN %d", g_state.net.ovn.sw_count + g_state.net.ovn.rt_count);
        break;
    }
    default: snprintf(out, sz, "F%d", idx+1); break;
    }
}

static void draw_tab_bar(WINDOW *win, int y, TuiView active, int w) {
    // 탭 바 배경
    wattron(win, COLOR_PAIR(C_DIM));
    mvwhline(win, y, 0, ' ', w);
    wattroff(win, COLOR_PAIR(C_DIM));

    int x = 1;
    for (int i = 0; i < TUI_VIEW_COUNT; i++) {
        char badge[32];
        tab_badge(badge, sizeof(badge), i);

        // T1: 활성 탭 — 밝은 강조, 비활성 탭 — 카운트 뱃지 포함
        if (i == (int)active) {
            wattron(win, COLOR_PAIR(C_TAB_ACT) | A_BOLD | A_REVERSE);
            mvwprintw(win, y, x, " [%s] ", badge);
            wattroff(win, COLOR_PAIR(C_TAB_ACT) | A_BOLD | A_REVERSE);
        } else {
            // 비활성: VM 탭은 running >0 이면 녹색 힌트
            int badge_cp = C_DIM;
            if (i == 0 && g_state.vm.fleet_count > 0) {
                int run = 0;
                for (int j = 0; j < g_state.vm.fleet_count; j++)
                    if (g_state.vm.fleet[j].is_running) run++;
                badge_cp = run > 0 ? C_GREEN : C_RED;
            }
            wattron(win, COLOR_PAIR(badge_cp));
            mvwprintw(win, y, x, " [%s] ", badge);
            wattroff(win, COLOR_PAIR(badge_cp));
        }
        x += (int)strlen(badge) + 4;
    }
    // 갱신 주기 표시
    wattron(win, COLOR_PAIR(C_DIM));
    // U2: 6단계 갱신 주기 표시
    char rstr[8];
    if      (g_state.refresh_ms == 500)   snprintf(rstr, sizeof(rstr), "500ms");
    else if (g_state.refresh_ms == 1000)  snprintf(rstr, sizeof(rstr), "1s");
    else if (g_state.refresh_ms == 3000)  snprintf(rstr, sizeof(rstr), "3s");
    else if (g_state.refresh_ms == 5000)  snprintf(rstr, sizeof(rstr), "5s");
    else if (g_state.refresh_ms == 10000) snprintf(rstr, sizeof(rstr), "10s");
    else                                   snprintf(rstr, sizeof(rstr), "30s");
    mvwprintw(win, y, w - 12, "[R]%-5s ", rstr);
    wattroff(win, COLOR_PAIR(C_DIM));
}

// ── Ratatui Table 차용 (범용 헤더+행 테이블) ──────────────────────────────
// draw_table, create_popup, destroy_popup, prompt_input, confirm_dialog -> tui_widgets.c

// =============================================================================
// LOG PANEL
// =============================================================================
// =============================================================================
// U5: 키 도움말 오버레이 ('?' 토글)
// =============================================================================
static gboolean g_show_help = FALSE;

static void draw_help_overlay(void) {
    int scr_rows, scr_cols;
    getmaxyx(stdscr, scr_rows, scr_cols);
    int pw = 66, ph = 50;
    if (pw > scr_cols - 4) pw = scr_cols - 4;
    if (ph > scr_rows - 4) ph = scr_rows - 4;

    WINDOW *pop = create_popup(ph, pw, "KEYBOARD SHORTCUTS");

    // 섹션별 단축키 테이블
    static const struct { const char *key; const char *desc; int section; } KEYS[] = {
        // section 0 = 전역
        {"q / Ctrl+C", "Quit",                        0},
        {"F1~F7",      "Tab: VM/NET/STG/CTR/HOST/HA/OVN", 0},
        {"t",          "Cycle top panel mode",        0},
        {"R",          "Cycle refresh (500ms→30s)",   0},
        {"PgUp/PgDn",  "Scroll event log",            0},
        {"?",          "Toggle this help",            0},
        // section 1 = VM 뷰 (F1)
        {"↑↓",         "Select VM",                   1},
        {"Tab",        "Sort (NAME/CPU/MEM/STATE)",   1},
        {"/",          "Filter VMs",                  1},
        {"Esc",        "Clear filter",                1},
        {"c",          "Create VM",                   1},
        {"s / x",      "Start / Stop VM",             1},
        {"p",          "Pause / Resume VM",           1},
        {"d",          "Delete VM",                   1},
        {"r",          "Rename shut off VM",          1},
        {"k",          "Clone VM",                    1},
        {"O",          "Export OVA",                  1},
        {"U",          "Import OVA",                  1},
        {"m",          "Show live metrics",           1},
        {"v / H",      "Set vCPU / RAM",              1},
        {"L",          "Set CPU limit",               1},
        {"n",          "Create snapshot",             1},
        {"l",          "List/manage snapshots",       1},
        {"e",          "Eject ISO",                   1},
        {"V",          "Open VNC",                    1},
        {"A",          "Toggle autostart",            1},
        {"I",          "Disk I/O throttle",           1},
        {"B",          "Memory balloon stats",        1},
        {"C",          "CPU detailed stats",          1},
        {"G",          "Guest agent (ping/exec/off)", 1},
        {"Z",          "Disk live resize",            1},
        {"W",          "Network bandwidth QoS",        1},
        {"Y",          "USB passthrough",              1},
        // section 2 = NET 뷰 (F2)
        {"a / d",      "Add / Remove bridge",         2},
        {"M",          "Change bridge mode",          2},
        {"P",          "Set physical uplink",         2},
        {"D",          "Toggle DHCP",                 2},
        {"i",          "Show DHCP leases",            2},
        {"Enter",      "Bridge detail popup",         2},
        {"F",          "Firewall rules popup",        2},
        {"G",          "Security group list",         2},
        {"r",          "Refresh",                     2},
        // section 3 = STG/CTR (F3/F4)
        {"c / d",      "[STG] Create / Delete zvol",  3},
        {"H",          "[STG] Pool health check",     3},
        {"C",          "[STG] Capacity forecast",     3},
        {"s/x/D/e",    "[CTR] Start/Stop/Destroy/Exec",3},
        // section 4 = OVN 뷰 (F7)
        {"↑↓",         "Select OVN resource",         4},
        {"S",          "Create logical switch",       4},
        {"R",          "Create logical router",       4},
        {"X",          "Delete selected resource",    4},
        {"T",          "Create multi-tenant",         4},
        {"N",          "Add NAT rule",                4},
        {"Enter",      "Resource detail popup",        4},
        {"A",          "ACL manager (switch)",         4},
        {"I",          "NAT/DHCP info popup",          4},
        {"r",          "Refresh",                     4},
        // section 5 = HOST 뷰 (F5)
        {"f",          "Fleet overview",              5},
        {"g",          "GPU metrics",                 5},
        {"S",          "Security Guard status",       5},
        {"T",          "Toggle Security Guard",       5},
        {"E",          "Security events",             5},
        {"P",          "Pending HIPS actions",        5},
        {"A",          "Approve HIPS action",         5},
        {"D",          "Dismiss HIPS action",         5},
        {"B",          "Refresh HIDS baseline path",  5},
        {"h",          "Config history",              5},
        // section 6 = HA/Cluster 뷰 (F6)
        {"M",          "Live migrate VM",             6},
        {"F",          "Failover test",               6},
        {"S",          "Trigger replication",         6},
        {"c",          "Create VM (scheduler)",       6},
        {"U",          "RBAC user list",              6},
        {"A",          "Affinity rules list",         6},
        {"a",          "Audit search (last 20)",      6},
        {"W",          "Webhook DLQ list",            6},
        {"Q",          "Cluster aggregate metrics",   6},
    };
    int nkeys = (int)(sizeof(KEYS)/sizeof(KEYS[0]));

    static const char *sec_labels[] = {
        "GLOBAL", "F1 VM", "F2 NET", "F3 STG / F4 CTR", "F7 OVN", "F5 HOST", "F6 HA"
    };
    static const int sec_colors[] = { C_YELLOW, C_GREEN, C_CYAN, C_FLEET, C_MEM, C_RED, C_TITLE };

    int ry = 1;
    int cur_sec = -1;
    for (int i = 0; i < nkeys && ry < ph-2; i++) {
        if (KEYS[i].section != cur_sec) {
            cur_sec = KEYS[i].section;
            ry++;
            wattron(pop, COLOR_PAIR(sec_colors[cur_sec])|A_BOLD|A_UNDERLINE);
            mvwprintw(pop, ry++, 2, "── %s ", sec_labels[cur_sec]);
            wattroff(pop, COLOR_PAIR(sec_colors[cur_sec])|A_BOLD|A_UNDERLINE);
        }
        wattron(pop, COLOR_PAIR(C_CYAN)|A_BOLD);
        mvwprintw(pop, ry, 4, "%-18s", KEYS[i].key);
        wattroff(pop, COLOR_PAIR(C_CYAN)|A_BOLD);
        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, ry, 23, "%s", KEYS[i].desc);
        wattroff(pop, COLOR_PAIR(C_DIM));
        ry++;
    }

    wattron(pop, COLOR_PAIR(C_DIM));
    mvwhline(pop, ph-3, 1, ACS_HLINE, pw-2);
    wattroff(pop, COLOR_PAIR(C_DIM));
    wattron(pop, COLOR_PAIR(C_YELLOW)|A_BOLD);
    mvwprintw(pop, ph-2, 2, "Press [?] [Esc] [q] [Enter] [Space] to close");
    wattroff(pop, COLOR_PAIR(C_YELLOW)|A_BOLD);

    wrefresh(pop);

    // 키 입력 대기 — B7-C1 (Phase 1 fix): Enter/Space/Q 추가하여 사용자 stuck 방지
    timeout(-1);
    int ch;
    while ((ch = wgetch(pop)) != '?' && ch != 27 && ch != 'q' && ch != 'Q'
           && ch != '\n' && ch != KEY_ENTER && ch != ' ') {}
    timeout(50);
    g_show_help = FALSE;
    delwin(pop);
    touchwin(stdscr);
    g_state.dirty.all = TRUE;
}

static void draw_log_panel(WINDOW *win, int y, int w, int h) {
    // U4: 패널 타이틀에 멈춤 표시
    char log_title[64];
    if (g_state.log_paused)
        snprintf(log_title, sizeof(log_title),
                 "event stream [PAUSED +%d] PgDn=resume", g_state.log_offset);
    else
        snprintf(log_title, sizeof(log_title), "event stream");
    draw_panel(win, y, 0, h, w, log_title, g_state.log_paused ? C_YELLOW : C_LOG);

    int vis = h - 2;
    // U4: 오프셋 기반 뷰포트 (0=최신)
    int count = g_state.log_count;
    for (int i = 0; i < vis; i++) {
        // 최신 기준으로 역산: log_head-1 이 가장 최신
        int back = vis - 1 - i + g_state.log_offset;
        int idx  = ((g_state.log_head - 1 - back) % LOG_BUF_SIZE + LOG_BUF_SIZE) % LOG_BUF_SIZE;
        if (back >= count) {
            mvwprintw(win, y+1+i, 2, "%*s", w-4, "");
            continue;
        }
        LogEntry *le = &g_state.log_buf[idx];
        if (!le->msg[0]) continue;

        // U4: 레벨별 색상 + 아이콘
        int cp; const char *icon;
        switch (le->level) {
        case LOG_ERROR:   cp = C_RED;    icon = "✖ "; break;
        case LOG_WARN:    cp = C_YELLOW; icon = "⚠ "; break;
        case LOG_SUCCESS: cp = C_GREEN;  icon = "✔ "; break;
        case LOG_SYS:     cp = C_CYAN;   icon = "◈ "; break;
        default:          cp = C_DIM;    icon = "  "; break;
        }
        // 최신 항목은 밝게
        int attr = (back == 0 && !g_state.log_paused) ? A_BOLD : 0;
        wattron(win, COLOR_PAIR(cp) | attr);
        // 아이콘 + 메시지
        mvwprintw(win, y+1+i, 1, "%s%-*.*s", icon, w-5, w-5, le->msg);
        wattroff(win, COLOR_PAIR(cp) | attr);
    }
}

// =============================================================================
// VIEW: VM — draw + key handler
// =============================================================================

// ── 스냅샷 오버레이 (Clear 팝업 패턴) ────────────────────────────────────────
static void show_snapshot_overlay(void) {
    SnapOverlay *so = &g_state.vm.snap_overlay;
    const char *vm  = g_state.vm.fleet[g_state.vm.selected].name;

    // 스냅샷 목록 조회 (동기 — 짧은 RPC)
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", vm);
    GError *err = NULL;
    gchar  *resp = tui_send_request("vm.snapshot.list", params, &err);

    so->count    = 0;
    so->selected = 0;
    strncpy(so->vm_name, vm, sizeof(so->vm_name)-1);

    if (err) {
        push_log("SNAP LIST ERR"); g_error_free(err);
        if (resp) g_free(resp);
        return;
    }
    if (resp) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, resp, -1, NULL)) {
            JsonObject *root_obj = json_node_get_object(json_parser_get_root(p));
            if (json_object_has_member(root_obj, "result")) {
                JsonNode *rn = json_object_get_member(root_obj, "result");
                // result는 배열 또는 문자열 (zfs list 출력)
                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                    JsonArray *arr = json_node_get_array(rn);
                    int n = (int)json_array_get_length(arr);
                    so->count = n < MAX_SNAP ? n : MAX_SNAP;
                    for (int i = 0; i < so->count; i++) {
                        JsonObject *sobj = json_array_get_object_element(arr, i);
                        snprintf(so->snaps[i], 127, "%-32s  %s",
                                 safe_str(sobj,"name","?"),
                                 safe_str(sobj,"creation","?"));
                    }
                } else if (JSON_NODE_TYPE(rn) == JSON_NODE_VALUE) {
                    // 텍스트 출력인 경우 줄 분리
                    const gchar *text = json_node_get_string(rn);
                    if (text) {
                        gchar **lines = g_strsplit(text, "\n", -1);
                        for (gchar **l = lines; *l && so->count < MAX_SNAP; l++) {
                            if (!**l) continue;
                            strncpy(so->snaps[so->count++], *l, 127);
                        }
                        g_strfreev(lines);
                    }
                }
            }
        }
        g_object_unref(p); g_free(resp);
    }

    so->scroll.total    = so->count;
    so->scroll.viewport = 8;
    so->active = TRUE;
}

// V2: 스냅샷 오버레이 내부 렌더링 헬퍼
static void snap_overlay_redraw(WINDOW *pop, SnapOverlay *so, int pw, int ph) {
    int vis = ph - 6;
    werase(pop); box(pop, 0, 0);

    // 타이틀
    wattron(pop, A_BOLD | COLOR_PAIR(C_TITLE));
    mvwprintw(pop, 0, 2, " SNAPSHOT MANAGER ");
    wattroff(pop, A_BOLD | COLOR_PAIR(C_TITLE));

    // VM 이름 + 카운트
    wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
    mvwprintw(pop, 1, 2, "VM: %-30.30s", so->vm_name);
    wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
    wattron(pop, COLOR_PAIR(C_DIM));
    mvwprintw(pop, 1, pw-14, "%2d snapshot(s)", so->count);
    wattroff(pop, COLOR_PAIR(C_DIM));

    // 컬럼 헤더
    wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvwprintw(pop, 2, 2, "%-4s  %-36.36s  %-16s", "#", "SNAPSHOT NAME", "CREATION");
    wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    wattron(pop, COLOR_PAIR(C_DIM));
    mvwhline(pop, 3, 1, ACS_HLINE, pw-2);
    wattroff(pop, COLOR_PAIR(C_DIM));

    // 스냅샷 목록
    so->scroll.viewport = vis;
    for (int i = 0; i < vis; i++) {
        int ri = i + so->scroll.position;
        if (ri >= so->count) {
            mvwprintw(pop, 4+i, 2, "%*s", pw-4, "");
            continue;
        }
        bool is_sel = (ri == so->selected);
        if (is_sel) wattron(pop, COLOR_PAIR(C_HIGHLIGHT) | A_REVERSE | A_BOLD);
        else        wattron(pop, COLOR_PAIR(C_DIM));

        // 스냅 이름/날짜 분리 파싱
        char snap_name[48] = {0}, snap_time[20] = {0};
        sscanf(so->snaps[ri], "%47s %19s", snap_name, snap_time);
        // @이후만 추출
        const char *at_ptr = strchr(snap_name, '@');
        const char *disp = at_ptr ? at_ptr + 1 : snap_name;
        mvwprintw(pop, 4+i, 2, "%-4d  %-36.36s  %-16.16s", ri+1, disp, snap_time);

        if (is_sel) wattroff(pop, COLOR_PAIR(C_HIGHLIGHT) | A_REVERSE | A_BOLD);
        else        wattroff(pop, COLOR_PAIR(C_DIM));
    }
    if (so->count == 0) {
        wattron(pop, COLOR_PAIR(C_DIM) | A_ITALIC);
        mvwprintw(pop, 5, 2, "(No snapshots found — press [n] to create one)");
        wattroff(pop, COLOR_PAIR(C_DIM) | A_ITALIC);
    }

    // 구분선 + 액션 바
    wattron(pop, COLOR_PAIR(C_DIM));
    mvwhline(pop, ph-3, 1, ACS_HLINE, pw-2);
    wattroff(pop, COLOR_PAIR(C_DIM));
    wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
    mvwprintw(pop, ph-2, 2, "[n]New  [r]Rollback  [D]Delete  [Esc]Close");
    wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);

    // 페이지 표시
    if (so->count > vis) {
        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, ph-2, pw-10, "%d/%d", so->selected+1, so->count);
        wattroff(pop, COLOR_PAIR(C_DIM));
    }

    draw_scrollbar(pop, 4, vis, pw-2, &so->scroll);
    wrefresh(pop);
}

static void draw_snapshot_overlay(WINDOW *win __attribute__((unused))) {
    SnapOverlay *so = &g_state.vm.snap_overlay;
    // V2: 와이드 팝업 (터미널 너비 80% 활용)
    int scr_rows, scr_cols;
    getmaxyx(stdscr, scr_rows, scr_cols);
    int pw = scr_cols > 80 ? (scr_cols * 4 / 5) : 72;
    int ph = scr_rows > 24 ? 18 : 14;
    if (pw > 78) pw = 78;
    WINDOW *pop = create_popup(ph, pw, "SNAPSHOT MANAGER");

    snap_overlay_redraw(pop, so, pw, ph);

    // V2: 키 처리 루프
    timeout(-1);
    int ch;
    while ((ch = wgetch(pop)) != 27 /* ESC */) {
        if (ch == KEY_UP || ch == 'k') {
            if (so->selected > 0) so->selected--;
        }
        else if (ch == KEY_DOWN || ch == 'j') {
            if (so->selected < so->count-1) so->selected++;
        }
        else if (ch == KEY_PPAGE) {   // PgUp
            so->selected -= (ph-6);
            if (so->selected < 0) so->selected = 0;
        }
        else if (ch == KEY_NPAGE) {   // PgDn
            so->selected += (ph-6);
            if (so->selected >= so->count) so->selected = so->count > 0 ? so->count-1 : 0;
        }
        else if (ch == 'n') {
            // 신규 스냅샷 생성
            char snap_name[64] = {0};
            endwin();
            printf("\n[PureCVisor] New snapshot name for [%s]: ", so->vm_name);
            fflush(stdout);
            if (fgets(snap_name, sizeof(snap_name), stdin)) {
                snap_name[strcspn(snap_name, "\n")] = '\0';
                if (snap_name[0]) {
                    JsonObject *p2 = json_object_new();
                    json_object_set_string_member(p2, "vm_id",     so->vm_name);
                    json_object_set_string_member(p2, "snap_name", snap_name);
                    char log_ok[256];
                    snprintf(log_ok, sizeof(log_ok), "SNAP CREATE [%s]@%s", so->vm_name, snap_name);
                    send_async_rpc("vm.snapshot.create", p2, log_ok, "SNAP NEW");
                }
            }
            refresh(); doupdate();
            break;
        }
        else if (ch == 'r' && so->count > 0) {
            char snap_clean[64] = {0};
            sscanf(so->snaps[so->selected], "%63s", snap_clean);
            const char *at = strchr(snap_clean, '@');
            if (at) at++; else at = snap_clean;
            char warn_msg[128];
            snprintf(warn_msg, sizeof(warn_msg),
                     "ROLLBACK '%s' — VM will power-cycle!", so->vm_name);
            if (confirm_dialog(warn_msg, at)) {
                JsonObject *p2 = json_object_new();
                json_object_set_string_member(p2, "vm_id",     so->vm_name);
                json_object_set_string_member(p2, "snap_name", at);
                char log_ok[256];
                snprintf(log_ok, sizeof(log_ok),
                         "ROLLBACK [%s]@%s initiated", so->vm_name, at);
                send_async_rpc("vm.snapshot.rollback", p2, log_ok, "ROLLBACK");
            }
            break;
        }
        else if (ch == 'D' && so->count > 0) {
            char snap_clean[64] = {0};
            sscanf(so->snaps[so->selected], "%63s", snap_clean);
            const char *at = strchr(snap_clean, '@');
            if (at) at++; else at = snap_clean;
            if (confirm_dialog("DELETE SNAPSHOT — irreversible!", at)) {
                JsonObject *p2 = json_object_new();
                json_object_set_string_member(p2, "vm_id",     so->vm_name);
                json_object_set_string_member(p2, "snap_name", at);
                char log_ok[256];
                snprintf(log_ok, sizeof(log_ok), "SNAP DELETE [%s]@%s", so->vm_name, at);
                send_async_rpc("vm.snapshot.delete", p2, log_ok, "SNAP DEL");
                so->count--;
                if (so->selected >= so->count) so->selected = so->count > 0 ? so->count-1 : 0;
            }
        }
        // 스크롤 동기화
        if (so->selected < so->scroll.position)
            so->scroll.position = so->selected;
        if (so->selected >= so->scroll.position + so->scroll.viewport)
            so->scroll.position = so->selected - so->scroll.viewport + 1;

        // V2: 헬퍼 함수로 리드로우
        snap_overlay_redraw(pop, so, pw, ph);
        wrefresh(pop);
    }
    timeout(50);
    so->active = FALSE;
    destroy_popup(pop);
    g_state.dirty.all = TRUE;
}

// ── VM 뷰 그리기 (Elm View) ──────────────────────────────────────────────────
static void draw_view_vm(WINDOW *win, int y0, int mid_h, int left_w, int right_w) {
    // ── 로스터 (좌측) ────────────────────────────────────────────────────────
    // U3: 정렬 모드 타이틀 반영
    static const char *sort_labels[] = { "NAME", "CPU↓", "MEM↓", "STATE", "VCPU↓", "RAM↓" };
    char roster_title[128];
    if (g_state.vm.filter_active && g_state.vm.multi_count > 0)
        snprintf(roster_title, sizeof(roster_title),
                 "vm roster [/%s] sort:%s [*%d]", g_state.vm.filter,
                 sort_labels[g_state.vm.sort_mode & 3], g_state.vm.multi_count);
    else if (g_state.vm.filter_active)
        snprintf(roster_title, sizeof(roster_title),
                 "vm roster [/%s] sort:%s", g_state.vm.filter,
                 sort_labels[g_state.vm.sort_mode & 3]);
    else if (g_state.vm.multi_count > 0)
        snprintf(roster_title, sizeof(roster_title),
                 "vm roster sort:%s [*%d sel] [X]StopAll [Z]Clear",
                 sort_labels[g_state.vm.sort_mode & 3], g_state.vm.multi_count);
    else
        snprintf(roster_title, sizeof(roster_title),
                 "vm roster sort:%s", sort_labels[g_state.vm.sort_mode & 3]);
    draw_panel(win, y0, 0, mid_h, left_w, roster_title, C_FLEET);

    if (g_state.error[0]) {
        wattron(win, COLOR_PAIR(C_RED)|A_BOLD);
        mvwprintw(win, y0+2, 2, "ERR: %.40s", g_state.error);
        wattroff(win, COLOR_PAIR(C_RED)|A_BOLD);
    }

    int row_y = y0 + 1;
    int roster_vis = mid_h - 2;

    // U3: 정렬 인덱스 배열
    static int sorted_idx[MAX_VMS];
    int fc = g_state.vm.fleet_count;
    for (int i = 0; i < fc; i++) sorted_idx[i] = i;
    for (int a = 0; a < fc-1; a++) {
        for (int b = a+1; b < fc; b++) {
            VMMetrics *va = &g_state.vm.fleet[sorted_idx[a]];
            VMMetrics *vb = &g_state.vm.fleet[sorted_idx[b]];
            int swap = 0;
            switch (g_state.vm.sort_mode % 6) {
            case 0: swap = (g_strcmp0(va->name,  vb->name)  > 0); break;
            case 1: swap = (va->live_cpu_pct < vb->live_cpu_pct); break;
            case 2: swap = (va->mem_percent  < vb->mem_percent);  break;
            case 3: swap = (g_strcmp0(va->state, vb->state) > 0); break;
            case 4: swap = (va->vcpu < vb->vcpu); break;
            case 5: swap = (va->mem_max_mb < vb->mem_max_mb); break;
            }
            if (swap) { int tmp = sorted_idx[a]; sorted_idx[a] = sorted_idx[b]; sorted_idx[b] = tmp; }
        }
    }

    g_state.vm.scroll.total    = fc;
    g_state.vm.scroll.viewport = roster_vis;

    for (int i = 0; i < roster_vis; i++) {
        int pos = i + g_state.vm.scroll.position;
        if (pos >= fc) break;
        int fi = sorted_idx[pos];
        VMMetrics *vm = &g_state.vm.fleet[fi];

        // U1: 필터 — 미매칭 dim 처리 (숨김 대신), 매칭 이름 하이라이트
        bool filter_match = true;
        if (g_state.vm.filter_active && g_state.vm.filter[0]) {
            filter_match = (strstr(vm->name, g_state.vm.filter) != NULL);
            if (!filter_match) {
                // dim으로 표시 (숨기지 않음)
                wattron(win, COLOR_PAIR(C_DIM)|A_DIM);
                mvwprintw(win, row_y, 3, "   %-14.14s", vm->name);
                wattroff(win, COLOR_PAIR(C_DIM)|A_DIM);
                row_y++; continue;
            }
        }

        bool is_sel = (fi == g_state.vm.selected);

        // A3: 다중 선택 마커
        bool is_multi = g_state.vm.multi_sel[fi];
        if (is_multi && !is_sel) {
            wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
            mvwaddch(win, row_y, 1, '*');
            wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
        }

        // V3: 상태 5단계 색상 + 태그
        int   st_cp;
        const char *st_tag;
        if      (g_strcmp0(vm->state, "RUNNING")  == 0) { st_cp = C_GREEN;  st_tag = "[R]"; }
        else if (g_strcmp0(vm->state, "PAUSED")   == 0) { st_cp = C_YELLOW; st_tag = "[P]"; }
        else if (g_strcmp0(vm->state, "CRASHED")  == 0) { st_cp = C_RED;    st_tag = "[!]"; }
        else if (g_strcmp0(vm->state, "PMSUSPENDED")==0){ st_cp = C_YELLOW; st_tag = "[Z]"; }
        else                                             { st_cp = C_DIM;   st_tag = "[S]"; }

        if (is_sel) {
            // 선택 행: 전체 폭을 C_HIGHLIGHT+A_REVERSE 로 먼저 클리어
            wattron(win, COLOR_PAIR(C_HIGHLIGHT) | A_REVERSE | A_BOLD);
            mvwhline(win, row_y, 1, ' ', left_w - 2);
            // ▶ 커서 마커
            mvwprintw(win, row_y, 1, "▶");
            // 상태 태그 (하이라이트 안에서)
            mvwprintw(win, row_y, 3, "%s", st_tag);
        } else {
            // 비선택 행: 커서 위치 클리어 + 상태 태그 색상
            mvwaddch(win, row_y, 1, ' ');
            wattron(win, COLOR_PAIR(st_cp)|A_BOLD);
            mvwprintw(win, row_y, 3, "%s", st_tag);
            wattroff(win, COLOR_PAIR(st_cp)|A_BOLD);
        }

        // U1: 필터 매칭 시 이름 부분 하이라이트
        if (filter_match && g_state.vm.filter_active && g_state.vm.filter[0]) {
            const char *hit = strstr(vm->name, g_state.vm.filter);
            int flen = (int)strlen(g_state.vm.filter);
            int pre  = hit ? (int)(hit - vm->name) : 0;
            // 매칭 전 부분
            if (pre > 0)
                mvwprintw(win, row_y, 7, "%.*s", pre, vm->name);
            // 매칭 부분 (노란색 강조)
            wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD|A_UNDERLINE);
            if (hit) mvwprintw(win, row_y, 7+pre, "%.*s", flen, hit);
            wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD|A_UNDERLINE);
            // 매칭 후 부분
            if (hit && hit+flen < vm->name+strlen(vm->name))
                mvwprintw(win, row_y, 7+pre+flen, "%-*.*s",
                          14-pre-flen, 14-pre-flen, hit+flen);
        } else {
            mvwprintw(win, row_y, 7, "%-14.14s", vm->name);
        }

        // V1: 인라인 CPU/MEM — 퍼센트 수치만 표시 (컴팩트)
        if (left_w >= 40 && vm->is_running) {
            mvwprintw(win, row_y, 22, "%dc", vm->vcpu);

            // CPU%
            int cpu_cp = !is_sel ? pcv_color_for_pct(vm->live_cpu_pct / 100.0) : C_HIGHLIGHT;
            if (!is_sel) wattron(win, COLOR_PAIR(cpu_cp) | A_BOLD);
            mvwprintw(win, row_y, 26, "C:%3d%%", vm->live_cpu_pct);
            if (!is_sel) wattroff(win, COLOR_PAIR(cpu_cp) | A_BOLD);

            // MEM%
            double eff_mem = vm->mem_percent;
            if (eff_mem < 0.0 && vm->mem_max_mb > 0) eff_mem = 50.0;
            int mem_cp = !is_sel
                ? (eff_mem < 0 ? C_DIM : pcv_color_for_pct(eff_mem / 100.0))
                : C_HIGHLIGHT;
            if (!is_sel) wattron(win, COLOR_PAIR(mem_cp) | A_BOLD);
            if (eff_mem < 0.0)
                mvwprintw(win, row_y, 33, "M: N/A");
            else
                mvwprintw(win, row_y, 33, "M:%3d%%", (int)eff_mem);
            if (!is_sel) wattroff(win, COLOR_PAIR(mem_cp) | A_BOLD);

            // 메모리 크기 (MB)
            if (left_w >= 50 && vm->mem_max_mb > 0) {
                wattron(win, COLOR_PAIR(C_DIM));
                mvwprintw(win, row_y, 40, "%.0f/%.0fM",
                    vm->mem_used_mb > 0 ? vm->mem_used_mb : 0, vm->mem_max_mb);
                wattroff(win, COLOR_PAIR(C_DIM));
            }
        }

        if (is_sel)
            wattroff(win, COLOR_PAIR(C_HIGHLIGHT) | A_REVERSE | A_BOLD);
        row_y++;
    }
    draw_scrollbar(win, y0+1, mid_h-2, left_w-2, &g_state.vm.scroll);

    // ── 인스펙터 (우측) ──────────────────────────────────────────────────────
    // K1-VM: VMware 스타일 2분할 — 상단 Devices + 하단 VM Details
    int dev_h  = mid_h * 3 / 5;   // 상단 Devices 영역
    int det_h  = mid_h - dev_h;   // 하단 VM Details 영역
    int det_y  = y0 + dev_h;
    draw_panel(win, y0,    left_w, dev_h, right_w, "devices",            C_CYAN);
    draw_panel(win, det_y, left_w, det_h, right_w, "virtual machine details", C_FLEET);

    if (g_state.vm.fleet_count == 0) return;
    VMMetrics *v = &g_state.vm.fleet[g_state.vm.selected];
    int ix = left_w + 3;
    int max_y_dev = det_y - 1;
    int max_y_det = y0 + mid_h - 5;  /* act_y-1: 액션바 구분선과 겹침 방지 */
    int bar_w = right_w - 22;

    // ── VM 이름 타이틀 ──────────────────────────────────────────────────────
    wattron(win, A_BOLD | COLOR_PAIR(C_TITLE));
    mvwprintw(win, y0+1, ix, "▶  %s", v->name);
    wattroff(win, A_BOLD | COLOR_PAIR(C_TITLE));

    // ── DEVICES 섹션 (VMware 좌측 패널 스타일) ─────────────────────────────
    // 디바이스 항목: 아이콘 + 레이블(14) + 값
    #define DEV_ROW(iy, icon, label, cp, fmt, ...) do {         if ((iy) < max_y_dev) {             wattron(win, COLOR_PAIR(C_DIM));             mvwprintw(win, iy, ix,    "%s %-18s", icon, label);             wattroff(win, COLOR_PAIR(C_DIM));             wattron(win, COLOR_PAIR(cp));             mvwprintw(win, iy, ix+21, fmt, ##__VA_ARGS__);             wattroff(win, COLOR_PAIR(cp));             (iy)++;         }     } while(0)

    int diy = y0 + 2;

    // Memory
    {
        char mem_str[32];
        if (v->mem_max_mb >= 1024.0)
            snprintf(mem_str, sizeof(mem_str), "%.0f GB", v->mem_max_mb/1024.0);
        else
            snprintf(mem_str, sizeof(mem_str), "%.0f MB", v->mem_max_mb);
        DEV_ROW(diy, "▪", "Memory", C_MEM, "%s", mem_str);
        // 메모리 사용률 바
        if (diy < max_y_dev && v->mem_used_mb >= 0.0) {
            double used_gb = v->mem_used_mb/1024.0;
            double max_gb  = v->mem_max_mb/1024.0;
            int bw = bar_w < 4 ? 4 : bar_w;
            int filled = (max_gb > 0) ? (int)(used_gb/max_gb * bw) : 0;
            int mem_cp = pcv_color_for_pct(v->mem_percent/100.0);
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, diy, ix+2, "  %.1fGB / %.1fGB [", used_gb, max_gb);
            wattroff(win, COLOR_PAIR(C_DIM));
            int bar_sx = ix + 22;
            wattron(win, COLOR_PAIR(mem_cp)|A_BOLD);
            for (int b=0; b<bw && bar_sx+b < left_w+right_w-4; b++)
                mvwaddch(win, diy, bar_sx+b, b<filled ? ACS_BLOCK : '-');
            wattroff(win, COLOR_PAIR(mem_cp)|A_BOLD);
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, diy, bar_sx+bw, "] %3d%%", (int)v->mem_percent);
            wattroff(win, COLOR_PAIR(C_DIM));
            diy++;
        }
    }

    // Processors
    DEV_ROW(diy, "▪", "Processors", C_CPU, "%d", v->vcpu);

    // Hard Disk
    {
        char disk_label[32];
        // 버스 타입 → 표시명
        const char *bus_disp = "Disk";
        if      (g_strcmp0(v->disk_bus,"nvme")   == 0) bus_disp = "Hard Disk (NVMe)";
        else if (g_strcmp0(v->disk_bus,"sata")   == 0) bus_disp = "Hard Disk (SATA)";
        else if (g_strcmp0(v->disk_bus,"virtio") == 0) bus_disp = "Hard Disk (VirtIO)";
        else if (g_strcmp0(v->disk_bus,"ide")    == 0) bus_disp = "Hard Disk (IDE)";
        snprintf(disk_label, sizeof(disk_label), "%-18.18s", bus_disp);
        char disk_disp[260];
        if (g_strcmp0(v->disk_size,"N/A") != 0)
            snprintf(disk_disp, sizeof(disk_disp), "%s", v->disk_size);
        else
            snprintf(disk_disp, sizeof(disk_disp), "%s", v->disk_path[0]=='/' ?
                     g_path_get_basename(v->disk_path) : v->disk_path);
        DEV_ROW(diy, "▪", disk_label, C_DIM, "%s", disk_disp);
        // 디스크 경로 (줄임)
        if (diy < max_y_dev && g_strcmp0(v->disk_path,"N/A") != 0) {
            int path_w = right_w - 6;
            wattron(win, COLOR_PAIR(C_DIM)|A_DIM);
            mvwprintw(win, diy++, ix+4, "%-*.*s", path_w, path_w, v->disk_path);
            wattroff(win, COLOR_PAIR(C_DIM)|A_DIM);
        }
    }

    // CD/DVD
    {
        char cdrom_disp[64];
        if (g_strcmp0(v->cdrom_path,"(empty)") == 0)
            snprintf(cdrom_disp, sizeof(cdrom_disp), "(empty)");
        else
            snprintf(cdrom_disp, sizeof(cdrom_disp), "%s",
                     g_path_get_basename(v->cdrom_path));
        int cdcp = (g_strcmp0(v->cdrom_path,"(empty)") == 0) ? C_DIM : C_YELLOW;
        DEV_ROW(diy, "▪", "CD/DVD (SATA)", cdcp, "%s", cdrom_disp);
    }

    // Network Adapter
    {
        char net_disp[120];
        snprintf(net_disp, sizeof(net_disp), "%s", v->net_source);
        if (g_strcmp0(v->net_model,"N/A") != 0)
            snprintf(net_disp, sizeof(net_disp), "%s  [%s]", v->net_source, v->net_model);
        DEV_ROW(diy, "▪", "Network Adapter", C_GREEN, "%s", net_disp);
        // MAC
        if (diy < max_y_dev) {
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, diy++, ix+4, "MAC: %s", v->mac);
            wattroff(win, COLOR_PAIR(C_DIM));
        }
    }

    // Display (VNC)
    {
        int disp_cp = (g_strcmp0(v->vnc_port,"N/A") == 0) ? C_DIM : C_CYAN;
        char disp_str[32];
        if (g_strcmp0(v->vnc_port,"N/A") == 0)
            snprintf(disp_str, sizeof(disp_str), "VNC N/A");
        else
            snprintf(disp_str, sizeof(disp_str), "VNC localhost%s", v->vnc_port);
        DEV_ROW(diy, "▪", "Display", disp_cp, "%s", disp_str);
    }

    // Live CPU 바 (RUNNING 시)
    if (v->has_live && diy < max_y_dev - 1) {
        int bw = bar_w < 4 ? 4 : bar_w;
        int cpu_cp = pcv_color_for_pct(v->live_cpu_pct / 100.0);
        wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
        mvwprintw(win, diy, ix, "▸ CPU Live  %3d%%  [", v->live_cpu_pct);
        wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
        int bar_sx = ix + 20;
        int filled = (int)(v->live_cpu_pct / 100.0 * bw);
        wattron(win, COLOR_PAIR(cpu_cp)|A_BOLD);
        for (int b=0; b<bw && bar_sx+b < left_w+right_w-4; b++)
            mvwaddch(win, diy, bar_sx+b, b<filled ? ACS_BLOCK : '-');
        wattroff(win, COLOR_PAIR(cpu_cp)|A_BOLD);
        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, diy, bar_sx+bw, "]");
        wattroff(win, COLOR_PAIR(C_DIM));
        diy++;
    }
    #undef DEV_ROW

    // ── VIRTUAL MACHINE DETAILS 섹션 ─────────────────────────────────────
    #define DET_ROW(iy, label, cp, fmt, ...) do {         if ((iy) < max_y_det) {             wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD);             mvwprintw(win, iy, ix, "%-18s", label);             wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);             wattron(win, COLOR_PAIR(cp));             mvwprintw(win, iy, ix+18, fmt, ##__VA_ARGS__);             wattroff(win, COLOR_PAIR(cp));             (iy)++;         }     } while(0)

    int tiy = det_y + 1;

    // State (색상 5단계)
    {
        int s_cp;
        if      (g_strcmp0(v->state,"RUNNING")    == 0) s_cp = C_GREEN;
        else if (g_strcmp0(v->state,"PAUSED")     == 0) s_cp = C_YELLOW;
        else if (g_strcmp0(v->state,"CRASHED")    == 0) s_cp = C_RED;
        else if (g_strcmp0(v->state,"PMSUSPENDED")== 0) s_cp = C_YELLOW;
        else                                             s_cp = C_DIM;
        DET_ROW(tiy, "State:", s_cp, "%s", v->state);
    }

    // Primary IP
    DET_ROW(tiy, "Primary IP:", C_GREEN, "%s", v->ip);

    // UUID
    DET_ROW(tiy, "UUID:", C_DIM, "%.36s", v->uuid);

    // Autostart
    DET_ROW(tiy, "Autostart:", v->autostart ? C_GREEN : C_DIM,
            "%s", v->autostart ? "Enabled" : "Disabled");

    // Persistent
    DET_ROW(tiy, "Persistent:", v->persistent ? C_GREEN : C_DIM,
            "%s", v->persistent ? "Yes" : "No");

    // Disk I/O
    {
        char rd[12],wr[12]; format_bytes(v->disk_rd,rd,sizeof(rd)); format_bytes(v->disk_wr,wr,sizeof(wr));
        DET_ROW(tiy, "Disk I/O:", C_DIM, "Rd %-8s Wr %s", rd, wr);
    }
    // Net I/O
    {
        char rx[12],tx[12]; format_bytes(v->net_rx,rx,sizeof(rx)); format_bytes(v->net_tx,tx,sizeof(tx));
        DET_ROW(tiy, "Net I/O:", C_DIM, "RX %-8s TX %s", rx, tx);
    }

    #undef DET_ROW

    // 액션 바 (패널 바닥 고정)
    int act_y = y0 + mid_h - 6;
    wattron(win, COLOR_PAIR(C_DIM));
    mvwhline(win, act_y-1, ix, ACS_HLINE, right_w-6);
    wattroff(win, COLOR_PAIR(C_DIM));
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, act_y,   ix, "ACTIONS:");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, act_y,   ix+9,
              "[c]Create [s]Start [x]Stop [d]Delete [m]Metrics");
    mvwprintw(win, act_y+1, ix+9,
              "[n]Snap   [l]SnapList [v]vCPU [H]RAM  [L]Limit");
    mvwprintw(win, act_y+2, ix+9,
              "[r]Rename [k]Clone [O]ExpOVA [U]ImpOVA [e]Eject");
    mvwprintw(win, act_y+3, ix+9,
              "[V]VNC [T]Templates [A]Autostart [I]IOThrottle [G]GuestAgent");
    mvwprintw(win, act_y+4, ix+9,
              "[W]Bandwidth [Y]USB [N]NICs [+]NIC+ [-]NIC- [/]Filter");
}

// ── VM 키 핸들러 (Elm Update) ─────────────────────────────────────────────────
static void handle_key_vm(int ch) {
    int fc = g_state.vm.fleet_count;
    int *sel = &g_state.vm.selected;

    // A3: Space → 다중 선택 토글
    if (ch == ' ') {
        int cur = *sel;
        if (cur < fc) {
            g_state.vm.multi_sel[cur] = !g_state.vm.multi_sel[cur];
            g_state.vm.multi_count += g_state.vm.multi_sel[cur] ? 1 : -1;
            if (g_state.vm.multi_count < 0) g_state.vm.multi_count = 0;
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "Multi-sel: %d VM(s) marked", g_state.vm.multi_count);
            push_log(tmp);
            if (*sel < fc-1) (*sel)++;
            g_state.dirty.roster = TRUE;
        }
        return;
    }
    // A3: 'X' → 선택 VM 전체 Stop
    if (ch == 'X' && g_state.vm.multi_count > 0) {
        char warn[80];
        snprintf(warn, sizeof(warn), "STOP ALL %d selected VMs?", g_state.vm.multi_count);
        if (confirm_dialog(warn, "STOP ALL")) {
            for (int i = 0; i < fc; i++) {
                if (!g_state.vm.multi_sel[i]) continue;
                JsonObject *p2 = json_object_new();
                json_object_set_string_member(p2, "vm_id", g_state.vm.fleet[i].name);
                char log_ok[256];
                snprintf(log_ok, sizeof(log_ok), "BATCH STOP [%s]", g_state.vm.fleet[i].name);
                send_async_rpc("vm.stop", p2, log_ok, "BATCH");
                g_state.vm.multi_sel[i] = FALSE;
            }
            g_state.vm.multi_count = 0;
            push_log("Batch Stop dispatched");
            g_state.dirty.all = TRUE;
        }
        return;
    }
    // A3: 'Z' → 다중 선택 전체 해제
    if (ch == 'Z') {
        memset(g_state.vm.multi_sel, 0, sizeof(g_state.vm.multi_sel));
        g_state.vm.multi_count = 0;
        push_log("Multi-sel cleared");
        g_state.dirty.roster = TRUE;
        return;
    }
    // U3: Tab → 정렬 모드 순환 (NAME→CPU→MEM→STATE)
    if (ch == '	') {
        g_state.vm.sort_mode = (g_state.vm.sort_mode + 1) % 6;
        static const char *slabels[] = { "NAME", "CPU↓", "MEM↓", "STATE" };
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "Sort → %s", slabels[g_state.vm.sort_mode]);
        push_log(tmp);
        g_state.dirty.roster = TRUE;
        return;
    }
    // 오버레이 활성 중
    if (g_state.vm.snap_overlay.active) return;

    switch (ch) {
    case KEY_UP:
        if (*sel > 0) { (*sel)--; g_state.dirty.roster = g_state.dirty.detail = TRUE; }
        break;
    case KEY_DOWN:
        if (*sel < fc-1) { (*sel)++; g_state.dirty.roster = g_state.dirty.detail = TRUE; }
        break;
    case KEY_PPAGE: // Page Up
        *sel -= g_state.vm.scroll.viewport;
        if (*sel < 0) *sel = 0;
        g_state.dirty.all = TRUE;
        break;
    case KEY_NPAGE: // Page Down
        *sel += g_state.vm.scroll.viewport;
        if (*sel >= fc) *sel = fc > 0 ? fc-1 : 0;
        g_state.dirty.all = TRUE;
        break;

    // ── 검색 (/ 키) ────────────────────────────────────────────────────────
    case '/': {
        char fbuf[64] = {0};
        prompt_input("Filter: ", fbuf, sizeof(fbuf), C_YELLOW);
        strncpy(g_state.vm.filter, fbuf, sizeof(g_state.vm.filter)-1);
        g_state.vm.filter_active = (fbuf[0] != '\0');
        g_state.dirty.all = TRUE;
        break;
    }
    case 27: // ESC — 필터 해제
        g_state.vm.filter_active = FALSE;
        g_state.vm.filter[0] = '\0';
        g_state.dirty.all = TRUE;
        break;

    // ── RENAME ─────────────────────────────────────────────────────────────
    case 'r': if (fc > 0) {
        const VMMetrics *vm = &g_state.vm.fleet[*sel];
        const char *vn = vm->name;
        if (!vm_state_is_shutoff(vm->state)) {
            push_log_level("RENAME blocked: VM must be shut off", LOG_WARN);
            break;
        }
        char new_name[64] = {0};
        char pmsg[160];
        snprintf(pmsg, sizeof(pmsg), "New name for [%s]: ", vn);
        if (prompt_input(pmsg, new_name, sizeof(new_name), C_YELLOW) && new_name[0]) {
            g_strstrip(new_name);
            if (!new_name[0] || g_strcmp0(new_name, vn) == 0) {
                push_log_level("RENAME skipped: unchanged name", LOG_WARN);
                break;
            }
            char warn[192];
            snprintf(warn, sizeof(warn), "RENAME VM '%s' -> '%s'?", vn, new_name);
            if (confirm_dialog(warn, "RENAME")) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", vn);
                json_object_set_string_member(p, "new_name", new_name);
                char log_ok[256];
                snprintf(log_ok, sizeof(log_ok), "RENAME [%s] -> [%s]", vn, new_name);
                send_async_rpc("vm.rename", p, log_ok, "RENAME");
            }
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── CREATE ─────────────────────────────────────────────────────────────
    case 'c': {
        char name[64]={0}, vcpu_s[8]={0}, ram_s[8]={0}, disk_s[8]={0};
        char iso_s[128]={0}, br_s[64]={0}, st_s[16]={0};
        if (!prompt_input("[1/7] VM Name: ", name, sizeof(name), C_YELLOW) || !name[0]) break;
        prompt_input("[2/7] vCPU (default 2): ", vcpu_s, sizeof(vcpu_s), C_YELLOW);
        prompt_input("[3/7] RAM GB (default 2): ", ram_s, sizeof(ram_s), C_YELLOW);
        prompt_input("[4/7] Disk GB (default 50): ", disk_s, sizeof(disk_s), C_YELLOW);
        prompt_input("[5/7] Storage [zvol/qcow2/raw] (default zvol): ", st_s, sizeof(st_s), C_YELLOW);
        prompt_input("[6/7] ISO path (Enter=skip): ", iso_s, sizeof(iso_s), C_YELLOW);
        prompt_input("[7/7] Bridge (Enter=skip): ", br_s, sizeof(br_s), C_YELLOW);
        int vcpu = atoi(vcpu_s); if (vcpu<=0) vcpu=2;
        int ram  = atoi(ram_s);  if (ram<=0)  ram=2;
        int disk = atoi(disk_s); if (disk<=0) disk=50;
        /* storage_type 검증: zvol/qcow2/raw 중 하나, 그 외 zvol */
        const char *st = "zvol";
        if (g_strcmp0(st_s, "qcow2") == 0 || g_strcmp0(st_s, "raw") == 0)
            st = st_s;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name",         name);
        json_object_set_int_member   (p, "vcpu",         vcpu);
        json_object_set_int_member   (p, "memory_mb",    (gint64)ram * 1024);
        json_object_set_int_member   (p, "disk_size_gb", disk);
        json_object_set_string_member(p, "storage_type", st);
        if (iso_s[0]) json_object_set_string_member(p, "iso_path",       iso_s);
        if (br_s[0])  json_object_set_string_member(p, "network_bridge", br_s);
        char log_ok[160];
        snprintf(log_ok, sizeof(log_ok),
                 "CREATE [%s] %dvCPU %dGB RAM %dGB DISK (%s)", name, vcpu, ram, disk, st);
        send_async_rpc("vm.create", p, log_ok, "CREATE");
        break;
    }

    // ── START / STOP ────────────────────────────────────────────────────────
    case 's': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "START [%s]", vn);
        send_async_rpc("vm.start", p, log_ok, "START");
        break;
    } break;
    case 'x': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "STOP [%s]", vn);
        send_async_rpc("vm.stop", p, log_ok, "STOP");
        break;
    } break;

    // ── PAUSE / RESUME toggle ───────────────────────────────────────────
    case 'p': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        const char *st = g_state.vm.fleet[*sel].state;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        if (g_strcmp0(st, "paused") == 0) {
            char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "RESUME [%s]", vn);
            send_async_rpc("vm.resume", p, log_ok, "RESUME");
        } else {
            char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "PAUSE [%s]", vn);
            send_async_rpc("vm.pause", p, log_ok, "PAUSE");
        }
        break;
    } break;

    // ── DELETE ─────────────────────────────────────────────────────────────
    case 'd': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        char warn[320];
        snprintf(warn, sizeof(warn), "DELETE VM '%s' IS IRREVERSIBLE!", vn);
        if (confirm_dialog(warn, vn)) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "vm_id", vn);
            char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "DELETE [%s]", vn);
            send_async_rpc("vm.delete", p, log_ok, "DELETE");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── CLONE ──────────────────────────────────────────────────────────────
    case 'k': if (fc > 0) {
        const VMMetrics *vm = &g_state.vm.fleet[*sel];
        const char *vn = vm->name;
        if (!vm_state_is_shutoff(vm->state)) {
            push_log_level("CLONE blocked: source VM must be shut off", LOG_WARN);
            break;
        }
        char clone_name[64] = {0}, mode_s[12] = {0}, prep_s[24] = {0};
        char pmsg[160];
        snprintf(pmsg, sizeof(pmsg), "Clone name for [%s]: ", vn);
        if (!prompt_input(pmsg, clone_name, sizeof(clone_name), C_YELLOW) || !clone_name[0])
            break;
        g_strstrip(clone_name);
        prompt_input("Mode [cow/full] (default cow): ", mode_s, sizeof(mode_s), C_YELLOW);
        g_strstrip(mode_s);
        const char *mode = (g_ascii_strcasecmp(mode_s, "full") == 0) ? "full" : "cow";
        prompt_input("Identity [guest-reset/prepared] (default guest-reset): ",
                     prep_s, sizeof(prep_s), C_YELLOW);
        g_strstrip(prep_s);
        gboolean prepared = g_ascii_strcasecmp(prep_s, "prepared") == 0 ||
                            g_ascii_strcasecmp(prep_s, "template") == 0 ||
                            g_ascii_strcasecmp(prep_s, "prepared-template") == 0;

        char warn[320];
        snprintf(warn, sizeof(warn), "CLONE '%s' -> '%s' (%s, %s)?",
                 vn, clone_name, mode, prepared ? "prepared" : "guest-reset");
        if (confirm_dialog(warn, "CLONE")) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "source", vn);
            json_object_set_string_member(p, "clone_name", clone_name);
            json_object_set_string_member(p, "mode", mode);
            if (prepared)
                json_object_set_boolean_member(p, "template_prepared", TRUE);
            else
                json_object_set_boolean_member(p, "guest_reset", TRUE);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "CLONE [%s] -> [%s] accepted", vn, clone_name);
            send_async_rpc("vm.clone", p, log_ok, "CLONE");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── OVA EXPORT ─────────────────────────────────────────────────────────
    case 'O': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        char output_dir[160] = {0};
        prompt_input("OVA output directory (default /tmp): ",
                     output_dir, sizeof(output_dir), C_YELLOW);
        g_strstrip(output_dir);
        char warn[192];
        snprintf(warn, sizeof(warn), "EXPORT OVA for '%s'?", vn);
        if (confirm_dialog(warn, "OVA EXPORT")) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", vn);
            if (output_dir[0])
                json_object_set_string_member(p, "output_dir", output_dir);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "OVA EXPORT [%s] accepted", vn);
            send_async_rpc("vm.export.ova", p, log_ok, "OVA EXPORT");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── OVA IMPORT ─────────────────────────────────────────────────────────
    case 'U': {
        char ova_path[192] = {0}, name[64] = {0}, pool[96] = {0};
        if (!prompt_input("OVA file path: ", ova_path, sizeof(ova_path), C_YELLOW) || !ova_path[0])
            break;
        if (!prompt_input("Target VM name: ", name, sizeof(name), C_YELLOW) || !name[0])
            break;
        prompt_input("ZFS pool/dataset (Enter=default): ", pool, sizeof(pool), C_YELLOW);
        g_strstrip(ova_path);
        g_strstrip(name);
        g_strstrip(pool);
        char warn[320];
        snprintf(warn, sizeof(warn), "IMPORT OVA '%s' as '%s'?", ova_path, name);
        if (confirm_dialog(warn, "OVA IMPORT")) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "ova_path", ova_path);
            json_object_set_string_member(p, "name", name);
            if (pool[0])
                json_object_set_string_member(p, "pool", pool);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "OVA IMPORT [%s] accepted", name);
            send_async_rpc("vm.import.ova", p, log_ok, "OVA IMPORT");
        }
        g_state.dirty.all = TRUE;
        break;
    }

    // ── METRICS ─────────────────────────────────────────────────────────────
    case 'm': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        GError *err = NULL;
        gchar  *resp = tui_send_request("vm.metrics", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    g_state.vm.fleet[*sel].live_cpu_pct = (gint)safe_int(res,"cpu");
                    g_state.vm.fleet[*sel].live_mem_pct = (gint)safe_int(res,"mem");
                    g_state.vm.fleet[*sel].has_live = TRUE;
                    char tmp[128];
                    snprintf(tmp, sizeof(tmp), "METRICS [%s] CPU=%d%% MEM=%d%%",
                             vn, g_state.vm.fleet[*sel].live_cpu_pct,
                             g_state.vm.fleet[*sel].live_mem_pct);
                    push_log(tmp);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("METRICS ERR"); g_error_free(err); }
        g_state.dirty.detail = TRUE;
        break;
    } break;

    // ── SNAPSHOT CREATE ──────────────────────────────────────────────────────
    case 'n': if (fc > 0) {
        char snap[64] = {0};
        const char *vn = g_state.vm.fleet[*sel].name;
        char pmsg[80]; snprintf(pmsg, sizeof(pmsg), "Snap name [%s]: ", vn);
        if (prompt_input(pmsg, snap, sizeof(snap), C_CYAN) && snap[0]) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "vm_id",     vn);
            json_object_set_string_member(p, "snap_name", snap);
            char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "SNAP [%s]@%s", vn, snap);
            send_async_rpc("vm.snapshot.create", p, log_ok, "SNAP");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── SNAPSHOT LIST (l) ──────────────────────────────────────────────────
    case 'l': if (fc > 0) {
        show_snapshot_overlay();
        draw_snapshot_overlay(stdscr);
        break;
    } break;

    // ── RAM HOTPLUG (h) ─────────────────────────────────────────────────────
    case 'h': /* 도움말 표시 */
        draw_help_overlay();
        break;

    case 'H': if (fc > 0) { /* Shift+H: 메모리 핫플러그 (기존 h 기능) */
        char mem_s[16] = {0};
        const char *vn = g_state.vm.fleet[*sel].name;
        char pmsg[256]; snprintf(pmsg, sizeof(pmsg), "New RAM for [%s] MB: ", vn);
        if (prompt_input(pmsg, mem_s, sizeof(mem_s), C_MEM) && mem_s[0]) {
            int mb = atoi(mem_s);
            if (mb > 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id",     vn);
                json_object_set_int_member   (p, "memory_mb", mb);
                char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "RAM [%s] → %dMB", vn, mb);
                send_async_rpc("vm.set_memory", p, log_ok, "HOTPLUG");
            }
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── vCPU HOTPLUG (v) ────────────────────────────────────────────────────
    case 'v': if (fc > 0) {
        char vc_s[8] = {0};
        const char *vn = g_state.vm.fleet[*sel].name;
        char pmsg[256]; snprintf(pmsg, sizeof(pmsg), "New vCPU for [%s]: ", vn);
        if (prompt_input(pmsg, vc_s, sizeof(vc_s), C_YELLOW) && vc_s[0]) {
            int vc = atoi(vc_s);
            if (vc > 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id",      vn);
                json_object_set_int_member   (p, "vcpu_count", vc);
                char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "vCPU [%s] → %d", vn, vc);
                send_async_rpc("vm.set_vcpu", p, log_ok, "VCPU");
            }
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── EJECT ISO (e) ────────────────────────────────────────────────────────
    case 'e': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "EJECT [%s] ISO", vn);
        send_async_rpc("vm.eject", p, log_ok, "EJECT");
        break;
    } break;

    // ── LIMIT (L) ────────────────────────────────────────────────────────────
    case 'L': if (fc > 0) {
        char cpu_s[8]={0}, mem_s[16]={0};
        const char *vn = g_state.vm.fleet[*sel].name;
        prompt_input("CPU quota % (0=unlimited): ", cpu_s, sizeof(cpu_s), C_YELLOW);
        prompt_input("MEM quota MB (0=unlimited): ", mem_s, sizeof(mem_s), C_YELLOW);
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        if (cpu_s[0]) json_object_set_int_member(p, "cpu", atoi(cpu_s));
        if (mem_s[0]) json_object_set_int_member(p, "mem", atoi(mem_s));
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "LIMIT [%s]", vn);
        send_async_rpc("vm.limit", p, log_ok, "LIMIT");
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── VNC (V) ──────────────────────────────────────────────────────────────
    case 'V': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        GError *err = NULL;
        gchar *resp = tui_send_request("vm.vnc", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    const gchar *addr = safe_str(res, "vnc_address", "");
                    const gchar *port = safe_str(res, "vnc_port",    "");
                    char tmp[128];
                    snprintf(tmp, sizeof(tmp), "VNC [%s] → %s:%s", vn, addr, port);
                    push_log(tmp);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("VNC ERR"); g_error_free(err); }
        g_state.dirty.detail = TRUE;
        break;
    } break;

    // ── AUTOSTART TOGGLE (A) ────────────────────────────────────────────────
    case 'A': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", vn);
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "AUTOSTART toggle [%s]", vn);
        send_async_rpc("vm.autostart", p, log_ok, "AUTOSTART");
        break;
    } break;

    // ── DISK I/O THROTTLE (I) ───────────────────────────────────────────────
    case 'I': if (fc > 0) {
        char riops_s[16]={0}, wiops_s[16]={0};
        const char *vn = g_state.vm.fleet[*sel].name;
        char pmsg[256]; snprintf(pmsg, sizeof(pmsg), "Read IOPS for [%s]: ", vn);
        if (prompt_input(pmsg, riops_s, sizeof(riops_s), C_YELLOW) && riops_s[0]) {
            snprintf(pmsg, sizeof(pmsg), "Write IOPS for [%s]: ", vn);
            prompt_input(pmsg, wiops_s, sizeof(wiops_s), C_YELLOW);
            int riops = atoi(riops_s);
            int wiops = wiops_s[0] ? atoi(wiops_s) : 0;
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", vn);
            json_object_set_string_member(p, "device", "vda");
            json_object_set_int_member(p, "read_iops_sec", riops);
            json_object_set_int_member(p, "write_iops_sec", wiops);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "IO THROTTLE [%s] r=%d w=%d", vn, riops, wiops);
            send_async_rpc("vm.blkio.set", p, log_ok, "THROTTLE");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── NETWORK BANDWIDTH QOS (W) ─────────────────────────────────────────
    case 'W': if (fc > 0) {
        char inbound_s[16] = {0}, outbound_s[16] = {0};
        const char *vn = g_state.vm.fleet[*sel].name;
        prompt_input("Inbound KB/s (Enter=skip): ", inbound_s, sizeof(inbound_s), C_YELLOW);
        prompt_input("Outbound KB/s (Enter=skip): ", outbound_s, sizeof(outbound_s), C_YELLOW);
        int inbound = inbound_s[0] ? atoi(inbound_s) : 0;
        int outbound = outbound_s[0] ? atoi(outbound_s) : 0;
        if (inbound <= 0 && outbound <= 0) {
            push_log_level("BANDWIDTH: at least one limit must be > 0", LOG_WARN);
        } else {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", vn);
            if (inbound > 0)
                json_object_set_int_member(p, "inbound_kbps", inbound);
            if (outbound > 0)
                json_object_set_int_member(p, "outbound_kbps", outbound);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "BANDWIDTH [%s] in=%dKB/s out=%dKB/s",
                     vn, inbound, outbound);
            send_async_rpc("vm.set_bandwidth", p, log_ok, "BANDWIDTH");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── TEMPLATE LIST (T) ───────────────────────────────────────────────────
    case 'T': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("template.list", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = json_array_get_length(arr);
                        push_log_level("=== VM Templates ===", LOG_SYS);
                        for (guint i = 0; i < len; i++) {
                            JsonObject *t = json_array_get_object_element(arr, i);
                            const gchar *tname = safe_str(t, "name", "?");
                            gint64 tvcpu = safe_int(t, "vcpu");
                            gint64 tmem  = safe_int(t, "memory_mb");
                            gint64 tdisk = safe_int(t, "disk_gb");
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp),
                                     "  [%s] %ldvCPU %ldMB %ldGB",
                                     tname, (long)tvcpu, (long)tmem, (long)tdisk);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no templates)");
                    }
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("TEMPLATE ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    // ── NIC LIST (N) ──────────────────────────────────────────────────────
    case 'N': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", vn);
        GError *err = NULL;
        gchar *resp = tui_send_request("device.nic.list", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = arr ? json_array_get_length(arr) : 0;
                        push_log_level("=== VM NICs ===", LOG_SYS);
                        for (guint ni = 0; ni < len; ni++) {
                            JsonObject *nic = json_array_get_object_element(arr, ni);
                            const gchar *mac = safe_str(nic, "mac", "?");
                            const gchar *br = safe_str(nic, "bridge", "?");
                            const gchar *model = safe_str(nic, "model", "?");
                            char tmp[128];
                            snprintf(tmp, sizeof(tmp), "  %s — %s (%s)", mac, br, model);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no NICs)");
                    }
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("NIC list error"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── NIC ADD / HOTPLUG (+) ───────────────────────────────────────────
    case '+': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        char bridge[32] = {0};
        prompt_input("Bridge (default pcvbr0): ", bridge, sizeof(bridge), C_YELLOW);
        if (!bridge[0]) strncpy(bridge, "pcvbr0", sizeof(bridge)-1);
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", vn);
        json_object_set_string_member(p, "bridge", bridge);
        json_object_set_string_member(p, "model", "virtio");
        char log_ok[128];
        snprintf(log_ok, sizeof(log_ok), "NIC added to %s (bridge=%s)", vn, bridge);
        send_async_rpc("device.nic.attach", p, log_ok, "NIC ADD");
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── NIC REMOVE (-) ─────────────────────────────────────────────────
    case '-': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        char mac[24] = {0};
        if (!prompt_input("MAC to remove: ", mac, sizeof(mac), C_RED) || !mac[0]) break;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", vn);
        json_object_set_string_member(p, "mac", mac);
        char log_ok[128];
        snprintf(log_ok, sizeof(log_ok), "NIC removed from %s (mac=%s)", vn, mac);
        send_async_rpc("device.nic.detach", p, log_ok, "NIC DEL");
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── USB PASSTHROUGH (Y) ───────────────────────────────────────────────
    case 'Y': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        int ph = 10, pw = 58;
        char title[128];
        snprintf(title, sizeof(title), "USB PASSTHROUGH: %s", vn);
        WINDOW *pop = create_popup(ph, pw, title);

        wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
        mvwprintw(pop, 2, 4, "[1] List attached USB hostdevs");
        mvwprintw(pop, 3, 4, "[2] Attach USB hostdev");
        mvwprintw(pop, 4, 4, "[3] Detach USB hostdev");
        wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, 7, 4, "Press 1-3 or Esc to cancel");
        wattroff(pop, COLOR_PAIR(C_DIM));
        wrefresh(pop);

        timeout(-1);
        int sub = wgetch(pop);
        timeout(50);
        destroy_popup(pop);

        if (sub == '1') {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "vm_id", vn);
            GError *err = NULL;
            gchar *resp = tui_send_request("vm.usb.list", p, &err);
            if (!err && resp) {
                JsonParser *jp = json_parser_new();
                gboolean parsed = FALSE;
                if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                    JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                    if (json_object_has_member(ro, "error")) {
                        JsonObject *eo = json_object_get_object_member(ro, "error");
                        char tmp[256];
                        snprintf(tmp, sizeof(tmp), "USB LIST: %s",
                                 safe_str(eo, "message", "error"));
                        push_log_level(tmp, LOG_WARN);
                        parsed = TRUE;
                    } else if (json_object_has_member(ro, "result")) {
                        JsonNode *rn = json_object_get_member(ro, "result");
                        if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                            JsonArray *arr = json_node_get_array(rn);
                            guint len = arr ? json_array_get_length(arr) : 0;
                            push_log_level("=== VM USB hostdevs ===", LOG_SYS);
                            for (guint ui = 0; ui < len; ui++) {
                                JsonObject *usb = json_array_get_object_element(arr, ui);
                                const gchar *vendor = safe_str(usb, "vendor_id", "?");
                                const gchar *product = safe_str(usb, "product_id", "?");
                                char tmp[128];
                                snprintf(tmp, sizeof(tmp), "  %s:%s", vendor, product);
                                push_log(tmp);
                            }
                            if (len == 0) push_log("  (no USB hostdev attached)");
                            parsed = TRUE;
                        }
                    }
                }
                if (!parsed)
                    push_log_level("USB LIST: failed to parse response", LOG_WARN);
                g_object_unref(jp);
                g_free(resp);
            } else if (err) {
                push_log_level("USB LIST ERR", LOG_WARN);
                g_error_free(err);
            }
        } else if (sub == '2' || sub == '3') {
            char vendor[16] = {0}, product[16] = {0};
            if (!prompt_input("Vendor ID (0xNNNN): ", vendor, sizeof(vendor), C_YELLOW) || !vendor[0])
                break;
            if (!prompt_input("Product ID (0xNNNN): ", product, sizeof(product), C_YELLOW) || !product[0])
                break;
            g_strstrip(vendor);
            g_strstrip(product);
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "vm_id", vn);
            json_object_set_string_member(p, "vendor_id", vendor);
            json_object_set_string_member(p, "product_id", product);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "USB %s [%s] %s:%s",
                     sub == '2' ? "ATTACH" : "DETACH", vn, vendor, product);
            send_async_rpc(sub == '2' ? "vm.usb.attach" : "vm.usb.detach",
                           p, log_ok, sub == '2' ? "USB ATTACH" : "USB DETACH");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── VM MEMORY STATS (B) ─────────────────────────────────────────────
    case 'B': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", vn);
        GError *err = NULL;
        gchar *resp = tui_send_request("vm.memory.stats", p, &err);

        int ph = 14, pw = 52;
        char title[80];
        snprintf(title, sizeof(title), "MEMORY STATS: %s", vn);
        WINDOW *pop = create_popup(ph, pw, title);

        if (err) {
            wattron(pop, COLOR_PAIR(C_RED) | A_BOLD);
            mvwprintw(pop, 3, 4, "Error: %s", err->message);
            wattroff(pop, COLOR_PAIR(C_RED) | A_BOLD);
            g_error_free(err);
        } else if (resp) {
            JsonParser *jp = json_parser_new();
            gboolean parsed = FALSE;
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "error")) {
                    JsonObject *eo = json_object_get_object_member(ro, "error");
                    wattron(pop, COLOR_PAIR(C_RED) | A_BOLD);
                    mvwprintw(pop, 3, 4, "RPC Error: %s",
                              safe_str(eo, "message", "unknown"));
                    wattroff(pop, COLOR_PAIR(C_RED) | A_BOLD);
                } else if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    parsed = TRUE;

                    gint64 actual    = safe_int(res, "actual_balloon_kb");
                    gint64 rss       = safe_int(res, "rss_kb");
                    gint64 unused    = safe_int(res, "unused_kb");
                    gint64 available = safe_int(res, "available_kb");
                    gint64 usable    = safe_int(res, "usable_kb");

                    int ry = 2;
                    wattron(pop, COLOR_PAIR(C_TITLE) | A_BOLD);
                    mvwprintw(pop, ry++, 4, "%-16s %12s", "Metric", "Value (MB)");
                    wattroff(pop, COLOR_PAIR(C_TITLE) | A_BOLD);
                    wattron(pop, COLOR_PAIR(C_DIM));
                    mvwhline(pop, ry++, 4, ACS_HLINE, pw - 8);
                    wattroff(pop, COLOR_PAIR(C_DIM));

                    wattron(pop, COLOR_PAIR(C_MEM));
                    mvwprintw(pop, ry++, 4, "%-16s %12.1f", "Balloon",
                              (double)actual / 1024.0);
                    wattroff(pop, COLOR_PAIR(C_MEM));
                    wattron(pop, COLOR_PAIR(C_CPU));
                    mvwprintw(pop, ry++, 4, "%-16s %12.1f", "RSS",
                              (double)rss / 1024.0);
                    mvwprintw(pop, ry++, 4, "%-16s %12.1f", "Available",
                              (double)available / 1024.0);
                    mvwprintw(pop, ry++, 4, "%-16s %12.1f", "Usable",
                              (double)usable / 1024.0);
                    mvwprintw(pop, ry++, 4, "%-16s %12.1f", "Unused",
                              (double)unused / 1024.0);
                    wattroff(pop, COLOR_PAIR(C_CPU));
                }
            }
            if (!parsed) {
                wattron(pop, COLOR_PAIR(C_RED));
                mvwprintw(pop, 3, 4, "Failed to parse response");
                wattroff(pop, COLOR_PAIR(C_RED));
            }
            g_object_unref(jp);
            g_free(resp);
        } else {
            wattron(pop, COLOR_PAIR(C_RED));
            mvwprintw(pop, 3, 4, "No response from daemon");
            wattroff(pop, COLOR_PAIR(C_RED));
        }

        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, ph - 2, 4, "Press any key to close");
        wattroff(pop, COLOR_PAIR(C_DIM));
        wrefresh(pop);
        timeout(-1); wgetch(pop); timeout(50);
        destroy_popup(pop);
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── VM CPU STATS (C) ────────────────────────────────────────────────
    case 'C': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", vn);
        GError *err = NULL;
        gchar *resp = tui_send_request("vm.cpu.stats", p, &err);

        int ph = 20, pw = 64;
        char title[80];
        snprintf(title, sizeof(title), "CPU STATS: %s", vn);
        WINDOW *pop = create_popup(ph, pw, title);

        if (err) {
            wattron(pop, COLOR_PAIR(C_RED) | A_BOLD);
            mvwprintw(pop, 3, 4, "Error: %s", err->message);
            wattroff(pop, COLOR_PAIR(C_RED) | A_BOLD);
            g_error_free(err);
        } else if (resp) {
            JsonParser *jp = json_parser_new();
            gboolean parsed = FALSE;
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "error")) {
                    JsonObject *eo = json_object_get_object_member(ro, "error");
                    wattron(pop, COLOR_PAIR(C_RED) | A_BOLD);
                    mvwprintw(pop, 3, 4, "RPC Error: %s",
                              safe_str(eo, "message", "unknown"));
                    wattroff(pop, COLOR_PAIR(C_RED) | A_BOLD);
                } else if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    parsed = TRUE;

                    gint64 vcpu_count = safe_int(res, "vcpu_count");
                    gint64 vcpu_max   = safe_int(res, "max_vcpu");
                    gint64 cpu_time   = safe_int(res, "cpu_time_ns");

                    int ry = 2;
                    wattron(pop, COLOR_PAIR(C_TITLE) | A_BOLD);
                    mvwprintw(pop, ry++, 4, "vCPU: %ld / %ld max    CPU Time: %ld ns",
                              (long)vcpu_count, (long)vcpu_max, (long)cpu_time);
                    wattroff(pop, COLOR_PAIR(C_TITLE) | A_BOLD);
                    ry++;

                    // Per-vCPU table
                    if (json_object_has_member(res, "vcpus")) {
                        JsonArray *arr = json_object_get_array_member(res, "vcpus");
                        guint len = arr ? json_array_get_length(arr) : 0;

                        wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
                        mvwprintw(pop, ry++, 4, "%-6s %-10s %14s  %-10s",
                                  "vCPU", "State", "CPU Time(ns)", "Affinity");
                        wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
                        wattron(pop, COLOR_PAIR(C_DIM));
                        mvwhline(pop, ry++, 4, ACS_HLINE, pw - 8);
                        wattroff(pop, COLOR_PAIR(C_DIM));

                        for (guint vi = 0; vi < len && ry < ph - 2; vi++) {
                            JsonObject *vc = json_array_get_object_element(arr, vi);
                            gint64 vnum    = safe_int(vc, "number");
                            gint64 vstate  = safe_int(vc, "state");
                            const gchar *st = vstate == 0 ? "offline" :
                                              vstate == 1 ? "running" :
                                              vstate == 2 ? "blocked" :
                                              "unknown";
                            gint64 vtime   = safe_int(vc, "cpu_time");
                            const gchar *aff = safe_str(vc, "cpu_affinity", "-");

                            gboolean running = (g_strcmp0(st, "running") == 0);
                            int color = running ? C_GREEN : C_YELLOW;
                            wattron(pop, COLOR_PAIR(color));
                            mvwprintw(pop, ry++, 4, "%-6ld %-10s %14ld  %-10s",
                                      (long)vnum, st, (long)vtime, aff);
                            wattroff(pop, COLOR_PAIR(color));
                        }
                    } else {
                        wattron(pop, COLOR_PAIR(C_DIM));
                        mvwprintw(pop, ry++, 4, "(per-vCPU data not available)");
                        wattroff(pop, COLOR_PAIR(C_DIM));
                    }
                }
            }
            if (!parsed) {
                wattron(pop, COLOR_PAIR(C_RED));
                mvwprintw(pop, 3, 4, "Failed to parse response");
                wattroff(pop, COLOR_PAIR(C_RED));
            }
            g_object_unref(jp);
            g_free(resp);
        } else {
            wattron(pop, COLOR_PAIR(C_RED));
            mvwprintw(pop, 3, 4, "No response from daemon");
            wattroff(pop, COLOR_PAIR(C_RED));
        }

        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, ph - 2, 4, "Press any key to close");
        wattroff(pop, COLOR_PAIR(C_DIM));
        wrefresh(pop);
        timeout(-1); wgetch(pop); timeout(50);
        destroy_popup(pop);
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── GUEST AGENT OPERATIONS (G) ──────────────────────────────────────
    case 'G': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;

        int ph = 12, pw = 56;
        char title[80];
        snprintf(title, sizeof(title), "GUEST AGENT: %s", vn);
        WINDOW *pop = create_popup(ph, pw, title);

        wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
        mvwprintw(pop, 2, 4, "[1] Ping agent");
        mvwprintw(pop, 3, 4, "[2] Execute command");
        mvwprintw(pop, 4, 4, "[3] Graceful shutdown");
        mvwprintw(pop, 5, 4, "[4] Status / package guide");
        mvwprintw(pop, 6, 4, "[5] Ensure libvirt channel");
        wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, 8, 4, "Press 1-5 or Esc to cancel");
        wattroff(pop, COLOR_PAIR(C_DIM));
        wrefresh(pop);

        timeout(-1);
        int sub = wgetch(pop);
        timeout(50);
        destroy_popup(pop);

        if (sub == '1') {
            // ── Guest Ping ──
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", vn);
            GError *gerr = NULL;
            gchar *resp = tui_send_request("vm.guest.ping", p, &gerr);

            int rph = 8, rpw = 44;
            WINDOW *rpop = create_popup(rph, rpw, "GUEST AGENT PING");
            if (gerr) {
                wattron(rpop, COLOR_PAIR(C_RED) | A_BOLD);
                mvwprintw(rpop, 3, 4, "Error: %s", gerr->message);
                wattroff(rpop, COLOR_PAIR(C_RED) | A_BOLD);
                g_error_free(gerr);
            } else if (resp) {
                JsonParser *jp = json_parser_new();
                gboolean ok = FALSE;
                if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                    JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                    if (json_object_has_member(ro, "error")) {
                        JsonObject *eo = json_object_get_object_member(ro, "error");
                        wattron(rpop, COLOR_PAIR(C_RED) | A_BOLD);
                        mvwprintw(rpop, 3, 4, "Not available: %s",
                                  safe_str(eo, "message", "agent error"));
                        wattroff(rpop, COLOR_PAIR(C_RED) | A_BOLD);
                    } else {
                        ok = TRUE;
                        wattron(rpop, COLOR_PAIR(C_GREEN) | A_BOLD);
                        mvwprintw(rpop, 3, 4, "Agent Connected");
                        wattroff(rpop, COLOR_PAIR(C_GREEN) | A_BOLD);
                    }
                }
                if (!ok && !json_object_has_member(
                        json_node_get_object(json_parser_get_root(jp)), "error")) {
                    wattron(rpop, COLOR_PAIR(C_RED));
                    mvwprintw(rpop, 3, 4, "Not available");
                    wattroff(rpop, COLOR_PAIR(C_RED));
                }
                g_object_unref(jp);
                g_free(resp);
            } else {
                wattron(rpop, COLOR_PAIR(C_RED));
                mvwprintw(rpop, 3, 4, "No response");
                wattroff(rpop, COLOR_PAIR(C_RED));
            }
            wattron(rpop, COLOR_PAIR(C_DIM));
            mvwprintw(rpop, rph - 2, 4, "Press any key");
            wattroff(rpop, COLOR_PAIR(C_DIM));
            wrefresh(rpop);
            timeout(-1); wgetch(rpop); timeout(50);
            destroy_popup(rpop);
        } else if (sub == '2') {
            // ── Guest Exec ──
            char cmd_buf[256] = {0};
            if (prompt_input("Command: ", cmd_buf, sizeof(cmd_buf), C_CYAN) && cmd_buf[0]) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", vn);
                json_object_set_string_member(p, "command", cmd_buf);
                GError *gerr = NULL;
                gchar *resp = tui_send_request("vm.guest.exec", p, &gerr);

                int eph = 20, epw = 72;
                WINDOW *epop = create_popup(eph, epw, "GUEST EXEC OUTPUT");

                if (gerr) {
                    wattron(epop, COLOR_PAIR(C_RED) | A_BOLD);
                    mvwprintw(epop, 2, 4, "Error: %s", gerr->message);
                    wattroff(epop, COLOR_PAIR(C_RED) | A_BOLD);
                    g_error_free(gerr);
                } else if (resp) {
                    JsonParser *jp = json_parser_new();
                    if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                        JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                        if (json_object_has_member(ro, "error")) {
                            JsonObject *eo = json_object_get_object_member(ro, "error");
                            wattron(epop, COLOR_PAIR(C_RED) | A_BOLD);
                            mvwprintw(epop, 2, 4, "RPC Error: %s",
                                      safe_str(eo, "message", "unknown"));
                            wattroff(epop, COLOR_PAIR(C_RED) | A_BOLD);
                        } else if (json_object_has_member(ro, "result")) {
                            JsonObject *res = json_object_get_object_member(ro, "result");
                            const gchar *stdout_str = safe_str(res, "stdout", "");
                            gint64 exit_code = safe_int(res, "exit_code");

                            int ry = 2;
                            int ec_color = (exit_code == 0) ? C_GREEN : C_RED;
                            wattron(epop, COLOR_PAIR(ec_color) | A_BOLD);
                            mvwprintw(epop, ry++, 4, "Exit code: %ld",
                                      (long)exit_code);
                            wattroff(epop, COLOR_PAIR(ec_color) | A_BOLD);
                            ry++;

                            // Render stdout line by line with scroll
                            wattron(epop, COLOR_PAIR(C_DIM));
                            const char *line = stdout_str;
                            while (*line && ry < eph - 2) {
                                const char *nl = strchr(line, '\n');
                                int len = nl ? (int)(nl - line) : (int)strlen(line);
                                if (len > epw - 8) len = epw - 8;
                                mvwprintw(epop, ry++, 4, "%.*s", len, line);
                                if (nl) line = nl + 1; else break;
                            }
                            wattroff(epop, COLOR_PAIR(C_DIM));
                        }
                    }
                    g_object_unref(jp);
                    g_free(resp);
                } else {
                    wattron(epop, COLOR_PAIR(C_RED));
                    mvwprintw(epop, 2, 4, "No response from daemon");
                    wattroff(epop, COLOR_PAIR(C_RED));
                }

                wattron(epop, COLOR_PAIR(C_DIM));
                mvwprintw(epop, eph - 2, 4, "Press any key to close");
                wattroff(epop, COLOR_PAIR(C_DIM));
                wrefresh(epop);
                timeout(-1); wgetch(epop); timeout(50);
                destroy_popup(epop);
            }
        } else if (sub == '3') {
            // ── Guest Shutdown ──
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", vn);
            char log_ok[128];
            snprintf(log_ok, sizeof(log_ok), "GUEST SHUTDOWN [%s] sent", vn);
            send_async_rpc("vm.guest.shutdown", p, log_ok, "GUEST SHUTDOWN");
        } else if (sub == '4') {
            // ── Guest Agent Status ──
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", vn);
            GError *gerr = NULL;
            gchar *resp = tui_send_request("vm.guest.agent.status", p, &gerr);

            int sph = 13, spw = 70;
            WINDOW *spop = create_popup(sph, spw, "GUEST AGENT STATUS");
            if (gerr) {
                wattron(spop, COLOR_PAIR(C_RED) | A_BOLD);
                mvwprintw(spop, 3, 4, "Error: %s", gerr->message);
                wattroff(spop, COLOR_PAIR(C_RED) | A_BOLD);
                g_error_free(gerr);
            } else if (resp) {
                JsonParser *jp = json_parser_new();
                gboolean parsed = FALSE;
                if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                    JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                    if (json_object_has_member(ro, "error")) {
                        JsonObject *eo = json_object_get_object_member(ro, "error");
                        wattron(spop, COLOR_PAIR(C_RED) | A_BOLD);
                        mvwprintw(spop, 3, 4, "RPC Error: %s",
                                  safe_str(eo, "message", "unknown"));
                        wattroff(spop, COLOR_PAIR(C_RED) | A_BOLD);
                        parsed = TRUE;
                    } else if (json_object_has_member(ro, "result")) {
                        JsonObject *res = json_object_get_object_member(ro, "result");
                        const gchar *status = safe_str(res, "status", "unknown");
                        const gchar *message = safe_str(res, "message", "");
                        gboolean running = json_object_get_boolean_member_with_default(res, "running", FALSE);
                        gboolean cfg = json_object_get_boolean_member_with_default(res, "channel_configured", FALSE);
                        gboolean live = json_object_get_boolean_member_with_default(res, "channel_live", FALSE);
                        gboolean ping = json_object_get_boolean_member_with_default(res, "agent_ping", FALSE);
                        gboolean pkg = json_object_get_boolean_member_with_default(res, "package_required", FALSE);
                        gboolean reboot = json_object_get_boolean_member_with_default(res, "reboot_required", FALSE);
                        parsed = TRUE;

                        int ry = 2;
                        wattron(spop, COLOR_PAIR(ping ? C_GREEN : C_YELLOW) | A_BOLD);
                        mvwprintw(spop, ry++, 4, "Status: %s", status);
                        wattroff(spop, COLOR_PAIR(ping ? C_GREEN : C_YELLOW) | A_BOLD);
                        mvwprintw(spop, ry++, 4, "Running: %-5s  Config channel: %-5s  Live channel: %-5s",
                                  running ? "true" : "false",
                                  cfg ? "true" : "false",
                                  live ? "true" : "false");
                        mvwprintw(spop, ry++, 4, "Agent ping: %-5s  Package required: %-5s  Reboot: %-5s",
                                  ping ? "true" : "false",
                                  pkg ? "true" : "false",
                                  reboot ? "true" : "false");
                        ry++;
                        wattron(spop, COLOR_PAIR(C_DIM));
                        mvwprintw(spop, ry++, 4, "%.*s", spw - 8, message);
                        if (pkg)
                            mvwprintw(spop, ry++, 4, "Install inside guest: qemu-guest-agent");
                        wattroff(spop, COLOR_PAIR(C_DIM));
                    }
                }
                if (!parsed) {
                    wattron(spop, COLOR_PAIR(C_RED));
                    mvwprintw(spop, 3, 4, "Failed to parse response");
                    wattroff(spop, COLOR_PAIR(C_RED));
                }
                g_object_unref(jp);
                g_free(resp);
            } else {
                wattron(spop, COLOR_PAIR(C_RED));
                mvwprintw(spop, 3, 4, "No response");
                wattroff(spop, COLOR_PAIR(C_RED));
            }
            wattron(spop, COLOR_PAIR(C_DIM));
            mvwprintw(spop, sph - 2, 4, "Press any key");
            wattroff(spop, COLOR_PAIR(C_DIM));
            wrefresh(spop);
            timeout(-1); wgetch(spop); timeout(50);
            destroy_popup(spop);
        } else if (sub == '5') {
            // ── Guest Agent Channel Ensure ──
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", vn);
            char log_ok[128];
            snprintf(log_ok, sizeof(log_ok), "GUEST CHANNEL ENSURE [%s]", vn);
            send_async_rpc("vm.guest.agent.ensure_channel", p, log_ok, "GUEST CHANNEL");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── VM DISK LIVE RESIZE (Z) ─────────────────────────────────────────
    case 'Z': if (fc > 0) {
        const char *vn = g_state.vm.fleet[*sel].name;
        char dev_buf[32] = {0}, size_buf[16] = {0};
        char pmsg[128];
        snprintf(pmsg, sizeof(pmsg), "Target device for [%s] (default vda): ", vn);
        prompt_input(pmsg, dev_buf, sizeof(dev_buf), C_YELLOW);
        if (!dev_buf[0]) strncpy(dev_buf, "vda", sizeof(dev_buf) - 1);

        snprintf(pmsg, sizeof(pmsg), "New size in GB for %s: ", dev_buf);
        if (prompt_input(pmsg, size_buf, sizeof(size_buf), C_YELLOW) && size_buf[0]) {
            int new_gb = atoi(size_buf);
            if (new_gb > 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", vn);
                json_object_set_string_member(p, "target", dev_buf);
                json_object_set_int_member(p, "new_size_gb", new_gb);
                char log_ok[256];
                snprintf(log_ok, sizeof(log_ok),
                         "DISK RESIZE [%s] %s -> %dGB", vn, dev_buf, new_gb);
                send_async_rpc("vm.disk.live_resize", p, log_ok, "DISK RESIZE");
            } else {
                push_log_level("DISK RESIZE: invalid size", LOG_ERROR);
            }
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    default: break;
    }

    // 스크롤 동기화
    ScrollState *sc = &g_state.vm.scroll;
    if (*sel < sc->position) sc->position = *sel;
    if (*sel >= sc->position + sc->viewport)
        sc->position = *sel - sc->viewport + 1;
    if (sc->position < 0) sc->position = 0;
}

// =============================================================================
// VIEW: NETWORK
// =============================================================================
static void fetch_net_data(void) {
    GError *err = NULL;
    gchar  *resp = tui_send_request("network.list", NULL, &err);
    g_state.net.bridge_count = 0;
    if (!err && resp) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, resp, -1, NULL)) {
            JsonObject *ro = json_node_get_object(json_parser_get_root(p));
            if (json_object_has_member(ro, "result")) {
                JsonNode *rn = json_object_get_member(ro, "result");
                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                    JsonArray *arr = json_node_get_array(rn);
                    int n = (int)json_array_get_length(arr);
                    g_state.net.bridge_count = n < MAX_NET ? n : MAX_NET;
                    for (int i = 0; i < g_state.net.bridge_count; i++) {
                        JsonObject *bo = json_array_get_object_element(arr, i);
                        NetInfo *ni = &g_state.net.bridges[i];
                        strncpy(ni->name,    safe_str(bo,"name","?"),    sizeof(ni->name) - 1);
                        ni->name[sizeof(ni->name) - 1] = '\0';
                        strncpy(ni->state,   safe_str(bo,"state","?"),   sizeof(ni->state) - 1);
                        ni->state[sizeof(ni->state) - 1] = '\0';
                        strncpy(ni->ip_cidr, safe_str(bo,"ip_cidr",""),  sizeof(ni->ip_cidr) - 1);
                        ni->ip_cidr[sizeof(ni->ip_cidr) - 1] = '\0';
                        strncpy(ni->mode,    safe_str(bo,"mode","?"),    sizeof(ni->mode) - 1);
                        ni->mode[sizeof(ni->mode) - 1] = '\0';
                        // K1: 신규 필드
                        ni->dhcp = json_object_has_member(bo,"dhcp")
                            ? json_object_get_boolean_member(bo,"dhcp") : FALSE;
                        strncpy(ni->phys,   safe_str(bo,"phys","-"),    sizeof(ni->phys) - 1);
                        ni->phys[sizeof(ni->phys) - 1] = '\0';
                        strncpy(ni->subnet, safe_str(bo,"subnet","-"),  sizeof(ni->subnet) - 1);
                        ni->subnet[sizeof(ni->subnet) - 1] = '\0';
                        // slaves 배열 → 문자열로 압축
                        ni->slaves[0] = '\0';
                        if (json_object_has_member(bo,"slaves")) {
                            JsonNode *sn = json_object_get_member(bo,"slaves");
                            if (JSON_NODE_TYPE(sn) == JSON_NODE_ARRAY) {
                                JsonArray *sa = json_node_get_array(sn);
                                for (guint si=0; si<json_array_get_length(sa); si++) {
                                    const gchar *sv = json_array_get_string_element(sa, si);
                                    if (sv) {
                                        if (ni->slaves[0]) g_strlcat(ni->slaves, ",", 128);
                                        g_strlcat(ni->slaves, sv, 128);
                                    }
                                }
                            }
                        }
                        // VM 수 캐시 계산
                        ni->vm_count = 0;
                        for (int vi=0; vi<g_state.vm.fleet_count; vi++)
                            if (strstr(g_state.vm.fleet[vi].net_source, ni->name))
                                ni->vm_count++;
                    }
                }
            }
        }
        g_object_unref(p); g_free(resp);
    }
    if (err) g_error_free(err);
    g_state.net.needs_refresh = FALSE;
    scroll_select_clamp(&g_state.net.selected, g_state.net.bridge_count);

    /* ── OVN SDN 상태 조회 ─────────────────────────────────────────────── */
    {
        g_state.net.ovn.available = FALSE;
        g_state.net.ovn.switch_count = 0;
        g_state.net.ovn.router_count = 0;
        g_state.net.ovn.sw_count = 0;
        g_state.net.ovn.rt_count = 0;
        g_state.net.ovn.detail_for = -1;
        g_state.net.ovn.detail_port_count = 0;
        g_state.net.ovn.detail_acl_count = 0;
        g_state.net.ovn.detail_nat_count = 0;

        /* ovn.status */
        GError *ovn_err = NULL;
        gchar *ovn_resp = tui_send_request("ovn.status", NULL, &ovn_err);
        if (ovn_err) g_error_free(ovn_err);
        if (ovn_resp) {
            JsonParser *op = json_parser_new();
            if (json_parser_load_from_data(op, ovn_resp, -1, NULL)) {
                JsonObject *oo = json_node_get_object(json_parser_get_root(op));
                if (json_object_has_member(oo, "result")) {
                    JsonObject *r = json_object_get_object_member(oo, "result");
                    g_state.net.ovn.available = json_object_get_boolean_member_with_default(r, "available", FALSE);
                    g_state.net.ovn.switch_count = json_object_get_int_member_with_default(r, "switch_count", 0);
                    g_state.net.ovn.router_count = json_object_get_int_member_with_default(r, "router_count", 0);
                }
            }
            g_object_unref(op);
            g_free(ovn_resp);
        }

        /* ovn.switch.list */
        GError *sw_err = NULL;
        gchar *sw_resp = tui_send_request("ovn.switch.list", NULL, &sw_err);
        if (sw_err) g_error_free(sw_err);
        if (sw_resp) {
            JsonParser *sp = json_parser_new();
            if (json_parser_load_from_data(sp, sw_resp, -1, NULL)) {
                JsonNode *root = json_parser_get_root(sp);
                if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject *ro = json_node_get_object(root);
                    if (json_object_has_member(ro, "result")) {
                        JsonNode *rn = json_object_get_member(ro, "result");
                        if (JSON_NODE_HOLDS_ARRAY(rn)) {
                            JsonArray *sa = json_node_get_array(rn);
                            guint sn = json_array_get_length(sa);
                            for (guint si = 0; si < sn && g_state.net.ovn.sw_count < 16; si++) {
                                JsonObject *so = json_array_get_object_element(sa, si);
                                g_strlcpy(g_state.net.ovn.switches[g_state.net.ovn.sw_count],
                                          json_object_get_string_member_with_default(so, "name", "?"), 32);
                                g_state.net.ovn.sw_count++;
                            }
                        }
                    }
                }
            }
            g_object_unref(sp);
            g_free(sw_resp);
        }

        /* ovn.router.list — 미등록 RPC이므로 에러 무시 */
        GError *rt_err = NULL;
        gchar *rt_resp = tui_send_request("ovn.router.list", NULL, &rt_err);
        if (rt_err) g_error_free(rt_err);
        if (rt_resp) {
            JsonParser *rp = json_parser_new();
            if (json_parser_load_from_data(rp, rt_resp, -1, NULL)) {
                JsonNode *root = json_parser_get_root(rp);
                if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject *ro = json_node_get_object(root);
                    if (json_object_has_member(ro, "result")) {
                        JsonNode *rn = json_object_get_member(ro, "result");
                        if (JSON_NODE_HOLDS_ARRAY(rn)) {
                            JsonArray *ra = json_node_get_array(rn);
                            guint rn2 = json_array_get_length(ra);
                            for (guint ri = 0; ri < rn2 && g_state.net.ovn.rt_count < 16; ri++) {
                                JsonObject *robj = json_array_get_object_element(ra, ri);
                                g_strlcpy(g_state.net.ovn.routers[g_state.net.ovn.rt_count],
                                          json_object_get_string_member_with_default(robj, "name", "?"), 32);
                                g_state.net.ovn.rt_count++;
                            }
                        }
                    }
                }
            }
            g_object_unref(rp);
            g_free(rt_resp);
        }
    }

    /* ── Phase 4: DPDK/SR-IOV 상태 조회 (30초 간격) ────────────────────── */
    {
        gint64 now_accel = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now_accel - g_state.net.accel.last_fetch >= 30) {
            g_state.net.accel.dpdk_available = FALSE;
            g_state.net.accel.dpdk_vdev_count = 0;
            g_state.net.accel.sriov_available = FALSE;
            g_state.net.accel.sriov_pf_count = 0;

            /* dpdk.status */
            GError *de = NULL;
            gchar *dr = tui_send_request("dpdk.status", NULL, &de);
            if (!de && dr) {
                JsonParser *dp = json_parser_new();
                if (json_parser_load_from_data(dp, dr, -1, NULL)) {
                    JsonObject *dro = json_node_get_object(json_parser_get_root(dp));
                    if (json_object_has_member(dro, "result")) {
                        JsonNode *drn = json_object_get_member(dro, "result");
                        if (JSON_NODE_TYPE(drn) == JSON_NODE_OBJECT) {
                            JsonObject *d = json_node_get_object(drn);
                            g_state.net.accel.dpdk_available =
                                json_object_get_boolean_member_with_default(d, "available", FALSE);
                            g_state.net.accel.dpdk_vdev_count =
                                (gint)json_object_get_int_member_with_default(d, "vdev_count", 0);
                            const gchar *pmd = safe_str(d, "pmd_cpu_mask", "");
                            g_strlcpy(g_state.net.accel.dpdk_pmd_mask, pmd,
                                      sizeof(g_state.net.accel.dpdk_pmd_mask));
                        }
                    }
                }
                g_object_unref(dp);
            }
            g_free(dr); if (de) g_error_free(de);

            /* sriov.status */
            GError *se = NULL;
            gchar *sr = tui_send_request("sriov.status", NULL, &se);
            if (!se && sr) {
                JsonParser *sp = json_parser_new();
                if (json_parser_load_from_data(sp, sr, -1, NULL)) {
                    JsonObject *sro = json_node_get_object(json_parser_get_root(sp));
                    if (json_object_has_member(sro, "result")) {
                        JsonNode *srn = json_object_get_member(sro, "result");
                        if (JSON_NODE_TYPE(srn) == JSON_NODE_OBJECT) {
                            JsonObject *s = json_node_get_object(srn);
                            g_state.net.accel.sriov_available =
                                json_object_get_boolean_member_with_default(s, "available", FALSE);
                            if (json_object_has_member(s, "physical_functions")) {
                                JsonArray *pfs = json_object_get_array_member(s, "physical_functions");
                                gint nc = (gint)json_array_get_length(pfs);
                                g_state.net.accel.sriov_pf_count = nc < 4 ? nc : 4;
                                for (gint pi = 0; pi < g_state.net.accel.sriov_pf_count; pi++) {
                                    JsonObject *pf = json_array_get_object_element(pfs, pi);
                                    g_strlcpy(g_state.net.accel.sriov_pfs[pi],
                                              safe_str(pf, "name", "?"), 32);
                                    g_state.net.accel.sriov_vf_counts[pi] =
                                        (gint)json_object_get_int_member_with_default(pf, "current_vfs", 0);
                                }
                            }
                        }
                    }
                }
                g_object_unref(sp);
            }
            g_free(sr); if (se) g_error_free(se);

            g_state.net.accel.last_fetch = now_accel;
        }
    }
}

/* ── NET 탭: Bridge Detail 팝업 (Enter 키) ──────────────────────────────── */
/**
 * show_net_detail_popup:
 *
 * F2 NET 뷰에서 Enter 키로 호출되는 브릿지 상세 정보 팝업.
 *
 * 동작:
 *   1. 선택된 브릿지(g_state.net.selected)의 NetInfo 참조
 *   2. 24x76 팝업 윈도우 생성 (화면 크기에 맞게 축소)
 *   3. Configuration: 이름, 상태(up=GREEN, down=RED), 모드(nat/bridge 등), CIDR, MTU
 *   4. Connected VMs: 브릿지에 연결된 VM 인터페이스 목록
 *   5. DHCP Leases: dnsmasq 임대 정보 (MAC → IP 매핑)
 *   6. Esc로 닫기 → timeout(50) 복원
 *
 * 팝업 크기: 24x76 (화면에 맞게 축소)
 * 색상: 상태 up=C_GREEN, down=C_RED, 키=C_YELLOW, 제목=C_CYAN
 */
static void show_net_detail_popup(void) {
    NetInfo *sel = &g_state.net.bridges[g_state.net.selected];

    char title[80];
    snprintf(title, sizeof(title), "BRIDGE DETAIL: %s", sel->name);
    int ph = 24, pw = 76;
    int scr_r, scr_c; getmaxyx(stdscr, scr_r, scr_c);
    if (pw > scr_c - 4) pw = scr_c - 4;
    if (ph > scr_r - 4) ph = scr_r - 4;
    WINDOW *pop = create_popup(ph, pw, title);

    int ry = 2;

    /* ── Configuration ── */
    wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
    mvwprintw(pop, ry++, 2, "Configuration");
    wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);

    wattron(pop, COLOR_PAIR(C_YELLOW));
    mvwprintw(pop, ry, 4, "Name     : ");
    wattroff(pop, COLOR_PAIR(C_YELLOW));
    wattron(pop, COLOR_PAIR(C_GREEN) | A_BOLD);
    wprintw(pop, "%s", sel->name);
    wattroff(pop, COLOR_PAIR(C_GREEN) | A_BOLD);

    int st_cp = (g_strcmp0(sel->state, "up") == 0) ? C_GREEN : C_RED;
    wattron(pop, COLOR_PAIR(st_cp));
    mvwprintw(pop, ry, 35, "[%s]", g_strcmp0(sel->state, "up") == 0 ? "UP" : "DOWN");
    wattroff(pop, COLOR_PAIR(st_cp));
    ry++;

    wattron(pop, COLOR_PAIR(C_YELLOW));
    mvwprintw(pop, ry, 4, "Mode     : ");
    wattroff(pop, COLOR_PAIR(C_YELLOW));
    wattron(pop, COLOR_PAIR(C_DIM));
    wprintw(pop, "%s", sel->mode);
    wattroff(pop, COLOR_PAIR(C_DIM));
    ry++;

    wattron(pop, COLOR_PAIR(C_YELLOW));
    mvwprintw(pop, ry, 4, "Subnet   : ");
    wattroff(pop, COLOR_PAIR(C_YELLOW));
    wattron(pop, COLOR_PAIR(C_DIM));
    wprintw(pop, "%s", sel->subnet);
    wattroff(pop, COLOR_PAIR(C_DIM));
    ry++;

    wattron(pop, COLOR_PAIR(C_YELLOW));
    mvwprintw(pop, ry, 4, "Host IP  : ");
    wattroff(pop, COLOR_PAIR(C_YELLOW));
    wattron(pop, COLOR_PAIR(C_DIM));
    wprintw(pop, "%s", sel->ip_cidr[0] ? sel->ip_cidr : "-");
    wattroff(pop, COLOR_PAIR(C_DIM));
    ry++;

    wattron(pop, COLOR_PAIR(C_YELLOW));
    mvwprintw(pop, ry, 4, "Physical : ");
    wattroff(pop, COLOR_PAIR(C_YELLOW));
    wattron(pop, COLOR_PAIR(C_DIM));
    wprintw(pop, "%s", g_strcmp0(sel->phys, "-") != 0 ? sel->phys : "none");
    wattroff(pop, COLOR_PAIR(C_DIM));
    ry++;

    wattron(pop, COLOR_PAIR(C_YELLOW));
    mvwprintw(pop, ry, 4, "DHCP     : ");
    wattroff(pop, COLOR_PAIR(C_YELLOW));
    int dhcp_cp = sel->dhcp ? C_GREEN : C_RED;
    wattron(pop, COLOR_PAIR(dhcp_cp) | A_BOLD);
    wprintw(pop, "%s", sel->dhcp ? "Enabled" : "Disabled");
    wattroff(pop, COLOR_PAIR(dhcp_cp) | A_BOLD);
    ry++;

    wattron(pop, COLOR_PAIR(C_YELLOW));
    mvwprintw(pop, ry, 4, "Slaves   : ");
    wattroff(pop, COLOR_PAIR(C_YELLOW));
    wattron(pop, COLOR_PAIR(C_DIM));
    wprintw(pop, "%s", sel->slaves[0] ? sel->slaves : "none");
    wattroff(pop, COLOR_PAIR(C_DIM));
    ry += 2;

    /* ── Attached VMs ── */
    wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
    mvwprintw(pop, ry++, 2, "Attached VMs (%d)", sel->vm_count);
    wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);

    int vm_shown = 0;
    for (int vi = 0; vi < g_state.vm.fleet_count && ry < ph - 3; vi++) {
        VMMetrics *vm = &g_state.vm.fleet[vi];
        if (!strstr(vm->net_source, sel->name)) continue;
        int vm_cp = vm->is_running ? C_GREEN : C_DIM;
        wattron(pop, COLOR_PAIR(vm_cp));
        mvwprintw(pop, ry++, 4, "%s %-24s vCPU:%-2d MEM:%3d%% CPU:%3d%%",
                  vm->is_running ? "[R]" : "[S]", vm->name,
                  vm->vcpu, (int)vm->mem_percent, vm->live_cpu_pct);
        wattroff(pop, COLOR_PAIR(vm_cp));
        vm_shown++;
    }
    if (vm_shown == 0) {
        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, ry++, 4, "(no VMs attached)");
        wattroff(pop, COLOR_PAIR(C_DIM));
    }

    wattron(pop, COLOR_PAIR(C_DIM));
    mvwhline(pop, ph - 3, 1, ACS_HLINE, pw - 2);
    wattroff(pop, COLOR_PAIR(C_DIM));
    wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvwprintw(pop, ph - 2, 2, "[Esc] Close");
    wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);

    wrefresh(pop);
    timeout(-1);
    int ch; while ((ch = wgetch(pop)) != 27 && ch != 'q' && ch != 'Q' && ch != '\n' && ch != KEY_ENTER && ch != ' ') {}  /* B7-C1: Enter/Space/Q 추가 */
    timeout(50);
    delwin(pop);
    touchwin(stdscr);
}

/* ── NET 탭: Firewall Rules 팝업 (F 키) ─────────────────────────────────── */
/**
 * show_net_firewall_popup:
 *
 * F2 NET 뷰에서 'F' 키로 호출되는 방화벽 규칙 조회 팝업.
 *
 * 동작:
 *   1. 선택된 브릿지 이름으로 nft list table inet purecvisor 출력 필터링
 *   2. popen()으로 nft 명령 실행 → 브릿지 관련 규칙만 grep
 *   3. 규칙 라인을 팝업에 렌더링 (ACCEPT=C_GREEN, DROP=C_RED, MASQUERADE=C_YELLOW)
 *   4. 규칙 없으면 "No rules found" 표시
 *   5. Esc로 닫기
 *
 * 팝업 크기: 22x78
 * 참고: nftables inet purecvisor 테이블은 network_firewall.c에서 관리
 */
static void show_net_firewall_popup(void) {
    if (g_state.net.bridge_count == 0) return;
    NetInfo *sel = &g_state.net.bridges[g_state.net.selected];

    char title[80];
    snprintf(title, sizeof(title), "FIREWALL RULES: %s", sel->name);
    int ph = 22, pw = 78;
    int scr_r, scr_c; getmaxyx(stdscr, scr_r, scr_c);
    if (pw > scr_c - 4) pw = scr_c - 4;
    if (ph > scr_r - 4) ph = scr_r - 4;
    WINDOW *pop = create_popup(ph, pw, title);

    int ry = 2;

    /* nft list ruleset에서 브릿지 관련 규칙 조회 */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "nft list table inet purecvisor 2>/dev/null | grep -i '%s'",
             sel->name);
    FILE *fp = popen(cmd, "r");

    wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvwprintw(pop, ry++, 2, "nftables inet purecvisor — %s", sel->name);
    wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    ry++;

    int rule_cnt = 0;
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp) && ry < ph - 3) {
            g_strstrip(line);
            if (!line[0]) continue;
            int rcp = C_DIM;
            if (strstr(line, "accept"))     rcp = C_GREEN;
            else if (strstr(line, "drop"))  rcp = C_RED;
            else if (strstr(line, "masquerade")) rcp = C_YELLOW;
            wattron(pop, COLOR_PAIR(rcp));
            mvwprintw(pop, ry++, 4, "%.*s", pw - 6, line);
            wattroff(pop, COLOR_PAIR(rcp));
            rule_cnt++;
        }
        pclose(fp);
    }

    if (rule_cnt == 0) {
        FILE *fp2 = popen("nft list table inet purecvisor 2>/dev/null | head -20", "r");
        if (fp2) {
            char line[256];
            while (fgets(line, sizeof(line), fp2) && ry < ph - 3) {
                g_strstrip(line);
                if (!line[0]) continue;
                wattron(pop, COLOR_PAIR(C_DIM));
                mvwprintw(pop, ry++, 4, "%.*s", pw - 6, line);
                wattroff(pop, COLOR_PAIR(C_DIM));
            }
            pclose(fp2);
        }
        if (ry == 4) {
            wattron(pop, COLOR_PAIR(C_DIM));
            mvwprintw(pop, ry++, 4, "(no firewall rules or nftables not available)");
            wattroff(pop, COLOR_PAIR(C_DIM));
        }
    }

    wattron(pop, COLOR_PAIR(C_DIM));
    mvwhline(pop, ph - 3, 1, ACS_HLINE, pw - 2);
    wattroff(pop, COLOR_PAIR(C_DIM));
    wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvwprintw(pop, ph - 2, 2, "[Esc] Close");
    wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);

    wrefresh(pop);
    timeout(-1);
    int ch; while ((ch = wgetch(pop)) != 27 && ch != 'q' && ch != 'Q' && ch != '\n' && ch != KEY_ENTER && ch != ' ') {}  /* B7-C1: Enter/Space/Q 추가 */
    timeout(50);
    delwin(pop);
    touchwin(stdscr);
}

static void draw_view_net(WINDOW *win, int y0, int mid_h, int w) {
    /* 자동 갱신: 10초 간격 */
    static gint64 last_net_fetch = 0;
    gint64 now_net = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (g_state.net.needs_refresh || (now_net - last_net_fetch >= 10)) {
        fetch_net_data();
        last_net_fetch = now_net;
    }
    int nrows = g_state.net.bridge_count;

    // ── K1: VMware VNE 스타일 레이아웃 ──────────────────────────────────────
    // 상단 2/3: 브릿지 테이블  (VNE 상단 리스트)
    // 하단 1/3: 인스펙터 패널  (VNE 하단 VMnet Information)
    int tbl_h  = mid_h * 2 / 3;
    int insp_h = mid_h - tbl_h;
    int tbl_y  = y0;
    int insp_y = y0 + tbl_h;

    draw_panel(win, tbl_y,  0, tbl_h,  w, "virtual network editor", C_FLEET);
    draw_panel(win, insp_y, 0, insp_h, w, "vmnet information",      C_CYAN);

    // ── 테이블 헤더 (VNE 8컬럼) ─────────────────────────────────────────────
    // NAME(14) TYPE(10) STATE(6) EXTERNAL(14) HOST(10) DHCP(8) VMs(6) SUBNET(16)
    static const int CW[] = {14, 10, 6, 14, 10, 8, 6, 16};
    static const char *CH[] = {
        "NAME", "TYPE", "STATE", "EXTERNAL", "HOST", "DHCP", "VMs", "SUBNET"
    };
    wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD|A_UNDERLINE);
    int hx = 2;
    for (int ci = 0; ci < 8; ci++) {
        mvwprintw(win, tbl_y+1, hx, "%-*.*s", CW[ci], CW[ci], CH[ci]);
        hx += CW[ci] + 1;
    }
    wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD|A_UNDERLINE);

    // ── 브릿지 행 렌더링 ─────────────────────────────────────────────────────
    int vis = tbl_h - 4;
    g_state.net.scroll.viewport = vis;
    for (int i = 0; i < vis; i++) {
        int ri = i + g_state.net.scroll.position;
        if (ri >= nrows) break;
        bool is_sel = (ri == g_state.net.selected);
        NetInfo *nb = &g_state.net.bridges[ri];

        // 타입 → 컬러 + 레이블
        int  type_cp;
        const char *type_label;
        if      (g_strcmp0(nb->mode,"nat")      == 0) { type_cp=C_YELLOW; type_label="NAT"; }
        else if (g_strcmp0(nb->mode,"bridge")   == 0) { type_cp=C_GREEN;  type_label="Bridged"; }
        else if (g_strcmp0(nb->mode,"isolated") == 0) { type_cp=C_CYAN;   type_label="Host-only"; }
        else if (g_strcmp0(nb->mode,"routed")   == 0) { type_cp=C_MEM;    type_label="Routed"; }
        else                                           { type_cp=C_DIM;    type_label=nb->mode; }

        // External Connection (Bridged → 물리NIC, NAT → "NAT", isolated → "-")
        const char *ext_conn;
        char ext_buf[32];
        if (g_strcmp0(nb->mode,"bridge") == 0 && g_strcmp0(nb->phys,"-") != 0)
            ext_conn = nb->phys;
        else if (g_strcmp0(nb->mode,"nat") == 0)
            ext_conn = "NAT";
        else
            ext_conn = "-";

        // Host Connection: IP 있으면 "Connected", 없으면 "-"
        const char *host_conn = (nb->ip_cidr[0]) ? "Connected" : "-";

        // DHCP
        const char *dhcp_str = nb->dhcp ? "Enabled" : "-";
        int dhcp_cp = nb->dhcp ? C_GREEN : C_DIM;

        // 상태
        int st_cp = (g_strcmp0(nb->state,"up")==0) ? C_GREEN : C_RED;

        // STATE 문자열
        const char *state_str = (g_strcmp0(nb->state,"up")==0) ? "UP" : "DOWN";
        // VMs 카운트 문자열
        char vm_buf[8];
        snprintf(vm_buf, sizeof(vm_buf), "%d", nb->vm_count);

        int row_y = tbl_y + 2 + i;
        if (is_sel) {
            wattron(win, COLOR_PAIR(C_HIGHLIGHT)|A_REVERSE|A_BOLD);
            int cx2 = 2;
            // 선택 행 전체를 단색으로 덮음
            snprintf(ext_buf, sizeof(ext_buf), "%-13.13s", ext_conn);
            mvwprintw(win, row_y, cx2, "%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s",
                CW[0], CW[0], nb->name,
                CW[1], CW[1], type_label,
                CW[2], CW[2], state_str,
                CW[3], CW[3], ext_conn,
                CW[4], CW[4], host_conn,
                CW[5], CW[5], dhcp_str,
                CW[6], CW[6], vm_buf,
                CW[7], CW[7], nb->subnet);
            wattroff(win, COLOR_PAIR(C_HIGHLIGHT)|A_REVERSE|A_BOLD);
        } else {
            int cx2 = 2;
            // NAME — 상태 색상
            wattron(win, COLOR_PAIR(st_cp)|A_BOLD);
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[0], CW[0], nb->name);
            wattroff(win, COLOR_PAIR(st_cp)|A_BOLD);
            cx2 += CW[0]+1;
            // TYPE — 타입 색상
            wattron(win, COLOR_PAIR(type_cp)|A_BOLD);
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[1], CW[1], type_label);
            wattroff(win, COLOR_PAIR(type_cp)|A_BOLD);
            cx2 += CW[1]+1;
            // STATE
            wattron(win, COLOR_PAIR(st_cp)|A_BOLD);
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[2], CW[2], state_str);
            wattroff(win, COLOR_PAIR(st_cp)|A_BOLD);
            cx2 += CW[2]+1;
            // EXTERNAL
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[3], CW[3], ext_conn);
            wattroff(win, COLOR_PAIR(C_DIM));
            cx2 += CW[3]+1;
            // HOST
            int hc_cp = nb->ip_cidr[0] ? C_GREEN : C_DIM;
            wattron(win, COLOR_PAIR(hc_cp));
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[4], CW[4], host_conn);
            wattroff(win, COLOR_PAIR(hc_cp));
            cx2 += CW[4]+1;
            // DHCP
            wattron(win, COLOR_PAIR(dhcp_cp));
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[5], CW[5], dhcp_str);
            wattroff(win, COLOR_PAIR(dhcp_cp));
            cx2 += CW[5]+1;
            // VMs
            int vm_cp2 = nb->vm_count > 0 ? C_CYAN : C_DIM;
            wattron(win, COLOR_PAIR(vm_cp2));
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[6], CW[6], vm_buf);
            wattroff(win, COLOR_PAIR(vm_cp2));
            cx2 += CW[6]+1;
            // SUBNET
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, row_y, cx2, "%-*.*s", CW[7], CW[7], nb->subnet);
            wattroff(win, COLOR_PAIR(C_DIM));
        }
    }
    if (nrows == 0) {
        wattron(win, COLOR_PAIR(C_DIM)|A_ITALIC);
        mvwprintw(win, tbl_y+3, 2, "(no virtual networks — [a] Add Network)");
        wattroff(win, COLOR_PAIR(C_DIM)|A_ITALIC);
    }
    draw_scrollbar(win, tbl_y+2, vis, w-2, &g_state.net.scroll);

    // 테이블 액션 바
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, tbl_y+tbl_h-2, 2,
              "[a]Add [d]Remove [Enter]Detail [F]Firewall [G]SecGroups [r]Refresh");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);

    // ── 하단 인스펙터 패널 (VNE VMnet Information) ───────────────────────────
    if (nrows > 0) {
        NetInfo *sel = &g_state.net.bridges[g_state.net.selected];
        int ix = 2;
        int iy = insp_y + 1;
        int col2_x = w / 2;  // 우측 컬럼 시작

        // ── 좌측: 타입 라디오 + 업링크 설정 ──────────────────────────────────
        // 타입 라디오 (VMware VNE 스타일)
        static const struct { const char *mode; const char *label; const char *desc; } MODES[] = {
            {"bridge",   "Bridged",   "connect VMs directly to the external network"},
            {"nat",      "NAT",       "share host's IP address with VMs"},
            {"isolated", "Host-only", "connect VMs internally in a private network"},
            {"routed",   "Routed",    "route between VM network and host"},
        };
        for (int mi = 0; mi < 4 && iy < insp_y+insp_h-3; mi++) {
            bool active = (g_strcmp0(sel->mode, MODES[mi].mode) == 0);
            int radio_cp = active ? C_GREEN : C_DIM;
            wattron(win, COLOR_PAIR(radio_cp)|(active?A_BOLD:0));
            mvwprintw(win, iy, ix, "[%s] %-10s", active?"●":"○", MODES[mi].label);
            wattroff(win, COLOR_PAIR(radio_cp)|(active?A_BOLD:0));
            if (active) {
                wattron(win, COLOR_PAIR(C_DIM)|A_ITALIC);
                mvwprintw(win, iy, ix+14, "(%s)", MODES[mi].desc);
                wattroff(win, COLOR_PAIR(C_DIM)|A_ITALIC);
            }
            iy++;
        }

        // Bridged to (물리 NIC)
        iy++;
        wattron(win, COLOR_PAIR(C_YELLOW));
        mvwprintw(win, iy, ix, "Bridged to : ");
        wattroff(win, COLOR_PAIR(C_YELLOW));
        int phys_cp = (g_strcmp0(sel->phys,"-")==0) ? C_DIM : C_GREEN;
        wattron(win, COLOR_PAIR(phys_cp)|A_BOLD);
        mvwprintw(win, iy, ix+13, "%-24.24s", sel->phys);
        wattroff(win, COLOR_PAIR(phys_cp)|A_BOLD);
        iy++;

        // DHCP 체크박스
        wattron(win, COLOR_PAIR(C_YELLOW));
        mvwprintw(win, iy, ix, "DHCP       : ");
        wattroff(win, COLOR_PAIR(C_YELLOW));
        wattron(win, COLOR_PAIR(sel->dhcp ? C_GREEN : C_DIM)|(sel->dhcp?A_BOLD:0));
        mvwprintw(win, iy, ix+13, sel->dhcp ? "[✔] Enabled" : "[  ] Disabled");
        wattroff(win, COLOR_PAIR(sel->dhcp ? C_GREEN : C_DIM)|(sel->dhcp?A_BOLD:0));

        // ── 우측: Subnet / Host IP / VM 연결 정보 ────────────────────────────
        int ry2 = insp_y + 1;
        // Subnet Address
        wattron(win, COLOR_PAIR(C_YELLOW));
        mvwprintw(win, ry2, col2_x, "Subnet     : ");
        wattroff(win, COLOR_PAIR(C_YELLOW));
        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry2++, col2_x+13, "%-20.20s", sel->subnet);
        wattroff(win, COLOR_PAIR(C_DIM));

        // Host IP (Gateway)
        wattron(win, COLOR_PAIR(C_YELLOW));
        mvwprintw(win, ry2, col2_x, "Host IP    : ");
        wattroff(win, COLOR_PAIR(C_YELLOW));
        wattron(win, COLOR_PAIR(C_GREEN));
        mvwprintw(win, ry2++, col2_x+13, "%-20.20s", sel->ip_cidr[0] ? sel->ip_cidr : "-");
        wattroff(win, COLOR_PAIR(C_GREEN));

        // Slaves (vnet 인터페이스)
        wattron(win, COLOR_PAIR(C_YELLOW));
        mvwprintw(win, ry2, col2_x, "Interfaces : ");
        wattroff(win, COLOR_PAIR(C_YELLOW));
        wattron(win, COLOR_PAIR(C_CYAN));
        mvwprintw(win, ry2++, col2_x+13, "%-20.20s", sel->slaves[0] ? sel->slaves : "-");
        wattroff(win, COLOR_PAIR(C_CYAN));

        // VMs 연결 수
        ry2++;
        wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
        mvwprintw(win, ry2++, col2_x, "Attached VMs (%d):", sel->vm_count);
        wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
        int vm_shown = 0;
        for (int vi=0; vi<g_state.vm.fleet_count && ry2<insp_y+insp_h-2; vi++) {
            VMMetrics *vm = &g_state.vm.fleet[vi];
            if (!strstr(vm->net_source, sel->name)) continue;
            int vm_cp = vm->is_running ? C_GREEN : C_DIM;
            wattron(win, COLOR_PAIR(vm_cp));
            mvwprintw(win, ry2++, col2_x+2, "%s %-20.20s CPU:%3d%% MEM:%3d%%",
                      vm->is_running?"[R]":"[S]", vm->name,
                      vm->live_cpu_pct, (int)vm->mem_percent);
            wattroff(win, COLOR_PAIR(vm_cp));
            vm_shown++;
        }
        if (vm_shown == 0) {
            wattron(win, COLOR_PAIR(C_DIM)|A_ITALIC);
            mvwprintw(win, ry2, col2_x+2, "(no VMs attached)");
            wattroff(win, COLOR_PAIR(C_DIM)|A_ITALIC);
        }

        // ── Phase 4: DPDK/SR-IOV 상태 (인스펙터 우측 하단) ────────────
        ry2 += 2;
        if (ry2 < insp_y + insp_h - 3) {
            int accel_cp = (g_state.net.accel.dpdk_available || g_state.net.accel.sriov_available)
                           ? C_GREEN : C_DIM;
            wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
            mvwprintw(win, ry2++, col2_x, "Acceleration:");
            wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);

            wattron(win, COLOR_PAIR(accel_cp));
            mvwprintw(win, ry2++, col2_x+2, "DPDK  : %s%s",
                      g_state.net.accel.dpdk_available ? "ON" : "OFF",
                      g_state.net.accel.dpdk_vdev_count > 0 ?
                          g_strdup_printf(" (%d dev)", g_state.net.accel.dpdk_vdev_count) : "");
            mvwprintw(win, ry2++, col2_x+2, "SR-IOV: %s%s",
                      g_state.net.accel.sriov_available ? "ON" : "OFF",
                      g_state.net.accel.sriov_pf_count > 0 ?
                          g_strdup_printf(" (%d PF)", g_state.net.accel.sriov_pf_count) : "");
            wattroff(win, COLOR_PAIR(accel_cp));
        }

        // 인스펙터 액션 바
        wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
        mvwprintw(win, insp_y+insp_h-2, 2,
                  "[M]ChgType [P]Uplink [D]DHCP [i]Leases [F]FW [K]DPDK [V]SR-IOV");
        wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    }

}


// K1: 물리 NIC 목록 조회 (업링크 선택 프롬프트용)
static void _net_list_phys_nics(char *buf, int bufsz) {
    buf[0] = '\0';
    FILE *fp = popen(
        "ip -o link show | grep -vE 'lo|bridge|vnet|tap|veth|lxc' "
        "| awk -F': ' '{print $2}' | cut -d@ -f1 2>/dev/null", "r");
    if (!fp) return;
    char line[64];
    while (fgets(line, sizeof(line), fp)) {
        g_strstrip(line);
        if (!line[0]) continue;
        if (buf[0]) g_strlcat(buf, " / ", bufsz);
        g_strlcat(buf, line, bufsz);
    }
    pclose(fp);
}

static void handle_key_net(int ch) {
    int *sel = &g_state.net.selected;
    int  cnt = g_state.net.bridge_count;

    switch (ch) {
    case KEY_UP:
        if (*sel > 0) { (*sel)--; g_state.dirty.all=TRUE; }
        break;
    case KEY_DOWN:
        if (*sel < cnt-1) { (*sel)++; g_state.dirty.all=TRUE; }
        break;

    case 'r':
        fetch_net_data();
        push_log_level("NET: refreshed", LOG_SYS);
        g_state.dirty.all = TRUE;
        break;

    // [Enter] Bridge Detail 팝업
    case '\n':
    case KEY_ENTER:
        if (cnt > 0) { show_net_detail_popup(); g_state.dirty.all = TRUE; }
        break;

    // [F] Firewall Rules 팝업
    case 'F':
        if (cnt > 0) { show_net_firewall_popup(); g_state.dirty.all = TRUE; }
        break;

    // [a] Add Network — VMware "Add Network" 버튼
    case 'a': {
        char br[64]={0}, cidr[48]={0}, mode[16]={0}, phys[32]={0};
        if (!prompt_input("Network name (e.g. pcvbr1): ", br, sizeof(br), C_YELLOW) || !br[0])
            break;
        char type_sel[16]={0};
        prompt_input("Type [nat/bridge/isolated/routed] (default nat): ",
                     type_sel, sizeof(type_sel), C_YELLOW);
        if (!type_sel[0]) strncpy(type_sel, "nat", sizeof(type_sel)-1);
        strncpy(mode, type_sel, sizeof(mode)-1);
        if (g_strcmp0(mode, "bridge") == 0) {
            char nics[256]={0}; _net_list_phys_nics(nics, sizeof(nics));
            char pmsg[320];
            snprintf(pmsg, sizeof(pmsg), "Bridged to [%s]: ", nics);
            prompt_input(pmsg, phys, sizeof(phys), C_YELLOW);
        } else {
            prompt_input("Subnet CIDR (e.g. 10.20.0.1/24): ", cidr, sizeof(cidr), C_YELLOW);
            if (!cidr[0]) strncpy(cidr, "10.10.0.1/24", sizeof(cidr)-1);
        }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "bridge_name", br);
        json_object_set_string_member(p, "mode",        mode);
        if (cidr[0])  json_object_set_string_member(p, "cidr",        cidr);
        if (phys[0])  json_object_set_string_member(p, "physical_if", phys);
        char log_ok[256];
        snprintf(log_ok, sizeof(log_ok), "NET ADD [%s] type=%s", br, mode);
        push_log_level(log_ok, LOG_SYS);
        send_async_rpc("network.create", p, log_ok, "NET ADD");
        g_state.net.needs_refresh = TRUE;
        g_state.dirty.all = TRUE;
        break;
    }

    // [d] Remove Network — VMware "Remove Network" 버튼
    case 'd': if (cnt > 0) {
        const char *bn = g_state.net.bridges[*sel].name;
        char warn[96]; snprintf(warn, sizeof(warn), "REMOVE network '%s'?", bn);
        if (confirm_dialog(warn, bn)) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "bridge_name", bn);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "NET REMOVE [%s]", bn);
            push_log_level(log_ok, LOG_WARN);
            send_async_rpc("network.delete", p, log_ok, "NET REMOVE");
            g_state.net.needs_refresh = TRUE;
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // [M] Change Type — 인스펙터 라디오 버튼 대응
    case 'M': if (cnt > 0) {
        char new_mode[16] = {0};
        const char *bn = g_state.net.bridges[*sel].name;
        char pmsg[256];
        snprintf(pmsg, sizeof(pmsg), "[%s] New type [nat/bridge/isolated/routed]: ", bn);
        if (prompt_input(pmsg, new_mode, sizeof(new_mode), C_YELLOW) && new_mode[0]) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "bridge_name", bn);
            json_object_set_string_member(p, "mode",        new_mode);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "NET TYPE [%s] → %s", bn, new_mode);
            push_log_level(log_ok, LOG_SYS);
            send_async_rpc("network.mode_set", p, log_ok, "TYPE SET");
            g_state.net.needs_refresh = TRUE;
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // [P] Set Physical Uplink — "Bridged to" 드롭다운 대응
    case 'P': if (cnt > 0) {
        char nics[256]={0}; _net_list_phys_nics(nics, sizeof(nics));
        char pmsg[320];
        snprintf(pmsg, sizeof(pmsg), "Bind uplink NIC [%s]: ", nics);
        char phys[32]={0};
        if (prompt_input(pmsg, phys, sizeof(phys), C_YELLOW) && phys[0]) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "bridge_name", g_state.net.bridges[*sel].name);
            json_object_set_string_member(p, "physical_if", phys);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "NET UPLINK [%s] → %s",
                     g_state.net.bridges[*sel].name, phys);
            push_log_level(log_ok, LOG_SYS);
            send_async_rpc("network.bind_phys", p, log_ok, "UPLINK");
            g_state.net.needs_refresh = TRUE;
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // [D] Toggle DHCP — "Use local DHCP service" 체크박스 대응
    case 'D': if (cnt > 0) {
        NetInfo *nb = &g_state.net.bridges[*sel];
        char warn[100];
        snprintf(warn, sizeof(warn), "%s DHCP for '%s'?",
                 nb->dhcp ? "DISABLE" : "ENABLE", nb->name);
        if (confirm_dialog(warn, nb->name)) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "bridge_name", nb->name);
            json_object_set_boolean_member(p, "enable", !nb->dhcp);
            char log_ok[256];
            snprintf(log_ok, sizeof(log_ok), "NET DHCP [%s] → %s",
                     nb->name, nb->dhcp ? "OFF" : "ON");
            push_log_level(log_ok, nb->dhcp ? LOG_WARN : LOG_SUCCESS);
            send_async_rpc("network.dhcp_toggle", p, log_ok, "DHCP");
            g_state.net.needs_refresh = TRUE;
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // [i] DHCP Lease 정보 팝업 — "DHCP Settings" 대응
    case 'i': if (cnt > 0) {
        NetInfo *nb = &g_state.net.bridges[*sel];
        char lease_path[128];
        snprintf(lease_path, sizeof(lease_path),
                 "/var/run/purecvisor/network/dnsmasq-%s.leases", nb->name);
        int ph = 16, pw = 72;
        WINDOW *pop = create_popup(ph, pw, "DHCP LEASES");
        int ry2 = 1;
        wattron(pop, COLOR_PAIR(C_YELLOW)|A_BOLD|A_UNDERLINE);
        mvwprintw(pop, ry2++, 2, "%-18s %-18s %-22s", "MAC", "IP", "HOSTNAME");
        wattroff(pop, COLOR_PAIR(C_YELLOW)|A_BOLD|A_UNDERLINE);
        FILE *fp = fopen(lease_path, "r");
        int lease_cnt = 0;
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp) && ry2 < ph-3) {
                char ts[16]={0}, mac[20]={0}, ip[20]={0}, host[32]={0};
                if (sscanf(line, "%15s %19s %19s %31s", ts, mac, ip, host) >= 3) {
                    wattron(pop, COLOR_PAIR(C_GREEN));
                    mvwprintw(pop, ry2++, 2, "%-18.18s %-18.18s %-22.22s", mac, ip, host);
                    wattroff(pop, COLOR_PAIR(C_GREEN));
                    lease_cnt++;
                }
            }
            fclose(fp);
        }
        if (lease_cnt == 0) {
            wattron(pop, COLOR_PAIR(C_DIM)|A_ITALIC);
            mvwprintw(pop, ry2, 2, nb->dhcp
                ? "(no active leases)"
                : "(DHCP disabled — press [D] to enable)");
            wattroff(pop, COLOR_PAIR(C_DIM)|A_ITALIC);
        }
        wattron(pop, COLOR_PAIR(C_CYAN)|A_BOLD);
        mvwprintw(pop, ph-2, 2, "Press any key to close");
        wattroff(pop, COLOR_PAIR(C_CYAN)|A_BOLD);
        wrefresh(pop);
        timeout(-1); wgetch(pop); timeout(50);
        delwin(pop); touchwin(stdscr);
        g_state.dirty.all = TRUE;
        break;
    } break;

    // [G] Security Group List ────────────────────────────────────────────────
    case 'G': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("security_group.list", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = json_array_get_length(arr);
                        push_log_level("=== Security Groups ===", LOG_SYS);
                        for (guint i = 0; i < len; i++) {
                            JsonObject *sg = json_array_get_object_element(arr, i);
                            const gchar *sgname = safe_str(sg, "name", "?");
                            gint64 rules = safe_int(sg, "rule_count");
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp), "  [%s] rules=%ld", sgname, (long)rules);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no security groups)");
                    }
                } else if (json_object_has_member(ro, "error")) {
                    JsonObject *eo = json_object_get_object_member(ro, "error");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "SecGroup: %s", safe_str(eo, "message", "error"));
                    push_log_level(tmp, LOG_WARN);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("SECGROUP ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    // [K] DPDK Status ────────────────────────────────────────────────────────
    case 'K': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("dpdk.status", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    const gchar *avail = safe_str(res, "available", "false");
                    gint64 devs = safe_int(res, "vdev_count");
                    gint64 hpg  = safe_int(res, "hugepage_total_mb");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp),
                             "DPDK: available=%s devs=%ld hugepages=%ldMB",
                             avail, (long)devs, (long)hpg);
                    push_log_level(tmp, LOG_SYS);
                } else if (json_object_has_member(ro, "error")) {
                    JsonObject *eo = json_object_get_object_member(ro, "error");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "DPDK: %s", safe_str(eo, "message", "error"));
                    push_log_level(tmp, LOG_WARN);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("DPDK ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    // [V] SR-IOV Status ──────────────────────────────────────────────────────
    case 'V': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("sriov.status", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    const gchar *avail = safe_str(res, "available", "false");
                    gint64 pfs = safe_int(res, "pf_count");
                    gint64 vfs = safe_int(res, "total_vfs");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp),
                             "SR-IOV: available=%s PFs=%ld VFs=%ld",
                             avail, (long)pfs, (long)vfs);
                    push_log_level(tmp, LOG_SYS);
                } else if (json_object_has_member(ro, "error")) {
                    JsonObject *eo = json_object_get_object_member(ro, "error");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "SR-IOV: %s", safe_str(eo, "message", "error"));
                    push_log_level(tmp, LOG_WARN);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("SRIOV ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    default: break;
    }

    // 스크롤 클램프
    ScrollState *sc = &g_state.net.scroll;
    if (*sel < sc->position) sc->position = *sel;
    if (sc->viewport > 0 && *sel >= sc->position + sc->viewport)
        sc->position = *sel - sc->viewport + 1;
}

// =============================================================================
// VIEW: OVN (F7) — detail fetch + draw + key handler
//
// OVN SDN 리소스(논리 스위치/라우터)를 시각화하는 F7 탭.
// 좌측에 LS/LR 목록, 우측에 선택된 리소스의 포트/ACL/NAT 인스펙터를 표시한다.
//
// 데이터 흐름:
//   fetch_net_data()로 10초 간격 자동 갱신 → OVN 데이터도 함께 fetch
//   fetch_ovn_detail()로 선택 변경 시 상세 정보 캐시
//   draw_view_ovn()으로 화면 렌더링
//   handle_key_ovn()으로 키 이벤트 처리
//
// 키 바인딩:
//   UP/DOWN: 리소스 선택     S: 스위치 생성      R: 라우터 생성
//   Enter: 상세 팝업         A: ACL 매니저        I: NAT/DHCP 정보
//   X: 리소스 삭제           T: 테넌트 생성       r: 수동 갱신
// =============================================================================

/**
 * fetch_ovn_detail:
 * @selected_idx: 선택된 리소스 인덱스 (0..sw_count-1=스위치, sw_count..=라우터)
 *
 * 선택된 OVN 리소스의 상세 정보를 RPC로 조회하여 g_state에 캐시.
 *
 * 캐시 메커니즘:
 *   - detail_for 필드에 마지막 조회한 인덱스를 저장
 *   - 동일 인덱스 재호출 시 즉시 반환 (캐시 히트)
 *   - 선택 변경(handle_key_ovn)에서 detail_for=-1로 초기화하여 다음 호출에서 재조회
 *
 * 조회 데이터:
 *   - 스위치(LS): 포트 목록 + ACL 목록 (ovn.switch.detail RPC)
 *   - 라우터(LR): 포트 목록(MAC/네트워크 포함) + NAT 목록 (ovn.router.detail RPC)
 *
 * 제한: 포트 최대 8개, ACL 최대 8개, NAT 최대 4개 (TUI 표시 공간 제약)
 */
static void fetch_ovn_detail(int selected_idx) {
    if (selected_idx == g_state.net.ovn.detail_for) return; /* 캐시 히트 */
    g_state.net.ovn.detail_for = selected_idx;
    g_state.net.ovn.detail_port_count = 0;
    g_state.net.ovn.detail_acl_count = 0;
    g_state.net.ovn.detail_nat_count = 0;

    if (selected_idx < 0) return;

    gboolean is_switch = (selected_idx < g_state.net.ovn.sw_count);
    const char *name = is_switch
        ? g_state.net.ovn.switches[selected_idx]
        : g_state.net.ovn.routers[selected_idx - g_state.net.ovn.sw_count];

    const char *method = is_switch ? "ovn.switch.detail" : "ovn.router.detail";
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);
    GError *err = NULL;
    gchar *resp = tui_send_request(method, params, &err);
    if (err) g_error_free(err);
    if (!resp) return;

    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, resp, -1, NULL)) {
        g_object_unref(p); g_free(resp);
        return;
    }
    JsonObject *ro = json_node_get_object(json_parser_get_root(p));
    if (!json_object_has_member(ro, "result")) {
        g_object_unref(p); g_free(resp);
        return;
    }
    JsonObject *result = json_object_get_object_member(ro, "result");

    /* 포트 목록 */
    if (json_object_has_member(result, "ports")) {
        JsonArray *ports = json_object_get_array_member(result, "ports");
        guint n = json_array_get_length(ports);
        for (guint i = 0; i < n && g_state.net.ovn.detail_port_count < 8; i++) {
            if (is_switch) {
                const gchar *pname = json_array_get_string_element(ports, i);
                if (pname) g_strlcpy(g_state.net.ovn.detail_ports[g_state.net.ovn.detail_port_count++], pname, 64);
            } else {
                JsonObject *pobj = json_array_get_object_element(ports, i);
                const gchar *pname = json_object_get_string_member_with_default(pobj, "name", "");
                const gchar *mac = json_object_get_string_member_with_default(pobj, "mac", "");
                const gchar *net = json_object_get_string_member_with_default(pobj, "networks", "");
                gint idx = g_state.net.ovn.detail_port_count;
                g_strlcpy(g_state.net.ovn.detail_ports[idx], pname, 64);
                g_strlcpy(g_state.net.ovn.detail_port_mac[idx], mac, 24);
                g_strlcpy(g_state.net.ovn.detail_port_net[idx], net, 32);
                g_state.net.ovn.detail_port_count++;
            }
        }
    }

    if (is_switch) {
        /* ACL 목록 */
        if (json_object_has_member(result, "acls")) {
            JsonArray *acls = json_object_get_array_member(result, "acls");
            guint n = json_array_get_length(acls);
            for (guint i = 0; i < n && g_state.net.ovn.detail_acl_count < 8; i++) {
                const gchar *line = json_array_get_string_element(acls, i);
                if (line && *line)
                    g_strlcpy(g_state.net.ovn.detail_acls[g_state.net.ovn.detail_acl_count++], line, 80);
            }
        }
    } else {
        /* NAT 목록 */
        if (json_object_has_member(result, "nats")) {
            JsonArray *nats = json_object_get_array_member(result, "nats");
            guint n = json_array_get_length(nats);
            /* 첫 줄은 헤더일 수 있으므로 스킵 */
            for (guint i = (n > 1 ? 1 : 0); i < n && g_state.net.ovn.detail_nat_count < 4; i++) {
                const gchar *line = json_array_get_string_element(nats, i);
                if (line && *line)
                    g_strlcpy(g_state.net.ovn.detail_nats[g_state.net.ovn.detail_nat_count++], line, 80);
            }
        }
    }

    g_object_unref(p);
    g_free(resp);
}

/**
 * draw_view_ovn:
 * @win: 메인 윈도우 (stdscr)
 * @y0: 렌더링 시작 Y 좌표 (탭 바 아래)
 * @mid_h: 사용 가능한 높이
 * @w: 터미널 폭
 *
 * F7 OVN 뷰 메인 렌더링 함수.
 *
 * 레이아웃 (좌우 분할):
 *   좌측 (w/2): OVN 리소스 목록
 *     - "LOGICAL SWITCHES" 섹션: 스위치 이름 나열
 *     - "LOGICAL ROUTERS" 섹션: 라우터 이름 나열
 *     - 선택된 항목: C_HIGHLIGHT + A_REVERSE
 *     - OVN 미설치 시: "OVN not available" 경고
 *
 *   우측 (w/2): 인스펙터 패널 (선택된 리소스 상세)
 *     - 스위치: PORTS 목록 + ACL 규칙 (allow=C_GREEN, drop=C_RED)
 *     - 라우터: PORTS(MAC/네트워크) + NAT 규칙
 *     - fetch_ovn_detail()로 캐시된 데이터 사용
 *
 * 10초 간격 자동 갱신 (fetch_net_data와 타이머 공유).
 */
static void draw_view_ovn(WINDOW *win, int y0, int mid_h, int w) {
    /* 자동 갱신: 10초 간격 (net 데이터 fetch와 공유) */
    static gint64 last_ovn_fetch = 0;
    gint64 now_ovn = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (g_state.net.needs_refresh || (now_ovn - last_ovn_fetch >= 10)) {
        fetch_net_data();  /* OVN 데이터도 여기서 fetch */
        last_ovn_fetch = now_ovn;
    }

    int tbl_h  = mid_h * 2 / 3;
    int insp_h = mid_h - tbl_h;
    int tbl_y  = y0;
    int insp_y = y0 + tbl_h;

    /* ── 상단: OVN 리소스 테이블 ── */
    draw_panel(win, tbl_y, 0, tbl_h, w, "OVN LOGICAL NETWORK", C_CYAN);

    /* 상태 서머리 바 */
    int hy = tbl_y + 1;
    {
        int scp = g_state.net.ovn.available ? C_GREEN : C_RED;
        wattron(win, COLOR_PAIR(scp) | A_BOLD);
        mvwprintw(win, hy, 2, " %s ", g_state.net.ovn.available ? "ACTIVE" : "OFFLINE");
        wattroff(win, COLOR_PAIR(scp) | A_BOLD);
        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, hy, 12, "Encap:Geneve  LS:%d  LR:%d  Total:%d",
                  g_state.net.ovn.sw_count, g_state.net.ovn.rt_count,
                  g_state.net.ovn.sw_count + g_state.net.ovn.rt_count);
        wattroff(win, COLOR_PAIR(C_DIM));
    }
    hy++;

    /* 테이블 헤더 */
    wattron(win, COLOR_PAIR(C_YELLOW) | A_BOLD | A_UNDERLINE);
    mvwprintw(win, hy, 2, "  %-4s  %-24s  %-10s  %-8s  %-16s", "TYPE", "NAME", "STATUS", "PORTS", "DETAIL");
    wattroff(win, COLOR_PAIR(C_YELLOW) | A_BOLD | A_UNDERLINE);
    hy++;

    /* OVN 미설치 */
    if (!g_state.net.ovn.available) {
        wattron(win, COLOR_PAIR(C_RED));
        mvwprintw(win, hy + 1, 4, "OVN not installed");
        mvwprintw(win, hy + 2, 4, "Install: sudo apt install ovn-central ovn-host");
        wattroff(win, COLOR_PAIR(C_RED));
    } else {
        int vis = tbl_h - 6;
        int row = 0;
        int total_items = g_state.net.ovn.sw_count + g_state.net.ovn.rt_count;

        /* 스위치 목록 */
        for (int i = 0; i < g_state.net.ovn.sw_count && row < vis; i++, row++) {
            gboolean is_sel = (row == g_state.net.ovn_selected);
            if (is_sel) wattron(win, A_REVERSE);
            int cp = C_FLEET;
            /* 테넌트 스위치는 다른 색상 */
            if (g_str_has_prefix(g_state.net.ovn.switches[i], "tenant-")) cp = C_CYAN;
            wattron(win, COLOR_PAIR(cp));
            mvwprintw(win, hy + row, 2, "  %-4s  %-24s  ", "LS", g_state.net.ovn.switches[i]);
            wattroff(win, COLOR_PAIR(cp));
            wattron(win, COLOR_PAIR(C_GREEN));
            mvwprintw(win, hy + row, 36, "%-10s", "active");
            wattroff(win, COLOR_PAIR(C_GREEN));
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, hy + row, 48, "%-8s  %-16s", "-",
                      g_str_has_prefix(g_state.net.ovn.switches[i], "tenant-") ? "multi-tenant" : "logical-sw");
            wattroff(win, COLOR_PAIR(C_DIM));
            if (is_sel) wattroff(win, A_REVERSE);
        }

        /* 라우터 목록 */
        for (int i = 0; i < g_state.net.ovn.rt_count && row < vis; i++, row++) {
            gboolean is_sel = (row == g_state.net.ovn_selected);
            if (is_sel) wattron(win, A_REVERSE);
            wattron(win, COLOR_PAIR(C_MEM));
            mvwprintw(win, hy + row, 2, "  %-4s  %-24s  ", "LR", g_state.net.ovn.routers[i]);
            wattroff(win, COLOR_PAIR(C_MEM));
            wattron(win, COLOR_PAIR(C_GREEN));
            mvwprintw(win, hy + row, 36, "%-10s", "active");
            wattroff(win, COLOR_PAIR(C_GREEN));
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, hy + row, 48, "%-8s  %-16s", "-", "gateway-rtr");
            wattroff(win, COLOR_PAIR(C_DIM));
            if (is_sel) wattroff(win, A_REVERSE);
        }

        if (total_items == 0) {
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, hy + 1, 4, "No OVN resources. Press [S] to create a logical switch.");
            wattroff(win, COLOR_PAIR(C_DIM));
        }
    }

    /* ACTIONS 바 */
    wattron(win, COLOR_PAIR(C_DIM));
    mvwprintw(win, tbl_y + tbl_h - 2, 2,
              "[S]Switch [R]Router [X]Del [T]Tenant [N]NAT [Enter]Detail [A]ACL [I]Info [G]SecGroups [r]Refresh");
    wattroff(win, COLOR_PAIR(C_DIM));

    /* ── 하단: OVN 인스펙터 (세분화) ── */
    draw_panel(win, insp_y, 0, insp_h, w, "OVN RESOURCE DETAIL", C_CYAN);
    int iy = insp_y + 1;

    /* 선택된 리소스 상세 */
    int total = g_state.net.ovn.sw_count + g_state.net.ovn.rt_count;
    int oi = g_state.net.ovn_selected;
    if (!g_state.net.ovn.available) {
        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, iy, 2, "OVN unavailable — no detail to display");
        wattroff(win, COLOR_PAIR(C_DIM));
    } else if (total == 0) {
        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, iy, 2, "No resources selected");
        wattroff(win, COLOR_PAIR(C_DIM));
    } else if (oi >= 0 && oi < total) {
        fetch_ovn_detail(oi);

        gboolean is_switch = (oi < g_state.net.ovn.sw_count);
        const char *res_name = is_switch
            ? g_state.net.ovn.switches[oi]
            : g_state.net.ovn.routers[oi - g_state.net.ovn.sw_count];

        /* 리소스 타입 아이콘 + 이름 */
        int type_cp = is_switch ? C_FLEET : C_MEM;
        wattron(win, COLOR_PAIR(type_cp) | A_BOLD);
        mvwprintw(win, iy, 2, "%s", is_switch ? "[LS]" : "[LR]");
        wattroff(win, COLOR_PAIR(type_cp) | A_BOLD);
        wattron(win, COLOR_PAIR(C_CYAN) | A_BOLD);
        mvwprintw(win, iy, 7, "%s", res_name);
        wattroff(win, COLOR_PAIR(C_CYAN) | A_BOLD);
        /* 상태 뱃지 */
        wattron(win, COLOR_PAIR(C_GREEN));
        mvwprintw(win, iy, 7 + (int)strlen(res_name) + 2, "ACTIVE");
        wattroff(win, COLOR_PAIR(C_GREEN));
        iy++;

        int left_w = w / 2 - 1;

        if (is_switch) {
            /* ── 좌측: 포트 + 연결 ── */
            wattron(win, COLOR_PAIR(C_FLEET) | A_BOLD);
            mvwprintw(win, iy, 2, "Ports (%d)", g_state.net.ovn.detail_port_count);
            wattroff(win, COLOR_PAIR(C_FLEET) | A_BOLD);
            for (int pi = 0; pi < g_state.net.ovn.detail_port_count && iy + pi + 1 < insp_y + insp_h - 1; pi++) {
                wattron(win, COLOR_PAIR(C_DIM));
                mvwprintw(win, iy + pi + 1, 4, "|- %s", g_state.net.ovn.detail_ports[pi]);
                wattroff(win, COLOR_PAIR(C_DIM));
            }

            /* ── 우측: ACL 보안 규칙 ── */
            int rx = left_w + 2;
            int ry = iy;
            wattron(win, COLOR_PAIR(C_YELLOW) | A_BOLD);
            mvwprintw(win, ry, rx, "ACL Rules (%d)", g_state.net.ovn.detail_acl_count);
            wattroff(win, COLOR_PAIR(C_YELLOW) | A_BOLD);
            ry++;
            if (g_state.net.ovn.detail_acl_count > 0) {
                for (int a = 0; a < g_state.net.ovn.detail_acl_count && ry < insp_y + insp_h - 1; a++) {
                    /* ACL 액션별 컬러: allow=녹색, drop=적색 */
                    int acl_cp = C_DIM;
                    if (strstr(g_state.net.ovn.detail_acls[a], "allow")) acl_cp = C_GREEN;
                    else if (strstr(g_state.net.ovn.detail_acls[a], "drop")) acl_cp = C_RED;
                    wattron(win, COLOR_PAIR(acl_cp));
                    mvwprintw(win, ry, rx, "  %.*s", w - rx - 2, g_state.net.ovn.detail_acls[a]);
                    wattroff(win, COLOR_PAIR(acl_cp));
                    ry++;
                }
            } else {
                wattron(win, COLOR_PAIR(C_DIM));
                mvwprintw(win, ry, rx, "  (no rules)");
                wattroff(win, COLOR_PAIR(C_DIM));
            }
        } else {
            /* ── 좌측: 라우터 포트 + MAC/네트워크 ── */
            wattron(win, COLOR_PAIR(C_FLEET) | A_BOLD);
            mvwprintw(win, iy, 2, "Interfaces (%d)", g_state.net.ovn.detail_port_count);
            wattroff(win, COLOR_PAIR(C_FLEET) | A_BOLD);
            for (int pi = 0; pi < g_state.net.ovn.detail_port_count && iy + pi + 1 < insp_y + insp_h - 1; pi++) {
                wattron(win, COLOR_PAIR(C_CYAN));
                mvwprintw(win, iy + pi + 1, 4, "|- %-16s", g_state.net.ovn.detail_ports[pi]);
                wattroff(win, COLOR_PAIR(C_CYAN));
                wattron(win, COLOR_PAIR(C_DIM));
                mvwprintw(win, iy + pi + 1, 24, "%s  %s",
                          g_state.net.ovn.detail_port_mac[pi],
                          g_state.net.ovn.detail_port_net[pi]);
                wattroff(win, COLOR_PAIR(C_DIM));
            }

            /* ── 우측: NAT 규칙 ── */
            int rx = left_w + 2;
            int ry = iy;
            wattron(win, COLOR_PAIR(C_YELLOW) | A_BOLD);
            mvwprintw(win, ry, rx, "NAT Rules (%d)", g_state.net.ovn.detail_nat_count);
            wattroff(win, COLOR_PAIR(C_YELLOW) | A_BOLD);
            ry++;
            if (g_state.net.ovn.detail_nat_count > 0) {
                for (int n = 0; n < g_state.net.ovn.detail_nat_count && ry < insp_y + insp_h - 1; n++) {
                    /* lr-nat-list 출력 파싱: "snat  EXT_IP  LOG_IP" (공백 구분) */
                    char ntype[16]={0}, ext[20]={0}, lip[24]={0};
                    if (sscanf(g_state.net.ovn.detail_nats[n], "%15s %*s %19s %*s %23s",
                               ntype, ext, lip) >= 3) {
                        wattron(win, COLOR_PAIR(C_GREEN));
                        mvwprintw(win, ry, rx, "  %-5s ", ntype);
                        wattroff(win, COLOR_PAIR(C_GREEN));
                        wattron(win, COLOR_PAIR(C_DIM));
                        wprintw(win, "%s -> %s", ext, lip);
                        wattroff(win, COLOR_PAIR(C_DIM));
                    } else {
                        wattron(win, COLOR_PAIR(C_DIM));
                        mvwprintw(win, ry, rx, "  %.*s", w - rx - 2, g_state.net.ovn.detail_nats[n]);
                        wattroff(win, COLOR_PAIR(C_DIM));
                    }
                    ry++;
                }
            } else {
                wattron(win, COLOR_PAIR(C_DIM));
                mvwprintw(win, ry, rx, "  (no rules)");
                wattroff(win, COLOR_PAIR(C_DIM));
            }
        }
    }
    (void)iy; /* suppress unused warning */
}

/* ── OVN Detail 팝업: Enter 키 → 선택 리소스 상세 ── */
/**
 * show_ovn_detail_popup:
 *
 * F7 OVN 뷰에서 Enter 키로 호출되는 리소스 상세 팝업.
 *
 * 동작:
 *   1. 선택된 리소스가 LS인지 LR인지 판별 (oi < sw_count → LS)
 *   2. ovn.switch.detail 또는 ovn.router.detail RPC 호출
 *   3. JSON 파싱 → 포트/ACL/NAT 렌더링
 *   4. Esc로 닫기 (timeout(-1) 블로킹 → timeout(50) 복원)
 *
 * 팝업 크기: 22x74 (화면에 맞게 축소)
 * 색상: ACL allow=C_GREEN, drop=C_RED, NAT=C_GREEN/C_DIM
 *       스위치 제목=C_CYAN, 라우터 제목=C_YELLOW
 */
static void show_ovn_detail_popup(void) {
    int oi = g_state.net.ovn_selected;
    int total = g_state.net.ovn.sw_count + g_state.net.ovn.rt_count;
    if (oi < 0 || oi >= total) return;

    gboolean is_switch = (oi < g_state.net.ovn.sw_count);
    const char *name = is_switch
        ? g_state.net.ovn.switches[oi]
        : g_state.net.ovn.routers[oi - g_state.net.ovn.sw_count];

    /* RPC 호출 */
    const char *method = is_switch ? "ovn.switch.detail" : "ovn.router.detail";
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);
    GError *err = NULL;
    gchar *resp = tui_send_request(method, params, &err);
    if (err) { g_error_free(err); if (resp) g_free(resp); return; }
    if (!resp) return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    JsonObject *result = NULL;
    if (json_object_has_member(root, "result"))
        result = json_object_get_object_member(root, "result");
    if (!result) { g_object_unref(parser); g_free(resp); return; }

    char title[80];
    snprintf(title, sizeof(title), "%s DETAIL: %s",
             is_switch ? "LOGICAL SWITCH" : "LOGICAL ROUTER", name);
    int ph = 22, pw = 74;
    int scr_r, scr_c; getmaxyx(stdscr, scr_r, scr_c);
    if (pw > scr_c - 4) pw = scr_c - 4;
    if (ph > scr_r - 4) ph = scr_r - 4;
    WINDOW *pop = create_popup(ph, pw, title);

    int ry = 2;

    wattron(pop, COLOR_PAIR(is_switch ? C_FLEET : C_MEM) | A_BOLD);
    mvwprintw(pop, ry, 2, "%s", is_switch ? "[Logical Switch]" : "[Logical Router]");
    wattroff(pop, COLOR_PAIR(is_switch ? C_FLEET : C_MEM) | A_BOLD);
    wattron(pop, COLOR_PAIR(C_GREEN));
    mvwprintw(pop, ry, 22, "ACTIVE");
    wattroff(pop, COLOR_PAIR(C_GREEN));
    ry += 2;

    if (is_switch) {
        JsonArray *ports = json_object_has_member(result, "ports")
            ? json_object_get_array_member(result, "ports") : NULL;
        gint port_count = ports ? (gint)json_array_get_length(ports) : 0;
        wattron(pop, COLOR_PAIR(C_FLEET) | A_BOLD);
        mvwprintw(pop, ry, 2, "Ports (%d)", port_count);
        wattroff(pop, COLOR_PAIR(C_FLEET) | A_BOLD);
        ry++;
        for (gint i = 0; i < port_count && ry < ph - 4; i++) {
            wattron(pop, COLOR_PAIR(C_DIM));
            mvwprintw(pop, ry++, 4, "|- %s", json_array_get_string_element(ports, i));
            wattroff(pop, COLOR_PAIR(C_DIM));
        }
        ry++;

        JsonArray *acls = json_object_has_member(result, "acls")
            ? json_object_get_array_member(result, "acls") : NULL;
        gint acl_count = acls ? (gint)json_array_get_length(acls) : 0;
        wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
        mvwprintw(pop, ry, 2, "ACL Rules (%d)", acl_count);
        wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
        ry++;
        for (gint i = 0; i < acl_count && ry < ph - 4; i++) {
            const gchar *acl = json_array_get_string_element(acls, i);
            int acp = C_DIM;
            if (acl && strstr(acl, "allow")) acp = C_GREEN;
            else if (acl && strstr(acl, "drop")) acp = C_RED;
            wattron(pop, COLOR_PAIR(acp));
            mvwprintw(pop, ry++, 4, "%.*s", pw - 6, acl ? acl : "");
            wattroff(pop, COLOR_PAIR(acp));
        }
        if (acl_count == 0) {
            wattron(pop, COLOR_PAIR(C_DIM));
            mvwprintw(pop, ry++, 4, "(no ACL rules)");
            wattroff(pop, COLOR_PAIR(C_DIM));
        }
    } else {
        JsonArray *ports = json_object_has_member(result, "ports")
            ? json_object_get_array_member(result, "ports") : NULL;
        gint port_count = ports ? (gint)json_array_get_length(ports) : 0;
        wattron(pop, COLOR_PAIR(C_FLEET) | A_BOLD);
        mvwprintw(pop, ry, 2, "Interfaces (%d)", port_count);
        wattroff(pop, COLOR_PAIR(C_FLEET) | A_BOLD);
        ry++;
        for (gint i = 0; i < port_count && ry < ph - 6; i++) {
            JsonObject *pobj = json_array_get_object_element(ports, i);
            const gchar *pname = json_object_get_string_member_with_default(pobj, "name", "?");
            const gchar *mac   = json_object_get_string_member_with_default(pobj, "mac", "-");
            const gchar *net   = json_object_get_string_member_with_default(pobj, "networks", "-");
            wattron(pop, COLOR_PAIR(C_CYAN));
            mvwprintw(pop, ry, 4, "|- %-18s", pname);
            wattroff(pop, COLOR_PAIR(C_CYAN));
            wattron(pop, COLOR_PAIR(C_DIM));
            mvwprintw(pop, ry, 26, "MAC: %-20s Net: %s", mac, net);
            wattroff(pop, COLOR_PAIR(C_DIM));
            ry++;
        }
        ry++;

        JsonArray *nats = json_object_has_member(result, "nats")
            ? json_object_get_array_member(result, "nats") : NULL;
        gint nat_count = nats ? (gint)json_array_get_length(nats) : 0;
        wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
        mvwprintw(pop, ry, 2, "NAT Rules (%d)", nat_count > 1 ? nat_count - 1 : 0);
        wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
        ry++;
        for (gint i = 0; i < nat_count && ry < ph - 4; i++) {
            const gchar *line = json_array_get_string_element(nats, i);
            if (!line || !*line) continue;
            if (g_str_has_prefix(line, "TYPE")) continue;
            char ntype[16]={0}, ext[20]={0}, lip[24]={0};
            if (sscanf(line, "%15s %*s %19s %*s %23s", ntype, ext, lip) >= 3) {
                wattron(pop, COLOR_PAIR(C_GREEN));
                mvwprintw(pop, ry, 4, "%-6s", ntype);
                wattroff(pop, COLOR_PAIR(C_GREEN));
                wattron(pop, COLOR_PAIR(C_DIM));
                wprintw(pop, "%s -> %s", ext, lip);
                wattroff(pop, COLOR_PAIR(C_DIM));
            } else {
                wattron(pop, COLOR_PAIR(C_DIM));
                mvwprintw(pop, ry, 4, "%.*s", pw - 6, line);
                wattroff(pop, COLOR_PAIR(C_DIM));
            }
            ry++;
        }
    }

    wattron(pop, COLOR_PAIR(C_DIM));
    mvwhline(pop, ph - 3, 1, ACS_HLINE, pw - 2);
    wattroff(pop, COLOR_PAIR(C_DIM));
    wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvwprintw(pop, ph - 2, 2, "[Esc] Close");
    wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);

    wrefresh(pop);
    timeout(-1);
    int ch;
    while ((ch = wgetch(pop)) != 27 && ch != 'q' && ch != 'Q' && ch != '\n' && ch != KEY_ENTER && ch != ' ') {}  /* B7-C1: Enter/Space/Q 추가 */
    timeout(50);
    delwin(pop);
    g_object_unref(parser);
    g_free(resp);
}

/* ── OVN ACL Manager 팝업: 'A' 키 → ACL 목록/추가 ── */
/**
 * show_ovn_acl_popup:
 *
 * F7 OVN 뷰에서 'A' 키로 호출되는 ACL 관리 팝업.
 * 스위치 전용 — 라우터 선택 시 "Select a switch first" 경고.
 *
 * 동작:
 *   1. 선택된 스위치의 ACL 목록 조회 (ovn.acl.list RPC)
 *   2. 기존 ACL 규칙을 팝업에 렌더링
 *   3. 'a' 키로 새 ACL 추가 (direction/priority/match/action 입력)
 *   4. Esc로 닫기
 *
 * 팝업 크기: 18x72
 * 루프 구조: ACL 추가 후 목록을 재조회하여 실시간 반영.
 */
static void show_ovn_acl_popup(void) {
    int oi = g_state.net.ovn_selected;
    if (oi < 0 || oi >= g_state.net.ovn.sw_count) {
        push_log_level("Select a switch first", LOG_WARN);
        return;
    }
    const char *sw = g_state.net.ovn.switches[oi];

    char title[80];
    snprintf(title, sizeof(title), "ACL MANAGER: %s", sw);
    int ph = 18, pw = 72;
    int scr_r, scr_c; getmaxyx(stdscr, scr_r, scr_c);
    if (pw > scr_c - 4) pw = scr_c - 4;
    if (ph > scr_r - 4) ph = scr_r - 4;

    gboolean running = TRUE;
    while (running) {
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "switch", sw);
        GError *err = NULL;
        gchar *resp = tui_send_request("ovn.acl.list", params, &err);
        if (err) g_error_free(err);

        gchar *acl_lines[16];
        int acl_count = 0;
        if (resp) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(p));
                if (json_object_has_member(ro, "result")) {
                    JsonArray *arr = json_object_get_array_member(ro, "result");
                    guint n = json_array_get_length(arr);
                    for (guint i = 0; i < n && acl_count < 16; i++) {
                        const gchar *line = json_array_get_string_element(arr, i);
                        if (line && *line) acl_lines[acl_count++] = g_strdup(line);
                    }
                }
            }
            g_object_unref(p); g_free(resp);
        }

        WINDOW *pop = create_popup(ph, pw, title);
        int ry = 2;
        wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD | A_UNDERLINE);
        mvwprintw(pop, ry, 2, "%-12s %-6s %-38s %-8s", "DIRECTION", "PRIO", "MATCH", "ACTION");
        wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD | A_UNDERLINE);
        ry++;

        for (int i = 0; i < acl_count && ry < ph - 4; i++) {
            int acp = C_DIM;
            if (strstr(acl_lines[i], "allow")) acp = C_GREEN;
            else if (strstr(acl_lines[i], "drop")) acp = C_RED;
            wattron(pop, COLOR_PAIR(acp));
            mvwprintw(pop, ry++, 2, " %.*s", pw - 4, acl_lines[i]);
            wattroff(pop, COLOR_PAIR(acp));
        }
        if (acl_count == 0) {
            wattron(pop, COLOR_PAIR(C_DIM));
            mvwprintw(pop, ry, 4, "(no ACL rules)");
            wattroff(pop, COLOR_PAIR(C_DIM));
        }

        wattron(pop, COLOR_PAIR(C_DIM));
        mvwhline(pop, ph - 3, 1, ACS_HLINE, pw - 2);
        wattroff(pop, COLOR_PAIR(C_DIM));
        wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
        mvwprintw(pop, ph - 2, 2, "[a] Add Rule  [Esc] Close");
        wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
        wrefresh(pop);

        timeout(-1);
        int ch = wgetch(pop);
        timeout(50);
        delwin(pop);

        for (int i = 0; i < acl_count; i++) g_free(acl_lines[i]);

        if (ch == 27 || ch == 'q') {
            running = FALSE;
        } else if (ch == 'a') {
            char dir[16]={0}, prio_s[8]={0}, match[64]={0}, action[16]={0};
            if (!prompt_input("Direction [to-lport/from-lport]: ", dir, sizeof(dir), C_YELLOW) || !dir[0]) continue;
            if (!prompt_input("Priority [0-65535]: ", prio_s, sizeof(prio_s), C_YELLOW) || !prio_s[0]) continue;
            if (!prompt_input("Match (e.g. tcp.dst==80): ", match, sizeof(match), C_YELLOW) || !match[0]) continue;
            prompt_input("Action [allow/drop] (default allow): ", action, sizeof(action), C_YELLOW);
            if (!action[0]) strncpy(action, "allow", sizeof(action)-1);

            JsonObject *ap = json_object_new();
            json_object_set_string_member(ap, "switch", sw);
            json_object_set_string_member(ap, "direction", dir);
            json_object_set_int_member(ap, "priority", atoi(prio_s));
            json_object_set_string_member(ap, "match", match);
            json_object_set_string_member(ap, "action", action);
            send_async_rpc("ovn.acl.add", ap, "OVN ACL ADD", "OVN");
            g_state.net.ovn.detail_for = -1;
        }
    }
}

/* ── OVN NAT/DHCP Info 팝업: 'I' 키 ── */
/**
 * show_ovn_info_popup:
 *
 * F7 OVN 뷰에서 'I' 키로 호출되는 부가 정보 팝업.
 * 리소스 종류에 따라 다른 정보를 표시:
 *
 *   - 라우터(LR) 선택 시: NAT 규칙 목록 (ovn.nat.list RPC)
 *     각 NAT 규칙을 C_GREEN으로 라인별 출력
 *   - 스위치(LS) 선택 시: DHCP 옵션 목록 (ovn.dhcp.list RPC)
 *     각 DHCP 옵션을 C_CYAN으로 라인별 출력
 *
 * 팝업 크기: 16x70
 * Esc 또는 'q'로 닫기.
 */
static void show_ovn_info_popup(void) {
    int oi = g_state.net.ovn_selected;
    int total = g_state.net.ovn.sw_count + g_state.net.ovn.rt_count;
    if (oi < 0 || oi >= total) return;

    gboolean is_switch = (oi < g_state.net.ovn.sw_count);
    const char *name = is_switch
        ? g_state.net.ovn.switches[oi]
        : g_state.net.ovn.routers[oi - g_state.net.ovn.sw_count];

    char title[80];
    snprintf(title, sizeof(title), "%s INFO: %s", is_switch ? "DHCP" : "NAT", name);
    int ph = 16, pw = 70;
    int scr_r, scr_c; getmaxyx(stdscr, scr_r, scr_c);
    if (pw > scr_c - 4) pw = scr_c - 4;
    if (ph > scr_r - 4) ph = scr_r - 4;

    WINDOW *pop = create_popup(ph, pw, title);
    int ry = 2;

    if (!is_switch) {
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "router", name);
        GError *err = NULL;
        gchar *resp = tui_send_request("ovn.nat.list", params, &err);
        if (err) g_error_free(err);
        if (resp) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(p));
                if (json_object_has_member(ro, "result")) {
                    JsonArray *arr = json_object_get_array_member(ro, "result");
                    wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
                    mvwprintw(pop, ry++, 2, "NAT Rules:");
                    wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
                    guint n = json_array_get_length(arr);
                    for (guint i = 0; i < n && ry < ph - 3; i++) {
                        const gchar *line = json_array_get_string_element(arr, i);
                        if (!line || !*line || g_str_has_prefix(line, "TYPE")) continue;
                        char ntype[16]={0}, ext[20]={0}, lip[24]={0};
                        if (sscanf(line, "%15s %*s %19s %*s %23s", ntype, ext, lip) >= 3) {
                            wattron(pop, COLOR_PAIR(C_GREEN));
                            mvwprintw(pop, ry, 4, "%-6s", ntype);
                            wattroff(pop, COLOR_PAIR(C_GREEN));
                            wattron(pop, COLOR_PAIR(C_DIM));
                            wprintw(pop, "%s -> %s", ext, lip);
                            wattroff(pop, COLOR_PAIR(C_DIM));
                        }
                        ry++;
                    }
                }
            }
            g_object_unref(p); g_free(resp);
        }
    } else {
        wattron(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
        mvwprintw(pop, ry++, 2, "DHCP Status:");
        wattroff(pop, COLOR_PAIR(C_CYAN) | A_BOLD);
        wattron(pop, COLOR_PAIR(C_DIM));
        mvwprintw(pop, ry++, 4, "Subnet DHCP options configured via ovn.dhcp.enable");
        mvwprintw(pop, ry++, 4, "Use [S] to create switch with subnet for auto-DHCP");
        wattroff(pop, COLOR_PAIR(C_DIM));
    }

    wattron(pop, COLOR_PAIR(C_DIM));
    mvwhline(pop, ph - 3, 1, ACS_HLINE, pw - 2);
    wattroff(pop, COLOR_PAIR(C_DIM));
    wattron(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvwprintw(pop, ph - 2, 2, "[Esc] Close");
    wattroff(pop, COLOR_PAIR(C_YELLOW) | A_BOLD);
    wrefresh(pop);

    timeout(-1);
    int ch; while ((ch = wgetch(pop)) != 27 && ch != 'q' && ch != 'Q' && ch != '\n' && ch != KEY_ENTER && ch != ' ') {}  /* B7-C1: Enter/Space/Q 추가 */
    timeout(50);
    delwin(pop);
}

/**
 * handle_key_ovn:
 * @ch: ncurses 키 코드
 *
 * F7 OVN 뷰의 키 이벤트 핸들러.
 * Elm Architecture의 Update 역할 — 키 입력에 따라 상태 변경.
 *
 * 키 바인딩:
 *   UP/DOWN : 리소스 선택 이동 (detail_for=-1로 캐시 무효화)
 *   'r'     : 수동 갱신 (fetch_net_data 호출)
 *   'S'     : 논리 스위치 생성 (이름 + 서브넷 입력)
 *   'R'     : 논리 라우터 생성 (이름 입력)
 *   'X'     : 선택 리소스 삭제 (확인 다이얼로그 표시)
 *   'T'     : 테넌트 생성 (이름 + 서브넷 → 스위치+ACL+DHCP 일괄)
 *   Enter   : 상세 팝업 (show_ovn_detail_popup)
 *   'A'     : ACL 매니저 팝업 (show_ovn_acl_popup)
 *   'I'     : NAT/DHCP 정보 팝업 (show_ovn_info_popup)
 *
 * 입력 검증: 이름은 영숫자+하이픈+언더스코어만 허용, 최대 63자.
 * 서브넷은 CIDR 형식(x.x.x.x/N) 필수.
 */
static void handle_key_ovn(int ch) {
    switch (ch) {
    case KEY_UP:
        if (g_state.net.ovn_selected > 0) { g_state.net.ovn_selected--; g_state.net.ovn.detail_for=-1; g_state.dirty.all=TRUE; }
        break;
    case KEY_DOWN: {
        int total = g_state.net.ovn.sw_count + g_state.net.ovn.rt_count;
        if (g_state.net.ovn_selected < total-1) { g_state.net.ovn_selected++; g_state.net.ovn.detail_for=-1; g_state.dirty.all=TRUE; }
        break;
    }
    case 'r':
        fetch_net_data();
        push_log_level("OVN: refreshed", LOG_SYS);
        g_state.dirty.all = TRUE;
        break;

    case 'S': {
        if (!g_state.net.ovn.available) { push_log_level("OVN not available", LOG_WARN); break; }
        char sw_name[64] = {0};
        if (!prompt_input("OVN Switch name: ", sw_name, sizeof(sw_name), C_CYAN) || !sw_name[0]) break;
        /* 입력 검증: 영숫자 + 하이픈만 허용, 최대 63자 */
        gboolean valid = TRUE;
        for (int i = 0; sw_name[i]; i++) {
            if (!g_ascii_isalnum(sw_name[i]) && sw_name[i] != '-' && sw_name[i] != '_') { valid = FALSE; break; }
        }
        if (!valid || strlen(sw_name) > 63) {
            push_log_level("Invalid name (alphanumeric, -, _ only)", LOG_WARN);
            break;
        }
        char subnet[48] = {0};
        prompt_input("Subnet (e.g. 10.200.0.0/24, empty=skip): ", subnet, sizeof(subnet), C_CYAN);
        /* 서브넷 검증: 비어있거나 CIDR 형식 */
        if (subnet[0] && !strchr(subnet, '/')) {
            push_log_level("Invalid subnet (must be CIDR: x.x.x.x/N)", LOG_WARN);
            break;
        }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", sw_name);
        if (subnet[0]) json_object_set_string_member(p, "subnet", subnet);
        send_async_rpc("ovn.switch.create", p, "OVN SW CREATE", "OVN");
        g_state.net.needs_refresh = TRUE;
        g_state.dirty.all = TRUE;
        break;
    }

    case 'R': {
        if (!g_state.net.ovn.available) { push_log_level("OVN not available", LOG_WARN); break; }
        char lr_name[64] = {0};
        if (!prompt_input("OVN Router name: ", lr_name, sizeof(lr_name), C_CYAN) || !lr_name[0]) break;
        /* 입력 검증 */
        gboolean valid = TRUE;
        for (int i = 0; lr_name[i]; i++) {
            if (!g_ascii_isalnum(lr_name[i]) && lr_name[i] != '-' && lr_name[i] != '_') { valid = FALSE; break; }
        }
        if (!valid || strlen(lr_name) > 63) {
            push_log_level("Invalid name (alphanumeric, -, _ only)", LOG_WARN);
            break;
        }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", lr_name);
        send_async_rpc("ovn.router.create", p, "OVN LR CREATE", "OVN");
        g_state.net.needs_refresh = TRUE;
        g_state.dirty.all = TRUE;
        break;
    }

    case 'X': {
        if (!g_state.net.ovn.available) { push_log_level("OVN not available", LOG_WARN); break; }
        int total_ovn = g_state.net.ovn.sw_count + g_state.net.ovn.rt_count;
        if (total_ovn == 0) { push_log_level("No OVN resources to delete", LOG_WARN); break; }
        int oi = g_state.net.ovn_selected;
        if (oi < 0 || oi >= total_ovn) { push_log_level("Invalid selection", LOG_WARN); break; }

        if (oi < g_state.net.ovn.sw_count) {
            const char *name = g_state.net.ovn.switches[oi];
            char warn[128];
            snprintf(warn, sizeof(warn), "DELETE OVN switch '%s'?", name);
            if (confirm_dialog(warn, name)) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                send_async_rpc("ovn.switch.delete", p, "OVN SW DEL", "OVN");
                g_state.net.needs_refresh = TRUE;
                if (g_state.net.ovn_selected >= total_ovn - 1 && g_state.net.ovn_selected > 0)
                    g_state.net.ovn_selected--;
            }
        } else {
            int ri = oi - g_state.net.ovn.sw_count;
            const char *name = g_state.net.ovn.routers[ri];
            char warn[128];
            snprintf(warn, sizeof(warn), "DELETE OVN router '%s'?", name);
            if (confirm_dialog(warn, name)) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                send_async_rpc("ovn.router.delete", p, "OVN LR DEL", "OVN");
                g_state.net.needs_refresh = TRUE;
                if (g_state.net.ovn_selected >= total_ovn - 1 && g_state.net.ovn_selected > 0)
                    g_state.net.ovn_selected--;
            }
        }
        g_state.dirty.all = TRUE;
        break;
    }

    case 'T': {
        if (!g_state.net.ovn.available) { push_log_level("OVN not available", LOG_WARN); break; }
        char tenant[64] = {0}, subnet[48] = {0};
        if (!prompt_input("Tenant name: ", tenant, sizeof(tenant), C_CYAN) || !tenant[0]) break;
        /* 입력 검증: 영숫자 + 하이픈만 */
        gboolean valid = TRUE;
        for (int i = 0; tenant[i]; i++) {
            if (!g_ascii_isalnum(tenant[i]) && tenant[i] != '-' && tenant[i] != '_') { valid = FALSE; break; }
        }
        if (!valid || strlen(tenant) > 63) {
            push_log_level("Invalid tenant name", LOG_WARN);
            break;
        }
        if (!prompt_input("Tenant subnet (e.g. 10.201.0.0/24): ", subnet, sizeof(subnet), C_CYAN) || !subnet[0]) break;
        if (!strchr(subnet, '/')) {
            push_log_level("Invalid subnet (must be CIDR)", LOG_WARN);
            break;
        }
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "tenant", tenant);
        json_object_set_string_member(p, "subnet", subnet);
        send_async_rpc("ovn.tenant.create", p, "OVN TENANT", "OVN");
        g_state.net.needs_refresh = TRUE;
        g_state.dirty.all = TRUE;
        break;
    }

    case 'N': {
        if (!g_state.net.ovn.available) { push_log_level("OVN not available", LOG_WARN); break; }
        char router[64]={0}, ext_ip[48]={0}, log_ip[48]={0}, nat_type[16]={0};
        if (!prompt_input("Router name: ", router, sizeof(router), C_CYAN) || !router[0]) break;
        prompt_input("NAT type [snat/dnat] (default snat): ", nat_type, sizeof(nat_type), C_CYAN);
        if (!nat_type[0]) strncpy(nat_type, "snat", sizeof(nat_type)-1);
        /* NAT 타입 검증 */
        if (g_strcmp0(nat_type, "snat") != 0 && g_strcmp0(nat_type, "dnat") != 0 && g_strcmp0(nat_type, "dnat_and_snat") != 0) {
            push_log_level("Invalid NAT type (snat/dnat/dnat_and_snat)", LOG_WARN);
            break;
        }
        if (!prompt_input("External IP: ", ext_ip, sizeof(ext_ip), C_CYAN) || !ext_ip[0]) break;
        /* IP 기본 검증: 점 포함 */
        if (!strchr(ext_ip, '.')) {
            push_log_level("Invalid external IP", LOG_WARN);
            break;
        }
        if (!prompt_input("Logical IP/CIDR: ", log_ip, sizeof(log_ip), C_CYAN) || !log_ip[0]) break;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "router", router);
        json_object_set_string_member(p, "type", nat_type);
        json_object_set_string_member(p, "external_ip", ext_ip);
        json_object_set_string_member(p, "logical_ip", log_ip);
        send_async_rpc("ovn.nat.add", p, "OVN NAT ADD", "OVN");
        g_state.net.needs_refresh = TRUE;
        g_state.dirty.all = TRUE;
        break;
    }

    case '\n':
    case KEY_ENTER: {
        if (!g_state.net.ovn.available) break;
        int total_e = g_state.net.ovn.sw_count + g_state.net.ovn.rt_count;
        if (total_e == 0 || g_state.net.ovn_selected < 0) break;
        show_ovn_detail_popup();
        g_state.dirty.all = TRUE;
        break;
    }

    case 'A':
        if (g_state.net.ovn.available)
            show_ovn_acl_popup();
        g_state.dirty.all = TRUE;
        break;

    case 'I':
        if (g_state.net.ovn.available)
            show_ovn_info_popup();
        g_state.dirty.all = TRUE;
        break;

    // [G] Security Group List ────────────────────────────────────────────────
    case 'G': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("security_group.list", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = arr ? json_array_get_length(arr) : 0;
                        push_log_level("=== Security Groups ===", LOG_SYS);
                        for (guint si = 0; si < len; si++) {
                            JsonObject *sg = json_array_get_object_element(arr, si);
                            const gchar *name = safe_str(sg, "name", "?");
                            gint64 rules = safe_int(sg, "rule_count");
                            char tmp[128];
                            snprintf(tmp, sizeof(tmp), "  %-20s %ld rules", name, (long)rules);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no security groups)");
                    }
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("SEC GROUP ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    default: break;
    }
}

// =============================================================================
// VIEW: STORAGE
// =============================================================================
static void fetch_stg_data(void) {
    // Pool 목록
    GError *err = NULL;
    gchar  *resp = tui_send_request("storage.pool.list", NULL, &err);
    g_state.stg.pool_count = 0;
    if (!err && resp) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, resp, -1, NULL)) {
            JsonObject *ro = json_node_get_object(json_parser_get_root(p));
            if (json_object_has_member(ro, "result")) {
                JsonNode *rn = json_object_get_member(ro, "result");
                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                    JsonArray *arr = json_node_get_array(rn);
                    int n = (int)json_array_get_length(arr);
                    g_state.stg.pool_count = n < MAX_POOL ? n : MAX_POOL;
                    for (int i = 0; i < g_state.stg.pool_count; i++) {
                        JsonObject *po = json_array_get_object_element(arr, i);
                        PoolInfo *pi = &g_state.stg.pools[i];
                        strncpy(pi->name,   safe_str(po,"name","?"),   sizeof(pi->name) - 1);
                        pi->name[sizeof(pi->name) - 1] = '\0';
                        strncpy(pi->size,   safe_str(po,"size","?"),   sizeof(pi->size) - 1);
                        pi->size[sizeof(pi->size) - 1] = '\0';
                        strncpy(pi->alloc,  safe_str(po,"alloc","?"),  sizeof(pi->alloc) - 1);
                        pi->alloc[sizeof(pi->alloc) - 1] = '\0';
                        strncpy(pi->free_,  safe_str(po,"free","?"),   sizeof(pi->free_) - 1);
                        pi->free_[sizeof(pi->free_) - 1] = '\0';
                        strncpy(pi->health, safe_str(po,"health","?"), sizeof(pi->health) - 1);
                        pi->health[sizeof(pi->health) - 1] = '\0';
                    }
                }
            }
        }
        g_object_unref(p); g_free(resp);
    }
    if (err) g_error_free(err);

    // zvol 목록
    err = NULL;
    resp = tui_send_request("storage.zvol.list", NULL, &err);
    g_state.stg.zvol_count = 0;
    if (!err && resp) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, resp, -1, NULL)) {
            JsonObject *ro = json_node_get_object(json_parser_get_root(p));
            if (json_object_has_member(ro, "result")) {
                JsonNode *rn = json_object_get_member(ro, "result");
                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                    JsonArray *arr = json_node_get_array(rn);
                    int n = (int)json_array_get_length(arr);
                    g_state.stg.zvol_count = n < MAX_ZVOL ? n : MAX_ZVOL;
                    for (int i = 0; i < g_state.stg.zvol_count; i++) {
                        JsonObject *zo = json_array_get_object_element(arr, i);
                        ZvolInfo *zi = &g_state.stg.zvols[i];
                        strncpy(zi->path, safe_str(zo,"name","?"),  sizeof(zi->path) - 1);
                        zi->path[sizeof(zi->path) - 1] = '\0';
                        strncpy(zi->size, safe_str(zo,"volsize","?"), sizeof(zi->size) - 1); /* [P0-Fix#1] daemon이 "volsize" 키로 전송 */
                        zi->size[sizeof(zi->size) - 1] = '\0';
                        strncpy(zi->used, safe_str(zo,"used","?"),    sizeof(zi->used) - 1); /* [P0-Fix#1] daemon이 "used" 키로 전송 */
                        zi->used[sizeof(zi->used) - 1] = '\0';
                    }
                }
            }
        }
        g_object_unref(p); g_free(resp);
    }
    if (err) g_error_free(err);
    g_state.stg.needs_refresh = FALSE;
    scroll_select_clamp(&g_state.stg.selected, g_state.stg.zvol_count);
}

static void draw_view_stg(WINDOW *win, int y0, int mid_h, int w) {
    /* 자동 갱신: 10초 간격 */
    static gint64 last_stg_fetch = 0;
    gint64 now_stg = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (g_state.stg.needs_refresh || (now_stg - last_stg_fetch >= 10)) {
        fetch_stg_data();
        last_stg_fetch = now_stg;
    }

    // T3: Pool 패널 — 용량 바 + 건강 상태 인디케이터
    int pool_h = g_state.stg.pool_count * 2 + 3;
    if (pool_h > mid_h * 2 / 5) pool_h = mid_h * 2 / 5;
    if (pool_h < 4) pool_h = 4;
    draw_panel(win, y0, 0, pool_h, w, "zfs pools", C_FLEET);

    // 컬럼 헤더
    wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
    mvwprintw(win, y0+1, 2, "%-20s %-8s %-8s %-8s  USAGE", "NAME", "SIZE", "ALLOC", "FREE");
    wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);

    int py = y0 + 2;
    for (int i = 0; i < g_state.stg.pool_count && py < y0 + pool_h - 1; i++) {
        PoolInfo *pi = &g_state.stg.pools[i];
        int is_online = (g_strcmp0(pi->health, "ONLINE") == 0);
        int h_cp = is_online ? C_GREEN : C_RED;

        // 풀 이름 + 건강 인디케이터
        wattron(win, COLOR_PAIR(h_cp)|A_BOLD);
        mvwprintw(win, py, 2, "%s %-18.18s", is_online ? "●" : "✗", pi->name);
        wattroff(win, COLOR_PAIR(h_cp)|A_BOLD);
        mvwprintw(win, py, 24, "%-8.8s %-8.8s %-8.8s",
                  pi->size, pi->alloc, pi->free_);

        // 용량 사용률 바 — 문자열 → GB 변환
        // "219M"→0.214, "1.88G"→1.88, "6.90T"→7065, "480K"→0.0004
        double alloc_gb = 0, size_gb = 0;
        char *end;
        alloc_gb = strtod(pi->alloc, &end);
        if      (*end == 'T' || *end == 't') alloc_gb *= 1024;
        else if (*end == 'M' || *end == 'm') alloc_gb /= 1024;
        else if (*end == 'K' || *end == 'k') alloc_gb /= (1024 * 1024);
        size_gb  = strtod(pi->size,  &end);
        if      (*end == 'T' || *end == 't') size_gb  *= 1024;
        else if (*end == 'M' || *end == 'm') size_gb  /= 1024;
        else if (*end == 'K' || *end == 'k') size_gb  /= (1024 * 1024);
        double usage_r = (size_gb > 0) ? (alloc_gb / size_gb) : 0;
        if (usage_r > 1.0) usage_r = 1.0;

        int bar_x = 52, bar_w = w - 58;
        if (bar_w > 4 && bar_w < w) {
            int usage_cp = pcv_color_for_pct(usage_r);
            int filled = (int)(usage_r * bar_w);
            mvwaddch(win, py, bar_x-1, '[');
            wattron(win, COLOR_PAIR(usage_cp)|A_BOLD);
            for (int b = 0; b < bar_w; b++)
                mvwaddch(win, py, bar_x+b, b < filled ? ACS_BLOCK : '-');
            wattroff(win, COLOR_PAIR(usage_cp)|A_BOLD);
            mvwaddch(win, py, bar_x+bar_w, ']');
            // 퍼센트
            wattron(win, COLOR_PAIR(usage_cp));
            mvwprintw(win, py, bar_x+bar_w+2, "%3d%%", (int)(usage_r*100));
            wattroff(win, COLOR_PAIR(usage_cp));
        }
        py++;
    }
    if (g_state.stg.pool_count == 0) {
        wattron(win, COLOR_PAIR(C_DIM)|A_ITALIC);
        mvwprintw(win, y0+2, 2, "(no ZFS pools detected)");
        wattroff(win, COLOR_PAIR(C_DIM)|A_ITALIC);
    }

    // 하단: zvol 테이블
    int zvol_y = y0 + pool_h;
    int zvol_h = mid_h - pool_h;
    if (zvol_h < 4) return;
    draw_panel(win, zvol_y, 0, zvol_h, w, "zvols", C_CYAN);

    static const char *hdrs[] = { "ZVOL PATH", "SIZE", "USED" };
    static const int col_w[]  = { 50, 10, 10 };
    int nrows = g_state.stg.zvol_count;
    static const char *rows[MAX_ZVOL * 3];
    for (int i = 0; i < nrows; i++) {
        rows[i*3+0] = g_state.stg.zvols[i].path;
        rows[i*3+1] = g_state.stg.zvols[i].size;
        rows[i*3+2] = g_state.stg.zvols[i].used;
    }
    draw_table(win, zvol_y+1, 0, zvol_h-2, w,
               hdrs, col_w, 3,
               rows, nrows,
               &g_state.stg.scroll, g_state.stg.selected, C_CYAN);

    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, y0+mid_h-2, 2,
              "ACTIONS: [c]Zvol [d]Del [H]Health [C]Capacity [B]Backup [b]History [I]iSCSI [r]Refresh");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);
}

static void handle_key_stg(int ch) {
    int *sel = &g_state.stg.selected;
    int  cnt = g_state.stg.zvol_count;
    switch (ch) {
    case KEY_UP:   if (*sel > 0)    { (*sel)--; g_state.dirty.all=TRUE; } break;
    case KEY_DOWN: if (*sel < cnt-1){ (*sel)++; g_state.dirty.all=TRUE; } break;
    case 'r': fetch_stg_data(); push_log("STG: refreshed"); g_state.dirty.all=TRUE; break;
    case 'c': {
        char path[128]={0}, sz[16]={0};
        if (!prompt_input("zvol path (e.g. pcvpool/vms/newdisk): ", path, sizeof(path), C_YELLOW) || !path[0]) break;
        if (!prompt_input("Size (e.g. 50G): ", sz, sizeof(sz), C_YELLOW) || !sz[0]) break;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "zvol_path", path);
        json_object_set_string_member(p, "size",      sz);
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "ZVOL CREATE %s %s", path, sz);
        send_async_rpc("storage.zvol.create", p, log_ok, "ZVOL CREATE");
        g_state.stg.needs_refresh = TRUE;
        g_state.dirty.all = TRUE;
        break;
    }
    case 'd': if (cnt > 0) {
        const char *zp = g_state.stg.zvols[*sel].path;
        char warn[256]; snprintf(warn, sizeof(warn), "DELETE zvol '%s'", zp);
        if (confirm_dialog(warn, zp)) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "zvol_path", zp);
            char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "ZVOL DELETE %s", zp);
            send_async_rpc("storage.zvol.delete", p, log_ok, "ZVOL DEL");
            g_state.stg.needs_refresh = TRUE;
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // [H] Pool Health Check ──────────────────────────────────────────────────
    case 'H': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("storage.pool.health", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = json_array_get_length(arr);
                        push_log_level("=== Pool Health ===", LOG_SYS);
                        for (guint i = 0; i < len; i++) {
                            JsonObject *ph2 = json_array_get_object_element(arr, i);
                            const gchar *pname = safe_str(ph2, "name", "?");
                            const gchar *health = safe_str(ph2, "health", "?");
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp), "  [%s] %s", pname, health);
                            push_log_level(tmp, g_strcmp0(health, "ONLINE") == 0 ? LOG_SUCCESS : LOG_WARN);
                        }
                        if (len == 0) push_log("  (no pools)");
                    } else if (JSON_NODE_TYPE(rn) == JSON_NODE_OBJECT) {
                        JsonObject *res = json_node_get_object(rn);
                        const gchar *pname = safe_str(res, "name", "?");
                        const gchar *health = safe_str(res, "health", "?");
                        char tmp[256];
                        snprintf(tmp, sizeof(tmp), "Pool [%s] → %s", pname, health);
                        push_log_level(tmp, g_strcmp0(health, "ONLINE") == 0 ? LOG_SUCCESS : LOG_WARN);
                    }
                } else if (json_object_has_member(ro, "error")) {
                    JsonObject *eo = json_object_get_object_member(ro, "error");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "POOL HEALTH: %s", safe_str(eo, "message", "error"));
                    push_log_level(tmp, LOG_WARN);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("POOL HEALTH ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    // [C] Capacity Forecast ──────────────────────────────────────────────────
    case 'C': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("capacity.forecast", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    double usage = safe_double(res, "usage_percent");
                    double forecast = safe_double(res, "forecast_percent");
                    const gchar *eta = safe_str(res, "full_eta", "N/A");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp),
                             "CAPACITY: current=%.1f%% forecast=%.1f%% full_eta=%s",
                             usage, forecast, eta);
                    push_log_level(tmp, usage > 85.0 ? LOG_WARN : LOG_SYS);
                } else if (json_object_has_member(ro, "error")) {
                    JsonObject *eo = json_object_get_object_member(ro, "error");
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "CAPACITY: %s", safe_str(eo, "message", "error"));
                    push_log_level(tmp, LOG_WARN);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("CAPACITY ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    // [B] Backup Policy List ─────────────────────────────────────────────────
    case 'B': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("backup.policy.list", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = json_array_get_length(arr);
                        push_log_level("=== Backup Policies ===", LOG_SYS);
                        for (guint i = 0; i < len; i++) {
                            JsonObject *bp = json_array_get_object_element(arr, i);
                            const gchar *vm = safe_str(bp, "vm_name", "*");
                            gint64 intv = safe_int(bp, "interval_hours");
                            gint64 ret  = safe_int(bp, "retention");
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp),
                                     "  [%s] interval=%ldh retention=%ld",
                                     vm, (long)intv, (long)ret);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no backup policies)");
                    }
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("BACKUP POLICY ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    // [b] Backup History (for selected zvol's VM) ────────────────────────────
    case 'b': {
        char vm_name[64] = {0};
        if (!prompt_input("VM name for backup history: ", vm_name, sizeof(vm_name), C_YELLOW) || !vm_name[0])
            break;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_name", vm_name);
        GError *err = NULL;
        gchar *resp = tui_send_request("backup.history", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = json_array_get_length(arr);
                        char hdr[128];
                        snprintf(hdr, sizeof(hdr), "=== Backup History [%s] ===", vm_name);
                        push_log_level(hdr, LOG_SYS);
                        for (guint i = 0; i < len; i++) {
                            JsonObject *bh = json_array_get_object_element(arr, i);
                            const gchar *snap = safe_str(bh, "snapshot", "?");
                            const gchar *ts   = safe_str(bh, "timestamp", "?");
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp), "  %s @ %s", snap, ts);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no backup history)");
                    }
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("BACKUP HISTORY ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    // [I] iSCSI Target List ────────────────────────────────────────────────
    case 'I': {
        JsonObject *p = json_object_new();
        GError *err = NULL;
        gchar *resp = tui_send_request("iscsi.target.list", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = arr ? json_array_get_length(arr) : 0;
                        push_log_level("=== iSCSI Targets ===", LOG_SYS);
                        for (guint ti = 0; ti < len; ti++) {
                            JsonObject *tgt = json_array_get_object_element(arr, ti);
                            const gchar *iqn = safe_str(tgt, "iqn", "?");
                            const gchar *state = safe_str(tgt, "state", "?");
                            char tmp[256];
                            snprintf(tmp, sizeof(tmp), "  %s [%s]", iqn, state);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no iSCSI targets)");
                    }
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("iSCSI error"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    }

    default: break;
    }
    ScrollState *sc = &g_state.stg.scroll;
    if (*sel < sc->position) sc->position = *sel;
    if (sc->viewport > 0 && *sel >= sc->position + sc->viewport)
        sc->position = *sel - sc->viewport + 1;
}

// =============================================================================
// VIEW: CONTAINER
// =============================================================================
static void fetch_ctr_data(void) {
    GError *err = NULL;
    gchar  *resp = tui_send_request("container.list", NULL, &err);
    g_state.ctr.ctr_count = 0;
    if (!err && resp) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, resp, -1, NULL)) {
            JsonObject *ro = json_node_get_object(json_parser_get_root(p));
            if (json_object_has_member(ro, "result")) {
                JsonNode *rn = json_object_get_member(ro, "result");
                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                    JsonArray *arr = json_node_get_array(rn);
                    int n = (int)json_array_get_length(arr);
                    g_state.ctr.ctr_count = n < MAX_CTR ? n : MAX_CTR;
                    for (int i = 0; i < g_state.ctr.ctr_count; i++) {
                        JsonObject *co = json_array_get_object_element(arr, i);
                        CtrInfo *ci = &g_state.ctr.ctrs[i];
                        strncpy(ci->name,    safe_str(co,"name","?"),    sizeof(ci->name) - 1);
                        ci->name[sizeof(ci->name) - 1] = '\0';
                        strncpy(ci->state,   safe_str(co,"state","?"),   sizeof(ci->state) - 1);
                        ci->state[sizeof(ci->state) - 1] = '\0';
                        strncpy(ci->ip_addr, safe_str(co,"ip_addr",""),  sizeof(ci->ip_addr) - 1);
                        ci->ip_addr[sizeof(ci->ip_addr) - 1] = '\0';
                        strncpy(ci->image,   safe_str(co,"image","?"),   sizeof(ci->image) - 1);
                        ci->image[sizeof(ci->image) - 1] = '\0';
                    }
                }
            }
        }
        g_object_unref(p); g_free(resp);
    }
    if (err) g_error_free(err);
    g_state.ctr.needs_refresh = FALSE;
    scroll_select_clamp(&g_state.ctr.selected, g_state.ctr.ctr_count);
}

static void draw_view_ctr(WINDOW *win, int y0, int mid_h, int w) {
    /* 자동 갱신: 10초 간격 */
    static gint64 last_ctr_fetch = 0;
    gint64 now_ctr = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (g_state.ctr.needs_refresh || (now_ctr - last_ctr_fetch >= 10)) {
        fetch_ctr_data();
        last_ctr_fetch = now_ctr;
    }

    // T4: 좌(목록) / 우(상세) 분할
    int ctr_left_w  = w * 2 / 5;
    int ctr_right_w = w - ctr_left_w;
    draw_panel(win, y0, 0,          mid_h, ctr_left_w,  "containers", C_FLEET);
    draw_panel(win, y0, ctr_left_w, mid_h, ctr_right_w, "container detail", C_CYAN);

    int nrows = g_state.ctr.ctr_count;

    // ── 좌측: 컨테이너 목록 ──────────────────────────────────────────────────
    // 컬럼 헤더
    wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
    mvwprintw(win, y0+1, 2, "%-4s %-18s %-8s", "ST", "NAME", "STATE");
    wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);

    int vis = mid_h - 4;
    g_state.ctr.scroll.viewport = vis;
    int row_y = y0 + 2;
    for (int i = 0; i < vis; i++) {
        int ri = i + g_state.ctr.scroll.position;
        if (ri >= nrows) break;
        CtrInfo *ct = &g_state.ctr.ctrs[ri];
        bool is_sel = (ri == g_state.ctr.selected);

        // T4: 상태 색상 분기
        int st_cp;
        const char *st_tag;
        if      (strstr(ct->state, "running")  || strstr(ct->state, "RUNNING"))
            { st_cp = C_GREEN;  st_tag = "[R]"; }
        else if (strstr(ct->state, "stopped")  || strstr(ct->state, "STOPPED"))
            { st_cp = C_DIM;    st_tag = "[S]"; }
        else if (strstr(ct->state, "frozen")   || strstr(ct->state, "FROZEN"))
            { st_cp = C_YELLOW; st_tag = "[F]"; }
        else if (strstr(ct->state, "error")    || strstr(ct->state, "ERROR"))
            { st_cp = C_RED;    st_tag = "[!]"; }
        else
            { st_cp = C_DIM;    st_tag = "[?]"; }

        if (is_sel) {
            wattron(win, COLOR_PAIR(C_HIGHLIGHT)|A_REVERSE|A_BOLD);
            mvwprintw(win, row_y, 2, "%-4s %-18.18s %-10.10s",
                      st_tag, ct->name, ct->state);
            wattroff(win, COLOR_PAIR(C_HIGHLIGHT)|A_REVERSE|A_BOLD);
        } else {
            wattron(win, COLOR_PAIR(st_cp)|A_BOLD);
            mvwprintw(win, row_y, 2, "%-4s", st_tag);
            wattroff(win, COLOR_PAIR(st_cp)|A_BOLD);
            wattron(win, COLOR_PAIR(C_CYAN));
            mvwprintw(win, row_y, 6, "%-18.18s", ct->name);
            wattroff(win, COLOR_PAIR(C_CYAN));
            wattron(win, COLOR_PAIR(st_cp));
            mvwprintw(win, row_y, 25, "%-10.10s", ct->state);
            wattroff(win, COLOR_PAIR(st_cp));
        }
        row_y++;
    }
    if (nrows == 0) {
        wattron(win, COLOR_PAIR(C_DIM)|A_ITALIC);
        mvwprintw(win, y0+3, 2, "(no containers)");
        wattroff(win, COLOR_PAIR(C_DIM)|A_ITALIC);
    }
    draw_scrollbar(win, y0+2, vis, ctr_left_w-2, &g_state.ctr.scroll);

    // ── 우측: 컨테이너 인스펙터 패널 (VMware 스타일) ───────────────────────
    if (nrows > 0) {
        CtrInfo *sel_ct = &g_state.ctr.ctrs[g_state.ctr.selected];
        int rx = ctr_left_w + 2, ry = y0 + 1;
        int detail_w = ctr_right_w - 4;

        int st_cp2;
        const char *st_icon;
        if      (strstr(sel_ct->state,"running")||strstr(sel_ct->state,"RUNNING"))
            { st_cp2 = C_GREEN;  st_icon = "● RUNNING"; }
        else if (strstr(sel_ct->state,"stopped")||strstr(sel_ct->state,"STOPPED"))
            { st_cp2 = C_DIM;    st_icon = "○ STOPPED"; }
        else if (strstr(sel_ct->state,"frozen")||strstr(sel_ct->state,"FROZEN"))
            { st_cp2 = C_YELLOW; st_icon = "◆ FROZEN"; }
        else if (strstr(sel_ct->state,"error")||strstr(sel_ct->state,"ERROR"))
            { st_cp2 = C_RED;    st_icon = "✖ ERROR"; }
        else
            { st_cp2 = C_DIM;    st_icon = "? UNKNOWN"; }

        // 컨테이너 이름 (제목)
        wattron(win, COLOR_PAIR(C_TITLE)|A_BOLD);
        mvwprintw(win, ry++, rx, "◈ %s", sel_ct->name);
        wattroff(win, COLOR_PAIR(C_TITLE)|A_BOLD);

        // 상태
        wattron(win, COLOR_PAIR(st_cp2)|A_BOLD);
        mvwprintw(win, ry++, rx, "  %s", st_icon);
        wattroff(win, COLOR_PAIR(st_cp2)|A_BOLD);

        ry++; // 빈 줄

        // ── Configuration 섹션 ──────────────────────────────────────────
        wattron(win, COLOR_PAIR(C_TITLE)|A_BOLD);
        mvwprintw(win, ry++, rx, "── Configuration ──");
        wattroff(win, COLOR_PAIR(C_TITLE)|A_BOLD);

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry++, rx, "  Name    ");
        wattroff(win, COLOR_PAIR(C_DIM));
        wattron(win, COLOR_PAIR(C_CYAN));
        mvwprintw(win, ry-1, rx + 10, "%-.*s", detail_w - 12, sel_ct->name);
        wattroff(win, COLOR_PAIR(C_CYAN));

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry++, rx, "  Image   ");
        wattroff(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry-1, rx + 10, "%-.*s", detail_w - 12, sel_ct->image);

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry++, rx, "  State   ");
        wattroff(win, COLOR_PAIR(C_DIM));
        wattron(win, COLOR_PAIR(st_cp2));
        mvwprintw(win, ry-1, rx + 10, "%s", sel_ct->state);
        wattroff(win, COLOR_PAIR(st_cp2));

        ry++; // 빈 줄

        // ── Network 섹션 ────────────────────────────────────────────────
        wattron(win, COLOR_PAIR(C_TITLE)|A_BOLD);
        mvwprintw(win, ry++, rx, "── Network ──");
        wattroff(win, COLOR_PAIR(C_TITLE)|A_BOLD);

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry++, rx, "  IP      ");
        wattroff(win, COLOR_PAIR(C_DIM));
        int ip_cp = (sel_ct->ip_addr[0] && g_strcmp0(sel_ct->ip_addr,"N/A") != 0)
                    ? C_GREEN : C_DIM;
        wattron(win, COLOR_PAIR(ip_cp));
        mvwprintw(win, ry-1, rx + 10, "%s",
                  sel_ct->ip_addr[0] ? sel_ct->ip_addr : "N/A");
        wattroff(win, COLOR_PAIR(ip_cp));

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry++, rx, "  Bridge  ");
        wattroff(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry-1, rx + 10, "lxcbr0");

        ry++; // 빈 줄

        // ── Container Details 섹션 ──────────────────────────────────────
        wattron(win, COLOR_PAIR(C_TITLE)|A_BOLD);
        mvwprintw(win, ry++, rx, "── Details ──");
        wattroff(win, COLOR_PAIR(C_TITLE)|A_BOLD);

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry++, rx, "  Engine  ");
        wattroff(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry-1, rx + 10, "LXC");

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry++, rx, "  Index   ");
        wattroff(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, ry-1, rx + 10, "#%d of %d",
                  g_state.ctr.selected + 1, nrows);

        ry++; // 빈 줄

        // ── Metrics 섹션 (container.metrics 결과 캐시) ────────────────
        wattron(win, COLOR_PAIR(C_TITLE)|A_BOLD);
        mvwprintw(win, ry++, rx, "── Metrics ──");
        wattroff(win, COLOR_PAIR(C_TITLE)|A_BOLD);

        if (sel_ct->metrics_loaded) {
            // CPU %
            int cpu_cp = sel_ct->cpu_percent > 80.0 ? C_RED :
                         sel_ct->cpu_percent > 50.0 ? C_YELLOW : C_GREEN;
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, ry++, rx, "  CPU     ");
            wattroff(win, COLOR_PAIR(C_DIM));
            wattron(win, COLOR_PAIR(cpu_cp)|A_BOLD);
            mvwprintw(win, ry-1, rx + 10, "%.1f%%", sel_ct->cpu_percent);
            wattroff(win, COLOR_PAIR(cpu_cp)|A_BOLD);

            // Memory
            int mem_cp = (sel_ct->mem_limit_mb > 0 &&
                          sel_ct->mem_used_mb / sel_ct->mem_limit_mb > 0.85)
                         ? C_RED : C_MEM;
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, ry++, rx, "  Memory  ");
            wattroff(win, COLOR_PAIR(C_DIM));
            wattron(win, COLOR_PAIR(mem_cp));
            mvwprintw(win, ry-1, rx + 10, "%.1f / %.1f MB",
                      sel_ct->mem_used_mb, sel_ct->mem_limit_mb);
            wattroff(win, COLOR_PAIR(mem_cp));

            // Network RX/TX
            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, ry++, rx, "  Net RX  ");
            wattroff(win, COLOR_PAIR(C_DIM));
            wattron(win, COLOR_PAIR(C_CYAN));
            mvwprintw(win, ry-1, rx + 10, "%.2f MB", sel_ct->net_rx_mb);
            wattroff(win, COLOR_PAIR(C_CYAN));

            wattron(win, COLOR_PAIR(C_DIM));
            mvwprintw(win, ry++, rx, "  Net TX  ");
            wattroff(win, COLOR_PAIR(C_DIM));
            wattron(win, COLOR_PAIR(C_CYAN));
            mvwprintw(win, ry-1, rx + 10, "%.2f MB", sel_ct->net_tx_mb);
            wattroff(win, COLOR_PAIR(C_CYAN));
        } else {
            wattron(win, COLOR_PAIR(C_DIM)|A_ITALIC);
            mvwprintw(win, ry++, rx, "  (press [m] to load metrics)");
            wattroff(win, COLOR_PAIR(C_DIM)|A_ITALIC);
        }
    }

    // 액션 바
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, y0+mid_h-2, 2,
              "ACTIONS: [s]Start [x]Stop [D]Destroy [e]Exec [m]Metrics [S]Snap [R]Rollback [N]NICs [L]Limits [r]Refresh");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);
}

static void handle_key_ctr(int ch) {
    int *sel = &g_state.ctr.selected;
    int  cnt = g_state.ctr.ctr_count;
    switch (ch) {
    case KEY_UP:   if (*sel > 0)    { (*sel)--; g_state.dirty.all=TRUE; } break;
    case KEY_DOWN: if (*sel < cnt-1){ (*sel)++; g_state.dirty.all=TRUE; } break;
    case 'r': fetch_ctr_data(); push_log("CTR: refreshed"); g_state.dirty.all=TRUE; break;
    case 's': if (cnt > 0) {
        const char *cn = g_state.ctr.ctrs[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", cn);
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "CTR START [%s]", cn);
        send_async_rpc("container.start", p, log_ok, "CTR START");
        break;
    } break;
    case 'x': if (cnt > 0) {
        const char *cn = g_state.ctr.ctrs[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", cn);
        char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "CTR STOP [%s]", cn);
        send_async_rpc("container.stop", p, log_ok, "CTR STOP");
        break;
    } break;
    case 'D': if (cnt > 0) {
        const char *cn = g_state.ctr.ctrs[*sel].name;
        char warn[128]; snprintf(warn, sizeof(warn), "DESTROY container '%s'", cn);
        if (confirm_dialog(warn, cn)) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", cn);
            char log_ok[256]; snprintf(log_ok, sizeof(log_ok), "CTR DESTROY [%s]", cn);
            send_async_rpc("container.destroy", p, log_ok, "CTR DEST");
            g_state.ctr.needs_refresh = TRUE;
        }
        g_state.dirty.all = TRUE;
        break;
    } break;
    case 'e': if (cnt > 0) {
        // exec — ncurses 일시 종료 후 직접 실행
        const char *cn = g_state.ctr.ctrs[*sel].name;
        endwin();
        printf("\n[PureCVisor] exec → container %s\n", cn);

        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name",    cn);
        json_object_set_string_member(p, "command", "/bin/bash");
        GError *err = NULL;
        gchar  *resp = tui_send_request("container.exec", p, &err);
        if (resp) g_free(resp);
        if (err) g_error_free(err);

        printf("[PureCVisor] exec 종료. Enter to return TUI...\n");
        getchar();
        refresh();
        g_state.dirty.all = TRUE;
        break;
    } break;
    case 'm': if (cnt > 0) {
        CtrInfo *ci = &g_state.ctr.ctrs[*sel];
        const char *cn = ci->name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", cn);
        GError *err = NULL;
        gchar  *resp = tui_send_request("container.metrics", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonObject *res = json_object_get_object_member(ro, "result");
                    ci->cpu_percent  = safe_double(res, "cpu_percent");
                    ci->mem_used_mb  = safe_double(res, "mem_used_mb");
                    ci->mem_limit_mb = safe_double(res, "mem_limit_mb");
                    ci->net_rx_mb    = safe_double(res, "net_rx_mb");
                    ci->net_tx_mb    = safe_double(res, "net_tx_mb");
                    ci->metrics_loaded = TRUE;
                    char tmp[128];
                    snprintf(tmp, sizeof(tmp),
                             "CTR METRICS [%s] CPU=%.1f%% MEM=%.1f/%.1fMB",
                             cn, ci->cpu_percent,
                             ci->mem_used_mb, ci->mem_limit_mb);
                    push_log(tmp);
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("CTR METRICS ERR"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── CONTAINER SNAPSHOT CREATE (S) ─────────────────────────────────────
    case 'S': if (cnt > 0) {
        char snap[64] = {0};
        const char *cn = g_state.ctr.ctrs[*sel].name;
        char pmsg[80]; snprintf(pmsg, sizeof(pmsg), "Snap name [%s]: ", cn);
        if (prompt_input(pmsg, snap, sizeof(snap), C_CYAN) && snap[0]) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name",      cn);
            json_object_set_string_member(p, "snap_name", snap);
            char log_ok[256]; snprintf(log_ok, sizeof(log_ok),
                                       "CTR SNAP CREATE [%s]@%s", cn, snap);
            send_async_rpc("container.snapshot.create", p, log_ok, "CTR SNAP");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── CONTAINER SNAPSHOT ROLLBACK (R) ───────────────────────────────────
    case 'R': if (cnt > 0) {
        char snap[64] = {0};
        const char *cn = g_state.ctr.ctrs[*sel].name;
        char pmsg[128]; snprintf(pmsg, sizeof(pmsg), "Rollback snap [%s]: ", cn);
        if (prompt_input(pmsg, snap, sizeof(snap), C_YELLOW) && snap[0]) {
            char warn[192]; snprintf(warn, sizeof(warn),
                                     "ROLLBACK '%s' to snapshot '%s'", cn, snap);
            if (confirm_dialog(warn, cn)) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name",      cn);
                json_object_set_string_member(p, "snap_name", snap);
                char log_ok[256]; snprintf(log_ok, sizeof(log_ok),
                                           "CTR SNAP ROLLBACK [%s]@%s", cn, snap);
                send_async_rpc("container.snapshot.rollback", p, log_ok, "CTR ROLL");
            }
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── CONTAINER NIC LIST (N) ─────────────────────────────────────────
    case 'N': if (cnt > 0) {
        const char *cn = g_state.ctr.ctrs[*sel].name;
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "name", cn);
        GError *err = NULL;
        gchar *resp = tui_send_request("container.nic.list", p, &err);
        if (!err && resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(ro, "result")) {
                    JsonNode *rn = json_object_get_member(ro, "result");
                    if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(rn);
                        guint len = arr ? json_array_get_length(arr) : 0;
                        push_log_level("=== Container NICs ===", LOG_SYS);
                        for (guint ni = 0; ni < len; ni++) {
                            JsonObject *nic = json_array_get_object_element(arr, ni);
                            const gchar *nic_name = safe_str(nic, "name", "?");
                            const gchar *hwaddr = safe_str(nic, "hwaddr", "?");
                            char tmp[128];
                            snprintf(tmp, sizeof(tmp), "  %s — %s", nic_name, hwaddr);
                            push_log(tmp);
                        }
                        if (len == 0) push_log("  (no NICs)");
                    }
                }
            }
            g_object_unref(jp); g_free(resp);
        } else if (err) { push_log("NIC list error"); g_error_free(err); }
        g_state.dirty.all = TRUE;
        break;
    } break;

    // ── CONTAINER RESOURCE LIMITS (L) ────────────────────────────────────
    case 'L': if (cnt > 0) {
        const char *cn = g_state.ctr.ctrs[*sel].name;
        char cpu_s[8]={0}, mem_s[16]={0};
        prompt_input("CPU limit (e.g. 2): ", cpu_s, sizeof(cpu_s), C_YELLOW);
        prompt_input("Memory limit (e.g. 512M): ", mem_s, sizeof(mem_s), C_YELLOW);
        if (cpu_s[0] || mem_s[0]) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", cn);
            if (cpu_s[0]) json_object_set_string_member(p, "cpu_limit", cpu_s);
            if (mem_s[0]) json_object_set_string_member(p, "memory_limit", mem_s);
            send_async_rpc("container.set_limits", p, "Limits set", "CTR LIMITS");
        }
        g_state.dirty.all = TRUE;
        break;
    } break;

    default: break;
    }
    ScrollState *sc = &g_state.ctr.scroll;
    if (*sel < sc->position) sc->position = *sel;
    if (sc->viewport > 0 && *sel >= sc->position + sc->viewport)
        sc->position = *sel - sc->viewport + 1;
}

// =============================================================================
// HOST VIEW — btop 스타일 전체 화면 호스트 대시보드 (F5)
// =============================================================================
// =============================================================================
// [T-5] CLUSTER VIEW — HA 클러스터 상태 대시보드
// =============================================================================

static void draw_view_host(WINDOW *win, int y0, int mid_h, int w) {
    int nc = g_state.core_count > 0 ? g_state.core_count : g_state.host.cpus;
    if (nc <= 0) nc = 1;

    // ── 상단: 호스트 정보 요약 ─────────────────────────────────────────────
    draw_panel(win, y0, 0, 3, w, "HOST INFO", C_FLEET);
    wattron(win, A_BOLD);
    mvwprintw(win, y0 + 1, 2, "%.48s  [%d cores]", g_state.host.cpu_model, nc);
    wattroff(win, A_BOLD);

    // Uptime
    if (g_state.host.uptime_secs > 0) {
        int up_d = (int)(g_state.host.uptime_secs / 86400);
        int up_h = (int)((long)g_state.host.uptime_secs % 86400) / 3600;
        int up_m = (int)((long)g_state.host.uptime_secs % 3600) / 60;
        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, y0 + 1, w/2 + 5, "Uptime: %dd %dh %dm", up_d, up_h, up_m);
        wattroff(win, COLOR_PAIR(C_DIM));
    }

    // Load + Temp
    wattron(win, COLOR_PAIR(C_LOAD));
    mvwprintw(win, y0 + 1, w - 40, "Load: %.2f %.2f %.2f",
              g_state.host.load_1, g_state.host.load_5, g_state.host.load_15);
    wattroff(win, COLOR_PAIR(C_LOAD));
    if (g_state.host.cpu_temp_c > 0) {
        int t_cp = g_state.host.cpu_temp_c > 80 ? C_RED :
                   g_state.host.cpu_temp_c > 60 ? C_YELLOW : C_GREEN;
        wattron(win, COLOR_PAIR(t_cp)|A_BOLD);
        mvwprintw(win, y0 + 1, w - 8, "%.0f°C", g_state.host.cpu_temp_c);
        wattroff(win, COLOR_PAIR(t_cp)|A_BOLD);
    }

    // ── 좌측: 코어별 CPU 바 ────────────────────────────────────────────────
    int cpu_panel_y = y0 + 3;
    int left_w = w / 2;
    int right_w = w - left_w;

    // CPU 전체
    int cpu_rows = mid_h - 7;  // 호스트 정보(3) + MEM/SWP/DSK/NET(4)
    if (cpu_rows < 2) cpu_rows = 2;
    int cols = 1;
    if (nc > cpu_rows) cols = 2;
    if (nc > cpu_rows * 2) cols = 4;
    int col_w = (left_w - 2) / cols;
    int bar_w = col_w - 14;
    if (bar_w < 4) bar_w = 4;
    int rows_avail = cpu_rows;

    // CPU 패널 제목
    int agg_cp = pcv_color_for_pct(g_state.cpu_pct / 100.0);
    wattron(win, COLOR_PAIR(agg_cp)|A_BOLD);
    mvwprintw(win, cpu_panel_y, 2, "CPU Total: %.1f%%", g_state.cpu_pct);
    wattroff(win, COLOR_PAIR(agg_cp)|A_BOLD);

    // 코어 바 렌더링
    for (int i = 0; i < nc; i++) {
        int col = 0, row = i;
        if (cols >= 2) { col = i / rows_avail; row = i % rows_avail; }
        if (col >= cols || row >= rows_avail) break;
        int cx = 2 + col * col_w;
        int cy = cpu_panel_y + 1 + row;
        double pct = (i < g_state.core_count) ? g_state.core_pct[i] : 0.0;
        int ccp = pcv_color_for_pct(pct / 100.0);

        wattron(win, COLOR_PAIR(C_DIM));
        mvwprintw(win, cy, cx, "c%-2d", i);
        wattroff(win, COLOR_PAIR(C_DIM));
        draw_bar(win, cy, cx + 4, bar_w, pct, ccp);
        wattron(win, COLOR_PAIR(ccp)|A_BOLD);
        mvwprintw(win, cy, cx + 4 + bar_w + 1, "%5.1f%%", pct);
        wattroff(win, COLOR_PAIR(ccp)|A_BOLD);
    }

    // ── 우측: CPU 히스토리 브레일 차트 ─────────────────────────────────────
    int chart_x = left_w + 1;
    int chart_w_cells = right_w - 8;
    int chart_h_cells = cpu_rows - 1;
    if (chart_w_cells > MAX_CHART_W) chart_w_cells = MAX_CHART_W;
    if (chart_h_cells > MAX_CHART_H) chart_h_cells = MAX_CHART_H;
    if (chart_w_cells >= 4 && chart_h_cells >= 2) {
        wattron(win, COLOR_PAIR(C_CH_CPU)|A_BOLD);
        mvwprintw(win, cpu_panel_y, chart_x, "CPU HISTORY");
        wattroff(win, COLOR_PAIR(C_CH_CPU)|A_BOLD);

        BrailleGrid cpu_grid;
        bgrid_init(&cpu_grid, chart_w_cells, chart_h_cells);
        bgrid_plot_series(&cpu_grid,
                          g_state.cpu_hist, HIST_SIZE, g_state.hist_pos,
                          0.0, 100.0);
        bgrid_render(win, &cpu_grid,
                     cpu_panel_y + 1, chart_x + 5,
                     braille_color_gradient, -1, -1);
        draw_y_axis(win, cpu_panel_y + 1, chart_h_cells, 0.0, 100.0, "%", chart_x);
    }

    // ── 하단: MEM / SWP / DSK / NET ────────────────────────────────────────
    int bot_y = cpu_panel_y + 1 + cpu_rows;
    int bar_full_w = w - 30;
    if (bar_full_w < 10) bar_full_w = 10;

    // MEM
    if (bot_y < y0 + mid_h) {
        int mem_cp = pcv_color_for_pct(g_state.host.mem_percent / 100.0);
        wattron(win, COLOR_PAIR(mem_cp)|A_BOLD);
        mvwprintw(win, bot_y, 2, "MEM");
        wattroff(win, COLOR_PAIR(mem_cp)|A_BOLD);
        draw_bar(win, bot_y, 6, bar_full_w / 2, g_state.host.mem_percent, C_MEM);
        mvwprintw(win, bot_y, 7 + bar_full_w / 2,
                  "%.1fG / %.1fG (%4.1f%%)  Buf:%.0fM Cch:%.0fM",
                  g_state.host.mem_used_gb, g_state.host.mem_total_gb,
                  g_state.host.mem_percent,
                  g_state.host.mem_buffers_mb, g_state.host.mem_cached_mb);
    }

    // SWP
    if (bot_y + 1 < y0 + mid_h && g_state.host.swap_total_gb > 0.001) {
        double swp_pct = (g_state.host.swap_used_gb / g_state.host.swap_total_gb) * 100.0;
        int swp_cp = pcv_color_for_pct(swp_pct / 100.0);
        wattron(win, COLOR_PAIR(swp_cp));
        mvwprintw(win, bot_y + 1, 2, "SWP");
        wattroff(win, COLOR_PAIR(swp_cp));
        draw_bar(win, bot_y + 1, 6, bar_full_w / 2, swp_pct, C_SWAP);
        mvwprintw(win, bot_y + 1, 7 + bar_full_w / 2,
                  "%.1fG / %.1fG (%4.1f%%)",
                  g_state.host.swap_used_gb, g_state.host.swap_total_gb, swp_pct);
    }

    // DSK
    if (bot_y + 2 < y0 + mid_h) {
        int dsk_cp = pcv_color_for_pct(g_state.host.disk_percent / 100.0);
        wattron(win, COLOR_PAIR(dsk_cp));
        mvwprintw(win, bot_y + 2, 2, "DSK");
        wattroff(win, COLOR_PAIR(dsk_cp));
        draw_bar(win, bot_y + 2, 6, bar_full_w / 2, g_state.host.disk_percent, C_CH_DSK);
        mvwprintw(win, bot_y + 2, 7 + bar_full_w / 2,
                  "%.1fG / %.1fG (%4.1f%%)",
                  g_state.host.disk_used_gb, g_state.host.disk_total_gb,
                  g_state.host.disk_percent);
    }

    // NET
    if (bot_y + 3 < y0 + mid_h) {
        char rx_s[16], tx_s[16];
        format_bytes(g_state.rx_speed, rx_s, sizeof(rx_s));
        format_bytes(g_state.tx_speed, tx_s, sizeof(tx_s));
        wattron(win, COLOR_PAIR(C_CH_NETDL)|A_BOLD);
        mvwprintw(win, bot_y + 3, 2, "NET %s", g_state.host.net_iface);
        wattroff(win, COLOR_PAIR(C_CH_NETDL)|A_BOLD);
        wattron(win, COLOR_PAIR(C_CH_NETDL));
        mvwprintw(win, bot_y + 3, 18, "▲DL:%s/s", rx_s);
        wattroff(win, COLOR_PAIR(C_CH_NETDL));
        wattron(win, COLOR_PAIR(C_CH_NETUL));
        mvwprintw(win, bot_y + 3, 36, "▼UL:%s/s", tx_s);
        wattroff(win, COLOR_PAIR(C_CH_NETUL));

        // NET 미니 스파크라인
        double net_max = 1.0;
        for (int i = 0; i < HIST_SIZE; i++)
            if (g_state.net_rx_hist[i] > net_max) net_max = g_state.net_rx_hist[i];
        draw_sparkline(win, bot_y + 3, 54, w - 56,
                       g_state.net_rx_hist, HIST_SIZE, g_state.hist_pos,
                       net_max, C_SPARK_NET);
    }

    /* 하단 액션 안내 */
    wattron(win, COLOR_PAIR(C_DIM));
    if (mid_h > 8) {
        mvwprintw(win, y0 + mid_h - 2, 2,
                  "[f]Fleet [g]GPU [G]gRPC [h]Config [a]Alert [d]Audit");
        mvwprintw(win, y0 + mid_h - 1, 2,
                  "[S]Sec [T]Tgl [E]Evt [P]Pend [A]Appr [D]Dismiss [B]Base");
    } else {
        mvwprintw(win, y0 + mid_h - 1, 2,
                  "[f]Fleet [g]GPU [S]Sec [E]Evt [P]Pend [A]Appr [B]Base");
    }
    wattroff(win, COLOR_PAIR(C_DIM));
}

static gboolean
tui_security_parse_response(const char *label, gchar *resp, JsonParser **out_parser, JsonObject **out_root)
{
    *out_parser = NULL;
    *out_root = NULL;
    if (!resp) {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s: no response", label);
        push_log_level(tmp, LOG_WARN);
        return FALSE;
    }

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, resp, -1, NULL) ||
        !json_parser_get_root(jp) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(jp))) {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s: invalid JSON response", label);
        push_log_level(tmp, LOG_WARN);
        g_object_unref(jp);
        return FALSE;
    }

    JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
    if (json_object_has_member(ro, "error")) {
        JsonObject *eo = json_object_get_object_member(ro, "error");
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s: %s", label, safe_str(eo, "message", "RPC error"));
        push_log_level(tmp, LOG_WARN);
        g_object_unref(jp);
        return FALSE;
    }

    *out_parser = jp;
    *out_root = ro;
    return TRUE;
}

static void
tui_security_status(void)
{
    GError *err = NULL;
    gchar *resp = tui_send_request("security.config.get", NULL, &err);
    if (err) {
        push_log_level("Security Guard status RPC failed", LOG_WARN);
        g_error_free(err);
        return;
    }

    JsonParser *jp = NULL;
    JsonObject *ro = NULL;
    if (tui_security_parse_response("Security Guard status", resp, &jp, &ro)) {
        JsonObject *res = json_object_get_object_member(ro, "result");
        if (res) {
            char tmp[256];
            snprintf(tmp, sizeof(tmp),
                     "Security Guard: enabled=%s baseline=%s risk=%ld pending=%ld degraded=%s",
                     json_object_get_boolean_member_with_default(res, "enabled", FALSE) ? "true" : "false",
                     safe_str(res, "baseline_status", "unknown"),
                     (long)safe_int(res, "open_risk"),
                     (long)safe_int(res, "pending_actions"),
                     json_object_get_boolean_member_with_default(res, "degraded", FALSE) ? "true" : "false");
            push_log_level(tmp, LOG_SYS);
        }
        g_object_unref(jp);
    }
    g_free(resp);
    g_state.dirty.all = TRUE;
}

static void
tui_security_toggle(void)
{
    GError *err = NULL;
    gchar *resp = tui_send_request("security.config.get", NULL, &err);
    if (err) {
        push_log_level("Security Guard toggle: status RPC failed", LOG_WARN);
        g_error_free(err);
        return;
    }

    JsonParser *jp = NULL;
    JsonObject *ro = NULL;
    gboolean next_enabled = TRUE;
    gboolean parsed = FALSE;
    if (tui_security_parse_response("Security Guard toggle", resp, &jp, &ro)) {
        JsonObject *res = json_object_get_object_member(ro, "result");
        next_enabled = !json_object_get_boolean_member_with_default(res, "enabled", FALSE);
        parsed = TRUE;
        g_object_unref(jp);
    }
    g_free(resp);
    if (!parsed) {
        return;
    }

    JsonObject *params = json_object_new();
    json_object_set_boolean_member(params, "enabled", next_enabled);
    send_async_rpc("security.config.set", params,
                   next_enabled ? "Security Guard enabled" : "Security Guard disabled",
                   "Security Guard toggle");
    g_state.dirty.all = TRUE;
}

static void
tui_security_events(void)
{
    JsonObject *params = json_object_new();
    json_object_set_int_member(params, "limit", 10);
    GError *err = NULL;
    gchar *resp = tui_send_request("security.event.list", params, &err);
    if (err) {
        push_log_level("Security events RPC failed", LOG_WARN);
        g_error_free(err);
        return;
    }

    JsonParser *jp = NULL;
    JsonObject *ro = NULL;
    if (tui_security_parse_response("Security events", resp, &jp, &ro)) {
        JsonNode *rn = json_object_get_member(ro, "result");
        JsonArray *arr = rn && JSON_NODE_HOLDS_ARRAY(rn) ? json_node_get_array(rn) : NULL;
        guint len = arr ? json_array_get_length(arr) : 0;
        push_log_level("=== Security Events (last 10) ===", LOG_SYS);
        for (guint i = 0; i < len; i++) {
            JsonObject *ev = json_array_get_object_element(arr, i);
            char tmp[300];
            snprintf(tmp, sizeof(tmp), "  [%s/%s] %s action=%s id=%s",
                     safe_str(ev, "severity", "-"),
                     safe_str(ev, "status", "-"),
                     safe_str(ev, "summary", "-"),
                     safe_str(ev, "recommended_action", "-"),
                     safe_str(ev, "event_id", "-"));
            push_log_level(tmp, g_strcmp0(safe_str(ev, "severity", ""), "crit") == 0 ? LOG_WARN : LOG_SYS);
        }
        if (len == 0) push_log("  (no security events)");
        g_object_unref(jp);
    }
    g_free(resp);
    g_state.dirty.all = TRUE;
}

static void
tui_security_pending(void)
{
    GError *err = NULL;
    gchar *resp = tui_send_request("security.action.pending", NULL, &err);
    if (err) {
        push_log_level("Security pending RPC failed", LOG_WARN);
        g_error_free(err);
        return;
    }

    JsonParser *jp = NULL;
    JsonObject *ro = NULL;
    if (tui_security_parse_response("Security pending", resp, &jp, &ro)) {
        JsonNode *rn = json_object_get_member(ro, "result");
        JsonArray *arr = rn && JSON_NODE_HOLDS_ARRAY(rn) ? json_node_get_array(rn) : NULL;
        guint len = arr ? json_array_get_length(arr) : 0;
        push_log_level("=== Pending HIPS Actions ===", LOG_SYS);
        for (guint i = 0; i < len; i++) {
            JsonObject *act = json_array_get_object_element(arr, i);
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "  [%s] %s target=%s id=%s",
                     safe_str(act, "status", "-"),
                     safe_str(act, "action", "-"),
                     safe_str(act, "target", "-"),
                     safe_str(act, "event_id", "-"));
            push_log_level(tmp, LOG_SYS);
        }
        if (len == 0) push_log("  (no pending actions)");
        g_object_unref(jp);
    }
    g_free(resp);
    g_state.dirty.all = TRUE;
}

static void
tui_security_approve_prompt(void)
{
    char event_id[160] = {0};
    if (!prompt_input("Security event_id to approve: ", event_id, sizeof(event_id), C_RED) || !event_id[0]) {
        return;
    }
    if (!confirm_dialog("APPROVE HIPS ACTION", event_id)) {
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "event_id", event_id);
    send_async_rpc("security.action.approve", params, "Security action approve", "Security approve");
    g_state.dirty.all = TRUE;
}

static void
tui_security_dismiss_prompt(void)
{
    char event_id[160] = {0};
    char reason[180] = {0};
    if (!prompt_input("Security event_id to dismiss: ", event_id, sizeof(event_id), C_YELLOW) || !event_id[0]) {
        return;
    }
    prompt_input("Dismiss reason (Enter=operator dismissed): ", reason, sizeof(reason), C_YELLOW);
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "event_id", event_id);
    json_object_set_string_member(params, "reason", reason[0] ? reason : "operator dismissed from TUI");
    send_async_rpc("security.action.dismiss", params, "Security action dismissed", "Security dismiss");
    g_state.dirty.all = TRUE;
}

static void
tui_security_baseline_prompt(void)
{
    char path[PATH_MAX] = {0};
    if (!prompt_input("Baseline path: ", path, sizeof(path), C_YELLOW) || !path[0]) {
        return;
    }
    JsonObject *params = json_object_new();
    JsonArray *paths = json_array_new();
    json_array_add_string_element(paths, path);
    json_object_set_array_member(params, "paths", paths);
    send_async_rpc("security.baseline.refresh", params, "Security baseline refreshed", "Security baseline");
    g_state.dirty.all = TRUE;
}

// =============================================================================
// MAIN DRAW DISPATCH (A2 — TuiView 상태 머신 + DrawFn 디스패치)
// =============================================================================
//
// Elm Architecture의 View에 해당한다.
// TuiState(Model)의 current_view에 따라 적절한 draw_view_*() 함수를 호출한다.
//
// 화면 레이아웃 (수직 4분할):
//   ┌────────────────────────────────────────┐
//   │  TOP PANEL (호스트 메트릭 차트)         │  row_sizes[0]
//   ├────────────────────────────────────────┤
//   │  TAB BAR (F1~F7 탭 + 갱신주기)         │  row_sizes[1] = 1
//   ├────────────────────────────────────────┤
//   │  MAIN CONTENT (현재 뷰)                │  row_sizes[2] = FILL
//   │  - VM: 좌측 로스터 + 우측 인스펙터      │
//   │  - NET/STG/CTR/OVN: 단일 패널          │
//   │  - HOST/CLUSTER: 전체 폭              │
//   ├────────────────────────────────────────┤
//   │  LOG PANEL (이벤트 스트림)              │  row_sizes[3] = 5
//   └────────────────────────────────────────┘
//
static void draw_tui(WINDOW *win) {
    int w, h;
    getmaxyx(win, h, w);

    // Constraint 레이아웃 (Ratatui Constraint 차용)
    // 브레일 모드: 상단 패널 12행, 스파크/간략: 5행
    int top_h = (g_state.top_mode == TOP_MODE_BRAILLE) ? 12
              : (g_state.top_mode == TOP_MODE_BTOP)    ? (4 + (g_state.core_count + 1) / 2)
              : (g_state.top_mode == TOP_MODE_COMPACT) ?  3 : 5;
    // btop 모드 높이 상한: 화면의 절반까지
    if (top_h > h / 2) top_h = h / 2;
    if (top_h < 3) top_h = 3;

    PcConstraint row_c[] = {
        { PC_LENGTH, top_h },  // 0: top panel
        { PC_LENGTH, 1 },      // 1: tab bar
        { PC_FILL,   1 },      // 2: main content
        { PC_LENGTH, 5 },      // 3: log panel
    };
    int row_sizes[4];
    pcv_layout_split(h, row_c, 4, row_sizes);

    int top_y   = 0;
    int tab_y   = top_y + row_sizes[0];
    int main_y  = tab_y + row_sizes[1];
    int main_h  = row_sizes[2];
    int log_y   = main_y + main_h;
    int log_h   = row_sizes[3];

    // 좌/우 분할 (VM 뷰만)
    PcConstraint col_c[] = {
        { PC_MIN, 52 },   // 0: 로스터 (최소 52열)
        { PC_FILL, 1 },   // 1: 인스펙터
    };
    int col_sizes[2];
    pcv_layout_split(w, col_c, 2, col_sizes);
    int left_w  = col_sizes[0];
    int right_w = col_sizes[1];

    // ── TOP PANEL ─────────────────────────────────────────────────────────
    draw_top_panel(win, top_y, w, row_sizes[0]);

    // ── TAB BAR (Ratatui Tabs 차용) ────────────────────────────────────────
    draw_tab_bar(win, tab_y, g_state.current_view, w);

    // ── MAIN CONTENT (뷰별 DrawFn 디스패치) ────────────────────────────────
    switch (g_state.current_view) {
    case TUI_VIEW_VM:
        draw_view_vm(win, main_y, main_h, left_w, right_w);
        break;
    case TUI_VIEW_NET:
        draw_view_net(win, main_y, main_h, w);
        break;
    case TUI_VIEW_OVN:
        draw_view_ovn(win, main_y, main_h, w);
        break;
    case TUI_VIEW_STG:
        draw_view_stg(win, main_y, main_h, w);
        break;
    case TUI_VIEW_CTR:
        draw_view_ctr(win, main_y, main_h, w);
        break;
    case TUI_VIEW_HOST:
        draw_view_host(win, main_y, main_h, w);
        break;
    default: break;
    }

    // ── LOG PANEL ──────────────────────────────────────────────────────────
    draw_log_panel(win, log_y, w, log_h);
}

// =============================================================================
// MAIN — TUI 진입점
// =============================================================================
//
// 초기화 순서:
//   1. setlocale(LC_ALL, "") — 와이드 문자(한글/브레일) 지원에 필수
//   2. ncursesw 초기화 (initscr, noecho, curs_set, keypad, timeout)
//   3. 컬러 페어 등록 (C_CPU ~ C_CACHED, 총 34개)
//   4. GLib 동기화 프리미티브 초기화 (GMutex, GAsyncQueue)
//   5. 백그라운드 fleet_worker 스레드 시작
//   6. getch() 메인 루프 진입 → 'q' 입력까지 반복
//
// 메인 루프 구조:
//   while ((ch = getch()) != 'q') {
//       process_events();     // 비동기 이벤트 소비 (fleet 데이터, RPC 결과)
//       글로벌 키 처리;       // F1~F7 탭 전환, t 모드, R 갱신주기, PgUp/PgDn 로그
//       뷰별 키 핸들러;       // handle_key_vm(), handle_key_net() 등
//       DirtyFlags 기반 선택적 리드로우;  // werase + draw_tui + wrefresh
//   }
//
// 종료 시:
//   g_quit_fleet=1 → fleet_worker 조인 → GAsyncQueue/GMutex 정리 → endwin()
//
/* B7-C4 (Phase 3 fix): pcvtui 다중 인스턴스 차단.
 * 동일 사용자가 2개 pcvtui를 동시 실행하면 ncurses stdscr 충돌로 화면이 깨진다.
 * $XDG_RUNTIME_DIR/pcvtui.lock (없으면 /tmp/pcvtui-<uid>.lock)에 flock(LOCK_EX|LOCK_NB)
 * 시도하여 이미 실행 중이면 stderr 안내 후 종료. */
static int g_tui_lock_fd = -1;
static int
_tui_acquire_singleton_lock(void)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    if (runtime_dir && *runtime_dir) {
        snprintf(path, sizeof(path), "%s/pcvtui.lock", runtime_dir);
    } else {
        snprintf(path, sizeof(path), "/tmp/pcvtui-%u.lock", (unsigned)getuid());
    }
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "[pcvtui] WARN: cannot open lock file %s: %s\n",
                path, strerror(errno));
        return -1;  /* fail-open: 경고만 + 진행 */
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, "[pcvtui] ERROR: another pcvtui instance is already running.\n"
                            "         Lock file: %s\n"
                            "         Close the other instance or remove the lock file.\n",
                            path);
            close(fd);
            return -2;
        }
        fprintf(stderr, "[pcvtui] WARN: flock failed on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* 잠금 획득 성공 — PID 기록 (디버깅용) */
    char pid_str[32];
    int n = snprintf(pid_str, sizeof(pid_str), "%d\n", (int)getpid());
    if (write(fd, pid_str, (size_t)n) < 0) { /* best-effort */ }
    g_tui_lock_fd = fd;
    return 0;
}

int main(int argc, char *argv[]) {
    /* B7-C4: 다중 인스턴스 차단 (ncurses stdscr 충돌 방지) — 가장 먼저 검사 */
    int lock_rc = _tui_acquire_singleton_lock();
    if (lock_rc == -2) return 1;  /* 다른 인스턴스 실행 중 */

    setlocale(LC_ALL, "");  /* UTF-8 로케일 설정 — 브레일/한글 렌더링에 필수 */
    initscr(); noecho(); curs_set(0); keypad(stdscr, TRUE); timeout(50);
    // U6: 마우스 지원
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(50);
    start_color(); use_default_colors();

    init_pair(C_CPU,       COLOR_GREEN,   COLOR_BLACK);
    init_pair(C_MEM,       COLOR_MAGENTA, COLOR_BLACK);
    init_pair(C_FLEET,     COLOR_CYAN,    COLOR_BLACK);
    init_pair(C_GREEN,     COLOR_GREEN,   COLOR_BLACK);
    init_pair(C_RED,       COLOR_RED,     COLOR_BLACK);
    init_pair(C_YELLOW,    COLOR_YELLOW,  COLOR_BLACK);
    init_pair(C_DIM,       COLOR_WHITE,   COLOR_BLACK);
    init_pair(C_HIGHLIGHT, COLOR_BLACK,   COLOR_WHITE);
    init_pair(C_LOG,       COLOR_BLUE,    COLOR_BLACK);
    init_pair(C_TITLE,     COLOR_MAGENTA, COLOR_BLACK);
    init_pair(C_CYAN,      COLOR_CYAN,    COLOR_BLACK);
    init_pair(C_TAB_ACT,   COLOR_WHITE,   COLOR_BLUE);
    init_pair(C_SPARK_CPU, COLOR_GREEN,   COLOR_BLACK);
    init_pair(C_SPARK_NET, COLOR_CYAN,    COLOR_BLACK);

    // ── 브레일 차트 컬러 팔레트 (그라디언트 7단계) ───────────────────────
    init_pair(C_GRAD_0,  COLOR_GREEN,   COLOR_BLACK);  // 0~20%
    init_pair(C_GRAD_1,  COLOR_GREEN,   COLOR_BLACK);  // 20~40%  (A_BOLD로 구분)
    init_pair(C_GRAD_2,  COLOR_YELLOW,  COLOR_BLACK);  // 40~60%
    init_pair(C_GRAD_3,  COLOR_YELLOW,  COLOR_BLACK);  // 60~75%  (A_BOLD)
    init_pair(C_GRAD_4,  COLOR_RED,     COLOR_BLACK);  // 75~85%
    init_pair(C_GRAD_5,  COLOR_RED,     COLOR_BLACK);  // 85~95%  (A_BOLD)
    init_pair(C_GRAD_6,  COLOR_RED,     COLOR_BLACK);  // 95~100% (A_BOLD|A_BLINK)
    // ── 채널 색상 ─────────────────────────────────────────────────────────
    init_pair(C_CH_CPU,   COLOR_GREEN,   COLOR_BLACK);
    init_pair(C_CH_MEM,   COLOR_MAGENTA, COLOR_BLACK);
    init_pair(C_CH_DSK,   COLOR_YELLOW,  COLOR_BLACK);
    init_pair(C_CH_NETDL, COLOR_CYAN,    COLOR_BLACK);
    init_pair(C_CH_NETUL, COLOR_BLUE,    COLOR_BLACK);
    // ── 차트 구조 색상 ────────────────────────────────────────────────────
    init_pair(C_CHART_FRAME, COLOR_WHITE,  COLOR_BLACK);
    init_pair(C_CHART_WARN,  COLOR_YELLOW, COLOR_BLACK);
    init_pair(C_CHART_CRIT,  COLOR_RED,    COLOR_BLACK);
    init_pair(C_CHART_AXIS,  COLOR_WHITE,  COLOR_BLACK);
    // btop HOST 전용
    init_pair(C_SWAP,        COLOR_YELLOW, COLOR_BLACK);
    init_pair(C_TEMP,        COLOR_RED,    COLOR_BLACK);
    init_pair(C_LOAD,        COLOR_CYAN,   COLOR_BLACK);
    init_pair(C_CACHED,      COLOR_BLUE,   COLOR_BLACK);

    // GLib 초기화
    g_mutex_init(&g_fleet_mu);
    g_event_queue = g_async_queue_new();

    // 뷰 초기화 (첫 탭 전환 시 자동 fetch)
    g_state.net.needs_refresh = TRUE;
    g_state.stg.needs_refresh = TRUE;
    g_state.ctr.needs_refresh = TRUE;

    push_log("PureCVisor TUI J-1 — Tactical Link Established.");
    push_log("F1=VM  F2=NET  F3=STG  F4=CTR  F5=HOST  F6=HA  F7=OVN  t=TopMode  R=Refresh  q=Quit");

    // ── 백그라운드 Fleet 스레드 시작 (A1 — UI 블로킹 제거) ─────────────────
    GThread *fleet_th = g_thread_new("fleet-worker", fleet_worker, NULL);

    // ── 메인 루프 ────────────────────────────────────────────────────────────
    int ch;
    while ((ch = getch()) != 'q') {

        // 이벤트 처리 (rat-salsa 패턴)
        process_events();

        // ── 글로벌 키 ──────────────────────────────────────────────────────
        if (ch == KEY_RESIZE) {
            clear(); g_state.dirty.all = TRUE;
        }
        // U6: 마우스 클릭 처리
        else if (ch == KEY_MOUSE) {
            MEVENT me;
            if (getmouse(&me) == OK) {
                int scr_rows, scr_cols;
                getmaxyx(stdscr, scr_rows, scr_cols);
                (void)scr_cols;
                // 탭 바 행 (탑 패널 바로 아래)
                int top_h = 0;
                switch (g_state.top_mode) {
                case TOP_MODE_BRAILLE: top_h = 13; break;
                case TOP_MODE_SPARK:   top_h = 6;  break;
                case TOP_MODE_COMPACT: top_h = 2;  break;
                case TOP_MODE_BTOP:    top_h = 4 + (g_state.core_count + 1) / 2; break;
                default:          top_h = 6;  break;
                }
                int tab_row = top_h;
                if (me.y == tab_row) {
                    // 탭 바 X 위치로 탭 선택
                    int tx = 1;
                    for (int ti = 0; ti < TUI_VIEW_COUNT; ti++) {
                        char badge[32]; tab_badge(badge, sizeof(badge), ti);
                        int tw = (int)strlen(badge) + 4;
                        if (me.x >= tx && me.x < tx + tw) {
                            g_state.current_view = (TuiView)ti;
                            g_state.dirty.all = TRUE;
                            break;
                        }
                        tx += tw;
                    }
                }
                // VM 뷰 로스터 행 클릭 — 해당 VM 선택
                else if (g_state.current_view == TUI_VIEW_VM) {
                    int main_y = tab_row + 1;
                    int main_h = scr_rows - main_y - 5; // 로그 패널 제외
                    int roster_top = main_y + 1;
                    int roster_bot = main_y + main_h - 1;
                    int left_w = scr_cols / 3;
                    if (me.x < left_w && me.y >= roster_top && me.y < roster_bot) {
                        int clicked = me.y - roster_top + g_state.vm.scroll.position;
                        if (clicked >= 0 && clicked < g_state.vm.fleet_count) {
                            g_state.vm.selected = clicked;
                            g_state.dirty.roster = g_state.dirty.detail = TRUE;
                        }
                    }
                }
            }
        }
        // U5: 도움말 오버레이
        else if (ch == '?') {
            draw_help_overlay();
            continue;
        }
        // 탭 전환 (Ratatui Tabs 차용)
        else if (ch == KEY_F(1) || ch == '1') {
            g_state.current_view = TUI_VIEW_VM;   g_state.dirty.all = TRUE;
        }
        else if (ch == KEY_F(2) || ch == '2') {
            g_state.current_view = TUI_VIEW_NET;  g_state.net.needs_refresh = TRUE; g_state.dirty.all = TRUE;
        }
        else if (ch == KEY_F(3) || ch == '3') {
            g_state.current_view = TUI_VIEW_STG;  g_state.stg.needs_refresh = TRUE; g_state.dirty.all = TRUE;
        }
        else if (ch == KEY_F(4) || ch == '4') {
            g_state.current_view = TUI_VIEW_CTR;  g_state.ctr.needs_refresh = TRUE; g_state.dirty.all = TRUE;
        }
        // F5 / 5 — HOST 탭 (btop 스타일 호스트 대시보드)
        else if (ch == KEY_F(5) || ch == '5') {
            g_state.current_view = TUI_VIEW_HOST;  g_state.dirty.all = TRUE;
        }
        // F7 / 7 — OVN 탭 (OVN SDN 대시보드)
        else if (ch == KEY_F(7) || ch == '7') {
            g_state.current_view = TUI_VIEW_OVN;  g_state.net.needs_refresh = TRUE; g_state.dirty.all = TRUE;
        }
        // t — 상단 패널 모드 토글 (브레일 / 스파크라인 / 간략 / btop)
        else if (ch == 't') {
            g_state.top_mode = (TopPanelMode)((g_state.top_mode + 1) % TOP_MODE_COUNT);
            static const char *mode_names[] = {"BRAILLE","SPARK","COMPACT","BTOP"};
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "TOP MODE → %s", mode_names[g_state.top_mode]);
            push_log(tmp);
            g_state.dirty.all = TRUE;
        }
        // 갱신 주기 토글 (R)
        else if (ch == 'R') {
            // U2: 500ms → 1s → 3s → 5s → 10s → 30s → 500ms 순환
            if      (g_state.refresh_ms == 500)   g_state.refresh_ms = 1000;
            else if (g_state.refresh_ms == 1000)  g_state.refresh_ms = 3000;
            else if (g_state.refresh_ms == 3000)  g_state.refresh_ms = 5000;
            else if (g_state.refresh_ms == 5000)  g_state.refresh_ms = 10000;
            else if (g_state.refresh_ms == 10000) g_state.refresh_ms = 30000;
            else                                   g_state.refresh_ms = 500;
            char tmp[64];
            if (g_state.refresh_ms < 1000)
                snprintf(tmp, sizeof(tmp), "Refresh interval → %dms", g_state.refresh_ms);
            else
                snprintf(tmp, sizeof(tmp), "Refresh interval → %ds", g_state.refresh_ms/1000);
            push_log(tmp);
            g_state.dirty.tab_bar = TRUE;
        }
        // U4: 로그 패널 스크롤 (PgUp/PgDn — 전역)
        else if (ch == KEY_PPAGE) {  // PgUp → 로그 위로
            g_state.log_paused = TRUE;
            g_state.log_offset += 3;
            if (g_state.log_offset >= g_state.log_count)
                g_state.log_offset = g_state.log_count - 1;
            g_state.dirty.log_area = TRUE;
        }
        else if (ch == KEY_NPAGE) {  // PgDn → 로그 아래로 / 자동스크롤 재개
            if (g_state.log_offset > 3)
                g_state.log_offset -= 3;
            else {
                g_state.log_offset = 0;
                g_state.log_paused = FALSE;
            }
            g_state.dirty.log_area = TRUE;
        }
        // 뷰별 키 핸들러 (Elm Update 패턴)
        else {
            switch (g_state.current_view) {
            case TUI_VIEW_VM:  handle_key_vm(ch);  break;
            case TUI_VIEW_NET: handle_key_net(ch); break;
            case TUI_VIEW_OVN: handle_key_ovn(ch); break;
            case TUI_VIEW_STG: handle_key_stg(ch); break;
            case TUI_VIEW_CTR: handle_key_ctr(ch); break;
            case TUI_VIEW_HOST: {
                // [f] Fleet Overview ─────────────────────────────────────────
                if (ch == 'f') {
                    JsonObject *p = json_object_new();
                    GError *err = NULL;
                    gchar *resp = tui_send_request("monitor.fleet", p, &err);
                    if (!err && resp) {
                        JsonParser *jp = json_parser_new();
                        if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                            JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                            if (json_object_has_member(ro, "result")) {
                                JsonObject *res = json_object_get_object_member(ro, "result");
                                double cpu = safe_double(res, "cpu_percent");
                                double mem = safe_double(res, "mem_percent");
                                double dsk = safe_double(res, "disk_percent");
                                gint64 vms = safe_int(res, "vm_count");
                                gint64 running = safe_int(res, "running_vms");
                                char tmp[256];
                                snprintf(tmp, sizeof(tmp),
                                         "=== Fleet === CPU:%.1f%% MEM:%.1f%% DSK:%.1f%% VMs:%ld (running:%ld)",
                                         cpu, mem, dsk, (long)vms, (long)running);
                                push_log_level(tmp, LOG_SYS);
                                // VM list summary
                                if (json_object_has_member(res, "vms")) {
                                    JsonArray *varr = json_object_get_array_member(res, "vms");
                                    guint vlen = varr ? json_array_get_length(varr) : 0;
                                    for (guint vi = 0; vi < vlen && vi < 20; vi++) {
                                        JsonObject *vm = json_array_get_object_element(varr, vi);
                                        const gchar *vn = safe_str(vm, "name", "?");
                                        const gchar *st = safe_str(vm, "state", "?");
                                        char vtmp[128];
                                        snprintf(vtmp, sizeof(vtmp), "  [%s] %s", st, vn);
                                        push_log(vtmp);
                                    }
                                }
                            }
                        }
                        g_object_unref(jp); g_free(resp);
                    } else if (err) { push_log("FLEET ERR"); g_error_free(err); }
                    g_state.dirty.all = TRUE;
                }
                // [g] GPU Metrics ────────────────────────────────────────────
                else if (ch == 'g') {
                    JsonObject *p = json_object_new();
                    GError *err = NULL;
                    gchar *resp = tui_send_request("gpu.metrics", p, &err);
                    if (!err && resp) {
                        JsonParser *jp = json_parser_new();
                        if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                            JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                            if (json_object_has_member(ro, "result")) {
                                JsonNode *rn = json_object_get_member(ro, "result");
                                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                                    JsonArray *arr = json_node_get_array(rn);
                                    guint len = json_array_get_length(arr);
                                    push_log_level("=== GPU Metrics ===", LOG_SYS);
                                    for (guint gi = 0; gi < len; gi++) {
                                        JsonObject *gpu = json_array_get_object_element(arr, gi);
                                        const gchar *gname = safe_str(gpu, "name", "?");
                                        double util = safe_double(gpu, "utilization");
                                        double temp = safe_double(gpu, "temperature");
                                        double mem_pct = safe_double(gpu, "memory_percent");
                                        char tmp[256];
                                        snprintf(tmp, sizeof(tmp),
                                                 "  [%s] util=%.0f%% temp=%.0fC mem=%.0f%%",
                                                 gname, util, temp, mem_pct);
                                        push_log_level(tmp, temp > 85.0 ? LOG_WARN : LOG_SYS);
                                    }
                                    if (len == 0) push_log("  (no GPUs detected)");
                                }
                            } else if (json_object_has_member(ro, "error")) {
                                JsonObject *eo = json_object_get_object_member(ro, "error");
                                char tmp[256];
                                snprintf(tmp, sizeof(tmp), "GPU: %s", safe_str(eo, "message", "error"));
                                push_log_level(tmp, LOG_WARN);
                            }
                        }
                        g_object_unref(jp); g_free(resp);
                    } else if (err) { push_log("GPU ERR"); g_error_free(err); }
                    g_state.dirty.all = TRUE;
                }
                // [G] gRPC Status ──────────────────────────────────────────
                else if (ch == 'G') {
                    int gfd = socket(AF_INET, SOCK_STREAM, 0);
                    if (gfd >= 0) {
                        struct sockaddr_in gaddr;
                        memset(&gaddr, 0, sizeof(gaddr));
                        gaddr.sin_family = AF_INET;
                        gaddr.sin_port = htons(50051);
                        inet_pton(AF_INET, "127.0.0.1", &gaddr.sin_addr);
                        struct timeval gtv = {.tv_sec = 1, .tv_usec = 0};
                        setsockopt(gfd, SOL_SOCKET, SO_SNDTIMEO, &gtv, sizeof(gtv));
                        if (connect(gfd, (struct sockaddr *)&gaddr, sizeof(gaddr)) == 0) {
                            push_log_level("[gRPC] Server ACTIVE on port 50051", LOG_SYS);
                        } else {
                            push_log_level("[gRPC] Server DISABLED -- set [grpc] enabled=true in daemon.conf", LOG_WARN);
                        }
                        close(gfd);
                    } else {
                        push_log_level("[gRPC] socket() failed", LOG_WARN);
	                    }
	                    g_state.dirty.all = TRUE;
	                }
	                // [S/T/E/P/A/D/B] Security Guard ───────────────────────────
	                else if (ch == 'S') {
	                    tui_security_status();
	                }
	                else if (ch == 'T') {
	                    tui_security_toggle();
	                }
	                else if (ch == 'E') {
	                    tui_security_events();
	                }
	                else if (ch == 'P') {
	                    tui_security_pending();
	                }
	                else if (ch == 'A') {
	                    tui_security_approve_prompt();
	                }
	                else if (ch == 'D') {
	                    tui_security_dismiss_prompt();
	                }
	                else if (ch == 'B') {
	                    tui_security_baseline_prompt();
	                }
	                // [h] Config History ─────────────────────────────────────────
	                else if (ch == 'h') {
                    JsonObject *p = json_object_new();
                    GError *err = NULL;
                    gchar *resp = tui_send_request("config.history", p, &err);
                    if (!err && resp) {
                        JsonParser *jp = json_parser_new();
                        if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                            JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                            if (json_object_has_member(ro, "result")) {
                                JsonNode *rn = json_object_get_member(ro, "result");
                                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                                    JsonArray *arr = json_node_get_array(rn);
                                    guint len = json_array_get_length(arr);
                                    push_log_level("=== Config History ===", LOG_SYS);
                                    for (guint ci = 0; ci < len; ci++) {
                                        JsonObject *cfg = json_array_get_object_element(arr, ci);
                                        const gchar *ts = safe_str(cfg, "timestamp", "?");
                                        const gchar *desc = safe_str(cfg, "description", "?");
                                        char tmp[256];
                                        snprintf(tmp, sizeof(tmp), "  %s  %s", ts, desc);
                                        push_log(tmp);
                                    }
                                    if (len == 0) push_log("  (no config backups)");
                                }
                            } else if (json_object_has_member(ro, "error")) {
                                JsonObject *eo = json_object_get_object_member(ro, "error");
                                char tmp[256];
                                snprintf(tmp, sizeof(tmp), "CONFIG: %s", safe_str(eo, "message", "error"));
                                push_log_level(tmp, LOG_WARN);
                            }
                        }
                        g_object_unref(jp); g_free(resp);
                    } else if (err) { push_log("CONFIG HISTORY ERR"); g_error_free(err); }
                    g_state.dirty.all = TRUE;
                }
                // [a] Alert Config ───────────────────────────────────────────
                else if (ch == 'a') {
                    JsonObject *p = json_object_new();
                    GError *err = NULL;
                    gchar *resp = tui_send_request("alert.config.get", p, &err);
                    if (!err && resp) {
                        JsonParser *jp = json_parser_new();
                        if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                            JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                            if (json_object_has_member(ro, "result")) {
                                JsonObject *res = json_object_get_object_member(ro, "result");
                                push_log_level("=== Alert Config ===", LOG_SYS);
                                char tmp[128];
                                snprintf(tmp, sizeof(tmp), "  enabled=%s cpu_warn=%ld cpu_crit=%ld",
                                    json_object_has_member(res, "enabled") && json_object_get_boolean_member(res, "enabled") ? "true" : "false",
                                    (long)safe_int(res, "cpu_warn"), (long)safe_int(res, "cpu_crit"));
                                push_log(tmp);
                                snprintf(tmp, sizeof(tmp), "  mem_warn=%ld mem_crit=%ld disk_warn=%ld disk_crit=%ld",
                                    (long)safe_int(res, "mem_warn"), (long)safe_int(res, "mem_crit"),
                                    (long)safe_int(res, "disk_warn"), (long)safe_int(res, "disk_crit"));
                                push_log(tmp);
                            }
                        }
                        g_object_unref(jp); g_free(resp);
                    } else if (err) { push_log("ALERT CFG ERR"); g_error_free(err); }
                    g_state.dirty.all = TRUE;
                }
                // [d] Audit Search (last 10) ─────────────────────────────────
                else if (ch == 'd') {
                    JsonObject *p = json_object_new();
                    json_object_set_int_member(p, "limit", 10);
                    GError *err = NULL;
                    gchar *resp = tui_send_request("audit.search", p, &err);
                    if (!err && resp) {
                        JsonParser *jp = json_parser_new();
                        if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                            JsonObject *ro = json_node_get_object(json_parser_get_root(jp));
                            if (json_object_has_member(ro, "result")) {
                                JsonNode *rn = json_object_get_member(ro, "result");
                                if (JSON_NODE_TYPE(rn) == JSON_NODE_ARRAY) {
                                    JsonArray *arr = json_node_get_array(rn);
                                    guint len = arr ? json_array_get_length(arr) : 0;
                                    push_log_level("=== Audit Log (last 10) ===", LOG_SYS);
                                    for (guint ai = 0; ai < len && ai < 10; ai++) {
                                        JsonObject *entry = json_array_get_object_element(arr, ai);
                                        const gchar *ts = safe_str(entry, "timestamp", "?");
                                        const gchar *method = safe_str(entry, "method", "?");
                                        const gchar *user = safe_str(entry, "username", "?");
                                        char tmp[256];
                                        snprintf(tmp, sizeof(tmp), "  %s %s [%s]", ts, method, user);
                                        push_log(tmp);
                                    }
                                    if (len == 0) push_log("  (no audit entries)");
                                }
                            }
                        }
                        g_object_unref(jp); g_free(resp);
                    } else if (err) { push_log("AUDIT ERR"); g_error_free(err); }
                    g_state.dirty.all = TRUE;
                }
                break;
            }
            default: break;
            }
        }

        // ── 더티 플래그 기반 선택적 리드로우 ──────────────────────────────
        // (전체 dirty이거나 개별 플래그가 있으면 전체 리드로우)
        // ncurses의 내부 diff가 실제 변경분만 터미널에 전송하므로
        // werase+redraw가 항상 최적은 아니지만 DirtyFlags는 미래 최적화 기반
        if (g_state.dirty.all || g_state.dirty.top || g_state.dirty.roster
            || g_state.dirty.detail || g_state.dirty.log_area
            || g_state.dirty.tab_bar) {
            werase(stdscr);
            draw_tui(stdscr);
            wrefresh(stdscr);
            memset(&g_state.dirty, 0, sizeof(DirtyFlags));
        } else {
            // 스피너만 갱신
            spinner_tick(&g_state.spinner);
            if (g_state.spinner.active) {
                draw_tui(stdscr);
                wrefresh(stdscr);
            }
        }
    }

    // ── 정리 ─────────────────────────────────────────────────────────────────
    g_atomic_int_set(&g_quit_fleet, 1);
    g_thread_join(fleet_th);
    g_async_queue_unref(g_event_queue);
    g_mutex_clear(&g_fleet_mu);

    endwin();
    printf("\n [ SYSTEM ] TACTICAL UPLINK SEVERED MANUALLY.\n");
    return 0;
}
